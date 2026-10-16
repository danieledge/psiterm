// pppat.h - AT commands over PPP (2.2, atremote.h): while a PPP call is up,
// PsiConnect on the Psion can send the modem its AT commands over TCP to
// 192.168.7.1:2323, as it would over the serial port that PPP is using. One
// client at a time (a second is closed at once), from the PPP side only (the
// server is bound to the PPP address, and each client's local address is
// checked too); no password, as only the Psion is on that side. The
// commands that would cut the link or change the serial line are refused
// ("+NOTVIAPPP: why", ERROR). Closed after 120 s without input.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_PPPAT_H
#define ATOM_PPPAT_H

#include <Arduino.h>
#include <WiFi.h>
#include "../modem.h"
#include "../atremote.h"

namespace am {

class PppAt
	{
public:
	PppAt() : iServer(0), iActive(false), iHaveClient(false) {}
	void Loop(Modem& aModem, Hal& aHal);          // from loop(): cheap when no PPP link is up

private:
	void Open(Hal& aHal);
	void Close(Hal& aHal);
	void EndClient(Hal& aHal);
	void Serve(Modem& aModem, Hal& aHal);

	WiFiServer* iServer;
	WiFiClient iClient;
	AtRemote iSession;
	bool iActive;
	bool iHaveClient;
	uint8_t iIn[256];
	};

} // namespace am

#endif
