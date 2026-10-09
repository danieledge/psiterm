# Wiring the Atom modem to a Psion Series 5mx

The Atom modem is an **M5Stack AtomS3R** on an **Atomic RS232 Base**, acting as a
Hayes-compatible WiFi modem for the Psion. The Psion talks to it over its serial
port; the Atom does the WiFi, TLS, NAT and image slimming.

Only three wires carry data: **TX, RX and GND**. (RTS/CTS are optional — see
*Flow control* below.)

## The Atomic RS232 Base (screw-terminal version)

The base is an RS232 transceiver. Its screw terminal is labelled:

| Terminal | Meaning |
|---|---|
| **R** | the base's **RX** — connect the **Psion's TX** here |
| **T** | the base's **TX** — connect the **Psion's RX** here |
| **DC 12V** | a **power input** — leave **empty** (the Atom is powered over USB) |
| **G** | **ground** |

> The base is printed "RX22 / TX19" — ignore that, it is for the original Atom.
> The AtomS3R firmware uses G5/G6 internally, which is correct for this base.

## Wiring a cut Psion cable

A genuine Psion serial cable has an RS232 transceiver in the 15-pin hood (powered
by Vin on pin 1), so the DB9 end carries **RS232 levels**. You can cut the DB9 off
and land three wires straight into the base's screw terminal — the base converts
RS232 back to the TTL the Atom wants.

**The wire colours on the cut DB9 end do _not_ match the 15-pin pinout** — they are
the cable's own internal colours, which vary. Do **not** trust a 15-pin colour map.
Find the real TX/RX pair with the loopback test below.

For the cable this was first proven on, the mapping was:

| Base terminal | Wire | Signal |
|---|---|---|
| **R** | yellow | Psion TX |
| **T** | green | Psion RX |
| **G** | black | GND |
| **12V** | — | empty |

Unused on that cable: blue, orange, red.

### Finding TX/RX on an unknown cable — the loopback test

No multimeter needed:

1. Twist two candidate wires together (nothing else connected).
2. On the Psion, open the **Comms** app: **Serial port**, **115200 / 8 data / No
   parity / 1 stop**, **Handshaking = None**, and **Local echo OFF** (Tools →
   Terminal).
3. Type a few letters.
   - **Letters come back** → those two wires are the Psion's **TX and RX**.
   - **Nothing** → try another pair. Skip black and the foil shield (those are
     ground).
4. The winning pair goes to **R** and **T** (either way round — if `AT` gives
   silence or junk, swap them). Black/shield goes to **G**.

## Testing

AT commands live on the **RS232 line**, never on the USB console (the USB console
is a **log only** — it has no AT parser).

1. Wire R/T/G as above, **12V empty**, ground screwed in **firmly** (a loose
   ground gives silence, or junk only while the wire is moving).
2. Psion **Comms**: Serial, **115200 / 8-N-1**, **Handshaking None**.
3. Type `AT` → expect **`OK`**. Junk → step the baud down (57600, 38400…).
   Silence → swap R/T, or the TX/RX pair is wrong (loopback test).

## Using it (PPP + NAT)

Once `AT` answers, set the Psion's **"Psion Internet"** to:

- Static IP **192.168.7.2**, gateway/DNS **192.168.7.1**, "get IP from server" OFF.
- Flow control OFF (unless RTS/CTS is wired — below), auth none.
- First-send string **`ATD777`** (brings up PPP-over-serial; the Atom NATs the
  Psion out over WiFi).
- Shared proxy (Connection settings, all three apps): **192.168.7.1:8080**.

## Flow control (RTS/CTS) — optional, for throughput

The three-wire setup runs with flow control **off**. Big transfers (web pages,
images) can overrun the un-paced link; the firmware paces output to compensate,
and image transcoding keeps payloads small.

For full speed, wire the two handshake lines as well — Psion RTS → Atom **G8**
(CTS in), Atom **G7** (RTS out) → Psion CTS — then `AT$FC=1`, `AT&W`, and turn
RTS/CTS on in the Psion's Connection settings. On the base's screw terminal there
are no handshake pins, so this needs tapping the transceiver or a different base.

## Power

The Atom needs **5V on its USB-C** (~150 mA average, ~300 mA on WiFi peaks). Use a
small **USB-C power bank** for a cable-free, portable setup. Do **not** try to power
it from the Psion — the Psion is battery-powered and cannot source the current, and
its "Vin" pin is a power *input*, not a supply.

## Updating the firmware (OTA)

After the first USB flash, firmware updates go over WiFi: the web UI's **System**
page has an **Upload & flash** button, or

```
curl -u atom:PASSWORD -F firmware=@firmware.bin http://<atom-ip>/ota
```

A web password must be set first (`AT$WEBPASS` over the serial line, then `AT&W`).
If a bad build will not boot, recover over USB (`esptool … 0x10000 firmware.bin`).
