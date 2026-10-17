// wgtunnel.h - the WireGuard client on the device (2.2): the vendored
// esphome-libs/wireguard (lib/wireguard) brought up and down on lwIP, the
// clock it needs (SNTP), and the split tunnel: an lwIP route hook that sends
// only the config's AllowedIPs through it.
//
// How the split routing works. lwIP has one default netif and routes by
// netif subnet, so a tunnel netif on its own would carry either nothing
// (its /32 address) or everything (if made the default, as most ESP32
// WireGuard examples do - which breaks the normal uplink for all else). The
// IDF's lwIP calls LWIP_HOOK_IP4_ROUTE_SRC (ip4_route_src_hook) first in
// every IPv4 route lookup: the Atom's own sockets (the modem route, the web
// proxy, the mail relay) and every packet it forwards (the Psion's, over
// PPP and NAT). The prebuilt liblwip.a defines that hook itself (routing by
// source address), so the firmware wraps it at link time
// (-Wl,--wrap=ip4_route_src_hook): __wrap_ip4_route_src_hook picks the
// tunnel when WgUseTunnel() (wgconf.h, host-tested) says so - the
// destination is inside an AllowedIPs prefix that is longer than any
// directly connected subnet holding it, and is not the endpoint - and
// otherwise hands over to the IDF's own hook (__real_), so everything else
// routes exactly as before. A socket bound to another interface's address
// stays on that interface. With no uplink the hook also sends the Psion's
// packets for the outside to be answered at once (board/pppoffline.h).
//
// The Psion's traffic: it reaches the Atom over PPP and is NAT'd (NAPT is on
// for the PPP netif) out of whichever netif the route picks; for the tunnel
// that means its source becomes the tunnel's address, which the home
// gateway expects, and the replies come back through the tunnel and are
// translated back to 192.168.7.2.
//
// Threads: every lwIP and WireGuard call here runs under LOCK_TCPIP_CORE
// (the core checks it). The route table the hook reads is changed only
// under that lock; the hook itself runs with it held.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_WGTUNNEL_H
#define ATOM_WGTUNNEL_H

#include <stdint.h>
#include "../modem.h"

namespace am {

class WgTunnel
	{
public:
	WgTunnel();
	void Begin(Hal& aHal) { iHal = &aHal; }   // for the log
	// from AtomHal::WgApply (the loop task): bring it up with aConfig, or down
	void Apply(const WgConfig& aConfig, bool aWant);
	// from loop(): the clock, the endpoint lookup, the handshake, retries
	void Loop(uint32_t aNowMs);
	void Info(WgStatus& aOut);
	bool Available() const;       // built in (the S3 boards)
	// the tunnel's DNS server if the tunnel is up and has one, else 0
	uint32_t Dns() const;

private:
	void Start(uint32_t aNowMs);
	void Stop();
	void Fail(uint32_t aNowMs, const char* aWhy);
	void Log(const char* aLine);
	Hal* iHal;
	bool iWant;
	bool iRunning;                // the netif exists (esp_wireguard_connect has succeeded)
	int iState;                   // TWgState
	uint32_t iStateMs;            // when iState was entered
	uint32_t iNextMs;             // the next try
	uint32_t iFails;
	char iWhy[48];
	WgConfig iCfg;                // a copy: the strings the library points at live here
	};

extern WgTunnel gWg;

} // namespace am

#endif
