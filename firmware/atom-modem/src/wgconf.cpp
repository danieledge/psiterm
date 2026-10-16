// wgconf.cpp - see wgconf.h. MIT licence (see LICENSE at the top of the repository).
#include "wgconf.h"
#include "settings.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace am {

void WgClear(WgConfig& aC)
	{
	// volatile writes, so a cleared key is really gone from RAM
	volatile uint8_t* p = (volatile uint8_t*)&aC;
	for (size_t i = 0; i < sizeof(aC); i++)
		p[i] = 0;
	aC.magic = kWgMagic;
	aC.version = kWgVersion;
	aC.length = (uint16_t)sizeof(WgConfig);
	}

static bool KeyField(char* aF, bool aOptional)
	{
	aF[kWgKeyText - 1] = 0;
	if (!aF[0])
		return aOptional;
	return WgKeyOk(aF);
	}

bool WgValid(const WgConfig& aC)
	{
	if (aC.magic != kWgMagic)
		return false;
	if (!aC.privKey[0] || !aC.pubKey[0] || !aC.endpoint[0] || !aC.port || !aC.addr)
		return false;
	if (aC.nAllowed == 0 || aC.nAllowed > kWgAllowedMax || aC.nDns > kWgDnsMax || aC.addrLen > 32)
		return false;
	for (int i = 0; i < aC.nAllowed; i++)
		if (aC.allowed[i].len > 32)
			return false;
	return true;
	}

bool WgAccept(WgConfig& aC, size_t aLength)
	{
	if (aLength < sizeof(WgConfig) || aC.magic != kWgMagic || aC.version == 0)
		return false;
	aC.endpoint[sizeof(aC.endpoint) - 1] = 0;
	if (!KeyField(aC.privKey, false) || !KeyField(aC.pubKey, false) || !KeyField(aC.psk, true))
		return false;
	return WgValid(aC);
	}

// ----- base64 and addresses ------------------------------------------------------

static int B64Value(int aC)
	{
	if (aC >= 'A' && aC <= 'Z') return aC - 'A';
	if (aC >= 'a' && aC <= 'z') return aC - 'a' + 26;
	if (aC >= '0' && aC <= '9') return aC - '0' + 52;
	if (aC == '+') return 62;
	if (aC == '/') return 63;
	return -1;
	}

int Base64Decode(const char* aIn, uint8_t* aOut, size_t aMax)
	{
	size_t len = strlen(aIn);
	if (len == 0 || len % 4)
		return -1;
	size_t n = 0;
	for (size_t i = 0; i < len; i += 4)
		{
		int v[4];
		int pad = 0;
		for (int k = 0; k < 4; k++)
			{
			char c = aIn[i + k];
			if (c == '=')
				{
				// padding only in the last group, only at its end
				if (i + 4 != len || k < 2)
					return -1;
				v[k] = 0;
				pad++;
				}
			else
				{
				if (pad)
					return -1;               // a character after '='
				v[k] = B64Value((unsigned char)c);
				if (v[k] < 0)
					return -1;
				}
			}
		uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
		uint8_t b[3] = { (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
		for (int k = 0; k < 3 - pad; k++)
			{
			if (n >= aMax)
				return -1;
			aOut[n++] = b[k];
			}
		}
	return (int)n;
	}

bool WgKeyOk(const char* aB64, uint8_t* aOut)
	{
	uint8_t k[33];
	if (strlen(aB64) != 44)
		return false;
	int n = Base64Decode(aB64, k, sizeof(k));
	bool ok = n == 32;
	if (ok && aOut)
		memcpy(aOut, k, 32);
	volatile uint8_t* p = k;               // (no copy of a key left on the stack)
	for (size_t i = 0; i < sizeof(k); i++)
		p[i] = 0;
	return ok;
	}

bool ParseIp4(const char* aText, uint32_t& aOut)
	{
	uint32_t ip = 0;
	const char* p = aText;
	for (int part = 0; part < 4; part++)
		{
		if (*p < '0' || *p > '9')
			return false;
		int v = 0, digits = 0;
		while (*p >= '0' && *p <= '9')
			{
			v = v * 10 + (*p++ - '0');
			if (++digits > 3 || v > 255)
				return false;
			}
		ip |= (uint32_t)v << (8 * part);
		if (part < 3)
			{
			if (*p != '.')
				return false;
			p++;
			}
		}
	if (*p)
		return false;
	aOut = ip;
	return true;
	}

void Ip4Text(uint32_t aIp, char* aOut, size_t aMax)
	{
	snprintf(aOut, aMax, "%u.%u.%u.%u", (unsigned)(aIp & 0xff), (unsigned)((aIp >> 8) & 0xff),
		(unsigned)((aIp >> 16) & 0xff), (unsigned)((aIp >> 24) & 0xff));
	}

// network order (a.b.c.d as a number) <-> the ip4_addr byte order
static uint32_t Swap(uint32_t a)
	{
	return (a >> 24) | ((a >> 8) & 0xff00) | ((a << 8) & 0xff0000) | (a << 24);
	}

uint32_t PrefixMask(int aLen)
	{
	if (aLen <= 0) return 0;
	if (aLen >= 32) return 0xffffffffu;
	return Swap(0xffffffffu << (32 - aLen));
	}

int MaskLen(uint32_t aMask)
	{
	uint32_t m = Swap(aMask);
	int n = 0;
	while (n < 32 && (m & 0x80000000u))
		{
		n++;
		m <<= 1;
		}
	return n;
	}

bool InPrefix(uint32_t aIp, uint32_t aNet, int aLen)
	{
	uint32_t m = PrefixMask(aLen);
	return (aIp & m) == (aNet & m);
	}

// ----- the parser ---------------------------------------------------------------

namespace {

int Lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

bool EqNoCase(const char* a, const char* b)
	{
	while (*a && *b)
		if (Lower((unsigned char)*a++) != Lower((unsigned char)*b++))
			return false;
	return *a == 0 && *b == 0;
	}

void Trim(char*& aS)
	{
	while (*aS == ' ' || *aS == '\t') aS++;
	size_t n = strlen(aS);
	while (n && (aS[n - 1] == ' ' || aS[n - 1] == '\t' || aS[n - 1] == '\r'))
		aS[--n] = 0;
	}

// one item of a comma-separated list, trimmed; returns the rest (0 at the end)
char* NextItem(char* aList, char*& aItem)
	{
	if (!aList)
		{
		aItem = 0;
		return 0;
		}
	char* comma = strchr(aList, ',');
	if (comma)
		*comma = 0;
	aItem = aList;
	Trim(aItem);
	return comma ? comma + 1 : 0;
	}

bool IsIp6(const char* aS) { return strchr(aS, ':') != 0; }

// "a.b.c.d/len" (len optional: 32)
bool ParsePrefix(char* aS, WgPrefix& aOut)
	{
	char* slash = strchr(aS, '/');
	int len = 32;
	if (slash)
		{
		*slash = 0;
		const char* l = slash + 1;
		if (!*l)
			return false;
		len = 0;
		int digits = 0;
		while (*l >= '0' && *l <= '9')
			{
			len = len * 10 + (*l++ - '0');
			if (++digits > 2)
				return false;
			}
		if (*l || len > 32)
			return false;
		}
	uint32_t ip;
	if (!ParseIp4(aS, ip))
		return false;
	aOut.ip = ip & PrefixMask(len);      // host bits off, as wg does
	aOut.len = (uint8_t)len;
	aOut.pad[0] = aOut.pad[1] = aOut.pad[2] = 0;
	return true;
	}

bool ParseNumber(const char* aS, long aMin, long aMax, long& aOut)
	{
	if (!*aS)
		return false;
	long v = 0;
	for (const char* p = aS; *p; p++)
		{
		if (*p < '0' || *p > '9')
			return false;
		v = v * 10 + (*p - '0');
		if (v > aMax)
			return false;
		}
	if (v < aMin)
		return false;
	aOut = v;
	return true;
	}

void Why(char* aWhy, size_t aMax, const char* aText)
	{
	if (aMax)
		snprintf(aWhy, aMax, "%s", aText);
	}

void Wipe(void* aP, size_t aN)
	{
	volatile uint8_t* p = (volatile uint8_t*)aP;
	for (size_t i = 0; i < aN; i++)
		p[i] = 0;
	}

} // namespace

bool WgParse(const char* aText, WgConfig& aOut, char* aWhy, size_t aWhyMax)
	{
	if (aWhyMax)
		aWhy[0] = 0;
	size_t total = strlen(aText);
	if (total > kWgTextMax)
		{
		Why(aWhy, aWhyMax, "The config is too long");
		return false;
		}
	static char buf[kWgTextMax + 1];      // (static: the loop task's stack is precious)
	memcpy(buf, aText, total + 1);
	static WgConfig c;
	WgClear(c);
	enum { ENone, EInterface, EPeer } section = ENone;
	int peers = 0;
	bool havePriv = false, havePub = false, haveAddr = false, haveEnd = false, haveAllowed = false;
	bool ok = true;
	char* line = buf;
	while (ok && line)
		{
		char* nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		char* next = nl ? nl + 1 : 0;
		char* l = line;
		line = next;
		Trim(l);
		if (strlen(l) > 255)
			{
			Why(aWhy, aWhyMax, "A line is too long");
			ok = false;
			break;
			}
		if (!*l || *l == '#' || *l == ';')
			continue;
		if (*l == '[')
			{
			if (EqNoCase(l, "[Interface]"))
				section = EInterface;
			else if (EqNoCase(l, "[Peer]"))
				{
				section = EPeer;
				if (++peers > 1)
					{
					Why(aWhy, aWhyMax, "Only one [Peer] is supported");
					ok = false;
					}
				}
			else
				{
				Why(aWhy, aWhyMax, "Unknown section (expected [Interface] or [Peer])");
				ok = false;
				}
			continue;
			}
		char* eq = strchr(l, '=');
		if (!eq)
			{
			Why(aWhy, aWhyMax, "A line is not \"Name = value\"");
			ok = false;
			break;
			}
		*eq = 0;
		char* key = l;
		char* val = eq + 1;
		Trim(key);
		Trim(val);
		if (section == ENone)
			{
			Why(aWhy, aWhyMax, "A setting before [Interface]");
			ok = false;
			break;
			}
		if (section == EInterface)
			{
			if (EqNoCase(key, "PrivateKey"))
				{
				if (!WgKeyOk(val))
					{
					Why(aWhy, aWhyMax, "PrivateKey is not a WireGuard key");
					ok = false;
					}
				else
					{
					memcpy(c.privKey, val, 45);
					havePriv = true;
					}
				}
			else if (EqNoCase(key, "Address"))
				{
				char* item;
				char* rest = val;
				while (!haveAddr && (rest = NextItem(rest, item), item))
					{
					if (!*item || IsIp6(item))
						continue;
					WgPrefix p;
					char tmp[40];
					snprintf(tmp, sizeof(tmp), "%s", item);
					// the address itself (host bits kept), and its length
					char* slash = strchr(tmp, '/');
					int len = 32;
					if (slash)
						{
						long lv;
						if (!ParseNumber(slash + 1, 0, 32, lv))
							{
							ok = false;
							break;
							}
						len = (int)lv;
						*slash = 0;
						}
					if (!ParseIp4(tmp, p.ip) || !p.ip)
						{
						ok = false;
						break;
						}
					c.addr = p.ip;
					c.addrLen = (uint8_t)len;
					haveAddr = true;
					}
				if (!ok)
					Why(aWhy, aWhyMax, "Address is not an IPv4 address");
				}
			else if (EqNoCase(key, "DNS"))
				{
				char* item;
				char* rest = val;
				while ((rest = NextItem(rest, item), item))
					{
					uint32_t ip;
					if (!*item || IsIp6(item) || !ParseIp4(item, ip))
						continue;                // (a search domain, or IPv6: skipped)
					if (c.nDns < kWgDnsMax)
						c.dns[c.nDns++] = ip;
					}
				}
			else if (EqNoCase(key, "ListenPort"))
				{
				long v;
				if (!ParseNumber(val, 0, 65535, v))
					{
					Why(aWhy, aWhyMax, "ListenPort is not a port");
					ok = false;
					}
				else
					c.listenPort = (uint16_t)v;
				}
			else if (EqNoCase(key, "MTU"))
				{
				long v;
				if (!ParseNumber(val, 576, 1500, v))
					{
					Why(aWhy, aWhyMax, "MTU must be 576 to 1500");
					ok = false;
					}
				else
					c.mtu = (uint16_t)v;
				}
			// anything else (Table, PostUp, SaveConfig, FwMark...) is for wg-quick on a PC: ignored
			}
		else
			{
			if (EqNoCase(key, "PublicKey"))
				{
				if (!WgKeyOk(val))
					{
					Why(aWhy, aWhyMax, "PublicKey is not a WireGuard key");
					ok = false;
					}
				else
					{
					memcpy(c.pubKey, val, 45);
					havePub = true;
					}
				}
			else if (EqNoCase(key, "PresharedKey"))
				{
				if (!WgKeyOk(val))
					{
					Why(aWhy, aWhyMax, "PresharedKey is not a WireGuard key");
					ok = false;
					}
				else
					memcpy(c.psk, val, 45);
				}
			else if (EqNoCase(key, "AllowedIPs"))
				{
				char* item;
				char* rest = val;
				while (ok && (rest = NextItem(rest, item), item))
					{
					if (!*item || IsIp6(item))
						continue;                // (::/0 and other IPv6: no IPv6 here)
					WgPrefix p;
					char tmp[40];
					snprintf(tmp, sizeof(tmp), "%s", item);
					if (!ParsePrefix(tmp, p))
						{
						Why(aWhy, aWhyMax, "AllowedIPs has an entry that is not an IPv4 prefix");
						ok = false;
						}
					else if (c.nAllowed >= kWgAllowedMax)
						{
						Why(aWhy, aWhyMax, "AllowedIPs has more than 8 IPv4 entries");
						ok = false;
						}
					else
						{
						bool dup = false;
						for (int i = 0; i < c.nAllowed; i++)
							if (c.allowed[i].ip == p.ip && c.allowed[i].len == p.len)
								dup = true;
						if (!dup)
							c.allowed[c.nAllowed++] = p;
						haveAllowed = true;
						}
					}
				}
			else if (EqNoCase(key, "Endpoint"))
				{
				char* colon = strrchr(val, ':');
				long port;
				if (*val == '[' || !colon || colon == val || strchr(val, ':') != colon)
					{
					Why(aWhy, aWhyMax, "Endpoint must be host:port (IPv4 or a name)");
					ok = false;
					}
				else
					{
					*colon = 0;
					if (!ParseNumber(colon + 1, 1, 65535, port) || strlen(val) >= sizeof(c.endpoint))
						{
						Why(aWhy, aWhyMax, "Endpoint must be host:port (IPv4 or a name)");
						ok = false;
						}
					else
						{
						for (const char* q = val; *q; q++)
							if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9')
								|| *q == '.' || *q == '-' || *q == '_'))
								ok = false;
						if (!ok)
							Why(aWhy, aWhyMax, "Endpoint's host has odd characters");
						else
							{
							snprintf(c.endpoint, sizeof(c.endpoint), "%s", val);
							c.port = (uint16_t)port;
							haveEnd = true;
							}
						}
					}
				}
			else if (EqNoCase(key, "PersistentKeepalive"))
				{
				long v;
				if (EqNoCase(val, "off"))
					c.keepalive = 0;
				else if (!ParseNumber(val, 0, 65535, v))
					{
					Why(aWhy, aWhyMax, "PersistentKeepalive is not a number of seconds");
					ok = false;
					}
				else
					c.keepalive = (uint16_t)v;
				}
			}
		}
	if (ok)
		{
		const char* missing = !havePriv ? "PrivateKey is missing" : !haveAddr ? "Address (IPv4) is missing"
			: peers == 0 ? "[Peer] is missing" : !havePub ? "PublicKey is missing"
			: !haveAllowed ? "AllowedIPs (IPv4) is missing" : !haveEnd ? "Endpoint is missing" : 0;
		if (missing)
			{
			Why(aWhy, aWhyMax, missing);
			ok = false;
			}
		}
	if (ok)
		aOut = c;
	Wipe(buf, sizeof(buf));
	Wipe(&c, sizeof(c));
	return ok;
	}

// ----- what may be shown ------------------------------------------------------------

void WgAllowedText(const WgConfig& aC, char* aOut, size_t aMax)
	{
	size_t n = 0;
	if (aMax) aOut[0] = 0;
	for (int i = 0; i < aC.nAllowed && i < kWgAllowedMax; i++)
		{
		char ip[16];
		Ip4Text(aC.allowed[i].ip, ip, sizeof(ip));
		int w = snprintf(aOut + n, aMax - n, "%s%s/%u", i ? "," : "", ip, (unsigned)aC.allowed[i].len);
		if (w < 0 || (size_t)w >= aMax - n)
			break;
		n += (size_t)w;
		}
	}

void WgSummary(const WgConfig& aC, char* aOut, size_t aMax)
	{
	if (!WgValid(aC))
		{
		snprintf(aOut, aMax, "Config: none");
		return;
		}
	char addr[16], dns[40], allowed[180];
	Ip4Text(aC.addr, addr, sizeof(addr));
	dns[0] = 0;
	for (int i = 0; i < aC.nDns; i++)
		{
		char d[16];
		Ip4Text(aC.dns[i], d, sizeof(d));
		size_t n = strlen(dns);
		snprintf(dns + n, sizeof(dns) - n, "%s%s", i ? "," : "", d);
		}
	WgAllowedText(aC, allowed, sizeof(allowed));
	snprintf(aOut, aMax,
		"Address: %s/%u\nDNS: %s\nAllowedIPs: %s\nEndpoint: %s:%u\nKeepalive: %u\nPrivate key: set\nPreshared key: %s",
		addr, (unsigned)aC.addrLen, dns[0] ? dns : "(none)", allowed, aC.endpoint, (unsigned)aC.port,
		(unsigned)aC.keepalive, aC.psk[0] ? "set" : "none");
	}

// ----- decisions ---------------------------------------------------------------------

int WgBestPrefix(const WgPrefix* aAllowed, int aCount, uint32_t aDest)
	{
	int best = -1;
	for (int i = 0; i < aCount; i++)
		if (InPrefix(aDest, aAllowed[i].ip, aAllowed[i].len) && (int)aAllowed[i].len > best)
			best = aAllowed[i].len;
	return best;
	}

bool WgUseTunnel(const WgPrefix* aAllowed, int aCount, uint32_t aDest, int aConnectedLen, uint32_t aEndpoint)
	{
	if (aDest == 0 || aDest == 0xffffffffu || aDest == aEndpoint)
		return false;
	uint8_t first = (uint8_t)(aDest & 0xff);
	if (first >= 224 || first == 127)            // multicast, reserved, loopback
		return false;
	int best = WgBestPrefix(aAllowed, aCount, aDest);
	return best >= 0 && best > aConnectedLen;
	}

bool WgAtHome(const WgConfig& aC, uint32_t aUplinkIp, uint32_t aUplinkMask)
	{
	if (!aUplinkIp)
		return false;
	for (int i = 0; i < aC.nAllowed; i++)
		if (aC.allowed[i].len > 0 && InPrefix(aUplinkIp, aC.allowed[i].ip, aC.allowed[i].len))
			return true;
	int ml = MaskLen(aUplinkMask);
	if (ml > 0 && ml < 32)
		for (int i = 0; i < aC.nDns; i++)
			if (InPrefix(aC.dns[i], aUplinkIp, ml))
				return true;
	return false;
	}

TWgWant WgDecide(int aMode, const WgConfig& aC, bool aUplinkUp, uint32_t aUplinkIp, uint32_t aUplinkMask)
	{
	if (aMode != EWgModeOn && aMode != EWgModeAuto)
		return EWgWantOff;
	if (!WgValid(aC))
		return EWgWantNoConfig;
	if (!aUplinkUp || !aUplinkIp)
		return EWgWantNoUplink;
	if (aMode == EWgModeAuto && WgAtHome(aC, aUplinkIp, aUplinkMask))
		return EWgWantHome;
	return EWgWantUp;
	}

const char* WgWantName(TWgWant aW)
	{
	switch (aW)
		{
	case EWgWantOff: return "off";
	case EWgWantNoConfig: return "no config";
	case EWgWantNoUplink: return "no uplink";
	case EWgWantHome: return "at home (not needed)";
	default: return "wanted";
		}
	}

} // namespace am
