// pppat.cpp - see pppat.h. MIT licence (see LICENSE at the top of the repository).
#include "pppat.h"
#include "../ppp.h"
#include <lwip/sockets.h>
#include <errno.h>

namespace am {

void PppAt::Open(Hal& aHal)
	{
	// listen on the PPP address only, so the port is not visible on the WiFi LAN at all
	IPAddress ours;
	ours.fromString(kPppLocalIp);
	iServer = new WiFiServer(ours, kAtRemotePort);
	iServer->begin();
	iActive = true;
	char m[80];
	snprintf(m, sizeof(m), "ppp at: PsiConnect can use %s:%u", kPppLocalIp, (unsigned)kAtRemotePort);
	aHal.Log(m);
	}

void PppAt::EndClient(Hal& aHal)
	{
	if (!iHaveClient)
		return;
	iClient.stop();
	iHaveClient = false;
	iSession.Begin(0);
	aHal.Log("ppp at: PsiConnect's connection closed");
	}

void PppAt::Close(Hal& aHal)
	{
	EndClient(aHal);
	if (iServer)
		{
		iServer->end();
		delete iServer;
		iServer = 0;
		}
	iActive = false;
	aHal.Log("ppp at: closed");
	}

void PppAt::Serve(Modem& aModem, Hal& aHal)
	{
	IPAddress ours;
	ours.fromString(kPppLocalIp);
	// a new connection: the first is served, any other is closed at once
	WiFiClient c = iServer->accept();
	if (c)
		{
		if (iHaveClient || c.localIP() != ours)
			c.stop();
		else
			{
			iClient = c;
			iClient.setNoDelay(true);
			iHaveClient = true;
			iSession.Begin(aHal.Millis());
			aHal.Log("ppp at: PsiConnect connected");
			}
		}
	if (!iHaveClient)
		return;
	uint32_t now = aHal.Millis();
	// the Psion's lines in, one answer at a time (an unsent answer holds the rest back)
	for (int i = 0; i < 4 && iSession.CanTake(); i++)
		{
		int avail = iClient.available();
		if (avail <= 0)
			break;
		// (peeked and taken only as far as the session took them: no byte is lost)
		int n = ::recv(iClient.fd(), iIn, avail < (int)sizeof(iIn) ? avail : (int)sizeof(iIn), MSG_PEEK | MSG_DONTWAIT);
		if (n <= 0)
			break;
		size_t took = iSession.Input(aModem, iIn, (size_t)n, now);
		if (took)
			::recv(iClient.fd(), iIn, took, MSG_DONTWAIT);
		if (took < (size_t)n)
			break;
		}
	// the answer out (a raw non-blocking send, as pppproxy.cpp: the loop task
	// also drains the PPP queue that would let a blocking write through)
	const uint8_t* p;
	size_t left;
	while ((left = iSession.Pending(p)) > 0)
		{
		int w = ::send(iClient.fd(), p, left > 1460 ? 1460 : left, MSG_DONTWAIT);
		if (w < 0)
			{
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				{
				EndClient(aHal);
				return;
				}
			break;
			}
		iSession.Sent((size_t)w);
		if (w == 0)
			break;
		}
	if ((!iClient.connected() && !iClient.available()) || iSession.Expired(aHal.Millis()))
		EndClient(aHal);
	}

void PppAt::Loop(Modem& aModem, Hal& aHal)
	{
	bool want = aModem.PppCall() && aModem.Ppp().Up();
	if (!want)
		{
		if (iActive)
			Close(aHal);
		return;
		}
	if (!iActive)
		Open(aHal);
	if (iServer)
		Serve(aModem, aHal);
	}

} // namespace am
