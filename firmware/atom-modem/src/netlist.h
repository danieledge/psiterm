// netlist.h - the saved WiFi networks (2.2): up to kNetMax names and
// passwords, kept in NVS as one record ("nets"), and the choice of which one
// to join. Plain C++ with no Arduino calls (host-tested).
//
// How it fits with the 1.x record: Settings.ssid/pass stay "the network the
// modem is on, or is joining" (AT$SSID, AT$PASS, AT$JOIN and ATW set it, as
// before). AT&W (and the web pages' Save) also put it into this list, so a
// join that is kept adds or updates an entry rather than replacing the only
// one. A modem updated from 2.1 or older has its one network moved into slot
// 1 at the first start.
//
// Choosing: at the start (with more than one saved) and whenever the WiFi
// has been down for a while, the modem scans and joins the strongest saved
// network in range, falling back through the rest. A network picked by hand
// (AT$NET, the web pages, PsiConnect, the button) is "pinned": it is preferred
// while it is in range, and the pin goes once it is not.
//
// Passwords: in NVS only, as the 1.x record's. Nothing prints them: the
// list shows only whether one is set.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_NETLIST_H
#define ATOM_NETLIST_H

#include <stdint.h>
#include <stddef.h>

namespace am {

struct WifiNet;

static const int kNetMax = 8;

struct SavedNet
	{
	char ssid[33];
	char pass[65];             // SECRET; "" = an open network
	uint8_t flags;             // spare
	uint8_t spare;
	};

struct NetList
	{
	uint32_t magic;            // kNetMagic
	uint16_t version;
	uint16_t length;           // sizeof(NetList) when saved
	uint8_t count;             // entries in use, from net[0]; the order is the order of preference
	uint8_t spare[7];
	SavedNet net[kNetMax];
	};

static const uint32_t kNetMagic = 0x41544e4c;   // 'ATNL'
static const uint16_t kNetVersion = 1;

void NetListInit(NetList& aL);
// a record from NVS (aLength bytes): true if it is one; strings ended, count clamped
bool NetListAccept(NetList& aL, size_t aLength);
// the index of a saved name (exact bytes), or -1
int NetFind(const NetList& aL, const char* aSsid);
// add, or update the password of, a network. A full list loses its last
// entry that is not aKeep (-1: none to keep) to make room; aEvicted says so.
// Returns the index, or -1 (an empty name)
int NetUpsert(NetList& aL, const char* aSsid, const char* aPass, int aKeep, bool* aEvicted = 0);
bool NetForget(NetList& aL, int aIndex);
// move an entry to the front (the most preferred)
bool NetPrefer(NetList& aL, int aIndex);

// The one to join, from a scan: the pinned name if it is in range (and not
// in aSkip), else the strongest saved network in range not in aSkip (bit i:
// entry i failed this time round); ties go to the earlier entry. -1: none
// in range. aRssi: its signal, if not 0
int NetChoose(const NetList& aL, const WifiNet* aScan, int aCount, const char* aPinned, uint32_t aSkip, int* aRssi = 0);

// "the n-th after aCurrent that is in range", for the button's "next
// network": the next saved entry after aCurrent (wrapping) that the scan
// found; if the scan found none, simply the next saved entry. -1: none other
int NetNext(const NetList& aL, int aCurrent, const WifiNet* aScan, int aCount);

} // namespace am

#endif
