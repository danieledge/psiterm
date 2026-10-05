// main.cpp - setup() and loop() for the Atom modem on the M5Stack AtomS3
// Lite (ESP32-S3) and the plain Atom (ESP32). The modem itself (Hayes
// commands, +++, pacing, the web proxy, pictures, psiexec) is the portable
// code in modem.cpp and friends; the board side is board/ and usbnet/.
// MIT licence (see LICENSE at the top of the repository).
//
// SAFETY: every serial pin here is 3.3 V TTL. The Psion's port is RS-232
// (±12 V). An RS-232 transceiver (MAX3232, or the Atomic RS232 Base) must
// sit between them, always. See docs/UPGRADE.md, section 1.
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "modem.h"
#include "board/boards.h"
#include "board/hal_esp.h"
#include "board/webui.h"
#include "usbnet/usbhost.h"

// The web proxy's TLS handshake (mbedTLS) and the picture decoder run
// inside loop(): its 8 KB default stack is too tight for them
SET_LOOP_TASK_STACK_SIZE(20 * 1024);

// A network preset at build time, for a modem that should join a known
// network from its first start without the portal. Empty in the tree: the
// values come only from the build's flags (PLATFORMIO_BUILD_FLAGS, with
// the password from the environment), never from a file. Used only when
// no network has been saved yet; AT$SSID and the pages override it
#ifndef AM_DEFAULT_SSID
#define AM_DEFAULT_SSID ""
#endif
#ifndef AM_DEFAULT_PASS
#define AM_DEFAULT_PASS ""
#endif

static AtomHal gHal;
static am::WebUi gWeb;
static am::UsbNet gUsb;
static am::Modem* gModem = 0;

// The ring towards the Psion: as big as the heap allows after WiFi, leaving
// room for the web proxy: the modem object with the proxy (about 25 KB), a
// TLS connection (mbedTLS: about 50 KB while it is open), the gzip writer
// (16 KB), the picture buffer and decoder (up to 64 KB + 40 KB) and, rarely,
// the gzip reader for a server that packs its pages anyway (43 KB). The
// sizes are logged at start-up, and ATI shows what is free.
static const size_t kKeepFree = 150 * 1024;
static uint8_t* AllocRing(size_t& aSize)
	{
#if defined(BOARD_HAS_PSRAM)
	// A board with PSRAM (the AtomS3R): the ring goes there, bigger, and the
	// whole internal heap is left to WiFi, TLS and the stacks. (The core's
	// malloc puts any block of 4 KB or more in PSRAM anyway; this asks for
	// it by name, so a board whose PSRAM failed falls through to the usual
	// sizes rather than taking 256 KB of internal memory.)
	if (psramFound())
		{
		static const size_t kPsramSizes[] = { 256 * 1024, 128 * 1024 };
		for (size_t i = 0; i < sizeof(kPsramSizes) / sizeof(kPsramSizes[0]); i++)
			{
			uint8_t* p = (uint8_t*)heap_caps_malloc(kPsramSizes[i], MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
			if (p)
				{
				aSize = kPsramSizes[i];
				return p;
				}
			}
		}
#endif
	static const size_t kSizes[] = { 128 * 1024, 96 * 1024, 64 * 1024, 48 * 1024, 32 * 1024 };
	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++)
		{
		uint8_t* p = (uint8_t*)malloc(kSizes[i]);
		if (p && ESP.getFreeHeap() >= kKeepFree)
			{
			aSize = kSizes[i];
			return p;
			}
		free(p);
		}
	// short of memory: the proxy needs at least 16 KB, plain calls work with less
	for (aSize = 16 * 1024; aSize >= 4 * 1024; aSize /= 2)
		{
		uint8_t* p = (uint8_t*)malloc(aSize);
		if (p)
			{
			if (aSize < am::Proxy::kSlack + 4 * 1024)
				Serial.printf("Atom modem: only %u bytes for the buffer: the web proxy will not work\n", (unsigned)aSize);
			return p;
			}
		}
	// nothing at all: there is no modem without it, so start again
	Serial.println("Atom modem: no memory for the buffer; restarting");
	Serial.flush();
	delay(1000);
	ESP.restart();
	return 0;                                 // (not reached)
	}

// Hold the button while plugging in (3 s, the LED flashes white): every
// setting, the WiFi network too, back to the factory ones. (The USB port is
// then a device again, so the board can be programmed.)
static void FactoryResetIfHeld()
	{
	pinMode(kPinButton, INPUT_PULLUP);
	if (digitalRead(kPinButton) != LOW)
		return;
	uint32_t start = millis();
	while (digitalRead(kPinButton) == LOW)
		{
		uint8_t v = ((millis() / 150) & 1) ? 32 : 0;
		LedWrite(v, v, v);
		if (millis() - start > 3000)
			{
			AtomHal::NvsClear();
			Serial.println("Factory reset");
			LedWrite(32, 32, 32);
			delay(1000);
			return;
			}
		delay(10);
		}
	}

// a short press of the button while running: config mode (the access
// point and the pages for ten minutes)
static void ButtonTick()
	{
	static bool was = false;
	static uint32_t downMs = 0;
	bool down = digitalRead(kPinButton) == LOW;
	if (down && !was)
		downMs = millis();
	if (!down && was && millis() - downMs > 30 && millis() - downMs < 2000)
		gModem->ConfigMode(!gModem->InConfigMode());
	was = down;
	}

void setup()
	{
	Serial.begin(115200);                     // USB: status messages only
#if ARDUINO_USB_CDC_ON_BOOT
	Serial.setTxTimeoutMs(0);                 // (never wait for a PC that is not there: host mode)
#endif
	FactoryResetIfHeld();
	WiFi.mode(WIFI_STA);                      // (WiFi takes its memory before the ring does)
	size_t size;
	uint8_t* ring = AllocRing(size);
	Serial.printf("Atom modem %s on the %s: buffer %u bytes, %u KB free\n", am::kVersion, kBoardName, (unsigned)size,
		(unsigned)(ESP.getFreeHeap() / 1024));
#if defined(BOARD_HAS_PSRAM)
	// (the figures the AtomS3R should show: 8192 KB of PSRAM, less the ring)
	if (psramFound())
		Serial.printf("PSRAM: %u KB, %u KB free\n", (unsigned)(ESP.getPsramSize() / 1024),
			(unsigned)(ESP.getFreePsram() / 1024));
	else
		Serial.println("PSRAM: not found; running from internal memory only");
#endif
	gHal.BeginTls();
	gHal.SetWeb(&gWeb);
	gHal.SetUsb(&gUsb);
	gModem = new am::Modem(gHal, ring, size);
	gWeb.Begin(*gModem, gHal);
	gModem->Begin();                          // (loads the settings; starts WiFi, the pages, the USB host)
	gModem->SeedNetwork(AM_DEFAULT_SSID, AM_DEFAULT_PASS);   // (first start only; empty in a plain build)
	gHal.Running();
	char m[160];
	snprintf(m, sizeof(m), "Modem, proxy and web pages: %u bytes; %u KB free, largest block %u KB; serial TX %d RX %d%s",
		(unsigned)sizeof(am::Modem), (unsigned)(ESP.getFreeHeap() / 1024),
		(unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024), gHal.PinTx(), gHal.PinRx(),
		gHal.Flow() ? ", RTS/CTS" : "");
	gHal.Log(m);
	}

void loop()
	{
	static bool wasUp = false;
	gModem->Loop();
	gWeb.Loop();
	ButtonTick();
	bool up = WiFi.status() == WL_CONNECTED;
	if (up != wasUp)
		{
		wasUp = up;
		char m[80];
		if (up)
			snprintf(m, sizeof(m), "WiFi: connected, IP %s", WiFi.localIP().toString().c_str());
		else
			snprintf(m, sizeof(m), "WiFi: down");
		gHal.Log(m);
		}
	yield();
	}
