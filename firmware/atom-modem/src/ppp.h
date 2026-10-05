// ppp.h - PPP-over-serial (PPPoS) with NAT, so the Psion's own "Psion
// Internet" dial-up can share the modem's WiFi (or USB) connection. The
// Psion dials a plain number (ATD777); instead of a TCP call the modem
// brings up a PPP link on the serial line and NATs the Psion out over the
// uplink. One dial, one link, many concurrent sockets - the dial-up-router
// model, self-contained in the modem (no PC needed).
//
// Addressing: the prebuilt S3 lwIP has PPPoS and NAPT but NOT the PPP
// *server* role (CONFIG_LWIP_PPP_SERVER_SUPPORT is off), so the modem cannot
// hand the Psion an address over IPCP. The Psion is therefore given a STATIC
// address in its Internet settings: IP 192.168.7.2, gateway/DNS 192.168.7.1
// (the modem's PPP end). See docs and PppLink::Start.
//
// All the lwIP/esp_netif code is behind the device guard, so the same header
// compiles for the host tests, where PppLink is a small stub that only tracks
// the mode (the dial dispatch is what the host test exercises; PPP itself is
// verified on hardware). Plain C++, no Arduino calls in the host build.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_PPP_H
#define ATOM_PPP_H

#include <stdint.h>
#include <stddef.h>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#define AM_PPP_DEVICE 1
#endif

namespace am {

class Hal;

// the modem's PPP end and the address the Psion must be set to (static)
static const char* const kPppLocalIp  = "192.168.7.1";   // the modem (gateway/DNS for the Psion)
static const char* const kPppRemoteIp = "192.168.7.2";   // the Psion's "Psion Internet" static IP

class PppLink
	{
public:
	PppLink();
	~PppLink();
	// Bring the PPP machinery up on the serial line. Output towards the Psion
	// goes through aHal.SerialWrite (so pacing/flow control are the HAL's).
	// Returns false if it could not start (then the dial gives NO CARRIER).
	bool Start(Hal& aHal);
	// Tear the link down and turn NAT off.
	void Stop(Hal& aHal);
	bool Active() const { return iActive; }
	bool Up() const { return iUp; }              // IPCP done; NAT on
	bool Dropped() const { return iDropped; }    // the peer closed / an error
	// Psion -> PPP: raw serial bytes from the Psion into the HDLC decoder.
	// Returns how many were taken (always aLen; lwIP copies them).
	size_t Input(Hal& aHal, const uint8_t* aData, size_t aLen);
	// Called often: service the link (bring NAT up when IPCP finishes, notice
	// a drop). Harmless when not active.
	void Poll(Hal& aHal);
	// host tests: how many bytes have been handed to PPP
	uint32_t BytesIn() const { return iBytesIn; }

private:
	bool iActive;
	bool iUp;
	bool iDropped;
	bool iNat;                 // NAT has been turned on
	uint32_t iBytesIn;
#ifdef AM_PPP_DEVICE
	void* iPcb;                // ppp_pcb* (opaque here, to keep lwIP out of the header)
	Hal* iHal;                 // for the output callback (the lwIP status thread uses it)
	void OnStatus(int aErr);   // called from the PPP status callback (tcpip thread)
	void Output(const uint8_t* aData, size_t aLen);  // PPP -> the Psion
	friend struct PppCallbacks;
#endif
	};

} // namespace am

#endif
