// atremote.cpp - see atremote.h. MIT licence (see LICENSE at the top of the repository).
#include "atremote.h"
#include "modem.h"
#include "schema.h"
#include <string.h>
#include <stdio.h>

namespace am {

static int Up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static void SkipNumber(const char*& p)
	{
	while (*p >= '0' && *p <= '9')
		p++;
	}

// the number at p (aDefault if none), as Modem's Number reads it
static long ReadNumber(const char*& p, long aDefault)
	{
	if (*p < '0' || *p > '9')
		return aDefault;
	long v = 0;
	while (*p >= '0' && *p <= '9')
		{
		if (v < 100000)
			v = v * 10 + (*p - '0');
		p++;
		}
	return v;
	}

static bool No(char* aWhy, size_t aWhyMax, const char* aText)
	{
	if (aWhyMax)
		snprintf(aWhy, aWhyMax, "%s", aText);
	return false;
	}

static const char kEndsLink[] = "it would end the PPP link";
static const char kLine[] = "it changes the serial line";
static const char kUplink[] = "it changes the uplink";

bool AtRemoteAllowed(const char* aCmd, char* aWhy, size_t aWhyMax)
	{
	if (aWhyMax)
		aWhy[0] = 0;
	const char* p = aCmd;
	for (;;)
		{
		while (*p == ' ')
			p++;
		if (!*p)
			return true;
		int c = Up((unsigned char)*p++);
		switch (c)
			{
		case 'H': case 'O': case 'D': case 'A':
			return No(aWhy, aWhyMax, kEndsLink);
		case 'B':
			return No(aWhy, aWhyMax, kLine);
		case 'C':
			if (ReadNumber(p, 1) == 0)
				return No(aWhy, aWhyMax, kEndsLink);       // (ATC0 leaves the WiFi and ends the call)
			break;
		case 'S':
			SkipNumber(p);
			if (*p == '=')
				return No(aWhy, aWhyMax, "it changes the escape");
			if (*p == '?')
				p++;
			break;
		case '&':
			{
			int c2 = Up((unsigned char)*p);
			if (c2)
				p++;
			SkipNumber(p);
			if (c2 == 'F' || c2 == 'C')
				return No(aWhy, aWhyMax, kLine);           // (&F: the factory speed; &C: DCD)
			break;
			}
		case '$':
			{
			// the name as SetCommand reads it; the command takes the rest of the line
			char name[16];
			size_t nl = 0;
			const char* rest = p;
			while (*rest && *rest != '=' && *rest != '?' && *rest != ' ' && nl < sizeof(name) - 1)
				name[nl++] = (char)Up((unsigned char)*rest++);
			name[nl] = 0;
			while (*rest == ' ')
				rest++;
			bool query = *rest != '=';
			if (strcmp(name, "RESET") == 0 || strcmp(name, "EXEC") == 0)
				return No(aWhy, aWhyMax, kEndsLink);
			if (query)
				return true;
			if (strcmp(name, "PPP") == 0)
				return No(aWhy, aWhyMax, kEndsLink);
			const SchemaEntry* e = SchemaFind(name);
			if (e && (e->apply == SABaud || e->apply == SAPins || e->apply == SADcd))
				return No(aWhy, aWhyMax, kLine);
			if (e && (e->apply == SAUplink || e->apply == SAUsb))
				return No(aWhy, aWhyMax, kUplink);
			return true;
			}
		case 'W':
			return true;                                    // (takes the rest of the line)
		case 'Z':
			return true;                                    // (ATZ ends the line too)
		default:
			// E V Q I L M X N P T Y and the rest: harmless here, or the modem
			// answers ERROR itself
			SkipNumber(p);
			break;
			}
		}
	}

// ----- one client ---------------------------------------------------------------

void AtRemote::Begin(uint32_t aNow)
	{
	iLineLen = 0;
	iTooLong = false;
	iLastMs = aNow;
	iOutLen = iOutPos = 0;
	}

size_t AtRemote::Input(Modem& aModem, const uint8_t* aData, size_t aLen, uint32_t aNow)
	{
	size_t i = 0;
	if (aLen)
		iLastMs = aNow;
	while (i < aLen && CanTake())
		{
		uint8_t c = aData[i++];
		if (c == '\n' || c == 0)
			continue;
		if (c != '\r')
			{
			if (iLineLen < kLineMax - 1)
				iLine[iLineLen++] = (char)c;
			else
				iTooLong = true;
			continue;
			}
		iLine[iLineLen] = 0;
		iOutPos = 0;
		if (iTooLong)
			{
			static const char kTooLong[] = "\r\n+TOOLONG\r\nERROR\r\n";
			memcpy(iOut, kTooLong, sizeof(kTooLong) - 1);
			iOutLen = sizeof(kTooLong) - 1;
			}
		else
			iOutLen = aModem.RemoteLine(iLine, iOut, sizeof(iOut));
		memset(iLine, 0, iLineLen);                         // (a password was in it, perhaps)
		iLineLen = 0;
		iTooLong = false;
		}
	return i;
	}

size_t AtRemote::Pending(const uint8_t*& aPtr) const
	{
	aPtr = (const uint8_t*)iOut + iOutPos;
	return iOutLen - iOutPos;
	}

void AtRemote::Sent(size_t aLen)
	{
	iOutPos += aLen;
	if (iOutPos >= iOutLen)
		iOutPos = iOutLen = 0;
	}

} // namespace am
