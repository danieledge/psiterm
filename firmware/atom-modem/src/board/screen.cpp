// screen.cpp - see screen.h. MIT licence (see LICENSE at the top of the repository).
#if defined(AM_HAS_LCD)

#include "screen.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "../canvas.h"

namespace am {

Screen gScreen;

// The page is drawn into this frame buffer (canvas.h: the same code the
// host renderer runs) and pushed to the panel whole: drawing straight to the
// panel (clear, then text) showed every redraw as a flicker. 32 KB, in PSRAM
// on the AtomS3R; /screen.bmp serves it as it is.
static uint16_t* gFb = 0;

static_assert(sizeof(GlyphRec) == sizeof(lgfx::GFXglyph), "GlyphRec must match M5GFX's GFXglyph");
static FaceData FaceOf(const lgfx::GFXfont& aF)
	{
	FaceData d;
	d.bitmap = aF.bitmap;
	d.glyph = reinterpret_cast<const GlyphRec*>(aF.glyph);
	d.first = aF.first;
	d.last = aF.last;
	return d;
	}

Screen::Screen()
	: iReady(false), iDimmed(false), iForce(true), iLastDrawMs(0), iLastActiveMs(0), iToastUntilMs(0), iBar(ELedNoWifi),
	  iHaveShown(false)
	{
	memset(&iShown, 0, sizeof(iShown));
	}

void Screen::Begin()
	{
	auto cfg = M5.config();
	cfg.serial_baudrate = 0;                    // (main.cpp opens the USB console itself)
	cfg.internal_imu = false;
	cfg.internal_rtc = false;
	cfg.clear_display = true;
	M5.begin(cfg);
	M5.Display.setBrightness(24);               // dim through the current-fragile boot/WiFi-init window; SetBar raises it once WiFi is up
	M5.Display.setRotation(0);
	M5.Display.fillScreen(TFT_BLACK);
	Canvas::SetFace(EFaceSans, FaceOf(fonts::FreeSans24pt7b));
	Canvas::SetFace(EFaceBold, FaceOf(fonts::FreeSansBold24pt7b));
	const size_t kBytes = Canvas::kSize * Canvas::kSize * sizeof(uint16_t);
#if defined(BOARD_HAS_PSRAM)
	gFb = (uint16_t*)heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
	if (!gFb)
		gFb = (uint16_t*)malloc(kBytes);
	if (gFb)
		memset(gFb, 0, kBytes);
	Serial.printf("LCD: %s\n", gFb ? "frame buffer ready" : "no memory for the frame buffer: the screen stays blank");
	iReady = true;
	iDimmed = false;
	iForce = true;
	iLastActiveMs = millis();
	}

void Screen::Backlight(bool aOn)
	{
	if (!iReady)
		return;
	iDimmed = !aOn;
	M5.Display.setBrightness(aOn ? 64 : 0);
	}

void Screen::Draw(const Layout& aLayout)
	{
	if (!gFb)
		return;
	Canvas::Render(aLayout, gFb);
	// one transfer, so no half-drawn frame is ever seen. The buffer is RGB565
	// in the CPU's byte order: said so by its type (M5GFX would otherwise
	// take a plain uint16_t array as byte-swapped)
	M5.Display.pushImage(0, 0, Canvas::kSize, Canvas::kSize, reinterpret_cast<const lgfx::rgb565_t*>(gFb));
	}

const uint16_t* Screen::Frame() const
	{
	return gFb;
	}

void Screen::Message(const char* aLine1, const char* aLine2, const char* aLine3)
	{
	if (!iReady)
		return;
	Backlight(true);
	static Layout l;
	StatusModel::LayMessage(aLine1, aLine2, aLine3, l);
	Draw(l);
	iForce = true;
	iHaveShown = false;                         // the next Tick draws the page whole
	}

void Screen::Toast(uint32_t aNowMs, const char* aLine1, const char* aLine2, const char* aLine3)
	{
	if (!iReady)
		return;
	iLastActiveMs = aNowMs;
	Backlight(true);
	static Layout l;
	StatusModel::LayMessage(aLine1, aLine2, aLine3, l);
	Draw(l);
	iToastUntilMs = aNowMs + 1800;
	if (!iToastUntilMs) iToastUntilMs = 1;
	iHaveShown = false;
	}

void Screen::SetBar(LedState aState)
	{
	if (aState == iBar)
		return;
	// a status change is worth waking the screen for; the data flash is not
	if (iReady && aState != ELedData && iBar != ELedData)
		{
		iLastActiveMs = millis();
		if (iDimmed)
			Backlight(true);
		}
	// once the modem is actually up (past OFFLINE/CONNECTING), bring the
	// backlight to full - the boot dim was only to ease the cold-start current
	if (iReady && !iDimmed && aState != ELedNoWifi && aState != ELedConnecting)
		M5.Display.setBrightness(64);
	iBar = aState;
	iForce = true;
	}

bool Screen::Wake(uint32_t aNowMs)
	{
	iLastActiveMs = aNowMs;
	if (!iDimmed)
		return false;
	Backlight(true);
	iForce = true;
	return true;
	}

void Screen::Pressed(uint32_t aNowMs)
	{
	iLastActiveMs = aNowMs;
	iForce = true;
	}

void Screen::Click(uint32_t aNowMs)
	{
	Pressed(aNowMs);
	iPager.Click(aNowMs);
	}

Pager::TAction Screen::Double(uint32_t aNowMs)
	{
	Pressed(aNowMs);
	return iPager.Double(aNowMs);
	}

void Screen::ConfigMode(bool aOn, uint32_t aNowMs)
	{
	Pressed(aNowMs);
	iPager.ConfigMode(aOn, aNowMs);
	}

void Screen::Tick(const Modem& aModem, Hal& aHal, uint32_t aNowMs)
	{
	if (!iReady)
		return;
	if (!iDimmed && aNowMs - iLastActiveMs >= kDimAfterMs)
		Backlight(false);
	if (iToastUntilMs)
		{
		if ((int32_t)(aNowMs - iToastUntilMs) < 0)
			return;                             // (the toast stays a moment)
		iToastUntilMs = 0;
		iForce = true;
		}
	if (!iForce && aNowMs - iLastDrawMs < kRedrawMs)
		return;
	iLastDrawMs = aNowMs;
	// back to Home after a while without a press; the Setup page stays while
	// the access point is up (its name and password are being read)
	bool keep = false;
	if (iPager.Page() == EPageSetup)
		{
		char ap[8];
		ap[0] = 0;
		aHal.ApInfo(ap, sizeof(ap));
		keep = ap[0] != 0;
		}
	if (iPager.Idle(aNowMs, keep))
		iForce = true;
	if (iDimmed && !iForce)
		return;                                 // (nothing to see: no work to do)
	iForce = false;
	static ScreenPage page;                     // (static: off the 20 KB loop stack)
	static Layout lay;
	StatusModel::Fill(aModem, aHal, iPager.Page(), page);
	page.hintOn = iPager.HintOn(aNowMs);
	iBar = page.bar;                            // the modem's own LED state: one source
	StatusModel::Lay(page, lay);
	// only a page that looks different is drawn (Lay clears every byte, so
	// two layouts of the same page compare equal)
	if (iHaveShown && memcmp(&lay, &iShown, sizeof(lay)) == 0)
		return;
	Draw(lay);
	memcpy(&iShown, &lay, sizeof(iShown));
	iHaveShown = true;
	}

} // namespace am

#endif
