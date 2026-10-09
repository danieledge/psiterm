// test_modem.cpp - unit tests for the Atom modem's AT parser, +++ escape
// and pacing, against fakehal.h (simulated time, UART, TCP, WiFi, NVS).
//   make -C hosttest test
// MIT licence (see LICENSE at the top of the repository).
#include "fakehal.h"
#include "../src/status.h"
#include "../src/button.h"
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
	CHECK(g.find("\r\nAtom modem 2.0 (Psion-tuned)") == 0);     // the first line: the name
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
	  for (char c : noise)
		r.hal.rx.push_back((uint8_t)c);
	  r.Run(50); }
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

	r.TypeRun("AT$PPP=0\r"); r.Got();
	r.TypeRun("ATDT777\r");                                      // a phone number, PPP off: goes nowhere
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
	// 6. 230400 and 460800: auto pacing at about half the line rate, 16-byte bursts
	{
	uint32_t rate; uint16_t burst, gap;
	am::AutoPacing(230400, false, rate, burst, gap);
	CHECK(rate == 11000 && burst == 16 && gap == 0);
	am::AutoPacing(460800, false, rate, burst, gap);
	CHECK(rate == 22000 && burst == 16 && gap == 0);
	am::AutoPacing(115200, false, rate, burst, gap);
	CHECK(rate == 5500);
	am::AutoPacing(115200, true, rate, burst, gap);               // RTS/CTS: 80 % of the line rate
	CHECK(rate == 9216 && burst == 16);
	am::AutoPacing(230400, true, rate, burst, gap);
	CHECK(rate == 18432);
	Rig r;
	r.TypeRun("AT$SB=230400\r", 20);
	CHECK(Has(r.Got(), "OK") && r.hal.baud == 230400);
	r.TypeRun("AT$PR?\r", 50);
	CHECK(Has(r.Got(), "AUTO 11000"));
	r.TypeRun("ATDT example.com:443\r"); r.Got();
	size_t from = r.hal.wire.size();
	r.Feed(std::string(40000, 'x'));
	r.Run(1000);
	size_t n = r.hal.wire.size() - from;
	printf("   230400: %zu bytes in the first second, most in 0.7 ms: %zu\n", n, MaxInWindow(r.hal.wire, from, 690));
	CHECK(n >= 10600 && n <= 11500);
	CHECK(MaxInWindow(r.hal.wire, from, 690) <= 17);              // 0.69 ms = 16 bytes at 230400
	}
	{
	Rig r;
	r.TypeRun("ATB460800\r", 20);
	CHECK(r.hal.baud == 460800 && r.modem->Config().paceRate == 22000);
	}
	// 7. the keystroke echo is not held back: one byte goes at once
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

// how many times the baud trial fell back (the log has WiFi lines too now)
static size_t Backs(const Rig& r)
	{
	size_t n = 0;
	for (const std::string& l : r.hal.logs) if (l.find("back to") != std::string::npos) n++;
	return n;
	}

// AT$SB / ATB: a speed nobody answers at goes back to the old one after 15 s
static void TestBaudFallback()
	{
	// 1. nothing at the new speed: back to 115200 after 15 s, said on the console
	{
	Rig r;
	r.TypeRun("AT$SB=230400\r", 20);
	CHECK(Has(r.Got(), "OK") && r.hal.baud == 230400);
	r.Run(14000);
	CHECK(r.hal.baud == 230400 && Backs(r) == 0);
	r.Run(1100);
	CHECK(r.hal.baud == 115200 && r.modem->Config().baud == 115200);
	CHECK(r.modem->Config().paceRate == 5500);                   // the pacing follows
	CHECK(r.hal.Logged("back to 115200"));
	r.TypeRun("AT\r");
	CHECK(Has(r.Got(), "OK"));                                   // reachable again
	r.Run(16000);
	CHECK(r.hal.baud == 115200 && Backs(r) == 1);                // and it stays
	}
	// garbage (no AT, or ERROR) at the new speed does not confirm it
	{
	Rig r;
	r.TypeRun("ATB230400\r", 20);
	r.Run(1000);
	r.TypeRun("\x93\xf1x\r"); r.TypeRun("ATX\xff\r");
	r.Run(15000);
	CHECK(r.hal.baud == 115200 && r.hal.Logged("back to 115200"));
	}
	// 2. a plain AT at the new speed confirms it
	{
	Rig r;
	r.TypeRun("AT$SB=230400\r", 20); r.Got();
	r.Run(2000);
	r.TypeRun("AT\r");
	CHECK(Has(r.Got(), "OK"));
	r.Run(16000);
	CHECK(r.hal.baud == 230400 && r.modem->Config().baud == 230400 && Backs(r) == 0);
	}
	// a change made at the new speed falls back to that speed, which worked
	{
	Rig r;
	r.TypeRun("AT$SB=230400\r", 20); r.Run(1000);
	r.TypeRun("AT$SB=460800\r", 20);
	CHECK(r.hal.baud == 460800);
	r.Run(15100);
	CHECK(r.hal.baud == 230400 && r.hal.Logged("back to 230400"));
	}
	// 3. AT&W at the new speed confirms it (and saves it)
	{
	Rig r;
	r.TypeRun("ATB230400\r", 20); r.Got();
	r.Run(500);
	r.TypeRun("AT&W\r");
	CHECK(r.hal.haveSaved && r.hal.saved.baud == 230400);
	r.Run(16000);
	CHECK(r.hal.baud == 230400 && Backs(r) == 0);
	}
	// 4. no change of speed: nothing to fall back from
	{
	Rig r;
	r.TypeRun("AT$SB=115200\r", 20); r.Got();
	r.Run(16000);
	CHECK(r.hal.baud == 115200 && Backs(r) == 0);
	r.TypeRun("ATB115200\r", 20);
	r.Run(16000);
	CHECK(r.hal.baud == 115200 && Backs(r) == 0);
	}
	// 5. a call dialled at the new speed confirms it; data mode is never touched
	{
	Rig r;
	r.TypeRun("AT$SB=230400\r", 20); r.Got();
	r.TypeRun("ATDT example.com:22\r");
	CHECK(r.modem->Online());
	r.Run(16000);
	CHECK(r.hal.baud == 230400 && r.modem->Online() && Backs(r) == 0);
	}
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

// ===== 2.0 =====================================================================

static_assert(sizeof(am::Settings) == 140, "the 1.x NVS record must keep its size");
static_assert(sizeof(am::Settings2) == 256, "the 2.0 NVS record must keep its size");

// the 2.0 record: defaults, AT&W, ATZ, an older (shorter) record, the factory reset
static void TestSettings2()
	{
	Rig r;
	const am::Settings2& s2 = r.modem->Config2();
	CHECK(s2.magic == am::kMagic2 && s2.uplink == am::EUplinkAuto && s2.flow == 0 && s2.web == am::EWebOn);
	CHECK(s2.tls == 0 && s2.tlsVerify == 1 && s2.img == 1 && s2.imgWidth == 300 && s2.exec == 0);
	CHECK(s2.tlsPorts[0] == 443 && s2.tlsPorts[3] == 995 && s2.tlsPorts[4] == 0);
	CHECK(s2.pins[0] == -1 && s2.pins[4] == -1 && s2.webPass[0] == 0 && s2.execToken[0] == 0);
	// AT&W saves both records; ATZ brings them back
	r.TypeRun("AT$FC=1\r"); r.TypeRun("AT$TLS=1\r"); r.TypeRun("AT$PW=240\r"); r.TypeRun("AT&W\r");
	CHECK(r.hal.haveSaved && r.hal.haveSaved2 && r.hal.saved2.flow == 1 && r.hal.saved2.tls == 1 && r.hal.saved2.imgWidth == 240);
	r.TypeRun("AT$FC=0\r"); r.TypeRun("AT$TLS=0\r");
	CHECK(s2.flow == 0);
	r.TypeRun("ATZ\r");
	CHECK(s2.flow == 1 && s2.tls == 1 && s2.imgWidth == 240);
	// AT&F: factory for both, the WiFi kept (and the link hardware: see TestFactoryKeepsLink)
	r.TypeRun("AT$SSID=Home\r"); r.TypeRun("AT&F\r");
	CHECK(s2.flow == 1 && s2.tls == 0 && strcmp(r.modem->Config().ssid, "Home") == 0);
	r.TypeRun("AT$FC=0\r");
	// a 1.x modem (no cfg2 at all) comes up with the defaults
	{
	Rig r1;
	r1.hal.haveSaved = true; am::FactoryDefaults(r1.hal.saved); r1.hal.saved.baud = 57600;
	r1.hal.haveSaved2 = false;
	r1.modem->Begin();
	CHECK(r1.modem->Config().baud == 57600 && r1.modem->Config2().uplink == am::EUplinkAuto && r1.modem->Config2().imgWidth == 300);
	}
	// an older, shorter cfg2 (a future field missing): the rest at its defaults
	{
	Rig r2;
	am::FactoryDefaults2(r2.hal.saved2);
	r2.hal.saved2.flow = 1;
	r2.hal.saved2.length = 20;              // (as a firmware with a 20-byte record would have saved it)
	r2.hal.saved2Len = 20;
	r2.hal.haveSaved2 = true;
	r2.modem->Begin();
	CHECK(r2.modem->Config2().flow == 1 && r2.modem->Config2().imgWidth == 300 && r2.modem->Config2().tlsPorts[0] == 443);
	CHECK(r2.hal.pinsApplied >= 1 && r2.hal.pins.flow == 1);
	// a record with the wrong magic is ignored
	r2.hal.saved2.magic = 0x12345678; r2.hal.saved2Len = 0;
	r2.modem->Begin();
	CHECK(r2.modem->Config2().flow == 0);
	}
	// AT$RESET=YES: everything, the WiFi too, and the board's FactoryReset
	r.TypeRun("AT$RESET\r");
	CHECK(Has(r.Got(), "ERROR") && !r.hal.reset);
	r.TypeRun("AT$RESET=no\r");
	CHECK(Has(r.Got(), "ERROR") && !r.hal.reset);
	r.TypeRun("AT$RESET=YES\r");
	CHECK(Has(r.Got(), "OK") && r.hal.reset && r.modem->Config().ssid[0] == 0 && s2.flow == 0);
	}

// every setting in the schema answers AT$X? and takes AT$X=, and AT$HELP lists it
static void TestSchema()
	{
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	CHECK(am::SchemaCount() >= 28);
	r.TypeRun("AT$HELP\r", 200);
	std::string help = r.Got();
	for (int i = 0; i < am::SchemaCount(); i++)
		{
		const am::SchemaEntry& e = am::SchemaAt(i);
		CHECK(am::SchemaFind(e.name) == &e);
		CHECK(Has(help, (std::string("$") + e.name + "=").c_str()));
		CHECK(e.help && e.help[0] && e.label && e.label[0]);
		r.TypeRun((std::string("AT$") + e.name + "?\r").c_str(), 100);
		std::string g = r.Got();
		CHECK(Has(g, "\r\nOK\r\n") && g.size() >= 8);
		if (e.type == am::STSecret)
			{
			CHECK(Has(g, "(none)") || Has(g, "(set)"));
			continue;
			}
		// set it to what it already is: accepted (the raw value, as the web
		// page gets it: AT$PX? and AT$UP? answer with words too)
		char raw[100];
		r.modem->WebGet(e, raw, sizeof(raw));
		std::string v = raw;
		if (e.type == am::STPace)
			v = "AUTO";
		r.TypeRun((std::string("AT$") + e.name + "=" + v + "\r").c_str(), 100);
		CHECK(Has(r.Got(), "\r\nOK\r\n"));
		}
	// lower case works, and a bad name does not
	r.TypeRun("at$tlsv?\r"); CHECK(Has(r.Got(), "\r\n1\r\nOK"));
	r.TypeRun("AT$NOSUCH=1\r"); CHECK(Has(r.Got(), "ERROR"));
	r.TypeRun("AT$NOSUCH?\r"); CHECK(Has(r.Got(), "ERROR"));
	// values out of range are refused and nothing changes
	const char* bad[] = { "AT$FC=2", "AT$PW=10", "AT$PW=1000", "AT$PM=1", "AT$LOGL=3", "AT$TLSP=abc", "AT$TLSP=1,2,3,4,5,6,7,8,9",
		"AT$TLSP=70000", "AT$PINS=1,1,2,3,4", "AT$PINS=1,2,3", "AT$PINS=1,2,3,4,99", "AT$UP=SOMETIMES", "AT$UP=3", "AT$WEB=4",
		"AT$APPASS=short", "AT$XH=" "0123456789012345678901234567890123456789012345678901234567890123456789", "AT$SB=12345",
		"AT$PX=5", "AT$DCD=49", 0 };
	for (int i = 0; bad[i]; i++)
		{
		r.TypeRun((std::string(bad[i]) + "\r").c_str(), 100);
		CHECK(Has(r.Got(), "ERROR"));
		}
	CHECK(r.modem->Config2().flow == 0 && r.modem->Config2().imgWidth == 300 && r.modem->Config2().tlsPorts[0] == 443);
	}

// the new settings do what they say
static void TestNewCommands()
	{
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	// flow control: the pins are applied again, AUTO pacing allows more
	int applied = r.hal.pinsApplied;
	r.TypeRun("AT$FC=1\r");
	CHECK(Has(r.Got(), "OK") && r.hal.pinsApplied == applied + 1 && r.hal.pins.flow == 1);
	CHECK(r.modem->Config().paceRate == 9216);
	r.TypeRun("AT$PACE?\r");
	CHECK(Has(r.Got(), "RTS/CTS"));
	r.TypeRun("ATI\r");
	std::string g = r.Got();
	CHECK(Has(g, "RTS/CTS") && Has(g, "Uplink AUTO") && Has(g, "USB-C: device") && Has(g, "TLS termination: off"));
	r.TypeRun("AT$FC=0\r");
	CHECK(r.modem->Config().paceRate == 5500);
	r.TypeRun("AT$FCSWAP=1\r");
	CHECK(r.hal.pins.flowSwap == 1);
	// the pin map
	r.TypeRun("AT$PINS=6,5,7,8,38\r");
	CHECK(Has(r.Got(), "OK") && r.hal.pins.pins[0] == 6 && r.hal.pins.pins[4] == 38);
	r.TypeRun("AT$PINS?\r");
	CHECK(Has(r.Got(), "6,5,7,8,38"));
	r.TypeRun("AT$PINS=-1,-1,-1,-1,-1\r");
	CHECK(Has(r.Got(), "OK") && r.hal.pins.pins[2] == -1);
	// the uplink
	int up = r.hal.uplinkApplied;
	r.TypeRun("AT$UP=USB\r");
	CHECK(Has(r.Got(), "OK") && r.modem->Config2().uplink == am::EUplinkUsb && r.hal.uplinkApplied == up + 1);
	r.TypeRun("AT$UP?\r");
	CHECK(Has(r.Got(), "USB: none in use; WiFi joined, Internet not confirmed (checking); USB no device"));
	r.TypeRun("AT$UP=auto\r");
	r.TypeRun("AT$UP?\r");
	CHECK(Has(r.Got(), "AUTO: WiFi in use"));
	r.TypeRun("AT$UP=1\r");
	CHECK(r.modem->Config2().uplink == am::EUplinkWifi);
	r.TypeRun("AT$USB=1\r");
	CHECK(r.modem->Config2().usbHost == 1 && r.hal.uplinkApplied == up + 4);
	// the web pages and the passwords (never shown)
	int web = r.hal.webApplied;
	r.TypeRun("AT$WEB=AP\r");
	CHECK(r.modem->Config2().web == am::EWebApAlways && r.hal.webApplied == web + 1);
	r.TypeRun("AT$WEBPASS=secret1\r"); r.TypeRun("AT$APPASS=hotspot99\r");
	CHECK(strcmp(r.modem->Config2().webPass, "secret1") == 0 && strcmp(r.modem->Config2().apPass, "hotspot99") == 0);
	r.TypeRun("AT$WEBPASS?\r"); CHECK(Has(r.Got(), "(set)"));
	r.TypeRun("AT$XK?\r"); CHECK(Has(r.Got(), "(none)"));
	r.TypeRun("AT$XK=tok3n\r"); r.TypeRun("AT$XK?\r");
	g = r.Got();
	CHECK(Has(g, "(set)") && !Has(g, "tok3n"));
	r.TypeRun("AT&V\r", 200);
	g = r.Got();
	CHECK(Has(g, "$SSID=") && Has(g, "$WEBPASS=(set)") && Has(g, "$XK=(set)") && !Has(g, "secret1") && !Has(g, "tok3n") && !Has(g, "hotspot99"));
	r.TypeRun("AT$HELP\r", 200);
	g = r.Got();
	CHECK(!Has(g, "secret1") && !Has(g, "tok3n"));
	// TLS: ports as a list
	r.TypeRun("AT$TLSP=993, 465,8443\r");
	CHECK(Has(r.Got(), "OK") && r.modem->Config2().tlsPorts[2] == 8443 && r.modem->Config2().tlsPorts[3] == 0);
	r.TypeRun("AT$TLSP?\r");
	CHECK(Has(r.Got(), "993,465,8443"));
	r.TypeRun("AT$TLSV=0\r");
	CHECK(!r.hal.TlsVerify());
	r.TypeRun("AT$TLSV=1\r");
	CHECK(r.hal.TlsVerify());
	// pictures, exec, log level
	r.TypeRun("AT$PI=1\r"); r.TypeRun("AT$PW=200\r"); r.TypeRun("AT$PM=32\r");
	CHECK(r.modem->Config2().img == 1 && r.modem->Config2().imgWidth == 200 && r.modem->Config2().imgMaxKB == 32);
	r.TypeRun("AT$XE=1\r"); r.TypeRun("AT$XH=pc.local:7777\r"); r.TypeRun("AT$LOGL=2\r");
	CHECK(r.modem->Config2().exec == 1 && strcmp(r.modem->Config2().execHost, "pc.local:7777") == 0 && r.modem->Config2().logLevel == 2);
	r.TypeRun("AT$PX=4\r"); r.TypeRun("AT$PX?\r");
	CHECK(Has(r.Got(), "4 (on: reader)"));
	r.TypeRun("AT$PZ=0\r"); r.TypeRun("AT$PZ?\r");
	CHECK(Has(r.Got(), "0") && r.modem->Config().proxyNoZip == 1);
	// the log ring: what the board logged, oldest first
	r.hal.Log("hello from the test");
	r.TypeRun("AT$LOG?\r", 200);
	g = r.Got();
	CHECK(Has(g, "hello from the test\r\n") && Has(g, "OK"));
	r.TypeRun("AT$AP?\r");
	CHECK(Has(r.Got(), "no access point"));
	// the ring itself: it wraps
	am::LogRing ring;
	for (int i = 0; i < 1000; i++) { char l[40]; snprintf(l, sizeof(l), "line %d", i); ring.Add(l); }
	char buf[5000];
	size_t n = ring.Read(0, buf, sizeof(buf));
	CHECK(n <= am::LogRing::kSize && n > 3000 && strstr(buf, "line 999\n") && !strstr(buf, "line 1\n"));
	}

// TLS termination: dials to the TLS ports, or with tls:, are made by the Atom over TLS
static void TestTlsTermination()
	{
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	// off: a dial to 993 is plain
	r.TypeRun("ATDT imap.example.com:993\r", 100);
	CHECK(Has(r.Got(), "CONNECT") && r.hal.upConnects == 0 && r.hal.tcpPort == 993);
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 100); r.Got();
	// tls: for one dial
	r.TypeRun("ATDT tls:imap.example.com:993\r", 100);
	CHECK(Has(r.Got(), "CONNECT") && r.hal.upConnects == 1 && r.hal.upTls && r.hal.tcpHost == "imap.example.com");
	// data goes both ways as on any call
	r.TypeRun("a001 CAPABILITY\r\n");
	CHECK(r.hal.toServer == "a001 CAPABILITY\r\n");
	r.Feed("* OK ready\r\n"); r.Run(50);
	CHECK(r.Got() == "* OK ready\r\n");
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 100); r.Got();
	// on: the ports in the list
	r.TypeRun("AT$TLS=1\r");
	r.TypeRun("ATDT smtp.example.com:465\r", 100);
	CHECK(Has(r.Got(), "CONNECT") && r.hal.upConnects == 2 && r.hal.upTls);
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 100); r.Got();
	r.TypeRun("ATDT ssh.example.com:22\r", 100);
	CHECK(Has(r.Got(), "CONNECT") && r.hal.upConnects == 2);       // not a TLS port: plain
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 100); r.Got();
	r.TypeRun("AT$TLSP=2222\r");
	r.TypeRun("ATDT host.example.com:2222\r", 100);
	CHECK(r.hal.upConnects == 3 && r.hal.upTls);
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 100); r.Got();
	// a handshake that fails: NO CARRIER, and the reason in the log
	r.hal.tcpOk = false;
	r.TypeRun("ATDT bad.example.com:2222\r", 100);
	CHECK(Has(r.Got(), "NO CARRIER") && !r.modem->Online() && r.hal.Logged("tls bad.example.com:2222"));
	r.hal.tcpOk = true;
	// tls: with nothing after it
	r.TypeRun("ATDT tls:\r", 100);
	CHECK(Has(r.Got(), "ERROR"));
	// psiproxy is never TLS-terminated (it does its own)
	r.TypeRun("ATDT psiproxy:443\r", 100);
	CHECK(Has(r.Got(), "CONNECT") && r.modem->ProxyCall() && r.hal.upConnects == 4);   // (the failed one counted)
	}

// psiexec: the text channel to the helper, and AT$EXEC= one-shots
static void TestExec()
	{
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	// off: refused
	r.TypeRun("ATDT psiexec\r", 100);
	CHECK(Has(r.Got(), "NO CARRIER") && r.hal.tcpHost.empty());
	r.TypeRun("AT$EXEC=uptime\r", 100);
	CHECK(Has(r.Got(), "ERROR"));
	r.TypeRun("AT$XE=1\r");
	r.TypeRun("ATDT psiexec\r", 100);
	CHECK(Has(r.Got(), "NO CARRIER"));                           // no host yet
	r.TypeRun("AT$XH=pc:7777\r"); r.TypeRun("AT$XK=t0k\r"); r.Got();
	// the channel: handshake, then text both ways, NO CARRIER when the helper closes
	r.Feed("OK\r\n");
	r.TypeRun("ATDT psiexec\r", 100);
	CHECK(Has(r.Got(), "CONNECT") && r.hal.tcpHost == "pc" && r.hal.tcpPort == 7777 && r.modem->ExecCall());
	CHECK(r.hal.toServer == "PSIEXEC/1 t0k channel\r\n");
	r.TypeRun("hello\r");
	CHECK(r.hal.toServer == "PSIEXEC/1 t0k channel\r\nhello\r");
	r.Feed("world\r\n"); r.Run(50);
	CHECK(r.Got() == "world\r\n");
	r.hal.tcpOpen = false; r.Run(50);
	CHECK(Has(r.Got(), "NO CARRIER") && !r.modem->Connected());
	// a one-shot: the command line, its output, then OK (not NO CARRIER)
	r.hal.toServer.clear();
	r.Feed("OK\r\n");
	r.TypeRun("AT$EXEC=uptime -p\r", 100);
	CHECK(r.hal.toServer == "PSIEXEC/1 t0k one\r\nuptime -p\n" && r.modem->Online());
	r.Feed("up 3 days\r\n"); r.hal.tcpOpen = false; r.Run(100);
	std::string g = r.Got();
	CHECK(Has(g, "up 3 days\r\n") && Has(g, "\r\nOK\r\n") && !Has(g, "NO CARRIER") && !Has(g, "CONNECT"));
	// refused by the helper: ERROR, the reason logged
	r.Feed("ERR bad token\r\n");
	r.TypeRun("AT$EXEC=uptime\r", 100);
	CHECK(Has(r.Got(), "ERROR") && r.hal.Logged("the helper refused: ERR bad token") && !r.hal.tcpOpen);
	// no answer within 5 s: ERROR
	r.TypeRun("AT$EXEC=uptime\r", 100);
	CHECK(Has(r.Got(), "ERROR") && r.hal.Logged("no answer from the helper"));
	// the helper not reachable
	r.hal.tcpOk = false;
	r.TypeRun("ATDT psiexec\r", 100);
	CHECK(Has(r.Got(), "NO CARRIER"));
	r.hal.tcpOk = true;
	// a bad host:port
	r.TypeRun("AT$XH=pc\r"); r.Feed("OK\r\n");
	r.TypeRun("ATDT psiexec\r", 100);
	CHECK(Has(r.Got(), "NO CARRIER"));
	}

// the uplink manager, on its own and through the modem
static void TestUplink()
	{
	am::Uplink u;
	u.Configure(am::EUplinkAuto);
	u.Tick(1000, true, am::EUsbNone);
	CHECK(u.Up() && u.Active() == am::EActiveWifi);
	u.Tick(2000, true, am::EUsbUp);
	CHECK(u.Active() == am::EActiveUsb);
	u.Tick(3000, false, am::EUsbUp);
	CHECK(u.Active() == am::EActiveUsb);                           // (WiFi down: USB carries on)
	// the USB link goes: held for 5 s (a phone re-enumerating), then WiFi
	u.Tick(4000, true, am::EUsbNoCarrier);
	CHECK(u.Active() == am::EActiveUsb);
	u.Tick(8000, true, am::EUsbNoCarrier);
	CHECK(u.Active() == am::EActiveUsb);
	u.Tick(9100, true, am::EUsbNoCarrier);
	CHECK(u.Active() == am::EActiveWifi);
	u.Tick(9200, false, am::EUsbNone);
	CHECK(!u.Up());
	// it comes back
	u.Tick(9300, false, am::EUsbUp);
	CHECK(u.Active() == am::EActiveUsb);
	// forced modes
	u.Configure(am::EUplinkWifi);
	u.Tick(10000, false, am::EUsbUp);
	CHECK(!u.Up());
	u.Tick(10100, true, am::EUsbUp);
	CHECK(u.Active() == am::EActiveWifi);
	u.Configure(am::EUplinkUsb);
	u.Tick(10200, true, am::EUsbNoCarrier);
	CHECK(!u.Up());
	char d[160];
	u.Describe(d, sizeof(d));
	CHECK(strcmp(d, "USB: none in use; WiFi joined, Internet not confirmed (checking); USB no carrier (tethering off, or an unpaired iPhone)") == 0);
	// the WiFi side: joined is not the Internet
	am::Uplink w;
	w.Configure(am::EUplinkAuto);
	w.Tick(1000, true, am::EUsbNone, -1);
	CHECK(w.WifiState() == am::EWifiAssociated && w.Up());               // (calls allowed: the check may be blocked)
	w.Tick(2000, true, am::EUsbNone, 0);
	CHECK(w.WifiState() == am::EWifiAssociated);
	w.Describe(d, sizeof(d));
	CHECK(Has(d, "WiFi joined, Internet not confirmed (the check failed)"));
	w.Tick(3000, true, am::EUsbNone, 1);
	CHECK(w.WifiState() == am::EWifiInternet);
	w.Describe(d, sizeof(d));
	CHECK(Has(d, "WiFi joined, Internet reachable"));
	// the hotspot goes: recovering (it had the Internet), then down after 10 min
	w.Tick(4000, false, am::EUsbNone, 1);
	CHECK(w.WifiState() == am::EWifiRecovering && !w.Up() && w.Internet() == -1);
	w.Tick(4000 + 9 * 60 * 1000, false, am::EUsbNone, -1);
	CHECK(w.WifiState() == am::EWifiRecovering);
	w.Tick(4000 + 11 * 60 * 1000, false, am::EUsbNone, -1);
	CHECK(w.WifiState() == am::EWifiDown);
	// it comes back: joined, then confirmed again
	w.Tick(5000 + 11 * 60 * 1000, true, am::EUsbNone, -1);
	CHECK(w.WifiState() == am::EWifiAssociated);
	w.Tick(6000 + 11 * 60 * 1000, true, am::EUsbNone, 1);
	CHECK(w.WifiState() == am::EWifiInternet);
	// a network that never had the Internet is plain down when it goes
	am::Uplink w2;
	w2.Tick(100, true, am::EUsbNone, 0);
	w2.Tick(200, false, am::EUsbNone, -1);
	CHECK(w2.WifiState() == am::EWifiDown);
	// through the modem: a USB uplink carries calls with WiFi down
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	r.hal.wifi = false;
	r.TypeRun("ATDT example.com:22\r", 100);
	CHECK(Has(r.Got(), "NO CARRIER"));
	r.hal.usb = am::EUsbUp;
	r.hal.usbDetail = ", 172.20.10.2";
	r.Run(10);
	r.TypeRun("ATI\r", 100);
	CHECK(Has(r.Got(), "USB in use"));
	r.TypeRun("ATDT example.com:22\r", 100);
	CHECK(Has(r.Got(), "CONNECT") && r.modem->Link().Active() == am::EActiveUsb);
	// the USB link drops mid-call: held for 5 s, then (no WiFi) the call ends
	r.hal.usb = am::EUsbNoCarrier;
	r.Run(3000);
	CHECK(r.modem->Connected());
	r.Run(2500);
	CHECK(Has(r.Got(), "NO CARRIER") && !r.modem->Connected());
	r.hal.usb = am::EUsbNone;
	r.hal.wifi = true;
	r.Run(10);
	r.TypeRun("AT$UP=USB\r");
	r.TypeRun("ATDT example.com:22\r", 100);
	CHECK(Has(r.Got(), "NO CARRIER"));
	// config mode: the access point for ten minutes
	int web = r.hal.webApplied;
	r.modem->ConfigMode(true);
	CHECK(r.modem->InConfigMode() && r.hal.webApplied == web + 1);
	r.Run(100);
	CHECK(r.hal.led == am::ELedConfig);
	r.Run(10 * 60 * 1000 + 200);
	CHECK(!r.modem->InConfigMode() && r.hal.webApplied == web + 2);
	}

// a network preset at build time: taken and saved on the first start only
static void TestSeedNetwork()
	{
	Rig r;
	CHECK(r.modem->Config().ssid[0] == 0 && !r.hal.haveSaved);
	int web = r.hal.webApplied;
	r.modem->SeedNetwork("", "");                                // (a plain build)
	CHECK(r.modem->Config().ssid[0] == 0 && !r.hal.haveSaved && r.hal.wifiSsid.empty());
	r.modem->SeedNetwork("Preset", "pre5et-pass");
	CHECK(strcmp(r.modem->Config().ssid, "Preset") == 0 && strcmp(r.modem->Config().pass, "pre5et-pass") == 0);
	CHECK(r.hal.haveSaved && strcmp(r.hal.saved.ssid, "Preset") == 0 && r.hal.wifiSsid == "Preset" && r.hal.wifiPass == "pre5et-pass");
	CHECK(r.hal.webApplied == web + 1 && r.hal.Logged("\"Preset\" was preset") && !r.hal.Logged("pre5et-pass"));
	// a network already saved is never overridden
	r.modem->SeedNetwork("Other", "x");
	CHECK(strcmp(r.modem->Config().ssid, "Preset") == 0);
	Rig r2;
	r2.hal.haveSaved = true; am::FactoryDefaults(r2.hal.saved); strcpy(r2.hal.saved.ssid, "Mine");
	r2.modem->Begin();
	r2.modem->SeedNetwork("Preset", "p");
	CHECK(strcmp(r2.modem->Config().ssid, "Mine") == 0);
	}

// the Internet check and joining again, through the modem
static void TestReachability()
	{
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	// the check runs soon after start-up, against AT$CHK's host, and ATI says
	// the Internet is not confirmed until it passes
	CHECK(r.hal.probes >= 1 && r.hal.probeHost == "1.1.1.1" && r.hal.probePort == 53);
	int probes = r.hal.probes;
	r.TypeRun("ATI\r", 100);
	CHECK(Has(r.Got(), "joined, Internet not confirmed"));
	r.TypeRun("AT$UP?\r", 100);
	CHECK(Has(r.Got(), "(checking)"));
	// unconfirmed: checked again every 15 s; confirmed: every 2 min
	r.Run(16000);
	CHECK(r.hal.probes == probes + 1);
	r.hal.internet = 1;
	r.Run(50);
	r.TypeRun("ATI\r", 100);
	CHECK(Has(r.Got(), "joined, Internet reachable") && r.hal.Logged("WiFi: joined, Internet reachable"));
	probes = r.hal.probes;
	r.Run(60000);
	CHECK(r.hal.probes == probes);
	r.Run(65000);
	CHECK(r.hal.probes == probes + 1);
	// not during a call
	r.TypeRun("ATDT example.com:22\r", 100); r.Got();
	probes = r.hal.probes;
	r.Run(130000);
	CHECK(r.hal.probes == probes);
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 100); r.Got();
	// a failed connect brings the check forward
	r.hal.tcpOk = false;
	probes = r.hal.probes;
	r.TypeRun("ATDT nowhere.example:22\r", 100);
	CHECK(Has(r.Got(), "NO CARRIER") && r.hal.probes == probes + 1);
	r.hal.tcpOk = true;
	// AT$CHK: another host, or none
	r.TypeRun("AT$CHK=example.org:443\r"); r.Got();
	r.Run(130000);
	CHECK(r.hal.probeHost == "example.org" && r.hal.probePort == 443);
	r.TypeRun("AT$CHK=\r"); r.Got();
	probes = r.hal.probes;
	r.Run(130000);
	CHECK(r.hal.probes == probes);
	CHECK(strcmp(r.modem->Config2().chkHost, "") == 0);
	// the hotspot drops: recovering, logged; joined again after 20 s, then every 30 s
	r.TypeRun("AT$SSID=Hotspot\r"); r.TypeRun("AT$PASS=secret12\r"); r.Got();
	r.hal.wifiSsid.clear();
	r.hal.wifi = false;
	r.Run(100);
	CHECK(r.hal.Logged("WiFi: lost, joining again") && !r.modem->Link().Up());
	r.Run(15000);
	CHECK(r.hal.wifiSsid.empty());                              // (not yet)
	r.Run(6000);
	CHECK(r.hal.wifiSsid == "Hotspot" && r.hal.Logged("joining \"Hotspot\" again"));
	r.hal.wifiSsid.clear();
	r.Run(25000);
	CHECK(r.hal.wifiSsid.empty());
	r.Run(6000);
	CHECK(r.hal.wifiSsid == "Hotspot");
	// it is back: joined, and checked again at once (not while it was down)
	r.TypeRun("AT$CHK=1.1.1.1:53\r"); r.Got();
	r.Run(1000);
	probes = r.hal.probes;
	r.hal.wifi = true;
	r.hal.internet = -1;
	r.Run(100);
	CHECK(r.modem->Link().WifiState() == am::EWifiAssociated && r.hal.probes == probes + 1);
	CHECK(r.hal.Logged("WiFi: joined, Internet not confirmed"));
	}

// PPP dial dispatch (the lwIP link itself is verified on hardware; here the
// host stub only flips the mode, so we check a numeric dial routes to PPP,
// data goes into PPP not a TCP server, escape keeps the link up, ATH ends it,
// *99# triggers it too, and an uplink-down or PPP-off dial gives NO CARRIER)
static void TestPpp()
	{
	Rig r;
	// on by default (the Psion's "Psion Internet" is its default first send)
	CHECK(r.modem->Config2().ppp == 1);
	// turned off, a numeric dial goes nowhere
	r.TypeRun("AT$PPP=0\r");
	CHECK(Has(r.Got(), "OK") && r.modem->Config2().ppp == 0);
	r.TypeRun("ATD777\r");
	CHECK(Has(r.Got(), "NO CARRIER") && !r.modem->PppCall());
	// and on again
	r.TypeRun("AT$PPP=1\r");
	CHECK(Has(r.Got(), "OK") && r.modem->Config2().ppp == 1);
	// a numeric dial now brings PPP up
	r.TypeRun("ATD777\r");
	CHECK(Has(r.Got(), "CONNECT") && r.modem->PppCall() && r.modem->Online() && r.hal.dcd);
	// Psion data goes into the PPP link, not to any TCP server
	r.TypeRun("pppdata", 1000);
	CHECK(r.hal.toServer.empty() && !r.hal.tcpOpen);
	// +++ escapes to command mode; the PPP link stays up (ATO/ATH can follow)
	r.TypeRun("+++", 900);
	CHECK(Has(r.Got(), "\r\nOK\r\n") && !r.modem->Online() && r.modem->PppCall());
	// ATH tears the link down
	r.TypeRun("ATH\r");
	CHECK(Has(r.Got(), "OK") && !r.modem->PppCall() && !r.modem->Connected());
	// a *99# dial string triggers PPP too
	r.TypeRun("ATDT*99#\r");
	CHECK(Has(r.Got(), "CONNECT") && r.modem->PppCall());
	r.Run(1000);                    // guard-time quiet before the escape
	r.TypeRun("+++", 900); r.Got();
	r.TypeRun("ATH\r"); r.Got();
	CHECK(!r.modem->PppCall());     // torn down before the next dial
	// uplink down: NO CARRIER even with PPP on
	r.hal.wifi = false;
	r.Run(300);
	r.TypeRun("ATD777\r");
	CHECK(Has(r.Got(), "NO CARRIER") && !r.modem->PppCall());
	}

// ----- the button: debounce, click, hold, boot-hold -----------------------------
struct BtnRig
	{
	am::Button b;
	uint32_t t = 1000;
	int clicks = 0, holds = 0;
	void Step(int aLevel, uint32_t aMs)          // aLevel held for aMs, looked at every 5 ms
		{
		for (uint32_t i = 0; i < aMs; i += 5)
			{
			am::Button::TEvent e = b.Feed(aLevel, t);
			if (e == am::Button::EClick) clicks++;
			if (e == am::Button::EHold) holds++;
			t += 5;
			}
		}
	};

static void TestButton()
	{
	{ BtnRig r;  r.Step(1, 100);  r.Step(0, 200);  r.Step(1, 100);       // a click
	  CHECK(r.clicks == 1 && r.holds == 0); }
	{ BtnRig r;  r.Step(1, 100);  r.Step(0, am::Button::kClickMaxMs - 10);  r.Step(1, 100);   // just under the click limit: still a click
	  CHECK(r.clicks == 1 && r.holds == 0); }
	{ BtnRig r;  r.Step(1, 100);  r.Step(0, 1000); r.Step(1, 100);       // 1 s: neither on the LCD board (limit 700), a click without it (limit 1900)
	  CHECK(r.holds == 0 && r.clicks == (am::Button::kClickMaxMs >= 1000 ? 1 : 0)); }
	{ BtnRig r;  r.Step(1, 100);  r.Step(0, am::Button::kClickMaxMs + 50); r.Step(1, 100);   // just over the click limit: neither
	  CHECK(r.clicks == 0 && r.holds == 0); }
	{ BtnRig r;  r.Step(1, 100);  r.Step(0, 3000); r.Step(1, 100);       // a hold: once, and its release is not a click
	  CHECK(r.clicks == 0 && r.holds == 1); }
	{ BtnRig r;  r.Step(1, 100);  r.Step(0, 1990);                        // not yet
	  CHECK(r.holds == 0);
	  r.Step(0, 30);
	  CHECK(r.holds == 1); }
	{ BtnRig r;  r.Step(1, 100);  r.Step(0, 10);  r.Step(1, 100);        // a 10 ms glitch is not a press
	  CHECK(r.clicks == 0 && r.holds == 0); }
	{ BtnRig r;  r.Step(1, 100);  r.Step(0, 25);  r.Step(1, 100);        // 25 ms: seen, but too short for a click
	  CHECK(r.clicks == 0 && r.holds == 0); }
	{ BtnRig r;  r.Step(1, 100);                                          // contact bounce on press and release: one click
	  for (int i = 0; i < 4; i++) { r.Step(0, 5); r.Step(1, 5); }
	  r.Step(0, 250);
	  for (int i = 0; i < 4; i++) { r.Step(1, 5); r.Step(0, 5); }
	  r.Step(1, 100);
	  CHECK(r.clicks == 1 && r.holds == 0); }
	{ BtnRig r;  r.Step(1, 100);  r.Step(0, 200);  r.Step(1, 50);        // two clicks
	  r.Step(0, 200);  r.Step(1, 100);
	  CHECK(r.clicks == 2); }
	{ BtnRig r;                                                           // held at power-on: counted from the first look
	  uint32_t start = r.t;
	  r.Step(0, 10);
	  CHECK(r.b.Down() && r.b.HeldMs(r.t) < 100);
	  r.Step(0, 3000);
	  CHECK(r.b.HeldMs(r.t) >= 3000 && r.t - start >= 3000);
	  r.Step(1, 50);
	  CHECK(!r.b.Down() && r.b.HeldMs(r.t) == 0); }
	}

// ----- the status screen's text -------------------------------------------------
static bool OnHero(const am::ScreenPage& p, const char* aText)
	{
	for (int i = 0; i < am::kHeroRows; i++)
		{
		char joined[48];
		snprintf(joined, sizeof(joined), "%s %s", p.hero[i].label, p.hero[i].value);
		if (strstr(joined, aText) || strstr(p.hero[i].value, aText))
			return true;
		}
	return false;
	}
static bool OnPage(const am::ScreenPage& p, const char* aText)
	{
	for (int i = 0; i < am::kScreenRows; i++)
		if (strstr(p.line[i], aText))
			return true;
	return false;
	}

static void TestStatusModel()
	{
	am::ScreenPage p;
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	// no WiFi, none set: the bar is red and the page says how to set it up
	r.hal.wifi = false;
	r.Run(300);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(p.bar == am::ELedNoWifi && OnPage(p, "STATUS") && OnPage(p, "1/4") && OnPage(p, "No WiFi network set"));
	CHECK(OnPage(p, "Call: none"));
	// the big hero view carries the same state, word for word
	CHECK(strcmp(p.state, "OFFLINE") == 0 && OnHero(p, "Not set") && OnHero(p, "Hold button"));
	for (int i = 0; i < am::kScreenRows; i++)
		CHECK(strlen(p.line[i]) <= (size_t)am::kScreenCols);
	// a network saved, not joined
	r.TypeRun("AT$SSID=Home\r"); r.Got();
	r.Run(300);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(OnPage(p, "WiFi Home") && OnPage(p, "Not connected") && p.bar == am::ELedNoWifi);
	// joined
	r.hal.wifi = true; r.hal.wifiSsid = "Home";
	r.Run(300);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(p.bar == am::ELedWifi && OnPage(p, "WiFi Home") && OnPage(p, "IP   192.168.1.50") && OnPage(p, "RSSI -60 dBm"));
	CHECK(OnPage(p, "115200 baud, FC off") && OnPage(p, "To Psion  0"));
	CHECK(strcmp(p.state, "ONLINE") == 0 && OnHero(p, "Home") && OnHero(p, "IP 192.168.1.50") && OnHero(p, "Ready to dial") && p.signal == 3);
	// a TCP call
	r.TypeRun("ATDT example.com:22\r", 100); r.Run(300);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(p.bar == am::ELedConnected && OnPage(p, "Call: tcp") && OnPage(p, "example.com:22"));
	CHECK(strcmp(p.state, "CONNECTED") == 0 && OnHero(p, "example.com:22"));
	r.Feed("0123456789abcdef"); r.Run(200);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(OnPage(p, "To Psion  16"));
	CHECK(OnHero(p, "P 16"));
	// the modes page, mid call: the last dial
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageModes, p);
	CHECK(OnPage(p, "MODES") && OnPage(p, "TCP: ready") && OnPage(p, "PPP: on") && OnPage(p, "TLS: off")
		&& OnPage(p, "Uplink AUTO->WIFI") && OnPage(p, "example.com:22") && OnPage(p, "CONNECT") && OnPage(p, "Proxy PX=1 zip"));
	// the server closes: NO CARRIER, and the bar and the call are back
	r.hal.tcpOpen = false; r.Run(300);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(p.bar == am::ELedWifi && OnPage(p, "Call: none"));
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageModes, p);
	CHECK(OnPage(p, "NO CARRIER") && OnPage(p, "example.com:22"));
	// a call that is refused: the result is there too
	r.hal.tcpOk = false;
	r.TypeRun("ATDT nowhere.example:22\r", 100); r.Run(300);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageModes, p);
	CHECK(OnPage(p, "nowhere.example:22") && OnPage(p, "NO CARRIER"));
	r.hal.tcpOk = true;
	// the proxy
	r.TypeRun("ATDT psiproxy\r", 100); r.Run(300);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(OnPage(p, "Call: proxy 0 req") && p.bar == am::ELedConnected);
	r.Run(1100); r.TypeRun("+++", 1100);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(OnPage(p, "command mode"));
	r.TypeRun("ATH\r", 100); r.Got();
	// PPP
	r.TypeRun("ATD777\r", 100); r.Run(300);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(OnPage(p, "Call: PPP up") && p.bar == am::ELedConnected);
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 100); r.Got();
	// psiexec
	r.TypeRun("AT$XE=1\r"); r.TypeRun("AT$XH=pc:7777\r"); r.TypeRun("AT$XK=t0k\r"); r.Got();
	r.Feed("OK\r\n");
	r.TypeRun("ATDT psiexec\r", 100); r.Run(300);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(OnPage(p, "Call: exec"));
	r.hal.tcpOpen = false; r.Run(300); r.Got();
	// config mode: purple
	r.modem->ConfigMode(true); r.Run(300);
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageStatus, p);
	CHECK(p.bar == am::ELedConfig);
	r.modem->ConfigMode(false); r.Run(300);
	// the setup page: the access point with its password
	r.hal.apInfo = "AP \"AtomModem-1a2b\" up at http://192.168.4.1/";
	r.hal.apPassword = "psion-12345678";
	r.hal.webInfo = "http://192.168.1.50/";
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageSetup, p);
	CHECK(OnPage(p, "SETUP") && OnPage(p, "AtomModem-1a2b") && OnPage(p, "psion-12345678") && OnPage(p, "http://192.168.4.1/")
		&& OnPage(p, "http://192.168.1.50/") && OnPage(p, "Atom modem 2.0") && OnPage(p, "FakeBoard") && OnPage(p, "Heap"));
	r.hal.apInfo.clear(); r.hal.apPassword.clear();
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageSetup, p);
	CHECK(OnPage(p, "AP: off") && !OnPage(p, "psion-1234"));
	// the log page: the newest line at the bottom, wrapped at 21
	r.hal.Log("a log line that is longer than twenty-one columns for sure");
	r.hal.Log("last line");
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageLog, p);
	CHECK(OnPage(p, "LOG") && strcmp(p.line[am::kScreenRows - 1], "last line") == 0 && OnPage(p, "a log line that is") && OnPage(p, "twenty-one columns") && OnPage(p, "for sure"));
	for (int i = 0; i < am::kScreenRows; i++)
		CHECK(strlen(p.line[i]) <= (size_t)am::kScreenCols);
	// the pages wrap round
	am::StatusModel::Fill(*r.modem, r.hal, 4, p);
	CHECK(OnPage(p, "1/4"));
	// the byte counts
	char c[12];
	am::StatusModel::Count(999, c, sizeof(c)); CHECK(strcmp(c, "999") == 0);
	am::StatusModel::Count(12345, c, sizeof(c)); CHECK(strcmp(c, "12K") == 0);
	am::StatusModel::Count(3400000, c, sizeof(c)); CHECK(strcmp(c, "3.4M") == 0);
	}

// ----- PPP output: whole frames, in order, paced, held when offline -----------
static std::string Frame(int aSeed, size_t aLen)
	{
	std::string f;
	for (size_t i = 0; i < aLen; i++)
		f += (char)(0x7e ^ (aSeed * 31 + (int)i * 7));
	return f;
	}

// lwIP flushes a frame in ~1.5 KB chunks, the last ending in the 0x7e flag
// (Frame() bytes are never 0x7e except by construction below: the tail byte)
static std::string PppFrame(int aSeed, size_t aLen)
	{
	std::string f = Frame(aSeed, aLen);
	for (size_t i = 0; i < f.size(); i++)
		if ((uint8_t)f[i] == 0x7e)
			f[i] = 0x55;
	f[f.size() - 1] = 0x7e;
	return f;
	}
static void PushChunks(am::PppLink& aPpp, const std::string& aF, size_t aChunk = 600)
	{
	for (size_t o = 0; o < aF.size(); o += aChunk)
		aPpp.QueueOut((const uint8_t*)aF.data() + o, aF.size() - o < aChunk ? aF.size() - o : aChunk);
	}

static am::PppLink& PppOf(Rig& r) { return const_cast<am::PppLink&>(r.modem->Ppp()); }

static void TestPppDrain()
	{
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	r.TypeRun("ATD777\r", 100);
	CHECK(Has(r.Got(), "CONNECT") && r.modem->PppCall());
	am::PppLink& ppp = PppOf(r);
	// a 1500-byte frame through the 128-byte FIFO arrives whole
	std::string f1 = PppFrame(1, 1500), f2 = PppFrame(2, 1500);
	PushChunks(ppp, f1);
	r.Run(800);
	CHECK(r.Got() == f1);
	CHECK(ppp.QueuedBytes() == 0 && ppp.DroppedFrames() == 0 && r.modem->ToPsion() >= 1500);
	// two frames: in order, nothing between
	PushChunks(ppp, f1);
	PushChunks(ppp, f2);
	r.Run(1500);
	CHECK(r.Got() == f1 + f2);
	// paced: no faster than the pacer's rate (5500 bytes/s at 115200 without RTS/CTS)
	PushChunks(ppp, f1);
	uint64_t t0 = r.hal.nowUs;
	size_t before = r.hal.wire.size();
	r.Run(100);
	size_t in100 = r.hal.wire.size() - before;
	CHECK(in100 > 0 && in100 <= 5500 / 10 + 40);
	r.Run(800); r.Got();
	(void)t0;
	// +++: command mode. Nothing leaks while offline
	r.Run(1100); r.TypeRun("+++", 1100);
	CHECK(Has(r.Got(), "OK") && !r.modem->Online() && r.modem->PppCall());
	PushChunks(ppp, f2);
	r.Run(1000);
	CHECK(r.Got().empty() && ppp.QueuedBytes() == f2.size());
	// ATO: the link resumes, and the held frame goes out whole
	r.TypeRun("ATO\r", 50);
	std::string g;
	r.Run(800);
	g = r.Got();
	CHECK(g.size() >= f2.size() && g.substr(g.size() - f2.size()) == f2);
	CHECK(ppp.QueuedBytes() == 0);
	// a full queue drops whole frames, and counts them
	r.Run(1100); r.TypeRun("+++", 1100); r.Got();
	for (int i = 0; i < 20; i++)
		PushChunks(ppp, f1);
	size_t fit = am::kPppOutQueue / f1.size();
	CHECK(ppp.QueuedBytes() == fit * f1.size());
	CHECK(ppp.DroppedFrames() == 20 - fit);
	// ... and what was kept is whole frames
	r.TypeRun("ATO\r", 50);
	r.Run(4000);
	g = r.Got();
	size_t at = g.find(f1);
	CHECK(at != std::string::npos);
	CHECK(g.size() - at == fit * f1.size());
	for (size_t i = 0; at != std::string::npos && i < fit; i++)
		CHECK(g.compare(at + i * f1.size(), f1.size(), f1) == 0);
	// a drop in the middle of a multi-chunk frame discards the whole frame:
	// no partial frame reaches the Psion, and it is counted once
	r.TypeRun("ATO\r", 50); r.Run(800); r.Got();
	r.Run(1100); r.TypeRun("+++", 1100); r.Got();
	{
	size_t room = am::kPppOutQueue / f1.size();
	for (size_t i = 0; i < room; i++)
		PushChunks(ppp, f1);                         // fill the queue to the last whole frame
	size_t queued = ppp.QueuedBytes();
	uint32_t drops = ppp.DroppedFrames();
	std::string big = PppFrame(3, 3000);             // 5 chunks of 600; none fits now
	std::string small = PppFrame(4, 400);           // more than the 288 bytes left
	// the first chunk of `big` may fit while later ones do not: whatever happens, no partial
	PushChunks(ppp, big);
	CHECK(ppp.DroppedFrames() == drops + 1);         // once, not per chunk
	CHECK(ppp.QueuedBytes() == queued);              // nothing of it queued
	// the next frame starts clean after the flag: it is dropped on its own merit (queue still full)
	PushChunks(ppp, small, 40);
	CHECK(ppp.DroppedFrames() == drops + 2);
	r.TypeRun("ATO\r", 50);
	r.Run(4000);
	g = r.Got();
	{
	std::string want;
	for (size_t i = 0; i < room; i++)
		want += f1;
	size_t w0 = g.find(f1);                          // (after the CONNECT text from ATO)
	CHECK(w0 != std::string::npos && g.substr(w0) == want);                                // only the whole frames queued before: no partial
	}
	// room again: a later frame fits and arrives whole, in order
	PushChunks(ppp, f2);
	r.Run(1500);
	CHECK(r.Got() == f2 && ppp.DroppedFrames() == drops + 2);
	// a frame whose tail chunk is dropped while the head was fine: drop-mode ends at the flag
	r.Run(1100); r.TypeRun("+++", 1100); r.Got();
	for (size_t i = 0; i < room; i++)
		PushChunks(ppp, f1);
	drops = ppp.DroppedFrames();
	std::string head = big.substr(0, 1200), tail = big.substr(1200);
	ppp.QueueOut((const uint8_t*)head.data(), 600);  // gathered, not yet queued
	ppp.QueueOut((const uint8_t*)head.data() + 600, 600);
	ppp.QueueOut((const uint8_t*)tail.data(), tail.size());   // flag: the queue is full -> whole frame dropped
	CHECK(ppp.DroppedFrames() == drops + 1 && ppp.QueuedBytes() == room * f1.size());
	r.TypeRun("ATO\r", 50);
	r.Run(4000); r.Got();
	}
	// hang-up empties the queue
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 100); r.Got();
	CHECK(ppp.QueuedBytes() == 0 && !r.modem->PppCall());
	}

// ----- AT&F keeps the link hardware; AT$RESET=YES clears everything -------------
static void TestFactoryKeepsLink()
	{
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	const am::Settings2& s2 = r.modem->Config2();
	r.TypeRun("AT$FC=1\r"); r.TypeRun("AT$FCSWAP=1\r"); r.TypeRun("AT$PPP=0\r");
	r.TypeRun("AT$PINS=6,5,7,8,-1\r"); r.TypeRun("AT$SWAP=1\r"); r.TypeRun("AT$TLS=1\r"); r.TypeRun("AT$SSID=Home\r");
	r.TypeRun("AT$PX=2\r");
	CHECK(s2.flow == 1 && s2.flowSwap == 1 && s2.ppp == 0 && s2.pins[0] == 6 && s2.pins[3] == 8);
	r.Got();
	r.TypeRun("AT&F\r");
	CHECK(Has(r.Got(), "OK"));
	CHECK(s2.flow == 1 && s2.flowSwap == 1 && s2.ppp == 0 && s2.pins[0] == 6 && s2.pins[1] == 5 && s2.pins[3] == 8 && s2.pins[4] == -1);
	CHECK(r.modem->Config().swapPins == 1 && strcmp(r.modem->Config().ssid, "Home") == 0);
	CHECK(s2.tls == 0 && r.modem->Config().proxy == 0);          // the rest is factory
	CHECK(r.hal.pins.flow == 1 && r.hal.pins.pins[0] == 6);      // and the board was told
	r.TypeRun("AT$RESET=YES\r");
	CHECK(r.hal.reset);
	CHECK(s2.flow == 0 && s2.flowSwap == 0 && s2.ppp == 1 && s2.pins[0] == -1 && s2.pins[3] == -1);
	CHECK(r.modem->Config().swapPins == 0 && r.modem->Config().ssid[0] == 0);
	// the factory default: PPP on, so ATD777 is PPP
	Rig r2;
	r2.TypeRun("ATD777\r", 100);
	CHECK(Has(r2.Got(), "CONNECT") && r2.modem->PppCall());
	}

// ----- the DNS the Psion is given ------------------------------------------------
static void TestPppDns()
	{
	{ Rig r;                                                      // no uplink DNS: 1.1.1.1
	  r.TypeRun("ATD777\r", 100);
	  CHECK(r.modem->PppCall() && r.modem->Ppp().Dns() == am::Ip4(1, 1, 1, 1));
	  CHECK(r.hal.Logged("DNS 1.1.1.1") && r.hal.Logged("IP 192.168.7.2") && r.hal.Logged("gateway 192.168.7.1")); }
	{ Rig r;                                                      // the uplink's
	  r.hal.dns = am::Ip4(192, 168, 1, 1);
	  r.TypeRun("ATD777\r", 100);
	  CHECK(r.modem->PppCall() && r.modem->Ppp().Dns() == am::Ip4(192, 168, 1, 1));
	  CHECK(r.hal.Logged("DNS 192.168.1.1") && !r.hal.Logged("DNS 1.1.1.1")); }
	CHECK(am::Ip4(1, 2, 3, 4) == 0x04030201);                      // (lwIP's byte order on a little-endian CPU)
	}

int main()
	{
	printf("basics\n");   TestBasics();
	printf("dial\n");     TestDial();
	printf("ppp\n");      TestPpp();
	printf("data\n");     TestData();
	printf("escape\n");   TestEscape();
	printf("carrier\n");  TestCarrier();
	printf("pacing\n");   TestPacing();
	printf("settings\n"); TestSettings();
	printf("baud fallback\n"); TestBaudFallback();
	printf("psiglue\n");  TestPsiglue();
	printf("settings 2.0\n"); TestSettings2();
	printf("schema\n");   TestSchema();
	printf("new commands\n"); TestNewCommands();
	printf("tls termination\n"); TestTlsTermination();
	printf("psiexec\n");  TestExec();
	printf("uplink\n");   TestUplink();
	printf("reachability\n"); TestReachability();
	printf("preset network\n"); TestSeedNetwork();
	printf("button\n");   TestButton();
	printf("status screen\n"); TestStatusModel();
	printf("ppp drain\n"); TestPppDrain();
	printf("factory keeps link\n"); TestFactoryKeepsLink();
	printf("ppp dns\n");  TestPppDns();
	printf("%d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
	}
