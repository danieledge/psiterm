// settings.h - the Atom modem's settings, as saved in NVS by AT&W.
// Plain C++ with no Arduino calls (the Atom and the host tests share it).
// MIT licence (see LICENSE at the top of the repository).
//
// Two records:
//   Settings  ("cfg", 140 bytes): everything 1.x had. Its size and layout
//             never change, so a modem updated from 1.x keeps its settings.
//   Settings2 ("cfg2", 256 bytes): what 2.0 added. Versioned, with spare
//             bytes; new fields are appended and the size never changes. A
//             missing or short record gives the factory values.
// Secrets (the WiFi, access point and web passwords, the exec token) live
// in these records in NVS only: nothing prints them, and the repository
// holds none of them.
#ifndef ATOM_SETTINGS_H
#define ATOM_SETTINGS_H

#include <stdint.h>
#include <stddef.h>

namespace am {

static const char* const kVersion = "2.0";

// ----- the 1.x record ------------------------------------------------------
struct Settings
	{
	uint32_t magic;            // kMagic: a valid record
	uint32_t baud;             // the serial line to the Psion
	char ssid[33];
	char pass[65];
	uint8_t echo;              // E1: echo commands back (as the WiRSa does)
	uint8_t verbose;           // V1: words ("OK"), V0: digits ("0")
	uint8_t quiet;             // Q1: no result codes at all
	uint8_t dcdMode;           // &C: 0 DCD always on, 1 DCD follows the connection
	uint8_t s2;                // escape character, '+'
	uint8_t s12;               // escape guard time, 1/50 s (40 = 0.8 s)
	uint8_t paceAuto;          // 1: pacing chosen from the baud rate
	uint8_t swapPins;          // 1: RX and TX swapped (a different cable or base)
	uint32_t paceRate;         // bytes/s towards the Psion; 0 = no pacing
	uint16_t paceBurst;        // bytes per burst (the token bucket's depth)
	uint16_t paceGap;          // extra quiet time after each burst, ms
	int8_t dcdPin;             // GPIO driving an emulated DCD, -1 = none
	uint8_t proxy;             // the web proxy (AT$PX), stored so that 0 (a record saved
	                           // by 1.0) means on: 0 on, 1 off, 2 text only, 3 unchanged, 4 reader
	uint8_t proxyNoZip;        // 1: AT$PZ=0, the proxy does not gzip pages for the Psion
	uint8_t spare[13];
	};

static const uint32_t kMagic = 0x41544d31;   // 'ATM1'

// AT$PX's value (Proxy::TMode) from the stored byte, and back
int ProxyMode(const Settings& aS);
void SetProxyMode(Settings& aS, int aMode);

void FactoryDefaults(Settings& aS);

// ----- the 2.0 record ------------------------------------------------------
enum TUplink { EUplinkAuto = 0, EUplinkWifi = 1, EUplinkUsb = 2 };
enum TWeb { EWebOff = 0, EWebOn = 1, EWebApAlways = 2 };

static const int kPinCount = 5;              // tx, rx, rts, cts, dcd
static const int kTlsPortMax = 8;

struct Settings2
	{
	uint32_t magic;            // kMagic2
	uint16_t version;          // kSettings2Version
	uint16_t length;           // sizeof(Settings2) when saved
	uint8_t uplink;            // TUplink
	uint8_t usbHost;           // 1: the USB-C port is a host (a phone or adapter); 0 a device
	uint8_t flow;              // 1: RTS/CTS in the UART hardware; 0 none (pacing only)
	uint8_t flowSwap;          // 1: the RTS and CTS pins the other way round
	int8_t pins[kPinCount];    // -1: the board's default
	uint8_t web;               // TWeb
	uint8_t tls;               // 1: dials to tlsPorts are made over TLS by the Atom
	uint8_t tlsVerify;         // 1: the server's certificate is checked (0 is dangerous)
	uint8_t img;               // 1: pictures through the proxy become 16-grey GIFs
	uint16_t imgWidth;         // ... at most this wide
	uint16_t imgMaxKB;         // ... if the file is at most this big
	uint8_t exec;              // 1: psiexec and AT$EXEC allowed
	uint8_t logLevel;          // 0 quiet, 1 normal, 2 verbose
	uint16_t tlsPorts[kTlsPortMax];   // 0 ends the list
	char webPass[33];          // HTTP basic auth (user "atom"); empty = station pages off
	char apPass[33];           // the access point's WPA2 password
	char execHost[64];         // host:port of psiexecd
	char execToken[33];
	char chkHost[40];          // host:port the Internet check connects to; empty = no check
	uint8_t ppp;               // 1: a numeric dial (ATD777) brings up PPP (the Psion's
	                           // "Psion Internet" route shares the modem's link, NAT'd)
	uint8_t spare[256 - 4 - 2 - 2 - 4 - 5 - 1 - 3 - 1 - 2 - 2 - 1 - 1 - 16 - 33 - 33 - 64 - 33 - 40 - 1];
	};

static const uint32_t kMagic2 = 0x41544d32;  // 'ATM2'
static const uint16_t kSettings2Version = 1;

void FactoryDefaults2(Settings2& aS);
// a record read from NVS: valid, with missing fields (an older, shorter
// record) at their factory values. False: not a record at all
bool AcceptSettings2(Settings2& aS, size_t aLength);
// the ports as "443,465,993,995", and back (false: not a list of ports)
void TlsPortsText(const Settings2& aS, char* aOut, size_t aMax);
bool SetTlsPorts(Settings2& aS, const char* aText);
bool TlsPort(const Settings2& aS, uint16_t aPort);
// "6,5,7,8,38" (-1 for a default), and back
void PinsText(const Settings2& aS, char* aOut, size_t aMax);
bool SetPins(Settings2& aS, const char* aText);

// The pacing that suits a Psion 5mx at this line rate (see README). With
// RTS/CTS the Psion can stop us, so the rate is higher; the 16-byte bursts
// stay, because the UART FIFO is what a late interrupt overruns
void AutoPacing(uint32_t aBaud, bool aFlow, uint32_t& aRate, uint16_t& aBurst, uint16_t& aGap);

// the pair, as the schema and the web pages see them
struct Config
	{
	Settings& s;
	Settings2& s2;
	};

} // namespace am

#endif
