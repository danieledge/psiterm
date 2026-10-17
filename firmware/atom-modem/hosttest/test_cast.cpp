// test_cast.cpp - PsiCast on the Atom (src/cast): link A's framing (CRC,
// resync, oversize), the session against a Psion played with the real
// encoder (HELLO/WELCOME, ACKs that keep the window moving, KEYREQ after a
// framing or decode error, PING, the 20 s silence, BYE), tokens, the
// WebSocket handshake and frames, per-viewer dirty tracking (a fast and a
// slow viewer, scrolls, both pixel-exact at the end, the slow one merged),
// input from a viewer with and without control, and the modem route
// (ATDT192.168.7.1:5961 through Modem). With the sanitizers. MIT licence.
#include "fakehal.h"
#include "../src/cast/cast.h"
#include "../src/cast/websock.h"
#include "pccodec.h"
#include <zlib.h>
#include <vector>
#include <string>

static int gChecks = 0, gFails = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef std::vector<uint8_t> Bytes;

static uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t Le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static Bytes Frame(int aType, int aFlags, const Bytes& aPayload)
	{
	Bytes out(aPayload.size() + 12);
	size_t n = am::CastFrame(out.data(), out.size(), aType, aFlags, aPayload.data(), aPayload.size());
	out.resize(n);
	return out;
	}

// the messages in a byte stream from the Atom (to the Psion)
struct Msg { int type; Bytes p; };
static std::vector<Msg> Parse(const Bytes& aIn)
	{
	std::vector<Msg> out;
	std::vector<uint8_t> buf(70000);
	am::CastRx rx;
	rx.Init(buf.data(), buf.size());
	size_t at = 0;
	while (at < aIn.size())
		{
		at += rx.Feed(aIn.data() + at, aIn.size() - at);
		uint8_t t, f;
		const uint8_t* p;
		uint32_t n;
		while (rx.Next(t, f, p, n))
			out.push_back({ t, Bytes(p, p + n) });
		}
	return out;
	}

// ----- framing -----------------------------------------------------------------------

static void TestFraming()
	{
	Bytes a = Frame(am::ECastStatus, 0, Bytes(12, 7));
	Bytes b = Frame(am::ECastUpdate, 1, Bytes(3000, 0x50));     // (full of 'P's: a hard resync)
	CHECK(a.size() == 24 && a[0] == 'P' && a[1] == 'C' && Le32(a.data() + 4) == 12);
	CHECK(Le32(a.data() + 20) == (uint32_t)crc32(0, a.data(), 20));
	// rubbish, a whole message, one with a bad CRC, another whole one, split byte by byte
	Bytes s;
	const char* junk = "PPC\x01\xff\xff\xff\xffhelloPCxx";
	s.insert(s.end(), junk, junk + 18);
	s.insert(s.end(), a.begin(), a.end());
	Bytes bad = b;
	bad[100] ^= 0x40;
	s.insert(s.end(), bad.begin(), bad.end());
	s.insert(s.end(), b.begin(), b.end());
	s.insert(s.end(), a.begin(), a.end());
	std::vector<uint8_t> buf(70000);
	am::CastRx rx;
	rx.Init(buf.data(), buf.size());
	std::vector<int> types;
	for (size_t i = 0; i < s.size(); i++)
		{
		CHECK(rx.Feed(&s[i], 1) == 1);
		uint8_t t, f;
		const uint8_t* p;
		uint32_t n;
		while (rx.Next(t, f, p, n))
			{
			types.push_back(t);
			if (t == am::ECastUpdate)
				CHECK(f == 1 && n == 3000 && p[2999] == 0x50);
			}
		}
	CHECK(types.size() == 3 && types[0] == am::ECastStatus && types[1] == am::ECastUpdate && types[2] == am::ECastStatus);
	CHECK(rx.Errors() == 2);                     // (the rubbish, and the bad CRC: one each)
	// a message longer than the buffer: skipped whole, the next one found
	std::vector<uint8_t> small(1024);
	am::CastRx r2;
	r2.Init(small.data(), small.size());
	Bytes s2 = b;
	s2.insert(s2.end(), a.begin(), a.end());
	size_t at = 0;
	int got = 0;
	while (at < s2.size())
		{
		at += r2.Feed(s2.data() + at, s2.size() - at);
		uint8_t t, f;
		const uint8_t* p;
		uint32_t n;
		while (r2.Next(t, f, p, n))
			got++;
		}
	CHECK(got == 1 && r2.Oversize() == 1);
	}

// ----- a Psion, played with the real encoder ------------------------------------------

struct Psion
	{
	pc_enc* enc;
	uint32_t seq = 0;
	std::vector<uint8_t> frame;                  // 640x240 packed, the model
	Psion() : enc(pc_enc_new(640, 240, 3)), frame(640 * 240 / 2, 0xff) {}
	~Psion() { pc_enc_free(enc); }
	static Bytes Hello(uint32_t aCaps = 3, const char* aName = "My 5mx")
		{
		Bytes p(12);
		p[0] = 1; p[2] = 640 & 255; p[3] = 640 >> 8; p[4] = 240; p[6] = 0;
		p[8] = (uint8_t)aCaps;
		p.insert(p.end(), aName, aName + strlen(aName));
		return Frame(am::ECastHello, 0, p);
		}
	// the next UPDATE for the frame as it is now (empty if nothing changed)
	Bytes Update(bool aKey)
		{
		std::vector<uint8_t> out(65536);
		int len = 0;
		int r = pc_enc_frame(enc, frame.data(), 320, aKey ? 1 : 0, out.data() + 4, (int)out.size() - 4, &len);
		if (r == 1 || r < 0)
			return Bytes();
		seq++;
		out[0] = (uint8_t)seq; out[1] = (uint8_t)(seq >> 8); out[2] = (uint8_t)(seq >> 16); out[3] = (uint8_t)(seq >> 24);
		out.resize((size_t)len + 4);
		return Frame(am::ECastUpdate, aKey ? 1 : 0, out);
		}
	static Bytes Status(int aBattery = 80, bool aCtl = true)
		{
		Bytes p(12, 0);
		p[0] = (uint8_t)aBattery; p[1] = 0; p[2] = 0; p[3] = 0; p[4] = 0x30;   // (3 MB free)
		p[6] = 50; p[8] = 20; p[10] = aCtl ? 1 : 0; p[11] = 0;
		return Frame(am::ECastStatus, 0, p);
		}
	void Px(int aX, int aY, int aG)
		{
		uint8_t& b = frame[aY * 320 + aX / 2];
		b = (aX & 1) ? (uint8_t)((b & 0x0f) | (aG << 4)) : (uint8_t)((b & 0xf0) | aG);
		}
	void Box(int aX, int aY, int aW, int aH, int aG)
		{
		for (int y = aY; y < aY + aH && y < 240; y++)
			for (int x = aX; x < aX + aW && x < 640; x++)
				Px(x, y, aG);
		}
	// a line of "text" (random marks) at row aY
	void Text(int aY, unsigned& aSeed)
		{
		for (int x = 4; x < 600; x += 7)
			{
			aSeed = aSeed * 1103515245 + 12345;
			if ((aSeed >> 16) % 5 == 0) continue;
			Box(x, aY + 2, 5, 9, (int)((aSeed >> 20) % 3) * 5);
			}
		}
	// scroll the area below the title by one 12-pixel line, and write a new one
	void Scroll(unsigned& aSeed)
		{
		memmove(&frame[20 * 320], &frame[32 * 320], (size_t)(228 - 32 + 12) * 320 - 12 * 320);
		Box(0, 216, 640, 12, 15);
		Text(216, aSeed);
		}
	};

// a viewer's canvas, from link B's messages
struct ViewModel
	{
	int w = 0, h = 0;
	std::vector<uint8_t> grey;                   // a pixel a byte
	am::WsReader reader;
	size_t bytes = 0;
	int fulls = 0, rects = 0, scrolls = 0, partial = 0;
	std::vector<std::string> texts;
	static void OnMsg(void* aCtx, int aOp, const uint8_t* aP, size_t aN)
		{
		ViewModel* self = (ViewModel*)aCtx;
		if (aOp == am::EWsText)
			self->texts.push_back(std::string((const char*)aP, aN));
		else if (aOp == am::EWsBinary)
			self->Binary(aP, aN);
		}
	void Binary(const uint8_t* aP, size_t aN)
		{
		if (aP[0] == am::ECastFull)
			{
			w = Le16(aP + 1); h = Le16(aP + 3);
			grey.assign((size_t)w * h, 15);
			CHECK(aN == 5 + (size_t)w * h / 2);
			for (size_t i = 0; i < (size_t)w * h / 2; i++)
				{
				grey[i * 2] = aP[5 + i] & 15;
				grey[i * 2 + 1] = aP[5 + i] >> 4;
				}
			fulls++;
			}
		else if (aP[0] == am::ECastRects)
			{
			int n = Le16(aP + 1);
			size_t o = 3 + (size_t)n * 8;
			for (int k = 0; k < n; k++)
				{
				int x = Le16(aP + 3 + k * 8), y = Le16(aP + 5 + k * 8), rw = Le16(aP + 7 + k * 8), rh = Le16(aP + 9 + k * 8);
				CHECK(x % 2 == 0 && rw % 2 == 0 && x + rw <= w && y + rh <= h);
				for (int r = 0; r < rh; r++)
					for (int c = 0; c < rw; c += 2)
						{
						uint8_t b = aP[o++];
						grey[(size_t)(y + r) * w + x + c] = b & 15;
						grey[(size_t)(y + r) * w + x + c + 1] = b >> 4;
						}
				}
			CHECK(o == aN);
			rects++;
			}
		else if (aP[0] == am::ECastScroll)
			{
			int y0 = Le16(aP + 1), y1 = Le16(aP + 3), dy = (int16_t)Le16(aP + 5);
			int x = 0, cw = w;
			if (aN >= 11) { x = Le16(aP + 7); cw = Le16(aP + 9); }   // (the columns that moved)
			CHECK(aN == 11 && x % 2 == 0 && cw % 2 == 0 && x + cw <= w);
			int d0 = y0 + dy, d1 = y1 + dy;
			if (d0 < 0) { y0 -= d0; d0 = 0; }
			if (d1 > h) { y1 -= d1 - h; d1 = h; }
			std::vector<uint8_t> band;
			for (int r = y0; r < y1; r++)
				band.insert(band.end(), &grey[(size_t)r * w + x], &grey[(size_t)r * w + x + cw]);
			for (int r = d0; r < d1; r++)
				memcpy(&grey[(size_t)r * w + x], &band[(size_t)(r - d0) * cw], cw);
			if (cw < w) partial++;
			scrolls++;
			}
		}
	void Feed(const uint8_t* aP, size_t aN)
		{
		bytes += aN;
		// (server frames are unmasked: set the mask bit so the client-side reader takes them)
		static std::vector<uint8_t> tmp;
		tmp.assign(aP, aP + aN);
		FeedUnmasked(tmp);
		}
	// a tiny unmasked-frame reader (WsReader is the server's, for masked frames)
	std::vector<uint8_t> pend;
	void FeedUnmasked(const std::vector<uint8_t>& aIn)
		{
		pend.insert(pend.end(), aIn.begin(), aIn.end());
		for (;;)
			{
			if (pend.size() < 2) return;
			int op = pend[0] & 15;
			uint64_t len = pend[1] & 127;
			size_t h = 2;
			if (len == 126) { if (pend.size() < 4) return; len = (pend[2] << 8) | pend[3]; h = 4; }
			else if (len == 127)
				{
				if (pend.size() < 10) return;
				len = 0;
				for (int i = 2; i < 10; i++) len = (len << 8) | pend[i];
				h = 10;
				}
			if (pend.size() < h + len) return;
			OnMsg(this, op, pend.data() + h, (size_t)len);
			pend.erase(pend.begin(), pend.begin() + h + len);
			}
		}
	bool Same(const uint8_t* aScreen) const
		{
		if (!w) return false;
		for (size_t i = 0; i < (size_t)w * h / 2; i++)
			if (grey[i * 2] != (aScreen[i] & 15) || grey[i * 2 + 1] != (aScreen[i] >> 4))
				return false;
		return true;
		}
	};

// a client frame (masked), as a browser sends it
static Bytes ClientFrame(int aOp, const std::string& aText, bool aFin = true)
	{
	Bytes f;
	f.push_back((uint8_t)((aFin ? 0x80 : 0) | aOp));
	size_t n = aText.size();
	if (n < 126) f.push_back((uint8_t)(0x80 | n));
	else { f.push_back(0x80 | 126); f.push_back((uint8_t)(n >> 8)); f.push_back((uint8_t)n); }
	uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
	f.insert(f.end(), mask, mask + 4);
	for (size_t i = 0; i < n; i++)
		f.push_back((uint8_t)aText[i] ^ mask[i & 3]);
	return f;
	}

struct Rig
	{
	FakeHal hal;
	am::Cast cast;
	uint32_t id = 0;
	Bytes fromAtom;                              // to the Psion
	Rig() : cast(hal) { hal.webInfo = "http://192.168.1.50/"; cast.SetEnabled(true); }
	void ToAtom(const Bytes& aB) { CHECK(cast.LinkIn(id, aB.data(), aB.size()) == aB.size()); }
	void Drain()
		{
		uint8_t b[512];
		size_t n;
		while ((n = cast.LinkOut(id, b, sizeof(b))) > 0)
			fromAtom.insert(fromAtom.end(), b, b + n);
		}
	std::vector<Msg> Take()
		{
		Drain();
		std::vector<Msg> m = Parse(fromAtom);
		fromAtom.clear();
		return m;
		}
	void Ms(uint32_t aMs) { for (uint32_t i = 0; i < aMs; i += 50) { hal.Advance(50000); cast.Tick(); } }
	int Viewer(bool aControl = false)
		{
		char t[32], why[64];
		CHECK(cast.NewToken(aControl, t, sizeof(t)));
		return cast.ViewerOpen(t, why, sizeof(why));
		}
	void Pump(int aIdx, ViewModel& aV, size_t aMax = 1 << 20)
		{
		uint8_t b[1460];
		size_t total = 0, n;
		while (total < aMax && (n = cast.ViewerOut(aIdx, b, aMax - total < sizeof(b) ? aMax - total : sizeof(b))) > 0)
			{
			aV.Feed(b, n);
			total += n;
			}
		}
	};

static int Count(const std::vector<Msg>& aM, int aType)
	{
	int n = 0;
	for (const Msg& m : aM) if (m.type == aType) n++;
	return n;
	}

static void TestSession()
	{
	Rig r;
	Psion ps;
	r.id = r.cast.LinkOpen(am::Cast::ERouteTcp);
	CHECK(r.id != 0 && !r.cast.LinkUp());
	r.ToAtom(Psion::Hello());
	std::vector<Msg> m = r.Take();
	CHECK(m.size() == 2 && m[0].type == am::ECastWelcome && m[1].type == am::ECastViewers);
	if (m.size() == 2)
		{
		CHECK(Le16(m[0].p.data()) == 1 && Le16(m[0].p.data() + 2) == 2 && (Le32(m[0].p.data() + 4) & 1));
		CHECK(Le32(m[0].p.data() + 8) == 65536);          // (the largest UPDATE payload taken)
		CHECK(std::string(m[0].p.begin() + 12, m[0].p.end()) == "http://192.168.1.50/screen");
		CHECK(m[1].p[0] == 0 && m[1].p[1] == 0);
		}
	CHECK(r.cast.LinkUp() && strcmp(r.cast.PsionName(), "My 5mx") == 0);
	// the keyframe, then two changes: each acknowledged
	unsigned seed = 1;
	ps.Box(0, 0, 640, 18, 0);
	for (int y = 20; y < 228; y += 12) ps.Text(y, seed);
	r.ToAtom(ps.Update(true));
	m = r.Take();
	CHECK(m.size() == 1 && m[0].type == am::ECastAck && Le32(m[0].p.data()) == 1);
	CHECK(memcmp(r.cast.Screen(), ps.frame.data(), ps.frame.size()) == 0);
	ps.Box(100, 100, 50, 20, 7);
	Bytes u2 = ps.Update(false);
	ps.Scroll(seed);
	Bytes u3 = ps.Update(false);
	Bytes both = u2;
	both.insert(both.end(), u3.begin(), u3.end());
	r.ToAtom(both);                              // (two in one read: one ACK, for the latest)
	m = r.Take();
	CHECK(m.size() == 1 && m[0].type == am::ECastAck && Le32(m[0].p.data()) == 3);
	CHECK(memcmp(r.cast.Screen(), ps.frame.data(), ps.frame.size()) == 0);
	// a damaged update (bad CRC): KEYREQ; what follows is not applied until a keyframe
	ps.Box(10, 30, 200, 40, 3);
	Bytes u4 = ps.Update(false);
	u4[20] ^= 1;
	r.ToAtom(u4);
	m = r.Take();
	CHECK(Count(m, am::ECastKeyReq) == 1 && Count(m, am::ECastAck) == 0);
	ps.Box(300, 30, 20, 40, 9);
	r.ToAtom(ps.Update(false));                  // seq 5: acknowledged, not applied (seq 4 went missing)
	m = r.Take();
	CHECK(Count(m, am::ECastAck) == 1 && Count(m, am::ECastKeyReq) == 0);
	CHECK(Count(m, am::ECastPing) == 1);          // (discarded: a PING with the last seq taken)
	for (const Msg& x : m)
		if (x.type == am::ECastPing)
			CHECK(x.p.size() == 8 && Le32(x.p.data() + 4) == 5);
	CHECK(memcmp(r.cast.Screen(), ps.frame.data(), ps.frame.size()) != 0);
	r.Ms(3100);                                  // no keyframe yet: asked again
	m = r.Take();
	CHECK(Count(m, am::ECastKeyReq) == 1);
	r.ToAtom(ps.Update(true));
	m = r.Take();
	CHECK(Count(m, am::ECastAck) == 1);
	CHECK(memcmp(r.cast.Screen(), ps.frame.data(), ps.frame.size()) == 0);
	// a body that does not decode (good CRC, rubbish inside): KEYREQ
	Bytes junk(4 + 50, 0xa5);
	junk[0] = (uint8_t)(++ps.seq);
	junk[1] = junk[2] = junk[3] = 0;
	r.ToAtom(Frame(am::ECastUpdate, 0, junk));
	m = r.Take();
	CHECK(Count(m, am::ECastKeyReq) == 1 && Count(m, am::ECastAck) == 1);
	CHECK(r.cast.Errors() >= 2);
	r.ToAtom(ps.Update(true));
	r.Take();
	CHECK(memcmp(r.cast.Screen(), ps.frame.data(), ps.frame.size()) == 0);
	// STATUS: its fields, and the PING it answered
	r.Ms(5200);
	m = r.Take();
	CHECK(Count(m, am::ECastPing) == 1);
	r.Ms(100);
	r.ToAtom(Psion::Status(64, true));
	CHECK(r.cast.Battery() == 64 && r.cast.ControlAllowed());
	// DEVINFO: the digitiser's size, to the viewers
	Bytes di(4);
	di[0] = 695 & 255; di[1] = 695 >> 8; di[2] = 280 & 255; di[3] = 280 >> 8;
	r.ToAtom(Frame(am::ECastDevInfo, 0, di));
	CHECK(r.cast.DigitiserW() == 695 && r.cast.DigitiserH() == 280);
	// silence: closed after 20 s
	CHECK(r.cast.LinkAlive(r.id));
	r.Ms(19000);
	CHECK(r.cast.LinkAlive(r.id));
	r.Ms(1500);
	CHECK(!r.cast.LinkAlive(r.id));
	r.cast.LinkClose(r.id);
	CHECK(!r.cast.LinkUp());
	// a new link, then BYE
	r.id = r.cast.LinkOpen(am::Cast::ERouteModem);
	r.ToAtom(Psion::Hello());
	r.ToAtom(Frame(am::ECastBye, 0, Bytes()));
	CHECK(!r.cast.LinkAlive(r.id));
	r.cast.LinkClose(r.id);
	// a HELLO it cannot take: BYE back
	r.id = r.cast.LinkOpen(am::Cast::ERouteTcp);
	Bytes h = Psion::Hello();
	h[8 + 2] = 0x80; h[8 + 3] = 0x02;            // (width 640 -> 0x280... made 0x0280 + odd below)
	Bytes p(12, 0);
	p[0] = 1; p[2] = 0x81; p[3] = 2; p[4] = 240;   // 641 wide: odd
	r.ToAtom(Frame(am::ECastHello, 0, p));
	m = r.Take();
	CHECK(Count(m, am::ECastBye) == 1 && !r.cast.LinkAlive(r.id));
	r.cast.LinkClose(r.id);
	// a second connection replaces the first
	uint32_t a = r.cast.LinkOpen(am::Cast::ERouteTcp);
	uint32_t b = r.cast.LinkOpen(am::Cast::ERouteTcp);
	CHECK(a && b && a != b && !r.cast.LinkAlive(a) && r.cast.LinkAlive(b));
	// off: refused
	r.cast.SetEnabled(false);
	CHECK(r.cast.LinkOpen(am::Cast::ERouteTcp) == 0);
	}

static void TestTokens()
	{
	Rig r;
	char t[32], t2[32], why[64];
	CHECK(r.cast.NewToken(false, t, sizeof(t)) && strlen(t) == 24);
	CHECK(r.cast.NewToken(true, t2, sizeof(t2)) && strcmp(t, t2) != 0);
	CHECK(r.cast.ViewerOpen("nonsense", why, sizeof(why)) < 0 && strstr(why, "token"));
	int v = r.cast.ViewerOpen(t, why, sizeof(why));
	CHECK(v >= 0);
	CHECK(r.cast.ViewerOpen(t, why, sizeof(why)) < 0);        // single use
	r.Ms(61000);
	CHECK(r.cast.ViewerOpen(t2, why, sizeof(why)) < 0);       // expired
	CHECK(r.cast.NewToken(false, t, sizeof(t)));
	r.Ms(59000);
	r.cast.ViewerIn(v, ClientFrame(am::EWsText, "{\"t\":\"ping\",\"n\":1}").data(), 20);   // (keep v alive)
	CHECK(r.cast.ViewerOpen(t, why, sizeof(why)) >= 0);       // (just in time)
	int n = 0;
	for (int i = 0; i < 6; i++)
		{
		char x[32];
		r.cast.NewToken(false, x, sizeof(x));
		if (r.cast.ViewerOpen(x, why, sizeof(why)) >= 0) n++;
		}
	CHECK(n == 2 && strstr(why, "viewers"));                  // (four at most)
	r.cast.SetEnabled(false);
	CHECK(!r.cast.NewToken(false, t, sizeof(t)));
	}

static void TestWebSocket()
	{
	uint8_t h[20];
	am::Sha1((const uint8_t*)"abc", 3, h);
	static const uint8_t kAbc[20] = { 0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d };
	CHECK(memcmp(h, kAbc, 20) == 0);
	std::string big(1000, 'a');
	am::Sha1((const uint8_t*)big.data(), big.size(), h);
	static const uint8_t kThousand[20] = { 0x29, 0x1e, 0x9a, 0x6c, 0x66, 0x99, 0x49, 0x49, 0xb5, 0x7b, 0xa5, 0xe6, 0x50, 0x36, 0x1e, 0x98, 0xfc, 0x36, 0xb1, 0xba };
	CHECK(memcmp(h, kThousand, 20) == 0);        // (two blocks and a padding block)
	char acc[29];
	CHECK(am::WsAcceptKey("dGhlIHNhbXBsZSBub25jZQ==", acc) && strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
	const char* req = "GET /cast?t=0123456789abcdef01234567&x=1 HTTP/1.1\r\nHost: 192.168.1.50:5962\r\nUpgrade: WebSocket\r\n"
		"Connection: keep-alive, Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\nrest";
	am::WsRequest rq;
	size_t used = 0;
	CHECK(am::WsParseRequest(req, 40, rq, used) == 0);
	CHECK(am::WsParseRequest(req, strlen(req), rq, used) == 1 && used == strlen(req) - 4);
	CHECK(rq.get && rq.upgrade && rq.version13 && strcmp(rq.key, "dGhlIHNhbXBsZSBub25jZQ==") == 0);
	char t[40];
	CHECK(am::WsQuery(rq.path, "t", t, sizeof(t)) && strcmp(t, "0123456789abcdef01234567") == 0);
	CHECK(!am::WsQuery(rq.path, "q", t, sizeof(t)));
	const char* plain = "GET /cast HTTP/1.1\r\nHost: x\r\n\r\n";
	CHECK(am::WsParseRequest(plain, strlen(plain), rq, used) == 1 && !rq.upgrade);
	CHECK(am::WsParseRequest("not http at all\r\n\r\n", 19, rq, used) == -1);
	// frames: text, fragmented with a ping in between, then errors
	struct Got { std::vector<std::pair<int, std::string>> m; } got;
	auto on = [](void* c, int op, const uint8_t* p, size_t n) { ((Got*)c)->m.push_back({ op, std::string((const char*)p, n) }); };
	am::WsReader rd;
	Bytes s = ClientFrame(am::EWsText, "{\"t\":\"full\"}");
	Bytes f1 = ClientFrame(am::EWsText, "hel", false), pg = ClientFrame(am::EWsPing, "p"), f2 = ClientFrame(am::EWsCont, "lo");
	s.insert(s.end(), f1.begin(), f1.end());
	s.insert(s.end(), pg.begin(), pg.end());
	s.insert(s.end(), f2.begin(), f2.end());
	Bytes e = ClientFrame(am::EWsText, "");
	s.insert(s.end(), e.begin(), e.end());
	for (size_t i = 0; i < s.size(); i++)
		CHECK(rd.Feed(&s[i], 1, on, &got) == 1);
	CHECK(got.m.size() == 4);
	if (got.m.size() == 4)
		{
		CHECK(got.m[0].first == am::EWsText && got.m[0].second == "{\"t\":\"full\"}");
		CHECK(got.m[1].first == am::EWsPing && got.m[1].second == "p");
		CHECK(got.m[2].first == am::EWsText && got.m[2].second == "hello");
		CHECK(got.m[3].first == am::EWsText && got.m[3].second.empty());
		}
	am::WsReader r2;
	uint8_t unmasked[] = { 0x81, 0x01, 'x' };
	CHECK(r2.Feed(unmasked, 3, on, &got) < 0 && r2.CloseCode() == 1002);
	am::WsReader r3;
	Bytes huge = ClientFrame(am::EWsText, std::string(2000, 'x'));
	CHECK(r3.Feed(huge.data(), huge.size(), on, &got) < 0 && r3.CloseCode() == 1009);
	uint8_t hd[10];
	CHECK(am::WsHeader(hd, am::EWsBinary, 100) == 2 && hd[0] == 0x82 && hd[1] == 100);
	CHECK(am::WsHeader(hd, am::EWsBinary, 300) == 4 && hd[1] == 126 && hd[2] == 1 && hd[3] == 44);
	CHECK(am::WsHeader(hd, am::EWsBinary, 76805) == 10 && hd[1] == 127 && hd[7] == 1 && hd[8] == 0x2c && hd[9] == 0x05);
	}

// two viewers, one fast and one slow; random changes and scrolls from the
// real encoder: both end up pixel-exact, and the slow one was sent less
static void TestViewers()
	{
	Rig r;
	Psion ps;
	r.id = r.cast.LinkOpen(am::Cast::ERouteTcp);
	r.ToAtom(Psion::Hello());
	unsigned seed = 7;
	ps.Box(0, 0, 640, 18, 0);
	for (int y = 20; y < 228; y += 12) ps.Text(y, seed);
	r.ToAtom(ps.Update(true));
	r.Take();
	int fast = r.Viewer(), slow = r.Viewer();
	CHECK(fast >= 0 && slow >= 0);
	std::vector<Msg> m = r.Take();
	CHECK(Count(m, am::ECastViewers) >= 1 && Count(m, am::ECastKeyReq) == 1);   // (the first viewer: a keyframe)
	r.ToAtom(ps.Update(true));
	r.Take();
	ViewModel vf, vs;
	r.Pump(fast, vf);
	CHECK(vf.fulls == 1 && vf.Same(r.cast.Screen()));
	CHECK(vf.texts.size() >= 2 && vf.texts[0].find("\"t\":\"hello\"") != std::string::npos);
	size_t fastBytes = 0;
	for (int step = 0; step < 300; step++)
		{
		seed = seed * 1103515245 + 12345;
		int what = (seed >> 16) % 10;
		if (what < 3)
			ps.Scroll(seed);
		else if (what < 8)
			ps.Box((int)((seed >> 8) % 600), 20 + (int)((seed >> 4) % 200), 8 + (int)(seed % 120), 4 + (int)((seed >> 12) % 30),
				(int)((seed >> 20) % 16));
		else
			ps.Text(20 + 12 * (int)((seed >> 9) % 17), seed);
		Bytes u = ps.Update(false);
		if (!u.empty())
			r.ToAtom(u);
		r.Take();
		CHECK(memcmp(r.cast.Screen(), ps.frame.data(), ps.frame.size()) == 0);
		size_t before = vf.bytes;
		r.Pump(fast, vf);
		fastBytes += vf.bytes - before;
		CHECK(vf.Same(r.cast.Screen()));
		if (step % 25 == 0)
			r.Pump(slow, vs, 3000);                  // (the slow viewer: 3 KB now and then)
		}
	r.Pump(slow, vs);
	CHECK(vs.Same(r.cast.Screen()) && vf.Same(r.cast.Screen()));
	CHECK(vs.bytes < vf.bytes / 2);
	CHECK(vf.scrolls > 20);
	printf("  viewers: fast %zu bytes (%d rects, %d scrolls), slow %zu bytes (%d rects, %d scrolls)\n", vf.bytes, vf.rects,
		vf.scrolls, vs.bytes, vs.rects, vs.scrolls);
	// a repaint asked for
	r.cast.ViewerIn(fast, ClientFrame(am::EWsText, "{\"t\":\"full\"}").data(), 18);
	r.Pump(fast, vf);
	CHECK(vf.fulls == 2 && vf.Same(r.cast.Screen()));
	// the ping answered
	Bytes pg = ClientFrame(am::EWsText, "{\"t\":\"ping\",\"n\":42}");
	r.cast.ViewerIn(fast, pg.data(), pg.size());
	r.Pump(fast, vf);
	CHECK(vf.texts.back() == "{\"t\":\"pong\",\"n\":42}");
	// a viewer that goes quiet is closed after 45 s (the slow one keeps pinging)
	for (int i = 0; i < 10; i++)
		{
		r.Ms(5000);
		r.cast.ViewerIn(slow, pg.data(), pg.size());
		r.ToAtom(Psion::Status());
		r.Pump(fast, vf);
		r.Pump(slow, vs);
		}
	CHECK(r.cast.ViewerDone(fast) && !r.cast.ViewerDone(slow));
	r.cast.ViewerClose(fast);
	CHECK(r.cast.Viewers() == 1);
	// the link goes: the viewer is told
	r.cast.LinkClose(r.id);
	r.Pump(slow, vs);
	bool bye = false, down = false;
	for (const std::string& t : vs.texts)
		{
		if (t == "{\"t\":\"bye\"}") bye = true;
		if (t.find("\"linkUp\":false") != std::string::npos) down = true;
		}
	CHECK(bye && down);
	CHECK(vs.Same(r.cast.Screen()));              // (the last picture stays)
	}

static void TestInput()
	{
	Rig r;
	Psion ps;
	r.id = r.cast.LinkOpen(am::Cast::ERouteTcp);
	r.ToAtom(Psion::Hello(3));
	r.ToAtom(ps.Update(true));
	int watch = r.Viewer(false), ctl = r.Viewer(true);
	r.Take();
	auto say = [&](int v, const char* j) { Bytes f = ClientFrame(am::EWsText, j); r.cast.ViewerIn(v, f.data(), f.size()); };
	say(watch, "{\"t\":\"pen\",\"e\":\"down\",\"x\":10,\"y\":20}");
	r.Ms(100);
	CHECK(Count(r.Take(), am::ECastInput) == 0);  // (no control asked for)
	say(ctl, "{\"t\":\"pen\",\"e\":\"down\",\"x\":-27,\"y\":262}");
	say(ctl, "{\"t\":\"pen\",\"e\":\"up\",\"x\":-27,\"y\":262}");
	say(ctl, "{\"t\":\"key\",\"e\":\"down\",\"sc\":148}");
	say(ctl, "{\"t\":\"key\",\"e\":\"up\",\"sc\":148}");
	say(ctl, "{\"t\":\"char\",\"c\":163}");
	r.Ms(100);
	std::vector<Msg> m = r.Take();
	CHECK(Count(m, am::ECastInput) == 1);
	for (const Msg& x : m)
		if (x.type == am::ECastInput)
			{
			CHECK(x.p.size() == 40);
			if (x.p.size() == 40)
				{
				CHECK(x.p[0] == 1 && (int16_t)Le16(&x.p[2]) == -27 && Le16(&x.p[4]) == 262);
				CHECK(x.p[8] == 3 && x.p[16] == 4 && Le16(&x.p[18]) == 148 && x.p[24] == 5 && x.p[32] == 6 && Le16(&x.p[34]) == 163);
				}
			}
	// the Psion turns control off (STATUS): input stops
	r.ToAtom(Psion::Status(80, false));
	say(ctl, "{\"t\":\"key\",\"e\":\"down\",\"sc\":4}");
	r.Ms(100);
	CHECK(Count(r.Take(), am::ECastInput) == 0);
	// the viewer count with control reaches the Psion
	r.ToAtom(Psion::Status(80, true));
	r.Ms(100);
	m = r.Take();
	(void)m;
	CHECK(r.cast.ViewersWithControl() == 1);
	}

// the modem route: ATDT192.168.7.1:5961 is a call to PsiCast, in the modem
static void TestModemRoute()
	{
	FakeHal hal;
	hal.webInfo = "http://192.168.1.50/";
	std::vector<uint8_t> ring(64 * 1024);
	am::Cast cast(hal);
	am::Modem modem(hal, ring.data(), ring.size());
	modem.SetCast(&cast);
	modem.Begin();
	auto run = [&](uint32_t ms) { for (uint32_t t = 0; t < ms * 1000; t += 200) { modem.Loop(); cast.Tick(); hal.Advance(200); } };
	auto type = [&](const char* s) { while (*s) hal.rx.push_back((uint8_t)*s++); };
	run(50);
	CHECK(cast.Enabled());                       // (on as standard)
	type("ATDT192.168.7.1:5961\r");
	run(200);
	std::string w = hal.Wire();
	CHECK(w.find("CONNECT") != std::string::npos && modem.CastCall());
	CHECK(hal.tcpHost.empty());                  // (no outside connection)
	size_t mark = hal.wire.size();
	Bytes h = Psion::Hello();
	for (uint8_t b : h) hal.rx.push_back(b);
	run(300);
	Bytes back;
	for (size_t i = mark; i < hal.wire.size(); i++) back.push_back(hal.wire[i].c);
	std::vector<Msg> m = Parse(back);
	CHECK(m.size() >= 2 && m[0].type == am::ECastWelcome);
	CHECK(cast.LinkUp() && cast.Route() == am::Cast::ERouteModem);
	// "+++" inside a message, with a guard time's quiet either side: data
	{
	Bytes odd = Frame(0x7f, 0, Bytes({ 'a', 'b', '+', '+', '+', 'c' }));   // (a type it ignores)
	for (size_t i = 0; i < 10; i++) hal.rx.push_back(odd[i]);
	run(1500);
	for (size_t i = 10; i < 13; i++) hal.rx.push_back(odd[i]);
	run(1500);
	for (size_t i = 13; i < odd.size(); i++) hal.rx.push_back(odd[i]);
	run(300);
	CHECK(modem.CastCall() && modem.Online() && cast.LinkUp() && cast.Errors() == 0);
	CHECK(hal.Wire().find("OK", mark) == std::string::npos);
	}
	// +++ and ATH between messages: the link ends with the call
	run(1000);
	type("+++");
	run(1200);
	type("ATH\r");
	run(200);
	CHECK(!modem.CastCall() && !cast.LinkUp());
	// off: the dial gives NO CARRIER and opens nothing
	type("AT$CAST=0\r");
	run(100);
	CHECK(!cast.Enabled());
	mark = hal.Wire().size();
	type("ATDT192.168.7.1:5961\r");
	run(200);
	CHECK(hal.Wire().substr(mark).find("NO CARRIER") != std::string::npos && hal.tcpHost.empty());
	type("AT$CAST?\r");
	run(100);
	CHECK(hal.Wire().find("0") != std::string::npos);
	// another address or port is an ordinary call, as before
	type("AT$CAST=1\r");
	run(100);
	type("ATDT192.168.7.1:5962\r");
	run(200);
	CHECK(hal.tcpHost == "192.168.7.1" && hal.tcpPort == 5962 && !modem.CastCall());
	}

int main()
	{
	TestFraming();
	TestSession();
	TestTokens();
	TestWebSocket();
	TestViewers();
	TestInput();
	TestModemRoute();
	printf("test_cast: %d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
	}
