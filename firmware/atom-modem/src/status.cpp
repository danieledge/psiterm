// status.cpp - see status.h. MIT licence (see LICENSE at the top of the repository).
#include "status.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace am {

namespace {

// A page being filled: rows are put or word-wrapped in, and anything past
// the last row is dropped
struct Builder
	{
	ScreenPage& p;
	int row;
	explicit Builder(ScreenPage& aP) : p(aP), row(0) {}

	static void Clean(char* aOut, const char* aText, size_t aMax)
		{
		size_t n = 0;
		for (; aText[n] && n < aMax; n++)
			aOut[n] = (aText[n] >= 32 && aText[n] < 127) ? aText[n] : '?';
		aOut[n] = 0;
		}
	void Put(const char* aText)                     // one row, clipped
		{
		if (row >= kScreenRows)
			return;
		Clean(p.line[row++], aText, kScreenCols);
		}
	void Putf(const char* aFmt, const char* aA)
		{
		char b[96];
		snprintf(b, sizeof(b), aFmt, aA);
		Put(b);
		}
	void Blank() { Put(""); }
	// word-wrapped into at most aMaxRows rows; returns the rows used
	int Wrap(const char* aText, int aMaxRows = kScreenRows)
		{
		int used = 0;
		const char* t = aText;
		while (*t && used < aMaxRows && row < kScreenRows)
			{
			while (*t == ' ') t++;
			size_t len = strlen(t);
			size_t n = len < (size_t)kScreenCols ? len : (size_t)kScreenCols;
			if (len > (size_t)kScreenCols)
				{
				size_t cut = n;
				while (cut > 0 && t[cut] != ' ' && t[cut - 1] != ',')
					cut--;
				if (cut > 4)
					n = cut;
				}
			char b[kScreenCols + 1];
			memcpy(b, t, n);
			while (n && b[n - 1] == ' ') n--;
			b[n] = 0;
			Put(b);
			used++;
			t += n ? n : 1;
			if (*t == 0) break;
			}
		return used;
		}
	};

// "SSID" and "IP" and RSSI out of Hal::WifiInfo's  "name" IP a.b.c.d RSSI -60
void SplitWifi(const char* aInfo, char* aSsid, size_t aSsidMax, char* aIp, size_t aIpMax, int& aRssi)
	{
	aSsid[0] = 0;
	aIp[0] = 0;
	aRssi = 0;
	const char* q1 = strchr(aInfo, '"');
	const char* q2 = q1 ? strrchr(aInfo, '"') : 0;
	if (q1 && q2 && q2 > q1)
		{
		size_t n = (size_t)(q2 - q1 - 1);
		if (n >= aSsidMax) n = aSsidMax - 1;
		memcpy(aSsid, q1 + 1, n);
		aSsid[n] = 0;
		}
	const char* ip = strstr(aInfo, " IP ");
	if (ip)
		{
		ip += 4;
		size_t n = 0;
		while (ip[n] && ip[n] != ' ' && n + 1 < aIpMax) { aIp[n] = ip[n]; n++; }
		aIp[n] = 0;
		}
	const char* r = strstr(aInfo, "RSSI ");
	if (r)
		aRssi = atoi(r + 5);
	}

const char* const kUplinkNames[] = { "AUTO", "WIFI", "USB" };

void PageStatus(const Modem& m, Hal& h, Builder& b)
	{
	b.Put("STATUS");
	if (h.WifiUp())
		{
		char info[96], ssid[40], ip[24];
		int rssi;
		info[0] = 0;
		h.WifiInfo(info, sizeof(info));
		SplitWifi(info, ssid, sizeof(ssid), ip, sizeof(ip), rssi);
		b.Putf("WiFi %s", ssid[0] ? ssid : "joined");
		b.Putf("IP   %s", ip);
		char r[24];
		snprintf(r, sizeof(r), "RSSI %d dBm", rssi);
		b.Put(r);
		}
	else if (m.Config().ssid[0])
		{
		b.Putf("WiFi %s", m.Config().ssid);
		b.Put("Not connected");
		b.Put(m.Link().Up() ? "Uplink: USB" : "No uplink");
		}
	else
		{
		b.Put("No WiFi network set");
		b.Put("Hold the button for");
		b.Put("the setup portal");
		}
	b.Blank();
	// the call
	char c[64];
	if (!m.Connected() && !m.Closing())
		snprintf(c, sizeof(c), "Call: none");
	else if (m.PppCall())
		snprintf(c, sizeof(c), "Call: PPP %s", m.Ppp().Up() ? "up" : "LCP");
	else if (m.ProxyCall())
		snprintf(c, sizeof(c), "Call: proxy %u req", (unsigned)m.WebProxy().Requests());
	else if (m.ExecCall())
		snprintf(c, sizeof(c), "Call: exec");
	else
		snprintf(c, sizeof(c), "Call: tcp %s", m.LastDial());
	b.Wrap(c, 2);
	if ((m.Connected() || m.Closing()) && !m.Online())
		b.Put("(command mode: ATO)");
	char n1[12], n2[12], l[40];
	StatusModel::Count(m.ToPsion(), n1, sizeof(n1));
	StatusModel::Count(m.ToServer(), n2, sizeof(n2));
	snprintf(l, sizeof(l), "To Psion  %s", n1);
	b.Put(l);
	snprintf(l, sizeof(l), "To server %s", n2);
	b.Put(l);
	snprintf(l, sizeof(l), "%lu baud, FC %s", (unsigned long)m.Config().baud, m.Config2().flow ? "on" : "off");
	b.Put(l);
	char last[96];
	last[0] = 0;
	h.LogLines().Line(0, last, sizeof(last));
	b.Blank();
	b.Wrap(last, 3);
	}

void PageModes(const Modem& m, Hal&, Builder& b)
	{
	b.Put("MODES");
	char l[64];
	bool up = m.Link().Up();
	b.Put(up ? "TCP: ready" : "TCP: no uplink");
	int px = ProxyMode(m.Config());
	if (px == Proxy::EOff)
		b.Put("Proxy: off");
	else
		{
		snprintf(l, sizeof(l), "Proxy PX=%d%s%s", px, m.Config().proxyNoZip ? "" : " zip", m.Config2().img ? " pi" : "");
		b.Put(l);
		}
	b.Putf("PPP: %s", m.Config2().ppp ? "on" : "off");
	b.Putf("TLS: %s", m.Config2().tls ? "on" : "off");
	int mode = m.Config2().uplink;
	int act = m.Link().Active();
	snprintf(l, sizeof(l), "Uplink %s->%s", kUplinkNames[mode >= 0 && mode <= 2 ? mode : 0],
		act == EActiveWifi ? "WIFI" : act == EActiveUsb ? "USB" : "none");
	b.Put(l);
	b.Blank();
	b.Put("Last dial:");
	b.Wrap(m.LastDial()[0] ? m.LastDial() : "(none yet)", 2);
	if (m.LastDialResult()[0])
		b.Put(m.LastDialResult());
	}

void PageSetup(const Modem&, Hal& h, Builder& b)
	{
	b.Put("SETUP");
	char ap[96];
	ap[0] = 0;
	h.ApInfo(ap, sizeof(ap));
	if (ap[0])
		{
		b.Wrap(ap, 3);                          // AP "name" up at http://192.168.4.1/
		char pw[40];
		pw[0] = 0;
		h.ApPass(pw, sizeof(pw));
		if (pw[0])
			{
			b.Put("Password:");
			b.Put(pw);
			}
		}
	else
		b.Put("AP: off (hold button)");
	char web[96];
	web[0] = 0;
	h.WebInfo(web, sizeof(web));
	if (web[0])
		{
		b.Put("LAN pages:");
		b.Wrap(web, 2);
		}
	b.Blank();
	char v[48];
	snprintf(v, sizeof(v), "Atom modem %s", kVersion);
	b.Put(v);
	b.Put(h.BoardName());
	char mem[200];
	mem[0] = 0;
	h.MemInfo(mem, sizeof(mem));
	b.Wrap(mem, 6);
	}

void PageLog(const Modem&, Hal& h, Builder&, ScreenPage& aOut)
	{
	// the last 14 lines, wrapped, newest at the bottom
	memset(aOut.line, 0, sizeof(aOut.line));
	Builder::Clean(aOut.line[0], "LOG", kScreenCols);
	int bottom = kScreenRows - 1;
	for (int k = 0; k < 14 && bottom >= 1; k++)
		{
		char ln[160];
		if (!h.LogLines().Line((size_t)k, ln, sizeof(ln)))
			break;
		// wrap this line into a scratch page, then place it above what is there
		ScreenPage tmp;
		memset(&tmp, 0, sizeof(tmp));
		Builder t(tmp);
		t.row = 0;
		int rows = t.Wrap(ln, kScreenRows);
		for (int r = rows - 1; r >= 0 && bottom >= 1; r--)
			memcpy(aOut.line[bottom--], tmp.line[r], kScreenCols + 1);
		}
	}

} // namespace

void StatusModel::Count(uint32_t aBytes, char* aOut, size_t aMax)
	{
	if (aBytes < 10000)
		snprintf(aOut, aMax, "%lu", (unsigned long)aBytes);
	else if (aBytes < 1000000)
		snprintf(aOut, aMax, "%luK", (unsigned long)(aBytes / 1000));
	else if (aBytes < 100000000)
		snprintf(aOut, aMax, "%lu.%luM", (unsigned long)(aBytes / 1000000), (unsigned long)(aBytes / 100000 % 10));
	else
		snprintf(aOut, aMax, "%luM", (unsigned long)(aBytes / 1000000));
	}

void StatusModel::Fill(const Modem& aModem, Hal& aHal, int aPage, ScreenPage& aOut)
	{
	memset(&aOut, 0, sizeof(aOut));
	int page = aPage % EPageCount;
	if (page < 0)
		page += EPageCount;
	int led = aModem.LedNow();
	aOut.bar = led >= 0 ? (LedState)led : ELedNoWifi;
	Builder b(aOut);
	switch (page)
		{
	case EPageStatus: PageStatus(aModem, aHal, b); break;
	case EPageModes:  PageModes(aModem, aHal, b); break;
	case EPageSetup:  PageSetup(aModem, aHal, b); break;
	default:          PageLog(aModem, aHal, b, aOut); break;
		}
	// the title carries the page number on the right ("STATUS          1/4")
	char t[kScreenCols + 1];
	char num[8];
	snprintf(num, sizeof(num), "%d/%d", page + 1, (int)EPageCount);
	size_t nl = strlen(num), tl = strlen(aOut.line[0]);
	memset(t, ' ', kScreenCols);
	t[kScreenCols] = 0;
	memcpy(t, aOut.line[0], tl < (size_t)kScreenCols - nl - 1 ? tl : (size_t)kScreenCols - nl - 1);
	memcpy(t + kScreenCols - nl, num, nl);
	memcpy(aOut.line[0], t, sizeof(t));
	}

} // namespace am
