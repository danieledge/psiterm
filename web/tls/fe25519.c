/* fe25519.c - X25519 (RFC 7748) for the Psion's ARM710, used by PsiWeb's and
 * PsiMail's TLS 1.3 (ssh/tls13.c with TLS_FAST_X25519). PsiTerm's Dropbear
 * keeps its own ssh/db/src/curve25519.c.
 *
 * The ARM710 has a 32 x 32 -> 32 bit multiply only. Field elements are 20
 * limbs of 13 bits, so a whole column of products adds up in one 32-bit
 * register with no carries (see gen_fe25519.py), and a square needs only 210
 * multiplies. With the multiply and square in assembler, an X25519 is about
 * 2.7 million ARM instructions, against 6.8 million for Dropbear's 16-bit
 * limbs (which carry after every product); the inversion uses the usual
 * chain of 11 multiplies instead of 253.
 *
 * Constant time: no branch or table index depends on a secret (the ladder
 * swaps with masks; the inversion is a fixed chain).
 *
 * Besides the one-shot tls_x25519(), the base-point multiply that makes a
 * key pair can be run a few ladder steps at a time (tls_x25519_kg_*), so
 * tls13.c can work out the next connection's key while it waits for the
 * network. The base point's multiply by 9 is a small multiply.
 */
typedef unsigned int fe_w;
typedef fe_w fe[20];

/* a multiple of p (65p) with every limb in [2^14, 2^15): a - b + K never
   goes below zero for a loose b (limbs < 2^13 + 2^8) */
static const fe_w K65P[20] = {
	0x5b2d, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd,
	0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x5ffd, 0x40fd };

/* fe_mul, fe_sq (r = a * b, a^2 mod p; r may be a or b), fe_add, fe_sub
   (a + 65p - b): ARM assembler for the Psion, C elsewhere. Every result is
   loose. */
#include "fe25519_mul.h"


/* r = limbs of x carried to 13 bits, the top carry folded (2^260 = 608);
   inputs up to 2^30 a limb (then the folded carry is below 2^27: two more
   carries bring it within limb 2) */
static void fe_carry(fe r, const fe_w *x)
{
	fe_w c = 0;
	int i;
	for (i = 0; i < 20; i++) { c += x[i]; r[i] = c & 0x1fff; c >>= 13; }
	c = r[0] + 608 * c; r[0] = c & 0x1fff;
	c = r[1] + (c >> 13); r[1] = c & 0x1fff; r[2] += c >> 13;
}

/* r = a * k, k < 2^17 */
static void fe_mul_small(fe r, const fe a, fe_w k)
{
	fe_w t[20];
	int i;
	for (i = 0; i < 20; i++) t[i] = a[i] * k;
	fe_carry(r, t);
}

static void fe_cswap(fe a, fe b, fe_w bit)
{
	fe_w m = (fe_w)0 - bit, t;
	int i;
	for (i = 0; i < 20; i++) { t = m & (a[i] ^ b[i]); a[i] ^= t; b[i] ^= t; }
}

static void fe_frombytes(fe r, const unsigned char *s)
{
	fe_w acc = 0;
	int bits = 0, i = 0, k;
	for (k = 0; k < 32; k++) {
		acc |= (fe_w)(k == 31 ? s[k] & 0x7f : s[k]) << bits;
		bits += 8;
		while (bits >= 13 && i < 20) { r[i++] = acc & 0x1fff; acc >>= 13; bits -= 13; }
	}
	while (i < 20) { r[i++] = acc & 0x1fff; acc >>= 13; }
}

/* the unique value below p, as 32 bytes little-endian */
static void fe_tobytes(unsigned char *s, const fe a)
{
	fe_w t[20], c, m;
	int i, k, bits;
	unsigned int acc;
	for (i = 0; i < 20; i++) t[i] = a[i];
	/* twice: fold bits 255 and up (limb 19 keeps 8 bits) as 19 * them */
	for (k = 0; k < 2; k++) {
		c = 0;
		for (i = 0; i < 20; i++) { c += t[i]; t[i] = c & 0x1fff; c >>= 13; }
		c = (c << 5) | (t[19] >> 8);
		t[19] &= 0xff;
		t[0] += 19 * c;
	}
	c = 0;
	for (i = 0; i < 20; i++) { c += t[i]; t[i] = c & 0x1fff; c >>= 13; }
	/* now t < 2^255 + small: subtract p if t >= p (t + 19 >= 2^255) */
	{
		fe_w u[20];
		c = 19;
		for (i = 0; i < 20; i++) { c += t[i]; u[i] = c & 0x1fff; c >>= 13; }
		m = (fe_w)0 - ((u[19] >> 8) & 1);          /* all ones: take u - 2^255 */
		u[19] &= 0xff;
		for (i = 0; i < 20; i++) t[i] ^= m & (t[i] ^ u[i]);
	}
	acc = 0; bits = 0; k = 0;
	for (i = 0; i < 20; i++) {
		acc |= t[i] << bits;
		bits += 13;
		while (bits >= 8) { s[k++] = (unsigned char)acc; acc >>= 8; bits -= 8; }
	}
	while (k < 32) { s[k++] = (unsigned char)acc; acc >>= 8; }
}

/* r = z^(p-2) = 1/z: 254 squares, 11 multiplies (the usual chain) */
static void fe_invert(fe r, const fe z)
{
	fe t0, t1, t2, t3;
	int i;
	fe_sq(t0, z);
	fe_sq(t1, t0); fe_sq(t1, t1);
	fe_mul(t1, z, t1);
	fe_mul(t0, t0, t1);
	fe_sq(t2, t0);
	fe_mul(t1, t1, t2);
	fe_sq(t2, t1); for (i = 1; i < 5; i++) fe_sq(t2, t2);
	fe_mul(t1, t2, t1);
	fe_sq(t2, t1); for (i = 1; i < 10; i++) fe_sq(t2, t2);
	fe_mul(t2, t2, t1);
	fe_sq(t3, t2); for (i = 1; i < 20; i++) fe_sq(t3, t3);
	fe_mul(t2, t3, t2);
	fe_sq(t2, t2); for (i = 1; i < 10; i++) fe_sq(t2, t2);
	fe_mul(t1, t2, t1);
	fe_sq(t2, t1); for (i = 1; i < 50; i++) fe_sq(t2, t2);
	fe_mul(t2, t2, t1);
	fe_sq(t3, t2); for (i = 1; i < 100; i++) fe_sq(t3, t3);
	fe_mul(t2, t3, t2);
	fe_sq(t2, t2); for (i = 1; i < 50; i++) fe_sq(t2, t2);
	fe_mul(t1, t2, t1);
	fe_sq(t1, t1); for (i = 1; i < 5; i++) fe_sq(t1, t1);
	fe_mul(r, t1, t0);
}

/* the Montgomery ladder's state, so that it can run in pieces */
typedef struct {
	unsigned char k[32];         /* the clamped scalar */
	fe x1, x2, z2, x3, z3;
	fe_w swap;
	int bit;                     /* next bit to do: 254 down to 0, -1 = done */
	int small;                   /* x1 is 9 (the base point): multiply by 9 */
} x25519_ladder;

static void ladder_start(x25519_ladder *L, const unsigned char *n, const unsigned char *u)
{
	int i;
	for (i = 0; i < 32; i++) L->k[i] = n[i];
	L->k[0] &= 248; L->k[31] &= 127; L->k[31] |= 64;
	fe_frombytes(L->x1, u);
	for (i = 0; i < 20; i++) { L->x2[i] = 0; L->z2[i] = 0; L->x3[i] = L->x1[i]; L->z3[i] = 0; }
	L->x2[0] = 1; L->z3[0] = 1;
	L->swap = 0;
	L->bit = 254;
	L->small = 0;
	if (u[0] == 9) {
		for (i = 1; i < 32 && !u[i]; i++) ;
		L->small = (i == 32);    /* public: the base point */
	}
}

/* runs up to n steps; returns 1 when the ladder has finished */
static int ladder_steps(x25519_ladder *L, int n)
{
	fe a, aa, b, bb, e, c, d, da, cb;
	while (n-- > 0 && L->bit >= 0) {
		fe_w kt = (L->k[L->bit >> 3] >> (L->bit & 7)) & 1;
		L->swap ^= kt;
		fe_cswap(L->x2, L->x3, L->swap);
		fe_cswap(L->z2, L->z3, L->swap);
		L->swap = kt;
		fe_add(a, L->x2, L->z2);
		fe_sq(aa, a);
		fe_sub(b, L->x2, L->z2);
		fe_sq(bb, b);
		fe_sub(e, aa, bb);
		fe_add(c, L->x3, L->z3);
		fe_sub(d, L->x3, L->z3);
		fe_mul(da, d, a);
		fe_mul(cb, c, b);
		fe_add(L->x3, da, cb);
		fe_sq(L->x3, L->x3);
		fe_sub(L->z3, da, cb);
		fe_sq(L->z3, L->z3);
		if (L->small) fe_mul_small(L->z3, L->z3, 9);
		else fe_mul(L->z3, L->z3, L->x1);
		fe_mul(L->x2, aa, bb);
		fe_mul_small(a, e, 121665);
		fe_add(a, aa, a);
		fe_mul(L->z2, e, a);
		L->bit--;
	}
	if (L->bit < 0 && L->swap != 2) {
		fe_cswap(L->x2, L->x3, L->swap);
		fe_cswap(L->z2, L->z3, L->swap);
		L->swap = 2;                    /* the final swap is done */
	}
	return L->bit < 0;
}

static void ladder_finish(x25519_ladder *L, unsigned char *q)
{
	fe zi;
	fe_invert(zi, L->z2);
	fe_mul(L->x2, L->x2, zi);
	fe_tobytes(q, L->x2);
	{
		volatile unsigned char *p = (volatile unsigned char *)L;
		unsigned int i;
		for (i = 0; i < sizeof(*L); i++) p[i] = 0;
	}
}

/* q = X25519(n, u) */
void tls_x25519(unsigned char *q, const unsigned char *n, const unsigned char *u)
{
	x25519_ladder L;
	ladder_start(&L, n, u);
	ladder_steps(&L, 255);
	ladder_finish(&L, q);
}

/* ---- a key pair made in pieces: start, then steps until it says done,
   then finish gives the public key (the private key is the one given) */
static x25519_ladder g_kg;
static int g_kg_on;

void tls_x25519_kg_start(const unsigned char priv[32])
{
	static const unsigned char nine[32] = { 9 };
	ladder_start(&g_kg, priv, nine);
	g_kg_on = 1;
}

/* returns 1 when the ladder is done (or none is running) */
int tls_x25519_kg_steps(int n)
{
	if (!g_kg_on) return 1;
	return ladder_steps(&g_kg, n);
}

void tls_x25519_kg_finish(unsigned char pub[32])
{
	ladder_steps(&g_kg, 255);
	ladder_finish(&g_kg, pub);
	g_kg_on = 0;
}
