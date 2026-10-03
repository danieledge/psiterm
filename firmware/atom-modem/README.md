# Atom modem: a Psion-tuned WiFi modem

Firmware for an **M5Stack Atom** (Lite or Matrix, ESP32) on an **Atomic RS232
Base**. It turns that pair into a Hayes-style WiFi modem for a Psion Series 5/5mx
running PsiTerm, PsiMail or PsiWeb. It talks to the apps exactly as a WiRSa
does (`ATDT host:port`, `CONNECT`, `+++`, `ATH`, `NO CARRIER`). It adds the one
thing a three-wire link lacks: **output pacing**, so the Psion is not
overrun even though there is no RTS/CTS.

It can also be **PsiWeb's web proxy**. Dial `psiproxy` instead of a server
and the Atom fetches pages itself, does the TLS for `https://`, simplifies
the HTML as it streams and gzips it for the line. The 36 MHz Psion then does
no TLS and lays out a fraction of the page. See
[The web proxy for PsiWeb](#the-web-proxy-for-psiweb).

MIT licence, as the rest of the repository (see `LICENSE` at the top).

## Contents

- [Why pacing](#why-pacing)
- [Wiring to the Psion cable](#wiring-to-the-psion-cable)
- [Flashing](#flashing)
- [First set-up (WiFi)](#first-set-up-wifi)
- [Settings for PsiTerm, PsiMail and PsiWeb](#settings-for-psiterm-psimail-and-psiweb)
- [The web proxy for PsiWeb](#the-web-proxy-for-psiweb)
- [AT command reference](#at-command-reference)
- [Pacing in detail](#pacing-in-detail)
- [Status LED and button](#status-led-and-button)
- [Optional: an emulated DCD](#optional-an-emulated-dcd)
- [Building and testing](#building-and-testing)
- [Not supported](#not-supported)

## Why pacing

The Atomic RS232 Base has only TX, RX and GND on its 4-pin VH-3.96 connector:
no RTS/CTS, DCD or DTR. A WiFi modem fetches a TLS stream from the Internet in
bursts and passes each burst on at the full line rate. The Psion's serial
driver has a 16-byte UART FIFO, and our apps give it a 16 KB receive buffer.
At 115200 it can overrun when a burst arrives while the Psion is busy, for
example laying out a page or writing to flash. A WiRSa without working RTS/CTS
has the same problem.

This firmware handles it at the other end:

1. Everything from the server goes into a large buffer in the ESP32's RAM
   (up to 128 KB, as much as the heap allows after WiFi and the web proxy's
   needs).
2. The buffer is sent to the Psion at a **paced rate below the line rate**, in
   small bursts with gaps between them. This is a token bucket. A 16-byte
   burst cannot overrun the Psion's 16-byte FIFO even if its interrupt is held
   off for the whole burst. The gaps let the driver catch up.
3. When the buffer is full, the firmware **stops reading the TCP socket**. The
   socket's window closes and the server waits. TCP's own flow control does
   the job of the missing RTS/CTS, end to end.

The defaults (5500 bytes/s at 115200, 4000 bytes/s at 57600) are still faster
than the Psion can decrypt and use a TLS stream. They are also slow enough
that a 16 KB receive buffer covers 3 to 4 seconds of the app not reading. See
[Pacing in detail](#pacing-in-detail) to tune them.

## Wiring to the Psion cable

The Psion's serial cable ends in a 9-pin D socket (the end that plugs into a
PC). Wire it to the base's screw terminal like this:

| Atomic RS232 Base | Psion cable (DB9) | |
|---|---|---|
| **G** (ground) | pin 5 | signal ground |
| **T** | pin 2 | |
| **R** | pin 3 | |
| red power terminal | **nothing** | never connect it to the Psion |
| — | pins 7 and 8 bridged | optional: the Psion then sees CTS follow its own RTS |
| — | pin 1 | leave unconnected (DCD, unless you add the [optional DCD](#optional-an-emulated-dcd)) |

- Power the Atom from its USB-C socket, not from the base's power terminal.
  Never connect that terminal to any pin of the Psion's cable.
- Set **Flow control to None** in the apps. This board has no RTS/CTS.
- If the apps' **Test** button says *"Nothing came back - check the cable, or
  try swapping TX and RX"*, swap the wires on pins 2 and 3.

The Atom's own pins (M5Stack Atom Lite/Matrix with the Atomic RS232 Base) are:

| Atom GPIO | Use |
|---|---|
| G22 | RX: from the base's MAX232 receiver (data from the Psion) |
| G19 | TX: to the base's MAX232 driver (data to the Psion) |
| G27 | the WS2812 status LED |
| G39 | the button |

These follow the base's pin map, which puts the RS232 transceiver on the
Atom's bottom-header UART pins. M5Stack's AtomS3R tutorial gives the same
header positions as G5 (RX) and G6 (TX), and community code for the Atom Lite
uses TX = 19 and RX = 22.

`AT$SWAP=1` swaps G19 and G22. It is only for a base that maps them the other
way round, when nothing at all works at any speed. A cable that is the other
way round is fixed by swapping pins 2 and 3, not by `AT$SWAP`.

WiRSa, WiFi232 and Zimodem each use the pins of their own boards, so their pin
choices do not carry over. What does carry over is their **command set**, and
this firmware follows it (see the reference below).

## Flashing

You need a USB-C cable. The Atom shows up as a USB serial port (CH9102 or FTDI).

### PlatformIO (recommended)

```sh
pip install platformio
cd firmware/atom-modem
pio run -t upload           # build and flash
pio device monitor          # optional: the USB status console, 115200 baud
```

`platformio.ini` pins `espressif32@6.9.0` (Arduino core 2.0.17) and board
`m5stack-atom`.

### Arduino IDE / arduino-cli

This folder is also a sketch (`atom-modem.ino`, with the code in `src/`).
Install the **esp32** boards package (2.0.x), choose the board
**M5Stack-ATOM**, and upload. With arduino-cli:

```sh
arduino-cli compile --fqbn espressif:esp32:m5stack-atom firmware/atom-modem
arduino-cli upload  --fqbn espressif:esp32:m5stack-atom -p /dev/ttyUSB0 firmware/atom-modem
```

### esptool (a ready-built image)

A build leaves three images: the bootloader, the partition table and the
firmware. With PlatformIO they are in `.pio/build/m5stack-atom/`. Arduino's
`boot_app0.bin` is in the esp32 package under `tools/partitions/`. Flash them
at their addresses:

```sh
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash -z \
  0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

Or merge them into one image that starts at address 0:

```sh
esptool.py --chip esp32 merge_bin -o atom-modem.bin \
  0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash 0x0 atom-modem.bin
```

### M5Burner

M5Burner can burn a firmware file you supply, through its custom or local
firmware option (the name depends on the version). Give it the merged image
above, which starts at address 0.

## First set-up (WiFi)

Connect the Psion and open PsiTerm (its terminal talks straight to the
modem). Alternatively, use any terminal program at 115200 baud, 8N1, no flow
control. Then type:

```
AT$SSID=My network
AT$PASS=my password
AT&W
ATI
```

`AT$PASS` joins the network once both are set. `AT&W` saves them in flash, so
the modem rejoins by itself at every power-up. `ATI` shows the modem's name,
the network, its IP address and the pacing. The Zimodem form
`ATW"My network,my password"` works too.

## Settings for PsiTerm, PsiMail and PsiWeb

In each app, go to **Tools > Connection settings**:

| Setting | Value |
|---|---|
| Baud rate | **115200**. Use 57600 if the Test button or the session log reports receive errors |
| Flow control | **None** |
| … connects via | **Modem (ATDT host:port)** |
| Psion Internet: first send | not used (this firmware has no PPP) |

Then press **Test** (Ctrl+T). It should say *"The modem answered OK at 115200
baud"* and *"Modem: Atom modem 1.1 (Psion-tuned)"*. If you changed the
modem's speed with `AT$SB`, set the same baud rate in the apps.

For the web proxy, also set PsiWeb's **Tools > Preferences**: *Use a proxy*
**Yes**, *Proxy host* **psiproxy**, *Proxy port* **8080**.

## The web proxy for PsiWeb

A modern front page is about 1 MB of HTML, scripts and styles, sent over TLS.
On a 36 MHz Psion the TLS alone takes seconds, and Links has to parse and lay
out all of it. The Atom has a 240 MHz CPU, hardware AES and WiFi, so it can do
that work instead.

### Using it

1. In PsiWeb: **Tools > Preferences**, *Use a proxy* **Yes**, *Proxy host*
   **psiproxy**, *Proxy port* **8080**.
2. Open any page, `http://` or `https://`.

PsiWeb then dials `ATDT psiproxy:8080`. The modem recognises the name and
opens no TCP connection. It answers `CONNECT` and is itself the HTTP proxy at
the other end of the line. Any port is accepted, and `psiproxy` alone works
too. Every other dial (PsiTerm, PsiMail, PsiWeb without a proxy) works exactly
as before.

The call stays up between pages (HTTP keep-alive), whatever the site, so
PsiWeb dials once and then fetches page after page. `http://psiproxy/` shows
the proxy's status: its mode, the requests so far, bytes in and out, and the
Atom's free memory.

### What it does

For each request from PsiWeb (an absolute address; Links sends `https://`
ones to the proxy as plain requests):

- **Fetches the page**, over TLS for `https://`. Certificates are checked
  against the Mozilla root certificates (`src/cabundle.h`) and the server's
  name. The connection to a server is kept for the next request to it.
- **Follows redirects** (up to 6), and remembers sites that moved from
  `http://` to `https://`, so the next `http://` request to them goes to
  `https://` straight away. A page reached through a redirect gets a
  `<base href>` so its relative links still work.
- **Unpacks gzip or deflate** if a server sends it, though the proxy asks for
  plain pages. This uses the ESP32 ROM's inflater with a 32 KB window.
- **Simplifies HTML as it streams** (`src/htmlsimp.cpp`): a state machine
  with fixed buffers (about 5 KB), so a page of any size uses the same memory.
  - Dropped, with their content: `script`, `style`, inline JSON, `noscript`,
    `svg`, `math`, `iframe`, `template`, `object`, `video`, `audio`, `canvas`,
    comments, and anything marked `hidden` or `style="display:none"`.
  - Dropped tags: `link`, `meta` (except the character set and refresh),
    `source`, `picture`, `span`, `font`, custom elements, form fields outside
    a form, tracking pixels, and `.svg` pictures (Links cannot draw them; their
    alt text stays).
  - Attributes: only what Links uses (`href`, `src`, `alt`, `width`, `height`,
    `name`, `value`, `type`, `action`, `method` and the like). Lazy-loaded
    pictures (`data-src`) get their real address. `javascript:` links lose it.
  - HTML5 blocks (`section`, `article`, `header`, `nav`, `figure`...) become
    `<div>`, which Links breaks lines for. Empty elements are left out. Runs
    of white space become one.
  - Kept: text, headings, paragraphs, lists, links, pictures, tables, forms,
    `pre`, the title and the character set.
- **Gzips the simplified HTML for the line** (`src/gzip.cpp`), because PsiWeb
  accepts gzip. This is deflate with fixed codes and a 4 KB window, in 16 KB
  of RAM. It makes the HTML about a third of the size. When the server is slow,
  the proxy flushes what it has, so PsiWeb can show the top of the page while
  the rest comes.
- **Passes everything else through unchanged**: pictures, files, error
  pages, cookies (`Set-Cookie`), `WWW-Authenticate` and caching headers.

`https://` links in pages are left as they are. PsiWeb sends every address to
the proxy, so they come back through it anyway, and the Psion never does TLS.

If the proxy cannot fetch a page, PsiWeb shows a short *Page not loaded* page
that gives the reason: the server could not be reached, or the certificate
was not trusted.

### Results

Measured on 3 October 2026 with PsiWeb's ARM code in the harness
(`web/links/emu/run_links.py`), replaying recorded traffic.

- *Line* is the bytes that crossed the serial line.
- *CPU* is the Psion's instructions, as seconds at 15 MIPS.
- *Time* is the Psion's time from the request to the page being laid out:
  CPU, plus waiting for data arriving at the modem's paced 5500 bytes/s.
- *Direct* is PsiWeb fetching through the modem as before, with Links doing
  TLS and gzip itself. *Proxy* is through `psiproxy`.

| Page | Direct: line, CPU, time, heap peak | Proxy: line, CPU, time, heap peak |
|---|---|---|
| www.bbc.co.uk (950 KB of HTML) | 114 KB, 6.5 s, 27.5 s, 2182 KB | 26.6 KB, 2.6 s, 9.3 s, 672 KB |
| en.wikipedia.org/wiki/Psion | 32 KB, 2.2 s, 9.6 s, 532 KB | 6.8 KB, 0.7 s, 4.7 s, 448 KB |
| news.ycombinator.com | 10.6 KB, 2.2 s, 6.6 s, 711 KB | 5.3 KB, 1.5 s, 5.0 s, 688 KB |
| info.cern.ch | 0.9 KB, 0.2 s, 3.4 s, 350 KB | 0.6 KB, 0.2 s, 3.4 s, 378 KB |

These times leave out the proxy's own fetch. On a PC that took 0.1 to 1 s;
on the Atom, add the TLS handshake, about 1 to 2 s for a new server.

The simplifier on its own (saved pages, `make -C hosttest test PAGES=...`):

| Page | HTML | Simplified | Text only (`AT$PX=2`) | Simplified and gzipped |
|---|---|---|---|---|
| bbc.co.uk | 949 KB | 57 KB (6.0 %) | 39 KB (4.1 %) | 20 KB |
| theguardian.com | 1636 KB | 99 KB (6.0 %) | 58 KB (3.5 %) | 44 KB |
| en.m.wikipedia.org (Psion Series 5) | 128 KB | 33 KB (26 %) | 23 KB (18 %) | 13 KB |
| news.ycombinator.com | 34 KB | 15 KB (43 %) | 15 KB (43 %) | 5 KB |

The harness figures above were taken before form fields outside forms were
dropped, which took BBC from 71 KB to 57 KB.

### Modes

| `AT$PX=` | The proxy |
|---|---|
| `1` | on: simplified pages (the default) |
| `2` | on: text only. Pictures become their alt text, and `nav`, `aside` and `footer` are dropped |
| `3` | on: pages unchanged. The proxy does only the TLS, and passes on the Psion's own gzip |
| `0` | off. `psiproxy` is then an ordinary name to dial |

`AT$PZ=0` stops the proxy gzipping pages (`AT$PZ=1`, the default, gzips them
when PsiWeb accepts gzip, which it always does). Save either setting with
`AT&W`.

### Security

With the proxy, the Atom sees your pages in plain text: it does the TLS, not
the Psion. Use it only with your own modem on your own WiFi. The Psion-to-Atom
line is a serial cable. Servers' certificates are checked on the Atom against
the Mozilla roots in `src/cabundle.h`. To update them, run
`python3 tools/mkcabundle.py > src/cabundle.h` on a PC with current roots,
then rebuild.

### Memory

The ESP32 has 320 KB of RAM for data, and WiFi takes much of it. The proxy
needs:

| Part | RAM | When |
|---|---|---|
| The modem object with the proxy and simplifier | about 25 KB | always |
| A TLS connection (mbedTLS, 16 KB record buffers) | about 50 KB | while it is open |
| The gzip writer | 16 KB | while a simplified page is sent |
| The gzip reader | 43 KB | only for a server that sends gzip unasked |

The ring to the Psion is therefore no longer simply as big as possible. The
firmware takes the largest of 128, 96, 64, 48 or 32 KB that leaves 120 KB
free, and the USB console prints the result at start-up. `ATI` shows the free
memory, and the USB console logs each TLS handshake with its time and memory,
and each request (`proxy: GET https://... -> 200, 950998 bytes in, 26577 out,
217 ms`). The proxy adds to the ring only when 12 KB of it is free, so
pacing and TCP back-pressure work as for any connection.

### Limits

- A request head (with its cookies) must fit in 4 KB, and a form's body with
  it.
- No `CONNECT` tunnels: the proxy fetches `https://` itself.
- Cookies set by a redirect itself are not passed on; the final page's are.
- Pictures are passed through as they are, not scaled or converted.
- One page at a time, as PsiWeb asks for them.

## AT command reference

Commands start with `AT`, and several can share a line (`ATE0V1`). Enter ends
the line. Backspace works. `A/` repeats the last line. Settings changed here
last until power-off; **`AT&W` saves them**.

### Hayes

| Command | Does |
|---|---|
| `AT` | `OK` |
| `ATDT host:port` | Opens a TCP connection and answers `CONNECT <baud>`; `NO CARRIER` if it cannot. `ATDThost:port`, `ATD host:port`, `ATDP…` and `ATDT"host:port"` also work. With no port, 23 is used |
| `ATDT psiproxy:8080` | The modem's own [web proxy](#the-web-proxy-for-psiweb) (any port): `CONNECT`, then HTTP proxy requests are answered by the modem |
| `ATDT777` (digits only) | `NO CARRIER`: phone numbers mean PPP, which this firmware does not do |
| `+++` | Quiet for the guard time, `+++`, then quiet again: back to command mode with the connection still up (`OK`). The pluses are not passed to the server |
| `ATO` | Back to the connection (`CONNECT`), or `NO CARRIER` if it has gone |
| `ATH`, `ATH0` | Hangs up |
| `ATE0` / `ATE1` | Echo off / on (on by default, as the WiRSa) |
| `ATV0` / `ATV1` | Result codes as digits (0 OK, 1 CONNECT, 3 NO CARRIER, 4 ERROR) / as words |
| `ATQ0` / `ATQ1` | Result codes on / off |
| `ATI` | Name and version, WiFi network and IP address, baud and pacing, the web proxy, free memory |
| `ATZ` | Back to the saved settings |
| `AT&F` | Factory settings, **keeping the WiFi network** (not saved until `AT&W`) |
| `AT&W` | Saves the settings in flash (NVS) |
| `AT&V` | Shows the settings |
| `AT&C0` / `AT&C1` | DCD always on / DCD follows the connection (for the optional DCD pin) |
| `AT&D`n, `AT&K`n, `AT&S`n, `ATL`n, `ATM`n, `ATX`n | Accepted and ignored, so modem init strings work. There are no DTR or RTS/CTS wires |
| `ATS2=`n | Escape character (43 = `+`) |
| `ATS12=`n | Escape guard time in 1/50 s. The default is 40 (0.8 s), because the apps wait 1.1 s either side of `+++` |
| `ATS`n`?` | Shows a register |

Result codes: `OK`, `CONNECT 115200`, `NO CARRIER`, `ERROR`. `NO CARRIER` is
sent when the server closes the connection, after the last of its data has
reached the Psion, or at once if the WiFi drops.

### WiFi and the Psion (WiFi232 / Zimodem style)

| Command | Does |
|---|---|
| `AT$SSID=name` / `AT$SSID?` | The WiFi network |
| `AT$PASS=password` / `AT$PASS?` | Its password (`?` only says whether one is set). Joins the network once both are set |
| `ATW"name,password"` | Zimodem: sets both and joins |
| `ATW` | Lists the networks in range |
| `ATC1` / `ATC0` | WiFi232: joins / leaves the saved network |
| `AT$SB=`n / `AT$SB?` | The serial baud (300 to 921600). It changes **after** the `OK`, which goes at the old speed. `ATB`n (Zimodem) is the same |
| `AT$PR=`n / `AT$PR=AUTO` / `AT$PR?` | Pacing rate towards the Psion in bytes/s. `0` turns pacing off. `AUTO` (the default) chooses from the baud rate |
| `AT$PB=`n / `AT$PB?` | Pacing burst: bytes sent back to back (the token bucket's depth) |
| `AT$PG=`n / `AT$PG?` | An extra gap after each burst, in ms (default 0) |
| `AT$PACE?` | The pacing in force, and the buffer size |
| `AT$SWAP=0/1` | Swaps the Atom's RX and TX pins (see [Wiring](#wiring-to-the-psion-cable)) |
| `AT$DCD=`n | The GPIO for an emulated DCD; `-1` (the default) for none |
| `AT$PX=`n / `AT$PX?` | The web proxy: `1` on (simplified pages, the default), `2` text only, `3` pages unchanged (TLS only), `0` off. See [The web proxy](#the-web-proxy-for-psiweb) |
| `AT$PZ=0/1` / `AT$PZ?` | Whether the web proxy gzips pages on the line (`1`, the default) |

## Pacing in detail

Bytes from the server go to the Psion through a token bucket. Tokens arrive at
`$PR` bytes per second, and the bucket holds `$PB` of them. Each byte sent
uses one token. The result is bursts of at most `$PB` bytes at the line rate,
with quiet gaps between them, at an average of `$PR` bytes/s. `$PG` adds a
fixed pause after every burst, if you want longer quiet spells.

Defaults (`AT$PR=AUTO`):

| Baud | Line rate | `$PR` | `$PB` | 16 KB lasts |
|---|---|---|---|---|
| 115200 | 11520 B/s | 5500 B/s (48 %) | 16 | 3.0 s |
| 57600 | 5760 B/s | 4000 B/s (69 %) | 16 | 4.1 s |
| 38400 | 3840 B/s | 3000 B/s (78 %) | 16 | 5.5 s |
| 19200 and below | | off | | |

Why these values:

- **The burst size is 16 bytes, the size of the Psion's UART FIFO.** The 5mx's
  UART (CL-PS7111) can hold 16 bytes. A burst no longer than that cannot
  overrun it, however late the interrupt is serviced. The quiet time between
  bursts then gives the driver time to empty it.
- **The average rate is about half the line rate at 115200.** The apps set a
  16 KB receive buffer (`SetReceiveBufferLength(16384)`), and the comms notes
  (`epoc-comms-best-practices.md`) show that its 75 % high-water mark only
  helps when RTS/CTS works. Without RTS/CTS, the buffer must instead absorb
  any time the app is not reading. At 5500 B/s it lasts 3 seconds, which
  covers a page layout or a flash write. That rate is still faster than PsiWeb
  or PsiMail can use a TLS stream on a 36 MHz ARM, so little speed is lost.
- **TCP does the rest.** The ESP32's buffer (up to 128 KB) fills while the
  Psion is slow. When it is full the socket is not read, and the server is
  held back by TCP. No data is dropped anywhere on the way.

Tuning:

- Receive errors or garbled pages at 115200: try `AT$PR=4000`, or drop to
  57600.
- A fast, reliable Psion (or a terminal program on a PC): `AT$PR=0` gives the
  full line rate.
- Save with `AT&W`. `AT$PR=AUTO` goes back to the defaults.

Pacing cannot make up for an app that stops reading altogether for longer
than the buffer lasts. Only RTS/CTS can do that, and this board has no wires
for it.

## Status LED and button

| LED | Means |
|---|---|
| red | no WiFi (not set up, or out of range) |
| amber | dialling (`ATDT`) |
| green | WiFi up, no connection |
| blue | connected |
| white flash | data moving |

**Factory reset:** hold the button while plugging the Atom in. The LED
flashes white; after 3 seconds every setting, including the WiFi network,
returns to the factory settings.

The USB port prints status messages at 115200 baud (WiFi joined, IP address).
It does not take AT commands.

## Optional: an emulated DCD

The base has no DCD line, so the apps detect a dropped connection from the
in-band `NO CARRIER`, which works well. If you want DCD as well (PsiTerm,
PsiMail and PsiWeb then notice a drop at once through `KConfigFailDCD`):

1. Choose a spare GPIO: G33, G23, G21 or G25 on the Atom's header.
2. Feed it through an RS232 driver: a spare channel of a MAX232/MAX3232, or a
   small TTL-to-RS232 board. **Never connect a GPIO straight to the Psion.**
   Connect the driver's output to DB9 **pin 1**.
3. `AT$DCD=33` (your pin), `AT&C1`, `AT&W`.

The pin is driven low for "DCD on", because the driver inverts. `AT&C0` holds
DCD on all the time.

## Building and testing

- `pio run` builds the firmware. `arduino-cli compile --fqbn
  espressif:esp32:m5stack-atom .` builds it the Arduino way, with no warnings
  at `--warnings all`.
- `make -C hosttest test` runs the unit tests on a PC (it needs zlib).
  - `test_modem`: the AT parser, `+++` with guard times (too soon, too slow,
    four pluses, data straight after), `NO CARRIER` on close and on a WiFi
    drop, and pacing under bursts. The pacing tests check the rate, the
    largest burst, buffer back-pressure with a small ring, and that every
    byte arrives in order.
  - `test_proxy`: the simplifier (what goes and what stays, the same output
    whatever pieces the page comes in, garbage, unclosed tags) and the proxy
    through the whole modem. This covers keep-alive, `https://`, redirects
    and the `http://` to `https://` memory, `POST` and 303, chunked, gzip and
    deflate from the server, pictures passed through, bodies that end at the
    close, `HEAD`, unreachable servers, `Connection: close` then
    `NO CARRIER`, back-pressure with a 16 KB ring, the gzip on the line
    (flushed while the server is slow), `AT$PX` and `AT$PZ`, and the settings
    record keeping its size. Regression checks cover a body with bytes after
    its length, a pass-through body cut short (the call ends), a page that
    grows fourfold in the simplifier with a small ring, window-wide
    back-references in gzip and deflate, Content-Length with chunked, a
    response head over 16 KB, requests that arrive faster than the proxy
    can take them, and a hidden element with no end tag.
  - `test_proxy_tinfl`: the same proxy tests with the inflater the Atom uses
    (tinfl from miniz 1.15, as in the ESP32 ROM; `hosttest/tinfl/`, public
    domain) in place of zlib, so its 32 KB wrapping window is tested too.
  - `make -C hosttest test PAGES=folder` adds real saved pages: `bbc.html`,
    `guardian.html`, `wiki.html` and `hn.html` (save them with `curl -L
    --compressed -A "Mozilla/5.0" -o bbc.html https://www.bbc.co.uk/` and
    the like). Each must come out well-formed for Links, keep its key text,
    and shrink below a set share. BBC is also sent through the whole modem,
    gzipped and chunked as a CDN sends it.

  They use a simulated clock, UART, TCP server, web server, WiFi and NVS
  (`hosttest/fakehal.h`). The modem code (`src/modem.cpp`, `proxy.cpp`,
  `htmlsimp.cpp`, `gzip.cpp`) is the same code the Atom runs.
- `make -C hosttest hostmodem` builds the modem as a PC program on a Unix
  socket, for the Psion emulator's serial bridge (`--serial-bridge-socket`).
  With it, PsiTerm's **Test** button and PsiWeb's dial were checked against
  this firmware's own code. `HOSTMODEM_REDIRECT=host:port` sends every dial
  to a local test server. Dialling `psiproxy` runs the web proxy against the
  real sites. Its TLS uses the PC's OpenSSL 3 (`libssl.so.3`, loaded at run
  time, so no OpenSSL headers are needed), with the system's root
  certificates. `HOSTPROXY_REDIRECT=host:port` sends the proxy's fetches to a
  local server instead.
- `hosttest/hostmodem --tcp 8080` is the web proxy alone on
  `127.0.0.1:8080`: each TCP connection is a `psiproxy` call. Point a browser,
  `curl -x`, or PsiWeb's ARM harness at it.
- `python3 tools/emu/net.py proxy` (from the top of the repository) is the
  end-to-end test in the Psion emulator. PsiWeb sets *Use a proxy* to
  `psiproxy`, 8080, and opens a real site (`EMU_PROXY_URL`, by default
  `news.ycombinator.com`) through the host modem. It needs the Internet.

`src/modem.cpp`, `proxy.cpp`, `htmlsimp.cpp` and `gzip.cpp` are plain C++
with no Arduino calls. `src/main.cpp` is the Atom's side: UART2, WiFi,
`WiFiClient` and `WiFiClientSecure` (TLS), `Preferences`, the LED and the
button. `src/cabundle.h` holds the root certificates, made by
`tools/mkcabundle.py`.

## Not supported

- **PPP** (the apps' "Psion Internet" route with `ATDT777`). Use the modem
  route; the apps do their own TCP and TLS over it (or, for PsiWeb, the
  [web proxy](#the-web-proxy-for-psiweb) does).
- Incoming connections (`ATA`, listening). Telnet option negotiation: the
  connection is a raw TCP stream, as SSH and TLS need.
- RTS/CTS, DTR and DSR: the base has no wires for them.
