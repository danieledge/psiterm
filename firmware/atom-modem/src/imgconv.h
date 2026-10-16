// imgconv.h - pictures to 16 greys for the Psion, on the Atom. A JPEG, PNG
// or GIF that the web proxy fetched is decoded with PsiMail's own decoders
// (mail/engine/img: picojpeg, a progressive JPEG decoder, PNG through
// zlib's inflate, GIF), shrunk to fit a width, dithered to the 16 greys,
// and written out as a 4-bit GIF a row at a time. Links on the Psion
// decodes a small GIF in a fraction of the time a JPEG takes, and the file
// is a fraction of the size on the line.
// Plain C++, no Arduino calls (the Atom and the host tests run this code).
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_IMGCONV_H
#define ATOM_IMGCONV_H

#include <stdint.h>
#include <stddef.h>
#include "htmlsimp.h"

namespace am {

// ----- a GIF writer: 16 greys, LZW, one row at a time ------------------------
class GifWriter
	{
public:
	GifWriter();
	~GifWriter();
	// false: no memory. The header and the colour table go out at once
	bool Begin(HtmlSink* aOut, int aWidth, int aHeight);
	// one row of pixels, two a byte, the left one in the low nibble (0 black
	// .. 15 white: the layout pmimg produces and EPOC's EGray16 uses)
	void Row(const uint8_t* aPacked);
	void End();                          // the last codes, the trailer; frees the memory
	bool Active() const { return iDict != 0; }
	uint32_t Written() const { return iWritten; }
	static const size_t kDictEntries = 4096;

private:
	void Pixel(uint8_t aV);
	void Code(unsigned aCode);
	void Byte(uint8_t aB);
	void FlushBlock();
	void ClearDict();
	void Put(const uint8_t* aData, size_t aLen);

	HtmlSink* iOut;
	int iWidth, iHeight;
	// the dictionary: each code's children as a list (an alphabet of 16, so
	// the lists are short)
	uint16_t* iFirst;                    // first child of a code, 0 = none
	uint16_t* iNext;                     // next sibling
	uint8_t* iSym;                       // the child's symbol
	void* iDict;                         // (the one allocation holding the three)
	unsigned iNextCode;
	unsigned iCodeBits;
	int iPrefix;                         // the current string's code, -1 at the start
	uint32_t iBitBuf;
	int iBitCount;
	uint8_t iBlock[256];
	size_t iBlockLen;
	uint32_t iWritten;
	};

// ----- a picture converted, resumably ------------------------------------------
class ImageConverter
	{
public:
	ImageConverter();
	~ImageConverter();
	// decodes aData (a whole JPEG, PNG or GIF) to at most aMaxWidth pixels
	// wide (and 3 times that high). False: not converted (why in aWhy): the
	// caller passes the file on unchanged
	bool Decode(const uint8_t* aData, size_t aLen, int aMaxWidth, char* aWhy, size_t aWhyMax);
	int Width() const { return iW; }
	int Height() const { return iH; }
	int SourceWidth() const { return iSrcW; }
	int SourceHeight() const { return iSrcH; }
	// writes the GIF to aOut: the head first, then up to aRows rows a call
	// (so the caller can wait for room); true once it is complete
	bool Write(HtmlSink* aOut, int aRows);
	uint32_t Written() const { return iGif.Written(); }
	void Reset();                        // frees everything

private:
	void* iImage;                        // a PmImage
	int iW, iH, iSrcW, iSrcH;
	int iRow;
	bool iStarted;
	GifWriter iGif;
	};

// what the first bytes say: 1 JPEG, 2 PNG, 3 GIF, 0 neither
int ImageType(const uint8_t* aHead, size_t aLen);

} // namespace am

#endif
