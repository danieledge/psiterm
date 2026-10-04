/* psi_digest.c - MD5 and SHA-256 for HTTP Digest authentication in PsiWeb
 * (Links' auth.c, PSIWEB). Small and self-contained: only a few hashes of
 * short strings a request, so it is written for size, not speed.
 *
 *   psi_digest_hex(alg, data, len, out): alg 0 MD5 (32 hex digits),
 *   alg 1 SHA-256 (64), lower case, out NUL-terminated (65 bytes). */
#include <string.h>

typedef unsigned int u32;

static void put_hex(const unsigned char *d, int n, char *out)
{
	static const char x[] = "0123456789abcdef";
	int i;
	for (i = 0; i < n; i++) {
		out[2 * i] = x[d[i] >> 4];
		out[2 * i + 1] = x[d[i] & 15];
	}
	out[2 * n] = 0;
}

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define ROR(v, n) (((v) >> (n)) | ((v) << (32 - (n))))

/* ---------------------------------------------------------------- MD5 (RFC 1321) */

static const u32 md5_k[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};
static const unsigned char md5_r[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void md5_block(u32 h[4], const unsigned char *p)
{
	u32 w[16], a = h[0], b = h[1], c = h[2], d = h[3], f, t;
	int i, g;
	for (i = 0; i < 16; i++)
		w[i] = p[4 * i] | ((u32)p[4 * i + 1] << 8) | ((u32)p[4 * i + 2] << 16) | ((u32)p[4 * i + 3] << 24);
	for (i = 0; i < 64; i++) {
		if (i < 16) f = (b & c) | (~b & d), g = i;
		else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
		else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) & 15;
		else f = c ^ (b | ~d), g = (7 * i) & 15;
		t = d;
		d = c;
		c = b;
		b = b + ROL(a + f + md5_k[i] + w[g], md5_r[i]);
		a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

/* ---------------------------------------------------------------- SHA-256 (FIPS 180-4) */

static const u32 sha_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha_block(u32 h[8], const unsigned char *p)
{
	u32 w[64], s[8], t1, t2;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) | ((u32)p[4 * i + 2] << 8) | p[4 * i + 3];
	for (; i < 64; i++)
		w[i] = (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7]
		     + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
	for (i = 0; i < 8; i++) s[i] = h[i];
	for (i = 0; i < 64; i++) {
		t1 = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + sha_k[i] + w[i];
		t2 = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
		s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1;
		s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
	}
	for (i = 0; i < 8; i++) h[i] += s[i];
}

/* ---------------------------------------------------------------- both */

void psi_digest_hex(int alg, const unsigned char *data, int len, char *out)
{
	unsigned char blk[64], dig[32];
	u32 h[8];
	unsigned long bits = (unsigned long)len * 8;
	int i, n = 0, words = alg ? 8 : 4;
	if (alg) {
		static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
		memcpy(h, iv, sizeof(iv));
	} else {
		h[0] = 0x67452301; h[1] = 0xefcdab89; h[2] = 0x98badcfe; h[3] = 0x10325476;
	}
	for (; len >= 64; len -= 64, data += 64)
		if (alg) sha_block(h, data); else md5_block(h, data);
	memcpy(blk, data, len);
	n = len;
	blk[n++] = 0x80;
	if (n > 56) {
		memset(blk + n, 0, 64 - n);
		if (alg) sha_block(h, blk); else md5_block(h, blk);
		n = 0;
	}
	memset(blk + n, 0, 56 - n);
	for (i = 0; i < 8; i++)	/* the length in bits: big-endian for SHA, little for MD5 */
		blk[alg ? 63 - i : 56 + i] = (unsigned char)(i < 4 ? bits >> (8 * i) : 0);
	if (alg) sha_block(h, blk); else md5_block(h, blk);
	for (i = 0; i < words; i++) {
		if (alg) {
			dig[4 * i] = (unsigned char)(h[i] >> 24); dig[4 * i + 1] = (unsigned char)(h[i] >> 16);
			dig[4 * i + 2] = (unsigned char)(h[i] >> 8); dig[4 * i + 3] = (unsigned char)h[i];
		} else {
			dig[4 * i] = (unsigned char)h[i]; dig[4 * i + 1] = (unsigned char)(h[i] >> 8);
			dig[4 * i + 2] = (unsigned char)(h[i] >> 16); dig[4 * i + 3] = (unsigned char)(h[i] >> 24);
		}
	}
	put_hex(dig, words * 4, out);
}

#ifdef PSI_DIGEST_TEST
#include <stdio.h>
int main(void)
{
	char o[65];
	int bad = 0;
	psi_digest_hex(0, (const unsigned char *)"", 0, o); bad |= strcmp(o, "d41d8cd98f00b204e9800998ecf8427e");
	psi_digest_hex(0, (const unsigned char *)"The quick brown fox jumps over the lazy dog", 43, o); bad |= strcmp(o, "9e107d9d372bb6826bd81d3542a419d6");
	psi_digest_hex(1, (const unsigned char *)"abc", 3, o); bad |= strcmp(o, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	psi_digest_hex(1, (const unsigned char *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, o);
	bad |= strcmp(o, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	/* RFC 7616 3.9.1: Mufasa, http-auth@example.org, GET /dir/index.html */
	{
		char ha1[65], ha2[65], buf[400];
		psi_digest_hex(1, (const unsigned char *)"Mufasa:http-auth@example.org:Circle of Life", 43, ha1);
		psi_digest_hex(1, (const unsigned char *)"GET:/dir/index.html", 19, ha2);
		sprintf(buf, "%s:7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v:00000001:f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ:auth:%s", ha1, ha2);
		psi_digest_hex(1, (const unsigned char *)buf, (int)strlen(buf), o);
		bad |= strcmp(o, "753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1");
		psi_digest_hex(0, (const unsigned char *)"Mufasa:http-auth@example.org:Circle of Life", 43, ha1);
		psi_digest_hex(0, (const unsigned char *)"GET:/dir/index.html", 19, ha2);
		sprintf(buf, "%s:7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v:00000001:f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ:auth:%s", ha1, ha2);
		psi_digest_hex(0, (const unsigned char *)buf, (int)strlen(buf), o);
		bad |= strcmp(o, "8ca523f5e9506fed4657c9700eebdbec");
	}
	printf(bad ? "FAIL\n" : "ok\n");
	return bad != 0;
}
#endif
