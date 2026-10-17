// status.h - what the modem's little screen shows (the AtomS3R's 128x128
// LCD), and how its one button moves between the pages. Portable C++ with no
// Arduino calls, so the host tests can read the pages, check the layout and
// drive the gestures; board/screen.cpp only shows the result.
//
// Three layers:
//   ScreenPage  the facts on a page, as text (StatusModel::Fill)
//   Layout      the page as a list of drawing steps on 128x128 pixels
//               (StatusModel::Lay): rectangles, smooth text in one font
//               family (FreeSans, regular and bold) at a given size, and
//               drawn icons. canvas.h draws it into a frame buffer, the same
//               on the Atom and on the PC (hosttest/render_screens), and the
//               LCD pushes that whole, only when it changed, so nothing
//               flickers.
//   Pager       which page is shown: the gestures and the return to Home
//
// The pages (2.2): Home is the default and the one the screen goes back to.
// A press steps round the short main ring Home -> Wi-Fi -> VPN -> Home. A
// double-press is the page's one action: on Home it opens the Details
// (setup, call, modes, mail, system, log), on Wi-Fi it joins the next saved
// network, on VPN it changes the mode; in the Details a press is the next
// Details page and a double-press goes back to Home. A hold is config mode,
// as before, and shows the Setup page while the access point is up. What
// the gestures do is shown for a few seconds after each press, in place of
// the page dots. The band's colour is the LED state (modem.h), taken from
// the modem's own LED state machine, so the LCD and the WS2812 boards can
// never disagree.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_STATUS_H
#define ATOM_STATUS_H

#include "modem.h"

namespace am {

static const int kScreenPx = 128;
static const int kScreenCols = 21;      // (a line of the log, before it is fitted by width)
static const int kScreenRows = 16;

enum TScreenPage
	{
	// the main ring: a press steps through these
	EPageHome = 0, EPageNetwork, EPageVpn,
	// the Details: a double-press on Home opens them at Setup
	EPageSetup, EPageCall, EPageModes, EPageMail, EPageSystem, EPageLog,
	EPageCount
	};
static const int kMainPages = EPageSetup;                  // Home, Wi-Fi, VPN
static const int kDetailPages = EPageCount - EPageSetup;

// the font family: FreeSans, drawn smooth at any size (canvas.h)
enum TFace { EFaceSans = 0, EFaceBold, EFaceCount };

// a tone for a text or an icon
enum TTone { EToneWhite = 0, EToneGrey, EToneGreen, EToneAmber, EToneRed, EToneBlue };

// one item on a page, top to bottom, all in the one text size
enum TBlock
	{
	EBlockRow = 0,      // a grey label and its value, right-aligned (the value goes under the label if it is long)
	EBlockKey,          // a value on its own, bold (a network's name, the VPN's state)
	EBlockLine,         // a line of plain text (the log, a note)
	EBlockIcon          // a Home-style row: an icon and a few bold words (the Wi-Fi and VPN pages)
	};
// the icons of the Home-style rows (EBlockIcon) and of the pages' banners
enum TIcon
	{
	EIconNone = 0,
	EIconWifi,          // the Wi-Fi fan: arg = arcs lit 0..3, or 255 none (dim)
	EIconBars,          // signal bars: arg = bars lit 0..4
	EIconBadge,         // a filled badge: arg = TBadge
	EIconLock,          // a padlock: arg = 1 closed
	EIconGlobe,         // an address (IP)
	EIconClock,         // a time (the handshake)
	EIconData,          // arrows down and up (data in and out)
	EIconPower,         // a mode (on, off, auto)
	EIconServer,        // a server
	EIconHome,          // a house (at home)
	EIconNext           // an arrow (what a double-press does next)
	};
struct Block
	{
	uint8_t kind;       // TBlock
	uint8_t tone;       // TTone of the value (EBlockIcon: of the icon)
	uint8_t icon, arg;  // EBlockIcon: TIcon and its argument
	bool dim;           // EBlockIcon: the words in grey (off, not joined)
	bool name;          // EBlockIcon: a name or address, which may go a size smaller and be cut
	char label[12];
	char text[34];      // (a Wi-Fi name is up to 32)
	};
static const int kBlocks = 12;

struct ScreenPage
	{
	int page;                                  // TScreenPage
	LedState bar;                              // the LED state (the band's colour)
	char title[14];                            // the band's title ("Wi-Fi", "Setup")
	// Home: the state word, then three rows with an icon each: the network
	// (with its signal), the Internet check and the tunnel
	char state[14];                            // "Ready", "In call", "No Wi-Fi", "Setup"
	char net[34];
	bool netDim;                               // saved but not joined: grey
	int signal;                                // Wi-Fi bars 0..4, or -1 (none)
	char inet[22];
	int inetTone;
	char vpn[22];
	int vpnTone;
	bool vpnLocked;                            // the padlock closed (the tunnel up or coming up)
	// the Wi-Fi and VPN pages: Home's band (p.state, coloured by bandTone,
	// with the page's icon) over three EBlockIcon rows
	int bandTone;                              // TTone
	int bandIcon;                              // TIcon
	// the other pages
	Block block[kBlocks];
	int blocks;
	char hint[32];                             // the gestures on this page
	bool hintOn;                               // show the hint (just after a press), else the page dots
	};

// ----- the drawing ----------------------------------------------------------
enum TDrawOp
	{
	EOpFill = 0,        // x, y, w, h
	EOpText,            // font = TFace; x, y = the baseline; h = the size (the height of a capital, px)
	EOpWifi,            // x, y = the centre of its foot; w = the size; h = arcs lit 0..3
	EOpBadge,           // a filled circle with a mark: x, y = centre; w = radius; h = TBadge
	EOpLock,            // a padlock: x, y = centre; w = the size; h = 1 closed, 0 open
	EOpDot,             // x, y = centre; w = radius
	EOpIcon,            // x, y = centre; w = the size; h = TIcon; font = its argument
	EOpPicture          // the layout's picture (Layout::pic): x, y = its top left; w, h = its size
	};
enum TBadge { EBadgeCheck = 0, EBadgeCross, EBadgeDots, EBadgeDash, EBadgeInfo };
struct DrawOp
	{
	uint8_t op;                                // TDrawOp
	uint8_t font;                              // TFace (text)
	int16_t x, y;
	int16_t w, h;
	uint16_t fg;                               // RGB565
	char text[34];
	};
static const int kDrawOps = 48;
struct Layout
	{
	DrawOp op[kDrawOps];
	int n;
	int dropped;                               // blocks that did not fit (the tests want none)
	// at most one grey picture (an extension module's page, ExtPen::Picture):
	// w x h bytes, 0 black .. 255 white, rows top down, owned by whoever laid
	// the page out and kept until it is drawn. picGen changes when its pixels
	// do, so two layouts still compare with memcmp
	const uint8_t* pic;
	uint32_t picGen;
	};

// An extension module's page on the LCD (ext.h: Extension::ScreenDraw) is
// drawn with this: the status pages' own fonts (TFace, at any size: aSize is
// the height of a capital in pixels), tones (TTone), icons (TIcon) and
// measures, on kScreenPx x kScreenPx pixels from the top left, and one grey
// picture. The page starts black. Text that does not fit in aMaxW is cut
// with "..".
class ExtPen
	{
public:
	static const int kSize = kScreenPx;
	static const int kText = 10;               // the pages' one text size
	static const int kSmall = 8;               // the gestures' hint
	static const int kMargin = 8;              // left and right
	static const int kBandH = 22;              // the band at the top of a page
	static const int kHomeBandH = 32;          // Home's band (and the Wi-Fi and VPN pages')
	explicit ExtPen(Layout& aLayout) : iL(aLayout) {}
	uint16_t Tone(int aTone) const;            // a TTone in RGB565, as the pages show it
	void Fill(int aX, int aY, int aW, int aH, uint16_t aColour);
	void Text(int aFace, int aSize, int aX, int aBase, uint16_t aColour, const char* aText, int aMaxW = kScreenPx);
	void Right(int aFace, int aSize, int aRight, int aBase, uint16_t aColour, const char* aText, int aMaxW = kScreenPx);
	void Centre(int aFace, int aSize, int aBase, uint16_t aColour, const char* aText);
	int TextWidth(int aFace, int aSize, const char* aText) const;
	// the largest size from aMax down to aMin at which aText is no wider than aW
	int FitSize(int aFace, const char* aText, int aW, int aMax, int aMin) const;
	void Dot(int aX, int aY, int aRadius, uint16_t aColour);
	// a row icon (TIcon, with its argument as the pages use it), centred on (aX, aY)
	void Icon(int aIcon, int aArg, int aX, int aY, uint16_t aColour);
	// a box with rounded corners (fills and dots: the canvas's own primitives)
	void RoundBox(int aX, int aY, int aW, int aH, int aRadius, uint16_t aColour);
	// the page's one picture: aW x aH greys (0 black .. 255 white, rows top
	// down) at (aX, aY). aGreys must stay put until the page is drawn; aGen
	// must change whenever its pixels do (the page is compared, not the pixels)
	void Picture(int aX, int aY, int aW, int aH, const uint8_t* aGreys, uint32_t aGen);
	// steps that did not fit in the layout (the tests want none)
	int Dropped() const { return iL.dropped; }
private:
	Layout& iL;
	};

class StatusModel
	{
public:
	// aPage is taken modulo EPageCount. (aHal is not const: its info calls are not.)
	static void Fill(const Modem& aModem, Hal& aHal, int aPage, ScreenPage& aOut);
	// the page as drawing steps; the same page gives the same bytes, so the
	// screen can compare two layouts with memcmp
	static void Lay(const ScreenPage& aPage, Layout& aOut);
	// a message over the whole screen (the reset countdown, what a
	// double-press chose): a grey line, a big one, a grey one
	static void LayMessage(const char* aLine1, const char* aLine2, const char* aLine3, Layout& aOut);
	// an extension module's own page (Extension::ScreenDraw), on black
	static void LayExt(Extension& aExt, uint32_t aNowMs, Layout& aOut);
	static int Pages() { return EPageCount; }
	// "1.2K", "34M": a byte count in at most 5 characters
	static void Count(uint32_t aBytes, char* aOut, size_t aMax);
	// the pixel width of aText in aFace with capitals aSize pixels high (as
	// canvas.h draws it)
	static int TextWidth(int aFace, int aSize, const char* aText);
	// UTF-8 to the font's ASCII: curly quotes and dashes as their look-alikes,
	// accented letters as the plain letter, anything else '?'
	static void Clean(char* aOut, const char* aText, size_t aMax);
	};

// RGB565, as the LCD takes it
static inline uint16_t Rgb565(int aR, int aG, int aB)
	{
	return (uint16_t)(((aR & 0xf8) << 8) | ((aG & 0xfc) << 3) | (aB >> 3));
	}

// ----- the gestures ----------------------------------------------------------
class Pager
	{
public:
	enum TAction { ENothing = 0, ENextNet, ECycleVpn };
	// back to Home after this long without a press (the Details: longer, as
	// there is more to read; the Setup page stays while the access point is up)
	static const uint32_t kHomeAfterMs = 30 * 1000;
	static const uint32_t kDetailsHomeAfterMs = 90 * 1000;
	// the gestures' hint shows this long after a press
	static const uint32_t kHintMs = 4000;

	Pager() : iPage(EPageHome), iLastMs(0), iPressed(false) {}
	int Page() const { return iPage; }
	bool InDetails() const { return iPage >= EPageSetup; }
	// a press: the next page of the ring it is in
	void Click(uint32_t aNowMs);
	// a double-press: the page's action. Home opens the Details, the Details
	// go back to Home (both ENothing); Wi-Fi and VPN ask the caller to act
	TAction Double(uint32_t aNowMs);
	// the hold turned config mode on (the Setup page, with the access point's
	// name and password) or off (Home)
	void ConfigMode(bool aOn, uint32_t aNowMs);
	// once a tick: true if the page went back to Home. aKeep: stay (the Setup
	// page while the access point is up)
	bool Idle(uint32_t aNowMs, bool aKeep);
	// the hint, just after a press (and just after start)
	bool HintOn(uint32_t aNowMs) const { return aNowMs - iLastMs < kHintMs; }
	void Show(int aPage, uint32_t aNowMs) { iPage = aPage; iLastMs = aNowMs; iPressed = true; }
	// an extension module's page may be shown over the pages now (ext.h:
	// Extension::ScreenWanted): on Home, with no press for as long as a page
	// stays before it goes back to Home. So a press shows the modem's pages
	// again, for a while; the Setup page (config mode) always wins
	bool Quiet(uint32_t aNowMs) const { return iPage == EPageHome && (!iPressed || aNowMs - iLastMs >= kHomeAfterMs); }

private:
	int iPage;
	uint32_t iLastMs;               // the last press
	bool iPressed;                  // (a press since start)
	};

} // namespace am

#endif
