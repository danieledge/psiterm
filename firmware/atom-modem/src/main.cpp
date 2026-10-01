// main.cpp - the M5Stack Atom side of the Atom modem: the RS232 base's UART,
// WiFi, TCP, NVS, the status LED, the button. The modem itself (Hayes
// commands, +++, pacing) is the portable code in modem.cpp.
// MIT licence (see LICENSE at the top of the repository).
//
// Pins (M5Stack Atom Lite / Matrix with the Atomic RS232 Base, SKU A131):
//   G22  RX  from the base's MAX232 receiver (the Psion's TXD, DB9 pin 3)
//   G19  TX  to the base's MAX232 driver (the Psion's RXD, DB9 pin 2)
//   G27  the WS2812 status LED     G39  the button
// AT$SWAP=1 swaps G22 and G19, for a base or cable wired the other way.
// AT$DCD=<gpio> drives an emulated DCD on a spare pin (G33, G23, G21 or
// G25) - it needs an RS232 driver of its own: see README.
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "modem.h"

static const int kPinRx = 22;
static const int kPinTx = 19;
static const int kPinLed = 27;
static const int kPinButton = 39;
static const bool kDcdActiveLow = true;   // through an inverting RS232 driver (MAX232/MAX3232)

class AtomHal : public am::Hal
	{
public:
	AtomHal() : iBaud(115200), iSwap(false), iDcdPin(-1), iStarted(false) {}

	uint32_t Millis() override { return millis(); }
	uint32_t Micros() override { return micros(); }

	int SerialRead() override { return Serial2.read(); }
	size_t SerialWritable() override { return Serial2.availableForWrite(); }
	size_t SerialWrite(const uint8_t* aData, size_t aLen) override
		{
		size_t room = Serial2.availableForWrite();
		if (aLen > room)
			aLen = room;
		return aLen ? Serial2.write(aData, aLen) : 0;
		}
	void SerialBaud(uint32_t aBaud) override
		{
		iBaud = aBaud;
		if (iStarted)
			{
			Serial2.flush();                  // the OK goes out at the old speed
			Serial2.updateBaudRate(aBaud);
			}
		else
			StartSerial();
		}

	bool TcpConnect(const char* aHost, uint16_t aPort) override
		{
		iClient.stop();
		if (!iClient.connect(aHost, aPort, 15000))
			return false;
		iClient.setNoDelay(true);             // keystrokes go at once (SSH)
		return true;
		}
	bool TcpOpen() override { return iClient.connected(); }
	size_t TcpAvailable() override { int n = iClient.available(); return n > 0 ? n : 0; }
	size_t TcpRead(uint8_t* aBuf, size_t aMax) override { int n = iClient.read(aBuf, aMax); return n > 0 ? n : 0; }
	size_t TcpWrite(const uint8_t* aData, size_t aLen) override { return iClient.write(aData, aLen); }
	void TcpClose() override { iClient.stop(); }

	bool WifiUp() override { return WiFi.status() == WL_CONNECTED; }
	void WifiBegin(const char* aSsid, const char* aPass) override
		{
		WiFi.mode(WIFI_STA);
		WiFi.setSleep(false);                 // modem sleep adds 100 ms+ of latency
		WiFi.setAutoReconnect(true);
		WiFi.begin(aSsid, aPass);
		Serial.printf("WiFi: joining \"%s\"\n", aSsid);
		}
	void WifiEnd() override { WiFi.disconnect(true); }
	void WifiInfo(char* aOut, size_t aMax) override
		{
		snprintf(aOut, aMax, "\"%s\" IP %s RSSI %d", WiFi.SSID().c_str(),
			WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
		}
	int WifiScan(char aNames[][33], int aMax) override
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

	bool LoadSettings(am::Settings& aS) override
		{
		Preferences p;
		if (!p.begin("atommodem", true))
			return false;
		size_t n = p.getBytes("cfg", &aS, sizeof(aS));
		p.end();
		return n == sizeof(aS);
		}
	bool SaveSettings(const am::Settings& aS) override
		{
		Preferences p;
		if (!p.begin("atommodem", false))
			return false;
		size_t n = p.putBytes("cfg", &aS, sizeof(aS));
		p.end();
		return n == sizeof(aS);
		}

	void Dcd(bool aOn) override
		{
		if (iDcdPin >= 0)
			digitalWrite(iDcdPin, (aOn != kDcdActiveLow) ? HIGH : LOW);
		}
	void Led(am::LedState aState) override
		{
		switch (aState)
			{
		case am::ELedNoWifi:     neopixelWrite(kPinLed, 24, 0, 0); break;    // red
		case am::ELedConnecting: neopixelWrite(kPinLed, 24, 16, 0); break;   // amber
		case am::ELedWifi:       neopixelWrite(kPinLed, 0, 16, 0); break;    // green
		case am::ELedConnected:  neopixelWrite(kPinLed, 0, 0, 32); break;    // blue
		case am::ELedData:       neopixelWrite(kPinLed, 24, 24, 48); break;  // white flash
			}
		}
	void ApplyPins(const am::Settings& aS) override
		{
		bool swap = aS.swapPins != 0;
		if (swap != iSwap && iStarted)
			{
			iSwap = swap;
			Serial2.flush();
			Serial2.end();
			iStarted = false;
			StartSerial();
			}
		iSwap = swap;
		if (iDcdPin >= 0 && iDcdPin != aS.dcdPin)
			pinMode(iDcdPin, INPUT);
		iDcdPin = aS.dcdPin;
		if (iDcdPin >= 0)
			pinMode(iDcdPin, OUTPUT);
		}

private:
	void StartSerial()
		{
		// a big receive buffer (the Psion can send a burst while WiFi is
		// busy), no transmit buffer: availableForWrite() is then the UART's
		// own FIFO, so the pacing is not undone by a queue behind it
		Serial2.setRxBufferSize(4096);
		Serial2.setTxBufferSize(0);
		Serial2.begin(iBaud, SERIAL_8N1, iSwap ? kPinTx : kPinRx, iSwap ? kPinRx : kPinTx);
		iStarted = true;
		}

	WiFiClient iClient;
	uint32_t iBaud;
	bool iSwap;
	int iDcdPin;
	bool iStarted;
	};

static AtomHal gHal;
static am::Modem* gModem = 0;

// The ring towards the Psion: as big as the heap allows after WiFi
static uint8_t* AllocRing(size_t& aSize)
	{
	static const size_t kSizes[] = { 128 * 1024, 96 * 1024, 64 * 1024, 32 * 1024, 16 * 1024 };
	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++)
		{
		uint8_t* p = (uint8_t*)malloc(kSizes[i]);
		if (p)
			{
			aSize = kSizes[i];
			return p;
			}
		}
	aSize = 0;
	return 0;
	}

// Hold the button while plugging in (3 s, the LED flashes white): every
// setting, the WiFi network too, back to the factory ones
static void FactoryResetIfHeld()
	{
	pinMode(kPinButton, INPUT);
	if (digitalRead(kPinButton) != LOW)
		return;
	uint32_t start = millis();
	while (digitalRead(kPinButton) == LOW)
		{
		neopixelWrite(kPinLed, ((millis() / 150) & 1) ? 32 : 0, ((millis() / 150) & 1) ? 32 : 0, ((millis() / 150) & 1) ? 32 : 0);
		if (millis() - start > 3000)
			{
			Preferences p;
			if (p.begin("atommodem", false))
				{
				p.clear();
				p.end();
				}
			Serial.println("Factory reset");
			neopixelWrite(kPinLed, 32, 32, 32);
			delay(1000);
			return;
			}
		delay(10);
		}
	}

void setup()
	{
	Serial.begin(115200);                     // USB: status messages only
	FactoryResetIfHeld();
	WiFi.mode(WIFI_STA);                      // (WiFi takes its memory before the ring does)
	size_t size;
	uint8_t* ring = AllocRing(size);
	Serial.printf("Atom modem %s: buffer %u bytes\n", am::kVersion, (unsigned)size);
	gModem = new am::Modem(gHal, ring, size);
	gModem->Begin();
	}

void loop()
	{
	static bool wasUp = false;
	gModem->Loop();
	bool up = WiFi.status() == WL_CONNECTED;
	if (up != wasUp)
		{
		wasUp = up;
		if (up)
			Serial.printf("WiFi: connected, IP %s\n", WiFi.localIP().toString().c_str());
		else
			Serial.println("WiFi: down");
		}
	yield();
	}
