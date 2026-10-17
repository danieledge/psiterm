// castnet.h - PsiCast's sockets (cast.h): the viewers' WebSocket server on
// port 5962 (the handshake with its token, then frames both ways, each
// viewer with its own small send buffer and a share of each pass), and link
// A's TCP end on 192.168.7.1:5961 while a PPP call is up (the Psion's own
// "Psion Internet" route; the modem route is Modem's). Non-blocking BSD
// sockets throughout - lwIP's on the Atom, the PC's in hostmodem - so a slow
// viewer or a stalled Psion never holds up the main loop, and the modem and
// PPP paths keep their turn. Plain C++, no Arduino calls.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_CASTNET_H
#define ATOM_CASTNET_H

#include "cast.h"

namespace am {

class CastNet
	{
public:
	CastNet();
	~CastNet() { Stop(); }
	// from the main loop. aViewers: the WebSocket server should be up
	// (PsiCast on); aLinkAddr: the address link A listens on now ("" = not
	// now: no PPP call), e.g. "192.168.7.1"; aLoopback: the viewers' server on
	// 127.0.0.1 only (hostmodem)
	void Loop(Cast& aCast, Hal& aHal, bool aViewers, const char* aLinkAddr, bool aLoopback = false);
	void Stop();
	int Clients() const;
	// (hostmodem and the tests: other ports)
	uint16_t iViewPort = kCastViewPort;
	uint16_t iLinkPort = kCastPort;

	static const size_t kOutBuf = 1460;          // a viewer's bytes waiting for the socket (one segment)
	static const size_t kPassBytes = 16 * 1024;  // the most one viewer is sent in a pass
	static const uint32_t kHandshakeMs = 5000;

private:
	struct Conn
		{
		int fd;
		bool open;                 // the handshake is done (idx is its viewer)
		bool refusing;             // a refusal is being sent: then closed
		int idx;
		uint32_t startMs;
		char in[kWsHeadMax];
		size_t inLen;
		uint8_t out[kOutBuf];
		size_t outLen, outPos;
		};
	void Accept(Cast& aCast, Hal& aHal);
	void Handshake(Conn& aC, Cast& aCast, Hal& aHal);
	void Serve(Conn& aC, Cast& aCast, Hal& aHal);
	bool Flush(Conn& aC);                        // false: the socket failed
	void Drop(Conn& aC, Cast& aCast);
	void Reply(Conn& aC, const char* aText);
	void LinkLoop(Cast& aCast, Hal& aHal, const char* aAddr);
	void LinkDrop(Cast& aCast);

	int iViewSrv;
	bool iViewLoop;
	static const int kConns = kCastViewers + 2;  // (two more for handshakes and refusals)
	Conn* iConn[kConns];                         // (allocated while connected: nothing kept when idle)
	int iLinkSrv;
	char iLinkAddr[16];
	int iLinkFd;
	uint32_t iLinkId;
	uint8_t iLinkOut[1460];
	size_t iLinkOutLen, iLinkOutPos;
	uint8_t iBuf[1460];
	};

} // namespace am

#endif
