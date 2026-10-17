// dnsrelay.cpp - see dnsrelay.h. MIT licence (see LICENSE at the top of the repository).
#include "dnsrelay.h"
#include <string.h>

namespace am {

uint32_t DnsUpstream(bool aTunnelUp, uint32_t aTunnelDns, uint32_t aUplinkDns)
	{
	if (aTunnelUp && aTunnelDns)
		return aTunnelDns;
	if (aUplinkDns)
		return aUplinkDns;
	return kDnsFallback;
	}

DnsRelay::DnsRelay() : iRand(0x2545f491u)
	{
	Reset();
	}

void DnsRelay::Reset()
	{
	memset(iSlot, 0, sizeof(iSlot));
	iForwarded = iAnswered = iDropped = iTimedOut = iRefused = 0;
	}

int DnsRelay::Pending() const
	{
	int n = 0;
	for (int i = 0; i < kSlots; i++)
		if (iSlot[i].used)
			n++;
	return n;
	}

// xorshift32, never 0; IDs unique among the pending queries
uint16_t DnsRelay::NewId()
	{
	for (;;)
		{
		iRand ^= iRand << 13;
		iRand ^= iRand >> 17;
		iRand ^= iRand << 5;
		uint16_t id = (uint16_t)(iRand >> 8);
		bool clash = false;
		for (int i = 0; i < kSlots; i++)
			if (iSlot[i].used && iSlot[i].newId == id)
				clash = true;
		if (!clash)
			return id;
		}
	}

void DnsRelay::Expire(uint32_t aNowMs)
	{
	for (int i = 0; i < kSlots; i++)
		if (iSlot[i].used && aNowMs - iSlot[i].sentMs >= kTimeoutMs)
			{
			iSlot[i].used = false;
			iTimedOut++;
			}
	}

static uint16_t Get16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void Put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// a standard query (QR 0, opcode 0) with one question and no answers
bool DnsRelay::IsQuery(const uint8_t* aPkt, size_t aLen) const
	{
	return aPkt && aLen >= 12 && aLen <= kMaxPacket && !(aPkt[2] & 0x80) && ((aPkt[2] >> 3) & 0x0f) == 0
		&& Get16(aPkt + 4) == 1 && Get16(aPkt + 6) == 0;
	}

bool DnsRelay::FromClient(uint8_t* aPkt, size_t aLen, uint32_t aClient, uint16_t aPort, uint32_t aServer, uint32_t aNowMs)
	{
	Expire(aNowMs);
	if (!IsQuery(aPkt, aLen) || !aServer || !aPort)
		{
		iDropped++;
		return false;
		}
	uint16_t orig = Get16(aPkt);
	// the Psion asking again (its own retry): the same slot, the same ID upstream
	int slot = -1;
	for (int i = 0; i < kSlots; i++)
		if (iSlot[i].used && iSlot[i].client == aClient && iSlot[i].port == aPort && iSlot[i].origId == orig)
			slot = i;
	if (slot < 0)
		{
		for (int i = 0; i < kSlots && slot < 0; i++)
			if (!iSlot[i].used)
				slot = i;
		if (slot < 0)
			{
			// full: the oldest goes (it has most likely been given up on)
			slot = 0;
			for (int i = 1; i < kSlots; i++)
				if (aNowMs - iSlot[i].sentMs > aNowMs - iSlot[slot].sentMs)
					slot = i;
			iTimedOut++;
			}
		Slot& s = iSlot[slot];
		s.used = false;                      // (NewId must not see the old one)
		s.newId = NewId();
		s.used = true;
		s.origId = orig;
		s.client = aClient;
		s.port = aPort;
		}
	Slot& s = iSlot[slot];
	s.server = aServer;
	s.sentMs = aNowMs;
	Put16(aPkt, s.newId);
	iForwarded++;
	return true;
	}

size_t DnsRelay::Refuse(uint8_t* aPkt, size_t aLen)
	{
	if (!IsQuery(aPkt, aLen))
		{
		iDropped++;
		return 0;
		}
	// the question's end: its name (plain labels, no pointers, at most 255
	// bytes), then the type and class. What follows it (an EDNS record) goes
	size_t o = 12, name = 0;
	for (;;)
		{
		if (o >= aLen)
			{
			iDropped++;
			return 0;
			}
		uint8_t n = aPkt[o];
		if (!n)
			break;
		name += n + 1u;
		if (n > 63 || name > 255)
			{
			iDropped++;
			return 0;
			}
		o += n + 1u;
		}
	o += 1 + 4;
	if (o > aLen)
		{
		iDropped++;
		return 0;
		}
	aPkt[2] = (uint8_t)(0x80 | (aPkt[2] & 0x01));   // an answer; RD as asked
	aPkt[3] = (uint8_t)(0x80 | kOfflineRcode);      // RA, and the code
	Put16(aPkt + 6, 0);
	Put16(aPkt + 8, 0);
	Put16(aPkt + 10, 0);
	iRefused++;
	return o;
	}

bool DnsRelay::FromServer(uint8_t* aPkt, size_t aLen, uint32_t aFrom, uint16_t aFromPort, uint32_t& aClient,
	uint16_t& aPort, uint32_t aNowMs)
	{
	Expire(aNowMs);
	if (!aPkt || aLen < 12 || aLen > 4096 || aFromPort != 53 || !(aPkt[2] & 0x80))
		{
		iDropped++;
		return false;
		}
	uint16_t id = Get16(aPkt);
	for (int i = 0; i < kSlots; i++)
		{
		Slot& s = iSlot[i];
		if (s.used && s.newId == id && s.server == aFrom)
			{
			Put16(aPkt, s.origId);
			aClient = s.client;
			aPort = s.port;
			s.used = false;
			iAnswered++;
			return true;
			}
		}
	iDropped++;
	return false;
	}

} // namespace am
