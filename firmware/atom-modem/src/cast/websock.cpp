// websock.cpp - see websock.h. MIT licence (see LICENSE at the top of the repository).
#include "websock.h"
#include <string.h>
#include <stdio.h>
#if defined(AM_FIRMWARE)
#include <mbedtls/version.h>
#include <mbedtls/sha1.h>
#endif

namespace am {

// ----- SHA-1 (the handshake only: RFC 6455 asks for it, not for security) ------

#if defined(AM_FIRMWARE)
// on the Atom, mbedTLS (in ESP-IDF, and accelerated)
void Sha1(const uint8_t* aData, size_t aLen, uint8_t aOut[20])
	{
#if MBEDTLS_VERSION_MAJOR >= 3
	mbedtls_sha1(aData, aLen, aOut);
#else
	mbedtls_sha1_ret(aData, aLen, aOut);
#endif
	}
#else
// in the host tests, a plain one (FIPS 180-4)
static uint32_t Rol(uint32_t aV, int aN) { return (aV << aN) | (aV >> (32 - aN)); }

static void Sha1Block(uint32_t aH[5], const uint8_t aB[64])
	{
	uint32_t w[80];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)aB[i * 4] << 24) | ((uint32_t)aB[i * 4 + 1] << 16) | ((uint32_t)aB[i * 4 + 2] << 8) | aB[i * 4 + 3];
	for (int i = 16; i < 80; i++)
		w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	uint32_t a = aH[0], b = aH[1], c = aH[2], d = aH[3], e = aH[4];
	for (int i = 0; i < 80; i++)
		{
		uint32_t f, k;
		if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
		else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
		else { f = b ^ c ^ d; k = 0xca62c1d6; }
		uint32_t t = Rol(a, 5) + f + e + k + w[i];
		e = d; d = c; c = Rol(b, 30); b = a; a = t;
		}
	aH[0] += a; aH[1] += b; aH[2] += c; aH[3] += d; aH[4] += e;
	}

void Sha1(const uint8_t* aData, size_t aLen, uint8_t aOut[20])
	{
	uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
	size_t i = 0;
	for (; i + 64 <= aLen; i += 64)
		Sha1Block(h, aData + i);
	uint8_t last[128];
	size_t rest = aLen - i;
	memcpy(last, aData + i, rest);
	last[rest] = 0x80;
	size_t total = rest + 1 + 8 <= 64 ? 64 : 128;
	memset(last + rest + 1, 0, total - rest - 1);
	uint64_t bits = (uint64_t)aLen * 8;
	for (int k = 0; k < 8; k++)
		last[total - 1 - k] = (uint8_t)(bits >> (8 * k));
	Sha1Block(h, last);
	if (total == 128)
		Sha1Block(h, last + 64);
	for (int k = 0; k < 5; k++)
		{
		aOut[k * 4] = (uint8_t)(h[k] >> 24);
		aOut[k * 4 + 1] = (uint8_t)(h[k] >> 16);
		aOut[k * 4 + 2] = (uint8_t)(h[k] >> 8);
		aOut[k * 4 + 3] = (uint8_t)h[k];
		}
	}
#endif

bool Base64(const uint8_t* aIn, size_t aLen, char* aOut, size_t aMax)
	{
	static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t need = (aLen + 2) / 3 * 4 + 1;
	if (aMax < need)
		return false;
	size_t o = 0;
	for (size_t i = 0; i < aLen; i += 3)
		{
		uint32_t v = (uint32_t)aIn[i] << 16;
		if (i + 1 < aLen) v |= (uint32_t)aIn[i + 1] << 8;
		if (i + 2 < aLen) v |= aIn[i + 2];
		aOut[o++] = kB64[(v >> 18) & 63];
		aOut[o++] = kB64[(v >> 12) & 63];
		aOut[o++] = i + 1 < aLen ? kB64[(v >> 6) & 63] : '=';
		aOut[o++] = i + 2 < aLen ? kB64[v & 63] : '=';
		}
	aOut[o] = 0;
	return true;
	}

bool WsAcceptKey(const char* aKey, char aOut[29])
	{
	static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	size_t kl = strlen(aKey);
	if (kl == 0 || kl > 60)
		return false;
	char buf[100];
	memcpy(buf, aKey, kl);
	memcpy(buf + kl, kGuid, sizeof(kGuid) - 1);
	uint8_t h[20];
	Sha1((const uint8_t*)buf, kl + sizeof(kGuid) - 1, h);
	return Base64(h, 20, aOut, 29);
	}

// ----- the opening request -----------------------------------------------------

static int Lower(int aC) { return (aC >= 'A' && aC <= 'Z') ? aC + 32 : aC; }

// does [aP, aP+aN) contain aWord (any case), as a token of a comma list?
static bool HasToken(const char* aP, size_t aN, const char* aWord)
	{
	size_t wl = strlen(aWord);
	for (size_t i = 0; i + wl <= aN; i++)
		{
		bool same = true;
		for (size_t k = 0; k < wl && same; k++)
			same = Lower((unsigned char)aP[i + k]) == Lower((unsigned char)aWord[k]);
		if (!same)
			continue;
		bool startOk = i == 0 || aP[i - 1] == ' ' || aP[i - 1] == ',' || aP[i - 1] == '\t';
		bool endOk = i + wl == aN || aP[i + wl] == ' ' || aP[i + wl] == ',' || aP[i + wl] == '\t' || aP[i + wl] == '\r';
		if (startOk && endOk)
			return true;
		}
	return false;
	}

static bool NameIs(const char* aP, size_t aN, const char* aName)
	{
	if (strlen(aName) != aN)
		return false;
	for (size_t i = 0; i < aN; i++)
		if (Lower((unsigned char)aP[i]) != Lower((unsigned char)aName[i]))
			return false;
	return true;
	}

int WsParseRequest(const char* aBuf, size_t aLen, WsRequest& aOut, size_t& aUsed)
	{
	memset(&aOut, 0, sizeof(aOut));
	size_t end = 0;
	for (size_t i = 0; i + 3 < aLen; i++)
		if (aBuf[i] == '\r' && aBuf[i + 1] == '\n' && aBuf[i + 2] == '\r' && aBuf[i + 3] == '\n')
			{
			end = i + 4;
			break;
			}
	if (!end)
		return aLen >= kWsHeadMax ? -1 : 0;
	if (end > kWsHeadMax)
		return -1;
	aUsed = end;
	// the request line: GET <path> HTTP/1.1
	size_t eol = 0;
	while (eol + 1 < end && !(aBuf[eol] == '\r' && aBuf[eol + 1] == '\n'))
		eol++;
	const char* sp1 = (const char*)memchr(aBuf, ' ', eol);
	if (!sp1)
		return -1;
	const char* sp2 = (const char*)memchr(sp1 + 1, ' ', (size_t)(aBuf + eol - (sp1 + 1)));
	if (!sp2 || (size_t)(aBuf + eol - sp2) < 6 || strncmp(sp2 + 1, "HTTP/", 5) != 0)
		return -1;
	aOut.get = sp1 - aBuf == 3 && strncmp(aBuf, "GET", 3) == 0;
	size_t pl = (size_t)(sp2 - sp1 - 1);
	if (pl >= sizeof(aOut.path))
		return -1;
	memcpy(aOut.path, sp1 + 1, pl);
	aOut.path[pl] = 0;
	// the headers
	bool upgrade = false, connection = false;
	size_t p = eol + 2;
	while (p + 2 <= end - 2)
		{
		size_t le = p;
		while (le + 1 < end && !(aBuf[le] == '\r' && aBuf[le + 1] == '\n'))
			le++;
		const char* line = aBuf + p;
		size_t ll = le - p;
		const char* colon = (const char*)memchr(line, ':', ll);
		if (colon)
			{
			size_t nl = (size_t)(colon - line);
			const char* v = colon + 1;
			size_t vl = ll - nl - 1;
			while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
			while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
			if (NameIs(line, nl, "upgrade"))
				upgrade = HasToken(v, vl, "websocket");
			else if (NameIs(line, nl, "connection"))
				connection = HasToken(v, vl, "upgrade");
			else if (NameIs(line, nl, "sec-websocket-version"))
				aOut.version13 = vl == 2 && v[0] == '1' && v[1] == '3';
			else if (NameIs(line, nl, "sec-websocket-key") && vl < sizeof(aOut.key))
				{
				memcpy(aOut.key, v, vl);
				aOut.key[vl] = 0;
				}
			}
		p = le + 2;
		}
	aOut.upgrade = upgrade && connection;
	return 1;
	}

bool WsQuery(const char* aPath, const char* aName, char* aOut, size_t aMax)
	{
	const char* q = strchr(aPath, '?');
	if (!q || !aMax)
		return false;
	size_t nl = strlen(aName);
	const char* p = q + 1;
	while (*p)
		{
		const char* amp = strchr(p, '&');
		size_t len = amp ? (size_t)(amp - p) : strlen(p);
		if (len > nl && strncmp(p, aName, nl) == 0 && p[nl] == '=')
			{
			size_t vl = len - nl - 1;
			if (vl >= aMax)
				return false;
			memcpy(aOut, p + nl + 1, vl);
			aOut[vl] = 0;
			return true;
			}
		if (!amp)
			break;
		p = amp + 1;
		}
	return false;
	}

// ----- the client's frames --------------------------------------------------------

void WsReader::Reset()
	{
	iState = SHead;
	iLenNeed = iLenHave = 0;
	iMaskHave = 0;
	iLen = iHave = 0;
	iOp = 0;
	iFin = false;
	iMsgOp = 0;
	iMsgLen = 0;
	iCloseCode = 0;
	memset(iHead, 0, sizeof(iHead));
	}

int WsReader::Feed(const uint8_t* aData, size_t aLen, TOnMessage aOn, void* aCtx)
	{
	size_t i = 0;
	while (i < aLen)
		{
		uint8_t c = aData[i++];
		switch (iState)
			{
		case SHead:
			if (iLenHave == 0)
				{
				iHead[0] = c;
				iLenHave = 1;
				break;
				}
			iHead[1] = c;
			iLenHave = 0;
			{
			iFin = (iHead[0] & 0x80) != 0;
			iOp = iHead[0] & 0x0f;
			bool masked = (iHead[1] & 0x80) != 0;
			int len7 = iHead[1] & 0x7f;
			if ((iHead[0] & 0x70) || !masked)
				{
				iCloseCode = 1002;                // reserved bits, or a client frame unmasked
				return -1;
				}
			bool control = iOp >= 8;
			if (control)
				{
				if ((iOp != EWsClose && iOp != EWsPing && iOp != EWsPong) || !iFin || len7 > 125)
					{
					iCloseCode = 1002;
					return -1;
					}
				}
			else if (iOp == EWsCont ? iMsgOp == 0 : (iOp != EWsText && iOp != EWsBinary) || iMsgOp != 0)
				{
				iCloseCode = 1002;                // a continuation of nothing, or a new message mid-message
				return -1;
				}
			if (len7 == 126) { iLenNeed = 2; iState = SLen; }
			else if (len7 == 127) { iLenNeed = 8; iState = SLen; }
			else { iLen = (uint64_t)len7; iState = SMask; }
			iMaskHave = 0;
			}
			break;
		case SLen:
			iLenBytes[iLenHave++] = c;
			if (iLenHave == iLenNeed)
				{
				iLen = 0;
				for (int k = 0; k < iLenNeed; k++)
					iLen = (iLen << 8) | iLenBytes[k];
				iLenHave = 0;
				iState = SMask;
				}
			break;
		case SMask:
			iMask[iMaskHave++] = c;
			if (iMaskHave < 4)
				break;
			iHave = 0;
			if (iOp < 8)
				{
				if (iOp != EWsCont)
					{
					iMsgOp = iOp;
					iMsgLen = 0;
					}
				if (iMsgLen + iLen > kMax)
					{
					iCloseCode = 1009;            // too big for us
					return -1;
					}
				}
			if (iLen == 0)
				Finish(aOn, aCtx);
			else
				iState = SData;
			break;
		case SData:
			{
			uint8_t b = c ^ iMask[iHave & 3];
			if (iOp >= 8)
				iCtl[iHave] = b;
			else
				iMsg[iMsgLen++] = b;
			iHave++;
			if (iHave == iLen)
				Finish(aOn, aCtx);
			break;
			}
			}
		}
	return (int)i;
	}

void WsReader::Finish(TOnMessage aOn, void* aCtx)
	{
	iState = SHead;
	if (iOp >= 8)
		aOn(aCtx, iOp, iCtl, (size_t)iLen);
	else if (iFin)
		{
		int op = iMsgOp;
		iMsgOp = 0;
		aOn(aCtx, op, iMsg, iMsgLen);
		}
	}

size_t WsHeader(uint8_t* aOut, int aOp, uint32_t aLen)
	{
	aOut[0] = (uint8_t)(0x80 | (aOp & 0x0f));
	if (aLen < 126)
		{
		aOut[1] = (uint8_t)aLen;
		return 2;
		}
	if (aLen < 65536)
		{
		aOut[1] = 126;
		aOut[2] = (uint8_t)(aLen >> 8);
		aOut[3] = (uint8_t)aLen;
		return 4;
		}
	aOut[1] = 127;
	aOut[2] = aOut[3] = aOut[4] = aOut[5] = 0;
	aOut[6] = (uint8_t)(aLen >> 24);
	aOut[7] = (uint8_t)(aLen >> 16);
	aOut[8] = (uint8_t)(aLen >> 8);
	aOut[9] = (uint8_t)aLen;
	return 10;
	}

} // namespace am
