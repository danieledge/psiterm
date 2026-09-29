/* minimal runtime for running bench.c bare in an ARM emulator */
typedef unsigned int size_t;
static unsigned char heap[2 * 1024 * 1024];
static size_t heap_top;
void *malloc(size_t n) { void *p; n = (n + 7) & ~7u; if (heap_top + n + 8 > sizeof(heap)) return 0; *(size_t*)(heap + heap_top) = n; p = heap + heap_top + 8; heap_top += n + 8; return p; }
void free(void *p) { (void)p; }
void *calloc(size_t a, size_t b) { unsigned char *p = malloc(a * b); size_t i; if (p) for (i = 0; i < a * b; i++) p[i] = 0; return p; }
void *realloc(void *o, size_t n) { unsigned char *p = malloc(n); size_t i, old; if (!o) return p; old = *((size_t*)o - 2 + 1 - 1); if (old > n) old = n; for (i = 0; p && i < old; i++) p[i] = ((unsigned char*)o)[i]; return p; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; } return d; }
void *memset(void *d, int c, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *(unsigned char*)a - *(unsigned char*)b; }
int strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? *(unsigned char*)a - *(unsigned char*)b : 0; }
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == c) return (char*)s; return 0; }
void abort(void) { for (;;) ; }
int fprintf(void *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
void *__stderr(void) { return 0; }
int *__errno(void) { static int e; return &e; }
void bench_halt(void) { for (;;) ; }
void genrandom(unsigned char *b, unsigned int n) { unsigned int i; for (i = 0; i < n; i++) b[i] = (unsigned char)(i * 73 + 11); }
int psi_fprintf(void *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
void *m_calloc(size_t a, size_t b) { return calloc(a, b); }
void m_free_ltm(void *p, size_t n) { (void)p; (void)n; }
void *m_realloc_ltm(void *p, size_t o, size_t n) { unsigned char *q = malloc(n); size_t i; for (i = 0; p && i < o && i < n; i++) q[i] = ((unsigned char*)p)[i]; return q; }
