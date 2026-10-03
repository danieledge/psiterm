/* psi_mem.c - heap accounting for the PC build of Links for PsiWeb
 *
 * Replaces malloc and friends for the whole process (Links, libpng, libjpeg,
 * zlib and OpenSSL alike) and keeps the live and peak totals. The figure is
 * the usable size of each block plus 4 bytes, which on a 32-bit glibc
 * (chunks rounded to 8 with a 4-byte header) is within a few bytes per block
 * of what EPOC's RHeap (4-byte header, 4-byte granularity) would take.
 *
 * PSI_MEM_BIG=<bytes> logs every block at least that big.
 * PSI_HEAP_LIMIT=<bytes> makes allocations fail above that total, to mimic
 * the 10 MB heap psiweb.exe gets on the Psion.
 *
 * On the Psion this file is not used: RHeap::Size()/Count() give the same
 * numbers.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <malloc.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);
extern void *__libc_memalign(size_t, size_t);

static size_t live, peak, page_peak, blocks, peak_blocks, limit, fails;
static int limit_read;

#define COST(p) (malloc_usable_size(p) + 4)

static int over(size_t n)
{
	if (!limit_read) {
		const char *s = getenv("PSI_HEAP_LIMIT");
		limit_read = 1;
		limit = s ? strtoul(s, NULL, 0) : 0;
	}
	if (limit && live + n + 4 > limit) {
		fails++;
		return 1;
	}
	return 0;
}

static size_t big = (size_t)-1;

static void add(void *p)
{
	if (!p) return;
	if (big == (size_t)-1) {
		const char *s = getenv("PSI_MEM_BIG");
		big = s ? strtoul(s, NULL, 0) : 0;
	}
	if (big && malloc_usable_size(p) >= big) {
		char m[96];
		int n = snprintf(m, sizeof(m), "[mem] big block %lu KB, live %lu KB\n",
			(unsigned long)(malloc_usable_size(p) >> 10), (unsigned long)(live >> 10));
		fwrite(m, 1, n, stderr);   /* (fprintf may allocate) */
	}
	live += COST(p);
	blocks++;
	if (live > peak) peak = live, peak_blocks = blocks;
	if (live > page_peak) page_peak = live;
}

static void sub(void *p)
{
	if (!p) return;
	live -= COST(p);
	blocks--;
}

void *malloc(size_t n)
{
	void *p;
	if (over(n)) { errno = ENOMEM; return NULL; }
	p = __libc_malloc(n);
	add(p);
	return p;
}

void *calloc(size_t a, size_t b)
{
	void *p;
	if (b && a > (size_t)-1 / b) { errno = ENOMEM; return NULL; }
	if (over(a * b)) { errno = ENOMEM; return NULL; }
	p = __libc_calloc(a, b);
	add(p);
	return p;
}

void *realloc(void *o, size_t n)
{
	void *p;
	size_t oc = o ? COST(o) : 0;
	if (!o) return malloc(n);
	if (!n) { free(o); return NULL; }
	if (n + 4 > oc && over(n + 4 - oc)) { errno = ENOMEM; return NULL; }
	p = __libc_realloc(o, n);
	if (p) {
		live -= oc; blocks--;
		add(p);
	}
	return p;
}

void free(void *p)
{
	sub(p);
	__libc_free(p);
}

void *memalign(size_t a, size_t n)
{
	void *p;
	if (over(n)) { errno = ENOMEM; return NULL; }
	p = __libc_memalign(a, n);
	add(p);
	return p;
}

void *aligned_alloc(size_t a, size_t n) { return memalign(a, n); }

int posix_memalign(void **r, size_t a, size_t n)
{
	void *p = memalign(a, n);
	if (!p) return ENOMEM;
	*r = p;
	return 0;
}

void *valloc(size_t n) { return memalign(4096, n); }
void *pvalloc(size_t n) { return memalign(4096, (n + 4095) & ~(size_t)4095); }

/* called by psi_drv.c */
void psi_mem_page_start(void)
{
	page_peak = live;
}

void psi_mem_report(const char *what)
{
	fprintf(stderr, "[mem] %s: live %lu KB (%lu blocks), page peak %lu KB, overall peak %lu KB (%lu blocks)%s\n",
		what ? what : "", (unsigned long)(live >> 10), (unsigned long)blocks,
		(unsigned long)(page_peak >> 10), (unsigned long)(peak >> 10),
		(unsigned long)peak_blocks, fails ? " - allocations refused" : "");
	if (fails) fprintf(stderr, "[mem] %lu allocations refused by PSI_HEAP_LIMIT\n", (unsigned long)fails);
}

/* bytes the heap may still grow by (psi_drv.c: the soft ceiling); with no
   PSI_HEAP_LIMIT, as if the limit were the Psion's 10 MB */
long psi_heap_room(void)
{
	over(0);
	return (long)(limit ? limit : 10 * 1024 * 1024) - (long)live;
}
