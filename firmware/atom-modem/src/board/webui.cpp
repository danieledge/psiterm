// webui.cpp - see webui.h. MIT licence (see LICENSE at the top of the repository).
#include "webui.h"
#include "hal_esp.h"
#include "boards.h"
#include <WiFi.h>
#include <esp_system.h>
#include <Update.h>
#include <esp_core_dump.h>
#include <esp_flash.h>
#include "fwsafe.h"
#include "../cast/cast.h"
#include "../cast/castpage.h"
#include "../cast/casticons.h"
#if defined(AM_HAS_LCD)
#include "screen.h"
#include "../canvas.h"
#endif

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
	: iModem(0), iHal(0), iCast(0), iServer(80), iApUp(false), iStarted(false), iDnsUp(false), iLastScanMs(0),
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
	snprintf(iApName, sizeof(iApName), "PsiConnect-%02X%02X", mac[4], mac[5]);
	iServer.on("/", HTTP_GET, [this]() { PageStatus(); });
	iServer.on("/wifi", HTTP_GET, [this]() { PageWifi(); });
	iServer.on("/settings", HTTP_GET, [this]() { PageSettings(); });
	iServer.on("/log", HTTP_GET, [this]() { PageLog(); });
	iServer.on("/crash", HTTP_GET, [this]() { PageCrash(); });
	iServer.on("/screen.bmp", HTTP_GET, [this]() { ScreenBmp(); });
	// PsiCast: the viewer (behind the password), its token, and the Home
	// Screen files (not secret, and fetched without the password by iOS)
	iServer.on("/screen", HTTP_GET, [this]() { PageScreen(); });
	iServer.on("/screen/token", HTTP_GET, [this]() { ScreenToken(); });
	iServer.on("/screen/manifest.json", HTTP_GET, [this]()
		{
		iServer.sendHeader("Cache-Control", "max-age=86400");
		iServer.send_P(200, "application/manifest+json", kCastManifest, sizeof(kCastManifest) - 1);
		});
	iServer.on("/screen/icon.png", HTTP_GET, [this]()
		{
		iServer.sendHeader("Cache-Control", "max-age=86400");
		iServer.send_P(200, "image/png", (const char*)kCastIcon180, kCastIcon180Len);
		});
	iServer.on("/screen/icon-512.png", HTTP_GET, [this]()
		{
		iServer.sendHeader("Cache-Control", "max-age=86400");
		iServer.send_P(200, "image/png", (const char*)kCastIcon512, kCastIcon512Len);
		});
	// the Series 7 and Modern themes' colour pictures (and their matcher), fetched only when one is chosen
	iServer.on("/screen/iconmatch.js", HTTP_GET, [this]() { CastAsset("text/javascript", kCastIconMatchGz, kCastIconMatchGzLen); });
	iServer.on("/screen/s7icons.json", HTTP_GET, [this]() { CastAsset("application/json", kCastS7IconsGz, kCastS7IconsGzLen); });
	iServer.on("/screen/modicons.json", HTTP_GET, [this]() { CastAsset("application/json", kCastModIconsGz, kCastModIconsGzLen); });
	iServer.on("/screen/silk5mx.jpg", HTTP_GET, [this]()          // (the 5mx's face, for full screen: not gzipped)
		{
		if (!Allowed()) return;
		iServer.sendHeader("Cache-Control", "max-age=604800");
		iServer.send_P(200, "image/jpeg", (const char*)kCastSilk5mx, kCastSilk5mxLen);
		});
	iServer.on("/system", HTTP_GET, [this]() { PageSystem(); });
	iServer.on("/vpn", HTTP_GET, [this]() { PageVpn(); });
	iServer.on("/wg", HTTP_POST, [this]() { PostWg(); });
	iServer.on("/net", HTTP_POST, [this]() { PostNet(); });
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
				iServer.requestAuthentication(BASIC_AUTH, "PsiConnect");
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
		{
		gFwSafe.WebServed();                  // (a new firmware on trial: the pages work)
		return true;
		}
	if (!iS2.webPass[0])
		{
		iServer.send(403, "text/plain", "No web password is set: set one with AT$WEBPASS, or use the access point");
		return false;
		}
	if (!iServer.authenticate("atom", iS2.webPass))
		{
		iServer.requestAuthentication(BASIC_AUTH, "PsiConnect");
		return false;
		}
	gFwSafe.WebServed();
	return true;
	}

// the same rule as Allowed(), but it sends nothing: the upload handler runs
// while the body streams in, so it cannot write a response here.
bool WebUi::OtaAuth()
	{
	bool ok = FromAp() || (iS2.webPass[0] && iServer.authenticate("atom", iS2.webPass));
	if (ok)
		gFwSafe.WebServed();
	return ok;
	}

// a gzipped file of the viewer's, behind the password like the page itself
void WebUi::CastAsset(const char* aType, const unsigned char* aData, unsigned aLen)
	{
	if (!Allowed())
		return;
	if (!aLen)                                   // (a pack this build was made without)
		{
		iServer.send(404, "text/plain", "Not found\n");
		return;
		}
	iServer.sendHeader("Content-Encoding", "gzip");
	iServer.sendHeader("Cache-Control", "max-age=604800");
	iServer.send_P(200, aType, (const char*)aData, aLen);
	}

// ----- the crash report ---------------------------------------------------------------------------

// The last crash's core dump, which the IDF writes to the coredump partition
// on a panic. Plain text: the task, the exception, the backtrace (decode the
// addresses with addr2line and the build's firmware.elf). ?raw=1 sends the
// whole dump (for esp-coredump), ?clear=1 erases it.
void WebUi::PageCrash()
	{
	if (!Allowed())
		return;
	size_t addr = 0, size = 0;
	if (esp_core_dump_image_get(&addr, &size) != ESP_OK || !size)
		{
		iServer.send(200, "text/plain", "No crash report saved\n");
		return;
		}
	if (iServer.hasArg("clear"))
		{
		esp_core_dump_image_erase();
		iServer.send(200, "text/plain", "Crash report erased\n");
		return;
		}
	if (iServer.hasArg("raw"))
		{
		iServer.setContentLength(size);
		iServer.sendHeader("Content-Disposition", "attachment; filename=coredump.elf");
		iServer.send(200, "application/octet-stream", "");
		static uint8_t buf[1024];
		for (size_t off = 0; off < size; off += sizeof(buf))
			{
			size_t n = size - off < sizeof(buf) ? size - off : sizeof(buf);
			if (esp_flash_read(NULL, buf, addr + off, n) != ESP_OK)
				break;
			iServer.client().write(buf, n);
			}
		return;
		}
	static esp_core_dump_summary_t s;
	if (esp_core_dump_get_summary(&s) != ESP_OK)
		{
		iServer.send(200, "text/plain", "A crash report is saved but cannot be read (?raw=1 to download it)\n");
		return;
		}
	String o;
	char line[96];
	snprintf(line, sizeof(line), "Task: %.16s\nPC: 0x%08lx\nCause: %lu  address: 0x%08lx\nELF: %.16s\nBacktrace%s:",
		s.exc_task, (unsigned long)s.exc_pc, (unsigned long)s.ex_info.exc_cause, (unsigned long)s.ex_info.exc_vaddr,
		(const char*)s.app_elf_sha256, s.exc_bt_info.corrupted ? " (corrupted)" : "");
	o += line;
	for (uint32_t i = 0; i < s.exc_bt_info.depth && i < 16; i++)
		{
		snprintf(line, sizeof(line), " 0x%08lx", (unsigned long)s.exc_bt_info.bt[i]);
		o += line;
		}
	o += "\n";
	iServer.send(200, "text/plain", o);
	}

// ----- pieces of pages ---------------------------------------------------------------------------

// Never send an empty piece: in a chunked reply, sendContent("") is the
// zero-length chunk that ENDS the page (a saved network with a password cut
// the WiFi page off after its name)
void WebUi::Send(const char* aText) { if (aText[0]) iServer.sendContent(aText); }
void WebUi::SendP(const __FlashStringHelper* aText) { String s(aText); if (s.length()) iServer.sendContent(s); }

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
	if (s.length())
		iServer.sendContent(s);
	}

void WebUi::Head(const char* aTitle)
	{
	iServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
	iServer.send(200, "text/html; charset=utf-8", "");
	Send("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>PsiConnect: ");
	SendEsc(aTitle);
	Send("</title>");
	SendP(FPSTR(kCss));
	Send("</head><body><header><h1>PsiConnect</h1><a href=\"/\">Status</a><a href=\"/wifi\">WiFi</a><a href=\"/vpn\">VPN</a>"
		"<a href=\"/settings\">Settings</a><a href=\"/log\">Log</a><a href=\"/system\">System</a><a href=\"/screen\">Screen</a></header><main>");
	}

void WebUi::Tail()
	{
	Send("<p><small>PsiConnect ");
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
			: iModem->ExecCall() ? "psiexec" : iModem->CastCall() ? "PsiCast" : "a connection", (unsigned long)iModem->ToPsion(), (unsigned long)iModem->ToServer());
		Send(w);
		}
	else
		Send("none");
	Send("</td></tr>");
	{
	static const char* const kModes[] = { "Off", "On", "Auto" };
	int mode = iModem->WgMode() <= 2 ? iModem->WgMode() : 0;
	char t[80];
	iModem->WgStateText(t, sizeof(t));
	Send("<tr><td><a href=\"/vpn\">WireGuard</a></td><td><b>"); Send(kModes[mode]); Send("</b>: "); SendEsc(t);
	Send("<form method=\"post\" action=\"/set\" style=\"margin:0\"><input type=\"hidden\" name=\"back\" value=\"/\">"
		"<input type=\"hidden\" name=\"save\" value=\"1\">");
	static const char* const kVals[] = { "OFF", "ON", "AUTO" };
	for (int i = 0; i < 3; i++)
		{
		Send("<button name=\"s_WG\" value=\""); Send(kVals[i]); Send(i == mode ? "\" class=\"go\">" : "\">");
		Send(kModes[i]); Send("</button>");
		}
	Send("</form></td></tr>");
	if (iCast)
		{
		char c[100];
		iCast->StateText(c, sizeof(c));
		Send("<tr><td><a href=\"/screen\">PsiCast</a></td><td>"); SendEsc(c);
		if (iCast->Enabled())
			Send(" &middot; <a href=\"/screen\">show the screen</a>");
		Send("</td></tr>");
		}
	int dns = iHal->DnsOk();
	Send("<tr><td>Internet check</td><td>");
	Send(iHal->Internet() == 1 ? "passed" : iHal->Internet() == 0 ? "failed" : "not run yet");
	Send(dns == 1 ? " (DNS works)" : dns == 0 ? " (DNS lookups fail)" : "");
	Send("</td></tr>");
	}
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
	Send("<p><small>A phone's Personal Hotspot is a network like any other: turn it on, then enter its name and password here. "
		"Join and save adds it to the saved networks below.</small></p>");
	// the saved networks (2.2)
	const NetList& nl = iModem->Nets();
	Send("<h2>Saved networks</h2>");
	if (!nl.count)
		Send("<p>None yet.</p>");
	else
		{
		Send("<table>");
		bool up = WiFi.status() == WL_CONNECTED;
		for (int i = 0; i < nl.count; i++)
			{
			bool cur = strcmp(nl.net[i].ssid, iModem->Config().ssid) == 0;
			char n[8];
			snprintf(n, sizeof(n), "%d", i);
			Send("<tr><td>"); SendEsc(nl.net[i].ssid);
			Send(nl.net[i].pass[0] ? "" : " <small>(open)</small>");
			if (cur) Send(up ? " <b>(in use)</b>" : " <b>(joining)</b>");
			if (strcmp(nl.net[i].ssid, iModem->Pinned()) == 0) Send(" <small>picked by hand</small>");
			Send("</td><td><form method=\"post\" action=\"/net\" style=\"margin:0\"><input type=\"hidden\" name=\"i\" value=\"");
			Send(n); Send("\"><button name=\"a\" value=\"switch\">Switch to it</button>"
				"<button name=\"a\" value=\"forget\" onclick=\"return confirm('Forget this network?')\">Forget</button></form></td></tr>");
			}
		Send("</table>");
		}
	Send("<p><small>At the start, and when the WiFi is lost, the modem joins the strongest saved network in range. "
		"One switched to by hand is kept while it is in range. Up to 8 are kept.</small></p>");
	Send("<h3>Add a network</h3><form method=\"post\" action=\"/net\"><input type=\"hidden\" name=\"a\" value=\"add\"><table>"
		"<tr><td>Network</td><td><input name=\"ssid\" maxlength=\"32\"></td></tr>"
		"<tr><td>Password</td><td><input type=\"password\" name=\"pass\" maxlength=\"64\" placeholder=\"(none for an open network)\"></td></tr>"
		"</table><button class=\"go\">Save</button></form>");
	Send("<h2>Access point</h2><p>This modem's own network, for these pages when it is not on yours: <b>");
	SendEsc(iApName); Send("</b>");
	if (iApUp) { Send(", up now with the password <b>"); SendEsc(iApPass); Send("</b>"); }
	Send(".</p><form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"back\" value=\"/wifi\"><table>");
	for (int i = 0; i < SchemaCount(); i++)
		{
		const SchemaEntry& e = SchemaAt(i);
		if (strcmp(SchemaWebPage(e), "/wifi") == 0 && e.group != SGWifi)
			Field(e);                           // (SSID and PASS: the form above)
		}
	Send("</table><button class=\"go\">Apply</button></form>");
	Tail();
	}

void WebUi::PageSettings()
	{
	if (!Allowed()) return;
	Head("Settings");
	Send("<div class=\"warn\"><b>Serial safety:</b> the Psion's port is RS-232 (up to ±12 V). Every serial pin below must reach it "
		"through an RS-232 transceiver (a MAX3232, or the Atomic RS232 Base). Never wire a pin straight to the Psion.</div>");
	// every group with a setting on this page (SchemaWebPage), in the table's order
	static const TSchemaGroup kGroups[] = { SGUplink, SGSerial, SGProxy, SGTls, SGMail, SGPictures, SGExec, SGCast, SGSystem, SGWifi, SGWg };
	for (size_t g = 0; g < sizeof(kGroups) / sizeof(kGroups[0]); g++)
		{
		bool any = false;
		for (int i = 0; i < SchemaCount(); i++)
			if (SchemaAt(i).group == kGroups[g] && strcmp(SchemaWebPage(SchemaAt(i)), "/settings") == 0)
				any = true;
		if (!any)
			continue;
		Send("<h2>"); Send(SchemaGroupName(kGroups[g])); Send("</h2>");
		Send("<form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"back\" value=\"/settings\"><table>");
		for (int i = 0; i < SchemaCount(); i++)
			{
			const SchemaEntry& e = SchemaAt(i);
			if (e.group != kGroups[g] || strcmp(SchemaWebPage(e), "/settings") != 0)
				continue;
			Field(e);
			}
		Send("</table><button class=\"go\">Apply</button></form>");
		if (kGroups[g] == SGTls)
			Send("<p><small>With TLS termination the modem sees your mail and pages in plain text, and so does the serial cable. "
				"For your own modem on your own network only. Set the account's TLS to <i>none</i> in PsiMail, keeping port 993 or 465.</small></p>");
		if (kGroups[g] == SGMail)
			Send("<p><small>Only for the PPP route. Set a host and the modem accepts plaintext mail on 192.168.7.1 (993/465/995) and does the TLS to it, "
				"checking the certificate (AT$TLSV). In PsiMail point IMAP/SMTP at <code>192.168.7.1</code> with security <i>None</i>. Empty = off.</small></p>");
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

// PsiCast's viewer: one page, gzipped (cast/viewer.html, tools/mkcastpage.py)
void WebUi::PageScreen()
	{
	if (!Allowed()) return;
	if (!iCast)
		{
		iServer.send(404, "text/plain", "No PsiCast in this firmware");
		return;
		}
	iServer.sendHeader("Content-Encoding", "gzip");
	iServer.sendHeader("Cache-Control", "no-cache");
	iServer.send_P(200, "text/html; charset=utf-8", (const char*)kCastPageGz, kCastPageGzLen);
	}

// {"t":"<token>","port":5962,"control":true}: single use, for 60 s; ?control=1 asks for control
void WebUi::ScreenToken()
	{
	if (!Allowed()) return;
	iServer.sendHeader("Cache-Control", "no-store");
	char t[32];
	if (!iCast || !iCast->NewToken(iServer.arg("control") == "1", t, sizeof(t)))
		{
		iServer.send(403, "application/json", iCast ? "{\"error\":\"PsiCast is off\"}" : "{\"error\":\"No PsiCast in this firmware\"}");
		return;
		}
	char m[96];
	snprintf(m, sizeof(m), "{\"t\":\"%s\",\"port\":%u,\"control\":%s}", t, (unsigned)kCastViewPort,
		iServer.arg("control") == "1" ? "true" : "false");
	iServer.send(200, "application/json", m);
	}

// the LCD as it is now: the frame buffer pushed to the panel, as a 24-bit
// BMP (about 48 KB), sent a row at a time
void WebUi::ScreenBmp()
	{
	if (!Allowed()) return;
#if defined(AM_HAS_LCD)
	const uint16_t* fb = gScreen.Frame();
	if (fb)
		{
		iServer.setContentLength(Canvas::kBmpSize);
		iServer.sendHeader("Cache-Control", "no-store");
		iServer.send(200, "image/bmp", "");
		uint8_t buf[Canvas::kBmpRow];
		Canvas::BmpHeader(buf);
		iServer.sendContent((const char*)buf, Canvas::kBmpHeader);
		for (int r = 0; r < Canvas::kSize; r++)
			{
			Canvas::BmpRow(fb, r, buf);
			iServer.sendContent((const char*)buf, Canvas::kBmpRow);
			}
		return;
		}
#endif
	iServer.send(404, "text/plain", "No screen on this board");
	}

void WebUi::PageSystem()
	{
	if (!Allowed()) return;
	Head("System");
	Send("<h2>Save</h2><form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"back\" value=\"/system\">"
		"<input type=\"hidden\" name=\"save\" value=\"1\"><button class=\"go\">Save settings to flash</button></form>");
	Send("<h2>Restart</h2><form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"restart\" value=\"1\"><button>Restart the modem</button></form>");
	{
	char t[160];
	gFwSafe.Text(t, sizeof(t));
	if (t[0])
		{
		Send(gFwSafe.Show() == EFwShowPrevious ? "<div class=\"warn\"><b>" : "<div class=\"ok\"><b>");
		SendEsc(t);
		Send(".</b></div>");
		}
	}
	Send("<h2>Firmware update</h2><p>Upload a new <code>firmware.bin</code>: the modem writes it to the spare slot and "
		"restarts on it. A web password must be set (<code>AT$WEBPASS</code>). The new firmware is on trial until it has "
		"the network (or a page is opened) after a restart; if it fails to start properly three times, the modem goes back "
		"to the firmware it runs now, by itself.</p>"
		"<form method=\"post\" action=\"/ota\" enctype=\"multipart/form-data\"><input type=\"file\" name=\"firmware\" accept=\".bin\">"
		"<button class=\"go\" onclick=\"return confirm('Flash this firmware?')\">Upload &amp; flash</button></form>"
		"<p><small>Or from a computer: <code>curl -u atom:PASSWORD -F firmware=@firmware.bin http://IP/ota</code></small></p>");
	Send("<h2>Factory reset</h2><p>Every setting, the WiFi network and passwords included, back to the factory ones. The modem restarts.</p>"
		"<form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"reset\" value=\"YES\"><button onclick=\"return confirm('Factory reset?')\">Factory reset</button></form>");
	{
	char t[96];
	gFwSafe.Slots(t, sizeof(t));
	Send("<p><small>Firmware: "); SendEsc(t); Send(".</small></p>");
	}
	Send("<h2>About</h2><p>PsiConnect "); Send(kVersion); Send(" on the "); Send(kBoardName);
	Send(". <a href=\"/api/settings\">Settings as JSON</a>. The firmware and its documentation are in the PsiTerm repository "
		"(<code>firmware/atom-modem/docs/UPGRADE.md</code>).</p>");
	Tail();
	}

// ----- 2.2: the VPN page, and the forms for it and the saved networks ------------------------

void WebUi::PageVpn()
	{
	if (!Allowed()) return;
	Head("VPN");
	const WgConfig& c = iModem->Wg();
	const WgStatus& st = iModem->WgNow();
	char t[120];
	Send("<h2>WireGuard</h2><table>");
	iModem->WgStateText(t, sizeof(t));
	Send("<tr><td>State</td><td>"); SendEsc(t); Send("</td></tr>");
	if (iModem->WgWant() == EWgWantUp)
		{
		char e[16];
		Ip4Text(st.endpointIp, e, sizeof(e));
		snprintf(t, sizeof(t), "%lu bytes in, %lu out; endpoint address %s", (unsigned long)st.rx, (unsigned long)st.tx,
			st.endpointIp ? e : "not looked up yet");
		Send("<tr><td>Traffic</td><td>"); Send(t); Send("</td></tr>");
		}
	Send("</table><form method=\"post\" action=\"/set\"><input type=\"hidden\" name=\"back\" value=\"/vpn\">"
		"<input type=\"hidden\" name=\"save\" value=\"1\"><table>");
	for (int i = 0; i < SchemaCount(); i++)
		if (strcmp(SchemaWebPage(SchemaAt(i)), "/vpn") == 0)
			Field(SchemaAt(i));
	Send("</table><button class=\"go\">Apply and save</button></form>");
	Send("<h2>Config</h2>");
	if (WgValid(c))
		{
		char sum[400];
		WgSummary(c, sum, sizeof(sum));
		Send("<pre>"); SendEsc(sum); Send("</pre>");
		Send("<form method=\"post\" action=\"/wg\"><input type=\"hidden\" name=\"clear\" value=\"YES\">"
			"<button onclick=\"return confirm('Delete the WireGuard config?')\">Delete the config</button></form>");
		}
	else
		Send("<p>None yet.</p>");
	Send("<h3>Paste a config</h3><p><small>The whole client config as your WireGuard server gives it ([Interface] and [Peer]). "
		"It is kept in this modem's flash only; the keys are never shown again. Only the AllowedIPs go through the tunnel "
		"(e.g. your home network, 192.168.10.0/24); everything else uses the WiFi as before.</small></p>"
		"<form method=\"post\" action=\"/wg\"><textarea name=\"conf\" rows=\"12\" style=\"width:100%;font:12px monospace\" "
		"autocomplete=\"off\" spellcheck=\"false\" placeholder=\"[Interface]&#10;PrivateKey = ...&#10;Address = ...&#10;DNS = ...&#10;&#10;[Peer]&#10;PublicKey = ...&#10;AllowedIPs = ...&#10;Endpoint = host:port\"></textarea>"
		"<button class=\"go\">Save the config</button></form>");
	Send("<p><small><b>UniFi:</b> Settings, VPN, VPN Server, your WireGuard server, Add client: name it, keep the "
		"generated keys, then download or copy the client config and paste it here. In <b>Auto</b> the tunnel is up "
		"except when the modem is already on the home network.</small></p>");
	Tail();
	}

void WebUi::Back(bool aBad, const char* aText, const char* aBack)
	{
	Head("Applied");
	Send(aBad ? "<p class=\"bad\">" : "<p class=\"ok\">");
	SendEsc(aText);
	Send("</p><p><a href=\""); SendEsc(aBack); Send("\">Back</a></p>");
	Tail();
	}

// /wg: conf=<the config text> (parsed and saved at once), or clear=YES
void WebUi::PostWg()
	{
	if (!Allowed()) return;
	if (iServer.hasArg("clear") && iServer.arg("clear") == "YES")
		{
		iModem->ClearWgConfig();
		Back(false, "The WireGuard config is deleted.", "/vpn");
		return;
		}
	if (!iServer.hasArg("conf"))
		{
		Back(true, "No config sent.", "/vpn");
		return;
		}
	char why[80];
	String conf = iServer.arg("conf");
	bool ok = conf.length() <= kWgTextMax && iModem->SetWgConfig(conf.c_str(), why, sizeof(why));
	if (conf.length() > kWgTextMax)
		snprintf(why, sizeof(why), "The config is too long");
	// (wipe our copy of the text: it holds the private key)
	for (unsigned i = 0; i < conf.length(); i++)
		conf.setCharAt(i, 0);
	if (ok)
		Back(false, "Saved. The tunnel follows the mode (Off, On or Auto).", "/vpn");
	else
		{
		char m[120];
		snprintf(m, sizeof(m), "Not saved: %s.", why);
		Back(true, m, "/vpn");
		}
	}

// /net: a=switch|forget with i=<index>, or a=add with ssid and pass (saved at once)
void WebUi::PostNet()
	{
	if (!Allowed()) return;
	String a = iServer.arg("a");
	if (a == "add")
		{
		String ssid = iServer.arg("ssid"), pass = iServer.arg("pass");
		bool ok = iModem->AddNet(ssid.c_str(), pass.c_str(), true);
		for (unsigned i = 0; i < pass.length(); i++)
			pass.setCharAt(i, 0);
		Back(!ok, ok ? "Saved." : "Not saved: a name of 1 to 32 characters, and a password of 8 to 64 (or none) are needed.", "/wifi");
		return;
		}
	int i = iServer.arg("i").toInt();
	bool ok = false;
	if (a == "switch")
		ok = iModem->SwitchNet(i);
	else if (a == "forget")
		ok = iModem->ForgetNet(i, true);
	Back(!ok, ok ? (a == "switch" ? "Switching: the page may stop answering if the modem leaves this network." : "Forgotten.")
		: "No such saved network.", "/wifi");
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
