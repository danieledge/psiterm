// ipheth.cpp - see ipheth.h. MIT licence (see LICENSE at the top of the repository).
#include "ipheth.h"
#include <string.h>

namespace am {

bool IphethMatch(uint16_t aVid, uint8_t aClass, uint8_t aSubclass, uint8_t aProtocol)
	{
	return aVid == kAppleVid && aClass == kIphethClass && aSubclass == kIphethSubclass && aProtocol == kIphethProtocol;
	}

bool IphethCarrier(const uint8_t* aAnswer, size_t aLen)
	{
	return aLen >= 1 && aAnswer[0] == kIphethCarrierOn;
	}

bool IphethUnpack(const uint8_t* aIn, size_t aLen, const uint8_t*& aFrame, size_t& aFrameLen)
	{
	if (aLen <= kIphethAlign)
		return false;
	aFrame = aIn + kIphethAlign;
	aFrameLen = aLen - kIphethAlign;
	if (aFrameLen > kIphethMaxFrame)
		aFrameLen = kIphethMaxFrame;
	return true;
	}

size_t IphethPack(uint8_t* aOut, size_t aOutMax, const uint8_t* aFrame, size_t aFrameLen)
	{
	if (!aFrameLen || aFrameLen > kIphethMaxFrame || aFrameLen + kIphethAlign > aOutMax)
		return 0;
	memset(aOut, 0, kIphethAlign);
	memcpy(aOut + kIphethAlign, aFrame, aFrameLen);
	return aFrameLen + kIphethAlign;
	}

} // namespace am
