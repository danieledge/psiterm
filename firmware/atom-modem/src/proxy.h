// proxy.h - the Atom modem's built-in web proxy for PsiWeb. When the Psion
// dials "psiproxy" (ATDT psiproxy:8080) the modem opens no TCP connection:
// it answers CONNECT and is itself an HTTP proxy on the line. PsiWeb (with
// Preferences > Use a proxy: psiproxy, 8080) sends it every request as plain
// HTTP with an absolute address, https:// ones too. The proxy:
//   - fetches the page itself, over TLS for https:// (so the 36 MHz Psion
//     does no TLS at all), following redirects;
//   - unpacks gzip/deflate if a server sends it;
//   - runs HTML through HtmlSimplifier (htmlsimp.h) as it streams, so the
//     Psion gets small, plain HTML that Links lays out quickly;
//   - packs that HTML with gzip (gzip.h) when PsiWeb accepts it (Links
//     does), so it takes a third of the time on the serial line;
//   - with AT$PI=1, turns pictures into small 16-grey GIFs (imgconv.h);
//   - passes other files through unchanged;
//   - keeps the call up between requests (keep-alive), so PsiWeb dials once
//     for any number of pages and sites.
// Its output goes into the modem's ring, which is paced to the Psion as
// every other connection is; when the ring is nearly full the proxy stops
// reading from the server, and TCP holds the server back.
// Plain C++, no Arduino calls (the Atom and the host tests run this code).
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_PROXY_H
#define ATOM_PROXY_H

#include "htmlsimp.h"
#include "gzip.h"
#include "imgconv.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace am {

class Hal;
class Ring;

// raw deflate, with the gzip or zlib header skipped by the caller: zlib on
// a PC, the ESP32 ROM's tinfl on the Atom (32 KB window, allocated only
// while a compressed page is being read)
class Inflater
	{
public:
	Inflater();
	~Inflater();
	// false: no memory. aZlib: a zlib header first ("deflate"), else raw
	bool Begin(bool aZlib);
	// 1 the stream ended, 0 more to come, -1 bad data
	int Run(const uint8_t* aIn, size_t aInLen, size_t& aUsed, const uint8_t*& aOut, size_t& aOutLen);
	void End();
private:
	void* iState;
	uint8_t* iBuf;
	size_t iOfs;
	int iFlags;
	};

class Proxy : public HtmlSink
	{
public:
	enum TMode { EOff = 0, EOn = 1, EText = 2, ERaw = 3, EReader = 4 };   // AT$PX
	static const char* const kName;      // "psiproxy"

	explicit Proxy(Hal& aHal);
	// the Psion has dialled psiproxy; aZip: gzip the simplified HTML (AT$PZ);
	// aImgWidth: pictures to 16 greys at most this wide (0: unchanged), if
	// the file is at most aImgMaxKB
	void Start(int aMode, bool aZip, int aImgWidth = 0, int aImgMaxKB = 64);
	void Stop();                         // the call has ended: close upstream
	size_t FromPsion(const uint8_t* aData, size_t aLen);   // bytes taken
	// moves things on; output goes into aOut. False once the call should
	// end (the Psion asked for "Connection: close" and has its answer)
	bool Pump(Ring& aOut);
	// the room aOut must have before the proxy will add to it
	static const size_t kSlack = 12 * 1024;

	// statistics (shown at http://psiproxy/ and by ATI)
	uint32_t Requests() const { return iRequests; }
	uint32_t Fetched() const { return iFetched; }      // body bytes from servers
	uint32_t Sent() const { return iSent; }            // bytes to the Psion
	uint32_t Pictures() const { return iPictures; }    // pictures converted

	void Put(const char* aData, size_t aLen) override; // (HtmlSink: simplified HTML, or a GIF)
	void Wire(const char* aData, size_t aLen);          // body bytes as they go on the line

private:
	enum TState { SIdle, SConnect, SHead, SBody, SPicture, SClosed };
	enum TChunk { CSize, CExt, CSizeLf, CData, CDataCr, CDataLf, CTrailer, CDone };
	bool TakeRequest();
	void NextRequest();
	bool ReadHead();
	bool ParseUrl(const char* aUrl);
	bool Resolve(const char* aRef, char* aOut, size_t aMax);
	void Upgrade(const char* aHost);
	bool Upgraded(const char* aHost) const;
	bool ConnectAndSend();
	bool Send(const char* aData, size_t aLen);
	void HeadLine(char* aLine);
	bool HeadDone();
	void SendHead();
	void Body();
	size_t BodySpan(const uint8_t*& aP);
	void Deliver(const uint8_t* aP, size_t aLen);
	void FinishResponse();
	void FinishBody();
	void RawOut(const char* aData, size_t aLen);
	void FlushChunk();
	void ErrorPage(int aCode, const char* aWhat, const char* aDetail);
	void StatusPage();
	void CloseUp();
	bool GzipHeader(const uint8_t*& aP, size_t& aN, size_t& aUsed);
	// pictures
	bool PictureStart();
	void PictureBytes(const uint8_t* aP, size_t aLen);
	void PicturePass();
	void PictureDone();
	void PictureOut();
	void PictureFree();

	Hal& iHal;
	Ring* iOut;
	int iMode;
	TState iState;
	uint32_t iRequests, iFetched, iSent, iPictures;
	// the request from the Psion
	char iReq[4096];
	size_t iReqLen;
	size_t iReqUsed;                     // bytes of iReq the current request takes
	char iMethod[8];
	char iUrl[1024];                     // the address being fetched (after redirects)
	char iHost[128];
	uint16_t iPort;
	bool iTls;
	const char* iPath;                   // in iUrl
	char iUpHdrs[2048];                  // the Psion's headers passed upstream
	size_t iUpHdrsLen;
	const char* iBody;                   // a POST body (in iReq)
	size_t iBodyLen;
	bool iPsionClose;                    // the Psion wants the call to end after this
	bool iPsionGzip;                     // ... and accepts gzip
	bool iOverflow;                      // the ring to the Psion overflowed (the call ends)
	bool iZip;                           // AT$PZ: gzip what the proxy simplifies
	int iRedirects;
	bool iRedirected;
	uint32_t iStartMs;                   // this request: when it came, and the
	uint32_t iFetched0, iSent0;          // counts then (for the log line)
	// the connection upstream
	bool iUpOpen;
	char iUpHost[128];
	uint16_t iUpPort;
	bool iUpTls;
	bool iUpReused;
	bool iUpClose;                       // the server said Connection: close
	uint32_t iLastUpMs;
	char iWhy[96];
	// the response
	uint8_t iIn[1460];
	size_t iInPos, iInLen;
	char iLine[1024];
	size_t iLineLen;
	bool iLineLong;
	int iStatus;
	char iReason[48];
	char iType[128];
	char iLoc[1024];
	char iFwd[2048];                     // the server's headers passed to the Psion
	size_t iFwdLen;
	long iLength;                        // Content-Length, -1 unknown
	bool iChunked;
	int iEncoding;                       // 0 none, 1 gzip, 2 deflate, 3 other
	bool iGotHead;                       // any byte of a response yet
	size_t iHeadBytes;                   // the size of the response head so far
	// the body
	bool iHasBody;
	bool iTransform;                     // HTML through the simplifier
	bool iInflate;
	bool iOutChunked;                    // to the Psion
	long iLeft;                          // body bytes still to come (length known)
	TChunk iChunkState;
	unsigned long iChunkLeft;
	int iGz;                             // gzip header: bytes still to skip, or a state
	int iGzFlags;
	unsigned iChunkLeftGz;               // gzip FEXTRA bytes to skip
	int iGzStep;
	bool iInflateStarted;
	bool iInflateDone;
	const uint8_t* iInfPend;             // unpacked bytes not yet simplified (in iInf's window)
	size_t iInfPendLen;
	bool iInfBad;                        // ... and the data was damaged after them
	bool iBodyDone;
	HtmlSimplifier iSimp;
	Inflater iInf;
	GzipWriter iGzw;
	bool iGzOut;                         // the body to the Psion is gzipped
	struct TWire : public HtmlSink       // (the gzip writer's output)
		{
		Proxy* iP;
		void Put(const char* aData, size_t aLen) override { iP->Wire(aData, aLen); }
		} iWireSink;
	struct GifBuf : public HtmlSink      // gathers the whole GIF, so it goes with a Content-Length
		{
		GifBuf(uint8_t* aB, size_t aCap) : iB(aB), iLen(0), iCap(aCap) {}
		void Put(const char* aData, size_t aLen) override
			{ if (iLen + aLen <= iCap) { memcpy(iB + iLen, aData, aLen); iLen += aLen; } }
		uint8_t* iB;
		size_t iLen, iCap;
		};
	char iChunk[1024];
	size_t iChunkLen;
	char iScratch[1100];                 // (big temporaries off the stack: the
	                                     // Atom's loop task has 8-16 KB)
	// hosts that answered http:// with a redirect to https:// (asked for
	// with https:// at once next time, which saves a round trip)
	char iUpgrades[8][48];
	int iUpgradeNext;
	// pictures (AT$PI): the file is gathered whole, then decoded and sent
	// as a GIF while the ring has room
	int iImgWidth;                       // 0: off
	size_t iImgMax;                      // bytes
	bool iImgMode;                       // this response is a picture being gathered
	uint8_t* iImgBuf;
	size_t iImgLen, iImgCap;
	ImageConverter iImg;
	uint32_t iImgMs;
	uint8_t* iGifBuf;                    // the whole GIF, buffered so it goes with a Content-Length
	size_t iGifLen, iGifPos;             // (PsiWeb's Links fetch mishandles a chunked image body)
	};

} // namespace am

#endif
