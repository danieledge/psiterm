// boards.h - the pins of each board the firmware runs on. See
// docs/UPGRADE.md, sections 1 and 3: every one of the UART pins must go
// through an RS-232 transceiver before it reaches the Psion.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_BOARDS_H
#define ATOM_BOARDS_H

#if defined(AM_BOARD_ATOMS3_LITE)
// M5Stack AtomS3 Lite (ESP32-S3). The bottom header: G5 G6 G7 G8 G38 G39.
// The Atomic RS232 Base puts its transceiver on G5 (RX) and G6 (TX); a
// MAX3232 on the Proto Kit takes the same two plus G7/G8 for RTS/CTS.
static const char* const kBoardName = "AtomS3 Lite";
static const int kPinRx = 5;              // from the transceiver's receiver (the Psion's TXD)
static const int kPinTx = 6;              // to the transceiver's driver (the Psion's RXD)
static const int kPinRts = 7;             // out: to the Psion's CTS, through a driver
static const int kPinCts = 8;             // in: the Psion's RTS, through a receiver
static const int kPinLed = 35;            // the WS2812
static const int kPinButton = 41;
static const bool kUsbHostPossible = true;
#elif defined(AM_BOARD_ATOMS3R)
// M5Stack AtomS3R (ESP32-S3-PICO-1-N8R8: 8 MB flash, 8 MB octal PSRAM). The
// same bottom header as the AtomS3 Lite (G5 G6 G7 G8 G38 G39), so the
// Atomic RS232 Base and the Proto Kit wiring are the same. There is no
// WS2812: its light is an LP5562 driver on the internal I2C bus (SCL G0,
// SDA G45, shared with the BMI270), which this firmware does not drive,
// so the status LED is a no-op here. The 128x128 LCD (GC9107, SPI) is not
// used. The pins here are from M5Unified's board table; not yet confirmed
// on hardware.
static const char* const kBoardName = "AtomS3R";
static const int kPinRx = 5;              // from the transceiver's receiver (the Psion's TXD)
static const int kPinTx = 6;              // to the transceiver's driver (the Psion's RXD)
static const int kPinRts = 7;             // out: to the Psion's CTS, through a driver
static const int kPinCts = 8;             // in: the Psion's RTS, through a receiver
static const int kPinLed = -1;            // no WS2812 (see above)
static const int kPinButton = 41;
static const bool kUsbHostPossible = true;
#else
// M5Stack Atom Lite / Matrix (ESP32), as 1.x. Header pins for RTS/CTS if a
// four-wire transceiver is fitted: G23 and G33 are free.
static const char* const kBoardName = "Atom Lite";
static const int kPinRx = 22;
static const int kPinTx = 19;
static const int kPinRts = 23;
static const int kPinCts = 33;
static const int kPinLed = 27;
static const int kPinButton = 39;
static const bool kUsbHostPossible = false;
#endif

static const bool kDcdActiveLow = true;   // through an inverting RS-232 driver (MAX3232)

// the WS2812 status LED (the call's name changed between the cores); nothing
// on a board without one (kPinLed < 0)
#include <Arduino.h>
inline void LedWrite(uint8_t aR, uint8_t aG, uint8_t aB)
	{
	if (kPinLed < 0)
		return;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
	rgbLedWrite(kPinLed, aR, aG, aB);
#else
	neopixelWrite(kPinLed, aR, aG, aB);
#endif
	}

#endif
