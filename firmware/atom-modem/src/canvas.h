// canvas.h - draws a status-screen Layout (status.h) into a 128x128 RGB565
// frame buffer: smooth (anti-aliased) FreeSans text scaled from the 24 pt
// fonts, and the drawn icons (Wi-Fi, padlock, status badges). Portable C++:
// the LCD (board/screen.cpp) pushes the buffer to the panel in one go and
// serves it as /screen.bmp, and the host renderer (hosttest/render_screens)
// writes the same buffer to a BMP, so what is seen on the PC is what the
// screen shows, pixel for pixel. The fonts' bitmaps are M5GFX's: the board
// hands them over with SetFace (the host reads the same headers).
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_CANVAS_H
#define ATOM_CANVAS_H

#include <stdint.h>
#include <stddef.h>
#include "status.h"

namespace am {

// one glyph, laid out as Adafruit GFX's (and M5GFX's) GFXglyph
struct GlyphRec
	{
	uint32_t off;           // into the bitmap
	uint8_t w, h, adv;
	int8_t xo, yo;          // from the pen, at the baseline
	};
struct FaceData
	{
	const uint8_t* bitmap;
	const GlyphRec* glyph;
	uint16_t first, last;
	};

class Canvas
	{
public:
	static const int kSize = kScreenPx;
	// the fonts (TFace): FreeSans24pt7b and FreeSansBold24pt7b
	static void SetFace(int aFace, const FaceData& aData);
	// aFb: kSize x kSize pixels, RGB565 in the CPU's byte order, rows top down
	static void Render(const Layout& aLayout, uint16_t* aFb);
	// the buffer as a 24-bit BMP: the 54-byte header, then each row (BMP
	// rows run bottom up: aRow 0 is the screen's last row) as 3 x kSize bytes
	static const size_t kBmpHeader = 54;
	static const size_t kBmpRow = 3 * kSize;
	static const size_t kBmpSize = kBmpHeader + kBmpRow * kSize;
	static void BmpHeader(uint8_t* aOut);
	static void BmpRow(const uint16_t* aFb, int aRow, uint8_t* aOut);
	};

} // namespace am

#endif
