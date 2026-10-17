// ext.cpp - see ext.h. MIT licence (see LICENSE at the top of the repository).
#include "ext.h"
#include "schema.h"
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

} // namespace am
