// hostmodem.cpp - the Atom modem's code (modem.cpp, unchanged) as a PC
// program: the "serial line" is a unix socket that the Psion emulator's
// serial bridge connects to (--serial-bridge-socket), TCP is the PC's own,
// and WiFi is always up. Used to check that PsiTerm, PsiMail and PsiWeb
// talk to this firmware exactly as they do to a WiRSa.
//   make hostmodem && ./hostmodem /tmp/modem.sock [baud]
// HOSTMODEM_REDIRECT=host:port sends every dial there instead (a local
// test server standing in for the real one).
// Dialling psiproxy makes it the web proxy (src/proxy.cpp), as on the Atom:
// its fetches go to the real servers, https:// through the PC's OpenSSL 3
// (libssl.so.3, loaded at run time, with the system's CA certificates and
// the host name checked). HOSTPROXY_REDIRECT=host:port sends the proxy's
// own fetches to a local test server instead (plain TCP).
//   ./hostmodem --tcp PORT   is the proxy alone on 127.0.0.1:PORT: each TCP
// connection is a psiproxy call (no AT commands, no pacing), for PsiWeb's
// ARM harness (web/links/emu/run_links.py --proxy 127.0.0.1:PORT) or a PC.
// Extension modules (../src/ext.h) built in by a module's own Makefile take
// part as on the Atom (each module says whether it does in this run): calls
// to ATDT192.168.7.1:<port> on the line, their loop with 127.0.0.1 standing
// in for the PPP link's address, and their web routes on a small web server
// on 127.0.0.1:5980 (HOSTMODEM_HTTP), with no password. HOSTMODEM_ANY=1
// listens on every address instead (for a phone on the LAN).
// MIT licence (see LICENSE at the top of the repository).
#include "../src/modem.h"
#include "../src/atremote.h"
#include "../src/ext.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <dlfcn.h>
#include <sys/time.h>

// ----- OpenSSL 3, loaded at run time (no headers needed to build) -------------
namespace ossl {
typedef const void* (*TMethod)();
static void* (*CTX_new)(const void*);
static int (*CTX_set_default_verify_paths)(void*);
static void (*CTX_set_verify)(void*, int, void*);
static void* (*SSL_new_)(void*);
static int (*set_fd)(void*, int);
static long (*ctrl)(void*, int, long, void*);
static int (*set1_host)(void*, const char*);
static int (*connect_)(void*);
static int (*read_)(void*, void*, int);
static int (*write_)(void*, const void*, int);
static int (*get_error)(const void*, int);
static long (*verify_result)(const void*);
static int (*shutdown_)(void*);
static void (*free_)(void*);
static void* gCtx;

static bool Load()
	{
	static int state = 0;                     // 0 not tried, 1 loaded, -1 not available
	if (state)
		return state > 0;
	state = -1;
	void* h = dlopen("libssl.so.3", RTLD_NOW);
	if (!h)
		return false;
	TMethod method = (TMethod)dlsym(h, "TLS_client_method");
	CTX_new = (void* (*)(const void*))dlsym(h, "SSL_CTX_new");
	CTX_set_default_verify_paths = (int (*)(void*))dlsym(h, "SSL_CTX_set_default_verify_paths");
	CTX_set_verify = (void (*)(void*, int, void*))dlsym(h, "SSL_CTX_set_verify");
	SSL_new_ = (void* (*)(void*))dlsym(h, "SSL_new");
	set_fd = (int (*)(void*, int))dlsym(h, "SSL_set_fd");
	ctrl = (long (*)(void*, int, long, void*))dlsym(h, "SSL_ctrl");
	set1_host = (int (*)(void*, const char*))dlsym(h, "SSL_set1_host");
	connect_ = (int (*)(void*))dlsym(h, "SSL_connect");
	read_ = (int (*)(void*, void*, int))dlsym(h, "SSL_read");
	write_ = (int (*)(void*, const void*, int))dlsym(h, "SSL_write");
	get_error = (int (*)(const void*, int))dlsym(h, "SSL_get_error");
	verify_result = (long (*)(const void*))dlsym(h, "SSL_get_verify_result");
	shutdown_ = (int (*)(void*))dlsym(h, "SSL_shutdown");
	free_ = (void (*)(void*))dlsym(h, "SSL_free");
	if (!method || !CTX_new || !CTX_set_default_verify_paths || !CTX_set_verify || !SSL_new_ || !set_fd
		|| !ctrl || !set1_host || !connect_ || !read_ || !write_ || !get_error || !verify_result
		|| !shutdown_ || !free_)
		return false;
	gCtx = CTX_new(method());
	if (!gCtx)
		return false;
	CTX_set_default_verify_paths(gCtx);
	CTX_set_verify(gCtx, 1 /* SSL_VERIFY_PEER */, 0);
	state = 1;
	return true;
	}
} // namespace ossl

static uint64_t NowUs()
	{
	timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ULL + t.tv_nsec / 1000;
	}

class PcHal : public am::Hal
	{
public:
	int ser = -1, tcp = -1;
	void* ssl = 0;                             // the proxy's TLS, over tcp
	bool tcpEof = false;
	std::vector<uint8_t> peek;                 // read ahead from the TCP socket
	am::Settings saved;
	bool haveSaved = false;
	bool tcpMode = false;                      // --tcp: the line is a TCP client
	bool serClosed = false;
	std::string inject;                        // typed for the client first (--tcp)
	bool quietLog = false;

	uint32_t Millis() override { return (uint32_t)(NowUs() / 1000); }
	uint32_t Micros() override { return (uint32_t)NowUs(); }
	int SerialRead() override
		{
		uint8_t c;
		if (!inject.empty())
			{
			c = (uint8_t)inject[0];
			inject.erase(0, 1);
			return c;
			}
		if (serClosed)
			return -1;
		ssize_t n = recv(ser, &c, 1, MSG_DONTWAIT);
		if (n == 1)
			{
			if (!quietLog)
				{
				fprintf(stderr, "%s", c == '\r' ? "\\r\n<- " : "");
				if (c != '\r') fputc(c >= 32 && c < 127 ? c : '.', stderr);
				}
			return c;
			}
		if (n == 0)
			{
			if (!tcpMode) { fprintf(stderr, "\n[emulator closed the serial bridge]\n"); exit(0); }
			serClosed = true;
			}
		return -1;
		}
	size_t SerialWritable() override { return 128; }
	size_t SerialWrite(const uint8_t* d, size_t n) override
		{
		ssize_t w = send(ser, d, n, MSG_NOSIGNAL);
		if (w > 0 && !quietLog)
			{
			static uint64_t total = 0;
			total += w;
			fprintf(stderr, "\n-> %zd bytes (total %llu): ", w, (unsigned long long)total);
			for (ssize_t i = 0; i < w && i < 40; i++)
				fputc(d[i] >= 32 && d[i] < 127 ? d[i] : '.', stderr);
			fprintf(stderr, "\n<- ");
			}
		return w > 0 ? (size_t)w : 0;
		}
	void SerialBaud(uint32_t b) override { fprintf(stderr, "[serial %u baud]\n", b); }
	bool TcpConnect(const char* host, uint16_t port) override
		{
		return Connect(host, port, getenv("HOSTMODEM_REDIRECT"));
		}
	bool Connect(const char* host, uint16_t port, const char* redirect)
		{
		TcpClose();
		char rh[128];
		if (redirect && strchr(redirect, ':'))
			{
			fprintf(stderr, "\n[dial %s:%u -> %s]", host, port, redirect);
			snprintf(rh, sizeof(rh), "%s", redirect);
			*strrchr(rh, ':') = 0;
			port = (uint16_t)atoi(strrchr(redirect, ':') + 1);
			host = rh;
			}
		char ps[8];
		snprintf(ps, sizeof(ps), "%u", port);
		addrinfo hints = {}, *res = 0;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host, ps, &hints, &res) != 0)
			{
			fprintf(stderr, "\n[dial %s:%u: no such host]\n", host, port);
			return false;
			}
		int s = socket(res->ai_family, SOCK_STREAM, 0);
		bool ok = false;
		if (s >= 0)
			{
			fcntl(s, F_SETFL, O_NONBLOCK);
			if (connect(s, res->ai_addr, res->ai_addrlen) == 0)
				ok = true;
			else if (errno == EINPROGRESS)
				{
				pollfd pf = { s, POLLOUT, 0 };
				int err = 0;
				socklen_t el = sizeof(err);
				ok = poll(&pf, 1, 10000) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0;
				}
			}
		freeaddrinfo(res);
		if (!ok)
			{
			fprintf(stderr, "\n[dial %s:%u: refused]\n", host, port);
			if (s >= 0) close(s);
			return false;
			}
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		tcp = s;
		tcpEof = false;
		fprintf(stderr, "\n[dial %s:%u: connected]\n", host, port);
		return true;
		}
	// the web proxy's connection: plain, or TLS checked against the PC's CAs
	bool UpConnect(const char* host, uint16_t port, bool tls, char* why, size_t whyMax) override
		{
		const char* redirect = getenv("HOSTPROXY_REDIRECT");
		if (redirect && *redirect)
			tls = false;
		if (tls && !ossl::Load())
			{
			snprintf(why, whyMax, "no libssl.so.3 on this PC");
			return false;
			}
		if (!Connect(host, port, redirect))
			{
			snprintf(why, whyMax, "no connection to %s:%u", host, port);
			return false;
			}
		if (!tls)
			return true;
		// the handshake blocking (15 s at most), then back to non-blocking
		fcntl(tcp, F_SETFL, 0);
		timeval tv = { 15, 0 };
		setsockopt(tcp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(tcp, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		ssl = ossl::SSL_new_(ossl::gCtx);
		ossl::set_fd(ssl, tcp);
		ossl::ctrl(ssl, 55 /* SSL_CTRL_SET_TLSEXT_HOSTNAME */, 0, (void*)host);
		ossl::set1_host(ssl, host);
		uint64_t t0 = NowUs();
		if (ossl::connect_(ssl) != 1)
			{
			long v = ossl::verify_result(ssl);
			snprintf(why, whyMax, v ? "certificate not trusted (OpenSSL verify error %ld)" : "TLS handshake failed", v);
			fprintf(stderr, "\n[tls %s: %s]\n", host, why);
			TcpClose();
			return false;
			}
		fprintf(stderr, "\n[tls %s: handshake %.0f ms]\n", host, (NowUs() - t0) / 1000.0);
		fcntl(tcp, F_SETFL, O_NONBLOCK);
		return true;
		}
	void Idle() override { usleep(1000); }
	void MemInfo(char* o, size_t m) override { snprintf(o, m, "Memory: (a PC)"); }
	void Log(const char* l) override { fprintf(stderr, "\n[%s]\n", l); Record(l); }
	void Fill()
		{
		if (tcp < 0 || tcpEof || peek.size() >= 4096) return;
		uint8_t b[4096];
		if (ssl)
			{
			int n = ossl::read_(ssl, b, (int)(sizeof(b) - peek.size()));
			if (n > 0) peek.insert(peek.end(), b, b + n);
			else
				{
				int e = ossl::get_error(ssl, n);
				if (e != 2 && e != 3) tcpEof = true;     // (not WANT_READ/WANT_WRITE)
				}
			return;
			}
		ssize_t n = recv(tcp, b, sizeof(b) - peek.size(), MSG_DONTWAIT);
		if (n > 0) peek.insert(peek.end(), b, b + n);
		else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) tcpEof = true;
		}
	bool TcpOpen() override { Fill(); return tcp >= 0 && !tcpEof; }
	size_t TcpAvailable() override { Fill(); return peek.size(); }
	size_t TcpRead(uint8_t* b, size_t m) override
		{
		Fill();
		size_t n = peek.size() < m ? peek.size() : m;
		memcpy(b, peek.data(), n);
		peek.erase(peek.begin(), peek.begin() + n);
		return n;
		}
	size_t TcpWrite(const uint8_t* d, size_t n) override
		{
		if (tcp < 0) return 0;
		if (ssl)
			{
			int w = ossl::write_(ssl, d, (int)n);
			return w > 0 ? (size_t)w : 0;
			}
		ssize_t w = send(tcp, d, n, MSG_NOSIGNAL);
		return w > 0 ? (size_t)w : 0;
		}
	void TcpClose() override
		{
		if (ssl) { ossl::shutdown_(ssl); ossl::free_(ssl); ssl = 0; }
		if (tcp >= 0) { close(tcp); fprintf(stderr, "\n[tcp closed]\n"); }
		tcp = -1;
		peek.clear();
		}
	// A pretend WiFi world, for PsiConnect (AT$SCAN, AT$JOIN, AT$STAT?): it starts
	// joined to "Home-WiFi"; a join takes 2.5 s and works with the password
	// "psion5mx" (none for the open one), else fails as the Atom would
	// report it. HOSTMODEM_JOIN_MS changes how long a join takes;
	// HOSTMODEM_WIFI_SAVED=1 starts it with "Home-WiFi" saved, as AT&W would. The scan has a duplicate, a hidden network and awkward names.
	// 2.2: HOSTMODEM_NETS=1 saves "Home-WiFi" and "Lab-WiFi" (both psion5mx);
	// HOSTMODEM_WG=1 adds a WireGuard config (throwaway keys made at the start)
	// in mode Auto and a pretend tunnel; HOSTMODEM_HOME=1 puts the uplink on
	// the home network (Auto then keeps the tunnel down).
	struct FakeNet { const char* ssid; int rssi; int secure; int ch; };
	std::string wifiCur = "Home-WiFi", wifiWant, wifiWantPass;
	bool wifiUp = true;
	uint64_t wifiJoinAt = 0;
	int wifiFail = am::EWifiFailNone;
	static const FakeNet* Nets(int& aCount)
		{
		static const FakeNet kNets[] = {
			{ "Lab-WiFi", -61, 1, 11 },
			{ "Home-WiFi", -83, 1, 36 },          // (the same name further away: listed once)
			{ "BTHub6-7X2Q", -74, 1, 1 },
			{ "Home-WiFi", -48, 1, 6 },
			{ "", -70, 1, 3 },                    // hidden
			{ "Cafe \"Guest\" Wi-Fi", -80, 0, 6 },
			{ "Caf\xc3\xa9 a/b", -88, 1, 9 },     // UTF-8, and an "a/"
			{ "VM5551234", -90, 1, 1 } };
		aCount = (int)(sizeof(kNets) / sizeof(kNets[0]));
		return kNets;
		}
	void WifiTick()
		{
		if (!wifiJoinAt || NowUs() < wifiJoinAt)
			return;
		wifiJoinAt = 0;
		int n;
		const FakeNet* nets = Nets(n);
		const FakeNet* f = 0;
		for (int i = 0; i < n; i++)
			if (nets[i].ssid[0] && wifiWant == nets[i].ssid)
				f = &nets[i];
		if (!f)
			wifiFail = am::EWifiFailNoNetwork;
		else if (f->secure ? wifiWantPass != "psion5mx" : !wifiWantPass.empty())
			wifiFail = am::EWifiFailPassword;
		else
			{
			wifiUp = true;
			wifiCur = wifiWant;
			}
		fprintf(stderr, "\n[wifi: %s \"%s\"]\n", wifiUp ? "joined" : "could not join", wifiWant.c_str());
		}
	bool WifiUp() override { WifiTick(); return wifiUp; }
	void WifiBegin(const char* s, const char*p) override
		{
		wifiWant = s;
		wifiWantPass = p;
		wifiUp = false;
		wifiFail = am::EWifiFailNone;
		const char* ms = getenv("HOSTMODEM_JOIN_MS");   // (the emulator's time can run far ahead of the PC's)
		wifiJoinAt = NowUs() + (uint64_t)(ms ? atoi(ms) : 2500) * 1000;
		fprintf(stderr, "\n[wifi: joining \"%s\"]\n", s);
		}
	void WifiEnd() override { wifiUp = false; }
	void WifiInfo(char* o, size_t m) override { snprintf(o, m, "\"%s\" IP 127.0.0.1 RSSI -48", wifiCur.c_str()); }
	int WifiScanFull(am::WifiNet* o, int m) override
		{
		int n;
		const FakeNet* nets = Nets(n);
		int k = 0;
		for (int i = 0; i < n && k < m; i++, k++)
			{
			snprintf(o[k].ssid, sizeof(o[k].ssid), "%s", nets[i].ssid);
			o[k].rssi = (int8_t)nets[i].rssi;
			o[k].secure = (uint8_t)nets[i].secure;
			o[k].channel = (uint8_t)nets[i].ch;
			}
		fprintf(stderr, "\n[wifi: scan, %d networks]\n", k);
		return k;
		}
	bool WifiLink(char* s, size_t sm, char* ip, size_t im, int& rssi) override
		{
		if (!WifiUp()) return false;
		snprintf(s, sm, "%s", wifiCur.c_str());
		snprintf(ip, im, "127.0.0.1");
		rssi = wifiCur == "Home-WiFi" ? -48 : -61;
		return true;
		}
	int WifiFail() override { WifiTick(); return wifiFail; }
	int Internet() override { return WifiUp() ? 1 : -1; }
	int DnsOk() override { return WifiUp() ? 1 : -1; }
	bool LoadSettings(am::Settings& s) override { if (!haveSaved) return false; s = saved; return true; }
	bool SaveSettings(const am::Settings& s) override { saved = s; haveSaved = true; return true; }
	// 2.2: the other records, kept in memory for the run
	am::Settings2 saved2; bool haveSaved2 = false;
	am::NetList savedNets; bool haveNets = false;
	am::WgConfig savedWg; bool haveWg = false;
	bool LoadSettings2(am::Settings2& s, size_t& len) override { len = haveSaved2 ? sizeof(s) : 0; if (haveSaved2) s = saved2; return haveSaved2; }
	bool SaveSettings2(const am::Settings2& s) override { saved2 = s; haveSaved2 = true; return true; }
	bool LoadNets(am::NetList& l, size_t& len) override { len = haveNets ? sizeof(l) : 0; if (haveNets) l = savedNets; return haveNets; }
	bool SaveNets(const am::NetList& l) override { savedNets = l; haveNets = true; fprintf(stderr, "\n[nets saved: %u]\n", (unsigned)l.count); return true; }
	bool LoadWg(am::WgConfig& c, size_t& len) override { len = haveWg ? sizeof(c) : 0; if (haveWg) c = savedWg; return haveWg; }
	bool SaveWg(const am::WgConfig& c) override { savedWg = c; haveWg = true; return true; }
	// the uplink: a phone's hotspot, or (HOSTMODEM_HOME=1) the home network 192.168.10.0/24
	bool UplinkAddr(uint32_t& ip, uint32_t& mask) override
		{
		if (!WifiUp()) { ip = mask = 0; return false; }
		ip = getenv("HOSTMODEM_HOME") ? am::Ip4(192, 168, 10, 50) : am::Ip4(172, 20, 10, 2);
		mask = getenv("HOSTMODEM_HOME") ? am::Ip4(255, 255, 255, 0) : am::Ip4(255, 255, 255, 240);
		return true;
		}
	// a pretend tunnel: connecting for 1.5 s, then up, a handshake every 2 minutes
	bool wgOn = false;
	uint64_t wgStartUs = 0;
	void WgApply(const am::WgConfig&, bool want) override
		{
		if (want == wgOn) return;
		wgOn = want;
		wgStartUs = NowUs();
		fprintf(stderr, "\n[wireguard: %s]\n", want ? "starting" : "stopped");
		}
	void WgInfo(am::WgStatus& o) override
		{
		memset(&o, 0, sizeof(o));
		o.handshakeAge = -1;
		if (!wgOn) { o.state = am::EWgStOff; return; }
		uint64_t ms = (NowUs() - wgStartUs) / 1000;
		if (ms < 1500) { o.state = am::EWgStConnecting; return; }
		o.state = am::EWgStUp;
		o.handshakeAge = (int32_t)(((ms - 1500) / 1000) % 120);
		o.rx = (uint32_t)(ms / 10);
		o.tx = (uint32_t)(ms / 20);
		o.endpointIp = am::Ip4(203, 0, 113, 7);
		}
	std::string webInfo;                       // (the web server's address, when there is one)
	void WebInfo(char* o, size_t m) override { snprintf(o, m, "%s", webInfo.c_str()); }
	void Random(uint8_t* o, size_t n) override
		{
		FILE* f = fopen("/dev/urandom", "rb");
		if (!f || fread(o, 1, n, f) != n)
			am::Hal::Random(o, n);
		if (f) fclose(f);
		}
	void Led(am::LedState s) override
		{
		static const char* const kNames[] = { "no WiFi", "WiFi up", "connected", "data", "connecting" };
		if (s != am::ELedData) fprintf(stderr, "\n[LED: %s]\n", kNames[s]);
		}
	};

// HOSTMODEM_ATPORT=PORT: AT commands over TCP on 127.0.0.1:PORT, as the Atom
// serves them on 192.168.7.1:2323 during a PPP call (board/pppat.cpp) - here
// always, with no PPP: a test seam for PsiConnect's TCP route
struct AtPort
	{
	int srv = -1, cli = -1;
	am::AtRemote session;
	void Open(int aPort)
		{
		srv = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = htons((uint16_t)aPort);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(srv, (sockaddr*)&a, sizeof(a)) != 0 || listen(srv, 2) != 0)
			{
			perror("hostmodem: AT port");
			close(srv);
			srv = -1;
			return;
			}
		fcntl(srv, F_SETFL, O_NONBLOCK);
		fprintf(stderr, "hostmodem: AT commands over TCP on 127.0.0.1:%d\n", aPort);
		}
	void Serve(am::Modem& aModem, am::Hal& aHal)
		{
		if (srv < 0)
			return;
		int c = accept(srv, 0, 0);
		if (c >= 0)
			{
			if (cli >= 0)
				close(c);                        // one client at a time
			else
				{
				cli = c;
				fcntl(cli, F_SETFL, O_NONBLOCK);
				session.Begin(aHal.Millis());
				}
			}
		if (cli < 0)
			return;
		uint8_t in[256];
		bool gone = false;
		while (session.CanTake())
			{
			ssize_t n = recv(cli, in, sizeof(in), MSG_PEEK);
			if (n == 0) { gone = true; break; }
			if (n < 0) { gone = errno != EAGAIN && errno != EWOULDBLOCK; break; }
			size_t took = session.Input(aModem, in, (size_t)n, aHal.Millis());
			if (took && recv(cli, in, took, 0) < 0) { gone = true; break; }
			if (took < (size_t)n)
				break;
			}
		const uint8_t* p;
		size_t left;
		while (!gone && (left = session.Pending(p)) > 0)
			{
			ssize_t w = send(cli, p, left, MSG_NOSIGNAL);
			if (w < 0)
				{
				gone = errno != EAGAIN && errno != EWOULDBLOCK;
				break;
				}
			session.Sent((size_t)w);
			}
		if (gone || session.Expired(aHal.Millis()))
			{
			close(cli);
			cli = -1;
			session.Begin(aHal.Millis());
			}
		}
	};

// the extension modules' web routes, as the Atom's web UI serves them (but
// with no password): one request a connection, then closed
struct ExtHttp
	{
	int srv = -1;
	struct Cli { int fd; std::string in; std::string out; size_t pos; };
	std::vector<Cli> cli;
	struct Query : am::ExtQuery
		{
		std::string q;
		mutable std::string last;
		const char* Arg(const char* aName) const override
			{
			last.clear();
			std::string k = std::string(aName) + "=";
			for (size_t at = 0; at < q.size(); )
				{
				size_t end = q.find('&', at);
				if (end == std::string::npos) end = q.size();
				if (q.compare(at, k.size(), k) == 0)
					{
					last = q.substr(at + k.size(), end - at - k.size());
					break;
					}
				at = end + 1;
				}
			return last.c_str();
			}
		};
	void Open(int aPort, bool aAny)
		{
		srv = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = htons((uint16_t)aPort);
		a.sin_addr.s_addr = htonl(aAny ? INADDR_ANY : INADDR_LOOPBACK);
		if (bind(srv, (sockaddr*)&a, sizeof(a)) != 0 || listen(srv, 8) != 0)
			{
			perror("hostmodem: the web port");
			close(srv);
			srv = -1;
			return;
			}
		fcntl(srv, F_SETFL, O_NONBLOCK);
		fprintf(stderr, "hostmodem: the extension modules' pages at http://127.0.0.1:%d/\n", aPort);
		}
	static std::string Reply(int aCode, const char* aType, const std::string& aBody, const char* aCache, bool aGzip)
		{
		char h[320];
		snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: %s\r\n%sConnection: close\r\n\r\n",
			aCode, aCode == 200 ? "OK" : aCode == 403 ? "Forbidden" : "Not Found", aType, aBody.size(), aCache ? aCache : "no-store",
			aGzip ? "Content-Encoding: gzip\r\n" : "");
		return h + aBody;
		}
	std::string Answer(const std::string& aReq, am::Modem& aModem)
		{
		size_t sp = aReq.find(' '), sp2 = aReq.find(' ', sp + 1);
		std::string path = sp2 != std::string::npos ? aReq.substr(sp + 1, sp2 - sp - 1) : "/";
		size_t qm = path.find('?');
		std::string base = path.substr(0, qm);
		for (int i = 0; i < aModem.ExtensionCount(); i++)
			{
			am::Extension* x = aModem.ExtensionAt(i);
			for (const am::ExtRoute* r = x->Routes(); r && r->path; r++)
				if (base == r->path)
					{
					Query q;
					if (qm != std::string::npos)
						q.q = path.substr(qm + 1);
					am::ExtReply rep;
					x->Get(r->path, q, rep);
					return Reply(rep.status, rep.type, std::string((const char*)rep.body, rep.len), rep.cache, rep.gzip);
					}
			}
		return Reply(404, "text/plain", "Not found\n", 0, false);
		}
	void Serve(am::Modem& aModem)
		{
		if (srv < 0)
			return;
		int c = accept(srv, 0, 0);
		if (c >= 0)
			{
			fcntl(c, F_SETFL, O_NONBLOCK);
			cli.push_back({ c, "", "", 0 });
			}
		for (size_t i = 0; i < cli.size(); i++)
			{
			Cli& k = cli[i];
			bool done = false;
			if (k.out.empty())
				{
				char b[2048];
				ssize_t n = recv(k.fd, b, sizeof(b), MSG_DONTWAIT);
				if (n > 0)
					k.in.append(b, (size_t)n);
				else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
					done = true;
				if (k.in.find("\r\n\r\n") != std::string::npos)
					k.out = Answer(k.in, aModem);
				}
			if (!k.out.empty())
				{
				ssize_t w = send(k.fd, k.out.data() + k.pos, k.out.size() - k.pos, MSG_NOSIGNAL | MSG_DONTWAIT);
				if (w > 0)
					k.pos += (size_t)w;
				else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
					done = true;
				if (k.pos >= k.out.size())
					done = true;
				}
			if (done)
				{
				close(k.fd);
				cli.erase(cli.begin() + i);
				i--;
				}
			}
		}
	};

// --tcp PORT: each connection to 127.0.0.1:PORT is a psiproxy call
static int TcpProxy(int aPort)
	{
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)aPort);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(srv, (sockaddr*)&a, sizeof(a)) != 0 || listen(srv, 4) != 0)
		{
		perror("bind");
		return 1;
		}
	fprintf(stderr, "hostmodem: the web proxy alone on 127.0.0.1:%d\n", aPort);
	static uint8_t ring[96 * 1024];
	for (;;)
		{
		PcHal hal;
		hal.ser = accept(srv, 0, 0);
		if (hal.ser < 0)
			continue;
		setsockopt(hal.ser, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		hal.tcpMode = true;
		hal.quietLog = getenv("HOSTMODEM_VERBOSE") == 0;
		// no echo, no result codes, no pacing (as if saved), then the dial
		am::FactoryDefaults(hal.saved);
		hal.saved.echo = 0;
		hal.saved.quiet = 1;
		hal.saved.paceAuto = 0;
		hal.saved.paceRate = 0;
		hal.haveSaved = true;
		hal.inject = "ATDT psiproxy\r";
		fprintf(stderr, "[client connected]\n");
		am::Modem modem(hal, ring, sizeof(ring));
		modem.Begin();
		bool dialled = false;
		for (;;)
			{
			modem.Loop();
			if (modem.ProxyCall())
				dialled = true;
			if (hal.serClosed || (dialled && !modem.Connected() && !modem.Buffered()))
				break;
			pollfd p[2] = { { hal.ser, POLLIN, 0 }, { hal.tcp, POLLIN, 0 } };
			poll(p, hal.tcp >= 0 ? 2 : 1, modem.Buffered() || !hal.inject.empty() ? 0 : 1);
			}
		hal.TcpClose();
		close(hal.ser);
		fprintf(stderr, "[client gone]\n");
		}
	}

int main(int argc, char** argv)
	{
	if (argc < 2)
		{
		fprintf(stderr, "usage: hostmodem SOCKET [baud]\n       hostmodem --tcp PORT\n");
		return 2;
		}
	signal(SIGPIPE, SIG_IGN);
	if (strcmp(argv[1], "--tcp") == 0)
		return TcpProxy(argc > 2 ? atoi(argv[2]) : 8080);
	unlink(argv[1]);
	int srv = socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un a = {};
	a.sun_family = AF_UNIX;
	strncpy(a.sun_path, argv[1], sizeof(a.sun_path) - 1);
	if (bind(srv, (sockaddr*)&a, sizeof(a)) != 0 || listen(srv, 1) != 0)
		{
		perror("bind");
		return 1;
		}
	fprintf(stderr, "hostmodem: waiting on %s\n", argv[1]);
	PcHal hal;
	hal.ser = accept(srv, 0, 0);
	fprintf(stderr, "hostmodem: emulator connected\n<- ");
	static uint8_t ring[96 * 1024];
	am::Modem modem(hal, ring, sizeof(ring));
	if (argc > 2)
		{
		am::FactoryDefaults(hal.saved);
		hal.saved.baud = (uint32_t)atol(argv[2]);
		hal.haveSaved = true;
		}
	if (getenv("HOSTMODEM_WIFI_SAVED"))
		{
		// as an Atom set up already: "Home-WiFi" saved (so ATZ goes back to it)
		if (!hal.haveSaved)
			am::FactoryDefaults(hal.saved);
		snprintf(hal.saved.ssid, sizeof(hal.saved.ssid), "Home-WiFi");
		snprintf(hal.saved.pass, sizeof(hal.saved.pass), "psion5mx");
		hal.haveSaved = true;
		}
	if (getenv("HOSTMODEM_NETS"))
		{
		// (2.2) networks saved already: "Home-WiFi" and "Lab-WiFi", both password psion5mx
		am::NetListInit(hal.savedNets);
		am::NetUpsert(hal.savedNets, "Home-WiFi", "psion5mx", -1);
		am::NetUpsert(hal.savedNets, "Lab-WiFi", "psion5mx", -1);
		hal.haveNets = true;
		}
	if (getenv("HOSTMODEM_WG"))
		{
		// (2.2) a WireGuard config, with throwaway keys made now (nothing real,
		// nothing kept): the endpoint is a documentation address (RFC 5737)
		char key[3][48];
		for (int k = 0; k < 3; k++)
			{
			uint8_t b[32];
			for (int i = 0; i < 32; i++) b[i] = (uint8_t)(rand() ^ (NowUs() >> (i % 16)));
			static const char* const kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			int n = 0;
			for (int i = 0; i < 30; i += 3)
				{
				uint32_t w = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
				key[k][n++] = kB64[(w >> 18) & 63]; key[k][n++] = kB64[(w >> 12) & 63];
				key[k][n++] = kB64[(w >> 6) & 63]; key[k][n++] = kB64[w & 63];
				}
			uint32_t w = (b[30] << 16) | (b[31] << 8);
			key[k][n++] = kB64[(w >> 18) & 63]; key[k][n++] = kB64[(w >> 12) & 63]; key[k][n++] = kB64[(w >> 6) & 63];
			key[k][n++] = '=';
			key[k][n] = 0;
			}
		char text[600], why[80];
		snprintf(text, sizeof(text), "[Interface]\nPrivateKey = %s\nAddress = 192.168.3.2/32\nDNS = 192.168.10.1\n"
			"[Peer]\nPublicKey = %s\nPresharedKey = %s\nAllowedIPs = 192.168.10.0/24\nEndpoint = 203.0.113.7:51820\n"
			"PersistentKeepalive = 25\n", key[0], key[1], key[2]);
		if (!am::WgParse(text, hal.savedWg, why, sizeof(why)))
			fprintf(stderr, "hostmodem: the test WireGuard config: %s\n", why);
		else
			hal.haveWg = true;
		memset(text, 0, sizeof(text));
		memset(key, 0, sizeof(key));
		am::FactoryDefaults2(hal.saved2);
		hal.saved2.wgMode = am::EWgModeAuto;
		hal.haveSaved2 = true;
		}
	// the extension modules built in (none in this directory's build)
	static ExtHttp extHttp;
	bool any = getenv("HOSTMODEM_ANY") != 0;
	bool routes = false;
	for (am::Extension* x = am::ExtModules(); x; x = x->iNextModule)
		if (x->Begin(hal) && modem.AddExtension(*x) && x->Routes())
			routes = true;
	if (routes)
		{
		int httpPort = getenv("HOSTMODEM_HTTP") ? atoi(getenv("HOSTMODEM_HTTP")) : 5980;
		char w[64];
		snprintf(w, sizeof(w), "http://127.0.0.1:%d/", httpPort);
		hal.webInfo = w;
		extHttp.Open(httpPort, any);
		}
	modem.Begin();
	static AtPort at;
	if (getenv("HOSTMODEM_ATPORT"))
		at.Open(atoi(getenv("HOSTMODEM_ATPORT")));
	for (;;)
		{
		modem.Loop();
		at.Serve(modem, hal);
		for (int i = 0; i < modem.ExtensionCount(); i++)
			modem.ExtensionAt(i)->Loop(any ? "0.0.0.0" : "127.0.0.1", any ? "" : "127.0.0.1");
		extHttp.Serve(modem);
		pollfd p[2] = { { hal.ser, POLLIN, 0 }, { hal.tcp, POLLIN, 0 } };
		poll(p, hal.tcp >= 0 ? 2 : 1, modem.Buffered() ? 0 : 1);
		}
	}
