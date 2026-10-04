/* links_rt.c - a small C library so the Psion-compiled Links PsiWeb objects
 * (built by the 1999 EPOC GCC) can run bare in an ARM emulator on a PC -
 * see run_links.py. A copy of web/emu/emu_rt.c (NetSurf's), with:
 *   - a heap limit (default 10 MB, as psiweb.exe gets on the Psion): malloc
 *     returns NULL above it, so Links' own out-of-memory paths are tested;
 *   - the heap counted as EPOC's RHeap would (size + 4, rounded to 4), live
 *     and peak, overall and per page (psi_mem_page_start/psi_mem_report);
 *   - the extra C library Links, libpng and libjpeg need.
 * NetSurf's notes follow. Pure functions are real C, compiled by the same
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
	HC_FTELL, HC_LOG, HC_PAGEMEM = 40
};

/* ------------------------------------------------------------- memory
 * Size-class free lists over one arena; big blocks are first-fit. */
#define ARENA (96 * 1024 * 1024)
static unsigned char arena[ARENA];
static size_t top;
static size_t in_use, peak, nallocs;
/* RHeap-equivalent accounting; the harness may poke emu_heap_limit */
size_t emu_heap_limit = 10 * 1024 * 1024;
size_t rh_live, rh_peak, rh_page_peak, rh_fails;	/* (the harness reads these) */
#define RH(n) (((n) + 4 + 3) & ~3u)
static void *bins[32];
typedef struct big { size_t size; struct big *next; } big;
static big *bigfree;

static int bin_of(size_t n) { int b = 3; while (((size_t)1 << b) < n) b++; return b; }

void *malloc(size_t n)
{
	size_t *h;
	int b;
	if (n == 0) n = 1;
	if (rh_live + RH(n) > emu_heap_limit) { rh_fails++; return 0; }
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
	rh_live += RH(n);
	if (rh_live > rh_peak) rh_peak = rh_live;
	if (rh_live > rh_page_peak) rh_page_peak = rh_live;
	return h + 2;
}

void free(void *p)
{
	size_t *h;
	int b;
	if (!p) return;
	h = (size_t *)p - 2;
	in_use -= h[0];
	rh_live -= RH(h[1]);
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

/* ESTLIB's memcpy/memset/memmove are EUSER's Mem::Copy/Fill, hand-written
 * ARM that moves words (ldm/stm) when it can: these word loops come close,
 * where NetSurf's byte loops would overstate the cost several times */
void *memset(void *d, int c, size_t n)
{
	unsigned char *a = d;
	unsigned int w = (unsigned char)c;
	while (n && ((unsigned int)a & 3)) { *a++ = (unsigned char)c; n--; }
	w |= w << 8; w |= w << 16;
	while (n >= 16) { unsigned int *p = (unsigned int *)a; p[0] = w; p[1] = w; p[2] = w; p[3] = w; a += 16; n -= 16; }
	while (n >= 4) { *(unsigned int *)a = w; a += 4; n -= 4; }
	while (n--) *a++ = (unsigned char)c;
	return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *a = d;
	const unsigned char *b = s;
	if ((((unsigned int)a ^ (unsigned int)b) & 3) == 0) {
		while (n && ((unsigned int)a & 3)) { *a++ = *b++; n--; }
		while (n >= 16) {
			unsigned int *p = (unsigned int *)a; const unsigned int *q = (const unsigned int *)b;
			unsigned int x0 = q[0], x1 = q[1], x2 = q[2], x3 = q[3];
			p[0] = x0; p[1] = x1; p[2] = x2; p[3] = x3;
			a += 16; b += 16; n -= 16;
		}
		while (n >= 4) { *(unsigned int *)a = *(const unsigned int *)b; a += 4; b += 4; n -= 4; }
	} else if ((((unsigned int)a ^ (unsigned int)b) & 1) == 0) {
		while (n && ((unsigned int)a & 1)) { *a++ = *b++; n--; }
		while (n >= 2) { *(unsigned short *)a = *(const unsigned short *)b; a += 2; b += 2; n -= 2; }
	}
	while (n--) *a++ = *b++;
	return d;
}
void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *a = d;
	const unsigned char *b = s;
	if (a <= b || a >= b + n) return memcpy(d, s, n);
	a += n; b += n;
	if ((((unsigned int)a ^ (unsigned int)b) & 3) == 0) {
		while (n && ((unsigned int)a & 3)) { *--a = *--b; n--; }
		while (n >= 4) { a -= 4; b -= 4; *(unsigned int *)a = *(const unsigned int *)b; n -= 4; }
	} else if ((((unsigned int)a ^ (unsigned int)b) & 1) == 0) {
		while (n && ((unsigned int)a & 1)) { *--a = *--b; n--; }
		while (n >= 2) { a -= 2; b -= 2; *(unsigned short *)a = *(const unsigned short *)b; n -= 2; }
	}
	while (n--) *--a = *--b;
	return d;
}
int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
void *memchr(const void *s, int c, size_t n) { const unsigned char *p = s; for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p; return 0; }
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void *realloc(void *o, size_t n)
{
	void *p;
	size_t old;
	if (!o) return malloc(n);
	old = ((size_t *)o)[-1];
	if (n <= ((size_t *)o)[-2] - 8) {
		if (rh_live - RH(old) + RH(n) > emu_heap_limit) { rh_fails++; return 0; }
		rh_live = rh_live - RH(old) + RH(n);
		if (rh_live > rh_peak) rh_peak = rh_live;
		if (rh_live > rh_page_peak) rh_page_peak = rh_live;
		((size_t *)o)[-1] = n;
		return o;
	}
	p = malloc(n);
	if (!p) return 0;
	memcpy(p, o, old < n ? old : n);
	free(o);
	return p;
}
void emu_mem_report(void) { emu_hc(HC_MEM, (int)rh_live, (int)rh_peak, (int)top, (int)nallocs); }
/* psi_drv.c: a page starts loading / has loaded */
void psi_mem_page_start(void) { rh_page_peak = rh_live; }
/* bytes the heap may still grow by (psi_drv.c: the soft ceiling) */
long psi_heap_room(void) { return (long)emu_heap_limit - (long)rh_live; }
void psi_mem_report(const char *what) { emu_hc(HC_PAGEMEM, (int)what, (int)rh_live, (int)rh_page_peak, (int)rh_fails); }

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
/* open() for writing only (PsiWeb's Save to file), through fopen's
   hypercall; descriptors 30 to 37 (Links checks them against FD_SETSIZE,
   and psi_os.c's own start at 40) */
static int wfd[8];
int open(const char *p, int fl, ...) { int h, i; if (!(fl & 3)) { err = 2; return -1; } for (i = 0; i < 8 && wfd[i]; i++) ; if (i == 8) { err = 24; return -1; } h = emu_hc(HC_FOPEN, (int)p, (int)"wb", 0, 0); if (h < 0) { err = 2; return -1; } wfd[i] = h + 1; return 30 + i; }
int read(int fd, void *b, size_t n) { (void)fd; (void)b; (void)n; return -1; }
int write(int fd, const void *b, size_t n) { if (fd >= 30 && fd < 38 && wfd[fd - 30]) { int r = emu_hc(HC_FWRITE, wfd[fd - 30] - 1, (int)b, n, 0); return r < 0 ? -1 : r; } (void)b; return n; }
int close(int fd) { if (fd >= 30 && fd < 38 && wfd[fd - 30]) emu_hc(HC_FCLOSE, wfd[fd - 30] - 1, 0, 0, 0), wfd[fd - 30] = 0; return 0; }
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

/* ------------------------------------------------------------- Links' extras
 * On the Psion these come from ESTLIB (estlib.lib). The maths that Links
 * uses in quantity (pow, for gamma tables) is real C here, compiled for the
 * ARM with soft float, so its cost shows in the instruction counts as it
 * would on the Psion; it is not handed to the PC as sin/cos/sqrt are. */
typedef union { double d; unsigned int w[2]; } dw;	/* w[0]: high word (FPA order) */

double frexp(double x, int *e)
{
	dw u;
	int ex;
	u.d = x;
	if (x == 0) { *e = 0; return x; }
	ex = (u.w[0] >> 20) & 0x7ff;
	if (ex == 0) {
		u.d = x * 18014398509481984.0;		/* 2^54 */
		ex = (int)((u.w[0] >> 20) & 0x7ff) - 54;
	}
	*e = ex - 1022;
	u.w[0] = (u.w[0] & 0x800fffffu) | (1022u << 20);
	return u.d;
}

double ldexp(double x, int e)
{
	dw u;
	int ex;
	u.d = x;
	ex = (u.w[0] >> 20) & 0x7ff;
	if (x == 0 || ex == 0 || ex + e <= 0 || ex + e >= 0x7ff) {	/* the rare cases, slowly */
		while (e > 0) { x *= 2; e--; }
		while (e < 0) { x *= 0.5; e++; }
		return x;
	}
	u.w[0] = (u.w[0] & 0x800fffffu) | ((unsigned)(ex + e) << 20);
	return u.d;
}

double modf(double x, double *ip)
{
	double i = x < 0 ? -floor(-x) : floor(x);
	*ip = i;
	return x - i;
}

#define LN2 0.69314718055994530942
/* close to fdlibm's cost (what ESTLIB's libm is): one division and about
 * 20 multiply-adds each, no division in the loops */
static const double inv_odd[] = { 1.0, 1/3., 1/5., 1/7., 1/9., 1/11., 1/13., 1/15., 1/17., 1/19., 1/21., 1/23. };
static const double inv_fact[] = { 1.0, 1.0, 1/2., 1/6., 1/24., 1/120., 1/720., 1/5040., 1/40320., 1/362880.,
	1/3628800., 1/39916800., 1/479001600., 1/6227020800., 1/87178291200. };
double log(double x)
{
	int e, i;
	double m, s, s2, sum;
	if (x <= 0) return -1e308;
	m = frexp(x, &e);			/* x = m * 2^e, m in [0.5, 1) */
	if (m < 0.70710678118654752440) { m *= 2; e--; }
	s = (m - 1) / (m + 1);			/* |s| < 0.172 */
	s2 = s * s;
	sum = inv_odd[11];
	for (i = 10; i >= 0; i--) sum = sum * s2 + inv_odd[i];
	return 2 * s * sum + e * LN2;
}

double exp(double x)
{
	int k, i;
	double r, sum;
	if (x > 709) return 1e308;
	if (x < -745) return 0;
	k = (int)(x * (1 / LN2) + (x < 0 ? -0.5 : 0.5));
	r = x - k * LN2;			/* |r| <= ln2/2 */
	sum = inv_fact[14];
	for (i = 13; i >= 0; i--) sum = sum * r + inv_fact[i];
	return ldexp(sum, k);
}

double pow(double x, double y)
{
	double ip;
	if (y == 0) return 1;
	if (y == 1) return x;
	if (x == 0) return 0;
	if (x < 0) {
		if (modf(y, &ip) != 0) return 0;
		return ((long long)ip & 1) ? -exp(y * log(-x)) : exp(y * log(-x));
	}
	return exp(y * log(x));
}

unsigned short htons(unsigned short v) { return (unsigned short)((v >> 8) | (v << 8)); }
unsigned short ntohs(unsigned short v) { return htons(v); }
unsigned long htonl(unsigned long v) { return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }
unsigned long ntohl(unsigned long v) { return htonl(v); }
int puts(const char *s) { fputs(s, &f_out); fputc('\n', &f_out); return 0; }
int getpid(void) { return 1; }
int kill(int p, int s) { (void)p; (void)s; err = 1; return -1; }
void _exit(int c) { exit(c); }
int fsync(int fd) { (void)fd; return 0; }
int ftruncate(int fd, long l) { (void)fd; (void)l; return -1; }
int dup2(int a, int b) { (void)a; (void)b; return -1; }
int chdir(const char *p) { (void)p; return 0; }
char *getcwd(char *b, size_t n) { if (b && n > 3) { strcpy(b, "C:\\"); return b; } return 0; }
int system(const char *c) { (void)c; return -1; }
void *signal(int s, void *h) { (void)s; (void)h; return 0; }
int readlink(const char *p, char *b, size_t n) { (void)p; (void)b; (void)n; return -1; }
int lstat(const char *p, void *st) { (void)p; (void)st; err = 2; return -1; }
int fstat(int fd, void *st) { (void)fd; (void)st; err = 9; return -1; }
int bind(int s, const void *a, int l) { (void)s; (void)a; (void)l; return -1; }
int listen(int s, int n) { (void)s; (void)n; return -1; }
int accept(int s, void *a, void *l) { (void)s; (void)a; (void)l; return -1; }
int connect(int s, const void *a, int l) { (void)s; (void)a; (void)l; return -1; }
int getsockname(int s, void *a, void *l) { (void)s; (void)a; (void)l; return -1; }
int getsockopt(int s, int lv, int o, void *v, void *l) { (void)s; (void)lv; (void)o; (void)v; (void)l; return -1; }
void *gethostbyname(const char *n) { (void)n; return 0; }
int execvp(const char *f, char *const a[]) { (void)f; (void)a; return -1; }
