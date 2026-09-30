/* pwgrey.c - RGB565 to the Psion's 16 greys with a 4x4 ordered dither.
 *
 * Plain black, white and the 16 exact greys come out undithered, so text
 * stays crisp; photos and coloured backgrounds get a fine, even pattern. */
#include "pwback.h"

/* Bayer thresholds scaled to 0..254 (one grey step = 255 in v below) */
static const unsigned char bayer[4][4] = {
	{   8, 136,  40, 168 },
	{ 200,  72, 232, 104 },
	{  56, 184,  24, 152 },
	{ 248, 120, 216,  88 }
};

/* one colour's grey: whole level and the fraction towards the next (0..254) */
#define GREY(p, q, f) do { \
	unsigned int r_ = ((p) >> 11) & 31, g_ = ((p) >> 5) & 63, b_ = (p) & 31, v_; \
	r_ = (r_ << 3) | (r_ >> 2); g_ = (g_ << 2) | (g_ >> 4); b_ = (b_ << 3) | (b_ >> 2); \
	v_ = ((r_ * 77 + g_ * 151 + b_ * 28) >> 8) * 15; \
	q = v_ / 255; f = v_ - q * 255; } while (0)

void pw_grey_convert(const unsigned short *fb, int fbw, unsigned char *out, int stride,
                     int x0, int y0, int x1, int y1)
{
	int x, y;
	unsigned int last = 0x10000, q = 15, f = 0;  /* ARM710 has no divide: */
	                                              /* only on a colour change */
	x0 &= ~1;                              /* whole bytes */
	if (x1 & 1) x1++;
	if (x1 > fbw) x1 = fbw;
	for (y = y0; y < y1; y++) {
		const unsigned short *src = fb + y * fbw;
		unsigned char *dst = out + y * stride;
		const unsigned char *th = bayer[y & 3];
		for (x = x0; x < x1; x += 2) {
			unsigned int p, a, b;
			p = src[x];
			if (p != last) { GREY(p, q, f); last = p; }
			a = q + (f + th[x & 3] >= 255);
			p = src[x + 1];
			if (p != last) { GREY(p, q, f); last = p; }
			b = q + (f + th[(x + 1) & 3] >= 255);
			dst[x >> 1] = (unsigned char)(a | (b << 4));
		}
	}
}
