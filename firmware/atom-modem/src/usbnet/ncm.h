// ncm.h - CDC-NCM framing (USB CDC NCM 1.0, 16-bit NTBs): the Ethernet
// frames an NCM device (an Android phone with USB tethering on, an NCM
// adapter) sends and expects are wrapped in NCM Transfer Blocks. This
// parses the blocks the device sends and builds the ones we send. Pure
// functions, no USB or Arduino calls: the ESP32-S3's USB host task uses
// them, and the host tests run them.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_NCM_H
#define ATOM_NCM_H

#include <stdint.h>
#include <stddef.h>

namespace am {

static const uint32_t kNcmNthSig = 0x484d434eUL;      // "NCMH"
static const uint32_t kNcmNdpSig = 0x304d434eUL;      // "NCM0" (no CRC)
static const uint32_t kNcmNdpCrcSig = 0x314d434eUL;   // "NCM1" (with CRC: accepted, not checked)

// class requests (bRequest), on the communication interface
static const uint8_t kNcmGetNtbParameters = 0x80;
static const uint8_t kNcmSetNtbInputSize = 0x86;
static const uint8_t kNcmSetNtbFormat = 0x84;
static const uint8_t kNcmSetEthernetPacketFilter = 0x43;   // (ECM too)
static const uint16_t kNcmFilterAll = 0x000f;             // promiscuous off; multicast, broadcast, directed on
static const uint16_t kNcmMaxFrame = 1514;

struct NcmParams              // GET_NTB_PARAMETERS, as the device answers it
	{
	uint32_t ntbInMaxSize;
	uint16_t ndpInDivisor, ndpInRemainder, ndpInAlignment;
	uint32_t ntbOutMaxSize;
	uint16_t ndpOutDivisor, ndpOutRemainder, ndpOutAlignment;
	uint16_t ntbOutMaxDatagrams;
	bool ntb32;               // the device supports 32-bit NTBs (we use 16)
	};
// false: too short or absurd
bool NcmParseParams(const uint8_t* aData, size_t aLen, NcmParams& aOut);

struct NcmDatagram
	{
	const uint8_t* data;
	size_t len;
	};
// the datagrams in an NTB16 the device sent: how many were found (at most
// aMax), or -1 if the block is not an NTB16. Pointers are into aNtb
int NcmParse(const uint8_t* aNtb, size_t aLen, NcmDatagram* aOut, int aMax);

// one Ethernet frame as an NTB16 for the device (the NDP first, then the
// datagram, each aligned as the parameters ask). The size written, or 0 if
// it does not fit
size_t NcmBuild(uint8_t* aOut, size_t aOutMax, const uint8_t* aFrame, size_t aFrameLen, uint16_t aSequence,
	const NcmParams& aParams);

} // namespace am

#endif
