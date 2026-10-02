/* psi_grey.c - pw_grey_convert for the Links engine: the same result as
 * web/fb/pwgrey.c (RGB565 to 16 greys, 4x4 ordered dither), faster for
 * what Links mostly draws (the ARM710 has no divide instruction, so the
 * division by 255 is a multiply here). A web page is largely white paper and black
 * ink, so two pixels at a time are read as one word and a pair of white or
 * a pair of black pixels is written straight out. Measured in the ARM
 * harness, this halves the cost of putting a screen on the display.
 *
 * web/fb/pwgrey.c stays as it is for the NetSurf engine. */
#include "pwback.h"

static const unsigned char bayer[4][4] = {
	{   8, 136,  40, 168 },
	{ 200,  72, 232, 104 },
	{  56, 184,  24, 152 },
	{ 248, 120, 216,  88 }
};

#define GREY(p, q, f) do { \
	unsigned int r_ = ((p) >> 11) & 31, g_ = ((p) >> 5) & 63, b_ = (p) & 31, v_; \
	r_ = (r_ << 3) | (r_ >> 2); g_ = (g_ << 2) | (g_ >> 4); b_ = (b_ << 3) | (b_ >> 2); \
	v_ = ((r_ * 77 + g_ * 151 + b_ * 28) >> 8) * 15; \
	q = (v_ * 0x8081) >> 23; f = v_ - q * 255; } while (0)	/* v_/255, exact for v_ <= 3825 */

void pw_grey_convert(const unsigned short *fb, int fbw, unsigned char *out, int stride,
                     int x0, int y0, int x1, int y1)
{
	int x, y;
	unsigned int last = 0x10000, q = 15, f = 0;

	x0 &= ~1;
	if (x1 & 1) x1++;
	if (x1 > fbw) x1 = fbw;
	for (y = y0; y < y1; y++) {
		const unsigned short *src = fb + y * fbw;
		/* (fb is word-aligned and fbw even, so each pair is one word) */
		const unsigned int *src2 = (const unsigned int *)(src + x0);
		unsigned char *dst = out + y * stride + (x0 >> 1);
		const unsigned char *th = bayer[y & 3];
		for (x = x0; x < x1; x += 2, src2++, dst++) {
			unsigned int w = *src2, p, a, b;
			if (w == 0xffffffffu) { *dst = 0xff; continue; }	/* white: grey 15, never dithered */
			if (w == 0) { *dst = 0; continue; }			/* black */
			p = w & 0xffff;		/* little-endian: the left pixel is the low half */
			if (p != last) { GREY(p, q, f); last = p; }
			a = q + (f + th[x & 3] >= 255);
			p = w >> 16;
			if (p != last) { GREY(p, q, f); last = p; }
			b = q + (f + th[(x + 1) & 3] >= 255);
			*dst = (unsigned char)(a | (b << 4));
		}
	}
}
