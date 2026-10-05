// ppp.cpp - PPPoS + NAT (see ppp.h). The lwIP half is built only on the
// device; the host build is a small stub so modem.cpp links and the dial
// dispatch can be tested. MIT licence (see LICENSE at the top of the repo).
#include "ppp.h"
#include "modem.h"            // am::Hal

namespace am {

PppLink::PppLink()
	: iActive(false), iUp(false), iDropped(false), iNat(false), iBytesIn(0)
#ifdef AM_PPP_DEVICE
	, iPcb(0), iHal(0)
#endif
	{}

PppLink::~PppLink() {}

#ifdef AM_PPP_DEVICE
// ---------------------------------------------------------------------------
// Device build: real PPPoS over the serial line, with NAPT out of the uplink.
// ---------------------------------------------------------------------------
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "lwip/netif.h"

// one PPP netif for the single link the modem ever has
static struct netif gPppIf;

// lwIP callbacks (C signatures) -> the PppLink instance through ctx
struct PppCallbacks
	{
	static u32_t Output(ppp_pcb* /*pcb*/, u8_t* data, u32_t len, void* ctx)
		{
		PppLink* self = (PppLink*)ctx;
		self->Output(data, len);
		return len;                         // the HAL buffers/paces it; report it sent
		}
	static void Status(ppp_pcb* /*pcb*/, int err, void* ctx)
		{
		((PppLink*)ctx)->OnStatus(err);
		}
	};

void PppLink::Output(const uint8_t* aData, size_t aLen)
	{
	if (iHal)
		iHal->SerialWrite(aData, aLen);     // towards the Psion (RTS/CTS or pacing in the HAL)
	}

void PppLink::OnStatus(int aErr)
	{
	// runs on the tcpip thread, so the NAPT calls are safe here
	if (aErr == PPPERR_NONE)
		{
		iUp = true;
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
		if (iHal)
			iHal->Log("ppp: up - the Psion is online (NAT on)");
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
	iHal = &aHal;
	iUp = iDropped = iNat = false;
	iBytesIn = 0;
	ppp_pcb* pcb = pppapi_pppos_create(&gPppIf, PppCallbacks::Output, PppCallbacks::Status, this);
	if (!pcb)
		{
		aHal.Log("ppp: create failed");
		iHal = 0;
		return false;
		}
	iPcb = pcb;
	// Static addressing. The prebuilt lwIP has no PPP *server* role
	// (CONFIG_LWIP_PPP_SERVER_SUPPORT off), so the modem cannot assign the
	// Psion an address: the Psion must be set to the static IP below. If
	// server support is ever built in (Arduino-as-IDF-component), the modem
	// could offer the address and no static setup would be needed.
	ip4_addr_t our, his, dns;
	ip4addr_aton(kPppLocalIp, &our);
	ip4addr_aton(kPppRemoteIp, &his);
	ip4addr_aton(kPppLocalIp, &dns);        // the modem is the Psion's DNS (it NATs it on)
	ppp_set_ipcp_ouraddr(pcb, &our);
	ppp_set_ipcp_hisaddr(pcb, &his);
	ppp_set_ipcp_dnsaddr(pcb, 0, &dns);
	ppp_set_auth(pcb, PPPAUTHTYPE_NONE, "", "");
	// the PPP link must NOT become the modem's own default route - WiFi/USB
	// stays the Atom's uplink; the Psion just routes through us and is NAT'd.
	pppapi_connect(pcb, 0);
	iActive = true;
	aHal.Log("ppp: starting (Psion static IP 192.168.7.2, gateway 192.168.7.1)");
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
	iHal = 0;
	iActive = iUp = iNat = false;
	iDropped = false;
	aHal.Log("ppp: stopped");
	(void)aHal;
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
	(void)aHal;                             // the link runs on the tcpip thread; nothing to pump here
	}

#else
// ---------------------------------------------------------------------------
// Host build: a stub that only tracks the mode, so the dial dispatch (and
// teardown) can be tested without lwIP. PPP itself is verified on hardware.
// ---------------------------------------------------------------------------
bool PppLink::Start(Hal& aHal)
	{
	(void)aHal;
	iActive = true;
	iUp = true;                             // pretend the link came up at once
	iDropped = false;
	iNat = true;
	iBytesIn = 0;
	return true;
	}

void PppLink::Stop(Hal& aHal)
	{
	(void)aHal;
	iActive = iUp = iNat = false;
	iDropped = false;
	}

size_t PppLink::Input(Hal& aHal, const uint8_t* aData, size_t aLen)
	{
	(void)aHal; (void)aData;
	if (iActive)
		iBytesIn += (uint32_t)aLen;
	return aLen;
	}

void PppLink::Poll(Hal& aHal) { (void)aHal; }

#endif

} // namespace am
