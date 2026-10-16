// ppptls.h - a stunnel-style TLS-offload relay on the PPP side, so a Psion
// online over PPP can reach TLS mail servers (IMAPS/SMTPS/POP3S) without doing
// any crypto on its 36 MHz ARM. While a PPP call is up the Psion is on the
// modem's little network (192.168.7.2 -> gateway 192.168.7.1 = the Atom).
// PsiMail (PPP route) connects IN PLAINTEXT to 192.168.7.1 on the standard TLS
// mail ports; the Atom accepts, opens a real TLS connection to the configured
// upstream host, checks its certificate and relays the bytes both ways:
//   Psion -> 192.168.7.1:993 (plaintext IMAP) -> Atom -> TLS -> imapHost:993
//   Psion -> 192.168.7.1:465 (plaintext SMTP) -> Atom -> TLS -> smtpHost:465
//   Psion -> 192.168.7.1:995 (plaintext POP3) -> Atom -> TLS -> popHost:995
// The upstreams are AT$MAILIMAP / AT$MAILSMTP / AT$MAILPOP (settings.h); an
// empty one is not offered. The certificate is checked unless AT$TLSV=0.
//
// Modelled on PppProxy (pppproxy.cpp): it listens on the PPP IP only, so the
// ports are not visible on the WiFi LAN, checks each client's localIP() as
// defence in depth, and writes to the Psion with a raw non-blocking
// ::send(fd, ..., MSG_DONTWAIT) - NetworkClient::write() blocks up to ~10 s on
// a full window and the loop task is also what drains the PPP queue, so a
// blocking write would deadlock. Driven from main.cpp loop() by Loop(), which
// is cheap when no PPP link is up. Memory for the relay is taken when the link
// comes up and given back when it goes.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_PPPTLS_H
#define ATOM_PPPTLS_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "../modem.h"
#include "../imapxcode.h"

namespace am {

class PppTls
	{
public:
	PppTls();
	void Loop(Modem& aModem, Hal& aHal);          // from loop(): cheap when no PPP link is up

private:
	static const int kPorts = 3;                  // 993 IMAPS, 465 SMTPS, 995 POP3S
	// Two concurrent sessions, not three: each live TLS session pins ~45-50 KB of
	// *internal* RAM (the mbedTLS record buffers are not in PSRAM), and three plus
	// the web-proxy's TLS and the picture decoder would exhaust the internal heap
	// - which shows up as WiFi dropping its buffers. IMAP + SMTP is the pair
	// PsiMail holds at once; POP takes a slot only if used instead of IMAP.
	static const int kSlots = 2;                  // concurrent relayed sessions
	static const size_t kBuf = 8 * 1024;          // upstream->Psion staging, per slot (PSRAM)

	// one relayed session: the PPP-side plaintext client, its upstream TLS
	// client, and the staging buffer for bytes read from upstream but not yet
	// sent to the slow Psion (its own TCP/TLS holds back while this drains)
	struct Slot
		{
		WiFiClient psion;
		WiFiClientSecure* up;
		uint8_t* buf;
		size_t len, ofs;                          // buf[ofs..len) still to go to the Psion
		bool inUse;
		// Layer 2 (IMAPS 993 only, when AT$MAILXCODE is on): an IMAP-aware filter
		// that transcodes embedded images. 0 = plain relay. uw/ch/sh are the small
		// socket<->filter holdovers (a read the filter has not consumed yet, or a
		// pull the socket has not taken yet).
		ImapXcode* xc;
		uint8_t* uw; size_t uwLen, uwOfs;         // filter -> upstream
		uint8_t* ch; size_t chLen, chOfs;         // Psion -> filter
		uint8_t* sh; size_t shLen, shOfs;         // upstream -> filter
		// diagnostics (so a hangup is never silent): what this session was, how
		// long it lived and how much it moved each way, logged when it ends
		uint16_t port;
		char host[48];
		uint32_t openMs;
		uint32_t nUp, nDown;                      // bytes Psion->upstream / upstream->Psion
		};

	void Open(Modem& aModem, Hal& aHal);
	void Close(Hal& aHal);
	void EndSlot(Slot& aS, Hal& aHal, const char* aReason);
	void Accept(Modem& aModem, Hal& aHal);
	void Service(Slot& aS, Hal& aHal);
	void ServiceXcode(Slot& aS, Hal& aHal);       // the Layer-2 pump (filter in the middle)
	bool StartXcode(Slot& aS, const Settings2& aS2, Hal& aHal);
	void EndXcode(Slot& aS);
	bool StartUpstream(Slot& aS, const char* aHost, uint16_t aPort, bool aVerify, Hal& aHal);

	static const size_t kXcHold = 2048;           // the socket<->filter holdover size

	// the filter's log lines ("relay: xcode ...") to the modem log
	struct Xl : public XcodeLog
		{
		Hal* h;
		Xl() : h(0) {}
		void XLog(const char* aLine) override { if (h) h->Log(aLine); }
		} iXl;

	WiFiServer* iServer[kPorts];                  // one per listen port; only the configured ones begin()
	uint16_t iPort[kPorts];
	Slot iSlot[kSlots];
	bool iActive;
	};

} // namespace am

#endif
