/* pmgfx.h - PsiMail's drawing: anti-aliased text and shapes in the Psion's
 * 16 greys (0 black .. 15 white), into a 4-bit buffer laid out like an
 * EGray16 CFbsBitmap (two pixels a byte, the left one in the low nibble).
 *
 * Plain C++ with no static data, so the same code runs in PsiMail.app (a
 * DLL: no writable statics) and in the PC tool that draws screenshots. */
#ifndef PMGFX_H
#define PMGFX_H

struct PmGlyph
	{
	unsigned char adv;
	signed char left;          /* from the pen to the bitmap's left edge */
	signed char top;           /* from the baseline up to the bitmap's top */
	unsigned char w;
	unsigned char h;
	unsigned int off;          /* into the font's data, in bytes */
	};

struct PmFont
	{
	int ascent;
	int descent;
	int height;                /* line spacing */
	int first;                 /* character of glyphs[0] */
	int count;
	const PmGlyph* glyphs;
	const unsigned char* data; /* 4-bit coverage, rows packed, low nibble first */
	};

struct PmCanvas
	{
	unsigned char* bits;
	int w, h, stride;
	int cx0, cy0, cx1, cy1;    /* clip rectangle (x1, y1 exclusive) */
	int mono;                  /* 1 = no anti-aliasing (crisper on some screens) */
	};

enum { KBlack = 0, KWhite = 15 };

void gfx_init(PmCanvas* c, unsigned char* bits, int w, int h, int stride);
void gfx_clip(PmCanvas* c, int x0, int y0, int x1, int y1);
void gfx_noclip(PmCanvas* c);
void gfx_fill(PmCanvas* c, int x, int y, int w, int h, int grey);
void gfx_pixel(PmCanvas* c, int x, int y, int grey, int alpha);
void gfx_hline(PmCanvas* c, int x, int y, int w, int grey);
void gfx_vline(PmCanvas* c, int x, int y, int h, int grey);
void gfx_dotted_hline(PmCanvas* c, int x, int y, int w, int grey);
void gfx_round(PmCanvas* c, int x, int y, int w, int h, int r, int grey);      /* filled */
void gfx_round_frame(PmCanvas* c, int x, int y, int w, int h, int r, int grey);
void gfx_circle(PmCanvas* c, int cx2, int cy2, int r2, int grey);             /* in half pixels */
int  gfx_text(PmCanvas* c, const PmFont* f, int x, int base, const char* s, int n, int grey);
int  gfx_text_width(const PmFont* f, const char* s, int n);
int  gfx_fit(const PmFont* f, const char* s, int n, int maxw);                 /* chars that fit */
int  gfx_text_clip(PmCanvas* c, const PmFont* f, int x, int base, const char* s, int n, int maxw, int grey);
int  gfx_text_right(PmCanvas* c, const PmFont* f, int right, int base, const char* s, int n, int grey);
void gfx_icon(PmCanvas* c, const PmFont* icons, int icon, int x, int y, int grey);
int  gfx_strlen(const char* s);

#endif
