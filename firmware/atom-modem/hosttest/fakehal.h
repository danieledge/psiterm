// fakehal.h - a simulated Atom for the host tests: a clock the test moves,
// a UART with a 128-byte transmit FIFO that drains at the line rate, a TCP
// "server" whose bytes wait in a queue (so back-pressure can be seen), and
// WiFi and NVS that do what they are told. MIT licence.
#ifndef FAKEHAL_H
#define FAKEHAL_H
#include "../src/modem.h"
#include <string>
#include <vector>
#include <deque>
#include <string.h>
#include <stdio.h>

struct TxByte { uint8_t c; uint64_t us; };     // a byte as it left the UART

class FakeHal : public am::Hal
	{
public:
	uint64_t nowUs = 0;
	uint32_t baud = 0;
	std::deque<uint8_t> rx;                    // Psion -> modem
	std::deque<uint8_t> fifo;                  // modem -> UART FIFO (not yet on the wire)
	std::vector<TxByte> wire;                  // what the Psion received, with times
	double drainCredit = 0;
	bool wifi = true;
	std::string wifiSsid, wifiPass;
	bool tcpOk = true;                         // connects succeed
	bool tcpOpen = false;
	std::string tcpHost; int tcpPort = 0;
	std::deque<uint8_t> server;                // the server's bytes, waiting to be read
	std::string toServer;
	size_t maxTcpRead = 0;                     // biggest single read
	bool haveSaved = false;
	am::Settings saved;
	bool dcd = false;
	int led = -1;

	uint32_t Millis() override { return (uint32_t)(nowUs / 1000); }
	uint32_t Micros() override { return (uint32_t)nowUs; }
	int SerialRead() override
		{
		if (rx.empty()) return -1;
		int c = rx.front(); rx.pop_front(); return c;
		}
	size_t SerialWritable() override { return 128 - fifo.size(); }
	size_t SerialWrite(const uint8_t* d, size_t n) override
		{
		if (fifo.size() >= 128)
			Advance(100);                      // a full FIFO: the write waits for the line, as on the Atom
		size_t room = 128 - fifo.size();
		if (n > room) n = room;
		for (size_t i = 0; i < n; i++) fifo.push_back(d[i]);
		return n;
		}
	void SerialBaud(uint32_t b) override { Drain(); baud = b; }
	bool TcpConnect(const char* h, uint16_t p) override
		{
		tcpHost = h; tcpPort = p;
		if (!tcpOk) return false;
		tcpOpen = true;
		return true;
		}
	bool TcpOpen() override { return tcpOpen; }
	size_t TcpAvailable() override { return server.size(); }
	size_t TcpRead(uint8_t* b, size_t m) override
		{
		size_t n = 0;
		while (n < m && !server.empty()) { b[n++] = server.front(); server.pop_front(); }
		if (n > maxTcpRead) maxTcpRead = n;
		return n;
		}
	size_t TcpWrite(const uint8_t* d, size_t n) override { toServer.append((const char*)d, n); return n; }
	void TcpClose() override { tcpOpen = false; server.clear(); }
	bool WifiUp() override { return wifi; }
	void WifiBegin(const char* s, const char* p) override { wifiSsid = s; wifiPass = p; }
	void WifiEnd() override { wifi = false; }
	void WifiInfo(char* o, size_t m) override { snprintf(o, m, "\"%s\" IP 192.168.1.50 RSSI -60", wifiSsid.c_str()); }
	bool LoadSettings(am::Settings& s) override { if (!haveSaved) return false; s = saved; return true; }
	bool SaveSettings(const am::Settings& s) override { saved = s; haveSaved = true; return true; }
	void Dcd(bool on) override { dcd = on; }
	void Led(am::LedState s) override { led = s; }

	// the UART puts bytes on the wire at baud/10 bytes a second
	void Drain()
		{
		while (!fifo.empty() && drainCredit >= 1)
			{
			wire.push_back({ fifo.front(), nowUs });
			fifo.pop_front();
			drainCredit -= 1;
			}
		if (fifo.empty() && drainCredit > 1) drainCredit = 1;
		}
	void Advance(uint64_t us)
		{
		nowUs += us;
		drainCredit += (double)us * (baud ? baud : 115200) / 10.0 / 1e6;
		Drain();
		}
	std::string Wire() const
		{
		std::string s;
		for (auto& b : wire) s += (char)b.c;
		return s;
		}
	};
#endif
