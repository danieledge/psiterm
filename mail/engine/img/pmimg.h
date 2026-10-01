/* pmimg.h - small picture decoders for the Psion: JPEG (baseline and progressive), PNG and
 * GIF (first frame) straight to 16 greys, scaled down to fit and dithered.
 *
 * Made for an ARM710 with no FPU and little memory: everything is integer,
 * the source is read through a callback a piece at a time, rows are
 * shrunk as they come (JPEGs eight times bigger than wanted are decoded at
 * 1/8 from the DC coefficients alone), and only the result - two pixels a
 * byte, a few hundred KB at most - is kept. A progressive JPEG needs its
 * coefficients kept until the last scan: only the luma's, and only those
 * the shrink uses, within max_full_bytes (pmjprog.c). Interlaced PNGs and GIFs need
 * the whole picture at 8 bits a pixel, so they are refused above a size.
 *
 * Corrupt or hostile input gives an error, never a crash: sizes are checked
 * before anything is allocated, every table index is bounded, and the
 * caller can stop a slow decode through the abort callback. A picture cut
 * short is returned as far as it got (partial = 1), the rest grey.
 *
 * The result's rows are laid out as EPOC's EGray16 bitmaps have them: the
 * left pixel of each byte in the low nibble, 0 black to 15 white, rows
 * padded to 32 bits - so a row can be copied into a CFbsBitmap as it is.
 *
 * Shared by PsiMail's engine (engine/pictures.c) and meant for PsiWeb too.
 * Nothing here depends on EPOC or on the rest of the engine.
 *
 * Third-party code: picojpeg (public domain, Rich Geldreich) and zlib's
 * inflate (ssh/zlib, zlib licence) - see THIRD-PARTY.md.
 */
#ifndef PMIMG_H
#define PMIMG_H

#ifdef __cplusplus
extern "C" {
#endif

/* reads up to n bytes: >0 read, 0 end of data, <0 error */
typedef int (*PmImgRead)(void *ctx, unsigned char *buf, int n);
/* back to the first byte (JPEGs are read twice when decoded at 1/8): 0 ok.
   May be 0, and then a big JPEG is decoded the slow way. */
typedef int (*PmImgRewind)(void *ctx);
/* called now and then while decoding, with how far it has got (0..100):
   return nonzero to stop */
typedef int (*PmImgAbort)(void *ctx, int percent);

enum { PMIMG_UNKNOWN = 0, PMIMG_JPEG, PMIMG_PNG, PMIMG_GIF };

enum
	{
	PMIMG_OK = 0,
	PMIMG_E_FORMAT,          /* not a picture we know, or too broken to start */
	PMIMG_E_UNSUPPORTED,     /* e.g. an arithmetic-coded JPEG */
	PMIMG_E_TOO_BIG,         /* beyond the limits in PmImgOpts */
	PMIMG_E_MEMORY,
	PMIMG_E_READ,            /* the read callback failed */
	PMIMG_E_ABORTED          /* the abort callback said stop */
	};

typedef struct
	{
	int max_w, max_h;        /* the result fits in this (0: 544 x 960) */
	long max_src_pixels;     /* refuse a source with more (0: 24 million) */
	long max_full_bytes;     /* whole-picture buffer for interlaced files (0: 640 KB) */
	int no_dither;           /* 1: plain rounding to 16 greys */
	PmImgAbort abort;        /* may be 0 */
	void *abort_ctx;
	} PmImgOpts;

typedef struct
	{
	int w, h;                /* the result */
	int src_w, src_h;        /* the picture's own size */
	int stride;              /* bytes a row: ((w + 7) / 8) * 4 */
	unsigned char *bits;     /* stride * h bytes (pmimg_free) */
	int type;                /* PMIMG_JPEG.. */
	int partial;             /* 1: the data ran out; the rest is grey */
	} PmImage;

/* what the first bytes say it is (n >= 8 is enough) */
int pmimg_type(const unsigned char *head, int n);
/* the size alone, from the first bytes of a file, without decoding it (0 ok) */
int pmimg_size(const unsigned char *head, int n, int *w, int *h);

/* decodes to *out (zeroed first; free with pmimg_free). opts may be 0.
   err (may be 0) gets a short reason on failure. */
int pmimg_decode(PmImgRead read, PmImgRewind rewind, void *ctx, const PmImgOpts *opts, PmImage *out, char *err, int errmax);
/* the same from a file, or from memory */
int pmimg_decode_file(const char *path, const PmImgOpts *opts, PmImage *out, char *err, int errmax);
int pmimg_decode_mem(const unsigned char *data, long len, const PmImgOpts *opts, PmImage *out, char *err, int errmax);
void pmimg_free(PmImage *img);

/* one pixel, 0..15 */
#define PMIMG_PIXEL(img, x, y) (((img)->bits[(y) * (img)->stride + ((x) >> 1)] >> (((x) & 1) * 4)) & 15)

/* tests: writes the result as a binary PGM (0 ok) */
int pmimg_write_pgm(const PmImage *img, const char *path);

/* ---- for the decoders (pmjpeg.c, pmpng.c, pmgif.c): the shared sink ---- */

typedef struct PmImgSink PmImgSink;
/* a source of src_w x src_h 8-bit grey rows; chooses the shrink factor */
PmImgSink *pmimg_sink_new(int src_w, int src_h, const PmImgOpts *opts, int type, int *err);
/* one source row, left to right, 0 black .. 255 white */
void pmimg_sink_row(PmImgSink *s, const unsigned char *row);
/* the same picture, all at once (for interlaced formats) */
void pmimg_sink_all(PmImgSink *s, const unsigned char *rows);
int  pmimg_sink_rows_done(const PmImgSink *s);
/* finishes: the result moves to *out (rows not fed are grey); frees the sink */
int  pmimg_sink_end(PmImgSink *s, PmImage *out);
void pmimg_sink_abandon(PmImgSink *s);
/* nonzero: stop (the abort callback, checked every few rows) */
int  pmimg_sink_abort(PmImgSink *s);

/* a buffered reader over the callback */
typedef struct
	{
	PmImgRead read;
	PmImgRewind rewind;
	void *ctx;
	unsigned char buf[1024];
	int pos, len;
	int eof, err;
	long total;
	} PmImgIn;
void pmimg_in_init(PmImgIn *in, PmImgRead read, PmImgRewind rewind, void *ctx);
int  pmimg_in_rewind(PmImgIn *in);                            /* 1 ok, 0 not possible */
int  pmimg_in_byte(PmImgIn *in);                              /* 0..255, or -1 at the end */
int  pmimg_in_read(PmImgIn *in, unsigned char *buf, int n);   /* bytes read (short at the end) */
int  pmimg_in_skip(PmImgIn *in, long n);                      /* 1 ok */
unsigned long pmimg_in_be32(PmImgIn *in);
unsigned int  pmimg_in_le16(PmImgIn *in);

/* the limits, filled in from opts (0 = the defaults) */
void pmimg_opts_fill(PmImgOpts *o, const PmImgOpts *from);

int pmimg_decode_jpeg(PmImgIn *in, const PmImgOpts *opts, PmImage *out, char *err, int errmax);
/* progressive (and extended sequential) JPEGs: pmjprog.c, from the start of the file */
int pmimg_decode_jpeg_prog(PmImgIn *in, const PmImgOpts *opts, PmImage *out, char *err, int errmax);
int pmimg_decode_png(PmImgIn *in, const PmImgOpts *opts, PmImage *out, char *err, int errmax);
int pmimg_decode_gif(PmImgIn *in, const PmImgOpts *opts, PmImage *out, char *err, int errmax);
void pmimg_err(char *err, int errmax, const char *text);

#define PMIMG_MAX_DIM 16384

#ifdef __cplusplus
}
#endif
#endif
