/* pwrandom.c - random numbers for PsiMail's TLS key exchange
 *
 * The Psion has no hardware random source. As in PsiTerm, noise comes from
 * psiglue's pg_entropy() (timer/CPU jitter, the clock, free RAM, and the
 * key and pen timings PsiMail.app passes in) and is hashed into a SHA-256
 * pool; output blocks are SHA-256(pool, counter) and the pool is re-keyed
 * after every call, with fresh jitter mixed in each time. */
#include "includes.h"

extern int pg_entropy(unsigned char *out, int max);

static unsigned char g_pool[32];
static unsigned long g_counter;
static int g_seeded;

static void mix(const unsigned char *d, unsigned long n)
{
	hash_state h;
	sha256_init(&h);
	sha256_process(&h, g_pool, 32);
	sha256_process(&h, d, n);
	sha256_done(&h, g_pool);
}

void genrandom(unsigned char *buf, unsigned int len)
{
	unsigned char e[256], blk[32];
	hash_state h;
	int n = pg_entropy(e, g_seeded ? 64 : (int)sizeof(e));
	mix(e, n);
	g_seeded = 1;
	while (len) {
		unsigned int k = len < 32 ? len : 32;
		g_counter++;
		sha256_init(&h);
		sha256_process(&h, g_pool, 32);
		sha256_process(&h, (const unsigned char *)&g_counter, sizeof(g_counter));
		sha256_process(&h, (const unsigned char *)"out", 3);
		sha256_done(&h, blk);
		memcpy(buf, blk, k);
		buf += k; len -= k;
	}
	mix((const unsigned char *)"rekey", 5);
	memset(e, 0, sizeof(e));
	memset(blk, 0, sizeof(blk));
}
