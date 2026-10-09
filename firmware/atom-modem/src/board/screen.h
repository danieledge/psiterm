// screen.h - the AtomS3R's 128x128 LCD (GC9107, through M5Unified) as a
// status screen: four pages of 21 columns by 16 rows in the 6x8 font (the
// text comes from status.h), a bar on the top row in the LED's colour. Only
// the rows that changed are redrawn, about four times a second, and the
// backlight dims after a minute without a button press. Built only when
// AM_HAS_LCD is set (the atoms3r environment). It is drawn from loop(), never
// from Modem::Loop().
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
	// a message over the whole screen (the reset countdown); two short lines
	void Message(const char* aLine1, const char* aLine2, const char* aLine3 = "");
	// from loop(), after the web pages: draw what changed, about four times a second
	void Tick(const Modem& aModem, Hal& aHal, uint32_t aNowMs);
	void NextPage(uint32_t aNowMs);             // a click: the next page, drawn at once
	// true if the backlight was dimmed: it is now on, and the click is used up
	bool Wake(uint32_t aNowMs);
	void SetBar(LedState aState);               // from the LED state machine (AtomHal::Led)
	bool Dimmed() const { return iDimmed; }
	int Page() const { return iPage; }

	static const uint32_t kRedrawMs = 250;
	static const uint32_t kDimAfterMs = 60 * 1000;

private:
	void Draw(const ScreenPage& aPage, bool aAll);
	void DrawStatus(const ScreenPage& aPage, LedState aBar);   // the big hero view
	void Backlight(bool aOn);

	int iPage;
	bool iReady;
	bool iDimmed;
	bool iForce;                                // redraw at the next Tick
	uint32_t iLastDrawMs;
	uint32_t iLastActiveMs;
	LedState iBar;                              // the bar now (set from the LED state machine)
	LedState iShownBar;
	int iShownPage;
	char iShown[kScreenRows][kScreenCols + 1];
	// what the hero (Status) view last showed, so it redraws only on a change
	char iShownState[14];
	HeroRow iShownHero[kHeroRows];
	int iShownHeroRows;
	int iShownSignal;
	char iShownFoot[kScreenCols + 1];
	};

extern Screen gScreen;

} // namespace am

#endif
#endif
