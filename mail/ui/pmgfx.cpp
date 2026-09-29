/* pmgfx.cpp - see pmgfx.h */
#include "pmgfx.h"

void gfx_init(PmCanvas* c, unsigned char* bits, int w, int h, int stride)
	{
	c->bits = bits;
	c->w = w;
	c->h = h;
	c->stride = stride;
	c->mono = 0;
	gfx_noclip(c);
	}

void gfx_noclip(PmCanvas* c)
	{
	c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h;
	}

void gfx_clip(PmCanvas* c, int x0, int y0, int x1, int y1)
	{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > c->w) x1 = c->w;
	if (y1 > c->h) y1 = c->h;
	c->cx0 = x0; c->cy0 = y0; c->cx1 = x1; c->cy1 = y1;
	}

int gfx_strlen(const char* s)
	{
	int n = 0;
	while (s[n]) n++;
	return n;
	}

/* dst + (fg - dst) * a / 15, without a divide (the ARM710 has none) */
static inline int blend(int dst, int fg, int a)
	{
	int v = dst * (15 - a) + fg * a;
	return (v * 273 + 2048) >> 12;
	}

static inline void put(PmCanvas* c, int x, int y, int grey)
	{
	unsigned char* p = c->bits + y * c->stride + (x >> 1);
	if (x & 1) *p = (unsigned char)((*p & 0x0f) | (grey << 4));
	else *p = (unsigned char)((*p & 0xf0) | grey);
	}

static inline int get(PmCanvas* c, int x, int y)
	{
	unsigned char v = c->bits[y * c->stride + (x >> 1)];
	return (x & 1) ? (v >> 4) : (v & 15);
	}

void gfx_pixel(PmCanvas* c, int x, int y, int grey, int alpha)
	{
	if (x < c->cx0 || x >= c->cx1 || y < c->cy0 || y >= c->cy1 || alpha <= 0)
		return;
	if (c->mono)
		{
		if (alpha >= 8) put(c, x, y, grey);
		return;
		}
	if (alpha >= 15) put(c, x, y, grey);
	else put(c, x, y, blend(get(c, x, y), grey, alpha));
	}

void gfx_fill(PmCanvas* c, int x, int y, int w, int h, int grey)
	{
	int x1 = x + w, y1 = y + h, yy;
	if (x < c->cx0) x = c->cx0;
	if (y < c->cy0) y = c->cy0;
	if (x1 > c->cx1) x1 = c->cx1;
	if (y1 > c->cy1) y1 = c->cy1;
	if (x >= x1 || y >= y1)
		return;
	unsigned char both = (unsigned char)(grey | (grey << 4));
	for (yy = y; yy < y1; yy++)
		{
		int xx = x;
		if (xx & 1) { put(c, xx, yy, grey); xx++; }
		unsigned char* p = c->bits + yy * c->stride + (xx >> 1);
		while (xx + 1 < x1) { *p++ = both; xx += 2; }
		if (xx < x1) put(c, xx, yy, grey);
		}
	}

void gfx_hline(PmCanvas* c, int x, int y, int w, int grey) { gfx_fill(c, x, y, w, 1, grey); }
void gfx_vline(PmCanvas* c, int x, int y, int h, int grey) { gfx_fill(c, x, y, 1, h, grey); }

void gfx_dotted_hline(PmCanvas* c, int x, int y, int w, int grey)
	{
	for (int i = 0; i < w; i += 2)
		gfx_pixel(c, x + i, y, grey, 15);
	}

/* coverage of a pixel by a circle of radius r (half pixels), 4x4 samples */
static int corner_cover(int px, int py, int cx2, int cy2, int r2)
	{
	int n = 0;
	for (int sy = 0; sy < 4; sy++)
		for (int sx = 0; sx < 4; sx++)
			{
			/* sample positions in 1/8 pixels, centred in the sub-cells */
			int dx = (px * 8 + sx * 2 + 1) - cx2 * 4;
			int dy = (py * 8 + sy * 2 + 1) - cy2 * 4;
			if (dx * dx + dy * dy <= r2 * r2 * 16) n++;
			}
	return n;                                  /* 0..16 */
	}

void gfx_round(PmCanvas* c, int x, int y, int w, int h, int r, int grey)
	{
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	if (r <= 0)
		{
		gfx_fill(c, x, y, w, h, grey);
		return;
		}
	gfx_fill(c, x + r, y, w - 2 * r, h, grey);
	gfx_fill(c, x, y + r, r, h - 2 * r, grey);
	gfx_fill(c, x + w - r, y + r, r, h - 2 * r, grey);
	/* the corners, anti-aliased: circle centres in half pixels */
	for (int yy = 0; yy < r; yy++)
		for (int xx = 0; xx < r; xx++)
			{
			int a = corner_cover(xx, yy, 2 * r, 2 * r, 2 * r);
			a = a >= 16 ? 15 : a;
			gfx_pixel(c, x + xx, y + yy, grey, a);
			gfx_pixel(c, x + w - 1 - xx, y + yy, grey, a);
			gfx_pixel(c, x + xx, y + h - 1 - yy, grey, a);
			gfx_pixel(c, x + w - 1 - xx, y + h - 1 - yy, grey, a);
			}
	}

void gfx_round_frame(PmCanvas* c, int x, int y, int w, int h, int r, int grey)
	{
	if (r * 2 > h) r = h / 2;
	gfx_hline(c, x + r, y, w - 2 * r, grey);
	gfx_hline(c, x + r, y + h - 1, w - 2 * r, grey);
	gfx_vline(c, x, y + r, h - 2 * r, grey);
	gfx_vline(c, x + w - 1, y + r, h - 2 * r, grey);
	for (int yy = 0; yy < r; yy++)
		for (int xx = 0; xx < r; xx++)
			{
			int outer = corner_cover(xx, yy, 2 * r, 2 * r, 2 * r);
			int inner = corner_cover(xx, yy, 2 * r, 2 * r, 2 * r - 2);
			int a = outer - inner;
			a = a >= 16 ? 15 : a;
			gfx_pixel(c, x + xx, y + yy, grey, a);
			gfx_pixel(c, x + w - 1 - xx, y + yy, grey, a);
			gfx_pixel(c, x + xx, y + h - 1 - yy, grey, a);
			gfx_pixel(c, x + w - 1 - xx, y + h - 1 - yy, grey, a);
			}
	}

void gfx_circle(PmCanvas* c, int cx2, int cy2, int r2, int grey)
	{
	int x0 = (cx2 - r2) / 2 - 1, x1 = (cx2 + r2) / 2 + 1;
	int y0 = (cy2 - r2) / 2 - 1, y1 = (cy2 + r2) / 2 + 1;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++)
			{
			int a = corner_cover(x, y, cx2, cy2, r2);
			if (a) gfx_pixel(c, x, y, grey, a >= 16 ? 15 : a);
			}
	}

static const PmGlyph* glyph(const PmFont* f, unsigned char ch)
	{
	int i = ch - f->first;
	if (i < 0 || i >= f->count)
		{
		i = '?' - f->first;
		if (i < 0 || i >= f->count) i = 0;
		}
	return &f->glyphs[i];
	}

static void draw_glyph(PmCanvas* c, const PmFont* f, const PmGlyph* g, int x, int y, int grey)
	{
	const unsigned char* d = f->data + g->off;
	int n = 0;
	for (int r = 0; r < g->h; r++)
		{
		int yy = y + r;
		for (int k = 0; k < g->w; k++, n++)
			{
			int a = (n & 1) ? (d[n >> 1] >> 4) : (d[n >> 1] & 15);
			if (a && yy >= c->cy0 && yy < c->cy1)
				gfx_pixel(c, x + k, yy, grey, a);
			}
		}
	}

int gfx_text(PmCanvas* c, const PmFont* f, int x, int base, const char* s, int n, int grey)
	{
	int x0 = x;
	for (int i = 0; i < n; i++)
		{
		const PmGlyph* g = glyph(f, (unsigned char)s[i]);
		if (x + g->left + g->w > c->cx0 && x + g->left < c->cx1)
			draw_glyph(c, f, g, x + g->left, base - g->top, grey);
		x += g->adv;
		}
	return x - x0;
	}

int gfx_text_width(const PmFont* f, const char* s, int n)
	{
	int w = 0;
	for (int i = 0; i < n; i++)
		w += glyph(f, (unsigned char)s[i])->adv;
	return w;
	}

int gfx_fit(const PmFont* f, const char* s, int n, int maxw)
	{
	int w = 0;
	for (int i = 0; i < n; i++)
		{
		w += glyph(f, (unsigned char)s[i])->adv;
		if (w > maxw) return i;
		}
	return n;
	}

/* text cut to maxw with an ellipsis (cp1252 0x85) if it doesn't fit */
int gfx_text_clip(PmCanvas* c, const PmFont* f, int x, int base, const char* s, int n, int maxw, int grey)
	{
	if (maxw <= 0)
		return 0;
	if (gfx_text_width(f, s, n) <= maxw)
		return gfx_text(c, f, x, base, s, n, grey);
	const char ell = (char)0x85;
	int ew = glyph(f, 0x85)->adv;
	int k = gfx_fit(f, s, n, maxw - ew);
	while (k > 0 && s[k - 1] == ' ') k--;
	int w = gfx_text(c, f, x, base, s, k, grey);
	return w + gfx_text(c, f, x + w, base, &ell, 1, grey);
	}

int gfx_text_right(PmCanvas* c, const PmFont* f, int right, int base, const char* s, int n, int grey)
	{
	int w = gfx_text_width(f, s, n);
	gfx_text(c, f, right - w, base, s, n, grey);
	return w;
	}

/* icons are glyphs of an icon font; y is the top of the icon's box */
void gfx_icon(PmCanvas* c, const PmFont* icons, int icon, int x, int y, int grey)
	{
	if (icon < 0 || icon >= icons->count)
		return;
	const PmGlyph* g = &icons->glyphs[icon];
	draw_glyph(c, icons, g, x + g->left, y + icons->ascent - g->top, grey);
	}
