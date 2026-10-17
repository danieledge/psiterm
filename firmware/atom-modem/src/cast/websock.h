// websock.h - the server side of RFC 6455 WebSocket, as small as PsiCast's
// viewer needs (cast.h): the opening handshake (the HTTP request parsed,
// Sec-WebSocket-Accept from SHA-1 and base64), the client's masked frames
// read (fragments joined, control frames between them, a size limit), and
// the server's unmasked frame headers. No sockets here: castnet.cpp moves
// the bytes. Plain C++, no Arduino calls (the host tests run it).
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_WEBSOCK_H
#define ATOM_WEBSOCK_H

#include <stdint.h>
#include <stddef.h>

namespace am {

void Sha1(const uint8_t* aData, size_t aLen, uint8_t aOut[20]);
// base64 of aLen bytes into aOut, NUL-terminated; false if aMax is too small
bool Base64(const uint8_t* aIn, size_t aLen, char* aOut, size_t aMax);
// the Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key (28 characters)
bool WsAcceptKey(const char* aKey, char aOut[29]);

// the request that opens a WebSocket
struct WsRequest
	{
	char path[192];            // "/cast?t=..."
	char key[64];              // Sec-WebSocket-Key
	bool get;                  // a GET
	bool upgrade;              // Upgrade: websocket, and Connection: ... Upgrade
	bool version13;            // Sec-WebSocket-Version: 13
	};
static const size_t kWsHeadMax = 1536;     // a longer request head is refused (Safari's is about 600 bytes)
// 1: a whole head was parsed (aUsed: its length, with the blank line);
// 0: not all there yet; -1: not acceptable (too long, not HTTP)
int WsParseRequest(const char* aBuf, size_t aLen, WsRequest& aOut, size_t& aUsed);
// the value of query parameter aName in aPath ("/cast?t=abc" -> "abc");
// false if it is not there (or does not fit)
bool WsQuery(const char* aPath, const char* aName, char* aOut, size_t aMax);

enum TWsOp { EWsCont = 0, EWsText = 1, EWsBinary = 2, EWsClose = 8, EWsPing = 9, EWsPong = 10 };

// the client's frames: Feed() takes bytes and calls back once for each whole
// message (a text or binary message with its fragments joined, or a control
// frame). It returns how many bytes it took (all of them, unless the callback
// asked it to stop), or -1 on a protocol error (unmasked, a bad opcode, a
// control frame fragmented or over 125 bytes, a message over kMax): the
// connection should then be closed (1002, or 1009 for the size)
class WsReader
	{
public:
	static const size_t kMax = 1024;           // the largest message taken (the viewer sends small JSON)
	typedef void (*TOnMessage)(void* aCtx, int aOp, const uint8_t* aData, size_t aLen);
	WsReader() { Reset(); }
	void Reset();
	int Feed(const uint8_t* aData, size_t aLen, TOnMessage aOn, void* aCtx);
	int CloseCode() const { return iCloseCode; } // what to close with after an error
private:
	void Finish(TOnMessage aOn, void* aCtx);
	enum TState { SHead, SLen, SMask, SData };
	TState iState;
	uint8_t iHead[2];
	uint8_t iLenBytes[8];
	int iLenNeed, iLenHave;
	uint8_t iMask[4];
	int iMaskHave;
	uint64_t iLen, iHave;
	int iOp;                       // this frame's
	bool iFin;
	int iMsgOp;                    // the message being joined (0 none)
	size_t iMsgLen;
	uint8_t iMsg[kMax];
	uint8_t iCtl[125];             // a control frame's payload
	int iCloseCode;
	};

// a server frame header (unmasked, FIN set) for a payload of aLen bytes:
// writes 2, 4 or 10 bytes and returns how many
size_t WsHeader(uint8_t* aOut, int aOp, uint32_t aLen);

} // namespace am

#endif
