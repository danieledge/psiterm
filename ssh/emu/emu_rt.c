/* emu_rt.c - minimal C runtime so the Psion-compiled psissh objects can run
 * bare in an ARM emulator. Pure functions are real C (compiled by the same
 * 1999 Psion GCC); anything needing the outside world is a "hypercall" into
 * the Python harness, which also plays the role of PsiTerm + psiglue.cpp. */

typedef unsigned int size_t;
#include <stdarg.h>

/* the harness hooks this address (kept in its own file so it is never
   inlined): r0=op, r1..r3 args, [sp] 4th arg */
int emu_hc(int op, int a, int b, int c, int d);

enum {
	HC_EXIT = 1, HC_TIME, HC_GETTIMEOFDAY, HC_FOPEN, HC_FCLOSE, HC_FREAD,
	HC_FWRITE, HC_FSEEK, HC_FGETC, HC_MKDIR, HC_GETENV, HC_SETENV,
	HC_PG = 100
};

/* ------------------------------------------------------------- memory */
static unsigned char heap[8 * 1024 * 1024];
static size_t heap_top;
void *malloc(size_t n) {
	size_t *h;
	n = (n + 7) & ~7u;
	if (heap_top + n + 8 > sizeof(heap)) { emu_hc(HC_EXIT, 250, 0, 0, 0); return 0; }
	h = (size_t*)(heap + heap_top);
	h[0] = n;
	heap_top += n + 8;
	return h + 2;
}
void free(void *p) { (void)p; }
void *memset(void *d, int c, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; } return d; }
int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void *realloc(void *o, size_t n) {
	void *p = malloc(n); size_t old;
	if (!o || !p) return p;
	old = ((size_t*)o)[-2];
	memcpy(p, o, old < n ? old : n);
	return p;
}

/* ------------------------------------------------------------- strings */
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *(unsigned char*)a - *(unsigned char*)b; }
int strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? *(unsigned char*)a - *(unsigned char*)b : 0; }
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int strncasecmp(const char *a, const char *b, size_t n) { while (n && *a && lower(*a) == lower(*b)) { a++; b++; n--; } return n ? lower(*(unsigned char*)a) - lower(*(unsigned char*)b) : 0; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char*)s; if (!*s) return 0; } }
char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char*)r; } }
unsigned long strtoul(const char *s, char **end, int base) {
	unsigned long v = 0; int d;
	while (*s == ' ') s++;
	if (base == 0) base = 10;
	for (;; s++) {
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (lower(*s) >= 'a' && lower(*s) <= 'z') d = lower(*s) - 'a' + 10;
		else break;
		if (d >= base) break;
		v = v * base + d;
	}
	if (end) *end = (char*)s;
	return v;
}
long strtol(const char *s, char **end, int base) { int neg = 0; while (*s == ' ') s++; if (*s == '-') { neg = 1; s++; } { long v = (long)strtoul(s, end, base); return neg ? -v : v; } }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
unsigned int htonl(unsigned int x) { return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24); }
unsigned short htons(unsigned short x) { return (unsigned short)((x >> 8) | (x << 8)); }
char *strerror(int e) { (void)e; return "error"; }
static int err;
int *__errno(void) { return &err; }

/* ------------------------------------------------------------- printf */
static char *fmt_num(char *o, unsigned long long v, int base, int upper, int neg, int width, int zero, int left, int prec)
{
	char tmp[32]; int n = 0, len, pad;
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	if (v == 0 && prec != 0) tmp[n++] = '0';
	while (v) { tmp[n++] = dig[v % base]; v /= base; }
	while (n < prec) tmp[n++] = '0';
	len = n + (neg ? 1 : 0);
	pad = width > len ? width - len : 0;
	if (!left && !zero) while (pad-- > 0) *o++ = ' ';
	if (neg) *o++ = '-';
	if (!left && zero) while (pad-- > 0) *o++ = '0';
	while (n) *o++ = tmp[--n];
	if (left) while (pad-- > 0) *o++ = ' ';
	return o;
}

int vsprintf(char *out, const char *f, va_list ap)
{
	char *o = out;
	for (; *f; f++) {
		int width = 0, prec = -1, zero = 0, left = 0, lng = 0;
		if (*f != '%') { *o++ = *f; continue; }
		f++;
		for (;; f++) { if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+' || *f == ' ' || *f == '#') ; else break; }
		if (*f == '*') { width = va_arg(ap, int); f++; } else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
		if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = va_arg(ap, int); f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
		while (*f == 'l' || *f == 'h' || *f == 'z') { if (*f == 'l') lng++; f++; }
		switch (*f) {
		case 'd': case 'i': {
			long long v = lng >= 2 ? va_arg(ap, long long) : va_arg(ap, int);
			o = fmt_num(o, v < 0 ? -v : v, 10, 0, v < 0, width, zero, left, prec); break; }
		case 'u': { unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int); o = fmt_num(o, v, 10, 0, 0, width, zero, left, prec); break; }
		case 'x': case 'X': case 'p': { unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int); o = fmt_num(o, v, 16, *f == 'X', 0, width, zero, left, prec); break; }
		case 'o': { unsigned int v = va_arg(ap, unsigned int); o = fmt_num(o, v, 8, 0, 0, width, zero, left, prec); break; }
		case 'c': *o++ = (char)va_arg(ap, int); break;
		case 's': {
			const char *s = va_arg(ap, const char*); int n, pad;
			if (!s) s = "(null)";
			n = strlen(s); if (prec >= 0 && n > prec) n = prec;
			pad = width > n ? width - n : 0;
			if (!left) while (pad-- > 0) *o++ = ' ';
			memcpy(o, s, n); o += n;
			if (left) while (pad-- > 0) *o++ = ' ';
			break; }
		case '%': *o++ = '%'; break;
		default: *o++ = '%'; *o++ = *f; break;
		}
	}
	*o = 0;
	return o - out;
}
int sprintf(char *out, const char *f, ...) { va_list ap; int n; va_start(ap, f); n = vsprintf(out, f, ap); va_end(ap); return n; }

/* ------------------------------------------------------------- stdio */
typedef struct { int fd; } FILE;
static FILE f_in = { -1 }, f_out = { -2 }, f_err = { -3 };
FILE *__stdin(void) { return &f_in; }
FILE *__stdout(void) { return &f_out; }
FILE *__stderr(void) { return &f_err; }
FILE *fopen(const char *p, const char *m) { int h = emu_hc(HC_FOPEN, (int)p, (int)m, 0, 0); FILE *f; if (h < 0) return 0; f = malloc(sizeof(FILE)); f->fd = h; return f; }
int fclose(FILE *f) { return emu_hc(HC_FCLOSE, f->fd, 0, 0, 0); }
size_t fread(void *b, size_t s, size_t n, FILE *f) { int r = emu_hc(HC_FREAD, f->fd, (int)b, s * n, 0); return r < 0 ? 0 : r / s; }
size_t fwrite(const void *b, size_t s, size_t n, FILE *f) { int r = emu_hc(HC_FWRITE, f->fd, (int)b, s * n, 0); return r < 0 ? 0 : r / s; }
int fseek(FILE *f, long off, int wh) { return emu_hc(HC_FSEEK, f->fd, off, wh, 0); }
int fgetc(FILE *f) { return emu_hc(HC_FGETC, f->fd, 0, 0, 0); }
int getc(FILE *f) { return fgetc(f); }
char *fgets(char *b, int n, FILE *f) { int i = 0, c; while (i < n - 1 && (c = fgetc(f)) >= 0) { b[i++] = (char)c; if (c == '\n') break; } b[i] = 0; return i ? b : 0; }
int fputs(const char *s, FILE *f) { return fwrite(s, 1, strlen(s), f); }
int fputc(int c, FILE *f) { char ch = (char)c; fwrite(&ch, 1, 1, f); return c; }
int fflush(FILE *f) { (void)f; return 0; }
int fileno(FILE *f) { return f->fd; }
int fsync(int fd) { (void)fd; return 0; }
int vfprintf(FILE *f, const char *fmt, va_list ap) { char buf[2048]; int n = vsprintf(buf, fmt, ap); fwrite(buf, 1, n, f); return n; }
struct stat;
int fstat(int fd, struct stat *st) { (void)fd; (void)st; return -1; }
int mkdir(const char *p, int m) { (void)m; return emu_hc(HC_MKDIR, (int)p, 0, 0, 0); }
int chdir(const char *p) { (void)p; return 0; }
int open(const char *p, int fl, ...) { (void)p; (void)fl; err = 2; return -1; }
int read(int fd, void *b, size_t n) { (void)fd; (void)b; (void)n; return -1; }
int write(int fd, const void *b, size_t n) { (void)fd; (void)b; return n; }
int close(int fd) { (void)fd; return 0; }
char *getenv(const char *n) { return (char*)emu_hc(HC_GETENV, (int)n, 0, 0, 0); }
int setenv(const char *n, const char *v, int o) { (void)o; return emu_hc(HC_SETENV, (int)n, (int)v, 0, 0); }

/* ------------------------------------------------------------- process/time */
void exit(int c) { emu_hc(HC_EXIT, c, 0, 0, 0); for (;;) ; }
void _exit(int c) { exit(c); }
void abort(void) { exit(134); }
int atexit(void (*f)(void)) { (void)f; return 0; }
int getpid(void) { return 42; }
struct timeval { long tv_sec, tv_usec; };
int gettimeofday(struct timeval *tv, void *tz) { (void)tz; emu_hc(HC_GETTIMEOFDAY, (int)tv, 0, 0, 0); return 0; }
long clock(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_usec; }
long time(long *t) { struct timeval tv; gettimeofday(&tv, 0); if (t) *t = tv.tv_sec; return tv.tv_sec; }

/* ------------------------------------------------------------- sockets (unused) */
int socket(int a, int b, int c) { (void)a; (void)b; (void)c; err = 97; return -1; }
int connect() { return -1; } int bind() { return -1; } int listen() { return -1; }
int getsockopt() { return -1; } int setsockopt() { return -1; }
int getpeername() { return -1; } int getsockname() { return -1; }
void *gethostbyname() { return 0; } void *gethostbyaddr() { return 0; }
int inet_aton() { return 0; } char *inet_ntoa() { return "0.0.0.0"; }
int __EH_FRAME_BEGIN__;

/* ------------------------------------------------------------- psiglue (pg_*) */
/* Implemented by the harness: each is a hypercall HC_PG + index */
#define PG(n) (HC_PG + (n))
void *pg_shared(void) { return (void*)emu_hc(PG(0), 0, 0, 0, 0); }
int pg_init(void) { return emu_hc(PG(1), 0, 0, 0, 0); }
void pg_close(void) { emu_hc(PG(2), 0, 0, 0, 0); }
void pg_set_state(int s) { emu_hc(PG(3), s, 0, 0, 0); }
void pg_dial_verbose(int on) { (void)on; }
void pg_set_exit(int c) { emu_hc(PG(4), c, 0, 0, 0); }
int pg_quit_requested(void) { return emu_hc(PG(5), 0, 0, 0, 0); }
void pg_msleep(int ms) { emu_hc(PG(6), ms, 0, 0, 0); }
int pg_serial_write(const void *b, int n) { return emu_hc(PG(7), (int)b, n, 0, 0); }
int pg_net_avail(void) { return emu_hc(PG(8), 0, 0, 0, 0); }
int pg_net_read(void *b, int m) { return emu_hc(PG(9), (int)b, m, 0, 0); }
int pg_net_closed(void) { return 0; }
void pg_net_set_closed(void) { emu_hc(PG(10), 0, 0, 0, 0); }
int pg_kbd_avail(void) { return emu_hc(PG(11), 0, 0, 0, 0); }
int pg_kbd_read(void *b, int m) { return emu_hc(PG(12), (int)b, m, 0, 0); }
void pg_out_write(const void *b, int n) { emu_hc(PG(13), (int)b, n, 0, 0); }
void pg_winsize(int *r, int *c) { emu_hc(PG(14), (int)r, (int)c, 0, 0); }
int pg_take_resize(void) { return emu_hc(PG(15), 0, 0, 0, 0); }
int pg_wait(int ms, int n, int k) { return emu_hc(PG(16), ms, n, k, 0); }
int pg_dial(char *why, int max) { return emu_hc(PG(17), (int)why, max, 0, 0); }
void pg_hangup(void) { emu_hc(PG(18), 0, 0, 0, 0); }
int pg_entropy(unsigned char *o, int m) { return emu_hc(PG(19), (int)o, m, 0, 0); }
const char *pg_home(void) { return (const char*)emu_hc(PG(20), 0, 0, 0, 0); }
