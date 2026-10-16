// test_imapxcode.cpp - tests for the Atom's IMAP image-transcoding filter
// (imapxcode.cpp). It drives am::ImapXcode as ppptls.cpp would: push a client
// command, pull what goes to the server, push a scripted server response, pull
// what reaches the Psion, and assert. A real embedded JPEG (pictures/photo.jpg,
// from mkpictures.py) is used so the transcode path runs end to end.
//   make -C hosttest test
// MIT licence (see LICENSE at the top of the repository).
#include "../src/imapxcode.h"
#include "../src/imgconv.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static int gChecks = 0, gFails = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool Has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

// ----- base64 (for the fixtures and for checking served bytes) ----------------
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64(const std::string& in)
	{
	std::string o;
	size_t i = 0;
	for (; i + 3 <= in.size(); i += 3)
		{
		uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
		o += kB64[(v >> 18) & 63]; o += kB64[(v >> 12) & 63]; o += kB64[(v >> 6) & 63]; o += kB64[v & 63];
		}
	if (in.size() - i == 1) { uint32_t v = (uint8_t)in[i] << 16; o += kB64[(v >> 18) & 63]; o += kB64[(v >> 12) & 63]; o += "=="; }
	else if (in.size() - i == 2) { uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8); o += kB64[(v >> 18) & 63]; o += kB64[(v >> 12) & 63]; o += kB64[(v >> 6) & 63]; o += '='; }
	return o;
	}
static int b64v(int c)
	{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
	}
static std::string unb64(const std::string& in)
	{
	std::string o; unsigned acc = 0; int bits = 0;
	for (char ch : in) { if (ch == '=') break; int v = b64v((unsigned char)ch); if (v < 0) continue; acc = ((acc << 6) | (unsigned)v) & 0xfffff; bits += 6; if (bits >= 8) { bits -= 8; o += (char)((acc >> bits) & 0xff); } }
	return o;
	}

static std::string ReadFile(const char* p)
	{
	FILE* f = fopen(p, "rb");
	if (!f) return "";
	std::string s; char b[65536]; size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
	fclose(f);
	return s;
	}

// the n bytes of the (first) literal in a FETCH response
static std::string LiteralOf(const std::string& s)
	{
	size_t b = s.find('{'); if (b == std::string::npos) return "";
	size_t e = s.find('}', b); if (e == std::string::npos) return "";
	unsigned long n = strtoul(s.c_str() + b + 1, 0, 10);
	size_t crlf = s.find("\r\n", e); if (crlf == std::string::npos) return "";
	size_t start = crlf + 2;
	if (start + n > s.size()) return "";
	return s.substr(start, n);
	}

// ===== the driver ===============================================================

struct Log : am::XcodeLog { std::vector<std::string> lines; void XLog(const char* l) override { lines.push_back(l); } };

struct Harness
	{
	am::ImapXcode x;
	Log log;
	std::string psion;                  // everything delivered to the Psion
	std::string srvGot;                 // everything the server received
	std::function<std::string(const std::string&)> server;

	bool Begin(int w = 200, int kb = 256) { return x.Begin(w, kb, &log); }

	void feedClient(const std::string& s)
		{ size_t o = 0; while (o < s.size()) { size_t c = x.PushClient((const uint8_t*)s.data() + o, s.size() - o); o += c; if (!c) break; } }
	void feedServer(const std::string& s)
		{ size_t o = 0; while (o < s.size()) { size_t c = x.PushServer((const uint8_t*)s.data() + o, s.size() - o); o += c; if (!c) break; } }
	std::string pullServer()
		{ std::string r; uint8_t b[4096]; size_t n; while ((n = x.PullToServer(b, sizeof b)) > 0) r.append((char*)b, n); return r; }
	std::string pullPsion()
		{ std::string r; uint8_t b[4096]; size_t n; while ((n = x.PullToPsion(b, sizeof b)) > 0) r.append((char*)b, n); return r; }

	void pump()
		{
		for (int i = 0; i < 20000; i++)
			{
			bool prog = false;
			std::string sv = pullServer();
			if (!sv.empty())
				{
				srvGot += sv;
				size_t p = 0;
				while (p < sv.size())
					{
					size_t e = sv.find("\r\n", p);
					if (e == std::string::npos) break;
					std::string cmd = sv.substr(p, e + 2 - p);
					p = e + 2;
					std::string resp = server ? server(cmd) : std::string();
					feedServer(resp);
					}
				prog = true;
				}
			std::string pp = pullPsion();
			if (!pp.empty()) { psion += pp; prog = true; }
			if (!prog) break;
			}
		}

	// returns what reached the Psion as a result of this command
	std::string client(const std::string& s) { size_t m = psion.size(); feedClient(s); pump(); return psion.substr(m); }
	};

static std::string Tag(const std::string& cmd)
	{ size_t sp = cmd.find(' '); return sp == std::string::npos ? cmd : cmd.substr(0, sp); }

// ===== 1. a text-only message: everything passes through byte-identical =========

static void TestPassThroughText()
	{
	Harness H;
	CHECK(H.Begin());
	const std::string structCmd = "P001 UID FETCH 100 (ENVELOPE BODYSTRUCTURE)\r\n";
	const std::string structResp =
		"* 5 FETCH (UID 100 ENVELOPE (NIL \"Hi\" NIL NIL NIL NIL NIL NIL NIL NIL) "
		"BODYSTRUCTURE (\"TEXT\" \"PLAIN\" (\"CHARSET\" \"UTF-8\") NIL NIL \"7BIT\" 1234 20 NIL NIL NIL))\r\n"
		"P001 OK FETCH completed\r\n";
	const std::string partCmd = "P002 UID FETCH 100 (BODY.PEEK[1]<0.1024>)\r\n";
	const std::string partResp = "* 5 FETCH (UID 100 BODY[1]<0> {11}\r\nHello world)\r\nP002 OK FETCH completed\r\n";

	H.server = [&](const std::string& c) -> std::string {
		if (Has(c, "BODYSTRUCTURE")) return structResp;
		if (Has(c, "BODY.PEEK[1]") || Has(c, "BODY[1]")) return partResp;
		return Tag(c) + " OK\r\n";
	};

	std::string a = H.client(structCmd);
	std::string b = H.client(partCmd);
	CHECK(a == structResp);                       // the Psion got the response unchanged
	CHECK(b == partResp);
	CHECK(H.srvGot == structCmd + partCmd);        // the server got the commands unchanged
	CHECK(!H.x.PassThrough());                     // no image: still active for the next message
	CHECK(H.x.Transcoded() == 0);

	// the same, arriving a byte at a time (framing must be resumable)
	Harness H2; CHECK(H2.Begin());
	H2.server = H.server;
	for (char ch : structCmd) H2.feedClient(std::string(1, ch)), H2.pump();
	CHECK(H2.psion == structResp && H2.srvGot == structCmd);
	}

// ===== a multipart/related message with a real embedded JPEG ====================

static std::string gJpeg, gJb64;

static std::string MultipartStruct(unsigned seq, unsigned uid, size_t jlen)
	{
	char s[1024];
	snprintf(s, sizeof s,
		"* %u FETCH (UID %u ENVELOPE (NIL {5}\r\nHello NIL NIL NIL NIL NIL NIL NIL NIL) "
		"BODYSTRUCTURE ("
		"(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"UTF-8\") NIL NIL \"7BIT\" 100 5 NIL NIL NIL)"
		"(\"IMAGE\" \"JPEG\" (\"NAME\" \"p.jpg\") \"<cid1>\" NIL \"BASE64\" %zu NIL NIL NIL)"
		" \"RELATED\" (\"BOUNDARY\" \"x\") NIL NIL))\r\nP001 OK FETCH completed\r\n",
		seq, uid, jlen);
	return s;
	}

static void TestImageTranscode()
	{
	Harness H;
	CHECK(H.Begin(200, 256));
	const std::string structResp = MultipartStruct(7, 200, gJb64.size());
	const std::string partBody =
		std::string("* 7 FETCH (UID 200 BODY[2] {") + std::to_string(gJb64.size()) + "}\r\n" + gJb64 + ")\r\nX_TAG_ OK done\r\n";

	H.server = [&](const std::string& c) -> std::string {
		if (Has(c, "BODYSTRUCTURE")) return structResp;
		if (Has(c, "BODY.PEEK[2]"))
			{
			std::string r = partBody;
			size_t t = r.find("X_TAG_");           // echo the filter's own tag
			r.replace(t, 6, Tag(c));
			return r;
			}
		return Tag(c) + " OK\r\n";
	};

	// the structure forwarded to the Psion is rewritten: IMAGE/GIF, BASE64, smaller
	std::string s = H.client("P001 UID FETCH 200 (ENVELOPE BODYSTRUCTURE)\r\n");
	CHECK(H.x.Transcoded() == 1);
	CHECK(!H.x.PassThrough());
	CHECK(Has(s, "\"GIF\"") && Has(s, "\"BASE64\""));
	CHECK(!Has(s, "\"JPEG\""));                     // the old subtype is gone
	// the rewritten size is smaller than the JPEG's base64 length
	size_t g = s.find("\"GIF\"");
	// the number right after "BASE64" in the rewritten image part (skip past the
	// token itself, whose name contains "64")
	size_t enc = s.find("\"BASE64\"", g);
	CHECK(enc != std::string::npos);
	size_t sz = s.find_first_of("0123456789", enc + 8);
	unsigned long newSize = strtoul(s.c_str() + sz, 0, 10);
	CHECK(newSize > 0 && newSize < gJb64.size());
	CHECK(H.x.SavedBytes() > 0);

	// fetching the (now GIF) part is served straight from the cache
	std::string p = H.client("P002 UID FETCH 200 (BODY.PEEK[2]<0.65536>)\r\n");
	CHECK(Has(p, "* 7 FETCH (UID 200 BODY[2]<0> {"));
	CHECK(H.srvGot.find("BODY.PEEK[2]<") == std::string::npos);   // the server never saw the client's part fetch
	std::string gif = LiteralOf(p);
	CHECK(!gif.empty());
	// the literal {N} equals the bytes delivered, and it is the whole GIF (a short read ends the part)
	CHECK(gif.size() == newSize);
	CHECK(gif.size() < 65536);
	std::string raw = unb64(gif);
	CHECK(raw.size() >= 8 && am::ImageType((const uint8_t*)raw.data(), raw.size()) == 3);   // a GIF
	// the tagged completion uses the client's tag
	CHECK(Has(p, "P002 OK"));
	}

// ===== 3. range / resume: consecutive, stable bytes of the one cached GIF =======

static void TestRange()
	{
	Harness H;
	CHECK(H.Begin(200, 256));
	const std::string structResp = MultipartStruct(7, 200, gJb64.size());
	const std::string partBody =
		std::string("* 7 FETCH (UID 200 BODY[2] {") + std::to_string(gJb64.size()) + "}\r\n" + gJb64 + ")\r\nX_TAG_ OK done\r\n";
	H.server = [&](const std::string& c) -> std::string {
		if (Has(c, "BODYSTRUCTURE")) return structResp;
		if (Has(c, "BODY.PEEK[2]")) { std::string r = partBody; r.replace(r.find("X_TAG_"), 6, Tag(c)); return r; }
		return Tag(c) + " OK\r\n";
	};
	H.client("P001 UID FETCH 200 (ENVELOPE BODYSTRUCTURE)\r\n");

	std::string full = LiteralOf(H.client("P010 UID FETCH 200 (BODY.PEEK[2]<0.65536>)\r\n"));
	std::string a = LiteralOf(H.client("P011 UID FETCH 200 (BODY.PEEK[2]<0.1024>)\r\n"));
	std::string b = LiteralOf(H.client("P012 UID FETCH 200 (BODY.PEEK[2]<1024.1024>)\r\n"));
	CHECK(a.size() == 1024);
	CHECK(a == full.substr(0, 1024));
	CHECK(b == full.substr(1024, b.size()));       // consecutive, deterministic
	// a fetch wholly past the end is an empty (end-of-part) read
	std::string past = LiteralOf(H.client("P013 UID FETCH 200 (BODY.PEEK[2]<" + std::to_string(full.size() + 10) + ".1024>)\r\n"));
	CHECK(past.empty());
	CHECK(!H.x.PassThrough());
	}

// ===== 4. fallbacks: a corrupt image, and a part over the size limit ============

static void TestFallbackCorrupt()
	{
	Harness H;
	CHECK(H.Begin(200, 256));
	std::string garbage(2048, 0);
	for (size_t i = 0; i < garbage.size(); i++) garbage[i] = (char)((i * 37 + 11) & 0xff);
	std::string gb64 = b64(garbage);
	const std::string structResp = MultipartStruct(7, 200, gb64.size());
	const std::string partBody =
		std::string("* 7 FETCH (UID 200 BODY[2] {") + std::to_string(gb64.size()) + "}\r\n" + gb64 + ")\r\nX_TAG_ OK done\r\n";
	H.server = [&](const std::string& c) -> std::string {
		if (Has(c, "BODYSTRUCTURE")) return structResp;
		if (Has(c, "BODY.PEEK[2]")) { std::string r = partBody; r.replace(r.find("X_TAG_"), 6, Tag(c)); return r; }
		return Tag(c) + " OK\r\n";
	};
	std::string s = H.client("P001 UID FETCH 200 (ENVELOPE BODYSTRUCTURE)\r\n");
	CHECK(H.x.PassThrough());                       // gave up
	CHECK(s == structResp);                         // the TRUE (unrewritten) structure reached the Psion
	CHECK(H.x.Transcoded() == 0);
	}

static void TestFallbackTooBig()
	{
	Harness H;
	CHECK(H.Begin(200, 4));                          // a 4 KB source limit: the JPEG is far bigger
	const std::string structResp = MultipartStruct(7, 200, gJb64.size());
	const std::string partBody =
		std::string("* 7 FETCH (UID 200 BODY[2] {") + std::to_string(gJb64.size()) + "}\r\n" + gJb64 + ")\r\nX_TAG_ OK done\r\n";
	H.server = [&](const std::string& c) -> std::string {
		if (Has(c, "BODYSTRUCTURE")) return structResp;
		if (Has(c, "BODY.PEEK[2]")) { std::string r = partBody; r.replace(r.find("X_TAG_"), 6, Tag(c)); return r; }
		return Tag(c) + " OK\r\n";
	};
	std::string s = H.client("P001 UID FETCH 200 (ENVELOPE BODYSTRUCTURE)\r\n");
	CHECK(H.x.PassThrough());
	CHECK(s == structResp);                         // relayed unchanged, the big part skipped cleanly
	// the whole oversized part was drained from the upstream (the stream stays in step)
	CHECK(H.srvGot.find("BODY.PEEK[2]") != std::string::npos);
	}

// ===== 5. a tiny fuzz loop: random / truncated streams, no crash / over-read ====

static uint32_t gRnd = 0x1234abcdu;
static uint32_t rnd() { gRnd = gRnd * 1103515245u + 12345u; return gRnd >> 8; }

static void TestFuzz()
	{
	for (int iter = 0; iter < 4000; iter++)
		{
		Harness H;
		if (!H.Begin(64 + (int)(rnd() % 200), 1 + (int)(rnd() % 64))) { CHECK(false); continue; }
		H.server = [&](const std::string& c) -> std::string {
			// a random, often-truncated, often-garbage "response"
			std::string r;
			if (rnd() & 1) r = Tag(c) + " OK\r\n";
			size_t n = rnd() % 3000;
			for (size_t i = 0; i < n; i++) r += (char)(rnd() & 0xff);
			if (rnd() & 1) r += "\r\n";
			return r;
		};
		// a random client command, sometimes a plausible FETCH
		int kind = rnd() % 4;
		std::string cmd;
		if (kind == 0) cmd = "P001 UID FETCH 100 (ENVELOPE BODYSTRUCTURE)\r\n";
		else if (kind == 1) cmd = "P001 UID FETCH 100 (BODY.PEEK[2]<0.1024>)\r\n";
		else { size_t n = rnd() % 1500; for (size_t i = 0; i < n; i++) cmd += (char)(rnd() & 0xff); if (rnd() & 1) cmd += "\r\n"; }
		H.feedClient(cmd);
		H.pump();
		// feed some more raw server noise directly, in random chunks
		for (int k = 0; k < 4; k++)
			{
			std::string noise; size_t n = rnd() % 2000;
			for (size_t i = 0; i < n; i++) noise += (char)(rnd() & 0xff);
			H.feedServer(noise);
			H.pullPsion(); H.pullServer();
			}
		// it must still answer Pull without looping or over-reading (ASan/UBSan watch)
		H.pullPsion(); H.pullServer();
		CHECK(H.x.PassThrough() || !H.x.PassThrough());   // the point is no crash / over-read (ASan/UBSan)
		}
	}

int main()
	{
	gJpeg = ReadFile("pictures/photo.jpg");
	if (gJpeg.empty())
		{
		printf("   pictures/photo.jpg missing (run `make pictures`); image tests skipped\n");
		}
	else
		{
		gJb64 = b64(gJpeg);
		printf("   fixture: photo.jpg %zu bytes, base64 %zu bytes\n", gJpeg.size(), gJb64.size());
		}

	TestPassThroughText();
	if (!gJpeg.empty())
		{
		TestImageTranscode();
		TestRange();
		TestFallbackCorrupt();
		TestFallbackTooBig();
		}
	TestFuzz();

	printf("test_imapxcode: %d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
	}
