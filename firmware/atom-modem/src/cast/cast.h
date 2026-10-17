// cast.h - PsiCast on the Atom: the Psion's screen, live in a web browser
// (cast/docs/PROTOCOL.md). The Psion's psicast.exe connects to the Atom's
// own address, 192.168.7.1:5961 (link A), over the PPP link or by dialling
// ATDT192.168.7.1:5961 on the modem route, and sends its screen as
// compressed updates (cast/codec). The Atom decodes them into its own copy
// of the screen and serves it to up to four browsers over WebSocket on port
// 5962 (link B), each at its own pace; the viewer page is /screen on the web
// UI. Taps and keys from a viewer go back to the Psion, if it allows them.
//
//   link A (one Psion): framing with a CRC and resync, WELCOME (the largest
//   UPDATE this board's buffer takes, and the viewer's address), a cumulative
//   ACK for every UPDATE taken (so the Psion's window of 2 always moves),
//   KEYREQ after a framing or decode error (and again every 3 s until a
//   keyframe comes), PING with the last seq taken when idle and after a
//   discarded update, closed after 20 s of silence, VIEWERS when the count
//   changes, DEVINFO (the digitiser) passed to the viewers, INPUT from them.
//   link B (each viewer): a dirty map of 16x8 tiles. Every change the decoder
//   reports marks every viewer's map; a viewer that can take more is sent
//   the dirty area as RECTS, read from the screen as the bytes go, so a slow
//   viewer gets one merged update rather than a backlog, and never holds up
//   link A. A move is sent as SCROLL when it can be (the map is moved with
//   it; SCROLL carries the columns that moved after PROTOCOL.md's fields, an
//   extension); a new viewer gets FULL first.
//   tokens: GET /screen/token (behind the web password) gives a single-use
//   token, good for 60 s, that the page's WebSocket must carry (?t=...);
//   "control" is asked for there and needs the Psion's permission as well.
//
// No sockets here (castnet.cpp moves the bytes for both links; the modem
// route feeds link A from Modem itself). Plain C++, no Arduino calls.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_CAST_H
#define ATOM_CAST_H

#include <stdint.h>
#include <stddef.h>
#include "websock.h"
#include "pccodec.h"        // cast/codec (the decoder)

namespace am {

class Hal;

static const uint16_t kCastPort = 5961;          // link A, on 192.168.7.1
static const uint16_t kCastViewPort = 5962;      // link B, the WebSocket
static const int kCastViewers = 4;

// the link A message types (PROTOCOL.md)
enum TCastType
	{
	ECastHello = 0x01, ECastUpdate = 0x02, ECastStatus = 0x03, ECastBye = 0x04, ECastDevInfo = 0x05,
	ECastWelcome = 0x81, ECastAck = 0x82, ECastKeyReq = 0x83, ECastViewers = 0x84, ECastInput = 0x85, ECastPing = 0x86
	};
// link B's binary messages
enum TCastView { ECastFull = 0x01, ECastRects = 0x02, ECastScroll = 0x03 };

uint32_t CastCrc(const uint8_t* aData, size_t aLen);     // zlib's crc32
// one whole link A message into aOut (header, payload, CRC); returns its
// length, or 0 if aMax is too small
size_t CastFrame(uint8_t* aOut, size_t aMax, int aType, int aFlags, const uint8_t* aPayload, size_t aLen);

// Link A's receive side: bytes in, whole checked messages out. A bad magic,
// a length over 65536 or a bad CRC drops one byte and looks for the next
// 'P','C' (resync); each run of dropped bytes counts as one error
class CastRx
	{
public:
	static const size_t kMaxPayload = 65536;
	CastRx() : iBuf(0), iSize(0), iLen(0), iErrors(0), iInError(false), iTaken(0) {}
	void Init(uint8_t* aBuf, size_t aSize) { iBuf = aBuf; iSize = aSize; iLen = 0; iInError = false; iTaken = 0; iSkip = 0; }
	size_t Room() const { return iSize - iLen; }
	size_t Feed(const uint8_t* aData, size_t aLen);
	// 1: a message (valid until the next call), 0: none whole yet
	int Next(uint8_t& aType, uint8_t& aFlags, const uint8_t*& aPayload, uint32_t& aLen);
	uint32_t Errors() const { return iErrors; }
	void ClearErrors() { iErrors = 0; }
	// a message longer than the buffer: skipped (counted), never applied
	uint32_t Oversize() const { return iOversize; }
	// part of a message is in (a 'P','C' and more, not yet whole)
	bool MidMessage() const { return iLen >= 2 && iBuf[0] == 'P' && iBuf[1] == 'C'; }
private:
	void Drop(size_t aN);
	uint8_t* iBuf;
	size_t iSize, iLen;
	uint32_t iErrors;
	bool iInError;
	size_t iTaken;                  // the message returned last (dropped at the next call)
	uint32_t iOversize = 0;
	uint32_t iSkip = 0;             // bytes of an oversize message still to skip
	};

// a token for the viewer page (GET /screen/token)
struct CastToken
	{
	char text[25];                  // 24 hex digits, "" = free
	bool control;
	uint32_t issuedMs;
	};

// one viewer: its dirty map and what it is being sent
struct CastViewer
	{
	static const int kTileW = 16, kTileH = 8;
	static const int kTilesX = 640 / kTileW, kTilesY = 480 / kTileH;
	static const int kMaxRects = 48;
	struct Rect { uint16_t x, y, w, h; };
	bool used;
	bool control;                   // it is controlling now (its token allowed it, and the page has not turned it off)
	bool mayControl;                // its token was issued with control
	uint32_t openedMs, lastInMs, lastStatusMs;
	bool needFull;                  // FULL before anything else
	bool needHello;
	bool needStatus;
	bool needBye;
	uint8_t dirty[kTilesY][(kTilesX + 7) / 8];
	// the message on its way (FULL or RECTS): its frame and message headers,
	// then the rects' rows read from the screen as they go
	int inType;                     // 0: none
	uint8_t head[16 + 2 + kMaxRects * 8];
	size_t headLen, headPos;
	Rect rect[kMaxRects];
	int nRect, curRect, curRow;
	size_t curOff;                  // bytes of the current row already out
	bool zeroFill;                  // the screen went (a new size): the rest of the message is zeros
	// whole small frames, in order: SCROLLs, JSON, pongs, a close. They go
	// out between the big messages, never inside one
	uint8_t small[1536];
	size_t smallLen, smallPos;
	bool closing;                   // a close frame is queued: then the connection ends
	uint32_t pingN;                 // (stats)
	uint32_t bytesOut, rateBytes;   // to this viewer, and per second
	uint32_t rateMark, rateMarkMs;
	WsReader reader;
	};

class Cast
	{
public:
	explicit Cast(Hal& aHal);
	~Cast();
	void SetEnabled(bool aOn);
	bool Enabled() const { return iEnabled; }
	void Tick();                    // from the main loop: timers, status, input to the Psion

	// ----- link A (the transports: Modem for the modem route, castnet for TCP)
	enum TRoute { ERouteModem = 1, ERouteTcp = 2 };
	// a new connection from the Psion: it replaces any other (the Psion has
	// come back). Returns its id, or 0 if refused (PsiCast off, no memory)
	uint32_t LinkOpen(int aRoute);
	size_t LinkIn(uint32_t aId, const uint8_t* aData, size_t aLen);
	size_t LinkOut(uint32_t aId, uint8_t* aOut, size_t aMax);
	// false: this link is finished with (replaced, BYE, silent for 20 s, an
	// error): the transport closes it once LinkOut has nothing more
	bool LinkAlive(uint32_t aId) const { return aId && aId == iLinkId && !iLinkEnding; }
	void LinkClose(uint32_t aId);   // the transport's end went away
	// the link is part-way through a message from the Psion (the modem route:
	// "+++" inside one is data, never the escape)
	bool LinkMidMessage(uint32_t aId) const { return aId && aId == iLinkId && iRx.MidMessage(); }

	// ----- link B (castnet: the WebSocket connections, after the handshake)
	// a token from the page: a viewer slot, or -1 (aWhy: a few words)
	int ViewerOpen(const char* aToken, char* aWhy, size_t aWhyMax);
	// bytes from the browser (WebSocket frames); false: close it now
	bool ViewerIn(int aIdx, const uint8_t* aData, size_t aLen);
	// WebSocket bytes for the browser, at most aMax
	size_t ViewerOut(int aIdx, uint8_t* aOut, size_t aMax);
	// true once everything for a closing viewer is out: close the socket
	bool ViewerDone(int aIdx) const;
	void ViewerClose(int aIdx);

	// ----- the page
	// a new token (single use, 60 s); false if PsiCast is off
	bool NewToken(bool aControl, char* aOut, size_t aMax);
	static const uint32_t kTokenMs = 60 * 1000;

	// ----- what the pages and the LCD show
	bool LinkUp() const { return iLinkId != 0 && iWelcomed; }
	int Route() const { return iLinkId ? iRoute : 0; }
	int Viewers() const;
	int ViewersWithControl() const;
	bool ControlAllowed() const { return iCtlAllowed; }
	bool Paused() const { return iPaused; }
	const char* PsionName() const { return iName; }
	uint32_t Updates() const { return iUpdates; }
	uint32_t Errors() const { return iRx.Errors() + iDecodeErrors; }
	uint32_t RateBytes() const { return iRateBytes; }
	int Battery() const { return iBattery; }
	int DigitiserW() const { return iDigW; }       // (DEVINFO: 0 not known)
	int DigitiserH() const { return iDigH; }
	// "Sharing: Psion (modem), 2 viewers" and the like
	void StateText(char* aOut, size_t aMax) const;
	const unsigned char* Screen() const;
	int Width() const { return iW; }
	int Height() const { return iH; }

	// timings (the tests use them)
	static const uint32_t kSilenceMs = 20000;      // link A closed after this long with nothing from the Psion
	static const uint32_t kPingMs = 5000;          // PING when nothing has gone to the Psion for this long
	static const uint32_t kKeyRetryMs = 3000;      // KEYREQ again while no keyframe has come
	static const uint32_t kViewerIdleMs = 45000;   // a viewer that has sent nothing this long is closed
	static const uint32_t kStatusMs = 1000;        // status JSON to the viewers at most this often

private:
	friend struct CastTest;
	void Handle(int aType, int aFlags, const uint8_t* aP, uint32_t aN);
	void OnHello(const uint8_t* aP, uint32_t aN);
	void OnUpdate(int aFlags, const uint8_t* aP, uint32_t aN);
	void OnStatus(const uint8_t* aP, uint32_t aN);
	void LinkEnd(const char* aWhy);
	void FreeLink();
	bool Send(int aType, const uint8_t* aP, size_t aN);   // into the link's out queue
	void SendViewers();
	void KeyReq();
	void Ping();
	bool EnsureScreen(int aW, int aH);
	void FreeScreen();
	// the decoder's changes, to every viewer
	static void OnScrollCb(void* aCtx, const pc_scroll* aS);
	static void OnRectCb(void* aCtx, const pc_rect* aR);
	void MarkRect(CastViewer& aV, int aX, int aY, int aW, int aH);
	void MarkAll(CastViewer& aV);
	void ScrollViewer(CastViewer& aV, int aY0, int aY1, int aDy, int aX, int aW);
	// a viewer's messages
	bool BuildRects(CastViewer& aV);
	void StartFull(CastViewer& aV);
	size_t Stream(CastViewer& aV, uint8_t* aOut, size_t aMax);
	void QueueText(CastViewer& aV, const char* aJson);
	void QueueCtl(CastViewer& aV, int aOp, const uint8_t* aP, size_t aN);
	void QueueStatus(CastViewer& aV);
	void QueueHello(CastViewer& aV);
	static void OnWsMessage(void* aCtx, int aOp, const uint8_t* aData, size_t aLen);
	void ViewerMessage(CastViewer& aV, int aOp, const uint8_t* aData, size_t aLen);
	void ViewerText(CastViewer& aV, const char* aJson);
	void Input(int aKind, int aA, int aB);
	void ViewersChanged();

	Hal& iHal;
	bool iEnabled;
	// link A
	uint32_t iLinkId, iNextId;
	int iRoute;
	bool iLinkEnding;
	bool iWelcomed;                 // HELLO taken, WELCOME sent
	CastRx iRx;
	uint8_t* iRxBuf;
	size_t iRxSize;
	uint8_t iOut[4096];             // to the Psion
	size_t iOutLen;
	uint32_t iLastInMs, iLastOutMs, iPingSentMs, iRttMs;
	uint32_t iPingToken;
	bool iPingWait;
	uint32_t iAckSeq;
	bool iAckDue;
	bool iAwaitKey;
	uint32_t iKeyReqMs;
	uint32_t iLastSeq;
	bool iHaveSeq;
	uint32_t iUpdates, iDecodeErrors;
	uint32_t iBytesIn, iRateBytes, iRateMark, iRateMarkMs;
	uint32_t iUpdMark, iUps10;
	char iName[48];
	bool iCtlAllowed, iPaused, iInputCap;
	int iBattery, iMains, iCpu10, iPsionUps10;
	uint32_t iFreeRam;
	int iDigW, iDigH;
	bool iHaveStatus;
	int iLastViewers, iLastViewersCtl;
	// the input for the Psion, waiting to go
	static const int kInputMax = 128;
	uint8_t iInput[kInputMax][8];
	int iInputN;
	// the screen
	pc_dec* iDec;
	int iW, iH;
	// link B
	CastViewer* iView[kCastViewers];
	CastViewer* iWsCur;             // (OnWsMessage's viewer)
	CastToken iTokens[8];
	};

// a few JSON helpers (flat objects, bounded): the value of "aKey"
bool JsonStr(const char* aJson, const char* aKey, char* aOut, size_t aMax);
bool JsonInt(const char* aJson, const char* aKey, long& aOut);

} // namespace am

#endif
