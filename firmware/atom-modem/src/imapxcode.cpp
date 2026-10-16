// imapxcode.cpp - see imapxcode.h. The IMAP-aware image-transcoding filter
// for the Atom's TLS relay. Resumable byte-in / byte-out; drops to a
// transparent pass-through on any uncertainty. Plain C++, no Arduino calls:
// the Atom and the host tests run the same code.
// MIT licence (see LICENSE at the top of the repository).
#include "imapxcode.h"
#include "imgconv.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#if defined(BOARD_HAS_PSRAM)
#include <Arduino.h>                      // psramFound (the AtomS3R build)
#include <esp_heap_caps.h>
#endif

namespace am {

// a large buffer from PSRAM on the AtomS3R (the message, the transcoded GIF),
// internal heap elsewhere / on the host. free() releases either.
static void* xmalloc(size_t aCap)
	{
#if defined(BOARD_HAS_PSRAM)
	if (psramFound())
		{
		void* p = heap_caps_malloc(aCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (p) return p;
		}
#endif
	return malloc(aCap);
	}

// transfer encodings, from the name in the BODYSTRUCTURE
enum { EIX_7BIT = 0, EIX_BASE64 = 1, EIX_QP = 2 };

// ===== small self-contained helpers ===========================================

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ci_cmp(const char* a, const char* b)
	{
	while (*a && *b)
		{
		int x = lc((unsigned char)*a), y = lc((unsigned char)*b);
		if (x != y) return x - y;
		a++; b++;
		}
	return lc((unsigned char)*a) - lc((unsigned char)*b);
	}

// case-insensitive substring search in a byte range; (size_t)-1 if not found
static size_t find_ci(const uint8_t* b, size_t p, size_t end, const char* needle)
	{
	size_t nl = strlen(needle);
	if (nl == 0 || end < nl) return (size_t)-1;
	for (size_t i = p; i + nl <= end; i++)
		{
		size_t k = 0;
		for (; k < nl; k++)
			if (lc(b[i + k]) != lc((unsigned char)needle[k])) break;
		if (k == nl) return i;
		}
	return (size_t)-1;
	}

static int enc_from_name(const char* n)
	{
	if (!ci_cmp(n, "base64")) return EIX_BASE64;
	if (!ci_cmp(n, "quoted-printable")) return EIX_QP;
	return EIX_7BIT;
	}

// ----- base64 and quoted-printable --------------------------------------------

static const char kB64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t* in, size_t n, uint8_t* out, size_t* outLen)
	{
	size_t o = 0, i = 0;
	for (; i + 3 <= n; i += 3)
		{
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
		out[o++] = kB64[(v >> 18) & 63]; out[o++] = kB64[(v >> 12) & 63];
		out[o++] = kB64[(v >> 6) & 63];  out[o++] = kB64[v & 63];
		}
	if (n - i == 1)
		{
		uint32_t v = (uint32_t)in[i] << 16;
		out[o++] = kB64[(v >> 18) & 63]; out[o++] = kB64[(v >> 12) & 63];
		out[o++] = '='; out[o++] = '=';
		}
	else if (n - i == 2)
		{
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
		out[o++] = kB64[(v >> 18) & 63]; out[o++] = kB64[(v >> 12) & 63];
		out[o++] = kB64[(v >> 6) & 63];  out[o++] = '=';
		}
	*outLen = o;
	}

static int b64_val(int c)
	{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
	}

static bool b64_decode(const uint8_t* in, size_t n, uint8_t* out, size_t cap, size_t* outLen)
	{
	size_t o = 0; unsigned acc = 0; int bits = 0;
	for (size_t i = 0; i < n; i++)
		{
		int c = in[i];
		if (c == '=') break;
		int v = b64_val(c);
		if (v < 0) continue;                 // whitespace / line breaks
		acc = ((acc << 6) | (unsigned)v) & 0xfffff; bits += 6;
		if (bits >= 8)
			{
			bits -= 8;
			if (o >= cap) return false;
			out[o++] = (uint8_t)((acc >> bits) & 0xff);
			}
		}
	*outLen = o;
	return true;
	}

static int hex_val(int c)
	{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
	}

static bool qp_decode(const uint8_t* in, size_t n, uint8_t* out, size_t cap, size_t* outLen)
	{
	size_t o = 0;
	for (size_t i = 0; i < n; i++)
		{
		uint8_t c = in[i];
		if (c == '=')
			{
			if (i + 2 < n)
				{
				int h = hex_val(in[i + 1]), l = hex_val(in[i + 2]);
				if (h >= 0 && l >= 0) { if (o >= cap) return false; out[o++] = (uint8_t)((h << 4) | l); i += 2; continue; }
				}
			if (i + 2 < n && in[i + 1] == '\r' && in[i + 2] == '\n') { i += 2; continue; }   // soft break
			if (i + 1 < n && in[i + 1] == '\n') { i += 1; continue; }
			if (o >= cap) return false;
			out[o++] = c;                    // a lone '='
			}
		else { if (o >= cap) return false; out[o++] = c; }
		}
	*outLen = o;
	return true;
	}

static bool decode_body(int enc, const uint8_t* in, size_t n, uint8_t* out, size_t cap, size_t* outLen)
	{
	if (enc == EIX_BASE64) return b64_decode(in, n, out, cap, outLen);
	if (enc == EIX_QP)     return qp_decode(in, n, out, cap, outLen);
	if (n > cap) return false;
	memcpy(out, in, n); *outLen = n;
	return true;
	}

// ----- an S-expression scan over a response buffer (literal-aware) ------------

static size_t skip_sp(const uint8_t* b, size_t p, size_t end)
	{ while (p < end && (b[p] == ' ' || b[p] == '\r' || b[p] == '\n')) p++; return p; }

// advance over one scalar (quoted string, {n} literal, or atom); bracket-aware
// like imapparse.c so BODY[HEADER.FIELDS (...)] stays one atom
static size_t skip_scalar(const uint8_t* b, size_t p, size_t end)
	{
	if (p >= end) return end;
	if (b[p] == '"')
		{
		p++;
		while (p < end && b[p] != '"') { if (b[p] == '\\' && p + 1 < end) p++; p++; }
		if (p < end) p++;
		return p;
		}
	if (b[p] == '{')
		{
		size_t q = p + 1; long n = 0;
		while (q < end && b[q] >= '0' && b[q] <= '9') { n = n * 10 + (b[q] - '0'); q++; }
		if (q < end && b[q] == '+') q++;
		while (q < end && b[q] != '\n') q++;
		if (q < end) q++;                    // past LF
		if (n < 0) n = 0;
		if ((size_t)n > end - q) n = end - q;
		return q + (size_t)n;
		}
	int br = 0;
	while (p < end)
		{
		uint8_t c = b[p];
		if (c == '[') br++;
		else if (c == ']') { if (br) br--; }
		else if (!br && (c == ' ' || c == '(' || c == ')' || c == '\r' || c == '\n')) break;
		p++;
		}
	return p;
	}

// advance over one value: a parenthesised list (balanced, literal-aware) or a scalar
static size_t skip_value(const uint8_t* b, size_t p, size_t end)
	{
	p = skip_sp(b, p, end);
	if (p >= end) return end;
	if (b[p] == '(')
		{
		p++; int d = 1;
		while (p < end && d > 0)
			{
			p = skip_sp(b, p, end);
			if (p >= end) break;
			uint8_t c = b[p];
			if (c == '(') { d++; p++; }
			else if (c == ')') { d--; p++; }
			else p = skip_scalar(b, p, end);
			}
		return p;
		}
	return skip_scalar(b, p, end);
	}

// the logical text of a token (quotes stripped, a literal's body copied, an
// atom as it stands); NUL-terminated and clipped to aCap
static void token_text(const uint8_t* b, size_t s, size_t e, char* out, size_t cap)
	{
	size_t k = 0;
	if (cap == 0) return;
	if (s < e && b[s] == '"')
		{
		size_t p = s + 1;
		while (p < e && b[p] != '"' && k < cap - 1) { if (b[p] == '\\' && p + 1 < e) p++; out[k++] = (char)b[p++]; }
		out[k] = 0; return;
		}
	if (s < e && b[s] == '{')
		{
		size_t q = s + 1; long n = 0;
		while (q < e && b[q] >= '0' && b[q] <= '9') { n = n * 10 + (b[q] - '0'); q++; }
		if (q < e && b[q] == '+') q++;
		while (q < e && b[q] != '\n') q++;
		if (q < e) q++;
		while (q < e && k < cap - 1 && (long)k < n) out[k++] = (char)b[q++];
		out[k] = 0; return;
		}
	for (size_t p = s; p < e && k < cap - 1; p++) out[k++] = (char)b[p];
	out[k] = 0;
	}

// ===== queue helpers ===========================================================

static size_t append_upto(uint8_t* buf, size_t& len, size_t cap, const uint8_t* d, size_t n)
	{
	size_t room = cap - len;
	if (n > room) n = room;
	if (n) { memcpy(buf + len, d, n); len += n; }
	return n;
	}

static bool append_all(uint8_t* buf, size_t& len, size_t cap, const void* d, size_t n)
	{
	if (len + n > cap) return false;
	memcpy(buf + len, d, n); len += n;
	return true;
	}

// ===== lifecycle ===============================================================

ImapXcode::ImapXcode()
	{
	memset(this, 0, sizeof(*this));
	iState = EClientLine;
	}

ImapXcode::~ImapXcode() { End(); }

bool ImapXcode::Alloc()
	{
	iToServerCap = 64 * 1024;
	iToPsionCap  = 256 * 1024;
	iRespCap     = 128 * 1024;
	iDecCap      = iMaxSrc + 64;
	iPartCap     = iMaxSrc * 2 + 16 * 1024;     // base64 body (~1.37x) + the framing text
	iToServer = (uint8_t*)xmalloc(iToServerCap);
	iToPsion  = (uint8_t*)xmalloc(iToPsionCap);
	iResp     = (uint8_t*)xmalloc(iRespCap);
	iDec      = (uint8_t*)xmalloc(iDecCap);
	iPart     = (uint8_t*)xmalloc(iPartCap);
	return iToServer && iToPsion && iResp && iDec && iPart;
	}

bool ImapXcode::Begin(int aMaxWidth, int aMaxSrcKB, XcodeLog* aLog)
	{
	End();
	iLog = aLog;
	iMaxWidth = aMaxWidth < 16 ? 16 : aMaxWidth;
	iMaxSrc = (aMaxSrcKB > 0 ? (size_t)aMaxSrcKB : 256) * 1024;
	iState = EClientLine;
	iLineLen = iToServerLen = iToPsionLen = iRespLen = 0;
	iNCached = iNImg = iImgIdx = 0;
	iInjSent = false; iInjNo = 1;
	iNImages = 0; iSaved = 0;
	ResetFrame(0);
	if (!Alloc()) { End(); return false; }
	return true;
	}

void ImapXcode::End()
	{
	ClearCache();
	free(iToServer); free(iToPsion); free(iResp); free(iDec); free(iPart);
	iToServer = iToPsion = iResp = iDec = iPart = 0;
	iToServerCap = iToPsionCap = iRespCap = iDecCap = iPartCap = 0;
	iToServerLen = iToPsionLen = iRespLen = iPartLen = iLineLen = 0;
	}

void ImapXcode::ClearCache()
	{
	for (int i = 0; i < iNCached; i++) { free(iCache[i].b64); iCache[i].b64 = 0; }
	iNCached = 0;
	}

void ImapXcode::Fail(const char* aWhy)
	{
	if (iState == EFail) return;
	if (iLog)
		{
		char m[96];
		snprintf(m, sizeof m, "relay: xcode off (%s)", aWhy ? aWhy : "");
		iLog->XLog(m);
		}
	// flush partial work so no bytes are lost and the stream stays in step
	if (iLineLen) { append_upto(iToServer, iToServerLen, iToServerCap, (const uint8_t*)iLine, iLineLen); iLineLen = 0; }
	if ((iState == EServerCapture || iState == ETranscode) && iRespLen)
		{ append_upto(iToPsion, iToPsionLen, iToPsionCap, iResp, iRespLen); iRespLen = 0; }
	iState = EFail;
	}

// ===== framing =================================================================

void ImapXcode::ResetFrame(const char* aTag)
	{
	iFrPhase = 0; iFrLitLeft = 0; iFrGotBody = false;
	iFrHeadLen = 0; iFrTailLen = 0;
	if (aTag) { size_t n = strlen(aTag); if (n >= sizeof iMatchTag) n = sizeof iMatchTag - 1; memcpy(iMatchTag, aTag, n); iMatchTag[n] = 0; }
	else iMatchTag[0] = 0;
	}

// does the current line's tail end with a "{n}" (or "{n+}") literal introducer?
bool ImapXcode::LiteralAtEnd(size_t* aN) const
	{
	size_t L = iFrTailLen;
	if (L < 3 || iFrTail[L - 1] != '}') return false;
	long i = (long)L - 2;
	if (i >= 0 && iFrTail[i] == '+') i--;
	if (i < 0 || iFrTail[i] < '0' || iFrTail[i] > '9') return false;
	long e = i;
	while (i >= 0 && iFrTail[i] >= '0' && iFrTail[i] <= '9') i--;
	if (i < 0 || iFrTail[i] != '{') return false;
	long n = 0;
	for (long k = i + 1; k <= e; k++) n = n * 10 + (iFrTail[k] - '0');
	*aN = (size_t)n;
	return true;
	}

// advance the framer by one byte (already appended to the capture/part buffer
// by the caller). Sets *aDone when the matched tag's completion line is read.
void ImapXcode::FrameByte(uint8_t aB, bool aInject, bool* aDone)
	{
	*aDone = false;
	if (iFrPhase == 1)                           // inside a literal body
		{
		if (iFrLitLeft > 0) iFrLitLeft--;
		if (iFrLitLeft == 0) { iFrPhase = 0; iFrTailLen = 0; }   // same line continues
		return;
		}
	if (aB == '\n')
		{
		size_t n;
		if (LiteralAtEnd(&n))
			{
			iFrPhase = 1; iFrLitLeft = n;
			if (aInject && !iFrGotBody)
				{
				iFrGotBody = true; iPartBodyLen = n;
				if (n > iMaxSrc * 2) iPartTooBig = true;
				else iPartBodyOff = iPartLen;    // iPartLen already points past this LF
				}
			iFrTailLen = 0;                      // keep the head: the logical line goes on
			}
		else
			{
			size_t tl = strlen(iMatchTag);
			if (tl && iFrHeadLen >= tl + 1 && memcmp(iFrHead, iMatchTag, tl) == 0 && iFrHead[tl] == ' ')
				*aDone = true;
			iFrHeadLen = 0; iFrTailLen = 0;      // next logical line
			}
		return;
		}
	if (aB == '\r') return;
	if (iFrHeadLen < sizeof iFrHead) iFrHead[iFrHeadLen++] = (char)aB;
	if (iFrTailLen < sizeof iFrTail) iFrTail[iFrTailLen++] = (char)aB;
	else { memmove(iFrTail, iFrTail + 1, sizeof iFrTail - 1); iFrTail[sizeof iFrTail - 1] = (char)aB; }
	}

// ===== pushing bytes in =========================================================

size_t ImapXcode::PushClient(const uint8_t* aData, size_t aLen)
	{
	if (iState == EFail) return append_upto(iToServer, iToServerLen, iToServerCap, aData, aLen);
	size_t i = 0;
	while (i < aLen)
		{
		if (iState != EClientLine) break;        // busy with a server response
		uint8_t b = aData[i];
		if (iLineLen >= kLineMax) { Fail("client line too long"); break; }
		iLine[iLineLen++] = (char)b; i++;
		if (b == '\n') OnClientLine();
		if (iState == EFail) { i += append_upto(iToServer, iToServerLen, iToServerCap, aData + i, aLen - i); break; }
		}
	return i;
	}

size_t ImapXcode::PushServer(const uint8_t* aData, size_t aLen)
	{
	if (iState == EFail) return append_upto(iToPsion, iToPsionLen, iToPsionCap, aData, aLen);
	size_t i = 0;
	while (i < aLen)
		{
		if (iState == EServerPass)
			{
			if (iToPsionLen >= iToPsionCap) break;              // back-pressure
			uint8_t b = aData[i++];
			iToPsion[iToPsionLen++] = b;
			bool done = false; FrameByte(b, false, &done);
			if (done) { iState = EClientLine; ResetFrame(0); break; }
			}
		else if (iState == EServerCapture)
			{
			uint8_t b = aData[i];
			if (iRespLen >= iRespCap) { Fail("response too large"); break; }
			iResp[iRespLen++] = b; i++;
			bool done = false; FrameByte(b, false, &done);
			if (done) OnCaptureDone();                           // may change state
			}
		else if (iState == ETranscode)
			{
			if (!iInjSent) break;                                // waiting to emit a fetch
			uint8_t b = aData[i];
			// never forward injected (X###) bytes to the client. If a buffer
			// overflows, DRAIN the rest of this response (framing only, no
			// store) and Fail at its clean boundary - leaking X### bytes to the
			// client mid-response would desync it (R1).
			bool store = !((iFrPhase == 1 && iPartTooBig) || iInjDrain);
			if (store && iPartLen >= iPartCap)
				{
				if (iFrPhase == 1) iPartTooBig = true;           // body too big: skip, fail later
				else iInjDrain = true;                           // header too big: drain, then fail
				store = false;
				}
			if (store) iPart[iPartLen++] = b;
			i++;
			bool done = false; FrameByte(b, true, &done);
			if (done) { if (iInjDrain) Fail("injected header too large"); else OnInjectDone(); }
			}
		else break;                                              // EClientLine / EServeCache: not expecting server bytes
		if (iState == EFail) { i += append_upto(iToPsion, iToPsionLen, iToPsionCap, aData + i, aLen - i); break; }
		}
	return i;
	}

// ===== pulling bytes out ========================================================

size_t ImapXcode::PullToPsion(uint8_t* aOut, size_t aMax)
	{
	size_t n = iToPsionLen < aMax ? iToPsionLen : aMax;
	if (n) { memcpy(aOut, iToPsion, n); iToPsionLen -= n; if (iToPsionLen) memmove(iToPsion, iToPsion + n, iToPsionLen); }
	return n;
	}

size_t ImapXcode::PullToServer(uint8_t* aOut, size_t aMax)
	{
	size_t n = iToServerLen < aMax ? iToServerLen : aMax;
	if (n) { memcpy(aOut, iToServer, n); iToServerLen -= n; if (iToServerLen) memmove(iToServer, iToServer + n, iToServerLen); }
	return n;
	}

// ===== classifying a client command ============================================

// match word (case-insensitive) at *p (after spaces), followed by a space;
// advances *p past it on success
static bool word_ci(const char* buf, size_t* p, size_t L, const char* w)
	{
	size_t q = *p; while (q < L && buf[q] == ' ') q++;
	size_t wl = strlen(w);
	if (q + wl > L) return false;
	for (size_t k = 0; k < wl; k++) if (lc((unsigned char)buf[q + k]) != lc((unsigned char)w[k])) return false;
	if (q + wl < L && buf[q + wl] != ' ' && buf[q + wl] != '\r' && buf[q + wl] != '\n' && buf[q + wl] != '(') return false;
	*p = q + wl;
	return true;
	}

// pull the section id and optional <off.len> out of a BODY[.PEEK][sec]<..> item
static bool parse_part_fetch(const char* buf, size_t L, char* sec, size_t secCap, size_t* off, size_t* len)
	{
	size_t i = find_ci((const uint8_t*)buf, 0, L, "BODY");
	if (i == (size_t)-1) return false;
	i += 4;
	if (i + 5 <= L)
		{
		bool peek = true;
		for (int k = 0; k < 5; k++) if (lc((unsigned char)buf[i + k]) != lc((unsigned char)".peek"[k])) { peek = false; break; }
		if (peek) i += 5;
		}
	if (i >= L || buf[i] != '[') return false;
	i++;
	size_t s = 0;
	while (i < L && buf[i] != ']' && s < secCap - 1) sec[s++] = buf[i++];
	sec[s] = 0;
	if (i >= L || buf[i] != ']') return false;
	i++;
	*off = 0; *len = (size_t)0xFFFFFFFFUL;
	if (i < L && buf[i] == '<')
		{
		i++; size_t o = 0; bool any = false;
		while (i < L && buf[i] >= '0' && buf[i] <= '9') { o = o * 10 + (buf[i] - '0'); i++; any = true; }
		if (any) *off = o;
		if (i < L && buf[i] == '.')
			{ i++; size_t ln = 0; while (i < L && buf[i] >= '0' && buf[i] <= '9') { ln = ln * 10 + (buf[i] - '0'); i++; } *len = ln; }
		}
	return true;
	}

void ImapXcode::ForwardCmd(TState aNext)
	{
	if (!append_all(iToServer, iToServerLen, iToServerCap, iLine, iLineLen)) { Fail("toserver full"); return; }
	ResetFrame(iTag);
	if (aNext == EServerCapture) iRespLen = 0;
	iLineLen = 0;
	iState = aNext;
	}

void ImapXcode::OnClientLine()
	{
	const char* L = iLine; size_t n = iLineLen;
	size_t p = 0;
	while (p < n && L[p] == ' ') p++;
	size_t ts = p;
	while (p < n && L[p] != ' ' && L[p] != '\r' && L[p] != '\n') p++;
	size_t tl = p - ts;
	if (tl == 0 || tl >= sizeof iTag) { ForwardCmd(EServerPass); return; }
	memcpy(iTag, L + ts, tl); iTag[tl] = 0;

	if (!word_ci(L, &p, n, "UID") || !word_ci(L, &p, n, "FETCH")) { ForwardCmd(EServerPass); return; }
	while (p < n && L[p] == ' ') p++;
	unsigned long uid = 0; bool hasUid = false;
	while (p < n && L[p] >= '0' && L[p] <= '9') { uid = uid * 10 + (L[p] - '0'); p++; hasUid = true; }
	if (!hasUid) { ForwardCmd(EServerPass); return; }

	if (find_ci((const uint8_t*)L, p, n, "BODYSTRUCTURE") != (size_t)-1)
		{ iUidSeq = (uint32_t)uid; ForwardCmd(EServerCapture); return; }

	char sec[kSectionMax]; size_t off, len;
	if (parse_part_fetch(L, n, sec, sizeof sec, &off, &len))
		{
		int ci = CacheFind((uint32_t)uid, sec);
		if (ci >= 0) { ServeCache(ci, off, len); return; }
		}
	ForwardCmd(EServerPass);
	}

int ImapXcode::CacheFind(uint32_t aUid, const char* aSec) const
	{
	for (int i = 0; i < iNCached; i++)
		if (iCache[i].uid == aUid && !strcmp(iCache[i].section, aSec)) return i;
	return -1;
	}

void ImapXcode::ServeCache(int aCi, size_t aOff, size_t aLen)
	{
	const Cached& C = iCache[aCi];
	size_t avail = aOff < C.b64len ? C.b64len - aOff : 0;
	size_t n = aLen < avail ? aLen : avail;
	char hdr[96];
	int hn = snprintf(hdr, sizeof hdr, "* %u FETCH (UID %u BODY[%s]<%lu> {%lu}\r\n",
		(unsigned)C.seq, (unsigned)C.uid, C.section, (unsigned long)aOff, (unsigned long)n);
	char tail[96];
	int tn = snprintf(tail, sizeof tail, ")\r\n%s OK FETCH completed\r\n", iTag);
	if (!append_all(iToPsion, iToPsionLen, iToPsionCap, hdr, (size_t)hn) ||
	    !append_all(iToPsion, iToPsionLen, iToPsionCap, C.b64 + aOff, n) ||
	    !append_all(iToPsion, iToPsionLen, iToPsionCap, tail, (size_t)tn))
		{ Fail("serve: topsion full"); return; }
	iLineLen = 0;                                // stays EClientLine
	}

// ===== the captured BODYSTRUCTURE response =====================================

bool ImapXcode::WalkStruct(const uint8_t* b, size_t p, size_t end, const char* prefix, int depth)
	{
	if (depth > 12) return true;
	p = skip_sp(b, p, end);
	if (p >= end || b[p] != '(') return true;
	size_t q = skip_sp(b, p + 1, end);
	if (q < end && b[q] == '(')                  // multipart: (part)(part)... "subtype" ...
		{
		int i = 1;
		while (q < end)
			{
			q = skip_sp(b, q, end);
			if (q >= end || b[q] == ')') break;
			if (b[q] == '(')
				{
				char id[kSectionMax];
				if (prefix[0]) snprintf(id, sizeof id, "%s.%d", prefix, i);
				else snprintf(id, sizeof id, "%d", i);
				if (!WalkStruct(b, q, end, id, depth + 1)) return false;
				q = skip_value(b, q, end); i++;
				}
			else q = skip_value(b, q, end);      // the subtype / params / disposition
			}
		return true;
		}
	// a leaf: (type subtype (params) id desc ENCODING SIZE ...)
	size_t fs[8], fe[8]; int cnt = 0;
	size_t c = p + 1;
	while (c < end && cnt < 8)
		{
		c = skip_sp(b, c, end);
		if (c >= end || b[c] == ')') break;
		size_t vs = c, ve = skip_value(b, c, end);
		fs[cnt] = vs; fe[cnt] = ve; cnt++; c = ve;
		}
	if (cnt < 2) return true;
	char type[16]; token_text(b, fs[0], fe[0], type, sizeof type);
	if (ci_cmp(type, "image") != 0) return true; // only image parts matter
	if (cnt < 7) return false;                   // malformed image leaf -> fail (relay true)
	const char* sec = prefix[0] ? prefix : "1";
	if (strlen(sec) >= kSectionMax) return false;
	if (iNImg >= kMaxImages) return false;       // more than we cache -> fail
	XLeaf& Lf = iLeaf[iNImg];
	snprintf(Lf.section, sizeof Lf.section, "%s", sec);
	Lf.subS = fs[1]; Lf.subE = fe[1];
	Lf.encS = fs[5]; Lf.encE = fe[5];
	Lf.szS = fs[6];  Lf.szE = fe[6];
	char enc[24]; token_text(b, fs[5], fe[5], enc, sizeof enc);
	Lf.enc = enc_from_name(enc);
	char szt[24]; token_text(b, fs[6], fe[6], szt, sizeof szt);
	if (szt[0] < '0' || szt[0] > '9') return false;
	Lf.origSize = (uint32_t)strtoul(szt, 0, 10);
	iNImg++;
	return true;
	}

bool ImapXcode::ParseStructure()
	{
	const uint8_t* b = iResp; size_t end = iRespLen;
	size_t p = 0;
	while (p < end && b[p] != '*') p++;
	if (p >= end) return false;
	p = skip_sp(b, p + 1, end);
	if (p >= end || b[p] < '0' || b[p] > '9') return false;
	unsigned long seq = 0;
	while (p < end && b[p] >= '0' && b[p] <= '9') { seq = seq * 10 + (b[p] - '0'); p++; }
	iSeqForUid = (uint32_t)seq;
	size_t fp = find_ci(b, p, end, "FETCH");
	if (fp == (size_t)-1) return false;
	size_t ip = fp + 5;
	while (ip < end && b[ip] != '(') ip++;
	if (ip >= end) return false;
	size_t structStart = (size_t)-1;
	size_t c = ip + 1;
	while (c < end)
		{
		c = skip_sp(b, c, end);
		if (c >= end || b[c] == ')') break;
		size_t ks = c, ke = skip_value(b, c, end);
		char key[24]; token_text(b, ks, ke, key, sizeof key);
		c = skip_sp(b, ke, end);
		if (c >= end || b[c] == ')') break;
		size_t vs = c, ve = skip_value(b, c, end);
		if (!ci_cmp(key, "BODYSTRUCTURE")) structStart = vs;
		c = ve;
		}
	if (structStart == (size_t)-1) return false;
	size_t s = skip_sp(b, structStart, end);
	if (s >= end || b[s] != '(') return false;
	iNImg = 0;
	return WalkStruct(b, s, end, "", 0);
	}

void ImapXcode::OnCaptureDone()
	{
	if (!ParseStructure()) { Fail("bodystructure parse"); return; }
	if (iNImg == 0)                              // nothing to transcode: relay verbatim, stay on
		{
		if (!append_all(iToPsion, iToPsionLen, iToPsionCap, iResp, iRespLen)) { Fail("relay struct"); return; }
		iRespLen = 0; iState = EClientLine; ResetFrame(0);
		return;
		}
	ClearCache();
	iImgIdx = 0; iInjSent = false;
	iState = ETranscode;
	StartInject();
	}

// ===== the injected BODY.PEEK fetches ==========================================

void ImapXcode::StartInject()
	{
	const XLeaf& Lf = iLeaf[iImgIdx];
	char cmd[128];
	int n = snprintf(cmd, sizeof cmd, "X%03d UID FETCH %u BODY.PEEK[%s]\r\n",
		iInjNo % 1000, (unsigned)iUidSeq, Lf.section);
	snprintf(iInjTag, sizeof iInjTag, "X%03d", iInjNo % 1000);
	iInjNo = (iInjNo + 1) % 1000;
	if (!append_all(iToServer, iToServerLen, iToServerCap, cmd, (size_t)n)) { Fail("inject: toserver full"); return; }
	iPartLen = 0; iPartBodyOff = 0; iPartBodyLen = 0; iPartTooBig = false; iInjDrain = false;
	ResetFrame(iInjTag);
	iInjSent = true;
	}

void ImapXcode::OnInjectDone()
	{
	iInjSent = false;
	if (iPartTooBig || !iFrGotBody) { Fail(iPartTooBig ? "part over limit" : "no part body"); return; }

	size_t decLen = 0;
	if (!decode_body(iLeaf[iImgIdx].enc, iPart + iPartBodyOff, iPartBodyLen, iDec, iDecCap, &decLen) || decLen == 0)
		{ Fail("part decode"); return; }

	ImageConverter conv;
	char why[64];
	if (!conv.Decode(iDec, decLen, iMaxWidth, why, sizeof why)) { Fail("image decode"); return; }

	size_t gcap = (size_t)conv.Width() * (size_t)conv.Height() * 5 / 4 + 2048;
	uint8_t* g = (uint8_t*)xmalloc(gcap);
	if (!g) { Fail("gif alloc"); return; }
	struct GifSink : public HtmlSink
		{
		uint8_t* b; size_t len, cap;
		GifSink(uint8_t* B, size_t C) : b(B), len(0), cap(C) {}
		void Put(const char* d, size_t n) override { if (len + n <= cap) { memcpy(b + len, d, n); len += n; } }
		} sink(g, gcap);
	while (!conv.Write(&sink, 64))
		;
	size_t gifLen = sink.len;
	if (gifLen == 0) { free(g); Fail("gif empty"); return; }

	size_t b64cap = ((gifLen + 2) / 3) * 4 + 4;
	uint8_t* b64 = (uint8_t*)xmalloc(b64cap);
	if (!b64) { free(g); Fail("b64 alloc"); return; }
	size_t b64len = 0; b64_encode(g, gifLen, b64, &b64len);
	free(g);

	Cached& C = iCache[iNCached];
	C.uid = iUidSeq; C.seq = iSeqForUid;
	memcpy(C.section, iLeaf[iImgIdx].section, sizeof C.section);
	C.b64 = b64; C.b64len = b64len; C.origSize = iLeaf[iImgIdx].origSize;
	iNCached++;
	iNImages++;
	if (C.origSize > b64len) iSaved += C.origSize - (uint32_t)b64len;

	iImgIdx++;
	if (iImgIdx >= iNImg) RewriteAndEmit();
	else StartInject();
	}

// ===== rewriting the BODYSTRUCTURE and sending it on ===========================

void ImapXcode::RewriteAndEmit()
	{
	// each image leaf contributes three in-place edits, in increasing position:
	// subtype -> "GIF", encoding -> "BASE64", size -> the base64-GIF length.
	// (The structure is inline in the FETCH line, not inside a literal, so no
	// enclosing {N} count has to be recomputed; a literal-typed token we touch
	// is replaced whole, dropping its {N}.)
	struct Edit { size_t s, e; char repl[24]; };
	Edit ed[kMaxImages * 3];
	int ne = 0;
	for (int i = 0; i < iNImg && i < iNCached; i++)
		{
		const XLeaf& Lf = iLeaf[i];
		ed[ne].s = Lf.subS; ed[ne].e = Lf.subE; snprintf(ed[ne].repl, sizeof ed[ne].repl, "\"GIF\""); ne++;
		ed[ne].s = Lf.encS; ed[ne].e = Lf.encE; snprintf(ed[ne].repl, sizeof ed[ne].repl, "\"BASE64\""); ne++;
		ed[ne].s = Lf.szS;  ed[ne].e = Lf.szE;  snprintf(ed[ne].repl, sizeof ed[ne].repl, "%lu", (unsigned long)iCache[i].b64len); ne++;
		}
	// (edits are already sorted: fields 1<5<6 within a leaf, leaves in order)
	size_t pos = 0;
	bool ok = true;
	for (int i = 0; i < ne && ok; i++)
		{
		if (ed[i].s < pos || ed[i].e > iRespLen) { ok = false; break; }   // overlap / out of range
		ok = append_all(iToPsion, iToPsionLen, iToPsionCap, iResp + pos, ed[i].s - pos) &&
		     append_all(iToPsion, iToPsionLen, iToPsionCap, ed[i].repl, strlen(ed[i].repl));
		pos = ed[i].e;
		}
	if (ok) ok = append_all(iToPsion, iToPsionLen, iToPsionCap, iResp + pos, iRespLen - pos);
	if (!ok) { Fail("rewrite emit"); return; }
	if (iLog)
		{
		char m[80];
		snprintf(m, sizeof m, "relay: xcode %d image(s), saved %lu bytes", iNImg, (unsigned long)iSaved);
		iLog->XLog(m);
		}
	iRespLen = 0;
	iState = EClientLine;
	ResetFrame(0);
	}

} // namespace am
