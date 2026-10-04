/* psi_str.c - string functions for Links on the Psion (ARM710, no cache
 * worth speaking of, 36 MHz). psicompat.h maps Links' calls to these.
 *
 * Links spends a large part of a page in strlen, strchr, memchr and
 * strcspn: the HTML parser scans the whole document for '<' several times,
 * and parse_url runs strcspn over every URL, including long data: URLs.
 * Plain byte loops cost 4 to 5 instructions a byte, and a strcspn that
 * calls strchr for each byte costs 4 more for each byte of the set. Here:
 *   - strlen, strchr and memchr look at a word (4 bytes) at a time, about
 *     1.5 instructions a byte;
 *   - strcspn and strspn use a 256-bit table, a few instructions a byte
 *     whatever the size of the set.
 * Reads never go past the aligned word holding the last byte looked at,
 * so they cannot cross into an unmapped page. */
#include <stddef.h>
#ifndef NULL
#define NULL ((void *)0)
#endif

typedef unsigned long w32;
#define ONES	0x01010101UL
#define HIGHS	0x80808080UL
#define HASZERO(v) (((v) - ones) & ~(v) & highs)
/* gcc 3.0 would build both constants afresh in every turn of a loop (8
   instructions a word): this makes it keep them in registers */
#define CONSTS w32 ones = ONES, highs = HIGHS; __asm__("" : "=r"(ones) : "0"(ones)); __asm__("" : "=r"(highs) : "0"(highs))

size_t psi_strlen(const char *s)
{
	const char *p = s;
	const w32 *w;
	w32 v;
	CONSTS;

	for (; (w32)p & 3; p++)
		if (!*p) return (size_t)(p - s);
	w = (const w32 *)p;
	for (;;) {
		v = *w;
		if (HASZERO(v)) break;
		w++;
	}
	for (p = (const char *)w; *p; p++)
		;
	return (size_t)(p - s);
}

void *psi_memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	unsigned char ch = (unsigned char)c;
	w32 m, v;
	CONSTS;

	for (; n && ((w32)p & 3); p++, n--)
		if (*p == ch) return (void *)p;
	m = ch * ONES;
	for (; n >= 4; p += 4, n -= 4) {
		v = *(const w32 *)p ^ m;
		if (HASZERO(v)) break;
	}
	for (; n; p++, n--)
		if (*p == ch) return (void *)p;
	return NULL;
}

char *psi_strchr(const char *s, int c)
{
	const unsigned char *p = (const unsigned char *)s;
	unsigned char ch = (unsigned char)c;
	const w32 *w;
	w32 m, v, x;
	CONSTS;

	for (; (w32)p & 3; p++) {
		if (*p == ch) return (char *)p;
		if (!*p) return NULL;
	}
	m = ch * ONES;
	w = (const w32 *)p;
	for (;;) {
		v = *w;
		x = v ^ m;
		if (HASZERO(v) | HASZERO(x)) break;
		w++;
	}
	for (p = (const unsigned char *)w; ; p++) {
		if (*p == ch) return (char *)p;
		if (!*p) return NULL;
	}
}

size_t psi_strcspn(const char *s, const char *reject)
{
	w32 set[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };	/* (the 0 byte always stops) */
	const unsigned char *r = (const unsigned char *)reject, *p = (const unsigned char *)s;
	unsigned c;

	if (!r[0]) return psi_strlen(s);
	for (; *r; r++) set[*r >> 5] |= 1UL << (*r & 31);
	for (;; p++) {
		c = *p;
		if (set[c >> 5] & (1UL << (c & 31))) break;
	}
	return (size_t)((const char *)p - s);
}

size_t psi_strspn(const char *s, const char *accept)
{
	w32 set[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	const unsigned char *a = (const unsigned char *)accept, *p = (const unsigned char *)s;
	unsigned c;

	for (; *a; a++) set[*a >> 5] |= 1UL << (*a & 31);
	for (;; p++) {
		c = *p;
		if (!(set[c >> 5] & (1UL << (c & 31)))) break;
	}
	return (size_t)((const char *)p - s);
}

/* strstr: find the first character with strchr, then compare (the C
 * library's compared the whole needle at every position) */
char *psi_strstr(const char *h, const char *n)
{
	const char *a, *b;
	char c0 = n[0];

	if (!c0) return (char *)h;
	for (;; h++) {
		h = psi_strchr(h, c0);
		if (!h) return NULL;
		for (a = h + 1, b = n + 1; *b && *a == *b; a++, b++)
			;
		if (!*b) return (char *)h;
	}
}
