# PsiCast protocol (version 1)

PsiCast shows the Psion's screen live in a web browser (first target: an
iPhone in Safari), through the Atom modem, and optionally lets the viewer
tap and type on the Psion.

```
Psion (psicast.exe)  --link A (TCP 5961)-->  Atom  --link B (WebSocket 5962)-->  browser
   capture, diff,        <-- acks, input --   decode into its own   <-- taps, keys --
   scroll detect,                             640x240 copy; per-viewer
   encode, deflate                            dirty tracking
```

Link A is a TCP connection from the Psion to the Atom's own address,
`192.168.7.1:5961`, made through psiglue:

- **PPP route:** an ordinary socket to 192.168.7.1:5961 over the Psion's
  PPP link to the Atom. Other apps keep using the same PPP link.
- **Modem route:** psiglue dials `ATDT192.168.7.1:5961`; the Atom sees its
  own address and port 5961 and connects the call to the local PsiCast
  service instead of the outside world. The line is PsiCast's alone.

The Psion side therefore has one code path: "connect to 192.168.7.1:5961".

All integers are little-endian. "u8/u16/u32" are unsigned.

## Screen model

- 640 x 240 pixels, 16 greys, **0 = black, 15 = white** (EPOC `EGray16`).
- Packed 4 bits per pixel, two pixels per byte, **left pixel in the low
  nibble** (EPOC's EGray16 bitmap layout). A row is 320 bytes; the screen
  is 76,800 bytes.
- The Psion converts from the hardware frame buffer's layout if it differs
  (polarity, nibble order) before encoding. Everything on the wire, in the
  Atom and in the browser uses the model above.
- HELLO carries width and height; the decoder must accept any size up to
  640 x 480 and reject larger ones.

## Link A framing (both directions)

Every message:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `'P','C'` (0x50 0x43) |
| 2 | 1 | type |
| 3 | 1 | flags (per type; 0 if unused) |
| 4 | 4 | payload length N (max 65536) |
| 8 | N | payload |
| 8+N | 4 | CRC-32 (zlib `crc32`) of bytes 0 .. 8+N-1 |

A receiver that sees a bad magic, an over-long length or a bad CRC drops
bytes until the next `'P','C'` that yields a valid message (resync), counts
the error, and (Atom side) sends KEYREQ so the picture recovers.

### Psion -> Atom

| Type | Name | Payload |
|---|---|---|
| 0x01 | HELLO | u16 protocol version (1), u16 width, u16 height, u8 format (0 = model above), u8 reserved, u32 capabilities (bit0 input supported, bit1 control allowed now, bit2 can stamp its UPDATEs: see *Latency*), then a UTF-8 device name (rest of payload, may be empty) |
| 0x02 | UPDATE | u32 seq, then (only if flags bit1) u32 capture time, then the compressed update (below). flags bit0 = keyframe (the zlib stream was reset before this update and it repaints the whole screen); bit1 = stamped, set only when WELCOME asked for it |
| 0x03 | STATUS | u8 battery % (255 unknown), u8 on mains (0/1/255), u32 free RAM bytes, u16 updates/s x10, u16 Psion CPU % x10 (the engine's share, roughly; 65535 unknown), u8 control allowed (0/1), u8 paused (0/1); then, optionally (PsiCast 0.11 on), u32 the Psion's clock in ms (the clock of UPDATE's capture time) and u32 the token of the PING this STATUS answers (0: it answers none). A receiver takes 12 bytes or more and ignores what it does not know |
| 0x04 | BYE | none |
| 0x05 | DEVINFO | u16 digitiser width, u16 digitiser height (695 x 280 on the 5mx). Sent right after HELLO; optional. The digitiser is bigger than the LCD: taps at x < 0 and y >= 240 are the silkscreen icons |

### Atom -> Psion

| Type | Name | Payload |
|---|---|---|
| 0x81 | WELCOME | u16 protocol version, u16 max updates in flight (window, normally 2), u32 capabilities (bit0 viewer can send input, bit1 stamp each UPDATE with its capture time: only if HELLO's bit2 said the Psion can), u32 max UPDATE payload in bytes (0 = 65536; a board short of memory says less, and the Psion keeps its updates within it), then an optional UTF-8 viewer address, e.g. `http://192.168.1.50/screen` (rest of payload) |
| 0x82 | ACK | u32 seq of the last UPDATE **taken**, applied or discarded. Cumulative: it acknowledges every UPDATE up to that seq |
| 0x83 | KEYREQ | none: send a keyframe next |
| 0x84 | VIEWERS | u8 number of viewers connected, u8 number with control |
| 0x85 | INPUT | a list of input events (below) |
| 0x86 | PING | u32 token, u32 last_seq (the last UPDATE taken, as for ACK). The Psion answers with STATUS; liveness both ways |

### Flow control ("always the newest picture")

- The Psion keeps at most `window` UPDATEs unacknowledged (WELCOME, normally
  2). While it waits it keeps capturing; changes are **merged** into the
  next update (the dirty set grows, the frame is the latest), never queued
  as separate stale frames.
- Every UPDATE the Atom takes is acknowledged, whether it was applied or
  discarded (one that does not decode, or a non-keyframe while the Atom waits
  for a keyframe), so the window always moves. ACK is cumulative. The Atom
  also sends PING, which carries the last seq taken, whenever the Psion's
  window may be stalled: when idle, and after a discarded update.
- The first UPDATE after HELLO, and the next one after KEYREQ, is a keyframe.
  The Atom sends KEYREQ after a framing error, a gap in the seqs or an
  update that does not decode, and again every 3 s until a keyframe comes.
- With no viewers (VIEWERS = 0) the Psion may stop sending updates and
  capture slowly; on the first viewer the Atom sends KEYREQ. (The Atom keeps
  its copy, so a keyframe is only needed if updates were skipped.)

The window of 2 is enough on a link of about 10 KB/s: its round trip is tens
of milliseconds, so one update in flight already keeps it busy while the next
is captured and encoded. A larger window would only queue older pictures in
front of the newest (more latency, no more pictures).

### Latency

A way to see how long a picture takes, without new hardware. Both halves are
optional and negotiated, so an older Psion or Atom works with a newer one as
before:

- A Psion that can stamp sets HELLO's capability bit2. The Atom asks for
  stamps with WELCOME's bit1, and only then does the Psion set UPDATE's
  flags bit1 and put the capture time (u32, its own clock in ms, wrapping)
  after seq. The payload is then 4 bytes longer, within WELCOME's maximum.
- The Psion's STATUS carries its clock and the token of the PING it answers
  (after the 12 bytes above). The Atom's PING token is its own clock (never
  0). For a STATUS that answers its last PING, the Atom knows the round trip
  and that the Psion read its clock while the PING was out: the offset
  between the clocks is the Psion's clock less the middle of the round trip,
  good to half of it. The Atom keeps the offset from the answer with the
  shortest round trip, takes a fresh one after 30 s, and while the Psion
  stamps it sends PING at least every 5 s even when the link is busy.
- For each stamped UPDATE it applies, the Atom works out capture -> applied
  (encoding, the Psion's send queue, the link, the Atom's receive buffer and
  decoding). A figure below zero or over 60 s means the clocks have moved
  (the Psion was off: its clock stops): the offset is dropped and measured
  again.
- The Psion stamps the time it read the screen, so the figure leaves out the
  time between the screen changing and the read (up to the capture interval:
  0.1 s as standard, or one look while the screen is still).
- The figures go to each viewer in the status JSON (link B, `lat`).

### Liveness

- Either side closes link A after 20 s with no message received. The Atom
  sends PING every 5 s when idle; the Psion sends STATUS at least every 5 s.

### The modem route

- The call is an ordinary data call: the Psion hangs up with `+++` (a guard
  time of quiet before and after, 0.8 s as standard) and `ATH`. `+++` inside
  a message (after a `'P','C'` and before the message is whole) is always
  data, whatever the pauses round it. The Atom never looks at the Psion's
  data for anything else.
- Towards the Psion the Atom sends only link A messages until the call ends,
  then `NO CARRIER`. A message's fixed header and CRC leave no room for the
  ten bytes `NO CARRIER` by chance, except in WELCOME's viewer address, which
  is an address. A call ends only by `+++` and `ATH`, BYE, 20 s of silence, or
  PsiCast being turned off.

## UPDATE body (the codec)

The UPDATE payload after `seq` is one chunk of a **single raw-deflate
stream for the whole session** (zlib `deflateInit2`, windowBits -12; the
Psion uses level 1 and memLevel 5, which the Atom does not need to know;
each update ends with `Z_SYNC_FLUSH`). A keyframe resets the stream on both
sides (`deflateReset` / `inflateReset`) and the screen to all white (15)
before it; its ops then paint the rest. A keyframe too big for one UPDATE
is finished by the following ordinary updates.

Decompressed, the update is a sequence of operations applied **in order**
to the current screen, defined byte by byte in `CODEC.md`:

- END;
- MOVE: copy rows `[y0, y1)` of a range of columns (8-pixel steps from the
  Psion, any even range accepted) by `dy` rows (scrolling);
- FILL: a rectangle of one grey;
- PIX1, PIX2, PIX4: a rectangle of pixels at 1 bit (2-grey palette), 2 bits
  (4-grey palette) or 4 bits a pixel, each optionally run-length coded.

Every op is bounds-checked by the decoder; an op outside the screen, a
truncated op, garbage, or anything after END makes the whole update fail
(the Atom then sends KEYREQ, and fails every update until the keyframe).
The decoder never reads or writes outside its buffers.

## Input events (INPUT payload)

A sequence of 8-byte events:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | kind |
| 1 | 1 | reserved |
| 2 | 2 | a (x, or scan code) |
| 4 | 2 | b (y, or 0) |
| 6 | 2 | reserved |

| Kind | Meaning |
|---|---|
| 1 | pen down at (x, y) |
| 2 | pen move to (x, y) |
| 3 | pen up at (x, y) |
| 4 | key down, `a` = EPOC standard scan code (`TStdScanCode`, e32keys.h) |
| 5 | key up, same |
| 6 | type a character, `a` = its code (Latin-1/CP1252, what ER5 uses) |

- Coordinates are screen pixels; x may be negative or beyond 640 for the
  5mx's silkscreen areas (the digitiser is larger than the LCD).
- Modifiers are ordinary key down/up events of their scan codes
  (`EStdKeyLeftShift`, `EStdKeyLeftCtrl`, `EStdKeyLeftFunc`, ...). The
  viewer sends a typed capital as Shift down, key down, key up, Shift up.
- Kind 6 exists because ER5 can only inject raw scan codes, and which keys
  make "@" or "£" depends on the Psion's keyboard layout. The Psion maps the
  character to scan code + modifiers with its own layout table (the 5mx's
  UK layout first) and injects the down/up sequence itself. Text typed on
  the phone is sent as kind 6; special keys (arrows, Menu, Esc, Tab,
  Ctrl+letter, Fn) as kinds 4/5.
- The Psion injects them with `UserSvr::AddEvent(TRawEvent)` only while
  "Allow control from the viewer" is on; otherwise it drops them.

## Link B (Atom <-> browser)

WebSocket on port **5962**, path `/cast`, from the viewer page served by the
Atom's web UI at `http://<atom>/screen`.

- **Auth:** the page is behind the web UI's password. The page fetches
  `GET /screen/token` (password-protected), which returns a random
  single-use token valid for 60 s; the WebSocket URL carries
  `?t=<token>`. No token, no connection.
- **Atom -> browser (binary):** `u8 type`, then:
  - 0x01 FULL: u16 width, u16 height, then width*height/2 bytes (the model
    above);
  - 0x02 RECTS: u16 count, then per rect u16 x, y, w, h (x and w even),
    then w*h/2 bytes of pixels, row by row;
  - 0x03 SCROLL: u16 y0, u16 y1 (exclusive), i16 dy, u16 x, u16 w (rows
    y0..y1 of columns x..x+w moved by dy, x and w even; the browser copies
    them on its canvas, then RECTS fill the rest. A SCROLL of 7 bytes, with no
    x and w, is the whole width).
- **Atom -> browser (text, JSON):**
  - `{"t":"hello","name":..,"w":640,"h":240,"digitiser":[w,h],"linkUp":bool,"route":"modem|ppp","askedControl":bool}`
    (`digitiser` from DEVINFO, `[0,0]` if not sent);
  - `{"t":"status","linkUp":bool,"route":..,"battery":n|null,"mains":bool|null,"freeRam":n,"ups":n,"psionUps":n,"cpu":n|null,"control":bool,"paused":bool,"viewers":n,"rateBytes":n,"viewBytes":n,"rtt":ms|null,"you":bool,"errors":n,"updates":n,"lat":{"link":ms|null,"linkMax":ms|null,"acc":ms|null,"queue":ms|null,"view":ms|null}}`,
    at most once a second: `ups` is the updates the Atom applied a second,
    `psionUps` the Psion's own figure, `rateBytes` link A's bytes a second,
    `viewBytes` this viewer's, `rtt` PING to STATUS on link A, `control`
    the Psion allows control, `you` this viewer's input is passed on.
    `lat` (*Latency*): `link` and `linkMax` the mean and the most of capture
    on the Psion -> applied on the Atom over the last second with updates
    (null if the Psion does not stamp, or the clocks are not yet compared),
    `acc` how sure that is (+/- ms: half the round trip the offset came from,
    plus the Psion's 15.6 ms tick), `queue` this viewer's last wait on the
    Atom (its dirty area first marked -> the message carrying it all handed
    to its socket), `view` the round trip of the Atom's last WebSocket ping
    to this viewer (behind whatever was on its way to it). Picture latency is
    about `link` + `queue` + `view`/2, plus the wait before the Psion reads
    its screen;
  - `{"t":"bye"}` (the Psion stopped sharing); `{"t":"pong","n":n}`.
- **Browser -> Atom (text, JSON):** `{"t":"pen","e":"down|move|up","x":..,"y":..}`,
  `{"t":"key","e":"down|up","sc":<EPOC scan code>}`,
  `{"t":"char","c":<character code>}`, `{"t":"control","on":0|1}` (take or
  give up control on this connection; only a token issued with control can
  take it, and the page asks for such a token every time so that switching
  never reconnects), `{"t":"full"}` (send a
  FULL now), `{"t":"ping","n":n}` (answered at once with pong: the page's
  latency). A viewer that sends nothing for 45 s is closed; the page pings
  every 2 s.
- Each viewer has its own dirty state on the Atom; a slow viewer gets merged
  rects, never a backlog, and never slows link A.
- The Atom sends each viewer a WebSocket ping (4 bytes: its clock) every 2 s;
  the browser answers it by itself. The answer gives `lat.view`, and does not
  count as the viewer sending anything (the 45 s rule).
- Input is passed on only if the Psion said control is allowed (HELLO /
  STATUS) and the viewer's token was issued with control (the page asks).

## Changes in PsiCast 0.11 (still version 1)

Latency figures (*Latency*): HELLO capability bit2, WELCOME capability bit1,
UPDATE flags bit1 and its capture time, STATUS's clock and PING token after
its 12 bytes, `lat` in the status JSON, and the Atom's WebSocket pings. All
optional: a Psion that does not set HELLO's bit2 is never asked for stamps,
an Atom that does not set WELCOME's bit1 never gets them, and both ends
already took a STATUS longer than 12 bytes and ignored capability bits they
did not know.

## Changes from draft 1

Agreed while building both ends, still version 1 (nothing was released):
WELCOME's max UPDATE payload and viewer address; ACK means taken (applied or
discarded) and is cumulative; PING carries the last seq taken and is also
sent after a discarded update; DEVINFO (0x05); the modem route's rules for
`+++` and `NO CARRIER`; SCROLL's x and w on link B; the JSON fields above,
with ping and pong.
