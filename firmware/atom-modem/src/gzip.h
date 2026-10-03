// gzip.h - a small streaming gzip encoder for the web proxy's output. The
// serial line to the Psion (about 5500 bytes/s) is the slowest part of a
// page, and PsiWeb (Links) already accepts gzip, so the proxy packs the
// simplified HTML it sends: about a third of the bytes, and Links unpacks
// it far more cheaply than the line would have taken to carry it.
//
// Deflate (RFC 1951) with fixed Huffman codes and greedy LZ77 matching in a
// 4 KB window: 16 KB of RAM in all, against zlib's 256 KB or more. Flush()
// ends the block so far (a "sync flush"), so the Psion can unpack what it
// has while the proxy waits for the server. Plain C++, no Arduino calls.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_GZIP_H
#define ATOM_GZIP_H

#include <stdint.h>
#include <stddef.h>
#include "htmlsimp.h"

namespace am {

uint32_t Crc32(uint32_t aCrc, const uint8_t* aData, size_t aLen);

class GzipWriter
	{
public:
	GzipWriter() : iBuf(0), iHead(0), iOut(0) {}
	~GzipWriter() { End(); }
	bool Begin(HtmlSink* aOut);          // false: no memory (nothing written)
	void Feed(const uint8_t* aData, size_t aLen);
	void Flush();                        // everything so far can be unpacked
	void Finish();                       // the end of the stream (and the trailer)
	void End();                          // frees the memory
	bool Active() const { return iBuf != 0; }
	uint32_t In() const { return iSize; }

	static const size_t kWindow = 4096;
	static const size_t kMaxOut = 1500;  // the most one Feed of 1 KB can write

private:
	void Process(bool aAll);
	void Bits(uint32_t aValue, int aCount);
	void Code(uint32_t aCode, int aLen); // a Huffman code (sent most significant bit first)
	void Literal(uint8_t aC);
	void Match(unsigned aLen, unsigned aDist);
	void Byte(uint8_t aB);
	void Drain();

	uint8_t* iBuf;                       // 2 x kWindow: history, then the bytes to code
	uint16_t* iHead;                     // hash of 3 bytes -> last position + 1
	HtmlSink* iOut;
	size_t iLen, iPos;
	uint32_t iBitBuf;
	int iBitCount;
	uint32_t iCrc, iSize;
	bool iDirty;                         // bytes fed since the last Flush
	uint8_t iStage[256];                 // bytes out, gathered
	size_t iStageLen;
	};

} // namespace am

#endif
