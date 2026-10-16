// gzip.cpp - see gzip.h. MIT licence (see LICENSE at the top of the repository).
#include "gzip.h"
#include <string.h>
#include <stdlib.h>

namespace am {

uint32_t Crc32(uint32_t aCrc, const uint8_t* aData, size_t aLen)
	{
	static const uint32_t kNibble[16] = {
		0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
		0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c };
	uint32_t c = ~aCrc;
	while (aLen--)
		{
		c ^= *aData++;
		c = (c >> 4) ^ kNibble[c & 15];
		c = (c >> 4) ^ kNibble[c & 15];
		}
	return ~c;
	}

static const size_t kBufSize = 2 * GzipWriter::kWindow;
static const size_t kHashSize = 4096;
static const size_t kMaxMatch = 258;
static const size_t kLookahead = kMaxMatch + 3;

static const uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
	59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
	5, 5, 5, 5, 0 };
static const uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
	513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
	10, 11, 11, 12, 12, 13, 13 };

static inline unsigned Hash(const uint8_t* aP)
	{
	return (((unsigned)aP[0] * 33 + aP[1]) * 33 + aP[2]) & (kHashSize - 1);
	}

bool GzipWriter::Begin(HtmlSink* aOut)
	{
	End();
	iBuf = (uint8_t*)malloc(kBufSize);
	iHead = (uint16_t*)calloc(kHashSize, sizeof(uint16_t));
	if (!iBuf || !iHead)
		{
		End();
		return false;
		}
	iOut = aOut;
	iLen = iPos = 0;
	iBitBuf = 0;
	iBitCount = 0;
	iCrc = 0;
	iSize = 0;
	iDirty = false;
	iStageLen = 0;
	// the gzip header: no name, no time, "unknown" OS
	static const uint8_t kHeader[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff };
	for (size_t i = 0; i < sizeof(kHeader); i++)
		Byte(kHeader[i]);
	Bits(0, 1);                              // a block, not the last
	Bits(1, 2);                              // with the fixed Huffman codes
	return true;
	}

void GzipWriter::End()
	{
	free(iBuf);
	free(iHead);
	iBuf = 0;
	iHead = 0;
	}

void GzipWriter::Byte(uint8_t aB)
	{
	iStage[iStageLen++] = aB;
	if (iStageLen == sizeof(iStage))
		Drain();
	}

void GzipWriter::Drain()
	{
	if (iStageLen && iOut)
		iOut->Put((const char*)iStage, iStageLen);
	iStageLen = 0;
	}

void GzipWriter::Bits(uint32_t aValue, int aCount)
	{
	iBitBuf |= aValue << iBitCount;
	iBitCount += aCount;
	while (iBitCount >= 8)
		{
		Byte((uint8_t)iBitBuf);
		iBitBuf >>= 8;
		iBitCount -= 8;
		}
	}

void GzipWriter::Code(uint32_t aCode, int aLen)
	{
	uint32_t r = 0;
	for (int i = 0; i < aLen; i++)
		r |= ((aCode >> i) & 1) << (aLen - 1 - i);
	Bits(r, aLen);
	}

void GzipWriter::Literal(uint8_t aC)
	{
	if (aC < 144)
		Code(0x30 + aC, 8);
	else
		Code(0x190 + (aC - 144), 9);
	}

void GzipWriter::Match(unsigned aLen, unsigned aDist)
	{
	int i = 28;
	while (kLenBase[i] > aLen)
		i--;
	unsigned sym = 257 + i;
	if (sym < 280)
		Code(sym - 256, 7);
	else
		Code(0xc0 + (sym - 280), 8);
	if (kLenExtra[i])
		Bits(aLen - kLenBase[i], kLenExtra[i]);
	int d = 29;
	while (kDistBase[d] > aDist)
		d--;
	Code((uint32_t)d, 5);
	if (kDistExtra[d])
		Bits(aDist - kDistBase[d], kDistExtra[d]);
	}

// codes the bytes from iPos on; all of them, or all but the lookahead a
// longest match might need
void GzipWriter::Process(bool aAll)
	{
	while (iPos < iLen && (aAll || iLen - iPos >= kLookahead))
		{
		size_t avail = iLen - iPos;
		size_t best = 0, dist = 0;
		if (avail >= 3)
			{
			unsigned h = Hash(iBuf + iPos);
			size_t cand = iHead[h];
			iHead[h] = (uint16_t)(iPos + 1);
			if (cand && cand - 1 < iPos && iPos - (cand - 1) <= kWindow)
				{
				const uint8_t* a = iBuf + iPos;
				const uint8_t* b = iBuf + cand - 1;
				size_t max = avail < kMaxMatch ? avail : kMaxMatch;
				size_t n = 0;
				while (n < max && a[n] == b[n])
					n++;
				if (n >= 3)
					{
					best = n;
					dist = iPos - (cand - 1);
					}
				}
			}
		if (best)
			{
			Match((unsigned)best, (unsigned)dist);
			// the positions inside the match go into the hash too
			for (size_t p = iPos + 1; p < iPos + best && p + 3 <= iLen; p++)
				iHead[Hash(iBuf + p)] = (uint16_t)(p + 1);
			iPos += best;
			}
		else
			{
			Literal(iBuf[iPos]);
			iPos++;
			}
		}
	}

void GzipWriter::Feed(const uint8_t* aData, size_t aLen)
	{
	if (!iBuf)
		return;
	iCrc = Crc32(iCrc, aData, aLen);
	iSize += (uint32_t)aLen;
	iDirty = iDirty || aLen;
	while (aLen)
		{
		size_t n = kBufSize - iLen;
		if (n > aLen)
			n = aLen;
		memcpy(iBuf + iLen, aData, n);
		iLen += n;
		aData += n;
		aLen -= n;
		Process(false);
		if (iLen == kBufSize)
			{
			// slide the window down: the older half goes
			memmove(iBuf, iBuf + kWindow, kBufSize - kWindow);
			iLen -= kWindow;
			iPos -= kWindow;
			for (size_t i = 0; i < kHashSize; i++)
				iHead[i] = iHead[i] > kWindow ? (uint16_t)(iHead[i] - kWindow) : 0;
			}
		}
	Drain();
	}

void GzipWriter::Flush()
	{
	if (!iBuf || !iDirty)
		return;                              // (nothing new: no empty blocks)
	iDirty = false;
	Process(true);
	Code(0, 7);                              // the end of this block
	Bits(0, 3);                              // an empty stored block...
	if (iBitCount)
		Bits(0, 8 - iBitCount);              // ...on a byte boundary,
	Byte(0); Byte(0); Byte(0xff); Byte(0xff);  // ...of length 0
	Bits(0, 1);                              // and the next fixed block begins
	Bits(1, 2);
	Drain();
	}

void GzipWriter::Finish()
	{
	if (!iBuf)
		return;
	Process(true);
	Code(0, 7);                              // the end of this block
	Bits(1, 1);                              // the last block: empty
	Bits(1, 2);
	Code(0, 7);
	if (iBitCount)
		Bits(0, 8 - iBitCount);
	for (int i = 0; i < 4; i++)
		Byte((uint8_t)(iCrc >> (8 * i)));
	for (int i = 0; i < 4; i++)
		Byte((uint8_t)(iSize >> (8 * i)));
	Drain();
	End();
	}

} // namespace am
