// test_v22.cpp - host tests for firmware 2.2: the WireGuard config parser
// and decisions (mode, Auto's "at home", the split-tunnel routing rule), the
// DNS relay, the saved networks (choice, migration, limits), the line-setting
// trial, the double-press, the schema's coverage of AT and the web pages, and
// that no key or password ever reaches the log or an answer.
// Keys here are made at run time from random bytes (throwaway, never real),
// or are obviously fake (all zero bytes, 'A' * 43 + '=').
//   make -C hosttest test
// MIT licence (see LICENSE at the top of the repository).
#include "fakehal.h"
#include "../src/status.h"
#include "../src/button.h"
#include <stdlib.h>
#include <time.h>

static int gChecks = 0, gFails = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Rig
	{
	FakeHal hal;
	std::vector<uint8_t> ring;
	am::Modem* modem = 0;
	size_t mark = 0;
	explicit Rig(bool aBegin = true) : ring(64 * 1024)
		{
		if (aBegin) Start();
		}
	void Start()
		{
		modem = new am::Modem(hal, ring.data(), ring.size());
		modem->Begin();
		Run(10);
		}
	~Rig() { delete modem; }
	void Run(uint32_t aMs)
		{
		for (uint64_t t = 0; t < (uint64_t)aMs * 1000; t += 200)
			{
			modem->Loop();
			hal.Advance(200);
			}
		}
	void Type(const char* s) { while (*s) hal.rx.push_back((uint8_t)*s++); }
	void TypeRun(const char* s, uint32_t ms = 60) { Type(s); Run(ms); }
	std::string Got()
		{
		std::string w = hal.Wire();
		std::string s = w.substr(mark);
		mark = w.size();
		return s;
		}
	std::string Log() const
		{
		std::string all;
		char buf[256];
		size_t from = 0, n;
		while ((n = hal.LogLines().Read(from, buf, sizeof(buf))) > 0) { all.append(buf, n); from += n; }
		for (const std::string& l : hal.logs) all += l + "\n";
		return all;
		}
	};

static bool Has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

// a throwaway WireGuard-shaped key: 32 random bytes in base64
static std::string RandomKey()
	{
	static const char* const kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint8_t b[33] = {};
	for (int i = 0; i < 32; i++) b[i] = (uint8_t)(rand() & 0xff);
	std::string out;
	for (int i = 0; i < 33; i += 3)
		{
		uint32_t w = (b[i] << 16) | (i + 1 < 33 ? b[i + 1] << 8 : 0) | (i + 2 < 33 ? b[i + 2] : 0);
		out += kB64[(w >> 18) & 63]; out += kB64[(w >> 12) & 63]; out += kB64[(w >> 6) & 63]; out += kB64[w & 63];
		}
	out.resize(43);
	// the last character carries only 4 bits of the 32nd byte: 2 zero bits
	out[42] = kB64[am::Base64Decode((out + "=").c_str(), b, sizeof(b)) == 32 ? (strchr(kB64, out[42]) - kB64) & 0x3c : 0];
	return out + "=";
	}

struct Keys { std::string priv, pub, psk; };
static Keys NewKeys() { return { RandomKey(), RandomKey(), RandomKey() }; }

static std::string Conf(const Keys& k, const char* aAllowed = "192.168.10.0/24", const char* aExtra = "")
	{
	return std::string("[Interface]\nPrivateKey = ") + k.priv + "\nAddress = 192.168.3.2/32\nDNS = 192.168.10.1, home.lan\n"
		"\n[Peer]\nPublicKey = " + k.pub + "\nPresharedKey = " + k.psk + "\nAllowedIPs = " + aAllowed +
		"\nEndpoint = vpn.example.org:51820\nPersistentKeepalive = 25\n" + aExtra;
	}

// ----- the parser ----------------------------------------------------------------------
static void TestWgParse()
	{
	CHECK(am::WgKeyOk("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));   // 32 zero bytes: a fake key
	CHECK(!am::WgKeyOk("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));    // no padding
	CHECK(!am::WgKeyOk("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=="));   // 31 bytes
	CHECK(!am::WgKeyOk("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA!="));   // not base64
	CHECK(!am::WgKeyOk(""));
	uint8_t b[8];
	CHECK(am::Base64Decode("aGVsbG8=", b, sizeof(b)) == 5 && memcmp(b, "hello", 5) == 0);
	CHECK(am::Base64Decode("aGVsbG8", b, sizeof(b)) == -1 && am::Base64Decode("a=GV", b, sizeof(b)) == -1);
	CHECK(am::Base64Decode("aGVsbG8gd29ybGQ=", b, sizeof(b)) == -1);         // too long for the buffer

	Keys k = NewKeys();
	CHECK(am::WgKeyOk(k.priv.c_str()) && am::WgKeyOk(k.pub.c_str()));
	am::WgConfig c;
	char why[80];
	std::string text = Conf(k, "192.168.10.0/24, fd00::/64, 10.9.0.0/16");
	CHECK(am::WgParse(text.c_str(), c, why, sizeof(why)));
	CHECK(am::WgValid(c) && c.addr == am::Ip4(192, 168, 3, 2) && c.addrLen == 32);
	CHECK(c.nDns == 1 && c.dns[0] == am::Ip4(192, 168, 10, 1));                 // "home.lan" skipped
	CHECK(c.nAllowed == 2 && c.allowed[0].ip == am::Ip4(192, 168, 10, 0) && c.allowed[0].len == 24
		&& c.allowed[1].ip == am::Ip4(10, 9, 0, 0) && c.allowed[1].len == 16);   // IPv6 skipped
	CHECK(strcmp(c.endpoint, "vpn.example.org") == 0 && c.port == 51820 && c.keepalive == 25);
	CHECK(k.priv == c.privKey && k.pub == c.pubKey && k.psk == c.psk);
	// what may be shown never holds a key
	char sum[400];
	am::WgSummary(c, sum, sizeof(sum));
	CHECK(!Has(sum, k.priv.c_str()) && !Has(sum, k.psk.c_str()) && !Has(sum, k.pub.c_str()));
	CHECK(Has(sum, "Private key: set") && Has(sum, "Preshared key: set") && Has(sum, "192.168.10.0/24,10.9.0.0/16")
		&& Has(sum, "vpn.example.org:51820"));
	// CR LF, comments, case, host bits, keepalive off, no PSK, unknown wg-quick keys
	std::string t2 = "# from UniFi\r\n[interface]\r\nprivatekey=" + k.priv + "\r\nADDRESS = 10.6.0.5/24, fd00::5/64\r\n"
		"PostUp = iptables -A FORWARD\r\nMTU = 1380\r\n[PEER]\r\npublickey = " + k.pub + "\r\nallowedips = 192.168.10.77/24\r\n"
		"endpoint = 203.0.113.9:443\r\npersistentkeepalive = off\r\n";
	am::WgConfig c2;
	CHECK(am::WgParse(t2.c_str(), c2, why, sizeof(why)));
	CHECK(c2.addr == am::Ip4(10, 6, 0, 5) && c2.addrLen == 24 && c2.mtu == 1380 && c2.keepalive == 0 && !c2.psk[0]);
	CHECK(c2.allowed[0].ip == am::Ip4(192, 168, 10, 0) && c2.port == 443 && c2.nDns == 0);
	am::WgSummary(c2, sum, sizeof(sum));
	CHECK(Has(sum, "Preshared key: none"));

	// malformed: each refused with a reason that names the field and never quotes a value
	struct Bad { std::string text; const char* why; };
	std::string bigAllowed;
	for (int i = 0; i < 9; i++) bigAllowed += std::string(i ? "," : "") + "10." + std::to_string(i) + ".0.0/16";
	std::string longLine = "[Interface]\n# " + std::string(300, 'x') + "\n";
	const Bad bad[] = {
		{ Conf(k).replace(Conf(k).find(k.priv), k.priv.size(), "notakey"), "PrivateKey is not" },
		{ Conf(k).replace(Conf(k).find(k.pub), k.pub.size(), k.pub.substr(0, 40) + "=="), "PublicKey is not" },
		{ Conf(k).replace(Conf(k).find(k.psk), k.psk.size(), "x"), "PresharedKey is not" },
		{ Conf(k).replace(Conf(k).find("PrivateKey"), 10, "PrivateKez"), "PrivateKey is missing" },
		{ Conf(k).replace(Conf(k).find("Address = 192.168.3.2/32"), 24, "Address = fd00::2/64"), "Address (IPv4) is missing" },
		{ Conf(k).replace(Conf(k).find("Address = 192.168.3.2/32"), 24, "Address = 999.1.1.1/32"), "Address is not" },
		{ Conf(k, "::/0"), "AllowedIPs (IPv4) is missing" },
		{ Conf(k, "192.168.10.0/33"), "AllowedIPs has an entry" },
		{ Conf(k, bigAllowed.c_str()), "more than 8" },
		{ Conf(k).replace(Conf(k).find("vpn.example.org:51820"), 21, "vpn.example.org"), "Endpoint must be" },
		{ Conf(k).replace(Conf(k).find("vpn.example.org:51820"), 21, "[2001:db8::1]:51820"), "Endpoint must be" },
		{ Conf(k).replace(Conf(k).find("vpn.example.org:51820"), 21, "vpn.example.org:70000"), "Endpoint must be" },
		{ Conf(k).replace(Conf(k).find("vpn.example.org:51820"), 21, "vp n<x>:51820"), "odd characters" },
		{ Conf(k).replace(Conf(k).find("Endpoint"), 8, "Endpoinx"), "Endpoint is missing" },
		{ Conf(k, "192.168.10.0/24", "[Peer]\nPublicKey = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n"), "Only one [Peer]" },
		{ "PrivateKey = " + k.priv + "\n" + Conf(k), "before [Interface]" },
		{ "[Wireguard]\n" + Conf(k), "Unknown section" },
		{ Conf(k) + "just some words\n", "not \"Name = value\"" },
		{ "[Interface]\nPrivateKey = " + k.priv + "\nAddress = 10.0.0.2/32\n", "[Peer] is missing" },
		{ Conf(k) + "PersistentKeepalive = soon\n", "PersistentKeepalive" },
		{ Conf(k) + "[Interface]\nMTU = 9000\n", "MTU" },
		{ Conf(k) + std::string(2100, '#'), "too long" },
		{ longLine, "too long" },
		{ "", "PrivateKey is missing" },
		};
	for (const Bad& x : bad)
		{
		am::WgConfig keep = c;
		bool ok = am::WgParse(x.text.c_str(), c, why, sizeof(why));
		CHECK(!ok && Has(why, x.why));
		if (ok || !Has(why, x.why)) printf("   (expected \"%s\", got \"%s\")\n", x.why, why);
		CHECK(memcmp(&keep, &c, sizeof(c)) == 0);                     // untouched on failure
		CHECK(!Has(why, k.priv.c_str()) && !Has(why, k.psk.c_str()) && !Has(why, "notakey"));
		}
	// garbage of every byte value: no crash, refused
	for (int n = 0; n < 200; n++)
		{
		std::string g;
		for (int i = 0; i < 1 + rand() % 600; i++) g += (char)(1 + rand() % 255);
		if (rand() % 3 == 0) g = "[Interface]\n" + g;
		if (rand() % 3 == 0) g = Conf(k) + g;
		am::WgParse(g.c_str(), c2, why, sizeof(why));
		}
	CHECK(true);
	// the stored record: accepted when sane, refused when not
	am::WgConfig r = c;
	CHECK(am::WgAccept(r, sizeof(r)));
	r = c; r.magic = 0; CHECK(!am::WgAccept(r, sizeof(r)));
	r = c; CHECK(!am::WgAccept(r, sizeof(r) - 4));
	r = c; r.nAllowed = 9; CHECK(!am::WgAccept(r, sizeof(r)));
	r = c; memset(r.privKey, 'A', sizeof(r.privKey)); CHECK(!am::WgAccept(r, sizeof(r)));   // unterminated/wrong length
	r = c; r.allowed[0].len = 40; CHECK(!am::WgAccept(r, sizeof(r)));
	am::WgClear(r);
	CHECK(!am::WgValid(r) && !am::WgAccept(r, sizeof(r)));
	}

// keys from the real tool, when it is installed (wireguard-tools' wg genkey,
// pubkey and genpsk; made now, thrown away): the parser must take them
static std::string Run(const std::string& aCmd)
	{
	FILE* f = popen(aCmd.c_str(), "r");
	if (!f) return "";
	char b[128] = {};
	if (!fgets(b, sizeof(b), f)) b[0] = 0;
	pclose(f);
	std::string s = b;
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
	return s;
	}

static void TestWgRealKeys()
	{
	if (system("command -v wg >/dev/null 2>&1") != 0)
		{
		printf("   (wg not installed: skipped)\n");
		return;
		}
	Keys k;
	k.priv = Run("wg genkey");
	k.pub = Run("echo " + Run("wg genkey") + " | wg pubkey");
	k.psk = Run("wg genpsk");
	am::WgConfig c;
	char why[80];
	CHECK(am::WgKeyOk(k.priv.c_str()) && am::WgKeyOk(k.pub.c_str()) && am::WgKeyOk(k.psk.c_str()));
	CHECK(am::WgParse(Conf(k).c_str(), c, why, sizeof(why)) && k.pub == c.pubKey);
	}

// ----- the decisions ---------------------------------------------------------------------
static void TestWgDecide()
	{
	Keys k = NewKeys();
	am::WgConfig c, def, none;
	char why[80];
	CHECK(am::WgParse(Conf(k).c_str(), c, why, sizeof(why)));
	CHECK(am::WgParse(Conf(k, "0.0.0.0/0").c_str(), def, why, sizeof(why)));
	am::WgClear(none);
	uint32_t hot = am::Ip4(172, 20, 10, 2), hotMask = am::Ip4(255, 255, 255, 240);
	uint32_t home = am::Ip4(192, 168, 10, 50), homeMask = am::Ip4(255, 255, 255, 0);
	CHECK(am::WgDecide(am::EWgModeOff, c, true, hot, hotMask) == am::EWgWantOff);
	CHECK(am::WgDecide(am::EWgModeOn, none, true, hot, hotMask) == am::EWgWantNoConfig);
	CHECK(am::WgDecide(am::EWgModeAuto, c, false, 0, 0) == am::EWgWantNoUplink);
	CHECK(am::WgDecide(am::EWgModeOn, c, true, hot, hotMask) == am::EWgWantUp);
	CHECK(am::WgDecide(am::EWgModeAuto, c, true, hot, hotMask) == am::EWgWantUp);
	CHECK(am::WgDecide(am::EWgModeAuto, c, true, home, homeMask) == am::EWgWantHome);   // inside AllowedIPs
	CHECK(am::WgDecide(am::EWgModeOn, c, true, home, homeMask) == am::EWgWantUp);       // On means on
	// a full tunnel (0.0.0.0/0): "home" comes from the DNS being on the WiFi's own subnet
	CHECK(am::WgDecide(am::EWgModeAuto, def, true, hot, hotMask) == am::EWgWantUp);
	CHECK(am::WgDecide(am::EWgModeAuto, def, true, home, homeMask) == am::EWgWantHome);
	CHECK(am::WgDecide(9, c, true, hot, hotMask) == am::EWgWantOff);
	// the routing rule: AllowedIPs, longest prefix against connected routes, never the endpoint
	am::WgPrefix a[2] = { { am::Ip4(192, 168, 10, 0), 24, {} }, { am::Ip4(10, 0, 0, 0), 8, {} } };
	uint32_t ep = am::Ip4(203, 0, 113, 7);
	CHECK(am::WgUseTunnel(a, 2, am::Ip4(192, 168, 10, 62), -1, ep));             // a home server
	CHECK(am::WgUseTunnel(a, 2, am::Ip4(10, 1, 2, 3), -1, ep));
	CHECK(!am::WgUseTunnel(a, 2, am::Ip4(1, 1, 1, 1), -1, ep));                // the Internet: the uplink
	CHECK(!am::WgUseTunnel(a, 2, am::Ip4(192, 168, 10, 62), 24, ep));            // at home: the LAN is connected (equal: local)
	CHECK(am::WgUseTunnel(a, 2, am::Ip4(10, 1, 2, 3), 0, ep));
	am::WgPrefix all[1] = { { 0, 0, {} } };
	CHECK(am::WgUseTunnel(all, 1, am::Ip4(93, 184, 216, 34), -1, ep));         // full tunnel
	CHECK(!am::WgUseTunnel(all, 1, ep, -1, ep));                               // ... but never the endpoint (a loop)
	CHECK(!am::WgUseTunnel(all, 1, am::Ip4(172, 20, 10, 1), 28, ep));          // ... nor the hotspot's own subnet
	CHECK(!am::WgUseTunnel(all, 1, am::Ip4(192, 168, 7, 2), 32, ep));          // ... nor the Psion over PPP
	CHECK(!am::WgUseTunnel(all, 1, am::Ip4(224, 0, 0, 251), -1, ep) && !am::WgUseTunnel(all, 1, 0xffffffffu, -1, ep)
		&& !am::WgUseTunnel(all, 1, 0, -1, ep) && !am::WgUseTunnel(all, 1, am::Ip4(127, 0, 0, 1), -1, ep));
	am::WgPrefix host[1] = { { am::Ip4(192, 168, 7, 2), 32, {} } };
	CHECK(!am::WgUseTunnel(host, 1, am::Ip4(192, 168, 7, 2), 32, ep));         // equal length: the connected route wins
	CHECK(am::WgBestPrefix(a, 2, am::Ip4(192, 168, 10, 9)) == 24 && am::WgBestPrefix(a, 2, am::Ip4(8, 8, 8, 8)) == -1);
	CHECK(am::MaskLen(am::Ip4(255, 255, 255, 240)) == 28 && am::PrefixMask(28) == am::Ip4(255, 255, 255, 240)
		&& am::PrefixMask(0) == 0 && am::MaskLen(0) == 0);
	}

// ----- the DNS relay ------------------------------------------------------------------------
static std::vector<uint8_t> Query(uint16_t aId, const char* aName = "example.org")
	{
	std::vector<uint8_t> q = { (uint8_t)(aId >> 8), (uint8_t)aId, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };
	const char* p = aName;
	while (*p)
		{
		const char* dot = strchr(p, '.');
		size_t n = dot ? (size_t)(dot - p) : strlen(p);
		q.push_back((uint8_t)n);
		q.insert(q.end(), p, p + n);
		p += n + (dot ? 1 : 0);
		}
	q.push_back(0); q.push_back(0); q.push_back(1); q.push_back(0); q.push_back(1);
	return q;
	}

static void TestDnsRelay()
	{
	CHECK(am::DnsUpstream(true, am::Ip4(192, 168, 10, 1), am::Ip4(172, 20, 10, 1)) == am::Ip4(192, 168, 10, 1));
	CHECK(am::DnsUpstream(false, am::Ip4(192, 168, 10, 1), am::Ip4(172, 20, 10, 1)) == am::Ip4(172, 20, 10, 1));
	CHECK(am::DnsUpstream(true, 0, am::Ip4(172, 20, 10, 1)) == am::Ip4(172, 20, 10, 1));
	CHECK(am::DnsUpstream(false, 0, 0) == am::Ip4(1, 1, 1, 1));

	am::DnsRelay r;
	r.Seed(12345);
	uint32_t psion = am::Ip4(192, 168, 7, 2), srv = am::Ip4(172, 20, 10, 1), now = 1000;
	std::vector<uint8_t> q = Query(0x1234);
	CHECK(r.FromClient(q.data(), q.size(), psion, 1025, srv, now));
	uint16_t up = (uint16_t)((q[0] << 8) | q[1]);
	CHECK(r.Pending() == 1 && r.Forwarded() == 1);
	// the answer: QR set, from the server, port 53, the upstream ID
	std::vector<uint8_t> a = q;
	a[2] |= 0x80;
	uint32_t cl; uint16_t cp;
	std::vector<uint8_t> wrong = a;
	CHECK(!r.FromServer(wrong.data(), wrong.size(), am::Ip4(6, 6, 6, 6), 53, cl, cp, now));   // another server
	CHECK(!r.FromServer(wrong.data(), wrong.size(), srv, 5353, cl, cp, now));                  // not port 53
	wrong[0] ^= 0xff;
	CHECK(!r.FromServer(wrong.data(), wrong.size(), srv, 53, cl, cp, now));                    // an unknown ID
	std::vector<uint8_t> notAnswer = a; notAnswer[2] &= 0x7f;
	CHECK(!r.FromServer(notAnswer.data(), notAnswer.size(), srv, 53, cl, cp, now));            // not an answer
	CHECK(r.FromServer(a.data(), a.size(), srv, 53, cl, cp, now + 30));
	CHECK(cl == psion && cp == 1025 && a[0] == 0x12 && a[1] == 0x34 && r.Pending() == 0 && r.Answered() == 1);
	CHECK(!r.FromServer(a.data(), a.size(), srv, 53, cl, cp, now + 40));                       // only once
	(void)up;
	// garbage from the Psion: short, an answer, another opcode, no question, two, too big
	std::vector<uint8_t> g1(5, 0), g2 = Query(1), g3 = Query(2), g4 = Query(3), g5 = Query(4), g6(600, 0);
	g2[2] |= 0x80; g3[2] |= 0x28; g4[5] = 0; g5[5] = 2;
	g6[5] = 1;
	CHECK(!r.FromClient(g1.data(), g1.size(), psion, 1, srv, now) && !r.FromClient(g2.data(), g2.size(), psion, 1, srv, now)
		&& !r.FromClient(g3.data(), g3.size(), psion, 1, srv, now) && !r.FromClient(g4.data(), g4.size(), psion, 1, srv, now)
		&& !r.FromClient(g5.data(), g5.size(), psion, 1, srv, now) && !r.FromClient(g6.data(), g6.size(), psion, 1, srv, now));
	CHECK(!r.FromClient(0, 40, psion, 1, srv, now) && !r.FromClient(q.data(), q.size(), psion, 1, 0, now));
	CHECK(r.Pending() == 0 && r.Dropped() >= 9);
	// random bytes both ways: never a crash, nothing answered that was not asked
	for (int i = 0; i < 3000; i++)
		{
		std::vector<uint8_t> x(1 + rand() % 700);
		for (auto& b : x) b = (uint8_t)rand();
		if (rand() & 1) r.FromClient(x.data(), x.size(), psion, 1000, srv, now);
		else r.FromServer(x.data(), x.size(), srv, 53, cl, cp, now);
		}
	CHECK(r.Pending() <= am::DnsRelay::kSlots);
	r.Reset();
	// a retry from the Psion (same port and ID) keeps its slot and upstream ID
	std::vector<uint8_t> q1 = Query(0x0101), q1b = Query(0x0101);
	CHECK(r.FromClient(q1.data(), q1.size(), psion, 2000, srv, now) && r.FromClient(q1b.data(), q1b.size(), psion, 2000, srv, now + 900));
	CHECK(r.Pending() == 1 && q1[0] == q1b[0] && q1[1] == q1b[1]);
	// timeouts
	r.Expire(now + 900 + am::DnsRelay::kTimeoutMs);
	CHECK(r.Pending() == 0 && r.TimedOut() == 1);
	// a full table: the oldest goes; the IDs upstream are all different
	r.Reset();
	std::vector<uint16_t> ids;
	for (int i = 0; i < am::DnsRelay::kSlots + 4; i++)
		{
		std::vector<uint8_t> qi = Query((uint16_t)(0x4000 + i));
		CHECK(r.FromClient(qi.data(), qi.size(), psion, (uint16_t)(3000 + i), srv, now + i));
		ids.push_back((uint16_t)((qi[0] << 8) | qi[1]));
		}
	CHECK(r.Pending() == am::DnsRelay::kSlots);
	std::vector<uint16_t> last(ids.end() - am::DnsRelay::kSlots, ids.end());
	std::sort(last.begin(), last.end());
	CHECK(std::unique(last.begin(), last.end()) == last.end());
	// the first ones were evicted: their answers are dropped; the newest is answered
	std::vector<uint8_t> old = Query(0); old[0] = (uint8_t)(ids[0] >> 8); old[1] = (uint8_t)ids[0]; old[2] |= 0x80;
	CHECK(!r.FromServer(old.data(), old.size(), srv, 53, cl, cp, now + 50));
	std::vector<uint8_t> nw = Query(0); nw[0] = (uint8_t)(ids.back() >> 8); nw[1] = (uint8_t)ids.back(); nw[2] |= 0x80;
	CHECK(r.FromServer(nw.data(), nw.size(), srv, 53, cl, cp, now + 50) && cp == 3000 + am::DnsRelay::kSlots + 3
		&& nw[0] == 0x40 && nw[1] == am::DnsRelay::kSlots + 3);
	}

// ----- the saved networks ---------------------------------------------------------------------
static am::WifiNet Net(const char* s, int rssi)
	{
	am::WifiNet w; memset(&w, 0, sizeof(w));
	snprintf(w.ssid, sizeof(w.ssid), "%s", s); w.rssi = (int8_t)rssi; w.secure = 1; w.channel = 1;
	return w;
	}

static void TestNetList()
	{
	am::NetList l;
	am::NetListInit(l);
	CHECK(am::NetUpsert(l, "Home", "password1", -1) == 0 && am::NetUpsert(l, "Phone", "password2", -1) == 1);
	CHECK(am::NetUpsert(l, "Home", "password3", -1) == 0 && l.count == 2 && strcmp(l.net[0].pass, "password3") == 0);
	CHECK(am::NetUpsert(l, "", "x", -1) == -1 && am::NetUpsert(l, "123456789012345678901234567890123", "", -1) == -1);
	CHECK(am::NetUpsert(l, "Long", std::string(65, 'p').c_str(), -1) == -1);
	// full: the last one not kept goes
	for (int i = 0; i < 6; i++) am::NetUpsert(l, ("N" + std::to_string(i)).c_str(), "", -1);
	CHECK(l.count == am::kNetMax);
	bool ev = false;
	CHECK(am::NetUpsert(l, "Extra", "", am::kNetMax - 1, &ev) == am::kNetMax - 1 && ev && l.count == am::kNetMax);
	CHECK(am::NetFind(l, "N5") == am::kNetMax - 2 && am::NetFind(l, "N4") < 0 && am::NetFind(l, "Extra") == am::kNetMax - 1);
	CHECK(am::NetForget(l, 1) && am::NetFind(l, "Phone") < 0 && l.count == am::kNetMax - 1 && !am::NetForget(l, 20));
	CHECK(am::NetPrefer(l, 2) && strcmp(l.net[0].ssid, "N1") == 0 && strcmp(l.net[1].ssid, "Home") == 0);
	// choosing: strongest in range; pinned first; skipped ones left out; none
	am::NetListInit(l);
	am::NetUpsert(l, "Home", "password1", -1);
	am::NetUpsert(l, "Phone", "password2", -1);
	am::NetUpsert(l, "Cafe", "", -1);
	am::WifiNet scan[] = { Net("Stranger", -30), Net("Phone", -70), Net("Cafe", -55), Net("Phone", -50), Net("", -20) };
	int rssi = 0;
	CHECK(am::NetChoose(l, scan, 5, 0, 0, &rssi) == 1 && rssi == -50);        // Phone at its best AP beats Cafe
	CHECK(am::NetChoose(l, scan, 5, "Cafe", 0) == 2);                          // pinned
	CHECK(am::NetChoose(l, scan, 5, "Home", 0) == 1);                          // pinned but not in range
	CHECK(am::NetChoose(l, scan, 5, 0, 1u << 1) == 2);                         // Phone failed: next best
	CHECK(am::NetChoose(l, scan, 5, 0, (1u << 1) | (1u << 2)) == -1);
	CHECK(am::NetChoose(l, scan, 0, 0, 0) == -1);
	am::WifiNet tie[] = { Net("Cafe", -60), Net("Home", -60) };
	CHECK(am::NetChoose(l, tie, 2, 0, 0) == 0);                                // a tie: the earlier saved
	// next for the button: the next saved in range, wrapping
	CHECK(am::NetNext(l, 1, scan, 5) == 2 && am::NetNext(l, 2, scan, 5) == 1 && am::NetNext(l, -1, scan, 5) == 1);
	CHECK(am::NetNext(l, 0, 0, 0) == 1);                                       // no scan: simply the next
	am::NetList one; am::NetListInit(one); am::NetUpsert(one, "Only", "", -1);
	CHECK(am::NetNext(one, 0, scan, 5) == -1);
	// a stored record with garbage in it
	am::NetList g = l;
	g.count = 200;
	memset(g.net[1].ssid, 'x', sizeof(g.net[1].ssid));
	memset(g.net[2].pass, 'y', sizeof(g.net[2].pass));
	g.net[0].ssid[0] = 0;
	CHECK(am::NetListAccept(g, sizeof(g)));
	CHECK(g.count <= am::kNetMax && strlen(g.net[0].ssid) <= 32 && strlen(g.net[1].pass) <= 64);
	for (int i = 0; i < g.count; i++) CHECK(g.net[i].ssid[0]);
	am::NetList m = l; m.magic = 1;
	CHECK(!am::NetListAccept(m, sizeof(m)) && !am::NetListAccept(l, 4));
	}

static void SaveOld(FakeHal& h, const char* ssid, const char* pass)
	{
	am::FactoryDefaults(h.saved);
	snprintf(h.saved.ssid, sizeof(h.saved.ssid), "%s", ssid);
	snprintf(h.saved.pass, sizeof(h.saved.pass), "%s", pass);
	h.haveSaved = true;
	}

static void TestSavedNetsModem()
	{
	// a 2.1 Atom with one network: it becomes saved entry 1 at the first start
	{ Rig r(false);
	  SaveOld(r.hal, "Home", "homepass1");
	  r.Start();
	  CHECK(r.hal.haveNets && r.hal.savedNets.count == 1 && strcmp(r.hal.savedNets.net[0].ssid, "Home") == 0
		&& strcmp(r.hal.savedNets.net[0].pass, "homepass1") == 0 && r.hal.wifiSsid == "Home");
	  CHECK(r.hal.scans == 0);                                       // one network: no scan needed
	  CHECK(!Has(r.Log(), "homepass1")); }
	// AT$JOIN then AT&W adds rather than replaces; AT$NETS? lists without passwords
	{ Rig r(false);
	  SaveOld(r.hal, "Home", "homepass1");
	  r.Start();
	  r.TypeRun("ATE0\r"); r.Got();
	  r.TypeRun("AT$JOIN=\"Phone\",\"phonepass\"\r");
	  r.TypeRun("AT&W\r");
	  CHECK(r.hal.savedNets.count == 2 && strcmp(r.hal.savedNets.net[1].ssid, "Phone") == 0);
	  r.TypeRun("AT$NETS?\r");
	  std::string g = r.Got();
	  CHECK(Has(g, "+NET:1,\"Home\",1,-\r\n") && Has(g, "+NET:2,\"Phone\",1,CUR,PIN\r\n") && !Has(g, "pass"));
	  // add one by command, forget one, the list kept by AT&W
	  r.TypeRun("AT$NETADD=\"Cafe \\\"Guest\\\"\",\"\"\r");
	  CHECK(Has(r.Got(), "OK"));
	  r.TypeRun("AT$NETADD=\"Bad\",\"short\"\r");
	  CHECK(Has(r.Got(), "ERROR"));
	  r.TypeRun("AT$NETADD=\"Office\",\"officepass9\"\r");
	  r.TypeRun("AT$FORGET=1\r");
	  CHECK(Has(r.Got(), "OK") && r.modem->Nets().count == 3 && am::NetFind(r.modem->Nets(), "Home") < 0);
	  r.TypeRun("AT$FORGET=\"Nowhere\"\r");
	  r.TypeRun("AT$FORGET=9\r");
	  CHECK(Has(r.Got(), "ERROR\r\n\r\nERROR"));
	  r.TypeRun("AT&W\r");
	  CHECK(r.hal.savedNets.count == 3);
	  // switching: by number or name, with the saved password, pinned
	  r.TypeRun("AT$NET=\"Office\"\r");
	  CHECK(Has(r.Got(), "OK") && r.hal.wifiSsid == "Office" && r.hal.wifiPass == "officepass9"
		&& std::string(r.modem->Pinned()) == "Office");
	  r.TypeRun("AT$NET=2\r");
	  CHECK(r.hal.wifiSsid == "Cafe \"Guest\"" && r.hal.wifiPass == "");
	  r.TypeRun("AT$NET=0\r");
	  CHECK(Has(r.Got(), "OK\r\n\r\nERROR"));
	  // forgetting the one in use leaves it
	  r.hal.wifi = true;
	  r.TypeRun("AT$FORGET=\"Cafe \\\"Guest\\\"\"\r");
	  CHECK(!r.hal.wifi && r.modem->Config().ssid[0] == 0);
	  // nothing secret in the log, or in AT&V
	  r.TypeRun("AT&V\r");
	  std::string all = r.Log() + r.Got();
	  CHECK(!Has(all, "phonepass") && !Has(all, "officepass9") && !Has(all, "homepass1"));
	  CHECK(Has(r.Log(), "$NETADD=(hidden)")); }
	// choosing at the start: the strongest saved one in range; falling back when it will not join
	{ Rig r(false);
	  SaveOld(r.hal, "Home", "homepass1");
	  am::NetListInit(r.hal.savedNets);
	  am::NetUpsert(r.hal.savedNets, "Home", "homepass1", -1);
	  am::NetUpsert(r.hal.savedNets, "Phone", "phonepass", -1);
	  am::NetUpsert(r.hal.savedNets, "Office", "officepass9", -1);
	  r.hal.haveNets = true;
	  r.hal.nets = { Net("Home", -85), Net("Phone", -50), Net("Office", -65), Net("Other", -40) };
	  r.hal.wifi = false;
	  r.Start();
	  CHECK(r.hal.scans == 1 && r.hal.wifiSsid == "Phone" && r.hal.wifiPass == "phonepass");
	  // Phone never joins: after the grace time the next best, then the next
	  r.Run(9000);
	  CHECK(r.hal.wifiSsid == "Office");
	  r.Run(15500);
	  CHECK(r.hal.wifiSsid == "Home");
	  r.Run(15500);
	  CHECK(r.hal.wifiSsid == "Phone");                               // round again
	  // joined: the failures are forgotten
	  r.hal.wifi = true;
	  r.Run(500);
	  CHECK(Has(r.Log(), "strongest saved network in range is \"Phone\""));
	  // a network picked by hand is kept while in range, then given up
	  r.TypeRun("AT$NET=3\r");
	  CHECK(r.hal.wifiSsid == "Office");
	  r.hal.wifi = false;
	  r.Run(50000);
	  CHECK(r.hal.wifiSsid == "Office");                              // (the 40 s after the switch: that one only)
	  r.hal.nets = { Net("Home", -85), Net("Phone", -50) };            // Office gone
	  r.Run(16000);
	  CHECK(r.hal.wifiSsid == "Phone" && r.modem->Pinned()[0] == 0 && Has(r.Log(), "out of range")); }
	// the button's next network
	{ Rig r(false);
	  am::NetListInit(r.hal.savedNets);
	  am::NetUpsert(r.hal.savedNets, "A", "password1", -1);
	  am::NetUpsert(r.hal.savedNets, "B", "password2", -1);
	  r.hal.haveNets = true;
	  r.hal.nets = { Net("A", -60), Net("B", -70) };
	  r.Start();
	  CHECK(r.hal.wifiSsid == "A");
	  CHECK(r.modem->NextNet() && r.hal.wifiSsid == "B" && std::string(r.modem->Pinned()) == "B");
	  CHECK(r.modem->NextNet() && r.hal.wifiSsid == "A"); }
	}

// ----- WireGuard through the modem -------------------------------------------------------------
static void TestWgModem()
	{
	Rig r;
	Keys k = NewKeys();
	r.TypeRun("ATE0\r"); r.Got();
	r.TypeRun("AT$WG?\r");
	CHECK(Has(r.Got(), "OFF: off"));
	// the config line by line (as a terminal would send it); END parses and saves it
	std::string text = Conf(k);
	size_t at = 0;
	r.TypeRun("AT$WGL=CLEAR\r");
	while (at < text.size())
		{
		size_t nl = text.find('\n', at);
		std::string line = text.substr(at, nl - at);
		at = nl + 1;
		// (a "/" after an "a" would be the A/ repeat: such a line is sent quoted, "/" as "\/")
		std::string esc;
		for (char ch : line) { if (ch == '/' ) esc += "\\/"; else if (ch == '"' || ch == '\\') { esc += '\\'; esc += ch; } else esc += ch; }
		r.TypeRun(("AT$WGL=\"" + esc + "\"\r").c_str());
		}
	r.Got();
	r.TypeRun("AT$WGL=END\r", 200);
	std::string g = r.Got();
	CHECK(Has(g, "OK") && am::WgValid(r.modem->Wg()) && r.hal.haveWg);
	CHECK(r.modem->Wg().privKey == k.priv);
	r.TypeRun("AT$WGCONF?\r");
	g = r.Got();
	CHECK(Has(g, "+WGCONF:Endpoint: vpn.example.org:51820") && Has(g, "+WGCONF:Private key: set") && !Has(g, k.priv.c_str())
		&& !Has(g, k.psk.c_str()));
	// a bad one: refused with the reason, the old one kept
	r.TypeRun("AT$WGL=[Interface]\r");
	r.TypeRun("AT$WGL=PrivateKey = nope\r");
	r.TypeRun("AT$WGL=END\r");
	g = r.Got();
	CHECK(Has(g, "+WGERR:PrivateKey is not a WireGuard key") && Has(g, "ERROR") && r.modem->Wg().privKey == k.priv);
	// the mode: off by default, so nothing started yet
	CHECK(r.hal.wgApplies >= 1 && !r.hal.wgWant);
	r.TypeRun("AT$WG=AUTO\r", 1200);
	CHECK(r.hal.wgWant && r.modem->WgWant() == am::EWgWantUp);   // 192.168.1.50 is not inside 192.168.10.0/24
	r.hal.wgStatus.state = am::EWgStUp;
	r.hal.wgStatus.handshakeAge = 12;
	r.hal.wgStatus.rx = 1234; r.hal.wgStatus.tx = 567;
	r.Run(1100);
	r.TypeRun("AT$WG?\r");
	CHECK(Has(r.Got(), "AUTO: up, handshake 12 s ago"));
	// the DNS relay follows: the tunnel's DNS while it is up
	CHECK(r.hal.relayUp == am::Ip4(192, 168, 10, 1));
	r.TypeRun("AT$STAT?\r", 100);
	g = r.Got();
	CHECK(Has(g, "+STAT:WG=AUTO\r\n") && Has(g, "+STAT:WGST=UP\r\n") && Has(g, "+STAT:WGHS=12\r\n")
		&& Has(g, "+STAT:WGRX=1234\r\n") && Has(g, "+STAT:WGTX=567\r\n") && Has(g, "+STAT:WGEP=\"vpn.example.org:51820\"")
		&& Has(g, "+STAT:NETS=") && Has(g, "+STAT:DNS=-1"));
	// the 2.1 lines are all still there, before the new ones
	CHECK(g.find("+STAT:FC=") < g.find("+STAT:DNS=") && Has(g, "+STAT:VER=2.2") && Has(g, "+STAT:WIFI="));
	// home: Auto takes it down
	r.hal.upIp = am::Ip4(192, 168, 10, 50); r.hal.upMask = am::Ip4(255, 255, 255, 0);
	r.Run(1100);
	CHECK(!r.hal.wgWant && r.modem->WgWant() == am::EWgWantHome);
	r.TypeRun("AT$STAT?\r", 100);
	CHECK(Has(r.Got(), "+STAT:WGST=HOME"));
	CHECK(r.hal.relayUp != am::Ip4(192, 168, 10, 1));                     // (the uplink's DNS again)
	// On forces it, at home too; a new uplink address starts it again
	r.TypeRun("AT$WG=ON\r", 1200);
	CHECK(r.hal.wgWant);
	int before = r.hal.wgApplies;
	r.hal.upIp = am::Ip4(10, 0, 0, 7);
	r.Run(1100);
	CHECK(r.hal.wgApplies >= before + 2 && r.hal.wgWant && Has(r.Log(), "address changed"));
	// AT&W keeps the mode; the button cycles it (saved at once)
	r.TypeRun("AT&W\r");
	CHECK(r.hal.saved2.wgMode == am::EWgModeOn);
	CHECK(r.modem->CycleWgMode() == am::EWgModeAuto && r.hal.saved2.wgMode == am::EWgModeAuto);
	CHECK(r.modem->CycleWgMode() == am::EWgModeOff && r.modem->CycleWgMode() == am::EWgModeOn);
	// no key in the log, ever (the AT$WGL lines are hidden)
	std::string log = r.Log();
	CHECK(!Has(log, k.priv.c_str()) && !Has(log, k.psk.c_str()) && Has(log, "$WGL=(hidden)"));
	// the LCD's VPN page
	am::ScreenPage p;
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageVpn, p);
	bool mode = false;
	for (int i = 0; i < p.blocks; i++) if (!strcmp(p.block[i].label, "Mode") && !strcmp(p.block[i].text, "Mode On")) mode = true;
	CHECK(Has(p.title, "VPN") && mode && Has(p.hint, "2x mode"));
	r.hal.wgStatus.state = am::EWgStUp; r.Run(1100);                    // (the cycling above took it down)
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageHome, p);         // Home: a green "VPN on", the padlock closed
	CHECK(!strcmp(p.vpn, "VPN on") && p.vpnTone == am::EToneGreen && p.vpnLocked);
	// deleting it
	r.TypeRun("AT$WGCLR\r");
	CHECK(Has(r.Got(), "ERROR"));
	r.TypeRun("AT$WGCLR=YES\r", 1200);
	CHECK(!am::WgValid(r.modem->Wg()) && r.modem->WgWant() == am::EWgWantNoConfig && !r.hal.wgWant);
	// a reload (ATZ) brings the saved record back
	r.TypeRun("ATZ\r");
	CHECK(!am::WgValid(r.modem->Wg()));
	}

// ----- the PPP side's DNS: the relay when it can run --------------------------------------------
static void TestPppRelayDns()
	{
	{ Rig r;
	  r.hal.relayOk = true;
	  r.hal.dns = am::Ip4(192, 168, 1, 1);
	  r.TypeRun("ATD777\r", 100);
	  CHECK(r.modem->PppCall() && r.modem->Ppp().Dns() == am::Ip4(192, 168, 7, 1) && r.hal.relayOn);
	  CHECK(r.hal.Logged("DNS 192.168.7.1"));
	  r.Run(1200);
	  r.TypeRun("+++", 1500);
	  r.TypeRun("ATH\r", 100);
	  CHECK(!r.hal.relayOn); }
	{ Rig r;                                                         // no relay: the uplink's, as before
	  r.hal.dns = am::Ip4(192, 168, 1, 1);
	  r.TypeRun("ATD777\r", 100);
	  CHECK(r.modem->Ppp().Dns() == am::Ip4(192, 168, 1, 1) && !r.hal.relayOn); }
	}

// ----- the line settings' trial -------------------------------------------------------------
static void TestLineTrial()
	{
	{ Rig r;
	  r.TypeRun("ATE0\r"); r.Got();
	  int before = r.hal.pinsApplied;
	  r.Type("AT$FC=1\r");
	  r.modem->Loop();                                   // the command runs; the OK is queued
	  CHECK(r.modem->Config2().flow == 1);
	  r.Run(50);
	  CHECK(Has(r.Got(), "OK") && r.hal.pinsApplied == before + 1 && r.hal.pins.flow == 1);   // applied after the OK
	  // nothing heard in 15 s: back to the old ones
	  r.Run(15200);
	  CHECK(r.modem->Config2().flow == 0 && r.hal.pins.flow == 0 && Has(r.Log(), "back to the old ones")); }
	{ Rig r;
	  r.TypeRun("ATE0\r"); r.Got();
	  r.TypeRun("AT$PINS=7,8,-1,-1,-1\r");
	  r.Run(3000);
	  r.TypeRun("AT\r");                                  // a command heard with them: kept
	  r.Run(16000);
	  int8_t want[5] = { 7, 8, -1, -1, -1 };
	  CHECK(memcmp(r.modem->Config2().pins, want, 5) == 0 && Has(r.Log(), "confirmed")); }
	{ Rig r;
	  r.TypeRun("ATE0\r"); r.Got();
	  r.TypeRun("AT$SWAP=1\r");
	  r.TypeRun("AT&W\r");                                // saving confirms too
	  r.Run(16000);
	  CHECK(r.modem->Config().swapPins == 1 && r.hal.saved.swapPins == 1); }
	{ Rig r;                                             // the same value: no trial
	  r.TypeRun("ATE0\r"); r.Got();
	  int before = r.hal.pinsApplied;
	  r.TypeRun("AT$FC=0\r", 200);
	  CHECK(r.hal.pinsApplied == before); }
	{ Rig r;                                             // the web page: at once, no trial
	  int before = r.hal.pinsApplied;
	  CHECK(r.modem->WebSet(*am::SchemaFind("FC"), "1") && r.hal.pinsApplied == before + 1);
	  r.Run(16000);
	  CHECK(r.modem->Config2().flow == 1); }
	}

// ----- every setting reachable from AT and the web, and listed for PsiConnect -----------------------
static void TestSchemaCoverage()
	{
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	r.TypeRun("AT$SCHEMA?\r", 200);
	std::string schema = r.Got();
	r.TypeRun("AT&V\r", 200);
	std::string v = r.Got();
	int lines = 0;
	for (size_t p = 0; (p = schema.find("+SCHEMA:", p)) != std::string::npos; p++) lines++;
	CHECK(lines == am::SchemaCount());
	for (int i = 0; i < am::SchemaCount(); i++)
		{
		const am::SchemaEntry& e = am::SchemaAt(i);
		CHECK(am::SchemaFind(e.name) == &e);
		std::string tag = std::string("+SCHEMA:") + e.name + ",";
		CHECK(Has(schema, tag.c_str()));
		std::string set = std::string("$") + e.name + "=";
		CHECK(Has(v, set.c_str()));                                   // in AT&V
		const char* page = am::SchemaWebPage(e);
		CHECK(page && (!strcmp(page, "/wifi") || !strcmp(page, "/settings") || !strcmp(page, "/vpn")));
		// the query answers (not ERROR)
		std::string q = std::string("AT$") + e.name + "?\r";
		r.TypeRun(q.c_str());
		std::string a = r.Got();
		CHECK(Has(a, "OK") && !Has(a, "ERROR"));
		if (!Has(a, "OK")) printf("   (AT$%s? did not answer)\n", e.name);
		char flags[8];
		am::SchemaFlags(e, flags, sizeof(flags));
		CHECK((e.type == am::STSecret) == (strchr(flags, 'S') != 0));
		}
	CHECK(Has(schema, "+SCHEMA:WG,ENUM:OFF|ON|AUTO,0,2,9,,\"WireGuard\"") && Has(schema, "+SCHEMA:SB,BAUD,300,921600,2,L,")
		&& Has(schema, "+SCHEMA:FC,BOOL,0,1,2,L,") && Has(schema, "+SCHEMA:USB,BOOL,0,1,1,R,"));
	// secrets never shown
	r.TypeRun("AT$WEBPASS=hunter2hunter2\r");
	r.TypeRun("AT$XK=tok3n-secret\r");
	r.TypeRun("AT&V\r", 200);
	r.TypeRun("AT$WEBPASS?\r");
	r.TypeRun("AT$SCHEMA?\r", 200);
	r.TypeRun("AT$HELP\r", 300);
	std::string all = r.Got() + r.Log();
	CHECK(!Has(all, "hunter2hunter2") && !Has(all, "tok3n-secret") && Has(all, "$WEBPASS=(set)"));
	}

// ----- the double-press ---------------------------------------------------------------------
struct Btn
	{
	am::Button b;
	uint32_t t = 0;
	int clicks = 0, holds = 0, doubles = 0;
	Btn() { b.EnableDouble(true); b.Feed(1, 0); }
	void Step(int level, uint32_t ms)
		{
		for (uint32_t i = 0; i < ms; i++, t++)
			{
			am::Button::TEvent e = b.Feed(level, t);
			if (e == am::Button::EClick) clicks++;
			if (e == am::Button::EHold) holds++;
			if (e == am::Button::EDouble) doubles++;
			}
		}
	};

static void TestDoublePress()
	{
	{ Btn b; b.Step(1, 50); b.Step(0, 120); b.Step(1, 150); b.Step(0, 120); b.Step(1, 600);
	  CHECK(b.doubles == 1 && b.clicks == 0 && b.holds == 0); }
	{ Btn b; b.Step(1, 50); b.Step(0, 120); b.Step(1, 200);
	  CHECK(b.clicks == 0);                                            // not yet: a second may follow
	  b.Step(1, 300);
	  CHECK(b.clicks == 1 && b.doubles == 0); }
	{ Btn b; b.Step(1, 50); b.Step(0, 120); b.Step(1, am::Button::kDoubleGapMs + 100); b.Step(0, 120); b.Step(1, 600);
	  CHECK(b.clicks == 2 && b.doubles == 0); }                       // too far apart: two clicks
	{ Btn b; b.Step(1, 50); b.Step(0, 120); b.Step(1, 150); b.Step(0, 2500); b.Step(1, 600);
	  CHECK(b.holds == 1 && b.clicks == 0 && b.doubles == 0); }       // a click then a hold: the hold
	{ Btn b; b.Step(1, 50); b.Step(0, 3000); b.Step(1, 600);
	  CHECK(b.holds == 1 && b.clicks == 0); }                         // a hold alone, as before
	{ Btn b; b.Step(1, 50); for (int i = 0; i < 3; i++) { b.Step(0, 100); b.Step(1, 100); } b.Step(1, 600);
	  CHECK(b.doubles == 1 && b.clicks == 1); }                       // three: a double and a click
	{ am::Button plain; uint32_t t = 0; int c = 0;                   // off (the boards without the LCD): clicks as before
	  plain.Feed(1, t);
	  for (int i = 0; i < 2; i++) { for (int k = 0; k < 120; k++) if (plain.Feed(0, ++t) == am::Button::EClick) c++;
		for (int k = 0; k < 100; k++) if (plain.Feed(1, ++t) == am::Button::EClick) c++; }
	  CHECK(c == 2); }
	}

// ----- the status pages ---------------------------------------------------------------------
static void TestPages()
	{
	Rig r(false);
	am::NetListInit(r.hal.savedNets);
	am::NetUpsert(r.hal.savedNets, "Home", "password1", -1);
	am::NetUpsert(r.hal.savedNets, "Phone", "password2", -1);
	r.hal.haveNets = true;
	r.hal.nets = { Net("Home", -60) };
	r.Start();
	am::ScreenPage p;
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageNetwork, p);
	bool cur = false, other = false, pw = false;
	for (int i = 0; i < p.blocks; i++)
		{
		if (p.block[i].kind == am::EBlockIcon && !strcmp(p.block[i].label, "Network") && !strcmp(p.block[i].text, "Home")) cur = true;
		if (strstr(p.block[i].text, "password")) pw = true;
		}
	other = Has(p.hint, "2x join Phone");                // what a double-press joins
	CHECK(cur && other && !pw);
	if (!(cur && other)) { printf("   hint [%s]\n", p.hint); for (int i = 0; i < p.blocks; i++) printf("   [%s] [%s]\n", p.block[i].label, p.block[i].text); }
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageVpn, p);
	CHECK(Has(p.title, "VPN"));
	am::StatusModel::Fill(*r.modem, r.hal, am::EPageHome, p);
	CHECK(!strcmp(p.vpn, "VPN off") && p.vpnTone == am::EToneGrey);   // off: a grey "VPN off" on Home
	}

int main()
	{
	srand((unsigned)time(0));
	printf("wireguard config\n"); TestWgParse();
	printf("wireguard keys from wg\n"); TestWgRealKeys();
	printf("wireguard decisions and routing\n"); TestWgDecide();
	printf("dns relay\n"); TestDnsRelay();
	printf("saved networks\n"); TestNetList();
	printf("saved networks in the modem\n"); TestSavedNetsModem();
	printf("wireguard in the modem\n"); TestWgModem();
	printf("ppp dns relay\n"); TestPppRelayDns();
	printf("line trial\n"); TestLineTrial();
	printf("schema coverage\n"); TestSchemaCoverage();
	printf("double-press\n"); TestDoublePress();
	printf("pages\n"); TestPages();
	printf("%d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
	}
