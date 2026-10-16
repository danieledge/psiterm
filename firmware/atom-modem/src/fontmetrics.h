// fontmetrics.h - made by hosttest/mkfontmetrics.py from M5GFX's GFXFF fonts
// (FreeFont, GNU FreeFont licence; only the metrics are here). Do not edit.
#ifndef ATOM_FONTMETRICS_H
#define ATOM_FONTMETRICS_H

#include <stdint.h>

namespace am {

struct FontMetric
	{
	uint8_t advance[95];     // 0x20..0x7e: the pen moves this far
	uint8_t ascent;          // the tallest glyph above the baseline
	uint8_t descent;         // the deepest below it
	uint8_t cap;             // the height of "H": what the eye reads as the size
	};

// FreeSans24pt7b
static const FontMetric kFreeSans24pt7bMetric = { {
	12, 16, 16, 26, 26, 42, 31,  9, 16, 16, 18, 27, 13, 16, 12, 13, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 12, 12, 27, 27, 27, 26, 48, 31, 31, 33, 33, 30,
	28, 36, 34, 13, 25, 32, 26, 40, 34, 37, 31, 37, 33, 31, 30, 34, 30, 44, 31,
	32, 29, 13, 13, 13, 22, 26, 12, 26, 26, 24, 26, 25, 13, 26, 25, 10, 11, 24,
	10, 38, 25, 25, 26, 26, 16, 23, 13, 25, 23, 34, 22, 22, 23, 16, 12, 16, 24,
	}, 34, 11, 33 };
// FreeSansBold24pt7b
static const FontMetric kFreeSansBold24pt7bMetric = { {
	13, 16, 22, 26, 26, 42, 34, 12, 16, 16, 18, 27, 12, 16, 12, 13, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 12, 12, 27, 27, 27, 29, 46, 33, 33, 34, 34, 31,
	30, 36, 35, 15, 27, 34, 29, 41, 35, 37, 32, 37, 34, 32, 30, 35, 31, 45, 32,
	30, 29, 16, 13, 16, 27, 26, 12, 27, 29, 26, 29, 27, 16, 29, 28, 13, 13, 27,
	13, 42, 29, 29, 29, 29, 18, 26, 16, 29, 25, 37, 26, 26, 24, 18, 13, 18, 23,
	}, 35, 12, 33 };
// FreeMonoBold24pt7b
static const FontMetric kFreeMonoBold24pt7bMetric = { {
	28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
	28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
	28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
	28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
	28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
	}, 32, 10, 26 };

} // namespace am

#endif
