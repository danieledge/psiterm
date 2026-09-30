/* pmimg.c - the parts the decoders share: the buffered reader, the sink
 * that shrinks 8-bit grey rows by a whole number and dithers them to 16
 * greys (Floyd-Steinberg, serpentine), and the front door (pmimg_decode).
 * See pmimg.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmimg.h"

/* ------------------------------------------------------------- options */

void pmimg_opts_fill(PmImgOpts *o, const PmImgOpts *from)
{
	if (from) *o = *from; else memset(o, 0, sizeof(*o));
	if (o->max_w <= 0) o->max_w = 544;
	if (o->max_h <= 0) o->max_h = 960;
	if (o->max_w > 4096) o->max_w = 4096;
	if (o->max_h > 4096) o->max_h = 4096;
	if (o->max_src_pixels <= 0) o->max_src_pixels = 24L * 1000L * 1000L;
	if (o->max_full_bytes <= 0) o->max_full_bytes = 640L * 1024L;
}

void pmimg_err(char *err, int errmax, const char *text)
{
	int i = 0;
	if (!err || errmax <= 0) return;
	while (text[i] && i < errmax - 1) { err[i] = text[i]; i++; }
	err[i] = 0;
}

/* -------------------------------------------------------------- reader */

void pmimg_in_init(PmImgIn *in, PmImgRead read, PmImgRewind rewind, void *ctx)
{
	memset(in, 0, sizeof(*in));
	in->read = read;
	in->rewind = rewind;
	in->ctx = ctx;
}

int pmimg_in_rewind(PmImgIn *in)
{
	if (!in->rewind || in->rewind(in->ctx) != 0) return 0;
	in->pos = in->len = 0;
	in->eof = in->err = 0;
	in->total = 0;
	return 1;
}

static int in_fill(PmImgIn *in)
{
	int n;
	if (in->eof || in->err) return 0;
	n = in->read(in->ctx, in->buf, (int)sizeof(in->buf));
	if (n < 0) { in->err = 1; in->len = in->pos = 0; return 0; }
	if (n == 0) { in->eof = 1; in->len = in->pos = 0; return 0; }
	in->pos = 0;
	in->len = n;
	in->total += n;
	return n;
}

int pmimg_in_byte(PmImgIn *in)
{
	if (in->pos >= in->len && !in_fill(in)) return -1;
	return in->buf[in->pos++];
}

int pmimg_in_read(PmImgIn *in, unsigned char *buf, int n)
{
	int got = 0;
	while (got < n) {
		int k;
		if (in->pos >= in->len && !in_fill(in)) break;
		k = in->len - in->pos;
		if (k > n - got) k = n - got;
		memcpy(buf + got, in->buf + in->pos, k);
		in->pos += k;
		got += k;
	}
	return got;
}

int pmimg_in_skip(PmImgIn *in, long n)
{
	while (n > 0) {
		int k;
		if (in->pos >= in->len && !in_fill(in)) return 0;
		k = in->len - in->pos;
		if ((long)k > n) k = (int)n;
		in->pos += k;
		n -= k;
	}
	return 1;
}

unsigned long pmimg_in_be32(PmImgIn *in)
{
	unsigned long v = 0;
	int i;
	for (i = 0; i < 4; i++) {
		int c = pmimg_in_byte(in);
		if (c < 0) c = 0;
		v = (v << 8) | (unsigned long)c;
	}
	return v;
}

unsigned int pmimg_in_le16(PmImgIn *in)
{
	int a = pmimg_in_byte(in), b = pmimg_in_byte(in);
	if (a < 0) a = 0;
	if (b < 0) b = 0;
	return (unsigned int)(a | (b << 8));
}

/* ---------------------------------------------------------------- sink */

struct PmImgSink
	{
	int src_w, src_h;
	int f;                   /* shrink: f x f source pixels make one */
	int out_w, out_h;
	int stride;
	unsigned char *bits;
	unsigned int *acc;       /* out_w sums of the rows of the current band */
	int band_rows;           /* rows summed so far */
	int src_rows;            /* rows fed */
	int out_rows;            /* rows written */
	int *err_cur, *err_next;     /* dithering error, out_w + 2 each, [x + 1] */
	int serp;                /* this row goes right to left */
	int dither;
	PmImgAbort abort;
	void *abort_ctx;
	int aborted;
	int type;
	};

PmImgSink *pmimg_sink_new(int src_w, int src_h, const PmImgOpts *from, int type, int *err)
{
	PmImgOpts o;
	PmImgSink *s;
	int f, fw, fh;
	long n;
	*err = PMIMG_OK;
	pmimg_opts_fill(&o, from);
	if (src_w <= 0 || src_h <= 0 || src_w > PMIMG_MAX_DIM || src_h > PMIMG_MAX_DIM ||
	    (long)src_w * (long)src_h > o.max_src_pixels) { *err = PMIMG_E_TOO_BIG; return 0; }
	fw = (src_w + o.max_w - 1) / o.max_w;
	fh = (src_h + o.max_h - 1) / o.max_h;
	f = fw > fh ? fw : fh;
	if (f < 1) f = 1;
	s = (PmImgSink *)calloc(1, sizeof(*s));
	if (!s) { *err = PMIMG_E_MEMORY; return 0; }
	s->src_w = src_w;
	s->src_h = src_h;
	s->f = f;
	s->out_w = (src_w + f - 1) / f;
	s->out_h = (src_h + f - 1) / f;
	s->stride = ((s->out_w + 7) / 8) * 4;
	s->dither = !o.no_dither;
	s->abort = o.abort;
	s->abort_ctx = o.abort_ctx;
	s->type = type;
	n = (long)s->stride * s->out_h;
	s->bits = (unsigned char *)malloc(n);
	s->acc = (unsigned int *)calloc(s->out_w, sizeof(unsigned int));
	s->err_cur = (int *)calloc(s->out_w + 2, sizeof(int));
	s->err_next = (int *)calloc(s->out_w + 2, sizeof(int));
	if (!s->bits || !s->acc || !s->err_cur || !s->err_next) { pmimg_sink_abandon(s); *err = PMIMG_E_MEMORY; return 0; }
	memset(s->bits, 0xcc, n);        /* rows never fed show grey */
	return s;
}

void pmimg_sink_abandon(PmImgSink *s)
{
	if (!s) return;
	free(s->bits);
	free(s->acc);
	free(s->err_cur);
	free(s->err_next);
	free(s);
}

int pmimg_sink_abort(PmImgSink *s)
{
	if (s->aborted) return 1;
	if (s->abort && s->abort(s->abort_ctx, s->src_h > 0 ? (int)((long)s->src_rows * 100 / s->src_h) : 0)) s->aborted = 1;
	return s->aborted;
}

int pmimg_sink_rows_done(const PmImgSink *s)
{
	return s->src_rows;
}

/* v / 255 for 0 <= v < 65535 (the ARM has no divide) */
#define DIV255(v) (((v) + 1 + ((v) >> 8)) >> 8)

/* a finished band: average, dither, pack one output row. With f == 1, row
   is the source row itself and acc is not used. */
static void sink_flush_band(PmImgSink *s, const unsigned char *row)
{
	int x, ow = s->out_w, f = s->f, rows = s->band_rows;
	unsigned char *dst;
	int *ec = s->err_cur, *en = s->err_next, *t;
	unsigned long rcp = 0, rcp_last = 0;   /* 2^24 / the pixels in a box: the last box may be narrow */
	int lastw;
	if (!rows || s->out_rows >= s->out_h) return;
	dst = s->bits + (long)s->out_rows * s->stride;
	memset(dst, 0, s->stride);
	if (f > 1) {
		lastw = s->src_w - (ow - 1) * f;
		rcp = (1UL << 24) / (unsigned long)(rows * f);
		rcp_last = (1UL << 24) / (unsigned long)(rows * lastw);
	}
	if (s->dither) {
		/* Floyd-Steinberg, serpentine: of each pixel's error, 7/16 goes on
		   to the next pixel (carried in fwd), 3/16 back and down, 5/16 down
		   and 1/16 on and down. The row below is built up in dn1 (the column
		   just passed, still to get its 3/16) and dn2 (this column's 1/16 from
		   the last pixel), and written once per pixel. */
		int step = s->serp ? -1 : 1;
		int x0 = s->serp ? ow - 1 : 0;
		int fwd = 0, dn1 = 0, dn2 = 0;
		for (x = x0; x >= 0 && x < ow; x += step) {
			int v, q, e, d, e3, e5;
			if (f == 1) v = row[x];
			else v = (int)((s->acc[x] * (x == ow - 1 ? rcp_last : rcp) + (1UL << 23)) >> 24);
			v += ec[x + 1] + fwd;
			if (v < 0) v = 0;
			if (v > 255) v = 255;
			q = DIV255(v * 15 + 127);
			e = v - q * 17;
			dst[x >> 1] |= (unsigned char)(q << ((x & 1) * 4));
			d = (e * 7) >> 4;
			e3 = (e * 3) >> 4;
			e5 = (e * 5) >> 4;
			fwd = d;
			en[x + 1 - step] = dn1 + e3;
			dn1 = dn2 + e5;
			dn2 = e - d - e3 - e5;
		}
		en[x + 1 - step] = dn1;                 /* the last column */
		s->serp = !s->serp;
	} else {
		for (x = 0; x < ow; x++) {
			int v, q;
			if (f == 1) v = row[x];
			else v = (int)((s->acc[x] * (x == ow - 1 ? rcp_last : rcp) + (1UL << 23)) >> 24);
			q = DIV255(v * 15 + 127);
			dst[x >> 1] |= (unsigned char)(q << ((x & 1) * 4));
		}
	}
	t = s->err_cur; s->err_cur = s->err_next; s->err_next = t;
	if (f > 1) memset(s->acc, 0, ow * sizeof(unsigned int));
	s->band_rows = 0;
	s->out_rows++;
}

void pmimg_sink_row(PmImgSink *s, const unsigned char *row)
{
	int f = s->f, w = s->src_w;
	unsigned int *acc = s->acc;
	if (s->src_rows >= s->src_h) return;
	if (f == 1) {
		/* straight through: one source row is one output row */
		s->src_rows++;
		s->band_rows = 1;
		sink_flush_band(s, row);
		return;
	}
	if (f == 2) {
		int x, ox = 0;
		for (x = 0; x + 1 < w; x += 2) acc[ox++] += row[x] + row[x + 1];
		if (x < w) acc[ox] += row[x];
	} else {
		int x, ox = 0, k = 0;
		unsigned int sum = 0;
		for (x = 0; x < w; x++) {
			sum += row[x];
			if (++k == f) { acc[ox++] += sum; sum = 0; k = 0; }
		}
		if (k) acc[ox] += sum;
	}
	s->src_rows++;
	if (++s->band_rows == f || s->src_rows == s->src_h) sink_flush_band(s, 0);
}

void pmimg_sink_all(PmImgSink *s, const unsigned char *rows)
{
	int y;
	for (y = 0; y < s->src_h; y++) pmimg_sink_row(s, rows + (long)y * s->src_w);
}

int pmimg_sink_end(PmImgSink *s, PmImage *out)
{
	if (s->band_rows) sink_flush_band(s, 0);
	out->w = s->out_w;
	out->h = s->out_h;
	out->src_w = s->src_w;
	out->src_h = s->src_h;
	out->stride = s->stride;
	out->bits = s->bits;
	out->type = s->type;
	out->partial = s->src_rows < s->src_h;
	s->bits = 0;
	pmimg_sink_abandon(s);
	return PMIMG_OK;
}

/* ----------------------------------------------------------- front door */

int pmimg_type(const unsigned char *h, int n)
{
	if (n >= 3 && h[0] == 0xff && h[1] == 0xd8 && h[2] == 0xff) return PMIMG_JPEG;
	if (n >= 8 && h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G' && h[4] == 13 && h[5] == 10 && h[6] == 26 && h[7] == 10) return PMIMG_PNG;
	if (n >= 6 && h[0] == 'G' && h[1] == 'I' && h[2] == 'F' && h[3] == '8' && (h[4] == '7' || h[4] == '9') && h[5] == 'a') return PMIMG_GIF;
	return PMIMG_UNKNOWN;
}

int pmimg_size(const unsigned char *h, int n, int *w, int *h_out)
{
	int t = pmimg_type(h, n);
	*w = *h_out = 0;
	if (t == PMIMG_PNG && n >= 24) {
		*w = (int)(((unsigned long)h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19]);
		*h_out = (int)(((unsigned long)h[20] << 24) | (h[21] << 16) | (h[22] << 8) | h[23]);
		return *w > 0 && *h_out > 0 ? 0 : -1;
	}
	if (t == PMIMG_GIF && n >= 10) {
		*w = h[6] | (h[7] << 8);
		*h_out = h[8] | (h[9] << 8);
		return *w > 0 && *h_out > 0 ? 0 : -1;
	}
	if (t == PMIMG_JPEG) {
		/* walk the markers to SOF */
		int i = 2;
		while (i + 9 < n) {
			int m, len;
			if (h[i] != 0xff) return -1;
			m = h[i + 1];
			if (m == 0xff) { i++; continue; }
			if (m == 0xd8 || (m >= 0xd0 && m <= 0xd7) || m == 0x01) { i += 2; continue; }
			len = (h[i + 2] << 8) | h[i + 3];
			if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
				*h_out = (h[i + 5] << 8) | h[i + 6];
				*w = (h[i + 7] << 8) | h[i + 8];
				return *w > 0 && *h_out > 0 ? 0 : -1;
			}
			i += 2 + len;
		}
		return -1;
	}
	return -1;
}

/* the first bytes are read here to pick a decoder, then handed back */
typedef struct { PmImgIn *in; unsigned char head[16]; int nhead, hpos; } Peeked;

static int peek_read(void *ctx, unsigned char *buf, int n)
{
	Peeked *p = (Peeked *)ctx;
	int k = 0;
	while (p->hpos < p->nhead && k < n) buf[k++] = p->head[p->hpos++];
	if (k) return k;
	return pmimg_in_read(p->in, buf, n);
}

static int peek_rewind(void *ctx)
{
	Peeked *p = (Peeked *)ctx;
	if (!pmimg_in_rewind(p->in)) return -1;
	/* the head is read again from the source, so it is not replayed */
	p->hpos = p->nhead;
	return 0;
}

int pmimg_decode(PmImgRead read, PmImgRewind rewind, void *ctx, const PmImgOpts *opts, PmImage *out, char *err, int errmax)
{
	PmImgIn raw, in;
	Peeked pk;
	int type;
	memset(out, 0, sizeof(*out));
	if (err && errmax > 0) err[0] = 0;
	pmimg_in_init(&raw, read, rewind, ctx);
	pk.in = &raw;
	pk.hpos = 0;
	pk.nhead = pmimg_in_read(&raw, pk.head, 12);
	type = pmimg_type(pk.head, pk.nhead);
	if (type == PMIMG_UNKNOWN) {
		pmimg_err(err, errmax, raw.err ? "could not read the file" : "not a JPEG, PNG or GIF");
		return raw.err ? PMIMG_E_READ : PMIMG_E_FORMAT;
	}
	pmimg_in_init(&in, peek_read, rewind ? peek_rewind : 0, &pk);
	switch (type) {
	case PMIMG_JPEG: return pmimg_decode_jpeg(&in, opts, out, err, errmax);
	case PMIMG_PNG: return pmimg_decode_png(&in, opts, out, err, errmax);
	default: return pmimg_decode_gif(&in, opts, out, err, errmax);
	}
}

static int file_read(void *ctx, unsigned char *buf, int n)
{
	size_t k = fread(buf, 1, (size_t)n, (FILE *)ctx);
	if (k == 0 && ferror((FILE *)ctx)) return -1;
	return (int)k;
}

static int file_rewind(void *ctx)
{
	return fseek((FILE *)ctx, 0, SEEK_SET);
}

int pmimg_decode_file(const char *path, const PmImgOpts *opts, PmImage *out, char *err, int errmax)
{
	FILE *f = fopen(path, "rb");
	int r;
	if (!f) { memset(out, 0, sizeof(*out)); pmimg_err(err, errmax, "could not open the file"); return PMIMG_E_READ; }
	r = pmimg_decode(file_read, file_rewind, f, opts, out, err, errmax);
	fclose(f);
	return r;
}

typedef struct { const unsigned char *p; long len, pos; } MemIn;

static int mem_read(void *ctx, unsigned char *buf, int n)
{
	MemIn *m = (MemIn *)ctx;
	long left = m->len - m->pos;
	if (left <= 0) return 0;
	if ((long)n > left) n = (int)left;
	memcpy(buf, m->p + m->pos, n);
	m->pos += n;
	return n;
}

static int mem_rewind(void *ctx)
{
	((MemIn *)ctx)->pos = 0;
	return 0;
}

int pmimg_decode_mem(const unsigned char *data, long len, const PmImgOpts *opts, PmImage *out, char *err, int errmax)
{
	MemIn m;
	m.p = data; m.len = len; m.pos = 0;
	return pmimg_decode(mem_read, mem_rewind, &m, opts, out, err, errmax);
}

void pmimg_free(PmImage *img)
{
	if (!img) return;
	free(img->bits);
	img->bits = 0;
}

int pmimg_write_pgm(const PmImage *img, const char *path)
{
	FILE *f = fopen(path, "wb");
	int x, y;
	if (!f) return -1;
	fprintf(f, "P5\n%d %d\n255\n", img->w, img->h);
	for (y = 0; y < img->h; y++)
		for (x = 0; x < img->w; x++)
			fputc(PMIMG_PIXEL(img, x, y) * 17, f);
	return fclose(f);
}
