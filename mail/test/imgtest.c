/* imgtest.c - the picture decoders (engine/img) on a PC
 *
 *   imgtest decode IN OUT.pgm [MAXW [MAXH]]   decode a JPEG/PNG/GIF to a PGM
 *   imgtest fuzz IN ROUNDS [SEED]             truncate and bit-flip IN, decode
 *                                             each variant from memory (no
 *                                             crashes; build with -fsanitize)
 *   imgtest time IN                           how long a decode takes here
 *   imgtest pmi IN OUT.pmi                    decode to the engine's .pmi file
 *                                             (engine/pictures.c), for seeding tests
 *
 * Built by imgtest.py, which also makes the sample pictures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../engine/img/pmimg.h"

static unsigned char *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *d;
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = (unsigned char *)malloc(*len + 1);
	if (fread(d, 1, *len, f) != (size_t)*len) { fclose(f); free(d); return 0; }
	fclose(f);
	return d;
}

static unsigned int rnd_state = 12345;
static unsigned int rnd(void)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return rnd_state >> 8;
}

static int abort_never(void *ctx, int percent) { (void)ctx; (void)percent; return 0; }

int main(int argc, char **argv)
{
	PmImgOpts o;
	PmImage img;
	char err[100];
	int r;
	memset(&o, 0, sizeof(o));
	o.abort = abort_never;
	if (argc >= 4 && !strcmp(argv[1], "decode")) {
		if (argc >= 5) o.max_w = atoi(argv[4]);
		if (argc >= 6) o.max_h = atoi(argv[5]);
		r = pmimg_decode_file(argv[2], &o, &img, err, sizeof(err));
		if (r != PMIMG_OK) { printf("error %d: %s\n", r, err); return 1; }
		printf("%s: %dx%d -> %dx%d type %d%s\n", argv[2], img.src_w, img.src_h, img.w, img.h, img.type, img.partial ? " (partial)" : "");
		if (pmimg_write_pgm(&img, argv[3]) != 0) { printf("could not write %s\n", argv[3]); return 1; }
		pmimg_free(&img);
		return 0;
	}
	if (argc >= 4 && !strcmp(argv[1], "fuzz")) {
		long len, i;
		int rounds = atoi(argv[3]), ok = 0, partial = 0, failed = 0, k;
		unsigned char *data = slurp(argv[2], &len), *v;
		if (!data) { printf("could not read %s\n", argv[2]); return 1; }
		if (argc >= 5) rnd_state = (unsigned int)atoi(argv[4]);
		v = (unsigned char *)malloc(len + 1);
		o.max_w = 200;                      /* small results: fast rounds */
		o.max_h = 400;
		for (k = 0; k < rounds; k++) {
			long vlen = len;
			int kind = k % 4, nflips, f;
			memcpy(v, data, len);
			switch (kind) {
			case 0:                                       /* cut short */
				vlen = (long)(rnd() % (unsigned int)len);
				break;
			case 1:                                       /* a few bits flipped */
				nflips = 1 + (int)(rnd() % 8);
				for (f = 0; f < nflips; f++) v[rnd() % (unsigned int)len] ^= (unsigned char)(1 << (rnd() % 8));
				break;
			case 2:                                       /* bytes overwritten */
				nflips = 1 + (int)(rnd() % 32);
				for (f = 0; f < nflips; f++) v[rnd() % (unsigned int)len] = (unsigned char)rnd();
				break;
			default:                                      /* the header stirred */
				nflips = 1 + (int)(rnd() % 4);
				for (f = 0; f < nflips; f++) { long at = rnd() % (unsigned int)(len < 64 ? len : 64); v[at] ^= (unsigned char)(1 << (rnd() % 8)); }
				break;
			}
			r = pmimg_decode_mem(v, vlen, &o, &img, err, sizeof(err));
			if (r == PMIMG_OK) { ok++; if (img.partial) partial++; pmimg_free(&img); }
			else failed++;
		}
		printf("%s: %d rounds: %d decoded (%d partial), %d refused\n", argv[2], rounds, ok, partial, failed);
		free(v);
		free(data);
		(void)i;
		return 0;
	}
	if (argc >= 4 && !strcmp(argv[1], "pmi")) {
		FILE *f;
		unsigned char h[16];
		o.max_w = 544; o.max_h = 960;
		r = pmimg_decode_file(argv[2], &o, &img, err, sizeof(err));
		if (r != PMIMG_OK) { printf("error %d: %s\n", r, err); return 1; }
		if (!(f = fopen(argv[3], "wb"))) return 1;
		memset(h, 0, sizeof(h));
		memcpy(h, "PMI1", 4);
		h[4] = (unsigned char)img.w; h[5] = (unsigned char)(img.w >> 8);
		h[6] = (unsigned char)img.h; h[7] = (unsigned char)(img.h >> 8);
		h[8] = (unsigned char)img.src_w; h[9] = (unsigned char)(img.src_w >> 8);
		h[10] = (unsigned char)img.src_h; h[11] = (unsigned char)(img.src_h >> 8);
		h[12] = (unsigned char)(img.partial ? 1 : 0);
		fwrite(h, 1, 16, f);
		fwrite(img.bits, 1, (long)img.stride * img.h, f);
		fclose(f);
		printf("%s: %dx%d -> %dx%d, %ld bytes\n", argv[3], img.src_w, img.src_h, img.w, img.h, 16 + (long)img.stride * img.h);
		pmimg_free(&img);
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "time")) {
		clock_t t0 = clock();
		r = pmimg_decode_file(argv[2], &o, &img, err, sizeof(err));
		printf("%s: %d (%s) %dx%d -> %dx%d in %ld ms\n", argv[2], r, err, img.src_w, img.src_h, img.w, img.h, (long)((clock() - t0) * 1000 / CLOCKS_PER_SEC));
		pmimg_free(&img);
		return r != PMIMG_OK;
	}
	printf("usage: imgtest decode IN OUT.pgm [MAXW [MAXH]] | fuzz IN ROUNDS [SEED] | time IN\n");
	return 2;
}
