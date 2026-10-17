// ext.cpp - see ext.h. MIT licence (see LICENSE at the top of the repository).
#include "ext.h"
#include "schema.h"
#include "modem.h"
#include <stdio.h>
#include <string.h>

namespace am {

// (zero before any constructor runs: modules can register in any order)
static Extension* gFirst = 0;

Extension* ExtModules() { return gFirst; }

ExtRegister::ExtRegister(Extension& aExt)
	{
	aExt.iNextModule = 0;
	Extension** p = &gFirst;
	while (*p)
		p = &(*p)->iNextModule;
	*p = &aExt;
	}

void ExtReply::Text(int aStatus, const char* aType, const char* aText, const char* aCache)
	{
	snprintf(text, sizeof(text), "%s", aText);
	Data(aStatus, aType, text, strlen(text), false, aCache);
	}

int ExtSchemaCount()
	{
	int n = 0;
	for (Extension* e = gFirst; e; e = e->iNextModule)
		{
		int c;
		e->Schema(c);
		n += c;
		}
	return n;
	}

const SchemaEntry& ExtSchemaAt(int aIndex)
	{
	for (Extension* e = gFirst; e; e = e->iNextModule)
		{
		int c;
		const SchemaEntry* s = e->Schema(c);
		if (aIndex < c)
			return s[aIndex];
		aIndex -= c;
		}
	static const SchemaEntry kNone = {};     // (never: callers stay below the count)
	return kNone;
	}

const Extension* ExtSchemaOwner(const SchemaEntry& aE)
	{
	for (Extension* e = gFirst; e; e = e->iNextModule)
		{
		int c;
		const SchemaEntry* s = e->Schema(c);
		if (c && &aE >= s && &aE < s + c)
			return e;
		}
	return 0;
	}

void ExtSanitise(Settings2& aS2)
	{
	for (int i = 0; i < ExtSchemaCount(); i++)
		{
		const SchemaEntry& e = ExtSchemaAt(i);
		if (e.rec != SR2 || e.size != 1 || (e.type != STBool && e.type != STBoolInv))
			continue;
		uint8_t* p = (uint8_t*)&aS2 + e.offset;
		if (*p > 1)
			*p = 0;
		}
	}

// ----- the LCD and the LED ------------------------------------------------------------

// how strong a module's light must be to show over each of the modem's
// states (0: never)
static int OverNeeded(int aModemLed)
	{
	switch (aModemLed)
		{
	case ELedWifi:       return EExtLedOverIdle;
	case ELedConnected:
	case ELedData:       return EExtLedOverCall;
	case ELedConnecting: return EExtLedOverDial;
	default:             return 0;      // no Wi-Fi, the access point, none yet
		}
	}

bool ExtLedPick(const Modem& aModem, int aModemLed, bool aBusy, ExtLed& aOut)
	{
	int need = OverNeeded(aModemLed);
	if (aBusy || !need)
		return false;
	bool have = false;
	for (int i = 0; i < aModem.ExtensionCount(); i++)
		{
		ExtLed l;
		if (!aModem.ExtensionAt(i)->LedWanted(l))
			continue;
		if (!have || l.over > aOut.over)
			{
			aOut = l;
			have = true;
			}
		}
	return have && aOut.over >= need;
	}

void ExtLedLevel(const ExtLed& aLed, uint32_t aNowMs, uint8_t& aR, uint8_t& aG, uint8_t& aB)
	{
	int level = 256;                          // of 256
	uint32_t period = aLed.periodMs ? aLed.periodMs : 1000;
	uint32_t t = aNowMs % period;
	if (aLed.style == EExtLedPulse)
		{
		// a triangle from an eighth up to full and back: never quite out, so
		// it reads as one light breathing rather than a blink
		uint32_t half = period / 2 ? period / 2 : 1;
		uint32_t up = t < half ? t : period - t;
		level = 32 + (int)(224 * (up > half ? half : up) / half);
		}
	else if (aLed.style == EExtLedBlink)
		level = t < period / 2 ? 256 : 0;
	aR = (uint8_t)(aLed.r * level / 256);
	aG = (uint8_t)(aLed.g * level / 256);
	aB = (uint8_t)(aLed.b * level / 256);
	}

Extension* ExtPagePick(const Modem& aModem)
	{
	for (int i = 0; i < aModem.ExtensionCount(); i++)
		if (aModem.ExtensionAt(i)->HasPage())
			return aModem.ExtensionAt(i);
	return 0;
	}

Extension* ExtScreenPick(const Modem& aModem)
	{
	for (int i = 0; i < aModem.ExtensionCount(); i++)
		if (aModem.ExtensionAt(i)->ScreenWanted())
			return aModem.ExtensionAt(i);
	return 0;
	}

} // namespace am
