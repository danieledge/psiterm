# Hardware and wiring guide: AtomS3 Lite + Atomic RS232 Base + the Psion cable

This guide is for the parts Dan owns: an **M5Stack AtomS3 Lite**, an
**M5Stack Atomic RS232 Base**, a **female DE-9 breakout with a screw
terminal**, the **null-modem cable** that already sits between his Psion
cable and his WiRSa, and the **Psion's own serial cable**, which is never
cut, stripped or resoldered. Nothing here needs a USB host shield, another
board, a custom PCB or a modified cable.

The wiring is specified from the documented pinouts and from one anchor:
**the Atom presents exactly the DE-9 a WiRSa presents**, so the chain that
works today (Psion → Psion cable → null-modem cable → WiRSa) works with the
Atom in the WiRSa's place, unchanged. Statements are marked **[verified]**
(from the sources in section 8, or by that equivalence) or **[not tested on
the bench]** (right on paper; the live run is in VALIDATION.md).

## 1. Electrical safety (read before anything else)

**The Psion's serial port is true RS-232: its signals swing between about
−12 V and +12 V (bipolar). The ESP32-S3 in the AtomS3 Lite runs 3.3 V logic
and is not RS-232 tolerant, nor 5 V tolerant. Connecting any Psion serial
signal (TXD, RXD, RTS, CTS, DTR, DSR, DCD, RI) to an ESP32-S3 pin destroys
the ESP32-S3 and can damage the Psion's port.**

- **An RS-232 transceiver is mandatory.** The Atomic RS232 Base contains one
  (a MAX232-family part), and it is the interface this project uses.
- **The ATOMIC Proto Kit converts nothing.** It brings the AtomS3 Lite's
  3.3 V pins out to pads. Never wire a Psion, a DE-9 breakout or any RS-232
  device to the Proto Kit, to the AtomS3 Lite's header pins, to the
  ESP32-S3's UART pins or to any unprotected 3.3 V input. It is mentioned
  here only to say *do not do this*.
- **The handshake lines are RS-232 too.** DE-9 pins 7 and 8 (RTS/CTS) swing
  the same ±12 V as pins 2 and 3. If they are ever used (section 7), they go
  through a MAX3232 channel as well: never from a DE-9 pin to G7, G8, G1,
  G2 or any ESP32 pin directly.
- Never connect an external voltage to a power pin of the Base or the Atom
  without checking the Base's own documentation (section 5).
- Never infer pin order or connector orientation from a photograph. Read
  the labels on the Base and the numbers moulded into the DE-9.
- **Gate:** no Psion serial connection is powered or tested until every item
  of the checklist in section 6 has been gone through. This applies
  to the modem mode, to every hand-off feature, and to any future mode
  (keyboard emulation, USB HID) that uses the same line.

## 2. The two kinds of serial, and the chain

| Segment | Level | Carries |
|---|---|---|
| AtomS3 Lite header → RS232 Base | **3.3 V TTL UART** (inside the stacked boards; nothing to wire) | TXD, RXD |
| RS232 Base screw terminal → DE-9 breakout → Psion cable → Psion | **RS-232, ±12 V** | TXD, RXD, GND (the Base has no RTS/CTS, DCD or DTR) |

```
 AtomS3 Lite ──(stacked on)── Atomic RS232 Base            ┐
                                   │ screw terminal: G, T, R  │  = the WiRSa's DE-9,
                                   ▼                          │    pin for pin
                      three wires: T → pin 2, R → pin 3,      │
                                   G → pin 5                  │
                                   ▼                          │
                      female DE-9 breakout (screw terminal)   ┘
                                   ▼
                      the existing null-modem cable (the one between the Psion cable and the WiRSa today)
                                   ▼
                      the Psion's own serial cable (unmodified; female DE-9 at this end)
                                   ▼
                              Psion Series 5mx
```

Every joint is a screw terminal or a plug: the chain comes apart without
tools and the Psion cable is untouched. The adapter adds **no crossover**:
the null-modem cable is the crossover, as it is with the WiRSa.

## 3. The wiring, and why it is right

### The two ends are both wired as modems (DCE)

- **The Psion cable [verified, source 1]:** its PC end is a **female DE-9
  socket**, and the Psion is presented as a **DCE**: at that DE-9, **pin 2
  carries the Psion's transmit** (data out of the Psion), **pin 3 the
  Psion's receive**, **pin 5 signal ground**. The handshake lines are wired
  too: pin 4 DTR, pin 6 DSR, **pin 7 RTS, pin 8 CTS** (the Psion's CTS on
  pin 7, its RTS on pin 8), pin 9 RI/DCD. This is why the cable plugs
  straight into a PC's male DTE port.
- **The WiRSa [verified, source 2]:** a **female DE-9** in the modem (DCE)
  convention: its **TX on pin 2, RX on pin 3, GND on pin 5**, with the full
  RS-232 set available on its board.
- **The Atom [verified by equivalence]:** the Atomic RS232 Base's **T** is
  the transceiver's RS-232 output (data from the Atom), **R** its input,
  **G** ground. Wired **T → DE-9 pin 2, R → pin 3, G → pin 5** on a
  **female** breakout, the Atom's DE-9 is the WiRSa's DE-9, pin for pin.
  This is the mapping the 1.x README has carried since the first build
  (source 3).

### One crossover, in the null-modem cable

Two DCE-wired DE-9s both put "data out" on pin 2. Joined by a straight
cable they would both talk on pin 2 and nothing would arrive; the
**null-modem cable crosses 2↔3** (and 7↔8) so that each side's pin 2 lands
on the other's pin 3. That is the cable Dan already uses between the Psion
cable and the WiRSa, and it is the reason his chain needs it. **The adapter
must therefore not cross anything**: T to 2, R to 3, nothing else, or there
would be two crossovers and the line would be dead again.

### Gender

Both the Psion cable's end and the WiRSa are female; the null-modem cable
has the two male ends that join them. The Atom's breakout is **female** for
the same reason: **use the same null-modem cable that works with the
WiRSa**. No gender changer, no adapter.

### `AT$SWAP`

The default wiring (T → 2, R → 3) is the correct one and should not need
swapping. `AT$SWAP=1` (swapping the Atom's two UART pins in software)
remains as a convenience for a differently wired breakout; it is not part
of this build.

### Handshake lines: a known limitation of the stock Base

The Psion cable carries RTS/CTS (pins 7/8) and the null-modem cable crosses
them, so the Psion's handshake *reaches* the breakout. But the stock
Atomic RS232 Base breaks out **T, R and G only**: it has no transceiver
channels for RTS/CTS, DCD or DTR. **Hardware flow control is not available
with the Base alone** — the firmware's pacing is the flow control, as in
1.x, and 230400 with the Base is pacing-only (VALIDATION.md). Pins 7 and 8
may be bridged at the breakout, as the 1.x README allowed, so the Psion
sees its own RTS come back as CTS should "RTS/CTS" ever be selected in the
apps; with "Flow control: None" (the setting for the Base) it does nothing.
Wiring real flow control would take further MAX3232 channels beyond the
stock Base: a **future four-wire option** (section 7), not a blocker.

### The Base's power terminal

The Base also has a **power terminal** (its own DC input for a stand-alone
Base). It is **not used** here and is never wired to the DE-9 or to
anything else (section 5).

## 4. The definitive pinout

| RS232 Base terminal | wire | Atom's DE-9 (female) | the WiRSa's DE-9 | after the null-modem cable, at the Psion cable's DE-9 |
|---|---|---|---|---|
| **T** (data from the Atom) | → | **pin 2** | TX, pin 2 | pin 3: the Psion's receive |
| **R** (data to the Atom) | ← | **pin 3** | RX, pin 3 | pin 2: the Psion's transmit |
| **G** | — | **pin 5** | GND, pin 5 | pin 5: signal ground |
| (none) | | pins 7–8: optionally bridged | RTS/CTS on the WiRSa board | pins 8/7: the Psion's RTS/CTS (not used by the Base) |
| (none) | | pins 1, 4, 6, 9: unconnected | | DCD/DTR/DSR/RI: not used |
| power terminal | | **nothing** | | |

Firmware side (fixed, `AT$PINS?`): the Base's transceiver is on the
AtomS3 Lite's **G5 (RX from R) and G6 (TX to T)** [verified, M5Stack's
AtomS3 pinout for the Base]. These are TTL and stay inside the stacked
boards; nothing TTL leaves the Base.

## 5. Power (what is known, what is not)

- **[verified]** The AtomS3 Lite is powered through its USB-C at 5 V and has
  no VBUS switch; in device mode it draws its own supply from the cable.
- **Known-safe arrangement for all testing in this release:** power the
  AtomS3 Lite from its **USB-C** with a **USB wall charger or a PC**, and
  leave the Base's power terminal **unconnected**. The iPhone is on WiFi
  only (section 8 of UPGRADE.md): no USB cable to the phone, so there is no
  question of the phone powering anything or being charged by anything.
- **The Base's power terminal** is a DC input for a stand-alone Base. It is
  not used in this build. **Never connect it while the USB-C is powered**
  (two sources on one rail), and never connect it to the Psion or the DE-9.
- **Back-feed risk:** any 5 V on the Atom's header "5V" pin reaches the
  USB-C VBUS pin on this family of boards. With the Proto Kit, a bench
  supply on 5V plus a PC on USB-C would be two sources. Do not.
- **The iPhone and USB (future, not this release):** an iPhone will not
  power the Atom and the Base while doing USB networking in a useful way,
  and the Atom cannot charge the phone meaningfully; a USB-host test needs
  its own 5 V supply arranged as described in `USB-TETHERING.md`. Not part
  of the validated set-up.
- **Current:** the AtomS3 Lite with WiFi active peaks around 300 mA; the Base
  adds a few mA. Any USB charger covers it.

## 6. Pre-test checklist (all items, every time the wiring changes)

With everything unplugged and unpowered unless the step says otherwise:

- [ ] The RS232 Base's terminals are identified by their **printed labels**
      (G, T, R, power), not by position or a photo.
- [ ] The Base is seated on the AtomS3 Lite in the orientation the pins
      allow (it only fits one way; do not force it).
- [ ] The Base's **power terminal has nothing connected**.
- [ ] Signal ground: Base **G → DE-9 pin 5** only.
- [ ] **T → pin 2, R → pin 3** (section 4), and nothing crossed in the
      adapter: the null-modem cable is the crossover.
- [ ] Handshake: pins 7–8 bridged or left open (either is fine with the
      Base); the apps set to *Flow control: None*.
- [ ] No DE-9 pin touches the Atom's 3V3 or 5V, the USB-C connector, or the
      Base's power terminal (look, and a meter if in doubt, unpowered).
- [ ] The chain is: Atom's female DE-9 ← **the null-modem cable that works
      with the WiRSa** ← the Psion cable, unmodified.
- [ ] The AtomS3 Lite is powered from USB-C (charger or PC) and nothing else.

Then, in this order: power the Atom (LED red: no WiFi, or green: WiFi up);
connect the DE-9 chain exactly as it was on the WiRSa; open PsiTerm's
Connection settings at 115200, flow control None; **Test**. "The modem
answered OK at 115200 baud" and "Modem: Atom modem 2.0" is the expected
result. Do not try other speeds until 115200 is clean.

## 7. The optional four-wire build: RTS/CTS for 230400

**Not the primary build, and not a requirement.** Three wires plus the
firmware's pacing is the proven baseline (it is almost certainly what the
WiRSa does too: it paces rather than doing true RTS/CTS). This build is an
enhancement for *reliable* 230400 and up, where a 16-byte burst leaves the
Psion's driver 0.7 ms to answer its FIFO interrupt and hardware flow
control lets the Psion stop the modem instead of hoping. Status: the
firmware side is **Implemented** and **Compiles**; the AUTO-pacing change
with `AT$FC=1` is **Host-unit-tested**; the wiring and the live 230400 run
are **Not tested (needs bench)**.

### What the firmware does with `AT$FC=1` (verified in the source)

`AT$FC=1` is not a flag that merely changes the pacing. It goes through the
schema's `SAPins` action to `AtomHal::ApplyPins`, which restarts the UART
through `StartSerial()` (`src/board/hal_esp.cpp`):

```
Serial2.begin(iBaud, SERIAL_8N1, iRx, iTx);
if (iFlow && iRts >= 0 && iCts >= 0)
	{
	Serial2.setPins(iRx, iTx, iCts, iRts);
	Serial2.setHwFlowCtrlMode(UART_HW_FLOWCTRL_CTS_RTS, 64);
	}
else
	Serial2.setHwFlowCtrlMode(UART_HW_FLOWCTRL_DISABLE, 64);
```

In the Arduino-ESP32 3.1.3 core, `HardwareSerial::setHwFlowCtrlMode` is
`uartSetHwFlowCtrlMode`, which calls the IDF driver's
`uart_set_hw_flow_ctrl(uart->num, mode, threshold)`
(`cores/esp32/esp32-hal-uart.c:384`), and `setPins` assigns the CTS and RTS
GPIOs with `uart_set_pin` (`esp32-hal-uart.c:213–267`). So the ESP32-S3's
UART hardware stops transmitting when its CTS input is deasserted and
deasserts its RTS output when its receive FIFO holds 64 bytes. The pins are
the board defaults in `src/board/boards.h` (**G7 = RTS out, G8 = CTS in**
on the AtomS3 Lite) or whatever `AT$PINS=tx,rx,rts,cts,dcd` sets;
`AT$FCSWAP=1` swaps the two. If a pin is missing (−1), the firmware logs
*"RTS/CTS asked for but this board has no pins set for them: flow control
off"* and runs without it. With the RS232 Base nothing is wired to G7/G8,
so `AT$FC=1` on the Base simply has no effect on the line.

### The gating fact: which GPIOs are free when the Base is fitted

The AtomS3 Lite exposes **G5, G6, G7, G8, G38, G39, 5V, 3V3, GND** on its
bottom header and **G1, G2, 5V, GND** on its Grove socket. The Atomic RS232
Base plugs onto the **bottom header** and uses **G5 and G6** for its
transceiver; its documentation does not show it re-exposing the header's
other pins (G7, G8, G38, G39, 3V3), so while it is fitted they must be
treated as **not accessible**. The **Grove socket stays accessible** (it is
on the Atom's body), giving **G1 and G2** plus 5 V and GND, but **no 3.3 V**.

That gives two honest paths:

- **Path A, keep the Base, add a second transceiver on the Grove pins:** a
  MAX3232 breakout for RTS/CTS only, on **G1 (RTS out) and G2 (CTS in)**,
  with `AT$PINS=6,5,1,2,-1` and `AT$FC=1`. Its VCC must be **3.3 V** (a
  MAX3232 fed 5 V would put 5 V on the Atom's CTS pin, which is not 5 V
  tolerant), and the Grove socket has only 5 V: so the breakout must have
  its own 3.3 V regulator, or a small LDO is added between Grove 5 V and the
  breakout's VCC. The two transceivers (the Base's and the breakout's) and
  the Atom share **one ground** (Grove GND, and DE-9 pin 5). Workable, but
  two boards, a regulator, and the DE-9 fed from two places.
- **Path B (recommended), drop the Base, one dual-channel MAX3232 breakout
  for all four signals on the bare AtomS3 Lite header:** channel 1 for
  TXD/RXD, channel 2 for RTS/CTS, powered from the header's **3V3** with
  the header's **GND**. The firmware's default pins are exactly these
  (`AT$PINS?` → `6,5,7,8,-1`), so only `AT$FC=1` and `AT&W` are needed. One
  board, one ground, one DE-9.

### Path B wiring (the DCE/WiRSa convention kept, the null-modem cable unchanged)

The Atom is the modem (DCE). On a DCE's DE-9, **pin 7 is the RTS input**
(the terminal's RTS arrives there) and **pin 8 is the CTS output** (the
modem says "send"). The null-modem cable crosses 7↔8, and at the Psion
cable's DE-9 pin 8 is the Psion's RTS (output) and pin 7 its CTS (input)
(source 1). So after the crossover: the Psion's RTS lands on the Atom's
**pin 7**, and the Atom's "you may send" must leave on **pin 8** to reach
the Psion's CTS. Wiring the Atom's RTS output to pin 7 instead would put
two outputs on one line through the null-modem: it would not work and it
is not the DCE convention.

| AtomS3 Lite (3.3 V TTL) | MAX3232 (3.3 V) | DE-9, female (RS-232) | meets, after the null-modem cable |
|---|---|---|---|
| **G6** TX out | T1IN → T1OUT | **pin 2** | the Psion's receive (pin 3 of its cable) |
| **G5** RX in | R1OUT ← R1IN | **pin 3** | the Psion's transmit (pin 2) |
| **G7** RTS out (UART: "I can take more") | T2IN → T2OUT | **pin 8** (CTS, output of a DCE) | the Psion's CTS input (pin 7) |
| **G8** CTS in (UART: "may I send") | R2OUT ← R2IN | **pin 7** (RTS, input of a DCE) | the Psion's RTS output (pin 8) |
| **3V3** | VCC (3.3 V, never 5 V) | | |
| **GND** | GND | **pin 5** | signal ground (pin 5) |
| | | pins 1, 4, 6, 9 unconnected | |

The MAX3232 inverts both ways, so the ESP32 UART's active-low RTS/CTS come
out as RS-232's "asserted = positive" with nothing to configure. Then:
`AT$FC=1`, `AT&W`; in the apps, *Flow control: RTS/CTS*; the Test button
must not report *CTS blocked* (it would if pin 8 never went positive).
`AT$PACE?` shows the AUTO rate at 80 % of the line rate; `AT$PR=0` turns
pacing off entirely once the handshake is proven.

Safety, again, because this build puts a second RS-232 pair on the DE-9:
**pins 7 and 8 are true ±12 V like pins 2 and 3; they go to the MAX3232's
channel-2 RS-232 side and nowhere else. Never to G7/G8 or any ESP32 pin
directly.** The breakout must be a 3.3 V part with 3.3 V on VCC, with a
common ground with the Atom (and with the Base, if Path A is used).

### Test (bench; Not tested here)

VALIDATION.md T3: at 115200 with `AT$PR=0`, a 1 MB download with no
errors proves the handshake; pulling the pin-7 wire mid-transfer must stop
the modem's output within a byte or two, and restore it when reconnected;
then 230400 (PsiKernTest sets the Psion's divider) with `AT$PR=0`.

## 8. Sources

1. "DIY Psion Series 5mx serial cable at a relatively low cost",
   cloudcube.info, 27 March 2025: the Psion cable's DE-9 is a female socket,
   DCE-wired (pin 2 the Psion's transmit, pin 3 its receive, pin 5 ground;
   DTR 4, DSR 6, RTS 7, CTS 8, RI/DCD 9).
2. WiRSa (github.com/nullvalue0/WiRSa): female DE-9 in the modem/DCE
   convention, TX pin 2, RX pin 3, GND pin 5; the full RS-232 set on its
   board.
3. This repository, `firmware/atom-modem/README.md`, the 1.x wiring table:
   Base G → pin 5, T → pin 2, R → pin 3.
4. M5Stack, Atomic RS232 Base and AtomS3 documentation: the Base's
   transceiver on G5/G6 of the AtomS3 family; the Base's T/R/G terminals
   and separate power terminal.
