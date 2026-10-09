// screen.cpp - see screen.h. MIT licence (see LICENSE at the top of the repository).
#if defined(AM_HAS_LCD)

#include "screen.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <string.h>

namespace am {

Screen gScreen;

Screen::Screen()
	: iPage(0), iReady(false), iDimmed(false), iForce(true), iLastDrawMs(0), iLastActiveMs(0), iBar(ELedNoWifi),
	  iShownBar(ELedNoWifi), iShownPage(-1), iShownHeroRows(-1), iShownSignal(-2)
	{
	memset(iShown, 0, sizeof(iShown));
	memset(iShownState, 0, sizeof(iShownState));
	memset(iShownHero, 0, sizeof(iShownHero));
	memset(iShownFoot, 0, sizeof(iShownFoot));
	}

static uint16_t BarColour(LedState aState, uint16_t& aText)
	{
	aText = TFT_WHITE;
	switch (aState)
		{
	case ELedNoWifi:     return M5.Display.color565(190, 0, 0);          // red
	case ELedConnecting: aText = TFT_BLACK; return M5.Display.color565(230, 160, 0);   // amber
	case ELedWifi:       aText = TFT_BLACK; return M5.Display.color565(0, 180, 0);     // green
	case ELedConnected:  return M5.Display.color565(0, 0, 220);          // blue
	case ELedData:       aText = TFT_BLACK; return M5.Display.color565(235, 235, 255); // white flash
	case ELedConfig:     return M5.Display.color565(150, 0, 170);        // purple: the access point is up
		}
	return TFT_DARKGREY;
	}

void Screen::Begin()
	{
	auto cfg = M5.config();
	cfg.serial_baudrate = 0;                    // (main.cpp opens the USB console itself)
	cfg.internal_imu = false;
	cfg.internal_rtc = false;
	cfg.clear_display = true;
	M5.begin(cfg);
	M5.Display.setBrightness(64);
	M5.Display.setRotation(0);
	M5.Display.setFont(&fonts::Font0);          // 6x8: 21 columns by 16 rows on 128x128
	M5.Display.setTextSize(1);
	M5.Display.setTextWrap(false);
	M5.Display.fillScreen(TFT_BLACK);
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

void Screen::Message(const char* aLine1, const char* aLine2, const char* aLine3)
	{
	if (!iReady)
		return;
	Backlight(true);
	M5.Display.fillScreen(TFT_BLACK);
	M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
	M5.Display.drawString(aLine1, 2, 40);
	M5.Display.drawString(aLine2, 2, 56);
	M5.Display.drawString(aLine3, 2, 72);
	iForce = true;
	iShownPage = -1;                            // the next Tick draws the whole page
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

void Screen::NextPage(uint32_t aNowMs)
	{
	iLastActiveMs = aNowMs;
	iPage = (iPage + 1) % StatusModel::Pages();
	iForce = true;
	}

void Screen::Draw(const ScreenPage& aPage, bool aAll)
	{
	M5.Display.startWrite();
	for (int r = 0; r < kScreenRows; r++)
		{
		bool title = r == 0;
		if (!aAll && !(title && iBar != iShownBar) && strcmp(iShown[r], aPage.line[r]) == 0)
			continue;
		uint16_t fg = TFT_WHITE, bg = TFT_BLACK;
		if (title)
			bg = BarColour(iBar, fg);
		char row[kScreenCols + 1];
		memset(row, ' ', kScreenCols);
		row[kScreenCols] = 0;
		size_t n = strlen(aPage.line[r]);
		memcpy(row, aPage.line[r], n < (size_t)kScreenCols ? n : (size_t)kScreenCols);
		M5.Display.setTextColor(fg, bg);
		if (title)
			M5.Display.fillRect(0, 0, 128, 8, bg);
		M5.Display.drawString(row, 0, r * 8);
		memcpy(iShown[r], aPage.line[r], sizeof(iShown[r]));
		}
	M5.Display.endWrite();
	iShownBar = iBar;
	}

// four Wi-Fi signal bars at (aX,aBase) rising to the right, aLevel of them lit
static void DrawSignal(int aX, int aBase, int aLevel, uint16_t aOn, uint16_t aOff)
	{
	for (int i = 0; i < 4; i++)
		{
		int bw = 4, gap = 2, bh = 5 + i * 4;
		int x = aX + i * (bw + gap);
		int y = aBase - bh;
		uint16_t c = i < aLevel ? aOn : aOff;
		if (i < aLevel)
			M5.Display.fillRect(x, y, bw, bh, c);
		else
			M5.Display.drawRect(x, y, bw, bh, c);
		}
	}

// the Status page as a dashboard: a state word on a colour band with Wi-Fi
// signal bars, then grey label / white value rows, and a footer. Legible
// across a desk; the dense 21x16 pages (Modes/Setup/Log) stay for detail.
void Screen::DrawStatus(const ScreenPage& aPage, LedState aBar)
	{
	const uint16_t kBody  = TFT_BLACK;
	const uint16_t kLabel = M5.Display.color565(150, 150, 150);
	const uint16_t kValue = TFT_WHITE;
	const int kBand = 34;
	uint16_t bfg;
	uint16_t bbg = BarColour(aBar, bfg);        // the band colour and its text
	M5.Display.startWrite();
	// the state band
	M5.Display.fillRect(0, 0, 128, kBand, bbg);
	M5.Display.setTextColor(bfg, bbg);
	M5.Display.setTextSize(2);                   // 12x16
	M5.Display.drawString(aPage.state, 5, (kBand - 16) / 2);
	if (aPage.signal >= 0)
		DrawSignal(96, kBand - 8, aPage.signal, bfg, bfg);
	// the body
	M5.Display.fillRect(0, kBand, 128, 128 - kBand, kBody);
	M5.Display.setTextSize(1);                   // 6x8
	int y = kBand + 8;
	for (int r = 0; r < aPage.heroRows && y <= 104; r++, y += 16)
		{
		if (aPage.hero[r].label[0])
			{
			M5.Display.setTextColor(kLabel, kBody);
			M5.Display.drawString(aPage.hero[r].label, 5, y);
			}
		M5.Display.setTextColor(kValue, kBody);
		M5.Display.drawString(aPage.hero[r].value, 46, y);   // value column
		}
	// the footer
	if (aPage.foot[0])
		{
		M5.Display.drawFastHLine(5, 116, 118, kLabel);
		M5.Display.setTextColor(kLabel, kBody);
		M5.Display.drawString(aPage.foot, 5, 119);
		}
	M5.Display.endWrite();
	M5.Display.setTextSize(1);
	}

void Screen::Tick(const Modem& aModem, Hal& aHal, uint32_t aNowMs)
	{
	if (!iReady)
		return;
	if (!iDimmed && aNowMs - iLastActiveMs >= kDimAfterMs)
		Backlight(false);
	if (!iForce && aNowMs - iLastDrawMs < kRedrawMs)
		return;
	iLastDrawMs = aNowMs;
	if (iDimmed && !iForce)
		return;                                 // (nothing to see: no work to do)
	iForce = false;
	static ScreenPage page;                     // (static: off the 20 KB loop stack)
	StatusModel::Fill(aModem, aHal, iPage, page);
	iBar = page.bar;                            // the modem's own LED state: one source
	if (iPage == EPageStatus)
		{
		// a data flash keeps the "on a call" colour, so the hero doesn't flash
		LedState cbar = iBar == ELedData ? ELedConnected : iBar;
		bool changed = iShownPage != iPage || cbar != iShownBar
			|| strcmp(iShownState, page.state) != 0 || iShownHeroRows != page.heroRows
			|| iShownSignal != page.signal || strcmp(iShownFoot, page.foot) != 0;
		for (int r = 0; !changed && r < page.heroRows; r++)
			if (strcmp(iShownHero[r].label, page.hero[r].label) != 0
				|| strcmp(iShownHero[r].value, page.hero[r].value) != 0)
				changed = true;
		if (changed)
			{
			DrawStatus(page, cbar);
			iShownPage = iPage;
			iShownBar = cbar;
			iShownSignal = page.signal;
			strncpy(iShownState, page.state, sizeof(iShownState) - 1);
			iShownState[sizeof(iShownState) - 1] = 0;
			strncpy(iShownFoot, page.foot, sizeof(iShownFoot) - 1);
			iShownFoot[sizeof(iShownFoot) - 1] = 0;
			iShownHeroRows = page.heroRows;
			for (int r = 0; r < page.heroRows; r++)
				iShownHero[r] = page.hero[r];
			memset(iShown, 0, sizeof(iShown));  // a later text page redraws in full
			}
		return;
		}
	bool all = iShownPage != iPage;
	if (all)
		M5.Display.fillScreen(TFT_BLACK);
	iShownPage = iPage;
	Draw(page, all);
	}

} // namespace am

#endif
