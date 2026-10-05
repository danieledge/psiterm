// test_usbnet.cpp - the USB network framing the ESP32-S3's USB host uses:
// CDC-NCM transfer blocks (ncm.cpp) and Apple's two-byte frames
// (ipheth.cpp). No USB here: the functions are pure.
//   make -C hosttest test
// MIT licence (see LICENSE at the top of the repository).
#include "usbnet/ncm.h"
#include "usbnet/ipheth.h"
#include <stdio.h>
#include <string.h>
#include <vector>

static int gChecks = 0, gFails = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void Le16(std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = (uint8_t)x; v[at + 1] = (uint8_t)(x >> 8); }
static void Le32(std::vector<uint8_t>& v, size_t at, uint32_t x) { for (int i = 0; i < 4; i++) v[at + i] = (uint8_t)(x >> (8 * i)); }

static am::NcmParams Params()
	{
	// as a phone answers GET_NTB_PARAMETERS: 16- and 32-bit, in 16 KB, align 4, out 2 KB
	std::vector<uint8_t> p(28, 0);
	Le16(p, 0, 28); Le16(p, 2, 3);
	Le32(p, 4, 16384); Le16(p, 8, 4); Le16(p, 10, 0); Le16(p, 12, 4);
	Le32(p, 16, 2048); Le16(p, 20, 4); Le16(p, 22, 0); Le16(p, 24, 4); Le16(p, 26, 8);
	am::NcmParams out;
	CHECK(am::NcmParseParams(p.data(), p.size(), out));
	CHECK(out.ntbInMaxSize == 16384 && out.ntbOutMaxSize == 2048 && out.ndpOutAlignment == 4 && out.ntb32);
	return out;
	}

static void TestParams()
	{
	am::NcmParams p = Params();
	// too short
	uint8_t s[10] = { 0 };
	CHECK(!am::NcmParseParams(s, sizeof(s), p));
	// absurd values are tamed
	std::vector<uint8_t> q(28, 0);
	Le16(q, 24, 3);                        // alignment 3: not a power of two
	Le16(q, 20, 0);                        // divisor 0
	Le32(q, 16, 1);                        // out max 1
	CHECK(am::NcmParseParams(q.data(), q.size(), p));
	CHECK(p.ndpOutAlignment == 4 && p.ndpOutDivisor == 4 && p.ntbOutMaxSize == 2048);
	}

static void TestBuildAndParse()
	{
	am::NcmParams p = Params();
	uint8_t frame[100];
	for (int i = 0; i < 100; i++) frame[i] = (uint8_t)(i * 3);
	uint8_t ntb[2048];
	size_t n = am::NcmBuild(ntb, sizeof(ntb), frame, sizeof(frame), 7, p);
	CHECK(n > 12 + 16 + 100 - 1 && n <= 12 + 16 + 100 + 8);
	CHECK(memcmp(ntb, "NCMH", 4) == 0);
	CHECK(ntb[6] == 7 && ntb[7] == 0);     // the sequence
	// parsed back: one datagram, the same bytes
	am::NcmDatagram d[4];
	int c = am::NcmParse(ntb, n, d, 4);
	CHECK(c == 1);
	CHECK(c == 1 && d[0].len == 100 && memcmp(d[0].data, frame, 100) == 0);
	// the datagram's offset obeys the divisor and remainder
	p.ndpOutDivisor = 8; p.ndpOutRemainder = 2;
	n = am::NcmBuild(ntb, sizeof(ntb), frame, 60, 1, p);
	CHECK(n && am::NcmParse(ntb, n, d, 4) == 1 && ((size_t)(d[0].data - ntb)) % 8 == 2);
	// too big for the device, or for the buffer, or an empty frame
	CHECK(am::NcmBuild(ntb, sizeof(ntb), frame, 0, 1, p) == 0);
	CHECK(am::NcmBuild(ntb, 64, frame, 100, 1, p) == 0);
	p.ntbOutMaxSize = 100;
	CHECK(am::NcmBuild(ntb, sizeof(ntb), frame, 100, 1, p) == 0);
	uint8_t big[2000];
	p.ntbOutMaxSize = 4096;
	CHECK(am::NcmBuild(ntb, sizeof(ntb), big, sizeof(big), 1, p) == 0);   // over the Ethernet frame size
	}

static void TestParseGarbage()
	{
	am::NcmDatagram d[8];
	uint8_t z[64] = { 0 };
	CHECK(am::NcmParse(z, sizeof(z), d, 8) == -1);
	CHECK(am::NcmParse(z, 4, d, 8) == -1);
	// a proper NTB with three datagrams in one NDP, and the terminator
	std::vector<uint8_t> v(200, 0xee);
	memcpy(&v[0], "NCMH", 4); Le16(v, 4, 12); Le16(v, 6, 1); Le16(v, 8, 200); Le16(v, 10, 12);
	memcpy(&v[12], "NCM0", 4); Le16(v, 16, 8 + 4 * 4); Le16(v, 18, 0);
	Le16(v, 20, 40); Le16(v, 22, 10);
	Le16(v, 24, 60); Le16(v, 26, 20);
	Le16(v, 28, 100); Le16(v, 30, 50);
	Le16(v, 32, 0); Le16(v, 34, 0);
	int c = am::NcmParse(v.data(), v.size(), d, 8);
	CHECK(c == 3 && d[1].len == 20 && d[2].data == v.data() + 100);
	// more datagrams than room: the count is clipped
	CHECK(am::NcmParse(v.data(), v.size(), d, 2) == 2);
	// a datagram past the end of the block
	Le16(v, 28, 190); Le16(v, 30, 50);
	CHECK(am::NcmParse(v.data(), v.size(), d, 8) == -1);
	Le16(v, 28, 100);
	// an NDP index past the end; a bad NDP signature; a block longer than the transfer
	Le16(v, 10, 199); CHECK(am::NcmParse(v.data(), v.size(), d, 8) == -1); Le16(v, 10, 12);
	v[12] = 'X'; CHECK(am::NcmParse(v.data(), v.size(), d, 8) == -1); v[12] = 'N';
	Le16(v, 8, 300); CHECK(am::NcmParse(v.data(), v.size(), d, 8) == -1); Le16(v, 8, 200);
	// an NDP chain that points to itself ends
	Le16(v, 18, 12);
	CHECK(am::NcmParse(v.data(), v.size(), d, 8) == 3);
	// the CRC variant is accepted
	memcpy(&v[12], "NCM1", 4);
	CHECK(am::NcmParse(v.data(), v.size(), d, 8) == 3);
	}

static void TestIpheth()
	{
	CHECK(am::IphethMatch(0x05ac, 0xff, 0xfd, 1));
	CHECK(!am::IphethMatch(0x18d1, 0xff, 0xfd, 1));     // a Google phone
	CHECK(!am::IphethMatch(0x05ac, 0xff, 0xfe, 2));     // the Apple Mobile Device (usbmux) interface
	uint8_t on = 0x04, off = 0x00;
	CHECK(am::IphethCarrier(&on, 1) && !am::IphethCarrier(&off, 1) && !am::IphethCarrier(&on, 0));
	uint8_t frame[64];
	for (int i = 0; i < 64; i++) frame[i] = (uint8_t)i;
	uint8_t out[80];
	size_t n = am::IphethPack(out, sizeof(out), frame, 64);
	CHECK(n == 66 && out[0] == 0 && out[1] == 0 && memcmp(out + 2, frame, 64) == 0);
	const uint8_t* f; size_t fl;
	CHECK(am::IphethUnpack(out, n, f, fl) && fl == 64 && memcmp(f, frame, 64) == 0);
	CHECK(!am::IphethUnpack(out, 2, f, fl));
	CHECK(am::IphethPack(out, 10, frame, 64) == 0);
	uint8_t big[1600];
	CHECK(am::IphethPack(big, sizeof(big), big, 1515) == 0 && am::IphethPack(big, sizeof(big), frame, 0) == 0);
	// a transfer longer than a frame is clipped
	uint8_t longIn[1600] = { 0 };
	CHECK(am::IphethUnpack(longIn, sizeof(longIn), f, fl) && fl == 1514);
	}

int main()
	{
	printf("ncm parameters\n"); TestParams();
	printf("ncm build and parse\n"); TestBuildAndParse();
	printf("ncm garbage\n"); TestParseGarbage();
	printf("ipheth\n"); TestIpheth();
	printf("%d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
	}
