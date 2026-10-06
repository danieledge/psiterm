// button.h - the one button (GPIO41 on the AtomS3 boards, which on the
// AtomS3R is the LCD itself pressed in): debounced, and told apart as a click
// or a hold. Portable C++ with no Arduino calls (host-tested): the board
// reads the pin and feeds it the level and the time.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_BUTTON_H
#define ATOM_BUTTON_H

#include <stdint.h>

namespace am {

class Button
	{
public:
	enum TEvent { ENone = 0, EClick, EHold };
	static const uint32_t kDebounceMs = 20;     // a level must stay this long to count
	static const uint32_t kClickMinMs = 30;     // pressed at least this long, and
#if defined(AM_HAS_LCD)
	static const uint32_t kClickMaxMs = 700;    // ... at most this long: a click (crisp click-versus-hold on the S3R's screen button)
#else
	static const uint32_t kClickMaxMs = 1900;   // ... boards without the LCD: as before, any press under the hold is a click
#endif
	static const uint32_t kHoldMs = 2000;       // held this long: a hold (once per press)

	Button() : iStarted(false), iRaw(1), iStable(1), iRawSinceMs(0), iDownMs(0), iHoldFired(false) {}
	// aLevel: the pin's level (0 = pressed, the button pulls it low). Call
	// often. A press between kClickMinMs and kClickMaxMs, once released, is
	// an EClick; one still down at kHoldMs is an EHold, once, and its
	// release is then nothing. A press in between is nothing.
	TEvent Feed(int aLevel, uint32_t aNowMs);
	// how long the button has been held now (0 if it is up): for the
	// "hold at power-on for 3 s" reset
	uint32_t HeldMs(uint32_t aNowMs) const;
	bool Down() const { return iStarted && iStable == 0; }

private:
	bool iStarted;
	int iRaw, iStable;
	uint32_t iRawSinceMs;
	uint32_t iDownMs;               // when the press began
	bool iHoldFired;
	};

} // namespace am

#endif
