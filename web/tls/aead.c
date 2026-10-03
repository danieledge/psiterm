/* aead.c - ChaCha20-Poly1305 (RFC 8439) for PsiWeb's and PsiMail's TLS 1.3
 * (ssh/tls13.c with TLS_FAST_CRYPTO), written for the ARM710: a ChaCha20
 * block in assembler with the rotations folded into the other instructions
 * (gen_chacha.py), and Poly1305 in 13-bit limbs so that every product fits
 * a 32-bit multiply (libtomcrypt's 26-bit limbs need a 64-bit multiply,
 * which the ARM710 does not have: a call to __muldi3 for each of 25
 * products a block). About 15 instructions a byte for both, against 120.
 *
 * Constant time: no branch or index depends on the key or the data, apart
 * from the tag comparison, which looks at every byte.
 */
#include <string.h>

typedef unsigned int aw;     /* a 32-bit word */

#if defined(__GNUC__) && defined(__arm__) && !defined(FE_PORTABLE)
#include "chacha_arm.h"
extern void tls_chacha_block(aw out[16], const aw in[16]);
extern void tls_poly_mul(aw h[10], const aw T[19]);
#define CHACHA_BLOCK tls_chacha_block
#define POLY_MUL tls_poly_mul
#else
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) \
	a += b; d ^= a; d = ROTL(d, 16); c += d; b ^= c; b = ROTL(b, 12); \
	a += b; d ^= a; d = ROTL(d, 8);  c += d; b ^= c; b = ROTL(b, 7)
static void chacha_block_c(aw out[16], const aw in[16])
{
	aw x[16];
	int i;
	for (i = 0; i < 16; i++) x[i] = in[i];
	for (i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
		QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
	}
	for (i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

/* h = h * r mod 2^130 - 5 (the same sums as the assembler) */
static void poly_mul_c(aw h[10], const aw T[19])
{
	aw A[10], B[10], c = 0, s;
	int k, i;
	for (k = 0; k < 10; k++) {
		A[k] = B[k] = 0;
		for (i = 0; i < 5; i++) A[k] += h[i] * T[k - i + 9];
		for (i = 5; i < 10; i++) B[k] += h[i] * T[k - i + 9];
	}
	for (k = 0; k < 10; k++) {
		s = (A[k] & 0x1fff) + (B[k] & 0x1fff) + c;
		h[k] = s & 0x1fff;
		c = (A[k] >> 13) + (B[k] >> 13) + (s >> 13);
	}
	s = h[0] + 5 * c;
	h[0] = s & 0x1fff;
	h[1] += s >> 13;
}
#define CHACHA_BLOCK chacha_block_c
#define POLY_MUL poly_mul_c
#endif

static aw ld32(const unsigned char *p)
{
	return (aw)p[0] | ((aw)p[1] << 8) | ((aw)p[2] << 16) | ((aw)p[3] << 24);
}

static void st32(unsigned char *p, aw v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* ---------------------------------------------------------------- ChaCha20 */

static void chacha_init(aw st[16], const unsigned char key[32], const unsigned char nonce[12], aw ctr)
{
	int i;
	st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
	for (i = 0; i < 8; i++) st[4 + i] = ld32(key + 4 * i);
	st[12] = ctr;
	for (i = 0; i < 3; i++) st[13 + i] = ld32(nonce + 4 * i);
}

/* buf ^= the key stream, from the block counter in st[12] on */
static void chacha_xor(aw st[16], unsigned char *buf, int len)
{
	aw ks[16];
	int i, n;
	while (len > 0) {
		CHACHA_BLOCK(ks, st);
		st[12]++;
		n = len < 64 ? len : 64;
		if (n == 64 && !((unsigned long)buf & 3)) {
			aw *w = (aw *)buf;      /* little-endian words, as on the ARM */
			for (i = 0; i < 16; i++) w[i] ^= ks[i];
		} else {
			for (i = 0; i < n; i++) buf[i] ^= (unsigned char)(ks[i >> 2] >> (8 * (i & 3)));
		}
		buf += n; len -= n;
	}
	memset(ks, 0, sizeof(ks));
}

/* ---------------------------------------------------------------- Poly1305 */

typedef struct {
	aw h[10];
	aw T[19];            /* r and 5r, as POLY_MUL wants them */
	aw s[4];
} poly;

static void poly_init(poly *p, const unsigned char key[32])
{
	aw r[4], rl[10];
	int i;
	r[0] = ld32(key) & 0x0fffffff;
	r[1] = ld32(key + 4) & 0x0ffffffc;
	r[2] = ld32(key + 8) & 0x0ffffffc;
	r[3] = ld32(key + 12) & 0x0ffffffc;
	/* 13-bit limbs: limb i is bits 13i .. 13i+12 */
	for (i = 0; i < 10; i++) {
		int b = 13 * i, w = b >> 5, o = b & 31;
		aw v = r[w] >> o;
		if (o > 19 && w < 3) v |= r[w + 1] << (32 - o);
		rl[i] = v & 0x1fff;
	}
	for (i = 0; i < 19; i++) p->T[i] = i >= 9 ? rl[i - 9] : 5 * rl[i + 1];
	for (i = 0; i < 10; i++) p->h[i] = 0;
	for (i = 0; i < 4; i++) p->s[i] = ld32(key + 16 + 4 * i);
	memset(r, 0, sizeof(r));
	memset(rl, 0, sizeof(rl));
}

/* whole 16-byte blocks, each with the 2^128 bit; a short last piece is
   padded with zeros (as the AEAD construction pads) */
static void poly_blocks(poly *p, const unsigned char *m, int len)
{
	unsigned char last[16];
	aw w0, w1, w2, w3;
	while (len > 0) {
		if (len < 16) {
			memset(last, 0, 16);
			memcpy(last, m, len);
			m = last;
			len = 16;
		}
		w0 = ld32(m); w1 = ld32(m + 4); w2 = ld32(m + 8); w3 = ld32(m + 12);
		p->h[0] += w0 & 0x1fff;
		p->h[1] += (w0 >> 13) & 0x1fff;
		p->h[2] += ((w0 >> 26) | (w1 << 6)) & 0x1fff;
		p->h[3] += (w1 >> 7) & 0x1fff;
		p->h[4] += ((w1 >> 20) | (w2 << 12)) & 0x1fff;
		p->h[5] += (w2 >> 1) & 0x1fff;
		p->h[6] += (w2 >> 14) & 0x1fff;
		p->h[7] += ((w2 >> 27) | (w3 << 5)) & 0x1fff;
		p->h[8] += (w3 >> 8) & 0x1fff;
		p->h[9] += (w3 >> 21) | (1 << 11);          /* bits 117..127, and 2^128 */
		POLY_MUL(p->h, p->T);
		m += 16; len -= 16;
	}
}

static void poly_finish(poly *p, unsigned char tag[16])
{
	aw h[10], g[10], c, mask, w[4];
	unsigned long long t;      /* (only additions: no multiply) */
	int i;
	for (i = 0; i < 10; i++) h[i] = p->h[i];
	/* carry fully, twice round */
	for (c = 0, i = 0; i < 10; i++) { h[i] += c; c = h[i] >> 13; h[i] &= 0x1fff; }
	h[0] += 5 * c;
	for (c = 0, i = 0; i < 10; i++) { h[i] += c; c = h[i] >> 13; h[i] &= 0x1fff; }
	h[0] += 5 * c;
	c = h[0] >> 13; h[0] &= 0x1fff; h[1] += c;
	/* g = h + 5 - 2^130: take it if it does not go negative */
	for (c = 5, i = 0; i < 10; i++) { g[i] = h[i] + c; c = g[i] >> 13; g[i] &= 0x1fff; }
	mask = (aw)0 - c;          /* c = 1: h >= p, take g */
	for (i = 0; i < 10; i++) h[i] ^= mask & (h[i] ^ g[i]);
	/* to 4 words (the low 128 bits), plus s */
	for (i = 0; i < 4; i++) {
		int b = 32 * i, k;
		aw v = 0;
		for (k = 0; k < 10; k++) {
			int sh = 13 * k - b;
			if (sh > -13 && sh < 32) v |= sh >= 0 ? h[k] << sh : h[k] >> -sh;
		}
		w[i] = v;
	}
	t = 0;
	for (i = 0; i < 4; i++) {
		t += (unsigned long long)w[i] + p->s[i];
		st32(tag + 4 * i, (aw)t);
		t >>= 32;
	}
	memset(h, 0, sizeof(h));
	memset(g, 0, sizeof(g));
	memset(p, 0, sizeof(*p));
}

/* ---------------------------------------------------------------- AEAD */

static void aead_tag(const unsigned char key[32], const unsigned char nonce[12],
                     const unsigned char *aad, int aadlen,
                     const unsigned char *ct, int ctlen, unsigned char tag[16])
{
	aw st[16], ks[16];
	unsigned char otk[32], lens[16];
	poly p;
	int i;
	chacha_init(st, key, nonce, 0);
	CHACHA_BLOCK(ks, st);                      /* block 0: the one-time Poly1305 key */
	for (i = 0; i < 8; i++) st32(otk + 4 * i, ks[i]);
	poly_init(&p, otk);
	poly_blocks(&p, aad, aadlen);
	poly_blocks(&p, ct, ctlen);
	for (i = 0; i < 4; i++) {
		st32(lens + 4 * i, 0);
	}
	st32(lens, (aw)aadlen);
	st32(lens + 8, (aw)ctlen);
	poly_blocks(&p, lens, 16);
	poly_finish(&p, tag);
	memset(st, 0, sizeof(st));
	memset(ks, 0, sizeof(ks));
	memset(otk, 0, sizeof(otk));
}

/* encrypt buf[0..len) in place and put the tag at buf + len */
void tls_aead_seal(const unsigned char key[32], const unsigned char nonce[12],
                   const unsigned char *aad, int aadlen, unsigned char *buf, int len)
{
	aw st[16];
	chacha_init(st, key, nonce, 1);
	chacha_xor(st, buf, len);
	aead_tag(key, nonce, aad, aadlen, buf, len, buf + len);
	memset(st, 0, sizeof(st));
}

/* buf[0..len) is ciphertext then the 16-byte tag: checks the tag, then
   decrypts in place. Returns 0 if the tag is good. */
int tls_aead_open(const unsigned char key[32], const unsigned char nonce[12],
                  const unsigned char *aad, int aadlen, unsigned char *buf, int len)
{
	aw st[16];
	unsigned char tag[16];
	int i, diff = 0;
	if (len < 16) return -1;
	len -= 16;
	aead_tag(key, nonce, aad, aadlen, buf, len, tag);
	for (i = 0; i < 16; i++) diff |= tag[i] ^ buf[len + i];
	if (diff) return -1;
	chacha_init(st, key, nonce, 1);
	chacha_xor(st, buf, len);
	memset(st, 0, sizeof(st));
	return 0;
}
