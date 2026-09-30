/* pmedit.cpp - a text editor drawn in PsiMail's own type (see pmui.h)
 *
 * One line (To, Subject...) or many (a message), word-wrapped, with a
 * caret. The wrapped lines are kept as a list of start offsets; an edit
 * lays out again only from the changed line until the old lines line up
 * again, so typing into a long reply stays quick on the Psion. */
#include "pmui.h"

static int grow_text(PmEditor* e, int need)
	{
	if (need <= e->cap) return 1;
	int cap = e->cap ? e->cap : 64;
	while (cap < need) cap *= 2;
	char* t = (char*)ui_alloc(cap);
	if (!t) return 0;
	for (int i = 0; i < e->len; i++) t[i] = e->text[i];
	ui_free(e->text);
	e->text = t;
	e->cap = cap;
	return 1;
	}

static int grow_lines(PmEditor* e, int need)
	{
	if (need <= e->lcap) return 1;
	int cap = e->lcap ? e->lcap * 2 : 32;
	while (cap < need) cap *= 2;
	int* l = (int*)ui_alloc(cap * (int)sizeof(int));
	if (!l) return 0;
	for (int i = 0; i < e->lcap; i++) l[i] = e->lines[i];   /* (all: a layout may be under way) */
	ui_free(e->lines);
	e->lines = l;
	e->lcap = cap;
	return 1;
	}

void ed_init(PmEditor* e, int single, int max, const PmFont* f, int width, int lineH)
	{
	char* p = (char*)e;
	for (unsigned int i = 0; i < sizeof(*e); i++) p[i] = 0;
	e->single = single;
	e->max = max;
	e->font = f;
	e->width = width;
	e->lineH = lineH;
	e->wantX = -1;
	grow_text(e, 64);
	grow_lines(e, 4);
	e->nlines = 1;
	e->lines[0] = 0;
	}

void ed_free(PmEditor* e)
	{
	ui_free(e->text);
	ui_free(e->lines);
	e->text = 0;
	e->lines = 0;
	e->len = e->cap = e->nlines = e->lcap = 0;
	}

/* where the line starting at 'from' ends: the next line's start */
static int line_break(const PmEditor* e, int from)
	{
	if (e->single) return e->len + 1;
	int w = 0, lastSpace = -1;
	for (int i = from; i < e->len; i++)
		{
		unsigned char c = (unsigned char)e->text[i];
		if (c == '\n') return i + 1;
		w += gfx_text_width(e->font, (const char*)&e->text[i], 1);
		if (c == ' ') lastSpace = i;
		if (w > e->width && i > from)
			return lastSpace >= from ? lastSpace + 1 : i;
		}
	return e->len + 1;                 /* the last line */
	}

/* lays out again from line 'l'; old line starts after 'edit' are 'delta'
   further on now, and once one matches we can stop */
static void relayout(PmEditor* e, int l, int edit, int delta)
	{
	if (l < 0) l = 0;
	if (l >= e->nlines) l = e->nlines - 1;
	/* the old starts, shifted, to compare with */
	int oldn = e->nlines;
	int* old = (int*)ui_alloc((oldn + 1) * (int)sizeof(int));
	if (old)
		for (int i = 0; i < oldn; i++) old[i] = e->lines[i] > edit ? e->lines[i] + delta : e->lines[i];
	int n = l + 1;
	int s = e->lines[l];
	int j = l + 1;                     /* where to look in old */
	for (;;)
		{
		int nx = line_break(e, s);
		if (nx > e->len) break;        /* s is the last line */
		/* in step with the old layout again? */
		while (old && j < oldn && old[j] < nx) j++;
		if (old && nx > edit + (delta > 0 ? delta : 0) && j < oldn && old[j] == nx)
			{
			int rest = oldn - j;
			if (!grow_lines(e, n + rest)) break;
			for (int k = 0; k < rest; k++) e->lines[n + k] = old[j + k];
			n += rest;
			ui_free(old);
			e->nlines = n;
			return;
			}
		if (!grow_lines(e, n + 1)) break;
		e->lines[n++] = nx;
		s = nx;
		}
	ui_free(old);
	e->nlines = n;
	}

void ed_layout(PmEditor* e, int width)
	{
	e->width = width;
	e->nlines = 1;
	e->lines[0] = 0;
	relayout(e, 0, e->len, 0);
	}

int ed_line_of(const PmEditor* e, int pos)
	{
	int lo = 0, hi = e->nlines - 1;
	while (lo < hi)
		{
		int mid = (lo + hi + 1) / 2;
		if (e->lines[mid] <= pos) lo = mid; else hi = mid - 1;
		}
	return lo;
	}

static int line_end(const PmEditor* e, int l)
	{
	int end = l + 1 < e->nlines ? e->lines[l + 1] : e->len;
	/* not the '\n' or the space the line was broken at */
	if (end > e->lines[l] && l + 1 < e->nlines && (e->text[end - 1] == '\n' || e->text[end - 1] == ' ')) end--;
	return end;
	}

static int x_of(const PmEditor* e, int pos)
	{
	int l = ed_line_of(e, pos);
	return gfx_text_width(e->font, e->text + e->lines[l], pos - e->lines[l]);
	}

/* the position on line l nearest to x pixels */
static int pos_at(const PmEditor* e, int l, int x)
	{
	int s = e->lines[l], end = line_end(e, l), w = 0;
	for (int i = s; i < end; i++)
		{
		int cw = gfx_text_width(e->font, e->text + i, 1);
		if (w + cw / 2 >= x) return i;
		w += cw;
		}
	return end;
	}

int ed_set(PmEditor* e, const char* s, int n)
	{
	if (n > e->max) n = e->max;
	if (!grow_text(e, n + 1)) return 0;
	for (int i = 0; i < n; i++)
		{
		char c = s[i];
		if (e->single && (c == '\n' || c == '\r')) c = ' ';
		e->text[i] = c;
		}
	e->len = n;
	e->cur = 0;
	e->top = 0;
	e->wantX = -1;
	ed_layout(e, e->width);
	return 1;
	}

static void insert(PmEditor* e, const char* s, int n)
	{
	if (e->len + n > e->max) n = e->max - e->len;
	if (n <= 0 || !grow_text(e, e->len + n + 1)) return;
	for (int i = e->len - 1; i >= e->cur; i--) e->text[i + n] = e->text[i];
	for (int i = 0; i < n; i++) e->text[e->cur + i] = s[i];
	e->len += n;
	int at = e->cur;
	e->cur += n;
	relayout(e, ed_line_of(e, at) - 1, at, n);
	e->wantX = -1;
	}

static void remove(PmEditor* e, int at, int n)
	{
	if (at < 0 || n <= 0 || at + n > e->len) return;
	for (int i = at; i + n < e->len; i++) e->text[i] = e->text[i + n];
	e->len -= n;
	e->cur = at;
	relayout(e, ed_line_of(e, at) - 1, at, -n);
	e->wantX = -1;
	}

int ed_char(PmEditor* e, int ch)
	{
	char c = (char)ch;
	if (ch == '\n' && e->single) return 0;
	insert(e, &c, 1);
	return 1;
	}

int ed_insert(PmEditor* e, const char* s, int n)
	{
	insert(e, s, n);
	return 1;
	}

int ed_key(PmEditor* e, int key, int page)
	{
	int l = ed_line_of(e, e->cur);
	switch (key)
		{
	case EdLeft:
		if (e->cur == 0) return 0;
		e->cur--;
		e->wantX = -1;
		return 1;
	case EdRight:
		if (e->cur >= e->len) return 0;
		e->cur++;
		e->wantX = -1;
		return 1;
	case EdHome:
		e->cur = e->lines[l];
		e->wantX = -1;
		return 1;
	case EdEnd:
		e->cur = line_end(e, l);
		e->wantX = -1;
		return 1;
	case EdDocStart: e->cur = 0; e->wantX = -1; return 1;
	case EdDocEnd: e->cur = e->len; e->wantX = -1; return 1;
	case EdUp:
	case EdDown:
	case EdPgUp:
	case EdPgDn:
		{
		int step = key == EdUp ? -1 : key == EdDown ? 1 : key == EdPgUp ? -page : page;
		int nl = l + step;
		if (nl < 0) { if (l == 0) return 0; nl = 0; }
		if (nl >= e->nlines) { if (l == e->nlines - 1) return 0; nl = e->nlines - 1; }
		if (e->wantX < 0) e->wantX = x_of(e, e->cur);
		e->cur = pos_at(e, nl, e->wantX);
		return 1;
		}
	case EdBack:
		if (e->cur == 0) return 0;
		remove(e, e->cur - 1, 1);
		return 1;
	case EdDel:
		if (e->cur >= e->len) return 0;
		remove(e, e->cur, 1);
		return 1;
	case EdEnter:
		return ed_char(e, '\n');
		}
	return 0;
	}

int ed_on_first_line(const PmEditor* e) { return ed_line_of(e, e->cur) == 0; }
int ed_on_last_line(const PmEditor* e) { return ed_line_of(e, e->cur) == e->nlines - 1; }

/* keeps the caret on screen: 'rows' lines fit */
static void scroll_to_caret(PmEditor* e, int rows, int w)
	{
	if (e->single)
		{
		int cx = x_of(e, e->cur);
		if (cx - e->top > w - 4) e->top = cx - w + 20;
		if (cx < e->top) e->top = cx - 20;
		if (e->top < 0) e->top = 0;
		return;
		}
	int l = ed_line_of(e, e->cur);
	if (l < e->top) e->top = l;
	if (l >= e->top + rows) e->top = l - rows + 1;
	if (e->top < 0) e->top = 0;
	}

void ed_draw(PmCanvas* c, PmEditor* e, int x, int y, int h, int grey, int focus)
	{
	int rows = e->single ? 1 : h / e->lineH;
	if (rows < 1) rows = 1;
	scroll_to_caret(e, rows, e->width);
	int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
	gfx_clip(c, x > cx0 ? x : cx0, y > cy0 ? y : cy0, x + e->width + 2 < cx1 ? x + e->width + 2 : cx1, y + h < cy1 ? y + h : cy1);
	int base = e->font->ascent + (e->lineH - e->font->ascent - e->font->descent) / 2;
	int caretL = ed_line_of(e, e->cur);
	for (int i = 0; i < rows && e->top + i < e->nlines; i++)
		{
		int l = (e->single ? 0 : e->top) + i;
		int s = e->lines[l], end = line_end(e, l);
		int ly = y + i * e->lineH;
		int xs = x - (e->single ? e->top : 0);
		gfx_text(c, e->font, xs, ly + base, e->text + s, end - s, grey);
		if (focus && l == caretL)
			{
			int px = xs + gfx_text_width(e->font, e->text + s, e->cur - s);
			gfx_fill(c, px, ly + 2, 2, e->lineH - 4, 1);
			}
		}
	gfx_clip(c, cx0, cy0, cx1, cy1);
	}

void ed_click(PmEditor* e, int x, int y)
	{
	int l = e->single ? 0 : e->top + y / e->lineH;
	if (l >= e->nlines) l = e->nlines - 1;
	if (l < 0) l = 0;
	e->cur = pos_at(e, l, x + (e->single ? e->top : 0));
	e->wantX = -1;
	}

int ed_height(const PmEditor* e) { return e->nlines * e->lineH; }
