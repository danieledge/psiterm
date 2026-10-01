// test_modem.cpp - unit tests for the Atom modem's AT parser, +++ escape
// and pacing, against fakehal.h (simulated time, UART, TCP, WiFi, NVS).
//   make -C hosttest test
// MIT licence (see LICENSE at the top of the repository).
#include "fakehal.h"
#include <stdlib.h>
#include <algorithm>

static int gChecks = 0, gFails = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Rig
	{
	FakeHal hal;
	std::vector<uint8_t> ring;
	am::Modem* modem;
	size_t mark = 0;
	explicit Rig(size_t aRing = 96 * 1024) : ring(aRing)
		{
		modem = new am::Modem(hal, ring.data(), ring.size());
		modem->Begin();
		Run(10);
		}
	~Rig() { delete modem; }
	// run the modem for aMs of simulated time, in 50 us steps
	void Run(uint32_t aMs)
		{
		for (uint64_t t = 0; t < (uint64_t)aMs * 1000; t += 50)
			{
			modem->Loop();
			hal.Advance(50);
			}
		}
	void Type(const char* s) { while (*s) hal.rx.push_back((uint8_t)*s++); }
	void TypeRun(const char* s, uint32_t ms = 50) { Type(s); Run(ms); }
	// what the Psion received since the last call
	std::string Got()
		{
		std::string w = hal.Wire();
		std::string s = w.substr(mark);
		mark = w.size();
		return s;
		}
	void Feed(const std::string& s) { for (char c : s) hal.server.push_back((uint8_t)c); }
	};

static bool Has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

static void TestBasics()
	{
	Rig r;
	r.TypeRun("AT\r");
	std::string g = r.Got();
	CHECK(g == "AT\r\r\nOK\r\n");                              // echo, then OK: as a WiRSa
	r.TypeRun("ATE0\r");
	CHECK(r.Got() == "ATE0\r\r\nOK\r\n");
	r.TypeRun("AT\r");
	CHECK(r.Got() == "\r\nOK\r\n");                              // no echo now
	r.TypeRun("at\r");
	CHECK(r.Got() == "\r\nOK\r\n");
	r.TypeRun("ATX\xff\r");
	CHECK(Has(r.Got(), "ERROR"));
	r.TypeRun("ATI\r");
	g = r.Got();
	CHECK(g.find("\r\nAtom modem 1.0 (Psion-tuned)") == 0);     // the first line: the name
	CHECK(Has(g, "IP 192.168.1.50"));
	CHECK(Has(g, "pacing 5500 bytes/s"));
	CHECK(g.size() >= 6 && g.substr(g.size() - 6) == "\r\nOK\r\n");
	r.TypeRun("ATE1V0\r");
	CHECK(r.Got() == "0\r");                                     // (E1 echoes from the next line)
	r.TypeRun("ATV1 S12? S2?\r");
	g = r.Got();
	CHECK(Has(g, "040") && Has(g, "043") && Has(g, "OK"));
	r.TypeRun("ATS12=50\r");
	CHECK(r.modem->Config().s12 == 50);
	r.TypeRun("A/");                                             // repeat, no Enter
	CHECK(Has(r.Got(), "OK"));
	// line noise before AT (a Remote link frame) is skipped; a line with no AT is ignored
	{ const char noise[] = { 0x16, 0x10, 0x02, '$', 0x00, 0x00, (char)0xd0, 'T', 0x10, 0x03, 'A', 'T', '\r' };
	  for (char c : noise) r.hal.rx.push_back((uint8_t)c); r.Run(50); }
	CHECK(Has(r.Got(), "\r\nOK\r\n"));
	r.TypeRun("hello\r");
	CHECK(!Has(r.Got(), "OK") );
	r.TypeRun("AT&C1&D2&K0\r");                                  // init strings must not fail
	CHECK(Has(r.Got(), "OK"));
	r.TypeRun("AT\x08\x08" "AT\r");                              // backspace
	CHECK(Has(r.Got(), "OK"));
	}

static void TestDial()
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r");
	std::string g = r.Got();
	CHECK(Has(g, "\r\nCONNECT 115200\r\n"));
	CHECK(r.hal.tcpHost == "example.com" && r.hal.tcpPort == 22);
	CHECK(r.modem->Online() && r.hal.dcd);
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 300);   // psiglue's hang-up
	g = r.Got();
	CHECK(Has(g, "OK\r\n") && !r.hal.tcpOpen && !r.modem->Connected());
	CHECK(r.hal.toServer.find('+') == std::string::npos);        // the escape never reached the server

	r.TypeRun("ATDTimap.example.com:993\r");                     // no space (ATDThost:port)
	CHECK(Has(r.Got(), "CONNECT") && r.hal.tcpHost == "imap.example.com" && r.hal.tcpPort == 993);
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH0\r", 100); r.Got();

	r.TypeRun("ATDT\"bbs.example.org:6400\"\r");                 // quoted (WiFi232)
	CHECK(r.hal.tcpHost == "bbs.example.org" && r.hal.tcpPort == 6400);
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 100); r.Got();

	r.TypeRun("ATD telnet.example.org\r");                       // no port: 23
	CHECK(r.hal.tcpHost == "telnet.example.org" && r.hal.tcpPort == 23);
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 100); r.Got();

	r.TypeRun("ATDT777\r");                                      // a phone number (PPP): not here
	CHECK(Has(r.Got(), "NO CARRIER") && !r.modem->Connected());
	r.TypeRun("ATDT host:99999\r");
	CHECK(Has(r.Got(), "ERROR"));
	r.hal.tcpOk = false;
	r.TypeRun("ATDT nowhere.example:22\r");
	CHECK(Has(r.Got(), "NO CARRIER") && !r.modem->Online());
	r.hal.tcpOk = true;
	r.hal.wifi = false;
	r.TypeRun("ATDT example.com:22\r");
	CHECK(Has(r.Got(), "NO CARRIER"));
	r.hal.wifi = true;
	}

static void TestData()
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r");
	r.Got();
	r.TypeRun("SSH-2.0-PsiTerm\r\n");
	CHECK(r.hal.toServer == "SSH-2.0-PsiTerm\r\n");
	r.Feed("SSH-2.0-OpenSSH_9.6\r\n");
	r.Run(20);
	CHECK(r.Got() == "SSH-2.0-OpenSSH_9.6\r\n");
	// AT in data mode is data
	r.TypeRun("AT\r");
	CHECK(Has(r.hal.toServer, "AT\r") && r.Got().empty());
	}

static void TestEscape()
	{
	// 1. a proper escape: quiet, +++, quiet -> OK, the call stays up; ATO resumes
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.TypeRun("data", 1000);
	r.TypeRun("+++", 900);
	CHECK(Has(r.Got(), "\r\nOK\r\n") && !r.modem->Online() && r.modem->Connected());
	CHECK(r.hal.toServer == "data");
	r.TypeRun("ATO\r");
	CHECK(Has(r.Got(), "CONNECT") && r.modem->Online());
	r.TypeRun("more");
	CHECK(r.hal.toServer == "datamore");
	}
	// 2. no guard time before: the pluses are data
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.TypeRun("x", 100);
	r.TypeRun("+++", 1500);
	CHECK(r.modem->Online() && r.hal.toServer == "x+++" && r.Got().empty());
	}
	// 3. data straight after the pluses: no escape, nothing lost
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.Run(1000);
	r.TypeRun("+++y", 1500);
	CHECK(r.modem->Online() && r.hal.toServer == "+++y");
	}
	// 4. two pluses then quiet: data after the guard time
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.Run(1000);
	r.TypeRun("++", 500);
	CHECK(r.hal.toServer.empty());                               // held while it might be an escape
	r.Run(600);
	CHECK(r.hal.toServer == "++" && r.modem->Online());
	}
	// 5. four pluses: data
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.Run(1000);
	r.TypeRun("++++", 1500);
	CHECK(r.hal.toServer == "++++" && r.modem->Online());
	}
	// 6. pluses typed slowly (more than the guard apart): data
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.Run(1000);
	r.TypeRun("+", 900); r.TypeRun("+", 900); r.TypeRun("+", 1500);
	CHECK(r.modem->Online() && r.hal.toServer == "+++");
	}
	// 7. the escape character can be changed (S2) and the guard time (S12)
	{
	Rig r;
	r.TypeRun("ATS2=42S12=10\r"); r.Got();
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.Run(250);
	r.TypeRun("***", 250);
	CHECK(!r.modem->Online() && Has(r.Got(), "OK"));
	}
	}

static void TestCarrier()
	{
	// the server closes: its last bytes, then NO CARRIER, then commands work
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.Feed("bye\r\n");
	r.hal.tcpOpen = false;                                       // (data still queued)
	r.Run(50);
	std::string g = r.Got();
	CHECK(g == "bye\r\n\r\nNO CARRIER\r\n");
	CHECK(!r.modem->Online() && !r.modem->Connected() && !r.hal.dcd);
	r.TypeRun("AT\r");
	CHECK(Has(r.Got(), "OK"));
	}
	// the WiFi drops mid-call
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.hal.wifi = false;
	r.Run(20);
	CHECK(Has(r.Got(), "NO CARRIER") && !r.modem->Connected() && !r.hal.tcpOpen);
	}
	// closed while in command mode after +++: NO CARRIER at once
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.Run(1000); r.TypeRun("+++", 1000); r.Got();
	r.hal.tcpOpen = false;
	r.Run(20);
	CHECK(Has(r.Got(), "NO CARRIER"));
	r.TypeRun("ATO\r");
	CHECK(Has(r.Got(), "NO CARRIER"));
	}
	// &C0: DCD always on
	{
	Rig r;
	r.TypeRun("AT&C0\r", 20);
	CHECK(r.hal.dcd);
	r.TypeRun("AT&C1\r", 20);
	CHECK(!r.hal.dcd);
	}
	}

// the most bytes the Psion gets in any window of aUs
static size_t MaxInWindow(const std::vector<TxByte>& w, size_t aFrom, uint64_t aUs)
	{
	size_t best = 0, j = aFrom;
	for (size_t i = aFrom; i < w.size(); i++)
		{
		while (w[i].us - w[j].us >= aUs) j++;
		best = std::max(best, i - j + 1);
		}
	return best;
	}

static void TestPacing()
	{
	// 1. a burst from the server at 115200: about 5500 bytes/s, never more than
	//    a 16-byte burst at the line rate, nothing lost, in order
	{
	Rig r;
	r.TypeRun("ATDT example.com:443\r"); r.Got();
	size_t from = r.hal.wire.size();
	std::string blob;
	for (int i = 0; i < 40000; i++) blob += (char)('a' + (i * 7) % 26);
	r.Feed(blob);
	r.Run(1000);
	size_t inOneSecond = r.hal.wire.size() - from;
	printf("   115200: %zu bytes in the first second; ring %zu\n", inOneSecond, r.modem->Buffered());
	CHECK(inOneSecond >= 5300 && inOneSecond <= 5800);
	size_t burst = MaxInWindow(r.hal.wire, from, 1400);        // 1.4 ms = 16 bytes at 115200
	printf("   most bytes in any 1.4 ms: %zu, in any 10 ms: %zu\n", burst, MaxInWindow(r.hal.wire, from, 10000));
	CHECK(burst <= 17);
	CHECK(MaxInWindow(r.hal.wire, from, 10000) <= 16 + 56);       // 5500 B/s plus one bucket
	r.Run(7000);
	std::string got = r.Got();
	CHECK(got == blob);
	}
	// 2. back-pressure: a small ring, a big download - the socket is read
	//    only as the ring drains, and every byte still arrives in order
	{
	Rig r(8192);
	r.TypeRun("ATDT example.com:443\r"); r.Got();
	std::string blob;
	for (int i = 0; i < 30000; i++) blob += (char)(i * 31 % 251);
	r.Feed(blob);
	r.Run(100);
	CHECK(r.modem->Buffered() <= 8192);
	CHECK(r.hal.server.size() >= 30000 - 8192 - 1000);          // the rest waits at the server
	size_t peak = 0;
	for (int i = 0; i < 60; i++) { r.Run(100); peak = std::max(peak, r.modem->Buffered()); }
	CHECK(peak <= 8192);
	CHECK(r.Got() == blob);
	CHECK(r.hal.server.empty());
	}
	// 3. 57600: auto pacing 4000 bytes/s; AT$SB answers at the old speed first
	{
	Rig r;
	r.TypeRun("AT$SB=57600\r", 20);
	CHECK(Has(r.Got(), "OK") && r.hal.baud == 57600);
	r.TypeRun("AT$PR?\r", 50);
	CHECK(Has(r.Got(), "AUTO 4000"));
	r.TypeRun("ATDT example.com:443\r"); r.Got();
	size_t from = r.hal.wire.size();
	r.Feed(std::string(20000, 'x'));
	r.Run(1000);
	size_t n = r.hal.wire.size() - from;
	printf("   57600: %zu bytes in the first second\n", n);
	CHECK(n >= 3850 && n <= 4200);
	}
	// 4. pacing off: the line rate
	{
	Rig r;
	r.TypeRun("AT$PR=0\r", 20); r.Got();
	r.TypeRun("ATDT example.com:443\r"); r.Got();
	size_t from = r.hal.wire.size();
	r.Feed(std::string(30000, 'x'));
	r.Run(1000);
	size_t n = r.hal.wire.size() - from;
	printf("   no pacing: %zu bytes in the first second\n", n);
	CHECK(n >= 11000);
	}
	// 5. manual: 2000 bytes/s in 64-byte bursts with 20 ms gaps
	{
	Rig r;
	r.TypeRun("AT$PR=2000\r", 20); r.TypeRun("AT$PB=64\r", 20); r.TypeRun("AT$PG=20\r", 20); r.Got();
	r.TypeRun("ATDT example.com:443\r"); r.Got();
	size_t from = r.hal.wire.size();
	r.Feed(std::string(10000, 'x'));
	r.Run(2000);
	size_t n = r.hal.wire.size() - from;
	printf("   manual 2000/64/20: %zu bytes in 2 s, most in 20 ms: %zu\n", n, MaxInWindow(r.hal.wire, from, 20000));
	CHECK(n >= 3700 && n <= 4200);
	CHECK(MaxInWindow(r.hal.wire, from, 20000) <= 64);
	}
	// 6. the keystroke echo is not held back: one byte goes at once
	{
	Rig r;
	r.TypeRun("ATDT example.com:22\r"); r.Got();
	r.Feed("k");
	r.Run(1);
	CHECK(r.Got() == "k");
	}
	}

static void TestSettings()
	{
	Rig r;
	r.TypeRun("AT$SSID=Home WiFi\r"); r.TypeRun("AT$PASS=\"pa ss,word\"\r");
	CHECK(r.hal.wifiSsid == "Home WiFi" && r.hal.wifiPass == "pa ss,word");
	r.TypeRun("AT$SSID?\r");
	CHECK(Has(r.Got(), "Home WiFi"));
	r.TypeRun("AT$PASS?\r");
	std::string g = r.Got();
	CHECK(Has(g, "(set)") && !Has(g, "pa ss"));
	r.TypeRun("AT$PR=3000\r");
	r.TypeRun("AT&W\r");
	CHECK(r.hal.haveSaved && strcmp(r.hal.saved.ssid, "Home WiFi") == 0 && r.hal.saved.paceRate == 3000);
	r.TypeRun("AT$PR=AUTO\r");
	CHECK(r.modem->Config().paceRate == 5500);
	r.TypeRun("ATZ\r");
	CHECK(r.modem->Config().paceRate == 3000 && !r.modem->Config().paceAuto);
	r.TypeRun("AT&F\r");                                         // factory, WiFi kept
	CHECK(r.modem->Config().paceAuto && strcmp(r.modem->Config().ssid, "Home WiFi") == 0);
	r.TypeRun("ATW\"Other,secret\"\r");                           // Zimodem
	CHECK(r.hal.wifiSsid == "Other" && r.hal.wifiPass == "secret");
	r.TypeRun("AT$SB=12345\r");
	CHECK(Has(r.Got(), "ERROR"));
	r.TypeRun("AT$SWAP=1\r");
	CHECK(r.modem->Config().swapPins == 1);
	r.TypeRun("AT$DCD=33\r");
	CHECK(r.modem->Config().dcdPin == 33);
	r.TypeRun("AT$NOPE=1\r");
	CHECK(Has(r.Got(), "ERROR"));
	// a fresh modem picks the saved settings up
	Rig r2;
	r2.hal.saved = r.hal.saved; r2.hal.haveSaved = true;
	r2.modem->Begin();
	CHECK(strcmp(r2.modem->Config().ssid, "Home WiFi") == 0 && r2.hal.wifiSsid == "Home WiFi");
	}

// exactly what PsiTerm/PsiMail/PsiWeb send (psiglue.cpp): the dial, and
// the Connection settings test
static void TestPsiglue()
	{
	Rig r;
	// pg_dial: "\r", wait, AT (wants OK within 0.8 s), ATDT host:port
	r.TypeRun("\r", 300); r.Got();
	r.TypeRun("AT\r", 100);
	CHECK(Has(r.Got(), "\r\nOK\r\n"));
	r.TypeRun("ATDT imap.example.com:993\r", 100);
	std::string g = r.Got();
	CHECK(g.find("ATDT imap.example.com:993\r") == 0);          // its echo (psiglue skips it)
	CHECK(Has(g, "\r\nCONNECT 115200\r\n"));
	// TLS bytes both ways, including 0x11/0x13 and '+'
	std::string bin;
	for (int i = 0; i < 256; i++) bin += (char)i;
	r.Type(bin.c_str()); for (int i = 1; i < 256; i++) r.hal.rx.push_back((uint8_t)i);
	r.Run(50);
	r.Feed(bin);
	r.Run(200);
	CHECK(r.Got() == bin);
	// pg_hangup: 1.1 s, +++, 1.1 s, ATH
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 300);
	CHECK(!r.hal.tcpOpen);
	// the Connection settings test (pg_link_test): CR, AT, ATI
	r.Got();
	r.TypeRun("\r", 200); r.TypeRun("AT\r", 200);
	CHECK(Has(r.Got(), "OK"));
	r.TypeRun("ATI\r", 300);
	g = r.Got();
	CHECK(g.find("ATI\r\r\nAtom modem") == 0);
	}

int main()
	{
	printf("basics\n");   TestBasics();
	printf("dial\n");     TestDial();
	printf("data\n");     TestData();
	printf("escape\n");   TestEscape();
	printf("carrier\n");  TestCarrier();
	printf("pacing\n");   TestPacing();
	printf("settings\n"); TestSettings();
	printf("psiglue\n");  TestPsiglue();
	printf("%d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
	}
