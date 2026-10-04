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

#ifdef TLS_FAST_CRYPTO
/* web/tls/sha256.c instead of libtomcrypt's (LTC_SMALL_CODE: slow) */
#define hash_state tls_sha256
#define sha256_init tls_sha256_init
#define sha256_process tls_sha256_process
#define sha256_done tls_sha256_done
#endif

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

/* TLS_FAST_X25519 (PsiWeb, PsiMail: web/tls, mail/tls includes.h): X25519
   from web/tls/fe25519.c, about twice as fast on the ARM710 as Dropbear's,
   and each connection's key pair is worked out ahead, a few ladder steps at
   a time while this file waits for the server (see read_exact), so a
   connection usually costs one X25519 instead of two. psissh (PsiTerm)
   keeps Dropbear's. */
#ifdef TLS_FAST_X25519
extern void tls_x25519(unsigned char *q, const unsigned char *n, const unsigned char *u);
extern void tls_x25519_kg_start(const unsigned char priv[32]);
extern int tls_x25519_kg_steps(int n);
extern void tls_x25519_kg_finish(unsigned char pub[32]);
#define X25519(q, n, u) tls_x25519(q, n, u)
#define KG_CHUNK 24                   /* ladder steps between looks at the network (~25 ms) */
static unsigned char g_kg_priv[32];  /* the next connection's private key */
static int g_kg_on, g_kg_done;       /* being worked out / finished */

static void kg_next(void)
{
	genrandom(g_kg_priv, 32);
	tls_x25519_kg_start(g_kg_priv);
	g_kg_on = 1;
	g_kg_done = 0;
}

/* the key pair for this connection: the one worked out ahead if there is
   one (finished here if need be). tls_connect starts the next one once the
   ClientHello is out (so the random numbers are drawn in the same order as
   before: the harness's recorded TLS sessions still play back). */
static void kg_take(unsigned char priv[32], unsigned char pub[32])
{
	if (!g_kg_on) kg_next();
	tls_x25519_kg_finish(pub);
	memcpy(priv, g_kg_priv, 32);
	memset(g_kg_priv, 0, sizeof(g_kg_priv));
	g_kg_on = 0;
}

/* Some of the next key's work, for an engine with nothing else to do
   (optional: tls13.c does it itself while it waits for a server). Returns
   1 while there is more to do. */
int tls_background(void)
{
	if (!g_kg_on || g_kg_done) return 0;
	g_kg_done = tls_x25519_kg_steps(KG_CHUNK);
	return !g_kg_done;
}
#else
#define X25519(q, n, u) dropbear_curve25519_scalarmult(q, n, u)
#endif

/* TLS_RESUME (PsiWeb, PsiMail): TLS 1.3 session resumption (RFC 8446 2.2,
   4.6.1). A server's NewSessionTickets are kept in memory, for this run of
   the engine only, by host and port; the next connection there offers one
   (psk_dhe_ke only: a new X25519 exchange every time, so forward secrecy is
   kept) and, if the server takes it, skips the certificate, its signature
   check and the chain check. Each ticket is used once. A ticket is only
   stored from a connection whose handshake was complete (and, in PsiMail,
   whose certificate was checked and trusted, or which was itself resumed
   from such a ticket): the PSK is bound to that proof. */
#ifdef TLS_RESUME
#include <time.h>
#include "psishared.h"
extern PsiShared *pg_shared(void);
extern int tls_rng_repeatable(void);    /* pwrandom.c, pmrandom.c */
#define TK_N      4                  /* tickets kept, all hosts together */
#define TK_MAX    1024               /* longest ticket kept */
#define TK_LIFE   (24L * 3600)       /* at most a day, whatever the server says */
typedef struct {
	char host[64];
	int port;
	int len;                         /* 0 = slot free */
	unsigned long got, life, age_add;
	unsigned char psk[32];
	unsigned char t[TK_MAX];
} tls_ticket;
static tls_ticket g_tk[TK_N];
static unsigned char g_res[32];      /* resumption_master_secret of this connection */
static int g_res_ok;                 /* tickets from this connection may be kept */
static char g_res_host[64];
static int g_res_port;
static int g_resumed;                /* this connection was resumed */

static int cur_port(void) { return pg_shared() ? pg_shared()->port : 0; }

/* 1 if the last tls_connect resumed a session (no certificate was sent) */
int tls_resumed(void) { return g_resumed; }

/* Forgets every ticket and the next key (e.g. when the engine is told to
   forget a server). */
void tls_forget(void)
{
	memset(g_tk, 0, sizeof(g_tk));
#ifdef TLS_FAST_X25519
	memset(g_kg_priv, 0, sizeof(g_kg_priv));
	g_kg_on = 0;
#endif
}
#endif

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

#ifdef TLS_FAST_CRYPTO
/* web/tls/aead.c: ChaCha20 in ARM assembler, Poly1305 without 64-bit
   multiplies (libtomcrypt's below are about 3 times slower on the ARM710) */
extern void tls_aead_seal(const unsigned char key[32], const unsigned char nonce[12],
                          const unsigned char *aad, int aadlen, unsigned char *buf, int len);
extern int tls_aead_open(const unsigned char key[32], const unsigned char nonce[12],
                         const unsigned char *aad, int aadlen, unsigned char *buf, int len);

static void aead_seal(tls_dir *d, const unsigned char *aad, int aadlen, unsigned char *buf, int len)
{
	unsigned char nonce[12];
	aead_nonce(d, nonce);
	tls_aead_seal(d->key, nonce, aad, aadlen, buf, len);
	d->seq++;
}

static int aead_open(tls_dir *d, const unsigned char *aad, int aadlen, unsigned char *buf, int len)
{
	unsigned char nonce[12];
	aead_nonce(d, nonce);
	if (tls_aead_open(d->key, nonce, aad, aadlen, buf, len) != 0) return -1;
	d->seq++;
	return 0;
}
#else
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
#endif

/* ---------------------------------------------------------------- raw record I/O */

static int read_exact(unsigned char *buf, int n, int timeout_ms)
{
	int k = 0;
	while (k < n) {
#ifdef TLS_FAST_X25519
		if (g_kg_on && !g_kg_done && pg_net_avail() < n - k) {
			/* not all here yet: work on the next connection's key pair
			   while the rest comes in (the port and TCP buffer it),
			   taking what has come between pieces */
			if (pg_wait(1, 1, 0) & 8) { g_err = "cancelled"; return -2; }
			if (pg_net_avail() < n - k) g_kg_done = tls_x25519_kg_steps(KG_CHUNK);
			k += pg_net_read(buf + k, n - k);
			continue;
		}
#endif
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
#ifdef TLS_RESUME
	memset(g_res, 0, sizeof(g_res));
	g_res_ok = 0;
#endif
}

int tls_connect(const char *host, char *why, int whymax)
{
#ifdef TLS_RESUME
	static unsigned char ch[600 + TK_MAX];
	tls_ticket *tk = NULL;
	unsigned char psk_early[32];
	int resumed = 0, binders_at = 0;
#else
	static unsigned char ch[512];
#endif
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
#ifdef TLS_FAST_X25519
	kg_take(priv, pub);
#else
	genrandom(priv, 32);
	priv[0] &= 248; priv[31] &= 127; priv[31] |= 64;
	dropbear_curve25519_scalarmult(pub, priv, basepoint);
#endif
#ifdef TLS_RESUME
	g_resumed = 0;
	/* a name too long to keep whole is never resumed (a cut name could
	   match another server's) */
	if (hn < (int)sizeof(g_res_host)) strcpy(g_res_host, host);
	else g_res_host[0] = 0;
	g_res_port = cur_port();
	if (g_res_host[0]) {	/* a ticket for this server, still in date (used once) */
		unsigned long now = (unsigned long)time(NULL);
		int i;
		for (i = 0; i < TK_N; i++) {
			tls_ticket *t = &g_tk[i];
			if (!t->len) continue;
			if (now - t->got >= t->life || now < t->got) { memset(t, 0, sizeof(*t)); continue; }
			if (!strcmp(t->host, g_res_host) && t->port == g_res_port && (!tk || t->got > tk->got)) tk = t;
		}
	}
#endif

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
#ifdef TLS_RESUME
	{
		/* psk_key_exchange_modes: psk_dhe_ke only. Sent on every
		   connection: BoringSSL servers (Google's) give tickets only to a
		   client that lists a mode. Left out only in the ARM harness's
		   repeatable runs (no entropy at all: see pwrandom.c), so that
		   the TLS sessions recorded before it was added still play back */
		if (tk || !tls_rng_repeatable()) {
			put16(ch + n, 45); put16(ch + n + 2, 2); ch[n + 4] = 1; ch[n + 5] = 1; n += 6;
		}
	}
	if (tk) {
		/* pre_shared_key (must be last): one identity, one binder */
		{
			unsigned long age = ((unsigned long)time(NULL) - tk->got) * 1000UL + tk->age_add;
			int il = 2 + tk->len + 4;
			put16(ch + n, 41); put16(ch + n + 2, 2 + il + 2 + 33); n += 4;
			put16(ch + n, il); n += 2;
			put16(ch + n, tk->len); memcpy(ch + n + 2, tk->t, tk->len); n += 2 + tk->len;
			ch[n] = (unsigned char)(age >> 24); ch[n + 1] = (unsigned char)(age >> 16);
			ch[n + 2] = (unsigned char)(age >> 8); ch[n + 3] = (unsigned char)age; n += 4;
			binders_at = n;
			n += 2 + 33;                         /* binders, filled in below */
		}
	}
#endif
	put16(ch + ext_start, n - ext_start - 2);
	ch[1] = 0; put16(ch + 2, n - 4);
#ifdef TLS_RESUME
	if (tk) {
		/* binder = HMAC(finished key of "res binder", hash of the ClientHello up to the binders) */
		unsigned char bk[32];
		hkdf_extract(zero, tk->psk, 32, psk_early);
		hkdf_label(psk_early, "res binder", empty_hash, 32, bk, 32);
		hkdf_label(bk, "finished", NULL, 0, fkey, 32);
		sha256_buf(ch, binders_at, th);
		put16(ch + binders_at, 33); ch[binders_at + 2] = 32;
		hmac_sha256(fkey, 32, th, 32, NULL, 0, ch + binders_at + 3);
		memset(bk, 0, sizeof(bk));
		memset(tk, 0, sizeof(*tk));          /* a ticket is used once */
	}
#endif

	sha256_init(&transcript);
	sha256_process(&transcript, ch, n);
	if (write_record(22, ch, n) != 0) { g_err = "could not send"; goto fail; }
#ifdef TLS_FAST_X25519
	kg_next();                           /* worked out while the server answers */
#endif

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
#ifdef TLS_RESUME
			if (et == 41) {                  /* the server took our ticket */
				if (!binders_at || elen != 2 || hs[p] || hs[p + 1]) { g_err = "bad session resumption"; goto fail; }
				resumed = 1;
			}
#endif
			p += elen;
		}
		if (!got_sh) { g_err = "server did not send an X25519 key"; goto fail; }
		sha256_process(&transcript, hs, 4 + mlen);
		memmove(hs, hs + 4 + mlen, hlen - 4 - mlen);
		hlen -= 4 + mlen;
		if (hlen) { g_err = "unexpected data after ServerHello"; goto fail; }
	}

	/* ---- key schedule up to the handshake keys */
	X25519(shared, priv, server_pub);
#ifdef TLS_RESUME
	if (resumed) memcpy(secret, psk_early, 32);                  /* early secret from the ticket */
	else
#endif
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
#ifdef TLS_RESUME
			if (!resumed)        /* resumed: proved by the ticket's own connection */
#endif
			if (!tlsv_ok()) { g_err = "the server did not prove who it is"; goto fail; }
#endif
			sha256_process(&transcript, hs, 4 + mlen);
			memmove(hs, hs + 4 + mlen, hlen - 4 - mlen);
			hlen -= 4 + mlen;
			break;
		}
		if (mt != 8 && mt != 11 && mt != 15) { g_err = "unexpected handshake message"; goto fail; }
#ifdef TLS_RESUME
		if (resumed && mt != 8) { g_err = "unexpected certificate in a resumed session"; goto fail; }
#endif
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
#ifdef TLS_RESUME
	/* resumption_master_secret: transcript up to the client Finished */
	sha256_process(&transcript, msg, 36);
	tmp = transcript; sha256_done(&tmp, th);
	hkdf_label(master, "res master", th, 32, g_res, 32);
	g_res_ok = 1;
	g_resumed = resumed;
#endif
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
#ifdef TLS_RESUME
	memset(psk_early, 0, sizeof(psk_early));
#endif
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

#ifdef TLS_RESUME
/* NewSessionTicket messages in a handshake record (whole messages only) */
static void take_tickets(const unsigned char *m, int len)
{
	while (g_res_ok && g_res_host[0] && len >= 4) {
		int mt = m[0], ml = (m[1] << 16) | (m[2] << 8) | m[3];
		if (ml > len - 4) return;                 /* split over records: not kept */
		if (mt == 4 && ml >= 13) {
			const unsigned char *b = m + 4;
			unsigned long life = ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16) | (b[2] << 8) | b[3];
			unsigned long add = ((unsigned long)b[4] << 24) | ((unsigned long)b[5] << 16) | (b[6] << 8) | b[7];
			int nl = b[8], p = 9 + nl, tl;
			if (p + 2 <= ml) {
				tl = (b[p] << 8) | b[p + 1];
				/* (the nonce goes into hkdf_label's 100-byte info: real
				   ones are 0 to 32 bytes) */
				if (tl > 0 && tl <= TK_MAX && p + 2 + tl <= ml && life > 0 && nl <= 32) {
					tls_ticket *t = NULL;
					int i, same = 0;
					/* a free slot; else the oldest of this server's (two
					   at most), else the oldest */
					for (i = 0; i < TK_N; i++)
						if (g_tk[i].len && !strcmp(g_tk[i].host, g_res_host) && g_tk[i].port == g_res_port) same++;
					for (i = 0; i < TK_N; i++) {
						tls_ticket *c = &g_tk[i];
						int mine = c->len && !strcmp(c->host, g_res_host) && c->port == g_res_port;
						if (same >= 2 && !mine) continue;
						if (!c->len) { t = c; break; }
						if (!t || c->got < t->got) t = c;
					}
					if (t) {
						memset(t, 0, sizeof(*t));
						strcpy(t->host, g_res_host);
						t->port = g_res_port;
						t->got = (unsigned long)time(NULL);
						t->life = life < (unsigned long)TK_LIFE ? life : (unsigned long)TK_LIFE;
						t->age_add = add;
						hkdf_label(g_res, "resumption", b + 9, nl, t->psk, 32);
						memcpy(t->t, b + p + 2, tl);
						t->len = tl;
					}
				}
			}
		}
		m += 4 + ml; len -= 4 + ml;
	}
}
#endif

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
#ifdef TLS_RESUME
		else if (type == 22) take_tickets(g_rec, len);
#endif
		/* other type 22 after the handshake (KeyUpdate...) - ignored */
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
