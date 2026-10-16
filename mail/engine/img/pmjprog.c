/* pmjprog.c - progressive JPEG (and extended sequential, SOF1) to 16 greys.
 *
 * picojpeg (pmjpeg.c) decodes baseline JPEGs one MCU at a time and refuses
 * progressive ones, whose coefficients arrive over several scans (the DC
 * terms first, then bands of AC terms, then the lower bits of each), so
 * every block's coefficients must be kept until the last scan. On the
 * Psion that has to be cheap, so only what the result needs is kept:
 *
 *  - the luma alone (as pmjpeg.c's pjpeg_grey_only): chroma scans are
 *    skipped over (AC) or decoded and thrown away (DC, interleaved with
 *    the luma);
 *  - only the low frequencies the shrink uses, as libjpeg's scale_denom:
 *    at 1/8 the DC term alone (one short a block; the AC scans are skipped
 *    entirely), at 1/4 the first 5 coefficients in zigzag order (the 2x2 in
 *    the corner), at 1/2 the first 25 (4x4), at full size all 64. A block
 *    also keeps one bit per coefficient it doesn't store, saying whether it
 *    is non-zero yet: the refinement scans need that to stay in step.
 *
 * The shrink is the one pmjpeg.c would choose (the biggest that still
 * leaves at least half the wanted size); if the coefficients for it would
 * not fit in max_full_bytes (640 KB in PsiMail), a bigger shrink is used,
 * down to DC only - 2 bytes a block, so a 20-megapixel photo still fits.
 * After the last scan each block is transformed (an integer IDCT, as
 * stb_image's; zero columns take a short cut), cut down to its 8, 4, 2 or 1
 * pixels across, and the rows go to the same sink as the other decoders
 * (shrink to fit, Floyd-Steinberg to 16 greys).
 *
 * Hostile input: every count and index is checked against the frame
 * before use, Huffman tables are validated, coefficient values are
 * clamped so the IDCT cannot overflow, and a scan whose data runs out
 * stops; what was decoded so far is shown (a progressive picture is
 * whole, only blurred, after its first scan). The abort callback is
 * asked every few rows of blocks during every scan.
 *
 * PsiMail's own code (MIT); the scan decoding follows the JPEG standard
 * (ITU T.81, Annex G), the IDCT stb_image's (public domain).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmimg.h"

#define MAXC 4

static const unsigned char zz[64 + 16] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
	/* (a run past the end lands here, harmlessly) */
	63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63 };

typedef struct
	{
	int ok;
	unsigned char vals[256];
	int maxcode[18];         /* the largest code of each length, -1 none */
	int valptr[17];
	int mincode[17];
	} Huff;

typedef struct
	{
	int id, h, v, tq;
	int dc_tbl, ac_tbl;      /* this scan's */
	int pred;                /* DC predictor */
	int bw, bh;              /* blocks across / down (this component's own size) */
	} Comp;

typedef struct
	{
	PmImgIn *in;
	unsigned long acc;
	int nbits;
	int marker;              /* a marker met in the data (0 none) */
	long pad;                /* bytes made up after the data ended or a marker */
	} Bits;

typedef struct
	{
	PmImgIn *in;
	const PmImgOpts *o;
	int w, h, ncomp, progressive;
	Comp c[MAXC];
	int hmax, vmax, mcux, mcuy;
	unsigned short q[4][64];
	int qok[4];
	Huff dc[4], ac[4];
	int ri;                  /* restart interval, 0 none */
	/* the luma's coefficients */
	int ycomp;               /* index of the luma in c[] */
	int keep;                /* coefficients kept a block (zigzag order): 1, 5, 25 or 64 */
	int masks;               /* 1: a 64-bit "non-zero" mask a block as well */
	int ybw, ybh;            /* blocks stored: padded to whole MCUs */
	short *coef;             /* ybw * ybh * keep */
	unsigned long *mask;     /* ybw * ybh * 2 */
	int shrink;
	int eobrun;
	int scans, scans_done, eoi;
	PmImgSink *sink;
	int aborted;
	} Dec;

/* ------------------------------------------------------------ the bits */

static void bits_init(Bits *b, PmImgIn *in)
{
	b->in = in; b->acc = 0; b->nbits = 0; b->marker = 0; b->pad = 0;
}

static void bits_fill(Bits *b)
{
	while (b->nbits <= 24) {
		int c = 0;
		if (!b->marker) {
			c = pmimg_in_byte(b->in);
			if (c < 0) { b->marker = 0xd9; c = 0; b->pad++; }     /* the data ended: as an EOI */
			else if (c == 0xff) {
				int c2 = pmimg_in_byte(b->in);
				while (c2 == 0xff) c2 = pmimg_in_byte(b->in);
				if (c2 == 0) c = 0xff;
				else { b->marker = c2 < 0 ? 0xd9 : c2; c = 0; b->pad++; }
			}
		} else b->pad++;
		b->acc |= (unsigned long)c << (24 - b->nbits);
		b->nbits += 8;
	}
}

static int get_bit(Bits *b)
{
	int v;
	if (b->nbits < 1) bits_fill(b);
	v = (int)((b->acc >> 31) & 1);
	b->acc = (b->acc << 1) & 0xffffffffUL;
	b->nbits--;
	return v;
}

static int get_bits(Bits *b, int n)
{
	int v;
	if (n <= 0) return 0;
	if (b->nbits < n) bits_fill(b);
	v = (int)((b->acc >> (32 - n)) & ((1UL << n) - 1));
	b->acc = (b->acc << n) & 0xffffffffUL;
	b->nbits -= n;
	return v;
}

/* a value of s bits, sign-extended as T.81 F.2.2.1 */
static int extend(int v, int s)
{
	if (s <= 0) return 0;
	if (s > 15) s = 15;
	return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static int decode(Bits *b, const Huff *t)
{
	int code = 0, l;
	for (l = 1; l <= 16; l++) {
		code = (code << 1) | get_bit(b);
		if (t->maxcode[l] >= 0 && code <= t->maxcode[l]) {
			int k = t->valptr[l] + code - t->mincode[l];
			return k >= 0 && k < 256 ? t->vals[k] : 0;
		}
	}
	return -1;                                   /* not a code: damaged */
}

/* ------------------------------------------------------------ markers */

static int be16(PmImgIn *in)
{
	int a = pmimg_in_byte(in), b = pmimg_in_byte(in);
	if (a < 0 || b < 0) return -1;
	return (a << 8) | b;
}

static int read_dqt(Dec *d, int len)
{
	while (len > 0) {
		int pq_tq = pmimg_in_byte(d->in), pq, tq, i;
		if (pq_tq < 0) return -1;
		pq = pq_tq >> 4; tq = pq_tq & 15;
		if (tq > 3 || pq > 1) return -1;
		for (i = 0; i < 64; i++) {
			int v = pq ? be16(d->in) : pmimg_in_byte(d->in);
			if (v < 0) return -1;
			d->q[tq][zz[i]] = (unsigned short)(v ? v : 1);
		}
		d->qok[tq] = 1;
		len -= 1 + 64 * (pq ? 2 : 1);
	}
	return len == 0 ? 0 : -1;
}

static int read_dht(Dec *d, int len)
{
	while (len > 0) {
		int tc_th = pmimg_in_byte(d->in), counts[17], total = 0, i, l, code, k;
		Huff *t;
		if (tc_th < 0 || (tc_th >> 4) > 1 || (tc_th & 15) > 3) return -1;
		t = (tc_th >> 4) ? &d->ac[tc_th & 15] : &d->dc[tc_th & 15];
		memset(t, 0, sizeof(*t));
		for (i = 1; i <= 16; i++) {
			counts[i] = pmimg_in_byte(d->in);
			if (counts[i] < 0) return -1;
			total += counts[i];
		}
		if (total > 256) return -1;
		for (i = 0; i < total; i++) {
			int v = pmimg_in_byte(d->in);
			if (v < 0) return -1;
			t->vals[i] = (unsigned char)v;
		}
		/* canonical codes (T.81 C.2, F.2.2.3), checked to fit their lengths */
		code = 0; k = 0;
		for (l = 1; l <= 16; l++) {
			t->valptr[l] = k;
			t->mincode[l] = code;
			code += counts[l];
			k += counts[l];
			t->maxcode[l] = counts[l] ? code - 1 : -1;
			if (code > (1 << l)) return -1;
			code <<= 1;
		}
		t->maxcode[17] = 0x7fffffff;
		t->ok = 1;
		len -= 17 + total;
	}
	return len == 0 ? 0 : -1;
}

static int read_sof(Dec *d, int len, int progressive)
{
	int p = pmimg_in_byte(d->in), i;
	d->h = be16(d->in);
	d->w = be16(d->in);
	d->ncomp = pmimg_in_byte(d->in);
	if (p != 8 || d->h <= 0 || d->w <= 0 || d->ncomp < 1 || d->ncomp > MAXC || len != 6 + 3 * d->ncomp) return -1;
	d->progressive = progressive;
	d->hmax = d->vmax = 1;
	for (i = 0; i < d->ncomp; i++) {
		Comp *c = &d->c[i];
		int hv;
		c->id = pmimg_in_byte(d->in);
		hv = pmimg_in_byte(d->in);
		c->tq = pmimg_in_byte(d->in);
		if (c->id < 0 || hv < 0 || c->tq < 0 || c->tq > 3) return -1;
		c->h = hv >> 4; c->v = hv & 15;
		if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) return -1;
		if (c->h > d->hmax) d->hmax = c->h;
		if (c->v > d->vmax) d->vmax = c->v;
	}
	d->mcux = (d->w + 8 * d->hmax - 1) / (8 * d->hmax);
	d->mcuy = (d->h + 8 * d->vmax - 1) / (8 * d->vmax);
	for (i = 0; i < d->ncomp; i++) {
		Comp *c = &d->c[i];
		c->bw = ((d->w * c->h + d->hmax - 1) / d->hmax + 7) / 8;
		c->bh = ((d->h * c->v + d->vmax - 1) / d->vmax + 7) / 8;
	}
	d->ycomp = 0;                                /* the first is the luma (JFIF) */
	return 0;
}

/* ------------------------------------------------------------ storage */

/* coefficients a block for a shrink: 1/8 DC, 1/4 the 2x2 corner (zigzag
   0..4), 1/2 the 4x4 (0..24), 1/1 all */
static int keep_for(int shrink) { return shrink >= 3 ? 1 : shrink == 2 ? 5 : shrink == 1 ? 25 : 64; }

static long bytes_for(const Dec *d, int keep)
{
	long blocks = (long)d->ybw * d->ybh;
	return blocks * (keep * 2 + (keep > 1 && keep < 64 ? 8 : 0));
}

static int setup(Dec *d, char *err, int errmax)
{
	const Comp *y = &d->c[d->ycomp];
	long need;
	int shrink, sw, sh, e;
	if ((long)d->w * d->h > d->o->max_src_pixels || d->w > PMIMG_MAX_DIM || d->h > PMIMG_MAX_DIM) {
		pmimg_err(err, errmax, "too big");
		return PMIMG_E_TOO_BIG;
	}
	d->ybw = d->ncomp > 1 ? d->mcux * y->h : y->bw;
	d->ybh = d->ncomp > 1 ? d->mcuy * y->v : y->bh;
	/* the shrink pmjpeg.c would use; more if the memory says so */
	for (shrink = 3; shrink > 0; shrink--)
		if ((d->w >> shrink) >= (d->o->exact ? d->o->max_w : d->o->max_w / 2)
			|| (d->h >> shrink) >= (d->o->exact ? d->o->max_h : d->o->max_h / 2)) break;
	while (shrink < 3 && bytes_for(d, keep_for(shrink)) > d->o->max_full_bytes) shrink++;
	need = bytes_for(d, keep_for(shrink));
	if (need > d->o->max_full_bytes) { pmimg_err(err, errmax, "too big"); return PMIMG_E_TOO_BIG; }
	d->shrink = shrink;
	d->keep = keep_for(shrink);
	d->masks = d->keep > 1 && d->keep < 64;
	d->coef = (short *)calloc((size_t)d->ybw * d->ybh, (size_t)d->keep * sizeof(short));
	if (d->masks) d->mask = (unsigned long *)calloc((size_t)d->ybw * d->ybh, 2 * sizeof(unsigned long));
	if (!d->coef || (d->masks && !d->mask)) { pmimg_err(err, errmax, "not enough memory"); return PMIMG_E_MEMORY; }
	/* the luma's own size at this shrink (it may be smaller than the
	   picture if it is subsampled, which is rare) */
	sw = ((d->w * y->h + d->hmax - 1) / d->hmax + (1 << shrink) - 1) >> shrink;
	sh = ((d->h * y->v + d->vmax - 1) / d->vmax + (1 << shrink) - 1) >> shrink;
	d->sink = pmimg_sink_new(sw, sh, d->o, PMIMG_JPEG, &e);
	if (!d->sink) { pmimg_err(err, errmax, e == PMIMG_E_TOO_BIG ? "too big" : "not enough memory"); return e; }
	return PMIMG_OK;
}

/* coefficient k (zigzag) of luma block (bx, by): its value, or for one not
   stored, whether it is non-zero (as 1) */
static int coef_get(Dec *d, int blk, int k)
{
	if (k < d->keep) return d->coef[(long)blk * d->keep + k];
	if (!d->masks) return 0;
	return (int)((d->mask[(long)blk * 2 + (k >> 5)] >> (k & 31)) & 1);
}

static void coef_set(Dec *d, int blk, int k, int v)
{
	if (k < d->keep) {
		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
		d->coef[(long)blk * d->keep + k] = (short)v;
	} else if (d->masks && v) d->mask[(long)blk * 2 + (k >> 5)] |= 1UL << (k & 31);
}

/* ------------------------------------------------------------ scans */

typedef struct { int ns, idx[MAXC], ss, se, ah, al; } Scan;

static void block_dc(Dec *d, Bits *b, const Scan *s, int ci, int blk)
{
	Comp *c = &d->c[ci];
	if (s->ah == 0) {
		int t = decode(b, &d->dc[c->dc_tbl]), diff;
		if (t < 0 || t > 11) t = 0;
		diff = extend(get_bits(b, t), t);
		c->pred += diff;
		if (c->pred > 32767) c->pred = 32767;
		if (c->pred < -32768) c->pred = -32768;
		if (blk >= 0) coef_set(d, blk, 0, c->pred * (1 << s->al));
	} else {
		if (get_bit(b) && blk >= 0 && d->keep > 0) {
			int v = coef_get(d, blk, 0);
			coef_set(d, blk, 0, v | (1 << s->al));
		}
	}
}

/* T.81 G.1.2.2: the first pass over a band */
static void block_ac_first(Dec *d, Bits *b, const Scan *s, int blk)
{
	const Huff *t = &d->ac[d->c[s->idx[0]].ac_tbl];
	int k;
	if (d->eobrun > 0) { d->eobrun--; return; }
	for (k = s->ss; k <= s->se; k++) {
		int rs = decode(b, t), r, sz;
		if (rs < 0) { d->eobrun = 0; return; }
		r = rs >> 4; sz = rs & 15;
		if (sz == 0) {
			if (r < 15) {
				d->eobrun = (1 << r) - 1;
				if (r) d->eobrun += get_bits(b, r);
				return;
			}
			k += 15;                             /* sixteen zeros */
			continue;
		}
		k += r;
		if (k > 63) return;
		coef_set(d, blk, k, extend(get_bits(b, sz), sz) * (1 << s->al));
	}
}

/* T.81 G.1.2.3: a refinement pass - one more bit of each non-zero
   coefficient, and new coefficients of magnitude 1 */
static void block_ac_refine(Dec *d, Bits *b, const Scan *s, int blk)
{
	const Huff *t = &d->ac[d->c[s->idx[0]].ac_tbl];
	int p1 = 1 << s->al, m1 = -(1 << s->al);
	int k = s->ss;
	if (d->eobrun <= 0) {
		for (; k <= s->se; k++) {
			int rs = decode(b, t), r, sz, v = 0;
			if (rs < 0) return;
			r = rs >> 4; sz = rs & 15;
			if (sz) {
				v = get_bit(b) ? p1 : m1;        /* (sz is 1 in a proper file) */
			} else if (r != 15) {
				d->eobrun = 1 << r;
				if (r) d->eobrun += get_bits(b, r);
				break;
			}
			while (k <= s->se) {
				int cur = coef_get(d, blk, k);
				if (cur != 0) {
					if (get_bit(b) && k < d->keep && (cur & p1) == 0)
						coef_set(d, blk, k, cur >= 0 ? cur + p1 : cur + m1);
				} else {
					if (--r < 0) break;
				}
				k++;
			}
			if (v && k <= s->se) coef_set(d, blk, k, v);
		}
	}
	if (d->eobrun > 0) {
		for (; k <= s->se; k++) {
			int cur = coef_get(d, blk, k);
			if (cur != 0 && get_bit(b) && k < d->keep && (cur & p1) == 0)
				coef_set(d, blk, k, cur >= 0 ? cur + p1 : cur + m1);
		}
		d->eobrun--;
	}
}

/* a whole sequential block (SOF1, or a progressive file's full scan):
   DC then all of the AC */
static void block_seq(Dec *d, Bits *b, const Scan *s, int ci, int blk)
{
	Comp *c = &d->c[ci];
	const Huff *t = &d->ac[c->ac_tbl];
	int k;
	block_dc(d, b, s, ci, blk);
	for (k = 1; k <= 63; k++) {
		int rs = decode(b, t), r, sz;
		if (rs < 0) return;
		r = rs >> 4; sz = rs & 15;
		if (sz == 0) {
			if (r == 15) { k += 15; continue; }
			return;
		}
		k += r;
		if (k > 63) return;
		{
			int v = extend(get_bits(b, sz), sz);
			if (blk >= 0) coef_set(d, blk, k, v);
		}
	}
}

static int restart(Dec *d, Bits *b, const Scan *s)
{
	int i;
	/* the RST marker: in the bits already, or next in the data */
	if (!b->marker) {
		b->nbits = 0; b->acc = 0;
		bits_fill(b);
	}
	if (b->marker >= 0xd0 && b->marker <= 0xd7) {
		bits_init(b, d->in);
		for (i = 0; i < d->ncomp; i++) d->c[i].pred = 0;
		d->eobrun = 0;
		return 1;
	}
	(void)s;
	return 0;                                    /* something else: the scan is over */
}

/* the entropy-coded data of one scan; returns 0 when it ended as it
   should (or as well as it could), -1 aborted */
static int decode_scan(Dec *d, const Scan *s)
{
	Bits b;
	int i, mx, my, n = 0, has_y = 0, rows;
	bits_init(&b, d->in);
	d->eobrun = 0;
	for (i = 0; i < d->ncomp; i++) d->c[i].pred = 0;
	for (i = 0; i < s->ns; i++) if (s->idx[i] == d->ycomp) has_y = 1;
	if (s->ns == 1) {
		/* non-interleaved: the component's own blocks */
		int ci = s->idx[0];
		Comp *c = &d->c[ci];
		rows = c->bh;
		for (my = 0; my < c->bh; my++) {
			for (mx = 0; mx < c->bw; mx++) {
				int blk = ci == d->ycomp && my < d->ybh && mx < d->ybw ? my * d->ybw + mx : -1;
				if (d->ri && n && n % d->ri == 0 && !restart(d, &b, s)) goto done;
				n++;
				if (!d->progressive)
					block_seq(d, &b, s, ci, blk);
				else if (s->ss == 0) block_dc(d, &b, s, ci, blk);
				else if (blk >= 0) {
					if (s->ah == 0) block_ac_first(d, &b, s, blk);
					else block_ac_refine(d, &b, s, blk);
				}
				if (b.pad > 64) goto done;           /* the data has run out */
			}
			if ((my & 7) == 7 && pmimg_sink_abort(d->sink)) { d->aborted = 1; return -1; }
		}
	} else {
		/* interleaved (DC scans, or a sequential file): MCU by MCU */
		for (my = 0; my < d->mcuy; my++) {
			for (mx = 0; mx < d->mcux; mx++) {
				if (d->ri && n && n % d->ri == 0 && !restart(d, &b, s)) goto done;
				n++;
				for (i = 0; i < s->ns; i++) {
					int ci = s->idx[i], u, v;
					Comp *c = &d->c[ci];
					for (v = 0; v < c->v; v++)
						for (u = 0; u < c->h; u++) {
							int bx = mx * c->h + u, by = my * c->v + v;
							int blk = ci == d->ycomp && by < d->ybh && bx < d->ybw ? by * d->ybw + bx : -1;
							if (!d->progressive) block_seq(d, &b, s, ci, blk);
							else block_dc(d, &b, s, ci, blk);
						}
				}
				if (b.pad > 64) goto done;
			}
			if ((my & 3) == 3 && pmimg_sink_abort(d->sink)) { d->aborted = 1; return -1; }
		}
	}
	(void)has_y;
done:
	/* back in step with the markers: the bit reader may have met one */
	if (b.marker && b.marker != 0xd9) {
		/* leave it for the marker loop: put "FF xx" back by remembering it */
		return b.marker;
	}
	return b.marker == 0xd9 ? 0xd9 : 0;
}

/* ------------------------------------------------------------ output */

#define F2F(x) ((int)((x) * 4096 + 0.5))
#define FSH(x) ((x) * 4096)
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7) \
	int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3; \
	p2 = s2; p3 = s6; \
	p1 = (p2 + p3) * F2F(0.5411961f); \
	t2 = p1 + p3 * F2F(-1.847759065f); \
	t3 = p1 + p2 * F2F(0.765366865f); \
	p2 = s0; p3 = s4; \
	t0 = FSH(p2 + p3); t1 = FSH(p2 - p3); \
	x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
	t0 = s7; t1 = s5; t2 = s3; t3 = s1; \
	p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; \
	p5 = (p3 + p4) * F2F(1.175875602f); \
	t0 = t0 * F2F(0.298631336f); t1 = t1 * F2F(2.053119869f); \
	t2 = t2 * F2F(3.072711026f); t3 = t3 * F2F(1.501321110f); \
	p1 = p5 + p1 * F2F(-0.899976223f); p2 = p5 + p2 * F2F(-2.562915447f); \
	p3 = p3 * F2F(-1.961570560f); p4 = p4 * F2F(-0.390180644f); \
	t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static unsigned char clamp255(int x) { return (unsigned char)(x < 0 ? 0 : x > 255 ? 255 : x); }
static int clampv(int x, int m) { return x < -m ? -m : x > m ? m : x; }

/* stb_image's integer IDCT: in dequantized, row-major; out 8x8 bytes */
static void idct8(const int *in, unsigned char *out)
{
	int i, val[64], *v = val;
	const int *d = in;
	for (i = 0; i < 8; i++, d++, v++) {
		if (!d[8] && !d[16] && !d[24] && !d[32] && !d[40] && !d[48] && !d[56]) {
			int dc = d[0] * 4;
			v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
		} else {
			IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
			x0 += 512; x1 += 512; x2 += 512; x3 += 512;
			/* (held within what a real picture can give, so the second
			   pass can't overflow on a hostile one) */
			v[0] = clampv((x0 + t3) >> 10, 16383); v[56] = clampv((x0 - t3) >> 10, 16383);
			v[8] = clampv((x1 + t2) >> 10, 16383); v[48] = clampv((x1 - t2) >> 10, 16383);
			v[16] = clampv((x2 + t1) >> 10, 16383); v[40] = clampv((x2 - t1) >> 10, 16383);
			v[24] = clampv((x3 + t0) >> 10, 16383); v[32] = clampv((x3 - t0) >> 10, 16383);
		}
	}
	for (i = 0, v = val; i < 8; i++, v += 8, out += 8) {
		IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
		x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17);
		x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
		out[0] = clamp255((x0 + t3) >> 17); out[7] = clamp255((x0 - t3) >> 17);
		out[1] = clamp255((x1 + t2) >> 17); out[6] = clamp255((x1 - t2) >> 17);
		out[2] = clamp255((x2 + t1) >> 17); out[5] = clamp255((x2 - t1) >> 17);
		out[3] = clamp255((x3 + t0) >> 17); out[4] = clamp255((x3 - t0) >> 17);
	}
}

static int output(Dec *d, PmImage *out, char *err, int errmax)
{
	const Comp *y = &d->c[d->ycomp];
	const unsigned short *q = d->q[y->tq];
	int bs = 8 >> d->shrink, f = 1 << d->shrink;
	int sw = ((d->w * y->h + d->hmax - 1) / d->hmax + f - 1) >> d->shrink;
	int sh = ((d->h * y->v + d->vmax - 1) / d->vmax + f - 1) >> d->shrink;
	int bw = d->ybw * bs, by, bx, r, k;
	unsigned char *band = (unsigned char *)malloc((size_t)bw * bs), px[64];
	int in[64];
	if (!band) { pmimg_err(err, errmax, "not enough memory"); return PMIMG_E_MEMORY; }
	for (by = 0; by < d->ybh && by * bs < sh; by++) {
		for (bx = 0; bx < d->ybw; bx++) {
			int blk = by * d->ybw + bx;
			const short *c = d->coef + (long)blk * d->keep;
			unsigned char *dst = band + bx * bs;
			if (d->keep == 1) {
				/* DC alone: the block's average, no transform */
				int v = clampv(c[0] * (int)q[0], 2047);
				dst[0] = clamp255(((v + 4) >> 3) + 128);
				continue;
			}
			for (k = 0; k < 64; k++) in[k] = 0;
			for (k = 0; k < d->keep; k++)
				if (c[k]) in[zz[k]] = clampv(c[k] * (int)q[zz[k]], 2047);
			idct8(in, px);
			if (bs == 8) {
				for (r = 0; r < 8; r++) memcpy(dst + (long)r * bw, px + r * 8, 8);
			} else {
				/* the block shrunk to bs x bs by averaging */
				int f2 = 8 / bs, u, v, i, j;
				for (v = 0; v < bs; v++)
					for (u = 0; u < bs; u++) {
						int sum = 0;
						for (j = 0; j < f2; j++)
							for (i = 0; i < f2; i++) sum += px[(v * f2 + j) * 8 + u * f2 + i];
						dst[(long)v * bw + u] = (unsigned char)(sum / (f2 * f2));
					}
			}
		}
		for (r = 0; r < bs && by * bs + r < sh; r++) pmimg_sink_row(d->sink, band + (long)r * bw);
		if ((by & 7) == 7 && pmimg_sink_abort(d->sink)) { free(band); d->aborted = 1; pmimg_err(err, errmax, "stopped"); return PMIMG_E_ABORTED; }
	}
	free(band);
	(void)sw;
	return PMIMG_OK;
}

/* ------------------------------------------------------------ the front */

static int skip_segment(PmImgIn *in, int len) { return len >= 0 && pmimg_in_skip(in, len) ? 0 : -1; }

int pmimg_decode_jpeg_prog(PmImgIn *in, const PmImgOpts *from, PmImage *out, char *err, int errmax)
{
	PmImgOpts o;
	Dec *d;
	int r = PMIMG_OK, m, pending = 0, have_frame = 0;
	pmimg_opts_fill(&o, from);
	d = (Dec *)calloc(1, sizeof(Dec));
	if (!d) { pmimg_err(err, errmax, "not enough memory"); return PMIMG_E_MEMORY; }
	d->in = in;
	d->o = &o;
	if (pmimg_in_byte(in) != 0xff || pmimg_in_byte(in) != 0xd8) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "not a JPEG"); goto end; }
	for (;;) {
		int len;
		if (pending) { m = pending; pending = 0; }
		else {
			int c = pmimg_in_byte(in);
			while (c >= 0 && c != 0xff) c = pmimg_in_byte(in);      /* (stray bytes) */
			while (c == 0xff) c = pmimg_in_byte(in);
			if (c < 0) break;                                      /* the data ended */
			m = c;
		}
		if (m == 0xd9) { d->eoi = 1; break; }
		if (m >= 0xd0 && m <= 0xd7) continue;                    /* a stray RST */
		len = be16(in);
		if (len < 2) { if (!have_frame) r = PMIMG_E_FORMAT; break; }
		len -= 2;
		switch (m) {
		case 0xc0: case 0xc1: case 0xc2:
			if (have_frame || read_sof(d, len, m == 0xc2) != 0) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged JPEG"); goto end; }
			have_frame = 1;
			if ((r = setup(d, err, errmax)) != PMIMG_OK) goto end;
			break;
		case 0xc3: case 0xc5: case 0xc6: case 0xc7: case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xcf:
			r = PMIMG_E_UNSUPPORTED;
			pmimg_err(err, errmax, m >= 0xc9 ? "an arithmetic-coded JPEG" : "a JPEG of a kind not supported");
			goto end;
		case 0xc4:
			if (read_dht(d, len) != 0) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged JPEG"); goto end; }
			break;
		case 0xdb:
			if (read_dqt(d, len) != 0) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged JPEG"); goto end; }
			break;
		case 0xdd:
			if (len != 2) { r = PMIMG_E_FORMAT; goto end; }
			d->ri = be16(in);
			if (d->ri < 0) d->ri = 0;
			break;
		case 0xda: {
			Scan s;
			int i, j, ok = 1;
			memset(&s, 0, sizeof(s));
			if (!have_frame) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged JPEG"); goto end; }
			s.ns = pmimg_in_byte(in);
			if (s.ns < 1 || s.ns > d->ncomp || len != 4 + 2 * s.ns) { ok = 0; skip_segment(in, len - 1); }
			for (i = 0; ok && i < s.ns; i++) {
				int id = pmimg_in_byte(in), tt = pmimg_in_byte(in);
				s.idx[i] = -1;
				for (j = 0; j < d->ncomp; j++) if (d->c[j].id == id) s.idx[i] = j;
				if (s.idx[i] < 0 || tt < 0 || (tt >> 4) > 3 || (tt & 15) > 3) ok = 0;
				else { d->c[s.idx[i]].dc_tbl = tt >> 4; d->c[s.idx[i]].ac_tbl = tt & 15; }
			}
			if (ok) {
				int ahal;
				s.ss = pmimg_in_byte(in); s.se = pmimg_in_byte(in); ahal = pmimg_in_byte(in);
				s.ah = ahal >> 4; s.al = ahal & 15;
				if (ahal < 0 || s.ss > 63 || s.se > 63 || s.ss > s.se || s.al > 13 || s.ah > 13) ok = 0;
				if (d->progressive) {
					if (s.ss == 0 && s.se != 0) ok = 0;                 /* DC scans are DC alone */
					if (s.ss > 0 && s.ns != 1) ok = 0;                  /* AC scans one component */
				} else if (s.ss != 0 || s.se != 63) s.ss = 0, s.se = 63;
			}
			if (ok) {
				/* the tables this scan uses must be there */
				for (i = 0; i < s.ns; i++) {
					const Comp *c = &d->c[s.idx[i]];
					if ((s.ss == 0 && s.ah == 0 && !d->dc[c->dc_tbl].ok) || ((s.se > 0 || !d->progressive) && !d->ac[c->ac_tbl].ok)) ok = 0;
				}
			}
			d->scans++;
			{
				int y = 0, skip;
				for (i = 0; ok && i < s.ns; i++) if (s.idx[i] == d->ycomp) y = 1;
				/* not the luma's, or AC terms we don't keep: skipped over */
				skip = !ok || (s.ns == 1 && !y) || (d->progressive && s.ss > 0 && d->keep == 1);
				if (skip) {
					/* to the next marker that isn't a restart */
					int c = 0;
					for (;;) {
						c = pmimg_in_byte(in);
						if (c < 0) break;
						if (c != 0xff) continue;
						do c = pmimg_in_byte(in); while (c == 0xff);
						if (c < 0) break;
						if (c != 0 && !(c >= 0xd0 && c <= 0xd7)) { pending = c; break; }
					}
					if (c < 0) goto out_of_data;
					break;
				}
				m = decode_scan(d, &s);
				if (m < 0) { r = PMIMG_E_ABORTED; pmimg_err(err, errmax, "stopped"); goto end; }
				d->scans_done++;
				if (m == 0xd9) { d->eoi = !in->eof; goto finish; }
				if (m) pending = m;
			}
			break;
		}
		default:
			if (skip_segment(in, len) != 0) goto out_of_data;
			break;
		}
	}
out_of_data:
finish:
	if (!have_frame || !d->sink) { r = PMIMG_E_FORMAT; pmimg_err(err, errmax, "a damaged JPEG"); goto end; }
	if (!d->scans_done) { r = in->err ? PMIMG_E_READ : PMIMG_E_FORMAT; pmimg_err(err, errmax, in->err ? "could not read the file" : "a damaged JPEG"); goto end; }
	r = output(d, out, err, errmax);
	if (r != PMIMG_OK) goto end;
	r = pmimg_sink_end(d->sink, out);
	d->sink = 0;
	out->src_w = d->w;
	out->src_h = d->h;
	if (!d->eoi) out->partial = 1;
end:
	if (d->sink) pmimg_sink_abandon(d->sink);
	free(d->coef);
	free(d->mask);
	free(d);
	return r;
}
