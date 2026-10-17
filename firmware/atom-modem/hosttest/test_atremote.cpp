// test_atremote.cpp - AT commands over PPP (2.2, src/atremote.h): the filter
// that refuses what would cut the PPP link or change the serial line, one
// client's session (lines, back-pressure, the idle timeout, the answer's
// bound), and Modem::RemoteLine (always verbose, ATZ keeping the live link,
// a PPP call surviving WiFi switches). MIT licence.
#include "fakehal.h"
#include "../src/atremote.h"
#include <string>
#include <vector>

static int gChecks = 0, gFails = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool Has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

struct Rig
	{
	FakeHal hal;
	std::vector<uint8_t> ring;
	am::Modem* modem;
	size_t mark = 0;
	Rig() : ring(96 * 1024)
		{
		modem = new am::Modem(hal, ring.data(), ring.size());
		modem->Begin();
		Run(10);
		}
	~Rig() { delete modem; }
	void Run(uint32_t aMs)
		{
		for (uint64_t t = 0; t < (uint64_t)aMs * 1000; t += 50)
			{
			modem->Loop();
			hal.Advance(50);
			}
		}
	void TypeRun(const char* s, uint32_t ms = 50) { while (*s) hal.rx.push_back((uint8_t)*s++); Run(ms); }
	std::string Got()
		{
		std::string w = hal.Wire();
		std::string s = w.substr(mark);
		mark = w.size();
		return s;
		}
	std::string Remote(const char* aLine, size_t aMax = 8192)
		{
		std::vector<char> out(aMax);
		size_t n = modem->RemoteLine(aLine, out.data(), aMax);
		return std::string(out.data(), n);
		}
	};

static bool Allowed(const char* aLine)
	{
	char why[48];
	const char* p = aLine;
	if ((p[0] == 'A' || p[0] == 'a') && (p[1] == 'T' || p[1] == 't'))
		p += 2;
	return am::AtRemoteAllowed(p, why, sizeof(why));
	}

static void TestFilter()
	{
	static const char* const kYes[] = { "ATQ0V1", "ATE0", "ATZ", "AT&W", "AT&V", "ATI", "AT", "AT$FC?", "AT$UP?",
		"ATC1", "ATC", "ATS2?", "ATS12?", "AT$SCAN", "AT$STAT?", "AT$JOIN=\"Lab-WiFi\",\"psion5mx\"", "AT$WG=OFF",
		"AT$WG?", "AT$NETS?", "AT$NET=2", "AT$FORGET=1", "AT$SCHEMA?", "AT$LOG?", "AT$HELP", "AT$AP", "AT$PX=2",
		"AT$CHK=1.1.1.1:53", "AT$SB?", "AT$PPP?", "ATW\"a,b\"", "ATW", "AT&K3", "ATL1M0X4", "ATQ0V1E0", "AT$PR=AUTO",
		"AT$SSID=x", "ATS2?S12?", "AT$WEB=ON", "ATK" };
	for (const char* l : kYes)
		{
		bool ok = Allowed(l);
		if (!ok) printf("  refused: %s\n", l);
		CHECK(ok);
		}
	static const char* const kNo[] = { "ATH", "ATH0", "ATQ0H", "AT$SB=57600", "AT$FC=1", "AT$fc=0", "AT$FCSWAP=1",
		"AT$SWAP=1", "AT$PINS=1,2,3,4,5", "AT$DCD=5", "AT$UP=USB", "AT$USB=1", "AT$PPP=0", "ATC0", "ATS2=43",
		"ATS12=50", "AT&F", "AT&C1", "ATDT777", "ATD*99#", "ATO", "ATA", "ATB57600", "AT$RESET=YES", "AT$EXEC=x",
		"ATV1&F" };
	for (const char* l : kNo)
		{
		bool ok = Allowed(l);
		if (ok) printf("  allowed: %s\n", l);
		CHECK(!ok);
		}
	char why[48];
	CHECK(!am::AtRemoteAllowed("H", why, sizeof(why)) && strcmp(why, "it would end the PPP link") == 0);
	CHECK(!am::AtRemoteAllowed("$SB=9600", why, sizeof(why)) && strcmp(why, "it changes the serial line") == 0);
	CHECK(!am::AtRemoteAllowed("$UP=WIFI", why, 4) && strlen(why) == 3);   // (bounded)
	}

static void TestRemoteLine()
	{
	Rig r;
	std::string g = r.Remote("AT");
	CHECK(g == "\r\nOK\r\n");
	CHECK(r.Remote("hello") == "");                                      // no AT: no answer
	CHECK(r.Remote("noise ATI").find("\r\nPsiConnect 2.3") == 0);
	// always in words, even after Q1 V0, which are the serial line's and stay as they were
	CHECK(r.Remote("ATQ1V0") == "\r\nOK\r\n");
	CHECK(r.Remote("AT") == "\r\nOK\r\n");
	CHECK(r.modem->Config().quiet == 0 && r.modem->Config().verbose == 1 && r.modem->Config().echo == 1);
	// refused, with the reason, and nothing done
	g = r.Remote("AT$SB=57600");
	CHECK(g == "\r\n+NOTVIAPPP: it changes the serial line\r\nERROR\r\n");
	CHECK(r.modem->Config().baud == 115200);
	CHECK(r.Remote("AT$FC=1") == "\r\n+NOTVIAPPP: it changes the serial line\r\nERROR\r\n" && r.modem->Config2().flow == 0);
	CHECK(Has(r.Remote("AT$FC?"), "\r\nOK\r\n"));
	CHECK(r.Remote("AT$ZZZ=1") == "\r\nERROR\r\n");                    // unknown: the modem's own ERROR
	// a settings change goes through
	CHECK(r.Remote("AT$PX=2") == "\r\nOK\r\n" && r.modem->Config().proxy == 2);
	// the serial side saw none of it
	CHECK(r.Got() == "");
	CHECK(r.hal.Logged("cmd (ppp): $SB=57600"));
	// a long answer: cut, said so, and the result still there
	g = r.Remote("AT$HELP", 200);
	CHECK(g.size() <= 199 && Has(g, "+TRUNCATED") && g.size() >= 6 && g.substr(g.size() - 6) == "\r\nOK\r\n");
	g = r.Remote("AT&V", 4096);
	CHECK(!Has(g, "+TRUNCATED") && g.substr(g.size() - 6) == "\r\nOK\r\n");
	}

// ATZ over PPP: the saved settings back, the live link kept
static void TestRemoteZ()
	{
	Rig r;
	// saved: 57600, no flow control, PPP off, pacing 1000; live: 115200, RTS/CTS, PPP on
	am::FactoryDefaults(r.hal.saved);
	r.hal.saved.baud = 57600;
	r.hal.saved.proxy = 3;
	r.hal.haveSaved = true;
	am::FactoryDefaults2(r.hal.saved2);
	r.hal.saved2.flow = 0;
	r.hal.saved2.ppp = 0;
	r.hal.haveSaved2 = true;
	r.hal.saved2Len = sizeof(r.hal.saved2);
	r.TypeRun("AT$FC=1\r", 200);
	r.Run(100);
	CHECK(r.modem->Config2().flow == 1);
	int pins = r.hal.pinsApplied;
	uint32_t baud = r.hal.baud;
	CHECK(r.Remote("ATZ") == "\r\nOK\r\n");
	r.Run(200);
	CHECK(r.modem->Config().proxy == 3);                                // reloaded
	CHECK(r.modem->Config().baud == 115200 && r.modem->Config2().flow == 1 && r.modem->Config2().ppp == 1);
	CHECK(r.hal.pinsApplied == pins && r.hal.baud == baud);              // nothing re-applied to the UART
	// a serial ATZ does reload them
	r.TypeRun("ATZ\r", 300);
	CHECK(r.modem->Config().baud == 57600 && r.modem->Config2().ppp == 0);
	}

// a PPP call survives everything PsiConnect does over TCP, and ATH is refused
static void TestPppSurvives()
	{
	Rig r;
	r.TypeRun("ATD777\r", 200);
	CHECK(Has(r.Got(), "CONNECT") && r.modem->PppCall() && r.modem->Online());
	std::string g = r.Remote("AT$JOIN=\"Lab-WiFi\",\"psion5mx\"");
	CHECK(g == "\r\nOK\r\n" && r.hal.wifiSsid == "Lab-WiFi");
	r.Run(300);
	CHECK(r.modem->PppCall() && r.modem->Online());
	CHECK(r.Remote("ATZ") == "\r\nOK\r\n");
	CHECK(r.Remote("ATQ0V1") == "\r\nOK\r\n");
	CHECK(r.Remote("ATC1") == "\r\nOK\r\n" || r.Remote("ATC1") == "\r\nERROR\r\n");
	CHECK(Has(r.Remote("AT$STAT?"), "+STAT:CALL=1"));
	CHECK(r.Remote("AT&W") == "\r\nOK\r\n");
	r.hal.wifi = false;                                                 // the WiFi drops while switching
	r.Run(500);
	r.hal.wifi = true;
	r.Run(500);
	g = r.Remote("ATH");
	CHECK(g == "\r\n+NOTVIAPPP: it would end the PPP link\r\nERROR\r\n");
	CHECK(r.Remote("ATC0").find("+NOTVIAPPP") != std::string::npos);
	CHECK(r.Remote("AT$PPP=0").find("+NOTVIAPPP") != std::string::npos && r.modem->Config2().ppp == 1);
	r.Run(200);
	CHECK(r.modem->PppCall() && r.modem->Online() && r.modem->Connected());
	CHECK(r.Got() == "");                                               // no stray answer down the PPP line
	}

// one client's session: lines, back-pressure, the idle timeout
static void TestSession()
	{
	Rig r;
	static am::AtRemote s;
	s.Begin(1000);
	CHECK(s.CanTake());
	const uint8_t* p;
	CHECK(s.Pending(p) == 0);
	// a line in pieces, with LF and NUL ignored
	const char* a = "AT";
	CHECK(s.Input(*r.modem, (const uint8_t*)a, 2, 1000) == 2 && s.CanTake());
	const uint8_t b[] = { 0, '\n', 'I', '\r', 'A', 'T', '\r' };
	size_t took = s.Input(*r.modem, b, sizeof(b), 1100);
	CHECK(took == 4);                                                   // up to the first answered line
	CHECK(!s.CanTake());
	CHECK(s.Input(*r.modem, b + took, sizeof(b) - took, 1100) == 0);  // back-pressure: nothing taken
	size_t n = s.Pending(p);
	std::string ans((const char*)p, n);
	CHECK(ans.find("\r\nPsiConnect 2.3") == 0 && ans.substr(ans.size() - 6) == "\r\nOK\r\n");
	s.Sent(10);
	CHECK(!s.CanTake() && s.Pending(p) == n - 10);
	s.Sent(n - 10);
	CHECK(s.CanTake() && s.Pending(p) == 0);
	CHECK(s.Input(*r.modem, b + took, sizeof(b) - took, 1200) == 3);
	n = s.Pending(p);
	CHECK(std::string((const char*)p, n) == "\r\nOK\r\n");
	s.Sent(n);
	// a line with no AT: no answer, and the next one is taken at once
	const char* c = "xyz\rAT\r";
	CHECK(s.Input(*r.modem, (const uint8_t*)c, 7, 1300) == 7);
	n = s.Pending(p);
	CHECK(std::string((const char*)p, n) == "\r\nOK\r\n");
	s.Sent(n);
	// an over-long line: refused, and the session goes on
	std::string longLine = "AT$CHK=" + std::string(400, 'x') + "\r";
	CHECK(s.Input(*r.modem, (const uint8_t*)longLine.data(), longLine.size(), 1400) == longLine.size());
	n = s.Pending(p);
	CHECK(std::string((const char*)p, n) == "\r\n+TOOLONG\r\nERROR\r\n");
	s.Sent(n);
	CHECK(strncmp(r.modem->Config2().chkHost, "xxx", 3) != 0);
	const char* d = "AT\r";
	CHECK(s.Input(*r.modem, (const uint8_t*)d, 3, 1500) == 3);
	n = s.Pending(p);
	CHECK(std::string((const char*)p, n) == "\r\nOK\r\n");
	s.Sent(n);
	// the idle timeout, from the last input
	CHECK(!s.Expired(1500 + am::AtRemote::kIdleMs - 1));
	CHECK(s.Expired(1500 + am::AtRemote::kIdleMs));
	// a refused command over the session
	const char* e = "ATH\r";
	s.Input(*r.modem, (const uint8_t*)e, 4, 2000);
	n = s.Pending(p);
	CHECK(Has(std::string((const char*)p, n), "+NOTVIAPPP"));
	s.Sent(n);
	// a big answer stays within the bound
	const char* h = "AT$HELP\r";
	s.Input(*r.modem, (const uint8_t*)h, 8, 2100);
	n = s.Pending(p);
	CHECK(n > 0 && n < am::AtRemote::kOutMax);
	s.Sent(n);
	}

int main()
	{
	TestFilter();
	TestRemoteLine();
	TestRemoteZ();
	TestPppSurvives();
	TestSession();
	printf("test_atremote: %d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
	}
