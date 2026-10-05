// uplink.h - which network carries the Psion's connections: the WiFi
// station, or a USB network device (a phone tethering, a USB Ethernet
// adapter) on the ESP32-S3's USB host. A small state machine fed a few
// facts each tick, with a hold-off so a phone that re-enumerates does not
// bounce the choice.
//
// Being on a WiFi network is not the same as having the Internet behind
// it (a hotspot whose phone has lost its signal, a network with a captive
// portal): the WiFi side has four states, told apart by a periodic
// Internet check that the board runs (Hal::ProbeInternet). Calls are
// allowed as soon as the network is joined - the check's target can be
// blocked where the real server is not - but the state is shown by AT$UP?,
// ATI and the web pages, and logged as it changes, so a failure is not
// a mystery. Plain C++, no Arduino calls (host-tested).
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_UPLINK_H
#define ATOM_UPLINK_H

#include <stdint.h>
#include <stddef.h>
#include "settings.h"

namespace am {

// what the USB host reports (Hal::UsbState)
enum TUsbState
	{
	EUsbNone = 0,        // no USB host (not this board, or AT$USB=0), or nothing plugged in
	EUsbEnumerating,     // a device is being read
	EUsbNoDriver,        // a device we have no driver for
	EUsbNoCarrier,       // a network device, but no link (an unpaired iPhone, tethering off)
	EUsbUp               // a network device with an address
	};

// the WiFi station
enum TWifiState
	{
	EWifiDown = 0,       // not joined (no network set, out of range, wrong password)
	EWifiAssociated,     // joined; the Internet check has not passed (or not run yet)
	EWifiInternet,       // joined, and the Internet check passed
	EWifiRecovering      // had the Internet, lost the network: joining again
	};

enum TUplinkActive { EActiveNone = 0, EActiveWifi = 1, EActiveUsb = 2 };

class Uplink
	{
public:
	Uplink();
	void Configure(int aMode);                   // TUplink
	// the facts, once a tick. aInternet: the last Internet check, -1 unknown,
	// 0 failed, 1 passed (only meaningful while joined)
	void Tick(uint32_t aNowMs, bool aWifiUp, int aUsbState, int aInternet = -1);
	bool Up() const { return iActive != EActiveNone; }
	int Active() const { return iActive; }       // TUplinkActive
	int Mode() const { return iMode; }
	int UsbState() const { return iUsb; }
	int WifiState() const { return iWifiState; } // TWifiState
	int Internet() const { return iInternet; }
	// a few words for AT$UP? and the status page
	void Describe(char* aOut, size_t aMax) const;
	static const char* UsbStateName(int aState);
	static const char* WifiStateName(int aState);
	// a USB link that goes is left this long before WiFi takes over
	static const uint32_t kHoldMs = 5000;
	// a lost network is "recovering" this long before it is plain "down"
	static const uint32_t kRecoverMs = 10 * 60 * 1000;

private:
	int iMode;
	int iActive;
	bool iWifi;
	int iUsb;
	int iInternet;
	int iWifiState;
	bool iHadInternet;
	uint32_t iLostMs;                            // when the network went (0: not lost)
	uint32_t iUsbDownSince;                      // 0: not down
	bool iUsbWasUp;
	};

} // namespace am

#endif
