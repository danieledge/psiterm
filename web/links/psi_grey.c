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
 * web/fb/pwgrey.c stays as it is for the NetSurf engine.
 *
 * Display work (October 2026, docs/display.md): the grey calibration
 * (ssh/psigrey.h, C:\System\Data\PsiGrey.ini) and the dithering choice.
 *   - Error diffusion (the standard): pictures are dithered once, when
 *     psi_drv.c packs their bitmaps (Floyd-Steinberg, to the calibrated
 *     levels, stored as lv[level]); everything else - text, its
 *     anti-aliased edges, rules, backgrounds - is not dithered at all: each
 *     pixel takes the nearest calibrated level. All 16 threshold tables
 *     are then the same table, so the loop below is unchanged.
 *   - Ordered (the old way): the 4x4 ordered dither over the whole frame,
 *     between the calibrated levels; with the calibration off it is
 *     exactly the old output. */
#include <stdio.h>
#include <string.h>
#include "pwback.h"
#include "../../ssh/psigrey.h"

static const unsigned char bayer[4][4] = {
	{   8, 136,  40, 168 },
	{ 200,  72, 232, 104 },
	{  56, 184,  24, 152 },
	{ 248, 120, 216,  88 }
};

static unsigned short rt[32], gt[64], bt[32];	/* r8 * 77, g8 * 151, b8 * 28 */
static unsigned char dt[16][256];		/* [y & 3][x & 3]: (15v + bayer) / 255 */
/* v for every 565 pixel (64 KB, made once at the first use): one load in
   place of three table loads, the adds and the shift. Phase 5: the grey
   conversion was a third of a Page Down on 68k.news. */
static unsigned char vt[65536];
static int ready;

static unsigned char lv[16];		/* how light each level looks (psigrey.h) */
static unsigned char cal[256];		/* v -> the nearest level */
static int diffuse = 1;			/* pictures by error diffusion, the rest undithered */
static char grey_text[512];		/* the file as last read: to see a change */
static int grey_len = -1;

/* reads PsiGrey.ini; 1 if it is not what it was */
static int read_greys(void)
{
	static char buf[512];
	PsiGrey g;
	FILE *f = fopen(PSIGREY_FILE, "rb");
	int n = 0;
	if (f) { n = (int)fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
	if (n < 0) n = 0;
	if (n == grey_len && !memcmp(buf, grey_text, n)) return 0;
	memcpy(grey_text, buf, n);
	grey_len = n;
	if (n) psigrey_parse(&g, buf, n);
	else psigrey_defaults(&g);
	psigrey_levels(&g, lv);
	psigrey_nearest(lv, cal);
	diffuse = g.dither;
	return 1;
}

static void make_level_tables(void)
{
	int t, v;
	for (t = 0; t < 16; t++) {
		unsigned th = bayer[t >> 2][t & 3];
		int q = 0;
		for (v = 0; v < 256; v++) {
			if (diffuse) { dt[t][v] = cal[v]; continue; }
			/* the level at or below v, one up where v's way on to the
			   next level passes the threshold: (15v + th) / 255 when
			   the levels are linear, as before */
			while (q < 15 && v >= lv[q + 1]) q++;
			if (q == 15 || v <= lv[q]) dt[t][v] = (unsigned char)q;
			else dt[t][v] = (unsigned char)(q + ((unsigned)(v - lv[q]) * 255 / (lv[q + 1] - lv[q]) + th >= 255));
		}
	}
}

static void make_tables(void)
{
	int i, r, g, b;
	unsigned char *o = vt;
	for (i = 0; i < 32; i++) {
		int c5 = (i << 3) | (i >> 2);
		rt[i] = (unsigned short)(c5 * 77);
		bt[i] = (unsigned short)(c5 * 28);
	}
	for (i = 0; i < 64; i++)
		gt[i] = (unsigned short)(((i << 2) | (i >> 4)) * 151);
	for (r = 0; r < 32; r++)
		for (g = 0; g < 64; g++) {
			unsigned base = rt[r] + gt[g];
			for (b = 0; b < 32; b++)
				*o++ = (unsigned char)((base + bt[b]) >> 8);
		}
	read_greys();
	make_level_tables();
	ready = 1;
}

/* PsiGrey.ini again (at each page): 1 if the greys changed, and the
   tables are made again */
int pw_grey_check(void)
{
	if (!ready) { make_tables(); return 0; }
	if (!read_greys()) return 0;
	make_level_tables();
	return 1;
}

/* for psi_drv.c's pictures: 1 if they are to be error-diffused, with the
   levels and the nearest-level table */
int pw_grey_diffuse(const unsigned char **levels, const unsigned char **nearest)
{
	if (!ready) make_tables();
	*levels = lv;
	*nearest = cal;
	return diffuse;
}

/* v of every 565 pixel, for psi_drv.c's grey picture bitmaps */
const unsigned char *pw_v565_table(void)
{
	if (!ready) make_tables();
	return vt;
}

/* a pair that is neither white nor black; d is the left pixel's threshold
   table (the right one's is the next); little-endian: the left pixel is
   the low half */
#define grey_pair(w, d) ((d)[vt[(w) & 0xffff]] | ((unsigned int)(d)[256 + vt[(w) >> 16]] << 4))
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
