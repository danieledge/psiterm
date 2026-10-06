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
// address in its Internet settings: IP 192.168.7.2, gateway 192.168.7.1 (the
// modem's PPP end) and a real DNS server (the uplink's, logged at the dial;
// 1.1.1.1 works anywhere): nothing on the modem answers DNS itself, the
// queries are NAT'd out like any other packet. See docs and PppLink::Start.
//
// Output towards the Psion: lwIP's output callback runs on the tcpip thread
// and must not touch the serial HAL. It only queues the frame (device: a
// FreeRTOS stream buffer; host: a vector), dropping the WHOLE frame and
// counting it when the queue is full; Modem::Loop() calls Drain(), which
// writes the queue to the UART paced like any other output.
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

// AM_PPP_DEVICE: lwIP with PPPoS and NAPT (the AtomS3 boards' prebuilt lwIP).
// AM_PPP_NONE: a board whose lwIP has neither (the plain Atom, Arduino-ESP32
// 2.0.17): the dial then gives NO CARRIER. Otherwise (the host tests) a stub
// with a real output queue.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if defined(CONFIG_LWIP_PPP_SUPPORT) && defined(CONFIG_LWIP_IPV4_NAPT) && defined(CONFIG_LWIP_IP_FORWARD)
#define AM_PPP_DEVICE 1
#else
#define AM_PPP_NONE 1
#endif
#endif
#ifndef AM_PPP_DEVICE
#include <deque>
#endif

namespace am {

class Hal;
class Pacer;

// an IPv4 address as lwIP's ip4_addr holds it: the bytes a,b,c,d in memory order
inline uint32_t Ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
	{
	return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
	}
static const uint32_t kPppFallbackDns = 0x01010101;   // 1.1.1.1, when the uplink has none
static const size_t kPppStage = 3200;                  // one escaped PPP frame (MTU 1500 worst case ~3 KB) is gathered here
static const size_t kPppOutQueue = 12 * 1024;          // frames waiting for the UART

// the modem's PPP end and the address the Psion must be set to (static)
static const char* const kPppLocalIp  = "192.168.7.1";   // the modem (gateway/DNS for the Psion)
static const char* const kPppRemoteIp = "192.168.7.2";   // the Psion's "Psion Internet" static IP

class PppLink
	{
public:
	PppLink();
	~PppLink();
	// Bring the PPP machinery up on the serial line. Output towards the Psion
	// is queued (QueueOut) and written by Drain. Returns false if it could not start (then the dial gives NO CARRIER).
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
	// PPP -> the Psion: write what is queued to the UART, paced by aPacer
	// (as PumpPsion does), at most a few bursts per call. Returns the bytes
	// written. The Modem calls it only while online (after +++ nothing goes)
	size_t Drain(Hal& aHal, Pacer& aPacer);
	// A frame from lwIP's output callback (any thread; it never touches the
	// HAL). lwIP calls it with a frame in ~1.5 KB chunks, the last one ending
	// in the 0x7e flag. The chunks are gathered, and the frame is queued whole
	// at the flag, or not at all: a full queue (or a frame too big to gather)
	// drops the whole frame, once, and counts it (DroppedFrames); the rest of
	// its chunks are swallowed up to the flag. Public so the host tests can
	// play lwIP
	void QueueOut(const uint8_t* aData, size_t aLen);
	uint32_t DroppedFrames() const { return iDropFrames; }
	size_t QueuedBytes() const;
	// the DNS server given to the Psion (an ip4_addr, see Ip4)
	uint32_t Dns() const { return iDns; }
	// host tests: how many bytes have been handed to PPP
	uint32_t BytesIn() const { return iBytesIn; }

private:
	bool iActive;
	bool iUp;
	bool iDropped;
	bool iNat;                 // NAT has been turned on
	uint32_t iBytesIn;
	uint32_t iDns;
	volatile uint32_t iDropFrames;
	bool iDropping;            // swallowing the rest of a dropped frame, up to its 0x7e
	size_t iStageLen;
	uint8_t iStage[kPppStage];
	bool Enqueue(const uint8_t* aData, size_t aLen);   // all of it or none, into the drain queue
	bool iUpLogged;
	uint8_t iPend[256];        // taken from the queue, not yet written
	size_t iPendOfs, iPendLen;
	size_t QueueRead(uint8_t* aOut, size_t aMax);
	void QueueReset();
#ifdef AM_PPP_DEVICE
	void* iPcb;                // ppp_pcb* (opaque here, to keep lwIP out of the header)
	void* iQueue;              // the FreeRTOS stream buffer (kept for the life of the firmware)
	void OnStatus(int aErr);   // called from the PPP status callback (tcpip thread)
	friend struct PppCallbacks;
#else
	std::deque<uint8_t> iHostQ;   // the host build's queue
#endif
	};

} // namespace am

#endif
