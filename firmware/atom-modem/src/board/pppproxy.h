// pppproxy.h - the web proxy for a Psion that is online through PPP. While a
// PPP call is up, the Psion is on the modem's own little network
// (192.168.7.2 -> 192.168.7.1), so PsiWeb can use the proxy as a plain TCP
// server: proxy host 192.168.7.1, port 8080. Each connection is a psiproxy
// call (the same code as ATDT psiproxy: the Psion's requests in, simplified
// pages out), without the dial or the serial pacing: PPP carries it. The
// server exists only while a PPP link is up, and takes connections from the
// PPP side only. Memory for the call is taken when the link comes up and
// given back when it goes.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_PPPPROXY_H
#define ATOM_PPPPROXY_H

#include <Arduino.h>
#include <WiFi.h>
#include "../modem.h"

namespace am {

class PppProxy
	{
public:
	static const uint16_t kPort = 8080;
	PppProxy();
	void Loop(Modem& aModem, Hal& aHal);          // from loop(): cheap when no PPP link is up

private:
	void Open(Modem& aModem, Hal& aHal);
	void Close(Hal& aHal);
	void EndClient();
	void Serve(Modem& aModem);

	WiFiServer* iServer;
	WiFiClient iClient;
	Proxy* iProxy;
	Ring iRing;
	uint8_t* iRingBuf;
	bool iActive;
	bool iHaveClient;
	bool iEnding;                                  // the call is over: the rest of the ring goes first
	uint8_t iIn[512];
	size_t iInLen;
	};

} // namespace am

#endif
