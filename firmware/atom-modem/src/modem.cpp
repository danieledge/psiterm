// modem.cpp - see modem.h. MIT licence (see LICENSE at the top of the repository).
#include "modem.h"
#include "atremote.h"
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

int Hal::WifiScanFull(WifiNet* aOut, int aMax)
	{
	char names[24][33];
	if (aMax > 24)
		aMax = 24;
	int n = WifiScan(names, aMax);
	for (int i = 0; i < n; i++)
		{
		memcpy(aOut[i].ssid, names[i], sizeof(aOut[i].ssid));
		aOut[i].ssid[sizeof(aOut[i].ssid) - 1] = 0;
		aOut[i].rssi = 0;
		aOut[i].secure = 1;
		aOut[i].channel = 0;
		}
	return n;
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

bool LogRing::Line(size_t aFromEnd, char* aOut, size_t aMax) const
	{
	if (!aMax)
		return false;
	aOut[0] = 0;
	// every line ends in '\n': walk back over aFromEnd of them
	size_t end = iCount;                      // one past the newline that ends the wanted line
	for (size_t skip = 0; skip <= aFromEnd; skip++)
		{
		if (end == 0)
			return false;
		size_t stop = end - 1;                // the newline itself
		size_t start = stop;
		while (start > 0 && iBuf[(iHead + start - 1) % kSize] != '\n')
			start--;
		if (skip == aFromEnd)
			{
			size_t n = stop - start;
			if (n >= aMax) n = aMax - 1;
			for (size_t i = 0; i < n; i++)
				aOut[i] = iBuf[(iHead + start + i) % kSize];
			aOut[n] = 0;
			return true;
			}
		end = start;
		}
	return false;
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

// A network name or password as AT$SCAN and AT$STAT? print it, and as
// AT$JOIN takes it: in double quotes, with \" for a quote, \\ for a
// backslash, \/ for a slash (so "A/" in a name can never look like the
// A/ repeat command) and \xHH for any byte that is not printable ASCII
static void Quote(const char* aIn, char* aOut, size_t aMax)
	{
	size_t n = 0;
	if (aMax < 3)
		{
		if (aMax) aOut[0] = 0;
		return;
		}
	aOut[n++] = '"';
	for (const unsigned char* p = (const unsigned char*)aIn; *p; p++)
		{
		char e[5];
		size_t el;
		if (*p == '"' || *p == '\\' || *p == '/')
			{ e[0] = '\\'; e[1] = (char)*p; el = 2; }
		else if (*p < 32 || *p > 126)
			{ snprintf(e, sizeof(e), "\\x%02X", *p); el = 4; }
		else
			{ e[0] = (char)*p; el = 1; }
		if (n + el + 2 > aMax)
			break;
		memcpy(aOut + n, e, el);
		n += el;
		}
	aOut[n++] = '"';
	aOut[n] = 0;
	}

static int HexDigit(int aC)
	{
	if (aC >= '0' && aC <= '9') return aC - '0';
	aC = Upper(aC);
	if (aC >= 'A' && aC <= 'F') return aC - 'A' + 10;
	return -1;
	}

// One field of AT$JOIN, quoted as Quote() writes it (or bare, ending at a
// comma, with the same escapes). False: not well formed, or longer than
// aMax-1 bytes
static bool Field(const char*& aP, char* aOut, size_t aMax)
	{
	while (*aP == ' ') aP++;
	bool quoted = *aP == '"';
	if (quoted)
		aP++;
	size_t n = 0;
	for (;;)
		{
		int c = (unsigned char)*aP;
		if (!c)
			{
			if (quoted)
				return false;                // no closing quote
			break;
			}
		if (quoted && c == '"')
			{
			aP++;
			break;
			}
		if (!quoted && c == ',')
			break;
		aP++;
		if (c == '\\')
			{
			c = (unsigned char)*aP;
			if (!c)
				return false;
			aP++;
			if (c == 'x' || c == 'X')
				{
				int h = HexDigit((unsigned char)aP[0]), l = h < 0 ? -1 : HexDigit((unsigned char)aP[1]);
				if (h < 0 || l < 0)
					return false;
				c = h * 16 + l;
				aP += 2;
				}
			}
		if (n + 1 >= aMax)
			return false;
		aOut[n++] = (char)c;
		}
	if (!quoted)
		while (n && aOut[n - 1] == ' ')
			n--;                             // (bare: trailing spaces are not part of it)
	aOut[n] = 0;
	while (*aP == ' ') aP++;
	return true;
	}

// The command line as the log shows it: printable, and with the secrets
// left out - the value of AT$PASS, $XK, $WEBPASS and $APPASS, all of
// AT$JOIN and AT$NETADD, every AT$WGL line (a WireGuard config: its keys),
// and the password in ATW"ssid,password". Public (modem.h) for the tests
void LogSafe(const char* aCmd, char* aOut, size_t aMax)
	{
	static const char* const kSecret[] = { "$PASS=", "$XK=", "$WEBPASS=", "$APPASS=", "$JOIN=", "$NETADD=", "$WGL=", 0 };
	size_t cut = (size_t)-1;
	for (int k = 0; kSecret[k]; k++)
		for (const char* q = aCmd; *q; q++)
			if (StartsNoCase(q, kSecret[k]))
				{
				size_t at = (size_t)(q - aCmd) + strlen(kSecret[k]);
				if (at < cut) cut = at;
				break;
				}
	// ATW"ssid,password" (Zimodem): from the comma
	const char* w = aCmd;
	while (*w == ' ') w++;
	if (Upper((unsigned char)*w) == 'W' && w[1] && strchr(w, ','))
		{
		size_t at = (size_t)(strchr(w, ',') - aCmd) + 1;
		if (at < cut) cut = at;
		}
	size_t j = 0;
	for (size_t i = 0; aCmd[i] && j + 1 < aMax && i < cut; i++)
		aOut[j++] = (aCmd[i] >= 32 && aCmd[i] < 127) ? aCmd[i] : '.';
	aOut[j] = 0;
	if (cut != (size_t)-1 && j + 9 < aMax)
		strcat(aOut, "(hidden)");
	}

Modem::Modem(Hal& aHal, uint8_t* aRing, size_t aRingSize)
	: iHal(aHal), iLineLen(0), iOnline(false), iConnected(false), iClosing(false), iWifiWasUp(false),
	  iLastSerialMs(0), iPluses(0), iLastPlusMs(0), iToPsion(0), iToServer(0), iLastDataMs(0),
	  iPendingBaud(0), iTrialFrom(0), iTrialStartMs(0), iTrialArmed(false), iTrialGen(0), iLed(-1), iProxy(aHal),
	  iProxyCall(false), iPppCall(false), iExecCall(false), iExecOneShot(false), iTlsCall(false), iConfigMode(false),
	  iConfigUntilMs(0), iLastTickMs(0), iJoinMs(0), iJoinFail(0), iWifiDownMs(0), iWifiRetryMs(0), iWifiRetries(0), iProbeDueMs(0), iLastWifiState(-1),
	  iLastActive(-1), iNetSkip(0), iNetTrying(-1), iWgWant(EWgWantOff), iWgApplied(false), iWgGen(1), iWgAppliedGen(0),
	  iWgIp(0), iWgLastState(-1), iWgTickMs(0), iLastDnsOk(-1), iDnsUp(0), iWgTextLen(0), iWgTextBad(false), iPendingPins(false),
	  iLineTrial(false), iLineArmed(false), iLineStartMs(0), iLineOldFlow(0), iLineOldFlowSwap(0), iLineOldSwap(0),
	  iLineOldDcd(-1), iUpLen(0), iCap(0), iCapLen(0), iCapMax(0), iCapCut(false), iInResult(false), iRemote(false)
	{
	iRing.Init(aRing, aRingSize);
	iPinned[0] = 0;
	iWgText[0] = 0;
	memset(iLineOldPins, -1, sizeof(iLineOldPins));
	memset(&iWgStatus, 0, sizeof(iWgStatus));
	iWgStatus.handshakeAge = -1;
	NetListInit(iNets);
	WgClear(iWg);
	iLast[0] = 0;
	iLastDial[0] = 0;
	iLastDialResult[0] = 0;
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
	// 2.2: the saved networks and the WireGuard config
	static NetList nl;                       // (static: kept off the stack)
	len = 0;
	memset(&nl, 0, sizeof(nl));
	if (iHal.LoadNets(nl, len) && NetListAccept(nl, len))
		iNets = nl;
	else
		NetListInit(iNets);
	memset(&nl, 0, sizeof(nl));
	static WgConfig wg;
	len = 0;
	memset(&wg, 0, sizeof(wg));
	if (iHal.LoadWg(wg, len) && WgAccept(wg, len))
		iWg = wg;
	else
		WgClear(iWg);
	WgClear(wg);
	iWgGen++;
	iPinned[0] = 0;
	iNetSkip = 0;
	iNetTrying = -1;
	iLineTrial = iLineArmed = iPendingPins = false;
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
	// 2.2: a network saved by an older firmware becomes the first saved one
	if (iNets.count == 0 && iS.ssid[0])
		{
		NetUpsert(iNets, iS.ssid, iS.pass, -1);
		SaveNetsNow();
		char m[96];
		snprintf(m, sizeof(m), "WiFi: \"%s\" moved into the saved networks (1 of %d)", iS.ssid, kNetMax);
		iHal.Log(m);
		}
	if (iNets.count > 1)
		ChooseNet(iHal.Millis(), true);      // the strongest saved network in range
	else if (iS.ssid[0])
		iHal.WifiBegin(iS.ssid, iS.pass);
	else if (iNets.count == 1)
		{
		memcpy(iS.ssid, iNets.net[0].ssid, sizeof(iS.ssid));
		memcpy(iS.pass, iNets.net[0].pass, sizeof(iS.pass));
		iHal.WifiBegin(iS.ssid, iS.pass);
		}
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
			{
			iHal.Log("ppp: link dropped by the Psion (or lost) - hanging up");
			Hangup(true);                    // the Psion closed the link (or an error)
			}
		else if (iOnline)                    // (after +++ nothing goes out; ATO resumes)
			{
			size_t n = iPpp.Drain(iHal, iPacer);   // PPP frames to the Psion, paced like PumpPsion
			if (n)
				{
				iToPsion += (uint32_t)n;
				iLastDataMs = now;
				}
			}
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
	if (iPendingPins && iHal.SerialWritable() > 0)
		{
		iPendingPins = false;
		iHal.ApplyPins(iS, iS2);                // (flushes the OK out first)
		ApplyPacing();
		UpdateDcd();
		if (iLineTrial)
			{
			iLineArmed = true;                   // the 15 s start now, with the new line settings
			iLineStartMs = iHal.Millis();
			}
		}
	BaudTrialTick(iHal.Millis());
	if (iLineTrial && iLineArmed && !iOnline && iHal.Millis() - iLineStartMs >= kBaudTrialMs)
		LineTrialRevert();
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
	// why a join is failing, kept: the rejoin below starts the board's afresh
	if (assoc)
		iJoinFail = EWifiFailNone;
	else if (iJoinMs)
		{
		int f = iHal.WifiFail();
		if (f != EWifiFailNone)
			iJoinFail = f;
		}
	char d[48];
	iUplink.Tick(aNow, assoc, iHal.UsbState(d, sizeof(d)), assoc ? iHal.Internet() : -1);
	// joining again: the core's own reconnect usually does it, but a hotspot
	// that went away and came back can leave the station stuck. With
	// networks saved (2.2) the rejoin is a choice: the strongest saved one
	// in range (a hand-picked one first), falling back through the list
	bool inJoin = iJoinMs && aNow - iJoinMs < 40000;   // (AT$JOIN/AT$NET: that network, not a choice)
	if ((iS.ssid[0] || iNets.count) && !assoc)
		{
		// iWifiRetryMs holds when the next rejoin is due. A short grace first
		// (the core's own reconnect gets a chance), then every so often. Each
		// rejoin resets the radio (WifiHardReset) before WifiBegin: a bare
		// begin() - and a soft reset - do not clear a wedged PHY/MAC, which is
		// why the fault used to need a hardware power cycle.
		if (!iWifiDownMs)
			{
			iWifiDownMs = aNow ? aNow : 1;
			iWifiRetryMs = aNow + kWifiRetryAfterMs;
			iWifiRetries = 0;
			}
		else if ((int32_t)(aNow - iWifiRetryMs) >= 0)
			{
			iWifiRetryMs = aNow + kWifiRetryEveryMs;
			iWifiRetries++;
			// A hard reset (WiFi.mode(OFF)) destroys the station netif and stalls
			// the loop ~300 ms - fine to un-wedge a dead PHY, but not while a call
			// is live (it would overrun the Psion's UART, and whether NAT resumes
			// on the new netif is unverified) nor when the uplink is USB (WiFi is
			// not even the path). So: a cheap begin() first, and a hard reset only
			// from the second try and only when no call is up and we are on WiFi.
			bool inCall = iConnected || iClosing;
			bool usbUplink = iUplink.Active() == EActiveUsb;
			bool hard = iWifiRetries >= 2 && !inCall && !usbUplink;
			// a choice to make: more than one saved, or the one saved is not the
			// current. A scan blocks the loop for 2-4 s: never during a call
			bool choose = !inCall && !inJoin && (iNets.count > 1 || (iNets.count == 1 && NetFind(iNets, iS.ssid) < 0));
			char m[112];
			if (choose)
				snprintf(m, sizeof(m), "WiFi: not joined for %lu s: %s and choosing a saved network",
					(unsigned long)((aNow - iWifiDownMs) / 1000), hard ? "resetting the radio" : "scanning");
			else
				snprintf(m, sizeof(m), "WiFi: not joined for %lu s: %s and joining \"%s\" again",
					(unsigned long)((aNow - iWifiDownMs) / 1000), hard ? "resetting the radio" : "retrying", iS.ssid);
			iHal.Log(m);
			if (hard)
				iHal.WifiHardReset();
			if (choose)
				ChooseNet(aNow, false);
			else if (iS.ssid[0])
				iHal.WifiBegin(iS.ssid, iS.pass);
			}
		}
	else
		{
		iWifiDownMs = 0;
		iWifiRetryMs = 0;
		iWifiRetries = 0;
		if (assoc)
			{
			iNetSkip = 0;
			iNetTrying = -1;
			}
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
	WgTick(aNow);
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

// ----- 2.2: AT commands over PPP (atremote.h, board/pppat.cpp) ------------------

size_t Modem::RemoteLine(const char* aLine, char* aOut, size_t aMax)
	{
	if (!aMax)
		return 0;
	aOut[0] = 0;
	const char* p = aLine;
	while (p[0] && p[1] && !(Upper(p[0]) == 'A' && Upper(p[1]) == 'T'))
		p++;
	if (!p[0] || !p[1])
		return 0;                            // no command on this line: no answer
	p += 2;
	char cmd[256];
	size_t n = strlen(p);
	if (n >= sizeof(cmd)) n = sizeof(cmd) - 1;
	memcpy(cmd, p, n);
	cmd[n] = 0;
	char m[80];
	memcpy(m, "cmd (ppp): ", 11);
	LogSafe(cmd, m + 11, sizeof(m) - 11);
	iHal.Log(m);
	iCap = aOut;
	iCapLen = 0;
	iCapMax = aMax - 1;
	iCapCut = false;
	iInResult = false;
	iRemote = true;
	char why[48];
	if (!AtRemoteAllowed(cmd, why, sizeof(why)))
		{
		char t[64];
		snprintf(t, sizeof(t), "+NOTVIAPPP: %s", why);
		SayLine(t);
		Result(RError);
		}
	else
		{
		// (RunCommands logs "cmd: ..." itself; no ConfirmBaud: a command over
		// TCP says nothing about the serial line's new settings)
		TResult r = RunCommands(cmd);
		if (r != RNone)
			Result(r);
		}
	memset(cmd, 0, sizeof(cmd));
	iRemote = false;
	iCap = 0;
	aOut[iCapLen] = 0;
	return iCapLen;
	}

// ATZ over PPP: the saved settings again, but the live link (the serial
// line, the PPP switch, the uplink) as it is now, and nothing re-applied to
// the UART: the Psion's PPP link runs over it
void Modem::RemoteReload()
	{
	static Settings s;                       // (static: kept off the loop task's stack)
	static Settings2 s2;
	s = iS;
	s2 = iS2;
	LoadAll();
	iS.baud = s.baud;
	iS.echo = s.echo;
	iS.verbose = s.verbose;
	iS.quiet = s.quiet;
	iS.swapPins = s.swapPins;
	iS.dcdPin = s.dcdPin;
	iS.dcdMode = s.dcdMode;
	iS.s2 = s.s2;
	iS.s12 = s.s12;
	iS2.flow = s2.flow;
	iS2.flowSwap = s2.flowSwap;
	memcpy(iS2.pins, s2.pins, sizeof(iS2.pins));
	iS2.ppp = s2.ppp;
	iS2.uplink = s2.uplink;
	iS2.usbHost = s2.usbHost;
	ApplyPacing();
	iHal.TlsVerify(iS2.tlsVerify != 0);
	memset(&s, 0, sizeof(s));                // (passwords)
	memset(&s2, 0, sizeof(s2));
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
	if (iLineTrial && iLineArmed && aGen == iTrialGen && aR != RError)
		{
		iLineTrial = iLineArmed = false;
		iHal.Log("Serial line settings confirmed: a command was heard with them");
		}
	}

// AT$FC, $FCSWAP, $SWAP, $PINS or $DCD from the Psion: what to go back to
void Modem::StartLineTrial()
	{
	iTrialGen++;                             // (this line itself does not confirm it)
	if (!iLineTrial || iLineArmed)
		{
		iLineOldFlow = iS2.flow;
		iLineOldFlowSwap = iS2.flowSwap;
		iLineOldSwap = iS.swapPins;
		memcpy(iLineOldPins, iS2.pins, sizeof(iLineOldPins));
		iLineOldDcd = iS.dcdPin;
		}
	iLineTrial = true;
	iLineArmed = false;
	iPendingPins = true;                     // after the OK, with the old settings
	}

void Modem::LineTrialRevert()
	{
	iLineTrial = iLineArmed = false;
	iS2.flow = iLineOldFlow;
	iS2.flowSwap = iLineOldFlowSwap;
	iS.swapPins = iLineOldSwap;
	memcpy(iS2.pins, iLineOldPins, sizeof(iS2.pins));
	iS.dcdPin = iLineOldDcd;
	iHal.ApplyPins(iS, iS2);
	ApplyPacing();
	UpdateDcd();
	iLineLen = 0;
	char m[120];
	snprintf(m, sizeof(m), "No command in %lu s with the new serial line settings: back to the old ones (FC %d)",
		(unsigned long)(kBaudTrialMs / 1000), iS2.flow ? 1 : 0);
	iHal.Log(m);
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
void Modem::Apply(TSchemaApply aWhat, uint32_t aOldBaud, bool aFromPsion)
	{
	switch (aWhat)
		{
	case SAWifi:
		if (iS.ssid[0] && iS.pass[0])
			{
			snprintf(iPinned, sizeof(iPinned), "%s", iS.ssid);   // (picked by hand)
			iHal.WifiBegin(iS.ssid, iS.pass);  // join once both are known
			}
		break;
	case SAWg:
		iWgGen++;                            // NetTick decides again at once
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
	case SADcd:
		if (aFromPsion)
			break;                           // (SetCommand put it on trial: applied after the OK)
		iHal.ApplyPins(iS, iS2);
		ApplyPacing();                       // (flow control changes the auto rate)
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
	iLineTrial = iLineArmed = false;         // ... and so are saved line settings
	UpsertCurrent();
	SaveNetsNow();
	return true;
	}

// the network the modem is on (or joining) into the saved list (AT&W)
void Modem::UpsertCurrent()
	{
	if (!iS.ssid[0])
		return;
	bool evicted = false;
	int keep = NetFind(iNets, iS.ssid);
	NetUpsert(iNets, iS.ssid, iS.pass, keep, &evicted);
	if (evicted)
		iHal.Log("WiFi: the saved networks were full: the last one was dropped to make room");
	}

void Modem::SaveNetsNow()
	{
	if (!iHal.SaveNets(iNets))
		iHal.Log("WiFi: could not save the networks");
	}

// Factory settings. aKeepWifi (AT&F): the network is kept, and so is the
// link hardware (flow control, pins, PPP), which belongs to the cable and
// the Psion, not to the "settings": losing it on AT&F would silently turn
// RTS/CTS off. AT$RESET=YES and the web reset clear everything
void Modem::Factory(bool aKeepWifi)
	{
	Settings f;
	FactoryDefaults(f);
	Settings2 f2;
	FactoryDefaults2(f2);
	if (aKeepWifi)
		{
		memcpy(f.ssid, iS.ssid, sizeof(f.ssid));
		memcpy(f.pass, iS.pass, sizeof(f.pass));
		f.swapPins = iS.swapPins;
		f.dcdPin = iS.dcdPin;
		f2.flow = iS2.flow;
		f2.flowSwap = iS2.flowSwap;
		memcpy(f2.pins, iS2.pins, sizeof(f2.pins));
		f2.ppp = iS2.ppp;
		}
	else
		{
		// everything: the saved networks and the WireGuard config too
		NetListInit(iNets);
		WgClear(iWg);
		iPinned[0] = 0;
		}
	iWgGen++;
	uint32_t oldBaud = iS.baud;
	iS = f;
	iS2 = f2;
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
	// log the AT line as it comes in (command mode only, so browsing - which
	// stays in PPP data mode - never spams this). Makes a dial, a hang-up or a
	// stray init string visible on the console when a connect misbehaves.
	// (Over PPP RemoteLine has logged it already, as "cmd (ppp): ...")
	if (aCmd[0] && !iRemote)
		{
		char m[80];
		memcpy(m, "cmd: ", 5);
		LogSafe(aCmd, m + 5, sizeof(m) - 5);
		iHal.Log(m);
		}
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
		// (over PPP E, V and Q are accepted and change nothing: they are the serial line's)
		case 'E': v = Number(p, 0); if (!iRemote) iS.echo = v ? 1 : 0; break;
		case 'V': v = Number(p, 0); if (!iRemote) iS.verbose = v ? 1 : 0; break;
		case 'Q': v = Number(p, 0); if (!iRemote) iS.quiet = v ? 1 : 0; break;
		case 'H':
			Number(p, 0);
			if (iConnected || iClosing)
				Hangup(false);
			break;
		case 'Z':
			{
			Number(p, 0);
			if (iRemote)
				{
				RemoteReload();
				return ROk;
				}
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
	if (strcmp(name, "SCAN") == 0)
		{
		aHandled = true;
		Scan();
		return ROk;
		}
	if (strcmp(name, "STAT") == 0 && query)
		{
		aHandled = true;
		Status();
		return ROk;
		}
	if (strcmp(name, "JOIN") == 0)
		{
		aHandled = true;
		if (query)
			return RError;
		return Join(rest);
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
	// ----- 2.2: the saved networks -----
	if (strcmp(name, "NETS") == 0 && query)
		{
		aHandled = true;
		ListNets();
		return ROk;
		}
	if (strcmp(name, "NET") == 0)
		{
		// AT$NET=<n> or AT$NET="name": switch to that saved network now
		aHandled = true;
		if (query)
			return RError;
		int i = NetArg(rest);
		return i >= 0 && SwitchNet(i) ? ROk : RError;
		}
	if (strcmp(name, "NETADD") == 0)
		{
		// AT$NETADD="name","password": save a network (AT&W keeps the list)
		aHandled = true;
		if (query)
			return RError;
		const char* p = rest;
		char ssid[33], pass[65];
		pass[0] = 0;
		bool ok = Field(p, ssid, sizeof(ssid)) && ssid[0];
		if (ok && *p == ',')
			{
			p++;
			ok = Field(p, pass, sizeof(pass));
			}
		ok = ok && !*p && AddNet(ssid, pass, false);
		memset(pass, 0, sizeof(pass));
		return ok ? ROk : RError;
		}
	if (strcmp(name, "FORGET") == 0)
		{
		aHandled = true;
		if (query)
			return RError;
		int i = NetArg(rest);
		return i >= 0 && ForgetNet(i, false) ? ROk : RError;
		}
	if (strcmp(name, "SCHEMA") == 0 && query)
		{
		aHandled = true;
		ShowSchema();
		return ROk;
		}
	// ----- 2.2: WireGuard -----
	if (strcmp(name, "WGCONF") == 0 && query)
		{
		// the config, as far as it may be shown (keys as "set")
		aHandled = true;
		char sum[400];
		WgSummary(iWg, sum, sizeof(sum));
		char* line = sum;
		while (line)
			{
			char* nl = strchr(line, '\n');
			if (nl) *nl = 0;
			char l2[440];
			snprintf(l2, sizeof(l2), "+WGCONF:%s", line);
			SayLine(l2);
			line = nl ? nl + 1 : 0;
			}
		return ROk;
		}
	if (strcmp(name, "WGL") == 0)
		{
		// AT$WGL=<a line of the config>, ..., AT$WGL=END: a config typed or
		// sent line by line (AT$WGL=CLEAR starts again). Never logged
		aHandled = true;
		if (query)
			return RError;
		const char* t = rest;
		while (*t == ' ') t++;
		char quoted[sizeof(iLine)];
		if (*t == '"')
			{
			// quoted, with AT$JOIN's escapes (a slash written as backslash-slash, so
			// an "a/" in a key cannot look like the A/ repeat; quote and backslash too)
			const char* q = t;
			if (!Field(q, quoted, sizeof(quoted)) || *q)
				return RError;
			t = quoted;
			}
		if (StartsNoCase(t, "CLEAR") && !t[5])
			{
			memset(iWgText, 0, sizeof(iWgText));
			iWgTextLen = 0;
			iWgTextBad = false;
			return ROk;
			}
		if (StartsNoCase(t, "END") && !t[3])
			{
			char why[80];
			bool ok = !iWgTextBad && SetWgConfig(iWgText, why, sizeof(why));
			if (iWgTextBad)
				snprintf(why, sizeof(why), "The config is too long");
			memset(iWgText, 0, sizeof(iWgText));
			iWgTextLen = 0;
			iWgTextBad = false;
			if (!ok)
				{
				snprintf(m, sizeof(m), "+WGERR:%s", why);
				SayLine(m);
				return RError;
				}
			return ROk;
			}
		size_t n = strlen(t);
		if (iWgTextLen + n + 1 >= sizeof(iWgText))
			{
			iWgTextBad = true;
			return RError;
			}
		memcpy(iWgText + iWgTextLen, t, n);
		iWgTextLen += n;
		iWgText[iWgTextLen++] = '\n';
		iWgText[iWgTextLen] = 0;
		return ROk;
		}
	if (strcmp(name, "WGCLR") == 0)
		{
		aHandled = true;
		if (query || !StartsNoCase(val, "YES"))
			return RError;
		ClearWgConfig();
		return ROk;
		}
	if (strcmp(name, "WG") == 0 && query)
		{
		// "AUTO: up, handshake 12 s ago" - the mode and the state
		aHandled = true;
		static const char* const kModes[] = { "OFF", "ON", "AUTO" };
		char t[80];
		WgStateText(t, sizeof(t));
		snprintf(m, sizeof(m), "%s: %s", kModes[iS2.wgMode <= 2 ? iS2.wgMode : 0], t);
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
	bool line = e->apply == SAPins || e->apply == SADcd;
	// (the old line settings, before the change)
	uint8_t f0 = iS2.flow, fs0 = iS2.flowSwap, sw0 = iS.swapPins;
	int8_t p0[kPinCount], d0 = iS.dcdPin;
	memcpy(p0, iS2.pins, sizeof(p0));
	// (AT$SSID/AT$PASS take the raw value; a name with quotes in it goes through AT$JOIN)
	if (!SchemaSet(c, *e, val))
		return RError;
	if (line)
		{
		bool changed = f0 != iS2.flow || fs0 != iS2.flowSwap || sw0 != iS.swapPins || d0 != iS.dcdPin
			|| memcmp(p0, iS2.pins, sizeof(p0)) != 0;
		if (!changed)
			return ROk;
		// the new values are in; the trial must remember the old ones
		uint8_t f1 = iS2.flow, fs1 = iS2.flowSwap, sw1 = iS.swapPins;
		int8_t p1[kPinCount], d1 = iS.dcdPin;
		memcpy(p1, iS2.pins, sizeof(p1));
		iS2.flow = f0; iS2.flowSwap = fs0; iS.swapPins = sw0; iS.dcdPin = d0;
		memcpy(iS2.pins, p0, sizeof(p0));
		StartLineTrial();
		iS2.flow = f1; iS2.flowSwap = fs1; iS.swapPins = sw1; iS.dcdPin = d1;
		memcpy(iS2.pins, p1, sizeof(p1));
		}
	Apply(e->apply, oldBaud, true);
	return ROk;
	}

// ATDT host:port (also ATDThost:port, ATD, ATDP, quotes): opens TCP.
// Dial() notes what was dialled and how it went, for the status screen
Modem::TResult Modem::Dial(const char* aArgs)
	{
	const char* p = aArgs;
	if (Upper(*p) == 'T' || Upper(*p) == 'P')
		p++;
	Value(p, iLastDial, sizeof(iLastDial));
	TResult r = DialTo(aArgs);
	if (r == RError)
		strcpy(iLastDialResult, "ERROR");
	return r;                                // (CONNECT and NO CARRIER are noted in Result())
	}

Modem::TResult Modem::DialTo(const char* aArgs)
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
				{
				iHal.Log("ppp: dial ignored - PPP is off (AT$PPP=1 to enable)");
				return RNoCarrier;           // PPP off: a phone number goes nowhere
				}
			if (!iUplink.Up())
				{
				iHal.Log("ppp: dial failed - no uplink (WiFi/USB down)");
				return RNoCarrier;
				}
			if (iConnected || iClosing)
				{
				iHal.Log("ppp: a call was still up at dial - hanging it up first");
				Hangup(false);
				}
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
			ApplyPacing();                   // a fresh token bucket for the PPP frames
			iLastSerialMs = iHal.Millis();
			iHal.Log("ppp: CONNECT - PPP starting for the Psion");
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
	snprintf(iLastDial, sizeof(iLastDial), "psiexec%s%s", aCommand ? ": " : "", aCommand ? aCommand : "");
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
	snprintf(m, sizeof(m), "PsiConnect %s (Psion-tuned)", kVersion);
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
	if (MailRelayAny(iS2))
		{
		char mm[320];
		snprintf(mm, sizeof(mm), "Mail TLS relay (PPP, %s:993/465/995): IMAP %s SMTP %s POP %s", kPppLocalIp,
			iS2.imapHost[0] ? iS2.imapHost : "-", iS2.smtpHost[0] ? iS2.smtpHost : "-", iS2.popHost[0] ? iS2.popHost : "-");
		SayLine(mm);
		}
	{
	char t[80];
	WgStateText(t, sizeof(t));
	static const char* const kModes[] = { "off", "on", "auto" };
	snprintf(m, sizeof(m), "WireGuard: %s, %s; saved networks: %u", kModes[iS2.wgMode <= 2 ? iS2.wgMode : 0], t,
		(unsigned)iNets.count);
	SayLine(m);
	}
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
	if (iHal.FirmwareInfo(m, sizeof(m)) && m[0])
		{
		char f[176];
		snprintf(f, sizeof(f), "Firmware: %s", m);
		SayLine(f);
		}
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
	SayLine("$PACE? $AP? $UP? $LOG? $STAT? $SCAN $HELP; $JOIN=\"ssid\",\"password\"; $EXEC=command; $RESET=YES");
	SayLine("$NETS? $NET=n|\"ssid\" $NETADD=\"ssid\",\"password\" $FORGET=n|\"ssid\" (AT&W keeps the list); $SCHEMA?");
	SayLine("$WG? $WGCONF? $WGL=line...$WGL=END (a WireGuard config; the web page is easier) $WGCLR=YES");
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

// ----- WiFi from the Psion (PsiConnect) -------------------------------------------

// AT$SCAN: the networks in range, strongest first, one line each:
//   +SCAN:<dBm>,<1 secured | 0 open>,<channel>,"<name>"
// Hidden networks are left out, and a name seen on several access points
// is listed once, at its strongest. A scan takes a few seconds.
void Modem::Scan()
	{
	static WifiNet nets[24];
	int n = iHal.WifiScanFull(nets, 24);
	if (n < 0) n = 0;
	if (n > 24) n = 24;
	int kept = 0;
	for (int i = 0; i < n; i++)
		{
		if (!nets[i].ssid[0])
			continue;
		nets[i].ssid[32] = 0;
		int same = -1;
		for (int j = 0; j < kept; j++)
			if (strcmp(nets[j].ssid, nets[i].ssid) == 0)
				same = j;
		int ri = nets[i].rssi ? nets[i].rssi : -127;
		if (same >= 0)
			{
			int rs = nets[same].rssi ? nets[same].rssi : -127;
			if (ri > rs)
				nets[same] = nets[i];
			continue;
			}
		nets[kept++] = nets[i];
		}
	// strongest first (an unknown signal last)
	for (int i = 1; i < kept; i++)
		for (int j = i; j > 0; j--)
			{
			int a = nets[j - 1].rssi ? nets[j - 1].rssi : -127, b = nets[j].rssi ? nets[j].rssi : -127;
			if (a >= b)
				break;
			WifiNet t = nets[j - 1];
			nets[j - 1] = nets[j];
			nets[j] = t;
			}
	char q[140], m[180];
	for (int i = 0; i < kept; i++)
		{
		Quote(nets[i].ssid, q, sizeof(q));
		snprintf(m, sizeof(m), "+SCAN:%d,%d,%u,%s", (int)nets[i].rssi, nets[i].secure ? 1 : 0,
			(unsigned)nets[i].channel, q);
		SayLine(m);
		}
	char l[48];
	snprintf(l, sizeof(l), "WiFi scan from the Psion: %d network%s", kept, kept == 1 ? "" : "s");
	iHal.Log(l);
	}

// AT$JOIN="name","password": joins that network now, as AT$SSID and AT$PASS
// would (an open network takes no password, or ""). OK at once; AT$STAT?
// then says how it went, and AT&W keeps it. A password is 8 to 64
// characters (WPA), and is never shown or logged.
Modem::TResult Modem::Join(const char* aArgs)
	{
	const char* p = aArgs;
	char ssid[sizeof(iS.ssid)], pass[sizeof(iS.pass)];
	if (!Field(p, ssid, sizeof(ssid)) || !ssid[0])
		return RError;
	pass[0] = 0;
	if (*p == ',')
		{
		p++;
		if (!Field(p, pass, sizeof(pass)))
			return RError;
		}
	if (*p)
		return RError;
	size_t pl = strlen(pass);
	if (pl && pl < 8)
		return RError;
	memcpy(iS.ssid, ssid, sizeof(iS.ssid));
	memcpy(iS.pass, pass, sizeof(iS.pass));
	uint32_t now = iHal.Millis();
	iJoinMs = now ? now : 1;
	iJoinFail = EWifiFailNone;
	iWifiDownMs = 0;                         // (the rejoin timer starts again from now)
	iWifiRetries = 0;
	snprintf(iPinned, sizeof(iPinned), "%s", iS.ssid);   // (picked by hand: kept while in range)
	iHal.WifiBegin(iS.ssid, iS.pass);
	return ROk;
	}

// AT$STAT?: the WiFi and the link, one +STAT:NAME=value line each, for a
// program to read (ATI is the same for people):
//   VER, BOARD   the firmware's version and the board
//   SSID         the network set (quoted, as AT$SCAN)
//   WIFI         DOWN, JOINING (within 40 s of AT$JOIN), JOINED or INTERNET
//   CUR, IP, RSSI  the network joined now, its address and signal (joined only)
//   NET          the Internet check: 1 passed, 0 failed, -1 not known
//   CHK          what the check connects to ("" = no check)
//   FAIL         why joining is not working: NONE, NONET (not found),
//                PASSWORD (refused: usually a wrong password) or OTHER
//   UP           the uplink in use: WIFI, USB or NONE
//   CALL         1 while a call is up
//   BAUD, FC     the serial line
// 2.2 adds, after those (a reader skips names it does not know):
//   DNS          the Internet check's DNS lookup: 1 worked, 0 failed, -1 not known
//   NETS         how many networks are saved (AT$NETS? lists them)
//   WG           the WireGuard mode: OFF, ON or AUTO
//   WGST         its state: OFF, NOCONF (no config), NOUPLINK, HOME (Auto, at
//                home: not needed), TIME (waiting for the clock), RESOLVE,
//                CONNECTING, UP or RETRY
//   WGEP         the endpoint (quoted host:port), when a config is set
//   WGHS         seconds since the last handshake, -1 none
//   WGRX, WGTX   bytes through the tunnel since it started
// and then:
//   FW           OK; TRIAL (a firmware just written over the web, not yet
//                seen to start properly); PREVIOUS (an update failed to start
//                three times, and the modem went back to the firmware before it)
void Modem::Status()
	{
	char m[200], q[140];
	snprintf(m, sizeof(m), "+STAT:VER=%s", kVersion);
	SayLine(m);
	snprintf(m, sizeof(m), "+STAT:BOARD=%s", iHal.BoardName());
	SayLine(m);
	Quote(iS.ssid, q, sizeof(q));
	snprintf(m, sizeof(m), "+STAT:SSID=%s", q);
	SayLine(m);
	bool up = iHal.WifiUp();
	int fail = up ? EWifiFailNone : iHal.WifiFail();
	if (!up && fail == EWifiFailNone)
		fail = iJoinFail;
	int net = up ? iHal.Internet() : -1;
	const char* w;
	if (up)
		w = net == 1 ? "INTERNET" : "JOINED";
	else if (iJoinMs && iHal.Millis() - iJoinMs < 40000 && fail == EWifiFailNone)
		w = "JOINING";
	else
		w = "DOWN";
	snprintf(m, sizeof(m), "+STAT:WIFI=%s", w);
	SayLine(m);
	char cur[33], ip[20];
	int rssi = 0;
	if (up && iHal.WifiLink(cur, sizeof(cur), ip, sizeof(ip), rssi))
		{
		Quote(cur, q, sizeof(q));
		snprintf(m, sizeof(m), "+STAT:CUR=%s", q);
		SayLine(m);
		snprintf(m, sizeof(m), "+STAT:IP=%s", ip);
		SayLine(m);
		snprintf(m, sizeof(m), "+STAT:RSSI=%d", rssi);
		SayLine(m);
		}
	snprintf(m, sizeof(m), "+STAT:NET=%d", net);
	SayLine(m);
	Quote(iS2.chkHost, q, sizeof(q));
	snprintf(m, sizeof(m), "+STAT:CHK=%s", q);
	SayLine(m);
	static const char* const kFail[] = { "NONE", "NONET", "PASSWORD", "OTHER" };
	snprintf(m, sizeof(m), "+STAT:FAIL=%s", kFail[fail >= 0 && fail <= 3 ? fail : 3]);
	SayLine(m);
	int act = iUplink.Active();
	snprintf(m, sizeof(m), "+STAT:UP=%s", act == EActiveUsb ? "USB" : act == EActiveWifi ? "WIFI" : "NONE");
	SayLine(m);
	snprintf(m, sizeof(m), "+STAT:CALL=%d", iConnected ? 1 : 0);
	SayLine(m);
	snprintf(m, sizeof(m), "+STAT:BAUD=%lu", (unsigned long)iS.baud);
	SayLine(m);
	snprintf(m, sizeof(m), "+STAT:FC=%d", iS2.flow ? 1 : 0);
	SayLine(m);
	// 2.2 (appended: an older reader skips names it does not know)
	snprintf(m, sizeof(m), "+STAT:DNS=%d", up ? iHal.DnsOk() : -1);
	SayLine(m);
	snprintf(m, sizeof(m), "+STAT:NETS=%u", (unsigned)iNets.count);
	SayLine(m);
	static const char* const kModes[] = { "OFF", "ON", "AUTO" };
	snprintf(m, sizeof(m), "+STAT:WG=%s", kModes[iS2.wgMode <= 2 ? iS2.wgMode : 0]);
	SayLine(m);
	const char* st;
	if (iWgWant == EWgWantOff) st = "OFF";
	else if (iWgWant == EWgWantNoConfig) st = "NOCONF";
	else if (iWgWant == EWgWantNoUplink) st = "NOUPLINK";
	else if (iWgWant == EWgWantHome) st = "HOME";
	else
		{
		switch (iWgStatus.state)
			{
		case EWgStTime: st = "TIME"; break;
		case EWgStResolve: st = "RESOLVE"; break;
		case EWgStConnecting: st = "CONNECTING"; break;
		case EWgStUp: st = "UP"; break;
		default: st = "RETRY"; break;
			}
		}
	snprintf(m, sizeof(m), "+STAT:WGST=%s", st);
	SayLine(m);
	if (WgValid(iWg))
		{
		char ep[80];
		snprintf(ep, sizeof(ep), "%s:%u", iWg.endpoint, (unsigned)iWg.port);
		Quote(ep, q, sizeof(q));
		snprintf(m, sizeof(m), "+STAT:WGEP=%s", q);
		SayLine(m);
		}
	snprintf(m, sizeof(m), "+STAT:WGHS=%ld", (long)(iWgWant == EWgWantUp ? iWgStatus.handshakeAge : -1));
	SayLine(m);
	snprintf(m, sizeof(m), "+STAT:WGRX=%lu", (unsigned long)iWgStatus.rx);
	SayLine(m);
	snprintf(m, sizeof(m), "+STAT:WGTX=%lu", (unsigned long)iWgStatus.tx);
	SayLine(m);
	static const char* const kFw[] = { "OK", "TRIAL", "PREVIOUS" };
	int fw = iHal.FirmwareInfo(q, sizeof(q));
	snprintf(m, sizeof(m), "+STAT:FW=%s", kFw[fw >= 0 && fw <= 2 ? fw : 0]);
	SayLine(m);
	}


// ----- 2.2: the saved networks -----------------------------------------------------

// Scan and join the best saved network in range: one picked by hand while
// it is in range, else the strongest saved one not yet failed this time
// round (aStart: the first choice at power-on, nothing has failed yet)
void Modem::ChooseNet(uint32_t aNow, bool aStart)
	{
	static WifiNet scan[24];
	int n = iHal.WifiScanFull(scan, 24);
	if (n < 0) n = 0;
	if (n > 24) n = 24;
	char m[128];
	if (iPinned[0])
		{
		bool seen = false;
		for (int i = 0; i < n; i++)
			if (strcmp(scan[i].ssid, iPinned) == 0)
				seen = true;
		if (seen)
			{
			int k = NetFind(iNets, iPinned);
			if (strcmp(iS.ssid, iPinned) != 0 && k >= 0)
				{
				memcpy(iS.ssid, iNets.net[k].ssid, sizeof(iS.ssid));
				memcpy(iS.pass, iNets.net[k].pass, sizeof(iS.pass));
				}
			snprintf(m, sizeof(m), "WiFi: joining \"%s\" again (picked by hand)", iS.ssid);
			iHal.Log(m);
			iHal.WifiBegin(iS.ssid, iS.pass);
			return;
			}
		snprintf(m, sizeof(m), "WiFi: \"%s\" (picked by hand) is out of range: choosing again", iPinned);
		iHal.Log(m);
		iPinned[0] = 0;
		}
	if (!aStart && iNetTrying >= 0 && iNetTrying < iNets.count)
		iNetSkip |= 1u << iNetTrying;          // the last one tried did not work
	int rssi = 0;
	int i = NetChoose(iNets, scan, n, 0, iNetSkip, &rssi);
	if (i < 0 && iNetSkip)
		{
		iNetSkip = 0;                        // all tried: round again
		i = NetChoose(iNets, scan, n, 0, 0, &rssi);
		}
	if (i < 0)
		{
		iNetTrying = -1;
		snprintf(m, sizeof(m), "WiFi: none of the %u saved networks is in range (%d seen)", (unsigned)iNets.count, n);
		iHal.Log(m);
		if (iS.ssid[0])
			iHal.WifiBegin(iS.ssid, iS.pass);  // (it may yet appear: hidden, or a slow hotspot)
		return;
		}
	iNetTrying = i;
	memcpy(iS.ssid, iNets.net[i].ssid, sizeof(iS.ssid));
	memcpy(iS.pass, iNets.net[i].pass, sizeof(iS.pass));
	snprintf(m, sizeof(m), "WiFi: the strongest saved network in range is \"%s\" (%d dBm, %d of %u)", iS.ssid, rssi,
		i + 1, (unsigned)iNets.count);
	iHal.Log(m);
	iHal.WifiBegin(iS.ssid, iS.pass);
	(void)aNow;
	}

bool Modem::SwitchNet(int aIndex)
	{
	if (aIndex < 0 || aIndex >= iNets.count)
		return false;
	memcpy(iS.ssid, iNets.net[aIndex].ssid, sizeof(iS.ssid));
	memcpy(iS.pass, iNets.net[aIndex].pass, sizeof(iS.pass));
	snprintf(iPinned, sizeof(iPinned), "%s", iS.ssid);
	uint32_t now = iHal.Millis();
	iJoinMs = now ? now : 1;
	iJoinFail = EWifiFailNone;
	iWifiDownMs = 0;
	iWifiRetries = 0;
	iNetTrying = aIndex;
	char m[96];
	snprintf(m, sizeof(m), "WiFi: switching to \"%s\" (picked by hand)", iS.ssid);
	iHal.Log(m);
	iHal.WifiBegin(iS.ssid, iS.pass);
	iWgGen++;                                // (Auto decides again on the new network)
	return true;
	}

bool Modem::NextNet()
	{
	if (iNets.count < 2 && !(iNets.count == 1 && NetFind(iNets, iS.ssid) < 0))
		return false;
	static WifiNet scan[24];
	int n = iConnected || iClosing ? 0 : iHal.WifiScanFull(scan, 24);   // (no scan during a call)
	if (n < 0) n = 0;
	int j = NetNext(iNets, NetFind(iNets, iS.ssid), scan, n);
	return j >= 0 && SwitchNet(j);
	}

bool Modem::AddNet(const char* aSsid, const char* aPass, bool aSave)
	{
	size_t pl = aPass ? strlen(aPass) : 0;
	if (!aSsid || !aSsid[0] || strlen(aSsid) > 32 || pl > 64 || (pl && pl < 8))
		return false;
	bool evicted = false;
	int keep = NetFind(iNets, iS.ssid);
	if (NetUpsert(iNets, aSsid, aPass, keep, &evicted) < 0)
		return false;
	if (strcmp(aSsid, iS.ssid) == 0)
		{
		memset(iS.pass, 0, sizeof(iS.pass));
		strncpy(iS.pass, aPass ? aPass : "", sizeof(iS.pass) - 1);
		}
	char m[96];
	snprintf(m, sizeof(m), "WiFi: \"%s\" saved%s", aSsid, evicted ? " (the last saved one was dropped)" : "");
	iHal.Log(m);
	if (aSave)
		SaveNetsNow();
	return true;
	}

bool Modem::ForgetNet(int aIndex, bool aSave)
	{
	if (aIndex < 0 || aIndex >= iNets.count)
		return false;
	char name[33];
	memcpy(name, iNets.net[aIndex].ssid, sizeof(name));
	NetForget(iNets, aIndex);
	iNetSkip = 0;
	iNetTrying = -1;
	char m[96];
	snprintf(m, sizeof(m), "WiFi: \"%s\" forgotten", name);
	iHal.Log(m);
	if (strcmp(name, iPinned) == 0)
		iPinned[0] = 0;
	if (strcmp(name, iS.ssid) == 0)
		{
		// the one it is on: leave it, as a phone does, and choose another
		memset(iS.ssid, 0, sizeof(iS.ssid));
		memset(iS.pass, 0, sizeof(iS.pass));
		if (iConnected && !iPppCall && iUplink.Active() == EActiveWifi)
			Hangup(false);
		iHal.WifiEnd();
		iJoinMs = 0;                         // (a join of it that was under way is over: choose at once)
		uint32_t now = iHal.Millis();
		iWifiDownMs = now ? now : 1;
		iWifiRetryMs = now + 1000;           // (choose again in a moment)
		if (aSave)
			iHal.SaveSettings(iS);
		}
	if (aSave)
		SaveNetsNow();
	return true;
	}

// an index (1-based, as AT$NETS? lists them) or a quoted name; -1 if neither
int Modem::NetArg(const char* aArg) const
	{
	const char* p = aArg;
	while (*p == ' ') p++;
	if (*p >= '0' && *p <= '9')
		{
		char* end;
		long v = strtol(p, &end, 10);
		while (*end == ' ') end++;
		if (*end || v < 1 || v > iNets.count)
			return -1;
		return (int)v - 1;
		}
	char name[33];
	if (!Field(p, name, sizeof(name)) || *p)
		return -1;
	return NetFind(iNets, name);
	}

// AT$NETS?: the saved networks, in their order, one line each:
//   +NET:<n>,"<name>",<1 has a password | 0 open>,<CUR joined now | SEL being joined | ->[,PIN]
void Modem::ListNets()
	{
	char q[140], m[200];
	bool up = iHal.WifiUp();
	for (int i = 0; i < iNets.count; i++)
		{
		Quote(iNets.net[i].ssid, q, sizeof(q));
		bool cur = strcmp(iNets.net[i].ssid, iS.ssid) == 0;
		snprintf(m, sizeof(m), "+NET:%d,%s,%d,%s%s", i + 1, q, iNets.net[i].pass[0] ? 1 : 0,
			cur ? (up ? "CUR" : "SEL") : "-", strcmp(iNets.net[i].ssid, iPinned) == 0 ? ",PIN" : "");
		SayLine(m);
		}
	}

void Modem::ShowSchema()
	{
	char m[200];
	for (int i = 0; i < SchemaCount(); i++)
		{
		SchemaLine(i, m, sizeof(m));
		SayLine(m);
		}
	}

// ----- 2.2: WireGuard ------------------------------------------------------------------

void Modem::SetWgMode(int aMode, bool aSave)
	{
	if (aMode < EWgModeOff || aMode > EWgModeAuto)
		return;
	iS2.wgMode = (uint8_t)aMode;
	iWgGen++;
	if (aSave)
		iHal.SaveSettings2(iS2);
	}

int Modem::CycleWgMode()
	{
	int next = iS2.wgMode == EWgModeOff ? EWgModeOn : iS2.wgMode == EWgModeOn ? EWgModeAuto : EWgModeOff;
	SetWgMode(next, true);
	static const char* const kNames[] = { "off", "on", "auto" };
	char m[48];
	snprintf(m, sizeof(m), "WireGuard: %s (the button)", kNames[next]);
	iHal.Log(m);
	return next;
	}

bool Modem::SetWgConfig(const char* aText, char* aWhy, size_t aWhyMax)
	{
	static WgConfig c;
	if (!WgParse(aText, c, aWhy, aWhyMax))
		{
		char m[96];
		snprintf(m, sizeof(m), "WireGuard: config not taken: %s", aWhy);
		iHal.Log(m);
		return false;
		}
	iWg = c;
	WgClear(c);
	if (!iHal.SaveWg(iWg))
		{
		snprintf(aWhy, aWhyMax, "Could not save it");
		return false;
		}
	iWgGen++;
	char ep[96];
	snprintf(ep, sizeof(ep), "WireGuard: config saved (endpoint %s:%u, %u AllowedIPs)", iWg.endpoint,
		(unsigned)iWg.port, (unsigned)iWg.nAllowed);
	iHal.Log(ep);
	return true;
	}

void Modem::ClearWgConfig()
	{
	WgClear(iWg);
	iHal.SaveWg(iWg);
	iWgGen++;
	iHal.Log("WireGuard: config deleted");
	}

static const char* WgStateWord(int aState)
	{
	switch (aState)
		{
	case EWgStTime: return "waiting for the time";
	case EWgStResolve: return "looking up the endpoint";
	case EWgStConnecting: return "connecting";
	case EWgStUp: return "up";
	case EWgStRetry: return "retrying";
	default: return "down";
		}
	}

void Modem::WgStateText(char* aOut, size_t aMax) const
	{
	if (iWgWant != EWgWantUp)
		{
		snprintf(aOut, aMax, "%s", iWgWant == EWgWantHome ? "not needed: at home" : iWgWant == EWgWantNoConfig
			? "no config" : iWgWant == EWgWantNoUplink ? "waiting for a network" : "off");
		return;
		}
	if (iWgStatus.state == EWgStUp && iWgStatus.handshakeAge >= 0)
		snprintf(aOut, aMax, "up, handshake %ld s ago", (long)iWgStatus.handshakeAge);
	else if (iWgStatus.why[0])
		snprintf(aOut, aMax, "%s (%s)", WgStateWord(iWgStatus.state), iWgStatus.why);
	else
		snprintf(aOut, aMax, "%s", WgStateWord(iWgStatus.state));
	}

// once a second: should the tunnel be up, tell the board when that changes,
// read how it is doing, and keep the DNS relay pointed at the right server
void Modem::WgTick(uint32_t aNow)
	{
	if (iWgTickMs && aNow - iWgTickMs < 1000 && iWgGen == iWgAppliedGen)
		return;
	iWgTickMs = aNow ? aNow : 1;
	uint32_t ip = 0, mask = 0;
	bool have = iHal.UplinkAddr(ip, mask) && iUplink.Up();
	TWgWant w = WgDecide(iS2.wgMode, iWg, have, ip, mask);
	bool restart = w == EWgWantUp && iWgWant == EWgWantUp && ip != iWgIp && iWgIp;
	if (w != iWgWant || iWgGen != iWgAppliedGen || restart || !iWgApplied)
		{
		bool logIt = w != iWgWant || restart;
		if (restart)
			iHal.WgApply(iWg, false);           // a new uplink address: start again (a new config restarts on the board)
		iWgWant = w;
		iWgAppliedGen = iWgGen;
		iWgApplied = true;
		iWgIp = ip;
		iHal.WgApply(iWg, w == EWgWantUp);
		if (logIt)
			{
			char m[96];
			snprintf(m, sizeof(m), "WireGuard: %s%s", w == EWgWantUp ? "starting" : WgWantName(w),
				restart ? " (the uplink's address changed)" : "");
			iHal.Log(m);
			}
		}
	int dns = iHal.WifiUp() ? iHal.DnsOk() : -1;
	if (dns != iLastDnsOk)
		{
		if (dns == 0)
			iHal.Log("Internet check: a DNS lookup failed (the Internet is not counted as working)");
		else if (dns == 1 && iLastDnsOk == 0)
			iHal.Log("Internet check: DNS lookups work again");
		iLastDnsOk = dns;
		}
	iHal.WgInfo(iWgStatus);
	if (iWgStatus.state != iWgLastState)
		{
		if (iWgLastState >= 0 && (iWgStatus.state == EWgStUp || iWgLastState == EWgStUp))
			{
			char m[96], t[64];
			WgStateText(t, sizeof(t));
			snprintf(m, sizeof(m), "WireGuard: %s", t);
			iHal.Log(m);
			}
		iWgLastState = iWgStatus.state;
		}
	uint32_t up = DnsUpstream(iWgStatus.state == EWgStUp, iWg.nDns ? iWg.dns[0] : 0, iHal.UplinkDns());
	if (up != iDnsUp)
		{
		iDnsUp = up;
		iHal.DnsRelayUpstream(up);
		}
	}

// ----- to the Psion -----------------------------------------------------------

void Modem::Send(const uint8_t* aData, size_t aLen)
	{
	if (iCap)
		{
		// RemoteLine: into its buffer, keeping room for the result line
		static const size_t kReserve = 32;
		size_t limit = iInResult || iCapMax <= kReserve ? iCapMax : iCapMax - kReserve;
		size_t n = iCapLen < limit ? limit - iCapLen : 0;
		if (n > aLen)
			n = aLen;
		memcpy(iCap + iCapLen, aData, n);
		iCapLen += n;
		if (n < aLen)
			iCapCut = true;
		return;
		}
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
	if (aR == RConnect)
		strcpy(iLastDialResult, "CONNECT");
	else if (aR == RNoCarrier)
		strcpy(iLastDialResult, "NO CARRIER");
	if (iRemote && aR != RNone)
		{
		// over PPP: always in words, after a line saying the answer was cut short
		iInResult = true;
		if (iCapCut)
			Say("\r\n+TRUNCATED");
		Say(aR == ROk ? "\r\nOK\r\n" : aR == RNoCarrier ? "\r\nNO CARRIER\r\n" : "\r\nERROR\r\n");
		iInResult = false;
		return;
		}
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
