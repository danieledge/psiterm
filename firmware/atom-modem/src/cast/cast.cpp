// cast.cpp - see cast.h. MIT licence (see LICENSE at the top of the repository).
#include "cast.h"
#include "../modem.h"
#include "pccodec.h"
#include "zlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>

namespace am {

// ----- little-endian fields --------------------------------------------------------

static uint16_t Le16(const uint8_t* aP) { return (uint16_t)(aP[0] | (aP[1] << 8)); }
static uint32_t Le32(const uint8_t* aP) { return (uint32_t)aP[0] | ((uint32_t)aP[1] << 8) | ((uint32_t)aP[2] << 16) | ((uint32_t)aP[3] << 24); }
static void Put16(uint8_t* aP, uint32_t aV) { aP[0] = (uint8_t)aV; aP[1] = (uint8_t)(aV >> 8); }
static void Put32(uint8_t* aP, uint32_t aV) { aP[0] = (uint8_t)aV; aP[1] = (uint8_t)(aV >> 8); aP[2] = (uint8_t)(aV >> 16); aP[3] = (uint8_t)(aV >> 24); }

uint32_t CastCrc(const uint8_t* aData, size_t aLen)
	{
	return (uint32_t)crc32(0L, aData, (uInt)aLen);
	}

size_t CastFrame(uint8_t* aOut, size_t aMax, int aType, int aFlags, const uint8_t* aPayload, size_t aLen)
	{
	if (aLen > CastRx::kMaxPayload || aMax < aLen + 12)
		return 0;
	aOut[0] = 'P';
	aOut[1] = 'C';
	aOut[2] = (uint8_t)aType;
	aOut[3] = (uint8_t)aFlags;
	Put32(aOut + 4, (uint32_t)aLen);
	if (aLen)
		memcpy(aOut + 8, aPayload, aLen);
	Put32(aOut + 8 + aLen, CastCrc(aOut, 8 + aLen));
	return aLen + 12;
	}

// ----- link A: receiving ---------------------------------------------------------------

size_t CastRx::Feed(const uint8_t* aData, size_t aLen)
	{
	size_t used = 0;
	if (iSkip)
		{
		// the rest of a message too long for the buffer: thrown away
		size_t n = aLen < iSkip ? aLen : iSkip;
		iSkip -= (uint32_t)n;
		used = n;
		}
	if (iTaken)
		{
		Drop(iTaken);
		iTaken = 0;
		}
	size_t room = iSize - iLen;
	size_t n = aLen - used < room ? aLen - used : room;
	memcpy(iBuf + iLen, aData + used, n);
	iLen += n;
	return used + n;
	}

void CastRx::Drop(size_t aN)
	{
	if (aN >= iLen)
		{
		iLen = 0;
		return;
		}
	memmove(iBuf, iBuf + aN, iLen - aN);
	iLen -= aN;
	}

int CastRx::Next(uint8_t& aType, uint8_t& aFlags, const uint8_t*& aPayload, uint32_t& aLen)
	{
	if (iTaken)
		{
		Drop(iTaken);
		iTaken = 0;
		}
	// (resync: a bad byte drops itself and the bytes up to the next 'P'. The
	// buffer is only moved once a run is found, so a long run of rubbish costs
	// one move, not one a byte)
	size_t start = 0;
	int result = 0;
	while (iLen - start >= 2)
		{
		const uint8_t* b = iBuf + start;
		if (b[0] != 'P' || b[1] != 'C')
			{
			if (!iInError) { iErrors++; iInError = true; }
			start++;
			while (start < iLen && iBuf[start] != 'P')
				start++;
			continue;
			}
		if (iLen - start < 8)
			break;
		uint32_t n = Le32(b + 4);
		if (n > kMaxPayload)
			{
			if (!iInError) { iErrors++; iInError = true; }
			start++;
			continue;
			}
		if (n + 12 > iSize)
			{
			// longer than this board's buffer: skipped, and counted as an error
			// (the Atom then asks for a keyframe)
			if (!iInError) { iErrors++; iInError = true; }
			iOversize++;
			size_t have = iLen - start;
			iSkip = (uint32_t)(n + 12 - have);
			iLen = 0;
			return 0;
			}
		if (iLen - start < n + 12)
			break;
		if (Le32(b + 8 + n) != CastCrc(b, 8 + n))
			{
			if (!iInError) { iErrors++; iInError = true; }
			start++;
			continue;
			}
		iInError = false;
		if (start)
			{
			Drop(start);
			start = 0;
			b = iBuf;
			}
		aType = b[2];
		aFlags = b[3];
		aPayload = b + 8;
		aLen = n;
		iTaken = n + 12;
		result = 1;
		break;
		}
	if (start)
		Drop(start);
	return result;
	}

// ----- JSON, small and flat --------------------------------------------------------------

static const char* JsonFind(const char* aJson, const char* aKey)
	{
	size_t kl = strlen(aKey);
	for (const char* p = aJson; (p = strchr(p, '"')) != 0; p++)
		{
		if (strncmp(p + 1, aKey, kl) == 0 && p[1 + kl] == '"')
			{
			const char* q = p + 2 + kl;
			while (*q == ' ') q++;
			if (*q != ':')
				continue;
			q++;
			while (*q == ' ') q++;
			return q;
			}
		}
	return 0;
	}

bool JsonStr(const char* aJson, const char* aKey, char* aOut, size_t aMax)
	{
	const char* q = JsonFind(aJson, aKey);
	if (!q || *q != '"' || !aMax)
		return false;
	q++;
	size_t n = 0;
	while (*q && *q != '"')
		{
		if (*q == '\\' && q[1])
			q++;
		if (n + 1 >= aMax)
			return false;
		aOut[n++] = *q++;
		}
	aOut[n] = 0;
	return *q == '"';
	}

bool JsonInt(const char* aJson, const char* aKey, long& aOut)
	{
	const char* q = JsonFind(aJson, aKey);
	if (!q)
		return false;
	bool neg = false;
	if (*q == '-') { neg = true; q++; }
	if (*q < '0' || *q > '9')
		return false;
	long v = 0;
	while (*q >= '0' && *q <= '9' && v < 100000000)
		v = v * 10 + (*q++ - '0');
	aOut = neg ? -v : v;
	return true;
	}

// a name for JSON: printable ASCII and whole UTF-8 sequences, others '?'
static void JsonSafe(const char* aIn, char* aOut, size_t aMax)
	{
	size_t o = 0;
	const unsigned char* p = (const unsigned char*)aIn;
	while (*p && o + 5 < aMax)
		{
		int len = *p < 0x80 ? 1 : (*p & 0xe0) == 0xc0 ? 2 : (*p & 0xf0) == 0xe0 ? 3 : (*p & 0xf8) == 0xf0 ? 4 : 0;
		bool ok = len > 0;
		for (int k = 1; ok && k < len; k++)
			ok = (p[k] & 0xc0) == 0x80;
		if (!ok || (len == 1 && (*p < 32 || *p == '"' || *p == '\\' || *p == 127)))
			{
			aOut[o++] = '?';
			p++;
			continue;
			}
		for (int k = 0; k < len; k++)
			aOut[o++] = (char)*p++;
		}
	aOut[o] = 0;
	}

// ----- the service ---------------------------------------------------------------------------

Cast::Cast(Hal& aHal)
	: iHal(aHal), iEnabled(false), iLinkId(0), iNextId(0), iRoute(0), iLinkEnding(false), iWelcomed(false),
	  iRxBuf(0), iRxSize(0), iOutLen(0), iLastInMs(0), iLastOutMs(0), iPingSentMs(0), iRttMs(0), iPingToken(0),
	  iPingWait(false), iAckSeq(0), iAckDue(false), iAwaitKey(true), iKeyReqMs(0), iLastSeq(0), iHaveSeq(false),
	  iUpdates(0), iDecodeErrors(0), iBytesIn(0), iRateBytes(0), iRateMark(0), iRateMarkMs(0), iUpdMark(0), iUps10(0),
	  iCtlAllowed(false), iPaused(false), iInputCap(false), iBattery(255), iMains(255), iCpu10(65535), iPsionUps10(0),
	  iFreeRam(0), iDigW(0), iDigH(0), iHaveStatus(false), iLastViewers(-1), iLastViewersCtl(-1), iInputN(0), iDec(0), iW(0), iH(0), iWsCur(0)
	{
	iName[0] = 0;
	for (int i = 0; i < kCastViewers; i++)
		iView[i] = 0;
	memset(iTokens, 0, sizeof(iTokens));
	}

Cast::~Cast()
	{
	for (int i = 0; i < kCastViewers; i++)
		{
		delete iView[i];
		iView[i] = 0;
		}
	iHal.BigFree(iRxBuf);
	iRxBuf = 0;
	FreeScreen();
	}

void Cast::SetEnabled(bool aOn)
	{
	if (aOn == iEnabled)
		return;
	iEnabled = aOn;
	if (!aOn)
		{
		if (iLinkId)
			{
			Send(ECastBye, 0, 0);
			LinkEnd("PsiCast was turned off");
			}
		for (int i = 0; i < kCastViewers; i++)
			if (iView[i])
				{
				static const uint8_t kGoing[] = { 0x03, 0xe9 };   // 1001: going away
				QueueCtl(*iView[i], EWsClose, kGoing, 2);
				iView[i]->closing = true;
				}
		memset(iTokens, 0, sizeof(iTokens));
		}
	iHal.Log(aOn ? "PsiCast: on" : "PsiCast: off");
	}

// ----- link A ------------------------------------------------------------------------------------

uint32_t Cast::LinkOpen(int aRoute)
	{
	if (!iEnabled)
		return 0;
	if (iLinkId)
		{
		LinkEnd("a new connection from the Psion");
		FreeLink();
		}
	// the receive buffer: a whole UPDATE (64 KB); a smaller one on a board
	// short of memory, leaving room for the screen copy to come with HELLO
	// (an UPDATE too big for it is skipped, and a keyframe asked for)
	static const size_t kSizes[] = { CastRx::kMaxPayload + 12, 32 * 1024 + 12, 16 * 1024 + 12 };
	size_t screen = iDec ? 0 : 640 * 240 / 2 + 16 * 1024;
	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]) && !iRxBuf; i++)
		{
		if (!iHal.BigRoom(kSizes[i] + screen))
			continue;
		iRxBuf = (uint8_t*)iHal.BigAlloc(kSizes[i]);
		iRxSize = iRxBuf ? kSizes[i] : 0;
		}
	if (!iRxBuf)
		{
		iHal.Log("PsiCast: not enough memory for the Psion's link");
		return 0;
		}
	iRx.Init(iRxBuf, iRxSize);
	iRx.ClearErrors();
	if (++iNextId == 0)
		iNextId = 1;
	iLinkId = iNextId;
	iRoute = aRoute;
	iLinkEnding = false;
	iWelcomed = false;
	iOutLen = 0;
	uint32_t now = iHal.Millis();
	iLastInMs = iLastOutMs = now;
	iPingWait = false;
	iAckDue = false;
	iAwaitKey = true;
	iKeyReqMs = now;
	iHaveSeq = false;
	iInputN = 0;
	iHaveStatus = false;
	iBattery = iMains = 255;
	iCpu10 = 65535;
	iPaused = false;
	iLastViewers = iLastViewersCtl = -1;
	iRateMark = iBytesIn;
	iRateMarkMs = now;
	iUpdMark = iUpdates;
	char m[96];
	snprintf(m, sizeof(m), "PsiCast: the Psion is connecting (%s), buffer %u KB", aRoute == ERouteModem ? "modem route" : "TCP",
		(unsigned)(iRxSize / 1024));
	iHal.Log(m);
	return iLinkId;
	}

void Cast::LinkEnd(const char* aWhy)
	{
	if (!iLinkId || iLinkEnding)
		return;
	iLinkEnding = true;
	char m[120];
	snprintf(m, sizeof(m), "PsiCast: the Psion's link ended: %s (%lu updates, %lu errors)", aWhy,
		(unsigned long)iUpdates, (unsigned long)Errors());
	iHal.Log(m);
	for (int i = 0; i < kCastViewers; i++)
		if (iView[i])
			{
			iView[i]->needBye = true;
			iView[i]->needStatus = true;
			}
	}

void Cast::FreeLink()
	{
	iHal.BigFree(iRxBuf);
	iRxBuf = 0;
	iRxSize = 0;
	iRx.Init(0, 0);
	iLinkId = 0;
	iWelcomed = false;
	iLinkEnding = false;
	iOutLen = 0;
	iInputN = 0;
	if (!Viewers())
		FreeScreen();
	}

void Cast::LinkClose(uint32_t aId)
	{
	if (!aId || aId != iLinkId)
		return;
	LinkEnd("the connection closed");
	FreeLink();
	for (int i = 0; i < kCastViewers; i++)
		if (iView[i])
			iView[i]->needStatus = true;
	}

size_t Cast::LinkIn(uint32_t aId, const uint8_t* aData, size_t aLen)
	{
	if (!aId || aId != iLinkId || iLinkEnding)
		return aLen;                          // (a link that is finished: its bytes go nowhere)
	size_t taken = 0;
	while (taken < aLen && aId == iLinkId && !iLinkEnding)
		{
		size_t n = iRx.Feed(aData + taken, aLen - taken);
		taken += n;
		uint32_t e0 = iRx.Errors();
		uint8_t type, flags;
		const uint8_t* p;
		uint32_t pn;
		while (aId == iLinkId && !iLinkEnding && iRx.Next(type, flags, p, pn))
			{
			iLastInMs = iHal.Millis();
			Handle(type, flags, p, pn);
			}
		if (aId == iLinkId && iRx.Errors() != e0 && iWelcomed)
			{
			iAwaitKey = true;
			KeyReq();
			}
		if (!n)
			break;
		}
	iBytesIn += (uint32_t)taken;
	return taken;
	}

size_t Cast::LinkOut(uint32_t aId, uint8_t* aOut, size_t aMax)
	{
	if (!aId || aId != iLinkId)
		return 0;
	if (iAckDue)
		{
		uint8_t p[4];
		Put32(p, iAckSeq);
		if (Send(ECastAck, p, 4))
			iAckDue = false;
		}
	size_t n = iOutLen < aMax ? iOutLen : aMax;
	memcpy(aOut, iOut, n);
	memmove(iOut, iOut + n, iOutLen - n);
	iOutLen -= n;
	return n;
	}

bool Cast::Send(int aType, const uint8_t* aP, size_t aN)
	{
	size_t n = CastFrame(iOut + iOutLen, sizeof(iOut) - iOutLen, aType, 0, aP, aN);
	if (!n)
		return false;
	iOutLen += n;
	iLastOutMs = iHal.Millis();
	return true;
	}

// PING: a token, and the last UPDATE taken (as ACK: applied or discarded),
// so a Psion whose window may be stalled moves on. When idle, and after an
// update that was discarded
void Cast::Ping()
	{
	uint8_t p[8];
	uint32_t now = iHal.Millis();
	iPingToken = now;
	Put32(p, iPingToken);
	Put32(p + 4, iHaveSeq ? iLastSeq : 0);
	if (Send(ECastPing, p, 8))
		{
		iPingSentMs = now;
		iPingWait = true;
		}
	}

void Cast::KeyReq()
	{
	iAwaitKey = true;
	if (Send(ECastKeyReq, 0, 0))
		iKeyReqMs = iHal.Millis();
	}

void Cast::SendViewers()
	{
	uint8_t p[2];
	int n = Viewers(), c = ViewersWithControl();
	p[0] = (uint8_t)n;
	p[1] = (uint8_t)c;
	if (Send(ECastViewers, p, 2))
		{
		iLastViewers = n;
		iLastViewersCtl = c;
		}
	}

void Cast::Handle(int aType, int aFlags, const uint8_t* aP, uint32_t aN)
	{
	if (aType == ECastHello)
		{
		OnHello(aP, aN);
		return;
		}
	if (!iWelcomed)
		return;                               // (nothing counts before HELLO)
	switch (aType)
		{
	case ECastUpdate: OnUpdate(aFlags, aP, aN); break;
	case ECastStatus: OnStatus(aP, aN); break;
	case ECastBye: LinkEnd("the Psion stopped sharing"); break;
	case ECastDevInfo:
		// the digitiser's size (bigger than the LCD: the silkscreen icons are
		// taps outside it), for the viewer page's icons
		if (aN >= 4)
			{
			iDigW = Le16(aP);
			iDigH = Le16(aP + 2);
			for (int i = 0; i < kCastViewers; i++)
				if (iView[i])
					iView[i]->needHello = true;
			}
		break;
	default: break;                           // (a newer Psion's: ignored)
		}
	}

void Cast::OnHello(const uint8_t* aP, uint32_t aN)
	{
	if (aN < 12)
		return;
	int version = Le16(aP), w = Le16(aP + 2), h = Le16(aP + 4), format = aP[6];
	uint32_t caps = Le32(aP + 8);
	char name[48];
	size_t nl = aN - 12 < sizeof(name) - 1 ? aN - 12 : sizeof(name) - 1;
	memcpy(name, aP + 12, nl);
	name[nl] = 0;
	JsonSafe(name, iName, sizeof(iName));
	char m[140];
	if (version < 1 || format != 0 || w < 2 || w > PC_MAX_W || (w & 1) || h < 1 || h > PC_MAX_H)
		{
		snprintf(m, sizeof(m), "PsiCast: a HELLO we cannot take (version %d, %dx%d, format %d)", version, w, h, format);
		iHal.Log(m);
		Send(ECastBye, 0, 0);
		LinkEnd("not a screen this Atom can show");
		return;
		}
	if (!EnsureScreen(w, h))
		{
		snprintf(m, sizeof(m), "PsiCast: not enough memory for a %dx%d screen", w, h);
		iHal.Log(m);
		Send(ECastBye, 0, 0);
		LinkEnd("not enough memory");
		return;
		}
	iInputCap = (caps & 1) != 0;
	iCtlAllowed = (caps & 2) != 0;
	iDigW = iDigH = 0;                        // (DEVINFO may follow)
	// WELCOME: version 1, a window of 2, viewers' input passed on, the
	// largest UPDATE payload this buffer takes; then the viewer page's address
	uint8_t p[12 + 64];
	Put16(p, 1);
	Put16(p + 2, 2);
	Put32(p + 4, 1);
	Put32(p + 8, (uint32_t)(iRxSize - 12));
	size_t n = 12;
	char url[64];
	iHal.WebInfo(url, sizeof(url));          // "http://192.168.1.50/" (and maybe a note after a space)
	char* sp = strchr(url, ' ');
	if (sp)
		*sp = 0;
	size_t ul = strlen(url);
	if (ul > 7 && url[ul - 1] == '/' && ul + 6 < sizeof(url))
		{
		strcat(url, "screen");
		ul += 6;
		memcpy(p + n, url, ul);
		n += ul;
		}
	Send(ECastWelcome, p, n);
	iWelcomed = true;
	iAwaitKey = true;                         // (the Psion's first UPDATE is a keyframe)
	iKeyReqMs = iHal.Millis();
	iHaveSeq = false;
	SendViewers();
	snprintf(m, sizeof(m), "PsiCast: \"%s\" is sharing its screen, %dx%d (%s)%s", iName, w, h,
		iRoute == ERouteModem ? "modem route" : "TCP", iCtlAllowed ? ", control allowed" : "");
	iHal.Log(m);
	for (int i = 0; i < kCastViewers; i++)
		if (iView[i])
			{
			iView[i]->needHello = true;
			iView[i]->needStatus = true;
			}
	}

void Cast::OnUpdate(int aFlags, const uint8_t* aP, uint32_t aN)
	{
	if (aN < 4 || !iDec)
		return;
	uint32_t seq = Le32(aP);
	bool key = (aFlags & 1) != 0;
	iAckSeq = seq;                            // (every UPDATE taken is acknowledged: the window moves)
	iAckDue = true;
	bool gap = iHaveSeq && seq != iLastSeq + 1;
	iLastSeq = seq;
	iHaveSeq = true;
	if (!key && (iAwaitKey || gap))
		{
		if (gap && !iAwaitKey)
			{
			iDecodeErrors++;
			KeyReq();                             // (one went missing: the stream is broken)
			}
		Ping();                                   // (discarded: the Psion's window may be stalled)
		return;
		}
	int r = pc_dec_update(iDec, aP + 4, (int)(aN - 4), key ? 1 : 0, &Cast::OnScrollCb, &Cast::OnRectCb, this);
	if (r == PC_OK)
		{
		iUpdates++;
		if (key)
			iAwaitKey = false;
		}
	else
		{
		iDecodeErrors++;
		if (iDecodeErrors <= 3 || (iDecodeErrors % 50) == 0)
			{
			char m[80];
			snprintf(m, sizeof(m), "PsiCast: an update did not decode (%d): asking for a keyframe", r);
			iHal.Log(m);
			}
		KeyReq();
		}
	}

void Cast::OnStatus(const uint8_t* aP, uint32_t aN)
	{
	if (aN < 12)
		return;
	iBattery = aP[0];
	iMains = aP[1];
	iFreeRam = Le32(aP + 2);
	iPsionUps10 = Le16(aP + 6);
	iCpu10 = Le16(aP + 8);
	iCtlAllowed = aP[10] != 0;
	iPaused = aP[11] != 0;
	iHaveStatus = true;
	if (iPingWait)
		{
		iRttMs = iHal.Millis() - iPingSentMs;
		iPingWait = false;
		}
	for (int i = 0; i < kCastViewers; i++)
		if (iView[i])
			iView[i]->needStatus = true;
	}

void Cast::Tick()
	{
	uint32_t now = iHal.Millis();
	if (iLinkId && !iLinkEnding)
		{
		if (now - iLastInMs >= kSilenceMs)
			LinkEnd("nothing from the Psion for 20 s");
		else if (iWelcomed)
			{
			if (now - iLastOutMs >= kPingMs)
				Ping();
			if (iAwaitKey && now - iKeyReqMs >= kKeyRetryMs)
				KeyReq();
			// the viewers' input, at most 64 events a message
			while (iInputN)
				{
				int n = iInputN > 64 ? 64 : iInputN;
				if (!Send(ECastInput, &iInput[0][0], (size_t)n * 8))
					break;
				memmove(iInput, iInput + n, (size_t)(iInputN - n) * 8);
				iInputN -= n;
				}
			}
		}
	if (now - iRateMarkMs >= 1000)
		{
		uint32_t dt = now - iRateMarkMs;
		iRateBytes = (uint32_t)((uint64_t)(iBytesIn - iRateMark) * 1000 / dt);
		iUps10 = (uint32_t)((uint64_t)(iUpdates - iUpdMark) * 10000 / dt);
		iRateMark = iBytesIn;
		iUpdMark = iUpdates;
		iRateMarkMs = now;
		for (int i = 0; i < kCastViewers; i++)
			{
			CastViewer* v = iView[i];
			if (!v)
				continue;
			v->rateBytes = (uint32_t)((uint64_t)(v->bytesOut - v->rateMark) * 1000 / dt);
			v->rateMark = v->bytesOut;
			}
		}
	for (int i = 0; i < kCastViewers; i++)
		{
		CastViewer* v = iView[i];
		if (!v || v->closing)
			continue;
		if (now - v->lastInMs >= kViewerIdleMs)
			{
			static const uint8_t kGoing[] = { 0x03, 0xe9 };
			QueueCtl(*v, EWsClose, kGoing, 2);
			v->closing = true;
			iHal.Log("PsiCast: a viewer went quiet: closed");
			continue;
			}
		if (now - v->lastStatusMs >= kStatusMs)
			v->needStatus = true;
		}
	// tokens are good for a minute
	for (size_t i = 0; i < sizeof(iTokens) / sizeof(iTokens[0]); i++)
		if (iTokens[i].text[0] && now - iTokens[i].issuedMs >= kTokenMs)
			iTokens[i].text[0] = 0;
	if (iWelcomed && !iLinkEnding && (Viewers() != iLastViewers || ViewersWithControl() != iLastViewersCtl))
		ViewersChanged();
	}

void Cast::ViewersChanged()
	{
	int n = Viewers();
	if (!iWelcomed || iLinkEnding)
		return;
	bool first = iLastViewers <= 0 && n > 0;
	SendViewers();
	if (first)
		KeyReq();                             // (PROTOCOL.md: on the first viewer)
	}

void Cast::StateText(char* aOut, size_t aMax) const
	{
	if (!iEnabled)
		snprintf(aOut, aMax, "off");
	else if (!LinkUp())
		snprintf(aOut, aMax, "on, waiting for the Psion; %d viewer%s", Viewers(), Viewers() == 1 ? "" : "s");
	else
		snprintf(aOut, aMax, "sharing \"%s\" (%s), %d viewer%s%s", iName, iRoute == ERouteModem ? "modem route" : "PPP",
			Viewers(), Viewers() == 1 ? "" : "s", iPaused ? ", paused" : "");
	}

// ----- the screen ----------------------------------------------------------------------------------

static const int kMapRow = (CastViewer::kTilesX + 7) / 8;
static inline bool Bit(const uint8_t aMap[][kMapRow], int aTy, int aTx) { return (aMap[aTy][aTx >> 3] >> (aTx & 7)) & 1; }
static inline bool Bit(const CastViewer& aV, int aTy, int aTx) { return Bit(aV.dirty, aTy, aTx); }
static inline void SetBit(CastViewer& aV, int aTy, int aTx) { aV.dirty[aTy][aTx >> 3] |= (uint8_t)(1 << (aTx & 7)); }
static inline void ClearBit(CastViewer& aV, int aTy, int aTx) { aV.dirty[aTy][aTx >> 3] &= (uint8_t)~(1 << (aTx & 7)); }

bool Cast::EnsureScreen(int aW, int aH)
	{
	if (iDec && iW == aW && iH == aH)
		return true;
	FreeScreen();
	// the screen copy, the inflater and its 4 KB window
	if (!iHal.BigRoom((size_t)aW * aH / 2 + 16 * 1024))
		return false;
	iDec = pc_dec_new(aW, aH);
	if (!iDec)
		return false;
	pc_dec_set_flags(iDec, PC_DEC_PARTIAL_SCROLL);   // (moves of part of the width are sent as SCROLL too)
	iW = aW;
	iH = aH;
	for (int i = 0; i < kCastViewers; i++)
		if (iView[i])
			iView[i]->needFull = true;
	return true;
	}

void Cast::FreeScreen()
	{
	if (!iDec)
		return;
	// a message on its way to a viewer reads the screen: it is finished with zeros
	for (int i = 0; i < kCastViewers; i++)
		if (iView[i] && iView[i]->inType)
			{
			iView[i]->zeroFill = true;
			iView[i]->needFull = true;
			}
	pc_dec_free(iDec);
	iDec = 0;
	iW = iH = 0;
	}

const unsigned char* Cast::Screen() const
	{
	return iDec ? pc_dec_screen(iDec) : 0;
	}

void Cast::OnRectCb(void* aCtx, const pc_rect* aR)
	{
	Cast* self = (Cast*)aCtx;
	for (int i = 0; i < kCastViewers; i++)
		if (self->iView[i])
			self->MarkRect(*self->iView[i], aR->x, aR->y, aR->w, aR->h);
	}

void Cast::OnScrollCb(void* aCtx, const pc_scroll* aS)
	{
	Cast* self = (Cast*)aCtx;
	for (int i = 0; i < kCastViewers; i++)
		if (self->iView[i])
			self->ScrollViewer(*self->iView[i], aS->y0, aS->y1, aS->dy, aS->x, aS->w);
	}


void Cast::MarkRect(CastViewer& aV, int aX, int aY, int aW, int aH)
	{
	if (aX < 0) { aW += aX; aX = 0; }
	if (aY < 0) { aH += aY; aY = 0; }
	if (aX + aW > iW) aW = iW - aX;
	if (aY + aH > iH) aH = iH - aY;
	if (aW <= 0 || aH <= 0)
		return;
	int tx0 = aX / CastViewer::kTileW, tx1 = (aX + aW - 1) / CastViewer::kTileW;
	int ty0 = aY / CastViewer::kTileH, ty1 = (aY + aH - 1) / CastViewer::kTileH;
	for (int ty = ty0; ty <= ty1; ty++)
		for (int tx = tx0; tx <= tx1; tx++)
			SetBit(aV, ty, tx);
	}

void Cast::MarkAll(CastViewer& aV)
	{
	MarkRect(aV, 0, 0, iW, iH);
	}

// A move (rows [y0, y1) of columns [x, x+w) by dy): sent on to the viewer as
// SCROLL, with its dirty map moved the same way. A destination tile takes
// the bits of the source tiles it was copied from, and keeps its own if part
// of it was not in the move (rows outside the band, or columns outside the
// moved ones). The area of a message still on its way is marked first: its
// rows not yet read will be read after the move. With no room for the
// SCROLL the destination is simply repainted (the map is not moved: nor is
// the canvas)
void Cast::ScrollViewer(CastViewer& aV, int aY0, int aY1, int aDy, int aX, int aW)
	{
	if (aV.needFull)
		return;                               // (the FULL will carry it)
	if (aV.inType == ECastFull)
		MarkAll(aV);
	else if (aV.inType == ECastRects)
		for (int r = 0; r < aV.nRect; r++)
			MarkRect(aV, aV.rect[r].x, aV.rect[r].y, aV.rect[r].w, aV.rect[r].h);
	if (aX < 0) { aW += aX; aX = 0; }
	if (aX + aW > iW) aW = iW - aX;
	int d0 = aY0 + aDy, d1 = aY1 + aDy;
	if (d0 < 0) d0 = 0;
	if (d1 > iH) d1 = iH;
	if (d0 >= d1 || aDy == 0 || aW <= 0)
		return;
	uint8_t frame[2 + 11];
	if (sizeof(aV.small) - aV.smallLen < sizeof(frame) || aV.closing)
		{
		MarkRect(aV, aX, d0, aW, d1 - d0);
		return;
		}
	static uint8_t old[CastViewer::kTilesY][kMapRow];
	memcpy(old, aV.dirty, sizeof(old));
	const int th = CastViewer::kTileH, tw = CastViewer::kTileW;
	int tx0 = aX / tw, tx1 = (aX + aW - 1) / tw;
	for (int ty = d0 / th; ty * th < d1; ty++)
		{
		int top = ty * th, bot = top + th < iH ? top + th : iH;
		int rs = top > d0 ? top : d0, re = bot < d1 ? bot : d1;
		bool partRows = top < d0 || bot > d1;
		int sy0 = (rs - aDy) / th, sy1 = (re - 1 - aDy) / th;
		for (int tx = tx0; tx <= tx1; tx++)
			{
			int left = tx * tw, right = left + tw < iW ? left + tw : iW;
			bool inside = left >= aX && right <= aX + aW;
			bool v = (partRows || !inside) && Bit(old, ty, tx);
			for (int sy = sy0; sy <= sy1 && !v; sy++)
				if (sy >= 0 && sy < CastViewer::kTilesY && Bit(old, sy, tx))
					v = true;
			if (v)
				SetBit(aV, ty, tx);
			else
				ClearBit(aV, ty, tx);
			}
		}
	// the SCROLL itself, after whatever is queued (the canvas moves in order)
	size_t h = WsHeader(frame, EWsBinary, 11);
	frame[h] = ECastScroll;
	Put16(frame + h + 1, (uint32_t)aY0);
	Put16(frame + h + 3, (uint32_t)aY1);
	Put16(frame + h + 5, (uint32_t)(uint16_t)(int16_t)aDy);
	Put16(frame + h + 7, (uint32_t)aX);          // (an extension: the columns that moved)
	Put16(frame + h + 9, (uint32_t)aW);
	memcpy(aV.small + aV.smallLen, frame, h + 11);
	aV.smallLen += h + 11;
	}

// ----- link B: what a viewer is sent -----------------------------------------------------------

void Cast::StartFull(CastViewer& aV)
	{
	aV.needFull = false;
	memset(aV.dirty, 0, sizeof(aV.dirty));
	aV.nRect = 1;
	aV.rect[0].x = 0;
	aV.rect[0].y = 0;
	aV.rect[0].w = (uint16_t)iW;
	aV.rect[0].h = (uint16_t)iH;
	uint32_t len = 1 + 4 + (uint32_t)iW * iH / 2;
	size_t h = WsHeader(aV.head, EWsBinary, len);
	aV.head[h] = ECastFull;
	Put16(aV.head + h + 1, (uint32_t)iW);
	Put16(aV.head + h + 3, (uint32_t)iH);
	aV.headLen = h + 5;
	aV.headPos = 0;
	aV.curRect = aV.curRow = 0;
	aV.curOff = 0;
	aV.zeroFill = false;
	aV.inType = ECastFull;
	}

// The dirty tiles as rectangles: runs of tiles in a row, merged down the
// rows while the run is the same. At most kMaxRects, and about 32 KB of
// pixels a message (what is left stays dirty for the next)
bool Cast::BuildRects(CastViewer& aV)
	{
	const int tw = CastViewer::kTileW, th = CastViewer::kTileH;
	int ntx = (iW + tw - 1) / tw, nty = (iH + th - 1) / th;
	struct T { int tx0, tx1, ty0, ty1; } rt[CastViewer::kMaxRects];
	int n = 0;
	uint32_t bytes = 0;
	bool full = false;
	for (int ty = 0; ty < nty && !full; ty++)
		{
		if (bytes >= 32 * 1024)
			break;
		int tx = 0;
		while (tx < ntx)
			{
			if (!Bit(aV, ty, tx))
				{
				tx++;
				continue;
				}
			int t0 = tx;
			while (tx < ntx && Bit(aV, ty, tx))
				tx++;
			int k = -1;
			for (int j = 0; j < n; j++)
				if (rt[j].ty1 == ty && rt[j].tx0 == t0 && rt[j].tx1 == tx)
					k = j;
			if (k < 0)
				{
				if (n == CastViewer::kMaxRects)
					{
					full = true;
					break;
					}
				k = n++;
				rt[k].tx0 = t0;
				rt[k].tx1 = tx;
				rt[k].ty0 = ty;
				}
			rt[k].ty1 = ty + 1;
			for (int c = t0; c < tx; c++)
				ClearBit(aV, ty, c);
			int x0 = t0 * tw, x1 = tx * tw < iW ? tx * tw : iW;
			int y0 = ty * th, y1 = y0 + th < iH ? y0 + th : iH;
			bytes += (uint32_t)((x1 - x0) / 2 * (y1 - y0));
			}
		}
	if (!n)
		return false;
	uint32_t len = 1 + 2 + 8 * (uint32_t)n;
	for (int j = 0; j < n; j++)
		{
		CastViewer::Rect& r = aV.rect[j];
		r.x = (uint16_t)(rt[j].tx0 * tw);
		r.y = (uint16_t)(rt[j].ty0 * th);
		int x1 = rt[j].tx1 * tw < iW ? rt[j].tx1 * tw : iW;
		int y1 = rt[j].ty1 * th < iH ? rt[j].ty1 * th : iH;
		r.w = (uint16_t)(x1 - r.x);
		r.h = (uint16_t)(y1 - r.y);
		len += (uint32_t)r.w / 2 * r.h;
		}
	aV.nRect = n;
	size_t h = WsHeader(aV.head, EWsBinary, len);
	aV.head[h] = ECastRects;
	Put16(aV.head + h + 1, (uint32_t)n);
	size_t o = h + 3;
	for (int j = 0; j < n; j++)
		{
		Put16(aV.head + o, aV.rect[j].x);
		Put16(aV.head + o + 2, aV.rect[j].y);
		Put16(aV.head + o + 4, aV.rect[j].w);
		Put16(aV.head + o + 6, aV.rect[j].h);
		o += 8;
		}
	aV.headLen = o;
	aV.headPos = 0;
	aV.curRect = aV.curRow = 0;
	aV.curOff = 0;
	aV.zeroFill = false;
	aV.inType = ECastRects;
	return true;
	}

// the message on its way: its headers, then each rect's rows from the screen
size_t Cast::Stream(CastViewer& aV, uint8_t* aOut, size_t aMax)
	{
	size_t o = 0;
	if (aV.headPos < aV.headLen)
		{
		size_t n = aV.headLen - aV.headPos;
		if (n > aMax)
			n = aMax;
		memcpy(aOut, aV.head + aV.headPos, n);
		aV.headPos += n;
		o += n;
		if (aV.headPos < aV.headLen)
			return o;
		}
	const unsigned char* scr = Screen();
	int stride = iDec ? pc_dec_stride(iDec) : 0;
	while (aV.curRect < aV.nRect && o < aMax)
		{
		const CastViewer::Rect& r = aV.rect[aV.curRect];
		size_t rowBytes = r.w / 2;
		size_t n = rowBytes - aV.curOff;
		if (n > aMax - o)
			n = aMax - o;
		if (aV.zeroFill || !scr)
			memset(aOut + o, 0, n);
		else
			memcpy(aOut + o, scr + (size_t)(r.y + aV.curRow) * stride + r.x / 2 + aV.curOff, n);
		o += n;
		aV.curOff += n;
		if (aV.curOff == rowBytes)
			{
			aV.curOff = 0;
			if (++aV.curRow == r.h)
				{
				aV.curRow = 0;
				aV.curRect++;
				}
			}
		}
	if (aV.curRect >= aV.nRect)
		aV.inType = 0;
	return o;
	}

void Cast::QueueCtl(CastViewer& aV, int aOp, const uint8_t* aP, size_t aN)
	{
	if (aN > 125 || sizeof(aV.small) - aV.smallLen < aN + 2)
		return;
	size_t h = WsHeader(aV.small + aV.smallLen, aOp, (uint32_t)aN);
	memcpy(aV.small + aV.smallLen + h, aP, aN);
	aV.smallLen += h + aN;
	}

void Cast::QueueText(CastViewer& aV, const char* aJson)
	{
	size_t n = strlen(aJson);
	if (sizeof(aV.small) - aV.smallLen < n + 4)
		return;                               // (no room: a status comes again in a second)
	size_t h = WsHeader(aV.small + aV.smallLen, EWsText, (uint32_t)n);
	memcpy(aV.small + aV.smallLen + h, aJson, n);
	aV.smallLen += h + n;
	}

void Cast::QueueHello(CastViewer& aV)
	{
	char m[240];
	snprintf(m, sizeof(m), "{\"t\":\"hello\",\"name\":\"%s\",\"w\":%d,\"h\":%d,\"digitiser\":[%d,%d],\"linkUp\":%s,\"route\":\"%s\",\"askedControl\":%s}",
		iName, iW, iH, iDigW, iDigH, LinkUp() ? "true" : "false", iRoute == ERouteModem ? "modem" : "ppp", aV.control ? "true" : "false");
	QueueText(aV, m);
	aV.needHello = false;
	}

void Cast::QueueStatus(CastViewer& aV)
	{
	char m[420];
	char bat[16], mains[8], cpu[16], rtt[16];
	if (iBattery <= 100) snprintf(bat, sizeof(bat), "%d", iBattery); else strcpy(bat, "null");
	if (iMains <= 1) snprintf(mains, sizeof(mains), "%s", iMains ? "true" : "false"); else strcpy(mains, "null");
	if (iCpu10 != 65535) snprintf(cpu, sizeof(cpu), "%d.%d", iCpu10 / 10, iCpu10 % 10); else strcpy(cpu, "null");
	if (iRttMs && LinkUp()) snprintf(rtt, sizeof(rtt), "%lu", (unsigned long)iRttMs); else strcpy(rtt, "null");
	bool up = LinkUp() && !iLinkEnding;
	bool you = up && aV.control && iCtlAllowed && iInputCap;
	snprintf(m, sizeof(m), "{\"t\":\"status\",\"linkUp\":%s,\"route\":\"%s\",\"battery\":%s,\"mains\":%s,\"freeRam\":%lu,"
		"\"ups\":%lu.%lu,\"psionUps\":%d.%d,\"cpu\":%s,\"control\":%s,\"paused\":%s,\"viewers\":%d,\"rateBytes\":%lu,"
		"\"viewBytes\":%lu,\"rtt\":%s,\"you\":%s,\"errors\":%lu,\"updates\":%lu}",
		up ? "true" : "false", iRoute == ERouteModem ? "modem" : "ppp", bat, mains, (unsigned long)(iHaveStatus ? iFreeRam : 0),
		(unsigned long)(iUps10 / 10), (unsigned long)(iUps10 % 10), iPsionUps10 / 10, iPsionUps10 % 10, cpu,
		up && iCtlAllowed ? "true" : "false", up && iPaused ? "true" : "false", Viewers(), (unsigned long)(up ? iRateBytes : 0),
		(unsigned long)aV.rateBytes, rtt, you ? "true" : "false", (unsigned long)Errors(), (unsigned long)iUpdates);
	QueueText(aV, m);
	aV.needStatus = false;
	aV.lastStatusMs = iHal.Millis();
	}

size_t Cast::ViewerOut(int aIdx, uint8_t* aOut, size_t aMax)
	{
	if (aIdx < 0 || aIdx >= kCastViewers || !iView[aIdx])
		return 0;
	CastViewer& v = *iView[aIdx];
	size_t o = 0;
	while (o < aMax)
		{
		if (v.inType)
			{
			o += Stream(v, aOut + o, aMax - o);
			if (v.inType)
				break;
			continue;
			}
		if (v.smallPos < v.smallLen)
			{
			size_t n = v.smallLen - v.smallPos;
			if (n > aMax - o)
				n = aMax - o;
			memcpy(aOut + o, v.small + v.smallPos, n);
			v.smallPos += n;
			o += n;
			if (v.smallPos == v.smallLen)
				v.smallPos = v.smallLen = 0;
			continue;
			}
		if (v.closing)
			break;
		if (v.needHello) { QueueHello(v); continue; }
		if (v.needBye)
			{
			QueueText(v, "{\"t\":\"bye\"}");
			v.needBye = false;
			continue;
			}
		if (v.needStatus) { QueueStatus(v); continue; }
		if (!Screen())
			break;
		if (v.needFull) { StartFull(v); continue; }
		if (BuildRects(v))
			continue;
		break;
		}
	v.bytesOut += (uint32_t)o;
	return o;
	}

bool Cast::ViewerDone(int aIdx) const
	{
	if (aIdx < 0 || aIdx >= kCastViewers || !iView[aIdx])
		return true;
	const CastViewer& v = *iView[aIdx];
	return v.closing && !v.inType && v.smallPos >= v.smallLen;
	}

// ----- link B: from a viewer --------------------------------------------------------------------

bool Cast::NewToken(bool aControl, char* aOut, size_t aMax)
	{
	if (!iEnabled || aMax < sizeof(iTokens[0].text))
		return false;
	uint32_t now = iHal.Millis();
	size_t slot = 0;
	for (size_t i = 0; i < sizeof(iTokens) / sizeof(iTokens[0]); i++)
		{
		if (!iTokens[i].text[0])
			{
			slot = i;
			break;
			}
		if (now - iTokens[i].issuedMs > now - iTokens[slot].issuedMs)
			slot = i;                          // (all in use: the oldest goes)
		}
	uint8_t r[12];
	iHal.Random(r, sizeof(r));
	static const char kHex[] = "0123456789abcdef";
	for (int i = 0; i < 12; i++)
		{
		iTokens[slot].text[i * 2] = kHex[r[i] >> 4];
		iTokens[slot].text[i * 2 + 1] = kHex[r[i] & 15];
		}
	iTokens[slot].text[24] = 0;
	iTokens[slot].control = aControl;
	iTokens[slot].issuedMs = now;
	memcpy(aOut, iTokens[slot].text, 25);
	return true;
	}

int Cast::ViewerOpen(const char* aToken, char* aWhy, size_t aWhyMax)
	{
	if (!iEnabled)
		{
		snprintf(aWhy, aWhyMax, "PsiCast is off");
		return -1;
		}
	uint32_t now = iHal.Millis();
	int t = -1;
	if (aToken && strlen(aToken) == 24)
		for (size_t i = 0; i < sizeof(iTokens) / sizeof(iTokens[0]); i++)
			if (iTokens[i].text[0] && now - iTokens[i].issuedMs < kTokenMs && strcmp(iTokens[i].text, aToken) == 0)
				t = (int)i;
	if (t < 0)
		{
		snprintf(aWhy, aWhyMax, "no such token, or it has expired");
		return -1;
		}
	int slot = -1;
	for (int i = 0; i < kCastViewers; i++)
		if (!iView[i])
			{
			slot = i;
			break;
			}
	if (slot < 0)
		{
		snprintf(aWhy, aWhyMax, "%d viewers already", kCastViewers);
		return -1;
		}
	CastViewer* v = new (std::nothrow) CastViewer();   // (value-initialised: all zero)
	if (!v)
		{
		snprintf(aWhy, aWhyMax, "not enough memory");
		return -1;
		}
	bool control = iTokens[t].control;
	iTokens[t].text[0] = 0;                   // (single use)
	v->used = true;
	v->control = control;
	v->openedMs = v->lastInMs = now;
	v->rateMarkMs = now;
	v->needFull = true;
	v->needHello = true;
	v->needStatus = true;
	iView[slot] = v;
	char m[80];
	snprintf(m, sizeof(m), "PsiCast: a viewer connected%s (%d now)", control ? ", asking for control" : "", Viewers());
	iHal.Log(m);
	ViewersChanged();
	return slot;
	}

void Cast::ViewerClose(int aIdx)
	{
	if (aIdx < 0 || aIdx >= kCastViewers || !iView[aIdx])
		return;
	delete iView[aIdx];
	iView[aIdx] = 0;
	char m[64];
	snprintf(m, sizeof(m), "PsiCast: a viewer left (%d now)", Viewers());
	iHal.Log(m);
	ViewersChanged();
	if (!Viewers() && !iLinkId)
		FreeScreen();
	}

int Cast::Viewers() const
	{
	int n = 0;
	for (int i = 0; i < kCastViewers; i++)
		if (iView[i] && !iView[i]->closing)
			n++;
	return n;
	}

int Cast::ViewersWithControl() const
	{
	int n = 0;
	for (int i = 0; i < kCastViewers; i++)
		if (iView[i] && !iView[i]->closing && iView[i]->control)
			n++;
	return n;
	}

bool Cast::ViewerIn(int aIdx, const uint8_t* aData, size_t aLen)
	{
	if (aIdx < 0 || aIdx >= kCastViewers || !iView[aIdx])
		return false;
	CastViewer& v = *iView[aIdx];
	if (v.closing)
		return true;                          // (its close is on the way: the rest is ignored)
	iWsCur = &v;
	int r = v.reader.Feed(aData, aLen, &Cast::OnWsMessage, this);
	iWsCur = 0;
	if (r < 0)
		{
		uint8_t code[2] = { (uint8_t)(v.reader.CloseCode() >> 8), (uint8_t)v.reader.CloseCode() };
		QueueCtl(v, EWsClose, code, 2);
		v.closing = true;
		}
	return true;
	}

void Cast::OnWsMessage(void* aCtx, int aOp, const uint8_t* aData, size_t aLen)
	{
	Cast* self = (Cast*)aCtx;
	if (self->iWsCur)
		self->ViewerMessage(*self->iWsCur, aOp, aData, aLen);
	}

void Cast::ViewerMessage(CastViewer& aV, int aOp, const uint8_t* aData, size_t aLen)
	{
	aV.lastInMs = iHal.Millis();
	if (aV.closing)
		return;
	switch (aOp)
		{
	case EWsText:
		{
		char json[WsReader::kMax + 1];
		memcpy(json, aData, aLen);
		json[aLen] = 0;
		ViewerText(aV, json);
		break;
		}
	case EWsPing:
		QueueCtl(aV, EWsPong, aData, aLen);
		break;
	case EWsClose:
		QueueCtl(aV, EWsClose, aData, aLen >= 2 ? 2 : 0);
		aV.closing = true;
		break;
	default:
		break;                                // (binary and pongs: nothing to do)
		}
	}

void Cast::ViewerText(CastViewer& aV, const char* aJson)
	{
	char t[12], e[8];
	if (!JsonStr(aJson, "t", t, sizeof(t)))
		return;
	long a = 0, b = 0;
	if (strcmp(t, "ping") == 0)
		{
		// (latency: the page's own ping, answered at once)
		long n = 0;
		JsonInt(aJson, "n", n);
		char m[48];
		snprintf(m, sizeof(m), "{\"t\":\"pong\",\"n\":%ld}", n);
		QueueText(aV, m);
		return;
		}
	if (strcmp(t, "full") == 0)
		{
		aV.needFull = true;
		return;
		}
	// input: only from a viewer whose token asked for control, while the Psion allows it
	if (!aV.control || !iCtlAllowed || !iInputCap || !LinkUp() || iLinkEnding)
		return;
	if (strcmp(t, "pen") == 0 && JsonStr(aJson, "e", e, sizeof(e)) && JsonInt(aJson, "x", a) && JsonInt(aJson, "y", b))
		{
		int kind = strcmp(e, "down") == 0 ? 1 : strcmp(e, "move") == 0 ? 2 : strcmp(e, "up") == 0 ? 3 : 0;
		if (kind && a > -2000 && a < 2000 && b > -2000 && b < 2000)
			Input(kind, (int)a, (int)b);
		}
	else if (strcmp(t, "key") == 0 && JsonStr(aJson, "e", e, sizeof(e)) && JsonInt(aJson, "sc", a))
		{
		int kind = strcmp(e, "down") == 0 ? 4 : strcmp(e, "up") == 0 ? 5 : 0;
		if (kind && a >= 0 && a < 65536)
			Input(kind, (int)a, 0);
		}
	else if (strcmp(t, "char") == 0 && JsonInt(aJson, "c", a))
		{
		if (a > 0 && a < 256)
			Input(6, (int)a, 0);
		}
	}

void Cast::Input(int aKind, int aA, int aB)
	{
	if (iInputN >= kInputMax)
		return;                               // (a flood: the newest are dropped)
	uint8_t* e = iInput[iInputN++];
	e[0] = (uint8_t)aKind;
	e[1] = 0;
	Put16(e + 2, (uint32_t)(uint16_t)(int16_t)aA);
	Put16(e + 4, (uint32_t)(uint16_t)(int16_t)aB);
	e[6] = e[7] = 0;
	}

} // namespace am
