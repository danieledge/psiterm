// ppp.cpp - PPPoS + NAT (see ppp.h). The lwIP half is built only on the
// device; the host build is a small stub (with a real output queue) so
// modem.cpp links and the dial dispatch and the output path can be tested.
// MIT licence (see LICENSE at the top of the repository).
#include "ppp.h"
#include "modem.h"            // am::Hal, am::Pacer
#include <string.h>
#include <stdio.h>

#ifdef AM_PPP_DEVICE
// lwIP's and FreeRTOS's headers are included outside namespace am (they pull
// in <stdlib.h> and friends, which must not land inside am::std)
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "lwip/netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

// one PPP netif for the single link the modem ever has
static struct netif gPppIf;
#endif

namespace am {

PppLink::PppLink()
	: iActive(false), iUp(false), iDropped(false), iNat(false), iBytesIn(0), iDns(0), iDropFrames(0), iDropping(false), iStageLen(0),
	  iUpLogged(false), iPendOfs(0), iPendLen(0)
#ifdef AM_PPP_DEVICE
	, iPcb(0), iQueue(0)
#endif
	{}

PppLink::~PppLink() {}

// the DNS to give the Psion: the uplink's, else 1.1.1.1
static uint32_t ChooseDns(Hal& aHal)
	{
	uint32_t d = aHal.UplinkDns();
	return d ? d : kPppFallbackDns;
	}

static void DotQuad(uint32_t aIp, char* aOut, size_t aMax)
	{
	snprintf(aOut, aMax, "%u.%u.%u.%u", (unsigned)(aIp & 0xff), (unsigned)((aIp >> 8) & 0xff),
		(unsigned)((aIp >> 16) & 0xff), (unsigned)((aIp >> 24) & 0xff));
	}

static void LogStart(Hal& aHal, uint32_t aDns)
	{
	char d[20], m[120];
	DotQuad(aDns, d, sizeof(d));
	snprintf(m, sizeof(m), "ppp: starting - set the Psion to IP %s, gateway %s, DNS %s", kPppRemoteIp, kPppLocalIp, d);
	aHal.Log(m);
	}

// ---------------------------------------------------------------------------
// Drain: shared by the device and the host. Takes bytes from the queue into
// iPend and writes them as PumpPsion would: only what the UART has room for,
// only what the pacer allows, and what was taken is never lost
// ---------------------------------------------------------------------------
size_t PppLink::Drain(Hal& aHal, Pacer& aPacer)
	{
	size_t sent = 0;
	for (int i = 0; i < 4; i++)
		{
		size_t room = aHal.SerialWritable();
		if (!room)
			break;
		size_t have = (iPendLen - iPendOfs) + QueuedBytes();
		if (!have)
			break;
		size_t want = have < room ? have : room;
		if (want > sizeof(iPend))
			want = sizeof(iPend);
		size_t n = aPacer.Allow(aHal.Micros(), want);
		if (!n)
			break;
		if (n > want)
			n = want;
		size_t buffered = iPendLen - iPendOfs;
		if (buffered < n)
			{
			if (iPendOfs)
				{
				memmove(iPend, iPend + iPendOfs, buffered);
				iPendLen = buffered;
				iPendOfs = 0;
				}
			iPendLen += QueueRead(iPend + iPendLen, n - buffered);
			}
		size_t avail = iPendLen - iPendOfs;
		if (n > avail)
			n = avail;
		if (!n)
			break;
		size_t w = aHal.SerialWrite(iPend + iPendOfs, n);
		aPacer.Spent(aHal.Micros(), w);
		iPendOfs += w;
		sent += w;
		if (iPendOfs == iPendLen)
			iPendOfs = iPendLen = 0;
		if (!w)
			break;
		}
	return sent;
	}

#ifdef AM_PPP_DEVICE
// ---------------------------------------------------------------------------
// Device build: real PPPoS over the serial line, with NAPT out of the uplink.
// ---------------------------------------------------------------------------

// lwIP callbacks (C signatures) -> the PppLink instance through ctx
struct PppCallbacks
	{
	// (pppos_output_cb_fn: u32_t (*)(ppp_pcb*, u8_t*, u32_t, void*); the tcpip thread)
	static u32_t Output(ppp_pcb* /*pcb*/, u8_t* data, u32_t len, void* ctx)
		{
		((PppLink*)ctx)->QueueOut(data, len);   // never the HAL: only the queue
		return len;                             // (a dropped frame is PPP's to recover: the peer retries)
		}
	static void Status(ppp_pcb* /*pcb*/, int err, void* ctx)
		{
		((PppLink*)ctx)->OnStatus(err);
		}
	};

bool PppLink::Enqueue(const uint8_t* aData, size_t aLen)
	{
	StreamBufferHandle_t q = (StreamBufferHandle_t)iQueue;
	if (!q)
		return false;
	// one writer (the tcpip thread): the room cannot shrink between the check and the send
	return xStreamBufferSpacesAvailable(q) >= aLen
		&& xStreamBufferSend(q, aData, aLen, 0) == aLen;
	}

size_t PppLink::QueuedBytes() const
	{
	StreamBufferHandle_t q = (StreamBufferHandle_t)iQueue;
	return q ? xStreamBufferBytesAvailable(q) : 0;
	}

size_t PppLink::QueueRead(uint8_t* aOut, size_t aMax)
	{
	StreamBufferHandle_t q = (StreamBufferHandle_t)iQueue;
	return q ? xStreamBufferReceive(q, aOut, aMax, 0) : 0;
	}

void PppLink::QueueReset()
	{
	iDropping = false;
	iStageLen = 0;
	iPendOfs = iPendLen = 0;
	if (iQueue)
		xStreamBufferReset((StreamBufferHandle_t)iQueue);
	}

void PppLink::OnStatus(int aErr)
	{
	// runs on the tcpip thread, so the NAPT calls are safe here; no logging
	// from here (Poll does that): the log ring is the loop task's
	if (aErr == PPPERR_NONE)
		{
		ppp_pcb* pcb = (ppp_pcb*)iPcb;
		struct netif* nif = pcb ? ppp_netif(pcb) : 0;
		if (nif)
			{
			// NAT the Psion out of the modem's default route (WiFi, or USB
			// when that is the uplink). Needs CONFIG_LWIP_IP_FORWARD and
			// CONFIG_LWIP_IPV4_NAPT (both on in the S3 prebuilt lwIP).
			ip_napt_enable_no(nif->num, 1);
			iNat = true;
			}
		iUp = true;
		}
	else
		{
		iUp = false;
		iDropped = true;                    // the Modem ends the call on the next Poll
		}
	}

bool PppLink::Start(Hal& aHal)
	{
	if (iActive)
		return true;
	if (!iQueue)
		iQueue = xStreamBufferCreate(kPppOutQueue, 1);   // kept: Output may run until the pcb is freed
	if (!iQueue)
		{
		aHal.Log("ppp: no memory for the output queue");
		return false;
		}
	QueueReset();
	iUp = iDropped = iNat = false;
	iUpLogged = false;
	iBytesIn = 0;
	iDropFrames = 0;
	ppp_pcb* pcb = pppapi_pppos_create(&gPppIf, PppCallbacks::Output, PppCallbacks::Status, this);
	if (!pcb)
		{
		aHal.Log("ppp: create failed");
		return false;
		}
	iPcb = pcb;
	// Static addressing. The prebuilt lwIP has no PPP *server* role
	// (CONFIG_LWIP_PPP_SERVER_SUPPORT off), so the modem cannot assign the
	// Psion an address: the Psion must be set to the static IP below. If
	// server support is ever built in (Arduino-as-IDF-component), the modem
	// could offer the address and no static setup would be needed.
	iDns = ChooseDns(aHal);
	ip4_addr_t our, his, dns;
	ip4addr_aton(kPppLocalIp, &our);
	ip4addr_aton(kPppRemoteIp, &his);
	ip4_addr_set_u32(&dns, iDns);           // the uplink's DNS (NAT'd on like the rest)
	ppp_set_ipcp_ouraddr(pcb, &our);
	ppp_set_ipcp_hisaddr(pcb, &his);
	ppp_set_ipcp_dnsaddr(pcb, 0, &dns);
	ppp_set_auth(pcb, PPPAUTHTYPE_NONE, "", "");
	// the PPP link must NOT become the modem's own default route - WiFi/USB
	// stays the Atom's uplink; the Psion just routes through us and is NAT'd.
	pppapi_connect(pcb, 0);
	iActive = true;
	LogStart(aHal, iDns);
	return true;
	}

void PppLink::Stop(Hal& aHal)
	{
	if (!iActive)
		return;
	ppp_pcb* pcb = (ppp_pcb*)iPcb;
	if (pcb)
		{
		if (iNat)
			{
			struct netif* nif = ppp_netif(pcb);
			if (nif)
				ip_napt_enable_no(nif->num, 0);
			}
		pppapi_close(pcb, 1);               // 1: drop carrier now
		pppapi_free(pcb);
		}
	iPcb = 0;
	iActive = iUp = iNat = false;
	iDropped = false;
	QueueReset();
	char m[64];
	snprintf(m, sizeof(m), "ppp: stopped (%u frames dropped)", (unsigned)iDropFrames);
	aHal.Log(m);
	}

size_t PppLink::Input(Hal& aHal, const uint8_t* aData, size_t aLen)
	{
	(void)aHal;
	if (iActive && iPcb && aLen)
		{
		pppos_input_tcpip((ppp_pcb*)iPcb, (u8_t*)aData, (int)aLen);
		iBytesIn += (uint32_t)aLen;
		}
	return aLen;                            // lwIP copies the bytes
	}

void PppLink::Poll(Hal& aHal)
	{
	// the link runs on the tcpip thread; here only the log line, from the loop task
	if (iActive && iUp && !iUpLogged)
		{
		iUpLogged = true;
		aHal.Log("ppp: up - the Psion is online (NAT on)");
		}
	}

#else
// ---------------------------------------------------------------------------
// Host build: a stub that tracks the mode and keeps a real output queue, so
// the dial dispatch, teardown and the output path can be tested without
// lwIP. PPP itself is verified on hardware.
// ---------------------------------------------------------------------------
bool PppLink::Start(Hal& aHal)
	{
#ifdef AM_PPP_NONE
	aHal.Log("ppp: not available on this board (its lwIP has no PPP or NAT)");
	return false;
#endif
	iActive = true;
	iUp = true;                             // pretend the link came up at once
	iDropped = false;
	iNat = true;
	iBytesIn = 0;
	iDropFrames = 0;
	iDropping = false;
	iStageLen = 0;
	iHostQ.clear();
	iPendOfs = iPendLen = 0;
	iDns = ChooseDns(aHal);
	LogStart(aHal, iDns);
	return true;
	}

void PppLink::Stop(Hal& aHal)
	{
	(void)aHal;
	iActive = iUp = iNat = false;
	iDropped = false;
	iHostQ.clear();
	iPendOfs = iPendLen = 0;
	}

size_t PppLink::Input(Hal& aHal, const uint8_t* aData, size_t aLen)
	{
	(void)aHal; (void)aData;
	if (iActive)
		iBytesIn += (uint32_t)aLen;
	return aLen;
	}

void PppLink::Poll(Hal& aHal) { (void)aHal; }

bool PppLink::Enqueue(const uint8_t* aData, size_t aLen)
	{
	if (iHostQ.size() + aLen > kPppOutQueue)
		return false;
	iHostQ.insert(iHostQ.end(), aData, aData + aLen);
	return true;
	}

size_t PppLink::QueuedBytes() const { return iHostQ.size(); }

size_t PppLink::QueueRead(uint8_t* aOut, size_t aMax)
	{
	size_t n = 0;
	while (n < aMax && !iHostQ.empty())
		{
		aOut[n++] = iHostQ.front();
		iHostQ.pop_front();
		}
	return n;
	}

void PppLink::QueueReset() { iDropping = false; iStageLen = 0; iHostQ.clear(); iPendOfs = iPendLen = 0; }

#endif

// lwIP hands a frame over in chunks (PBUF_POOL_BUFSIZE, ~1.5 KB) and the last
// one ends in the 0x7e flag. A chunk queued on its own could leave half a
// frame on the line (a bad FCS, and the line time wasted), so the chunks are
// gathered and the frame is queued whole at its flag, or dropped whole.
void PppLink::QueueOut(const uint8_t* aData, size_t aLen)
	{
	if (!aLen)
		return;
	bool last = aData[aLen - 1] == 0x7e;
	if (iDropping)                               // the rest of a dropped frame
		{
		if (last)
			iDropping = false;
		return;
		}
	if (iStageLen + aLen > kPppStage)            // too big to gather: drop it
		{
		iStageLen = 0;
		iDropFrames = iDropFrames + 1;
		iDropping = !last;
		return;
		}
	memcpy(iStage + iStageLen, aData, aLen);
	iStageLen += aLen;
	if (!last)
		return;
	size_t n = iStageLen;
	iStageLen = 0;
	if (!Enqueue(iStage, n))
		iDropFrames = iDropFrames + 1;           // (counted once for the frame)
	}

} // namespace am
