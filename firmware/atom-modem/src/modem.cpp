// modem.cpp - see modem.h. MIT licence (see LICENSE at the top of the repository).
#include "modem.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace am {

// ===== the platform's defaults ==============================================

bool Hal::UpConnect(const char* aHost, uint16_t aPort, bool aTls, char* aWhy, size_t aWhyMax)
	{
	if (aTls)
		{
		snprintf(aWhy, aWhyMax, "no TLS on this platform");
		return false;
		}
	if (!TcpConnect(aHost, aPort))
		{
		snprintf(aWhy, aWhyMax, "no connection to %s:%u", aHost, aPort);
		return false;
		}
	return true;
	}

void Hal::MemInfo(char* aOut, size_t aMax)
	{
	if (aMax)
		aOut[0] = 0;
	}

// ===== the log ring ==========================================================

void LogRing::Add(const char* aLine)
	{
	size_t n = strlen(aLine);
	if (n > kSize / 4)
		n = kSize / 4;
	for (size_t i = 0; i <= n; i++)
		{
		char c = i < n ? aLine[i] : '\n';
		iBuf[(iHead + iCount) % kSize] = c;
		if (iCount < kSize)
			iCount++;
		else
			iHead = (iHead + 1) % kSize;
		}
	}

size_t LogRing::Read(size_t aFrom, char* aOut, size_t aMax) const
	{
	size_t n = 0;
	while (aFrom + n < iCount && n + 1 < aMax)
		{
		aOut[n] = iBuf[(iHead + aFrom + n) % kSize];
		n++;
		}
	if (aMax)
		aOut[n] = 0;
	return n;
	}

// ===== ring ================================================================

size_t Ring::Put(const uint8_t* aData, size_t aLen)
	{
	size_t n = 0;
	while (n < aLen && iCount < iSize)
		{
		size_t run = iSize - iHead;
		size_t want = aLen - n;
		if (run > want) run = want;
		if (run > iSize - iCount) run = iSize - iCount;
		memcpy(iBuf + iHead, aData + n, run);
		iHead = (iHead + run) % iSize;
		iCount += run;
		n += run;
		}
	return n;
	}

size_t Ring::Peek(const uint8_t*& aPtr) const
	{
	aPtr = iBuf + iTail;
	size_t run = iSize - iTail;
	return run < iCount ? run : iCount;
	}

void Ring::Drop(size_t aLen)
	{
	if (aLen > iCount) aLen = iCount;
	iTail = (iTail + aLen) % iSize;
	iCount -= aLen;
	}

size_t Ring::Get(uint8_t* aOut, size_t aMax)
	{
	size_t n = 0;
	while (n < aMax && iCount)
		{
		const uint8_t* p;
		size_t run = Peek(p);
		if (run > aMax - n) run = aMax - n;
		memcpy(aOut + n, p, run);
		Drop(run);
		n += run;
		}
	return n;
	}

// ===== pacer ===============================================================

static const uint64_t kMicro = 1000000ULL;

void Pacer::Configure(uint32_t aRate, uint16_t aBurst, uint16_t aGapMs)
	{
	iRate = aRate;
	iBurst = aBurst ? aBurst : 1;
	iGapUs = (uint32_t)aGapMs * 1000;
	iTokens = (uint64_t)iBurst * kMicro;
	iInBurst = 0;
	iStarted = 0;
	iQuietUntil = 0;
	}

size_t Pacer::Allow(uint32_t aNowUs, size_t aWant)
	{
	if (iRate == 0)
		return aWant;
	if (!iStarted)
		{
		iStarted = 1;
		iLastUs = aNowUs;
		iQuietUntil = aNowUs;
		}
	uint32_t dt = aNowUs - iLastUs;            // (wraps cleanly every 71 minutes)
	iLastUs = aNowUs;
	iTokens += (uint64_t)dt * iRate;
	uint64_t cap = (uint64_t)iBurst * kMicro;
	if (iTokens > cap)
		iTokens = cap;
	if ((int32_t)(aNowUs - iQuietUntil) < 0)
		return 0;                              // the gap after a burst
	size_t n = (size_t)(iTokens / kMicro);
	if (iGapUs && n > iBurst - iInBurst)
		n = iBurst - iInBurst;
	return n < aWant ? n : aWant;
	}

void Pacer::Spent(uint32_t aNowUs, size_t aSent)
	{
	if (iRate == 0 || aSent == 0)
		return;
	uint64_t s = (uint64_t)aSent * kMicro;
	iTokens = iTokens > s ? iTokens - s : 0;
	iInBurst += aSent;
	if (iGapUs && iInBurst >= iBurst)
		{
		iQuietUntil = aNowUs + iGapUs;
		iInBurst = 0;
		}
	}

// ===== modem ===============================================================

static int Upper(int aC) { return (aC >= 'a' && aC <= 'z') ? aC - 32 : aC; }

static bool StartsNoCase(const char* aS, const char* aP)
	{
	while (*aP)
		if (Upper((unsigned char)*aS++) != Upper((unsigned char)*aP++))
			return false;
	return true;
	}

static long Number(const char*& aP, long aDefault)
	{
	if (*aP < '0' || *aP > '9')
		{
		if (*aP == '-' && aP[1] >= '0' && aP[1] <= '9')
			{
			aP++;
			return -Number(aP, 0);
			}
		return aDefault;
		}
	long v = 0;
	while (*aP >= '0' && *aP <= '9')
		v = v * 10 + (*aP++ - '0');
	return v;
	}

// a string value: the rest of the line, trimmed, without surrounding quotes
static void Value(const char* aP, char* aOut, size_t aMax)
	{
	while (*aP == ' ') aP++;
	size_t n = strlen(aP);
	while (n && aP[n - 1] == ' ') n--;
	if (n >= 2 && aP[0] == '"' && aP[n - 1] == '"') { aP++; n -= 2; }
	if (n >= aMax) n = aMax - 1;
	memcpy(aOut, aP, n);
	aOut[n] = 0;
	}

Modem::Modem(Hal& aHal, uint8_t* aRing, size_t aRingSize)
	: iHal(aHal), iLineLen(0), iOnline(false), iConnected(false), iClosing(false), iWifiWasUp(false),
	  iLastSerialMs(0), iPluses(0), iLastPlusMs(0), iToPsion(0), iToServer(0), iLastDataMs(0),
	  iPendingBaud(0), iTrialFrom(0), iTrialStartMs(0), iTrialArmed(false), iTrialGen(0), iLed(-1), iProxy(aHal),
	  iProxyCall(false), iPppCall(false), iExecCall(false), iExecOneShot(false), iTlsCall(false), iConfigMode(false),
	  iConfigUntilMs(0), iLastTickMs(0), iWifiDownMs(0), iWifiRetryMs(0), iProbeDueMs(0), iLastWifiState(-1),
	  iLastActive(-1), iUpLen(0)
	{
	iRing.Init(aRing, aRingSize);
	iLast[0] = 0;
	FactoryDefaults(iS);
	FactoryDefaults2(iS2);
	}

void Modem::LoadAll()
	{
	Settings s;
	if (iHal.LoadSettings(s) && s.magic == kMagic)
		iS = s;
	else
		FactoryDefaults(iS);
	Settings2 s2;
	size_t len = 0;
	memset(&s2, 0, sizeof(s2));
	if (iHal.LoadSettings2(s2, len) && AcceptSettings2(s2, len))
		iS2 = s2;
	else
		FactoryDefaults2(iS2);
	}

void Modem::Begin()
	{
	LoadAll();
	iHal.ApplyPins(iS, iS2);
	iHal.SerialBaud(iS.baud);
	ApplyPacing();
	iHal.TlsVerify(iS2.tlsVerify != 0);
	iUplink.Configure(iS2.uplink);
	iHal.ApplyUplink(iS2);
	if (iS.ssid[0])
		iHal.WifiBegin(iS.ssid, iS.pass);
	iHal.ApplyWeb(iS2);
	iLastSerialMs = iHal.Millis();
	char d[48];
	iUplink.Tick(iHal.Millis(), iHal.WifiUp(), iHal.UsbState(d, sizeof(d)));
	UpdateDcd();
	UpdateLed(iHal.Millis());
	}

void Modem::ApplyPacing()
	{
	if (iS.paceAuto)
		AutoPacing(iS.baud, iS2.flow != 0, iS.paceRate, iS.paceBurst, iS.paceGap);
	iPacer.Configure(iS.paceRate, iS.paceBurst, iS.paceGap);
	}

void Modem::Loop()
	{
	uint32_t now = iHal.Millis();
	int c;
	for (int n = 0; n < 512; n++)
		{
		// online, a byte can put up to 4 into iUpBuf (held pluses, then it).
		// No room, even once it has gone on: the rest is left in the UART
		// until the server (or the proxy) takes more, rather than lost
		if (iOnline && iUpLen + 4 > sizeof(iUpBuf))
			{
			FlushUp();
			if (iUpLen + 4 > sizeof(iUpBuf))
				break;
			}
		if ((c = iHal.SerialRead()) < 0)
			break;
		SerialIn((uint8_t)c);
		}
	EscapeTick(iHal.Millis());
	FlushUp();
	if (iPppCall)
		{
		iPpp.Poll(iHal);                     // PPP runs on the lwIP thread; notice a drop
		if (iPpp.Dropped())
			Hangup(true);                    // the Psion closed the link (or an error)
		}
	else
		{
		PumpServer(now);
		PumpPsion(now);
		}
	if (iPendingBaud && iHal.SerialWritable() > 0)
		{
		iHal.SerialBaud(iPendingBaud);          // (the HAL lets the OK go out first)
		iPendingBaud = 0;
		if (iTrialFrom)
			{
			iTrialArmed = true;                  // the 15 s start now, at the new speed
			iTrialStartMs = iHal.Millis();
			}
		}
	BaudTrialTick(iHal.Millis());
	NetTick(now);                            // (every pass: a WiFi drop must end a call at once)
	if (now - iLastTickMs >= 100 || iLastTickMs == 0)
		{
		iLastTickMs = now;
		if (iConfigMode && (int32_t)(now - iConfigUntilMs) >= 0)
			ConfigMode(false);
		}
	UpdateDcd();
	UpdateLed(now);
	}

// The network's facts into the uplink manager; the WiFi joined again when
// it stays down; the Internet check now and then; the changes logged
void Modem::NetTick(uint32_t aNow)
	{
	bool assoc = iHal.WifiUp();
	char d[48];
	iUplink.Tick(aNow, assoc, iHal.UsbState(d, sizeof(d)), assoc ? iHal.Internet() : -1);
	// joining again: the core's own reconnect usually does it, but a hotspot
	// that went away and came back can leave the station stuck
	if (iS.ssid[0] && !assoc)
		{
		if (!iWifiDownMs)
			iWifiDownMs = aNow ? aNow : 1;
		else if (aNow - iWifiDownMs >= kWifiRetryAfterMs && aNow - iWifiRetryMs >= kWifiRetryEveryMs)
			{
			iWifiRetryMs = aNow;
			char m[96];
			snprintf(m, sizeof(m), "WiFi: not joined for %lu s: joining \"%s\" again", (unsigned long)((aNow - iWifiDownMs) / 1000), iS.ssid);
			iHal.Log(m);
			iHal.WifiBegin(iS.ssid, iS.pass);
			}
		}
	else
		{
		iWifiDownMs = 0;
		iWifiRetryMs = aNow - kWifiRetryEveryMs;   // (the first retry after a drop is prompt)
		}
	// what changed (before the check below, which it may bring forward)
	int ws = iUplink.WifiState();
	if (ws != iLastWifiState)
		{
		if (ws == EWifiAssociated && iLastWifiState != EWifiInternet)
			iProbeDueMs = aNow;              // just joined (or back): check soon
		else if (ws == EWifiInternet)
			iProbeDueMs = aNow + kProbeAgainMs;   // confirmed: not again for a while
		iLastWifiState = ws;
		char m[96];
		snprintf(m, sizeof(m), "WiFi: %s", Uplink::WifiStateName(ws));
		iHal.Log(m);
		}
	int act = iUplink.Active();
	if (act != iLastActive)
		{
		iLastActive = act;
		iHal.Log(act == EActiveUsb ? "Uplink: USB" : act == EActiveWifi ? "Uplink: WiFi" : "Uplink: none");
		}
	// the Internet check: not during a call (the line is busy, and a failed
	// connect would say nothing new), soon while unconfirmed, rarely once passed
	if (assoc && !iConnected && !iClosing && iS2.chkHost[0] && (int32_t)(aNow - iProbeDueMs) >= 0)
		{
		char host[40];
		strncpy(host, iS2.chkHost, sizeof(host) - 1);
		host[sizeof(host) - 1] = 0;
		char* colon = strrchr(host, ':');
		long port = 53;
		if (colon)
			{
			*colon = 0;
			const char* q = colon + 1;
			port = Number(q, 53);
			}
		if (host[0] && port > 0 && port <= 65535)
			iHal.ProbeInternet(host, (uint16_t)port);
		iProbeDueMs = aNow + (iUplink.WifiState() == EWifiInternet ? kProbeAgainMs : kProbeSoonMs);
		}
	}

// ----- from the Psion -------------------------------------------------------

void Modem::SerialIn(uint8_t aC)
	{
	if (iOnline)
		OnlineChar(aC);
	else
		CommandChar(aC);
	iLastSerialMs = iHal.Millis();
	}

// Data mode: everything goes to the server, except a "+++" with a quiet
// guard time before, between (each + within the guard time of the last)
// and after it, which returns to command mode. The pluses are held back
// until that is decided, so an escape never reaches the server.
void Modem::OnlineChar(uint8_t aC)
	{
	uint32_t now = iHal.Millis();
	if (aC == iS.s2 && iS.s2 < 128)
		{
		if (iPluses == 0 && now - iLastSerialMs >= GuardMs())
			{
			iPluses = 1;
			iLastPlusMs = now;
			return;
			}
		if (iPluses > 0 && iPluses < 3 && now - iLastPlusMs < GuardMs())
			{
			iPluses++;
			iLastPlusMs = now;
			return;
			}
		}
	FlushHeldPluses();
	if (iUpLen >= sizeof(iUpBuf))
		FlushUp();
	if (iUpLen < sizeof(iUpBuf))
		iUpBuf[iUpLen++] = aC;
	}

void Modem::FlushHeldPluses()
	{
	while (iPluses > 0)
		{
		if (iUpLen >= sizeof(iUpBuf))
			FlushUp();
		if (iUpLen < sizeof(iUpBuf))
			iUpBuf[iUpLen++] = iS.s2;
		iPluses--;
		}
	}

void Modem::EscapeTick(uint32_t aNow)
	{
	if (!iPluses || aNow - iLastPlusMs < GuardMs())
		return;
	if (iPluses == 3 && iOnline)
		{
		iPluses = 0;
		iOnline = false;                     // command mode; the call stays up (ATO, ATH)
		iLineLen = 0;
		Result(ROk);
		}
	else
		FlushHeldPluses();                   // one or two: they were data after all
	}

void Modem::FlushUp()
	{
	if (!iUpLen)
		return;
	if (!iConnected)
		{
		iUpLen = 0;                          // nowhere to go
		return;
		}
	size_t n = iPppCall ? iPpp.Input(iHal, iUpBuf, iUpLen)
	         : iProxyCall ? iProxy.FromPsion(iUpBuf, iUpLen)
	         : iHal.TcpWrite(iUpBuf, iUpLen);
	iToServer += n;
	if (n)
		iLastDataMs = iHal.Millis();
	if (n >= iUpLen)
		iUpLen = 0;
	else
		{
		memmove(iUpBuf, iUpBuf + n, iUpLen - n);
		iUpLen -= n;
		}
	}

void Modem::CommandChar(uint8_t aC)
	{
	if (iS.echo)
		Send(&aC, 1);
	if (aC == '\r')
		{
		iLine[iLineLen] = 0;
		RunCommandLine();
		iLineLen = 0;
		return;
		}
	if (aC == '\n' || aC == 0)
		return;                              // (a NUL would end the line early)
	if (aC == 8 || aC == 127)
		{
		if (iLineLen)
			iLineLen--;
		return;
		}
	if (iLineLen < sizeof(iLine) - 1)
		iLine[iLineLen++] = (char)aC;
	// A/ repeats the last command at once, without Enter
	if (iLineLen >= 2 && iLine[iLineLen - 1] == '/' && Upper(iLine[iLineLen - 2]) == 'A')
		{
		iLineLen = 0;
		if (iLast[0])
			{
			uint32_t gen = iTrialGen;
			TResult r = RunCommands(iLast);
			ConfirmBaud(gen, r);
			if (r != RNone)
				Result(r);
			}
		}
	}

void Modem::RunCommandLine()
	{
	// find "AT": anything before it (line noise, a half-sent frame) is ignored
	const char* p = iLine;
	while (p[0] && p[1] && !(Upper(p[0]) == 'A' && Upper(p[1]) == 'T'))
		p++;
	if (!p[0] || !p[1])
		return;                              // no command on this line
	p += 2;
	size_t n = strlen(p);
	if (n >= sizeof(iLast)) n = sizeof(iLast) - 1;
	memcpy(iLast, p, n);
	iLast[n] = 0;
	uint32_t gen = iTrialGen;
	TResult r = RunCommands(iLast);
	ConfirmBaud(gen, r);
	if (r != RNone)
		Result(r);
	}

// ----- AT$SB / ATB: back to the old speed if the new one never works ---------

// a speed change from aOld to iS.baud: on trial until a valid command
// line arrives at the new speed (or AT&W saves it)
void Modem::StartBaudTrial(uint32_t aOld)
	{
	iTrialGen++;
	// aOld is known to work if nothing is on trial, or if this command was
	// heard at it (the trial is armed); otherwise (two changes on one line)
	// the trial keeps the speed it started from
	if (!iTrialFrom || iTrialArmed)
		iTrialFrom = aOld;
	if (iS.baud == iTrialFrom)
		iTrialFrom = 0;                      // no change: nothing to fall back from
	iTrialArmed = false;
	}

// a command line has run (aGen: iTrialGen before it). A valid one heard at
// the new speed confirms it, unless that line itself started a new change
void Modem::ConfirmBaud(uint32_t aGen, TResult aR)
	{
	if (iTrialFrom && iTrialArmed && aGen == iTrialGen && aR != RError)
		iTrialFrom = 0;
	}

void Modem::BaudTrialTick(uint32_t aNow)
	{
	if (!iTrialFrom || !iTrialArmed || iOnline || iPendingBaud)
		return;
	if (iS.baud == iTrialFrom)
		{
		iTrialFrom = 0;                      // (ATZ or AT&F went back to it)
		return;
		}
	if (aNow - iTrialStartMs < kBaudTrialMs)
		return;
	uint32_t from = iTrialFrom, to = iS.baud;
	iTrialFrom = 0;
	iTrialArmed = false;
	iS.baud = from;
	ApplyPacing();
	iHal.SerialBaud(from);
	iLineLen = 0;                            // (whatever arrived at the wrong speed)
	char m[96];
	snprintf(m, sizeof(m), "No command at %lu baud in %lu s: back to %lu baud",
		(unsigned long)to, (unsigned long)(kBaudTrialMs / 1000), (unsigned long)from);
	iHal.Log(m);
	}

// what has to follow a setting's change (AT$ or the web page)
void Modem::Apply(TSchemaApply aWhat, uint32_t aOldBaud)
	{
	switch (aWhat)
		{
	case SAWifi:
		if (iS.ssid[0] && iS.pass[0])
			iHal.WifiBegin(iS.ssid, iS.pass);  // join once both are known
		break;
	case SABaud:
		if (iS.baud != aOldBaud)
			{
			ApplyPacing();
			iPendingBaud = iS.baud;          // after the OK, at the old speed
			StartBaudTrial(aOldBaud);        // and back to it if nothing works at the new one
			}
		break;
	case SAPaceManual:
		iS.paceAuto = 0;
		ApplyPacing();
		break;
	case SAPacing:
		ApplyPacing();
		break;
	case SAPins:
		iHal.ApplyPins(iS, iS2);
		ApplyPacing();                       // (flow control changes the auto rate)
		break;
	case SADcd:
		iHal.ApplyPins(iS, iS2);
		UpdateDcd();
		break;
	case SAWeb:
		iHal.ApplyWeb(iS2);
		break;
	case SAUplink:
		iUplink.Configure(iS2.uplink);
		iHal.ApplyUplink(iS2);
		break;
	case SAUsb:
		iHal.ApplyUplink(iS2);
		break;
	default:
		break;
		}
	iHal.TlsVerify(iS2.tlsVerify != 0);
	}

bool Modem::WebSet(const SchemaEntry& aE, const char* aValue)
	{
	am::Config c = { iS, iS2 };
	uint32_t oldBaud = iS.baud;
	if (!SchemaSet(c, aE, aValue))
		return false;
	Apply(aE.apply, oldBaud);
	return true;
	}

void Modem::WebGet(const SchemaEntry& aE, char* aOut, size_t aMax) const
	{
	am::Config c = { const_cast<Settings&>(iS), const_cast<Settings2&>(iS2) };
	SchemaGet(c, aE, aOut, aMax);
	}

bool Modem::Save()
	{
	if (!iHal.SaveSettings(iS))
		return false;
	if (!iHal.SaveSettings2(iS2))
		return false;
	iTrialFrom = 0;                          // a saved speed is confirmed
	return true;
	}

void Modem::Factory(bool aKeepWifi)
	{
	Settings f;
	FactoryDefaults(f);
	if (aKeepWifi)
		{
		memcpy(f.ssid, iS.ssid, sizeof(f.ssid));
		memcpy(f.pass, iS.pass, sizeof(f.pass));
		}
	uint32_t oldBaud = iS.baud;
	iS = f;
	FactoryDefaults2(iS2);
	iHal.ApplyPins(iS, iS2);
	ApplyPacing();
	iHal.TlsVerify(true);
	iUplink.Configure(iS2.uplink);
	if (iS.baud != oldBaud)
		iPendingBaud = iS.baud;
	}

void Modem::SeedNetwork(const char* aSsid, const char* aPass)
	{
	if (!aSsid || !aSsid[0] || iS.ssid[0])
		return;
	strncpy(iS.ssid, aSsid, sizeof(iS.ssid) - 1);
	iS.ssid[sizeof(iS.ssid) - 1] = 0;
	strncpy(iS.pass, aPass ? aPass : "", sizeof(iS.pass) - 1);
	iS.pass[sizeof(iS.pass) - 1] = 0;
	char m[96];
	snprintf(m, sizeof(m), "First start: the network \"%s\" was preset in this build; saved", iS.ssid);
	iHal.Log(m);
	Save();
	iHal.WifiBegin(iS.ssid, iS.pass);
	iHal.ApplyWeb(iS2);                      // (a network is set: no portal)
	}

void Modem::ConfigMode(bool aOn)
	{
	iConfigMode = aOn;
	if (aOn)
		{
		iConfigUntilMs = iHal.Millis() + 10 * 60 * 1000;
		iHal.Log("Config mode: the access point is up for 10 minutes");
		}
	else
		iHal.Log("Config mode over");
	iHal.ApplyWeb(iS2);
	}

Modem::TResult Modem::RunCommands(const char* aCmd)
	{
	const char* p = aCmd;
	for (;;)
		{
		while (*p == ' ')
			p++;
		if (!*p)
			return ROk;
		int c = Upper((unsigned char)*p++);
		long v;
		switch (c)
			{
		case 'E': iS.echo = Number(p, 0) ? 1 : 0; break;
		case 'V': iS.verbose = Number(p, 0) ? 1 : 0; break;
		case 'Q': iS.quiet = Number(p, 0) ? 1 : 0; break;
		case 'H':
			Number(p, 0);
			if (iConnected || iClosing)
				Hangup(false);
			break;
		case 'Z':
			{
			Number(p, 0);
			uint32_t oldBaud = iS.baud;
			LoadAll();
			iHal.ApplyPins(iS, iS2);
			ApplyPacing();
			iHal.TlsVerify(iS2.tlsVerify != 0);
			iUplink.Configure(iS2.uplink);
			if (iS.baud != oldBaud)
				iPendingBaud = iS.baud;
			return ROk;
			}
		case 'O':
			Number(p, 0);
			if (!iConnected)
				return RNoCarrier;
			iOnline = true;
			iLastSerialMs = iHal.Millis();
			Result(RConnect);
			return RNone;
		case 'D':
			return Dial(p);
		case 'A':
			return RError;                   // (answering: no incoming calls)
		case 'I':
			Number(p, 0);
			Info();
			break;
		case 'S':
			{
			long reg = Number(p, -1);
			if (reg < 0 || reg > 255)
				return RError;
			if (*p == '=')
				{
				p++;
				v = Number(p, 0);
				if (v < 0 || v > 255)
					return RError;
				if (reg == 2) iS.s2 = (uint8_t)v;
				else if (reg == 12) iS.s12 = (uint8_t)(v ? v : 1);
				// S0, S3, S4, S5, S7 and the rest: accepted, fixed
				}
			else if (*p == '?')
				{
				p++;
				char m[8];
				long val = reg == 2 ? iS.s2 : reg == 12 ? iS.s12 : reg == 3 ? 13 : reg == 4 ? 10
					: reg == 5 ? 8 : reg == 7 ? 30 : 0;
				snprintf(m, sizeof(m), "%03ld", val);
				SayLine(m);
				}
			break;
			}
		case '&':
			{
			int c2 = Upper((unsigned char)*p);
			if (c2) p++;
			v = Number(p, 0);
			switch (c2)
				{
			case 'F':
				Factory(true);               // factory settings, but keep the WiFi network
				break;
			case 'W':
				if (!Save())
					return RError;
				break;
			case 'C': iS.dcdMode = v ? 1 : 0; break;
			case 'V':
				ShowSettings();
				break;
			case 'D': case 'K': case 'S': case 'B': case 'N': case 'Q': case 'R': case 'Y':
				break;                       // accepted (no DTR on this board; RTS/CTS is AT$FC)
			default:
				return RError;
				}
			break;
			}
		case '$':
			{
			bool handled = false;
			TResult r = SetCommand(p, p, handled);
			if (!handled)
				return RError;
			return r;
			}
		case 'W':
			{
			// Zimodem: ATW"ssid,password" joins; ATW alone lists the networks
			while (*p == ' ') p++;
			if (!*p)
				{
				char names[16][33];
				int n = iHal.WifiScan(names, 16);
				for (int i = 0; i < n; i++)
					SayLine(names[i]);
				return ROk;
				}
			char val[100];
			Value(p, val, sizeof(val));
			char* comma = strchr(val, ',');
			if (!comma)
				return RError;
			*comma = 0;
			strncpy(iS.ssid, val, sizeof(iS.ssid) - 1);
			iS.ssid[sizeof(iS.ssid) - 1] = 0;
			strncpy(iS.pass, comma + 1, sizeof(iS.pass) - 1);
			iS.pass[sizeof(iS.pass) - 1] = 0;
			iHal.WifiBegin(iS.ssid, iS.pass);
			return ROk;
			}
		case 'C':
			// WiFi232: ATC1 joins the saved network, ATC0 leaves it
			v = Number(p, 1);
			if (v)
				{
				if (!iS.ssid[0])
					return RError;
				iHal.WifiBegin(iS.ssid, iS.pass);
				}
			else
				{
				if (iConnected)
					Hangup(false);
				iHal.WifiEnd();
				}
			break;
		case 'B':
			// Zimodem: ATB<baud>
			v = Number(p, -1);
			if (!ValidBaud(v))
				return RError;
			{
			uint32_t old = iS.baud;
			iS.baud = (uint32_t)v;
			Apply(SABaud, old);
			}
			break;
		case 'L': case 'M': case 'X': case 'N': case 'P': case 'T': case 'Y':
			Number(p, 0);                    // speaker, result set, pulse/tone: accepted
			break;
		default:
			return RError;
			}
		}
	}

// AT$...: the settings (WiFi232 style), from the schema, plus a few
// commands of their own. Each takes the rest of the line. AT&W saves them.
Modem::TResult Modem::SetCommand(const char* aCmd, const char*& aP, bool& aHandled)
	{
	aP = aCmd + strlen(aCmd);                // these commands end the line
	// the name: letters and digits up to '=', '?' or the end
	char name[16];
	size_t nl = 0;
	const char* rest = aCmd;
	while (*rest && *rest != '=' && *rest != '?' && *rest != ' ' && nl < sizeof(name) - 1)
		name[nl++] = (char)Upper((unsigned char)*rest++);
	name[nl] = 0;
	while (*rest == ' ') rest++;
	bool query = *rest != '=';
	if (*rest == '=' || *rest == '?')
		rest++;
	char m[160];
	char val[100];
	Value(rest, val, sizeof(val));
	// ----- commands that are not settings -----
	if (strcmp(name, "PACE") == 0)
		{
		aHandled = true;
		if (iS.paceRate)
			snprintf(m, sizeof(m), "%s%lu bytes/s, burst %u, gap %u ms, buffer %lu%s",
				iS.paceAuto ? "auto: " : "", (unsigned long)iS.paceRate, iS.paceBurst, iS.paceGap,
				(unsigned long)iRing.Size(), iS2.flow ? ", RTS/CTS" : "");
		else
			snprintf(m, sizeof(m), "off, buffer %lu%s", (unsigned long)iRing.Size(), iS2.flow ? ", RTS/CTS" : "");
		SayLine(m);
		return ROk;
		}
	if (strcmp(name, "HELP") == 0)
		{
		aHandled = true;
		Help();
		return ROk;
		}
	if (strcmp(name, "LOG") == 0)
		{
		aHandled = true;
		ShowLog();
		return ROk;
		}
	if (strcmp(name, "AP") == 0)
		{
		aHandled = true;
		char w[96];
		iHal.ApInfo(w, sizeof(w));
		SayLine(w[0] ? w : "no access point");
		return ROk;
		}
	if (strcmp(name, "RESET") == 0)
		{
		aHandled = true;
		if (query || !StartsNoCase(val, "YES"))
			return RError;
		Factory(false);
		Result(ROk);
		iHal.FactoryReset();                 // (clears NVS and restarts, on the board)
		return RNone;
		}
	if (strcmp(name, "EXEC") == 0)
		{
		aHandled = true;
		if (query || !val[0])
			return RError;
		return DialExec(val);
		}
	if (strcmp(name, "UP") == 0 && query)
		{
		aHandled = true;
		iUplink.Describe(m, sizeof(m));
		SayLine(m);
		return ROk;
		}
	// ----- the settings -----
	const SchemaEntry* e = SchemaFind(name);
	if (!e)
		return RError;
	aHandled = true;
	am::Config c = { iS, iS2 };
	if (query)
		{
		if (e->type == STProxyMode)
			{
			static const char* const kModes[] = { "off", "on: simplified pages", "on: text only",
				"on: pages unchanged (TLS only)", "on: reader" };
			snprintf(m, sizeof(m), "%d (%s)", ProxyMode(iS), kModes[ProxyMode(iS) % 5]);
			}
		else
			SchemaGet(c, *e, m, sizeof(m));
		SayLine(m);
		return ROk;
		}
	uint32_t oldBaud = iS.baud;
	if (!SchemaSet(c, *e, val))
		return RError;
	Apply(e->apply, oldBaud);
	return ROk;
	}

// ATDT host:port (also ATDThost:port, ATD, ATDP, quotes): opens TCP
Modem::TResult Modem::Dial(const char* aArgs)
	{
	// T or P straight after the D is tone/pulse ("ATDT host"); after a space
	// it is the host's first letter ("ATD telnet.example.org")
	const char* p = aArgs;
	if (Upper(*p) == 'T' || Upper(*p) == 'P')
		p++;
	while (*p == ' ') p++;
	char target[200];
	Value(p, target, sizeof(target));
	if (!target[0])
		return RError;
	// "tls:host:port": the modem does the TLS for this call
	bool tls = false;
	if (StartsNoCase(target, "tls:"))
		{
		tls = true;
		memmove(target, target + 4, strlen(target + 4) + 1);
		if (!target[0])
			return RError;
		}
	char* colon = strrchr(target, ':');
	long port = 23;
	if (colon)
		{
		const char* q = colon + 1;
		port = Number(q, -1);
		if (port <= 0 || port > 65535 || *q)
			return RError;
		*colon = 0;
		}
	else
		{
		// a "phone number" (digits and the usual dial punctuation, e.g.
		// ATD777 or ATDT*99#): no host to resolve. With AT$PPP=1 this brings
		// up PPP so the Psion's own "Psion Internet" stack shares our link.
		bool digits = target[0] != 0;
		for (const char* q = target; *q; q++)
			if ((*q < '0' || *q > '9') && *q != ',' && *q != '-' && *q != '*' && *q != '#')
				digits = false;
		if (digits)
			{
			if (!iS2.ppp)
				return RNoCarrier;           // PPP off: a phone number goes nowhere
			if (!iUplink.Up())
				return RNoCarrier;
			if (iConnected || iClosing)
				Hangup(false);
			if (!iS2.flow)
				iHal.Log("ppp: RTS/CTS (AT$FC=1) and a higher baud are strongly recommended");
			iHal.Led(ELedConnecting);
			iLed = ELedConnecting;
			if (!iPpp.Start(iHal))
				return RNoCarrier;
			iPppCall = true;
			iProxyCall = false;
			iExecCall = false;
			iExecOneShot = false;
			iTlsCall = false;
			iConnected = true;
			iClosing = false;
			iOnline = true;
			iRing.Clear();
			iUpLen = 0;
			iPluses = 0;
			iLastSerialMs = iHal.Millis();
			Result(RConnect);
			UpdateDcd();
			return RNone;
			}
		}
	// "psiexec": the text channel to the helper on the LAN
	if (StartsNoCase(target, "psiexec") && target[7] == 0)
		return DialExec(0);
	if (!iUplink.Up())
		return RNoCarrier;
	if (iConnected || iClosing)
		Hangup(false);
	// "psiproxy" (any port): the modem is PsiWeb's web proxy itself
	bool proxy = ProxyMode(iS) != Proxy::EOff && StartsNoCase(target, Proxy::kName)
		&& target[strlen(Proxy::kName)] == 0;
	if (!tls && iS2.tls && TlsPort(iS2, (uint16_t)port))
		tls = true;
	iHal.Led(ELedConnecting);
	iLed = ELedConnecting;
	if (proxy)
		iProxy.Start(ProxyMode(iS), !iS.proxyNoZip, iS2.img ? iS2.imgWidth : 0, iS2.imgMaxKB);
	else if (tls)
		{
		char why[96];
		why[0] = 0;
		iHal.TlsVerify(iS2.tlsVerify != 0);
		if (!iHal.UpConnect(target, (uint16_t)port, true, why, sizeof(why)))
			{
			char m[320];
			snprintf(m, sizeof(m), "tls %s:%ld: %s", target, port, why[0] ? why : "failed");
			iHal.Log(m);
			return RNoCarrier;
			}
		}
	else if (!iHal.TcpConnect(target, (uint16_t)port))
		{
		iProbeDueMs = iHal.Millis();         // (a failed connect: is the Internet there at all?)
		return RNoCarrier;
		}
	iProxyCall = proxy;
	iExecCall = false;
	iExecOneShot = false;
	iTlsCall = tls && !proxy;
	iConnected = true;
	iClosing = false;
	iOnline = true;
	iRing.Clear();
	iUpLen = 0;
	iPluses = 0;
	ApplyPacing();                           // a fresh token bucket
	iLastSerialMs = iHal.Millis();
	Result(RConnect);
	UpdateDcd();
	return RNone;
	}

// The handshake with psiexecd: "PSIEXEC/1 <token> one|channel\r\n", then
// "OK\r\n" (or "ERR ...\r\n" and the helper closes). "one" (AT$EXEC=): one
// command line follows, the helper runs it, sends its output and closes.
// "channel" (ATDT psiexec): a line at a time for as long as the call lasts
bool Modem::ExecHandshake(const char* aCommand)
	{
	char line[128];
	int n = snprintf(line, sizeof(line), "PSIEXEC/1 %s %s\r\n", iS2.execToken, aCommand ? "one" : "channel");
	if (n <= 0 || n >= (int)sizeof(line))
		return false;
	if (iHal.TcpWrite((const uint8_t*)line, (size_t)n) != (size_t)n)
		return false;
	// the answer: a line within 5 s
	uint32_t start = iHal.Millis();
	size_t got = 0;
	for (;;)
		{
		uint8_t c;
		if (iHal.TcpAvailable() && iHal.TcpRead(&c, 1) == 1)
			{
			if (c == '\n')
				break;
			if (c != '\r' && got < sizeof(line) - 1)
				line[got++] = (char)c;
			continue;
			}
		if (!iHal.TcpOpen() || iHal.Millis() - start > 5000)
			{
			iHal.Log("psiexec: no answer from the helper");
			return false;
			}
		iHal.Idle();
		}
	line[got] = 0;
	if (strncmp(line, "OK", 2) != 0)
		{
		char m[160];
		snprintf(m, sizeof(m), "psiexec: the helper refused: %.100s", line);
		iHal.Log(m);
		return false;
		}
	if (aCommand)
		{
		size_t len = strlen(aCommand);
		if (iHal.TcpWrite((const uint8_t*)aCommand, len) != len || iHal.TcpWrite((const uint8_t*)"\n", 1) != 1)
			return false;
		}
	return true;
	}

// ATDT psiexec, or AT$EXEC=command: a call to the helper
Modem::TResult Modem::DialExec(const char* aCommand)
	{
	if (!iS2.exec || !iS2.execHost[0])
		return aCommand ? RError : RNoCarrier;
	if (!iUplink.Up())
		return aCommand ? RError : RNoCarrier;
	char host[64];
	strncpy(host, iS2.execHost, sizeof(host) - 1);
	host[sizeof(host) - 1] = 0;
	char* colon = strrchr(host, ':');
	if (!colon)
		return aCommand ? RError : RNoCarrier;
	*colon = 0;
	const char* q = colon + 1;
	long port = Number(q, -1);
	if (port <= 0 || port > 65535 || *q || !host[0])
		return aCommand ? RError : RNoCarrier;
	if (iConnected || iClosing)
		Hangup(false);
	iHal.Led(ELedConnecting);
	iLed = ELedConnecting;
	if (!iHal.TcpConnect(host, (uint16_t)port) || !ExecHandshake(aCommand))
		{
		iHal.TcpClose();
		return aCommand ? RError : RNoCarrier;
		}
	iProxyCall = false;
	iExecCall = true;
	iExecOneShot = aCommand != 0;
	iTlsCall = false;
	iConnected = true;
	iClosing = false;
	iOnline = true;
	iRing.Clear();
	iUpLen = 0;
	iPluses = 0;
	ApplyPacing();
	iLastSerialMs = iHal.Millis();
	if (!aCommand)
		Result(RConnect);                    // (a one-shot prints the output, then OK)
	UpdateDcd();
	return RNone;
	}

void Modem::EndCall()
	{
	if (iPppCall)
		iPpp.Stop(iHal);                     // (tears the PPP link down, NAT off)
	else if (iProxyCall)
		iProxy.Stop();                       // (closes its server connection)
	else
		iHal.TcpClose();
	iProxyCall = false;
	iPppCall = false;
	iExecCall = false;
	iTlsCall = false;
	iConnected = false;
	}

void Modem::Hangup(bool aSayNoCarrier)
	{
	EndCall();
	iExecOneShot = false;
	iClosing = false;
	iOnline = false;
	iRing.Clear();
	iUpLen = 0;
	iPluses = 0;
	UpdateDcd();
	if (aSayNoCarrier)
		Result(RNoCarrier);
	}

void Modem::Info()
	{
	char m[160];
	snprintf(m, sizeof(m), "Atom modem %s (Psion-tuned)", kVersion);
	SayLine(m);
	char w[96];
	if (iHal.WifiUp())
		{
		iHal.WifiInfo(w, sizeof(w));
		snprintf(m, sizeof(m), "WiFi: %s; %s", w, Uplink::WifiStateName(iUplink.WifiState()));
		}
	else
		snprintf(m, sizeof(m), "WiFi: not connected%s%s%s (%s)", iS.ssid[0] ? " (\"" : "", iS.ssid, iS.ssid[0] ? "\")" : "",
			Uplink::WifiStateName(iUplink.WifiState()));
	SayLine(m);
	iUplink.Describe(w, sizeof(w));
	snprintf(m, sizeof(m), "Uplink %s", w);
	SayLine(m);
	char d[48];
	int us = iHal.UsbState(d, sizeof(d));
	snprintf(m, sizeof(m), "USB-C: %s%s%s%s", iS2.usbHost ? "host" : "device", iS2.usbHost ? ", " : "",
		iS2.usbHost ? Uplink::UsbStateName(us) : "", d[0] ? d : "");
	SayLine(m);
	if (iS.paceRate)
		snprintf(m, sizeof(m), "Serial %lu baud, %s, pacing %lu bytes/s in %u-byte bursts",
			(unsigned long)iS.baud, iS2.flow ? "RTS/CTS" : "no flow control", (unsigned long)iS.paceRate, iS.paceBurst);
	else
		snprintf(m, sizeof(m), "Serial %lu baud, %s, no pacing", (unsigned long)iS.baud, iS2.flow ? "RTS/CTS" : "no flow control");
	SayLine(m);
	if (ProxyMode(iS) == Proxy::EOff)
		snprintf(m, sizeof(m), "Web proxy: off (AT$PX=1 turns it on)");
	else
		snprintf(m, sizeof(m), "Web proxy: ATDT %s:8080, %s%s; %lu requests", Proxy::kName,
			ProxyMode(iS) == Proxy::EText ? "text only" : ProxyMode(iS) == Proxy::ERaw ? "pages unchanged"
				: ProxyMode(iS) == Proxy::EReader ? "reader" : "simplified pages",
			iS2.img ? ", pictures to 16 greys" : "", (unsigned long)iProxy.Requests());
	SayLine(m);
	char ports[48];
	TlsPortsText(iS2, ports, sizeof(ports));
	snprintf(m, sizeof(m), "TLS termination: %s (ports %s%s); remote compute: %s", iS2.tls ? "on" : "off", ports,
		iS2.tlsVerify ? "" : ", certificates NOT checked", iS2.exec ? "on" : "off");
	SayLine(m);
	iHal.WebInfo(w, sizeof(w));
	iHal.ApInfo(d, sizeof(d));
	if (w[0] || d[0])
		{
		snprintf(m, sizeof(m), "Web pages: %s%s%s", w, w[0] && d[0] ? "; " : "", d);
		SayLine(m);
		}
	iHal.MemInfo(w, sizeof(w));
	if (w[0])
		SayLine(w);
	}

// AT&V: every setting (secrets as set/none)
void Modem::ShowSettings()
	{
	char m[160];
	snprintf(m, sizeof(m), "E%d V%d Q%d &C%d S2=%d S12=%d", iS.echo, iS.verbose, iS.quiet, iS.dcdMode, iS.s2, iS.s12);
	SayLine(m);
	am::Config c = { iS, iS2 };
	for (int i = 0; i < SchemaCount(); i++)
		{
		const SchemaEntry& e = SchemaAt(i);
		char v[100];
		SchemaGet(c, e, v, sizeof(v));
		snprintf(m, sizeof(m), "$%s=%s", e.name, v);
		SayLine(m);
		}
	}

// AT$HELP: the settings with their ranges
void Modem::Help()
	{
	char m[200];
	am::Config c = { iS, iS2 };
	for (int i = 0; i < SchemaCount(); i++)
		{
		const SchemaEntry& e = SchemaAt(i);
		char v[100];
		SchemaGet(c, e, v, sizeof(v));
		switch (e.type)
			{
		case STEnum:
			{
			char names[64];
			size_t n = 0;
			names[0] = 0;
			for (int j = 0; e.names[j] && n < sizeof(names) - 8; j++)
				n += (size_t)snprintf(names + n, sizeof(names) - n, "%s%s", j ? "|" : "", e.names[j]);
			snprintf(m, sizeof(m), "$%s=%s (%s): %s", e.name, names, v, e.help);
			break;
			}
		case STInt: case STPace:
			snprintf(m, sizeof(m), "$%s=%ld..%ld (%s): %s", e.name, (long)e.min, (long)e.max, v, e.help);
			break;
		case STBool: case STBoolInv:
			snprintf(m, sizeof(m), "$%s=0|1 (%s): %s", e.name, v, e.help);
			break;
		default:
			snprintf(m, sizeof(m), "$%s=... (%s): %s", e.name, v, e.help);
			break;
			}
		SayLine(m);
		}
	SayLine("$PACE? $AP? $UP? $LOG? $HELP; $EXEC=command; $RESET=YES");
	}

// AT$LOG?: the log ring, oldest first
void Modem::ShowLog()
	{
	const LogRing& log = iHal.LogLines();
	char buf[128];
	size_t from = 0, n;
	Say("\r\n");
	while ((n = log.Read(from, buf, sizeof(buf))) > 0)
		{
		// the ring's '\n' become CR LF on the line
		for (size_t i = 0; i < n; i++)
			{
			if (buf[i] == '\n')
				Say("\r\n");
			else
				Send((const uint8_t*)&buf[i], 1);
			}
		from += n;
		}
	}

// ----- to the Psion -----------------------------------------------------------

void Modem::Send(const uint8_t* aData, size_t aLen)
	{
	size_t done = 0;
	uint32_t start = iHal.Millis();
	while (done < aLen)
		{
		size_t n = iHal.SerialWrite(aData + done, aLen - done);
		done += n;
		if (!n && iHal.Millis() - start > 2000)
			break;                           // never hang on a stuck UART
		}
	}

void Modem::Say(const char* aText)
	{
	Send((const uint8_t*)aText, strlen(aText));
	}

void Modem::SayLine(const char* aText)
	{
	Say("\r\n");
	Say(aText);
	}

void Modem::Result(TResult aR)
	{
	if (iS.quiet || aR == RNone)
		return;
	if (!iS.verbose)
		{
		char m[4];
		snprintf(m, sizeof(m), "%d\r", (int)aR);
		Say(m);
		return;
		}
	switch (aR)
		{
	case ROk: Say("\r\nOK\r\n"); break;
	case RConnect:
		{
		char m[32];
		snprintf(m, sizeof(m), "\r\nCONNECT %lu\r\n", (unsigned long)iS.baud);
		Say(m);
		break;
		}
	case RRing: Say("\r\nRING\r\n"); break;
	case RNoCarrier: Say("\r\nNO CARRIER\r\n"); break;
	default: Say("\r\nERROR\r\n"); break;
		}
	}

// ----- the server side ------------------------------------------------------

void Modem::PumpServer(uint32_t aNow)
	{
	if (iConnected && iProxyCall)
		{
		// the web proxy: it fills the ring itself, and ends the call when the
		// Psion asked it to (or the uplink goes)
		if (!iProxy.Pump(iRing) || !iUplink.Up())
			{
			iProxy.Stop();
			iProxyCall = false;
			iConnected = false;
			iClosing = true;
			}
		}
	else if (iConnected)
		{
		// read only what the ring can hold: when it is full the socket is
		// left alone, its window closes, and the server waits (TCP flow
		// control standing in for the RTS/CTS a three-wire board lacks)
		for (int i = 0; i < 8; i++)
			{
			size_t room = iRing.Free();
			size_t avail = iHal.TcpAvailable();
			if (!avail || room < 64)
				break;
			size_t want = avail < room ? avail : room;
			if (want > sizeof(iTcpBuf))
				want = sizeof(iTcpBuf);
			size_t n = iHal.TcpRead(iTcpBuf, want);
			if (!n)
				break;
			iRing.Put(iTcpBuf, n);
			}
		if ((!iHal.TcpOpen() && iHal.TcpAvailable() == 0) || !iUplink.Up())
			{
			// the server closed, or the uplink went: say NO CARRIER once what
			// has arrived has been delivered
			iHal.TcpClose();
			iConnected = false;
			iExecCall = false;
			iTlsCall = false;
			iClosing = true;
			}
		}
	if (iClosing && (iRing.Count() == 0 || !iOnline))
		{
		bool oneShot = iExecOneShot;
		iExecOneShot = false;
		iClosing = false;
		iOnline = false;
		iRing.Clear();
		iPluses = 0;
		iUpLen = 0;
		UpdateDcd();
		Result(oneShot ? ROk : RNoCarrier);
		}
	(void)aNow;
	}

void Modem::PumpPsion(uint32_t aNow)
	{
	if (!iOnline || iRing.Count() == 0)
		return;
	for (int i = 0; i < 4 && iRing.Count(); i++)
		{
		size_t room = iHal.SerialWritable();
		if (!room)
			break;
		const uint8_t* p;
		size_t run = iRing.Peek(p);
		size_t want = run < room ? run : room;
		size_t n = iPacer.Allow(iHal.Micros(), want);
		if (!n)
			break;
		n = iHal.SerialWrite(p, n);
		iPacer.Spent(iHal.Micros(), n);
		iRing.Drop(n);
		iToPsion += n;
		if (n)
			iLastDataMs = aNow;
		else
			break;
		}
	}

void Modem::UpdateDcd()
	{
	iHal.Dcd(iS.dcdMode == 0 || iConnected || (iClosing && iRing.Count()));
	}

void Modem::UpdateLed(uint32_t aNow)
	{
	bool up = iUplink.Up();
	int s;
	if (iConnected || iClosing)
		s = (aNow - iLastDataMs < 60) ? ELedData : ELedConnected;
	else if (iConfigMode)
		s = ELedConfig;
	else
		s = up ? ELedWifi : ELedNoWifi;
	if (s != iLed)
		{
		iLed = s;
		iHal.Led((LedState)s);
		}
	iWifiWasUp = up;
	}

} // namespace am
