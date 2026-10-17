/* rt.c - round trip: frames encoded then decoded must match bit for bit.
 *
 * Random and screen-like frame sequences: noise, fills, text-like specks,
 * scrolls of the whole width or a column range by many offsets, partial
 * width changes, keyframes mid-stream, small output buffers (so updates
 * are split), odd strides and sizes. A viewer copy kept only through the
 * decoder's callbacks must match too.
 */
#include "tutil.h"

static int fails;

static void mutate(unsigned char* f, int stride, int w, int h)
{
	int kind = t_range(10), i, n;
	switch (kind) {
	case 0: {	/* noise in a rect */
		int x = t_range(w), y = t_range(h), rw = 1 + t_range(w - x), rh = 1 + t_range(h - y), a, b;
		for (b = y; b < y + rh; b++)
			for (a = x; a < x + rw; a++)
				t_pix(f, stride, a, b, t_range(16));
		break;
	}
	case 1: case 2: {	/* fills */
		n = 1 + t_range(5);
		for (i = 0; i < n; i++) {
			int x = t_range(w), y = t_range(h);
			t_fillrect(f, stride, x, y, 1 + t_range(w - x), 1 + t_range(h - y), t_range(16));
		}
		break;
	}
	case 3: case 4: {	/* text-like specks: a "line" of glyphs */
		int y = t_range(h), x = t_range(w), a, b, len = 1 + t_range(60);
		for (i = 0; i < len; i++) {
			int gx = x + i * 6;
			for (b = y; b < y + 9 && b < h; b++)
				for (a = gx; a < gx + 5 && a < w; a++)
					if (t_rand() & 1)
						t_pix(f, stride, a, b, (t_rand() & 1) ? 0 : 15);
		}
		break;
	}
	case 5: case 6: case 7: {	/* a scroll, whole width or a column range */
		int full = t_rand() & 1;
		int x0 = full ? 0 : t_range(w / 8) * 8, x1 = full ? w : x0 + 8 + t_range((w - x0) / 8) * 8;
		int y0 = t_range(h / 2), y1 = y0 + 2 + t_range(h - y0 - 1), dy, y;
		unsigned char* tmp = (unsigned char*)malloc(stride * h);
		if (y1 > h)
			y1 = h;
		dy = 1 + t_range((y1 - y0) / 2 + 1);
		if (t_rand() & 1)
			dy = -dy;
		memcpy(tmp, f, stride * h);
		for (y = y0; y < y1; y++) {
			int sy = y - dy;
			if (sy >= y0 && sy < y1)
				memcpy(f + y * stride + x0 / 2, tmp + sy * stride + x0 / 2, (x1 - x0) / 2);
			else {	/* new content comes in */
				int a;
				for (a = x0; a < x1; a++)
					t_pix(f, stride, a, y, (a * 7 + y * 3 + (int)(t_rand() & 3)) & 15);
			}
		}
		/* and sometimes a change elsewhere at the same time */
		if (t_rand() & 1)
			t_fillrect(f, stride, t_range(w), t_range(h), 1, 1, t_range(16));
		free(tmp);
		break;
	}
	case 8: {	/* single pixels */
		n = 1 + t_range(20);
		for (i = 0; i < n; i++)
			t_pix(f, stride, t_range(w), t_range(h), t_range(16));
		break;
	}
	default: {	/* a dithered fill (0x0F style bytes) */
		int x = t_range(w), y = t_range(h), rw = 1 + t_range(w - x), rh = 1 + t_range(h - y), a, b;
		for (b = y; b < y + rh; b++)
			for (a = x; a < x + rw; a++)
				t_pix(f, stride, a, b, ((a + b) & 1) ? 15 : 0);
		break;
	}
	}
}

static int same(const unsigned char* f, int stride, const unsigned char* s, int rb, int h)
{
	int y;
	for (y = 0; y < h; y++)
		if (memcmp(f + y * stride, s + y * rb, rb))
			return 0;
	return 1;
}

/* One run: n frames of size w x h, output buffer omax, deflate level. */
static void run(int w, int h, int stridepad, int n, int omax, int level, int partial, unsigned int seed)
{
	int stride = w / 2 + stridepad, i, k, len, r, nkey = 0, nupd = 0, npend = 0;
	unsigned char* f = (unsigned char*)calloc(stride * h + 4, 1);
	unsigned char* out = (unsigned char*)malloc(omax);
	pc_enc* e = pc_enc_new(w, h, level);
	pc_dec* d = pc_dec_new(w, h);
	t_viewer v;

	t_seed = seed;
	if (!e || !d || !f || !out) {
		printf("FAIL: alloc %dx%d\n", w, h);
		fails++;
		return;
	}
	pc_dec_set_flags(d, partial ? PC_DEC_PARTIAL_SCROLL : 0);
	memset(&v, 0, sizeof(v));
	v.w = w;
	v.h = h;
	v.rb = w / 2;
	v.dec = d;
	v.scr = (unsigned char*)malloc(v.rb * h);
	memset(v.scr, 0xff, v.rb * h);
	memset(f, 0x99, stride * h);

	for (i = 0; i < n; i++) {
		int key = i == 0 || t_range(40) == 0;
		int nm = 1 + t_range(3);
		for (k = 0; k < nm; k++)
			mutate(f, stride, w, h);
		for (k = 0; k < 500; k++) {
			r = pc_enc_frame(e, f, stride, key, out, omax, &len);
			if (r == 1)
				break;
			if (r != PC_OK) {
				printf("FAIL: enc returned %d (frame %d)\n", r, i);
				fails++;
				goto done;
			}
			nupd++;
			nkey += key;
			r = pc_dec_update(d, out, len, key, t_onscroll, t_onrect, &v);
			if (r != PC_OK) {
				printf("FAIL: dec returned %d (frame %d, %d bytes)\n", r, i, len);
				fails++;
				goto done;
			}
			key = 0;
			if (!pc_enc_pending(e))
				break;
			npend++;
		}
		if (pc_enc_pending(e) || pc_enc_changed(e, f, stride)) {
			printf("FAIL: still pending after 500 updates (frame %d)\n", i);
			fails++;
			goto done;
		}
		if (!same(f, stride, pc_dec_screen(d), pc_dec_stride(d), h)) {
			printf("FAIL: decoded screen differs (%dx%d frame %d)\n", w, h, i);
			fails++;
			goto done;
		}
		if (memcmp(v.scr, pc_dec_screen(d), v.rb * h)) {
			printf("FAIL: viewer copy differs: callbacks missed a change (frame %d)\n", i);
			fails++;
			goto done;
		}
		/* the same frame again: nothing to send */
		if (t_range(10) == 0 && pc_enc_frame(e, f, stride, 0, out, omax, &len) != 1) {
			printf("FAIL: unchanged frame not reported (frame %d)\n", i);
			fails++;
			goto done;
		}
	}
	printf("ok   %3dx%-3d stride+%d out %6d level %d %s: %d frames, %d updates (%d keyframes, %d split)\n",
	       w, h, stridepad, omax, level, partial ? "partial" : "full   ", n, nupd, nkey, npend);
done:
	pc_enc_free(e);
	pc_dec_free(d);
	free(f);
	free(out);
	free(v.scr);
}

static void args(void)
{
	unsigned char f[4 * 8 + 8], out[2048];
	int len;
	pc_enc* e;
	/* bad sizes */
	if (pc_enc_new(642, 240, 1) || pc_enc_new(644, 240, 1) || pc_enc_new(12, 2, 1) ||
	    pc_enc_new(640, 481, 1) || pc_enc_new(0, 1, 1) ||
	    pc_dec_new(641, 2) || pc_dec_new(642, 2) || pc_dec_new(640, 481) || pc_dec_new(0, 0)) {
		printf("FAIL: a bad size was accepted\n");
		fails++;
	}
	e = pc_enc_new(8, 8, 1);
	memset(f, 0, sizeof(f));
	/* the first update must be a keyframe; a tiny buffer is refused */
	if (pc_enc_frame(e, f, 4, 0, out, sizeof(out), &len) != PC_ERR_ARG ||
	    pc_enc_frame(e, f, 4, 1, out, 100, &len) != PC_ERR_SPACE ||
	    pc_enc_frame(e, f + 1, 4, 1, out, sizeof(out), &len) != PC_ERR_ARG ||
	    pc_enc_frame(e, f, 4, 1, out, sizeof(out), &len) != PC_OK ||
	    pc_enc_frame(e, f, 4, 0, out, sizeof(out), &len) != 1 || len != 0) {
		printf("FAIL: argument checks\n");
		fails++;
	} else
		printf("ok   argument checks\n");
	pc_enc_free(e);
}

int main(int argc, char** argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 300;
	args();
	run(640, 240, 0, n, 65532, 1, 0, 1);
	run(640, 240, 0, n, 65532, 6, 1, 2);
	run(640, 240, 4, n, 4096, 1, 0, 3);		/* small buffer: splits */
	run(640, 240, 0, n / 2, PC_ENC_MIN_OUT, 3, 1, 4);
	run(640, 480, 12, n / 2, 65532, 1, 0, 5);
	run(64, 16, 0, n, 2048, 1, 1, 6);
	run(8, 1, 0, n, 1024, 1, 0, 7);
	run(8, 3, 4, n, 1024, 9, 1, 8);
	run(200, 37, 0, n, 1500, 2, 0, 9);
	if (fails) {
		printf("rt: %d FAILED\n", fails);
		return 1;
	}
	printf("rt: all passed\n");
	return 0;
}
