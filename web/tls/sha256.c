/* sha256.c - SHA-256 for PsiWeb's and PsiMail's TLS 1.3 (ssh/tls13.c with
 * TLS_FAST_CRYPTO: the transcript, HMAC and HKDF). libtomcrypt's, built
 * with LTC_SMALL_CODE for Dropbear, keeps the eight working variables in an
 * array and shuffles them every round; here the rounds are written out
 * eight at a time with the variables renamed instead, and the message
 * schedule is kept as 16 words. About 2.5 times faster on the ARM710.
 */
#include <string.h>
#include "../../ssh/tls13.h"      /* tls_sha256 */

typedef unsigned int sw;

static const sw K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

#undef ROR
#undef S0
#undef S1
#undef G0
#undef G1
#undef CH
#undef MAJ
#undef RND
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define S0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define S1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define G0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define G1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))
#define CH(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define MAJ(x, y, z) (((x) & (y)) | ((z) & ((x) | (y))))

/* one round; the caller renames the variables instead of moving them */
#define RND(a, b, c, d, e, f, g, h, i) \
	t1 = h + S1(e) + CH(e, f, g) + K256[i] + W[(i) & 15]; \
	d += t1; \
	h = t1 + S0(a) + MAJ(a, b, c)

/* W[i & 15] for round i >= 16 */
#define SCHED(i) \
	W[(i) & 15] += G1(W[((i) - 2) & 15]) + W[((i) - 7) & 15] + G0(W[((i) - 15) & 15])

#define EIGHT(i) \
	RND(a, b, c, d, e, f, g, h, i);     RND(h, a, b, c, d, e, f, g, i + 1); \
	RND(g, h, a, b, c, d, e, f, i + 2); RND(f, g, h, a, b, c, d, e, i + 3); \
	RND(e, f, g, h, a, b, c, d, i + 4); RND(d, e, f, g, h, a, b, c, i + 5); \
	RND(c, d, e, f, g, h, a, b, i + 6); RND(b, c, d, e, f, g, h, a, i + 7)
#define SCHED8(i) \
	SCHED(i); SCHED(i + 1); SCHED(i + 2); SCHED(i + 3); \
	SCHED(i + 4); SCHED(i + 5); SCHED(i + 6); SCHED(i + 7)

static void sha256_block(sw h8[8], const unsigned char *p)
{
	sw W[16], a, b, c, d, e, f, g, h, t1;
	int i;
	for (i = 0; i < 16; i++, p += 4)
		W[i] = ((sw)p[0] << 24) | ((sw)p[1] << 16) | ((sw)p[2] << 8) | p[3];
	a = h8[0]; b = h8[1]; c = h8[2]; d = h8[3];
	e = h8[4]; f = h8[5]; g = h8[6]; h = h8[7];
	/* written out with constant indices: gcc 3.0 then addresses K256 and W
	   directly instead of working out each index at run time */
	EIGHT(0); EIGHT(8);
	SCHED8(16); EIGHT(16); SCHED8(24); EIGHT(24); SCHED8(32); EIGHT(32);
	SCHED8(40); EIGHT(40); SCHED8(48); EIGHT(48); SCHED8(56); EIGHT(56);
	h8[0] += a; h8[1] += b; h8[2] += c; h8[3] += d;
	h8[4] += e; h8[5] += f; h8[6] += g; h8[7] += h;
}

void tls_sha256_init(tls_sha256 *s)
{
	s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85; s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
	s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c; s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
	s->len_lo = s->len_hi = 0;
	s->n = 0;
}

void tls_sha256_process(tls_sha256 *s, const unsigned char *m, unsigned long len)
{
	sw lo = s->len_lo + (sw)len;
	if (lo < s->len_lo) s->len_hi++;
	s->len_lo = lo;
	while (len > 0) {
		if (s->n == 0 && len >= 64) {
			sha256_block(s->h, m);
			m += 64; len -= 64;
		} else {
			int k = 64 - s->n;
			if ((unsigned long)k > len) k = (int)len;
			memcpy(s->buf + s->n, m, k);
			s->n += k; m += k; len -= k;
			if (s->n == 64) { sha256_block(s->h, s->buf); s->n = 0; }
		}
	}
}

void tls_sha256_done(tls_sha256 *s, unsigned char out[32])
{
	sw hi = (s->len_hi << 3) | (s->len_lo >> 29), lo = s->len_lo << 3;
	int i;
	s->buf[s->n++] = 0x80;
	if (s->n > 56) {
		memset(s->buf + s->n, 0, 64 - s->n);
		sha256_block(s->h, s->buf);
		s->n = 0;
	}
	memset(s->buf + s->n, 0, 56 - s->n);
	for (i = 0; i < 4; i++) {
		s->buf[56 + i] = (unsigned char)(hi >> (24 - 8 * i));
		s->buf[60 + i] = (unsigned char)(lo >> (24 - 8 * i));
	}
	sha256_block(s->h, s->buf);
	for (i = 0; i < 8; i++) {
		out[4 * i] = (unsigned char)(s->h[i] >> 24);
		out[4 * i + 1] = (unsigned char)(s->h[i] >> 16);
		out[4 * i + 2] = (unsigned char)(s->h[i] >> 8);
		out[4 * i + 3] = (unsigned char)s->h[i];
	}
	memset(s, 0, sizeof(*s));
}
