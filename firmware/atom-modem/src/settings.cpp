// settings.cpp - see settings.h. MIT licence (see LICENSE at the top of the repository).
#include "settings.h"
#include "proxy.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace am {

void AutoPacing(uint32_t aBaud, bool aFlow, uint32_t& aRate, uint16_t& aBurst, uint16_t& aGap)
	{
	// The Psion 5mx's UART (CL-PS7111) has a 16-byte receive FIFO and the
	// apps give the serial driver a 16 KB buffer (SetReceiveBufferLength).
	// Bursts of 16 bytes can never overrun the FIFO even if its interrupt
	// is held off for a whole burst; the average rate is held well below
	// the line rate so the gaps between bursts let the driver catch up,
	// and a 16 KB buffer lasts 3-4 s if the app stops reading for a while
	// (a page being laid out, a flash write). See README "Pacing".
	// From 230400 up the same holds at the same share of the line rate;
	// a 16-byte burst then leaves the driver 0.69 ms (230400) or 0.35 ms
	// (460800) to answer the FIFO interrupt, which only the kernel driver
	// that sets those speeds can promise.
	// With RTS/CTS the driver drops RTS at 75 % of its buffer and the UART
	// stops within a byte, so the buffer no longer has to cover the app's
	// pauses: 80 % of the line rate, the bursts unchanged.
	aGap = 0;
	aBurst = 16;
	if (aBaud >= 921600)      aRate = 44000;  // 48 % of 92160 bytes/s
	else if (aBaud >= 460800) aRate = 22000;  // 48 % of 46080
	else if (aBaud >= 230400) aRate = 11000;  // 48 % of 23040
	else if (aBaud >= 115200) aRate = 5500;   // 48 % of 11520
	else if (aBaud >= 57600)  aRate = 4000;   // 69 % of 5760
	else if (aBaud >= 38400)  aRate = 3000;   // 78 % of 3840
	else                      aRate = 0;      // slow enough as it is
	if (aFlow && aRate)
		aRate = aBaud / 10 * 8 / 10;          // 80 % of the line rate
	}

void FactoryDefaults(Settings& aS)
	{
	memset(&aS, 0, sizeof(aS));
	aS.magic = kMagic;
	aS.baud = 115200;
	aS.echo = 1;
	aS.verbose = 1;
	aS.quiet = 0;
	aS.dcdMode = 1;
	aS.s2 = '+';
	aS.s12 = 40;                 // 0.8 s: PsiTerm/PsiMail/PsiWeb wait 1.1 s each side
	aS.paceAuto = 1;
	AutoPacing(aS.baud, false, aS.paceRate, aS.paceBurst, aS.paceGap);
	aS.swapPins = 0;
	aS.dcdPin = -1;
	aS.proxy = 0;                // the web proxy on (when psiproxy is dialled)
	}

int ProxyMode(const Settings& aS)
	{
	return aS.proxy == 0 ? (int)Proxy::EOn : aS.proxy == 1 ? (int)Proxy::EOff : (int)aS.proxy;
	}

void SetProxyMode(Settings& aS, int aMode)
	{
	aS.proxy = aMode == Proxy::EOn ? 0 : aMode == Proxy::EOff ? 1 : (uint8_t)aMode;
	}

// ===== the 2.0 record =========================================================

void FactoryDefaults2(Settings2& aS)
	{
	memset(&aS, 0, sizeof(aS));
	aS.magic = kMagic2;
	aS.version = kSettings2Version;
	aS.length = (uint16_t)sizeof(Settings2);
	aS.uplink = EUplinkAuto;
	aS.usbHost = 0;
	aS.flow = 0;
	aS.flowSwap = 0;
	for (int i = 0; i < kPinCount; i++)
		aS.pins[i] = -1;
	aS.web = EWebOn;
	aS.tls = 0;
	aS.tlsVerify = 1;
	aS.img = 0;
	aS.imgWidth = 300;
	aS.imgMaxKB = 64;
	aS.exec = 0;
	aS.logLevel = 1;
	aS.ppp = 1;                // a numeric dial (ATD777) is PPP: the Psion's "Psion Internet" is its default first send
	static const uint16_t kPorts[] = { 443, 465, 993, 995 };
	for (size_t i = 0; i < sizeof(kPorts) / sizeof(kPorts[0]); i++)
		aS.tlsPorts[i] = kPorts[i];
	// a TCP connect to a public resolver: no DNS needed to tell "on the
	// WiFi" from "on the Internet"
	strncpy(aS.chkHost, "1.1.1.1:53", sizeof(aS.chkHost) - 1);
	}

bool AcceptSettings2(Settings2& aS, size_t aLength)
	{
	if (aLength < 8 || aS.magic != kMagic2 || aS.version == 0)
		return false;
	size_t have = aS.length;
	if (have > aLength) have = aLength;
	if (have > sizeof(Settings2)) have = sizeof(Settings2);
	if (have < sizeof(Settings2))
		{
		// an older, shorter record: the rest at its factory values
		Settings2 f;
		FactoryDefaults2(f);
		memcpy((uint8_t*)&f, (const uint8_t*)&aS, have);
		aS = f;
		}
	aS.version = kSettings2Version;
	aS.length = (uint16_t)sizeof(Settings2);
	// strings must end
	aS.webPass[sizeof(aS.webPass) - 1] = 0;
	aS.apPass[sizeof(aS.apPass) - 1] = 0;
	aS.execHost[sizeof(aS.execHost) - 1] = 0;
	aS.execToken[sizeof(aS.execToken) - 1] = 0;
	aS.chkHost[sizeof(aS.chkHost) - 1] = 0;
	if (aS.imgWidth < 64 || aS.imgWidth > 640) aS.imgWidth = 300;
	if (aS.imgMaxKB < 16 || aS.imgMaxKB > 512) aS.imgMaxKB = 64;
	return true;
	}

void TlsPortsText(const Settings2& aS, char* aOut, size_t aMax)
	{
	size_t n = 0;
	aOut[0] = 0;
	for (int i = 0; i < kTlsPortMax && aS.tlsPorts[i]; i++)
		{
		int w = snprintf(aOut + n, aMax - n, "%s%u", i ? "," : "", aS.tlsPorts[i]);
		if (w < 0 || (size_t)w >= aMax - n)
			break;
		n += (size_t)w;
		}
	}

bool SetTlsPorts(Settings2& aS, const char* aText)
	{
	uint16_t ports[kTlsPortMax];
	int n = 0;
	const char* p = aText;
	while (*p == ' ') p++;
	while (*p)
		{
		if (*p < '0' || *p > '9')
			return false;
		long v = 0;
		while (*p >= '0' && *p <= '9')
			v = v * 10 + (*p++ - '0');
		if (v <= 0 || v > 65535 || n >= kTlsPortMax)
			return false;
		ports[n++] = (uint16_t)v;
		while (*p == ' ') p++;
		if (*p == ',')
			{
			p++;
			while (*p == ' ') p++;
			if (!*p)
				return false;
			}
		else if (*p)
			return false;
		}
	memset(aS.tlsPorts, 0, sizeof(aS.tlsPorts));
	for (int i = 0; i < n; i++)
		aS.tlsPorts[i] = ports[i];
	return true;
	}

bool TlsPort(const Settings2& aS, uint16_t aPort)
	{
	for (int i = 0; i < kTlsPortMax && aS.tlsPorts[i]; i++)
		if (aS.tlsPorts[i] == aPort)
			return true;
	return false;
	}

void PinsText(const Settings2& aS, char* aOut, size_t aMax)
	{
	snprintf(aOut, aMax, "%d,%d,%d,%d,%d", aS.pins[0], aS.pins[1], aS.pins[2], aS.pins[3], aS.pins[4]);
	}

bool SetPins(Settings2& aS, const char* aText)
	{
	int8_t pins[kPinCount];
	const char* p = aText;
	for (int i = 0; i < kPinCount; i++)
		{
		while (*p == ' ') p++;
		char* end;
		long v = strtol(p, &end, 10);
		if (end == p || v < -1 || v > 48)
			return false;
		pins[i] = (int8_t)v;
		p = end;
		while (*p == ' ') p++;
		if (i < kPinCount - 1)
			{
			if (*p != ',')
				return false;
			p++;
			}
		}
	while (*p == ' ') p++;
	if (*p)
		return false;
	// no two signals on one pin
	for (int i = 0; i < kPinCount; i++)
		for (int j = i + 1; j < kPinCount; j++)
			if (pins[i] >= 0 && pins[i] == pins[j])
				return false;
	memcpy(aS.pins, pins, sizeof(pins));
	return true;
	}

} // namespace am
