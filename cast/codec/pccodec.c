/* pccodec.c - the PsiCast screen codec: encoder (Psion) and decoder (Atom).
 *
 * The byte format of the op list, the algorithms and the measurements are
 * in cast/docs/CODEC.md. In short: an update is a list of ops (MOVE, FILL,
 * PIX1/PIX2/PIX4 - pixels at 1, 2 or 4 bits, optionally run-length coded -
 * and END) applied in order to the screen, sent as one chunk of a raw
 * deflate stream that lives as long as the session.
 *
 * The encoder keeps a copy of the screen as the decoder has it (the model)
 * and always encodes "model -> new frame". Moves are found first (row
 * hashes, a vote for the offset, then the best band of rows), applied to
 * the model, and what is left is sent as rectangles built from 16-pixel
 * tiles. Anything that does not fit in the output buffer simply stays
 * different from the model and goes in the next update.
 *
 * MIT licence, as the rest of PsiTerm.
 */
#include <stdlib.h>
#include <string.h>
#include "zlib.h"
#include "pccodec.h"
#include "pctune.h"

typedef unsigned int pc_u32;	/* 32 bits on EPOC, the ESP32 and hosts */

/* op codes (CODEC.md) */
#define OP_END   0x00
#define OP_MOVE  0x01
#define OP_FILL  0x02
#define OP_PIX4  0x03		/* 4 bits a pixel */
#define OP_PIX2  0x04		/* 2 bits a pixel, 4-grey palette */
#define OP_PIX1  0x05		/* 1 bit a pixel, 2-grey palette */
#define OP_RLEF  0x10		/* (flag) the pixel data is RLE-coded */

#define PC_WBITS  12		/* raw deflate, 4 KB window */
#define PC_MEMLVL 5

#ifndef PC_NO_ENCODER

/* ---------------------------------------------------------------- encoder */

#define TWW     2		/* tile width in words (16 pixels) */
#define MW      3		/* mask words a row: up to 96 tile columns */
#define STAGE   2048		/* op bytes gathered before each deflate call */
#define PK      4096		/* the most packed pixel bytes in one op */
#define HB      256		/* hash buckets for move detection */
#define MARGIN  32		/* room kept for END and the sync flush */
#define SCROLL_MIN   6		/* dirty rows before moves are looked for */
#define SCROLL_GAIN  3		/* rows a move must save */

struct pc_enc {
	int w, h, rb, nw, ntx, level;
	unsigned char* scr;	/* the decoder's screen, as far as we know */
	z_stream zs;
	int zok, needkey, failed;
	/* per row: first and last dirty word (-1 if clean), tile mask */
	short* rw0;
	short* rw1;
	pc_u32* rmask;
	/* move detection */
	pc_u32* ho;
	pc_u32* hn;
	short* chain;
	short* votes;
	unsigned char* mrow;	/* rows that match after the move */
	short head[HB];
	/* output */
	int outmax;
	int ofl, ufl;		/* out bytes at the last sync; op bytes since */
	int stop, pending;
	int stn;
	unsigned char st[STAGE];
	pc_stats stats;
	pc_u32 gmask[256];	/* frame byte -> bits of its two greys */
	pc_u32 lutset;		/* the greys lut is for (0: none) */
	unsigned char lut[256];	/* frame byte -> two pixels' codes */
	unsigned char pk[PK];	/* packed pixels */
	unsigned char rl[PK];	/* and their RLE form */
	/* tuning */
	int t_scroll, t_rle, t_gap, t_th, t_strategy, t_memlevel;
};

/* zlib's conservative deflateBound for n bytes, plus a little */
static int bnd(int n)
{
	return n + ((n + 7) >> 3) + ((n + 63) >> 6) + 5 + 8;
}

static void z_run(pc_enc* e, const unsigned char* p, int n, int flush)
{
	int r;
	if (e->failed)
		return;
	e->zs.next_in = (Bytef*)p;
	e->zs.avail_in = (uInt)n;
	for (;;) {
		r = deflate(&e->zs, flush);
		if (r != Z_OK && r != Z_BUF_ERROR) {
			e->failed = 1;
			return;
		}
		if (e->zs.avail_out == 0) {	/* the bound was wrong: give up */
			e->failed = 1;
			return;
		}
		if (e->zs.avail_in == 0)
			return;
	}
}

static void st_flush(pc_enc* e)
{
	if (e->stn) {
		z_run(e, e->st, e->stn, Z_NO_FLUSH);
		e->stn = 0;
	}
}

#define ST_BYTE(e, b) do { if ((e)->stn == STAGE) st_flush(e); \
	(e)->st[(e)->stn++] = (unsigned char)(b); } while (0)

static void st_u16(pc_enc* e, int v)
{
	ST_BYTE(e, v & 0xff);
	ST_BYTE(e, (v >> 8) & 0xff);
}

static void st_put(pc_enc* e, const unsigned char* p, int n)
{
	int k;
	while (n > 0) {
		if (e->stn == STAGE)
			st_flush(e);
		k = STAGE - e->stn;
		if (k > n)
			k = n;
		memcpy(e->st + e->stn, p, k);
		e->stn += k;
		p += k;
		n -= k;
	}
}

static void z_sync(pc_enc* e)
{
	st_flush(e);
	z_run(e, e->st, 0, Z_SYNC_FLUSH);
	e->ofl = e->outmax - (int)e->zs.avail_out;
	e->ufl = 0;
}

static int fits(pc_enc* e, int n)
{
	return e->ofl + bnd(e->ufl + n + 1) + MARGIN <= e->outmax;
}

/* Is there room for n more op bytes? Syncs once to learn the real size. */
static int ensure(pc_enc* e, int n)
{
	if (fits(e, n))
		return 1;
	if (e->ufl > 0) {
		z_sync(e);
		if (fits(e, n))
			return 1;
	}
	return 0;
}

/* How many leading groups of four words of p and q (k groups) are equal.
 * Most rows are unchanged, so this loop is most of the cost of a small
 * update; on the Psion it uses LDM, which gcc 3.0 does not. */
static int eq4(const pc_u32* p, const pc_u32* q, int k)
{
#if defined(__MARM__) && defined(__GNUC__)
	int k0 = k;
	__asm__ volatile(
		"1:\n\t"
		"subs %0, %0, #1\n\t"
		"bmi 2f\n\t"
		"ldmia %1!, {r4, r5, r6, r7}\n\t"
		"ldmia %2!, {r8, r9, r10, r12}\n\t"
		"eor r4, r4, r8\n\t"
		"eor r5, r5, r9\n\t"
		"orr r4, r4, r5\n\t"
		"eor r6, r6, r10\n\t"
		"orr r4, r4, r6\n\t"
		"eor r7, r7, r12\n\t"
		"orrs r4, r4, r7\n\t"
		"beq 1b\n"
		"2:"
		: "+r"(k), "+r"(p), "+r"(q)
		:
		: "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r12", "cc", "memory");
	return k0 - k - 1;
#else
	int i = 0;
	while (i < k && !((p[0] ^ q[0]) | (p[1] ^ q[1]) | (p[2] ^ q[2]) | (p[3] ^ q[3]))) {
		p += 4;
		q += 4;
		i++;
	}
	return i;
#endif
}

/* Compare row y of the frame with the model: first and last dirty word,
 * and which tiles differ. Returns 1 if the row is dirty. */
static int row_diff(pc_enc* e, const unsigned char* fr, int stride, int y)
{
	const pc_u32* a = (const pc_u32*)(fr + y * stride);
	const pc_u32* b = (const pc_u32*)(e->scr + y * e->rb);
	pc_u32* m = e->rmask + y * MW;
	int n = e->nw, i = 0, j, t;

	m[0] = m[1] = m[2] = 0;
	i = 4 * eq4(a, b, n >> 2);
	while (i < n && a[i] == b[i])
		i++;
	if (i == n) {
		e->rw0[y] = -1;
		e->rw1[y] = -1;
		return 0;
	}
	j = n - 1;
	while (a[j] == b[j])
		j--;
	e->rw0[y] = (short)i;
	e->rw1[y] = (short)j;
	/* the tiles between the two ends */
	for (t = i / TWW; t <= j / TWW; t++) {
		int k = t * TWW;
		if (a[k] != b[k] || (k + 1 < n && a[k + 1] != b[k + 1]))
			m[t >> 5] |= (pc_u32)1 << (t & 31);
	}
	return 1;
}

static int diff_all(pc_enc* e, const unsigned char* fr, int stride)
{
	int y, nd = 0;
	for (y = 0; y < e->h; y++)
		nd += row_diff(e, fr, stride, y);
	return nd;
}

/* A hash of every fourth word of a row's words w0..w1: enough to vote
 * with (the rows a move would cover are then compared in full). */
static pc_u32 row_hash(const unsigned char* row, int w0, int w1)
{
	const pc_u32* p = (const pc_u32*)row;
	pc_u32 h = 0x2545;
	int i;
	for (i = w0; i <= w1; i += 4)
		h = ((h << 5) | (h >> 27)) + p[i];
	return h;
}

/* Is row y of the frame the same as model row ys, in words w0..w1? */
static int row_same(const pc_enc* e, const unsigned char* fr, int stride,
                    int y, int ys, int w0, int w1)
{
	const pc_u32* a = (const pc_u32*)(fr + y * stride) + w0;
	const pc_u32* b = (const pc_u32*)(e->scr + ys * e->rb) + w0;
	int n = w1 - w0 + 1, i = 4 * eq4(a, b, n >> 2);
	for (; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

#define BUCKET(h) (((h) ^ ((h) >> 8) ^ ((h) >> 16)) & (HB - 1))

/* The best band of destination rows b0..b1 (in: the rows to look at) for
 * a move by dy of words w0..w1. A row gains if it is dirty in place and
 * matches after the move, and loses if it is clean in place and would not
 * match (Kadane). Leaves e->mrow[] set; returns the band's score. */
static int best_band(pc_enc* e, const unsigned char* fr, int stride, int dy,
                     int w0, int w1, int* aB0, int* aB1)
{
	int y, run = 0, runs = *aB0, bs = 0, bb0 = -1, bb1 = -1;
	for (y = *aB0; y <= *aB1; y++) {
		int match = row_same(e, fr, stride, y, y - dy, w0, w1);
		int dirty = e->rw0[y] >= 0;
		int sc = match ? (dirty ? 1 : 0) : (dirty ? 0 : -1);
		e->mrow[y] = (unsigned char)match;
		if (run <= 0 && sc <= 0) {
			run = 0;
			runs = y + 1;
			continue;
		}
		run += sc;
		if (run > bs) {
			bs = run;
			bb0 = runs;
			bb1 = y;
		}
		if (run <= 0) {
			run = 0;
			runs = y + 1;
		}
	}
	if (bb0 < 0)
		return 0;
	*aB0 = bb0;
	*aB1 = bb1;
	return bs;
}

/* In how many rows b0..b1 does word w not follow a move by dy? */
static int col_miss(const pc_enc* e, const unsigned char* fr, int stride, int dy,
                    int w, int b0, int b1)
{
	int y, n = 0;
	for (y = b0; y <= b1; y++)
		if (((const pc_u32*)(fr + y * stride))[w] != ((const pc_u32*)(e->scr + (y - dy) * e->rb))[w])
			n++;
	return n;
}

/* Look for one vertical move among the dirty rows; if one saves enough,
 * write a MOVE, apply it to the model and re-diff the rows it covered.
 * Returns 1 if a move was made. */
static int enc_scroll(pc_enc* e, const unsigned char* fr, int stride)
{
	int y, ya = -1, yb = -1, nd = 0, wx0 = e->nw, wx1 = -1;
	int dy, best, bestv, h = e->h;
	int b0, b1;
	short* votes = e->votes + h;	/* votes[dy], dy in -(h-1)..h-1 */

	for (y = 0; y < h; y++) {
		if (e->rw0[y] < 0)
			continue;
		nd++;
		if (ya < 0)
			ya = y;
		yb = y;
		if (e->rw0[y] < wx0)
			wx0 = e->rw0[y];
		if (e->rw1[y] > wx1)
			wx1 = e->rw1[y];
	}
	if (nd < SCROLL_MIN)
		return 0;

	/* hashes of the dirty columns, old (model) and new; and a table of
	 * the old rows by hash */
	for (y = 0; y < HB; y++)
		e->head[y] = -1;
	for (y = ya; y <= yb; y++) {
		pc_u32 ho = row_hash(e->scr + y * e->rb, wx0, wx1);
		int k = BUCKET(ho);
		e->ho[y] = ho;
		e->hn[y] = e->rw0[y] >= 0 ? row_hash(fr + y * stride, wx0, wx1) : ho;
		e->chain[y] = e->head[k];
		e->head[k] = (short)y;
	}

	/* each dirty new row votes for the offsets its old copies are at;
	 * rows with many copies (blank ones) do not vote */
	memset(e->votes, 0, (2 * h) * sizeof(short));
	for (y = ya; y <= yb; y++) {
		int cand[4], nc = 0, s;
		pc_u32 hv = e->hn[y];
		if (e->rw0[y] < 0)
			continue;
		for (s = e->head[BUCKET(hv)]; s >= 0; s = e->chain[s]) {
			if (e->ho[s] != hv || s == y)
				continue;
			if (nc == 4) {
				nc = 5;
				break;
			}
			cand[nc++] = s;
		}
		if (nc <= 4)
			while (nc > 0) {
				nc--;
				votes[y - cand[nc]]++;
			}
	}
	best = 0;
	bestv = 0;
	for (dy = -(h - 1); dy < h; dy++)
		if (votes[dy] > bestv) {
			bestv = votes[dy];
			best = dy;
		}
	if (bestv < SCROLL_GAIN)
		return 0;
	dy = best;

	/* the best band of rows for this move. If the band leaves out dirty
	 * rows, see whether it does better without the edge words (a pane's
	 * border or scroll bar inside the dirty columns, which does not move):
	 * if so, leave out the edge words that do not follow the move */
	{
		int r0 = ya > ya + dy ? ya : ya + dy, r1 = yb < yb + dy ? yb : yb + dy;
		int s1, ndr = 0;
		b0 = r0;
		b1 = r1;
		s1 = best_band(e, fr, stride, dy, wx0, wx1, &b0, &b1);
		for (y = r0; y <= r1; y++)
			ndr += e->rw0[y] >= 0;
		if (s1 < ndr && wx1 - wx0 >= 2) {
			int i0 = r0, i1 = r1, s2, t0 = wx0, t1 = wx1;
			s2 = best_band(e, fr, stride, dy, wx0 + 1, wx1 - 1, &i0, &i1);
			if (s2 > s1) {
				if (16 * col_miss(e, fr, stride, dy, wx0, i0, i1) > i1 - i0 + 1)
					t0++;
				if (16 * col_miss(e, fr, stride, dy, wx1, i0, i1) > i1 - i0 + 1)
					t1--;
				b0 = r0;
				b1 = r1;
				s2 = best_band(e, fr, stride, dy, t0, t1, &b0, &b1);
				if (s2 > s1) {
					wx0 = t0;
					wx1 = t1;
					s1 = s2;
				} else {
					b0 = r0;	/* back to the first answer */
					b1 = r1;
					s1 = best_band(e, fr, stride, dy, wx0, wx1, &b0, &b1);
				}
			} else {
				b0 = r0;
				b1 = r1;
				s1 = best_band(e, fr, stride, dy, wx0, wx1, &b0, &b1);
			}
		}
		if (s1 < SCROLL_GAIN)
			return 0;
		/* and edge words that miss in many rows of the band (a scroll
		 * bar's thumb that moved the other way) */
		{
			int t0 = wx0, t1 = wx1, nb = b1 - b0 + 1;
			while (t0 < t1 && 4 * col_miss(e, fr, stride, dy, t0, b0, b1) > nb)
				t0++;
			while (t1 > t0 && 4 * col_miss(e, fr, stride, dy, t1, b0, b1) > nb)
				t1--;
			if (t0 != wx0 || t1 != wx1) {
				int c0 = r0, c1 = r1, s2 = best_band(e, fr, stride, dy, t0, t1, &c0, &c1);
				if (s2 >= s1) {
					wx0 = t0;
					wx1 = t1;
					b0 = c0;
					b1 = c1;
				} else
					best_band(e, fr, stride, dy, wx0, wx1, &b0, &b1);
			}
		}
	}
	if (!ensure(e, 11))
		return 0;

	/* MOVE x w y0 y1 dy: the source rows [y0, y1) go dy rows down */
	ST_BYTE(e, OP_MOVE);
	st_u16(e, wx0 * 8);
	st_u16(e, (wx1 - wx0 + 1) * 8);
	st_u16(e, b0 - dy);
	st_u16(e, b1 + 1 - dy);
	st_u16(e, dy & 0xffff);
	e->ufl += 11;
	e->stats.moves++;

	/* the same on the model, in the order that keeps the sources */
	{
		int nb = (wx1 - wx0 + 1) * 4, xo = wx0 * 4;
		if (dy < 0)
			for (y = b0; y <= b1; y++)
				memcpy(e->scr + y * e->rb + xo, e->scr + (y - dy) * e->rb + xo, nb);
		else
			for (y = b1; y >= b0; y--)
				memcpy(e->scr + y * e->rb + xo, e->scr + (y - dy) * e->rb + xo, nb);
	}
	/* a row that matched is clean now if its dirty words were all inside
	 * the moved columns; the others are compared again */
	for (y = b0; y <= b1; y++)
		if (e->mrow[y] && (e->rw0[y] < 0 || (e->rw0[y] >= wx0 && e->rw1[y] <= wx1))) {
			e->rw0[y] = e->rw1[y] = -1;
			e->rmask[y * MW] = e->rmask[y * MW + 1] = e->rmask[y * MW + 2] = 0;
		} else
			row_diff(e, fr, stride, y);
	return 1;
}

/* Literals s[ls..i) into d at o, in tokens of up to 128. Returns the new
 * length, or -1 once it would not be shorter than n. */
static int rle_lits(const unsigned char* s, int ls, int i, unsigned char* d, int o, int n)
{
	while (ls < i) {
		int k = i - ls > 128 ? 128 : i - ls;
		if (o + k + 1 >= n)
			return -1;
		d[o++] = (unsigned char)(k - 1);
		memcpy(d + o, s + ls, k);
		o += k;
		ls += k;
	}
	return o;
}

/* RLE of s[0..n) into d (CODEC.md); its length, or -1 as soon as it
 * would not be shorter than n. s is word aligned. */
static int rle_buf(const unsigned char* s, int n, unsigned char* d)
{
	int i = 0, ls = 0, o = 0;
	while (i + 2 < n) {
		int b = s[i], j, lim;
		pc_u32 bw;
		/* the literal bytes go by quickly: a run needs three the same */
		if (b != s[i + 1]) {
			i++;
			continue;
		}
		if (b != s[i + 2]) {
			i += 2;
			continue;
		}
		lim = n - i > 65535 ? i + 65535 : n;
		j = i + 3;
		while (j < lim && (j & 3) && s[j] == b)
			j++;
		bw = (pc_u32)b * 0x01010101u;
		if (!(j & 3))
			while (j + 4 <= lim && *(const pc_u32*)(s + j) == bw)
				j += 4;
		while (j < lim && s[j] == b)
			j++;
		o = rle_lits(s, ls, i, d, o, n);
		if (o < 0 || o + 4 >= n)
			return -1;
		if (j - i <= 129) {
			d[o++] = (unsigned char)(0x80 + j - i - 3);
		} else {
			d[o++] = 0xff;
			d[o++] = (unsigned char)((j - i) & 0xff);
			d[o++] = (unsigned char)((j - i) >> 8);
		}
		d[o++] = (unsigned char)b;
		ls = i = j;
	}
	return rle_lits(s, ls, n, d, o, n);
}

/* Pack rows y.. (h of them) of bytes xb.. (wb of them) into e->pk, at
 * 1, 2 or 4 bits a pixel; lut maps a frame byte (two pixels) to their
 * codes. Returns the packed length. */
static int pack(pc_enc* e, const unsigned char* fr, int stride, int xb, int wb,
                int y, int h, int bpp, const unsigned char* lut)
{
	unsigned char* o = e->pk;
	int r, i;
	for (r = 0; r < h; r++) {
		const unsigned char* p = fr + (y + r) * stride + xb;
		if (bpp == 4) {
			memcpy(o, p, wb);
			o += wb;
		} else if (bpp == 2) {
			for (i = 0; i + 1 < wb; i += 2)
				*o++ = (unsigned char)(lut[p[i]] | (lut[p[i + 1]] << 4));
			if (i < wb)
				*o++ = lut[p[i]];
		} else {
			for (i = 0; i + 3 < wb; i += 4)
				*o++ = (unsigned char)(lut[p[i]] | (lut[p[i + 1]] << 2) |
				                       (lut[p[i + 2]] << 4) | (lut[p[i + 3]] << 6));
			if (i < wb) {
				int v = 0, s = 0;
				for (; i < wb; i++, s += 2)
					v |= lut[p[i]] << s;
				*o++ = (unsigned char)v;
			}
		}
	}
	return (int)(o - e->pk);
}

static void put_hdr(pc_enc* e, int op, int xb, int y, int wb, int h)
{
	ST_BYTE(e, op);
	st_u16(e, xb * 2);
	st_u16(e, y);
	st_u16(e, wb * 2);
	st_u16(e, h);
}

/* Send rows y.. (h of them) of words w0..w1 and copy them to the model.
 * One grey: FILL. Two: PIX1. Three or four: PIX2. More: PIX4. Each in
 * strips of up to PK packed bytes, RLE-coded when that is shorter. */
static void enc_rect(pc_enc* e, const unsigned char* fr, int stride,
                     int w0, int w1, int y, int h)
{
	int xb = w0 * 4, wb = (w1 - w0 + 1) * 4, r, i, ng = 0, bpp, pr, sr, ys;
	int pal[4], npal = 0;
	pc_u32 set = 0;

	if (e->stop) {
		e->pending = 1;
		return;
	}
	/* which greys are used (stop counting once there are more than 4);
	 * a word like the last one adds nothing */
	for (r = 0; r < h; r++) {
		const pc_u32* p = (const pc_u32*)(fr + (y + r) * stride + xb);
		pc_u32 m, last = p[0] + 1;
		for (i = 0; i < wb / 4; i++) {
			pc_u32 v = p[i];
			if (v == last)
				continue;
			last = v;
			set |= e->gmask[v & 255] | e->gmask[(v >> 8) & 255] |
			       e->gmask[(v >> 16) & 255] | e->gmask[v >> 24];
		}
		m = set - ((set >> 1) & 0x5555);
		m = (m & 0x3333) + ((m >> 2) & 0x3333);
		m = (m + (m >> 4)) & 0x0f0f;
		ng = (int)((m + (m >> 8)) & 0x1f);
		if (ng > 4)
			break;
	}
	if (ng == 1) {
		int g = 0;
		while (!(set & (1u << g)))
			g++;
		if (!ensure(e, 10)) {
			e->stop = 1;
			e->pending = 1;
			return;
		}
		put_hdr(e, OP_FILL, xb, y, wb, h);
		ST_BYTE(e, g);
		e->ufl += 10;
		e->stats.fills++;
		for (r = 0; r < h; r++)
			memcpy(e->scr + (y + r) * e->rb + xb, fr + (y + r) * stride + xb, wb);
		return;
	}
	bpp = ng == 2 ? 1 : ng <= 4 ? 2 : 4;
	if (bpp < 4) {
		int g;
		for (g = 0; g < 16; g++)
			if (set & (1u << g))
				pal[npal++] = g;
		while (npal < (bpp == 1 ? 2 : 4)) {
			pal[npal] = pal[npal - 1];
			npal++;
		}
		npal /= 2;	/* palette bytes */
		/* the byte -> codes table, kept while the greys are the same */
		if (set != e->lutset) {
			int idx[16], k = 0;
			for (g = 0; g < 16; g++)
				idx[g] = set & (1u << g) ? k++ : 0;
			for (i = 0; i < 256; i++)
				e->lut[i] = (unsigned char)(idx[i & 15] | (idx[i >> 4] << bpp));
			e->lutset = set;
		}
	}
	pr = bpp == 4 ? wb : bpp == 2 ? (wb + 1) / 2 : (wb + 3) / 4;
	sr = PK / pr;
	for (ys = y; ys < y + h; ) {
		int rows = y + h - ys, plen, n, op;
		if (rows > sr)
			rows = sr;
		for (;;) {
			plen = pack(e, fr, stride, xb, wb, ys, rows, bpp, e->lut);
			n = e->t_rle ? rle_buf(e->pk, plen, e->rl) : -1;
			if (ensure(e, 9 + npal + (n >= 0 ? n : plen)))
				break;
			if (rows == 1) {
				e->stop = 1;
				e->pending = 1;
				return;
			}
			rows /= 2;
		}
		op = (bpp == 4 ? OP_PIX4 : bpp == 2 ? OP_PIX2 : OP_PIX1) | (n >= 0 ? OP_RLEF : 0);
		put_hdr(e, op, xb, ys, wb, rows);
		if (npal >= 1)
			ST_BYTE(e, pal[0] | (pal[1] << 4));
		if (npal == 2)
			ST_BYTE(e, pal[2] | (pal[3] << 4));
		if (n >= 0)
			st_put(e, e->rl, n);
		else
			st_put(e, e->pk, plen);
		e->ufl += 9 + npal + (n >= 0 ? n : plen);
		if (n >= 0)
			e->stats.rles++;
		else
			e->stats.raws++;
		if (bpp == 1)
			e->stats.pix1++;
		else if (bpp == 2)
			e->stats.pix2++;
		else
			e->stats.pix4++;
		for (r = ys; r < ys + rows; r++)
			memcpy(e->scr + r * e->rb + xb, fr + r * stride + xb, wb);
		ys += rows;
	}
}

/* Is any tile c0..c1 set in mask m? */
static int mrange(const pc_u32* m, int c0, int c1)
{
	int c;
	for (c = c0; c <= c1; c++)
		if (m[c >> 5] & ((pc_u32)1 << (c & 31)))
			return 1;
	return 0;
}

/* Close a rectangle of tile columns c0..c1, rows y0..y1-1: trim it to the
 * rows and words that changed and send it. */
static void close_rect(pc_enc* e, const unsigned char* fr, int stride,
                       int c0, int c1, int y0, int y1)
{
	int w0 = c0 * TWW, w1 = (c1 + 1) * TWW - 1, y, lo, hi;
	if (w1 >= e->nw)
		w1 = e->nw - 1;
	while (y0 < y1 && !mrange(e->rmask + y0 * MW, c0, c1))
		y0++;
	while (y1 > y0 && !mrange(e->rmask + (y1 - 1) * MW, c0, c1))
		y1--;
	if (y0 >= y1)
		return;
	/* the row ends bound the dirty words of every rect in the row */
	lo = e->nw;
	hi = -1;
	for (y = y0; y < y1; y++)
		if (e->rw0[y] >= 0) {
			if (e->rw0[y] < lo)
				lo = e->rw0[y];
			if (e->rw1[y] > hi)
				hi = e->rw1[y];
		}
	if (lo > w0)
		w0 = lo;
	if (hi < w1)
		w1 = hi;
	if (w0 > w1)
		return;
	enc_rect(e, fr, stride, w0, w1, y0, y1 - y0);
}

typedef struct {
	short c0, c1, y0;
} pc_run;

/* Dirty tiles -> runs per tile row -> runs with the same columns in
 * successive tile rows merged -> rectangles. */
static void enc_rects(pc_enc* e, const unsigned char* fr, int stride)
{
	pc_run op[48], np[48];
	int no = 0, nn, ty, i, j, th = e->t_th;

	for (ty = 0; ty < e->h; ty += th) {
		pc_u32 tm[MW];
		int y, ye = ty + th > e->h ? e->h : ty + th, c;
		tm[0] = tm[1] = tm[2] = 0;
		for (y = ty; y < ye; y++) {
			const pc_u32* m = e->rmask + y * MW;
			tm[0] |= m[0];
			tm[1] |= m[1];
			tm[2] |= m[2];
		}
		/* runs, bridging gaps of up to t_gap clean tiles */
		nn = 0;
		c = 0;
		while (c < e->ntx) {
			int s, last, gap;
			if (!(tm[c >> 5] & ((pc_u32)1 << (c & 31)))) {
				if (!tm[c >> 5] && !(c & 31))
					c += 32;
				else
					c++;
				continue;
			}
			s = c;
			last = c;
			gap = 0;
			for (c++; c < e->ntx; c++) {
				if (tm[c >> 5] & ((pc_u32)1 << (c & 31))) {
					last = c;
					gap = 0;
				} else if (++gap > e->t_gap)
					break;
			}
			if (nn == 48) {	/* cannot happen with 96 columns, gap >= 1 */
				np[nn - 1].c1 = (short)(e->ntx - 1);
				break;
			}
			np[nn].c0 = (short)s;
			np[nn].c1 = (short)last;
			np[nn].y0 = (short)ty;
			nn++;
			c = last + 1;
		}
		/* carry on the open rects with the same columns, close the rest */
		j = 0;
		for (i = 0; i < no; i++) {
			while (j < nn && np[j].c0 < op[i].c0)
				j++;
			if (j < nn && np[j].c0 == op[i].c0 && np[j].c1 == op[i].c1)
				np[j].y0 = op[i].y0;
			else
				close_rect(e, fr, stride, op[i].c0, op[i].c1, op[i].y0, ty);
		}
		memcpy(op, np, nn * sizeof(pc_run));
		no = nn;
	}
	for (i = 0; i < no; i++)
		close_rect(e, fr, stride, op[i].c0, op[i].c1, op[i].y0, e->h);
}

static void* z_alloc(voidpf aOpaque, uInt aItems, uInt aSize)
{
	(void)aOpaque;
	return calloc(aItems, aSize);
}

static void z_free(voidpf aOpaque, voidpf aPtr)
{
	(void)aOpaque;
	free(aPtr);
}

pc_enc* pc_enc_new(int aWidth, int aHeight, int aLevel)
{
	pc_enc* e;
	int h = aHeight, y;
	if (aWidth < 8 || aWidth > PC_MAX_W || (aWidth & 7) || h < 1 || h > PC_MAX_H)
		return 0;
	if (aLevel < 1)
		aLevel = 1;
	if (aLevel > 9)
		aLevel = 9;
	e = (pc_enc*)calloc(1, sizeof(pc_enc));
	if (!e)
		return 0;
	e->w = aWidth;
	e->h = h;
	e->rb = aWidth / 2;
	e->nw = aWidth / 8;
	e->ntx = (e->nw + TWW - 1) / TWW;
	e->level = aLevel;
	e->t_scroll = 2;
	e->t_rle = 1;
	e->t_gap = 1;
	e->t_th = 8;
	e->t_strategy = Z_DEFAULT_STRATEGY;
	e->t_memlevel = PC_MEMLVL;
	e->scr = (unsigned char*)malloc(e->rb * h);
	e->rw0 = (short*)malloc(h * sizeof(short));
	e->rw1 = (short*)malloc(h * sizeof(short));
	e->rmask = (pc_u32*)malloc(h * MW * sizeof(pc_u32));
	e->ho = (pc_u32*)malloc(h * sizeof(pc_u32));
	e->hn = (pc_u32*)malloc(h * sizeof(pc_u32));
	e->chain = (short*)malloc(h * sizeof(short));
	e->votes = (short*)malloc(2 * h * sizeof(short));
	e->mrow = (unsigned char*)malloc(h);
	if (!e->scr || !e->rw0 || !e->rw1 || !e->rmask || !e->ho || !e->hn ||
	    !e->chain || !e->votes || !e->mrow) {
		pc_enc_free(e);
		return 0;
	}
	memset(e->scr, 0xff, e->rb * h);
	for (y = 0; y < 256; y++)
		e->gmask[y] = (1u << (y & 15)) | (1u << (y >> 4));
	e->zs.zalloc = z_alloc;
	e->zs.zfree = z_free;
	if (deflateInit2(&e->zs, aLevel, Z_DEFLATED, -PC_WBITS, PC_MEMLVL,
	                 Z_DEFAULT_STRATEGY) != Z_OK) {
		pc_enc_free(e);
		return 0;
	}
	e->zok = 1;
	e->needkey = 1;
	return e;
}

void pc_enc_free(pc_enc* e)
{
	if (!e)
		return;
	if (e->zok)
		deflateEnd(&e->zs);
	free(e->scr);
	free(e->rw0);
	free(e->rw1);
	free(e->rmask);
	free(e->ho);
	free(e->hn);
	free(e->chain);
	free(e->votes);
	free(e->mrow);
	free(e);
}

void pc_enc_tune(pc_enc* e, int aKnob, int aValue)
{
	if (!e)
		return;
	switch (aKnob) {
	case PC_TUNE_SCROLL: e->t_scroll = aValue; break;
	case PC_TUNE_RLE: e->t_rle = aValue; break;
	case PC_TUNE_GAP: e->t_gap = aValue < 0 ? 0 : aValue; break;
	case PC_TUNE_TH: e->t_th = aValue < 1 ? 1 : aValue > 32 ? 32 : aValue; break;
	case PC_TUNE_STRATEGY:
	case PC_TUNE_MEMLEVEL:
		if (aKnob == PC_TUNE_STRATEGY)
			e->t_strategy = aValue;
		else
			e->t_memlevel = aValue;
		deflateEnd(&e->zs);
		e->zok = deflateInit2(&e->zs, e->level, Z_DEFLATED, -PC_WBITS, e->t_memlevel,
		                      e->t_strategy) == Z_OK;
		e->needkey = 1;
		break;
	}
}

static int frame_ok(const pc_enc* e, const unsigned char* fr, int stride)
{
	return e && fr && !((unsigned long)fr & 3) && !(stride & 3) && stride >= e->rb;
}

int pc_enc_frame(pc_enc* e, const unsigned char* fr, int stride,
                 int aKeyframe, unsigned char* aOut, int aOutMax, int* aOutLen)
{
	int nd, i;
	uLong tin0;
	if (!frame_ok(e, fr, stride) || !aOut || !aOutLen)
		return PC_ERR_ARG;
	*aOutLen = 0;
	if (aOutMax < PC_ENC_MIN_OUT)
		return PC_ERR_SPACE;
	if (e->needkey && !aKeyframe)
		return PC_ERR_ARG;
	memset(&e->stats, 0, sizeof(e->stats));
	if (aKeyframe) {
		if (deflateReset(&e->zs) != Z_OK)
			return PC_ERR_ARG;
		memset(e->scr, 0xff, e->rb * e->h);
		e->needkey = 0;
		e->failed = 0;
	}
	nd = diff_all(e, fr, stride);
	if (!nd && !aKeyframe) {
		e->pending = 0;
		return 1;
	}
	e->zs.next_out = aOut;
	e->zs.avail_out = (uInt)aOutMax;
	e->outmax = aOutMax;
	e->ofl = 0;
	e->ufl = 0;
	e->stop = 0;
	e->pending = 0;
	e->stn = 0;
	tin0 = e->zs.total_in;
	if (!aKeyframe)
		for (i = 0; i < e->t_scroll && enc_scroll(e, fr, stride); i++)
			;
	enc_rects(e, fr, stride);
	ST_BYTE(e, OP_END);
	e->ufl++;
	z_sync(e);
	e->stats.in_bytes = (int)(e->zs.total_in - tin0);
	if (e->failed) {
		e->needkey = 1;
		return PC_ERR_SPACE;
	}
	*aOutLen = aOutMax - (int)e->zs.avail_out;
	e->stats.out_bytes = *aOutLen;
	e->stats.pending = e->pending;
	return PC_OK;
}

int pc_enc_changed(pc_enc* e, const unsigned char* fr, int stride)
{
	int y;
	if (!frame_ok(e, fr, stride))
		return 0;
	if (e->needkey)
		return 1;
	for (y = 0; y < e->h; y++) {
		const pc_u32* a = (const pc_u32*)(fr + y * stride);
		const pc_u32* b = (const pc_u32*)(e->scr + y * e->rb);
		int i = 4 * eq4(a, b, e->nw >> 2);
		for (; i < e->nw; i++)
			if (a[i] != b[i])
				return 1;
	}
	return 0;
}

int pc_enc_pending(const pc_enc* e)
{
	return e ? e->pending : 0;
}

void pc_enc_stats(const pc_enc* e, pc_stats* s)
{
	if (e && s)
		*s = e->stats;
}

#endif /* PC_NO_ENCODER */

#ifndef PC_NO_DECODER

/* ---------------------------------------------------------------- decoder */

#define DBUF 1024

struct pc_dec {
	int w, h, rb, flags;
	unsigned char* scr;
	z_stream zs;
	int zok, broken;
	int bp, bn;
	unsigned char buf[DBUF];
};

static void* d_alloc(voidpf aOpaque, uInt aItems, uInt aSize)
{
	(void)aOpaque;
	return calloc(aItems, aSize);
}

static void d_free(voidpf aOpaque, voidpf aPtr)
{
	(void)aOpaque;
	free(aPtr);
}

pc_dec* pc_dec_new(int aWidth, int aHeight)
{
	pc_dec* d;
	if (aWidth < 2 || aWidth > PC_MAX_W || (aWidth & 1) || aHeight < 1 || aHeight > PC_MAX_H)
		return 0;
	d = (pc_dec*)calloc(1, sizeof(pc_dec));
	if (!d)
		return 0;
	d->w = aWidth;
	d->h = aHeight;
	d->rb = aWidth / 2;
	d->scr = (unsigned char*)malloc(d->rb * aHeight);
	if (!d->scr) {
		free(d);
		return 0;
	}
	memset(d->scr, 0xff, d->rb * aHeight);
	d->zs.zalloc = d_alloc;
	d->zs.zfree = d_free;
	if (inflateInit2(&d->zs, -PC_WBITS) != Z_OK) {
		free(d->scr);
		free(d);
		return 0;
	}
	d->zok = 1;
	return d;
}

void pc_dec_free(pc_dec* d)
{
	if (!d)
		return;
	if (d->zok)
		inflateEnd(&d->zs);
	free(d->scr);
	free(d);
}

void pc_dec_set_flags(pc_dec* d, int aFlags)
{
	if (d)
		d->flags = aFlags;
}

const unsigned char* pc_dec_screen(const pc_dec* d)
{
	return d ? d->scr : 0;
}

int pc_dec_stride(const pc_dec* d)
{
	return d ? d->rb : 0;
}

/* Inflate more of the update into buf. 1 = got some, 0 = the input is
 * used up, -1 = corrupt. */
static int d_fill(pc_dec* d)
{
	for (;;) {
		uInt in0 = d->zs.avail_in;
		int r, got;
		if (in0 == 0)
			return 0;
		d->zs.next_out = d->buf;
		d->zs.avail_out = DBUF;
		r = inflate(&d->zs, Z_SYNC_FLUSH);
		got = DBUF - (int)d->zs.avail_out;
		if (r != Z_OK && r != Z_BUF_ERROR)
			return -1;	/* includes Z_STREAM_END: we never finish */
		if (got > 0) {
			d->bp = 0;
			d->bn = got;
			return 1;
		}
		if (d->zs.avail_in == in0)
			return -1;	/* no progress */
	}
}

/* Next n bytes of the op list into dst (or XORed into it). 0 or -1. */
static int d_read(pc_dec* d, unsigned char* dst, int n)
{
	while (n > 0) {
		int k;
		if (d->bp == d->bn && d_fill(d) != 1)
			return -1;
		k = d->bn - d->bp;
		if (k > n)
			k = n;
		memcpy(dst, d->buf + d->bp, k);
		d->bp += k;
		dst += k;
		n -= k;
	}
	return 0;
}

static int d_byte(pc_dec* d)
{
	if (d->bp == d->bn && d_fill(d) != 1)
		return -1;
	return d->buf[d->bp++];
}

/* Reads n u16s; -1 if truncated. */
static int d_u16s(pc_dec* d, int* v, int n)
{
	unsigned char b[10];
	int i;
	if (d_read(d, b, 2 * n))
		return -1;
	for (i = 0; i < n; i++)
		v[i] = b[2 * i] | (b[2 * i + 1] << 8);
	return 0;
}

/* A rectangle op's x y w h, checked against the screen. */
static int d_rect(pc_dec* d, int* v)
{
	if (d_u16s(d, v, 4))
		return -1;
	if ((v[0] & 1) || (v[2] & 1) || v[2] == 0 || v[3] == 0 ||
	    v[0] + v[2] > d->w || v[1] + v[3] > d->h)
		return -1;
	return 0;
}

/* The pixel data of one op: plain, or RLE tokens that must cover it
 * exactly. */
typedef struct {
	int rle;
	int left;	/* bytes still to be read */
	int cover;	/* bytes not yet covered by a token */
	int run, lit, val;
} d_pay;

static int pay_read(pc_dec* d, d_pay* p, unsigned char* dst, int n)
{
	if (n > p->left)
		return -1;
	p->left -= n;
	if (!p->rle)
		return d_read(d, dst, n);
	while (n > 0) {
		int k;
		if (p->run > 0) {
			k = p->run < n ? p->run : n;
			memset(dst, p->val, k);
			p->run -= k;
		} else if (p->lit > 0) {
			k = p->lit < n ? p->lit : n;
			if (d_read(d, dst, k))
				return -1;
			p->lit -= k;
		} else {
			int c = d_byte(d), cnt;
			if (c < 0)
				return -1;
			if (c < 0x80)
				cnt = c + 1;
			else if (c < 0xff)
				cnt = c - 0x80 + 3;
			else {
				int lo = d_byte(d), hi = d_byte(d);
				if (lo < 0 || hi < 0)
					return -1;
				cnt = lo | (hi << 8);
				if (cnt < 3)
					return -1;
			}
			if (cnt > p->cover)
				return -1;
			p->cover -= cnt;
			if (c < 0x80)
				p->lit = cnt;
			else {
				p->val = d_byte(d);
				if (p->val < 0)
					return -1;
				p->run = cnt;
			}
			continue;
		}
		dst += k;
		n -= k;
	}
	return 0;
}

/* A PIX1, PIX2 or PIX4 op's palette and pixels (header already read). */
static int d_pix(pc_dec* d, int kind, int rle, const int* v)
{
	unsigned char pal[4], row[PC_MAX_W / 2];
	int bpp = kind == OP_PIX4 ? 4 : kind == OP_PIX2 ? 2 : 1;
	int xb = v[0] / 2, wb = v[2] / 2, pr, y, i;
	d_pay p;
	if (bpp == 1) {
		int c = d_byte(d);
		if (c < 0)
			return -1;
		pal[0] = (unsigned char)(c & 15);
		pal[1] = (unsigned char)(c >> 4);
	} else if (bpp == 2) {
		int c = d_byte(d), c2 = d_byte(d);
		if (c < 0 || c2 < 0)
			return -1;
		pal[0] = (unsigned char)(c & 15);
		pal[1] = (unsigned char)(c >> 4);
		pal[2] = (unsigned char)(c2 & 15);
		pal[3] = (unsigned char)(c2 >> 4);
	}
	pr = bpp == 4 ? wb : bpp == 2 ? (wb + 1) / 2 : (wb + 3) / 4;
	p.rle = rle;
	p.left = p.cover = pr * v[3];
	p.run = p.lit = p.val = 0;
	for (y = v[1]; y < v[1] + v[3]; y++) {
		unsigned char* o = d->scr + y * d->rb + xb;
		if (bpp == 4) {
			if (pay_read(d, &p, o, wb))
				return -1;
			continue;
		}
		if (pay_read(d, &p, row, pr))
			return -1;
		if (bpp == 2)
			for (i = 0; i < wb; i++) {
				int b = row[i >> 1] >> ((i & 1) * 4);
				o[i] = (unsigned char)(pal[b & 3] | (pal[(b >> 2) & 3] << 4));
			}
		else
			for (i = 0; i < wb; i++) {
				int b = row[i >> 2] >> ((i & 3) * 2);
				o[i] = (unsigned char)(pal[b & 1] | (pal[(b >> 1) & 1] << 4));
			}
	}
	return 0;
}

int pc_dec_update(pc_dec* d, const unsigned char* aBody, int aLen, int aKeyframe,
                  void (*aOnScroll)(void* aCtx, const pc_scroll* aScroll),
                  void (*aOnRect)(void* aCtx, const pc_rect* aRect),
                  void* aCtx)
{
	int v[5], r, op;
	if (!d || (!aBody && aLen) || aLen < 0)
		return PC_ERR_ARG;
	if (aKeyframe) {
		if (inflateReset(&d->zs) != Z_OK)
			return PC_ERR_DATA;
		memset(d->scr, 0xff, d->rb * d->h);
		d->broken = 0;
	} else if (d->broken)
		return PC_ERR_DATA;
	d->zs.next_in = (Bytef*)aBody;
	d->zs.avail_in = (uInt)aLen;
	d->bp = d->bn = 0;
	d->broken = 1;		/* until the update is through */

	for (;;) {
		op = d_byte(d);
		if (op < 0)
			return PC_ERR_DATA;
		if (op == OP_END)
			break;
		switch (op) {
		case OP_MOVE: {
			int x, w, y0, y1, dy, xb, wb, y;
			if (d_u16s(d, v, 5))
				return PC_ERR_DATA;
			x = v[0];
			w = v[1];
			y0 = v[2];
			y1 = v[3];
			dy = v[4] >= 0x8000 ? v[4] - 0x10000 : v[4];
			if ((x & 1) || (w & 1) || w == 0 || x + w > d->w ||
			    y0 >= y1 || y1 > d->h || dy == 0 ||
			    y0 + dy < 0 || y1 + dy > d->h)
				return PC_ERR_DATA;
			xb = x / 2;
			wb = w / 2;
			if (dy < 0)
				for (y = y0; y < y1; y++)
					memcpy(d->scr + (y + dy) * d->rb + xb, d->scr + y * d->rb + xb, wb);
			else
				for (y = y1 - 1; y >= y0; y--)
					memcpy(d->scr + (y + dy) * d->rb + xb, d->scr + y * d->rb + xb, wb);
			if (aKeyframe)
				break;
			if (w == d->w || (d->flags & PC_DEC_PARTIAL_SCROLL)) {
				pc_scroll s;
				s.y0 = (unsigned short)y0;
				s.y1 = (unsigned short)y1;
				s.dy = (short)dy;
				s.x = (unsigned short)x;
				s.w = (unsigned short)w;
				if (aOnScroll)
					aOnScroll(aCtx, &s);
			} else if (aOnRect) {
				pc_rect rc;
				rc.x = (unsigned short)x;
				rc.y = (unsigned short)(y0 + dy);
				rc.w = (unsigned short)w;
				rc.h = (unsigned short)(y1 - y0);
				aOnRect(aCtx, &rc);
			}
			break;
		}
		case OP_FILL:
		case OP_PIX4:
		case OP_PIX2:
		case OP_PIX1:
		case OP_PIX4 | OP_RLEF:
		case OP_PIX2 | OP_RLEF:
		case OP_PIX1 | OP_RLEF: {
			int xb, wb, y;
			if (d_rect(d, v))
				return PC_ERR_DATA;
			xb = v[0] / 2;
			wb = v[2] / 2;
			if (op == OP_FILL) {
				int g = d_byte(d);
				if (g < 0 || g > 15)
					return PC_ERR_DATA;
				for (y = v[1]; y < v[1] + v[3]; y++)
					memset(d->scr + y * d->rb + xb, g | (g << 4), wb);
			} else if (d_pix(d, op & 0x0f, op & OP_RLEF, v))
				return PC_ERR_DATA;
			if (!aKeyframe && aOnRect) {
				pc_rect rc;
				rc.x = (unsigned short)v[0];
				rc.y = (unsigned short)v[1];
				rc.w = (unsigned short)v[2];
				rc.h = (unsigned short)v[3];
				aOnRect(aCtx, &rc);
			}
			break;
		}
		default:
			return PC_ERR_DATA;
		}
	}
	/* nothing may follow END */
	if (d->bp != d->bn)
		return PC_ERR_DATA;
	r = d_fill(d);
	if (r != 0)
		return PC_ERR_DATA;
	d->broken = 0;
	if (aKeyframe && aOnRect) {
		pc_rect rc;
		rc.x = 0;
		rc.y = 0;
		rc.w = (unsigned short)d->w;
		rc.h = (unsigned short)d->h;
		aOnRect(aCtx, &rc);
	}
	return PC_OK;
}

#endif /* PC_NO_DECODER */
