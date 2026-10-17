// screen.h - the AtomS3R's 128x128 LCD (GC9107, through M5Unified) as a
// status screen. The pages, their layout and the gestures are portable
// (status.h), and so is the drawing (canvas.h): each page is drawn whole into
// a frame buffer (32 KB, in PSRAM) and pushed to the panel in one go, and
// only when its layout changed, so nothing flickers. The web pages serve the
// buffer as /screen.bmp. The backlight dims
// after a minute without a press. An extension module may put a page of its
// own over the pages while it has something to show (ext.h: ScreenWanted),
// while Home would be showing (Pager::Quiet). Built only when AM_HAS_LCD is set (the
// atoms3r environment). It is drawn from loop(), never from Modem::Loop().
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_SCREEN_H
#define ATOM_SCREEN_H

#if defined(AM_HAS_LCD)

#include <stdint.h>
#include "../status.h"

namespace am {

class Screen
	{
public:
	Screen();
	void Begin();                               // M5Unified: the display up, the backlight on
	// a message over the whole screen (the reset countdown), until the next one
	void Message(const char* aLine1, const char* aLine2, const char* aLine3 = "");
	// the same for a moment (what a double-press chose), then the page again
	void Toast(uint32_t aNowMs, const char* aLine1, const char* aLine2, const char* aLine3 = "");
	// from loop(), after the web pages: draw what changed, about four times a second
	void Tick(const Modem& aModem, Hal& aHal, uint32_t aNowMs);
	// the gestures (main.cpp): see Pager in status.h
	void Click(uint32_t aNowMs);
	Pager::TAction Double(uint32_t aNowMs);
	void ConfigMode(bool aOn, uint32_t aNowMs);
	// true if the backlight was dimmed: it is now on, and the press is used up
	bool Wake(uint32_t aNowMs);
	void SetBar(LedState aState);               // from the LED state machine (AtomHal::Led)
	bool Dimmed() const { return iDimmed; }
	int Page() const { return iPager.Page(); }
	// what the panel shows, as drawn: 128x128 RGB565 (canvas.h), or null
	const uint16_t* Frame() const;

	static const uint32_t kRedrawMs = 250;
	static const uint32_t kDimAfterMs = 60 * 1000;

private:
	void Draw(const Layout& aLayout);           // into the sprite, then to the panel
	void Backlight(bool aOn);
	void Pressed(uint32_t aNowMs);

	Pager iPager;
	bool iReady;
	bool iDimmed;
	bool iForce;                                // redraw at the next Tick
	uint32_t iLastDrawMs;
	uint32_t iLastActiveMs;
	uint32_t iToastUntilMs;                     // a toast is showing until then (0: none)
	LedState iBar;                              // the bar now (set from the LED state machine)
	bool iHaveShown;
	Layout iShown;                              // what the panel shows now
	const Extension* iExtShown;                 // the module whose page is up (0: the pages)
	uint32_t iExtNews;                          // its ScreenNews() when last looked at
	};

extern Screen gScreen;

} // namespace am

#endif
#endif
