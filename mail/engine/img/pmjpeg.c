/* pmjpeg.c - baseline JPEG to 16 greys, through picojpeg (public domain).
 *
 * picojpeg decodes one MCU (8x8 to 16x16 pixels) at a time with a few KB
 * of state; only the luma blocks are transformed (pjpeg_grey_only), the
 * chroma is entropy-decoded to keep the stream in step. A row of MCUs is
 * assembled here and fed to the sink row by row. A picture bigger than
 * wanted is shrunk as it is decoded, as libjpeg's scale_denom does: to 1/2
 * or 1/4 by a reduced IDCT that gives each block as 4x4 or 2x2 pixels
 * (pjpeg_set_shrink, added to picojpeg for the Psion), or to 1/8 in
 * picojpeg's reduce mode - one pixel per block, from the DC coefficient
 * alone, with no IDCT. The biggest shrink that still leaves at least half
 * the wanted size is used; the sink box-filters the rest. That is many
 * times faster on a 36 MHz ARM, and the screen is only 240 pixels tall.
 * Choosing needs the size first, so the header is read once at full size
 * and the stream rewound (when it can be; else it is decoded whole).
 * Progressive JPEGs (and the kinds picojpeg refuses that are still
 * Huffman-coded: SOF1, odd sampling factors, 16-bit tables) go to
 * pmjprog.c, which keeps every block's coefficients until the last scan.
 * Arithmetic-coded JPEGs are refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmimg.h"
#include "picojpeg.h"

static unsigned char need_bytes(unsigned char *buf, unsigned char size, unsigned char *got, void *ctx)
{
	PmImgIn *in = (PmImgIn *)ctx;
	int n = pmimg_in_read(in, buf, size);
	*got = (unsigned char)n;
	if (n == 0 && in->err) return PJPG_STREAM_READ_ERROR;
	/* the end of the data shows as a short read; picojpeg then stops at
	   the missing marker and what was decoded is kept */
	return 0;
}

static const char *reason(int status)
{
	switch (status) {
	case PJPG_UNSUPPORTED_MODE: return "a progressive JPEG";
	case PJPG_NO_ARITHMITIC_SUPPORT: return "an arithmetic-coded JPEG";
	case PJPG_NOT_JPEG: return "not a JPEG";
	case PJPG_UNSUPPORTED_COLORSPACE:
	case PJPG_UNSUPPORTED_SAMP_FACTORS:
	case PJPG_UNSUPPORTED_COMP_IDENT:
	case PJPG_UNSUPPORTED_QUANT_TABLE:
	case PJPG_TOO_MANY_COMPONENTS: return "a JPEG of a kind not supported";
	case PJPG_STREAM_READ_ERROR: return "could not read the file";
	case PJPG_NOTENOUGHMEM: return "not enough memory";
	case PJPG_BAD_WIDTH:
	case PJPG_BAD_HEIGHT: return "too big";
	default: return "a damaged JPEG";
	}
}

static int unsupported(int status)
{
	return status == PJPG_UNSUPPORTED_MODE || status == PJPG_NO_ARITHMITIC_SUPPORT ||
		status == PJPG_UNSUPPORTED_COLORSPACE || status == PJPG_UNSUPPORTED_SAMP_FACTORS ||
		status == PJPG_UNSUPPORTED_COMP_IDENT || status == PJPG_UNSUPPORTED_QUANT_TABLE ||
		status == PJPG_TOO_MANY_COMPONENTS;
}

/* picojpeg's block offsets within an MCU, and the block's place in it */
static void mcu_geometry(pjpeg_scan_type_t t, int *nblocks, int ofs[4], int bx[4], int by[4])
{
	int n = 1;
	ofs[0] = 0; bx[0] = 0; by[0] = 0;
	switch (t) {
	case PJPG_YH2V1: ofs[1] = 64; bx[1] = 1; by[1] = 0; n = 2; break;
	case PJPG_YH1V2: ofs[1] = 128; bx[1] = 0; by[1] = 1; n = 2; break;
	case PJPG_YH2V2:
		ofs[1] = 64; bx[1] = 1; by[1] = 0;
		ofs[2] = 128; bx[2] = 0; by[2] = 1;
		ofs[3] = 192; bx[3] = 1; by[3] = 1;
		n = 4;
		break;
	default: break;
	}
	*nblocks = n;
}

int pmimg_decode_jpeg(PmImgIn *in, const PmImgOpts *from, PmImage *out, char *err, int errmax)
{
	PmImgOpts o;
	pjpeg_image_info_t info;
	PmImgSink *sink = 0;
	unsigned char *band = 0;         /* one row of MCUs, grey */
	int status, reduce = 0, shrink, bs, sw, sh, mw, mh, bw, nb, ofs[4], bx[4], by[4];
	int mcux = 0, mcuy = 0, per_row, per_col, r = PMIMG_OK, e, y;
	int orig_w, orig_h;              /* the picture's own size */
	long src_pixels;

	pmimg_opts_fill(&o, from);
	pjpeg_grey_only(1);
	pjpeg_set_shrink(0);
	status = pjpeg_decode_init(&info, need_bytes, in, 0);
	if (status && (status == PJPG_UNSUPPORTED_MODE || status == PJPG_UNSUPPORTED_MARKER ||
	               status == PJPG_UNSUPPORTED_SAMP_FACTORS || status == PJPG_UNSUPPORTED_QUANT_TABLE) &&
	    pmimg_in_rewind(in))
		/* progressive, extended (SOF1), odd sampling, 16-bit tables: pmjprog.c */
		return pmimg_decode_jpeg_prog(in, from, out, err, errmax);
	if (status) {
		pmimg_err(err, errmax, reason(status));
		return status == PJPG_STREAM_READ_ERROR ? PMIMG_E_READ : unsupported(status) ? PMIMG_E_UNSUPPORTED : PMIMG_E_FORMAT;
	}
	orig_w = info.m_width;
	orig_h = info.m_height;
	src_pixels = (long)info.m_width * (long)info.m_height;
	if (info.m_width > PMIMG_MAX_DIM || info.m_height > PMIMG_MAX_DIM || src_pixels > o.max_src_pixels) {
		pmimg_err(err, errmax, "too big");
		return PMIMG_E_TOO_BIG;
	}
	/* a half, quarter or eighth of it is plenty? then decode only that much */
	for (shrink = 3; shrink > 0; shrink--)
		if ((info.m_width >> shrink) >= o.max_w / 2 || (info.m_height >> shrink) >= o.max_h / 2) break;
	if (shrink && pmimg_in_rewind(in)) {
		if (shrink == 3) reduce = 1;
		else pjpeg_set_shrink((unsigned char)shrink);
		status = pjpeg_decode_init(&info, need_bytes, in, (unsigned char)reduce);
		if (status) { pmimg_err(err, errmax, reason(status)); return PMIMG_E_FORMAT; }
	} else shrink = 0;
	per_row = info.m_MCUSPerRow;
	per_col = info.m_MCUSPerCol;
	mcu_geometry(info.m_scanType, &nb, ofs, bx, by);
	bs = 8 >> shrink;                    /* pixels along a block now */
	mw = info.m_MCUWidth >> shrink;      /* pixels along an MCU */
	mh = info.m_MCUHeight >> shrink;
	sw = (info.m_width + (1 << shrink) - 1) >> shrink;
	sh = (info.m_height + (1 << shrink) - 1) >> shrink;
	if (per_row <= 0 || per_col <= 0 || mw <= 0 || mh <= 0 || (long)per_row * mw < sw) {
		pmimg_err(err, errmax, "a damaged JPEG");
		return PMIMG_E_FORMAT;
	}
	sink = pmimg_sink_new(sw, sh, &o, PMIMG_JPEG, &e);
	if (!sink) { pmimg_err(err, errmax, e == PMIMG_E_TOO_BIG ? "too big" : "not enough memory"); return e; }
	bw = per_row * mw;
	band = (unsigned char *)malloc((long)bw * mh);
	if (!band) { pmimg_sink_abandon(sink); pmimg_err(err, errmax, "not enough memory"); return PMIMG_E_MEMORY; }

	for (;;) {
		int b;
		status = pjpeg_decode_mcu();
		if (status) {
			if (status == PJPG_NO_MORE_BLOCKS) break;
			/* damaged or cut short: keep the rows so far, if there are any */
			if (mcuy == 0 && (mcux == 0 || status == PJPG_STREAM_READ_ERROR)) {
				r = status == PJPG_STREAM_READ_ERROR ? PMIMG_E_READ : PMIMG_E_FORMAT;
				pmimg_err(err, errmax, reason(status));
			}
			break;
		}
		for (b = 0; b < nb; b++) {
			/* each block's bs x bs pixels, bs a row, at its slot in the MCU buffer */
			const unsigned char *src = info.m_pMCUBufR + ofs[b];
			unsigned char *dst = band + (long)(by[b] * bs) * bw + mcux * mw + bx[b] * bs;
			for (y = 0; y < bs; y++, src += bs, dst += bw) memcpy(dst, src, bs);
		}
		if (++mcux == per_row) {
			int rows = sh - mcuy * mh;
			if (rows > mh) rows = mh;
			for (y = 0; y < rows; y++) pmimg_sink_row(sink, band + (long)y * bw);
			mcux = 0;
			if (++mcuy >= per_col) break;
			if (pmimg_sink_abort(sink)) { r = PMIMG_E_ABORTED; pmimg_err(err, errmax, "stopped"); break; }
		}
	}
	if (r == PMIMG_OK && mcux > 0 && mcuy < per_col) {
		/* a row of MCUs cut off part-way still shows what it had; the rest
		   of that row is grey */
		int rows = sh - mcuy * mh;
		if (rows > mh) rows = mh;
		for (y = 0; y < rows; y++) memset(band + (long)y * bw + mcux * mw, 0xcc, (long)(per_row - mcux) * mw);
		for (y = 0; y < rows; y++) pmimg_sink_row(sink, band + (long)y * bw);
	}
	free(band);
	if (r != PMIMG_OK) { pmimg_sink_abandon(sink); return r; }
	r = pmimg_sink_end(sink, out);
	out->src_w = orig_w;
	out->src_h = orig_h;
	return r;
}
