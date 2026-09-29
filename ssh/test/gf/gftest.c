/* Differential test: PSI_GF32 field arithmetic vs the original TweetNaCl
 * i64 code in db/src/curve25519.c, on random inputs, plus the RFC 7748
 * iterated X25519 vectors and RFC 8032 Ed25519 verify. Host build only.
 * Build+run: make -f test/gf/Makefile (from /home/claude/psissh) */
#include <stdio.h>
#undef printf
#undef sscanf
#include <stdlib.h>
#include <string.h>

void new_x25519(unsigned char *q, const unsigned char *n, const unsigned char *p);
void ref_x25519(unsigned char *q, const unsigned char *n, const unsigned char *p);
int new_ed_verify(const unsigned char *m, unsigned long mlen, const unsigned char *s, unsigned long slen, const unsigned char *pk);
int ref_ed_verify(const unsigned char *m, unsigned long mlen, const unsigned char *s, unsigned long slen, const unsigned char *pk);
void new_ed_sign(const unsigned char *m, unsigned long mlen, unsigned char *s, unsigned long *slen, const unsigned char *sk, const unsigned char *pk);
void new_ed_make_key(unsigned char *pk, unsigned char *sk);

/* stand-in for Dropbear's genrandom (only used by make_key in this test) */
void genrandom(unsigned char *buf, unsigned int len);
static unsigned int rs = 12345;
static unsigned char rb(void) { rs = rs * 1103515245u + 12345u; return (unsigned char)(rs >> 16); }
static void rnd(unsigned char *b, int n) { while (n--) *b++ = rb(); }
void genrandom(unsigned char *buf, unsigned int len) { rnd(buf, (int)len); }
static void unhex(const char *h, unsigned char *o) { while (h[0] && h[1]) { unsigned v; sscanf(h, "%2x", &v); *o++ = v; h += 2; } }
static void hex(const unsigned char *b, int n) { while (n--) printf("%02x", *b++); }

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 2000, i, bad = 0;
	unsigned char n[32], p[32], q1[32], q2[32];

	/* random scalars x random points: new == reference */
	for (i = 0; i < iters; i++) {
		rnd(n, 32); rnd(p, 32);
		if (i % 7 == 0) memset(p, 0xff, 32);      /* non-canonical, all limbs 0xffff */
		if (i % 11 == 0) memset(p, 0, 32), p[0] = i & 0xff;
		if (i % 13 == 0) memset(n, 0xff, 32);
		new_x25519(q1, n, p); ref_x25519(q2, n, p);
		if (memcmp(q1, q2, 32)) { printf("MISMATCH x25519 iter %d\n", i); bad++; if (bad > 5) return 1; }
	}
	printf("x25519 differential: %d cases %s\n", iters, bad ? "FAIL" : "ok");

	/* RFC 7748 section 5.2 iterated test: 1 and 1000 iterations */
	{
		unsigned char k[32], u[32], r[32], want1[32], want1000[32];
		unhex("0900000000000000000000000000000000000000000000000000000000000000", k);
		memcpy(u, k, 32);
		unhex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", want1);
		unhex("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", want1000);
		for (i = 1; i <= 1000; i++) {
			new_x25519(r, k, u);
			memcpy(u, k, 32); memcpy(k, r, 32);
			if (i == 1 && memcmp(r, want1, 32)) { printf("RFC7748 iter1 FAIL\n"); bad++; }
		}
		printf("RFC 7748 iterated (1000): %s\n", memcmp(r, want1000, 32) ? "FAIL" : "ok");
		if (memcmp(r, want1000, 32)) bad++;
	}

	/* Ed25519: random keys, sign with new code, verify with both, corrupt -> reject */
	{
		unsigned char pk[32], sk[64], sig[64], msg[100]; unsigned long sl;
		int ok1 = 0, ok2 = 0, rej = 0;
		for (i = 0; i < iters / 20 + 5; i++) {
			int ml = (i * 7) % 100;
			rnd(msg, 100);
			new_ed_make_key(pk, sk);
			new_ed_sign(msg, ml, sig, &sl, sk, pk);
			ok1 += new_ed_verify(msg, ml, sig, 64, pk) == 0;
			ok2 += ref_ed_verify(msg, ml, sig, 64, pk) == 0;
			sig[i % 32] ^= 1 << (i % 8);
			rej += new_ed_verify(msg, ml, sig, 64, pk) != 0;
		}
		printf("ed25519 sign/verify: new %d/%d ref %d/%d rejected-corrupt %d/%d\n", ok1, i, ok2, i, rej, i);
		if (ok1 != i || ok2 != i || rej != i) bad++;
	}
	printf(bad ? "GF TEST FAILED\n" : "GF TEST OK\n");
	return bad != 0;
}
