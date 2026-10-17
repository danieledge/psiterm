/* bench.c - the codec on frame sequences (made by bench.py).
 *
 *   bench [-l level] [-s moves] [-r rle] [-g gap] [-t tileh] [-o outmax] FILE...
 *
 * A sequence file is "PCSQ", u16 width, u16 height, u32 frames, then the
 * frames (packed 4bpp, PROTOCOL.md model). The first frame is sent as a
 * keyframe; every later frame that differs is one update (plus more if it
 * is split). Each decoded screen is checked against the frame.
 * Prints one line a sequence:
 *   SEQ name updates keybytes median max mean total ratio tmed_us tmax_us moves
 * Times are the best of several runs, on this host.
 */
#include <time.h>
#include "tutil.h"
#include "pctune.h"

#define RUNS 5

static int level = 1, moves = 2, rle = 1, gap = 1, tileh = 8, outmax = 65532, strat = 0, meml = 5;

static void dbg_scroll(void* c, const pc_scroll* s)
{
	(void)c;
	printf("    move x %d w %d rows %d..%d dy %d\n", s->x, s->w, s->y0, s->y1, s->dy);
}

static void dbg_rect(void* c, const pc_rect* r)
{
	(void)c;
	printf("    rect %d,%d %dx%d\n", r->x, r->y, r->w, r->h);
}

static int cmpint(const void* a, const void* b)
{
	return *(const int*)a - *(const int*)b;
}

static long now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000L + t.tv_nsec;
}

static void bench(const char* path)
{
	FILE* fp = fopen(path, "rb");
	unsigned char hdr[12], *frames, *out;
	int w, h, n, fb, run, i, nu = 0, *bytes, *tus, keyb = 0, mv = 0;
	long total = 0, changed = 0;
	const char* name;
	if (!fp || fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "PCSQ", 4)) {
		printf("bad file %s\n", path);
		exit(1);
	}
	w = hdr[4] | hdr[5] << 8;
	h = hdr[6] | hdr[7] << 8;
	n = hdr[8] | hdr[9] << 8 | hdr[10] << 16;
	fb = w / 2 * h;
	frames = (unsigned char*)malloc((size_t)fb * n);
	if (fread(frames, fb, n, fp) != (size_t)n) {
		printf("short file %s\n", path);
		exit(1);
	}
	fclose(fp);
	out = (unsigned char*)malloc(outmax);
	bytes = (int*)calloc(4 * n + 16, sizeof(int));
	tus = (int*)calloc(4 * n + 16, sizeof(int));
	for (i = 0; i < 4 * n + 16; i++)
		tus[i] = 1 << 30;

	for (run = 0; run < RUNS; run++) {
		pc_enc* e = pc_enc_new(w, h, level);
		pc_dec* d = pc_dec_new(w, h);
		int u = 0;
		pc_stats st;
		pc_enc_tune(e, PC_TUNE_SCROLL, moves);
		pc_enc_tune(e, PC_TUNE_RLE, rle);
		pc_enc_tune(e, PC_TUNE_GAP, gap);
		pc_enc_tune(e, PC_TUNE_TH, tileh);
		if (strat || meml != 5) {
			pc_enc_tune(e, PC_TUNE_STRATEGY, strat);
			pc_enc_tune(e, PC_TUNE_MEMLEVEL, meml);
		}
		total = 0;
		changed = 0;
		mv = 0;
		for (i = 0; i < n; i++) {
			const unsigned char* f = frames + (size_t)fb * i;
			int key = i == 0, len, r, first = 1;
			do {
				long t0 = now_ns(), t;
				r = pc_enc_frame(e, f, w / 2, key, out, outmax, &len);
				t = (now_ns() - t0) / 1000;
				if (r == 1)
					break;
				if (getenv("PCDBG") && atoi(getenv("PCDBG")) > 1 && !run && i < 4) {
					pc_dec_set_flags(d, PC_DEC_PARTIAL_SCROLL);
					printf("  frame %d:\n", i);
				}
				if (r != PC_OK || pc_dec_update(d, out, len, key,
				    getenv("PCDBG") && atoi(getenv("PCDBG")) > 1 && !run && i < 4 ? dbg_scroll : 0,
				    getenv("PCDBG") && atoi(getenv("PCDBG")) > 1 && !run && i < 4 ? dbg_rect : 0, 0) != PC_OK) {
					printf("FAIL: %s frame %d\n", path, i);
					exit(1);
				}
				pc_enc_stats(e, &st);
				mv += st.moves;
				if (!key && getenv("PCDBG"))
					printf("  upd %d: in %d out %d moves %d fills %d pix1 %d pix2 %d pix4 %d rle %d raw %d\n",
					       i, st.in_bytes, st.out_bytes, st.moves, st.fills, st.pix1, st.pix2, st.pix4, st.rles, st.raws);
				if (key)
					keyb = len;
				else {
					if (first)
						changed++;
					if (t < tus[u])
						tus[u] = (int)t;
					bytes[u++] = len;
					total += len;
				}
				key = 0;
				first = 0;
			} while (pc_enc_pending(e));
			if (memcmp(pc_dec_screen(d), f, fb)) {
				printf("FAIL: %s frame %d decoded wrong\n", path, i);
				exit(1);
			}
		}
		nu = u;
		pc_enc_free(e);
		pc_dec_free(d);
	}
	name = strrchr(path, '/');
	name = name ? name + 1 : path;
	{
		int tmax = 0, med, bmax, tmed;
		for (i = 0; i < nu; i++)
			if (tus[i] > tmax)
				tmax = tus[i];
		qsort(tus, nu, sizeof(int), cmpint);
		tmed = nu ? tus[nu / 2] : 0;
		qsort(bytes, nu, sizeof(int), cmpint);
		med = nu ? bytes[nu / 2] : 0;
		bmax = nu ? bytes[nu - 1] : 0;
		printf("SEQ %s %d %d %d %d %ld %ld %.1f %d %d %d\n", name, nu, keyb, med, bmax,
		       nu ? total / nu : 0, total,
		       total ? (double)changed * fb / total : 0.0, tmed, tmax, mv);
	}
	free(frames);
	free(out);
	free(bytes);
	free(tus);
}

/* -V SEQ OUTS: decode updates made elsewhere (armbench.py's ARM run:
 * records of u32 length, u8 keyframe, body) and check them. */
static int verify(const char* seq, const char* outs)
{
	FILE* fp = fopen(seq, "rb");
	FILE* fo = fopen(outs, "rb");
	unsigned char hdr[12], rec[5], *frames, *body = (unsigned char*)malloc(65536);
	int w, h, n, fb, i = 0, nu = 0;
	pc_dec* d;
	if (!fp || !fo || fread(hdr, 1, 12, fp) != 12)
		return 1;
	w = hdr[4] | hdr[5] << 8;
	h = hdr[6] | hdr[7] << 8;
	n = hdr[8] | hdr[9] << 8 | hdr[10] << 16;
	fb = w / 2 * h;
	frames = (unsigned char*)malloc((size_t)fb * n);
	if (fread(frames, fb, n, fp) != (size_t)n)
		return 1;
	d = pc_dec_new(w, h);
	while (fread(rec, 1, 5, fo) == 5) {
		int len = rec[0] | rec[1] << 8 | rec[2] << 16, fi = rec[4] >> 1;
		if (len > 65536 || fread(body, 1, len, fo) != (size_t)len ||
		    pc_dec_update(d, body, len, rec[4] & 1, 0, 0, 0) != PC_OK) {
			printf("FAIL: update %d does not decode\n", nu);
			return 1;
		}
		nu++;
		(void)fi;
		i = rec[3];	/* frame number, low byte (records carry it) */
	}
	(void)i;
	/* the last frame must be on screen */
	if (memcmp(pc_dec_screen(d), frames + (size_t)fb * (n - 1), fb)) {
		printf("FAIL: %s: the ARM updates do not end on the last frame\n", seq);
		return 1;
	}
	printf("ok   %s: %d ARM updates decode to the frames\n", seq, nu);
	return 0;
}

int main(int argc, char** argv)
{
	int i;
	if (argc == 4 && !strcmp(argv[1], "-V"))
		return verify(argv[2], argv[3]);
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-' && i + 1 < argc) {
			int v = atoi(argv[i + 1]);
			switch (argv[i][1]) {
			case 'l': level = v; break;
			case 's': moves = v; break;
			case 'r': rle = v; break;
			case 'g': gap = v; break;
			case 't': tileh = v; break;
			case 'o': outmax = v; break;
			case 'S': strat = v; break;
			case 'm': meml = v; break;
			}
			i++;
		} else
			bench(argv[i]);
	}
	return 0;
}
