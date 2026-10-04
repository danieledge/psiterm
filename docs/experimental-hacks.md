# Experimental, low-level ideas for the Series 5mx

Research notes, October 2026. Nothing here has been tried on Dan's machine. Every
idea is graded for **gain**, **effort** and **risk**, and every claim is tagged:

- **[register]**: backed by a register-level source (Symbian's `WINDMERE.INC` in the
  emulator checkout, the SDK headers and import libraries, the NetBSD or Linux
  drivers).
- **[sourced]**: a published project or document that did it.
- **[anecdotal]**: a forum post, a mailing-list claim or a single report.
- **[speculation]**: our own reasoning. Not tried by anyone we could find.

The machine, for reference: Windermere SoC (Psion's own ASIC, ARM710T core) at
**36.864 MHz**, 16 MB EDO DRAM, 10 or 16 MB ROM, 640x240 STN panel with 16
frame-rate-modulated greys, two UARTs (one SIR IrDA, one RS-232 via a level
shifter), a PCMCIA-style card interface used as a CompactFlash slot, an 8 kHz
A-law codec, a 12-bit ADC on an SSI bus for the digitiser and the batteries, and
EPOC Release 5 (EKA1 kernel) in ROM.

---

## 1. Ranked shortlist

Best first. "Try safely" says how to do it without risking the machine or the data.

### 1. Serial at 230400 (and perhaps 460800) [register] [sourced]

**Why.** The link is the bottleneck for everything PsiMail and PsiWeb do. The
hardware is not limited to 115200: the Windermere UART baud register is a plain
divisor of a 7.3728 MHz reference. NetBSD's driver computes
`UBRCR = 7372800 / (16 * rate) - 1`
(`sys/arch/epoc32/windermere/wmcom.c`), and Symbian's include agrees
(`WndmBaud115200 EQU 3`). So 230400 is divisor 1 and 460800 is divisor 0.
The Linux 5mx HOWTO states plainly: "Although the 5MX is specified to support
speeds to 115200 baud, in reality speeds of 230400 baud can be obtained."

**What stands in the way.** EPOC's `TBps` enum stops at `EBps115200`
(`d32comm.h:22`), but it also has `EBpsSpecial` with `iSpecialRate` and a
`KCapsBpsSpecial` capability bit. Whether the 5mx's `ECUART` driver honours that
is unknown. The Atom firmware already accepts 230400, 460800 and 921600
(`firmware/atom-modem/src/modem.cpp:187-194`). The RS-232 level shifter on the
5mx board is the other unknown: Linux got 230400 through it, so 230400 is fine;
460800 is [speculation].

**Gain.** Up to 2x link speed for PsiTerm and uploads. For TLS pages the CPU is
the limit (the Atom paces to 5.5 KB/s at 115200 because the ARM can't keep up),
so the gain is mostly for PsiTerm, SFTP and plain HTTP.

**Effort.** Small. `CSerialPort::Probe` in `app/psiterm.cpp:211-252` already
tries `EBpsSpecial` with 230400 and 460800 and reports whether the driver accepts
it. Run it on the device. If the driver refuses, the fallback is a tiny LDD (see
item 3) that pokes `UBRCR` after `ECUART` has configured the port.

**Risk.** None to hardware. Worst case is framing errors.

**Try safely.** Run the probe; if accepted, loop back through the Atom at
230400 with RTS/CTS on and count errors over 1 MB.

### 2. A kernel-side device driver (LDD) built with the SDK [register] [sourced]

**Why.** This is the key that unlocks almost everything else below: registers,
physical memory, the LCD controller, idle/HALT, the card slot's I/O windows and
the UART divisor. It is also proven: **ArLo**, the Linux loader, is a user-mode
EPOC program plus a device driver ("before you run any version of Arlo, you MUST
do a cold reboot first because usually the device driver has been changed.
Unfortunately, the device driver cannot be unloaded other than by rebooting").
It runs on a retail 5mx ROM, so third-party LDDs load and run.

**What the SDK gives us.**
- `makmake` accepts `LDD` and `PDD` target types (`epoc32/tools/mmp.pm:585`),
  with default UID2s `0x100000ae` / `0x100000ad` (`mmp.pm:716-739`).
- `User::LoadLogicalDevice` (`e32std.h:2836`) searches `\System\Libs` on all drives.
- `epoc32/release/marm/rel/ekern.lib` exports the kernel-side classes
  (`DLogicalDevice`, `DLogicalChannel`, `DPhysicalDevice`, `DPlatChunkHw`,
  `TInterrupt::Bind`, `TDfc`, `Kern::*`, `Mmu::AllocShadowPage`,
  `ImpHal::ProcessorClockInKHz`, `Power::AddPowerHandler`, and about 240
  `TEiger::*` register accessors).
- **What it does not give:** any kernel header (`kernel.h`, `k32*.h`) or an
  example driver. The ER5 docs keep saying "available in the Device Driver SDK",
  which we don't have. Class layouts would have to be reconstructed from the
  import library's mangled names, from the ROM, and from ArLo's GPL source
  (SourceForge `linux-7110`, Arlo 2.1.0).
- Two user-side back doors exist in `e32svr.h` and may remove the need for an
  LDD for experiments: `RDebug::SupervisorMode(TBool)` (line 450) and `RMmu`
  (`PhysicalAddress`, `LinearAddress`, `ChunkCreate`, `GetPagePerms`, lines
  507-527). Both are undocumented and untested on a retail ROM [speculation].

**Gain.** Enables items 1, 4, 5, 6 and 9.

**Effort.** Medium to large for the first driver (reconstructing the DDK
contract), small after that.

**Risk.** A kernel fault takes the machine down, and **C: is a RAM disk**: a
crash can lose everything on C:. Nothing is written to ROM or flash, so it
cannot brick the machine.

**Try safely.** Develop in the emulator first. `psionEmulators` runs the real
ROM with the real register map (`core/windermere.cpp`), so an LDD can be loaded
and crashed there with no consequences. On the device, back up C: to CF before
every test, and keep the first driver read-only (dump `PWRCNT`, `PWRSR`, the
`MEMCFG` wait states and the UART registers).

### 3. Use the Atom as a real co-processor [sourced for the pattern] [speculation for us]

**Why.** The Atom is a 240 MHz dual-core ESP32 with Wi-Fi and 500 KB of RAM,
tied to a 36 MHz ARM that takes about a second per TLS handshake and 20 s to lay
out the BBC front page. The project's own plan (`docs/psiweb-modern-web-plan.md`,
stage C2 "PsiProxy") already puts TLS ending, readability extraction and
16-grey GIF conversion on a PC. The same can live in the modem, so there is
nothing else to run. Kian Ryan's **Sidecar** (Pi Zero W as an RS-232 internet
gateway) is the proven shape of this.

**Concrete roles.**
- `ATDT` extended with a TLS-terminating mode: the Psion speaks plain HTTP/IMAP
  to the Atom, the Atom does TLS 1.3 with hardware AES. Saves the 0.4-1 s per
  handshake and the ChaCha20 cost. Trade-off: the Atom sees plaintext, so this
  is only acceptable on Dan's own modem.
- Image decode on the Atom: fetch, scale and dither to 16-grey `.pmi` on the
  ESP32 (its JPEG decoder is ~100x faster), ship the result. The engine's
  `pictures.c` path becomes a passthrough.
- Page simplification: readability on the ESP32 (tight on RAM; a PSRAM Atom
  variant or ESP32-S3 helps), or forward to a small proxy on the LAN.
- Remote compute: an `AT$EXEC` style escape to run a command on a LAN box and
  stream the result. This is how a "dedicated LLM terminal" works: the Psion
  only sends text and renders text.

**Gain.** Large: the ARM stops doing crypto and decoding. **Effort.** Medium,
all in firmware and in the existing engine abstraction. **Risk.** None to the
hardware.

### 4. Bigger UART receive path and real flow control, from the driver side [register]

**Why.** We already see overruns at 115200 without RTS/CTS
(`docs/epoc-comms-best-practices.md:105`). The UART FIFO is tiny, and RTS is not
a UART pin at all: it is a GPIO (`WndmPortCRs232Enable`/PC0 per the emulator's
port map) that EPOC's driver toggles at the 75%/25% marks of its own buffer.
NetBSD's driver has a wry comment that the UART itself cannot do hardware flow
control. An LDD that services the UART interrupt with a larger ring, or that
drives RTS earlier, would make 230400 reliable. Depends on item 2. [speculation]

### 5. Idle, HALT and power measurement [register]

**Why.** `HALT` (0x408) stops the core until the next interrupt; `STBY` (0x40C)
enters standby. EPOC uses them already, but we have no way to measure what our
engines cost. `PWRSR` exposes external-power and battery flags; the main and
backup battery voltages come through the SSI ADC (commands 0xA4A4 and 0xE4E4,
`core/windermere.cpp:225-292`). A read-only LDD could log battery voltage against
time for each app state and tell us whether the 20 ms input poll in PsiWeb or
the engine heartbeats are what is eating the AAs. `UserHal::SupplyInfo`
(`e32hal.h:73`) gives mV and mA from user mode and is the place to start, no
driver needed. **Effort** small. **Risk** none.

### 6. Display: refresh rate and grey quality from the LCD controller [register]

**Why.** The panel is passive STN and the 16 greys are frame-rate modulated,
which is why `ssh/psigrey.h` carries a calibration table. The controller's
registers are known: `LCDCTL` 0x200, `LCDT0/1/2` 0x220-0x228 (horizontal and
vertical timing, polarity), 16-entry palette RAM at 0x1000, and `DBAR1` for the
frame buffer (`WINDMERE.INC`, NetBSD `windermerereg.h`). Options a driver could
test: a higher frame rate (less flicker in mid-greys; costs power and may exceed
the panel's spec), a different grey-to-palette mapping, and double buffering by
flipping `DBAR1` between two buffers for tear-free scrolling in Links and the
terminal. Note the emulator does not model LCD timing at all (`LCDST` reads
0xFFFFFFFF, `LCDINT` never fires), so this one has to be tested on hardware.
**Effort** medium (needs item 2). **Risk** low-moderate: timing values outside
the panel's range could stress the driver electronics; keep changes small and
reversible, and never leave the panel in a non-standard mode across a reset.
External video: there is none; the controller feeds the internal panel only.

### 7. Wi-Fi file drop through the CF slot with a FlashAir in an SD-to-CF adapter [anecdotal] [speculation]

**Why.** The 5mx cannot drive an I/O card (see section 3), but it can talk ATA
to a storage card, and a Toshiba FlashAir is a storage card with a Wi-Fi web
server inside. Photographers run them in SD-to-CF adapters in old cameras
(DigiGear adapter, reports on dpreview). On the Psion it would appear as D:,
and files could be dropped onto it from a browser with no driver at all. Caveats:
the 5mx cuts card power when idle (PB7 "CF power" in the emulator's port map) and
EPOC caches FAT, so new files may not show until a remount
(`UserSvr::ForceRemountMedia` exists in `e32svr.h`); the card draws tens of mA;
FlashAir cards are discontinued and second-hand. **Effort** small, hardware only.
**Risk** none to the machine, moderate to battery life. Not reported on a Psion
by anyone we found.

### 8. Power: USB-C and better cells [anecdotal]

- The 6 V jack is 3.5/1.3 mm, **centre positive** (the opposite of the Series 3).
  A USB-C PD trigger board set to a 6 V-compatible profile, or a 5 V-to-6 V boost,
  works: zedstarr documents powering old Psions from USB DC-DC converters.
- NiMH AAs work; the gauge reads low because it is calibrated for 1.5 V alkaline.
  `UserHal::SetBatteryType` / `SetBatteryCapacity` (`e32hal.h`) exist and could be
  tried from a small utility [speculation]. 1.5 V lithium "AA" cells with a
  built-in regulator give a flat 1.5 V until they die with no warning: avoid,
  because the machine's low-battery warning then never fires and C: is at risk.
- Backup cell: CR2032-type (CJE Micros still stocks it). **If the AAs and the
  backup cell are both flat, C: is gone.** Change the backup cell with the AAs in.
- RTC trim: `RDebug::ReadXtalError` / `SetXtalError` (`e32svr.h:457-458`)
  suggest EPOC has a crystal-error correction; semantics undocumented, worth a
  look if Dan's clock drifts [speculation].

### 9. Overclocking: possible on paper, nobody has done it, and it is not worth it [register] [speculation]

See section 2 for the details. Summary: there is no software-settable PLL; the
only clock bit is `PWRCNT.ClkFlg` ("0=18MHz core clock, 1=36MHz core clock"), and
EPOC already sets it to 36 MHz. A faster core needs a crystal change, which also
shifts the UART reference, the timers, the LCD clock, the codec and the DRAM
timing. Rated as **not recommended**.

### 10. Linux or NetBSD as a second OS [sourced]

OpenPsion's 2.4.27 kernel supports everything on the 5mx (CF, touch, both
serial ports including IrDA, sound play and record, RTC, frame buffer and X) and
boots from EPOC with ArLo. NetBSD/epoc32 (in NetBSD 7.0 onward) is experimental
and covers serial, IR, LCD, keyboard and the CF controller. The data warning is
real: **Linux uses the whole of RAM, so everything on C: is destroyed every time
you boot it** ("if you install it on the C: drive, you will need to reinstall it
each time you boot linux, because linux will use the C: drive as system
memory"). It is a research platform for driver work (its `psionw.h` and
`psionw-arch.c` are the best Windermere documentation outside the NDA'd
"Windermere Software Interface Specification"), not a daily driver.

---

## 2. CPU: clocks, overclocking, cache, SRAM

**What the clock tree looks like [register].**
- The emulator runs the 5mx family at `CLOCK_SPEED = 0x9000*1000` = 36.864 MHz
  (`core/wind_defs.h:10`), the Series 5 at 18.432 MHz, and the Series 7/netBook
  at 221.184 MHz, all multiples of the 3.6864 MHz crystal family MAME uses for
  the Series 5 (`3.6864_MHz_XTAL * 5`).
- `PWRCNT` (0x80000404) bits, from Symbian's `WINDMERE.INC`:
  `Excken 0x1` (EXPCLK always on), `Wakedis 0x2`, **`Clkflg 0x4` "0=18MHz core
  clock, 1=36MHz core clock"**, `AdcclkMask 0xff00` (ADC clock divider).
  Nothing above 36 MHz. The 5mx ROM sets the bit at boot (Ninji: "Set clock
  speed to 36MHz using PWRCNT register").
- `MEMCFG` wait-state fields are specified "@ 36MHz" (`Ac225`…`Ac50`,
  `Sac40/20`), i.e. the ROM and card-slot timings are tuned to this clock.
- The 64 Hz system tick (`TINT`) is derived from the RTC crystal
  (`PWRSR.RtcDiv`), but the nanokernel's 3 ms tick runs from TC2 at 512 kHz,
  which is derived from the main clock (`core/windermere.cpp:2434`).
- The UART reference (7.3728 MHz, i.e. main clock / 5), the codec's 8 kHz, the
  LCD pixel clock and the DRAM refresh all come from the main clock.

**So a "fast 5mx" needs a crystal swap, and then [speculation]:** every
peripheral runs proportionally fast. +8.5% (a 4.0 MHz crystal) would put the
UART 8.5% off every standard rate (async serial tolerates ~2-3%; the Atom could
be set to a matching odd rate, but PsiWin, IrDA and the Test button could not),
make the codec play sharp, shorten the nanokernel tick (EPOC's timers would run
fast relative to the RTC), tighten the ROM/DRAM/CF wait states past what the
`MEMCFG` values assume, and raise the LCD frame rate. The ARM710T core itself
(a 0.35 µm part) would probably tolerate 40 MHz; the surrounding timings are the
problem. No report of anyone trying this on a Series 5, 5mx or Revo was found.
The gain would be under 10% on a machine whose real bottlenecks are the link and
the lack of hardware crypto. **Not recommended.** It also means desoldering a
crystal on a board that is already fragile around the screen flex.

**Series 7 and netBook** are different: the SA-1100's `PPCR` sets the PLL from
software, and Psion's netBook ROM on a Series 7 runs it at 191.692 MHz; "some
Series 7 units failed testing to work reliably at the higher netBook speed"
[sourced, Wikipedia/MobileRead]. Not applicable to the 5mx.

**Cache and write buffer [register].** The ARM710T has an 8 KB unified 4-way
cache with 16-byte lines and a write buffer, both enabled by EPOC. Our code
already respects this (the X25519 field code is kept to about 0.5 KB; `psi_grey.c`
reads pixel pairs as words). There is no internal SRAM on Windermere to run hot
code from (none modelled, none in the memory map). What *is* true is that ROM
code runs in place through the static memory controller's wait states, while
our apps, loaded from C:, run from DRAM: so a RAM copy of a ROM DLL would run
faster, and the loader looks on C: before Z: (`eudll.html`: DLLs are searched on
"D:, A:, B:, C:, E:…Y: and finally, drive Z:"). Copying, say, `EUSER.DLL` to
`C:\System\Libs` is therefore a legal experiment [speculation]: it would cost
RAM, and an incompatible copy would make the machine unbootable until a reset
with the card removed, so only try it with a known-good copy and a backup.

**Measuring instead of guessing.** `TMachineInfoV1::iProcessorClockInKHz` and
`iSpeedFactor` (`e32hal.h:113-114`) report what the ROM thinks the clock is;
`ImpHal::ProcessorClockInKHz` is in `ekern.lib`. PsiTerm's built-in speed test
(`ssh/psishim.c:684-730`) prints KB/s for ChaCha20, Poly1305, AES and SHA-256.
No device numbers are in the repo yet; everything is a 15 MIPS harness estimate.
Running it once would settle whether the harness is 1.5x optimistic, as
`PORTING.md` suspects.

## 3. Memory and the card slot

**More RAM.** The 5mx board has "8-32MB DRAM build options" (the Snowdrop/MX
schematic, per `core/series5.h:1007-1012`), and the ROM probes `DRAM_CFG` at boot
to size the banks. The factory 5mx Pro shipped 24 or 32 MB. So a 32 MB 5mx is a
rework job, not a design change, but it means replacing the EDO DRAM population
on a fine-pitch board with no documentation of the part numbers. No community
report of anyone doing it on a 5mx was found. **High effort, moderate risk (board
damage), untested** [speculation]. The emulator has no 32 MB configuration either.

**CF as swap or XIP.** EPOC R5 has no demand paging, so there is no swap to
enable. The loader does execute-in-place from Z: and C: (`fsintro.html:230`),
not from D:, and running from CF would be far slower than DRAM anyway.

**Faster CF.** The card interface is PCMCIA-style, not True IDE: the card's CIS
is read from attribute memory, the CCR Option register switches it to I/O mode,
and ATA registers sit in a 16-byte I/O window (`core/vcfcard.cpp:23-131`). CF
IREQ# arrives as `EINT3`. There is no DMA (`TDma` exists in `ekern.lib` but the
card path is PIO). Period benchmarks put writes at 0.1-1 MB/s depending on the
card. A driver-level gain is unlikely; a 16-bit I/O window and fewer, larger
transfers are what the ROM presumably already does. Modern CF cards and SD-to-CF
adapters work fine for storage; the FAQ's "128 MB" ceiling is a FAT16 era note.

**I/O cards in the CF slot (Wi-Fi, Ethernet, modem).**
- Under EPOC on the 5mx: **no**. Contemporary reviews: "there is no support for
  Ethernet cards and other such devices". The netBook got Ethernet and some
  Wi-Fi PC Cards with Psion's own alpha drivers; the Series 7 did not even get
  those (and its slot only supplies 300 mA).
- In the SDK: `d_pccdif.h` declares `RPcCardCntrlIf` with `EFNetworkCard`,
  `EPcCardIo8Mem/Io16Mem`, `ReqMem`, `ReqConfig` and an IREQ event, i.e. a
  user-side raw PC Card interface, but `PCCDIF.LDD` is not shipped, and there is
  no Ethernet or Wi-Fi NIF (`tcpintro.html`: "EPOC machines will access TCP/IP
  networks through a dial-up serial connection"). Writing a NIF plus a card
  driver for, say, an old Prism-based CF Wi-Fi card is months of work with no
  kernel headers, and the result would still need WPA, which nothing of that era
  speaks. The Atom on the serial port is the right answer.
- Under Linux: the 5mx HOWTO lists CF storage only; the netBook HOWTO is where
  "most wireless network cards work out of the box".

**A co-processor in CF form [speculation].** The slot carries a 16-bit data bus,
address lines and IREQ#, so a card that implements the CIS and a small ATA
command set can exchange blocks with the Psion with no driver: the Psion reads
and writes sectors of a "disk" that is really a mailbox. The RP2350-based
**PicoIDE** (2026) proves PIO can emulate a PATA drive at full speed, and the
Psion's PIO-mode ATA is a subset. A 50-pin CF Type I form factor is the hard part
(a Type II is 5 mm, which the 5mx also takes). What it would buy over the serial
Atom: ~1 MB/s instead of 11 KB/s for bulk data (page bodies, pictures), with the
Atom still doing Wi-Fi over serial for control. Effort: a board design, firmware
for the card side, and a tiny EPOC file protocol on top of raw sectors
(`RFile` on D: with `ForceRemountMedia`). Nobody has built one for a Psion.

## 4. I/O: serial, IrDA, docking connector, audio, digitiser, GPIO

**Serial.** See shortlist item 1. Facts: two UARTs at 0x80000600 (IrDA) and
0x80000700 (RS-232), registers `DR, FCR, UBRCR, CON, FLG, INT, INTM, INTR`;
`FCR.Ufifoen` enables a FIFO whose depth is not documented (the Atom firmware
assumes 16 bytes); `FLG` carries CTS/DSR/DCD inputs on UART2 only; RTS and DTR
are GPIO outputs PC0/PC1. The RS-232 connector pinout gives the Psion a full
set: TXD, RXD, RTS, CTS, DTR, DSR, DCD, RI and GND, which is why psiglue can arm
`KConfigFailDCD`. The Atom's three-wire build loses all of that; wiring its DCD
GPIO through the MAX3232 to pin 1 (already documented in the firmware README) is
the cheapest reliability win on the table.

**IrDA.** SIR only: `UARTCON.Siren` "set to enable SIR (UART1 only)" and `Irtxm`
"reduce IR pulse width". The SDK's `KCapsSIR4Mbs` flag is a generic capability
bit; no Psion of this family had FIR hardware. The IR UART has the same divisor
register, so 230400 over IR is a register possibility, but IrDA SIR encoding
specifies 115200 as the top rate and the transceiver is unlikely to follow
[speculation]. Also "the use of IRDA is meant to consume a considerable amount
of power". As a high-speed link it is a dead end; as a second serial port for a
second Atom it is not, which is a cheap way to get two links.

**Audio.** Codec registers `CODR 0xA00, CONFG 0xA04, COLFG 0xA08`, 8-bit samples at
8 kHz, 16-entry FIFOs. EPOC's `RDevSound` offers `PlayAlawData` and
`RecordAlawData` only ("Playback is fixed at 8KHz", `edsound.html`), with
simultaneous play and record as a capability. 8 kHz A-law **is** G.711a, so the
Psion is natively a VoIP terminal: the Atom could run SIP and RTP and pass raw
A-law frames over the serial line. 64 kbit/s each way is 8 KB/s, inside a
115200 link's 11.5 KB/s per direction if the Atom strips the RTP headers; at
230400 it is comfortable. Latency would be hundreds of milliseconds. Audio
streaming (internet radio transcoded to 8 kHz A-law on the Atom) is the same
pipe, one way. Nobody has done either [speculation]. "Modem over the codec" is
pointless when there is a real UART.

**Digitiser and batteries.** The touch screen and both battery voltages are
read through the SSI (`SSCR0/1, SSDR, SSSR`) with the ADC commands above. There
are no spare ADC inputs exposed outside the case.

**GPIO.** Ports A-E at 0xE00-0xE24. The known pins are all spoken for (codec,
amp, LCD power, CF door and power, sled, pump, EEPROM, contrast, case-open
switch, RTS/DTR, power LED, backlight, UART enables). Nothing is brought out to
the docking connector except the RS-232 signals, so "extra inputs" means
sacrificing one of those (RI or DSR as a button input, for instance, via
`FLG`/`INT.Ms` on UART2 [speculation]).

## 5. Display

What is already done in the apps: `EGray16` bitmaps blitted straight in, a
calibrated grey table (`ssh/psigrey.h`), word-wide conversions. Beyond that:

- `UserSvr::ScreenInfo` (`e32svr.h:97`, `TScreenInfoV01::iScreenAddress`) hands a
  user process the frame-buffer address. Writing to it directly bypasses WSERV
  and the bitmap copy, which is what the window server itself does. It is
  undocumented and the window server will overdraw on redraw, so it only suits a
  full-screen mode (terminal, Links). [register, untested]
- Everything else needs the controller registers (shortlist item 6).
- Contrast is `UserHal::SetDisplayContrast` and is already used for "Reading
  mode"; the backlight is a GPIO behind `UserHal::SetBacklightOn`.
- The palette RAM at 0x1000 defines how the 16 logical greys map to the panel's
  modulation; it is in the hardware register block, so it needs supervisor mode.

Community display mods [sourced, hpcfactor 2024 thread]: removing the
touch-screen layer and the diffuser film "even better contrast" (but no touch),
a frosted film between LCD and backlight. There is "no space at all" for an LED
backlight; the original is an EL foil driven by an HV823, and the realistic
replacement is a new EL foil, which is why 5mx backlights are dim and whine.
Flex cable: an open-source replacement flex exists (PCBWay shared project) and
there are several disassembly guides; it is the number one failure and the
reason to minimise opening the case.

## 6. OS-level

- **Kernel extensions and drivers:** shortlist item 2. `makmake` has `LDD`/`PDD`
  targets, `ekern.lib` has the exports, the docs point at a DDK we lack, ArLo
  proves it works on a retail ROM.
- **Patching ROM behaviour from RAM:** three legitimate hooks. (a) The DLL search
  order puts C: before Z:, so a patched copy of a ROM DLL on C: wins for
  programs loaded afterwards. (b) `Mmu::AllocShadowPage` / `FreezeShadowPage`
  are exported by `ekern.lib`: EKA1 can shadow a ROM page in RAM, the mechanism
  Symbian used for ROM patches. (c) `UserSvr::ChangeLocale(RLibrary)` swaps the
  locale DLL at run time. All [register], all untested here.
- **The 5mx Pro route:** the Pro has no OS ROM, only a 128 KB AT29LV010A
  bootloader that loads `SYS$ROM.BIN` from the CF card (or over YMODEM) into a
  protected 10-12 MB of RAM. The Psion-ROM repository on GitHub keeps stock and
  patched images. A Pro is therefore the "patch the OS" platform: edit the image
  on a PC, copy to CF, boot. A standard 5mx has an 8 MB mask ROM plus a 2 MB
  flash (build 250) or a 16 MB mask ROM (build 260). **Never write to that 2 MB
  flash** (see warnings).
- **Second OS:** shortlist item 10.
- **Modern toolchains:** no project was found that builds ER5 code with a gcc
  newer than the Psion 98r2 series. The obstacles are the old APCS calling
  convention and GNU-COFF object format expected by the SDK's `.lib` import
  libraries and `petran`, not the ARM7 target itself. Pure-C engine objects
  built with `arm-none-eabi-gcc -mcpu=arm7tdmi -mapcs-frame` and linked with
  the old linker are the plausible experiment; C++ with EIKON is not
  [speculation]. A modern C library is the same story: ESTLIB is what links.

## 7. Power

- Idle: EPOC writes `HALT` in its null thread; `STBY` is the deep state the
  power handler uses on switch-off. There is no lighter "clock down to 18 MHz"
  state in use, but `PWRCNT.ClkFlg` would allow it: a driver could run the core
  at 18 MHz while idle in PsiTerm waiting for keys, halving CPU power
  [speculation; it would also halve the UART reference, so the divisor must be
  rewritten in the same instant, which is why nobody does this].
- `UserHal::SupplyInfo` gives mV and mA now; log it before trying anything else.
- See shortlist item 8 for cells, USB-C and the backup battery.

## 8. Hardware mods in the community (what exists)

| Mod | Status | Source |
|---|---|---|
| Screen flex replacement | Several guides; open-source flex PCB | [sourced] R3UK, psionwelt, PCBWay |
| Backlight | EL foil replacement; LED impractical (no depth) | [anecdotal] hpcfactor 2024 |
| Contrast by removing touch layer/film | Done, loses touch | [anecdotal] hpcfactor |
| Serial connector replaced by pin header | Done | [anecdotal] hpcfactor |
| Hinge wire, keyboard swap | Documented | [sourced] petervis, service manual |
| Internal Bluetooth (UART-attached module) | 2000s "Bluetooth surgery" page (site now unreachable); the unreleased Conan had a UART Bluetooth stack | [anecdotal] |
| Wi-Fi via RS-232 (Sidecar, Pi Zero W) | Works | [sourced] Kian Ryan, The Register |
| ESP32 Hayes modems | Several, including ours | [sourced] |
| Pi inside the shell (PsionPi, Psioπ, PsiOnSD) | Replace the mainboard, keep keyboard/screen; a 17 mm board does not fit, a Pi Zero does | [sourced] hackaday.io, osresearch |
| "The Last Psion" Wi-Fi pack | SIBO (Series 3), not EPOC | [sourced] |
| 32 MB RAM on a 5mx | Nothing found | - |
| Overclock | Nothing found | - |
| USB-C power | Trigger boards to 6 V, centre positive | [anecdotal] zedstarr |

## 9. Everything assessed

| Idea | Feasible? | Gain | Effort | Risk | Evidence |
|---|---|---|---|---|---|
| 230400 serial via `EBpsSpecial` | Likely | High for PsiTerm/SFTP | Small | None | [register][sourced] |
| 460800 serial | Divisor 0 exists | Medium | Small | Framing errors | [register][speculation] |
| 230400 via LDD poking `UBRCR` | Yes | As above | Medium | Crash = C: loss | [register] |
| Third-party LDD/PDD | Yes (ArLo) | Enabler | Medium-large | Crash = C: loss | [sourced][register] |
| `RDebug::SupervisorMode` / `RMmu` from user mode | Unknown | Enabler | Small | Crash | [register][speculation] |
| Atom as TLS/image/readability co-processor | Yes | High | Medium | None | [sourced pattern] |
| Remote-compute / LLM terminal via Atom | Yes | High | Small | None | [speculation] |
| Driver-side UART ring and early RTS | Yes | Medium | Medium | Crash | [speculation] |
| Battery/idle measurement (`SupplyInfo`, LDD) | Yes | Insight | Small | None | [register] |
| LCD refresh/palette/double-buffer via driver | Yes | Medium (flicker, tearing) | Medium | Panel stress | [register][speculation] |
| Direct frame-buffer writes via `ScreenInfo` | Yes | Small | Small | WSERV fights | [register] |
| External video | No | - | - | - | [register] |
| FlashAir in SD-to-CF adapter | Probably | File drop over Wi-Fi | Small | Battery drain | [anecdotal] |
| CF-form co-processor card (RP2350 ATA emulation) | Plausible | ~1 MB/s bulk path | Large | Slot damage if wrong | [speculation] |
| CF Wi-Fi/Ethernet I/O card under EPOC | No driver, no NIF | - | Months | - | [sourced: not supported] |
| Faster CF / DMA | No DMA on card path | - | - | - | [register] |
| CF as swap / XIP | No paging in R5 | - | - | - | [sourced] |
| 32 MB RAM rework | Board supports it | Medium | Large | Board damage | [register][speculation] |
| Overclock via crystal | Mechanically yes | <10% | Large | Serial, timers, panel, flash timing | [register][speculation] |
| `Clkflg` 18 MHz idle mode | Yes via driver | Battery | Medium | UART divisor must follow | [register][speculation] |
| ROM DLL override from C: | Yes | Varies | Small | Unbootable until reset | [sourced] |
| ROM shadow pages | Exported | Patch ROM | Large | Crash | [register] |
| 5mx Pro patched `SYS$ROM.BIN` | Yes | Patch OS | Medium | Needs a Pro | [sourced] |
| Linux (OpenPsion) | Yes | Research | Medium | **Wipes C:** | [sourced] |
| NetBSD/epoc32 | Experimental | Research | Medium | Wipes C: | [sourced] |
| Newer gcc for ER5 | Nobody has | - | Large | - | [speculation] |
| IrDA as fast link | SIR only | - | - | - | [register] |
| IrDA as a second serial port | Yes | Second link | Small | Power | [sourced] |
| VoIP / audio streaming via codec | Yes (G.711a native) | Novelty | Medium | None | [register][speculation] |
| Extra inputs via modem-control pins | Yes | Novelty | Small | None | [speculation] |
| USB-C power | Yes | Convenience | Small | Polarity! | [anecdotal] |
| NiMH + gauge fix | Yes | Convenience | Small | 1.5 V Li cells hide low battery | [sourced][speculation] |
| RTC trim via `SetXtalError` | Exists | Accuracy | Small | Unknown units | [register] |
| Internal Bluetooth/ESP32 on the UART pads | Done once (2000s) | Cable-free | Medium | Flex, soldering | [anecdotal] |
| Pi inside | Replaces the Psion | Not a Psion any more | Large | - | [sourced] |

## 10. Warnings

- **C: is RAM.** Every kernel-mode experiment, every Linux boot and every
  flat-battery event can erase it. Back up to CF before each test, and keep the
  backup cell fresh. "If your machine's batteries go completely flat (including
  Series 3/5 backup batteries), your data is completely lost."
- **Never write to the ROM-space flash.** A build-250 5mx has a 2 MB flash part
  alongside the mask ROM, and a 5mx Pro has a 128 KB bootloader flash on CS7.
  `TMemoryInfoV1::iRomIsReprogrammable` exists for a reason. A bad write to
  either makes a machine that will not boot and cannot be recovered without a
  programmer and a known-good dump. Nothing in this document needs it.
- **Crystal or voltage changes** touch everything: the UART reference, the CF
  timing, the LCD and the codec all derive from the main clock. The CF slot is
  3.3 V; the external jack is 6 V centre positive, and reversed polarity or a
  12 V PD profile will damage the board.
- **Linux boots destroy C:** by design; it is not dual boot in the PC sense.
- **LCD timing registers:** values outside the panel's range stress the panel
  driver; change one field at a time and never persist a change.
- **Kernel drivers:** cannot be unloaded once loaded ("the device driver cannot
  be unloaded other than by rebooting"); a buggy one means a reset, which on a
  5mx is usually survivable but is still a C: risk if the reset is a cold one.
- **Opening the case:** the screen flex is the component most likely to fail;
  each disassembly is a gamble. Order a replacement flex before opening.
- **Untested APIs:** `RDebug::SupervisorMode`, `RMmu`, `ScreenInfo`, shadow pages
  and `SetXtalError` are in the headers but undocumented. Treat each as a
  possible instant panic.

## 11. Sources

Register-level and primary [well-sourced]:
- Symbian `WINDMERE.INC` (1998-1999), in the emulator checkout:
  `/home/daniel/www/psion/psionEmulators/tests/windmere.inc` (PWRCNT, PWRSR, MEMCFG,
  LCD, UART, timers, SSI, GPIO definitions).
- psionEmulators core: `/home/daniel/www/psion/psionEmulators/core/windermere.cpp`,
  `wind_defs.h`, `etna.cpp`, `vcfcard.cpp`, `audio_codec.h`.
- EPOC R5 SDK: `epoc32/include/e32svr.h` (RDebug, RMmu, UserSvr::ScreenInfo),
  `e32hal.h` (UserHal, TMachineInfoV1, TSupplyInfoV1), `d32comm.h` (TBps,
  EBpsSpecial, KCapsSIR*), `d32snd.h`, `d_pccdif.h`, `epoc32/tools/mmp.pm`
  (LDD/PDD targets), `epoc32/release/marm/rel/ekern.lib` (kernel exports),
  `sysdoc/cpp/drivers/edsound.html`, `sysdoc/cpp/e32/eudll.html`,
  `sysdoc/cpp/f32/fsintro.html`.
- NetBSD windermere UART driver (baud formula):
  https://raw.githubusercontent.com/NetBSD/src/trunk/sys/arch/epoc32/windermere/wmcom.c
  and registers: https://raw.githubusercontent.com/NetBSD/src/trunk/sys/arch/epoc32/windermere/windermerereg.h
- NetBSD/epoc32 port page: https://www.netbsd.org/ports/epoc32/index.html
- Ninji, "16 Shades of Grey", building a Psion/EPOC32 emulator (PWRCNT at boot,
  memory map, the NDA'd Windermere spec): https://wuffs.org/blog/building-a-psion-emulator
- MAME `psion5.cpp` (Series 5 clock = 3.6864 MHz x 5):
  https://raw.githubusercontent.com/mamedev/mame/master/src/mame/psion/psion5.cpp
- Linux on the Psion 5mx HOWTO (hardware support, ArLo, 230400, data warnings):
  https://linux-7110.sourceforge.net/howtos/series5mx_new/t1.htm ,
  serial section https://linux-7110.sourceforge.net/howtos/series5mx_new/x473.htm ,
  ArLo section https://linux-7110.sourceforge.net/howtos/series5mx_new/x192.htm
- OpenPsion project page: https://linux-7110.sourceforge.net/
- ArLo files and source (GPL): https://sourceforge.net/projects/linux-7110/files/Arlo/
- Psion-ROM repository (5mx builds, 5mx Pro bootloader and SYS$ROM.BIN):
  https://github.com/explit28/Psion-ROM
- PicoIDE (RP2350 PIO ATA emulation): https://picoide.com/ ,
  https://www.cnx-software.com/2026/02/02/picoide-an-open-source-hardware-ide-atapi-drive-emulator-for-vintage-computers/
- Project files cited: `app/psiterm.cpp:211-252` (baud probe),
  `firmware/atom-modem/src/modem.cpp:187-194` (Atom baud list),
  `ssh/psiglue.cpp:1113-1137` (handshake), `ssh/psigrey.h`, `web/links/psi_grey.c`,
  `web/links/PORTING.md` (timings), `docs/psiweb-modern-web-plan.md` (PsiProxy).

Community and secondary [anecdotal unless noted]:
- Kian Ryan, Sidecar (Pi Zero W RS-232 gateway) [sourced]:
  https://www.theregister.com/2023/01/03/sidecar_getting_psions_online/ ,
  https://www.kianryan.co.uk/2022-06-13-connecting-a-psion-to-a-raspberry-pi-with-serial/
- hpcfactor "Psion 5 - Thoughts and Mods" (2024: contrast, backlight space, EL/HV823, pin header):
  https://www.hpcfactor.com/forums/forums/thread-view.asp?tid=21517&start=1
- hpcfactor "Backlight on a Psion 5": https://www.hpcfactor.com/forums/forums/thread-view.asp?tid=21343&start=1
- R3UK 5mx disassembly and screen cable repair:
  https://www.r3uk.com/index.php/tech-tips/34-disassembly-guides/9-psion-series-5mx-disassembly-and-screen-cable-repair
- Open-source flex replacement: https://www.pcbway.com/project/shareproject/Simple_flex_replacement_Psion_5mx_614e6774.html
- psionwelt screen cable workshop: https://www.psionwelt.de/linkliste/118-workshops-ws/483-changing-the-screen-cable-of-the-psion-5mx-imre-oliver-kozak.html
- Pi-inside projects: https://github.com/osresearch/psionpi , https://hackaday.io/project/4042-psio ,
  https://hackaday.io/project/167400-psionsd , https://www.recantha.co.uk/blog/?page_id=22470
- The Last Psion (SIBO Wi-Fi pack): https://hackaday.io/project/161291-the-last-psion
- 5mx vs 5mx Pro (RAM-only design, SYS$ROM from CF): https://groups.google.com/g/comp.sys.psion.misc/c/v4vwhUowqvc
- 5mx Pro 32 MB CF under Linux: https://sourceforge.net/p/linux-7110/mailman/linux-7110-psion/thread/3C6387DA.A3B03141@yipton.net/
- Series 7 vs netBook PC Card support and clock: https://www.filesaveas.com/psionconnecting.html ,
  https://en.wikipedia.org/wiki/Psion_Series_7 , https://wiki.mobileread.com/wiki/Psion_netbook
- The Gadgeteer 5mx review ("no support for Ethernet cards"): https://the-gadgeteer.com/1999/08/29/psion_series_5mx_review/
- CF write benchmarks on the 5mx: https://groups.google.com/g/comp.sys.psion.misc/c/6NvmihfDyfY
- Psion FAQ (batteries, data loss): https://www.filesaveas.com/psionfaq.html
- Backup cell: https://www.cjemicros.co.uk/micros/individual/newprodpages/prodinfo.php?prodcode=BAT-BAC5MX
- USB power for old Psions (6 V centre positive): https://zedstarr.com/2022/09/25/powering-old-consumer-electronics-from-usb/
- FlashAir in a CF adapter: https://www.bhphotovideo.com/c/product/1312919-REG/digigear_sdxcf1_slim_csd_sdhc.html ,
  https://www.dpreview.com/forums/thread/3880019
- 5mx serial pinout: https://www.kianryan.co.uk/2023-06-22-connecting-rc2014-to-psion-or-other-rs232-terminal/
- Psion 5mx Bluetooth surgery (2000s, site unreachable at time of writing):
  http://www.penguin.cz/~utx/zaurus/www.iral.com/~albertr/linux/psion5mx/blue/
- Wikipedia talk page on the 5mx SoC: https://en.wikipedia.org/wiki/Talk:Psion_Series_5
