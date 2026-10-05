// usbhost.h - the ESP32-S3's USB-C as a USB host for a network device: an
// Android phone with USB tethering on (CDC-NCM), a CDC-ECM or NCM USB
// Ethernet adapter, or an iPhone (Apple's vendor interface, ipheth.h; its
// carrier stays down until Apple's pairing exists, which is not here: see
// docs/UPGRADE.md, section 12). Frames go into lwIP through esp_netif, a
// DHCP client asks the device for an address, and the modem then uses the
// network through sockets exactly as it uses WiFi. Built only with
// AM_USB_HOST (the atoms3-lite environment); a stub otherwise.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_USBHOST_H
#define ATOM_USBHOST_H

#include <stdint.h>
#include <stddef.h>
#include "../uplink.h"

namespace am {

class UsbNet
	{
public:
	UsbNet();
	// starts the host library and its tasks (the USB-C is then a host, and
	// the USB console is gone until the next restart). False: not possible
	bool Begin();
	// a TUsbState, and a few words (the device, the address)
	int State(char* aDetail, size_t aMax);
	bool Started() const { return iStarted; }

private:
	bool iStarted;
	};

} // namespace am

#endif
