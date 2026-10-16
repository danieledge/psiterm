// imapxcode.h - "Layer 2": an IMAP-aware filter for the Atom's TLS relay on
// the PPP route (port 993). It sits between PsiMail (plaintext, over PPP) and
// the upstream IMAPS server and transcodes EMBEDDED image MIME parts to small
// 16-grey GIFs before they cross the ~5.5 KB/s serial link. PsiMail fetches a
// message part-by-part: first UID FETCH (ENVELOPE BODYSTRUCTURE), then each
// part as UID FETCH BODY.PEEK[section]<off.len>. This filter watches the
// BODYSTRUCTURE reply, pre-fetches each image part from upstream itself,
// transcodes it, caches the GIF, and rewrites that part's size/type/encoding
// in the BODYSTRUCTURE so the Psion downloads the small GIF automatically; it
// then serves the part fetches straight from the cache.
//
// Everything it is not transcoding passes through BYTE-FOR-BYTE, and any
// uncertainty (an unparseable frame, a decode/transcode failure, no memory, a
// cache miss) drops the whole session to raw pass-through for good - so the
// dumb-relay behaviour is always the floor and email cannot be made worse.
//
// It is a resumable byte-in / byte-out state machine: the caller (ppptls.cpp)
// owns the two sockets and shuttles bytes through Push*/Pull*. Because a
// BODYSTRUCTURE reply makes the filter run its own fetches to upstream, the
// caller must loop push/pull both ways until quiet. Pure C++, no Arduino calls
// (the Atom and the host tests run this code).
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_IMAPXCODE_H
#define ATOM_IMAPXCODE_H

#include <stdint.h>
#include <stddef.h>

namespace am {

// an optional log sink (the "relay: xcode ..." lines); the caller wires it to
// the modem log. Pure virtual so the module stays Arduino-free.
class XcodeLog
	{
public:
	virtual ~XcodeLog() {}
	virtual void XLog(const char* aLine) = 0;
	};

class ImapXcode
	{
public:
	ImapXcode();
	~ImapXcode();

	// aMaxWidth: transcode target width (as the web proxy's AT$PIW); aMaxSrcKB:
	// skip (pass through) a source image bigger than this; aLog: optional.
	// Returns false if the working buffers could not be allocated - the caller
	// must then fall back to the plain relay for this session.
	bool Begin(int aMaxWidth, int aMaxSrcKB, XcodeLog* aLog);
	void End();                                  // frees everything

	// Feed bytes that arrived from the Psion (the IMAP client) / from the
	// upstream server. Returns how many were consumed; < aLen means the
	// internal buffers are full and the caller should pull, then retry the rest.
	size_t PushClient(const uint8_t* aData, size_t aLen);
	size_t PushServer(const uint8_t* aData, size_t aLen);

	// Pull bytes the filter wants to send onward: ToPsion carries server
	// responses (possibly rewritten or served from cache); ToServer carries the
	// client's commands plus the filter's own injected fetches. Return the byte
	// count (0 = nothing ready).
	size_t PullToPsion(uint8_t* aOut, size_t aMax);
	size_t PullToServer(uint8_t* aOut, size_t aMax);

	// True once the filter has given up and is a transparent pass-through for
	// the rest of the session (the caller may keep using it; it just relays).
	bool PassThrough() const { return iState == EFail; }

	// Which direction can take bytes right now, so the relay pump only reads a
	// socket when the filter will actually consume (avoids a socket holdover).
	bool WantsClient() const { return iState == EClientLine || iState == EFail; }
	bool WantsServer() const
		{ return iState == EServerPass || iState == EServerCapture || iState == EFail
		      || (iState == ETranscode && iInjSent); }

	// stats (for the log/tests)
	uint32_t Transcoded() const { return iNImages; }   // image parts rewritten
	uint32_t SavedBytes() const { return iSaved; }     // original minus GIF, summed

	// limits
	static const int kMaxImages = 8;             // cached transcoded parts per message
	static const size_t kSectionMax = 24;        // "1.2.3" section ids
	static const size_t kLineMax = 1024;         // a framed IMAP line (not a literal body)

private:
	enum TState
		{
		EClientLine,      // accumulating a client command line (to classify it)
		EServerPass,      // framing a server response, forwarding verbatim
		EServerCapture,   // buffering a BODYSTRUCTURE response to parse/rewrite
		ETranscode,       // running our own BODY.PEEK fetches for the images
		EServeCache,      // emitting a part response from the cache (no upstream)
		EFail             // transparent pass-through for the rest of the session
		};

	struct Cached
		{
		uint32_t uid;
		uint32_t seq;                            // the * <seq> FETCH this uid answered with
		char section[kSectionMax];
		uint8_t* b64;                            // the transcoded GIF, base64 (PSRAM)
		size_t b64len;
		uint32_t origSize;                       // the part's original size (for the log/stats)
		};

	// an IMAGE leaf found in a BODYSTRUCTURE: its section id and the raw byte
	// spans (into iResp) of the subtype, encoding and size tokens to rewrite.
	struct XLeaf
		{
		char section[kSectionMax];
		int enc;                                 // EIX_* decoded-from-name
		size_t subS, subE;                       // subtype token  (e.g. "jpeg")
		size_t encS, encE;                       // encoding token (e.g. "base64")
		size_t szS, szE;                         // size number token
		uint32_t origSize;
		};

	// ... the state machine, framing helpers, BODYSTRUCTURE parse/rewrite,
	// transcode-and-cache and cache lookup live in imapxcode.cpp; the data they
	// need is below.
	void Fail(const char* aWhy);                 // -> EFail, logs once

	// helpers (imapxcode.cpp)
	bool Alloc();
	void ClearCache();
	void ResetFrame(const char* aTag);
	void FrameByte(uint8_t aB, bool aInject, bool* aDone);
	bool LiteralAtEnd(size_t* aN) const;
	void OnClientLine();
	void ForwardCmd(TState aNext);
	void OnCaptureDone();
	void StartInject();
	void OnInjectDone();
	bool ParseStructure();
	bool WalkStruct(const uint8_t* aB, size_t aP, size_t aEnd, const char* aPrefix, int aDepth);
	void RewriteAndEmit();
	void ServeCache(int aCi, size_t aOff, size_t aLen);
	int  CacheFind(uint32_t aUid, const char* aSec) const;

	TState iState;
	XcodeLog* iLog;
	int iMaxWidth;
	size_t iMaxSrc;

	// output rings (drained by Pull*), and the line/response work buffers.
	// head is always index 0: Pull copies then compacts what is left down.
	uint8_t* iToPsion; size_t iToPsionLen, iToPsionCap;
	uint8_t* iToServer; size_t iToServerLen, iToServerCap;
	uint8_t* iResp; size_t iRespLen, iRespCap;   // a captured server response (PSRAM)
	char iLine[kLineMax]; size_t iLineLen;       // the client command being read

	// our own injected BODY.PEEK fetch and its framed response
	uint8_t* iPart; size_t iPartLen, iPartCap;
	size_t iPartBodyOff, iPartBodyLen;           // the part body literal within iPart
	bool iPartTooBig;                            // its literal was over the limit: skip + fail
	bool iInjDrain;                              // an inject response must be drained, then Fail (R1)
	uint8_t* iDec; size_t iDecCap;               // base64/QP-decoded source image

	// the IMAGE leaves of the message being transcoded
	XLeaf iLeaf[kMaxImages];
	int iNImg;
	int iImgIdx;
	bool iInjSent;
	int iInjNo;                                  // X%03d tag counter

	// framing (one server response at a time)
	char iTag[16];                               // the client command's tag
	char iInjTag[16];                            // our injected fetch's tag
	char iMatchTag[16];                          // the tag whose completion ends this response
	int iFrPhase;                                // 0 = line, 1 = literal body
	size_t iFrLitLeft;                           // literal bytes still to read
	bool iFrGotBody;                             // the part body literal has been seen (inject)
	char iFrHead[24]; size_t iFrHeadLen;         // the current line's start (for the tag)
	char iFrTail[48]; size_t iFrTailLen;         // ... and its tail (for a trailing {n})

	Cached iCache[kMaxImages];
	int iNCached;
	uint32_t iSeqForUid;                         // seq learned from the last FETCH response
	uint32_t iUidSeq;                            // ... its uid

	uint32_t iNImages;
	uint32_t iSaved;
	};

} // namespace am

#endif
