// modem.cpp - see modem.h. MIT licence (see LICENSE at the top of the repository).
#include "modem.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace am {

// ===== settings ============================================================

void AutoPacing(uint32_t aBaud, uint32_t& aRate, uint16_t& aBurst, uint16_t& aGap)
	{
	// The Psion 5mx's UART (CL-PS7111) has a 16-byte receive FIFO and the
	// apps give the serial driver a 16 KB buffer (SetReceiveBufferLength).
	// Bursts of 16 bytes can never overrun the FIFO even if its interrupt
	// is held off for a whole burst; the average rate is held well below
	// the line rate so the gaps between bursts let the driver catch up,
	// and a 16 KB buffer lasts 3-4 s if the app stops reading for a while
	// (a page being laid out, a flash write). See README "Pacing".
	aGap = 0;
	aBurst = 16;
	if (aBaud >= 115200)      aRate = 5500;   // 48 % of 11520 bytes/s
	else if (aBaud >= 57600)  aRate = 4000;   // 69 % of 5760
	else if (aBaud >= 38400)  aRate = 3000;   // 78 % of 3840
	else                      aRate = 0;      // slow enough as it is
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
	AutoPacing(aS.baud, aS.paceRate, aS.paceBurst, aS.paceGap);
	aS.swapPins = 0;
	aS.dcdPin = -1;
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

static bool ValidBaud(long aB)
	{
	static const long kBauds[] = { 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
	for (size_t i = 0; i < sizeof(kBauds) / sizeof(kBauds[0]); i++)
		if (kBauds[i] == aB)
			return true;
	return false;
	}

Modem::Modem(Hal& aHal, uint8_t* aRing, size_t aRingSize)
	: iHal(aHal), iLineLen(0), iOnline(false), iConnected(false), iClosing(false), iWifiWasUp(false),
	  iLastSerialMs(0), iPluses(0), iLastPlusMs(0), iToPsion(0), iToServer(0), iLastDataMs(0),
	  iPendingBaud(0), iLed(-1), iUpLen(0)
	{
	iRing.Init(aRing, aRingSize);
	iLast[0] = 0;
	FactoryDefaults(iS);
	}

void Modem::Begin()
	{
	Settings s;
	if (iHal.LoadSettings(s) && s.magic == kMagic)
		iS = s;
	else
		FactoryDefaults(iS);
	iHal.ApplyPins(iS);
	iHal.SerialBaud(iS.baud);
	ApplyPacing();
	if (iS.ssid[0])
		iHal.WifiBegin(iS.ssid, iS.pass);
	iLastSerialMs = iHal.Millis();
	UpdateDcd();
	UpdateLed(iHal.Millis());
	}

void Modem::ApplyPacing()
	{
	if (iS.paceAuto)
		AutoPacing(iS.baud, iS.paceRate, iS.paceBurst, iS.paceGap);
	iPacer.Configure(iS.paceRate, iS.paceBurst, iS.paceGap);
	}

void Modem::Loop()
	{
	uint32_t now = iHal.Millis();
	int c;
	for (int n = 0; n < 512 && (c = iHal.SerialRead()) >= 0; n++)
		SerialIn((uint8_t)c);
	EscapeTick(iHal.Millis());
	FlushUp();
	PumpServer(now);
	PumpPsion(now);
	if (iPendingBaud && iHal.SerialWritable() > 0)
		{
		iHal.SerialBaud(iPendingBaud);          // (the HAL lets the OK go out first)
		iPendingBaud = 0;
		}
	UpdateDcd();
	UpdateLed(now);
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
	size_t n = iHal.TcpWrite(iUpBuf, iUpLen);
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
			TResult r = RunCommands(iLast);
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
	TResult r = RunCommands(iLast);
	if (r != RNone)
		Result(r);
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
			Settings s;
			uint32_t oldBaud = iS.baud;
			if (iHal.LoadSettings(s) && s.magic == kMagic)
				iS = s;
			else
				FactoryDefaults(iS);
			iHal.ApplyPins(iS);
			ApplyPacing();
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
				{
				// factory settings, but keep the WiFi network (AT$SSID/AT$PASS)
				Settings f;
				FactoryDefaults(f);
				memcpy(f.ssid, iS.ssid, sizeof(f.ssid));
				memcpy(f.pass, iS.pass, sizeof(f.pass));
				uint32_t oldBaud = iS.baud;
				iS = f;
				iHal.ApplyPins(iS);
				ApplyPacing();
				if (iS.baud != oldBaud)
					iPendingBaud = iS.baud;
				break;
				}
			case 'W':
				if (!iHal.SaveSettings(iS))
					return RError;
				break;
			case 'C': iS.dcdMode = v ? 1 : 0; break;
			case 'V':
				{
				char m[96];
				snprintf(m, sizeof(m), "E%d V%d Q%d &C%d S2=%d S12=%d baud %lu swap %d dcd pin %d",
					iS.echo, iS.verbose, iS.quiet, iS.dcdMode, iS.s2, iS.s12,
					(unsigned long)iS.baud, iS.swapPins, iS.dcdPin);
				SayLine(m);
				snprintf(m, sizeof(m), "pacing %s%lu bytes/s, burst %u, gap %u ms",
					iS.paceAuto ? "auto " : "", (unsigned long)iS.paceRate, iS.paceBurst, iS.paceGap);
				SayLine(m);
				snprintf(m, sizeof(m), "SSID \"%s\"", iS.ssid);
				SayLine(m);
				break;
				}
			case 'D': case 'K': case 'S': case 'B': case 'N': case 'Q': case 'R': case 'Y':
				break;                       // accepted (no DTR, no RTS/CTS on this board)
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
			iS.baud = (uint32_t)v;
			ApplyPacing();
			iPendingBaud = iS.baud;
			break;
		case 'L': case 'M': case 'X': case 'N': case 'P': case 'T': case 'Y':
			Number(p, 0);                    // speaker, result set, pulse/tone: accepted
			break;
		default:
			return RError;
			}
		}
	}

// AT$...: the WiFi and Psion settings (WiFi232 style). Each takes the rest
// of the line. AT&W saves them.
Modem::TResult Modem::SetCommand(const char* aCmd, const char*& aP, bool& aHandled)
	{
	static const char* const kNames[] = { "SSID", "PASS", "SB", "PR", "PB", "PG", "SWAP", "DCD", "PACE", 0 };
	int which = -1;
	size_t len = 0;
	for (int i = 0; kNames[i]; i++)
		{
		size_t l = strlen(kNames[i]);
		if (StartsNoCase(aCmd, kNames[i]) && (aCmd[l] == '=' || aCmd[l] == '?' || aCmd[l] == 0) && l > len)
			{
			which = i;
			len = l;
			}
		}
	aP = aCmd + strlen(aCmd);                // these commands end the line
	if (which < 0)
		return RError;
	aHandled = true;
	const char* rest = aCmd + len;
	bool query = *rest != '=';
	if (*rest == '=')
		rest++;
	char m[100];
	char val[70];
	Value(rest, val, sizeof(val));
	const char* q = val;
	long v = Number(q, -99999);
	bool num = v != -99999 && *q == 0;
	switch (which)
		{
	case 0:                                  // SSID
		if (query) { snprintf(m, sizeof(m), "%s", iS.ssid); SayLine(m); return ROk; }
		strncpy(iS.ssid, val, sizeof(iS.ssid) - 1);
		iS.ssid[sizeof(iS.ssid) - 1] = 0;
		return ROk;
	case 1:                                  // PASS (never shown)
		if (query) { SayLine(iS.pass[0] ? "(set)" : "(none)"); return ROk; }
		strncpy(iS.pass, val, sizeof(iS.pass) - 1);
		iS.pass[sizeof(iS.pass) - 1] = 0;
		if (iS.ssid[0])
			iHal.WifiBegin(iS.ssid, iS.pass);  // join once both are known
		return ROk;
	case 2:                                  // SB: serial baud
		if (query) { snprintf(m, sizeof(m), "%lu", (unsigned long)iS.baud); SayLine(m); return ROk; }
		if (!num || !ValidBaud(v))
			return RError;
		iS.baud = (uint32_t)v;
		ApplyPacing();
		iPendingBaud = iS.baud;              // after the OK, at the old speed
		return ROk;
	case 3:                                  // PR: pacing rate, bytes/s; AUTO; 0 = off
		if (query)
			{
			snprintf(m, sizeof(m), "%s%lu", iS.paceAuto ? "AUTO " : "", (unsigned long)iS.paceRate);
			SayLine(m);
			return ROk;
			}
		if (StartsNoCase(val, "AUTO"))
			iS.paceAuto = 1;
		else if (num && v >= 0 && v <= 200000)
			{
			iS.paceAuto = 0;
			iS.paceRate = (uint32_t)v;
			}
		else
			return RError;
		ApplyPacing();
		return ROk;
	case 4:                                  // PB: burst bytes
	case 5:                                  // PG: gap ms
		if (query)
			{
			snprintf(m, sizeof(m), "%u", which == 4 ? iS.paceBurst : iS.paceGap);
			SayLine(m);
			return ROk;
			}
		if (!num || v < (which == 4 ? 1 : 0) || v > 4096)
			return RError;
		iS.paceAuto = 0;
		if (which == 4) iS.paceBurst = (uint16_t)v; else iS.paceGap = (uint16_t)v;
		ApplyPacing();
		return ROk;
	case 6:                                  // SWAP: RX and TX the other way round
		if (query) { SayLine(iS.swapPins ? "1" : "0"); return ROk; }
		if (!num || v < 0 || v > 1)
			return RError;
		iS.swapPins = (uint8_t)v;
		iHal.ApplyPins(iS);
		return ROk;
	case 7:                                  // DCD: the GPIO for an emulated DCD
		if (query) { snprintf(m, sizeof(m), "%d", iS.dcdPin); SayLine(m); return ROk; }
		if (!num || v < -1 || v > 39)
			return RError;
		iS.dcdPin = (int8_t)v;
		iHal.ApplyPins(iS);
		UpdateDcd();
		return ROk;
	case 8:                                  // PACE?: the whole pacing
		if (iS.paceRate)
			snprintf(m, sizeof(m), "%s%lu bytes/s, burst %u, gap %u ms, buffer %lu",
				iS.paceAuto ? "auto: " : "", (unsigned long)iS.paceRate, iS.paceBurst, iS.paceGap,
				(unsigned long)iRing.Size());
		else
			snprintf(m, sizeof(m), "off, buffer %lu", (unsigned long)iRing.Size());
		SayLine(m);
		return ROk;
		}
	return RError;
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
		bool digits = true;
		for (const char* q = target; *q; q++)
			if ((*q < '0' || *q > '9') && *q != ',' && *q != '-')
				digits = false;
		if (digits)
			return RNoCarrier;               // a phone number (e.g. 777 for PPP): no PPP here
		}
	if (!iHal.WifiUp())
		return RNoCarrier;
	if (iConnected || iClosing)
		Hangup(false);
	iHal.Led(ELedConnecting);
	iLed = ELedConnecting;
	if (!iHal.TcpConnect(target, (uint16_t)port))
		return RNoCarrier;
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

void Modem::Hangup(bool aSayNoCarrier)
	{
	iHal.TcpClose();
	iConnected = false;
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
	char m[120];
	snprintf(m, sizeof(m), "Atom modem %s (Psion-tuned)", kVersion);
	SayLine(m);
	char w[90];
	if (iHal.WifiUp())
		{
		iHal.WifiInfo(w, sizeof(w));
		snprintf(m, sizeof(m), "WiFi: %s", w);
		}
	else
		snprintf(m, sizeof(m), "WiFi: not connected%s%s%s", iS.ssid[0] ? " (\"" : "", iS.ssid, iS.ssid[0] ? "\")" : "");
	SayLine(m);
	if (iS.paceRate)
		snprintf(m, sizeof(m), "Serial %lu baud, no flow control, pacing %lu bytes/s in %u-byte bursts",
			(unsigned long)iS.baud, (unsigned long)iS.paceRate, iS.paceBurst);
	else
		snprintf(m, sizeof(m), "Serial %lu baud, no flow control, no pacing", (unsigned long)iS.baud);
	SayLine(m);
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
	if (iConnected)
		{
		// read only what the ring can hold: when it is full the socket is
		// left alone, its window closes, and the server waits (TCP flow
		// control standing in for the RTS/CTS this board does not have)
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
		if ((!iHal.TcpOpen() && iHal.TcpAvailable() == 0) || !iHal.WifiUp())
			{
			// the server closed, or the WiFi went: say NO CARRIER once what
			// has arrived has been delivered
			iHal.TcpClose();
			iConnected = false;
			iClosing = true;
			}
		}
	if (iClosing && (iRing.Count() == 0 || !iOnline))
		{
		iClosing = false;
		iOnline = false;
		iRing.Clear();
		iPluses = 0;
		iUpLen = 0;
		UpdateDcd();
		Result(RNoCarrier);
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
	bool up = iHal.WifiUp();
	int s;
	if (iConnected || iClosing)
		s = (aNow - iLastDataMs < 60) ? ELedData : ELedConnected;
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
