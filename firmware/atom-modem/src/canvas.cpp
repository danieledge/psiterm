// canvas.cpp - see canvas.h. MIT licence (see LICENSE at the top of the repository).
#include "canvas.h"
#include "fontmetrics.h"
#include <math.h>
#include <string.h>

namespace am {

namespace {

FaceData gFace[EFaceCount];

const int N = Canvas::kSize;
const int kSub = 4;                     // 4x4 samples a pixel: 16 levels of coverage

// colour aC over the pixel at (aX, aY), aCov of 16 covered
void Blend(uint16_t* aFb, int aX, int aY, uint16_t aC, int aCov)
	{
	if (aX < 0 || aY < 0 || aX >= N || aY >= N || aCov <= 0)
		return;
	uint16_t& d = aFb[aY * N + aX];
	if (aCov >= kSub * kSub)
		{
		d = aC;
		return;
		}
	int r1 = (aC >> 11) & 31, g1 = (aC >> 5) & 63, b1 = aC & 31;
	int r0 = (d >> 11) & 31, g0 = (d >> 5) & 63, b0 = d & 31;
	const int k = kSub * kSub;
	int r = (r1 * aCov + r0 * (k - aCov) + k / 2) / k;
	int g = (g1 * aCov + g0 * (k - aCov) + k / 2) / k;
	int b = (b1 * aCov + b0 * (k - aCov) + k / 2) / k;
	d = (uint16_t)((r << 11) | (g << 5) | b);
	}

// a shape given by an inside test at a point, drawn with coverage over the
// box (aX0, aY0)-(aX1, aY1)
template <class F> void Shape(uint16_t* aFb, float aX0, float aY0, float aX1, float aY1, uint16_t aC, F aInside)
	{
	int x0 = (int)floorf(aX0), y0 = (int)floorf(aY0), x1 = (int)ceilf(aX1), y1 = (int)ceilf(aY1);
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++)
			{
			int cov = 0;
			for (int sy = 0; sy < kSub; sy++)
				for (int sx = 0; sx < kSub; sx++)
					if (aInside(x + (sx + 0.5f) / kSub, y + (sy + 0.5f) / kSub))
						cov++;
			Blend(aFb, x, y, aC, cov);
			}
	}

float SegDist(float aPx, float aPy, float aAx, float aAy, float aBx, float aBy)
	{
	float vx = aBx - aAx, vy = aBy - aAy;
	float t = ((aPx - aAx) * vx + (aPy - aAy) * vy) / (vx * vx + vy * vy);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	float dx = aPx - (aAx + t * vx), dy = aPy - (aAy + t * vy);
	return sqrtf(dx * dx + dy * dy);
	}

void Circle(uint16_t* aFb, float aCx, float aCy, float aR, uint16_t aC)
	{
	Shape(aFb, aCx - aR, aCy - aR, aCx + aR, aCy + aR, aC,
		[&](float x, float y) { return (x - aCx) * (x - aCx) + (y - aCy) * (y - aCy) <= aR * aR; });
	}

void Line(uint16_t* aFb, float aAx, float aAy, float aBx, float aBy, float aW, uint16_t aC)
	{
	float h = aW / 2;
	Shape(aFb, fminf(aAx, aBx) - h, fminf(aAy, aBy) - h, fmaxf(aAx, aBx) + h, fmaxf(aAy, aBy) + h, aC,
		[&](float x, float y) { return SegDist(x, y, aAx, aAy, aBx, aBy) <= h; });
	}

// the Wi-Fi fan: a dot and three arcs over it, aLit of the arcs bright
void Wifi(uint16_t* aFb, float aCx, float aBase, float aSize, int aLit, uint16_t aOn, uint16_t aOff)
	{
	float cy = aBase - aSize * 0.08f;           // the fan's centre, at the dot
	Circle(aFb, aCx, cy - aSize * 0.04f, aSize * 0.11f, aLit >= 0 ? aOn : aOff);
	float t = aSize * 0.12f;
	for (int i = 0; i < 3; i++)
		{
		float r = aSize * (0.36f + 0.27f * i);
		Shape(aFb, aCx - r - t, cy - r - t, aCx + r + t, cy, i < aLit ? aOn : aOff, [&](float x, float y)
			{
			float dx = x - aCx, dy = y - cy;
			float d = sqrtf(dx * dx + dy * dy);
			return dy < 0 && fabsf(dx) <= -dy * 1.0f && d >= r - t / 2 && d <= r + t / 2;
			});
		}
	}

void Badge(uint16_t* aFb, float aCx, float aCy, float aR, int aKind, uint16_t aC)
	{
	Circle(aFb, aCx, aCy, aR, aC);
	const uint16_t k = 0;                       // the mark: black, cut out of the badge
	float w = aR * 0.28f;
	switch (aKind)
		{
	case EBadgeCheck:
		Line(aFb, aCx - aR * 0.45f, aCy + aR * 0.02f, aCx - aR * 0.12f, aCy + aR * 0.38f, w, k);
		Line(aFb, aCx - aR * 0.12f, aCy + aR * 0.38f, aCx + aR * 0.48f, aCy - aR * 0.35f, w, k);
		break;
	case EBadgeCross:
		Line(aFb, aCx - aR * 0.38f, aCy - aR * 0.38f, aCx + aR * 0.38f, aCy + aR * 0.38f, w, k);
		Line(aFb, aCx - aR * 0.38f, aCy + aR * 0.38f, aCx + aR * 0.38f, aCy - aR * 0.38f, w, k);
		break;
	case EBadgeDots:
		for (int i = -1; i <= 1; i++)
			Circle(aFb, aCx + i * aR * 0.45f, aCy, aR * 0.15f, k);
		break;
	case EBadgeInfo:                        // an "i": a dot over a stem
		Circle(aFb, aCx, aCy - aR * 0.45f, aR * 0.16f, k);
		Line(aFb, aCx, aCy - aR * 0.12f, aCx, aCy + aR * 0.5f, w, k);
		break;
	default:
		Line(aFb, aCx - aR * 0.42f, aCy, aCx + aR * 0.42f, aCy, w, k);
		break;
		}
	}

// a ring of radius aR, aT thick; aGapUp: the top left open (the power sign)
void Ring(uint16_t* aFb, float aCx, float aCy, float aR, float aT, uint16_t aC, bool aGapUp = false)
	{
	float o = aR + aT / 2, i = aR - aT / 2;
	Shape(aFb, aCx - o, aCy - o, aCx + o, aCy + o, aC, [&](float x, float y)
		{
		float dx = x - aCx, dy = y - aCy, d2 = dx * dx + dy * dy;
		if (d2 > o * o || d2 < i * i) return false;
		return !(aGapUp && dy < 0 && fabsf(dx) < -dy * 0.6f);
		});
	}

// a box with rounded corners, filled
void RoundBox(uint16_t* aFb, float aX0, float aY0, float aX1, float aY1, float aR, uint16_t aC)
	{
	Shape(aFb, aX0, aY0, aX1, aY1, aC, [&](float x, float y)
		{
		if (x < aX0 || x > aX1 || y < aY0 || y > aY1) return false;
		float cx = x < aX0 + aR ? aX0 + aR : x > aX1 - aR ? aX1 - aR : x;
		float cy = y < aY0 + aR ? aY0 + aR : y > aY1 - aR ? aY1 - aR : y;
		return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= aR * aR;
		});
	}

// an arrow from (aX, aY0) to (aX, aY1), with its head at aY1
void Arrow(uint16_t* aFb, float aX, float aY0, float aY1, float aHead, float aW, uint16_t aC)
	{
	float s = aY1 > aY0 ? 1.0f : -1.0f;
	Line(aFb, aX, aY0, aX, aY1, aW, aC);
	Line(aFb, aX - aHead, aY1 - s * aHead, aX, aY1, aW, aC);
	Line(aFb, aX + aHead, aY1 - s * aHead, aX, aY1, aW, aC);
	}

// the row icons (TIcon) not drawn by the ops of their own; all in a box aS
// across, centred on (aCx, aCy), in the stroke weight of the Wi-Fi fan
void Icon(uint16_t* aFb, float aCx, float aCy, float aS, int aKind, int aArg, uint16_t aC, uint16_t aOff)
	{
	float h = aS / 2, t = aS * 0.12f;
	switch (aKind)
		{
	case EIconBars:                         // four bars, rising
		{
		float bw = aS * 0.17f, gap = aS * 0.09f;
		float x = aCx - (4 * bw + 3 * gap) / 2, foot = aCy + h * 0.85f;
		for (int i = 0; i < 4; i++, x += bw + gap)
			{
			float top = foot - aS * (0.32f + 0.22f * i) * 0.9f;
			RoundBox(aFb, x, top, x + bw, foot, bw * 0.25f, i < aArg ? aC : aOff);
			}
		break;
		}
	case EIconGlobe:                        // a ring, a meridian and the equator
		{
		float r = h * 0.82f;
		Ring(aFb, aCx, aCy, r, t, aC);
		float rx = r * 0.42f;
		Shape(aFb, aCx - rx - t, aCy - r, aCx + rx + t, aCy + r, aC, [&](float x, float y)
			{
			float dx = (x - aCx) / rx, dy = (y - aCy) / r;
			float d = sqrtf(dx * dx + dy * dy) * rx;
			return d >= rx - t / 2 && d <= rx + t / 2;
			});
		Line(aFb, aCx - r, aCy, aCx + r, aCy, t, aC);
		break;
		}
	case EIconClock:                        // a ring and two hands
		{
		float r = h * 0.82f;
		Ring(aFb, aCx, aCy, r, t, aC);
		Line(aFb, aCx, aCy, aCx, aCy - r * 0.6f, t, aC);
		Line(aFb, aCx, aCy, aCx + r * 0.45f, aCy + r * 0.1f, t, aC);
		break;
		}
	case EIconData:                         // in (down) on the left, out (up) on the right
		Arrow(aFb, aCx - h * 0.42f, aCy - h * 0.8f, aCy + h * 0.8f, h * 0.38f, t * 1.1f, aC);
		Arrow(aFb, aCx + h * 0.42f, aCy + h * 0.8f, aCy - h * 0.8f, h * 0.38f, t * 1.1f, aC);
		break;
	case EIconPower:                        // the power sign: an open ring and a stroke
		Ring(aFb, aCx, aCy + h * 0.08f, h * 0.7f, t, aC, true);
		Line(aFb, aCx, aCy - h * 0.85f, aCx, aCy - h * 0.05f, t, aC);
		break;
	case EIconServer:                       // two units, a light on each
		for (int i = 0; i < 2; i++)
			{
			float y0 = aCy - h * 0.8f + i * h * 0.86f;
			RoundBox(aFb, aCx - h * 0.85f, y0, aCx + h * 0.85f, y0 + h * 0.68f, aS * 0.06f, aC);
			Circle(aFb, aCx + h * 0.5f, y0 + h * 0.34f, aS * 0.06f, 0);
			}
		break;
	case EIconHome:                         // a roof and a body, a door cut out
		{
		float top = aCy - h * 0.85f, eave = aCy - h * 0.1f, foot = aCy + h * 0.8f, w = h * 0.9f;
		Shape(aFb, aCx - w, top, aCx + w, eave, aC, [&](float x, float y)
			{ return y >= top && y <= eave && fabsf(x - aCx) <= (y - top) / (eave - top) * w; });
		Shape(aFb, aCx - w * 0.68f, eave - 1, aCx + w * 0.68f, foot, aC, [&](float x, float y)
			{ return y >= eave - 0.5f && y <= foot && fabsf(x - aCx) <= w * 0.68f && !(fabsf(x - aCx) < w * 0.2f && y > foot - h * 0.6f); });
		break;
		}
	case EIconNext:                         // an arrow to the right
		Line(aFb, aCx - h * 0.8f, aCy, aCx + h * 0.75f, aCy, t * 1.1f, aC);
		Line(aFb, aCx + h * 0.75f - h * 0.45f, aCy - h * 0.45f, aCx + h * 0.75f, aCy, t * 1.1f, aC);
		Line(aFb, aCx + h * 0.75f - h * 0.45f, aCy + h * 0.45f, aCx + h * 0.75f, aCy, t * 1.1f, aC);
		break;
	default:
		break;
		}
	}

// a padlock: a rounded body, and the shackle over it (raised and cut on
// one side when open)
void Lock(uint16_t* aFb, float aCx, float aCy, float aS, bool aClosed, uint16_t aC)
	{
	float bw = aS * 0.78f, bh = aS * 0.52f, rr = aS * 0.1f;
	float bx0 = aCx - bw / 2, by0 = aCy + aS * 0.5f - bh, bx1 = aCx + bw / 2, by1 = aCy + aS * 0.5f;
	Shape(aFb, bx0, by0, bx1, by1, aC, [&](float x, float y)
		{
		if (x < bx0 || x > bx1 || y < by0 || y > by1) return false;
		float cx = x < bx0 + rr ? bx0 + rr : x > bx1 - rr ? bx1 - rr : x;
		float cy = y < by0 + rr ? by0 + rr : y > by1 - rr ? by1 - rr : y;
		return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr * rr;
		});
	float r = aS * 0.25f, t = aS * 0.12f;
	float lift = aClosed ? 0 : aS * 0.16f;
	float sy = by0 - aS * 0.12f - lift;         // the centre of the shackle's arch
	Shape(aFb, aCx - r - t, sy - r - t, aCx + r + t, by0 + 1, aC, [&](float x, float y)
		{
		float dx = x - aCx, dy = y - sy;
		if (y <= sy)
			{
			float d = sqrtf(dx * dx + dy * dy);
			return d >= r - t / 2 && d <= r + t / 2;
			}
		if (y > by0 + 0.5f) return false;
		bool left = fabsf(dx + r) <= t / 2;
		bool right = fabsf(dx - r) <= t / 2 && (aClosed || y < sy + aS * 0.08f);
		return left || right;
		});
	}

const FontMetric& Master(int aFace)
	{
	return aFace == EFaceBold ? kFreeSansBold24pt7bMetric : kFreeSans24pt7bMetric;
	}

void Text(uint16_t* aFb, const DrawOp& o)
	{
	const FaceData& f = gFace[o.font < EFaceCount ? o.font : 0];
	if (!f.bitmap || o.h <= 0)
		return;
	float k = (float)o.h / Master(o.font).cap;  // output pixels per font unit
	int pen = 0;                                // in font units
	for (const char* t = o.text; *t; t++)
		{
		unsigned ch = (unsigned char)*t;
		if (ch < f.first || ch > f.last) ch = '?';
		const GlyphRec& g = f.glyph[ch - f.first];
		const uint8_t* bm = f.bitmap + g.off;
		if (g.w && g.h)
			{
			float gx = o.x + (pen + g.xo) * k, gy = o.y + g.yo * k;
			int x0 = (int)floorf(gx), x1 = (int)ceilf(gx + g.w * k);
			int y0 = (int)floorf(gy), y1 = (int)ceilf(gy + g.h * k);
			for (int y = y0; y < y1; y++)
				for (int x = x0; x < x1; x++)
					{
					int cov = 0;
					for (int sy = 0; sy < kSub; sy++)
						{
						int by = (int)floorf((y + (sy + 0.5f) / kSub - gy) / k);
						if (by < 0 || by >= g.h) continue;
						for (int sx = 0; sx < kSub; sx++)
							{
							int bx = (int)floorf((x + (sx + 0.5f) / kSub - gx) / k);
							if (bx < 0 || bx >= g.w) continue;
							int bit = by * g.w + bx;
							if (bm[bit >> 3] & (0x80 >> (bit & 7)))
								cov++;
							}
						}
					Blend(aFb, x, y, o.fg, cov);
					}
			}
		pen += g.adv;
		}
	}

} // namespace

void Canvas::SetFace(int aFace, const FaceData& aData)
	{
	if (aFace >= 0 && aFace < EFaceCount)
		gFace[aFace] = aData;
	}

void Canvas::Render(const Layout& aLayout, uint16_t* aFb)
	{
	const uint16_t kOff = Rgb565(60, 60, 64);   // the unlit arcs of the Wi-Fi fan
	for (int i = 0; i < aLayout.n; i++)
		{
		const DrawOp& o = aLayout.op[i];
		switch (o.op)
			{
		case EOpFill:
			for (int y = o.y; y < o.y + o.h; y++)
				for (int x = o.x; x < o.x + o.w; x++)
					if (x >= 0 && y >= 0 && x < N && y < N)
						aFb[y * N + x] = o.fg;
			break;
		case EOpText:  Text(aFb, o); break;
		case EOpWifi:  Wifi(aFb, o.x, o.y, o.w, o.h, o.fg, kOff); break;
		case EOpBadge: Badge(aFb, o.x, o.y, o.w, o.h, o.fg); break;
		case EOpLock:  Lock(aFb, o.x, o.y, o.w, o.h != 0, o.fg); break;
		case EOpDot:   Circle(aFb, o.x, o.y, o.w, o.fg); break;
		case EOpIcon:  Icon(aFb, o.x, o.y, o.w, o.h, o.font, o.fg, kOff); break;
			}
		}
	}

void Canvas::BmpHeader(uint8_t* aOut)
	{
	memset(aOut, 0, kBmpHeader);
	auto put32 = [&](int aAt, uint32_t aV) { for (int i = 0; i < 4; i++) aOut[aAt + i] = (uint8_t)(aV >> (8 * i)); };
	aOut[0] = 'B'; aOut[1] = 'M';
	put32(2, (uint32_t)kBmpSize);
	put32(10, (uint32_t)kBmpHeader);             // where the pixels start
	put32(14, 40);                               // BITMAPINFOHEADER
	put32(18, (uint32_t)kSize);
	put32(22, (uint32_t)kSize);                  // (positive: rows bottom up)
	aOut[26] = 1;                                // planes
	aOut[28] = 24;                               // bits a pixel
	put32(34, (uint32_t)(kBmpRow * kSize));
	put32(38, 2835); put32(42, 2835);            // 72 dpi
	}

void Canvas::BmpRow(const uint16_t* aFb, int aRow, uint8_t* aOut)
	{
	const uint16_t* p = aFb + (kSize - 1 - aRow) * kSize;
	for (int x = 0; x < kSize; x++)
		{
		uint16_t c = p[x];
		int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
		aOut[3 * x + 0] = (uint8_t)((b << 3) | (b >> 2));
		aOut[3 * x + 1] = (uint8_t)((g << 2) | (g >> 4));
		aOut[3 * x + 2] = (uint8_t)((r << 3) | (r >> 2));
		}
	}

} // namespace am
