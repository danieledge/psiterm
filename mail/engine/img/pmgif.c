/* pmgif.c - GIF (the first frame) to 16 greys.
 *
 * GIF87a and 89a, global or local palette, the transparent colour shown
 * as white. A frame smaller than the screen it is meant for is laid on a
 * white screen; an interlaced frame is put together in an 8-bit copy of
 * the whole picture (refused above max_full_bytes), otherwise rows go to
 * the sink as the LZW codes come out. Every code and table index is
 * bounded, so a damaged file gives an error or a picture cut short, not
 * a crash. Later frames (animation) are not read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmimg.h"

#define LZW_MAX 4096

typedef struct
	{
	PmImgIn *in;
	int sub_left;            /* bytes left in the current data sub-block */
	int eod;
	unsigned int bits;       /* bit buffer */
	int nbits;
	} LzwIn;

/* the next LZW code, or -1 at the end of the data */
static int lzw_code(LzwIn *l, int size)
{
	while (l->nbits < size) {
		int c;
		if (l->eod) return -1;
		if (l->sub_left == 0) {
			c = pmimg_in_byte(l->in);
			if (c <= 0) { l->eod = 1; return -1; }
			l->sub_left = c;
		}
		c = pmimg_in_byte(l->in);
		if (c < 0) { l->eod = 1; return -1; }
		l->sub_left--;
		l->bits |= (unsigned int)c << l->nbits;
		l->nbits += 8;
	}
	{
		int code = (int)(l->bits & ((1u << size) - 1));
		l->bits >>= size;
		l->nbits -= size;
		return code;
	}
}

/* skips the rest of a run of sub-blocks */
static int skip_blocks(PmImgIn *in)
{
	for (;;) {
		int n = pmimg_in_byte(in);
		if (n < 0) return 0;
		if (n == 0) return 1;
		if (!pmimg_in_skip(in, n)) return 0;
	}
}

typedef struct
	{
	unsigned short prefix[LZW_MAX];
	unsigned char suffix[LZW_MAX];
	unsigned char stack[LZW_MAX + 1];
	} LzwTables;

int pmimg_decode_gif(PmImgIn *in, const PmImgOpts *from, PmImage *out, char *err, int errmax)
{
	PmImgOpts o;
	unsigned char hdr[13], pal[256], lpal[256];
	unsigned char *row = 0, *full = 0;
	LzwTables *t = 0;
	PmImgSink *sink = 0;
	int sw, sh, gflags, transparent = -1, have_frame = 0, r = PMIMG_OK, e, i;
	int fx = 0, fy = 0, fw = 0, fh = 0, interlace = 0, ncol = 0;
	const unsigned char *use_pal = pal;
	int whole;               /* the frame is the whole picture: rows can stream */

	pmimg_opts_fill(&o, from);
	memset(pal, 255, sizeof(pal));
	if (pmimg_in_read(in, hdr, 13) != 13) { pmimg_err(err, errmax, "a damaged GIF"); return PMIMG_E_FORMAT; }
	sw = hdr[6] | (hdr[7] << 8);
	sh = hdr[8] | (hdr[9] << 8);
	gflags = hdr[10];
	if (gflags & 0x80) {
		int n = 2 << (gflags & 7);
		for (i = 0; i < n; i++) {
			unsigned char c[3];
			if (pmimg_in_read(in, c, 3) != 3) { pmimg_err(err, errmax, "a damaged GIF"); return PMIMG_E_FORMAT; }
			pal[i] = (unsigned char)((c[0] * 77 + c[1] * 150 + c[2] * 29) >> 8);
		}
		ncol = n;
	}
	/* blocks up to the first image */
	for (;;) {
		int b = pmimg_in_byte(in);
		if (b < 0 || b == 0x3b) { pmimg_err(err, errmax, "a GIF with no picture in it"); return PMIMG_E_FORMAT; }
		if (b == 0x21) {
			int label = pmimg_in_byte(in);
			if (label == 0xf9) {
				int n = pmimg_in_byte(in);
				unsigned char gce[8];
				if (n < 4 || n > 8 || pmimg_in_read(in, gce, n) != n) { pmimg_err(err, errmax, "a damaged GIF"); return PMIMG_E_FORMAT; }
				if (gce[0] & 1) transparent = gce[3];
				if (!skip_blocks(in)) { pmimg_err(err, errmax, "a damaged GIF"); return PMIMG_E_FORMAT; }
			} else if (!skip_blocks(in)) { pmimg_err(err, errmax, "a damaged GIF"); return PMIMG_E_FORMAT; }
			continue;
		}
		if (b == 0x2c) {
			unsigned char d[9];
			int lflags;
			if (pmimg_in_read(in, d, 9) != 9) { pmimg_err(err, errmax, "a damaged GIF"); return PMIMG_E_FORMAT; }
			fx = d[0] | (d[1] << 8); fy = d[2] | (d[3] << 8);
			fw = d[4] | (d[5] << 8); fh = d[6] | (d[7] << 8);
			lflags = d[8];
			interlace = (lflags & 0x40) != 0;
			if (lflags & 0x80) {
				int n = 2 << (lflags & 7);
				memset(lpal, 255, sizeof(lpal));
				for (i = 0; i < n; i++) {
					unsigned char c[3];
					if (pmimg_in_read(in, c, 3) != 3) { pmimg_err(err, errmax, "a damaged GIF"); return PMIMG_E_FORMAT; }
					lpal[i] = (unsigned char)((c[0] * 77 + c[1] * 150 + c[2] * 29) >> 8);
				}
				use_pal = lpal;
				ncol = n;
			}
			have_frame = 1;
			break;
		}
		pmimg_err(err, errmax, "a damaged GIF");
		return PMIMG_E_FORMAT;
	}
	(void)have_frame;
	if (fw <= 0 || fh <= 0) { pmimg_err(err, errmax, "a damaged GIF"); return PMIMG_E_FORMAT; }
	/* a screen smaller than the frame (or none): the frame is the picture */
	if (sw <= 0 || sh <= 0 || fx + fw > sw || fy + fh > sh) { sw = fw; sh = fh; fx = fy = 0; }
	if (sw > PMIMG_MAX_DIM || sh > PMIMG_MAX_DIM || (long)sw * sh > o.max_src_pixels) { pmimg_err(err, errmax, "too big"); return PMIMG_E_TOO_BIG; }
	whole = !interlace && fx == 0 && fy == 0 && fw == sw && fh == sh;
	if (!whole && (long)sw * sh > o.max_full_bytes) { pmimg_err(err, errmax, "too big"); return PMIMG_E_TOO_BIG; }
	if (!ncol) ncol = 256;
	if (transparent >= 0 && transparent < 256) {
		/* over white */
		if (use_pal == pal) pal[transparent] = 255; else lpal[transparent] = 255;
	}

	sink = pmimg_sink_new(sw, sh, &o, PMIMG_GIF, &e);
	if (!sink) { pmimg_err(err, errmax, e == PMIMG_E_TOO_BIG ? "too big" : "not enough memory"); return e; }
	row = (unsigned char *)malloc(fw);
	t = (LzwTables *)malloc(sizeof(LzwTables));
	if (!whole) full = (unsigned char *)malloc((long)sw * sh);
	if (!row || !t || (!whole && !full)) { r = PMIMG_E_MEMORY; pmimg_err(err, errmax, "not enough memory"); goto done; }
	if (full) memset(full, 255, (long)sw * sh);

	/* the LZW stream */
	{
		LzwIn l;
		int min = pmimg_in_byte(in), clear, end, size, next, prev = -1, first = 0, x = 0, y = 0;
		int pass = 0, il_y = 0;
		static const int il_start[4] = { 0, 4, 2, 1 }, il_step[4] = { 8, 8, 4, 2 };
		if (min < 1 || min > 11) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged GIF"); goto done; }
		memset(&l, 0, sizeof(l));
		l.in = in;
		clear = 1 << min;
		end = clear + 1;
		size = min + 1;
		next = end + 1;
		while (y < fh) {
			int code = lzw_code(&l, size), sp = 0, c;
			if (code < 0 || code == end) break;
			if (code == clear) { size = min + 1; next = end + 1; prev = -1; continue; }
			if (prev < 0) {
				if (code >= clear) break;                    /* must be a root */
				t->stack[sp++] = (unsigned char)code;
				first = code;
				prev = code;
			} else {
				if (code < next) c = code;
				else if (code == next) { c = prev; t->stack[sp++] = (unsigned char)first; }
				else break;                                  /* damaged */
				while (c >= clear) {
					if (c >= LZW_MAX || c <= end || sp >= LZW_MAX) { c = -1; break; }
					t->stack[sp++] = t->suffix[c];
					c = t->prefix[c];
				}
				if (c < 0) break;
				t->stack[sp++] = (unsigned char)c;
				first = c;
				if (next < LZW_MAX) {
					t->prefix[next] = (unsigned short)prev;
					t->suffix[next] = (unsigned char)first;
					next++;
					if (next == (1 << size) && size < 12) size++;
				}
				prev = code;
			}
			/* out, reversed */
			while (sp > 0 && y < fh) {
				int idx = t->stack[--sp];
				row[x++] = idx < ncol ? use_pal[idx] : 255;
				if (x == fw) {
					x = 0;
					if (whole) pmimg_sink_row(sink, row);
					else {
						int yy = y;
						if (interlace) {
							yy = il_y;
							il_y += il_step[pass];
							while (il_y >= fh && pass < 3) { pass++; il_y = il_start[pass]; }
						}
						if (yy < fh) memcpy(full + (long)(fy + yy) * sw + fx, row, fw);
					}
					y++;
					if ((y & 15) == 0 && pmimg_sink_abort(sink)) { r = PMIMG_E_ABORTED; pmimg_err(err, errmax, "stopped"); goto done; }
				}
			}
		}
		if (y == 0) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged GIF"); goto done; }
		if (!whole) pmimg_sink_all(sink, full);
		r = pmimg_sink_end(sink, out);
		sink = 0;
		if (y < fh) out->partial = 1;
	}
done:
	free(row);
	free(t);
	free(full);
	if (sink) pmimg_sink_abandon(sink);
	return r;
}
