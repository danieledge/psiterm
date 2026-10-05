// uplink.cpp - see uplink.h. MIT licence (see LICENSE at the top of the repository).
#include "uplink.h"
#include <stdio.h>
#include <string.h>

namespace am {

Uplink::Uplink()
	: iMode(EUplinkAuto), iActive(EActiveNone), iWifi(false), iUsb(EUsbNone), iInternet(-1), iWifiState(EWifiDown),
	  iHadInternet(false), iLostMs(0), iUsbDownSince(0), iUsbWasUp(false)
	{
	}

void Uplink::Configure(int aMode)
	{
	iMode = aMode;
	iUsbDownSince = 0;
	}

void Uplink::Tick(uint32_t aNowMs, bool aWifiUp, int aUsbState, int aInternet)
	{
	iWifi = aWifiUp;
	iUsb = aUsbState;
	iInternet = aWifiUp ? aInternet : -1;
	// the WiFi side
	if (aWifiUp)
		{
		iLostMs = 0;
		if (aInternet == 1)
			{
			iWifiState = EWifiInternet;
			iHadInternet = true;
			}
		else
			iWifiState = EWifiAssociated;    // (joined; the check failed, or has not run)
		}
	else
		{
		if (iHadInternet && !iLostMs)
			iLostMs = aNowMs ? aNowMs : 1;
		if (iLostMs && aNowMs - iLostMs < kRecoverMs)
			iWifiState = EWifiRecovering;
		else
			{
			iWifiState = EWifiDown;
			iHadInternet = false;
			iLostMs = 0;
			}
		}
	// the USB side
	bool usbUp = aUsbState == EUsbUp;
	if (usbUp)
		iUsbDownSince = 0;
	else if (iUsbWasUp && !iUsbDownSince)
		iUsbDownSince = aNowMs ? aNowMs : 1;     // (0 means "not down")
	iUsbWasUp = usbUp;
	// a USB link that has just gone may come back (a phone changing mode):
	// hold on to it for a moment before WiFi takes over
	bool usbHeld = !usbUp && iUsbDownSince && aNowMs - iUsbDownSince < kHoldMs;
	switch (iMode)
		{
	case EUplinkWifi:
		iActive = aWifiUp ? EActiveWifi : EActiveNone;
		break;
	case EUplinkUsb:
		iActive = usbUp ? EActiveUsb : EActiveNone;
		break;
	default:
		// USB only once it has an address (EUsbUp): a device that is merely
		// plugged in, or an iPhone without a carrier, never takes over
		if (usbUp)
			iActive = EActiveUsb;
		else if (usbHeld && iActive == EActiveUsb)
			iActive = EActiveUsb;                // (a call in progress keeps its route)
		else
			iActive = aWifiUp ? EActiveWifi : EActiveNone;
		break;
		}
	if (!usbHeld && !usbUp && iUsbDownSince && aNowMs - iUsbDownSince >= kHoldMs)
		iUsbDownSince = 0;
	}

const char* Uplink::UsbStateName(int aState)
	{
	switch (aState)
		{
	case EUsbEnumerating: return "enumerating";
	case EUsbNoDriver: return "no driver for this device";
	case EUsbNoCarrier: return "no carrier (tethering off, or an unpaired iPhone)";
	case EUsbUp: return "up";
	default: return "no device";
		}
	}

const char* Uplink::WifiStateName(int aState)
	{
	switch (aState)
		{
	case EWifiAssociated: return "joined, Internet not confirmed";
	case EWifiInternet: return "joined, Internet reachable";
	case EWifiRecovering: return "lost, joining again";
	default: return "down";
		}
	}

void Uplink::Describe(char* aOut, size_t aMax) const
	{
	static const char* const kModes[] = { "AUTO", "WIFI", "USB" };
	const char* mode = iMode >= 0 && iMode <= 2 ? kModes[iMode] : "?";
	const char* active = iActive == EActiveUsb ? "USB" : iActive == EActiveWifi ? "WiFi" : "none";
	const char* wifi = WifiStateName(iWifiState);
	snprintf(aOut, aMax, "%s: %s in use; WiFi %s%s; USB %s", mode, active, wifi,
		iWifiState == EWifiAssociated ? (iInternet == 0 ? " (the check failed)" : " (checking)") : "", UsbStateName(iUsb));
	}

} // namespace am
