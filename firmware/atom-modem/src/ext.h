// ext.h - extension modules: optional parts of the firmware that live
// outside this tree and are built in only when present (tools/ext.py for
// the board, hosttest/sources.mk for the PC). A module is a directory with a
// module.json (its sources, include directories and defines); a link to it
// in ext/ (not part of the repository) or a path in $AM_EXT_DIRS builds it
// in, and AM_EXT=0 leaves every module out. Without one the firmware is
// complete: nothing here is needed, and nothing in the modem names a module.
//
// A module registers itself (ExtRegister, a static object in one of its
// files) and then gets, through Extension:
//   - settings rows (a schema table of its own, in Settings2's ext[] bytes:
//     AT$, AT$SCHEMA?, the web pages and the record's checks all see them);
//   - calls to the modem's own address (ATDT192.168.7.1:<port>), answered
//     here in the modem with no uplink, paced like any other call;
//   - a share of the main loop, with the address of the PPP link while a
//     PPP call is up (for listeners on the Psion's own route);
//   - lines in ATI and AT$STAT?, rows on the web status page and the LCD's
//     Call page;
//   - web routes (GET), behind the web password unless marked open.
// The Hal's Random, BigAlloc, BigFree and BigRoom are there for modules.
// Plain C++, no Arduino calls.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_EXT_H
#define ATOM_EXT_H

#include <stdint.h>
#include <stddef.h>

namespace am {

class Hal;
struct Settings2;
struct SchemaEntry;

// a web route a module serves (GET only). aOpen: served without the web
// password (for files fetched by things that cannot give one)
struct ExtRoute
	{
	const char* path;
	bool open;
	};

// the request's query arguments ("" when absent)
class ExtQuery
	{
public:
	virtual const char* Arg(const char* aName) const = 0;
protected:
	~ExtQuery() {}
	};

// the reply to a web route: a body that stays put (flash, or the module's
// own memory) or a short text made into iText
struct ExtReply
	{
	int status;
	const char* type;
	const uint8_t* body;
	size_t len;
	bool gzip;                 // the body is gzipped (Content-Encoding: gzip)
	const char* cache;         // Cache-Control, 0 for none
	char text[192];
	ExtReply() : status(404), type("text/plain"), body((const uint8_t*)"Not found\n"), len(10), gzip(false), cache(0) { text[0] = 0; }
	void Data(int aStatus, const char* aType, const void* aBody, size_t aLen, bool aGzip = false, const char* aCache = 0)
		{ status = aStatus; type = aType; body = (const uint8_t*)aBody; len = aLen; gzip = aGzip; cache = aCache; }
	void Text(int aStatus, const char* aType, const char* aText, const char* aCache = 0);
	};

class Extension
	{
public:
	Extension() : iNextModule(0) {}
	virtual ~Extension() {}
	// its name: the settings group, the call's name, the label of its rows
	virtual const char* Name() const = 0;
	// at start, before the web pages and the modem begin. False: it takes no
	// part in this run (then nothing below is called)
	virtual bool Begin(Hal& aHal) { (void)aHal; return true; }
	// its settings rows (SchemaEntry, group SGExt, in Settings2's ext[]):
	// fixed, so they can be listed before Begin
	virtual const SchemaEntry* Schema(int& aCount) const { aCount = 0; return 0; }
	// every pass of the modem's loop, with the settings as they are now
	// (AT$, the pages, ATZ, AT&F: whichever changed them)
	virtual void Apply(const Settings2& aS2) { (void)aS2; }
	// the main loop. aPppAddr: the modem's address on the PPP link while a
	// PPP call is up, else "". aListenAddr: the address its own listeners
	// take ("" every address; "127.0.0.1" on the PC)
	virtual void Loop(const char* aPppAddr, const char* aListenAddr) { (void)aPppAddr; (void)aListenAddr; }

	// ----- calls to the modem's own address (ATDT192.168.7.1:<port>) -----
	virtual bool Answers(uint16_t aPort) const { (void)aPort; return false; }
	// true: not now (aWhy, for the log, says why); the dial gives NO CARRIER
	virtual bool CallRefused(uint16_t aPort, char* aWhy, size_t aMax) { (void)aPort; (void)aWhy; (void)aMax; return false; }
	// the call starts: its id, 0 if it cannot
	virtual uint32_t CallOpen(uint16_t aPort) { (void)aPort; return 0; }
	// bytes from the Psion: how many were taken
	virtual size_t CallIn(uint32_t aId, const uint8_t* aData, size_t aLen) { (void)aId; (void)aData; return aLen; }
	// bytes to the Psion, at most aMax
	virtual size_t CallOut(uint32_t aId, uint8_t* aOut, size_t aMax) { (void)aId; (void)aOut; (void)aMax; return 0; }
	// false: it is done with the call (the modem hangs up: NO CARRIER)
	virtual bool CallAlive(uint32_t aId) const { (void)aId; return false; }
	// the call has ended (a hang-up, or after CallAlive said false)
	virtual void CallClose(uint32_t aId) { (void)aId; }
	// true: a "+++" now is data (the middle of one of its messages)
	virtual bool CallHoldsEscape(uint32_t aId) const { (void)aId; return false; }

	// ----- reports -----
	// its state in a line (ATI, after "Name: "; the web status page)
	virtual void StateText(char* aOut, size_t aMax) const { if (aMax) aOut[0] = 0; }
	// AT$STAT?: aIndex 0.. of its "KEY=VALUE" lines (after "+STAT:")
	virtual int StatCount() const { return 0; }
	virtual void StatLine(int aIndex, char* aOut, size_t aMax) const { (void)aIndex; if (aMax) aOut[0] = 0; }
	// the LCD's Call page: a row labelled Name(). aIdle: no call now. False: no row
	virtual bool CallPageRow(bool aIdle, char* aOut, size_t aMax) const { (void)aIdle; (void)aOut; (void)aMax; return false; }

	// ----- the web pages -----
	// the status page's row: its label links here (0: no link), and the
	// row ends with a link to it saying WebAction() (0: none, now)
	virtual const char* WebHome() const { return 0; }
	virtual const char* WebAction() const { return 0; }
	// its routes, ending with a 0 path (0: none)
	virtual const ExtRoute* Routes() const { return 0; }
	virtual void Get(const char* aPath, const ExtQuery& aQuery, ExtReply& aReply) { (void)aPath; (void)aQuery; (void)aReply; }

	Extension* iNextModule;    // (the registry's list)
	};

// the modules in this build, in the order they registered
Extension* ExtModules();
// a static object in a module's file: it is in the build
struct ExtRegister
	{
	explicit ExtRegister(Extension& aExt);
	};

// the modules' settings rows, after the modem's own (schema.cpp)
int ExtSchemaCount();
const SchemaEntry& ExtSchemaAt(int aIndex);
const Extension* ExtSchemaOwner(const SchemaEntry& aE);
// a record read from NVS: the modules' yes/no bytes made valid
void ExtSanitise(Settings2& aS2);

} // namespace am

#endif
