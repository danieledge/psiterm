/* psi_grey.c - pw_grey_convert for the Links engine: the same result as
 * web/fb/pwgrey.c (RGB565 to 16 greys, 4x4 ordered dither), faster for
 * what Links mostly draws (the ARM710 has no divide instruction, and its
 * multiplies take several cycles).
 *
 * pwgrey.c works out, for each pixel,
 *     v  = (r8 * 77 + g8 * 151 + b8 * 28) >> 8     (0..255)
 *     q  = 15v / 255,  f = 15v - 255q
 *     grey = q + (f + bayer >= 255)
 * which is the same as grey = (15v + bayer) / 255, as bayer < 255. Here:
 *   - v is the sum of three small tables (r, g and b already multiplied);
 *   - grey comes from a 256-entry table for each of the 16 dither
 *     thresholds (4 KB), so a pixel is a few loads and adds;
 *   - a page is mostly white paper and black ink, so two pixels are read
 *     as one word and a pair of white or of black is written straight out;
 *     runs of the same colour (anti-aliased text has few) reuse v.
 * Measured in the ARM harness: about 3 times faster than the multiply
 * version on text, the same pictures to the bit.
 *
 * web/fb/pwgrey.c stays as it is for the NetSurf engine. */
#include "pwback.h"

static const unsigned char bayer[4][4] = {
	{   8, 136,  40, 168 },
	{ 200,  72, 232, 104 },
	{  56, 184,  24, 152 },
	{ 248, 120, 216,  88 }
};

static unsigned short rt[32], gt[64], bt[32];	/* r8 * 77, g8 * 151, b8 * 28 */
static unsigned char dt[16][256];		/* [y & 3][x & 3]: (15v + bayer) / 255 */
static int ready;

static void make_tables(void)
{
	int i, t, v;
	for (i = 0; i < 32; i++) {
		int c5 = (i << 3) | (i >> 2);
		rt[i] = (unsigned short)(c5 * 77);
		bt[i] = (unsigned short)(c5 * 28);
	}
	for (i = 0; i < 64; i++)
		gt[i] = (unsigned short)(((i << 2) | (i >> 4)) * 151);
	for (t = 0; t < 16; t++) {
		unsigned th = bayer[t >> 2][t & 3], q = 0, acc = th;	/* acc = 15v + th - 255q */
		for (v = 0; v < 256; v++) {
			while (acc >= 255) { acc -= 255; q++; }
			dt[t][v] = (unsigned char)q;
			acc += 15;
		}
	}
	ready = 1;
}

#define V565(p) ((rt[(p) >> 11] + gt[((p) >> 5) & 63] + bt[(p) & 31]) >> 8)

/* a pair that is neither white nor black; d is the left pixel's threshold
   table (the right one's is the next). The last colour's v is kept: runs
   of one colour are common. */
static unsigned int last = 0x10000, lv;
static unsigned int grey_pair(unsigned int w, const unsigned char *d)
{
	unsigned int p = w & 0xffff, a;	/* little-endian: the left pixel is the low half */
	if (p != last) { last = p; lv = V565(p); }
	a = d[lv];
	p = w >> 16;
	if (p != last) { last = p; lv = V565(p); }
	return a | ((unsigned int)d[256 + lv] << 4);
}
#define PAIR(w, d) ((w) == 0xffffffffu ? 0xffu : !(w) ? 0u : grey_pair(w, d))

void pw_grey_convert(const unsigned short *fb, int fbw, unsigned char *out, int stride,
                     int x0, int y0, int x1, int y1)
{
	int y;

	if (!ready) make_tables();
	x0 &= ~1;
	if (x1 & 1) x1++;
	if (x1 > fbw) x1 = fbw;
	for (y = y0; y < y1; y++) {
		/* (fb is word-aligned and fbw even, so each pair is one word) */
		const unsigned int *src2 = (const unsigned int *)(fb + y * fbw + x0);
		const unsigned int *end2 = src2 + ((x1 - x0) >> 1);
		unsigned char *dst = out + y * stride + (x0 >> 1);
		/* the row's four thresholds; x0 is even, so a pair starts at
		   x & 3 = 0 (tables 0, 1) or 2 (tables 2, 3), by turns */
		const unsigned char *da = dt[(y & 3) << 2] + (x0 & 2 ? 512 : 0);
		const unsigned char *db = dt[(y & 3) << 2] + (x0 & 2 ? 0 : 512), *t;
		/* single pairs until the output is word-aligned */
		while (src2 < end2 && ((unsigned long)dst & 3)) {
			*dst++ = (unsigned char)PAIR(*src2, da);
			src2++;
			t = da; da = db; db = t;
		}
		/* then 8 pixels (4 output bytes) at a time: paper and ink go
		   straight through */
		for (; end2 - src2 >= 4; src2 += 4, dst += 4) {
			unsigned int w0 = src2[0], w1 = src2[1], w2 = src2[2], w3 = src2[3];
			if ((w0 & w1 & w2 & w3) == 0xffffffffu) {
				*(unsigned int *)dst = 0xffffffffu;
				continue;
			}
			if (!(w0 | w1 | w2 | w3)) {
				*(unsigned int *)dst = 0;
				continue;
			}
			*(unsigned int *)dst = PAIR(w0, da) | (PAIR(w1, db) << 8) | (PAIR(w2, da) << 16) | (PAIR(w3, db) << 24);
		}
		for (; src2 < end2; src2++) {
			*dst++ = (unsigned char)PAIR(*src2, da);
			t = da; da = db; db = t;
		}
	}
}
