// proxy.cpp - see proxy.h. MIT licence (see LICENSE at the top of the repository).
#include "proxy.h"
#include "modem.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// tinfl (miniz) on the Atom, from the ESP32 ROM; zlib on a PC, unless the
// host tests ask for tinfl too (AM_TINFL, with hosttest/tinfl), so the code
// the Atom runs is tested
#if defined(ARDUINO) || defined(AM_TINFL)
#define AM_USE_TINFL 1
#if defined(ARDUINO)
#include <esp_idf_version.h>
#endif
#if defined(ARDUINO) && ESP_IDF_VERSION_MAJOR >= 5
#include <miniz.h>                           // (the ROM's tinfl, as the IDF 5 headers call it)
#else
#include <rom/miniz.h>
#endif
#else
#include <zlib.h>
#endif

namespace am {

const char* const Proxy::kName = "psiproxy";

static const uint32_t kIdleMs = 30000;          // a server silent this long has gone
static const int kMaxRedirects = 6;
static const size_t kMaxHead = 16 * 1024;       // a response head bigger than this: 502
// Body(): the ring room kept back for what is already held (a tag in the
// simplifier, up to 4 KB in the gzip writer, a chunk) ...
static const size_t kMargin = 10 * 1024;
// ... and the most the simplifier (then gzip) can make of one input byte
// ("<" is "&lt;", and fixed Huffman codes take 9 bits a byte)
static const size_t kGrowth = 5;
static_assert(Proxy::kSlack >= kMargin + kGrowth * 256, "Body() must be able to move on with kSlack free");

// ===== inflate =============================================================

#ifdef AM_USE_TINFL
static const size_t kDict = TINFL_LZ_DICT_SIZE;   // 32 KB, the deflate window
#else
static const size_t kDict = 4096;                 // (zlib keeps its own window): the
                                                  // most output from one Run
#endif

Inflater::Inflater() : iState(0), iBuf(0), iOfs(0), iFlags(0) {}
Inflater::~Inflater() { End(); }

bool Inflater::Begin(bool aZlib)
	{
	End();
	iBuf = (uint8_t*)malloc(kDict);
	if (!iBuf)
		return false;
	iOfs = 0;
#ifdef AM_USE_TINFL
	tinfl_decompressor* r = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
	if (!r)
		{
		End();
		return false;
		}
	tinfl_init(r);
	iState = r;
	iFlags = TINFL_FLAG_HAS_MORE_INPUT | (aZlib ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0);
#else
	z_stream* z = (z_stream*)calloc(1, sizeof(z_stream));
	if (!z || inflateInit2(z, aZlib ? 15 : -15) != Z_OK)
		{
		free(z);
		End();
		return false;
		}
	iState = z;
#endif
	return true;
	}

int Inflater::Run(const uint8_t* aIn, size_t aInLen, size_t& aUsed, const uint8_t*& aOut, size_t& aOutLen)
	{
	aUsed = 0;
	aOutLen = 0;
	aOut = iBuf;
	if (!iState)
		return -1;
#ifdef AM_USE_TINFL
	// the output buffer is the 32 KB window itself, used round and round:
	// tinfl needs all of it, from iOfs to its end (so up to 32 KB out of
	// one Run; the caller hands it on in slices). A smaller piece is a
	// window that is not a power of two: wrong back-references, or
	// TINFL_STATUS_BAD_PARAM
	tinfl_decompressor* r = (tinfl_decompressor*)iState;
	size_t inBytes = aInLen;
	size_t outBytes = kDict - iOfs;
	tinfl_status st = tinfl_decompress(r, aIn, &inBytes, iBuf, iBuf + iOfs, &outBytes, iFlags);
	aUsed = inBytes;
	aOut = iBuf + iOfs;
	aOutLen = outBytes;
	iOfs = (iOfs + outBytes) & (kDict - 1);
	if (st == TINFL_STATUS_DONE)
		return 1;
	return st < 0 ? -1 : 0;
#else
	z_stream* z = (z_stream*)iState;
	z->next_in = (Bytef*)aIn;
	z->avail_in = (uInt)aInLen;
	z->next_out = iBuf;
	z->avail_out = (uInt)kDict;
	int st = inflate(z, Z_NO_FLUSH);
	aUsed = aInLen - z->avail_in;
	aOutLen = (size_t)(z->next_out - iBuf);
	if (st == Z_STREAM_END)
		return 1;
	return (st == Z_OK || st == Z_BUF_ERROR) ? 0 : -1;
#endif
	}

void Inflater::End()
	{
	if (iState)
		{
#ifndef AM_USE_TINFL
		inflateEnd((z_stream*)iState);
#endif
		free(iState);
		iState = 0;
		}
	free(iBuf);
	iBuf = 0;
	}

// ===== helpers =============================================================

static int Lower(int aC) { return (aC >= 'A' && aC <= 'Z') ? aC + 32 : aC; }

static bool EqNoCase(const char* aA, const char* aB)
	{
	while (*aA && *aB)
		if (Lower((unsigned char)*aA++) != Lower((unsigned char)*aB++))
			return false;
	return *aA == *aB;
	}

static bool StartsNoCase(const char* aS, const char* aP)
	{
	while (*aP)
		if (Lower((unsigned char)*aS++) != Lower((unsigned char)*aP++))
			return false;
	return true;
	}

static bool ContainsNoCase(const char* aS, const char* aP)
	{
	for (; *aS; aS++)
		if (StartsNoCase(aS, aP))
			return true;
	return false;
	}

static void Copy(char* aOut, size_t aMax, const char* aIn, size_t aLen)
	{
	if (aLen >= aMax)
		aLen = aMax - 1;
	memmove(aOut, aIn, aLen);
	aOut[aLen] = 0;
	}

static const char* SkipSpace(const char* aP)
	{
	while (*aP == ' ' || *aP == '\t')
		aP++;
	return aP;
	}

// appends "fmt..." to aBuf (aLen so far, aMax its size); false if it did not fit
static bool Append(char* aBuf, size_t& aLen, size_t aMax, const char* aName, const char* aValue)
	{
	size_t n = strlen(aName), v = strlen(aValue);
	if (aLen + n + v + 5 >= aMax)
		return false;
	memcpy(aBuf + aLen, aName, n);
	aLen += n;
	memcpy(aBuf + aLen, ": ", 2);
	aLen += 2;
	memcpy(aBuf + aLen, aValue, v);
	aLen += v;
	memcpy(aBuf + aLen, "\r\n", 3);         // (and a NUL)
	aLen += 2;
	return true;
	}

// ===== the proxy ===========================================================

Proxy::Proxy(Hal& aHal)
	: iHal(aHal), iOut(0), iMode(EOn), iState(SClosed), iRequests(0), iFetched(0), iSent(0), iPictures(0),
	  iReqLen(0), iReqUsed(0), iStartMs(0), iFetched0(0), iSent0(0), iUpOpen(false), iChunkLen(0),
	  iUpgradeNext(0), iImgWidth(0), iImgMax(0), iImgMode(false), iImgBuf(0), iImgLen(0), iImgCap(0), iImgMs(0),
	  iGifBuf(0), iGifLen(0), iGifPos(0)
	{
	iStatus = 0;
	memset(iUpgrades, 0, sizeof(iUpgrades));
	iUpHost[0] = 0;
	iUrl[0] = 0;
	iMethod[0] = 0;
	}

void Proxy::Start(int aMode, bool aZip, int aImgWidth, int aImgMaxKB)
	{
	iMode = aMode;
	iZip = aZip;
	iImgWidth = aImgWidth;
	iImgMax = (size_t)(aImgMaxKB > 0 ? aImgMaxKB : 64) * 1024;
	iGzOut = false;
	iWireSink.iP = this;
	iState = SIdle;
	iReqLen = 0;
	iReqUsed = 0;
	iChunkLen = 0;
	iPsionClose = false;
	iOverflow = false;
	iInfPendLen = 0;
	iInfBad = false;
	PictureFree();
	}

void Proxy::Stop()
	{
	CloseUp();
	iInf.End();
	iGzw.End();
	PictureFree();
	iState = SClosed;
	iReqLen = 0;
	}

void Proxy::CloseUp()
	{
	if (iUpOpen)
		iHal.TcpClose();
	iUpOpen = false;
	}

size_t Proxy::FromPsion(const uint8_t* aData, size_t aLen)
	{
	if (iState == SClosed)
		return aLen;                         // (nowhere to go)
	size_t room = sizeof(iReq) - 1 - iReqLen;
	if (aLen > room)
		aLen = room;
	memcpy(iReq + iReqLen, aData, aLen);
	iReqLen += aLen;
	iReq[iReqLen] = 0;
	return aLen;
	}

// ----- to the Psion ----------------------------------------------------------

void Proxy::RawOut(const char* aData, size_t aLen)
	{
	if (!iOut)
		return;
	size_t n = iOut->Put((const uint8_t*)aData, aLen);
	iSent += n;
	if (n < aLen && !iOverflow)
		{
		// (Body() keeps room enough, so this should never happen.) What the
		// Psion has is now broken: the call ends after this answer rather
		// than go on out of step
		iOverflow = true;
		iPsionClose = true;
		iHal.Log("proxy: the buffer to the Psion overflowed; the call will end");
		}
	}

void Proxy::FlushChunk()
	{
	if (!iChunkLen)
		return;
	char h[12];
	int n = snprintf(h, sizeof(h), "%x\r\n", (unsigned)iChunkLen);
	RawOut(h, (size_t)n);
	RawOut(iChunk, iChunkLen);
	RawOut("\r\n", 2);
	iChunkLen = 0;
	}

// body bytes for the Psion: the simplifier's output (gzipped, if it can
// be), or a file passed through
void Proxy::Put(const char* aData, size_t aLen)
	{
	if (iGzOut)
		iGzw.Feed((const uint8_t*)aData, aLen);
	else
		Wire(aData, aLen);
	}

// body bytes as they go on the line: framed as chunks (unless the length is
// known: a file passed through as it is)
void Proxy::Wire(const char* aData, size_t aLen)
	{
	if (!iOutChunked)
		{
		RawOut(aData, aLen);
		return;
		}
	while (aLen)
		{
		size_t n = sizeof(iChunk) - iChunkLen;
		if (n > aLen)
			n = aLen;
		memcpy(iChunk + iChunkLen, aData, n);
		iChunkLen += n;
		aData += n;
		aLen -= n;
		if (iChunkLen == sizeof(iChunk))
			FlushChunk();
		}
	}

// text for HTML: < & " escaped, at most aMax - 1 bytes
static void Esc(char* aOut, size_t aMax, const char* aIn)
	{
	size_t n = 0;
	for (; *aIn && n + 7 < aMax; aIn++)
		{
		const char* e = *aIn == '<' ? "&lt;" : *aIn == '&' ? "&amp;" : *aIn == '"' ? "&quot;" : 0;
		if (e)
			{
			memcpy(aOut + n, e, strlen(e));
			n += strlen(e);
			}
		else
			aOut[n++] = *aIn;
		}
	aOut[n] = 0;
	}

// a short page of our own in place of the one asked for
void Proxy::ErrorPage(int aCode, const char* aWhat, const char* aDetail)
	{
	char url[160], detail[160];
	iStatus = aCode;
	Esc(url, sizeof(url), iUrl);
	Esc(detail, sizeof(detail), aDetail ? aDetail : "");
	int bl = snprintf(iScratch, sizeof(iScratch),
		"<html><head><title>Page not loaded</title></head><body>"
		"<h2>Page not loaded</h2><p>%s</p><p>%s</p><p>%s</p>"
		"<p><small>Atom modem %s web proxy</small></p></body></html>",
		aWhat, url, detail, kVersion);
	if (bl < 0 || bl >= (int)sizeof(iScratch))
		bl = (int)strlen(iScratch);
	char head[220];
	int hl = snprintf(head, sizeof(head),
		"HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\n"
		"Cache-Control: no-store\r\nConnection: %s\r\n\r\n",
		aCode, aCode == 504 ? "Gateway Timeout" : aCode == 400 ? "Bad Request"
			: aCode == 501 ? "Not Implemented" : aCode == 413 ? "Payload Too Large" : "Bad Gateway",
		bl, iPsionClose ? "close" : "keep-alive");
	RawOut(head, (size_t)hl);
	if (!EqNoCase(iMethod, "HEAD"))
		RawOut(iScratch, (size_t)bl);
	}

// http://psiproxy/: what the proxy is doing
void Proxy::StatusPage()
	{
	char mem[96];
	iStatus = 200;
	iHal.MemInfo(mem, sizeof(mem));
	static const char* const kModes[] = { "off", "simplified pages", "text only", "unchanged pages (TLS only)", "reader" };
	unsigned pct = iFetched ? (unsigned)((uint64_t)iSent * 100 / iFetched) : 0;
	int bl = snprintf(iScratch, sizeof(iScratch),
		"<html><head><title>Atom modem web proxy</title></head><body>"
		"<h2>Atom modem %s: web proxy</h2>"
		"<p>Mode: %s (AT$PX=%d)%s</p>"
		"<p>Requests: %lu<br>From servers: %lu bytes<br>To the Psion: %lu bytes (%u%%)<br>Pictures converted: %lu</p>"
		"<p>%s</p></body></html>",
		kVersion, kModes[iMode >= 0 && iMode <= 4 ? iMode : 0], iMode, iImgWidth ? ", pictures to 16 greys" : "",
		(unsigned long)iRequests, (unsigned long)iFetched, (unsigned long)iSent, pct, (unsigned long)iPictures, mem);
	if (bl < 0 || bl >= (int)sizeof(iScratch))
		bl = (int)strlen(iScratch);
	char head[200];
	int hl = snprintf(head, sizeof(head),
		"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\n"
		"Cache-Control: no-store\r\nConnection: %s\r\n\r\n", bl, iPsionClose ? "close" : "keep-alive");
	RawOut(head, (size_t)hl);
	if (!EqNoCase(iMethod, "HEAD"))
		RawOut(iScratch, (size_t)bl);
	}

// ----- the request -----------------------------------------------------------

// "http://host[:port]/path" or "https://...": iHost, iPort, iTls, iPath
bool Proxy::ParseUrl(const char* aUrl)
	{
	if (aUrl != iUrl)
		Copy(iUrl, sizeof(iUrl), aUrl, strlen(aUrl));
	const char* p;
	if (StartsNoCase(iUrl, "http://")) { iTls = false; iPort = 80; p = iUrl + 7; }
	else if (StartsNoCase(iUrl, "https://")) { iTls = true; iPort = 443; p = iUrl + 8; }
	else return false;
	const char* h = p;
	while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#')
		p++;
	if (p == h || (size_t)(p - h) >= sizeof(iHost))
		return false;
	Copy(iHost, sizeof(iHost), h, (size_t)(p - h));
	for (char* c = iHost; *c; c++)
		*c = (char)Lower((unsigned char)*c);
	if (strchr(iHost, '@'))
		return false;                        // (user@host: Links sends Authorization instead)
	if (*p == ':')
		{
		char* e;
		long port = strtol(p + 1, &e, 10);
		if (port <= 0 || port > 65535)
			return false;
		iPort = (uint16_t)port;
		p = e;
		}
	iPath = (*p == '/' || *p == '?') ? p : "/";
	return true;
	}

// a Location (absolute, scheme-relative, absolute-path or relative) against iUrl
bool Proxy::Resolve(const char* aRef, char* aOut, size_t aMax)
	{
	aRef = SkipSpace(aRef);
	if (StartsNoCase(aRef, "http://") || StartsNoCase(aRef, "https://"))
		return snprintf(aOut, aMax, "%s", aRef) < (int)aMax;
	const char* scheme = iTls ? "https:" : "http:";
	if (aRef[0] == '/' && aRef[1] == '/')
		return snprintf(aOut, aMax, "%s%s", scheme, aRef) < (int)aMax;
	char base[160];
	if ((iTls && iPort == 443) || (!iTls && iPort == 80))
		snprintf(base, sizeof(base), "%s//%s", scheme, iHost);
	else
		snprintf(base, sizeof(base), "%s//%s:%u", scheme, iHost, iPort);
	int n;
	if (aRef[0] == '/')
		n = snprintf(aOut, aMax, "%s%s", base, aRef);
	else
		{
		// relative to the current path's folder
		const char* end = iPath + strcspn(iPath, "?#");
		const char* slash = iPath;
		for (const char* q = iPath; q < end; q++)
			if (*q == '/')
				slash = q;
		n = snprintf(aOut, aMax, "%s%.*s/%s", base, (int)(slash - iPath), iPath, aRef);
		}
	if (n <= 0 || n >= (int)aMax)
		return false;
	// "/a/b/../c" is "/a/c", "/a/./b" is "/a/b" (RFC 3986 5.2.4), up to any ?query
	char* path = aOut + strlen(base);
	char* stop = path + strcspn(path, "?#");
	char* w = path;
	for (char* r = path; r < stop; )
		{
		// r is at a '/'
		char* seg = r + 1;
		char* e = seg;
		while (e < stop && *e != '/')
			e++;
		size_t len = (size_t)(e - seg);
		if (len == 1 && seg[0] == '.')
			{
			if (e == stop) *w++ = '/';
			}
		else if (len == 2 && seg[0] == '.' && seg[1] == '.')
			{
			while (w > path && *--w != '/') {}
			if (e == stop) *w++ = '/';
			}
		else
			{
			memmove(w, r, (size_t)(e - r));
			w += e - r;
			}
		r = e;
		}
	if (w == path)
		*w++ = '/';
	memmove(w, stop, strlen(stop) + 1);
	return true;
	}

void Proxy::Upgrade(const char* aHost)
	{
	if (Upgraded(aHost) || strlen(aHost) >= sizeof(iUpgrades[0]))
		return;
	strcpy(iUpgrades[iUpgradeNext], aHost);
	iUpgradeNext = (iUpgradeNext + 1) % 8;
	}

bool Proxy::Upgraded(const char* aHost) const
	{
	for (int i = 0; i < 8; i++)
		if (iUpgrades[i][0] && strcmp(iUpgrades[i], aHost) == 0)
			return true;
	return false;
	}

// the Content-Length in a request head (not yet taken apart), or 0
static long RequestLength(const char* aHead, size_t aLen)
	{
	for (size_t i = 0; i + 15 < aLen; i++)
		if ((i == 0 || aHead[i - 1] == '\n') && StartsNoCase(aHead + i, "content-length:"))
			return atol(SkipSpace(aHead + i + 15));
	return 0;
	}

// A whole request in iReq? Take it apart: method, address, the headers
// worth passing on, any body. False: not all here yet. True: iState is
// SConnect (fetch it), or SIdle with the answer already given.
bool Proxy::TakeRequest()
	{
	size_t skip = 0;                         // (stray CR/LF between requests)
	while (skip < iReqLen && (iReq[skip] == '\r' || iReq[skip] == '\n'))
		skip++;
	if (skip)
		{
		memmove(iReq, iReq + skip, iReqLen - skip + 1);
		iReqLen -= skip;
		}
	char* end = strstr(iReq, "\r\n\r\n");
	size_t headLen = end ? (size_t)(end - iReq) + 4 : 0;
	if (!end && (end = strstr(iReq, "\n\n")) != 0)
		headLen = (size_t)(end - iReq) + 2;
	iMethod[0] = 0;
	iUrl[0] = 0;
	if (!end)
		{
		if (iReqLen < sizeof(iReq) - 1)
			return false;
		// a head bigger than 4 KB (cookies): refused, and the call ends
		iPsionClose = true;
		iReqUsed = iReqLen;
		ErrorPage(400, "The request was too big for the Atom modem's proxy", "");
		return true;
		}
	long contentLength = RequestLength(iReq, headLen);
	if (contentLength < 0 || headLen + (size_t)contentLength >= sizeof(iReq))
		{
		iPsionClose = true;
		iReqUsed = iReqLen;
		ErrorPage(413, "The form was too big for the Atom modem's proxy", "");
		return true;
		}
	if (iReqLen < headLen + (size_t)contentLength)
		return false;                        // the body is still coming
	iReqUsed = headLen + (size_t)contentLength;
	iRequests++;
	iStartMs = iHal.Millis();
	iFetched0 = iFetched;
	iSent0 = iSent;
	// the request line (taken apart in place from here on)
	char* line = iReq;
	char* eol = strchr(line, '\n');
	*eol = 0;
	if (eol > line && eol[-1] == '\r') eol[-1] = 0;
	char* sp1 = strchr(line, ' ');
	char* sp2 = sp1 ? strchr(sp1 + 1, ' ') : 0;
	if (sp1) *sp1 = 0;
	if (sp2) *sp2 = 0;
	Copy(iMethod, sizeof(iMethod), line, strlen(line));
	const char* target = sp1 ? sp1 + 1 : "";
	const char* version = sp2 ? sp2 + 1 : "HTTP/1.0";
	iPsionClose = !StartsNoCase(version, "HTTP/1.1");
	Copy(iUrl, sizeof(iUrl), target, strlen(target));
	// the headers
	iUpHdrsLen = 0;
	iUpHdrs[0] = 0;
	iPsionGzip = false;
	char hostHdr[128] = "";
	static const char* const kPass[] = { "user-agent", "accept", "accept-language", "accept-charset",
		"cookie", "referer", "authorization", "content-type", "if-modified-since", "if-none-match",
		"origin", "range", "if-range", 0 };
	for (char* h = eol + 1; h < iReq + headLen; )
		{
		char* e = strchr(h, '\n');
		if (!e)
			break;
		*e = 0;
		if (e > h && e[-1] == '\r') e[-1] = 0;
		char* colon = strchr(h, ':');
		if (colon)
			{
			*colon = 0;
			const char* v = SkipSpace(colon + 1);
			if (EqNoCase(h, "host"))
				Copy(hostHdr, sizeof(hostHdr), v, strlen(v));
			else if (EqNoCase(h, "connection") || EqNoCase(h, "proxy-connection"))
				{
				if (ContainsNoCase(v, "close")) iPsionClose = true;
				else if (ContainsNoCase(v, "keep-alive")) iPsionClose = false;
				}
			else if (EqNoCase(h, "accept-encoding"))
				{
				iPsionGzip = ContainsNoCase(v, "gzip");
				// unchanged pages: the Psion's own gzip is asked for (fewer bytes on the line)
				if (iMode == ERaw)
					Append(iUpHdrs, iUpHdrsLen, sizeof(iUpHdrs), "Accept-Encoding", v);
				}
			else
				for (int i = 0; kPass[i]; i++)
					if (EqNoCase(h, kPass[i]))
						{
						Append(iUpHdrs, iUpHdrsLen, sizeof(iUpHdrs), h, v);
						break;
						}
			}
		h = e + 1;
		}
	iBody = iReq + headLen;
	iBodyLen = (size_t)contentLength;
	// origin-form ("GET / HTTP/1.1" with Host: psiproxy): the proxy itself
	if (iUrl[0] == '/')
		{
		if (!hostHdr[0] || StartsNoCase(hostHdr, kName))
			{
			StatusPage();
			return true;
			}
		size_t hl = strlen(hostHdr), ul = strlen(iUrl);
		if (7 + hl + ul >= sizeof(iUrl))
			{
			ErrorPage(400, "The address is too long for the Atom modem's proxy", "");
			return true;
			}
		memmove(iUrl + 7 + hl, iUrl, ul + 1);
		memcpy(iUrl, "http://", 7);
		memcpy(iUrl + 7, hostHdr, hl);
		}
	if (EqNoCase(iMethod, "CONNECT"))
		{
		iPsionClose = true;                  // (a client that tunnels gives up on this connection)
		ErrorPage(501, "The Atom modem's proxy fetches https:// pages itself: ask for them as plain requests", "");
		return true;
		}
	if (!ParseUrl(iUrl))
		{
		ErrorPage(400, "The address is not one the Atom modem's proxy can fetch", "");
		return true;
		}
	if (EqNoCase(iHost, kName))
		{
		StatusPage();
		return true;
		}
	iRedirects = 0;
	iRedirected = false;
	iState = SConnect;
	return true;
	}

// the answer has been given: the next request (perhaps already here)
void Proxy::NextRequest()
	{
	char m[200];
	snprintf(m, sizeof(m), "proxy: %s %.100s -> %d, %lu bytes in, %lu out, %lu ms", iMethod, iUrl, iStatus,
		(unsigned long)(iFetched - iFetched0), (unsigned long)(iSent - iSent0),
		(unsigned long)(iHal.Millis() - iStartMs));
	iHal.Log(m);
	if (iReqUsed > iReqLen)
		iReqUsed = iReqLen;
	memmove(iReq, iReq + iReqUsed, iReqLen - iReqUsed + 1);
	iReqLen -= iReqUsed;
	iReqUsed = 0;
	iState = iPsionClose ? SClosed : SIdle;
	iOverflow = false;
	}

// ----- upstream --------------------------------------------------------------

bool Proxy::Send(const char* aData, size_t aLen)
	{
	uint32_t start = iHal.Millis();
	while (aLen)
		{
		size_t n = iHal.TcpWrite((const uint8_t*)aData, aLen);
		aData += n;
		aLen -= n;
		if (!n)
			{
			if (!iHal.TcpOpen() || iHal.Millis() - start > 10000)
				return false;
			iHal.Idle();
			}
		}
	return true;
	}

// connect (or use the open connection again) and send the request
bool Proxy::ConnectAndSend()
	{
	// http:// to a site seen to move to https:// before: https:// at once
	if (!iTls && iPort == 80 && iUrl[7 + strlen(iHost)] != ':' && Upgraded(iHost))
		{
		if (snprintf(iScratch, sizeof(iScratch), "https://%s", iUrl + 7) < (int)sizeof(iScratch))
			ParseUrl(iScratch);
		}
	iUpReused = iUpOpen && iUpTls == iTls && iUpPort == iPort && strcmp(iUpHost, iHost) == 0 && iHal.TcpOpen();
	if (!iUpReused)
		{
		CloseUp();
		iWhy[0] = 0;
		if (!iHal.UpConnect(iHost, iPort, iTls, iWhy, sizeof(iWhy)))
			{
			ErrorPage(502, iTls ? "The secure connection to the server failed" : "The server could not be reached",
				iWhy);
			return false;
			}
		iUpOpen = true;
		Copy(iUpHost, sizeof(iUpHost), iHost, strlen(iHost));
		iUpPort = iPort;
		iUpTls = iTls;
		}
	// the request line and Host (a #fragment is never sent)
	size_t pl = strcspn(iPath, "#");
	int n;
	if ((iTls && iPort == 443) || (!iTls && iPort == 80))
		n = snprintf(iScratch, sizeof(iScratch), "%s %s%.*s HTTP/1.1\r\nHost: %s\r\n",
			iMethod, iPath[0] == '?' ? "/" : "", (int)pl, iPath, iHost);
	else
		n = snprintf(iScratch, sizeof(iScratch), "%s %s%.*s HTTP/1.1\r\nHost: %s:%u\r\n",
			iMethod, iPath[0] == '?' ? "/" : "", (int)pl, iPath, iHost, iPort);
	if (n <= 0 || n >= (int)sizeof(iScratch))
		{
		ErrorPage(400, "The address is too long for the Atom modem's proxy", "");
		return false;
		}
	bool post = iBodyLen && !EqNoCase(iMethod, "GET") && !EqNoCase(iMethod, "HEAD");
	bool ok = Send(iScratch, (size_t)n) && Send(iUpHdrs, iUpHdrsLen);
	// (simplified pages: the proxy reads the HTML, so plain is quickest for it)
	if (ok && iMode != ERaw)
		ok = Send("Accept-Encoding: identity\r\n", 27);
	if (ok && post)
		{
		n = snprintf(iScratch, sizeof(iScratch), "Content-Length: %u\r\n", (unsigned)iBodyLen);
		ok = Send(iScratch, (size_t)n);
		}
	ok = ok && Send("Connection: keep-alive\r\n\r\n", 26);
	if (ok && post)
		ok = Send(iBody, iBodyLen);
	if (!ok)
		{
		CloseUp();
		if (iUpReused)
			return ConnectAndSend();             // the kept connection had gone: once more, afresh
		ErrorPage(502, "The connection to the server broke", "");
		return false;
		}
	// ready for the response
	iInPos = iInLen = 0;
	iLineLen = 0;
	iLineLong = false;
	iStatus = 0;
	iReason[0] = 0;
	iType[0] = 0;
	iLoc[0] = 0;
	iFwdLen = 0;
	iFwd[0] = 0;
	iLength = -1;
	iChunked = false;
	iEncoding = 0;
	iUpClose = false;
	iGotHead = false;
	iHeadBytes = 0;
	iLastUpMs = iHal.Millis();
	iState = SHead;
	return true;
	}

// one line of the response head
void Proxy::HeadLine(char* aLine)
	{
	if (!iStatus)
		{
		// "HTTP/1.1 200 OK"
		const char* p = strchr(aLine, ' ');
		iStatus = p ? atoi(p + 1) : 0;
		if (iStatus <= 0)
			iStatus = 502;
		const char* r = p ? strchr(p + 1, ' ') : 0;
		Copy(iReason, sizeof(iReason), r ? r + 1 : "", r ? strlen(r + 1) : 0);
		if (StartsNoCase(aLine, "HTTP/1.0"))
			iUpClose = true;
		return;
		}
	char* colon = strchr(aLine, ':');
	if (!colon)
		return;
	*colon = 0;
	const char* v = SkipSpace(colon + 1);
	const char* h = aLine;
	if (EqNoCase(h, "content-type"))
		Copy(iType, sizeof(iType), v, strlen(v));
	else if (EqNoCase(h, "content-length"))
		iLength = atol(v);
	else if (EqNoCase(h, "transfer-encoding"))
		iChunked = ContainsNoCase(v, "chunked");
	else if (EqNoCase(h, "content-encoding"))
		iEncoding = ContainsNoCase(v, "gzip") ? 1 : ContainsNoCase(v, "deflate") ? 2
			: (EqNoCase(v, "identity") || !*v) ? 0 : 3;
	else if (EqNoCase(h, "location"))
		Copy(iLoc, sizeof(iLoc), v, strlen(v));
	else if (EqNoCase(h, "connection"))
		{
		if (ContainsNoCase(v, "close"))
			iUpClose = true;
		}
	else
		{
		static const char* const kPass[] = { "set-cookie", "cache-control", "expires", "last-modified",
			"etag", "www-authenticate", "content-disposition", "refresh", "content-range", "accept-ranges",
			"date", 0 };
		for (int i = 0; kPass[i]; i++)
			if (EqNoCase(h, kPass[i]))
				{
				Append(iFwd, iFwdLen, sizeof(iFwd), h, v);   // (dropped if there is no room)
				break;
				}
		}
	}

// The head is complete: follow a redirect, or answer the Psion. False: this
// response is finished with already (a redirect, or an error page sent).
bool Proxy::HeadDone()
	{
	if (iStatus >= 100 && iStatus < 200 && iStatus != 101)
		{
		iStatus = 0;                         // 100 Continue and the like: the real head follows
		iFwdLen = 0;
		return true;
		}
	// Content-Length and chunked both: the length is ignored (RFC 9112 6.3)
	if (iChunked)
		iLength = -1;
	bool get = EqNoCase(iMethod, "GET") || EqNoCase(iMethod, "HEAD");
	bool redirect = (iStatus == 301 || iStatus == 302 || iStatus == 303 || iStatus == 307 || iStatus == 308)
		&& iLoc[0] && iRedirects < kMaxRedirects && (get || iStatus <= 303);
	if (redirect && Resolve(iLoc, iScratch, sizeof(iScratch)))
		{
		char oldHost[sizeof(iHost)];
		strcpy(oldHost, iHost);
		bool wasTls = iTls;
		if (ParseUrl(iScratch))
			{
			if (!wasTls && iTls && strcmp(oldHost, iHost) == 0)
				Upgrade(iHost);
			if (!get)
				{
				strcpy(iMethod, "GET");      // a form's answer is fetched with GET
				iBodyLen = 0;
				}
			iRedirects++;
			iRedirected = true;
			CloseUp();                       // (the redirect's own body is not wanted)
			iState = SConnect;
			return false;
			}
		}
	iHasBody = !EqNoCase(iMethod, "HEAD") && iStatus != 204 && iStatus != 304 && iStatus >= 200;
	const char* type = SkipSpace(iType);
	bool html = StartsNoCase(type, "text/html") || StartsNoCase(type, "application/xhtml");
	iTransform = iHasBody && html && iMode != ERaw && iEncoding != 3;
	iInflate = iTransform && iEncoding != 0;
	iInflateStarted = iInflateDone = false;
	iInfPendLen = 0;
	iInfBad = false;
	iGz = 0;
	iGzStep = iEncoding == 1 ? 0 : 10;       // gzip: its header is skipped first
	iGzFlags = 0;
	iOutChunked = iHasBody && (iTransform || iLength < 0);
	// gzip for the line (its header waits in the writer until the first body bytes)
	iGzOut = iTransform && iZip && iPsionGzip && iGzw.Begin(&iWireSink);
	iLeft = iLength;
	iChunkState = CSize;
	iChunkLeft = 0;
	iBodyDone = !iHasBody || (!iChunked && iLength == 0);
	if (!iHasBody && !iChunked && iLength > 0)
		iUpClose = true;                     // (a HEAD answer's length is not a body)
	iLineLen = 0;                            // (counts chunk trailer lines from here)
	iState = SBody;
	// a picture to convert: the head waits until it is known whether it can be
	if (PictureStart())
		return true;
	SendHead();
	return true;
	}

// the head for the Psion, sent in pieces (for a picture: the original's,
// when it is passed through after all)
void Proxy::SendHead()
	{
	char line[200];
	int n = snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", iStatus, iReason[0] ? iReason : "OK");
	RawOut(line, (size_t)n);
	if (iType[0])
		{
		RawOut("Content-Type: ", 14);
		RawOut(iType, strlen(iType));
		RawOut("\r\n", 2);
		}
	if (iLoc[0])
		{
		// a redirect not followed (too many, or a form sent on): Links follows it
		RawOut("Location: ", 10);
		RawOut(iLoc, strlen(iLoc));
		RawOut("\r\n", 2);
		}
	if (iHasBody && !iTransform && iEncoding)
		RawOut(iEncoding == 1 ? "Content-Encoding: gzip\r\n" : "Content-Encoding: deflate\r\n",
			iEncoding == 1 ? 24 : 27);
	if (iGzOut)
		RawOut("Content-Encoding: gzip\r\n", 24);
	RawOut(iFwd, iFwdLen);
	if (iOutChunked)
		n = snprintf(line, sizeof(line), "Transfer-Encoding: chunked\r\n");
	else if (iHasBody)
		n = snprintf(line, sizeof(line), "Content-Length: %ld\r\n", iLength);
	else if (iStatus != 304 && iStatus != 204)
		n = snprintf(line, sizeof(line), "Content-Length: 0\r\n");
	else
		n = 0;
	n += snprintf(line + n, sizeof(line) - n, "Connection: %s\r\n\r\n", iPsionClose ? "close" : "keep-alive");
	RawOut(line, (size_t)n);
	iChunkLen = 0;
	if (iTransform)
		iSimp.Begin(this, iMode == EText ? HtmlSimplifier::ETextOnly : iMode == EReader ? HtmlSimplifier::EReader
			: HtmlSimplifier::EKeepPictures, iRedirected ? iUrl : 0);
	}

// ----- pictures ----------------------------------------------------------------

// a picture that can be converted? Then its bytes are gathered (true)
bool Proxy::PictureStart()
	{
	iImgMode = false;
	if (!iImgWidth || !iHasBody || iStatus != 200 || iEncoding != 0 || !EqNoCase(iMethod, "GET"))
		return false;
	const char* type = SkipSpace(iType);
	if (!(StartsNoCase(type, "image/jpeg") || StartsNoCase(type, "image/png") || StartsNoCase(type, "image/gif")))
		return false;
	if (iLength >= 0 && (size_t)iLength > iImgMax)
		return false;                        // too big to hold: passed through
	size_t cap = iLength > 0 ? (size_t)iLength : iImgMax;
	PictureFree();
	iImgBuf = (uint8_t*)malloc(cap);
	if (!iImgBuf)
		{
		iHal.Log("picture: no memory to convert it; passed through");
		return false;
		}
	iImgCap = cap;
	iImgLen = 0;
	iImgMode = true;
	iImgMs = iHal.Millis();
	return true;
	}

void Proxy::PictureFree()
	{
	if (iImgBuf)
		free(iImgBuf);
	iImgBuf = 0;
	iImgLen = iImgCap = 0;
	iImgMode = false;
	if (iGifBuf)
		free(iGifBuf);
	iGifBuf = 0;
	iGifLen = iGifPos = 0;
	iImg.Reset();
	}

void Proxy::PictureBytes(const uint8_t* aP, size_t aLen)
	{
	if (iImgLen + aLen > iImgCap)
		{
		PicturePass();                       // bigger than it said, or than allowed: as it is
		Wire((const char*)aP, aLen);
		return;
		}
	memcpy(iImgBuf + iImgLen, aP, aLen);
	iImgLen += aLen;
	}

// the picture is passed through after all: the original head, then what
// was gathered (the rest follows as it comes)
void Proxy::PicturePass()
	{
	iImgMode = false;
	iOutChunked = iLength < 0;
	SendHead();
	if (iImgLen)
		Wire((const char*)iImgBuf, iImgLen);
	free(iImgBuf);
	iImgBuf = 0;
	iImgLen = iImgCap = 0;
	}

// the whole picture is here: decoded, or passed through
void Proxy::PictureDone()
	{
	char why[64];
	bool ok = iImg.Decode(iImgBuf, iImgLen, iImgWidth, why, sizeof(why));
	char m[200];
	if (!ok)
		{
		snprintf(m, sizeof(m), "picture %.60s: not converted (%s), %lu bytes passed through", iUrl, why,
			(unsigned long)iImgLen);
		iHal.Log(m);
		PicturePass();
		FinishBody();
		return;
		}
	free(iImgBuf);                       // the source; the decoder keeps its own copy
	iImgBuf = 0;
	iImgCap = 0;
	// Gather the whole GIF so it goes to the Psion with a Content-Length, not
	// chunked: PsiWeb's Links fetch mishandles a chunked image body (text, sent
	// with a length, is fine, and so is the Linux proxy, which never chunks).
	// gcap is a safe upper bound for the LZW of a 16-grey image (well under
	// 1.25*pixels + headers), so Write cannot overflow it; with no memory for
	// it (a big picture), fall back to the old chunked stream.
	size_t gcap = (size_t)iImg.Width() * iImg.Height() * 5 / 4 + 2048;
	uint8_t* g = (uint8_t*)malloc(gcap);
	bool buffered = false;
	if (g)
		{
		GifBuf sink(g, gcap);
		while (!iImg.Write(&sink, 64))   // Write returns true when the whole GIF is out
			;
		iGifBuf = g;
		iGifLen = sink.iLen;
		iGifPos = 0;
		buffered = true;
		}
	iOutChunked = !buffered;
	int n = snprintf(iScratch, sizeof(iScratch), "HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\n");
	RawOut(iScratch, (size_t)n);
	RawOut(iFwd, iFwdLen);
	if (buffered)
		n = snprintf(iScratch, sizeof(iScratch), "Content-Length: %lu\r\nConnection: %s\r\n\r\n",
			(unsigned long)iGifLen, iPsionClose ? "close" : "keep-alive");
	else
		n = snprintf(iScratch, sizeof(iScratch), "Transfer-Encoding: chunked\r\nConnection: %s\r\n\r\n",
			iPsionClose ? "close" : "keep-alive");
	RawOut(iScratch, (size_t)n);
	iChunkLen = 0;
	iPictures++;
	// the server's connection: kept if the body ended cleanly, as for any response
	bool clean = !iUpClose && (iChunked ? iChunkState == CDone : (iLength >= 0 && iLeft <= 0)) && iInPos >= iInLen;
	if (!clean)
		CloseUp();
	iState = SPicture;
	}

// rows of the GIF while the ring has room; then the end of the response
void Proxy::PictureOut()
	{
	if (iGifBuf)                         // Content-Length: stream the gathered GIF, paced to the ring
		{
		while (iGifPos < iGifLen)
			{
			size_t room = iOut->Free();
			if (room <= kSlack)
				return;
			size_t n = iGifLen - iGifPos;
			if (n > room - kSlack)
				n = room - kSlack;
			RawOut((const char*)(iGifBuf + iGifPos), n);
			iGifPos += n;
			}
		char m[200];
		snprintf(m, sizeof(m), "picture %.60s: %dx%d -> %dx%d gif, %lu -> %lu bytes, %lu ms", iUrl,
			iImg.SourceWidth(), iImg.SourceHeight(), iImg.Width(), iImg.Height(), (unsigned long)iImgLen,
			(unsigned long)iGifLen, (unsigned long)(iHal.Millis() - iImgMs));
		iHal.Log(m);
		PictureFree();
		NextRequest();
		return;
		}
	while (iOut->Free() >= kSlack)       // chunked fallback (no memory to gather the GIF)
		{
		if (!iImg.Write(this, 8))
			continue;
		FlushChunk();
		RawOut("0\r\n\r\n", 5);
		char m[200];
		snprintf(m, sizeof(m), "picture %.60s: %dx%d -> %dx%d gif, %lu -> %lu bytes, %lu ms", iUrl,
			iImg.SourceWidth(), iImg.SourceHeight(), iImg.Width(), iImg.Height(), (unsigned long)iImgLen,
			(unsigned long)iImg.Written(), (unsigned long)(iHal.Millis() - iImgMs));
		iHal.Log(m);
		PictureFree();
		NextRequest();
		return;
		}
	}

// reads the response head; true once it is complete (iState then SBody,
// SConnect for a redirect, or SIdle with an error page sent)
bool Proxy::ReadHead()
	{
	for (;;)
		{
		if (iInPos >= iInLen)
			{
			size_t avail = iHal.TcpAvailable();
			if (!avail)
				{
				if (!iHal.TcpOpen())
					{
					CloseUp();
					if (iUpReused && !iGotHead)
						{
						iUpReused = false;
						iState = SConnect;   // a kept connection the server had closed: afresh
						return true;
						}
					iState = SIdle;
					ErrorPage(502, "The server closed the connection", "");
					return true;
					}
				if (iHal.Millis() - iLastUpMs > kIdleMs)
					{
					CloseUp();
					iState = SIdle;
					ErrorPage(504, "The server did not answer", "");
					return true;
					}
				return false;
				}
			iInLen = iHal.TcpRead(iIn, avail < sizeof(iIn) ? avail : sizeof(iIn));
			iInPos = 0;
			if (!iInLen)
				return false;
			iLastUpMs = iHal.Millis();
			iGotHead = true;
			}
		while (iInPos < iInLen)
			{
			if (++iHeadBytes > kMaxHead)
				{
				CloseUp();
				iState = SIdle;
				ErrorPage(502, "The server's answer had too many headers for the Atom modem's proxy", "");
				return true;
				}
			char c = (char)iIn[iInPos++];
			if (c != '\n')
				{
				if (iLineLen < sizeof(iLine) - 1)
					iLine[iLineLen++] = c;
				else
					iLineLong = true;            // a header too long to keep: dropped
				continue;
				}
			if (iLineLen && iLine[iLineLen - 1] == '\r')
				iLineLen--;
			iLine[iLineLen] = 0;
			bool blank = iLineLen == 0 && !iLineLong;
			if (!blank && !iLineLong)
				HeadLine(iLine);
			iLineLen = 0;
			iLineLong = false;
			if (!blank || !iStatus)
				continue;                        // (blank lines before the status line)
			if (HeadDone())
				{
				if (iState == SBody)
					return true;
				continue;                        // (100 Continue: the next head)
				}
			if (iState == SConnect)
				return true;                     // a redirect
			iState = SIdle;
			return true;
			}
		}
	}

// ----- the body ----------------------------------------------------------------

// skips the gzip header (RFC 1952) ahead of the deflate data; true once
// the deflate data begins (at aP, aN bytes; aUsed header bytes passed)
bool Proxy::GzipHeader(const uint8_t*& aP, size_t& aN, size_t& aUsed)
	{
	aUsed = 0;
	while (aN && iGzStep < 10)
		{
		uint8_t c = *aP;
		switch (iGzStep)
			{
		case 0:                              // ID1 ID2 CM FLG MTIME(4) XFL OS
			if (iGz == 3) iGzFlags = c;
			if (++iGz == 10) { iGz = 0; iGzStep = 1; }
			break;
		case 1:                              // FEXTRA: XLEN, then that many bytes
			if (!(iGzFlags & 4)) { iGzStep = 3; continue; }
			if (iGz == 0) { iGz = 1; iChunkLeftGz = c; break; }
			iChunkLeftGz |= (unsigned)c << 8;
			iGz = 0;
			iGzStep = 2;
			break;
		case 2:
			if (!iChunkLeftGz) { iGzStep = 3; continue; }
			iChunkLeftGz--;
			break;
		case 3:                              // FNAME, zero-terminated
			if (!(iGzFlags & 8)) { iGzStep = 4; continue; }
			if (!c) iGzStep = 4;
			break;
		case 4:                              // FCOMMENT
			if (!(iGzFlags & 16)) { iGzStep = 5; continue; }
			if (!c) iGzStep = 5;
			break;
		case 5:                              // FHCRC
			if (!(iGzFlags & 2)) { iGzStep = 10; continue; }
			if (++iGz == 2) iGzStep = 10;
			break;
			}
		aP++;
		aN--;
		aUsed++;
		}
	return iGzStep == 10;
	}

// the body bytes waiting in iIn (after any chunk framing), or 0
size_t Proxy::BodySpan(const uint8_t*& aP)
	{
	aP = iIn + iInPos;
	if (!iChunked)
		{
		size_t n = iInLen - iInPos;
		if (iLeft >= 0 && (long)n > iLeft)
			n = (size_t)iLeft;
		return n;
		}
	while (iInPos < iInLen)
		{
		uint8_t c = iIn[iInPos];
		switch (iChunkState)
			{
		case CSize:
			{
			int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
				: (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
			if (d >= 0)
				{
				if (iChunkLeft < 0x1000000UL)
					iChunkLeft = iChunkLeft * 16 + (unsigned long)d;
				}
			else if (c == '\n')
				iChunkState = iChunkLeft ? CData : CTrailer;
			else if (c != '\r')
				iChunkState = CExt;          // ";name=value" after the size
			break;
			}
		case CExt:
			if (c == '\n')
				iChunkState = iChunkLeft ? CData : CTrailer;
			break;
		case CData:
			{
			aP = iIn + iInPos;
			size_t n = iInLen - iInPos;
			return n > iChunkLeft ? (size_t)iChunkLeft : n;
			}
		case CDataCr:
			if (c == '\n')
				{
				iChunkState = CSize;
				iChunkLeft = 0;
				}
			break;
		case CTrailer:
			// trailer lines, up to an empty one (iLineLen: this line's length)
			if (c == '\n')
				{
				if (iLineLen == 0)
					{
					iInPos++;
					iChunkState = CDone;
					iBodyDone = true;
					return 0;
					}
				iLineLen = 0;
				}
			else if (c != '\r')
				iLineLen++;
			break;
		default:
			iBodyDone = true;
			return 0;
			}
		iInPos++;
		}
	return 0;
	}

void Proxy::Deliver(const uint8_t* aP, size_t aLen)
	{
	if (iImgMode)
		PictureBytes(aP, aLen);
	else if (iTransform)
		iSimp.Feed(aP, aLen);
	else
		Put((const char*)aP, aLen);
	}

void Proxy::Body()
	{
	// while the ring is nearly full nothing is read: the server waits (TCP
	// flow control), as for an ordinary connection. Each pass hands on only
	// as much as the room left can take, whatever the simplifier and the
	// gzip writer make of it; the rest waits for the next pass
	while (iOut->Free() >= kSlack && !iBodyDone)
		{
		size_t slice = (iOut->Free() - kMargin) / kGrowth;
		if (iInfPendLen)
			{
			// unpacked bytes still waiting for the simplifier
			size_t n = iInfPendLen < slice ? iInfPendLen : slice;
			iSimp.Feed(iInfPend, n);
			iInfPend += n;
			iInfPendLen -= n;
			if (!iInfPendLen && iInfBad)
				{
				iInfBad = false;
				iInf.End();
				iSimp.Feed((const uint8_t*)"<p>[the rest of the page was damaged]</p>", 41);
				}
			continue;
			}
		if (!iChunked && iLeft == 0)
			{
			// the length is all here (anything after it is not this body's:
			// FinishResponse closes the connection)
			iBodyDone = true;
			break;
			}
		if (iInPos >= iInLen)
			{
			size_t avail = iHal.TcpAvailable();
			if (!avail)
				{
				if (!iHal.TcpOpen() || iHal.Millis() - iLastUpMs > kIdleMs)
					{
					// the end of a body that runs to the close (or one cut short)
					iUpClose = true;
					iBodyDone = true;
					}
				break;
				}
			iInLen = iHal.TcpRead(iIn, avail < sizeof(iIn) ? avail : sizeof(iIn));
			iInPos = 0;
			if (!iInLen)
				break;
			iLastUpMs = iHal.Millis();
			}
		const uint8_t* p;
		size_t n = BodySpan(p);
		if (!n)
			continue;
		size_t used = n;
		if (iInflate)
			{
			size_t hdr = 0;
			const uint8_t* q = p;
			size_t m = n;
			if (!GzipHeader(q, m, hdr) || iInflateDone || !m)
				used = iInflateDone ? n : hdr;   // (header bytes; or the trailer after the end)
			else
				{
				if (!iInflateStarted)
					{
					iInflateStarted = true;
					// deflate is nearly always zlib-wrapped (0x78...): a raw stream
					// cannot start with a low nibble of 8
					if (!iInf.Begin(iEncoding == 2 && (q[0] & 0x0f) == 8))
						{
						iInflateDone = true;
						iSimp.Feed((const uint8_t*)"<p>Not enough memory in the Atom modem to unpack this page</p>", 61);
						}
					}
				if (!iInflateDone)
					{
					size_t in = 0, outLen = 0;
					const uint8_t* out;
					int st = iInf.Run(q, m, in, out, outLen);
					// (fed to the simplifier in slices, from the top of the loop:
					// the inflater is not run again until all of it has gone)
					iInfPend = out;
					iInfPendLen = outLen;
					used = hdr + in;
					if (st != 0 || (!in && !outLen))
						{
						iInflateDone = true;
						if (st < 0 || (!in && !outLen))
							{
							if (iInfPendLen)
								iInfBad = true;  // (said once what came before it has gone)
							else
								{
								iInf.End();
								iSimp.Feed((const uint8_t*)"<p>[the rest of the page was damaged]</p>", 41);
								}
							}
						// (otherwise iInf's window holds the last of the page,
						// freed by FinishResponse once it has gone)
						}
					}
				else
					used = n;
				}
			}
		else
			{
			if (n > slice)
				n = used = slice;
			Deliver(p, n);
			}
		iFetched += used;
		iInPos += used;
		if (iChunked)
			{
			iChunkLeft -= used;
			if (!iChunkLeft)
				iChunkState = CDataCr;
			}
		else if (iLeft > 0)
			iLeft -= (long)used;
		}
	if (iBodyDone)
		FinishResponse();
	else if (iOutChunked && iOut->Count() < 256)
		{
		// the Psion is waiting: what there is, now
		if (iGzOut)
			iGzw.Flush();
		FlushChunk();
		}
	}

void Proxy::FinishResponse()
	{
	if (iImgMode)
		{
		PictureDone();                       // (ends the response itself, now or row by row)
		return;
		}
	FinishBody();
	}

void Proxy::FinishBody()
	{
	if (iTransform)
		iSimp.End();
	if (iGzOut)
		iGzw.Finish();
	iGzOut = false;
	iInf.End();
	iInfPendLen = 0;
	iInfBad = false;
	if (iOutChunked)
		{
		FlushChunk();
		RawOut("0\r\n\r\n", 5);
		}
	// keep the server's connection for the next request if the body ended cleanly
	// a body sent with its length that ended short: the Psion still waits
	// for the rest, and would take the next answer as part of this one; the
	// call ends instead
	if (!iOutChunked && iHasBody && iLeft > 0)
		iPsionClose = true;
	bool clean = !iUpClose && (iChunked ? iChunkState == CDone : (iLength >= 0 && iLeft <= 0))
		&& iInPos >= iInLen;
	if (!clean)
		CloseUp();
	NextRequest();
	}

bool Proxy::Pump(Ring& aOut)
	{
	iOut = &aOut;
	for (int step = 0; step < 4; step++)
		{
		switch (iState)
			{
		case SClosed:
			return aOut.Count() > 0;              // (the call ends once the answer is out)
		case SIdle:
			if (aOut.Free() < kSlack || !iReqLen)
				return true;
			if (!TakeRequest())
				return true;
			if (iState == SIdle)
				NextRequest();                    // answered at once (status page, error)
			break;
		case SConnect:
			if (!ConnectAndSend())
				NextRequest();                    // (an error page was sent)
			break;
		case SHead:
			if (!ReadHead())
				return true;
			if (iState == SIdle)
				NextRequest();
			break;
		case SBody:
			Body();
			if (iState != SPicture)
				return iState != SClosed || aOut.Count() > 0;
			break;
		case SPicture:
			PictureOut();
			return iState != SClosed || aOut.Count() > 0;
			}
		}
	return true;
	}

} // namespace am
