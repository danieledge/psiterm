/* tutil.h - shared bits for the PsiCast codec host tests. */
#ifndef TUTIL_H
#define TUTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pccodec.h"

static unsigned int t_seed = 12345;

static unsigned int t_rand(void)
{
	/* xorshift32 */
	t_seed ^= t_seed << 13;
	t_seed ^= t_seed >> 17;
	t_seed ^= t_seed << 5;
	return t_seed;
}

static int t_range(int n)
{
	return n > 0 ? (int)(t_rand() % (unsigned int)n) : 0;
}

/* Set pixel (x, y) of a packed 4bpp frame. */
static void t_pix(unsigned char* f, int stride, int x, int y, int g)
{
	unsigned char* p = f + y * stride + (x >> 1);
	if (x & 1)
		*p = (unsigned char)((*p & 0x0f) | (g << 4));
	else
		*p = (unsigned char)((*p & 0xf0) | g);
}

static void t_fillrect(unsigned char* f, int stride, int x, int y, int w, int h, int g)
{
	int i, j;
	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			t_pix(f, stride, i, j, g);
}

/* A viewer's copy of the screen, kept only through the callbacks: proves
 * that the reported moves and rects cover every change. */
typedef struct {
	unsigned char* scr;
	const pc_dec* dec;
	int w, h, rb;
	int nrect, nscroll;
} t_viewer;

static void t_onscroll(void* aCtx, const pc_scroll* s)
{
	t_viewer* v = (t_viewer*)aCtx;
	int y, xb = s->x / 2, wb = s->w / 2;
	v->nscroll++;
	if (s->dy < 0)
		for (y = s->y0; y < s->y1; y++)
			memcpy(v->scr + (y + s->dy) * v->rb + xb, v->scr + y * v->rb + xb, wb);
	else
		for (y = s->y1 - 1; y >= s->y0; y--)
			memcpy(v->scr + (y + s->dy) * v->rb + xb, v->scr + y * v->rb + xb, wb);
}

static void t_onrect(void* aCtx, const pc_rect* r)
{
	t_viewer* v = (t_viewer*)aCtx;
	const unsigned char* src = pc_dec_screen(v->dec);
	int y;
	v->nrect++;
	if ((r->x & 1) || (r->w & 1) || r->x + r->w > v->w || r->y + r->h > v->h) {
		printf("FAIL: bad rect reported %d,%d %dx%d\n", r->x, r->y, r->w, r->h);
		exit(1);
	}
	for (y = r->y; y < r->y + r->h; y++)
		memcpy(v->scr + y * v->rb + r->x / 2, src + y * v->rb + r->x / 2, r->w / 2);
}

#endif
