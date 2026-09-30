/* pmpng.c - PNG to 16 greys, with zlib's inflate (ssh/zlib).
 *
 * All colour types and bit depths; a palette or an alpha channel is laid
 * over white (mail is read on a white page). Rows are unfiltered and
 * turned to grey as they come out of inflate, two rows in memory at a
 * time; an interlaced (Adam7) picture needs a whole 8-bit copy, so it is
 * refused above max_full_bytes. Chunk CRCs are not checked: inflate's own
 * check catches most damage, and a damaged row is better shown than not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmimg.h"
#include "zlib.h"

typedef struct
	{
	int w, h, depth, ctype, interlace;
	int channels, bpp;           /* bytes per complete pixel, at least 1 */
	int bits_per_pixel;
	int has_trns;
	unsigned char pal_grey[256]; /* palette entries as grey */
	unsigned char pal_alpha[256];
	int trns_key[3];             /* the transparent colour (grey / rgb) at the file's depth */
	} PngInfo;

static unsigned char to_grey(int r, int g, int b)
{
	return (unsigned char)((r * 77 + g * 150 + b * 29) >> 8);
}

/* n bits from a packed row */
static int sample(const unsigned char *row, int i, int depth)
{
	switch (depth) {
	case 1: return (row[i >> 3] >> (7 - (i & 7))) & 1;
	case 2: return (row[i >> 2] >> (6 - 2 * (i & 3))) & 3;
	case 4: return (row[i >> 1] >> (4 - 4 * (i & 1))) & 15;
	case 8: return row[i];
	default: return (row[i * 2] << 8) | row[i * 2 + 1];
	}
}

/* one unfiltered row (w pixels) to grey */
static void row_to_grey(const PngInfo *p, const unsigned char *row, unsigned char *grey, int w)
{
	int x, d = p->depth;
	int maxv = (1 << (d > 8 ? 8 : d)) - 1;
	for (x = 0; x < w; x++) {
		int v, a;
		switch (p->ctype) {
		case 0: {                                   /* grey */
			int s = sample(row, x, d);
			if (p->has_trns && s == p->trns_key[0]) { grey[x] = 255; break; }
			if (d == 16) s >>= 8;
			grey[x] = (unsigned char)(s * 255 / maxv);
			break;
		}
		case 2: {                                   /* rgb */
			int r, g, b;
			if (d == 16) {
				r = (row[x * 6] << 8) | row[x * 6 + 1]; g = (row[x * 6 + 2] << 8) | row[x * 6 + 3]; b = (row[x * 6 + 4] << 8) | row[x * 6 + 5];
				if (p->has_trns && r == p->trns_key[0] && g == p->trns_key[1] && b == p->trns_key[2]) { grey[x] = 255; break; }
				r >>= 8; g >>= 8; b >>= 8;
			} else {
				r = row[x * 3]; g = row[x * 3 + 1]; b = row[x * 3 + 2];
				if (p->has_trns && r == p->trns_key[0] && g == p->trns_key[1] && b == p->trns_key[2]) { grey[x] = 255; break; }
			}
			grey[x] = to_grey(r, g, b);
			break;
		}
		case 3: {                                   /* palette */
			int i = sample(row, x, d);
			v = p->pal_grey[i];
			a = p->pal_alpha[i];
			grey[x] = (unsigned char)((v * a + 255 * (255 - a)) / 255);
			break;
		}
		case 4: {                                   /* grey + alpha */
			if (d == 16) { v = row[x * 4]; a = row[x * 4 + 2]; }
			else { v = row[x * 2]; a = row[x * 2 + 1]; }
			grey[x] = (unsigned char)((v * a + 255 * (255 - a)) / 255);
			break;
		}
		default: {                                  /* rgb + alpha */
			int r, g, b;
			if (d == 16) { r = row[x * 8]; g = row[x * 8 + 2]; b = row[x * 8 + 4]; a = row[x * 8 + 6]; }
			else { r = row[x * 4]; g = row[x * 4 + 1]; b = row[x * 4 + 2]; a = row[x * 4 + 3]; }
			v = to_grey(r, g, b);
			grey[x] = (unsigned char)((v * a + 255 * (255 - a)) / 255);
			break;
		}
		}
	}
}

static int paeth(int a, int b, int c)
{
	int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
	if (pa <= pb && pa <= pc) return a;
	return pb <= pc ? b : c;
}

/* undoes the row's filter in place; prev may be 0 for the first row */
static int unfilter(unsigned char *row, const unsigned char *prev, int n, int bpp)
{
	int f = row[0], i;
	unsigned char *d = row + 1;
	switch (f) {
	case 0: break;
	case 1: for (i = bpp; i < n; i++) d[i] = (unsigned char)(d[i] + d[i - bpp]); break;
	case 2: if (prev) for (i = 0; i < n; i++) d[i] = (unsigned char)(d[i] + prev[i + 1]); break;
	case 3:
		for (i = 0; i < n; i++) {
			int left = i >= bpp ? d[i - bpp] : 0, up = prev ? prev[i + 1] : 0;
			d[i] = (unsigned char)(d[i] + ((left + up) >> 1));
		}
		break;
	case 4:
		for (i = 0; i < n; i++) {
			int left = i >= bpp ? d[i - bpp] : 0, up = prev ? prev[i + 1] : 0, ul = (prev && i >= bpp) ? prev[i + 1 - bpp] : 0;
			d[i] = (unsigned char)(d[i] + paeth(left, up, ul));
		}
		break;
	default: return -1;
	}
	return 0;
}

static const int k_pass_x0[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const int k_pass_y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const int k_pass_dx[7] = { 8, 8, 4, 4, 2, 2, 1 };
static const int k_pass_dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

typedef struct
	{
	PngInfo info;
	z_stream z;
	int z_ok;
	unsigned char *row, *prev, *grey;    /* row buffers: 1 + rowbytes each */
	int rowbytes;                        /* of the current pass */
	unsigned char *full;                 /* interlaced: the whole picture, grey */
	PmImgSink *sink;
	int pass, pass_w, pass_h, pass_y;    /* interlaced progress */
	int rows_out;
	int done;
	int fail;
	} Png;

static int pass_setup(Png *g)
{
	PngInfo *p = &g->info;
	for (;;) {
		if (g->pass >= (p->interlace ? 7 : 1)) { g->done = 1; return 0; }
		if (p->interlace) {
			g->pass_w = (p->w - k_pass_x0[g->pass] + k_pass_dx[g->pass] - 1) / k_pass_dx[g->pass];
			g->pass_h = (p->h - k_pass_y0[g->pass] + k_pass_dy[g->pass] - 1) / k_pass_dy[g->pass];
		} else { g->pass_w = p->w; g->pass_h = p->h; }
		if (g->pass_w > 0 && g->pass_h > 0) break;
		g->pass++;
	}
	g->pass_y = 0;
	g->rowbytes = (g->pass_w * p->bits_per_pixel + 7) / 8;
	memset(g->prev, 0, g->rowbytes + 1);
	return 1;
}

/* a complete filtered row is in g->row */
static int row_done(Png *g)
{
	PngInfo *p = &g->info;
	if (unfilter(g->row, g->pass_y ? g->prev : 0, g->rowbytes, p->bpp) != 0) return -1;
	row_to_grey(p, g->row + 1, g->grey, g->pass_w);
	if (p->interlace) {
		int x, y = k_pass_y0[g->pass] + g->pass_y * k_pass_dy[g->pass];
		unsigned char *dst = g->full + (long)y * p->w + k_pass_x0[g->pass];
		for (x = 0; x < g->pass_w; x++) dst[(long)x * k_pass_dx[g->pass]] = g->grey[x];
	} else {
		pmimg_sink_row(g->sink, g->grey);
		g->rows_out++;
	}
	{ unsigned char *t = g->row; g->row = g->prev; g->prev = t; }
	if (++g->pass_y >= g->pass_h) { g->pass++; pass_setup(g); }
	return 0;
}

/* feeds IDAT bytes through inflate, a row at a time */
static int idat(Png *g, unsigned char *data, int n)
{
	g->z.next_in = data;
	g->z.avail_in = (uInt)n;
	while (g->z.avail_in && !g->done) {
		int r;
		if (g->z.avail_out == 0) {
			g->z.next_out = g->row;
			g->z.avail_out = (uInt)(g->rowbytes + 1);
		}
		r = inflate(&g->z, Z_NO_FLUSH);
		if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) return -1;
		if (g->z.avail_out == 0) {
			if (row_done(g) != 0) return -1;
			if (!g->done) { g->z.next_out = g->row; g->z.avail_out = (uInt)(g->rowbytes + 1); }
		}
		if (r == Z_STREAM_END) { if (!g->done) return -2; break; }
		if (r == Z_BUF_ERROR && g->z.avail_in == 0) break;
	}
	return 0;
}

int pmimg_decode_png(PmImgIn *in, const PmImgOpts *from, PmImage *out, char *err, int errmax)
{
	PmImgOpts o;
	Png g;
	PngInfo *p = &g.info;
	unsigned char buf[1024];
	int seen_ihdr = 0, seen_idat = 0, r = PMIMG_OK, e, i;
	long maxrow;

	pmimg_opts_fill(&o, from);
	memset(&g, 0, sizeof(g));
	if (!pmimg_in_skip(in, 8)) { pmimg_err(err, errmax, "a damaged PNG"); return PMIMG_E_FORMAT; }
	for (i = 0; i < 256; i++) g.info.pal_alpha[i] = 255;

	for (;;) {
		unsigned long len = pmimg_in_be32(in);
		unsigned char type[4];
		if (pmimg_in_read(in, type, 4) != 4) {
			/* the file ends early: what came is shown */
			if (!seen_idat) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, in->err ? "could not read the file" : "a damaged PNG"); }
			break;
		}
		if (len > 0x7fffffffUL) { if (!seen_idat) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged PNG"); } break; }
		if (!memcmp(type, "IHDR", 4)) {
			unsigned char h[13];
			if (len != 13 || pmimg_in_read(in, h, 13) != 13) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged PNG"); break; }
			p->w = (int)(((unsigned long)h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3]);
			p->h = (int)(((unsigned long)h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7]);
			p->depth = h[8]; p->ctype = h[9]; p->interlace = h[12];
			if (h[0] & 0x80 || h[4] & 0x80 || p->w <= 0 || p->h <= 0 || h[10] != 0 || h[11] != 0 || p->interlace > 1) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged PNG"); break; }
			switch (p->ctype) {
			case 0: p->channels = 1; if (p->depth != 1 && p->depth != 2 && p->depth != 4 && p->depth != 8 && p->depth != 16) r = PMIMG_E_FORMAT; break;
			case 2: p->channels = 3; if (p->depth != 8 && p->depth != 16) r = PMIMG_E_FORMAT; break;
			case 3: p->channels = 1; if (p->depth != 1 && p->depth != 2 && p->depth != 4 && p->depth != 8) r = PMIMG_E_FORMAT; break;
			case 4: p->channels = 2; if (p->depth != 8 && p->depth != 16) r = PMIMG_E_FORMAT; break;
			case 6: p->channels = 4; if (p->depth != 8 && p->depth != 16) r = PMIMG_E_FORMAT; break;
			default: r = PMIMG_E_FORMAT;
			}
			if (r) { pmimg_err(err, errmax, "a PNG of a kind not supported"); r = PMIMG_E_UNSUPPORTED; break; }
			p->bits_per_pixel = p->channels * p->depth;
			p->bpp = (p->bits_per_pixel + 7) / 8;
			if (p->w > PMIMG_MAX_DIM || p->h > PMIMG_MAX_DIM || (long)p->w * p->h > o.max_src_pixels ||
			    (p->interlace && (long)p->w * p->h > o.max_full_bytes)) { r = PMIMG_E_TOO_BIG; pmimg_err(err, errmax, "too big"); break; }
			maxrow = ((long)p->w * p->bits_per_pixel + 7) / 8 + 1;
			g.row = (unsigned char *)malloc(maxrow);
			g.prev = (unsigned char *)malloc(maxrow);
			g.grey = (unsigned char *)malloc(p->w);
			if (p->interlace) g.full = (unsigned char *)malloc((long)p->w * p->h);
			if (!g.row || !g.prev || !g.grey || (p->interlace && !g.full)) { r = PMIMG_E_MEMORY; pmimg_err(err, errmax, "not enough memory"); break; }
			if (p->interlace) memset(g.full, 0xcc, (long)p->w * p->h);
			g.sink = pmimg_sink_new(p->w, p->h, &o, PMIMG_PNG, &e);
			if (!g.sink) { r = e; pmimg_err(err, errmax, e == PMIMG_E_TOO_BIG ? "too big" : "not enough memory"); break; }
			seen_ihdr = 1;
			pmimg_in_skip(in, 4);                 /* crc */
			continue;
		}
		if (!seen_ihdr) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged PNG"); break; }
		if (!memcmp(type, "PLTE", 4)) {
			int n = (int)(len / 3);
			if (n > 256) n = 256;
			for (i = 0; i < n; i++) {
				unsigned char c[3];
				if (pmimg_in_read(in, c, 3) != 3) break;
				p->pal_grey[i] = to_grey(c[0], c[1], c[2]);
			}
			if (!pmimg_in_skip(in, (long)len - 3L * i + 4)) break;
			continue;
		}
		if (!memcmp(type, "tRNS", 4)) {
			if (p->ctype == 3) {
				int n = (int)len;
				if (n > 256) n = 256;
				for (i = 0; i < n; i++) { int c = pmimg_in_byte(in); if (c < 0) break; p->pal_alpha[i] = (unsigned char)c; }
				if (!pmimg_in_skip(in, (long)len - i + 4)) break;
			} else if (p->ctype == 0 && len >= 2) {
				p->trns_key[0] = (pmimg_in_byte(in) << 8); p->trns_key[0] |= pmimg_in_byte(in) & 255;
				if (p->depth < 16) p->trns_key[0] &= (1 << p->depth) - 1;
				p->has_trns = 1;
				if (!pmimg_in_skip(in, (long)len - 2 + 4)) break;
			} else if (p->ctype == 2 && len >= 6) {
				for (i = 0; i < 3; i++) { p->trns_key[i] = pmimg_in_byte(in) << 8; p->trns_key[i] |= pmimg_in_byte(in) & 255; if (p->depth == 8) p->trns_key[i] &= 255; }
				p->has_trns = 1;
				if (!pmimg_in_skip(in, (long)len - 6 + 4)) break;
			} else if (!pmimg_in_skip(in, (long)len + 4)) break;
			continue;
		}
		if (!memcmp(type, "IDAT", 4)) {
			long left = (long)len;
			if (!seen_idat) {
				g.z.zalloc = Z_NULL; g.z.zfree = Z_NULL; g.z.opaque = Z_NULL;
				if (inflateInit(&g.z) != Z_OK) { r = PMIMG_E_MEMORY; pmimg_err(err, errmax, "not enough memory"); break; }
				g.z_ok = 1;
				g.pass = 0;
				pass_setup(&g);
				g.z.next_out = g.row;
				g.z.avail_out = (uInt)(g.rowbytes + 1);
				seen_idat = 1;
			}
			while (left > 0 && !g.done && !g.fail) {
				int n = pmimg_in_read(in, buf, left > (long)sizeof(buf) ? (int)sizeof(buf) : (int)left);
				if (n <= 0) break;
				left -= n;
				if (idat(&g, buf, n) != 0) g.fail = 1;
				if (pmimg_sink_abort(g.sink)) { r = PMIMG_E_ABORTED; pmimg_err(err, errmax, "stopped"); break; }
			}
			if (r == PMIMG_E_ABORTED) break;
			if (g.fail || g.done) break;         /* (damage: what came is shown) */
			if (!pmimg_in_skip(in, left + 4)) break;
			continue;
		}
		if (!memcmp(type, "IEND", 4)) break;
		if (!pmimg_in_skip(in, (long)len + 4)) break;
	}
	if (g.z_ok) inflateEnd(&g.z);
	free(g.row);
	free(g.prev);
	free(g.grey);
	if (r == PMIMG_OK && seen_idat) {
		if (g.fail && !g.rows_out && !(p->interlace && g.pass > 0)) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged PNG"); }
		if (r == PMIMG_OK && p->interlace) {
			pmimg_sink_all(g.sink, g.full);
			if (!g.done) { /* not all passes came: the partial flag from the sink is
			                  not set, since every row was fed; say so anyway */
				(void)0;
			}
		}
	} else if (r == PMIMG_OK) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged PNG"); }
	free(g.full);
	if (r != PMIMG_OK) { pmimg_sink_abandon(g.sink); return r; }
	r = pmimg_sink_end(g.sink, out);
	if (p->interlace && !g.done) out->partial = 1;
	return r;
}
