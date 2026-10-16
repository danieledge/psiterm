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
	SADcd,           // the DCD line is driven again
	SAWg             // the WireGuard tunnel decides again (2.2)
	};

enum TSchemaGroup { SGWifi, SGUplink, SGSerial, SGProxy, SGTls, SGMail, SGPictures, SGExec, SGSystem, SGWg };
static const int kSchemaGroups = 10;

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

// AT$SCHEMA? (2.2): the table for a program (PsiConnect builds its settings
// pages from it). Flags: 'L' a serial line setting (a wrong value can cut
// the Psion off: applied after the OK, and undone if no command is heard
// with it within 15 s), 'R' takes effect at the next restart, 'S' a secret
// (never shown)
void SchemaFlags(const SchemaEntry& aE, char* aOut, size_t aMax);
// "BOOL", "INT", "ENUM:OFF|ON|AUTO", "STR", "SECRET", "BAUD", "PACE",
// "PROXY", "PORTS", "PINS"
void SchemaTypeText(const SchemaEntry& aE, char* aOut, size_t aMax);
// the web page that shows a setting ("/wifi", "/settings" or "/vpn"): every
// entry is on exactly one, and the host test checks it (webui.cpp draws each
// page from this, so a new setting cannot be left off the web)
const char* SchemaWebPage(const SchemaEntry& aE);
// one +SCHEMA: line for entry aIndex (without CR LF)
void SchemaLine(int aIndex, char* aOut, size_t aMax);

bool ValidBaud(long aBaud);

} // namespace am

#endif
