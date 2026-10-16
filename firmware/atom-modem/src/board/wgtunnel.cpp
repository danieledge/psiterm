// wgtunnel.cpp - see wgtunnel.h. MIT licence (see LICENSE at the top of the repository).
#include "wgtunnel.h"
#include <Arduino.h>
#include <string.h>
#include <time.h>

#if defined(AM_WG)
// lwIP and the library outside namespace am (their headers pull in the C library)
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcpip.h"
#include "esp_sntp.h"
#include "esp_wireguard.h"

extern "C" volatile uint32_t wireguardif_rx_bytes;
extern "C" volatile uint32_t wireguardif_tx_bytes;

// ----- the split-tunnel route hook ---------------------------------------------------
// Read on the tcpip thread (or with the core lock held); written only with
// the lock held (Install/Uninstall below)
static struct netif* volatile gRouteIf = 0;
static am::WgPrefix gRoutes[am::kWgAllowedMax];
static int gRouteCount = 0;
static uint32_t gRouteEndpoint = 0;

// the most specific directly connected route to aDest over a netif other
// than the tunnel: its prefix length (32 for a point-to-point peer, such as
// the Psion over PPP, or our own address), -1 for none
static int ConnectedLen(uint32_t aDest, struct netif* aSkip)
	{
	int best = -1;
	struct netif* n;
	NETIF_FOREACH(n)
		{
		if (n == aSkip || !netif_is_up(n) || !netif_is_link_up(n))
			continue;
		uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(n));
		if (!ip)
			continue;
		if (aDest == ip)
			return 32;
		if (!(n->flags & NETIF_FLAG_BROADCAST))
			{
			// point to point (PPP): the far end only
			if (aDest == ip4_addr_get_u32(netif_ip4_gw(n)))
				return 32;
			continue;
			}
		uint32_t mask = ip4_addr_get_u32(netif_ip4_netmask(n));
		if (mask && (aDest & mask) == (ip & mask))
			{
			int len = am::MaskLen(mask);
			if (len > best)
				best = len;
			}
		}
	return best;
	}

// is aSrc the address of a netif other than the tunnel (a socket bound there)?
static bool OtherNetifAddress(const ip4_addr_t* aSrc, struct netif* aTunnel)
	{
	if (!aSrc || ip4_addr_isany(aSrc))
		return false;
	struct netif* n;
	NETIF_FOREACH(n)
		if (n != aTunnel && netif_is_up(n) && ip4_addr_cmp(aSrc, netif_ip4_addr(n)))
			return true;
	return false;
	}

extern "C" struct netif* __real_ip4_route_src_hook(const ip4_addr_t* src, const ip4_addr_t* dest);

extern "C" struct netif* __wrap_ip4_route_src_hook(const ip4_addr_t* src, const ip4_addr_t* dest)
	{
	struct netif* wg = gRouteIf;
	if (wg && dest && netif_is_up(wg) && !OtherNetifAddress(src, wg))
		{
		uint32_t d = ip4_addr_get_u32(dest);
		if (am::WgUseTunnel(gRoutes, gRouteCount, d, ConnectedLen(d, wg), gRouteEndpoint))
			return wg;
		}
	return __real_ip4_route_src_hook(src, dest);
	}

// ----- the library's state ----------------------------------------------------------------
static wireguard_config_t gCfg = ESP_WIREGUARD_CONFIG_DEFAULT();
static wireguard_ctx_t gCtx = ESP_WIREGUARD_CONTEXT_DEFAULT();
static bool gInited = false;
static bool gSntp = false;
static char gAddr[16];
static int32_t gHandshakeAge = -1;

static const time_t kTimeValid = 1704067200;    // 2024-01-01: a clock that has been set

#endif

namespace am {

WgTunnel gWg;

WgTunnel::WgTunnel() : iHal(0), iWant(false), iRunning(false), iState(EWgStOff), iStateMs(0), iNextMs(0), iFails(0)
	{
	iWhy[0] = 0;
	memset(&iCfg, 0, sizeof(iCfg));
	}

void WgTunnel::Log(const char* aLine)
	{
	if (iHal)
		iHal->Log(aLine);
	}

bool WgTunnel::Available() const
	{
#if defined(AM_WG)
	return true;
#else
	return false;
#endif
	}

uint32_t WgTunnel::Dns() const
	{
	return iState == EWgStUp && iCfg.nDns ? iCfg.dns[0] : 0;
	}

void WgTunnel::Info(WgStatus& aOut)
	{
	memset(&aOut, 0, sizeof(aOut));
	aOut.state = iWant ? iState : EWgStOff;
	aOut.handshakeAge = -1;
#if defined(AM_WG)
	if (iWant)
		{
		aOut.handshakeAge = gHandshakeAge;
		aOut.rx = wireguardif_rx_bytes;
		aOut.tx = wireguardif_tx_bytes;
		aOut.endpointIp = ip4_addr_get_u32(ip_2_ip4(&gCfg.endpoint_ip));
		}
#endif
	snprintf(aOut.why, sizeof(aOut.why), "%s", iWant ? iWhy : "");
	}

void WgTunnel::Apply(const WgConfig& aConfig, bool aWant)
	{
#if defined(AM_WG)
	if (!aWant)
		{
		if (iWant)
			Stop();
		iWant = false;
		iState = EWgStOff;
		iWhy[0] = 0;
		WgClear(iCfg);
		return;
		}
	bool same = iWant && memcmp(&iCfg, &aConfig, sizeof(iCfg)) == 0;
	if (same)
		return;
	if (iWant)
		Stop();
	iCfg = aConfig;
	iWant = true;
	iFails = 0;
	iWhy[0] = 0;
	iState = EWgStTime;
	iStateMs = millis();
	iNextMs = iStateMs;
#else
	(void)aConfig;
	iWant = false;
	iState = EWgStOff;
	if (aWant)
		snprintf(iWhy, sizeof(iWhy), "not on this board");
#endif
	}

#if defined(AM_WG)

void WgTunnel::Stop()
	{
	LOCK_TCPIP_CORE();
	gRouteIf = 0;                            // (the hook stops at once)
	gRouteCount = 0;
	gRouteEndpoint = 0;
	if (gCtx.netif)
		esp_wireguard_disconnect(&gCtx);     // (removes the peer and the netif)
	UNLOCK_TCPIP_CORE();
	iRunning = false;
	gHandshakeAge = -1;
	}

void WgTunnel::Fail(uint32_t aNowMs, const char* aWhy)
	{
	Stop();
	iFails++;
	uint32_t backoff = 10000u << (iFails > 4 ? 4 : iFails - 1);   // 10, 20, 40, 80, 160 s
	if (backoff > 120000) backoff = 120000;
	iState = EWgStRetry;
	iStateMs = aNowMs;
	iNextMs = aNowMs + backoff;
	snprintf(iWhy, sizeof(iWhy), "%s", aWhy);
	char m[96];
	snprintf(m, sizeof(m), "WireGuard: %s; trying again in %lu s", aWhy, (unsigned long)(backoff / 1000));
	Log(m);
	}

// the config into the library's (it keeps pointers to these strings)
void WgTunnel::Start(uint32_t aNowMs)
	{
	Ip4Text(iCfg.addr, gAddr, sizeof(gAddr));
	gCfg.private_key = iCfg.privKey;
	gCfg.listen_port = iCfg.listenPort;
	gCfg.public_key = iCfg.pubKey;
	gCfg.preshared_key = iCfg.psk[0] ? iCfg.psk : 0;
	gCfg.address = gAddr;
	gCfg.netmask = "255.255.255.255";          // (the hook does the routing, not the netif's subnet)
	gCfg.endpoint = iCfg.endpoint;
	gCfg.port = iCfg.port;
	gCfg.persistent_keepalive = iCfg.keepalive;
	ip_addr_set_zero(&gCfg.endpoint_ip);
	LOCK_TCPIP_CORE();
	bool ok = true;
	if (!gInited)
		{
		// once: the random generator and the context (the config struct is ours, reused)
		ok = esp_wireguard_init(&gCfg, &gCtx) == ESP_OK;
		gInited = ok;
		}
	UNLOCK_TCPIP_CORE();
	wireguardif_rx_bytes = 0;
	wireguardif_tx_bytes = 0;
	iState = EWgStResolve;
	iStateMs = aNowMs;
	iNextMs = aNowMs;
	if (!ok)
		Fail(aNowMs, "could not start");
	}

void WgTunnel::Loop(uint32_t aNowMs)
	{
	if (!iWant || (int32_t)(aNowMs - iNextMs) < 0)
		return;
	switch (iState)
		{
	case EWgStTime:
		{
		if (!gSntp)
			{
			// the handshake carries a timestamp the server checks: the clock
			// must be right. Polled SNTP, and it keeps the time from then on
			esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
			esp_sntp_setservername(0, "pool.ntp.org");
			esp_sntp_setservername(1, "time.cloudflare.com");
			esp_sntp_setservername(2, "time.google.com");
			esp_sntp_init();
			gSntp = true;
			Log("WireGuard: setting the clock (NTP)");
			}
		if (time(0) >= kTimeValid)
			{
			iWhy[0] = 0;
			Start(aNowMs);
			}
		else
			{
			if (aNowMs - iStateMs > 30000)
				snprintf(iWhy, sizeof(iWhy), "no time from NTP yet");
			iNextMs = aNowMs + 500;
			}
		break;
		}
	case EWgStResolve:
		{
		LOCK_TCPIP_CORE();
		esp_err_t r = esp_wireguard_connect(&gCtx);
		bool ok = r == ESP_OK;
		if (ok)
			{
			// the peer is in: its AllowedIPs, and the route table for the hook
			char ip[16], mask[16];
			for (int i = 0; i < iCfg.nAllowed && i < kWgAllowedMax; i++)
				{
				Ip4Text(iCfg.allowed[i].ip, ip, sizeof(ip));
				Ip4Text(PrefixMask(iCfg.allowed[i].len), mask, sizeof(mask));
				esp_wireguard_add_allowed_ip(&gCtx, ip, mask);
				}
			memcpy(gRoutes, iCfg.allowed, sizeof(gRoutes));
			gRouteCount = iCfg.nAllowed;
			gRouteEndpoint = ip4_addr_get_u32(ip_2_ip4(&gCfg.endpoint_ip));
			gRouteIf = gCtx.netif;
			}
		UNLOCK_TCPIP_CORE();
		if (ok)
			{
			iRunning = true;
			iState = EWgStConnecting;
			iStateMs = aNowMs;
			iNextMs = aNowMs + 1000;
			char e[16], m[96];
			Ip4Text(gRouteEndpoint, e, sizeof(e));
			snprintf(m, sizeof(m), "WireGuard: handshake with %s:%u (%s)", iCfg.endpoint, (unsigned)iCfg.port, e);
			Log(m);
			}
		else if (r == ESP_ERR_RETRY && aNowMs - iStateMs < 20000)
			iNextMs = aNowMs + 500;          // the lookup is still going
		else
			Fail(aNowMs, r == ESP_ERR_RETRY ? "endpoint not found" : "could not start");
		break;
		}
	case EWgStConnecting:
	case EWgStUp:
		{
		LOCK_TCPIP_CORE();
		bool up = esp_wireguard_peer_is_up(&gCtx) == ESP_OK;
		time_t hs = 0;
		bool have = esp_wireguard_latest_handshake(&gCtx, &hs) == ESP_OK;
		UNLOCK_TCPIP_CORE();
		time_t now = time(0);
		gHandshakeAge = have && hs > 0 && now >= hs ? (int32_t)(now - hs) : -1;
		if (up)
			{
			if (iState != EWgStUp)
				iFails = 0;
			iState = EWgStUp;
			iWhy[0] = 0;
			}
		else
			{
			if (iState == EWgStUp)
				{
				iStateMs = aNowMs;
				snprintf(iWhy, sizeof(iWhy), "handshake lost");
				}
			iState = EWgStConnecting;
			// The library resends the handshake by itself, but not for ever:
			// like any WireGuard peer it gives up after about 90 s with no
			// answer and waits for traffic, and a first try made as a phone's
			// hotspot comes up can be lost that way (seen on the AtomS3R:
			// "connecting" for good, until the VPN was turned off and on). So
			// after 30 s with no handshake, start again from the top, with
			// the back-off - what turning it off and on did by hand
			if (aNowMs - iStateMs > 30000)
				{
				iNextMs = aNowMs;
				Fail(aNowMs, "no answer from the server");
				break;
				}
			}
		iNextMs = aNowMs + 1000;
		break;
		}
	case EWgStRetry:
		iState = EWgStTime;                  // again from the top
		iStateMs = aNowMs;
		break;
	default:
		break;
		}
	}

#else

void WgTunnel::Loop(uint32_t) {}
void WgTunnel::Stop() {}
void WgTunnel::Fail(uint32_t, const char*) {}
void WgTunnel::Start(uint32_t) {}

#endif

} // namespace am
