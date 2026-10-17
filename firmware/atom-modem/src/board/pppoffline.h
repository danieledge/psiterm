// pppoffline.h - the PPP side with no uplink: the Psion's packets for the
// outside world are answered at once, here, rather than left to time out.
// A new TCP connection (a SYN) is reset, as the far end would if nothing
// listened there, so the Psion's Connect fails in a moment; a connection
// already open is left alone (its packets dropped, as before), so a moment
// without WiFi does not end it. Any other packet but TCP and ICMP (UDP)
// gets an ICMP "network unreachable" from 192.168.7.1. Addresses on a
// network the Atom is on itself (the access point's, say) are forwarded as
// ever, and so is everything once the uplink is back.
//
// How. Every packet the Atom forwards is routed through the IDF lwIP's
// ip4_route_src_hook, which the firmware wraps (board/wgtunnel.h): while
// there is no uplink it routes the Psion's packets for the outside to a
// netif of our own that is never in lwIP's list. That netif is marked as
// NAT'd itself, so lwIP leaves the packet untranslated (ip4_forward does
// no NAPT out of a NAPT netif), and its output function builds the answer
// and sends it back over PPP (lwIP's tcp_rst and icmp_dest_unreach, which
// route to the Psion through the PPP link's peer address). All of it runs
// on the tcpip thread, inside the forward. A board without the hook (no
// WireGuard, AM_WG unset) leaves such packets to be dropped, as before.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_PPPOFFLINE_H
#define ATOM_PPPOFFLINE_H

#include <stdint.h>

struct netif;
struct ip4_addr;

namespace am {

// the modem's loop: there is no uplink (true) or there is again
void PppOfflineSet(bool aOffline);
// how many packets have been answered so far, and the count cleared
uint32_t PppOfflineTake();

// (the route hook, on the tcpip thread) true: there is no uplink and aSrc
// is the Psion. Then a packet for the outside (not for a network the Atom
// is on itself) is routed to PppOfflineNetif(), which answers it
bool PppOfflineFrom(const struct ip4_addr* aSrc);
struct netif* PppOfflineNetif();

} // namespace am

#endif
