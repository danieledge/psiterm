// ncm.cpp - see ncm.h. MIT licence (see LICENSE at the top of the repository).
#include "ncm.h"
#include <string.h>

namespace am {

static uint16_t Le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t Le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void PutLe16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void PutLe32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

bool NcmParseParams(const uint8_t* aData, size_t aLen, NcmParams& aOut)
	{
	// wLength(2) bmNtbFormatsSupported(2) dwNtbInMaxSize(4) wNdpInDivisor(2)
	// wNdpInPayloadRemainder(2) wNdpInAlignment(2) reserved(2) dwNtbOutMaxSize(4)
	// wNdpOutDivisor(2) wNdpOutPayloadRemainder(2) wNdpOutAlignment(2) wNtbOutMaxDatagrams(2)
	if (aLen < 28)
		return false;
	memset(&aOut, 0, sizeof(aOut));
	aOut.ntb32 = (Le16(aData + 2) & 2) != 0;
	aOut.ntbInMaxSize = Le32(aData + 4);
	aOut.ndpInDivisor = Le16(aData + 8);
	aOut.ndpInRemainder = Le16(aData + 10);
	aOut.ndpInAlignment = Le16(aData + 12);
	aOut.ntbOutMaxSize = Le32(aData + 16);
	aOut.ndpOutDivisor = Le16(aData + 20);
	aOut.ndpOutRemainder = Le16(aData + 22);
	aOut.ndpOutAlignment = Le16(aData + 24);
	aOut.ntbOutMaxDatagrams = Le16(aData + 26);
	// an alignment must be a power of two, 4 or more; a divisor at least 1
	if (aOut.ndpOutAlignment < 4 || (aOut.ndpOutAlignment & (aOut.ndpOutAlignment - 1)))
		aOut.ndpOutAlignment = 4;
	if (aOut.ndpOutDivisor == 0)
		aOut.ndpOutDivisor = 4;
	if (aOut.ndpOutRemainder >= aOut.ndpOutDivisor)
		aOut.ndpOutRemainder = 0;
	if (aOut.ntbOutMaxSize < 64 || aOut.ntbOutMaxSize > 65535)
		aOut.ntbOutMaxSize = 2048;
	if (aOut.ntbInMaxSize < 64)
		aOut.ntbInMaxSize = 2048;
	return true;
	}

int NcmParse(const uint8_t* aNtb, size_t aLen, NcmDatagram* aOut, int aMax)
	{
	if (aLen < 12 || Le32(aNtb) != kNcmNthSig)
		return -1;
	uint16_t headerLen = Le16(aNtb + 4);
	uint16_t blockLen = Le16(aNtb + 8);
	uint16_t ndpIndex = Le16(aNtb + 10);
	if (headerLen != 12 || blockLen > aLen || blockLen < 12)
		return -1;
	int n = 0;
	for (int chain = 0; chain < 8 && ndpIndex; chain++)
		{
		// NDP16: dwSignature(4) wLength(2) wNextNdpIndex(2) then (wDatagramIndex, wDatagramLength) pairs
		if ((size_t)ndpIndex + 8 > blockLen)
			return -1;
		const uint8_t* ndp = aNtb + ndpIndex;
		uint32_t sig = Le32(ndp);
		if (sig != kNcmNdpSig && sig != kNcmNdpCrcSig)
			return -1;
		uint16_t ndpLen = Le16(ndp + 4);
		uint16_t next = Le16(ndp + 6);
		if (ndpLen < 16 || (size_t)ndpIndex + ndpLen > blockLen)
			return -1;
		for (size_t p = 8; p + 4 <= ndpLen; p += 4)
			{
			uint16_t di = Le16(ndp + p), dl = Le16(ndp + p + 2);
			if (!di || !dl)
				break;                       // the terminator
			if ((size_t)di + dl > blockLen)
				return -1;
			if (n < aMax)
				{
				aOut[n].data = aNtb + di;
				aOut[n].len = dl;
				}
			n++;
			}
		if (next == ndpIndex)
			break;
		ndpIndex = next;
		}
	return n > aMax ? aMax : n;
	}

static size_t AlignUp(size_t aV, size_t aAlign)
	{
	return (aV + aAlign - 1) & ~(aAlign - 1);
	}

size_t NcmBuild(uint8_t* aOut, size_t aOutMax, const uint8_t* aFrame, size_t aFrameLen, uint16_t aSequence,
	const NcmParams& aParams)
	{
	if (!aFrameLen || aFrameLen > kNcmMaxFrame)
		return 0;
	size_t ndpAt = AlignUp(12, aParams.ndpOutAlignment);
	size_t ndpLen = 16;                      // signature, length, next, one pair, the terminator
	// the datagram at an offset that is remainder (mod divisor)
	size_t dataAt = ndpAt + ndpLen;
	size_t div = aParams.ndpOutDivisor, rem = aParams.ndpOutRemainder;
	dataAt = ((dataAt - rem + div - 1) / div) * div + rem;
	size_t total = dataAt + aFrameLen;
	if (total > aOutMax || total > aParams.ntbOutMaxSize || total > 65535)
		return 0;
	memset(aOut, 0, dataAt);
	PutLe32(aOut, kNcmNthSig);
	PutLe16(aOut + 4, 12);
	PutLe16(aOut + 6, aSequence);
	PutLe16(aOut + 8, (uint16_t)total);
	PutLe16(aOut + 10, (uint16_t)ndpAt);
	uint8_t* ndp = aOut + ndpAt;
	PutLe32(ndp, kNcmNdpSig);
	PutLe16(ndp + 4, (uint16_t)ndpLen);
	PutLe16(ndp + 6, 0);
	PutLe16(ndp + 8, (uint16_t)dataAt);
	PutLe16(ndp + 10, (uint16_t)aFrameLen);
	PutLe16(ndp + 12, 0);
	PutLe16(ndp + 14, 0);
	memcpy(aOut + dataAt, aFrame, aFrameLen);
	return total;
	}

} // namespace am
