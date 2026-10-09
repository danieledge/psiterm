// webui.cpp - see webui.h. MIT licence (see LICENSE at the top of the repository).
#include "webui.h"
#include "hal_esp.h"
#include "boards.h"
#include <WiFi.h>
#include <esp_system.h>
#include <Update.h>

namespace am {

static const char kCss[] PROGMEM =
	"<style>body{font:16px/1.4 -apple-system,Helvetica,Arial,sans-serif;margin:0;background:#f4f4f2;color:#222}"
	"header{background:#223;color:#fff;padding:10px 16px}header a{color:#fff;text-decoration:none;margin-right:14px}"
	"main{padding:12px 16px;max-width:640px}h1{font-size:20px;margin:0}h2{font-size:17px;margin:18px 0 6px}"
	"table{border-collapse:collapse;width:100%}td{padding:5px 4px;border-bottom:1px solid #ddd;vertical-align:top}"
	"td:first-child{width:42%;color:#444}input,select{font:inherit;width:100%;box-sizing:border-box;padding:5px}"
	"button{font:inherit;padding:8px 14px;margin:8px 6px 0 0;border:1px solid #889;background:#fff;border-radius:4px}"
	"button.go{background:#223;color:#fff}.ok{background:#dfd;padding:6px 10px}.bad{background:#fdd;padding:6px 10px}"
	"small{color:#666}pre{background:#fff;border:1px solid #ddd;padding:8px;overflow:auto;font-size:12px}"
	".warn{background:#fff3cd;border:1px solid #e0c060;padding:8px 10px;margin:8px 0}</style>";

WebUi::WebUi()
	: iModem(0), iHal(0), iServer(80), iApUp(false), iStarted(false), iDnsUp(false), iLastScanMs(0),
	  iOtaAllowed(false), iOtaBegun(false)
	{
	iApName[0] = 0;
	iApPass[0] = 0;
	}

void WebUi::Begin(Modem& aModem, AtomHal& aHal)
	{
	iModem = &aModem;
	iHal = &aHal;
	uint8_t mac[6];
	WiFi.macAddress(mac);
	snprintf(iApName, sizeof(iApName), "AtomModem-%02x%02x", mac[4], mac[5]);
	iServer.on("/", HTTP_GET, [this]() { PageStatus(); });
	iServer.on("/wifi", HTTP_GET, [this]() { PageWifi(); });
	iServer.on("/settings", HTTP_GET, [this]() { PageSettings(); });
	iServer.on("/log", HTTP_GET, [this]() { PageLog(); });
	iServer.on("/system", HTTP_GET, [this]() { PageSystem(); });
	iServer.on("/api/settings", HTTP_GET, [this]() { ApiSettings(); });
	iServer.on("/set", HTTP_POST, [this]() { Post(); });
	iServer.on("/api/settings", HTTP_POST, [this]() { Post(); });
	// over-the-air firmware: a multipart upload of firmware.bin, written to the
	// spare app slot; the modem restarts on it. Behind the same web password.
	iServer.on("/ota", HTTP_POST,
		[this]()                                        // after the body: the result
			{
			if (!iOtaAllowed)
				{
				iServer.requestAuthentication(BASIC_AUTH, "Atom modem");
				return;
				}
			bool ok = iOtaBegun && !Update.hasError();
			iServer.sendHeader("Connection", "close");
			iServer.send(ok ? 200 : 500, "text/plain",
				ok ? "OK - the modem is restarting on the new firmware\n"
				   : "Update failed - the modem keeps the current firmware\n");
			if (ok)
				{
				delay(400);
				ESP.restart();
				}
			},
		[this]()                                        // the body, in chunks
			{
			HTTPUpload& up = iServer.upload();
			if (up.status == UPLOAD_FILE_START)
				{
				iOtaAllowed = OtaAuth();
				iOtaBegun = false;
				if (iOtaAllowed && Update.begin(UPDATE_SIZE_UNKNOWN))
					iOtaBegun = true;
				}
			else if (up.status == UPLOAD_FILE_WRITE)
				{
				if (iOtaBegun && Update.write(up.buf, up.currentSize) != up.currentSize)
					iOtaBegun = false;              // a short write: it has failed
				}
			else if (up.status == UPLOAD_FILE_END)
				{
				if (iOtaBegun && !Update.end(true))
					iOtaBegun = false;
				}
			else if (up.status == UPLOAD_FILE_ABORTED)
				{
				if (iOtaBegun)
					Update.abort();
				iOtaBegun = false;
				}
			});
	iServer.onNotFound([this]() { NotFound(); });
	}

// ----- the access point ----------------------------------------------------------------

void WebUi::StartAp()
	{
	if (iApUp)
		return;
	// the password: the saved one, else one made here and kept for the session
	if (iS2.apPass[0])
		strncpy(iApPass, iS2.apPass, sizeof(iApPass) - 1);
	else if (!iApPass[0])
		{
		uint32_t r1 = esp_random(), r2 = esp_random();
		snprintf(iApPass, sizeof(iApPass), "psion-%04lx%04lx", (unsigned long)(r1 & 0xffff), (unsigned long)(r2 & 0xffff));
		}
	iApPass[sizeof(iApPass) - 1] = 0;
	WiFi.mode(WiFi.status() == WL_CONNECTED || iModem->Config().ssid[0] ? WIFI_AP_STA : WIFI_AP);
	WiFi.softAP(iApName, iApPass);
	iApUp = true;
	iDns.start(53, "*", WiFi.softAPIP());
	iDnsUp = true;
	char m[120];
	snprintf(m, sizeof(m), "Access point \"%s\" up, password \"%s\", pages at http://%s/", iApName, iApPass,
		WiFi.softAPIP().toString().c_str());
	iHal->Log(m);
	}

void WebUi::StopAp()
	{
	if (!iApUp)
		return;
	iDns.stop();
	iDnsUp = false;
	WiFi.softAPdisconnect(true);
	WiFi.mode(WIFI_STA);
	iApUp = false;
	iHal->Log("Access point down");
	}

void WebUi::Apply(const Settings2& aS2)
	{
	iS2 = aS2;
	bool wantAp = iS2.web == EWebApAlways || iModem->InConfigMode()
		|| (iS2.web == EWebOn && !iModem->Config().ssid[0]);
	bool wantServer = iS2.web != EWebOff || iModem->InConfigMode();
	if (wantAp)
		StartAp();
	else
		StopAp();
	if (wantServer && !iStarted)
		{
		iServer.begin();
		iStarted = true;
		}
	else if (!wantServer && iStarted)
		{
		iServer.stop();
		iStarted = false;
		}
	}

void WebUi::Loop()
	{
	if (iDnsUp)
		iDns.processNextRequest();
	if (iStarted)
		iServer.handleClient();
	}

void WebUi::ApInfo(char* aOut, size_t aMax) const
	{
	if (iApUp)
		snprintf(aOut, aMax, "AP \"%s\" up at http://%s/", iApName, WiFi.softAPIP().toString().c_str());
	else if (aMax)
		aOut[0] = 0;
	}

void WebUi::WebInfo(char* aOut, size_t aMax) const
	{
	if (iStarted && WiFi.status() == WL_CONNECTED)
		snprintf(aOut, aMax, "http://%s/%s", WiFi.localIP().toString().c_str(), iS2.webPass[0] ? "" : " (set AT$WEBPASS first)");
	else if (aMax)
		aOut[0] = 0;
	}

// ----- access ------------------------------------------------------------------------------------

bool WebUi::FromAp()
	{
	return iApUp && iServer.client().localIP() == WiFi.softAPIP();
	}

// the pages on the network need the web password; the access point's own
// network is trusted (its WPA2 password is the gate)
bool WebUi::Allowed()
	{
	if (FromAp())
		return true;
	if (!iS2.webPass[0])
		{
		iServer.send(403, "text/plain", "No web password is set: set one with AT$WEBPASS, or use the access point");
		return false;
		}
	if (!iServer.authenticate("atom", iS2.webPass))
		{
		iServer.requestAuthentication(BASIC_AUTH, "Atom modem");
		return false;
		}
	return true;
	}

// the same rule as Allowed(), but it sends nothing: the upload handler runs
// while the body streams in, so it cannot write a response here.
bool WebUi::OtaAuth()
	{
	if (FromAp())
		return true;
	if (!iS2.webPass[0])
		return false;
	return iServer.authenticate("atom", iS2.webPass);
	}

// ----- pieces of pages ---------------------------------------------------------------------------

void WebUi::Send(const char* aText) { iServer.sendContent(aText); }
void WebUi::SendP(const __FlashStringHelper* aText) { iServer.sendContent(String(aText)); }

void WebUi::SendEsc(const char* aText)
	{
	String s;
	for (const char* p = aText; *p; p++)
		{
		if (*p == '<') s += "&lt;";
		else if (*p == '&') s += "&amp;";
		else if (*p == '"') s += "&quot;";
		else s += *p;
		}
	iServer.sendContent(s);
	}

void WebUi::Head(const char* aTitle)
	{
	iServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
	iServer.send(200, "text/html; charset=utf-8", "");
	Send("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Atom modem: ");
	SendEsc(aTitle);
	Send("</title>");
	SendP(FPSTR(kCss));
	Send("</head><body><header><h1>Atom modem</h1><a href=\"/\">Status</a><a href=\"/wifi\">WiFi</a>"
		"<a href=\"/settings\">Settings</a><a href=\"/log\">Log</a><a href=\"/system\">System</a></header><main>");
	}

void WebUi::Tail()
	{
	Send("<p><small>Atom modem ");
	Send(kVersion);
	Send(" on the ");
	Send(kBoardName);
	Send(". Settings take effect at once; <a href=\"/system\">save</a> them to keep them after a restart.</small></p></main></body></html>");
	iServer.sendContent("");
	}

// one row of a settings form, from the schema
void WebUi::Field(const SchemaEntry& aE)
	{
	char v[100];
	iModem->WebGet(aE, v, sizeof(v));
	Send("<tr><td>");
	SendEsc(aE.label);
	Send("<br><small>");
	SendEsc(aE.help);
	Send("</small></td><td>");
	String name = String("s_") + aE.name;
	switch (aE.type)
		{
	case STBool: case STBoolInv:
		Send("<select name=\""); Send(name.c_str()); Send("\"><option value=\"0\"");
		if (v[0] == '0') Send(" selected");
		Send(">Off</option><option value=\"1\"");
		if (v[0] == '1') Send(" selected");
		Send(">On</option></select>");
		break;
	case STEnum:
		Send("<select name=\""); Send(name.c_str()); Send("\">");
		for (int i = 0; aE.names[i]; i++)
			{
			Send("<option value=\""); Send(aE.names[i]); Send("\"");
			if (strcmp(aE.names[i], v) == 0) Send(" selected");
			Send(">"); Send(aE.names[i]); Send("</option>");
			}
		Send("</select>");
		break;
	case STProxyMode:
		{
		static const char* const kModes[] = { "Off", "Simplified pages", "Text only", "Pages unchanged (TLS only)", "Reader" };
		Send("<select name=\""); Send(name.c_str()); Send("\">");
		for (int i = 0; i < 5; i++)
			{
			char o[8];
			snprintf(o, sizeof(o), "%d", i);
			Send("<option value=\""); Send(o); Send("\"");
			if (atoi(v) == i) Send(" selected");
			Send(">"); Send(kModes[i]); Send("</option>");
			}
		Send("</select>");
		break;
		}
	case STSecret:
		Send("<input type=\"password\" name=\""); Send(name.c_str());
		Send("\" placeholder=\""); Send(strcmp(v, "(set)") == 0 ? "(set; leave blank to keep)" : "(none)"); Send("\">");
		break;
	case STInt: case STBaud:
		{
		char r[64];
		snprintf(r, sizeof(r), "\" type=\"number\" min=\"%ld\" max=\"%ld\" value=\"", (long)aE.min, (long)aE.max);
		Send("<input name=\""); Send(name.c_str()); Send(r); Send(v); Send("\">");
		break;
		}
	default:
		Send("<input name=\""); Send(name.c_str()); Send("\" value=\""); SendEsc(aE.type == STPace && strncmp(v, "AUTO", 4) == 0 ? "AUTO" : v); Send("\">");
		break;
		}
	Send("</td></tr>");
	}

// ----- the pages ---------------------------------------------------------------------------------

void WebUi::PageStatus()
	{
	if (!Allowed()) return;
	Head("Status");
	char w[160], d[64];
	Send("<h2>Status</h2><table>");
	Send("<tr><td>Uplink</td><td>"); iModem->Link().Describe(w, sizeof(w)); SendEsc(w); Send("</td></tr>");
	Send("<tr><td>WiFi</td><td>");
	if (WiFi.status() == WL_CONNECTED) { iHal->WifiInfo(w, sizeof(w)); SendEsc(w); }
	else { Send("not connected"); if (iModem->Config().ssid[0]) { Send(" to \""); SendEsc(iModem->Config().ssid); Send("\""); } }
	Send("</td></tr>");
	int us = iHal->UsbState(d, sizeof(d));
	Send("<tr><td>USB-C</td><td>"); Send(iModem->Config2().usbHost ? "host: " : "device (programming, console)");
	if (iModem->Config2().usbHost) { Send(Uplink::UsbStateName(us)); SendEsc(d); }
	Send("</td></tr>");
	snprintf(w, sizeof(w), "%lu baud, %s, pins TX %d RX %d%s", (unsigned long)iModem->Config().baud,
		iHal->Flow() ? "RTS/CTS" : "no flow control", iHal->PinTx(), iHal->PinRx(), iHal->Flow() ? "" : "");
	Send("<tr><td>Serial</td><td>"); Send(w);
	if (iHal->Flow()) { snprintf(w, sizeof(w), " RTS %d CTS %d", iHal->PinRts(), iHal->PinCts()); Send(w); }
	Send("</td></tr>");
	if (iModem->Config().paceRate)
		snprintf(w, sizeof(w), "%s%lu bytes/s in %u-byte bursts, buffer %u KB", iModem->Config().paceAuto ? "auto: " : "",
			(unsigned long)iModem->Config().paceRate, iModem->Config().paceBurst, (unsigned)(iModem->RingSize() / 1024));
	else
		snprintf(w, sizeof(w), "off, buffer %u KB", (unsigned)(iModem->RingSize() / 1024));
	Send("<tr><td>Pacing</td><td>"); Send(w); Send("</td></tr>");
	Send("<tr><td>Call</td><td>");
	if (iModem->Connected())
		{
		snprintf(w, sizeof(w), "%s, %lu bytes to the Psion, %lu to the server", iModem->ProxyCall() ? "the web proxy"
			: iModem->ExecCall() ? "psiexec" : "a connection", (unsigned long)iModem->ToPsion(), (unsigned long)iModem->ToServer());
		Send(w);
		}
	else
		Send("none");
	Send("</td></tr>");
	snprintf(w, sizeof(w), "proxy %s; TLS termination %s; pictures %s; remote compute %s",
		ProxyMode(iModem->Config()) ? "on" : "off", iModem->Config2().tls ? "on" : "off", iModem->Config2().img ? "on" : "off",
		iModem->Config2().exec ? "on" : "off");
	Send("<tr><td>Features</td><td>"); Send(w); Send("</td></tr>");
	iHal->MemInfo(w, sizeof(w));
	Send("<tr><td>Memory</td><td>"); Send(w + 8); Send("</td></tr>");
	snprintf(w, sizeof(w), "%lu min", (unsigned long)(millis() / 60000));
	Send("<tr><td>Up</td><td>"); Send(w); Send("</td></tr>");
	if (iApUp)
		{
		Send("<tr><td>Access point</td><td>"); SendEsc(iApName); Send(" (");
		Send(iModem->InConfigMode() ? "config mode, 10 minutes" : iS2.web == EWebApAlways ? "always on" : "until a WiFi network is set");
		Send(")</td></tr>");
		}
	Send("</table>");
	if (!iModem->Config().ssid[0])
		Send("<div class=\"warn\">No WiFi network is set yet: go to <a href=\"/wifi\">WiFi</a>.</div>");
	Tail();
	}

void WebUi::PageWifi()
	{
	if (!Allowed()) return;
	Head("WiFi");
	Send("<h2>WiFi network</h2><form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"back\" value=\"/wifi\">"
		"<input type=\"hidden\" name=\"save\" value=\"1\"><table>");
	Send("<tr><td>Network</td><td><input name=\"s_SSID\" list=\"nets\" value=\""); SendEsc(iModem->Config().ssid); Send("\">");
	Send("<datalist id=\"nets\">");
	// a scan (a few seconds; not while a call is up, and not more than once in 10 s)
	if (!iModem->Connected() && (millis() - iLastScanMs > 10000 || !iLastScanMs))
		{
		iLastScanMs = millis();
		int n = WiFi.scanNetworks();
		for (int i = 0; i < n && i < 20; i++)
			{
			Send("<option value=\""); SendEsc(WiFi.SSID(i).c_str()); Send("\">");
			}
		WiFi.scanDelete();
		}
	Send("</datalist></td></tr>");
	Send("<tr><td>Password</td><td><input type=\"password\" name=\"s_PASS\" placeholder=\"");
	Send(iModem->Config().pass[0] ? "(set; leave blank to keep)" : "(none)"); Send("\"></td></tr></table>");
	Send("<button class=\"go\">Join and save</button></form>");
	Send("<p><small>A phone's Personal Hotspot is a network like any other: turn it on, then enter its name and password here.</small></p>");
	Send("<h2>Access point</h2><p>This modem's own network, for these pages when it is not on yours: <b>");
	SendEsc(iApName); Send("</b>");
	if (iApUp) { Send(", up now with the password <b>"); SendEsc(iApPass); Send("</b>"); }
	Send(".</p><form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"back\" value=\"/wifi\"><table>");
	Field(*SchemaFind("APPASS"));
	Field(*SchemaFind("WEB"));
	Field(*SchemaFind("WEBPASS"));
	Send("</table><button class=\"go\">Apply</button></form>");
	Tail();
	}

void WebUi::PageSettings()
	{
	if (!Allowed()) return;
	Head("Settings");
	Send("<div class=\"warn\"><b>Serial safety:</b> the Psion's port is RS-232 (up to ±12 V). Every serial pin below must reach it "
		"through an RS-232 transceiver (a MAX3232, or the Atomic RS232 Base). Never wire a pin straight to the Psion.</div>");
	static const TSchemaGroup kGroups[] = { SGUplink, SGSerial, SGProxy, SGTls, SGPictures, SGExec, SGSystem };
	for (size_t g = 0; g < sizeof(kGroups) / sizeof(kGroups[0]); g++)
		{
		Send("<h2>"); Send(SchemaGroupName(kGroups[g])); Send("</h2>");
		Send("<form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"back\" value=\"/settings\"><table>");
		for (int i = 0; i < SchemaCount(); i++)
			{
			const SchemaEntry& e = SchemaAt(i);
			if (e.group != kGroups[g] || (kGroups[g] == SGSystem && (strcmp(e.name, "WEB") == 0 || strcmp(e.name, "WEBPASS") == 0
				|| strcmp(e.name, "APPASS") == 0)))
				continue;
			Field(e);
			}
		Send("</table><button class=\"go\">Apply</button></form>");
		if (kGroups[g] == SGTls)
			Send("<p><small>With TLS termination the modem sees your mail and pages in plain text, and so does the serial cable. "
				"For your own modem on your own network only. Set the account's TLS to <i>none</i> in PsiMail, keeping port 993 or 465.</small></p>");
		if (kGroups[g] == SGExec)
			Send("<p><small>Runs commands on a PC through <code>tools/psiexecd.py</code>, which allows only the commands it is started with. "
				"The token is kept in the modem's flash only.</small></p>");
		if (kGroups[g] == SGUplink)
			Send("<p><small>USB host mode takes the USB-C port: the console and programming need it back (hold the button through a reset "
				"for the download mode). An iPhone enumerates but reports no carrier until Apple's pairing is implemented; use its WiFi hotspot.</small></p>");
		}
	Tail();
	}

void WebUi::PageLog()
	{
	if (!Allowed()) return;
	Head("Log");
	Send("<h2>Log</h2><p><a href=\"/log\">Refresh</a></p><pre>");
	const LogRing& log = iHal->LogLines();
	char buf[256];
	size_t from = 0, n;
	while ((n = log.Read(from, buf, sizeof(buf))) > 0)
		{
		SendEsc(buf);
		from += n;
		}
	Send("</pre>");
	Tail();
	}

void WebUi::PageSystem()
	{
	if (!Allowed()) return;
	Head("System");
	Send("<h2>Save</h2><form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"back\" value=\"/system\">"
		"<input type=\"hidden\" name=\"save\" value=\"1\"><button class=\"go\">Save settings to flash</button></form>");
	Send("<h2>Restart</h2><form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"restart\" value=\"1\"><button>Restart the modem</button></form>");
	Send("<h2>Firmware update</h2><p>Upload a new <code>firmware.bin</code>: the modem writes it to the spare slot and "
		"restarts on it. A web password must be set (<code>AT$WEBPASS</code>). If a bad build will not boot, recover over USB.</p>"
		"<form method=\"post\" action=\"/ota\" enctype=\"multipart/form-data\"><input type=\"file\" name=\"firmware\" accept=\".bin\">"
		"<button class=\"go\" onclick=\"return confirm('Flash this firmware?')\">Upload &amp; flash</button></form>"
		"<p><small>Or from a computer: <code>curl -u atom:PASSWORD -F firmware=@firmware.bin http://IP/ota</code></small></p>");
	Send("<h2>Factory reset</h2><p>Every setting, the WiFi network and passwords included, back to the factory ones. The modem restarts.</p>"
		"<form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"reset\" value=\"YES\"><button onclick=\"return confirm('Factory reset?')\">Factory reset</button></form>");
	Send("<h2>About</h2><p>Atom modem "); Send(kVersion); Send(" on the "); Send(kBoardName);
	Send(". <a href=\"/api/settings\">Settings as JSON</a>. The firmware and its documentation are in the PsiTerm repository "
		"(<code>firmware/atom-modem/docs/UPGRADE.md</code>).</p>");
	Tail();
	}

void WebUi::ApiSettings()
	{
	if (!Allowed()) return;
	String s = "{";
	for (int i = 0; i < SchemaCount(); i++)
		{
		const SchemaEntry& e = SchemaAt(i);
		char v[100];
		iModem->WebGet(e, v, sizeof(v));
		if (i) s += ",";
		s += "\""; s += e.name; s += "\":";
		if (e.type == STSecret)
			s += strcmp(v, "(set)") == 0 ? "true" : "false";
		else if (e.type == STBool || e.type == STBoolInv || e.type == STInt || e.type == STBaud || e.type == STProxyMode)
			s += v;
		else
			{
			s += "\"";
			for (const char* p = v; *p; p++) { if (*p == '"' || *p == '\\') s += '\\'; s += *p; }
			s += "\"";
			}
		}
	s += "}";
	iServer.send(200, "application/json", s);
	}

// a form: s_<NAME>=value for any setting (an empty secret is unchanged),
// save=1, restart=1, reset=YES; back=<page> to return to
void WebUi::Post()
	{
	if (!Allowed()) return;
	String result;
	bool bad = false;
	for (int i = 0; i < iServer.args(); i++)
		{
		String name = iServer.argName(i);
		if (!name.startsWith("s_"))
			continue;
		const SchemaEntry* e = SchemaFind(name.c_str() + 2);
		if (!e)
			{
			result += "Unknown setting " + name.substring(2) + ". ";
			bad = true;
			continue;
			}
		String v = iServer.arg(i);
		if (e->type == STSecret && v.length() == 0)
			continue;                         // kept
		if (!iModem->WebSet(*e, v.c_str()))
			{
			result += String(e->label) + ": not accepted. ";
			bad = true;
			}
		}
	if (iServer.hasArg("reset") && iServer.arg("reset") == "YES")
		{
		iServer.send(200, "text/html", "<p>Factory reset: the modem is restarting.</p>");
		delay(200);
		iModem->Factory(false);
		iHal->FactoryReset();
		return;
		}
	if (iServer.hasArg("save"))
		{
		if (iModem->Save())
			result += "Saved. ";
		else
			{
			result += "Could not save. ";
			bad = true;
			}
		}
	if (iServer.hasArg("restart"))
		{
		iServer.send(200, "text/html", "<p>Restarting.</p>");
		delay(200);
		iHal->Restart();
		return;
		}
	if (iServer.hasArg("back"))
		{
		String back = iServer.arg("back");
		if (!back.startsWith("/")) back = "/";
		Head("Applied");
		Send(bad ? "<p class=\"bad\">" : "<p class=\"ok\">");
		SendEsc(result.length() ? result.c_str() : "Applied.");
		Send("</p><p><a href=\""); SendEsc(back.c_str()); Send("\">Back</a></p>");
		Tail();
		return;
		}
	iServer.send(bad ? 400 : 200, "text/plain", result.length() ? result : "OK");
	}

// the captive portal: anything else goes to the status page
void WebUi::NotFound()
	{
	if (iApUp && FromAp())
		{
		iServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
		iServer.send(302, "text/plain", "");
		return;
		}
	iServer.send(404, "text/plain", "Not found");
	}

} // namespace am
