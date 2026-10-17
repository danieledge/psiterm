/* fuzz.c - the decoder against corrupt and hostile updates (run it under
 * ASan/UBSan: the Makefile does).
 *
 * 1. Real updates, truncated at every length and with bytes flipped,
 *    inserted or removed.
 * 2. Hostile op lists, deflated correctly: random ops with random fields,
 *    near the screen's edges, and plain garbage.
 * 3. Random bytes as the deflate stream.
 * Every result must be PC_OK or PC_ERR_DATA; after an error, updates fail
 * until a keyframe, and a good keyframe must then decode exactly.
 */
#include "tutil.h"
#include "zlib.h"

#define W 640
#define H 240
#define RB (W / 2)
#define NUPD 40

static int fails;
static long n_ok, n_err, n_hostile_ok;

static unsigned char* bodies[NUPD];
static int blen[NUPD];
static unsigned char frames[NUPD][RB * H];

static void nop_scroll(void* c, const pc_scroll* s)
{
	(void)c;
	if (s->y0 >= s->y1 || s->y1 > H || s->y0 + s->dy < 0 || s->y1 + s->dy > H ||
	    s->x + s->w > W) {
		printf("FAIL: bad scroll reported\n");
		exit(1);
	}
}

static void nop_rect(void* c, const pc_rect* r)
{
	(void)c;
	if (r->x + r->w > W || r->y + r->h > H || r->w == 0 || r->h == 0) {
		printf("FAIL: bad rect reported\n");
		exit(1);
	}
}

static int dec(pc_dec* d, const unsigned char* b, int n, int key)
{
	int r = pc_dec_update(d, b, n, key, nop_scroll, nop_rect, 0);
	if (r == PC_OK)
		n_ok++;
	else if (r == PC_ERR_DATA)
		n_err++;
	else {
		printf("FAIL: decoder returned %d\n", r);
		fails++;
	}
	return r;
}

/* A session of real updates: a keyframe, then changes and scrolls. */
static void make_session(void)
{
	static unsigned char f[RB * H], out[65532];
	pc_enc* e = pc_enc_new(W, H, 1);
	int i, len, y;
	memset(f, 0x99, sizeof(f));
	for (i = 0; i < NUPD; i++) {
		int k = t_range(4);
		if (k == 0)
			t_fillrect(f, RB, t_range(W - 40), t_range(H - 20), 40, 20, t_range(16));
		else if (k == 1) {	/* scroll up by 9 */
			memmove(f + 10 * RB, f + 19 * RB, (H - 30) * RB);
			for (y = H - 20; y < H - 11; y++)
				t_fillrect(f, RB, 0, y, W, 1, t_range(16));
		} else {
			int j;
			for (j = 0; j < 200; j++)
				t_pix(f, RB, t_range(W), t_range(H), t_range(16));
		}
		if (pc_enc_frame(e, f, RB, i == 0, out, sizeof(out), &len) != PC_OK) {
			printf("FAIL: encoder\n");
			exit(1);
		}
		bodies[i] = (unsigned char*)malloc(len);
		memcpy(bodies[i], out, len);
		blen[i] = len;
		memcpy(frames[i], f, sizeof(f));
	}
	pc_enc_free(e);
}

/* After whatever happened, a good keyframe must decode exactly. */
static void recover(pc_dec* d)
{
	if (dec(d, bodies[0], blen[0], 1) != PC_OK || memcmp(pc_dec_screen(d), frames[0], RB * H)) {
		printf("FAIL: no recovery after a keyframe\n");
		fails++;
	}
}

static void mutate_real(pc_dec* d, int iters)
{
	static unsigned char buf[65536 + 64];
	int it, i, n;
	/* every truncation of the keyframe and of one update */
	for (n = 0; n < blen[0]; n++)
		if (dec(d, bodies[0], n, 1) == PC_OK && n < blen[0] - 6) {
			/* (the last few bytes are the sync flush's empty block) */
			printf("FAIL: keyframe truncated to %d of %d accepted\n", n, blen[0]);
			fails++;
		}
	recover(d);
	for (n = 0; n < blen[1]; n++) {
		recover(d);
		dec(d, bodies[1], n, 0);
	}
	for (it = 0; it < iters; it++) {
		int k = t_range(NUPD - 1) + 1, m = t_range(4), j;
		recover(d);
		for (j = 1; j < k; j++)
			dec(d, bodies[j], blen[j], 0);
		n = blen[k];
		memcpy(buf, bodies[k], n);
		switch (m) {
		case 0:	/* flip bits */
			for (i = 0; i <= t_range(4); i++)
				buf[t_range(n)] ^= (unsigned char)(1 << t_range(8));
			break;
		case 1:	/* random bytes */
			for (i = 0; i <= t_range(8); i++)
				buf[t_range(n)] = (unsigned char)t_rand();
			break;
		case 2:	/* insert */
			i = t_range(n);
			memmove(buf + i + 1, buf + i, n - i);
			buf[i] = (unsigned char)t_rand();
			n++;
			break;
		default:	/* remove */
			if (n > 1) {
				i = t_range(n);
				memmove(buf + i, buf + i + 1, n - i - 1);
				n--;
			}
		}
		if (dec(d, buf, n, 0) != PC_OK) {
			/* until a keyframe, everything fails */
			if (dec(d, bodies[k], blen[k], 0) != PC_ERR_DATA) {
				printf("FAIL: an update after an error was accepted\n");
				fails++;
			}
		}
	}
	recover(d);
}

static void put16(unsigned char* p, int v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

/* A coordinate that is often at or just beyond an edge. */
static int coord(int lim)
{
	switch (t_range(6)) {
	case 0: return lim;
	case 1: return lim - 1 - t_range(3);
	case 2: return lim + 1 + t_range(3);
	case 3: return t_rand() & 0xffff;
	case 4: return 0;
	default: return t_range(lim + 1);
	}
}

/* Hostile op lists, deflated properly as a keyframe or as a following
 * update on the same stream. */
static void hostile(pc_dec* d, int iters)
{
	static unsigned char ops[200000], out[300000];
	z_stream zs;
	int it;
	for (it = 0; it < iters; it++) {
		int n = 0, nops = 1 + t_range((t_rand() & 1) ? 2 : 6), i, key = t_rand() & 1;
		for (i = 0; i < nops && n < (int)sizeof(ops) - 160000; i++) {
			int op = t_range(8), x = coord(W), y = coord(H), w = coord(W), h = coord(H), k;
			if (op >= 3 && op <= 5 && (t_rand() & 1))
				op |= 0x10;	/* RLE-coded */
			if (t_rand() & 1) {	/* mostly well-formed */
				x = t_range(W / 2) * 2;
				w = 2 + t_range((W - x) / 2) * 2;
				if (x + w > W)
					w = W - x;
				y = t_range(H);
				h = 1 + t_range(H - y < 30 ? H - y : 30);
			}
			if (t_range(4) == 0)
				op = t_rand() & 0xff;
			ops[n++] = (unsigned char)op;
			if (op == 1) {
				put16(ops + n, x); put16(ops + n + 2, w);
				put16(ops + n + 4, y); put16(ops + n + 6, coord(H));
				put16(ops + n + 8, (t_range(2 * H) - H) & 0xffff);
				n += 10;
				continue;
			}
			put16(ops + n, x); put16(ops + n + 2, y);
			put16(ops + n + 4, w); put16(ops + n + 6, h);
			n += 8;
			if (op == 2)
				ops[n++] = (unsigned char)t_range(18);
			else if ((op & 15) >= 3 && (op & 15) <= 5 && op < 0x20) {
				int kind = op & 15, wb = (w & 0xfffe) / 2, len;
				int pr = kind == 3 ? wb : kind == 4 ? (wb + 1) / 2 : (wb + 3) / 4;
				len = pr * h;
				if (kind == 4)
					ops[n++] = (unsigned char)t_rand();
				if (kind != 3)
					ops[n++] = (unsigned char)t_rand();
				if (len > 60000 || t_range(8) == 0)
					len = t_range(400);
				if (t_range(6) == 0)
					len += t_range(5) - 2;
				if (len < 0)
					len = 0;
				if (op & 0x10) {	/* tokens that roughly cover len */
					while (len > 0) {
						int c = t_range(5), cnt;
						if (c == 0) {
							cnt = 1 + t_range(128);
							ops[n++] = (unsigned char)(cnt - 1);
							for (k = 0; k < cnt; k++)
								ops[n++] = (unsigned char)t_rand();
						} else if (c == 1) {
							cnt = 3 + t_range(127);
							ops[n++] = (unsigned char)(0x80 + cnt - 3);
							ops[n++] = (unsigned char)t_rand();
						} else if (c == 2) {
							cnt = t_range(len + 4);
							ops[n++] = 0xff;
							put16(ops + n, cnt);
							n += 2;
							ops[n++] = (unsigned char)t_rand();
						} else {	/* exactly what is left */
							cnt = len;
							if (cnt >= 3) {
								ops[n++] = 0xff;
								put16(ops + n, cnt);
								n += 2;
							} else
								ops[n++] = (unsigned char)(0x80 + t_range(3));
							ops[n++] = (unsigned char)t_rand();
						}
						len -= cnt;
					}
				} else
					for (k = 0; k < len; k++)
						ops[n++] = (unsigned char)t_rand();
			} else
				for (k = t_range(12); k > 0; k--)
					ops[n++] = (unsigned char)t_rand();
		}
		if (t_range(8))
			ops[n++] = 0;	/* END */
		if (t_range(5) == 0)
			ops[n++] = (unsigned char)t_rand();	/* junk after END */

		memset(&zs, 0, sizeof(zs));
		deflateInit2(&zs, 1, Z_DEFLATED, -12, 5, Z_DEFAULT_STRATEGY);
		if (!key) {	/* a keyframe first, on the same stream */
			static unsigned char op0[] = { 0x02, 0, 0, 0, 0, 0x80, 2, 0xf0, 0, 7, 0 };
			int len0;
			zs.next_in = op0;
			zs.avail_in = sizeof(op0);
			zs.next_out = out;
			zs.avail_out = sizeof(out);
			deflate(&zs, Z_SYNC_FLUSH);
			len0 = (int)(sizeof(out) - zs.avail_out);
			if (dec(d, out, len0, 1) != PC_OK) {
				printf("FAIL: a good keyframe was refused\n");
				fails++;
			}
		}
		zs.next_in = ops;
		zs.avail_in = n;
		zs.next_out = out;
		zs.avail_out = sizeof(out);
		deflate(&zs, Z_SYNC_FLUSH);
		if (dec(d, out, (int)(sizeof(out) - zs.avail_out), key) == PC_OK)
			n_hostile_ok++;
		deflateEnd(&zs);
	}
	recover(d);
}

static void random_bytes(pc_dec* d, int iters)
{
	static unsigned char buf[4096];
	int it, i;
	for (it = 0; it < iters; it++) {
		int n = t_range(sizeof(buf));
		for (i = 0; i < n; i++)
			buf[i] = (unsigned char)t_rand();
		dec(d, buf, n, t_rand() & 1);
	}
	dec(d, 0, 0, 1);
	dec(d, 0, 0, 0);
	recover(d);
}

int main(int argc, char** argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 3000;
	pc_dec* d = pc_dec_new(W, H);
	t_seed = 777;
	make_session();
	mutate_real(d, iters);
	printf("ok   mutated real updates (%ld ok, %ld rejected so far)\n", n_ok, n_err);
	hostile(d, iters * 4);
	printf("ok   hostile op lists (%ld of %d applied without error)\n", n_hostile_ok, iters * 4);
	random_bytes(d, iters * 4);
	printf("ok   random bytes (%ld ok, %ld rejected)\n", n_ok, n_err);
	pc_dec_free(d);
	if (fails) {
		printf("fuzz: %d FAILED\n", fails);
		return 1;
	}
	printf("fuzz: all passed\n");
	return 0;
}
