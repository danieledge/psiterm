/* nscompat.h is force-included (before this line): undo its stdio renames
 * here, where the real functions are wanted */
#undef fprintf
#undef vfprintf
#undef printf
#undef fputs
#undef fputc
#undef putc
#undef putchar
#undef puts
#undef fwrite
#undef perror
#undef fflush
#undef setbuf
#undef setvbuf
/* nscompat.c - C library pieces NetSurf needs that EPOC R5's estlib lacks */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "iconv.h"

/* ------------------------------------------------------------------ pread */

ssize_t nsc_pread(int fd, void *buf, size_t n, off_t off)
{
	off_t old = lseek(fd, 0, SEEK_CUR);
	ssize_t r;
	if (lseek(fd, off, SEEK_SET) < 0) return -1;
	r = read(fd, buf, n);
	lseek(fd, old, SEEK_SET);
	return r;
}

ssize_t nsc_pwrite(int fd, const void *buf, size_t n, off_t off)
{
	off_t old = lseek(fd, 0, SEEK_CUR);
	ssize_t r;
	if (lseek(fd, off, SEEK_SET) < 0) return -1;
	r = write(fd, buf, n);
	lseek(fd, old, SEEK_SET);
	return r;
}

/* ------------------------------------------------------------------ iconv
 * NetSurf converts between UTF-8 and "local" encodings with iconv. Page
 * charsets are decoded by libparserutils itself, so only a few encodings
 * are needed here: UTF-8, UTF-16 (LE/BE), UCS-4, US-ASCII, ISO-8859-1 and
 * Windows-1252 (EPOC's own 8-bit character set). */

enum { E_UTF8, E_UTF16LE, E_UTF16BE, E_UCS4LE, E_UCS4BE, E_ASCII, E_LATIN1, E_CP1252 };

struct nsc_iconv { int from, to, translit; };

static const unsigned short cp1252[32] = {
	0x20ac, 0x81, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
	0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8d, 0x017d, 0x8f,
	0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
	0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x9d, 0x017e, 0x0178
};

static int enc_id(const char *name, int *translit)
{
	char n[40];
	int i;
	for (i = 0; name[i] && i < 39; i++) {
		char c = name[i];
		if (c >= 'a' && c <= 'z') c -= 32;
		n[i] = c;
	}
	n[i] = 0;
	if (strstr(n, "//TRANSLIT") || strstr(n, "//IGNORE")) {
		*translit = 1;
		*strchr(n, '/') = 0;
	}
	if (!strcmp(n, "UTF-8") || !strcmp(n, "UTF8")) return E_UTF8;
	if (!strcmp(n, "UTF-16LE") || !strcmp(n, "UTF-16")) return E_UTF16LE;
	if (!strcmp(n, "UTF-16BE")) return E_UTF16BE;
	if (!strcmp(n, "UCS-4LE") || !strcmp(n, "UCS-4") || !strcmp(n, "UCS-4-INTERNAL")) return E_UCS4LE;
	if (!strcmp(n, "UCS-4BE") || !strcmp(n, "UTF-32BE")) return E_UCS4BE;
	if (!strcmp(n, "US-ASCII") || !strcmp(n, "ASCII")) return E_ASCII;
	if (!strcmp(n, "ISO-8859-1") || !strcmp(n, "LATIN1") || !strcmp(n, "ISO8859-1")) return E_LATIN1;
	if (!strcmp(n, "WINDOWS-1252") || !strcmp(n, "CP1252")) return E_CP1252;
	return -1;
}

iconv_t iconv_open(const char *to, const char *from)
{
	struct nsc_iconv *cd;
	int t = 0, f, tt;
	f = enc_id(from, &t);
	tt = enc_id(to, &t);
	if (f < 0 || tt < 0) {
		errno = EINVAL;
		return (iconv_t)-1;
	}
	cd = malloc(sizeof(*cd));
	if (!cd) return (iconv_t)-1;
	cd->from = f; cd->to = tt; cd->translit = t;
	return cd;
}

int iconv_close(iconv_t cd)
{
	if (cd && cd != (iconv_t)-1) free(cd);
	return 0;
}

/* decodes one character; returns bytes used, 0 = need more, -1 = bad */
static int dec(int e, const unsigned char *s, size_t n, unsigned int *u)
{
	switch (e) {
	case E_UTF8: {
		unsigned int c = s[0];
		int len, i;
		if (c < 0x80) { *u = c; return 1; }
		if (c < 0xc2) return -1;
		len = c < 0xe0 ? 2 : c < 0xf0 ? 3 : c < 0xf5 ? 4 : 0;
		if (!len) return -1;
		if (n < (size_t)len) return 0;
		c &= 0x3f >> (len - 1);
		for (i = 1; i < len; i++) {
			if ((s[i] & 0xc0) != 0x80) return -1;
			c = (c << 6) | (s[i] & 0x3f);
		}
		*u = c;
		return len;
	}
	case E_UTF16LE: case E_UTF16BE: {
		unsigned int a, b;
		if (n < 2) return 0;
		a = e == E_UTF16LE ? s[0] | (s[1] << 8) : (s[0] << 8) | s[1];
		if (a < 0xd800 || a > 0xdfff) { *u = a; return 2; }
		if (a > 0xdbff) return -1;
		if (n < 4) return 0;
		b = e == E_UTF16LE ? s[2] | (s[3] << 8) : (s[2] << 8) | s[3];
		if (b < 0xdc00 || b > 0xdfff) return -1;
		*u = 0x10000 + ((a - 0xd800) << 10) + (b - 0xdc00);
		return 4;
	}
	case E_UCS4LE:
		if (n < 4) return 0;
		*u = s[0] | (s[1] << 8) | (s[2] << 16) | ((unsigned)s[3] << 24);
		return 4;
	case E_UCS4BE:
		if (n < 4) return 0;
		*u = ((unsigned)s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3];
		return 4;
	case E_ASCII:
		if (s[0] > 0x7f) return -1;
		*u = s[0]; return 1;
	case E_LATIN1:
		*u = s[0]; return 1;
	case E_CP1252:
		*u = (s[0] >= 0x80 && s[0] < 0xa0) ? cp1252[s[0] - 0x80] : s[0];
		return 1;
	}
	return -1;
}

/* encodes one character; returns bytes written, 0 = no room, -1 = can't */
static int enc(int e, unsigned int u, unsigned char *d, size_t n)
{
	int i;
	switch (e) {
	case E_UTF8:
		if (u < 0x80) { if (n < 1) return 0; d[0] = (unsigned char)u; return 1; }
		if (u < 0x800) { if (n < 2) return 0; d[0] = 0xc0 | (u >> 6); d[1] = 0x80 | (u & 0x3f); return 2; }
		if (u < 0x10000) { if (n < 3) return 0; d[0] = 0xe0 | (u >> 12); d[1] = 0x80 | ((u >> 6) & 0x3f); d[2] = 0x80 | (u & 0x3f); return 3; }
		if (n < 4) return 0;
		d[0] = 0xf0 | (u >> 18); d[1] = 0x80 | ((u >> 12) & 0x3f);
		d[2] = 0x80 | ((u >> 6) & 0x3f); d[3] = 0x80 | (u & 0x3f);
		return 4;
	case E_UTF16LE: case E_UTF16BE: {
		unsigned int a = u, b = 0;
		int k = 2;
		if (u >= 0x10000) { a = 0xd800 + ((u - 0x10000) >> 10); b = 0xdc00 + ((u - 0x10000) & 0x3ff); k = 4; }
		if (n < (size_t)k) return 0;
		if (e == E_UTF16LE) { d[0] = a; d[1] = a >> 8; if (k == 4) { d[2] = b; d[3] = b >> 8; } }
		else { d[0] = a >> 8; d[1] = a; if (k == 4) { d[2] = b >> 8; d[3] = b; } }
		return k;
	}
	case E_UCS4LE: case E_UCS4BE:
		if (n < 4) return 0;
		for (i = 0; i < 4; i++)
			d[e == E_UCS4LE ? i : 3 - i] = (unsigned char)(u >> (8 * i));
		return 4;
	case E_ASCII:
		if (u > 0x7f) return -1;
		if (n < 1) return 0;
		d[0] = (unsigned char)u; return 1;
	case E_LATIN1:
		if (u > 0xff) return -1;
		if (n < 1) return 0;
		d[0] = (unsigned char)u; return 1;
	case E_CP1252:
		if (n < 1) return 0;
		if (u < 0x80 || (u >= 0xa0 && u <= 0xff)) { d[0] = (unsigned char)u; return 1; }
		for (i = 0; i < 32; i++)
			if (cp1252[i] == u) { d[0] = (unsigned char)(0x80 + i); return 1; }
		return -1;
	}
	return -1;
}

size_t iconv(iconv_t vcd, char **in, size_t *inleft, char **out, size_t *outleft)
{
	struct nsc_iconv *cd = vcd;
	size_t count = 0;
	if (!in || !*in)
		return 0;                      /* reset: we keep no shift state */
	while (*inleft > 0) {
		unsigned int u;
		int k = dec(cd->from, (const unsigned char *)*in, *inleft, &u);
		int w;
		if (k == 0) { errno = EINVAL; return (size_t)-1; }
		if (k < 0) { errno = EILSEQ; return (size_t)-1; }
		w = enc(cd->to, u, (unsigned char *)*out, *outleft);
		if (w < 0) {
			if (!cd->translit) { errno = EILSEQ; return (size_t)-1; }
			w = enc(cd->to, '?', (unsigned char *)*out, *outleft);
			count++;
		}
		if (w == 0) { errno = E2BIG; return (size_t)-1; }
		*in += k; *inleft -= k;
		*out += w; *outleft -= w;
	}
	return count;
}

/* ------------------------------------------------------------ maths, strto */
#include <math.h>
#include <ctype.h>

void bzero(void *p, size_t n) { memset(p, 0, n); }
#undef remove
int remove(const char *path) { return unlink(path); }

float ceilf(float f) { return (float)ceil(f); }
float strtof(const char *s, char **end) { return (float)strtod(s, end); }

/* strtoull itself comes from NetSurf's utils/utils.c */
long long strtoll(const char *s, char **end, int base)
{
	const char *p = s;
	while (isspace((unsigned char)*p)) p++;
	if (*p == '-') return -(long long)strtoull(p + 1, end, base);
	return (long long)strtoull(p, end, base);
}

/* ------------------------------------------------------------ gz files
 * NetSurf reads its Messages file through zlib's gz calls; PsiWeb ships it
 * uncompressed, so plain stdio is enough. */
#include <stdio.h>
#include <zlib.h>

gzFile gzopen(const char *path, const char *mode) { return (gzFile)fopen(path, mode); }
char *gzgets(gzFile f, char *buf, int len) { return fgets(buf, len, (FILE *)f); }
int gzclose(gzFile f) { return fclose((FILE *)f); }

/* ------------------------------------------------------------ stdio log */
#include <stdarg.h>

extern const char *pwb_res_dir(void);

#define LOG_MAX (64 * 1024)
static FILE *g_log;
static long g_log_len;
static int g_log_failed;

static int is_con(FILE *f) { return f == stdout || f == stderr; }

static FILE *log_file(void)
{
	char path[160];
	if (g_log || g_log_failed)
		return g_log;
	snprintf(path, sizeof(path), "%spsiweb.log", pwb_res_dir());
	g_log = fopen(path, "w");
	if (!g_log)
		g_log_failed = 1;
	return g_log;
}

static int log_write(const char *s, size_t n)
{
	FILE *f;
	if (g_log_len + (long)n > LOG_MAX || !(f = log_file()))
		return (int)n;
	g_log_len += (long)fwrite(s, 1, n, f);
	fflush(f);
	return (int)n;
}

int nsc_vfprintf(FILE *f, const char *fmt, va_list ap)
{
	char buf[512];
	int n;
	if (!is_con(f))
		return vfprintf(f, fmt, ap);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	log_write(buf, n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
	return n;
}

int nsc_fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = nsc_vfprintf(f, fmt, ap);
	va_end(ap);
	return n;
}

int nsc_printf(const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = nsc_vfprintf(stdout, fmt, ap);
	va_end(ap);
	return n;
}

int nsc_fputs(const char *s, FILE *f)
{
	return is_con(f) ? log_write(s, strlen(s)) : fputs(s, f);
}

int nsc_fputc(int c, FILE *f)
{
	char ch = (char)c;
	if (!is_con(f))
		return fputc(c, f);
	log_write(&ch, 1);
	return (unsigned char)c;
}

int nsc_puts(const char *s)
{
	log_write(s, strlen(s));
	return log_write("\n", 1);
}

size_t nsc_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
	if (!is_con(f))
		return fwrite(p, sz, n, f);
	log_write((const char *)p, sz * n);
	return n;
}

void nsc_perror(const char *s)
{
	nsc_fprintf(stderr, "%s: error %d\n", s ? s : "", errno);
}

int nsc_fflush(FILE *f)
{
	return (f == NULL || is_con(f)) ? 0 : fflush(f);
}

int nsc_setvbuf(FILE *f, char *buf, int mode, size_t size)
{
	(void)buf; (void)mode; (void)size;
	return is_con(f) ? 0 : 0;
}
