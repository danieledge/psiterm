// status.cpp - see status.h. MIT licence (see LICENSE at the top of the repository).
#include "status.h"
#include "fontmetrics.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace am {

namespace {

// Text for the screen, cleaned to the fonts' ASCII, and word-wrapped
struct Builder
	{
	// The screen's fonts are ASCII only. Text is UTF-8 (an iPhone's hotspot is
	// "Robert’s iPhone", with U+2019), so each character, not each byte, becomes
	// one cell: the common punctuation as its ASCII look-alike, accented Latin
	// letters as the plain letter, anything else one '?'
	static void Clean(char* aOut, const char* aText, size_t aMax)
		{
		const unsigned char* s = (const unsigned char*)aText;
		size_t n = 0;
		while (*s && n < aMax)
			{
			unsigned c = *s, len = 1;
			if (c >= 0xc0 && c < 0xe0) { c &= 0x1f; len = 2; }
			else if (c >= 0xe0 && c < 0xf0) { c &= 0x0f; len = 3; }
			else if (c >= 0xf0 && c < 0xf8) { c &= 0x07; len = 4; }
			else if (c >= 0x80) len = 0;                // a stray continuation byte
			unsigned i = 1;
			for (; len && i < len; i++)
				{
				if ((s[i] & 0xc0) != 0x80)
					break;                              // cut short: not valid UTF-8
				c = (c << 6) | (s[i] & 0x3f);
				}
			if (len && i < len)
				len = 0;
			aOut[n++] = len ? Ascii(c) : '?';
			s += len ? len : 1;
			}
		aOut[n] = 0;
		}
	static char Ascii(unsigned aC)
		{
		if (aC >= 32 && aC < 127) return (char)aC;
		switch (aC)
			{
			case 0xa0: return ' ';                      // no-break space
			case 0x2018: case 0x2019: case 0x201a: case 0x2032: return '\'';
			case 0x201c: case 0x201d: case 0x201e: case 0x2033: return '"';
			case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
			case 0x2026: return '.';                    // ellipsis
			case 0x2022: case 0xb7: return '*';
			}
		// Latin-1 letters with accents: the plain letter
		static const char kLatin[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
		if (aC >= 0xc0 && aC <= 0xff) return kLatin[aC - 0xc0];
		return '?';
		}
	};


// ----- sizes and colours -------------------------------------------------------

const int kText = 10;           // the one text size of the pages (the height of a capital, px)
const int kSmall = 8;           // the gestures' hint
const int kLogText = 9;         // the log's lines
const int kMargin = 8;          // left and right
const int kBandH = 22;          // the band on the pages other than Home
const int kHomeBandH = 32;
const int kBodyTop = kBandH + 4;
const int kBodyBottom = 116;    // the page dots or the hint below this
const int kRowH = 18;           // a label and its value on one line
const int kStackH = 34;         // a label over its value

const uint16_t kBlack = 0x0000;

uint16_t ToneColour(int aTone)
	{
	switch (aTone)
		{
	case EToneGrey:  return Rgb565(145, 145, 150);
	case EToneGreen: return Rgb565(70, 210, 110);
	case EToneAmber: return Rgb565(250, 175, 40);
	case EToneRed:   return Rgb565(250, 85, 75);
	case EToneBlue:  return Rgb565(90, 155, 255);
		}
	return Rgb565(235, 235, 235);
	}

// the band's colour (the LED state) and the colour of text on it
uint16_t BandColour(LedState aState, uint16_t& aText)
	{
	aText = Rgb565(255, 255, 255);
	switch (aState)
		{
	case ELedNoWifi:     return Rgb565(190, 0, 0);          // red
	case ELedConnecting: aText = kBlack; return Rgb565(230, 160, 0);   // amber
	case ELedWifi:       aText = kBlack; return Rgb565(0, 180, 0);     // green
	case ELedConnected:
	case ELedData:       return Rgb565(0, 0, 220);          // blue (a data flash is the same call: no strobing)
	case ELedConfig:     return Rgb565(150, 0, 170);        // purple: the access point is up
		}
	return Rgb565(80, 80, 80);
	}

// ----- filling a page ---------------------------------------------------------

void Add(ScreenPage& p, int aKind, int aTone, const char* aLabel, const char* aText)
	{
	if (p.blocks >= kBlocks)
		return;
	Block& b = p.block[p.blocks++];
	b.kind = (uint8_t)aKind;
	b.tone = (uint8_t)aTone;
	Builder::Clean(b.label, aLabel ? aLabel : "", sizeof(b.label) - 1);
	Builder::Clean(b.text, aText ? aText : "", sizeof(b.text) - 1);
	}
void Row(ScreenPage& p, const char* aLabel, const char* aText, int aTone = EToneWhite) { Add(p, EBlockRow, aTone, aLabel, aText); }
void Key(ScreenPage& p, const char* aText, int aTone = EToneWhite) { Add(p, EBlockKey, aTone, "", aText); }
void Line(ScreenPage& p, const char* aText, int aTone = EToneWhite) { Add(p, EBlockLine, aTone, "", aText); }
void Copy(char* aOut, size_t aMax, const char* aText) { Builder::Clean(aOut, aText, aMax - 1); }

// aText broken into lines no wider than aMaxW pixels, at spaces (or anywhere
// in a word too long for a line); aPut is called for each
template <class F> int WrapPx(const char* aText, int aFace, int aSize, int aMaxW, int aMaxRows, F aPut)
	{
	int used = 0;
	const char* t = aText;
	char b[64];
	while (*t && used < aMaxRows)
		{
		while (*t == ' ') t++;
		if (!*t) break;
		size_t n = 0, lastSpace = 0;
		while (t[n] && n + 1 < sizeof(b))
			{
			b[n] = t[n];
			b[n + 1] = 0;
			if (StatusModel::TextWidth(aFace, aSize, b) > aMaxW)
				break;
			if (t[n] == ' ') lastSpace = n;
			n++;
			}
		if (t[n] && t[n] != ' ' && lastSpace > 0)
			n = lastSpace;                      // (break at the last space)
		if (n == 0) n = 1;
		memcpy(b, t, n);
		b[n] = 0;
		while (n && b[n - 1] == ' ') b[--n] = 0;
		aPut(b);
		used++;
		t += strlen(b);
		}
	return used;
	}

// "SSID" and "IP" and RSSI out of Hal::WifiInfo's  "name" IP a.b.c.d RSSI -60
void SplitWifi(const char* aInfo, char* aSsid, size_t aSsidMax, char* aIp, size_t aIpMax, int& aRssi)
	{
	aSsid[0] = 0;
	aIp[0] = 0;
	aRssi = 0;
	const char* q1 = strchr(aInfo, '"');
	const char* q2 = q1 ? strrchr(aInfo, '"') : 0;
	if (q1 && q2 && q2 > q1)
		{
		size_t n = (size_t)(q2 - q1 - 1);
		if (n >= aSsidMax) n = aSsidMax - 1;
		memcpy(aSsid, q1 + 1, n);
		aSsid[n] = 0;
		}
	const char* ip = strstr(aInfo, " IP ");
	if (ip)
		{
		ip += 4;
		size_t n = 0;
		while (ip[n] && ip[n] != ' ' && n + 1 < aIpMax) { aIp[n] = ip[n]; n++; }
		aIp[n] = 0;
		}
	const char* r = strstr(aInfo, "RSSI ");
	if (r)
		aRssi = atoi(r + 5);
	}

// "http://192.168.4.1/ ..." -> "192.168.4.1"
void BareHost(const char* aUrl, char* aOut, size_t aMax)
	{
	const char* u = strstr(aUrl, "//");
	u = u ? u + 2 : aUrl;
	size_t n = 0;
	while (u[n] && u[n] != '/' && u[n] != ' ' && n + 1 < aMax) { aOut[n] = u[n]; n++; }
	aOut[n] = 0;
	}

const char* const kWgModes[] = { "Off", "On", "Auto" };

// RSSI (dBm, 0 = unknown) to 0..4 signal bars
int Bars(int aRssi)
	{
	if (aRssi == 0)   return 2;   // joined but no reading: a middling guess
	if (aRssi >= -55) return 4;
	if (aRssi >= -65) return 3;
	if (aRssi >= -73) return 2;
	if (aRssi >= -82) return 1;
	return 0;
	}

int WgModeOf(const Modem& m) { return m.Config2().wgMode <= 2 ? m.Config2().wgMode : 0; }

// the tunnel in a word or two, and its tone: "Up", "At home", "Connecting"
const char* VpnWord(const Modem& m, int& aTone)
	{
	aTone = EToneGrey;
	switch (m.WgWant())
		{
	case EWgWantOff: return "Off";
	case EWgWantNoConfig: aTone = EToneRed; return "No config";
	case EWgWantNoUplink: return "Waiting";
	case EWgWantHome: return "At home";
	default: break;
		}
	switch (m.WgNow().state)
		{
	case EWgStUp: aTone = EToneGreen; return "Up";
	case EWgStTime: aTone = EToneAmber; return "Getting time";
	case EWgStResolve: aTone = EToneAmber; return "Finding server";
	case EWgStConnecting: aTone = EToneAmber; return "Connecting";
	default: aTone = EToneAmber; return "Retrying";
		}
	}

bool ApUp(Hal& h, char* aAp, size_t aMax)
	{
	aAp[0] = 0;
	h.ApInfo(aAp, aMax);
	return aAp[0] != 0;
	}

// HOME: only the essentials. The state word on the band, then three rows
// with an icon each: the network and its signal, the Internet check, the VPN
void PageHome(const Modem& m, Hal& h, ScreenPage& p)
	{
	const char* word = "";
	switch (p.bar)
		{
	case ELedNoWifi:     word = m.Link().Active() == EActiveUsb ? "Ready" : "No Wi-Fi"; break;
	case ELedConnecting: word = "Dialling"; break;
	case ELedWifi:       word = "Ready"; break;
	case ELedConnected:
	case ELedData:       word = "In call"; break;
	case ELedConfig:     word = "Setup"; break;
		}
	Copy(p.state, sizeof(p.state), word);
	p.signal = -1;
	bool wifi = h.WifiUp();
	if (wifi)
		{
		char info[96], ssid[40], ip[24];
		int rssi;
		info[0] = 0;
		h.WifiInfo(info, sizeof(info));
		SplitWifi(info, ssid, sizeof(ssid), ip, sizeof(ip), rssi);
		Copy(p.net, sizeof(p.net), ssid[0] ? ssid : "Wi-Fi");
		p.signal = Bars(rssi);
		}
	else if (m.Link().Active() == EActiveUsb)
		Copy(p.net, sizeof(p.net), "USB tether");
	else if (m.Config().ssid[0])
		{
		Copy(p.net, sizeof(p.net), m.Config().ssid);
		p.netDim = true;
		p.signal = 0;
		}
	else
		{
		Copy(p.net, sizeof(p.net), "Not set up");
		p.netDim = true;
		}
	// the Internet check
	const char* in;
	int tone;
	if (wifi)
		{
		int r = m.Link().Internet();
		in = r == 1 ? "Internet" : r == 0 ? "No Internet" : "Checking";
		tone = r == 1 ? EToneGreen : r == 0 ? EToneRed : EToneAmber;
		}
	else if (m.Link().Active() == EActiveUsb)
		{ in = "Via USB"; tone = EToneGreen; }
	else if (m.Config().ssid[0])
		{
		bool again = m.Link().WifiState() == EWifiRecovering;
		in = again ? "Rejoining" : "Offline";
		tone = again ? EToneAmber : EToneRed;
		}
	else
		{ in = "Hold 2 s"; tone = EToneAmber; }
	Copy(p.inet, sizeof(p.inet), in);
	p.inetTone = tone;
	// the tunnel, in words short enough to stay large
	VpnWord(m, tone);
	const char* v = "VPN off";
	switch (m.WgWant())
		{
	case EWgWantOff: break;
	case EWgWantNoConfig: v = "VPN unset"; break;
	case EWgWantNoUplink: v = "VPN idle"; break;
	case EWgWantHome: v = "VPN home"; break;
	default:
		v = m.WgNow().state == EWgStUp ? "VPN on" : m.WgNow().state == EWgStRetry ? "VPN retry" : "VPN trying";
		break;
		}
	Copy(p.vpn, sizeof(p.vpn), v);
	p.vpnTone = tone;
	p.vpnLocked = m.WgWant() == EWgWantUp;
	char ap[96];
	Copy(p.hint, sizeof(p.hint), ApUp(h, ap, sizeof(ap)) ? "1x Wi-Fi   2x password" : "1x Wi-Fi   2x details");
	}

// a Home-style row: an icon and a few bold words (label: what it is, for
// the tests and the web page; not drawn)
void IconRow(ScreenPage& p, int aIcon, int aArg, int aTone, const char* aLabel, const char* aText,
	bool aDim = false, bool aName = false)
	{
	if (p.blocks >= kBlocks)
		return;
	Add(p, EBlockIcon, aTone, aLabel, aText);
	Block& b = p.block[p.blocks - 1];
	b.icon = (uint8_t)aIcon;
	b.arg = (uint8_t)aArg;
	b.dim = aDim;
	b.name = aName;
	}

// the Wi-Fi fan's arcs lit for 0..4 bars (as Home draws it)
int FanLit(int aBars) { return aBars >= 4 ? 3 : aBars == 3 ? 2 : aBars >= 1 ? 1 : 0; }

// the saved network a double-press would join next, or 0
const char* NextNet(const Modem& m)
	{
	const NetList& l = m.Nets();
	if (l.count < 2)
		return 0;
	int cur = -1;
	for (int i = 0; i < l.count; i++)
		if (strcmp(l.net[i].ssid, m.Config().ssid) == 0)
			cur = i;
	return l.net[(cur + 1) % l.count].ssid;
	}

// the hint "<aHead><aName>", the name cut so that the whole fits the foot
void FitHint(ScreenPage& p, const char* aHead, const char* aName)
	{
	char name[34], t[64];
	Copy(name, sizeof(name), aName);
	for (size_t n = strlen(name); ; n--)
		{
		snprintf(t, sizeof(t), "%s%.*s%s", aHead, (int)n, name, n < strlen(name) ? ".." : "");
		if (n == 0 || StatusModel::TextWidth(EFaceSans, kSmall, t) <= kScreenPx - 2 * kMargin)
			break;
		}
	Copy(p.hint, sizeof(p.hint), t);
	}

// WI-FI: Home's look. The state on the band; the network (the fan lit by
// its signal), the signal in words and dBm, the address. A double-press
// joins the next saved network: the hint names it (and so does a row when
// there is room)
void PageNetwork(const Modem& m, Hal& h, ScreenPage& p)
	{
	Copy(p.title, sizeof(p.title), "Wi-Fi");
	p.bandIcon = EIconWifi;
	const char* next = NextNet(m);
	char b[64];
	if (h.WifiUp())
		{
		char info[96], ssid[40], ip[24];
		int rssi;
		info[0] = 0;
		h.WifiInfo(info, sizeof(info));
		SplitWifi(info, ssid, sizeof(ssid), ip, sizeof(ip), rssi);
		p.signal = Bars(rssi);
		Copy(p.state, sizeof(p.state), "Connected");
		p.bandTone = EToneGreen;
		IconRow(p, EIconWifi, FanLit(p.signal), EToneWhite, "Network", ssid[0] ? ssid : "Joined", false, true);
		if (rssi)
			snprintf(b, sizeof(b), "%d dBm", rssi);   // (the bars say how good)
		else
			snprintf(b, sizeof(b), "No reading");
		IconRow(p, EIconBars, p.signal, p.signal >= 2 ? EToneWhite : EToneAmber, "Signal", b);
		IconRow(p, EIconGlobe, 0, EToneWhite, "IP", ip[0] ? ip : "No address", !ip[0], true);
		}
	else if (m.Config().ssid[0])
		{
		bool again = m.Link().WifiState() == EWifiRecovering;
		Copy(p.state, sizeof(p.state), again ? "Joining" : "No Wi-Fi");
		p.bandTone = again ? EToneAmber : EToneRed;
		IconRow(p, EIconWifi, 255, EToneGrey, "Network", m.Config().ssid, true, true);
		IconRow(p, EIconBadge, again ? EBadgeDots : EBadgeCross, again ? EToneAmber : EToneRed, "State",
			again ? "Rejoining" : "Offline");
		if (m.Link().Active() == EActiveUsb)
			IconRow(p, EIconBadge, EBadgeCheck, EToneGreen, "Uplink", "Via USB");
		else if (next)
			{
			snprintf(b, sizeof(b), "Next: %s", next);
			IconRow(p, EIconNext, 0, EToneGrey, "Next", b, true, true);
			}
		}
	else
		{
		Copy(p.state, sizeof(p.state), "Not set up");
		p.bandTone = EToneAmber;
		IconRow(p, EIconBadge, EBadgeDots, EToneAmber, "Setup", "Hold 2 s");
		IconRow(p, EIconBadge, EBadgeInfo, EToneGrey, "Then", "for setup", true);
		}
	if (next)
		FitHint(p, "1x VPN  2x join ", next);
	else
		Copy(p.hint, sizeof(p.hint), "1x VPN");
	}

// "vpn.example.org:51820" -> "vpn.example.org"
void BareServer(const char* aEndpoint, char* aOut, size_t aMax)
	{
	Copy(aOut, aMax, aEndpoint);
	char* c = strrchr(aOut, ':');
	if (c && strchr(aOut, ':') == c)            // (one colon: a port, not IPv6)
		*c = 0;
	}

// VPN: Home's look. The state on the band; the mode (a double-press changes
// it: Off, On, Auto); then the handshake and the data while it is up, or
// why it is not and the server it would use
void PageVpn(const Modem& m, Hal&, ScreenPage& p)
	{
	Copy(p.title, sizeof(p.title), "VPN");
	Copy(p.hint, sizeof(p.hint), "1x home   2x mode");
	const WgStatus& st = m.WgNow();
	const WgConfig& c = m.Wg();
	int want = m.WgWant();
	bool up = want == EWgWantUp && st.state == EWgStUp;
	p.bandIcon = EIconLock;
	p.signal = want == EWgWantUp ? 1 : 0;      // (the band's padlock: closed while it is wanted up)
	const char* word;
	int tone;
	VpnWord(m, tone);
	switch (want)
		{
	case EWgWantOff:      word = "VPN off"; break;
	case EWgWantNoConfig: word = "Not set up"; break;
	case EWgWantNoUplink: word = "Waiting"; break;
	case EWgWantHome:     word = "At home"; break;
	default:              word = up ? "VPN up" : st.state == EWgStRetry ? "Retrying" : "Connecting"; break;
		}
	Copy(p.state, sizeof(p.state), word);
	p.bandTone = tone;
	int mode = WgModeOf(m);
	char b[64];
	snprintf(b, sizeof(b), "Mode %s", kWgModes[mode]);
	IconRow(p, EIconPower, 0, mode ? EToneWhite : EToneGrey, "Mode", b, mode == 0);
	char server[40];
	BareServer(c.endpoint, server, sizeof(server));
	bool valid = WgValid(c);
	if (up)
		{
		long a = (long)st.handshakeAge;
		if (a < 0) snprintf(b, sizeof(b), "No handshake");
		else if (a < 60) snprintf(b, sizeof(b), "%ld s ago", a);
		else if (a < 3600) snprintf(b, sizeof(b), "%ld min ago", a / 60);
		else snprintf(b, sizeof(b), "%ld h ago", a / 3600);
		IconRow(p, EIconClock, 0, EToneWhite, "Handshake", b);
		char n1[12], n2[12];
		StatusModel::Count(st.rx, n1, sizeof(n1));
		StatusModel::Count(st.tx, n2, sizeof(n2));
		snprintf(b, sizeof(b), "%s / %s", n1, n2);   // (in, out: as the arrows)
		IconRow(p, EIconData, 0, EToneWhite, "Data", b, false, true);
		return;
		}
	switch (want)
		{
	case EWgWantNoConfig:
		IconRow(p, EIconBadge, EBadgeInfo, EToneAmber, "Setup", "No config");
		IconRow(p, EIconGlobe, 0, EToneWhite, "How", "Paste on web", false, true);
		return;
	case EWgWantOff:
		if (!valid)
			{
			IconRow(p, EIconBadge, EBadgeInfo, EToneGrey, "Setup", "No config", true);
			IconRow(p, EIconGlobe, 0, EToneGrey, "How", "Paste on web", true, true);
			return;
			}
		break;
	case EWgWantHome:
		IconRow(p, EIconHome, 0, EToneGreen, "Why", "Home LAN");
		break;
	case EWgWantNoUplink:
		IconRow(p, EIconBadge, EBadgeDots, EToneGrey, "Why", "No uplink");
		break;
	default:
		if (st.state == EWgStRetry)
			{
			// wgtunnel's reasons, in a word or two
			const char* w = st.why;
			if (strstr(w, "not found")) w = "Not found";
			else if (strstr(w, "no answer")) w = "No answer";
			else if (strstr(w, "lost")) w = "Lost link";
			else if (strstr(w, "start") || !w[0]) w = "Failed";
			IconRow(p, EIconBadge, EBadgeCross, EToneRed, "Why", w, false, true);
			}
		else
			IconRow(p, EIconBadge, EBadgeDots, EToneAmber, "State",
				st.state == EWgStTime ? "Set clock" : st.state == EWgStResolve ? "Lookup" : "Handshake");
		break;
		}
	if (valid)
		IconRow(p, EIconServer, 0, EToneGrey, "Server", server, want == EWgWantOff || want == EWgWantHome, true);
	}

// SETUP: what someone needs to join the access point - its name, its
// password and the address to open
void PageSetup(const Modem&, Hal& h, ScreenPage& p)
	{
	Copy(p.title, sizeof(p.title), "Setup");
	char ap[96];
	if (ApUp(h, ap, sizeof(ap)))
		{
		char ssid[40];
		ssid[0] = 0;
		const char* q1 = strchr(ap, '"');
		const char* q2 = q1 ? strchr(q1 + 1, '"') : 0;
		if (q1 && q2 && q2 > q1)
			{
			size_t n = (size_t)(q2 - q1 - 1);
			if (n >= sizeof(ssid)) n = sizeof(ssid) - 1;
			memcpy(ssid, q1 + 1, n);
			ssid[n] = 0;
			}
		Row(p, "Join Wi-Fi", ssid[0] ? ssid : "PsiConnect");
		char pw[40];
		pw[0] = 0;
		h.ApPass(pw, sizeof(pw));
		if (pw[0])
			Row(p, "Password", pw);
		char url[40];
		BareHost(ap, url, sizeof(url));
		if (url[0])
			Key(p, url);                         // (then open this: an address needs no label)
		}
	else
		{
		Line(p, "Access point off", EToneGrey);
		Line(p, "Hold 2 s to start");
		char web[96];
		web[0] = 0;
		h.WebInfo(web, sizeof(web));
		if (web[0])
			{
			char host[40];
			BareHost(web, host, sizeof(host));
			Row(p, "Web pages", host);
			}
		}
	}

// CALL: the call now (or the last one), and the bytes each way
void PageCall(const Modem& m, Hal&, ScreenPage& p)
	{
	Copy(p.title, sizeof(p.title), "Call");
	char b[64];
	if (!m.Connected() && !m.Closing())
		{
		Key(p, "No call", EToneGrey);
		Row(p, "Last", m.LastDial()[0] ? m.LastDial() : "none yet");
		if (m.LastDialResult()[0])
			Row(p, "Result", m.LastDialResult());
		return;
		}
	if (m.PppCall())
		Key(p, m.Ppp().Up() ? "PPP up" : "PPP starting");
	else if (m.ProxyCall())
		{
		Key(p, "Web proxy");
		snprintf(b, sizeof(b), "%u", (unsigned)m.WebProxy().Requests());
		Row(p, "Pages", b);
		}
	else if (m.ExecCall())
		Key(p, "Command");
	else
		{
		Key(p, "TCP");
		Row(p, "To", m.LastDial());
		}
	char n[12];
	StatusModel::Count(m.ToPsion(), n, sizeof(n));
	Row(p, "To Psion", n);
	StatusModel::Count(m.ToServer(), n, sizeof(n));
	Row(p, "To server", n);
	if (!m.Online())
		Line(p, "Command mode: ATO", EToneAmber);
	}

// MODES: the proxy, PPP, TLS and the line
void PageModes(const Modem& m, Hal&, ScreenPage& p)
	{
	Copy(p.title, sizeof(p.title), "Modes");
	char b[64];
	int px = ProxyMode(m.Config());
	if (px == Proxy::EOff)
		Row(p, "Proxy", "Off");
	else
		{
		snprintf(b, sizeof(b), "PX%d", px);   // (its zip and picture options are on the web page)
		Row(p, "Proxy", b);
		}
	Row(p, "PPP", m.Config2().ppp ? "On" : "Off");
	Row(p, "TLS", m.Config2().tls ? "On" : "Off");
	snprintf(b, sizeof(b), "%lu", (unsigned long)m.Config().baud);
	Row(p, "Baud", b);
	Row(p, "RTS/CTS", m.Config2().flow ? "On" : "Off");
	}

// MAIL: exactly what to type into PsiMail (PPP route) to use the TLS-offload
// relay
void PageMail(const Modem& m, Hal&, ScreenPage& p)
	{
	Copy(p.title, sizeof(p.title), "Mail");
	const Settings2& s2 = m.Config2();
	if (MailRelayAny(s2))
		{
		Row(p, "Server", "192.168.7.1");
		if (s2.imapHost[0]) Row(p, "IMAP port", "993");
		if (s2.smtpHost[0]) Row(p, "SMTP port", "465");
		if (s2.popHost[0])  Row(p, "POP port", "995");
		Row(p, "Security", "None");
		}
	else
		{
		Key(p, "Relay off", EToneGrey);
		Line(p, "Set AT$MAILIMAP");
		Line(p, "then AT&W");
		}
	}

// SYSTEM: the version, the board, the uplink, the pages' address, memory
void PageSystem(const Modem& m, Hal& h, ScreenPage& p)
	{
	Copy(p.title, sizeof(p.title), "System");
	Row(p, "Version", kVersion);
	Row(p, "Board", h.BoardName());
	int act = m.Link().Active();
	Row(p, "Uplink", act == EActiveWifi ? "Wi-Fi" : act == EActiveUsb ? "USB" : "None");
	char b[64];
	// "Memory: 120 KB free, ..." -> "120 KB free"
	char mem[200];
	mem[0] = 0;
	h.MemInfo(mem, sizeof(mem));
	const char* k = strstr(mem, ": ");
	const char* e = k ? strchr(k, ',') : 0;
	if (k && e && e - k - 2 < 30)
		{
		snprintf(b, sizeof(b), "%.*s", (int)(e - k - 2), k + 2);
		Row(p, "Memory", b);
		}
	}

// LOG: the last lines, wrapped, newest at the bottom
void PageLog(const Modem&, Hal& h, ScreenPage& p)
	{
	Copy(p.title, sizeof(p.title), "Log");
	const int kMax = 8;                         // the lines between the band and the foot
	char rows[kMax][34];
	int have = 0;
	for (int k = 0; k < 10 && have < kMax; k++)
		{
		char ln[160], clean[160];
		if (!h.LogLines().Line((size_t)k, ln, sizeof(ln)))
			break;
		Builder::Clean(clean, ln, sizeof(clean) - 1);
		char w[kMax][34];
		int n = 0;
		WrapPx(clean, EFaceSans, kLogText, kScreenPx - 2 * kMargin + 4, kMax,
			[&](const char* aRow) { snprintf(w[n++], sizeof(w[0]), "%s", aRow); });
		for (int r = n - 1; r >= 0 && have < kMax; r--)   // (built from the bottom up)
			memcpy(rows[have++], w[r], sizeof(rows[0]));
		}
	for (int i = have - 1; i >= 0; i--)
		Line(p, rows[i]);
	}

// ----- laying a page out ----------------------------------------------------

struct Pen
	{
	Layout& l;
	explicit Pen(Layout& aL) : l(aL) {}
	DrawOp* Op(int aKind, int aX, int aY, int aW, int aH, uint16_t aC)
		{
		if (l.n >= kDrawOps)
			return 0;
		DrawOp* o = &l.op[l.n++];
		o->op = (uint8_t)aKind;
		o->x = (int16_t)aX; o->y = (int16_t)aY; o->w = (int16_t)aW; o->h = (int16_t)aH;
		o->fg = aC;
		return o;
		}
	void Fill(int aX, int aY, int aW, int aH, uint16_t aC) { Op(EOpFill, aX, aY, aW, aH, aC); }
	// aText at (aX, baseline aBase); cut with ".." only if it can be no
	// narrower (a long network name): the pages choose words that fit
	void Text(int aFace, int aSize, int aX, int aBase, uint16_t aC, const char* aText, int aMaxW = kScreenPx)
		{
		DrawOp* o = Op(EOpText, aX, aBase, 0, aSize, aC);
		if (!o)
			return;
		o->font = (uint8_t)aFace;
		snprintf(o->text, sizeof(o->text), "%s", aText);
		if (StatusModel::TextWidth(aFace, aSize, o->text) <= aMaxW)
			return;
		size_t n = strlen(o->text);
		while (n > 0)
			{
			o->text[--n] = 0;
			char t[40];
			snprintf(t, sizeof(t), "%.32s..", o->text);
			if (StatusModel::TextWidth(aFace, aSize, t) <= aMaxW)
				{
				memcpy(o->text, t, strlen(t) + 1);
				return;
				}
			}
		}
	void Right(int aFace, int aSize, int aRight, int aBase, uint16_t aC, const char* aText, int aMaxW)
		{
		int w = StatusModel::TextWidth(aFace, aSize, aText);
		if (w > aMaxW) w = aMaxW;
		Text(aFace, aSize, aRight - w, aBase, aC, aText, aMaxW);
		}
	void Centre(int aFace, int aSize, int aBase, uint16_t aC, const char* aText)
		{
		int w = StatusModel::TextWidth(aFace, aSize, aText);
		int x = w < kScreenPx ? (kScreenPx - w) / 2 : 0;
		Text(aFace, aSize, x, aBase, aC, aText, kScreenPx - 2);
		}
	};

// the largest size from aMax down to aMin at which aText is no wider than aW
int FitSize(int aFace, const char* aText, int aW, int aMax, int aMin)
	{
	int s = aMax;
	while (s > aMin && StatusModel::TextWidth(aFace, s, aText) > aW)
		s--;
	return s;
	}

// the foot: the page dots, or the gestures just after a press
void LayFoot(const ScreenPage& p, Pen& aPen)
	{
	if (p.hintOn && p.hint[0])
		{
		aPen.Centre(EFaceSans, kSmall, 126, ToneColour(EToneGrey), p.hint);
		return;
		}
	bool details = p.page >= EPageSetup;
	int n = details ? kDetailPages : kMainPages;
	int at = details ? p.page - EPageSetup : p.page;
	const int kGap = 9;
	int x0 = (kScreenPx - (n - 1) * kGap) / 2;
	for (int i = 0; i < n; i++)
		aPen.Op(EOpDot, x0 + i * kGap, 122, i == at ? 3 : 2, 0,
			i == at ? ToneColour(EToneWhite) : Rgb565(70, 70, 75));
	}

void LayHome(const ScreenPage& p, Pen& aPen)
	{
	uint16_t fg;
	uint16_t bg = BandColour(p.bar, fg);
	aPen.Fill(0, 0, kScreenPx, kHomeBandH, bg);
	int s = FitSize(EFaceBold, p.state, kScreenPx - 2 * kMargin, 17, 12);
	aPen.Centre(EFaceBold, s, (kHomeBandH + s) / 2, fg, p.state);
	// three rows, an icon and a few words each, all in one size: the
	// largest at which the Internet and VPN words fit (the name may be cut)
	const int kTextX = 30, kW = kScreenPx - kTextX - 4;
	int size = FitSize(EFaceBold, p.inet, kW, 14, 11);
	int s2 = FitSize(EFaceBold, p.vpn, kW, 14, 11);
	if (s2 < size) size = s2;
	int ns = FitSize(EFaceBold, p.net, kW, size, 10);
	const int kRow[3] = { 50, 77, 104 };       // the rows' centres
	uint16_t white = ToneColour(EToneWhite), grey = ToneColour(EToneGrey);
	// the network, with the Wi-Fi fan as its signal
	int lit = p.signal >= 4 ? 3 : p.signal == 3 ? 2 : p.signal >= 1 ? 1 : 0;
	aPen.Op(EOpWifi, 15, kRow[0] + 7, 18, p.signal >= 0 ? lit : -1, p.netDim ? grey : white);
	aPen.Text(EFaceBold, ns, kTextX, kRow[0] + ns / 2, p.netDim ? grey : white, p.net, kW);
	// the Internet check: a badge in its colour
	int badge = p.inetTone == EToneGreen ? EBadgeCheck : p.inetTone == EToneRed ? EBadgeCross : EBadgeDots;
	aPen.Op(EOpBadge, 15, kRow[1], 8, badge, ToneColour(p.inetTone));
	aPen.Text(EFaceBold, size, kTextX, kRow[1] + size / 2, white, p.inet, kW);
	// the VPN: a padlock, closed while the tunnel is wanted up
	aPen.Op(EOpLock, 15, kRow[2], 17, p.vpnLocked ? 1 : 0, ToneColour(p.vpnTone));
	aPen.Text(EFaceBold, size, kTextX, kRow[2] + size / 2, p.vpnTone == EToneGrey ? grey : white, p.vpn, kW);
	LayFoot(p, aPen);
	}

// a band in a tone (the Wi-Fi and VPN pages): the LED states' colours
uint16_t ToneBand(int aTone, uint16_t& aText)
	{
	switch (aTone)
		{
	case EToneGreen: return BandColour(ELedWifi, aText);
	case EToneAmber: return BandColour(ELedConnecting, aText);
	case EToneRed:   return BandColour(ELedNoWifi, aText);
	case EToneBlue:  return BandColour(ELedConnected, aText);
		}
	aText = Rgb565(255, 255, 255);
	return Rgb565(80, 80, 80);
	}

// an icon of a row or a band, centred on (aX, aY)
void LayIcon(Pen& aPen, int aIcon, int aArg, int aX, int aY, uint16_t aC)
	{
	switch (aIcon)
		{
	case EIconNone: break;
	case EIconWifi: aPen.Op(EOpWifi, aX, aY + 7, 18, aArg == 255 ? -1 : aArg, aC); break;
	case EIconBadge: aPen.Op(EOpBadge, aX, aY, 8, aArg, aC); break;
	case EIconLock: aPen.Op(EOpLock, aX, aY, 17, aArg, aC); break;
	default:
		{
		DrawOp* o = aPen.Op(EOpIcon, aX, aY, 17, aIcon, aC);
		if (o) o->font = (uint8_t)aArg;
		break;
		}
		}
	}

// the Wi-Fi and VPN pages, in Home's look: the band with the page's icon
// and its state, then up to three rows of an icon and bold words, all in
// one size (the largest at which every row but the names fits; a name may
// go smaller still, and is cut only if it fits at no size)
void LayIconPage(const ScreenPage& p, Pen& aPen)
	{
	const int kIconX = 15, kTextX = 30, kW = kScreenPx - kTextX - 1;
	uint16_t fg;
	uint16_t bg = ToneBand(p.bandTone, fg);
	aPen.Fill(0, 0, kScreenPx, kHomeBandH, bg);
	// the page's icon, small, at the left of the band; the state centred
	// in the rest of it, in Home's size where it fits
	const int kBandIconX = 10, kBandL = 20, kBandW = kScreenPx - kBandL - 3;
	if (p.bandIcon == EIconLock)
		aPen.Op(EOpLock, kBandIconX, kHomeBandH / 2, 13, p.signal, fg);
	else
		aPen.Op(EOpWifi, kBandIconX, kHomeBandH / 2 + 5, 13, 3, fg);
	int s = FitSize(EFaceBold, p.state, kBandW, 17, 12);
	int w = StatusModel::TextWidth(EFaceBold, s, p.state);
	aPen.Text(EFaceBold, s, kBandL + (kBandW - w) / 2, (kHomeBandH + s) / 2, fg, p.state, kBandW);
	int size = 14;
	for (int i = 0; i < p.blocks && i < 3; i++)
		if (!p.block[i].name)
			{
			int f = FitSize(EFaceBold, p.block[i].text, kW, 14, 11);
			if (f < size) size = f;
			}
	const int kRow[3] = { 50, 77, 104 };       // the rows' centres, as Home's
	uint16_t white = ToneColour(EToneWhite), grey = ToneColour(EToneGrey);
	for (int i = 0; i < p.blocks && i < 3; i++)
		{
		const Block& b = p.block[i];
		int ts = b.name ? FitSize(EFaceBold, b.text, kW, size, 9) : size;
		LayIcon(aPen, b.icon, b.arg, kIconX, kRow[i], ToneColour(b.tone));
		aPen.Text(EFaceBold, ts, kTextX, kRow[i] + ts / 2, b.dim ? grey : white, b.text, kW);
		}
	if (p.blocks > 3)
		aPen.l.dropped = p.blocks - 3;
	LayFoot(p, aPen);
	}

// the band on the other pages: the title (slate grey in the Details)
void LayBand(const ScreenPage& p, Pen& aPen)
	{
	uint16_t fg, bg;
	if (p.page >= EPageSetup)
		{ bg = Rgb565(55, 65, 85); fg = Rgb565(255, 255, 255); }
	else
		bg = BandColour(p.bar, fg);
	aPen.Fill(0, 0, kScreenPx, kBandH, bg);
	aPen.Text(EFaceBold, kText + 1, kMargin, (kBandH + kText + 1) / 2, fg, p.title, kScreenPx - 2 * kMargin);
	}

// a row: the label on the left, the value right-aligned on the same line,
// or under the label (left-aligned, a size smaller if need be) when the two
// do not fit side by side
struct RowFit { bool stacked; int size; };
RowFit FitRow(const Block& b)
	{
	RowFit f = { false, kText };
	const int kW = kScreenPx - 2 * kMargin;
	if (b.kind == EBlockLine)
		{
		f.size = FitSize(EFaceSans, b.text, kW, kText, kText - 2);
		return f;
		}
	if (b.kind == EBlockKey)
		{
		f.size = FitSize(EFaceBold, b.text, kW, kText + 1, kText - 2);
		return f;
		}
	int lw = StatusModel::TextWidth(EFaceSans, kText, b.label);
	int vw = StatusModel::TextWidth(EFaceSans, kText, b.text);
	if (lw + 10 + vw <= kW)
		return f;
	f.stacked = true;
	f.size = FitSize(EFaceSans, b.text, kW, kText, kText - 2);
	return f;
	}

int RowHeight(const Block& b, const RowFit& f)
	{
	if (b.kind == EBlockLine && b.text[0] == 0)
		return kRowH / 2;
	return f.stacked ? kStackH : kRowH;
	}

void LayBlocks(const ScreenPage& p, Pen& aPen)
	{
	int y = kBodyTop;
	const int kRight = kScreenPx - kMargin;
	uint16_t grey = ToneColour(EToneGrey);
	for (int i = 0; i < p.blocks; i++)
		{
		const Block& b = p.block[i];
		RowFit f = FitRow(b);
		int h = RowHeight(b, f);
		if (y + h > kBodyBottom + 2)
			{
			aPen.l.dropped = p.blocks - i;      // (left off: the tests want none)
			break;
			}
		uint16_t c = ToneColour(b.tone);
		int base = y + 2 + kText;
		if (b.kind == EBlockLine)
			aPen.Text(EFaceSans, f.size, kMargin, base, c, b.text, kScreenPx - 2 * kMargin);
		else if (b.kind == EBlockKey)
			aPen.Text(EFaceBold, f.size, kMargin, base, c, b.text, kScreenPx - 2 * kMargin);
		else if (!f.stacked)
			{
			aPen.Text(EFaceSans, kText, kMargin, base, grey, b.label);
			aPen.Right(EFaceSans, kText, kRight, base, c, b.text, kScreenPx - 2 * kMargin);
			}
		else
			{
			aPen.Text(EFaceSans, kText, kMargin, base, grey, b.label);
			aPen.Text(EFaceSans, f.size, kMargin, base + kText + 6, c, b.text, kScreenPx - 2 * kMargin);
			}
		y += h;
		}
	}

void LayLog(const ScreenPage& p, Pen& aPen)
	{
	const int kPitch = kLogText + 3;
	int base = kBodyBottom - 1;
	for (int i = p.blocks - 1; i >= 0 && base - kLogText >= kBodyTop - 2; i--, base -= kPitch)
		aPen.Text(EFaceSans, kLogText, kMargin - 2, base, ToneColour(EToneWhite), p.block[i].text, kScreenPx - kMargin);
	}

const FontMetric& Master(int aFace)
	{
	return aFace == EFaceBold ? kFreeSansBold24pt7bMetric : kFreeSans24pt7bMetric;
	}

} // namespace

void StatusModel::Clean(char* aOut, const char* aText, size_t aMax)
	{
	Builder::Clean(aOut, aText, aMax);
	}

int StatusModel::TextWidth(int aFace, int aSize, const char* aText)
	{
	const FontMetric& f = Master(aFace);
	int units = 0;
	for (const char* t = aText; *t; t++)
		{
		unsigned c = (unsigned char)*t;
		units += f.advance[c >= 0x20 && c <= 0x7e ? c - 0x20 : '?' - 0x20];
		}
	return (int)((float)units * aSize / f.cap + 0.999f);
	}

void StatusModel::Count(uint32_t aBytes, char* aOut, size_t aMax)
	{
	if (aBytes < 10000)
		snprintf(aOut, aMax, "%lu", (unsigned long)aBytes);
	else if (aBytes < 1000000)
		snprintf(aOut, aMax, "%luK", (unsigned long)(aBytes / 1000));
	else if (aBytes < 100000000)
		snprintf(aOut, aMax, "%lu.%luM", (unsigned long)(aBytes / 1000000), (unsigned long)(aBytes / 100000 % 10));
	else
		snprintf(aOut, aMax, "%luM", (unsigned long)(aBytes / 1000000));
	}

void StatusModel::Fill(const Modem& aModem, Hal& aHal, int aPage, ScreenPage& aOut)
	{
	memset(&aOut, 0, sizeof(aOut));
	int page = aPage % EPageCount;
	if (page < 0)
		page += EPageCount;
	aOut.page = page;
	aOut.signal = -1;
	int led = aModem.LedNow();
	aOut.bar = led >= 0 ? (LedState)led : ELedNoWifi;
	switch (page)
		{
	case EPageHome:    PageHome(aModem, aHal, aOut); break;
	case EPageNetwork: PageNetwork(aModem, aHal, aOut); break;
	case EPageVpn:     PageVpn(aModem, aHal, aOut); break;
	case EPageSetup:   PageSetup(aModem, aHal, aOut); break;
	case EPageCall:    PageCall(aModem, aHal, aOut); break;
	case EPageModes:   PageModes(aModem, aHal, aOut); break;
	case EPageMail:    PageMail(aModem, aHal, aOut); break;
	case EPageSystem:  PageSystem(aModem, aHal, aOut); break;
	default:           PageLog(aModem, aHal, aOut); break;
		}
	if (page >= EPageSetup)
		Copy(aOut.hint, sizeof(aOut.hint), "1x next   2x home");
	}

void StatusModel::Lay(const ScreenPage& aPage, Layout& aOut)
	{
	memset(&aOut, 0, sizeof(aOut));             // (padding too: layouts are compared whole)
	Pen pen(aOut);
	pen.Fill(0, 0, kScreenPx, kScreenPx, kBlack);
	if (aPage.page == EPageHome)
		{
		LayHome(aPage, pen);
		return;
		}
	if (aPage.page == EPageNetwork || aPage.page == EPageVpn)
		{
		LayIconPage(aPage, pen);
		return;
		}
	LayBand(aPage, pen);
	if (aPage.page == EPageLog)
		LayLog(aPage, pen);
	else
		LayBlocks(aPage, pen);
	LayFoot(aPage, pen);
	}

void StatusModel::LayMessage(const char* aLine1, const char* aLine2, const char* aLine3, Layout& aOut)
	{
	memset(&aOut, 0, sizeof(aOut));
	Pen pen(aOut);
	pen.Fill(0, 0, kScreenPx, kScreenPx, kBlack);
	char t[34];
	uint16_t grey = ToneColour(EToneGrey);
	Builder::Clean(t, aLine1, sizeof(t) - 1);
	pen.Centre(EFaceSans, FitSize(EFaceSans, t, kScreenPx - 2 * kMargin, kText, 8), 44, grey, t);
	Builder::Clean(t, aLine2, sizeof(t) - 1);
	int s = FitSize(EFaceBold, t, kScreenPx - 2 * kMargin, 18, 10);
	pen.Centre(EFaceBold, s, 64 + s / 2, ToneColour(EToneWhite), t);
	Builder::Clean(t, aLine3, sizeof(t) - 1);
	pen.Centre(EFaceSans, FitSize(EFaceSans, t, kScreenPx - 2 * kMargin, kText - 1, 8), 96, grey, t);
	}

// ----- the gestures ----------------------------------------------------------

void Pager::Click(uint32_t aNowMs)
	{
	iLastMs = aNowMs;
	if (InDetails())
		iPage = iPage + 1 < EPageCount ? iPage + 1 : EPageSetup;
	else
		iPage = (iPage + 1) % kMainPages;
	}

Pager::TAction Pager::Double(uint32_t aNowMs)
	{
	iLastMs = aNowMs;
	switch (iPage)
		{
	case EPageHome: iPage = EPageSetup; return ENothing;
	case EPageNetwork: return ENextNet;
	case EPageVpn: return ECycleVpn;
	default: iPage = EPageHome; return ENothing;
		}
	}

void Pager::ConfigMode(bool aOn, uint32_t aNowMs)
	{
	iLastMs = aNowMs;
	iPage = aOn ? EPageSetup : EPageHome;
	}

bool Pager::Idle(uint32_t aNowMs, bool aKeep)
	{
	if (iPage == EPageHome || aKeep)
		return false;
	uint32_t after = InDetails() ? kDetailsHomeAfterMs : kHomeAfterMs;
	if (aNowMs - iLastMs < after)
		return false;
	iPage = EPageHome;
	return true;
	}

} // namespace am
