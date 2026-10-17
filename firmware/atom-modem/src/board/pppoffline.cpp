// pppoffline.cpp - see pppoffline.h. MIT licence (see LICENSE at the top of the repository).
#include "pppoffline.h"
#include "../ppp.h"
#include <Arduino.h>
#include <string.h>

#if defined(AM_PPP_DEVICE)
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/icmp.h"
#include "lwip/prot/tcp.h"
#include "lwip/priv/tcp_priv.h"      // tcp_rst

static volatile bool gOffline = false;
static volatile uint32_t gAnswered = 0;
static struct netif gRejectIf;
static bool gRejectReady = false;

static uint16_t Get16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t Get32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// (ip4_forward, the tcpip thread) a packet from the Psion for the outside,
// untranslated, with p->payload at its IP header
static err_t RejectOutput(struct netif*, struct pbuf* p, const ip4_addr_t*)
	{
	uint8_t h[60 + 20];
	u16_t n = pbuf_copy_partial(p, h, sizeof(h), 0);
	if (n < 20 || (h[0] >> 4) != 4)
		return ERR_OK;
	unsigned ihl = (h[0] & 0x0f) * 4u;
	if (ihl < 20 || ihl > n || (Get16(h + 6) & 0x1fff))
		return ERR_OK;                          // (a later fragment: the first was answered)
	uint8_t proto = h[9];
	if (proto == IP_PROTO_TCP)
		{
		if (n < ihl + 20)
			return ERR_OK;
		const uint8_t* t = h + ihl;
		uint8_t flags = t[13];
		// a new connection only: one already open is left to wait (dropped,
		// as before), so a moment without WiFi does not end it
		if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) != TCP_SYN)
			return ERR_OK;
		unsigned off = (t[12] >> 4) * 4u, tot = Get16(h + 2);
		uint32_t len = tot > ihl + off ? tot - ihl - off : 0;
		len++;                                  // (the SYN)
		if (flags & TCP_FIN)
			len++;
		// RFC 793: the reset for a SYN has sequence number 0 and
		// acknowledges all of the segment
		uint32_t seq = 0;
		uint32_t ack = Get32(t + 4) + len;
		ip_addr_t from, to;
		uint32_t a;
		memcpy(&a, h + 16, 4);
		ip_addr_set_ip4_u32(&from, a);          // (the far end it was for)
		memcpy(&a, h + 12, 4);
		ip_addr_set_ip4_u32(&to, a);            // (the Psion)
		tcp_rst(NULL, seq, ack, &from, &to, Get16(t + 2), Get16(t));
		gAnswered = gAnswered + 1;
		}
	else if (proto != IP_PROTO_ICMP)
		{
		icmp_dest_unreach(p, ICMP_DUR_NET);     // (never an ICMP error about ICMP)
		gAnswered = gAnswered + 1;
		}
	return ERR_OK;
	}

namespace am {

void PppOfflineSet(bool aOffline) { gOffline = aOffline; }

uint32_t PppOfflineTake()
	{
	uint32_t n = gAnswered;
	gAnswered = 0;
	return n;
	}

bool PppOfflineFrom(const struct ip4_addr* aSrc)
	{
	return gOffline && aSrc && ip4_addr_get_u32(aSrc) == Ip4(192, 168, 7, 2);
	}

struct netif* PppOfflineNetif()
	{
	if (!gRejectReady)
		{
		// never netif_add'ed (no address, not up, in no list): only the
		// forward that the hook steers here sees it. napt: no translation
		// on the way out; mtu 0: no fragmenting
		memset(&gRejectIf, 0, sizeof(gRejectIf));
		gRejectIf.output = RejectOutput;
		gRejectIf.napt = 1;
		gRejectIf.mtu = 0;
		gRejectIf.name[0] = 'r';
		gRejectIf.name[1] = 'j';
		gRejectReady = true;
		}
	return &gRejectIf;
	}

} // namespace am

#else

namespace am {
void PppOfflineSet(bool) {}
uint32_t PppOfflineTake() { return 0; }
bool PppOfflineFrom(const struct ip4_addr*) { return false; }
struct netif* PppOfflineNetif() { return 0; }
} // namespace am

#endif
