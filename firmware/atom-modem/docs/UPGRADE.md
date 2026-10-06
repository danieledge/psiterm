# Atom modem 2.0: the AtomS3 Lite build

This document is the engineering record for the 2.0 firmware: what the
hardware can and cannot do, how the modem is wired to the Psion, how the
firmware is put together, every AT command, the web pages, how to flash it
and how to test it with the Psion. The user guide is `../README.md`.

Contents

1. [Electrical safety](#1-electrical-safety-read-first)
2. [Hardware feasibility](#2-hardware-feasibility)
3. [Wiring](#3-wiring)
4. [Architecture](#4-architecture)
5. [Settings and NVS](#5-settings-and-nvs)
6. [AT command reference](#6-at-command-reference)
7. [The web pages](#7-the-web-pages)
8. [The uplinks](#8-the-uplinks)
9. [The hand-off features](#9-the-hand-off-features)
10. [Building, flashing and the host tests](#10-building-flashing-and-the-host-tests)
11. [Testing with the Psion](#11-testing-with-the-psion)
12. [What is deferred, and why](#12-what-is-deferred-and-why)
13. [Sources](#13-sources)

---

## 1. Electrical safety: read first

**The Psion's serial port is true RS-232. Its signals swing between about
−12 V and +12 V. The ESP32-S3's pins are 3.3 V logic and are not tolerant of
5 V, let alone ±12 V. Connecting the Psion's TXD, RXD, RTS, CTS, DTR, DSR,
DCD or RI to an ESP32 pin, directly or through a breakout that only passes
the wires on, will destroy the ESP32-S3 and may damage the Psion's port.**

Two kinds of serial exist in this build and the words are used strictly:

- **TTL UART**: the ESP32-S3's own serial pins (G5, G6, G7, G8 and so on).
  0 V and 3.3 V. Everything on the AtomS3 Lite and on the ATOMIC Proto Kit is
  TTL UART.
- **RS-232**: the Psion's port, the DE-9 socket on its cable, and the
  terminal block of a DE-9 breakout. Bipolar, up to ±12 V, with inverted
  logic (a 1 is negative).

**An RS-232 transceiver is mandatory hardware.** Every signal between the
Psion and the ESP32-S3 must pass through a MAX232-family transceiver (a
MAX3232 at 3.3 V, or the one inside the M5Stack Atomic RS232 Base). The
transceiver's RS-232 side goes to the DE-9; its TTL side goes to the ESP32.
The two sides never touch.

**The ATOMIC Proto Kit has no transceiver.** It is a prototyping board that
brings the AtomS3 Lite's pins out to pads and a header. It converts no
voltages. A DE-9 breakout board has no transceiver either; it is a socket on
a terminal block. If you have the Proto Kit and a DE-9 breakout and no
MAX3232, **do not connect the Psion yet**. Either add a MAX3232 module to the
Proto Kit as described in [Wiring](#3-wiring), or use the Atomic RS232 Base
instead (which is the transceiver, with only TXD, RXD and ground).

Before power is applied, with everything unplugged:

1. Check the wiring against `HARDWARE.md` section 4 (Base T → DE-9 pin 2,
   R → pin 3, G → pin 5; the same null-modem cable as with the WiRSa; the
   DE-9 pin numbers are printed on the socket).
2. Check that no DE-9 pin has continuity to any ESP32 pin, to the Proto
   Kit's 3V3 or 5V pads, or to the USB-C connector.
3. Check that the transceiver's VCC is 3.3 V (a MAX3232 or MAX3243, not a
   5 V MAX232), and that it is wired as its datasheet shows (the four charge
   pump capacitors, V+ and V− decoupled).
4. Check that the Proto Kit's 5 V and the USB-C VBUS go nowhere near the
   DE-9.

**Acceptance gate for the prototype:** no Psion serial connection is powered
or tested until the transceiver and the wiring have been checked as above.
This applies to every mode of the firmware: the modem itself, the hand-off
features, and anything added later (keyboard emulation or USB HID bridging
would use the same UART and the same rule). The firmware cannot protect the
hardware from a wiring mistake; only the transceiver does.

## 2. Hardware feasibility

### The board

The **M5Stack AtomS3 Lite** is an ESP32-S3FN8: dual-core at 240 MHz, 8 MB of
flash, **no PSRAM**, about 400 KB of SRAM usable by a program after the
system's own. It has hardware AES, SHA and RSA accelerators; one USB-C
connector on the ESP32-S3's native USB (which can be a USB device, which is
how it is programmed, or a **full-speed USB host**, 12 Mbit/s); a WS2812
RGB LED on G35; a button on G41; a Grove socket (G1, G2); and the bottom
header shared by the whole Atom family: **G5, G6, G7, G8, G38, G39, 5V, 3V3,
GND**. It has no screen.

The Atomic bases (RS232 Base, Proto Kit) plug into that bottom header, so
the Psion link uses header pins, not the USB-C: putting the USB-C into host
mode for a phone does not touch the Psion link. M5Stack's own RS232 Base
tutorial for the AtomS3 family puts the transceiver on **G5 (RX) and G6
(TX)**, and this firmware uses those by default.

### What was asked for, and what the board can do

| Feature | Verdict | Reason |
|---|---|---|
| WiFi station uplink: a home network, or an **iPhone's Personal Hotspot** | **Works** | The firmware has done this since 1.0. The hotspot is an ordinary WPA2 network to the ESP32. This is the path that gives the Psion the iPhone's internet today. |
| USB host on the ESP32-S3 | **Works** | ESP-IDF's `usb_host` library (stable since IDF 5.x). Full speed is far more than the Psion's serial line can use. The one USB-C connector is then the phone's, so programming and the USB console need the port back: see [Flashing](#10-building-flashing-and-the-host-tests). |
| **Wired iPhone tethering** | **Experimental and unsupported in this release.** The USB side is implemented and compiles; Apple's pairing is not implemented, so the phone's network interface reports no carrier. | An iPhone's USB tethering is not the standard CDC-ECM/NCM class. It is Apple's own interface (class `0xFF`, subclass `0xFD`, protocol `1`: what Linux's `ipheth` driver talks to): Ethernet frames with a two-byte header on a bulk pair, and the carrier reported by a vendor control request. **The carrier is only reported up once the host has paired with the phone** ("Trust This Computer"): the `usbmuxd` multiplexer, `lockdownd`'s plist messages, a TLS session with a host certificate, a pair record. That is implementable on the S3 (mbedTLS, NVS and the host stack are there) but is a project of its own, and it is not done; `USB-TETHERING.md` lists the ten concerns with the status of each and what completing pairing requires. The firmware never selects USB by itself (the host starts only with `AT$USB=1`, and AUTO routes through USB only once a device has an address), so this cannot destabilise the WiFi path. The **supported iPhone path is its Personal Hotspot over WiFi.** |
| Android USB tethering, CDC Ethernet dongles | **Works in principle (untested here)** | Android offers NCM (recent versions) or RNDIS (older ones) when USB tethering is turned on, with no pairing. NCM and ECM are implemented; RNDIS is not (section 12). |
| Powering the phone from the Atom | **Barely; and it needs an external supply** | The AtomS3 Lite has no VBUS switch or source: the USB-C's 5 V is an *input* that feeds the board. In host mode, 5 V has to be fed into the header's **5V** pin from a separate supply, and it reaches the USB-C VBUS pin only if the board joins those two nets directly (the Atom Lite does; the AtomS3 Lite is believed to, but **measure it** before relying on it). The phone takes up to 1 A when VBUS is present and the ESP32 takes 300 mA in bursts: use a 2 A supply. A phone on a USB-C to USB-C cable (iPhone 15 and later) also needs the host to pull its CC line up (an Rp of 56 kΩ to 5 V); the AtomS3 Lite, built as a USB device, has the pull-downs instead, so such a phone will not even enumerate without an adapter that adds Rp. A Lightning phone on a USB-C to Lightning cable needs nothing of the kind. In every case the phone charges slowly at best. |
| RTS/CTS hardware flow control | **Works, with a transceiver that carries four signals** | The ESP32's UART does RTS/CTS in hardware. The Atomic RS232 Base has only TXD and RXD, so it is pacing only; a MAX3232 on the Proto Kit has the two extra channels for RTS and CTS. |
| Serial at 230400, 460800, 921600 | **Works at the Atom's end; the Psion's end is the limit** | `AT$SB` has done these since 1.1, with a 15 s fall-back if nothing answers at the new speed. The 5mx's UART divider is a plain divisor of 7.3728 MHz, and `PsiKernTest` has set it to 230400 (`UBRCR` 1) on Dan's machine. 460800 (`UBRCR` 0) is untested on the level shifter inside the Psion. 921600 is beyond the divider: it is listed because the Atom accepts it, not because the Psion can reach it. |
| TLS termination (plain IMAP/SMTP/HTTP from the Psion, TLS from the Atom) | **Works** | `WiFiClientSecure` with the Mozilla roots, as the web proxy already does. Hardware AES on the S3. |
| Pictures scaled and dithered to 16 greys on the Atom | **Works, within fixed limits** | PsiMail's own decoders (`mail/engine/img`) run on the Atom: JPEGs decoded at 1/8 from the DC terms when big, PNG and GIF a row at a time. Interlaced files and very large pictures are passed through unchanged rather than buffered, because there is no PSRAM. |
| Page simplification / readability | **Streaming heuristics only** | A proper readability pass needs the whole page in memory; the BBC front page is 950 KB. The simplifier is a state machine with 5 KB of buffers, so "reader" mode is a set of rules applied as the page streams (section 9). |
| `AT$EXEC` remote compute | **Works, with a helper on the LAN** | The Atom relays text to a small program on a PC that runs allowed commands. Off unless turned on. |
| Memory | **The real constraint** | WiFi takes about 60 KB, the USB host stack 25 KB plus transfer buffers, a TLS connection 50 KB while open, the modem with the proxy 25 KB, the gzip writer 16 KB, the picture decoder up to 40 KB, and the ring towards the Psion the rest (32 to 128 KB). The features are used one at a time (the modem handles one call at a time), the USB host is only started when the USB uplink is chosen or `AUTO` finds a device, and the ring is sized last, as before. `ATI` shows what is free. |

## 3. Wiring

**The primary build is the Atomic RS232 Base**, which has the transceiver:
`HARDWARE.md` is the wiring guide for it (the reversible adapter chain to
the Psion's unmodified cable, what is known and unknown about the cable's
pinout and how to measure it, power, and the pre-test checklist). Read
section 1 and `HARDWARE.md` first.

What follows is the **four-wire option** (a MAX3232 module on the Proto
Kit, for RTS/CTS and DCD). It is not needed for the modem, the hotspot or
any feature, it has not been built, and the Proto Kit itself must never
carry a Psion signal. The chain for it is:

```
 iPhone / Android / USB Ethernet
        |  USB-C (the AtomS3 Lite as USB host; 5 V fed from the header)
        v
 +----------------+  TTL UART (3.3 V)   +----------------+  RS-232 (±12 V)  +------------------+
 |  AtomS3 Lite   | G6 TXD ---------->  | MAX3232 T1IN   | T1OUT ---------> | DE-9 breakout    |
 |  (bare header) | G5 RXD <----------  |         R1OUT  | R1IN  <--------- | (terminal block) |
 |                | G7 RTS ---------->  |         T2IN   | T2OUT ---------> | pin 8 (DCE CTS)  |
 |                | G8 CTS <----------  |         R2OUT  | R2IN  <--------- | pin 7 (DCE RTS)  |
 |                | GND --------------  | GND            | GND ------------ | pin 5            |
 |                | 3V3 --------------  | VCC (3.3 V!)   |                  |                  |
 +----------------+                     +----------------+                  +---------+--------+
                                                                                      |
                                                                           Psion serial cable
                                                                                      |
                                                                              Psion Series 5mx
```

The transceiver is the wall between the two voltages. Nothing crosses it
except through its own drivers and receivers.

### The transceiver

A **MAX3232** (3.3 V supply, two drivers, two receivers) is the right part
for TXD, RXD, RTS and CTS. Ready-made modules ("MAX3232 RS232 to TTL
converter", with the capacitors fitted) are fine as long as the module is
rated for 3.3 V logic on its TTL side; power it from the Proto Kit's **3V3**
pad, not 5V. For DCD as well (and DSR, DTR) use a second MAX3232, or a
**MAX3243** (three drivers, five receivers, 3.3 V).

The Atomic RS232 Base is a transceiver too, and its two channels are
already wired to G5 and G6. With it there is no RTS/CTS and no DCD, and the
firmware's pacing does the flow control, as in 1.x.

### Which ESP32 pins

| AtomS3 Lite | Signal | Direction | Transceiver | DE-9 pin | Psion signal |
|---|---|---|---|---|---|
| G6 | TXD | out | driver 1 | 2 | data **into** the Psion (its RXD) |
| G5 | RXD | in | receiver 1 | 3 | data **from** the Psion (its TXD) |
| G7 | RTS | out | driver 2 | **8** (a DCE's CTS output) | the Psion's CTS input ("you may send"), through the null-modem cable's 7↔8 crossover |
| G8 | CTS | in | receiver 2 | **7** (a DCE's RTS input) | the Psion's RTS output ("I can take more"), likewise |
| G38 | DCD (optional) | out | driver 3 (second chip) | 1 | the Psion's DCD input |
| GND | | | GND | 5 | signal ground |
| 3V3 | | | VCC | | **never to the DE-9** |
| 5V | USB host power only | | | | **never to the DE-9** |

- Pins 2 and 3 follow the DCE convention of the WiRSa and of the Base's
  own build (TX to 2, RX to 3), so the same null-modem cable applies; see
  `HARDWARE.md` §3–4 and its sources. `AT$SWAP=1` swaps G5 and G6 for a
  breakout wired the other way round; `AT$FCSWAP=1` swaps G7 and G8.
- Pins 7 and 8: only with `AT$FC=1`. With the RS232 Base, or no flow
  control, bridge pins 7 and 8 on the DE-9 so the Psion sees its own RTS
  come back as CTS if "RTS/CTS on" is ever chosen in the apps.
- Pin 1 (DCD) is optional: it lets the apps notice a dropped call at once
  (`KConfigFailDCD`). `AT$DCD=38 AT&C1`. Without it the in-band `NO
  CARRIER` is used, which works well. (`AT$DCD`, the 1.x setting, is the
  DCD pin; the fifth number of `AT$PINS` is used only when `AT$DCD` is -1.)
- Polarity: the MAX3232 inverts. The ESP32's RTS is active-low and its CTS
  expects active-low, which the transceiver turns into RS-232's "asserted =
  positive". No software inversion is needed on either control line.
- `AT$PINS=tx,rx,rts,cts,dcd` moves any of these to another header pin (G5,
  G6, G7, G8, G38, G39; G1 and G2 on the Grove socket).

### USB and power

- The AtomS3 Lite is normally powered from its USB-C. In host mode the
  USB-C is the phone's, so power the board through the Proto Kit's **5V**
  pad from a 5 V, 2 A supply. That same 5 V is what the phone sees on VBUS
  if the board joins the nets (measure between the 5V pad and the USB-C
  VBUS pin with nothing plugged in: 0 Ω means yes).
- Never route that 5 V, or 3V3, towards the transceiver's RS-232 side or
  the DE-9.
- A USB-C to USB-C cable to a modern iPhone needs Rp on CC; see section 2.

## 4. Architecture

The 1.x split stays: everything the modem *does* is plain C++ with no
Arduino calls, compiled identically for the ESP32 and for the host tests;
one HAL class per platform does the I/O.

```
 src/
   modem.cpp/.h      the modem: AT parser, +++ escape, dial, ring, pacing, baud trial
   settings.cpp/.h   the two NVS records (Settings: 140 bytes as in 1.x; Settings2: new)
   schema.cpp/.h     one table of every setting: AT name, type, range, help, get/set
                     -> drives both the AT$ parser and the web pages (parity by construction)
   uplink.cpp/.h     the uplink manager: AUTO / WIFI / USB, fallback, status
   proxy.cpp/.h      the web proxy for PsiWeb (TLS, redirects, gzip, pictures, reader mode)
   htmlsimp.cpp/.h   the streaming HTML simplifier (modes 1, 2 and the new 4 "reader")
   gzip.cpp/.h       the streaming gzip writer
   imgconv.cpp/.h    pictures to 16-grey GIF, on top of mail/engine/img (pmimg)
   exec.cpp/.h       psiexec: the text channel to a helper on the LAN, and AT$EXEC
   usbnet/           ESP32-S3 only: the USB host task and the network class drivers
     usbhost.cpp       enumeration, interface claim, transfers
     ncm.cpp/.h        CDC-NCM NTB16 parse and build (pure functions, host-tested)
     ecm.cpp           CDC-ECM
     ipheth.cpp/.h     Apple's vendor interface: config, carrier check, 2-byte frames
     netif.cpp         esp_netif glue: frames in and out of lwIP, DHCP client
   board/
     hal_esp.cpp       the Arduino/ESP32 HAL: UART with RTS/CTS, WiFi, TCP, TLS,
                       NVS, LED, button, the RAM log, the web server
     webui.cpp         the pages and /api, built from the schema
     boards.h          pin tables: m5stack-atom (G22/G19, LED 27, button 39) and
                       atoms3-lite (G5/G6/G7/G8/G38, LED 35, button 41)
   main.cpp          setup() and loop()
 hosttest/           the PC tests (fakehal.h) and hostmodem for the emulator
 tools/              mkcabundle.py, psiexecd.py (the AT$EXEC helper)
```

Everything runs in `loop()`, one pass at a time, as before: serial in,
escape timing, server pump, paced serial out, baud trial, web server
`handleClient()`, uplink tick. The USB host library has its own task (it
must), and hands Ethernet frames to lwIP, which has its own task too; the
modem only ever sees sockets, as it does with WiFi.

The web server is the Arduino core's own `WebServer` with `DNSServer` for
the captive portal, not an asynchronous one. Reasons: no third-party
library to pin, about 10 KB less RAM, and its handlers run inside `loop()`
on the same thread as the modem, so a settings change from a browser can
never race the AT parser. A page is a few kilobytes and takes milliseconds
to send, which the paced serial output does not notice.

## 5. Settings and NVS

Two records in the NVS namespace `atommodem`:

- **`cfg`**, 140 bytes, unchanged from 1.x (`static_assert` in the tests).
  A modem updated from 1.x keeps every setting.
- **`cfg2`**, 256 bytes, new: magic `ATM2`, a version and a length, then
  the fields below with spare bytes at the end. A missing or short record
  gives the factory values for everything in it. New fields are appended;
  the size never changes.

The `ppp=1` default applies only to fresh NVS. A board that already has
saved settings keeps `ppp=0`: on an updated board run `AT$PPP=1` then `AT&W`.

| Field | Default | AT | Notes |
|---|---|---|---|
| uplink | AUTO | `$UP` | AUTO prefers USB when a network device is present and up, else WiFi |
| usbMode | 0 device | `$USB` | 1 = host |
| flow | 0 none | `$FC` | 1 = RTS/CTS |
| flowSwap | 0 | `$FCSWAP` | |
| pins tx,rx,rts,cts,dcd | board defaults | `$PINS` | −1 = the board's default |
| web | 1 | `$WEB` | 0 off; 1 on (portal when unconfigured, station pages with a password); 2 the access point always |
| webPass | (none) | `$WEBPASS` | HTTP basic auth, user `atom`. No password: station-mode pages are off |
| apPass | generated | `$APPASS` | WPA2 for the access point; shown by ATI until changed |
| tls | 0 | `$TLS` | TLS termination for dials to the ports in tlsPorts |
| tlsPorts | 443,465,993,995 | `$TLSP` | |
| tlsVerify | 1 | `$TLSV` | 0 is dangerous: see section 9 |
| img | 0 | `$PI` | pictures through the proxy are converted |
| imgWidth | 300 | `$PW` | |
| imgMaxKB | 64 | `$PM` | bigger pictures pass through unchanged (the file is held whole while it is decoded) |
| exec | 0 | `$XE` | |
| execHost | (none) | `$XH` | host:port of psiexecd |
| execToken | (none) | `$XK` | shown as set/none only |
| log | 1 | `$LOGL` | 0 quiet, 1 normal, 2 verbose |
| chkHost | 1.1.1.1:53 | `$CHK` | the Internet check's target; empty = no check |

`AT&W` saves both records. `AT&F` resets both but keeps the WiFi network,
as before. `AT$RESET=YES`, the web page's *Factory reset*, and holding the
button while powering up clear both records.

**No secret ever leaves the device.** The WiFi password, the access point
password, the web password and the exec token live in NVS only. `AT$PASS?`,
`AT$APPASS?`, `AT$WEBPASS?` and `AT$XK?` say *(set)* or *(none)*; the web
pages never fill them into forms; `AT&V` and `ATI` never print them; the
repository contains none of them.

## 6. AT command reference

Everything in the 1.x reference (`../README.md`) still works unchanged:
`AT`, `ATDT host:port`, `ATDT psiproxy:8080`, `+++`, `ATO`, `ATH`, `ATE`,
`ATV`, `ATQ`, `ATI`, `ATZ`, `AT&F`, `AT&W`, `AT&V`, `AT&C`, `ATS2`, `ATS12`,
`AT$SSID`, `AT$PASS`, `ATW`, `ATC`, `AT$SB`/`ATB`, `AT$PR`, `AT$PB`,
`AT$PG`, `AT$PACE?`, `AT$SWAP`, `AT$DCD`, `AT$PX`, `AT$PZ`.

New in 2.0. Every `AT$X=value` has an `AT$X?`; settings last until
power-off unless `AT&W` saves them.

| Command | Does |
|---|---|
| `AT$HELP` | Lists every `AT$` setting with its range and current value (from the schema) |
| `AT$UP=AUTO`, `WIFI`, `USB` | Which uplink carries the Psion's connections. `AT$UP?` also says which is in use and why |
| `AT$USB=0/1` | The USB-C port: 0 a device (programming, the USB console), 1 a host (a phone or adapter; experimental, see `USB-TETHERING.md`). Takes effect at the next restart; `AT$USB=1` with `AT&W` and `ATZ` |
| `AT$CHK=host:port` | The Internet check's target (default `1.1.1.1:53`, a TCP connect with no DNS); empty turns the check off. See section 8 |
| `AT$FC=0/1` | Flow control to the Psion: 0 none (pacing only, the RS232 Base), 1 RTS/CTS in the UART hardware. AUTO pacing then allows a higher rate |
| `AT$FCSWAP=0/1` | Swaps the RTS and CTS pins |
| `AT$PINS=tx,rx,rts,cts,dcd` | The header pins (−1 = the board's default). `AT$PINS?` shows them |
| `AT$WEB=0/1/2` | The web pages: 0 off, 1 on, 2 the access point always on |
| `AT$WEBPASS=text` | The web password (user `atom`) |
| `AT$APPASS=text` | The access point's WPA2 password (8 to 63 characters) |
| `AT$AP?` | The access point's name (`AtomModem-xxxx`) and whether it is up |
| `AT$TLS=0/1` | TLS termination: a dial to a port in `$TLSP` is made over TLS by the Atom. `ATDT tls:host:port` does it for one dial whatever the port |
| `AT$TLSP=443,465,993,995` | The ports `$TLS` applies to (up to 8) |
| `AT$TLSV=0/1` | Check the server's certificate (1). 0 only for a server of your own with a self-signed certificate |
| `AT$PI=0/1` | Pictures through the web proxy are scaled and dithered to 16 greys |
| `AT$PW=n` | ... to at most n pixels wide (64 to 640, default 300) |
| `AT$PM=n` | ... if the file is at most n KB (16 to 512, default 96); bigger ones pass through |
| `AT$PX=4` | The proxy's reader mode (with 0 to 3 as before) |
| `AT$XE=0/1` | Remote compute allowed |
| `AT$XH=host:port` | Where psiexecd runs |
| `AT$XK=token` | The shared token psiexecd expects |
| `AT$EXEC=command` | Runs one command through psiexecd and prints its output, then `OK` (`ERROR` if `$XE=0`, the helper is not reachable, or the token is refused) |
| `ATDT psiexec` | A call that is a text channel to psiexecd (`NO CARRIER` if `$XE=0`) |
| `AT$LOG?` | The last 4 KB of the status log (what the USB console shows when it is available) |
| `AT$LOGL=0/1/2` | How much is logged |
| `AT$RESET=YES` | Factory reset, both records, WiFi included; the modem restarts |

`ATI` now adds the uplink in use, the USB port's mode and what is on it,
flow control, the web pages' address, and TLS/picture/reader/exec states.
`AT&V` prints every setting except secrets.

## 7. The web pages

- **When there is no WiFi network saved**, the Atom starts an access point
  `AtomModem-xxxx` (the last four hex digits of its MAC) with a WPA2
  password: the one set with `AT$APPASS`, or one made at random and logged
  (the USB console, `AT$LOG?`, the pages' WiFi tab), and a captive portal:
  any address opens the pages. Join it with a phone and set the WiFi network;
  the Atom then joins that network and the access point stops.
- **On the network**, the pages are at `http://<ip>/` (the IP is in `ATI`
  and on the USB console), with the web password. With no web password set
  they are off, so a modem that was only ever set up by AT commands exposes
  nothing.
- **A short press of the button** starts "config mode": the access point
  and portal for ten minutes, whatever the settings, so a modem on an
  unreachable network can still be reached.
- `AT$WEB=2` keeps the access point up always.

Pages (one stylesheet, no scripts beyond a few lines, fits a phone):

| Page | Has |
|---|---|
| `/` | Status: uplink, WiFi (network, IP, signal), USB (device or host, what is attached, carrier), serial (baud, flow control, pins), pacing, the current call, features, free memory, uptime, the access point |
| `/wifi` | Scan, pick a network, enter its password (*Join and save*); the access point's password, the web mode and password |
| `/settings` | Every other setting, a form per group: Uplink (mode, USB host), Serial (baud, flow control, swaps, the pin map, DCD, pacing), Web proxy (mode including reader, gzip), TLS termination (on, ports, verify), Pictures (on, width, size), Remote compute (on, host, token), System (log level). The safety note heads the page |
| `/log` | The log ring, refreshed on demand |
| `/system` | Save to flash, restart, factory reset (with a confirmation), the firmware version |
| `/api/settings` | `GET` the settings as JSON (secrets as `true`/`false`). `POST` (form-encoded `s_NAME=value`, any subset; `save=1` to save) applies them and answers `OK` or what was refused. Same auth |

Every field on these pages is one row of the schema, so the web and `AT$`
sets are the same set; a host test walks the schema and checks every entry
answers `AT$X?` and accepts `AT$X=`.

## 8. The uplinks

`uplink.cpp` is a small state machine fed by two facts a tick: WiFi up or
down, USB network device up or down (with a carrier). Modes:

- **AUTO**: USB when it is up, else WiFi. A USB link that drops is left for
  five seconds before WiFi takes over (phones re-enumerate when they
  change mode); a link that comes back is used at the next call, never
  mid-call.
- **WIFI**, **USB**: that one only. `ATDT` answers `NO CARRIER` when it is
  down, and `AT$UP?` says why.

The modem sees whichever interface is lwIP's default route; `esp_netif`
sets the route priority (USB above WiFi when both are up).

### WiFi station (the supported path, the iPhone's hotspot included)

`AT$SSID`, `AT$PASS`, `ATW`, `ATC`, no modem sleep, as 1.x. An iPhone's
Personal Hotspot is a WPA2 network like any other: keep the phone's
hotspot on and the Atom within range.

Being on the network is not the same as having the Internet, so the WiFi
side has four states, shown by `AT$UP?`, `ATI` and the status page and
logged as they change:

| State | Means |
|---|---|
| down | not joined: no network set, out of range, wrong password |
| joined, Internet not confirmed | the station is on the network; the Internet check has not passed (*checking*) or failed (*the check failed*): a hotspot whose phone has no signal, a captive portal |
| joined, Internet reachable | the check passed |
| lost, joining again | the network went after the Internet had been confirmed; the modem rejoins (after 20 s, then every 30 s) for up to 10 minutes before calling it down |

The check (`AT$CHK=host:port`, default a TCP connect to `1.1.1.1:53`, no
DNS needed) runs in a task of its own on the board, never in the modem's
loop and never during a call: soon after joining, every 15 s until it
passes, every 2 minutes after, and again at once after a dial fails to
connect. `AT$CHK=` (empty) turns it off. **Calls are allowed as soon as the
network is joined**, whatever the check says, because a hotspot can block
the check's target and not the real server; the state tells the user why a
dial failed rather than stopping it.

### USB host (ESP32-S3 only)

With `AT$USB=1` saved, the firmware starts the ESP-IDF USB host library at
boot, waits for a device, and reads its descriptors:

| Device | Driver | Status |
|---|---|---|
| CDC-NCM (an Android phone with USB tethering on; NCM adapters) | `ncm.cpp`: NTB16 in and out, one datagram a frame | implemented, untested on hardware |
| CDC-ECM (ECM adapters, some phones) | `ecm.cpp` | implemented, untested on hardware |
| An iPhone (`0x05ac`, interface class `0xFF/0xFD/0x01`) | `ipheth.cpp`: selects the configuration with that interface, claims it, polls the carrier, frames with the two-byte header | the transport is implemented; **the carrier stays down until pairing exists**, see section 12 |
| RNDIS (older Android) | — | not implemented |
| Anything else | — | ignored, logged |

Frames go into lwIP through a custom `esp_netif` driver, and the DHCP
client asks the phone for an address. `AT$UP?` and the status page show the
stage: *no device*, *enumerating*, *no driver for it*, *no carrier
(unpaired?)*, *up: 172.20.10.x*.

Honest notes on this stack:

- **None of the USB host code has run on hardware yet.** It compiles
  against ESP-IDF 5.3's `usb_host` and `esp_netif`, and its framing
  (`ncm.cpp`, `ipheth.cpp`) is unit-tested on the PC; the enumeration,
  interface claiming, control transfers and the lwIP glue are written from
  the IDF documentation and the Linux drivers, and must be expected to need
  a session with a phone and the log before they are right. The modem
  works without them: with `AT$USB=0` (the default) nothing of this runs.
- The Arduino core does not compile in the host library's enumeration
  filter, so a device whose network interface is in another configuration
  (an iPhone's is) gets a `SET_CONFIGURATION` from this code after the
  library has opened it with its first one; the library is not told, which
  is a known grey area.
- ESP-IDF's host library is full speed only (the S3 has no high-speed PHY);
  an NCM phone will negotiate full speed and work.
- Enumeration failures show up as a log line with the error; some phones
  need a second plug-in.
- VBUS and CC are the board's limits, not the library's (section 2).
- The USB console (`Serial`) is gone in host mode. The log ring and
  `AT$LOG?` replace it.

## 9. The hand-off features

Each is a setting of its own, off by default except the proxy, which 1.1
already had on.

### TLS termination (`AT$TLS`)

With `AT$TLS=1`, `ATDT imap.example.com:993` makes the Atom connect over
TLS (because 993 is in `$TLSP`) and present the Psion with the plain
protocol on the line. `ATDT tls:host:port` does the same for one call.
PsiMail is set to **TLS: none** on the account, keeping port 993 (or 465
for SMTP); PsiWeb without a proxy likewise gets `https://` hosts on 443.

The certificate is checked against the Mozilla roots in `src/cabundle.h`
and the host name, as the proxy does; a failure is `NO CARRIER` with the
reason in the log. `AT$TLSV=0` turns the check off; it leaves the
connection open to anyone on the path between the Atom and the server, and
exists only for a server of your own with a self-signed certificate.

**The trade-off:** the Atom sees your mail and pages in plain text, and the
serial cable carries them in plain text. That is acceptable for a modem you
own, on a WiFi network you trust, with a cable on your desk, and for
nothing else. The Psion then does no TLS at all, which saves about a second
a handshake and all the ChaCha20 on a 36 MHz ARM.

### Pictures to 16 greys (`AT$PI`)

Through the web proxy only (it needs to see the HTTP response). A picture
of `image/jpeg`, `image/png` or `image/gif` up to `$PM` KB (64) is gathered
whole, decoded with PsiMail's decoders (`mail/engine/img`: picojpeg for
baseline JPEG, the progressive decoder within its memory limit, PNG through
zlib's inflate, GIF's first frame), shrunk to at most `$PW` pixels wide
(300) by a whole number, dithered to the 16 greys, and sent as a **4-bit
GIF** (LZW, written row by row while the ring has room). Links decodes a
small GIF in a fraction of the time it takes to decode a JPEG, and the file
on the line is a fraction of the size (the test's 800x500 JPEG of 25 KB
becomes a 200x125 GIF of 4.5 KB). Pictures the decoder refuses and pictures
over `$PM` KB pass through unchanged, so a page is never worse than before.
The settings are taken at the dial, like `AT$PX`.

What the decoders can and cannot do without PSRAM:

- A baseline JPEG of any size: decoded at 1/2, 1/4 or 1/8 as needed, one
  MCU row at a time. `PmImgOpts.exact` (new, 0 for PsiMail's own use) makes
  the Atom choose the scale that still gives the width asked, where the
  Psion settles for half of it to save its CPU.
- A **progressive JPEG** keeps its coefficients until the last scan, within
  96 KB: a photo-sized one is decoded at 1/8 from the DC terms alone (the
  test's 800x500 comes out 100 wide).
- A PNG: a row at a time, unless interlaced; interlaced ones need the whole
  picture (8 bits a pixel, 96 KB at most, so about 350x270).
- A **GIF** needs the whole picture in the same way (the decoder only
  streams a frame that is exactly the logical screen): a photo-sized GIF is
  passed through unchanged; small ones (icons, graphics) are converted.

Memory, while a picture is converted: the file (≤ 64 KB), one MCU row of a
JPEG (16 bytes a pixel of width), or up to 96 KB for a whole-picture
format, the 16-grey result (half a byte a pixel), and the LZW dictionary
(20 KB). Freed after each picture. The decode runs inside `loop()`, so the
serial line pauses for its duration (tens to hundreds of milliseconds);
the `+++` timing is unaffected.

### Reader mode (`AT$PX=4`)

Mode 2 (text only) plus rules that drop the furniture of a modern page as
it streams:

- Once `<main>`, `<article>` or `role="main"` has been seen, nothing
  outside it is sent (the page's head and title still are).
- `header`, `nav`, `aside` and `footer`, and anything with
  `role="navigation|banner|contentinfo|complementary"`, are dropped with
  their contents.
- Elements whose `class` or `id` contains one of *nav, menu, sidebar,
  footer, cookie, consent, promo, share, social, related, comments, breadcrumb*
  are dropped with their contents.
- A list (`ul`/`ol`) whose items are nearly all links is dropped (a menu),
  using the simplifier's pending buffer, so a list of article links in the
  body survives if it has text around the links.

It is a heuristic. A page with no `<main>` and odd class names gets mode 2.

### Remote compute (`AT$EXEC`, `ATDT psiexec`)

The Atom cannot run programs, but a PC on the LAN can. `tools/psiexecd.py`
listens on a port, and the Atom relays:

```
Atom -> helper:  PSIEXEC/1 <token> one\r\n        (AT$EXEC=command)
             or  PSIEXEC/1 <token> channel\r\n    (ATDT psiexec)
helper -> Atom:  OK\r\n            (or ERR reason\r\n, and the connection closes)
```

then, for `one`, a single command line: the helper runs it, sends its
output and closes, and the modem prints the output followed by `OK`
(`AT$EXEC=uptime`); the helper caps the output (64 KB) and the time (30 s).
For `channel` (`ATDT psiexec`), a plain text channel both ways, a command a
line, until `exit` or the helper closes it (`NO CARRIER`).
From PsiTerm the latter is a terminal session to whatever the helper
offers: a shell with an allow-list, a chat program, a log tail.

Safety: `AT$XE=1` must be set; the token must match; the helper runs only
commands named in its allow-list, without a shell, with a timeout, as the
user that started it. The token is given to the helper in its environment
(`PSIEXEC_TOKEN`), never on the command line or in the repository. The
channel is plain TCP on your LAN; it is not for use over the Internet.

## 10. Building, flashing and the host tests

### Build

`platformio.ini` has three environments:

| Environment | Board | Platform | USB host |
|---|---|---|---|
| `atoms3-lite` | `m5stack-atoms3` (AtomS3 Lite) | pioarduino `platform-espressif32` 53.03.13 (Arduino-ESP32 3.1.3 on ESP-IDF 5.3), pinned by its release URL | yes |
| `atoms3r` | `m5stack-atoms3` with `memory_type = qio_opi` (AtomS3R: the same SoC, header and Atomic RS232 Base wiring as the Lite, plus 8 MB octal PSRAM; no WS2812, so the status is the 128x128 LCD, through M5Unified, built with `-DAM_HAS_LCD=1`). Not yet tried on hardware | as `atoms3-lite` | yes |
| `m5stack-atom` | `m5stack-atom` (the plain Atom, as 1.x) | `espressif32@6.9.0` (Arduino-ESP32 2.0.17) | compiled out |

On the AtomS3R the core's malloc puts blocks of 4 KB and more in PSRAM, so
the ring (256 KB there), the picture buffers and the gzip buffers leave the
internal memory to WiFi and TLS; interlaced pictures of any size are
converted. The start-up line and `ATI` show the PSRAM figures.

```sh
pip install platformio
cd firmware/atom-modem
pio run -e atoms3-lite              # build
pio run -e atoms3-lite -t upload    # flash over USB-C (device mode)
pio device monitor                  # the USB console, 115200 (device mode only)
```

No libraries beyond the Arduino core are used, so there is nothing else to
pin. The S3 environment uses the `default_8MB.csv` partition table (one
app, no OTA, 3 MB for the program, the rest free).

### A build that joins a known network from its first start

Two macros, **empty in the tree**, can preset the WiFi network at build
time: `AM_DEFAULT_SSID` and `AM_DEFAULT_PASS` (`src/main.cpp`). On the first
start, when no network has been saved yet (the state that would otherwise
start the portal), a non-empty preset is taken, saved to NVS and joined;
afterwards `AT$SSID`/`AT$PASS` and the pages override it as usual, and a
factory reset brings the preset back. The values are given on the command
line only, with the password **from the environment** (the project's
`~/.secrets`), so that it exists nowhere but in the shell that builds and
in the resulting `.bin`:

```sh
. ~/.secrets && [ -n "$WIFI_IOT_SSID" ] && [ -n "$WIFI_IOT_PASSWORD" ] && \
PLATFORMIO_BUILD_FLAGS="-DAM_DEFAULT_SSID=\\\"$WIFI_IOT_SSID\\\" -DAM_DEFAULT_PASS=\\\"$WIFI_IOT_PASSWORD\\\"" \
pio run -e atoms3r -t upload              # or -e atoms3-lite
```

(`WIFI_IOT_SSID` and `WIFI_IOT_PASSWORD` come from Vaultwarden through
`sync-secrets`. A name or password with spaces, quotes or a backslash does not
survive this quoting.) The preset is taken **only when no network is saved**:
on a board that already holds settings, erase them first (`pio run -t erase`,
the factory-reset hold at power-on, or `AT$RESET=YES`), or set the network with
`AT$SSID`/`AT$PASS`. Without a network the modem starts its access point
instead; its **password is shown on the AtomS3R's Setup page** (the third
screen page), and logged (`AT$LOG?`). Check the preset reached the build with the
`strings` line below, using a dummy value if you like. The `.bin` holds the
password: do not share or commit it; build again without the variables for a
clean image.

The three backslashes matter. PlatformIO splits the flags with `shlex`
before it sees them, which strips plain `"..."` quotes; the value then
reaches the compiler unquoted, the build fails, and the compiler's error
message **prints pieces of the password**. Written as `\\\"`, the shell
passes a literal `\"` (the `platformio.ini` idiom for a string define),
which `shlex` keeps as a quote character and PlatformIO re-escapes for the
compiler. Check the result with `strings .pio/build/<env>/firmware.bin |
grep -c "$WIFI_IOT_SSID"` (never grep for the password).

Never put the password in `platformio.ini`, a header, a script or a log; a
`.bin` built this way is itself a credential and must not be committed or
shared. (The object files under `.pio/`, which is ignored by git, contain
it too: `pio run -t clean` removes them.)

### Flashing an AtomS3 Lite that is in USB host mode

In host mode the USB-C is a host port and a PC cannot talk to it. Either:

- `AT$USB=0` from the Psion (or the web page), `AT&W`, `ATZ`; the port is a
  device again and `pio run -t upload` works; or
- force the download mode: with the board powered, **hold the button
  (G41) and keep it held while pressing reset (or re-plugging power) until
  the LED turns green**, then connect the PC and upload. Afterwards press
  reset. This works whatever the firmware has done to the port, because the
  ROM bootloader owns it then.

A factory reset (hold the button through power-up for three seconds, as
before) also puts the port back to device mode.

### Host tests

```sh
make -C hosttest test
```

builds and runs:

- `test_modem`: the 1.x tests plus the 2.0 settings record (size, defaults,
  save and load, a 1.x modem's record being picked up), the schema walk
  (every setting answers `?` and `=`), each new `AT$` command's values and
  errors, `AT&F` and `AT$RESET=YES`, flow control's effect on pacing,
  `ATDT tls:` and `$TLS`/`$TLSP` reaching the fake HAL as TLS, `psiexec`
  and `AT$EXEC` against a fake helper (refused when off, the token, the
  bounds), and the uplink state machine (AUTO's choice, the fall-back
  hold-off, the forced modes).
- `test_usbnet`: the NCM NTB16 builder and parser (round trips, bad
  signatures, lengths past the end, several datagrams in one NTB) and the
  Apple two-byte framing split across transfers.
- `test_proxy` and `test_proxy_tinfl`: the 1.x tests plus pictures through
  the proxy (`make` draws JPEG, PNG and GIF test pictures with Pillow; each
  must come out as a GIF no wider than the limit, with only 16 greys, and
  decode back with a mean brightness within a tolerance of the original),
  pass-through of oversize and refused pictures, and reader mode on
  synthetic pages (and on the saved real pages with `PAGES=`).

`hostmodem` (the modem as a PC program for the emulator's serial bridge)
gains the same features, so `python3 tools/emu/net.py` keeps working.

### Measured (4 October 2026, this box)

- `pio run -e atoms3-lite`: 1,334 KB of flash (40 % of the 3.3 MB slot),
  55 KB of static RAM. `pio run -e m5stack-atom`: 1,105 KB, 56 KB. Both
  build clean (the only warnings are in the vendored picojpeg).
- The host tests: `test_modem` 371 checks, `test_usbnet` 37, `test_proxy`
  and `test_proxy_tinfl` 180 each, all passing.
- `tools/psiexecd.py` against `hostmodem` end to end: `AT$EXEC=` refused
  when off, a one-shot with `OK`, a disallowed command refused by the
  helper, a bad token refused, and the `psiexec` channel ending in
  `NO CARRIER` on `exit`.
- **Not measured, because no AtomS3 Lite was on this bench:** the free heap
  with everything on (the 1.1 figures were 120 KB kept free for the proxy;
  2.0 keeps 150 KB before sizing the ring), the USB host with a phone, the
  web pages on a real browser, the RTS/CTS wiring, and 230400 with the
  Psion. Section 11 is the list to work through on hardware.

## 11. Testing with the Psion

`VALIDATION.md` is the authority: the matrix of what has been verified at
which level, and the full bench procedure (T0–T7). The outline below is
kept as a quick list. All of it comes after the wiring gate in section 1
and the checklist in `HARDWARE.md`.

1. **Plain modem**: PsiTerm's Connection settings, 115200, flow control
   none, Test: *"The modem answered OK at 115200 baud"*, *"Modem: Atom modem
   2.0"*. Dial something. This is the 1.x behaviour and must work before
   anything else.
2. **RTS/CTS** (four-wire build only): `AT$FC=1 AT&W`. In the apps set
   *Flow control: RTS/CTS*; Test must not report *CTS blocked* (it would if
   the Atom's RTS did not reach the Psion's CTS, or the polarity were wrong).
   Then a download in PsiTerm (`cat` a large file): no receive errors in the
   session log.
3. **230400**: PsiKernTest (`experimental/kernel`) > Write > *Serial link
   with the modem*. It sends `AT$SB=230400`, sets the 5mx's divider, runs
   `ATI` ten times, and goes back. With RTS/CTS on, repeat with `AT$PR=0`
   (no pacing) to see the line's real rate; with pacing, `AT$PACE?` shows
   the auto rate (11000 bytes/s without flow control, 18000 with).
   460800 the same way if 230400 is clean; the Psion's level shifter is
   the unknown.
4. **TLS termination**: `AT$TLS=1 AT&W`. In PsiMail set the account's
   TLS to *none*, port 993 (and SMTP 465). Check mail; the log on the Atom
   shows `tls imap.example.com: 1200 ms`. The Psion's `psimail.log` shows no
   TLS lines and a faster connection.
5. **Pictures**: PsiWeb with the proxy (`psiproxy:8080`), `AT$PI=1`.
   Open a page with photographs; the Atom's log shows `picture 1200x800
   jpeg -> 300x200 gif, 12 KB -> 9 KB, 180 ms`; PsiWeb's Page information
   shows the smaller size.
6. **Reader mode**: `AT$PX=4`, open a news front page: the headline list
   without the navigation. `AT$PX=1` to compare.
7. **Remote compute**: on a PC on the LAN, `PSIEXEC_TOKEN=... python3
   tools/psiexecd.py --allow uptime,date,fortune`. `AT$XE=1 AT$XH=pc:7777
   AT$XK=<token> AT&W`. `AT$EXEC=uptime` prints the PC's uptime. In PsiTerm,
   `ATDT psiexec` opens the channel.
8. **Uplinks**: `AT$UP?` at each stage. WiFi: as before. USB: `AT$USB=1
   AT&W ATZ`, power from the 5V pad, plug an Android phone with USB
   tethering on: `AT$UP?` goes *enumerating* → *up: 192.168.42.x*, and a
   dial works with WiFi off (`ATC0`). Plug an iPhone: expect *no carrier
   (unpaired)*, and AUTO falling back to WiFi; that is the state described
   in section 12, not a wiring fault.

## 12. What is deferred, and why

- **iPhone pairing over USB (lockdownd)**: the carrier on Apple's USB
  Ethernet interface only comes up for a paired host. Pairing is the
  usbmuxd protocol (a multiplexed TCP over the bulk endpoints of the
  "Apple Mobile Device" interface), lockdownd's plist request/response,
  `StartSession` over TLS 1.2 with a host certificate and key the host
  generates (RSA 2048), `Pair` with the device's public key signed by that
  host, and the pair record kept for next time. A port of the relevant part
  of libimobiledevice to the ESP32-S3 is feasible in principle (mbedTLS has
  the primitives; the RSA key is generated once and kept in NVS) and is a
  project of its own. Everything below it (host stack, Apple transport,
  carrier polling, netif) is in this firmware so that the day the pairing
  exists, the link comes up with no other change. Until then the iPhone's
  WiFi hotspot is the path, and the firmware says *no carrier (unpaired?)*
  rather than pretending.
- **RNDIS**: the older Android tethering class (and Windows' own). Simple
  enough to add later (`REMOTE_NDIS_INITIALIZE`, packet messages); NCM is
  what current phones offer, so it came first.
- **The web proxy's pictures for PsiMail**: PsiMail decodes web pictures
  itself and does not use the proxy. Routing PsiMail's web pictures through
  the Atom would need a change on the Psion side.
- **A full readability pass**: needs the page in memory; see section 9.
- **Keyboard emulation / USB HID bridging**: not part of this build. If
  added, they use the same UART and the same transceiver rule.

## 13. Sources

- M5Stack AtomS3 Lite documentation (pins: G5–G8, G38, G39, LED G35, button
  G41; USB-C on the ESP32-S3's native USB) and the Atomic RS232 Base page
  (TTL on G5/G6 for the AtomS3 family; the base has TXD, RXD and GND only).
- ESP-IDF 5.3 USB Host Library documentation (full speed only; class
  drivers; the single-port caveat on boards with no second USB connector).
- Linux `drivers/net/usb/ipheth.c` (Apple's vendor interface, the two-byte
  frame header, `IPHETH_CMD_CARRIER_CHECK`) and `usbmuxd`/`libimobiledevice`
  (why pairing is needed for the carrier; the lockdownd protocol).
- USB CDC Subclass specification for NCM 1.0 (NTB16 layout) and ECM 1.2.
- MAX3232 datasheet (3.3 V operation, inverting drivers and receivers).
- This repository: `ssh/psiglue.cpp` (what the apps send; `KConfigObeyCTS`;
  `KConfigFailDCD`), `experimental/kernel/results/2026-10-03-5mx.md`
  (`UBRCR` 1 for 230400 on a real 5mx), `docs/experimental-hacks.md`,
  `docs/psiweb-modern-web-plan.md` (the PsiProxy idea), `mail/engine/img`
  (the decoders).
