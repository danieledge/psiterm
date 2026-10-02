/* emu_rt.c - a small C library so the Psion-compiled psiweb objects (NetSurf
 * and all, built by the 1999 EPOC GCC) can run bare in an ARM emulator on a
 * PC - see run_psiweb.py. Pure functions are real C, compiled by the same
 * Psion GCC; anything that needs the outside world (files, time, maths
 * library, the network) is a hypercall into the Python harness, in the style
 * of PsiTerm's ssh/emu. The allocator keeps statistics so a run also shows
 * how much memory a page needs. */

typedef unsigned int size_t;
#include <stdarg.h>

int emu_hc(int op, int a, int b, int c, int d);

enum {
	HC_EXIT = 1, HC_TIME, HC_GETTIMEOFDAY, HC_FOPEN, HC_FCLOSE, HC_FREAD,
	HC_FWRITE, HC_FSEEK, HC_FGETC, HC_MKDIR, HC_GETENV, HC_SETENV,
	HC_MATH = 20, HC_STRTOD, HC_LOCALTIME, HC_MKTIME, HC_STRFTIME, HC_MEM,
	HC_FTELL, HC_LOG
};

/* ------------------------------------------------------------- memory
 * Size-class free lists over one arena; big blocks are first-fit. */
#define ARENA (48 * 1024 * 1024)
static unsigned char arena[ARENA];
static size_t top;
static size_t in_use, peak, nallocs;
static void *bins[32];
typedef struct big { size_t size; struct big *next; } big;
static big *bigfree;

static int bin_of(size_t n) { int b = 3; while (((size_t)1 << b) < n) b++; return b; }

void *malloc(size_t n)
{
	size_t *h;
	int b;
	if (n == 0) n = 1;
	b = bin_of(n + 8);
	if (b <= 12) {
		size_t sz = (size_t)1 << b;
		if (bins[b]) {
			h = bins[b];
			bins[b] = *(void **)(h + 2);
		} else {
			if (top + sz > ARENA) { emu_hc(HC_EXIT, 251, 0, 0, 0); return 0; }
			h = (size_t *)(arena + top);
			top += sz;
		}
		h[0] = sz; h[1] = n;
	} else {
		big **pp, *p;
		size_t sz = (n + 8 + 15) & ~15u;
		for (pp = &bigfree; (p = *pp); pp = &p->next)
			if (p->size >= sz) { *pp = p->next; break; }
		if (p) { h = (size_t *)p; sz = p->size; }
		else {
			if (top + sz > ARENA) { emu_hc(HC_EXIT, 251, 0, 0, 0); return 0; }
			h = (size_t *)(arena + top);
			top += sz;
		}
		h[0] = sz; h[1] = n;
	}
	in_use += h[0];
	nallocs++;
	if (in_use > peak) peak = in_use;
	return h + 2;
}

void free(void *p)
{
	size_t *h;
	int b;
	if (!p) return;
	h = (size_t *)p - 2;
	in_use -= h[0];
	b = bin_of(h[0]);
	if (h[0] <= 4096 && ((size_t)1 << b) == h[0]) {
		*(void **)p = bins[b];
		bins[b] = h;
	} else {
		big *g = (big *)h;
		g->size = h[0];
		g->next = bigfree;
		bigfree = g;
	}
}

void *memset(void *d, int c, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; } return d; }
int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
void *memchr(const void *s, int c, size_t n) { const unsigned char *p = s; for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p; return 0; }
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void *realloc(void *o, size_t n)
{
	void *p;
	size_t old;
	if (!o) return malloc(n);
	old = ((size_t *)o)[-1];
	if (n <= ((size_t *)o)[-2] - 8) { ((size_t *)o)[-1] = n; return o; }
	p = malloc(n);
	if (!p) return 0;
	memcpy(p, o, old < n ? old : n);
	free(o);
	return p;
}
void emu_mem_report(void) { emu_hc(HC_MEM, (int)in_use, (int)peak, (int)top, (int)nallocs); }

/* ------------------------------------------------------------- strings */
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *(unsigned char *)a - *(unsigned char *)b; }
int strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? *(unsigned char *)a - *(unsigned char *)b : 0; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int strcasecmp(const char *a, const char *b) { while (*a && tolower(*a) == tolower(*b)) { a++; b++; } return tolower(*(unsigned char *)a) - tolower(*(unsigned char *)b); }
int strncasecmp(const char *a, const char *b, size_t n) { while (n && *a && tolower(*a) == tolower(*b)) { a++; b++; n--; } return n ? tolower(*(unsigned char *)a) - tolower(*(unsigned char *)b) : 0; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n) { char *r = d; while (n && *s) { *d++ = *s++; n--; } while (n--) *d++ = 0; return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
char *strstr(const char *h, const char *n) { size_t l = strlen(n); for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h; return l ? 0 : (char *)h; }
char *strdup(const char *s) { char *d = malloc(strlen(s) + 1); if (d) strcpy(d, s); return d; }
size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }
char *strpbrk(const char *s, const char *a) { for (; *s; s++) if (strchr(a, *s)) return (char *)s; return 0; }
static char *tok_save;
char *strtok(char *s, const char *d)
{
	char *e;
	if (!s) s = tok_save;
	if (!s) return 0;
	s += strspn(s, d);
	if (!*s) { tok_save = 0; return 0; }
	e = s + strcspn(s, d);
	if (*e) { *e = 0; tok_save = e + 1; } else tok_save = 0;
	return s;
}
unsigned long strtoul(const char *s, char **end, int base)
{
	unsigned long v = 0; int d;
	while (isspace(*s)) s++;
	if (*s == '+') s++;
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
	if (base == 0) base = (*s == '0') ? 8 : 10;
	for (;; s++) {
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (tolower(*s) >= 'a' && tolower(*s) <= 'z') d = tolower(*s) - 'a' + 10;
		else break;
		if (d >= base) break;
		v = v * base + d;
	}
	if (end) *end = (char *)s;
	return v;
}
long strtol(const char *s, char **end, int base) { int neg = 0; while (isspace(*s)) s++; if (*s == '-') { neg = 1; s++; } { long v = (long)strtoul(s, end, base); return neg ? -v : v; } }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
int abs(int x) { return x < 0 ? -x : x; }
static int err;
int *__errno(void) { return &err; }
char *strerror(int e) { (void)e; return "error"; }
static unsigned long rnd = 1;
int rand(void) { rnd = rnd * 1103515245 + 12345; return (int)((rnd >> 16) & 0x7fff); }
char *setlocale(int c, const char *l) { (void)c; (void)l; return "C"; }

/* ------------------------------------------------------------- sort */
static void swapb(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
	/* insertion sort: small arrays only in NetSurf */
	size_t i, j;
	char *b = base;
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--)
			swapb(b + (j - 1) * sz, b + j * sz, sz);
}
void *bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		const char *p = (const char *)base + mid * sz;
		int c = cmp(key, p);
		if (c == 0) return (void *)p;
		if (c < 0) hi = mid; else lo = mid + 1;
	}
	return 0;
}

/* ------------------------------------------------------------- maths
 * doubles travel as two words; the harness knows the word order */
typedef union { double d; int w[2]; } dbl;
static double m1(int op, double x) { dbl a, r; a.d = x; r.w[0] = emu_hc(HC_MATH, op, a.w[0], a.w[1], (int)&r); return r.d; }
double sin(double x) { return m1(1, x); }
double cos(double x) { return m1(2, x); }
double ceil(double x) { return m1(3, x); }
double floor(double x) { return m1(4, x); }
double fabs(double x) { return x < 0 ? -x : x; }
double sqrt(double x) { return m1(5, x); }
double strtod(const char *s, char **end)
{
	dbl r;
	int used = emu_hc(HC_STRTOD, (int)s, (int)&r, 0, 0);
	if (end) *end = (char *)s + used;
	return r.d;
}
double atof(const char *s) { return strtod(s, 0); }

/* ------------------------------------------------------------- sscanf (%d %i %u %x %s %c %n) */
int sscanf(const char *str, const char *fmt, ...)
{
	va_list ap;
	int n = 0;
	const char *s = str;
	va_start(ap, fmt);
	for (; *fmt; fmt++) {
		int width = 0;
		if (isspace(*fmt)) { while (isspace(*s)) s++; continue; }
		if (*fmt != '%') { if (*s != *fmt) break; s++; continue; }
		fmt++;
		while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l' || *fmt == 'h') fmt++;
		if (*fmt == 'n') { *va_arg(ap, int *) = s - str; continue; }
		if (*fmt != 'c') while (isspace(*s)) s++;
		if (!*s) break;
		if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x' || *fmt == 'X') {
			char *e;
			long v = (*fmt == 'x' || *fmt == 'X') ? (long)strtoul(s, &e, 16) : strtol(s, &e, *fmt == 'i' ? 0 : 10);
			if (e == s) break;
			*va_arg(ap, int *) = (int)v;
			s = e; n++;
		} else if (*fmt == 's') {
			char *o = va_arg(ap, char *);
			while (*s && !isspace(*s) && (!width || width--)) *o++ = *s++;
			*o = 0; n++;
		} else if (*fmt == 'c') {
			*va_arg(ap, char *) = *s++; n++;
		} else break;
	}
	va_end(ap);
	return n;
}

/* ------------------------------------------------------------- printf */
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int vsprintf(char *out, const char *f, va_list ap) { return vsnprintf(out, 0x7fffffff, f, ap); }
int sprintf(char *out, const char *f, ...) { va_list ap; int n; va_start(ap, f); n = vsprintf(out, f, ap); va_end(ap); return n; }

/* ------------------------------------------------------------- stdio */
typedef struct { int fd; int eof; } FILE;
static FILE f_in = { -1, 0 }, f_out = { -2, 0 }, f_err = { -3, 0 };
FILE *__stdin(void) { return &f_in; }
FILE *__stdout(void) { return &f_out; }
FILE *__stderr(void) { return &f_err; }
FILE *fopen(const char *p, const char *m) { int h = emu_hc(HC_FOPEN, (int)p, (int)m, 0, 0); FILE *f; if (h < 0) { err = 2; return 0; } f = malloc(sizeof(FILE)); f->fd = h; f->eof = 0; return f; }
FILE *fdopen(int fd, const char *m) { (void)fd; (void)m; return 0; }
int fclose(FILE *f) { int r = emu_hc(HC_FCLOSE, f->fd, 0, 0, 0); free(f); return r; }
size_t fread(void *b, size_t s, size_t n, FILE *f) { int r = emu_hc(HC_FREAD, f->fd, (int)b, s * n, 0); if (r <= 0) { f->eof = 1; return 0; } return r / s; }
size_t fwrite(const void *b, size_t s, size_t n, FILE *f) { int r = emu_hc(HC_FWRITE, f->fd, (int)b, s * n, 0); return r < 0 ? 0 : r / s; }
int fseek(FILE *f, long off, int wh) { f->eof = 0; return emu_hc(HC_FSEEK, f->fd, off, wh, 0); }
long ftell(FILE *f) { return emu_hc(HC_FTELL, f->fd, 0, 0, 0); }
int fgetc(FILE *f) { int c = emu_hc(HC_FGETC, f->fd, 0, 0, 0); if (c < 0) f->eof = 1; return c; }
int getc(FILE *f) { return fgetc(f); }
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { (void)f; return 0; }
void clearerr(FILE *f) { f->eof = 0; }
char *fgets(char *b, int n, FILE *f) { int i = 0, c; while (i < n - 1 && (c = fgetc(f)) >= 0) { b[i++] = (char)c; if (c == '\n') break; } b[i] = 0; return i ? b : 0; }
int fputs(const char *s, FILE *f) { return fwrite(s, 1, strlen(s), f); }
int fputc(int c, FILE *f) { char ch = (char)c; fwrite(&ch, 1, 1, f); return c; }
int fflush(FILE *f) { (void)f; return 0; }
int vfprintf(FILE *f, const char *fmt, va_list ap) { char buf[1024]; int n = vsnprintf(buf, sizeof(buf), fmt, ap); fwrite(buf, 1, n < 1023 ? n : 1023, f); return n; }
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; int n; va_start(ap, fmt); n = vfprintf(f, fmt, ap); va_end(ap); return n; }
struct stat;
int stat(const char *p, struct stat *st) { (void)p; (void)st; err = 2; return -1; }
int access(const char *p, int m) { (void)p; (void)m; return -1; }
int mkdir(const char *p, int m) { (void)m; return emu_hc(HC_MKDIR, (int)p, 0, 0, 0); }
int rmdir(const char *p) { (void)p; return -1; }
int unlink(const char *p) { (void)p; return -1; }
int rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
void *opendir(const char *p) { (void)p; return 0; }
void *readdir(void *d) { (void)d; return 0; }
int closedir(void *d) { (void)d; return 0; }
int open(const char *p, int fl, ...) { (void)p; (void)fl; err = 2; return -1; }
int read(int fd, void *b, size_t n) { (void)fd; (void)b; (void)n; return -1; }
int write(int fd, const void *b, size_t n) { (void)fd; (void)b; return n; }
int close(int fd) { (void)fd; return 0; }
long lseek(int fd, long o, int w) { (void)fd; (void)o; (void)w; return -1; }
int dup(int fd) { return fd; }
char *getenv(const char *n) { return (char *)emu_hc(HC_GETENV, (int)n, 0, 0, 0); }
int socket(int a, int b, int c) { (void)a; (void)b; (void)c; err = 97; return -1; }

/* ------------------------------------------------------------- process/time */
void exit(int c) { emu_mem_report(); emu_hc(HC_EXIT, c, 0, 0, 0); for (;;) ; }
void abort(void) { exit(134); }
int atexit(void (*f)(void)) { (void)f; return 0; }
int _epoc32_atexit(void (*f)(void)) { (void)f; return 0; }
struct timeval { long tv_sec, tv_usec; };
int gettimeofday(struct timeval *tv, void *tz) { (void)tz; emu_hc(HC_GETTIMEOFDAY, (int)tv, 0, 0, 0); return 0; }
int pwb_gettimeofday(struct timeval *tv, void *tz) { return gettimeofday(tv, tz); }
long time(long *t) { struct timeval tv; gettimeofday(&tv, 0); if (t) *t = tv.tv_sec; return tv.tv_sec; }
static int tmbuf[11];
void *localtime(const long *t) { emu_hc(HC_LOCALTIME, (int)*t, (int)tmbuf, 0, 0); return tmbuf; }
void *gmtime(const long *t) { emu_hc(HC_LOCALTIME, (int)*t, (int)tmbuf, 1, 0); return tmbuf; }
long mktime(void *tm) { return emu_hc(HC_MKTIME, (int)tm, 0, 0, 0); }
size_t strftime(char *s, size_t max, const char *fmt, const void *tm) { return emu_hc(HC_STRFTIME, (int)s, max, (int)fmt, (int)tm); }
int __EH_FRAME_BEGIN__;
