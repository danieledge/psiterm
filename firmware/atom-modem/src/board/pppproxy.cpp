// pppproxy.cpp - see pppproxy.h. MIT licence (see LICENSE at the top of the repository).
#include "pppproxy.h"
#include "../ppp.h"
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <errno.h>

namespace am {

static const size_t kWritePerPass = 4096;         // most bytes sent to the client per Serve() pass: one client must not hog the loop
static const size_t kRingBytes = 32 * 1024;       // (Proxy::kSlack, 12 KB, must fit with room to spare)

PppProxy::PppProxy()
	: iServer(0), iProxy(0), iRingBuf(0), iActive(false), iHaveClient(false), iEnding(false), iInLen(0)
	{}

void PppProxy::Open(Modem& aModem, Hal& aHal)
	{
	(void)aModem;
	uint8_t* buf = 0;
#if defined(BOARD_HAS_PSRAM)
	if (psramFound())
		buf = (uint8_t*)heap_caps_malloc(kRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
	if (!buf)
		buf = (uint8_t*)malloc(kRingBytes);
	Proxy* px = buf ? new (std::nothrow) Proxy(aHal) : 0;
	if (!buf || !px)
		{
		free(buf);
		aHal.Log("ppp proxy: no memory; not offered");
		iActive = true;                           // (do not try again until the link goes)
		return;
		}
	iRingBuf = buf;
	iRing.Init(buf, kRingBytes);
	iProxy = px;
	// listen on the PPP address only, so the port is not visible on the WiFi LAN at all
	// (Serve() still checks the client's localIP(), as defence in depth)
	IPAddress ours;
	ours.fromString(kPppLocalIp);
	iServer = new WiFiServer(ours, kPort);
	iServer->begin();
	iActive = true;
	char m[80];
	snprintf(m, sizeof(m), "ppp proxy: PsiWeb can use %s:%u", kPppLocalIp, (unsigned)kPort);
	aHal.Log(m);
	}

void PppProxy::EndClient()
	{
	if (iProxy)
		iProxy->Stop();
	iClient.stop();
	iHaveClient = false;
	iEnding = false;
	iInLen = 0;
	iRing.Clear();
	}

void PppProxy::Close(Hal& aHal)
	{
	EndClient();
	if (iServer)
		{
		iServer->end();
		delete iServer;
		iServer = 0;
		}
	delete iProxy;
	iProxy = 0;
	free(iRingBuf);
	iRingBuf = 0;
	iActive = false;
	aHal.Log("ppp proxy: closed");
	}

void PppProxy::Serve(Modem& aModem)
	{
	if (!iHaveClient)
		{
		WiFiClient c = iServer->accept();
		if (!c)
			return;
		// only from the PPP side: the Psion talks to 192.168.7.1
		IPAddress local = c.localIP();
		IPAddress ours;
		ours.fromString(kPppLocalIp);
		if (local != ours)
			{
			c.stop();
			return;
			}
		const Settings& s = aModem.Config();
		const Settings2& s2 = aModem.Config2();
		iProxy->Start(ProxyMode(s), !s.proxyNoZip, s2.img ? s2.imgWidth : 0, s2.imgMaxKB);
		iRing.Clear();
		iClient = c;
		iClient.setNoDelay(true);
		iHaveClient = true;
		iEnding = false;
		iInLen = 0;
		return;
		}
	// the Psion's request bytes into the proxy
	if (!iEnding)
		{
		for (int i = 0; i < 4; i++)
			{
			if (!iInLen)
				{
				int avail = iClient.available();
				if (avail <= 0)
					break;
				int n = iClient.read(iIn, avail < (int)sizeof(iIn) ? avail : (int)sizeof(iIn));
				if (n <= 0)
					break;
				iInLen = (size_t)n;
				}
			size_t took = iProxy->FromPsion(iIn, iInLen);
			if (took < iInLen)
				{
				memmove(iIn, iIn + took, iInLen - took);
				iInLen -= took;
				break;                            // (the proxy is full: the rest waits)
				}
			iInLen = 0;
			}
		if (!iProxy->Pump(iRing))
			iEnding = true;                       // (the Psion asked for the close, and has its answer)
		}
	// the proxy's pages out
	// (a raw non-blocking send: NetworkClient::write() would wait up to ~10 s on a full TCP
	// buffer, and the loop task is also what drains the PPP queue that would unblock it)
	size_t budget = kWritePerPass;
	while (iRing.Count() && budget)
		{
		const uint8_t* p;
		size_t run = iRing.Peek(p);
		if (run > 1460) run = 1460;
		if (run > budget) run = budget;
		int w = ::send(iClient.fd(), p, run, MSG_DONTWAIT);
		if (w < 0)
			{
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				{
				EndClient();                      // a real error: the Psion went
				return;
				}
			break;                                // full: try again next loop
			}
		iRing.Drop((size_t)w);
		budget -= (size_t)w;
		if ((size_t)w < run)
			break;
		}
	if (!iClient.connected() && !iClient.available())
		EndClient();                              // the Psion went
	else if (iEnding && !iRing.Count())
		EndClient();
	}

void PppProxy::Loop(Modem& aModem, Hal& aHal)
	{
	bool want = aModem.PppCall() && aModem.Ppp().Up() && ProxyMode(aModem.Config()) != Proxy::EOff;
	if (!want)
		{
		if (iActive)
			Close(aHal);
		return;
		}
	if (!iActive)
		Open(aModem, aHal);
	if (iServer && iProxy)
		Serve(aModem);
	}

} // namespace am
