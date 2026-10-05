// schema.cpp - see schema.h. MIT licence (see LICENSE at the top of the repository).
#include "schema.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

namespace am {

static const char* const kUplinkNames[] = { "AUTO", "WIFI", "USB", 0 };
static const char* const kWebNames[] = { "OFF", "ON", "AP", 0 };

#define F1(field) SR1, (uint16_t)offsetof(Settings, field), (uint8_t)sizeof(((Settings*)0)->field)
#define F2(field) SR2, (uint16_t)offsetof(Settings2, field), (uint8_t)sizeof(((Settings2*)0)->field)

static const SchemaEntry kSchema[] =
	{
	// WiFi
	{ "SSID", STStr, F1(ssid), 0, 32, 0, SAWifi, SGWifi, "Network", "The WiFi network to join (a home network, or a phone's hotspot)" },
	{ "PASS", STSecret, F1(pass), 0, 64, 0, SAWifi, SGWifi, "Password", "Its password. Joins once both are set" },
	// uplink
	{ "UP", STEnum, F2(uplink), 0, 2, kUplinkNames, SAUplink, SGUplink, "Uplink", "AUTO: USB when a network device is up, else WiFi. WIFI or USB: that one only" },
	{ "USB", STBool, F2(usbHost), 0, 1, 0, SAUsb, SGUplink, "USB-C port is a host", "1: a phone or USB Ethernet device on the USB-C (at the next restart). 0: a device, for programming" },
	{ "CHK", STStr, F2(chkHost), 0, 39, 0, SANone, SGUplink, "Internet check", "host:port the modem connects to now and then, to tell a WiFi it has joined from one with the Internet behind it. Empty: no check" },
	// serial
	{ "SB", STBaud, F1(baud), 300, 921600, 0, SABaud, SGSerial, "Baud", "The serial speed. Changes after the OK; falls back in 15 s if nothing answers" },
	{ "FC", STBool, F2(flow), 0, 1, 0, SAPins, SGSerial, "RTS/CTS flow control", "1: hardware flow control on the RTS and CTS pins (needs a four-wire transceiver). 0: pacing only" },
	{ "FCSWAP", STBool, F2(flowSwap), 0, 1, 0, SAPins, SGSerial, "Swap RTS and CTS", "1: the RTS and CTS pins the other way round" },
	{ "SWAP", STBool, F1(swapPins), 0, 1, 0, SAPins, SGSerial, "Swap TX and RX", "1: the TX and RX pins the other way round" },
	{ "PINS", STPins, F2(pins), 0, 0, 0, SAPins, SGSerial, "Pins tx,rx,rts,cts,dcd", "The header pins, -1 for the board's default" },
	{ "DCD", STInt, F1(dcdPin), -1, 48, 0, SADcd, SGSerial, "DCD pin", "The pin driving an emulated DCD (through a transceiver), -1 for none" },
	{ "PR", STPace, F1(paceRate), 0, 200000, 0, SAPacing, SGSerial, "Pacing rate", "Bytes a second towards the Psion: AUTO (from the baud and flow control), 0 off, or a number" },
	{ "PB", STInt, F1(paceBurst), 1, 4096, 0, SAPaceManual, SGSerial, "Pacing burst", "Bytes sent back to back" },
	{ "PG", STInt, F1(paceGap), 0, 4096, 0, SAPaceManual, SGSerial, "Pacing gap", "Extra quiet time after each burst, ms" },
	// the web proxy
	{ "PX", STProxyMode, F1(proxy), 0, 4, 0, SANone, SGProxy, "Web proxy", "0 off, 1 simplified pages, 2 text only, 3 pages unchanged (TLS only), 4 reader" },
	{ "PZ", STBoolInv, F1(proxyNoZip), 0, 1, 0, SANone, SGProxy, "Gzip pages", "1: the proxy gzips pages for the line" },
	// TLS termination
	{ "TLS", STBool, F2(tls), 0, 1, 0, SANone, SGTls, "TLS termination", "1: a dial to one of the ports below is made over TLS by the modem; the Psion sees the plain protocol" },
	{ "TLSP", STPorts, F2(tlsPorts), 0, 0, 0, SANone, SGTls, "TLS ports", "The ports it applies to, e.g. 443,465,993,995" },
	{ "TLSV", STBool, F2(tlsVerify), 0, 1, 0, SANone, SGTls, "Check certificates", "1: the server's certificate is checked. 0 is dangerous: only for your own server" },
	// pictures
	{ "PI", STBool, F2(img), 0, 1, 0, SANone, SGPictures, "Convert pictures", "1: pictures through the proxy are scaled and dithered to 16 greys" },
	{ "PW", STInt, F2(imgWidth), 64, 640, 0, SANone, SGPictures, "Width", "At most this many pixels wide" },
	{ "PM", STInt, F2(imgMaxKB), 16, 512, 0, SANone, SGPictures, "Largest file (KB)", "Bigger pictures pass through unchanged" },
	// remote compute
	{ "XE", STBool, F2(exec), 0, 1, 0, SANone, SGExec, "Remote compute", "1: AT$EXEC and dialling psiexec are allowed" },
	{ "XH", STStr, F2(execHost), 0, 63, 0, SANone, SGExec, "Helper host:port", "Where psiexecd runs on your network" },
	{ "XK", STSecret, F2(execToken), 0, 32, 0, SANone, SGExec, "Token", "The token psiexecd expects" },
	// system
	{ "WEB", STEnum, F2(web), 0, 2, kWebNames, SAWeb, SGSystem, "Web pages", "OFF; ON (an access point when unconfigured, pages on the network with a password); AP (the access point always)" },
	{ "WEBPASS", STSecret, F2(webPass), 0, 32, 0, SAWeb, SGSystem, "Web password", "For the pages on the network (user atom). None: those pages are off" },
	{ "APPASS", STSecret, F2(apPass), 8, 32, 0, SAWeb, SGSystem, "Access point password", "WPA2, 8 to 32 characters" },
	{ "LOGL", STInt, F2(logLevel), 0, 2, 0, SANone, SGSystem, "Log level", "0 quiet, 1 normal, 2 verbose" },
	};

int SchemaCount() { return (int)(sizeof(kSchema) / sizeof(kSchema[0])); }
const SchemaEntry& SchemaAt(int aIndex) { return kSchema[aIndex]; }

const char* SchemaGroupName(TSchemaGroup aG)
	{
	switch (aG)
		{
	case SGWifi: return "WiFi";
	case SGUplink: return "Uplink";
	case SGSerial: return "Serial";
	case SGProxy: return "Web proxy";
	case SGTls: return "TLS termination";
	case SGPictures: return "Pictures";
	case SGExec: return "Remote compute";
	default: return "System";
		}
	}

static int Upper(int aC) { return (aC >= 'a' && aC <= 'z') ? aC - 32 : aC; }

static bool EqNoCase(const char* aA, const char* aB)
	{
	while (*aA && *aB)
		if (Upper((unsigned char)*aA++) != Upper((unsigned char)*aB++))
			return false;
	return *aA == 0 && *aB == 0;
	}

const SchemaEntry* SchemaFind(const char* aName)
	{
	for (int i = 0; i < SchemaCount(); i++)
		if (EqNoCase(kSchema[i].name, aName))
			return &kSchema[i];
	return 0;
	}

bool ValidBaud(long aB)
	{
	static const long kBauds[] = { 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
	for (size_t i = 0; i < sizeof(kBauds) / sizeof(kBauds[0]); i++)
		if (kBauds[i] == aB)
			return true;
	return false;
	}

static void* Field(const Config& aC, const SchemaEntry& aE)
	{
	return aE.rec == SR1 ? (void*)((uint8_t*)&aC.s + aE.offset) : (void*)((uint8_t*)&aC.s2 + aE.offset);
	}

static long GetNumber(const Config& aC, const SchemaEntry& aE)
	{
	const void* p = Field(aC, aE);
	switch (aE.size)
		{
	case 1: return aE.min < 0 ? (long)*(const int8_t*)p : (long)*(const uint8_t*)p;
	case 2: return (long)*(const uint16_t*)p;
	default: return (long)*(const uint32_t*)p;
		}
	}

static void SetNumber(Config& aC, const SchemaEntry& aE, long aV)
	{
	void* p = Field(aC, aE);
	switch (aE.size)
		{
	case 1: if (aE.min < 0) *(int8_t*)p = (int8_t)aV; else *(uint8_t*)p = (uint8_t)aV; break;
	case 2: *(uint16_t*)p = (uint16_t)aV; break;
	default: *(uint32_t*)p = (uint32_t)aV; break;
		}
	}

bool SchemaSecretSet(const Config& aC, const SchemaEntry& aE)
	{
	return ((const char*)Field(aC, aE))[0] != 0;
	}

void SchemaGet(const Config& aC, const SchemaEntry& aE, char* aOut, size_t aMax)
	{
	switch (aE.type)
		{
	case STBool: case STInt: case STBaud:
		snprintf(aOut, aMax, "%ld", GetNumber(aC, aE));
		break;
	case STBoolInv:
		snprintf(aOut, aMax, "%d", GetNumber(aC, aE) ? 0 : 1);
		break;
	case STEnum:
		{
		long v = GetNumber(aC, aE);
		int n = 0;
		while (aE.names[n]) n++;
		snprintf(aOut, aMax, "%s", v >= 0 && v < n ? aE.names[v] : "?");
		break;
		}
	case STPace:
		snprintf(aOut, aMax, "%s%lu", aC.s.paceAuto ? "AUTO " : "", (unsigned long)aC.s.paceRate);
		break;
	case STProxyMode:
		snprintf(aOut, aMax, "%d", ProxyMode(aC.s));
		break;
	case STStr:
		snprintf(aOut, aMax, "%s", (const char*)Field(aC, aE));
		break;
	case STSecret:
		snprintf(aOut, aMax, "%s", SchemaSecretSet(aC, aE) ? "(set)" : "(none)");
		break;
	case STPorts:
		TlsPortsText(aC.s2, aOut, aMax);
		break;
	case STPins:
		PinsText(aC.s2, aOut, aMax);
		break;
		}
	}

// a whole number, and nothing else on the line
static bool Whole(const char* aV, long& aOut)
	{
	while (*aV == ' ') aV++;
	if (!*aV) return false;
	char* end;
	long v = strtol(aV, &end, 10);
	while (*end == ' ') end++;
	if (*end) return false;
	aOut = v;
	return true;
	}

bool SchemaSet(Config& aC, const SchemaEntry& aE, const char* aValue)
	{
	long v;
	switch (aE.type)
		{
	case STBool: case STBoolInv:
		if (!Whole(aValue, v) || v < 0 || v > 1)
			return false;
		SetNumber(aC, aE, aE.type == STBoolInv ? !v : v);
		return true;
	case STInt:
		if (!Whole(aValue, v) || v < aE.min || v > aE.max)
			return false;
		SetNumber(aC, aE, v);
		return true;
	case STBaud:
		if (!Whole(aValue, v) || !ValidBaud(v))
			return false;
		SetNumber(aC, aE, v);
		return true;
	case STEnum:
		{
		while (*aValue == ' ') aValue++;
		for (int i = 0; aE.names[i]; i++)
			if (EqNoCase(aE.names[i], aValue))
				{
				SetNumber(aC, aE, i);
				return true;
				}
		if (Whole(aValue, v) && v >= aE.min && v <= aE.max)
			{
			SetNumber(aC, aE, v);
			return true;
			}
		return false;
		}
	case STPace:
		{
		while (*aValue == ' ') aValue++;
		if (EqNoCase(aValue, "AUTO"))
			{
			aC.s.paceAuto = 1;
			return true;
			}
		if (!Whole(aValue, v) || v < 0 || v > aE.max)
			return false;
		aC.s.paceAuto = 0;
		aC.s.paceRate = (uint32_t)v;
		return true;
		}
	case STProxyMode:
		if (!Whole(aValue, v) || v < 0 || v > 4)
			return false;
		SetProxyMode(aC.s, (int)v);
		return true;
	case STStr: case STSecret:
		{
		size_t n = strlen(aValue);
		if (n >= aE.size || (long)n > aE.max || ((long)n < aE.min && n != 0))
			return false;
		char* f = (char*)Field(aC, aE);
		memcpy(f, aValue, n);
		f[n] = 0;
		return true;
		}
	case STPorts:
		return SetTlsPorts(aC.s2, aValue);
	case STPins:
		return SetPins(aC.s2, aValue);
		}
	return false;
	}

} // namespace am
