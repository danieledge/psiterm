// status.h - what the modem's little screen shows (the AtomS3R's 128x128
// LCD), as text: four pages of 21 columns by 16 rows (the 6x8 font), and a
// bar colour. Portable C++ with no Arduino calls, so the host tests can read
// the pages; board/screen.cpp only draws them. The bar colour is the LED
// state (modem.h), taken from the modem's own LED state machine, so the LCD
// and the WS2812 boards can never disagree.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_STATUS_H
#define ATOM_STATUS_H

#include "modem.h"

namespace am {

static const int kScreenCols = 21;
static const int kScreenRows = 16;
static const int kHeroRows = 6;

enum TScreenPage { EPageStatus = 0, EPageModes, EPageSetup, EPageLog, EPageCount };

// one fact on the Status "hero": a short grey label and a white value
struct HeroRow
	{
	char label[8];
	char value[kScreenCols + 1];
	};

struct ScreenPage
	{
	char line[kScreenRows][kScreenCols + 1];   // row 0 is the title (drawn on the bar)
	LedState bar;                              // the bar's colour: the LED state
	// The Status page also comes as a "dashboard" hero: a state word on a
	// coloured band (with Wi-Fi signal bars), then label/value rows and a
	// footer. The LCD (board/screen.cpp) draws it; the other pages keep the
	// dense 21x16 text in line[] above. The host tests read both, so this
	// layer stays portable.
	char state[14];                            // "OFFLINE", "ONLINE", "ON A CALL"...
	HeroRow hero[kHeroRows];                   // key facts under the band
	int heroRows;
	int signal;                                // Wi-Fi bars 0..4, or -1 (no Wi-Fi)
	char foot[kScreenCols + 1];                // the footer line (baud, flow control)
	};

class StatusModel
	{
public:
	// aPage is taken modulo EPageCount. (aHal is not const: its info calls are not.)
	static void Fill(const Modem& aModem, Hal& aHal, int aPage, ScreenPage& aOut);
	static int Pages() { return EPageCount; }
	// "1.2K", "34M": a byte count in at most 5 characters
	static void Count(uint32_t aBytes, char* aOut, size_t aMax);
	};

} // namespace am

#endif
