/* config2.h - added to Links' configure output (cfg.h includes it when
 * HAVE_CONFIG2_H is defined) for the PC proof of Links for PsiWeb.
 *
 * configure runs in text mode (it refuses graphics without a known
 * driver); this turns graphics on with only the "psi" driver, and the
 * image libraries we build from source (web/links/Makefile).
 */
#define G 1
#define GRDRV_PSI 1

#define HAVE_PNG_H 1
#define HAVE_PNG_CREATE_INFO_STRUCT 1
#define HAVE_PNG_GET_BIT_DEPTH 1
#define HAVE_PNG_GET_COLOR_TYPE 1
#define HAVE_PNG_GET_GAMA 1
#define HAVE_PNG_GET_IMAGE_HEIGHT 1
#define HAVE_PNG_GET_IMAGE_WIDTH 1
#define HAVE_PNG_GET_LIBPNG_VER 1
#define HAVE_PNG_GET_SRGB 1
#define HAVE_PNG_GET_VALID 1
#define HAVE_PNG_SET_RGB_TO_GRAY 1
#define HAVE_PNG_SET_STRIP_ALPHA 1

#define HAVE_JPEG 1
#define HAVE_JPEGLIB_H 1

/* zlib is built from source (ssh/zlib), not found by configure */
#ifndef HAVE_ZLIB
#define HAVE_ZLIB 1
#endif
#ifndef HAVE_ZLIB_H
#define HAVE_ZLIB_H 1
#endif

/* configure ran 64-bit; the proof is built with -m32 like the ARM target */
#if defined(__i386__)
#undef SIZEOF_UNSIGNED_LONG
#define SIZEOF_UNSIGNED_LONG 4
#endif

/* no thread or fork for DNS: the Psion has neither */
#ifndef NO_ASYNC_LOOKUP
#define NO_ASYNC_LOOKUP
#endif

/* PsiWeb changes in Links (patches/links-2.30-psion.diff) */
#define PSI_LAZY_IMAGES 1	/* img.c: no decoder kept for pictures not yet drawn */
#define PSIWEB 1		/* html.c: no <link> lines or prefetches */
#define PSI_JPEG_SCALE 1	/* jpeg.c: libjpeg scales down while decoding */
#define PSI_JPEG_MAX_W 1280	/* ... and caps pictures with no size given */
