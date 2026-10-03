/* tls13.c - a minimal TLS 1.3 client for psissh (PsiTerm's updater)
 *
 * Just enough TLS to download files from GitHub over HTTPS on a Psion 5mx:
 *   TLS 1.3 only, TLS_CHACHA20_POLY1305_SHA256, X25519 key exchange.
 * The server certificate is NOT checked (the Psion has no CA store and could
 * not afford the RSA/ECDSA maths). Downloads are instead protected by an
 * Ed25519 signature over the file, checked with a public key built into
 * PsiTerm - see update_verify() in psishim.c. TLS here only gets the bytes
 * through GitHub's HTTPS-only servers and protects them in transit.
 *
 * The server's Finished message IS checked, so a broken or truncated
 * handshake is detected.
 *
 * Uses Dropbear's crypto: libtomcrypt chacha/poly1305/sha256, Dropbear's
 * curve25519 and its random pool. I/O goes through the psiglue net_* calls,
 * so it works over the modem (ATDT host:443) and the Psion's own TCP/IP.
 */
#include <string.h>
#include "includes.h"
#include "dbrandom.h"
#include "curve25519.h"
#include "tls13.h"

#ifdef TLS_VERIFY
/* PsiMail checks the server's certificate chain and its CertificateVerify
   signature (mail/engine/certcheck.c); PsiTerm and PsiWeb don't */
extern void tlsv_start(void);
extern const char *tlsv_certificate(const unsigned char *msg, int len);
extern const char *tlsv_verify(const unsigned char *msg, int len, const unsigned char th[32]);
extern int tlsv_ok(void);
#endif

/* psiglue */
extern int pg_net_avail(void);
extern int pg_net_read(void *buf, int max);
extern int pg_serial_write(const void *buf, int len);
extern int pg_wait(int ms, int want_net, int want_kbd);

#define REC_MAX   (16384 + 256 + 5)
#define HS_MAX    16384              /* longest handshake message we keep */

static unsigned char g_rec[REC_MAX];     /* one record being read */
static unsigned char g_app[16384 + 256]; /* decrypted application data */
static int g_app_pos, g_app_len;
static int g_closed;

typedef struct {
	unsigned char key[32];
	unsigned char iv[12];
	unsigned long long seq;
} tls_dir;

static tls_dir g_rd, g_wr;               /* read (server) / write (client) keys */
static int g_encrypted;
static const char *g_err;

/* ---------------------------------------------------------------- SHA-256 / HMAC / HKDF */

static void sha256_buf(const unsigned char *in, unsigned long len, unsigned char out[32])
{
	hash_state h;
	sha256_init(&h);
	sha256_process(&h, in, len);
	sha256_done(&h, out);
}

static void hmac_sha256(const unsigned char *key, int keylen,
                        const unsigned char *m1, int l1, const unsigned char *m2, int l2,
                        unsigned char out[32])
{
	unsigned char k[64], pad[64], inner[32];
	hash_state h;
	int i;
	memset(k, 0, sizeof(k));
	if (keylen > 64) { sha256_buf(key, keylen, k); } else memcpy(k, key, keylen);
	for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
	sha256_init(&h);
	sha256_process(&h, pad, 64);
	if (l1) sha256_process(&h, m1, l1);
	if (l2) sha256_process(&h, m2, l2);
	sha256_done(&h, inner);
	for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
	sha256_init(&h);
	sha256_process(&h, pad, 64);
	sha256_process(&h, inner, 32);
	sha256_done(&h, out);
}

static void hkdf_extract(const unsigned char *salt, const unsigned char *ikm, int ikmlen, unsigned char out[32])
{
	hmac_sha256(salt, 32, ikm, ikmlen, NULL, 0, out);
}

/* HKDF-Expand-Label(secret, label, context, length), length <= 32 */
static void hkdf_label(const unsigned char secret[32], const char *label,
                       const unsigned char *ctx, int ctxlen, unsigned char *out, int len)
{
	unsigned char info[100], t[32];
	int n = 0, ll = (int)strlen(label);
	info[n++] = 0; info[n++] = (unsigned char)len;
	info[n++] = (unsigned char)(6 + ll);
	memcpy(info + n, "tls13 ", 6); n += 6;
	memcpy(info + n, label, ll); n += ll;
	info[n++] = (unsigned char)ctxlen;
	if (ctxlen) { memcpy(info + n, ctx, ctxlen); n += ctxlen; }
	info[n++] = 1;                             /* T(1) counter */
	hmac_sha256(secret, 32, info, n, NULL, 0, t);
	memcpy(out, t, len);
}

static void set_keys(tls_dir *d, const unsigned char secret[32])
{
	hkdf_label(secret, "key", NULL, 0, d->key, 32);
	hkdf_label(secret, "iv", NULL, 0, d->iv, 12);
	d->seq = 0;
}

/* ---------------------------------------------------------------- ChaCha20-Poly1305 (RFC 8439) */

static void aead_nonce(const tls_dir *d, unsigned char nonce[12])
{
	int i;
	memcpy(nonce, d->iv, 12);
	for (i = 0; i < 8; i++)
		nonce[11 - i] ^= (unsigned char)(d->seq >> (8 * i));
}

static void poly_tag(chacha_state *cs, const unsigned char *aad, int aadlen,
                     const unsigned char *ct, int ctlen, unsigned char tag[16])
{
	unsigned char otk[64], pad[16], lens[16];
	poly1305_state p;
	unsigned long taglen = 16;
	int i;
	memset(otk, 0, sizeof(otk));
	chacha_keystream(cs, otk, 64);             /* block 0: one-time Poly1305 key */
	poly1305_init(&p, otk, 32);
	memset(pad, 0, sizeof(pad));
	poly1305_process(&p, aad, aadlen);
	if (aadlen % 16) poly1305_process(&p, pad, 16 - aadlen % 16);
	poly1305_process(&p, ct, ctlen);
	if (ctlen % 16) poly1305_process(&p, pad, 16 - ctlen % 16);
	for (i = 0; i < 8; i++) { lens[i] = (unsigned char)((unsigned long long)aadlen >> (8 * i));
	                          lens[8 + i] = (unsigned char)((unsigned long long)ctlen >> (8 * i)); }
	poly1305_process(&p, lens, 16);
	poly1305_done(&p, tag, &taglen);
}

/* encrypt in place: buf holds len bytes of plaintext, 16 bytes of room after */
static void aead_seal(tls_dir *d, const unsigned char *aad, int aadlen, unsigned char *buf, int len)
{
	chacha_state cs;
	unsigned char nonce[12];
	aead_nonce(d, nonce);
	chacha_setup(&cs, d->key, 32, 20);
	chacha_ivctr32(&cs, nonce, 12, 0);
	{
		unsigned char otk[64];
		chacha_keystream(&cs, otk, 64);        /* skip block 0 */
	}
	chacha_ivctr32(&cs, nonce, 12, 1);
	chacha_crypt(&cs, buf, len, buf);
	chacha_ivctr32(&cs, nonce, 12, 0);
	poly_tag(&cs, aad, aadlen, buf, len, buf + len);
	d->seq++;
}

/* decrypt in place; returns 0 if the tag is good */
static int aead_open(tls_dir *d, const unsigned char *aad, int aadlen, unsigned char *buf, int len)
{
	chacha_state cs;
	unsigned char nonce[12], tag[16];
	int i, diff = 0;
	if (len < 16) return -1;
	len -= 16;
	aead_nonce(d, nonce);
	chacha_setup(&cs, d->key, 32, 20);
	chacha_ivctr32(&cs, nonce, 12, 0);
	poly_tag(&cs, aad, aadlen, buf, len, tag);
	for (i = 0; i < 16; i++) diff |= tag[i] ^ buf[len + i];
	if (diff) return -1;
	chacha_ivctr32(&cs, nonce, 12, 1);
	chacha_crypt(&cs, buf, len, buf);
	d->seq++;
	return 0;
}

/* ---------------------------------------------------------------- raw record I/O */

static int read_exact(unsigned char *buf, int n, int timeout_ms)
{
	int k = 0;
	while (k < n) {
		if (pg_net_avail() == 0) {
			int m = pg_wait(timeout_ms, 1, 0);
			if (m & 8) { g_err = "cancelled"; return -2; }
			if (pg_net_avail() == 0) { g_err = k ? "connection dropped" : "no reply from server"; return -1; }
		}
		k += pg_net_read(buf + k, n - k);
	}
	return 0;
}

/* Reads one record. Returns its content type (after decryption, the inner
   type) with the payload in g_rec[0..*len), or <0 on error. */
static int g_first;                      /* no record read yet on this connection */
static char g_junk[48];                  /* what came before the first record, for errors */

static int read_record(int *len, int timeout_ms)
{
	unsigned char hdr[5];
	int n, r;
	for (;;) {
		if (g_first) {
			/* The modem's "CONNECT ..." line can leave a CR/LF (or more text)
			   ahead of the server's first record: skip up to 64 bytes of it */
			int skipped = 0, jl = 0;
			g_first = 0;
			for (;;) {
				if ((r = read_exact(hdr, 1, timeout_ms)) < 0) return r;
				if (hdr[0] >= 20 && hdr[0] <= 23) break;   /* a TLS content type */
				if (jl < (int)sizeof(g_junk) - 4) { sprintf(g_junk + jl, " %02x", hdr[0]); jl += 3; }
				if (++skipped > 64) { g_err = "unexpected reply (not TLS?)"; return -1; }
			}
			if ((r = read_exact(hdr + 1, 4, timeout_ms)) < 0) return r;
		} else if ((r = read_exact(hdr, 5, timeout_ms)) < 0) return r;
		if (hdr[1] != 3) {
			static char e[96];
			sprintf(e, "unexpected reply (not TLS?):%.40s %02x %02x %02x", g_junk, hdr[0], hdr[1], hdr[2]);
			g_err = e;
			return -1;
		}
		n = (hdr[3] << 8) | hdr[4];
		if (n > REC_MAX - 5) { g_err = "record too big"; return -1; }
		if ((r = read_exact(g_rec, n, timeout_ms)) < 0) return r;
		if (hdr[0] == 20) continue;            /* ChangeCipherSpec: ignored in 1.3 */
		if (!g_encrypted) {
			*len = n;
			return hdr[0];
		}
		/* once the keys are on, everything but CCS comes encrypted (RFC 8446
		   section 5): a plaintext alert or handshake record here is an
		   injection, not the server */
		if (hdr[0] != 23) { g_err = "unexpected plaintext record"; return -1; }
		if (aead_open(&g_rd, hdr, 5, g_rec, n) != 0) { g_err = "data damaged in transit"; return -1; }
		n -= 16;
		while (n > 0 && g_rec[n - 1] == 0) n--; /* padding */
		if (n == 0) { g_err = "bad record"; return -1; }
		*len = n - 1;
		return g_rec[n - 1];
	}
}

static unsigned char g_out[5 + 16384 + 1 + 16];   /* one record being sent */

static int write_record(int type, const unsigned char *data, int len)
{
	unsigned char *out = g_out;
	if (len > 16384) return -1;
	if (!g_encrypted) {
		out[0] = (unsigned char)type; out[1] = 3; out[2] = (type == 22) ? 1 : 3;
		out[3] = (unsigned char)(len >> 8); out[4] = (unsigned char)len;
		memcpy(out + 5, data, len);
		return pg_serial_write(out, 5 + len) == 5 + len ? 0 : -1;
	}
	memcpy(out + 5, data, len);
	out[5 + len] = (unsigned char)type;        /* inner content type */
	len += 1;
	out[0] = 23; out[1] = 3; out[2] = 3;
	out[3] = (unsigned char)((len + 16) >> 8); out[4] = (unsigned char)(len + 16);
	aead_seal(&g_wr, out, 5, out + 5, len);
	return pg_serial_write(out, 5 + len + 16) == 5 + len + 16 ? 0 : -1;
}

/* ---------------------------------------------------------------- handshake */

static void put16(unsigned char *p, int v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }

/* Forgets the connection: the traffic keys, and every buffer that held
   plaintext or key material. Called after each connection by the users of
   this file, and by tls_connect before the next one. */
void tls_close(void)
{
	memset(&g_rd, 0, sizeof(g_rd));
	memset(&g_wr, 0, sizeof(g_wr));
	memset(g_app, 0, sizeof(g_app));
	memset(g_rec, 0, sizeof(g_rec));
	memset(g_out, 0, sizeof(g_out));
	g_encrypted = 0;
	g_app_pos = g_app_len = 0;
	g_closed = 1;
}

int tls_connect(const char *host, char *why, int whymax)
{
	static unsigned char ch[512];
	static unsigned char hs[HS_MAX];     /* handshake bytes not yet parsed */
	unsigned char priv[32], pub[32], shared[32], zero[32], secret[32], derived[32];
	unsigned char hs_secret[32], c_hs[32], s_hs[32], th[32], fin[32], empty_hash[32];
	unsigned char server_pub[32];
	unsigned char master[32], c_ap[32], s_ap[32], fkey[32], msg[36];
	hash_state transcript, tmp;
	int n = 0, ext_start, hlen = 0, got_sh = 0, type, len;
	int hn = (int)strlen(host);
	int ret = -1;
	static const unsigned char basepoint[32] = { 9 };

	tls_close();                         /* nothing of the last connection survives */
	g_err = "handshake failed";
	g_encrypted = 0;
	g_closed = 0;
	g_first = 1;
	g_junk[0] = 0;
	g_app_pos = g_app_len = 0;
#ifdef TLS_VERIFY
	tlsv_start();
#endif
	memset(zero, 0, 32);
	sha256_buf((const unsigned char *)"", 0, empty_hash);

	/* key pair */
	genrandom(priv, 32);
	priv[0] &= 248; priv[31] &= 127; priv[31] |= 64;
	dropbear_curve25519_scalarmult(pub, priv, basepoint);

	/* ---- ClientHello */
	ch[n++] = 1; n += 3;                         /* type, length (later) */
	ch[n++] = 3; ch[n++] = 3;                    /* legacy_version */
	genrandom(ch + n, 32); n += 32;              /* random */
	ch[n++] = 32; genrandom(ch + n, 32); n += 32;/* legacy session id (middlebox compat) */
	put16(ch + n, 2); n += 2; ch[n++] = 0x13; ch[n++] = 0x03;   /* TLS_CHACHA20_POLY1305_SHA256 */
	ch[n++] = 1; ch[n++] = 0;                    /* no compression */
	ext_start = n; n += 2;
	/* server_name */
	put16(ch + n, 0); put16(ch + n + 2, hn + 5); put16(ch + n + 4, hn + 3);
	ch[n + 6] = 0; put16(ch + n + 7, hn); memcpy(ch + n + 9, host, hn); n += 9 + hn;
	/* supported_groups: x25519 */
	put16(ch + n, 10); put16(ch + n + 2, 4); put16(ch + n + 4, 2); put16(ch + n + 6, 0x001d); n += 8;
	/* signature_algorithms (we don't check the signature, but must offer some) */
	{
#ifdef TLS_VERIFY
		/* RSA-PSS for CertificateVerify; PKCS#1 v1.5 for certificates */
		static const unsigned short sa[] = { 0x0804, 0x0401, 0x0501, 0x0601 };
#else
		static const unsigned short sa[] = { 0x0403, 0x0804, 0x0401, 0x0503, 0x0805, 0x0501, 0x0806, 0x0601, 0x0807 };
#endif
		int k, cnt = sizeof(sa) / sizeof(sa[0]);
		put16(ch + n, 13); put16(ch + n + 2, 2 + 2 * cnt); put16(ch + n + 4, 2 * cnt); n += 6;
		for (k = 0; k < cnt; k++) { put16(ch + n, sa[k]); n += 2; }
	}
	/* supported_versions: TLS 1.3 */
	put16(ch + n, 43); put16(ch + n + 2, 3); ch[n + 4] = 2; put16(ch + n + 5, 0x0304); n += 7;
	/* key_share: x25519 */
	put16(ch + n, 51); put16(ch + n + 2, 38); put16(ch + n + 4, 36); put16(ch + n + 6, 0x001d);
	put16(ch + n + 8, 32); memcpy(ch + n + 10, pub, 32); n += 42;
	put16(ch + ext_start, n - ext_start - 2);
	ch[1] = 0; put16(ch + 2, n - 4);

	sha256_init(&transcript);
	sha256_process(&transcript, ch, n);
	if (write_record(22, ch, n) != 0) { g_err = "could not send"; goto fail; }

	/* ---- ServerHello (plaintext) */
	for (;;) {
		type = read_record(&len, 30000);
		if (type < 0) goto fail;
		if (type == 21) { g_err = "server refused the connection (TLS alert)"; goto fail; }
		if (type != 22) { g_err = "unexpected reply (not TLS?)"; goto fail; }
		if (hlen + len > HS_MAX) { g_err = "handshake too big"; goto fail; }
		memcpy(hs + hlen, g_rec, len); hlen += len;
		if (hlen >= 4) {
			int mlen = (hs[1] << 16) | (hs[2] << 8) | hs[3];
			if (hlen >= 4 + mlen) break;
		}
	}
	{
		int mlen = (hs[1] << 16) | (hs[2] << 8) | hs[3], p = 4, sid, el, end;
		static const unsigned char hrr[32] = {
			0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
			0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C };
		if (hs[0] != 2) { g_err = "expected ServerHello"; goto fail; }
		if (mlen < 38 || 4 + mlen > hlen) { g_err = "bad ServerHello"; goto fail; }
		if (!memcmp(hs + 6, hrr, 32)) { g_err = "server wants a different key exchange"; goto fail; }
		p += 2 + 32;
		sid = hs[p]; p += 1 + sid;
		if (p + 5 > 4 + mlen) { g_err = "bad ServerHello"; goto fail; }
		if (hs[p] != 0x13 || hs[p + 1] != 0x03) { g_err = "server chose an unsupported cipher"; goto fail; }
		p += 3;
		el = (hs[p] << 8) | hs[p + 1]; p += 2;
		end = p + el;
		if (end > 4 + mlen) goto fail;
		while (p + 4 <= end) {
			int et = (hs[p] << 8) | hs[p + 1], elen = (hs[p + 2] << 8) | hs[p + 3];
			p += 4;
			if (p + elen > end) goto fail;
			if (et == 51 && elen >= 36 && ((hs[p] << 8) | hs[p + 1]) == 0x001d && hs[p + 3] == 32) {
				memcpy(server_pub, hs + p + 4, 32);
				got_sh = 1;
			}
			if (et == 43 && !(elen == 2 && hs[p] == 3 && hs[p + 1] == 4)) { g_err = "server does not speak TLS 1.3"; goto fail; }
			p += elen;
		}
		if (!got_sh) { g_err = "server did not send an X25519 key"; goto fail; }
		sha256_process(&transcript, hs, 4 + mlen);
		memmove(hs, hs + 4 + mlen, hlen - 4 - mlen);
		hlen -= 4 + mlen;
		if (hlen) { g_err = "unexpected data after ServerHello"; goto fail; }
	}

	/* ---- key schedule up to the handshake keys */
	dropbear_curve25519_scalarmult(shared, priv, server_pub);
	hkdf_extract(zero, zero, 32, secret);                        /* early secret */
	hkdf_label(secret, "derived", empty_hash, 32, derived, 32);
	hkdf_extract(derived, shared, 32, hs_secret);                /* handshake secret */
	tmp = transcript; sha256_done(&tmp, th);
	hkdf_label(hs_secret, "c hs traffic", th, 32, c_hs, 32);
	hkdf_label(hs_secret, "s hs traffic", th, 32, s_hs, 32);
	set_keys(&g_rd, s_hs);
	set_keys(&g_wr, c_hs);
	g_encrypted = 1;

	/* ---- EncryptedExtensions, Certificate, CertificateVerify, Finished */
	for (;;) {
		int mt, mlen;
		while (hlen < 4 || hlen < 4 + ((hs[1] << 16) | (hs[2] << 8) | hs[3])) {
			type = read_record(&len, 30000);
			if (type < 0) goto fail;
			if (type == 21) { g_err = "server refused the connection (TLS alert)"; goto fail; }
			if (type != 22) { g_err = "unexpected record during handshake"; goto fail; }
			if (hlen + len > HS_MAX) { g_err = "handshake message too big"; goto fail; }
			memcpy(hs + hlen, g_rec, len); hlen += len;
		}
		mt = hs[0];
		mlen = (hs[1] << 16) | (hs[2] << 8) | hs[3];
		if (mt == 20) {                                          /* server Finished */
			tmp = transcript; sha256_done(&tmp, th);
			hkdf_label(s_hs, "finished", NULL, 0, fkey, 32);
			hmac_sha256(fkey, 32, th, 32, NULL, 0, fin);
			if (mlen != 32 || memcmp(fin, hs + 4, 32)) { g_err = "server Finished did not verify"; goto fail; }
#ifdef TLS_VERIFY
			if (!tlsv_ok()) { g_err = "the server did not prove who it is"; goto fail; }
#endif
			sha256_process(&transcript, hs, 4 + mlen);
			memmove(hs, hs + 4 + mlen, hlen - 4 - mlen);
			hlen -= 4 + mlen;
			break;
		}
		if (mt != 8 && mt != 11 && mt != 15) { g_err = "unexpected handshake message"; goto fail; }
#ifdef TLS_VERIFY
		if (mt == 11) {
			const char *e = tlsv_certificate(hs + 4, mlen);
			if (e) { g_err = e; goto fail; }
		} else if (mt == 15) {
			const char *e;
			tmp = transcript; sha256_done(&tmp, th);   /* up to the Certificate */
			e = tlsv_verify(hs + 4, mlen, th);
			if (e) { g_err = e; goto fail; }
		}
#endif
		sha256_process(&transcript, hs, 4 + mlen);   /* certificate contents not checked - see top */
		memmove(hs, hs + 4 + mlen, hlen - 4 - mlen);
		hlen -= 4 + mlen;
	}

	/* ---- application keys (transcript up to server Finished) */
	tmp = transcript; sha256_done(&tmp, th);
	hkdf_label(hs_secret, "derived", empty_hash, 32, derived, 32);
	hkdf_extract(derived, zero, 32, master);
	hkdf_label(master, "c ap traffic", th, 32, c_ap, 32);
	hkdf_label(master, "s ap traffic", th, 32, s_ap, 32);

	/* client Finished, still under the handshake keys */
	{
		static const unsigned char ccs[6] = { 20, 3, 3, 0, 1, 1 };
		pg_serial_write(ccs, 6);                         /* middlebox compatibility */
		hkdf_label(c_hs, "finished", NULL, 0, fkey, 32);
		hmac_sha256(fkey, 32, th, 32, NULL, 0, fin);
		msg[0] = 20; msg[1] = 0; msg[2] = 0; msg[3] = 32;
		memcpy(msg + 4, fin, 32);
		if (write_record(22, msg, 36) != 0) { g_err = "could not send"; goto fail; }
	}
	set_keys(&g_rd, s_ap);
	set_keys(&g_wr, c_ap);
	ret = 0;
	goto done;

fail:
	if (why) {
		strncpy(why, g_err ? g_err : "TLS failed", whymax - 1);
		why[whymax - 1] = 0;
	}
	tls_close();                         /* no keys left from a handshake that failed */
done:
	/* every secret of the handshake goes, whichever way it ended: only the
	   traffic keys in g_rd/g_wr remain, until tls_close */
	memset(priv, 0, sizeof(priv));
	memset(shared, 0, sizeof(shared));
	memset(secret, 0, sizeof(secret));
	memset(derived, 0, sizeof(derived));
	memset(hs_secret, 0, sizeof(hs_secret));
	memset(c_hs, 0, sizeof(c_hs));
	memset(s_hs, 0, sizeof(s_hs));
	memset(master, 0, sizeof(master));
	memset(c_ap, 0, sizeof(c_ap));
	memset(s_ap, 0, sizeof(s_ap));
	memset(fkey, 0, sizeof(fkey));
	memset(fin, 0, sizeof(fin));
	memset(msg, 0, sizeof(msg));
	memset(&transcript, 0, sizeof(transcript));
	memset(&tmp, 0, sizeof(tmp));
	memset(hs, 0, sizeof(hs));
	memset(ch, 0, sizeof(ch));
	return ret;
}

int tls_write(const void *buf, int len)
{
	const unsigned char *p = (const unsigned char *)buf;
	while (len > 0) {
		int k = len > 4096 ? 4096 : len;
		if (write_record(23, p, k) != 0) return -1;
		p += k; len -= k;
	}
	return 0;
}

/* Returns bytes read (>0), 0 when the server has closed, -1 on timeout or
   error, -2 if cancelled. */
int tls_read(void *buf, int max, int timeout_ms)
{
	while (g_app_pos >= g_app_len) {
		int type, len;
		if (g_closed) return 0;
		type = read_record(&len, timeout_ms);
		if (type < 0) {
			/* server closed the TCP connection without close_notify */
			if (!strcmp(g_err, "no reply from server") && pg_net_avail() == 0)
				return -1;
			return type;
		}
		if (type == 23) {
			memcpy(g_app, g_rec, len);
			g_app_pos = 0;
			g_app_len = len;
		} else if (type == 21) {                  /* alert: close_notify or error */
			g_closed = 1;
			return 0;
		}
		/* type 22 after the handshake: NewSessionTicket etc. - ignored */
	}
	{
		int k = g_app_len - g_app_pos;
		if (k > max) k = max;
		memcpy(buf, g_app + g_app_pos, k);
		g_app_pos += k;
		return k;
	}
}

const char *tls_error(void) { return g_err ? g_err : ""; }

/* decrypted bytes waiting to be read (PsiWeb polls without blocking) */
int tls_pending(void) { return g_app_pos < g_app_len; }
