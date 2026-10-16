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

## Faster email: let the modem do the TLS (recommended)

The Psion's 36 MHz ARM is slow at TLS — the handshake alone is seconds of dead
waiting on every mail check. The Atom can do it instead, so the ARM does **no
crypto at all**:

1. On the Atom (over the serial line, once):

   ```
   AT$TLS=1      the Atom terminates TLS on its TLS ports
   AT&W          save
   ```

   The default TLS ports are already `443, 465, 993, 995` (`AT$TLSP?`), i.e.
   IMAPS/SMTPS/POP3S, and the certificate is checked (`AT$TLSV=1`).

2. In **PsiMail → Tools → Connection**, Link page: **Modem provides TLS = Yes**.
   Leave the mail account itself on **TLS, port 993** (SMTP 465) as normal.

PsiMail then dials `993`/`465` in plain down the cable and speaks plaintext; the
Atom opens the real TLS connection, checks the certificate, and tunnels. The
handshake and the per-byte decrypt both move off the Psion.

This applies to the **modem route only**. On the Psion Internet (PPP) route the
setting is ignored and PsiMail does its own TLS, so the one account is safe both
ways. (The certificate is now checked by the Atom, not PsiMail's own "trust this
certificate?" prompt.)

## Using it (PPP + NAT)

Once `AT` answers, set the Psion's **"Psion Internet"** to:

- Static IP **192.168.7.2**, gateway **192.168.7.1**, "get IP from server" OFF.
- DNS **192.168.7.1** with firmware 2.2 or later (the Atom relays DNS there: to
  the home DNS while the WireGuard tunnel is up, else the network's own, else
  1.1.1.1). With older firmware nothing answers DNS on the Atom: use
  **1.1.1.1**.
- Flow control OFF (unless RTS/CTS is wired — below), auth none.
- First-send string **`ATD777`** (brings up PPP-over-serial; the Atom NATs the
  Psion out over WiFi).
- Shared proxy (Connection settings, all three apps): **192.168.7.1:8080**.

## The home network from a hotspot (WireGuard, 2.2)

On a phone's hotspot the home LAN (SSH to a machine at home, a PsiProxy at
home) is not reachable. Firmware 2.2 has a WireGuard client: create
a client on the UniFi gateway's WireGuard VPN server, paste its config on the
Atom's web page **VPN**, and set the mode to **Auto** (up except at home).
Only the config's AllowedIPs (e.g. `192.168.10.0/24`) go through it. Nothing
changes on the Psion beyond DNS 192.168.7.1 above. See the README's
*WireGuard* section. The keys stay on the Atom (NVS) and are never printed;
never commit them, and keep the client's `.conf` file out of the repository
folder.

## TLS offload over PPP (mail: IMAPS/SMTPS/POP3S)

The [modem-route TLS offload](#faster-email-let-the-modem-do-the-tls-recommended)
above only works on the raw-modem route. The **PPP route** has its own offload: a
small stunnel-style relay on the Atom. The Psion connects **in plaintext** to the
Atom (192.168.7.1) on the standard TLS mail ports, and the Atom makes the real TLS
connection to your mail host, checks the certificate, and tunnels the bytes. The
36 MHz ARM again does **no crypto**.

Why bother, when the PPP route *could* carry the Psion's own TLS? Two reasons:

- **PPP's HDLC framing rides a marginal serial link better than the raw modem
  route** — a dropped or corrupted byte is a dropped frame, retried, not a
  desynced TLS stream — so you get PPP's robustness *and* no crypto on the Psion.
- The Atom checks the certificate (`AT$TLSV`), not PsiMail's "trust this
  certificate?" prompt.

It is **not transparent**: you must point PsiMail at the Atom, not at the mail
host (the Atom is not intercepting — it is a named relay).

1. On the Atom, set the upstream host(s) and save (once):

   ```
   AT$MAILIMAP=imap.example.com     IMAPS: 192.168.7.1:993 -> imap.example.com:993
   AT$MAILSMTP=smtp.example.com     SMTPS: 192.168.7.1:465 -> smtp.example.com:465
   AT$MAILPOP=pop.example.com       POP3S: 192.168.7.1:995 -> pop.example.com:995   (optional)
   AT&W
   ```

   Only the ports you set a host for are opened. Clear one with `AT$MAILIMAP=`
   (empty). `AT$MAILIMAP?` queries it; `ATI` shows the relay when any host is set.
   The certificate is checked unless `AT$TLSV=0` (dangerous; your own server only).

2. In **PsiMail**, with the account on the **Psion Internet (PPP)** route:
   - **IMAP** host `192.168.7.1`, port **993**, security **None**.
   - **SMTP** host `192.168.7.1`, port **465**, security **None**.
   - (POP host `192.168.7.1`, port **995**, security **None**.)

   Leave "Modem provides TLS" **off** — that setting is for the modem route; here
   you are pointing the account straight at the relay.

PsiMail may hold IMAP and open SMTP at the same time; the relay supports
**two** concurrent sessions (e.g. IMAP + SMTP), each its own TLS connection.
Two, not three, because each live TLS session needs ~45-50 KB of internal RAM
that cannot come from PSRAM; a third alongside the web proxy's TLS and the
picture decoder would exhaust the internal heap and drop the Wi-Fi link. The
staging buffers come from PSRAM on the AtomS3R and are taken when the PPP call
comes up and freed when it goes.

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
