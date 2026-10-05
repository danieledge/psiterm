// webui.h - the Atom modem's web pages: the same settings as AT$ (built
// from the schema, so the two cannot drift), the status, the log, a factory
// reset. An access point with a captive portal when the modem is
// unconfigured (no WiFi network saved), in "config mode" (the button) or
// when asked (AT$WEB=2); pages on the network with a password otherwise.
// The Arduino core's own WebServer and DNSServer: no extra library, and
// its handlers run in loop() on the modem's own thread.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_WEBUI_H
#define ATOM_WEBUI_H

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "../modem.h"

class AtomHal;

namespace am {

class WebUi
	{
public:
	WebUi();
	void Begin(Modem& aModem, AtomHal& aHal);
	void Apply(const Settings2& aS2);         // the settings changed (or config mode did)
	void Loop();
	void ApInfo(char* aOut, size_t aMax) const;
	void WebInfo(char* aOut, size_t aMax) const;
	bool ApUp() const { return iApUp; }
	const char* ApName() const { return iApName; }

private:
	bool Allowed();                            // the password, or the access point
	void Head(const char* aTitle);
	void Tail();
	void Send(const char* aText);
	void SendP(const __FlashStringHelper* aText);
	void SendEsc(const char* aText);
	void PageStatus();
	void PageWifi();
	void PageSettings();
	void PageLog();
	void PageSystem();
	void ApiSettings();
	void Post();
	void NotFound();
	void StartAp();
	void StopAp();
	void Field(const SchemaEntry& aE);
	bool FromAp();

	Modem* iModem;
	AtomHal* iHal;
	WebServer iServer;
	DNSServer iDns;
	bool iApUp;
	bool iStarted;
	bool iDnsUp;
	char iApName[20];
	char iApPass[33];
	Settings2 iS2;
	uint32_t iLastScanMs;
	};

} // namespace am

#endif
