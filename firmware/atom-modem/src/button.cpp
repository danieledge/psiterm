// button.cpp - see button.h. MIT licence (see LICENSE at the top of the repository).
#include "button.h"

namespace am {

Button::TEvent Button::Feed(int aLevel, uint32_t aNowMs)
	{
	int level = aLevel ? 1 : 0;
	if (!iStarted)
		{
		// the first look: whatever the level is, it is the state (a button held
		// at power-on counts from now)
		iStarted = true;
		iRaw = iStable = level;
		iRawSinceMs = aNowMs;
		iDownMs = aNowMs;
		iHoldFired = false;
		return ENone;
		}
	if (level != iRaw)
		{
		iRaw = level;
		iRawSinceMs = aNowMs;                   // (a change: the debounce clock starts again)
		}
	TEvent ev = ENone;
	if (iRaw != iStable && aNowMs - iRawSinceMs >= kDebounceMs)
		{
		iStable = iRaw;
		if (iStable == 0)
			{
			iDownMs = iRawSinceMs;              // the press began at the edge, not when it was accepted
			iHoldFired = false;
			}
		else
			{
			uint32_t held = iRawSinceMs - iDownMs;
			if (!iHoldFired && held >= kClickMinMs && held <= kClickMaxMs)
				ev = EClick;
			iHoldFired = false;
			}
		}
	if (iStable == 0 && !iHoldFired && aNowMs - iDownMs >= kHoldMs)
		{
		iHoldFired = true;
		ev = EHold;
		}
	return ev;
	}

uint32_t Button::HeldMs(uint32_t aNowMs) const
	{
	return Down() ? aNowMs - iDownMs : 0;
	}

} // namespace am
