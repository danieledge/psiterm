/* armrt.c - the C library calls the encoder and zlib make, for the ARM
 * benchmark only (no EPOC underneath). memcpy and memset copy words when
 * they can, roughly as ESTLIB's do; malloc is a bump allocator that
 * records the peak. */
#include <stddef.h>

#define HEAP (600 * 1024)
static unsigned int heap[HEAP / 4];
unsigned int rt_used, rt_peak;

void* memcpy(void* d, const void* s, size_t n)
{
	unsigned char* dp = (unsigned char*)d;
	const unsigned char* sp = (const unsigned char*)s;
	if (!(((unsigned long)dp | (unsigned long)sp) & 3)) {
		while (n >= 16) {
			unsigned int a = ((const unsigned int*)sp)[0], b = ((const unsigned int*)sp)[1];
			unsigned int c = ((const unsigned int*)sp)[2], e = ((const unsigned int*)sp)[3];
			((unsigned int*)dp)[0] = a;
			((unsigned int*)dp)[1] = b;
			((unsigned int*)dp)[2] = c;
			((unsigned int*)dp)[3] = e;
			dp += 16;
			sp += 16;
			n -= 16;
		}
		while (n >= 4) {
			*(unsigned int*)dp = *(const unsigned int*)sp;
			dp += 4;
			sp += 4;
			n -= 4;
		}
	}
	while (n--)
		*dp++ = *sp++;
	return d;
}

void* memmove(void* d, const void* s, size_t n)
{
	unsigned char* dp = (unsigned char*)d;
	const unsigned char* sp = (const unsigned char*)s;
	if (dp <= sp || dp >= sp + n)
		return memcpy(d, s, n);
	while (n--)
		dp[n] = sp[n];
	return d;
}

void* memset(void* d, int c, size_t n)
{
	unsigned char* dp = (unsigned char*)d;
	unsigned int w = (c & 0xff) * 0x01010101u;
	while (n && ((unsigned long)dp & 3)) {
		*dp++ = (unsigned char)c;
		n--;
	}
	while (n >= 16) {
		((unsigned int*)dp)[0] = w;
		((unsigned int*)dp)[1] = w;
		((unsigned int*)dp)[2] = w;
		((unsigned int*)dp)[3] = w;
		dp += 16;
		n -= 16;
	}
	while (n--)
		*dp++ = (unsigned char)c;
	return d;
}

int memcmp(const void* a, const void* b, size_t n)
{
	const unsigned char* p = (const unsigned char*)a;
	const unsigned char* q = (const unsigned char*)b;
	if (!(((unsigned long)p | (unsigned long)q) & 3))
		while (n >= 4 && *(const unsigned int*)p == *(const unsigned int*)q) {
			p += 4;
			q += 4;
			n -= 4;
		}
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

void* malloc(size_t n)
{
	void* p;
	n = (n + 7) & ~7u;
	if (rt_used + n > HEAP)
		return 0;
	p = (unsigned char*)heap + rt_used;
	rt_used += n;
	if (rt_used > rt_peak)
		rt_peak = rt_used;
	return p;
}

void* calloc(size_t a, size_t b)
{
	void* p = malloc(a * b);
	if (p)
		memset(p, 0, a * b);
	return p;
}

void free(void* p)
{
	(void)p;
}

size_t strlen(const char* s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}
