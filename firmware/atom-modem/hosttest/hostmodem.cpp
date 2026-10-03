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
// MIT licence (see LICENSE at the top of the repository).
#include "../src/modem.h"
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
	void Log(const char* l) override { fprintf(stderr, "\n[%s]\n", l); }
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
	bool WifiUp() override { return true; }
	void WifiBegin(const char*, const char*) override {}
	void WifiEnd() override {}
	void WifiInfo(char* o, size_t m) override { snprintf(o, m, "\"(PC)\" IP 127.0.0.1 RSSI 0"); }
	bool LoadSettings(am::Settings& s) override { if (!haveSaved) return false; s = saved; return true; }
	bool SaveSettings(const am::Settings& s) override { saved = s; haveSaved = true; return true; }
	void Led(am::LedState s) override
		{
		static const char* const kNames[] = { "no WiFi", "WiFi up", "connected", "data", "connecting" };
		if (s != am::ELedData) fprintf(stderr, "\n[LED: %s]\n", kNames[s]);
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
	modem.Begin();
	for (;;)
		{
		modem.Loop();
		pollfd p[2] = { { hal.ser, POLLIN, 0 }, { hal.tcp, POLLIN, 0 } };
		poll(p, hal.tcp >= 0 ? 2 : 1, modem.Buffered() ? 0 : 1);
		}
	}
