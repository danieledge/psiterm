// render_screens.cpp - draws the AtomS3R's status pages on the PC, from the
// same Layout the LCD replays (status.h), with the same fonts (M5GFX's 6x8
// Font0 and FreeSans, read from the PlatformIO library folder), so the
// design can be looked at without the hardware. Each page in a few states
// goes to screens/<name>.ppm; mkscreens.py turns them into PNGs (and one
// contact sheet). Needs `pio run -e atoms3r` to have fetched M5GFX once.
//   make -C hosttest screens
// MIT licence (see LICENSE at the top of the repository).
#include "fakehal.h"
#include "../src/status.h"
#include "../src/canvas.h"
#include <stdint.h>
#include <string>
#include <vector>
#include <sys/stat.h>

// ----- the fonts, as M5GFX holds them, and the screen's own drawing ---------------
#define PROGMEM
typedef am::GlyphRec GFXglyph;
struct GFXfont { uint8_t* bitmap; GFXglyph* glyph; uint16_t first, last; uint8_t yAdvance; };
#include "GFXFF/FreeSans24pt7b.h"
#include "GFXFF/FreeSansBold24pt7b.h"

static uint16_t gFb[am::Canvas::kSize * am::Canvas::kSize];

// the frame buffer as the Atom serves it at /screen.bmp
static void Save(const std::string& aName, const am::Layout& l)
	{
	memset(gFb, 0, sizeof(gFb));
	am::Canvas::Render(l, gFb);
	std::string path = "screens/" + aName + ".bmp";
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) { perror(path.c_str()); return; }
	uint8_t h[am::Canvas::kBmpHeader], row[am::Canvas::kBmpRow];
	am::Canvas::BmpHeader(h);
	fwrite(h, 1, sizeof(h), f);
	for (int r = 0; r < am::Canvas::kSize; r++)
		{
		am::Canvas::BmpRow(gFb, r, row);
		fwrite(row, 1, sizeof(row), f);
		}
	fclose(f);
	printf("%s%s\n", path.c_str(), l.dropped ? "  (some rows did not fit)" : "");
	}

// ----- the modem in a few states ------------------------------------------------------
struct Rig
	{
	FakeHal hal;
	std::vector<uint8_t> ring;
	am::Modem* modem = 0;
	Rig() : ring(64 * 1024) {}
	void Start() { modem = new am::Modem(hal, ring.data(), ring.size()); modem->Begin(); Run(10); }
	~Rig() { delete modem; }
	void Run(uint32_t aMs) { for (uint64_t t = 0; t < (uint64_t)aMs * 1000; t += 200) { modem->Loop(); hal.Advance(200); } }
	void TypeRun(const char* s, uint32_t ms = 60) { while (*s) hal.rx.push_back((uint8_t)*s++); Run(ms); }
	void Page(const std::string& aName, int aPage, bool aHint = false)
		{
		am::ScreenPage p;
		am::Layout l;
		am::StatusModel::Fill(*modem, hal, aPage, p);
		p.hintOn = aHint;
		am::StatusModel::Lay(p, l);
		Save(aName, l);
		}
	};

static void Nets(Rig& r, std::initializer_list<const char*> aNames)
	{
	am::NetListInit(r.hal.savedNets);
	for (const char* n : aNames) am::NetUpsert(r.hal.savedNets, n, "password1", -1);
	r.hal.haveNets = true;
	}
static void Wg(Rig& r)
	{
	const char* fake = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";   // 32 zero bytes: not a real key
	std::string text = std::string("[Interface]\nPrivateKey = ") + fake + "\nAddress = 192.168.3.2/32\n\n[Peer]\nPublicKey = "
		+ fake + "\nAllowedIPs = 192.168.10.0/24\nEndpoint = vpn.example.org:51820\n";
	char why[80];
	if (am::WgParse(text.c_str(), r.hal.savedWg, why, sizeof(why)))
		r.hal.haveWg = true;
	else
		printf("wg config: %s\n", why);
	}

int main()
	{
	mkdir("screens", 0755);
	am::Canvas::SetFace(am::EFaceSans, { FreeSans24pt7bBitmaps, FreeSans24pt7bGlyphs, 0x20, 0x7e });
	am::Canvas::SetFace(am::EFaceBold, { FreeSansBold24pt7bBitmaps, FreeSansBold24pt7bGlyphs, 0x20, 0x7e });
	{ Rig r; Nets(r, { "Home", "Phone" }); r.hal.wifiSsid = "Home"; r.hal.internet = 1; r.Start(); r.Run(300);
	  r.Page("01-home-ready", am::EPageHome); r.Page("07-wifi", am::EPageNetwork); r.Page("01-home-hint", am::EPageHome, true); r.Page("09-vpn-off", am::EPageVpn);
	  r.Page("12-modes", am::EPageModes, true); r.Page("11-call-none", am::EPageCall); }
	{ Rig r; Nets(r, { "Home", "Phone", "Office", "Cafe" }); r.hal.wifiSsid = "Home"; r.hal.internet = 1; Wg(r); r.Start();
	  r.TypeRun("AT$WG=ON\r", 1200);
	  r.hal.wgStatus.state = am::EWgStUp; r.hal.wgStatus.handshakeAge = 12; r.hal.wgStatus.rx = 123456; r.hal.wgStatus.tx = 45678;
	  r.Run(1100);
	  r.TypeRun("ATDT example.com:22\r", 100); r.Run(300);
	  for (int i = 0; i < 40; i++) r.hal.server.push_back('x');
	  r.Run(300);
	  r.Page("02-home-incall-vpn", am::EPageHome); r.Page("08-wifi-many", am::EPageNetwork); r.Page("10-vpn-up", am::EPageVpn);
	  r.Page("11-call-tcp", am::EPageCall); }
	{ Rig r; Nets(r, { "Lab-WiFi" }); r.hal.wifiSsid = "Lab-WiFi"; r.hal.internet = 1; Wg(r);
	  r.hal.upIp = am::Ip4(192, 168, 10, 50); r.hal.upMask = am::Ip4(255, 255, 255, 0); r.Start();
	  r.TypeRun("AT$WG=AUTO\r", 1200);
	  r.Page("02-home-athome", am::EPageHome); r.Page("10-vpn-athome", am::EPageVpn); }
	{ Rig r; Nets(r, { "Home" }); r.hal.wifi = false; r.Start(); r.Run(300);
	  r.Page("03-home-nowifi", am::EPageHome); }
	{ Rig r; r.hal.wifi = false; r.Start(); r.Run(300);
	  r.Page("04-home-notsetup", am::EPageHome); r.Page("07-wifi-notsetup", am::EPageNetwork); }
	{ Rig r; Nets(r, { "Robert\xe2\x80\x99s iPhone" }); r.hal.wifiSsid = "Robert\xe2\x80\x99s iPhone"; r.hal.internet = -1; r.Start();
	  r.TypeRun("AT$WG=AUTO\r", 300);
	  r.Page("05-home-iphone-checking", am::EPageHome); }
	{ Rig r; Nets(r, { "Home" }); r.hal.wifiSsid = "Home"; r.hal.internet = 1; r.Start();
	  r.hal.apInfo = "AP \"PsiConnect-1A2B\" up at http://192.168.4.1/";
	  r.hal.apPassword = "psion-1a2bc3d4";
	  r.hal.webInfo = "http://192.168.1.50/";
	  r.modem->ConfigMode(true); r.Run(300);
	  r.Page("06-home-setup", am::EPageHome); r.Page("13-setup-ap", am::EPageSetup);
	  r.hal.apInfo.clear(); r.hal.apPassword.clear(); r.modem->ConfigMode(false); r.Run(300);
	  r.Page("14-setup-off", am::EPageSetup);
	  r.TypeRun("AT$MAILIMAP=imap.example.com\r"); r.TypeRun("AT$MAILSMTP=smtp.example.com\r");
	  r.Page("15-mail", am::EPageMail); r.Page("16-system", am::EPageSystem);
	  r.hal.Log("WiFi: connected, IP 192.168.1.50");
	  r.hal.Log("ATDT example.com:22 -> CONNECT");
	  r.hal.Log("NO CARRIER after 12 s, 1.2K to the Psion");
	  r.Page("17-log", am::EPageLog); }
	{ Rig r; r.Start(); r.Page("15-mail-off", am::EPageMail); }
	// the Wi-Fi page in every state: offline, rejoining, a long name and address, weak
	{ Rig r; Nets(r, { "Home", "Phone" }); r.hal.wifi = false; r.Start(); r.TypeRun("AT$SSID=Home\r"); r.Run(300);
	  r.Page("07-wifi-offline", am::EPageNetwork); r.Page("07-wifi-offline-hint", am::EPageNetwork, true); }
	{ Rig r; Nets(r, { "Home" }); r.hal.wifiSsid = "Home"; r.hal.internet = 1; r.Start(); r.Run(16000);
	  r.hal.wifi = false; r.Run(300);
	  r.Page("07-wifi-joining", am::EPageNetwork); r.Page("03-home-joining", am::EPageHome); }
	{ Rig r; Nets(r, { "Robert\xe2\x80\x99s iPhone", "The Office Guest Network" }); r.hal.wifiSsid = "Robert\xe2\x80\x99s iPhone";
	  r.hal.wifiIp = "172.100.200.250"; r.hal.wifiRssi = -79; r.hal.internet = 1; r.Start(); r.Run(300);
	  r.Page("08-wifi-long", am::EPageNetwork); r.Page("08-wifi-long-hint", am::EPageNetwork, true); }
	// the VPN page in every state
	{ Rig r; Nets(r, { "Home" }); r.hal.wifiSsid = "Home"; r.hal.internet = 1; Wg(r); r.Start(); r.Run(300);
	  r.Page("09-vpn-off-config", am::EPageVpn);
	  r.TypeRun("AT$WG=ON\r", 1200);
	  r.hal.wgStatus.state = am::EWgStConnecting; r.Run(1100);
	  r.Page("10-vpn-connecting", am::EPageVpn);
	  r.hal.wgStatus.state = am::EWgStResolve; r.Run(1100);
	  r.Page("10-vpn-finding", am::EPageVpn);
	  r.hal.wgStatus.state = am::EWgStRetry; snprintf(r.hal.wgStatus.why, sizeof(r.hal.wgStatus.why), "endpoint not found"); r.Run(1100);
	  r.Page("10-vpn-failed", am::EPageVpn); r.Page("02-home-vpn-failed", am::EPageHome);
	  r.hal.wgStatus.state = am::EWgStUp; r.hal.wgStatus.handshakeAge = 312; r.hal.wgStatus.rx = 12345678; r.hal.wgStatus.tx = 2345678; r.Run(1100);
	  r.Page("10-vpn-up-long", am::EPageVpn, true);
	  r.hal.wifi = false; r.Run(1100);
	  r.Page("10-vpn-waiting", am::EPageVpn); }
	{ Rig r; Nets(r, { "Home" }); r.hal.wifiSsid = "Home"; r.hal.internet = 1; r.Start();
	  r.TypeRun("AT$WG=ON\r", 1200);
	  r.Page("10-vpn-notsetup", am::EPageVpn); }
	am::Layout l;
	am::StatusModel::LayMessage("VPN mode", "Auto", "", l); Save("18-toast", l);
	am::StatusModel::LayMessage("Hold for reset", "3..2..", "", l); Save("19-reset", l);
	return 0;
	}
