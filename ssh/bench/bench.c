/* bench.c - correctness + speed probes for the exact crypto code psissh
   uses, compiled with the Psion compiler. Run in an ARM emulator (and later
   on the device itself). */
#include "../db/libtomcrypt/src/headers/tomcrypt.h"
#include "../db/libtommath/tommath.h"
#include "rsa_vec.h"
#include "../zlib/zlib.h"

void dropbear_curve25519_scalarmult(unsigned char *q, const unsigned char *n, const unsigned char *p);
int dropbear_ed25519_verify(const unsigned char *m, unsigned long mlen, const unsigned char *s, unsigned long slen, const unsigned char *pk);

unsigned char bench_out[64];
int bench_len;

static int unhex(const char *h, unsigned char *o)
{
	int n = 0;
	while (h[0] && h[1]) {
		int a = h[0] <= '9' ? h[0] - '0' : (h[0] | 32) - 'a' + 10;
		int b = h[1] <= '9' ? h[1] - '0' : (h[1] | 32) - 'a' + 10;
		o[n++] = (unsigned char)(a * 16 + b);
		h += 2;
	}
	return n;
}

static int check(const unsigned char *got, const char *hex)
{
	unsigned char want[64];
	int n = unhex(hex, want), i;
	for (i = 0; i < n; i++)
		if (got[i] != want[i]) return 1;
	return 0;
}

static unsigned char big[4096];
static unsigned char ztext[4096], zbuf[4096 + 64], zout[4096];
static unsigned long zlen;

/* 4 KB of typical terminal output (an "ls -l"-style listing) */
static void make_text(void)
{
	static const char *names[] = { "README.md", "Makefile", "src", "config.h", "main.c", "notes.txt", "build", "psion.log" };
	unsigned int seed = 12345, i = 0, n;
	char line[96];
	while (i < sizeof(ztext)) {
		seed = seed * 1103515245u + 12345u;
		n = sprintf(line, "-rw-r--r--  1 dan  staff  %6u Sep %2u %02u:%02u %s\r\n",
			(seed >> 8) % 99999, 1 + (seed >> 3) % 28, (seed >> 5) % 24, (seed >> 11) % 60, names[(seed >> 16) % 8]);
		if (n > sizeof(ztext) - i) n = sizeof(ztext) - i;
		memcpy(ztext + i, line, n);
		i += n;
	}
}


int psi_bench_case(int which)
{
	unsigned char k[64], m[128], s[128];
	bench_len = 32;
	switch (which) {
	case 1: { /* X25519, RFC 7748 5.2 */
		unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k);
		unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", m);
		dropbear_curve25519_scalarmult(bench_out, k, m);
		return check(bench_out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
	}
	case 2: { /* Ed25519 verify, RFC 8032 test 1 */
		unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", k);
		unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", s);
		return dropbear_ed25519_verify(m, 0, s, 64, k) == 0 ? 0 : 1;
	}
	case 3: { /* SHA-256("abc") */
		hash_state hs;
		sha256_init(&hs);
		sha256_process(&hs, (const unsigned char*)"abc", 3);
		sha256_done(&hs, bench_out);
		return check(bench_out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	}
	case 4: { /* AES-128, FIPS-197 C.1 */
		symmetric_key sk;
		unhex("000102030405060708090a0b0c0d0e0f", k);
		unhex("00112233445566778899aabbccddeeff", m);
		if (aes_setup(k, 16, 0, &sk) != CRYPT_OK) return 2;
		aes_ecb_encrypt(m, bench_out, &sk);
		return check(bench_out, "69c4e0d86a7b0430d8cdb78070b4c55a");
	}
	case 5: { /* ChaCha20, RFC 8439 2.4.2 */
		chacha_state st;
		const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
		int i;
		for (i = 0; i < 32; i++) k[i] = (unsigned char)i;
		unhex("000000000000004a00000000", m);
		chacha_setup(&st, k, 32, 20);
		chacha_ivctr32(&st, m, 12, 1);
		chacha_crypt(&st, (const unsigned char*)pt, 32, bench_out);
		return check(bench_out, "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b");
	}
	case 6: { /* Poly1305, RFC 8439 2.5.2 */
		poly1305_state ps;
		unsigned long tl = 16;
		const char *msg = "Cryptographic Forum Research Group";
		unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", k);
		poly1305_init(&ps, k, 32);
		poly1305_process(&ps, (const unsigned char*)msg, 34);
		poly1305_done(&ps, bench_out, &tl);
		return check(bench_out, "a8061dc1305136c6c22b8baf0c0127a9");
	}
	case 7: { /* RSA-2048 public operation (host key signature check) */
		mp_int n, b, r, e, want;
		mp_init_multi(&n, &b, &r, &e, &want, NULL);
		mp_read_radix(&n, RSA_N, 16);
		mp_read_radix(&b, RSA_B, 16);
		mp_read_radix(&want, RSA_R, 16);
		mp_set_u32(&e, 65537);
		mp_exptmod(&b, &e, &n, &r);
		return mp_cmp(&r, &want) == MP_EQ ? 0 : 1;
	}
	case 8: { /* bulk: ChaCha20 over 4 KB (throughput) */
		chacha_state st;
		for (int i = 0; i < 32; i++) k[i] = (unsigned char)i;
		chacha_setup(&st, k, 32, 20);
		chacha_ivctr64(&st, k, 8, 0);
		chacha_crypt(&st, big, sizeof(big), big);
		return 0;
	}
	case 9: { /* bulk: Poly1305 over 4 KB */
		poly1305_state ps; unsigned long tl = 16;
		poly1305_init(&ps, k, 32);
		poly1305_process(&ps, big, sizeof(big));
		poly1305_done(&ps, bench_out, &tl);
		return 0;
	}
	case 10: { /* bulk: AES-128-CTR over 4 KB */
		symmetric_CTR ctr;
		register_cipher(&aes_desc);
		ctr_start(find_cipher("aes"), k, k, 16, 0, CTR_COUNTER_BIG_ENDIAN, &ctr);
		ctr_encrypt(big, big, sizeof(big), &ctr);
		return 0;
	}
	case 11: { /* bulk: SHA-256 over 4 KB (HMAC cost) */
		hash_state hs;
		sha256_init(&hs);
		sha256_process(&hs, big, sizeof(big));
		sha256_done(&hs, bench_out);
		return 0;
	}
	case 12: { /* zlib deflate of 4 KB text (what the server does) */
		z_stream z;
		make_text();
		memset(&z, 0, sizeof(z));
		if (deflateInit(&z, Z_DEFAULT_COMPRESSION) != Z_OK) return 1;
		z.next_in = ztext; z.avail_in = sizeof(ztext);
		z.next_out = zbuf; z.avail_out = sizeof(zbuf);
		if (deflate(&z, Z_SYNC_FLUSH) != Z_OK) return 2;
		zlen = sizeof(zbuf) - z.avail_out;
		deflateEnd(&z);
		bench_len = (int)zlen;
		return zlen > 0 && zlen < sizeof(ztext) ? 0 : 3;
	}
	case 13: { /* zlib inflate back to 4 KB (what the Psion does) */
		/* one stream, reset between runs: the speed test repeats this case */
		static z_stream z;
		static int zready;
		if (!zready) {
			memset(&z, 0, sizeof(z));
			if (inflateInit(&z) != Z_OK) return 1;
			zready = 1;
		} else if (inflateReset(&z) != Z_OK)
			return 1;
		z.next_in = zbuf; z.avail_in = zlen;
		z.next_out = zout; z.avail_out = sizeof(zout);
		if (inflate(&z, Z_SYNC_FLUSH) != Z_OK) return 2;
		if (sizeof(zout) - z.avail_out != sizeof(ztext)) return 3;
		return memcmp(zout, ztext, sizeof(ztext)) ? 4 : 0;
	}
	}
	return 99;
}
