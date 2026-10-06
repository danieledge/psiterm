// hal_esp.cpp - see hal_esp.h. MIT licence (see LICENSE at the top of the repository).
#include "hal_esp.h"
#include "boards.h"
#include "webui.h"
#if defined(AM_HAS_LCD)
#include "screen.h"
#endif
#include "../usbnet/usbhost.h"
#include "../cabundle.h"
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

static const char* const kNvs = "atommodem";

AtomHal::AtomHal()
	: iInternet(-1), iProbing(false), iProbePort(0), iCur(&iPlain), iWeb(0), iUsb(0), iBaud(115200), iTx(kPinTx),
	  iRx(kPinRx), iRts(kPinRts), iCts(kPinCts), iDcdPin(-1), iFlow(false), iStarted(false), iRunning(false),
	  iUsbStarted(false), iUsbWanted(false)
	{
	iProbeHost[0] = 0;
	}

// ----- the Internet check: a TCP connect in a task of its own ------------------------

void AtomHal::ProbeTask(void* aSelf)
	{
	AtomHal* h = (AtomHal*)aSelf;
	WiFiClient c;
	bool ok = c.connect(h->iProbeHost, h->iProbePort, 4000);
	c.stop();
	h->iInternet = ok ? 1 : 0;
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
	if (xTaskCreate(ProbeTask, "inetchk", 4096, this, 1, 0) != pdPASS)
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
	WiFi.setSleep(false);                     // modem sleep adds 100 ms+ of latency
	WiFi.setAutoReconnect(true);
	iInternet = -1;                           // (a new network: not checked yet)
	WiFi.begin(aSsid, aPass);
	char m[80];
	snprintf(m, sizeof(m), "WiFi: joining \"%s\"", aSsid);
	Log(m);
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
	Preferences p;
	if (!p.begin(kNvs, true))
		return false;
	size_t n = p.getBytes("cfg", &aS, sizeof(aS));
	p.end();
	return n == sizeof(aS);
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

void AtomHal::Led(am::LedState aState)
	{
#if defined(AM_HAS_LCD)
	am::gScreen.SetBar(aState);               // the LCD's bar: the same state as the LED's
#endif
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
