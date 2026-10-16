# Validation: what has been verified, and the physical test plan

Every row says how far a thing has been verified. The levels, in order:

- **Implemented**: the code exists.
- **Compiles**: it is in a firmware image that builds (`pio run`, both
  environments, clean builds on 4 October 2026).
- **Host-unit-tested**: exercised by `make -C hosttest test` on a PC against
  the simulated board (`fakehal.h`); the same source the Atom runs.
- **Hardware-tested**: run on an AtomS3 Lite.
- **Verified on a real Psion**: seen working with a Series 5mx on the line.
- **Not tested**: none of the above beyond compiling.
- **Known limitation**: cannot work as described, by design or by hardware.

No item in this release is at *Hardware-tested* or *Verified on a real
Psion* for the 2.0 firmware: there was no AtomS3 Lite on this bench. The
1.x behaviour that 2.0 preserves had been used by Dan on a plain Atom with
the RS232 Base and the Psion (the README's wiring and the Test button
results); that is the baseline the first bench session confirms first.

## 1. Validation matrix

### Serial modem (the Psion's view)

| Item | Level | Evidence / notes |
|---|---|---|
| `AT`, `ATE/V/Q`, `ATI`, `ATZ`, `AT&F/W/V/C`, `ATS2/S12`, `A/`, init strings accepted | Host-unit-tested | `test_modem` basics, settings |
| `ATDT host:port` and its variants; `CONNECT <baud>`; `NO CARRIER` on failure, on close (after the last byte), on WiFi loss | Host-unit-tested | dial, carrier, psiglue sections; the psiglue section types exactly what `ssh/psiglue.cpp` sends |
| `+++` with guard times (every edge case), `ATO`, `ATH` | Host-unit-tested | escape section |
| Pacing: rate, 16-byte bursts, back-pressure with a small ring, order kept; 57600/115200/230400 rates | Host-unit-tested (simulated UART and clock) | pacing section; the simulated line obeys the baud rate, not a real UART FIFO |
| `AT$SB`/`ATB` with the 15 s fall-back trial; the PsiKernTest dialogue | Host-unit-tested; the dialogue also exercised in the emulator in 1.x (`tools/emu/net.py kern*`) | baud fallback section |
| **230400 on the real line** | **Not tested (needs bench)** | The Psion side set `UBRCR` 1 on Dan's 5mx (experimental/kernel results); the Atom side accepts it; the pair has not run together with 2.0 |
| **460800, 921600** | Not tested; 921600 is beyond the Psion's divider: Known limitation | |
| **RTS/CTS hardware flow control** (`AT$FC=1`) | **Implemented** (the UART driver's `uart_set_hw_flow_ctrl(CTS_RTS)` and `uart_set_pin` for G7/G8 through the core's `setHwFlowCtrlMode`/`setPins`: verified in the source, HARDWARE.md §7), Compiles; the AUTO-pacing change Host-unit-tested; the handshake itself **Not tested on hardware**; **Known limitation with the RS232 Base** (no RTS/CTS wires: `AT$FC=1` then has no effect on the line) | the four-wire build, HARDWARE.md §7 (Path B: one MAX3232 on the bare header, G7→pin 8, pin 7→G8) |
| **The RS-232 electrical path** (the Atomic RS232 Base's transceiver; nothing TTL leaves the stacked boards) | Verified by the part (the Base is the transceiver) | HARDWARE.md §1 |
| **The serial wiring** (Base T → DE-9 pin 2, R → pin 3, G → pin 5 on a female breakout; the existing null-modem cable; the unmodified Psion cable) | **Verified by equivalence to the WiRSa** (documented pinouts, HARDWARE.md §3–4, §8); the live T0 run **Not tested on the bench** | no crossover in the adapter; no gender changer; `AT$SWAP` not needed |
| UART receive buffer 4 KB, no transmit buffer (pacing is exact) | Implemented, Compiles | as 1.x |
| Command line limit 255 characters; longer lines are cut (no overflow) | Host-unit-tested indirectly (bounded copies) | `iLine[256]` |
| Binary transparency (0x00–0xFF both ways, 0xFF untouched, no telnet) | Host-unit-tested | psiglue section sends all 256 values |
| `Send()` to a stuck UART gives up after 2 s rather than hang | Implemented | not provoked in tests |
| Memory under sustained traffic: the ring is fixed; TCP is throttled when it is full | Host-unit-tested (30 KB through an 8 KB ring) | no allocation per byte |
| Large responses through the proxy (950 KB BBC page) | Host-unit-tested with the saved page when `PAGES=` is given (not on this box: no saved pages); synthetic pages always | 1.x evidence in the README's table |

### Uplink and WiFi

| Item | Level | Evidence / notes |
|---|---|---|
| WiFi station join, auto-reconnect, `ATW`, `ATC0/1`, scan | Compiles (board side); the modem's calls Host-unit-tested | as 1.x |
| **WiFi states**: down / joined-unconfirmed / joined-Internet / recovering, with the Internet check (`AT$CHK`, default a TCP connect to 1.1.1.1:53), its cadence (15 s unconfirmed, 2 min confirmed, never during a call, brought forward after a failed connect or a rejoin) | Host-unit-tested (`reachability` section) | the probe itself (a connect in a FreeRTOS task) Compiles, Not tested |
| **Rejoining** after the hotspot drops: after 20 s down, then every 30 s, logged | Host-unit-tested | |
| Calls allowed while "joined, Internet not confirmed" | Implemented by design; Host-unit-tested | a hotspot can block the check's target; the state is shown, not enforced |
| AUTO/WIFI/USB selection; USB only with an address; 5 s hold-off | Host-unit-tested | |
| USB host never started unless `AT$USB=1` saved; never auto-selected without a DHCP address | Host-unit-tested (the uplink), by construction (the HAL) | |
| An iPhone's Personal Hotspot as the network | Compiles (a WPA2 network like any other); **Not tested** with a phone | |
| USB tethering (all ten concerns) | see `USB-TETHERING.md`: Compiles / framing Host-unit-tested / pairing Not implemented | experimental, unsupported |

### Configuration

| Item | Level | Evidence / notes |
|---|---|---|
| The 1.x settings record unchanged (140 bytes); a 1.x device's settings kept | Host-unit-tested (`static_assert`, a 1.x record loaded) | |
| The 2.0 record (256 bytes), defaults, `AT&W`, `ATZ`, older/shorter record, factory reset | Host-unit-tested | |
| Every setting answers `AT$X?` and takes `AT$X=` (schema walk); bad values refused | Host-unit-tested | |
| Secrets never printed | Host-unit-tested (`AT&V`, `AT$HELP`, `?`) | |
| Web pages, captive portal, password, `/api/settings` | Compiles; **Not tested** in a browser | the pages are built from the same schema the tests walk |
| NVS load/save on the board | Compiles; as 1.x for `cfg`; `cfg2` Not tested | |

### Hand-off features

| Item | Level | Evidence / notes |
|---|---|---|
| Web proxy: TLS, redirects, gzip, chunked, keep-alive, back-pressure | Host-unit-tested (1.x suite, 2 × 180 checks with zlib and with tinfl) | the TLS handshake itself is the board's (as 1.x) |
| Reader mode (`AT$PX=4`) | Host-unit-tested (synthetic pages; same output in any chunking) | heuristic; real pages only with `PAGES=` |
| Pictures to 16-grey GIF | Host-unit-tested (JPEG, progressive JPEG, PNG, GIF, decoded back with Pillow; oversize and undecodable pass-through; small ring; GIF writer exact round trip) | decode time on the S3 Not measured |
| TLS termination (`AT$TLS`, `tls:`) | Host-unit-tested (reaches the HAL as TLS; failure → `NO CARRIER`) | the handshake on the board as the proxy's |
| `psiexec` / `AT$EXEC` with `psiexecd.py` | Host-unit-tested, and **end-to-end on this PC** through `hostmodem` (refusals, one-shot `OK`, channel `NO CARRIER`) | |
| `AT$LOG?` ring, `AT$HELP`, `AT$RESET=YES` | Host-unit-tested | the board's `FactoryReset()` (NVS clear + restart) Compiles |

### Memory (AtomS3 Lite, no PSRAM)

| Item | Level | Notes |
|---|---|---|
| Static RAM 55 KB (S3), 56 KB (Atom); flash 1.34 MB / 1.11 MB | Verified from the build | |
| Free heap with WiFi + web server + ring, and after a TLS handshake, a picture, the USB host | **Not measured** | 1.1 on the plain Atom: the ring took the largest of 128/96/64/48/32 KB leaving 120 KB; 2.0 leaves 150 KB. `ATI` prints the numbers |
| Loop task stack 20 KB (was 16 KB) for TLS + the decoder | Compiles | stack high-water Not measured |
| Fragmentation: a 64 KB picture buffer allocated and freed per picture, a 20 KB LZW dictionary, the gzip writer 16 KB | Implemented; **Not measured** over a long session | the largest-free-block figure in `ATI` is the thing to watch |
| Recovery: no memory for the ring → restart; no memory for a picture → pass-through; no memory for gzip → plain | Implemented; the pass-through path Host-unit-tested (malloc not failed artificially) | |

## 2. The physical test procedure (the Psion bench)

Prerequisites: the chain wired as HARDWARE.md section 4 says (the WiRSa's
DE-9, pin for pin, with the same null-modem cable) and its section 6
checklist ticked. Record every result in `TEST-REPORT.md`.

**T0. Baseline (must pass before anything else)**
1. Flash `atoms3-lite`. USB console: the start-up lines (buffer size, free
   heap, pins). Record the heap figures.
2. On the console (or from the Psion later): `AT$SSID=…`, `AT$PASS=…`,
   `AT&W`, `ATI`. Expect `WiFi: … joined, Internet reachable` within ~20 s.
3. PsiTerm → Connection settings → 115200, flow control None → **Test**:
   *"The modem answered OK at 115200 baud"*, *"Modem: Atom modem 2.0"*.
4. PsiTerm: SSH to a known host; type, `cat` a 1 MB file; no receive errors
   in the session log. This is the 1.x behaviour and the gate for the rest.

**T1. The hotspot**
1. iPhone: Personal Hotspot on. Atom: `AT$SSID=<iPhone>`, `AT$PASS=…`.
2. `AT$UP?` → `AUTO: WiFi in use; WiFi joined, Internet reachable`.
3. PsiMail: check mail. PsiWeb: a page.
4. Turn the hotspot off: `AT$UP?` → `lost, joining again`; a dial → `NO
   CARRIER`. Turn it on: within 30 s `joined`, then `Internet reachable`.
5. Put the phone in airplane mode with the hotspot kept on (if iOS allows):
   expect `joined, Internet not confirmed (the check failed)` and PsiMail's
   own timeout message; back out of airplane mode: `reachable`.

**T2. 230400 (mandatory target; pacing only with the RS232 Base)**
1. PsiKernTest (`experimental/kernel`) → Write → *Serial link with the
   modem*. It does `AT$SB=230400`, sets the 5mx's divider, `ATI` ten times
   at each speed, back to 115200. Record "N of 10 clean" at both speeds.
2. If clean: PsiTerm at 230400 (the apps' baud list ends at 115200: this
   needs the kernel driver path from PsiKernTest, or a PsiTerm build with
   `EBpsSpecial`); `cat` the 1 MB file; errors in the log. `AT$PACE?` shows
   11000 bytes/s AUTO. Try `AT$PR=16000` and note where errors begin.
3. 460800: same steps; expect it may fail at the Psion's level shifter.
4. Record: speed, clean/10, bytes/s achieved, errors.

**T3. RTS/CTS (mandatory target; needs the four-wire build, HARDWARE.md §7)**
1. `AT$FC=1`, `AT&W`. Apps: flow control RTS/CTS. Test must not say *CTS
   blocked*. 2. `cat` the 1 MB file at 115200 and 230400 with `AT$PR=0`
   (pacing off): no errors means the handshake works. 3. Pull the CTS wire
   mid-transfer: output must stop, and resume when it is back.
   With the RS232 Base: record **Known limitation: not possible**.

**T4. Hand-off features**
1. `AT$TLS=1`, PsiMail account TLS none, port 993: mail arrives; the log
   shows `TLS imap…: N ms`. Compare the time with 1.x.
2. PsiWeb through `psiproxy` with `AT$PI=1`: a page with photos; the log's
   `picture … -> … gif, … ms` lines; the page's own time.
3. `AT$PX=4` on a news front page.
4. `psiexecd.py` on a PC; `AT$EXEC=uptime`; `ATDT psiexec` from PsiTerm.

**T5. Robustness**
1. Switch the Psion off mid-call, on again: PsiTerm's "no answer – hanging
   up an old call" path; the modem answers `AT` afterwards.
2. Pull the DE-9 mid-call and reconnect: the modem's state after `+++`.
3. Leave the modem up overnight on the hotspot; `ATI` heap and largest
   block in the morning.

**T6. Web pages**: the portal from a phone with no network saved; the pages
on the network with the password; a settings change from the page visible
in `AT&V`; factory reset from the page.

**T7. USB (experimental, last, optional)**: `AT$USB=1`, `AT&W`, `ATZ`, a
5 V supply arranged as in HARDWARE.md §5; an Android phone with tethering;
`AT$UP?` through the states; a dial with WiFi off. Then an iPhone: expect
`no carrier (unpaired)`. Record exactly what the log says.

## 3. Serial modem review (what was looked at, and the real limitations)

- **Parsing**: `AT` is found anywhere on the line (line noise before it is
  skipped); commands chain (`ATE0V1`); `AT$` takes the rest of the line
  (so `AT$PX=2&W` is a value, by design and tested). Unknown commands →
  `ERROR`. Lines over 255 characters are cut silently: a limitation, not a
  fault, as no app sends such a line.
- **Responses**: `\r\nOK\r\n` and friends, `CONNECT <baud>`, numeric mode
  (`V0`), quiet mode (`Q1`); the echo is on by default as a WiRSa's is and
  psiglue skips it.
- **Buffering**: 4 KB UART receive, the paced ring (32–128 KB) to the Psion,
  256-byte batches to the server; the ring is never grown and the socket
  is not read when it is full.
- **Escape and disconnect**: `+++` with S12 guard (0.8 s; the apps wait
  1.1 s); `ATH`; `NO CARRIER` only after the last byte has gone (or at once
  when the uplink itself goes).
- **Errors**: a failed connect is `NO CARRIER` (and now brings the Internet
  check forward so `AT$UP?` can say why); a TLS failure logs the reason.
- **Limitations to know**: the picture decoder and a TLS handshake run
  inside `loop()`, so the serial line pauses while they run (bounded, but
  real); there is no DTR; the RS232 Base build has no RTS/CTS; `AT$EXEC`
  output is bounded by the helper (64 KB, 30 s), not by the modem.

## 4. The shared-code change (`mail/engine/img`)

`PmImgOpts` gained a field, `exact` (int, at the end of the struct);
`pmjpeg.c` and `pmjprog.c` read it when choosing the JPEG decode scale.

- **Is it needed for the gateway?** Yes. PsiMail's rule decodes at the
  largest shrink that still gives *half* the wanted width (to save the
  Psion's CPU), so the Atom's 200-pixel-wide pictures came out 100 wide.
  With `exact = 1` the Atom gets the scale that still gives the full width.
- **Does it change PsiMail's behaviour?** No. `pictures.c` zero-fills the
  options (`memset`, line 315) before `pmimg_decode`, so `exact` is 0 and the
  old rule applies unchanged; `mail/test/imgtest.c` does the same.
- **Are both paths correct?** `exact = 0` is the previous expression
  verbatim; `exact = 1` substitutes `max_w` for `max_w / 2` (and `max_h`),
  which is the documented intent. Host-unit-tested on the Atom side
  (800x500 → 200x125 with `exact`, 100x63 for a progressive one bounded by
  memory) and by PsiMail's own suite: `python3 mail/test/imgtest.py
  --rounds 30` on this box, 4 October 2026: *"ok: no crashes, no sanitizer
  reports, no leaks"* (ASan, every format, fuzzing).
- **Memory or compatibility regression?** The struct grows by 4 bytes on
  the stack; no stored format includes it; no ABI crosses a process
  boundary (the decoders are compiled into each program). On the Psion the
  decode scale is unchanged, so memory is unchanged.
- **Flag:** this is the one change outside `firmware/atom-modem` (with the
  one-line `tools/emu/net.py` version-string match). It should be reviewed
  as a PsiMail change in the same commit.
