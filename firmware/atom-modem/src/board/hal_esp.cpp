// hal_esp.cpp - see hal_esp.h. MIT licence (see LICENSE at the top of the repository).
#include "hal_esp.h"
#include "boards.h"
#include "webui.h"
#if defined(AM_HAS_LCD)
#include "screen.h"
#endif
#include "../usbnet/usbhost.h"
#include "../cabundle.h"
#include "wgtunnel.h"
#include "pppdns.h"
#include "fwsafe.h"
#include <Preferences.h>
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include <esp_random.h>
#endif

static const char* const kNvs = "atommodem";

static AtomHal* sAtomHal = 0;             // the one HAL, for the WiFi event callback

AtomHal::AtomHal()
	: iInternet(-1), iDnsOk(-1), iWifiFail(am::EWifiFailNone), iWifiEvents(false), iProbing(false), iProbePort(0), iCur(&iPlain), iWeb(0), iUsb(0), iBaud(115200), iTx(kPinTx),
	  iRx(kPinRx), iRts(kPinRts), iCts(kPinCts), iDcdPin(-1), iFlow(false), iStarted(false), iRunning(false),
	  iUsbStarted(false), iUsbWanted(false), iLedState(-1), iExtLed(false), iExtLedMs(0)
	{
	iProbeHost[0] = 0;
	sAtomHal = this;                          // (for WifiEvent: there is one)
	}

// ----- the Internet check: a TCP connect in a task of its own ------------------------

// (2.2) a TCP connect alone can pass with DNS broken (the check's target is
// an address), so a real name is looked up too, and the Internet counts as
// working only if both did. The lookup goes through lwIP's resolver, as the
// modem's own dials do; its cache can hide a fault for up to the name's TTL
static const char* const kProbeName = "one.one.one.one";

void AtomHal::ProbeTask(void* aSelf)
	{
	AtomHal* h = (AtomHal*)aSelf;
	WiFiClient c;
	bool ok = c.connect(h->iProbeHost, h->iProbePort, 4000);
	c.stop();
	IPAddress a;
	bool dns = WiFi.hostByName(kProbeName, a) == 1 && (uint32_t)a != 0;
	h->iDnsOk = dns ? 1 : 0;
	h->iInternet = ok && dns ? 1 : 0;
	h->iProbing = false;
	vTaskDelete(0);
	}

void AtomHal::ProbeInternet(const char* aHost, uint16_t aPort)
	{
	if (iProbing)
		return;
	strncpy(iProbeHost, aHost, sizeof(iProbeHost) - 1);
	iProbeHost[sizeof(iProbeHost) - 1] = 0;
	iProbePort = aPort;
	iProbing = true;
	if (xTaskCreate(ProbeTask, "inetchk", 6144, this, 1, 0) != pdPASS)
		iProbing = false;
	}

void AtomHal::BeginTls()
	{
	// servers are checked against the Mozilla roots (cabundle.h, made by
	// tools/mkcabundle.py) and their names; 20 s at most for a handshake
#if ESP_ARDUINO_VERSION_MAJOR >= 3
	iTls.setCACertBundle(kCaBundle, sizeof(kCaBundle));
#else
	iTls.setCACertBundle(kCaBundle);
#endif
	iTls.setHandshakeTimeout(20);
	iTls.setTimeout(15);
	}

// ----- the UART to the Psion ------------------------------------------------------

int AtomHal::SerialRead() { return Serial2.read(); }
size_t AtomHal::SerialWritable() { return Serial2.availableForWrite(); }

size_t AtomHal::SerialWrite(const uint8_t* aData, size_t aLen)
	{
	size_t room = Serial2.availableForWrite();
	if (aLen > room)
		aLen = room;
	return aLen ? Serial2.write(aData, aLen) : 0;
	}

void AtomHal::SerialBaud(uint32_t aBaud)
	{
	iBaud = aBaud;
	if (iStarted)
		{
		Serial2.flush();                      // the OK goes out at the old speed
		Serial2.updateBaudRate(aBaud);
		}
	else
		StartSerial();
	}

void AtomHal::StartSerial()
	{
	// a big receive buffer (the Psion can send a burst while WiFi is busy),
	// no transmit buffer: availableForWrite() is then the UART's own FIFO,
	// so the pacing is not undone by a queue behind it
	Serial2.setRxBufferSize(4096);
	Serial2.setTxBufferSize(0);
	Serial2.begin(iBaud, SERIAL_8N1, iRx, iTx);
	if (iFlow && iRts >= 0 && iCts >= 0)
		{
		// the UART's own RTS/CTS: it stops sending when the Psion drops RTS
		// (our CTS input), and drops our RTS when its buffer is half full.
		// The transceiver inverts, so the ESP32's active-low lines come out
		// as RS-232's "asserted = positive" with nothing to do here
		Serial2.setPins(iRx, iTx, iCts, iRts);
		Serial2.setHwFlowCtrlMode(UART_HW_FLOWCTRL_CTS_RTS, 64);
		}
	else
		Serial2.setHwFlowCtrlMode(UART_HW_FLOWCTRL_DISABLE, 64);
	iStarted = true;
	}

void AtomHal::ApplyPins(const am::Settings& aS, const am::Settings2& aS2)
	{
	int tx = aS2.pins[0] >= 0 ? aS2.pins[0] : kPinTx;
	int rx = aS2.pins[1] >= 0 ? aS2.pins[1] : kPinRx;
	int rts = aS2.pins[2] >= 0 ? aS2.pins[2] : kPinRts;
	int cts = aS2.pins[3] >= 0 ? aS2.pins[3] : kPinCts;
	if (aS.swapPins) { int t = tx; tx = rx; rx = t; }
	if (aS2.flowSwap) { int t = rts; rts = cts; cts = t; }
	bool flow = aS2.flow != 0;
	if (flow && (rts < 0 || cts < 0))
		{
		Log("RTS/CTS asked for but this board has no pins set for them: flow control off");
		flow = false;
		}
	bool changed = tx != iTx || rx != iRx || rts != iRts || cts != iCts || flow != iFlow;
	iTx = tx; iRx = rx; iRts = rts; iCts = cts; iFlow = flow;
	if (changed && iStarted)
		{
		Serial2.flush();
		Serial2.end();
		iStarted = false;
		StartSerial();
		}
	// the DCD pin: AT$DCD (the 1.x way), or the fifth of AT$PINS
	int dcd = aS.dcdPin >= 0 ? aS.dcdPin : aS2.pins[4];
	if (iDcdPin >= 0 && iDcdPin != dcd)
		pinMode(iDcdPin, INPUT);
	iDcdPin = dcd;
	if (iDcdPin >= 0)
		pinMode(iDcdPin, OUTPUT);
	}

void AtomHal::Dcd(bool aOn)
	{
	if (iDcdPin >= 0)
		digitalWrite(iDcdPin, (aOn != kDcdActiveLow) ? HIGH : LOW);
	}

// ----- TCP and TLS ---------------------------------------------------------------

bool AtomHal::TcpConnect(const char* aHost, uint16_t aPort)
	{
	TcpClose();
	iCur = &iPlain;
	if (!iPlain.connect(aHost, aPort, 15000))
		return false;
	iPlain.setNoDelay(true);                  // keystrokes go at once (SSH)
	return true;
	}

// a connection for the web proxy or a TLS-terminated dial: plain, or TLS here
bool AtomHal::UpConnect(const char* aHost, uint16_t aPort, bool aTls, char* aWhy, size_t aWhyMax)
	{
	TcpClose();
	uint32_t t0 = millis();
	if (!aTls)
		{
		iCur = &iPlain;
		if (iPlain.connect(aHost, aPort, 15000))
			{
			iPlain.setNoDelay(true);
			return true;
			}
		snprintf(aWhy, aWhyMax, "no connection to %s:%u", aHost, aPort);
		return false;
		}
	iCur = &iTls;
	if (TlsVerify())
		{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
		iTls.setCACertBundle(kCaBundle, sizeof(kCaBundle));
#else
		iTls.setCACertBundle(kCaBundle);
#endif
		}
	else
		iTls.setInsecure();                   // AT$TLSV=0: no check at all (dangerous)
	size_t before = ESP.getFreeHeap();
	if (!iTls.connect(aHost, aPort))
		{
		char e[80];
		e[0] = 0;
		iTls.lastError(e, sizeof(e));
		snprintf(aWhy, aWhyMax, "%s", e[0] ? e : "no connection");
		char m[160];
		snprintf(m, sizeof(m), "TLS %s: failed after %lu ms: %s", aHost, (unsigned long)(millis() - t0), aWhy);
		Log(m);
		return false;
		}
	iTls.setNoDelay(true);
	char m[160];
	snprintf(m, sizeof(m), "TLS %s: %lu ms, it took %u KB (%u KB free, largest block %u KB)%s", aHost,
		(unsigned long)(millis() - t0), (unsigned)((before - ESP.getFreeHeap()) / 1024),
		(unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024),
		TlsVerify() ? "" : ", certificate NOT checked");
	Log(m);
	return true;
	}

void AtomHal::MemInfo(char* aOut, size_t aMax)
	{
	// (internal memory; with PSRAM the largest 8-bit block would be PSRAM's, so
	// that is asked for by name and PSRAM reported on its own)
	int n = snprintf(aOut, aMax, "Memory: %u KB free, largest block %u KB, lowest %u KB",
		(unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024),
		(unsigned)(ESP.getMinFreeHeap() / 1024));
#if defined(BOARD_HAS_PSRAM)
	if (n > 0 && (size_t)n < aMax && psramFound())
		snprintf(aOut + n, aMax - n, "; PSRAM %u KB, %u KB free, lowest %u KB", (unsigned)(ESP.getPsramSize() / 1024),
			(unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getMinFreePsram() / 1024));
#else
	(void)n;
#endif
	}

void AtomHal::Log(const char* aLine)
	{
	Record(aLine);
	Serial.println(aLine);                    // (the USB console, when there is one: see main.cpp)
	}

// ----- WiFi ---------------------------------------------------------------------------

void AtomHal::WifiBegin(const char* aSsid, const char* aPass)
	{
	// the station, keeping the access point if the web pages have one up
	WiFi.mode(WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA ? WIFI_AP_STA : WIFI_STA);
	WiFi.persistent(false);                   // our own NVS holds the creds; don't wear the flash
	WiFi.setAutoReconnect(true);
	iInternet = -1;                           // (a new network: not checked yet)
	iDnsOk = -1;
	iWifiFail = am::EWifiFailNone;            // (AT$STAT? FAIL=: for this join)
	if (!iWifiEvents)
		{
		WiFi.onEvent(WifiEvent);
		iWifiEvents = true;
		}
	WiFi.begin(aSsid, aPass);
	// modem sleep must be turned off AFTER begin() - begin() resets it to the
	// default on this core, which parks the radio and drops the first inbound
	// packets to a sleeping device (cold pings/HTTP time out until it is warm;
	// it has bitten OTA and the web UI). Off = the web UI/OTA answer at once.
	WiFi.setSleep(false);
	char m[80];
	snprintf(m, sizeof(m), "WiFi: joining \"%s\"", aSsid);
	Log(m);
	}

// A real radio down/up, not a bare begin(): esp_wifi off then on clears a
// wedged PHY/MAC - the soft case of "won't rejoin until a power cycle" (a
// bare WiFi.begin() and a soft reset both leave the radio as it was). Used
// only by the rejoin retry, never on the first clean join. Left alone while
// an access point is up, so a config-mode AP is not torn down.
void AtomHal::WifiHardReset()
	{
	if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA)
		return;
	WiFi.mode(WIFI_OFF);
	delay(100);
	WiFi.mode(WIFI_STA);
	}

void AtomHal::WifiInfo(char* aOut, size_t aMax)
	{
	snprintf(aOut, aMax, "\"%s\" IP %s RSSI %d", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
	}

int AtomHal::WifiScan(char aNames[][33], int aMax)
	{
	int n = WiFi.scanNetworks();
	if (n < 0) n = 0;
	if (n > aMax) n = aMax;
	for (int i = 0; i < n; i++)
		{
		strncpy(aNames[i], WiFi.SSID(i).c_str(), 32);
		aNames[i][32] = 0;
		}
	WiFi.scanDelete();
	return n;
	}

// AT$SCAN: the networks with their signal, security and channel (a blocking
// scan, about 2 to 4 s, hidden networks left out by the core)
int AtomHal::WifiScanFull(am::WifiNet* aOut, int aMax)
	{
	if (WiFi.getMode() == WIFI_OFF)
		WiFi.mode(WIFI_STA);
	// the station cannot scan while it is still trying to join (the IDF says
	// "busy"): stop that attempt first; whoever asked joins again after
	if (WiFi.status() != WL_CONNECTED)
		{
		WiFi.disconnect(false, false);
		delay(50);
		}
	int n = WiFi.scanNetworks();
	if (n < 0)
		{
		delay(200);
		n = WiFi.scanNetworks();            // (once more: a join that was just ending)
		}
	if (n < 0) n = 0;
	if (n > aMax) n = aMax;
	for (int i = 0; i < n; i++)
		{
		strncpy(aOut[i].ssid, WiFi.SSID(i).c_str(), sizeof(aOut[i].ssid) - 1);
		aOut[i].ssid[sizeof(aOut[i].ssid) - 1] = 0;
		int r = WiFi.RSSI(i);
		aOut[i].rssi = (int8_t)(r < -127 ? -127 : r > -1 ? -1 : r);
		aOut[i].secure = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1;
		aOut[i].channel = (uint8_t)WiFi.channel(i);
		}
	WiFi.scanDelete();
	return n;
	}

bool AtomHal::WifiLink(char* aSsid, size_t aSsidMax, char* aIp, size_t aIpMax, int& aRssi)
	{
	if (WiFi.status() != WL_CONNECTED)
		return false;
	snprintf(aSsid, aSsidMax, "%s", WiFi.SSID().c_str());
	snprintf(aIp, aIpMax, "%s", WiFi.localIP().toString().c_str());
	aRssi = (int)WiFi.RSSI();
	return true;
	}

// Why a join is not working, from the station's disconnect reasons (the
// IDF's wifi_err_reason_t, by number so that both cores' headers do). The
// disconnect that WiFi.begin() itself causes (8, "leaving") and the like
// are not failures and are ignored.
void AtomHal::WifiEvent(arduino_event_id_t aEvent, arduino_event_info_t aInfo)
	{
	AtomHal* h = sAtomHal;
	if (!h)
		return;
	if (aEvent == ARDUINO_EVENT_WIFI_STA_GOT_IP)
		h->iWifiFail = am::EWifiFailNone;
	else if (aEvent == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
		{
		switch (aInfo.wifi_sta_disconnected.reason)
			{
		case 201:                            // NO_AP_FOUND
		case 210: case 211: case 212:        // NO_AP_FOUND with compatible security / in the auth or RSSI threshold
			h->iWifiFail = am::EWifiFailNoNetwork;
			break;
		case 2:                              // AUTH_EXPIRE
		case 14:                             // MIC_FAILURE
		case 15:                             // 4WAY_HANDSHAKE_TIMEOUT: a wrong WPA2 password
		case 202:                            // AUTH_FAIL
		case 204:                            // HANDSHAKE_TIMEOUT
			h->iWifiFail = am::EWifiFailPassword;
			break;
		case 203:                            // ASSOC_FAIL
		case 205:                            // CONNECTION_FAIL
			h->iWifiFail = am::EWifiFailOther;
			break;
		default:
			break;                           // (8 ASSOC_LEAVE and the rest: not a failure to join)
			}
		}
	}

// ----- the USB host -----------------------------------------------------------------

int AtomHal::UsbState(char* aDetail, size_t aMax)
	{
	if (!iUsb || !iUsbStarted)
		{
		if (aMax) aDetail[0] = 0;
		return am::EUsbNone;
		}
	return iUsb->State(aDetail, aMax);
	}

void AtomHal::ApplyUplink(const am::Settings2& aS2)
	{
	iUsbWanted = aS2.usbHost != 0;
	if (!kUsbHostPossible)
		{
		if (iUsbWanted)
			Log("USB host: not on this board (an ESP32-S3 is needed)");
		return;
		}
	if (iUsbWanted && !iUsbStarted)
		{
		if (iRunning)
			Log("USB-C as a host: from the next restart (AT&W, then ATZ or power off and on)");
		else if (iUsb && iUsb->Begin())
			{
			iUsbStarted = true;
			Log("USB-C: host mode (the USB console is off; AT$LOG? shows the log)");
			}
		else
			Log("USB host: could not start");
		}
	else if (!iUsbWanted && iUsbStarted)
		Log("USB-C as a device again: from the next restart");
	}

// ----- NVS -------------------------------------------------------------------------------

bool AtomHal::LoadSettings(am::Settings& aS)
	{
	// Length-tolerant, like LoadSettings2: accept a record SHORTER than the
	// current struct (one saved by an older, smaller build) and zero-fill the
	// rest, so growing Settings in a future build does not fail the load and
	// trigger a factory reset that would wipe the saved WiFi network. Validity
	// is the caller's magic check (magic is at offset 0, always present).
	Preferences p;
	if (!p.begin(kNvs, true))
		return false;
	size_t have = p.getBytesLength("cfg");
	if (have == 0 || have > sizeof(aS))
		{
		p.end();
		return false;
		}
	memset(&aS, 0, sizeof(aS));
	size_t n = p.getBytes("cfg", &aS, have);
	p.end();
	return n == have;
	}

bool AtomHal::SaveSettings(const am::Settings& aS)
	{
	Preferences p;
	if (!p.begin(kNvs, false))
		return false;
	size_t n = p.putBytes("cfg", &aS, sizeof(aS));
	p.end();
	return n == sizeof(aS);
	}

bool AtomHal::LoadSettings2(am::Settings2& aS, size_t& aLength)
	{
	Preferences p;
	aLength = 0;
	if (!p.begin(kNvs, true))
		return false;
	size_t have = p.getBytesLength("cfg2");
	if (have == 0 || have > sizeof(aS))
		{
		p.end();
		return false;
		}
	memset(&aS, 0, sizeof(aS));
	aLength = p.getBytes("cfg2", &aS, have);
	p.end();
	return aLength == have;
	}

bool AtomHal::SaveSettings2(const am::Settings2& aS)
	{
	Preferences p;
	if (!p.begin(kNvs, false))
		return false;
	size_t n = p.putBytes("cfg2", &aS, sizeof(aS));
	p.end();
	return n == sizeof(aS);
	}

void AtomHal::NvsClear()
	{
	Preferences p;
	if (p.begin(kNvs, false))
		{
		p.clear();
		p.end();
		}
	}

void AtomHal::FactoryReset()
	{
	Log("Factory reset: every setting cleared; restarting");
	NvsClear();
	Serial2.flush();
	delay(300);
	ESP.restart();
	}

void AtomHal::Restart()
	{
	Log("Restarting");
	Serial2.flush();
	delay(200);
	ESP.restart();
	}

// ----- the LED, the web pages ----------------------------------------------------------

static void ModemLed(int aState)
	{
	switch (aState)
		{
	case am::ELedNoWifi:     LedWrite(24, 0, 0); break;    // red
	case am::ELedConnecting: LedWrite(24, 16, 0); break;   // amber
	case am::ELedWifi:       LedWrite(0, 16, 0); break;    // green
	case am::ELedConnected:  LedWrite(0, 0, 32); break;    // blue
	case am::ELedData:       LedWrite(24, 24, 48); break;  // white flash
	case am::ELedConfig:     LedWrite(24, 0, 32); break;   // purple: the access point is up
		}
	}

void AtomHal::Led(am::LedState aState)
	{
#if defined(AM_HAS_LCD)
	am::gScreen.SetBar(aState);               // the LCD's bar: the same state as the LED's
#endif
	iLedState = aState;
	if (!iExtLed)                             // (a module's light: LedTick decides)
		ModemLed(aState);
	}

void AtomHal::LedTick(const am::Modem& aModem, bool aBusy, uint32_t aNowMs)
	{
	if (kPinLed < 0)
		return;                               // (no WS2812: the LCD shows the module's page instead)
	am::ExtLed l;
	bool ext = am::ExtLedPick(aModem, iLedState, aBusy, l);
	if (!ext)
		{
		if (iExtLed)
			{
			iExtLed = false;
			ModemLed(iLedState);              // the modem's own state, at once
			}
		return;
		}
	// about 50 times a second is smooth enough for a pulse, and keeps the
	// WS2812's (RMT) writes out of most passes
	if (iExtLed && aNowMs - iExtLedMs < 20)
		return;
	iExtLed = true;
	iExtLedMs = aNowMs;
	uint8_t r, g, b;
	am::ExtLedLevel(l, aNowMs, r, g, b);
	LedWrite(r, g, b);
	}

void AtomHal::ApplyWeb(const am::Settings2& aS2)
	{
	if (iWeb)
		iWeb->Apply(aS2);
	}

const char* AtomHal::BoardName() { return kBoardName; }

void AtomHal::ApPass(char* aOut, size_t aMax)
	{
	if (aMax)
		aOut[0] = 0;
	if (iWeb && iWeb->ApUp())
		snprintf(aOut, aMax, "%s", iWeb->ApPass());
	}

void AtomHal::ApInfo(char* aOut, size_t aMax)
	{
	if (iWeb)
		iWeb->ApInfo(aOut, aMax);
	else if (aMax)
		aOut[0] = 0;
	}

void AtomHal::WebInfo(char* aOut, size_t aMax)
	{
	if (iWeb)
		iWeb->WebInfo(aOut, aMax);
	else if (aMax)
		aOut[0] = 0;
	}

// big buffers for extension modules (ext.h): PSRAM where there is some;
// else only while the internal heap keeps enough for WiFi and a TLS connection
static const size_t kBigKeepFree = 40 * 1024;

void AtomHal::Random(uint8_t* aOut, size_t aLen)
	{
	esp_fill_random(aOut, aLen);              // (the RF subsystem is on: true random)
	}

bool AtomHal::BigRoom(size_t aLen)
	{
#if defined(BOARD_HAS_PSRAM)
	if (psramFound())
		return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= aLen + 64 * 1024;
#endif
	size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	return free >= aLen + kBigKeepFree && big >= (aLen < 80 * 1024 ? aLen : 80 * 1024);
	}

void* AtomHal::BigAlloc(size_t aLen)
	{
#if defined(BOARD_HAS_PSRAM)
	if (psramFound())
		return heap_caps_malloc(aLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
	return BigRoom(aLen) ? malloc(aLen) : 0;
	}

int AtomHal::FirmwareInfo(char* aOut, size_t aMax)
	{
	am::gFwSafe.Text(aOut, aMax);
	return am::gFwSafe.Show();
	}

// ----- 2.2: the uplink's address, the tunnel, the DNS relay, the new records -------------

bool AtomHal::UplinkAddr(uint32_t& aIp, uint32_t& aMask)
	{
	// the default netif is the uplink (WiFi, or USB): the tunnel and the PPP
	// link never become the default
	LOCK_TCPIP_CORE();
	struct netif* n = netif_default;
	aIp = n && netif_is_up(n) ? ip4_addr_get_u32(netif_ip4_addr(n)) : 0;
	aMask = aIp ? ip4_addr_get_u32(netif_ip4_netmask(n)) : 0;
	UNLOCK_TCPIP_CORE();
	return aIp != 0;
	}

void AtomHal::WgApply(const am::WgConfig& aConfig, bool aWant)
	{
	am::gWg.Begin(*this);
	am::gWg.Apply(aConfig, aWant);
	}

void AtomHal::WgInfo(am::WgStatus& aOut) { am::gWg.Info(aOut); }

bool AtomHal::DnsRelayStart()
	{
	bool ok = am::gPppDns.Start();
	if (!ok)
		Log("ppp: the DNS relay could not start (port 53 in use?): the Psion gets the uplink's DNS");
	return ok;
	}

void AtomHal::DnsRelayStop()
	{
	if (!am::gPppDns.Running())
		return;
	const am::DnsRelay& r = am::gPppDns.Relay();
	char m[96];
	snprintf(m, sizeof(m), "ppp: DNS relay: %lu queries, %lu answered, %lu timed out, %lu dropped",
		(unsigned long)r.Forwarded(), (unsigned long)r.Answered(), (unsigned long)r.TimedOut(), (unsigned long)r.Dropped());
	Log(m);
	am::gPppDns.Stop();
	}

void AtomHal::DnsRelayUpstream(uint32_t aServer)
	{
	am::gPppDns.SetUpstream(aServer);
	char d[16], m[64];
	am::Ip4Text(aServer, d, sizeof(d));
	snprintf(m, sizeof(m), "DNS relay: queries go to %s", d);
	Log(m);
	}

template <class T> static bool LoadRecord(const char* aKey, T& aOut, size_t& aLength)
	{
	Preferences p;
	aLength = 0;
	if (!p.begin(kNvs, true))
		return false;
	size_t have = p.getBytesLength(aKey);
	if (have == 0 || have > sizeof(aOut))
		{
		p.end();
		return false;
		}
	memset(&aOut, 0, sizeof(aOut));
	aLength = p.getBytes(aKey, &aOut, have);
	p.end();
	return aLength == have;
	}

template <class T> static bool SaveRecord(const char* aKey, const T& aIn)
	{
	Preferences p;
	if (!p.begin(kNvs, false))
		return false;
	size_t n = p.putBytes(aKey, &aIn, sizeof(aIn));
	p.end();
	return n == sizeof(aIn);
	}

bool AtomHal::LoadNets(am::NetList& aL, size_t& aLength) { return LoadRecord("nets", aL, aLength); }
bool AtomHal::SaveNets(const am::NetList& aL) { return SaveRecord("nets", aL); }
bool AtomHal::LoadWg(am::WgConfig& aC, size_t& aLength) { return LoadRecord("wg", aC, aLength); }
bool AtomHal::SaveWg(const am::WgConfig& aC) { return SaveRecord("wg", aC); }
