/* config.h - Links 2.30 configuration for PsiWeb on EPOC R5 (ESTLIB, gcc 3.0
 * Psion 98r2, ARM710T). Written by hand: Links' configure cannot run for
 * this target. Used for psiweb's ARM build (web/links/epoc.mk), both the
 * device EXE and the emulator harness build (psiweb-emu.pe).
 *
 * Only what ESTLIB really has is declared. Anything Links needs that ESTLIB
 * lacks is in psicompat.h / psi_os.c.
 */
#ifndef PSI_EPOC_CONFIG_H
#define PSI_EPOC_CONFIG_H

#define PSI_EPOC 1		/* the EPOC build: pwn network, psi_os.c loop */

#define PACKAGE "links"
#define VERSION "2.30"

/* C and types: ARM, 32-bit, little-endian */
#define STDC_HEADERS 1
#define RETSIGTYPE void
#define SIZEOF_UNSIGNED 4
#define SIZEOF_UNSIGNED_LONG 4
#define SIZEOF_UNSIGNED_LONG_LONG 8
#define SIZEOF_UNSIGNED_SHORT 2
#define HAVE_LONG_LONG 1
#define HAVE_VOLATILE 1
#define HAVE___RESTRICT 1
#define HAVE_ERRNO 1
#define C_LITTLE_ENDIAN 1
#define DEBUGLEVEL 0
#define HAVE_STDLIB_H_X 1

/* headers ESTLIB has */
#define HAVE_STDARG_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_TIME_H 1
#define HAVE_SYS_TIME_H 1
#define TIME_WITH_SYS_TIME 1
#define HAVE_DIRENT_H 1
#define HAVE_SETJMP_H 1
#define HAVE_MATH_H 1
#define HAVE_LOCALE_H 1
#define HAVE_ARPA_INET_H 1

/* functions ESTLIB has (or psi_os.c supplies) */
#define HAVE_STRFTIME 1
#define HAVE_VPRINTF 1
#define HAVE_SNPRINTF 1		/* web/compat/nsprintf.c */
#define HAVE_CALLOC 1
#define HAVE_GETCWD 1
#define HAVE_GETPID 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_GMTIME 1
#define HAVE_MKTIME 1
#define HAVE_STRTOD 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRLEN 1
#define HAVE_STRCPY 1
#define HAVE_STRNCPY 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_STRCSPN 1
#define HAVE_STRSPN 1
#define HAVE_STRSTR 1
#define HAVE_MEMCMP 1
#define HAVE_MEMCHR 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_GCC_ASSEMBLER 1

#define RENAME_OVER_EXISTING_FILES 1

/* no IPv6, no threads, no fork for DNS: every connection goes through
 * psiglue (web/engine/pwnet.c), which also resolves the name */
#define NO_ASYNC_LOOKUP
#define HAVE_GETHOSTBYNAME 1

/* graphics with only the psi driver */
#define G 1
#define GRDRV_PSI 1

/* pictures: libpng, IJG libjpeg and zlib built from source */
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
#define HAVE_ZLIB 1
#define HAVE_ZLIB_H 1
#define HAVE_LIBZ 1

/* PsiWeb changes in Links (patches/links-2.30-psion.diff) */
#define PSI_LAZY_IMAGES 1	/* img.c: no decoder kept for pictures not yet drawn */
#define PSIWEB 1		/* html.c: no <link> lines or prefetches */
#define PSI_JPEG_SCALE 1	/* jpeg.c: libjpeg scales down while decoding */
#define PSI_JPEG_MAX_W 1280	/* ... and caps pictures with no size given */

#endif
