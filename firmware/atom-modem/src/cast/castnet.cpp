// castnet.cpp - see castnet.h. MIT licence (see LICENSE at the top of the repository).
#include "castnet.h"
#include "../modem.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <new>
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <unistd.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace am {

static bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }

static int Listen(uint32_t aAddr, uint16_t aPort)
	{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(aPort);
	a.sin_addr.s_addr = aAddr;
	if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 2) != 0)
		{
		close(s);
		return -1;
		}
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	return s;
	}

static void Prepare(int aFd)
	{
	fcntl(aFd, F_SETFL, fcntl(aFd, F_GETFL, 0) | O_NONBLOCK);
	int one = 1;
	setsockopt(aFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#if !defined(ARDUINO) && !defined(ESP_PLATFORM)
	// (hostmodem: a send buffer the size of lwIP's, so a slow viewer is slow here too)
	int snd = 5760;
	setsockopt(aFd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
#endif
	}

CastNet::CastNet()
	: iViewSrv(-1), iViewLoop(false), iLinkSrv(-1), iLinkFd(-1), iLinkId(0), iLinkOutLen(0), iLinkOutPos(0)
	{
	iLinkAddr[0] = 0;
	for (int i = 0; i < kConns; i++)
		iConn[i] = 0;
	}

int CastNet::Clients() const
	{
	int n = 0;
	for (int i = 0; i < kConns; i++)
		if (iConn[i] && iConn[i]->open)
			n++;
	return n;
	}

void CastNet::Stop()
	{
	for (int i = 0; i < kConns; i++)
		if (iConn[i])
			{
			close(iConn[i]->fd);
			delete iConn[i];
			iConn[i] = 0;
			}
	if (iViewSrv >= 0)
		close(iViewSrv);
	iViewSrv = -1;
	if (iLinkFd >= 0)
		close(iLinkFd);
	iLinkFd = -1;
	if (iLinkSrv >= 0)
		close(iLinkSrv);
	iLinkSrv = -1;
	iLinkAddr[0] = 0;
	}

void CastNet::Loop(Cast& aCast, Hal& aHal, bool aViewers, const char* aLinkAddr, bool aLoopback)
	{
	// ----- link B: the viewers
	if (aViewers && iViewSrv < 0)
		{
		iViewSrv = Listen(aLoopback ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY), iViewPort);
		iViewLoop = aLoopback;
		char m[80];
		snprintf(m, sizeof(m), iViewSrv >= 0 ? "PsiCast: viewers on port %u" : "PsiCast: port %u is not available",
			(unsigned)iViewPort);
		aHal.Log(m);
		}
	else if (!aViewers && iViewSrv >= 0)
		{
		for (int i = 0; i < kConns; i++)
			if (iConn[i])
				Drop(*iConn[i], aCast);
		close(iViewSrv);
		iViewSrv = -1;
		}
	if (iViewSrv >= 0)
		Accept(aCast, aHal);
	for (int i = 0; i < kConns; i++)
		{
		if (!iConn[i])
			continue;
		Conn& c = *iConn[i];
		if (!c.open)
			Handshake(c, aCast, aHal);
		else
			Serve(c, aCast, aHal);
		if (c.fd < 0)
			{
			delete iConn[i];                      // (dropped: its memory back)
			iConn[i] = 0;
			}
		}
	// ----- link A over TCP (the PPP route)
	LinkLoop(aCast, aHal, aLinkAddr ? aLinkAddr : "");
	}

void CastNet::Accept(Cast& aCast, Hal& aHal)
	{
	for (int k = 0; k < 2; k++)
		{
		int fd = accept(iViewSrv, 0, 0);
		if (fd < 0)
			return;
		int slot = -1;
		for (int i = 0; i < kConns && slot < 0; i++)
			if (!iConn[i])
				slot = i;
		Conn* c = slot >= 0 ? new (std::nothrow) Conn : 0;
		if (!c)
			{
			close(fd);                            // (all in use, or no memory)
			continue;
			}
		iConn[slot] = c;
		Prepare(fd);
		c->fd = fd;
		c->open = false;
		c->refusing = false;
		c->idx = -1;
		c->startMs = aHal.Millis();
		c->inLen = 0;
		c->outLen = c->outPos = 0;
		}
	(void)aCast;
	}

void CastNet::Reply(Conn& aC, const char* aText)
	{
	size_t n = strlen(aText);
	if (n > sizeof(aC.out))
		n = sizeof(aC.out);
	memcpy(aC.out, aText, n);
	aC.outLen = n;
	aC.outPos = 0;
	}

void CastNet::Handshake(Conn& aC, Cast& aCast, Hal& aHal)
	{
	if (aC.refusing)
		{
		if (!Flush(aC) || aC.outPos >= aC.outLen || aHal.Millis() - aC.startMs > kHandshakeMs)
			Drop(aC, aCast);
		return;
		}
	if (aHal.Millis() - aC.startMs > kHandshakeMs)
		{
		Drop(aC, aCast);
		return;
		}
	int n = recv(aC.fd, aC.in + aC.inLen, sizeof(aC.in) - aC.inLen, MSG_DONTWAIT);
	if (n == 0 || (n < 0 && !WouldBlock()))
		{
		Drop(aC, aCast);
		return;
		}
	if (n < 0)
		return;
	aC.inLen += (size_t)n;
	WsRequest rq;
	size_t used = 0;
	int r = WsParseRequest(aC.in, aC.inLen, rq, used);
	if (r == 0)
		return;
	char token[40] = "";
	char why[64] = "";
	bool ours = r > 0 && strncmp(rq.path, "/cast", 5) == 0 && (rq.path[5] == 0 || rq.path[5] == '?');
	if (r < 0 || !rq.get || !ours || !rq.upgrade || !rq.version13 || !rq.key[0])
		{
		Reply(aC, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nThis is PsiCast's WebSocket: open /screen on the web pages\n");
		aC.refusing = true;
		return;
		}
	WsQuery(rq.path, "t", token, sizeof(token));
	int idx = aCast.ViewerOpen(token, why, sizeof(why));
	if (idx < 0)
		{
		char m[200];
		snprintf(m, sizeof(m), "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s\n", why);
		Reply(aC, m);
		aC.refusing = true;
		char l[128];
		snprintf(l, sizeof(l), "PsiCast: a viewer was turned away: %s", why);
		aHal.Log(l);
		return;
		}
	char accept[29];
	WsAcceptKey(rq.key, accept);
	char m[200];
	snprintf(m, sizeof(m), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	Reply(aC, m);
	aC.open = true;
	aC.idx = idx;
	// anything the browser sent after the request is already frames
	if (aC.inLen > used)
		aCast.ViewerIn(idx, (const uint8_t*)aC.in + used, aC.inLen - used);
	aC.inLen = 0;
	}

bool CastNet::Flush(Conn& aC)
	{
	while (aC.outPos < aC.outLen)
		{
		int w = send(aC.fd, aC.out + aC.outPos, aC.outLen - aC.outPos, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (w < 0)
			return WouldBlock();
		if (w == 0)
			break;
		aC.outPos += (size_t)w;
		}
	if (aC.outPos >= aC.outLen)
		aC.outPos = aC.outLen = 0;
	return true;
	}

void CastNet::Serve(Conn& aC, Cast& aCast, Hal& aHal)
	{
	// in: frames from the browser (small)
	for (int k = 0; k < 4; k++)
		{
		int n = recv(aC.fd, iBuf, sizeof(iBuf), MSG_DONTWAIT);
		if (n == 0 || (n < 0 && !WouldBlock()))
			{
			Drop(aC, aCast);
			return;
			}
		if (n < 0)
			break;
		aCast.ViewerIn(aC.idx, iBuf, (size_t)n);
		}
	// out: as much as the socket takes, up to this viewer's share of the pass
	size_t sent = 0;
	while (sent < kPassBytes)
		{
		if (!Flush(aC))
			{
			Drop(aC, aCast);
			return;
			}
		if (aC.outLen)
			break;                                // (the socket is full: this viewer waits)
		size_t n = aCast.ViewerOut(aC.idx, aC.out, sizeof(aC.out));
		if (!n)
			break;
		aC.outLen = n;
		aC.outPos = 0;
		sent += n;
		}
	if (!aC.outLen && aCast.ViewerDone(aC.idx))
		Drop(aC, aCast);
	(void)aHal;
	}

void CastNet::Drop(Conn& aC, Cast& aCast)
	{
	if (aC.fd >= 0)
		close(aC.fd);
	aC.fd = -1;
	if (aC.open && aC.idx >= 0)
		aCast.ViewerClose(aC.idx);
	aC.open = false;
	aC.idx = -1;
	aC.outLen = aC.outPos = 0;
	}

// ----- link A over TCP ---------------------------------------------------------------------

void CastNet::LinkDrop(Cast& aCast)
	{
	if (iLinkFd >= 0)
		close(iLinkFd);
	iLinkFd = -1;
	aCast.LinkClose(iLinkId);
	iLinkId = 0;
	iLinkOutLen = iLinkOutPos = 0;
	}

void CastNet::LinkLoop(Cast& aCast, Hal& aHal, const char* aAddr)
	{
	bool want = aAddr[0] && aCast.Enabled();
	if (iLinkSrv >= 0 && (!want || strcmp(aAddr, iLinkAddr) != 0))
		{
		if (iLinkFd >= 0)
			LinkDrop(aCast);
		close(iLinkSrv);
		iLinkSrv = -1;
		iLinkAddr[0] = 0;
		}
	if (!want)
		return;
	if (iLinkSrv < 0)
		{
		// on that address only: the port is not visible on the WiFi LAN
		iLinkSrv = Listen(inet_addr(aAddr), iLinkPort);
		if (iLinkSrv < 0)
			return;
		snprintf(iLinkAddr, sizeof(iLinkAddr), "%s", aAddr);
		char m[80];
		snprintf(m, sizeof(m), "PsiCast: the Psion can connect to %s:%u", aAddr, (unsigned)iLinkPort);
		aHal.Log(m);
		}
	int fd = accept(iLinkSrv, 0, 0);
	if (fd >= 0)
		{
		// a new connection: the Psion has come back (the old one is dead)
		if (iLinkFd >= 0)
			LinkDrop(aCast);
		uint32_t id = aCast.LinkOpen(Cast::ERouteTcp);
		if (!id)
			close(fd);
		else
			{
			Prepare(fd);
			iLinkFd = fd;
			iLinkId = id;
			iLinkOutLen = iLinkOutPos = 0;
			}
		}
	if (iLinkFd < 0)
		return;
	// in: everything the link takes (peeked first, so nothing it cannot take is lost)
	for (int k = 0; k < 8; k++)
		{
		int n = recv(iLinkFd, iBuf, sizeof(iBuf), MSG_PEEK | MSG_DONTWAIT);
		if (n == 0 || (n < 0 && !WouldBlock()))
			{
			LinkDrop(aCast);
			return;
			}
		if (n < 0)
			break;
		size_t took = aCast.LinkIn(iLinkId, iBuf, (size_t)n);
		if (took)
			recv(iLinkFd, iBuf, took, MSG_DONTWAIT);
		if (took < (size_t)n)
			break;
		}
	// out
	for (;;)
		{
		if (iLinkOutPos >= iLinkOutLen)
			{
			iLinkOutLen = aCast.LinkOut(iLinkId, iLinkOut, sizeof(iLinkOut));
			iLinkOutPos = 0;
			if (!iLinkOutLen)
				break;
			}
		int w = send(iLinkFd, iLinkOut + iLinkOutPos, iLinkOutLen - iLinkOutPos, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (w < 0)
			{
			if (!WouldBlock())
				{
				LinkDrop(aCast);
				return;
				}
			break;
			}
		iLinkOutPos += (size_t)w;
		if (iLinkOutPos < iLinkOutLen)
			break;
		}
	if (!aCast.LinkAlive(iLinkId) && iLinkOutPos >= iLinkOutLen)
		LinkDrop(aCast);
	}

} // namespace am
