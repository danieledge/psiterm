// imgconv.cpp - see imgconv.h. MIT licence (see LICENSE at the top of the repository).
#include "imgconv.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
extern "C" {
#include "pmimg.h"
}
#if defined(BOARD_HAS_PSRAM)
#include <Arduino.h>                      // psramFound (the AtomS3R build only)
#endif

namespace am {

// ===== the GIF writer =========================================================

static const unsigned kClear = 16;       // a 4-bit alphabet: 16 greys
static const unsigned kEoi = 17;
static const unsigned kFirstFree = 18;

GifWriter::GifWriter()
	: iOut(0), iWidth(0), iHeight(0), iFirst(0), iNext(0), iSym(0), iDict(0), iNextCode(0), iCodeBits(0),
	  iPrefix(-1), iBitBuf(0), iBitCount(0), iBlockLen(0), iWritten(0)
	{
	}

GifWriter::~GifWriter()
	{
	End();
	}

void GifWriter::Put(const uint8_t* aData, size_t aLen)
	{
	if (iOut && aLen)
		{
		iOut->Put((const char*)aData, aLen);
		iWritten += (uint32_t)aLen;
		}
	}

bool GifWriter::Begin(HtmlSink* aOut, int aWidth, int aHeight)
	{
	End();
	if (aWidth <= 0 || aHeight <= 0 || aWidth > 65535 || aHeight > 65535)
		return false;
	size_t n = kDictEntries;
	iDict = malloc(n * sizeof(uint16_t) * 2 + n);
	if (!iDict)
		return false;
	iFirst = (uint16_t*)iDict;
	iNext = iFirst + n;
	iSym = (uint8_t*)(iNext + n);
	iOut = aOut;
	iWidth = aWidth;
	iHeight = aHeight;
	iBitBuf = 0;
	iBitCount = 0;
	iBlockLen = 0;
	iWritten = 0;
	iPrefix = -1;
	// the header: GIF89a, the logical screen, a global colour table of 16
	// greys (level k is 17k), the image descriptor, the minimum code size
	uint8_t h[13 + 48 + 10 + 1];
	size_t p = 0;
	memcpy(h + p, "GIF89a", 6); p += 6;
	h[p++] = (uint8_t)(aWidth & 255); h[p++] = (uint8_t)(aWidth >> 8);
	h[p++] = (uint8_t)(aHeight & 255); h[p++] = (uint8_t)(aHeight >> 8);
	h[p++] = 0xf3;                       // a global colour table, 16 entries, 4 bits a colour
	h[p++] = 15;                         // background: white
	h[p++] = 0;                          // pixel aspect ratio: none
	for (int k = 0; k < 16; k++)
		{
		h[p++] = (uint8_t)(k * 17); h[p++] = (uint8_t)(k * 17); h[p++] = (uint8_t)(k * 17);
		}
	h[p++] = 0x2c;                       // image descriptor
	h[p++] = 0; h[p++] = 0; h[p++] = 0; h[p++] = 0;
	h[p++] = (uint8_t)(aWidth & 255); h[p++] = (uint8_t)(aWidth >> 8);
	h[p++] = (uint8_t)(aHeight & 255); h[p++] = (uint8_t)(aHeight >> 8);
	h[p++] = 0;                          // no local colour table, not interlaced
	h[p++] = 4;                          // LZW minimum code size
	Put(h, p);
	ClearDict();
	Code(kClear);
	return true;
	}

void GifWriter::ClearDict()
	{
	memset(iFirst, 0, kDictEntries * sizeof(uint16_t));
	iNextCode = kFirstFree;
	iCodeBits = 5;
	}

void GifWriter::Byte(uint8_t aB)
	{
	iBlock[iBlockLen++] = aB;
	if (iBlockLen == 255)
		FlushBlock();
	}

void GifWriter::FlushBlock()
	{
	if (!iBlockLen)
		return;
	uint8_t len = (uint8_t)iBlockLen;
	Put(&len, 1);
	Put(iBlock, iBlockLen);
	iBlockLen = 0;
	}

void GifWriter::Code(unsigned aCode)
	{
	iBitBuf |= aCode << iBitCount;
	iBitCount += (int)iCodeBits;
	while (iBitCount >= 8)
		{
		Byte((uint8_t)(iBitBuf & 255));
		iBitBuf >>= 8;
		iBitCount -= 8;
		}
	}

void GifWriter::Pixel(uint8_t aV)
	{
	if (iPrefix < 0)
		{
		iPrefix = aV;
		return;
		}
	// is prefix+v in the dictionary?
	for (unsigned c = iFirst[iPrefix]; c; c = iNext[c])
		if (iSym[c] == aV)
			{
			iPrefix = (int)c;
			return;
			}
	// no: output the prefix, add prefix+v, start again from v
	Code((unsigned)iPrefix);
	if (iNextCode < kDictEntries)
		{
		unsigned c = iNextCode++;
		iSym[c] = aV;
		iNext[c] = iFirst[iPrefix];
		iFirst[iPrefix] = (uint16_t)c;
		iFirst[c] = 0;
		// (the code just output was written with the old size; the size
		// grows once the new entry's code needs it)
		if (iNextCode > (1u << iCodeBits) && iCodeBits < 12)
			iCodeBits++;
		}
	else
		{
		Code(kClear);
		ClearDict();
		}
	iPrefix = aV;
	}

void GifWriter::Row(const uint8_t* aPacked)
	{
	if (!iDict)
		return;
	for (int x = 0; x < iWidth; x++)
		Pixel((uint8_t)((aPacked[x >> 1] >> ((x & 1) * 4)) & 15));
	}

void GifWriter::End()
	{
	if (!iDict)
		return;
	if (iPrefix >= 0)
		Code((unsigned)iPrefix);
	Code(kEoi);
	if (iBitCount > 0)
		Byte((uint8_t)(iBitBuf & 255));
	FlushBlock();
	static const uint8_t kTail[] = { 0, 0x3b };   // the block terminator, the trailer
	Put(kTail, 2);
	free(iDict);
	iDict = 0;
	iFirst = iNext = 0;
	iSym = 0;
	iOut = 0;
	}

// ===== the converter ===========================================================

int ImageType(const uint8_t* aHead, size_t aLen)
	{
	return aLen >= 8 ? pmimg_type(aHead, (int)aLen) : 0;
	}

ImageConverter::ImageConverter() : iImage(0), iW(0), iH(0), iSrcW(0), iSrcH(0), iRow(0), iStarted(false) {}

ImageConverter::~ImageConverter()
	{
	Reset();
	}

void ImageConverter::Reset()
	{
	iGif.End();
	if (iImage)
		{
		pmimg_free((PmImage*)iImage);
		free(iImage);
		iImage = 0;
		}
	iW = iH = iSrcW = iSrcH = 0;
	iRow = 0;
	iStarted = false;
	}

bool ImageConverter::Decode(const uint8_t* aData, size_t aLen, int aMaxWidth, char* aWhy, size_t aWhyMax)
	{
	Reset();
	if (aWhyMax)
		aWhy[0] = 0;
	if (aMaxWidth < 16)
		aMaxWidth = 16;
	PmImage* img = (PmImage*)malloc(sizeof(PmImage));
	if (!img)
		{
		snprintf(aWhy, aWhyMax, "no memory");
		return false;
		}
	memset(img, 0, sizeof(*img));
	PmImgOpts o;
	memset(&o, 0, sizeof(o));
	o.max_w = aMaxWidth;
	o.max_h = aMaxWidth * 3;
	o.max_src_pixels = 6L * 1000L * 1000L;    // (a 1/8 decode of this still needs a row of it)
	o.max_full_bytes = 96L * 1024L;           // interlaced files above this are refused (no PSRAM)
#if defined(BOARD_HAS_PSRAM)
	if (psramFound())                         // the AtomS3R: a whole interlaced file fits in PSRAM
		o.max_full_bytes = 2L * 1024L * 1024L;
#endif
	o.exact = 1;                              // full detail at the width asked (the Psion's own
	                                          // decoders settle for half, to save its CPU)
	char err[64];
	int r = pmimg_decode_mem(aData, (long)aLen, &o, img, err, sizeof(err));
	if (r != PMIMG_OK)
		{
		snprintf(aWhy, aWhyMax, "%s", err[0] ? err : "not decoded");
		pmimg_free(img);
		free(img);
		return false;
		}
	iImage = img;
	iW = img->w;
	iH = img->h;
	iSrcW = img->src_w;
	iSrcH = img->src_h;
	iRow = 0;
	return true;
	}

bool ImageConverter::Write(HtmlSink* aOut, int aRows)
	{
	PmImage* img = (PmImage*)iImage;
	if (!img)
		return true;
	if (!iStarted)
		{
		iStarted = true;
		if (!iGif.Begin(aOut, iW, iH))
			return true;                         // (no memory: the caller sees nothing written)
		}
	while (iRow < iH && aRows-- > 0)
		{
		iGif.Row(img->bits + (size_t)iRow * (size_t)img->stride);
		iRow++;
		}
	if (iRow >= iH)
		{
		iGif.End();
		return true;
		}
	return false;
	}

} // namespace am
