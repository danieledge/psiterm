// pppdns.cpp - see pppdns.h. MIT licence (see LICENSE at the top of the repository).
#include "pppdns.h"
#include "../ppp.h"
#include <Arduino.h>
#include <string.h>

#if defined(AM_PPP_DEVICE)
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/sys.h"
#include "lwip/ip_addr.h"

static void ClientRecv(void* aArg, struct udp_pcb*, struct pbuf* p, const ip_addr_t* aAddr, u16_t aPort)
	{
	((am::PppDns*)aArg)->FromPsion(p, ip4_addr_get_u32(ip_2_ip4(aAddr)), aPort);
	}

static void UpRecv(void* aArg, struct udp_pcb*, struct pbuf* p, const ip_addr_t* aAddr, u16_t aPort)
	{
	((am::PppDns*)aArg)->FromServer(p, ip4_addr_get_u32(ip_2_ip4(aAddr)), aPort);
	}

static bool SendTo(void* aPcb, const uint8_t* aData, size_t aLen, uint32_t aIp, uint16_t aPort)
	{
	struct pbuf* q = pbuf_alloc(PBUF_TRANSPORT, (u16_t)aLen, PBUF_RAM);
	if (!q)
		return false;
	memcpy(q->payload, aData, aLen);
	ip_addr_t to;
	ip_addr_set_ip4_u32(&to, aIp);
	err_t e = udp_sendto((struct udp_pcb*)aPcb, q, &to, aPort);
	pbuf_free(q);
	return e == ERR_OK;
	}
#endif

namespace am {

PppDns gPppDns;

#if defined(AM_PPP_DEVICE)

bool PppDns::Start()
	{
	if (iRunning)
		return true;
	LOCK_TCPIP_CORE();
	struct udp_pcb* c = udp_new();
	struct udp_pcb* u = udp_new();
	bool ok = c && u;
	if (ok)
		{
		ip_addr_t local;
		ip_addr_set_ip4_u32(&local, Ip4(192, 168, 7, 1));
#if SO_REUSE
		ip_set_option(c, SOF_REUSEADDR);    // (port 53 shared with the access point's captive DNS)
#endif
		ok = udp_bind(c, &local, 53) == ERR_OK && udp_bind(u, IP_ANY_TYPE, 0) == ERR_OK;
		}
	if (ok)
		{
		udp_recv(c, ClientRecv, this);
		udp_recv(u, UpRecv, this);
		iRelay.Reset();
		iRelay.Seed(esp_random());
		iClient = c;
		iUp = u;
		iRunning = true;
		}
	else
		{
		if (c) udp_remove(c);
		if (u) udp_remove(u);
		}
	UNLOCK_TCPIP_CORE();
	return ok;
	}

void PppDns::Stop()
	{
	if (!iRunning)
		return;
	LOCK_TCPIP_CORE();
	udp_remove((struct udp_pcb*)iClient);
	udp_remove((struct udp_pcb*)iUp);
	iClient = iUp = 0;
	iRunning = false;
	UNLOCK_TCPIP_CORE();
	}

// (the tcpip thread)
void PppDns::FromPsion(void* aPbuf, uint32_t aIp, uint16_t aPort)
	{
	struct pbuf* p = (struct pbuf*)aPbuf;
	static uint8_t buf[DnsRelay::kMaxPacket];
	if (p->tot_len <= sizeof(buf) && iRunning)
		{
		size_t n = pbuf_copy_partial(p, buf, p->tot_len, 0);
		uint32_t up = iUpstream;
		if (iOffline)
			{
			// no uplink: the answer at once, from 192.168.7.1:53
			size_t a = iRelay.Refuse(buf, n);
			if (a)
				SendTo(iClient, buf, a, aIp, aPort);
			}
		else if (iRelay.FromClient(buf, n, aIp, aPort, up, sys_now()))
			SendTo(iUp, buf, n, up, 53);
		}
	pbuf_free(p);
	}

void PppDns::FromServer(void* aPbuf, uint32_t aIp, uint16_t aPort)
	{
	struct pbuf* p = (struct pbuf*)aPbuf;
	static uint8_t buf[1500];
	if (p->tot_len <= sizeof(buf) && iRunning)
		{
		size_t n = pbuf_copy_partial(p, buf, p->tot_len, 0);
		uint32_t client;
		uint16_t port;
		if (iRelay.FromServer(buf, n, aIp, aPort, client, port, sys_now()))
			SendTo(iClient, buf, n, client, port);
		}
	pbuf_free(p);
	}

#else

bool PppDns::Start() { return false; }
void PppDns::Stop() {}
void PppDns::FromPsion(void*, uint32_t, uint16_t) {}
void PppDns::FromServer(void*, uint32_t, uint16_t) {}

#endif

} // namespace am
