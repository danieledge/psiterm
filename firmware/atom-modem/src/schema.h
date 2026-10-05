// schema.h - one table of every setting the modem has: its AT$ name, its
// type and range, where it lives in the two records, what has to be
// re-applied after it changes, and a line of help. The AT$ parser
// (modem.cpp) and the web pages (board/webui.cpp) both work from this
// table, so a setting cannot exist on one and not the other, and the host
// tests walk it. Plain C++, no Arduino calls.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_SCHEMA_H
#define ATOM_SCHEMA_H

#include "settings.h"

namespace am {

enum TSchemaType
	{
	STBool,          // 0/1 in a uint8_t
	STBoolInv,       // 0/1 shown, stored inverted (AT$PZ: proxyNoZip)
	STInt,           // a number in a uint8_t, uint16_t, int8_t or uint32_t field
	STEnum,          // a number with names (AT$UP=AUTO)
	STBaud,          // AT$SB: one of the known rates
	STPace,          // AT$PR: a rate, or AUTO
	STProxyMode,     // AT$PX: 0..4 through ProxyMode/SetProxyMode
	STStr,           // a string field
	STSecret,        // a string field that is never shown ("(set)" / "(none)")
	STPorts,         // AT$TLSP: a list of ports
	STPins           // AT$PINS: tx,rx,rts,cts,dcd
	};

enum TSchemaRec { SR1, SR2 };

// what the modem does after a change
enum TSchemaApply
	{
	SANone,
	SAWifi,          // join the network again (SSID, PASS)
	SABaud,          // AT$SB: the speed changes after the OK, on trial
	SAPacing,        // the pacer is set up again
	SAPaceManual,    // ... and AT$PR=AUTO is off from now on (PB, PG)
	SAPins,          // the UART's pins, flow control, the DCD pin
	SAWeb,           // the web pages and the access point
	SAUplink,        // the uplink manager
	SAUsb,           // the USB port's mode: at the next restart
	SADcd            // the DCD line is driven again
	};

enum TSchemaGroup { SGWifi, SGUplink, SGSerial, SGProxy, SGTls, SGPictures, SGExec, SGSystem };

struct SchemaEntry
	{
	const char* name;        // after "AT$", upper case
	TSchemaType type;
	TSchemaRec rec;
	uint16_t offset;         // in the record
	uint8_t size;            // of the field (1, 2, 4; strings: the buffer)
	int32_t min, max;        // STInt; STStr/STSecret: the shortest and longest value
	const char* const* names;    // STEnum: one name per value, ending with 0
	TSchemaApply apply;
	TSchemaGroup group;
	const char* label;       // for the web page
	const char* help;        // one line
	};

int SchemaCount();
const SchemaEntry& SchemaAt(int aIndex);
// by AT name, case-insensitive; 0 if unknown
const SchemaEntry* SchemaFind(const char* aName);
// the value as text (secrets as "(set)" or "(none)")
void SchemaGet(const Config& aC, const SchemaEntry& aE, char* aOut, size_t aMax);
// false: the value is not acceptable (nothing changed)
bool SchemaSet(Config& aC, const SchemaEntry& aE, const char* aValue);
// STSecret: whether one is set
bool SchemaSecretSet(const Config& aC, const SchemaEntry& aE);
const char* SchemaGroupName(TSchemaGroup aG);

bool ValidBaud(long aBaud);

} // namespace am

#endif
