// hostmodem.cpp - the Atom modem's code (modem.cpp, unchanged) as a PC
// program: the "serial line" is a unix socket that the Psion emulator's
// serial bridge connects to (--serial-bridge-socket), TCP is the PC's own,
// and WiFi is always up. Used to check that PsiTerm, PsiMail and PsiWeb
// talk to this firmware exactly as they do to a WiRSa.
//   make hostmodem && ./hostmodem /tmp/modem.sock [baud]
// HOSTMODEM_REDIRECT=host:port sends every dial there instead (a local
// test server standing in for the real one).
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
	bool tcpEof = false;
	std::vector<uint8_t> peek;                 // read ahead from the TCP socket
	am::Settings saved;
	bool haveSaved = false;

	uint32_t Millis() override { return (uint32_t)(NowUs() / 1000); }
	uint32_t Micros() override { return (uint32_t)NowUs(); }
	int SerialRead() override
		{
		uint8_t c;
		ssize_t n = recv(ser, &c, 1, MSG_DONTWAIT);
		if (n == 1)
			{
			fprintf(stderr, "%s", c == '\r' ? "\\r\n<- " : "");
			if (c != '\r') fputc(c >= 32 && c < 127 ? c : '.', stderr);
			return c;
			}
		if (n == 0) { fprintf(stderr, "\n[emulator closed the serial bridge]\n"); exit(0); }
		return -1;
		}
	size_t SerialWritable() override { return 128; }
	size_t SerialWrite(const uint8_t* d, size_t n) override
		{
		ssize_t w = send(ser, d, n, 0);
		if (w > 0)
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
		TcpClose();
		char rh[128];
		const char* redirect = getenv("HOSTMODEM_REDIRECT");
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
	void Fill()
		{
		if (tcp < 0 || tcpEof || peek.size() >= 4096) return;
		uint8_t b[4096];
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
		ssize_t w = send(tcp, d, n, MSG_NOSIGNAL);
		return w > 0 ? (size_t)w : 0;
		}
	void TcpClose() override
		{
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

int main(int argc, char** argv)
	{
	if (argc < 2)
		{
		fprintf(stderr, "usage: hostmodem SOCKET [baud]\n");
		return 2;
		}
	signal(SIGPIPE, SIG_IGN);
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
