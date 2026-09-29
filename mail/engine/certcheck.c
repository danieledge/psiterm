/* certcheck.c - checks who PsiMail is talking to over TLS
 *
 * PsiTerm's TLS client (ssh/tls13.c) doesn't check certificates: its
 * downloads carry their own signatures. PsiMail sends passwords, so here,
 * built into tls13.c with TLS_VERIFY:
 *
 *  1. the server's CertificateVerify signature is checked (RSA-PSS with
 *     SHA-256) against the key in its certificate - proof it holds the key;
 *  2. the certificate chain is checked up to a root built into PsiMail
 *     (roots.h: Let's Encrypt's ISRG Root X1, which Fastmail uses, and a
 *     few other common RSA roots), with the name and dates;
 *  3. if the chain can't be checked (another CA, a self-signed server),
 *     the user can choose to trust that server's key: its SHA-256 is kept
 *     in pins.txt and must match from then on.
 *
 * RSA only (keys up to 4096 bits): the Psion can afford RSA verification
 * (public exponent 65537) in well under a second, not ECDSA. Servers with
 * both kinds of certificate pick RSA because that is all PsiMail offers.
 */
#include <string.h>
#include <stdio.h>
#include "includes.h"
#include "pm.h"
#include "roots.h"

#define MAX_CERTS  6
#define CHAIN_MAX  (14 * 1024)

typedef struct
	{
	const unsigned char *der; int derlen;
	const unsigned char *tbs; int tbslen;       /* whole TBSCertificate TLV */
	const unsigned char *sig; int siglen;       /* signature bytes */
	int sighash;                                 /* 256 / 384 / 512, 0 = unsupported */
	const unsigned char *issuer; int issuerlen;  /* Name TLVs */
	const unsigned char *subject; int subjectlen;
	const unsigned char *spki; int spkilen;      /* SubjectPublicKeyInfo TLV */
	const unsigned char *n; int nlen;            /* RSA modulus */
	const unsigned char *e; int elen;
	long not_before, not_after;
	int is_ca;                                   /* basicConstraints cA */
	const unsigned char *san; int sanlen;        /* subjectAltName GeneralNames */
	} Cert;

static unsigned char g_chain[CHAIN_MAX];
static Cert g_cert[MAX_CERTS];
static int g_ncert;
static int g_proved;             /* CertificateVerify checked */
static int g_trusted;            /* chain or pin accepted */
static char g_host[80];
static int g_port;
static char g_problem[96];
static char g_fp[100];
static unsigned char g_known[8][32];   /* intermediates already checked to a root */
static int g_nknown;

void tlsv_set_host(const char *host, int port)
{
	pm_copy(g_host, host, sizeof(g_host));
	g_port = port;
}

const char *tlsv_fingerprint(void) { return g_fp; }
const char *tlsv_problem(void) { return g_problem; }

void tlsv_start(void)
{
	g_ncert = 0;
	g_proved = 0;
	g_trusted = 0;
	g_problem[0] = 0;
	g_fp[0] = 0;
}

int tlsv_ok(void) { return g_proved && g_trusted; }

/* ------------------------------------------------------------------ DER */

/* reads one TLV at *p; returns its tag, or -1 */
static int tlv(const unsigned char **p, const unsigned char *end, const unsigned char **val, int *len)
{
	const unsigned char *q = *p;
	int tag, l;
	if (q + 2 > end) return -1;
	tag = *q++;
	l = *q++;
	if (l & 0x80) {
		int nb = l & 0x7f;
		if (nb < 1 || nb > 3 || q + nb > end) return -1;
		l = 0;
		while (nb--) l = (l << 8) | *q++;
	}
	if (l < 0 || q + l > end) return -1;
	*val = q;
	*len = l;
	*p = q + l;
	return tag;
}

static const unsigned char OID_RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
static const unsigned char OID_SHA256RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
static const unsigned char OID_SHA384RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
static const unsigned char OID_SHA512RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d };
static const unsigned char OID_SAN[] = { 0x55, 0x1d, 0x11 };
static const unsigned char OID_BC[] = { 0x55, 0x1d, 0x13 };

static int oid_is(const unsigned char *v, int l, const unsigned char *oid, int ol)
{
	return l == ol && !memcmp(v, oid, ol);
}

/* UTCTime / GeneralizedTime -> seconds since 1970 */
static long der_time(int tag, const unsigned char *v, int l)
{
	int y, mo, d, h, mi, s = 0, i = 0;
	static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	long days;
#define D2(k) ((v[k] - '0') * 10 + (v[(k) + 1] - '0'))
	if (tag == 0x17 && l >= 12) { y = D2(0); y += y < 50 ? 2000 : 1900; i = 2; }
	else if (tag == 0x18 && l >= 14) { y = D2(0) * 100 + D2(2); i = 4; }
	else return 0;
	mo = D2(i); d = D2(i + 2); h = D2(i + 4); mi = D2(i + 6);
	if (l >= i + 10 && v[i + 8] >= '0' && v[i + 8] <= '9') s = D2(i + 8);
#undef D2
	if (mo < 1 || mo > 12 || y < 1970) return 0;
	days = (y - 1970) * 365L + (y - 1969) / 4 + cum[mo - 1] + (d - 1);
	if (mo > 2 && y % 4 == 0) days++;
	return days * 86400L + h * 3600L + mi * 60L + s;
}

static int parse_cert(Cert *c, const unsigned char *der, int len)
{
	const unsigned char *p = der, *end = der + len, *v, *q, *qe, *w;
	int l, tag, wl;
	memset(c, 0, sizeof(*c));
	c->der = der; c->derlen = len;
	if (tlv(&p, end, &v, &l) != 0x30) return -1;          /* Certificate */
	p = v; end = v + l;
	c->tbs = p;
	if (tlv(&p, end, &v, &l) != 0x30) return -1;          /* TBSCertificate */
	c->tbslen = (int)(p - c->tbs);
	q = v; qe = v + l;
	/* signatureAlgorithm */
	if (tlv(&p, end, &v, &l) != 0x30) return -1;
	w = v;
	if (tlv(&w, v + l, &v, &wl) != 0x06) return -1;
	if (oid_is(v, wl, OID_SHA256RSA, 9)) c->sighash = 256;
	else if (oid_is(v, wl, OID_SHA384RSA, 9)) c->sighash = 384;
	else if (oid_is(v, wl, OID_SHA512RSA, 9)) c->sighash = 512;
	/* signatureValue BIT STRING */
	if (tlv(&p, end, &v, &l) != 0x03 || l < 2) return -1;
	c->sig = v + 1; c->siglen = l - 1;

	/* inside the TBS */
	tag = tlv(&q, qe, &v, &l);
	if (tag == 0xa0) tag = tlv(&q, qe, &v, &l);            /* [0] version */
	if (tag != 0x02) return -1;                             /* serial */
	if (tlv(&q, qe, &v, &l) != 0x30) return -1;            /* signature alg */
	c->issuer = q;
	if (tlv(&q, qe, &v, &l) != 0x30) return -1;
	c->issuerlen = (int)(q - c->issuer);
	if (tlv(&q, qe, &v, &l) != 0x30) return -1;            /* validity */
	w = v;
	{
		const unsigned char *we = v + l, *tv;
		int tl, tt;
		tt = tlv(&w, we, &tv, &tl); c->not_before = der_time(tt, tv, tl);
		tt = tlv(&w, we, &tv, &tl); c->not_after = der_time(tt, tv, tl);
	}
	c->subject = q;
	if (tlv(&q, qe, &v, &l) != 0x30) return -1;
	c->subjectlen = (int)(q - c->subject);
	c->spki = q;
	if (tlv(&q, qe, &v, &l) != 0x30) return -1;
	c->spkilen = (int)(q - c->spki);
	{
		/* SEQ { SEQ { OID rsaEncryption, NULL }, BIT STRING { SEQ { n, e } } } */
		const unsigned char *s = v, *se = v + l, *a, *b;
		int al, bl;
		if (tlv(&s, se, &a, &al) == 0x30) {
			const unsigned char *ao = a;
			const unsigned char *oid; int ol;
			if (tlv(&ao, a + al, &oid, &ol) == 0x06 && oid_is(oid, ol, OID_RSA, 9) &&
			    tlv(&s, se, &b, &bl) == 0x03 && bl > 1) {
				const unsigned char *k = b + 1, *ke = b + bl, *kv, *nv, *ev;
				int kl, nl, el;
				if (tlv(&k, ke, &kv, &kl) == 0x30) {
					const unsigned char *r = kv;
					if (tlv(&r, kv + kl, &nv, &nl) == 0x02 && tlv(&r, kv + kl, &ev, &el) == 0x02) {
						while (nl > 0 && *nv == 0) { nv++; nl--; }
						c->n = nv; c->nlen = nl; c->e = ev; c->elen = el;
					}
				}
			}
		}
	}
	/* optional [1] [2] unique ids, then [3] extensions */
	while (q < qe) {
		tag = tlv(&q, qe, &v, &l);
		if (tag < 0) return -1;
		if (tag == 0xa3) {
			const unsigned char *x = v, *xe = v + l, *ev;
			int exl;
			if (tlv(&x, xe, &ev, &exl) != 0x30) break;
			x = ev; xe = ev + exl;
			while (x < xe) {
				const unsigned char *e1, *e2, *oid, *val;
				int e1l, ol, vl, t;
				if (tlv(&x, xe, &e1, &e1l) != 0x30) break;
				e2 = e1;
				if (tlv(&e2, e1 + e1l, &oid, &ol) != 0x06) continue;
				t = tlv(&e2, e1 + e1l, &val, &vl);
				if (t == 0x01) t = tlv(&e2, e1 + e1l, &val, &vl);     /* critical */
				if (t != 0x04) continue;
				if (oid_is(oid, ol, OID_SAN, 3)) {
					const unsigned char *g = val, *gv; int gl;
					if (tlv(&g, val + vl, &gv, &gl) == 0x30) { c->san = gv; c->sanlen = gl; }
				} else if (oid_is(oid, ol, OID_BC, 3)) {
					const unsigned char *g = val, *gv, *bv; int gl, bl2;
					if (tlv(&g, val + vl, &gv, &gl) == 0x30 && gl > 0) {
						const unsigned char *h = gv;
						if (tlv(&h, gv + gl, &bv, &bl2) == 0x01 && bl2 == 1 && bv[0]) c->is_ca = 1;
					}
				}
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ RSA */

/* pmrsa.c: out = sig^e mod n, nlen bytes */
int rsa_public(const unsigned char *n, int nlen, const unsigned char *e, int elen,
               const unsigned char *sig, int siglen, unsigned char *out);

static void hash_n(int bits, const unsigned char *m, int len, unsigned char *out)
{
	hash_state h;
	if (bits == 384) { sha384_init(&h); sha384_process(&h, m, len); sha384_done(&h, out); }
	else if (bits == 512) { sha512_init(&h); sha512_process(&h, m, len); sha512_done(&h, out); }
	else { sha256_init(&h); sha256_process(&h, m, len); sha256_done(&h, out); }
}

/* PKCS #1 v1.5 signature over data (certificates) */
static int verify_pkcs1(const unsigned char *n, int nlen, const unsigned char *e, int elen,
                        int bits, const unsigned char *data, int dlen, const unsigned char *sig, int siglen)
{
	static const unsigned char di256[] = { 0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
	static const unsigned char di384[] = { 0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
	static const unsigned char di512[] = { 0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40 };
	static unsigned char em[512], want[512];
	unsigned char hash[64];
	const unsigned char *di = bits == 384 ? di384 : bits == 512 ? di512 : di256;
	int hl = bits / 8, dil = 19, pad;
	if (!bits || rsa_public(n, nlen, e, elen, sig, siglen, em) != 0) return -1;
	hash_n(bits, data, dlen, hash);
	pad = nlen - 3 - dil - hl;
	if (pad < 8) return -1;
	want[0] = 0; want[1] = 1;
	memset(want + 2, 0xff, pad);
	want[2 + pad] = 0;
	memcpy(want + 3 + pad, di, dil);
	memcpy(want + 3 + pad + dil, hash, hl);
	return memcmp(em, want, nlen) ? -1 : 0;
}

/* RSASSA-PSS with SHA-256, MGF1-SHA-256, salt of 32 bytes (RFC 8446 4.2.3) */
static int verify_pss(const Cert *c, const unsigned char *m, int mlen, const unsigned char *sig, int siglen)
{
	static unsigned char em[512], db[512];
	unsigned char mhash[32], h2[32], cnt[4], blk[32];
	hash_state h;
	int nbits, emlen, dblen, i, j, off;
	const unsigned char *hp;
	if (!c->n || c->nlen < 128) return -1;
	if (rsa_public(c->n, c->nlen, c->e, c->elen, sig, siglen, em) != 0) return -1;
	/* modBits - 1 bits of encoded message */
	nbits = c->nlen * 8;
	for (i = 0x80; i && !(c->n[0] & i); i >>= 1) nbits--;
	emlen = (nbits - 1 + 7) / 8;
	off = c->nlen - emlen;                     /* 0 or 1 leading zero byte */
	for (i = 0; i < off; i++) if (em[i]) return -1;
	if (em[off + emlen - 1] != 0xbc) return -1;
	dblen = emlen - 32 - 1;
	hp = em + off + dblen;
	/* MGF1 */
	for (i = 0, j = 0; i < dblen; j++) {
		int k;
		cnt[0] = (unsigned char)(j >> 24); cnt[1] = (unsigned char)(j >> 16); cnt[2] = (unsigned char)(j >> 8); cnt[3] = (unsigned char)j;
		sha256_init(&h); sha256_process(&h, hp, 32); sha256_process(&h, cnt, 4); sha256_done(&h, blk);
		for (k = 0; k < 32 && i < dblen; k++, i++) db[i] = em[off + i] ^ blk[k];
	}
	db[0] &= 0xff >> (8 * emlen - (nbits - 1));
	for (i = 0; i < dblen - 32 - 1; i++) if (db[i]) return -1;
	if (db[dblen - 32 - 1] != 0x01) return -1;
	sha256_init(&h); sha256_process(&h, m, mlen); sha256_done(&h, mhash);
	memset(blk, 0, 8);
	sha256_init(&h);
	sha256_process(&h, blk, 8);
	sha256_process(&h, mhash, 32);
	sha256_process(&h, db + dblen - 32, 32);
	sha256_done(&h, h2);
	return memcmp(h2, hp, 32) ? -1 : 0;
}

/* ------------------------------------------------------------- the chain */

static int name_eq(const unsigned char *a, int al, const unsigned char *b, int bl)
{
	return al == bl && !memcmp(a, b, al);
}

static int host_matches(const Cert *c, const char *host)
{
	const unsigned char *p = c->san, *end = c->san + c->sanlen, *v;
	int l, tag;
	if (!c->san) return 0;
	while (p < end && (tag = tlv(&p, end, &v, &l)) >= 0) {
		char name[128];
		if (tag != 0x82 || l <= 0 || l >= (int)sizeof(name)) continue;   /* dNSName */
		memcpy(name, v, l); name[l] = 0;
		if (!pm_strcasecmp(name, host)) return 1;
		if (name[0] == '*' && name[1] == '.') {
			const char *dot = strchr(host, '.');
			if (dot && dot != host && !pm_strcasecmp(dot + 1, name + 2)) return 1;
		}
	}
	return 0;
}

static void fingerprint(const Cert *c)
{
	unsigned char h[32];
	int i, k = 0;
	hash_n(256, c->spki, c->spkilen, h);
	for (i = 0; i < 32; i++) k += sprintf(g_fp + k, i ? ":%02X" : "%02X", h[i]);
}

static int known(const Cert *c)
{
	unsigned char h[32];
	int i;
	hash_n(256, c->der, c->derlen, h);
	for (i = 0; i < g_nknown; i++) if (!memcmp(g_known[i], h, 32)) return 1;
	return 0;
}

static void remember(const Cert *c)
{
	if (g_nknown < 8 && !known(c)) { hash_n(256, c->der, c->derlen, g_known[g_nknown]); g_nknown++; }
}

/* 1 if the chain from the leaf reaches a built-in root */
static int chain_ok(void)
{
	int cur = 0, depth, i, j;
	int path[MAX_CERTS], np = 0;
	for (depth = 0; depth < MAX_CERTS; depth++) {
		Cert *c = &g_cert[cur];
		path[np++] = cur;
		if (cur && known(c)) goto trusted;
		/* issued by a root we know? */
		for (i = 0; i < (int)(sizeof(k_roots) / sizeof(k_roots[0])); i++) {
			const Root *r = &k_roots[i];
			if (!name_eq(c->issuer, c->issuerlen, r->subject, r->subjectlen)) continue;
			pm_progress("Checking %s's certificate...", g_host);
			if (verify_pkcs1(r->n, r->nlen, r->e, r->elen, c->sighash, c->tbs, c->tbslen, c->sig, c->siglen) == 0)
				goto trusted;
		}
		/* or by the next certificate the server sent */
		for (j = 0; j < g_ncert; j++) {
			Cert *iss = &g_cert[j];
			if (j == cur || !name_eq(c->issuer, c->issuerlen, iss->subject, iss->subjectlen)) continue;
			if (!iss->n || !iss->is_ca) continue;
			pm_progress("Checking %s's certificate...", g_host);
			if (verify_pkcs1(iss->n, iss->nlen, iss->e, iss->elen, c->sighash, c->tbs, c->tbslen, c->sig, c->siglen) != 0)
				continue;
			break;
		}
		if (j == g_ncert) {
			snprintf(g_problem, sizeof(g_problem), "its certificate is not from an authority PsiMail knows");
			return 0;
		}
		cur = j;
	}
	snprintf(g_problem, sizeof(g_problem), "its certificate chain is too long");
	return 0;
trusted:
	for (i = 1; i < np; i++) remember(&g_cert[path[i]]);
	return 1;
}

const char *tlsv_certificate(const unsigned char *msg, int len)
{
	const unsigned char *p = msg, *end = msg + len;
	int ctx, list, n = 0;
	char hp[100];
	long now;
	g_ncert = 0;
	if (len < 4) return "bad certificate message";
	ctx = *p++;
	p += ctx;
	if (p + 3 > end) return "bad certificate message";
	list = (p[0] << 16) | (p[1] << 8) | p[2];
	p += 3;
	if (p + list > end) return "bad certificate message";
	end = p + list;
	while (p + 3 <= end && g_ncert < MAX_CERTS) {
		int cl = (p[0] << 16) | (p[1] << 8) | p[2], el;
		p += 3;
		if (p + cl + 2 > end) break;
		if (n + cl <= CHAIN_MAX) {
			memcpy(g_chain + n, p, cl);
			if (parse_cert(&g_cert[g_ncert], g_chain + n, cl) == 0) g_ncert++;
			else if (g_ncert == 0) return "could not read the server's certificate";
			n += cl;
		}
		p += cl;
		el = (p[0] << 8) | p[1];
		p += 2 + el;
	}
	if (!g_ncert) return "the server sent no certificate";
	if (!g_cert[0].n) return "the server's key is not RSA (PsiMail can only check RSA)";
	fingerprint(&g_cert[0]);

	now = pm_time();
	g_trusted = chain_ok();
	if (g_trusted && !host_matches(&g_cert[0], g_host)) {
		g_trusted = 0;
		snprintf(g_problem, sizeof(g_problem), "its certificate is for a different name");
	}
	/* dates only if the Psion's clock looks set (after 2024) */
	if (g_trusted && now > 1704067200L) {
		if (g_cert[0].not_after && now > g_cert[0].not_after) {
			g_trusted = 0;
			snprintf(g_problem, sizeof(g_problem), "its certificate has expired (is the Psion's clock right?)");
		} else if (g_cert[0].not_before && now + 86400L < g_cert[0].not_before) {
			g_trusted = 0;
			snprintf(g_problem, sizeof(g_problem), "its certificate is not valid yet (is the Psion's clock right?)");
		}
	}
	if (!g_trusted) {
		/* the user may have chosen to trust this server's key */
		snprintf(hp, sizeof(hp), "%s:%d", g_host, g_port);
		if (st_pin_check(hp, g_fp) == 1) { g_trusted = 1; g_problem[0] = 0; }
		else return "the server's certificate is not trusted";
	}
	g_problem[0] = 0;
	return 0;
}

const char *tlsv_verify(const unsigned char *msg, int len, const unsigned char th[32])
{
	static unsigned char content[64 + 34 + 32];
	int alg, sl;
	if (!g_ncert) return "CertificateVerify without a certificate";
	if (len < 4) return "bad CertificateVerify";
	alg = (msg[0] << 8) | msg[1];
	sl = (msg[2] << 8) | msg[3];
	if (4 + sl > len) return "bad CertificateVerify";
	if (alg != 0x0804) return "the server used a signature PsiMail can't check";
	memset(content, 0x20, 64);
	memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
	content[97] = 0;
	memcpy(content + 98, th, 32);
	pm_progress("Checking %s's signature...", g_host);
	if (verify_pss(&g_cert[0], content, 130, msg + 4, sl) != 0) {
		snprintf(g_problem, sizeof(g_problem), "its signature did not match its certificate");
		return "the server's signature is wrong";
	}
	g_proved = 1;
	return 0;
}
