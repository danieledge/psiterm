// dnsrelay.h - a small DNS forwarder for the PPP side (2.2): the Psion asks
// 192.168.7.1:53, the modem passes the query on to a real server and the
// answer back, so the Psion can be set once to DNS 192.168.7.1 and work on
// any network (and see the home network's names while the WireGuard tunnel
// is up). Plain C++ with no lwIP calls (host-tested); board/pppdns.cpp binds
// it to lwIP's raw UDP on the tcpip thread.
//
// Which server: DnsUpstream() - the tunnel's DNS while the tunnel is up and
// its config names one, else the uplink's DHCP DNS, else 1.1.1.1.
//
// Bounded and untrusting: a query is at most 512 bytes and must look like
// one (a standard query with one question); at most kSlots are pending,
// each with its own random upstream ID and a kTimeoutMs life; an answer is
// taken only from the server the query went to, from port 53, with a
// pending ID. Anything else is counted and dropped.
//
// With no uplink (Refuse) a query is answered at once, here, with
// SERVFAIL ("the server cannot answer now"), rather than sent into nothing
// to time out: the Psion's apps then say so in a moment, and the link check
// in psiglue (a lookup that must be answered) sees the PPP link working.
// SERVFAIL rather than NXDOMAIN, which says the name does not exist and may
// be remembered after the uplink is back.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_DNSRELAY_H
#define ATOM_DNSRELAY_H

#include <stdint.h>
#include <stddef.h>

namespace am {

static const uint32_t kDnsFallback = 0x01010101;   // 1.1.1.1

// the server queries go to (an ip4_addr, a.b.c.d in memory order)
uint32_t DnsUpstream(bool aTunnelUp, uint32_t aTunnelDns, uint32_t aUplinkDns);

class DnsRelay
	{
public:
	static const int kSlots = 16;
	static const uint32_t kTimeoutMs = 5000;
	static const size_t kMaxPacket = 512;
	static const uint8_t kOfflineRcode = 2;          // SERVFAIL

	DnsRelay();
	void Reset();
	// A query from the Psion (aClient:aPort). True: send aPkt (its ID
	// rewritten in place) to aServer:53. False: drop it.
	bool FromClient(uint8_t* aPkt, size_t aLen, uint32_t aClient, uint16_t aPort, uint32_t aServer, uint32_t aNowMs);
	// An answer from aFrom:aFromPort. True: send aPkt (the Psion's ID put
	// back) to aClient:aPort. False: drop it.
	bool FromServer(uint8_t* aPkt, size_t aLen, uint32_t aFrom, uint16_t aFromPort, uint32_t& aClient,
		uint16_t& aPort, uint32_t aNowMs);
	// No uplink: aPkt (a query from the Psion) made into its own answer in
	// place, with kOfflineRcode, the question kept and nothing else. Returns
	// the answer's length (send it back to the Psion), 0 to drop it
	size_t Refuse(uint8_t* aPkt, size_t aLen);
	// forget queries older than kTimeoutMs
	void Expire(uint32_t aNowMs);
	int Pending() const;
	// counters, for the log and the status
	uint32_t Forwarded() const { return iForwarded; }
	uint32_t Answered() const { return iAnswered; }
	uint32_t Dropped() const { return iDropped; }
	uint32_t TimedOut() const { return iTimedOut; }
	uint32_t Refused() const { return iRefused; }
	// (the tests) seed the ID generator
	void Seed(uint32_t aSeed) { iRand = aSeed ? aSeed : 1; }

private:
	struct Slot
		{
		bool used;
		uint16_t origId, newId;
		uint32_t client;
		uint16_t port;
		uint32_t server;
		uint32_t sentMs;
		};
	Slot iSlot[kSlots];
	uint32_t iRand;
	uint32_t iForwarded, iAnswered, iDropped, iTimedOut, iRefused;
	uint16_t NewId();
	bool IsQuery(const uint8_t* aPkt, size_t aLen) const;
	};

} // namespace am

#endif
