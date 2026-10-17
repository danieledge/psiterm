// pppdns.h - the DNS relay (dnsrelay.h) on lwIP's raw UDP (2.2): it answers
// on 192.168.7.1:53, the modem's end of the PPP link, while a PPP call is
// up, and passes each query on to the upstream the modem chose (the
// tunnel's DNS, the uplink's, or 1.1.1.1). All its lwIP work runs on the
// tcpip thread (the receive callbacks) or under LOCK_TCPIP_CORE (Start,
// Stop). The upstream socket is bound to no address, so the route hook
// (wgtunnel.h) sends a query for the tunnel's DNS through the tunnel.
// The relay's socket is bound with SO_REUSEADDR, as the access point's
// captive DNS is (the Arduino DNSServer, on every address): the two share
// port 53, and lwIP gives a query for 192.168.7.1 to the relay, the more
// specific. If the bind fails all the same, the PPP link offers the
// uplink's DNS instead (see PppLink::Start).
// With no uplink (SetOffline) each query is answered at once, SERVFAIL
// (DnsRelay::Refuse), and nothing goes upstream.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_PPPDNS_H
#define ATOM_PPPDNS_H

#include <stdint.h>
#include "../dnsrelay.h"

namespace am {

class PppDns
	{
public:
	bool Start();                 // false: could not bind 192.168.7.1:53
	void Stop();
	bool Running() const { return iRunning; }
	void SetUpstream(uint32_t aServer) { iUpstream = aServer; }
	uint32_t Upstream() const { return iUpstream; }
	void SetOffline(bool aOffline) { iOffline = aOffline; }   // no uplink: answer at once
	const DnsRelay& Relay() const { return iRelay; }
	// (the lwIP callbacks)
	void FromPsion(void* aPbuf, uint32_t aIp, uint16_t aPort);
	void FromServer(void* aPbuf, uint32_t aIp, uint16_t aPort);
	PppDns() : iRunning(false), iOffline(false), iUpstream(kDnsFallback), iClient(0), iUp(0) {}

private:
	bool iRunning;
	volatile bool iOffline;
	volatile uint32_t iUpstream;
	void* iClient;                // udp_pcb on 192.168.7.1:53
	void* iUp;                    // udp_pcb towards the servers
	DnsRelay iRelay;
	};

extern PppDns gPppDns;

} // namespace am

#endif
