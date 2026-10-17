# PsiCast screen codec

The codec turns the Psion's screen into UPDATE bodies (PROTOCOL.md, "UPDATE
body") and back. The source is `cast/codec/pccodec.c` and its API is
`cast/codec/pccodec.h`. The encoder runs on the Psion; the decoder runs on
the Atom and in the host tests.

## The stream

- One **raw deflate stream** a session: `deflateInit2(level, Z_DEFLATED,
  -12, 5, Z_DEFAULT_STRATEGY)` on the Psion, `inflateInit2(-12)` on the
  Atom. Each update ends with `Z_SYNC_FLUSH`, so it is complete in itself
  and ends with the bytes `00 00 FF FF`.
- The encoder uses level 1. memLevel and level only matter to the encoder;
  the decoder needs only windowBits 12 (a 4 KB window).
- A **keyframe** (UPDATE flags bit 0) resets both sides: `deflateReset` /
  `inflateReset`, and the screen becomes **all white (15)**. The keyframe's
  ops then paint whatever is not white. A keyframe that does not fit in one
  UPDATE is finished by the next, ordinary, updates (see "Space" below).
- Decompressed, an update is a list of **ops**, applied in order to the
  screen, ending with END. Nothing may follow END.

## Ops (byte level)

All numbers are little-endian. Coordinates are pixels. In every op, x and
w are even (a whole byte of the 4bpp screen), w and h are at least 1, and
the area lies inside the screen. Any other value, an unknown op byte, a
truncated op or data after END fails the update.

| Op | Bytes after the op byte | Meaning |
|---|---|---|
| `0x00` END | none | the update is complete |
| `0x01` MOVE | u16 x, u16 w, u16 y0, u16 y1, i16 dy | rows `[y0, y1)` of columns `[x, x+w)` are copied to rows `[y0+dy, y1+dy)`, as if through a temporary copy. Rows left behind keep their old pixels. `y0 < y1`, `dy != 0`, and the destination is inside the screen. |
| `0x02` FILL | u16 x, y, w, h, u8 grey | the rectangle becomes one grey (0..15) |
| `0x03` PIX4 | u16 x, y, w, h, pixels | h rows of w/2 bytes, as the screen model (left pixel in the low nibble) |
| `0x04` PIX2 | u16 x, y, w, h, u8 p01, u8 p23, pixels | a 4-grey palette: entry 0 is the low nibble of p01, entry 1 its high nibble, entries 2 and 3 likewise in p23. h rows of ceil(w/4) bytes, 2 bits a pixel: pixel i of a row is bits 2(i mod 4) and up of byte i/4 (first pixel in the low bits) |
| `0x05` PIX1 | u16 x, y, w, h, u8 p01, pixels | a 2-grey palette in p01 as above. h rows of ceil(w/8) bytes, 1 bit a pixel: pixel i is bit (i mod 8) of byte i/8 |
| `0x13`, `0x14`, `0x15` | as PIX4, PIX2, PIX1 | the same, with the pixel bytes (all rows, one after the other) **RLE-coded** |

Unused bits at the end of a PIX2 or PIX1 row are 0 from the encoder and
ignored by the decoder.

**RLE tokens** (bit `0x10` of the op): the pixel bytes are a sequence of
tokens, which must cover them exactly (a token may not run past the end):

| Token | Then | Gives |
|---|---|---|
| `0x00`..`0x7F` (c) | c+1 bytes | those bytes (a literal) |
| `0x80`..`0xFE` (c) | 1 byte v | v, c-0x80+3 times (3..129) |
| `0xFF` | u16 n (n >= 3), 1 byte v | v, n times |

The palette ops are where most of the saving is. EPOC's own UI draws in 4
greys, and text is 2, so most rectangles are PIX1 or PIX2: a quarter or
half of the bytes before deflate even starts. RLE then takes out the
runs, which also lets more useful history fit in deflate's 4 KB window.

## Decoder

`pc_dec_update` inflates the body in 1 KB pieces and parses the ops as they
come; it never holds a whole update. Every op is checked before it touches
the screen (above), RLE runs are checked against what is left of the op,
inflate errors and `Z_STREAM_END` (an encoder never finishes the stream)
fail the update, and so does any output after END. After a failure every
update fails with `PC_ERR_DATA` until a keyframe, because the deflate
stream is out of step.

Callbacks, as each op is applied:

- MOVE: `aOnScroll` with `y0, y1, dy` and the columns `x, w`, if the move is
  the full width (or the decoder was given `PC_DEC_PARTIAL_SCROLL`);
  otherwise `aOnRect` with the destination area.
- FILL and PIX*: `aOnRect` with the rectangle.
- A keyframe makes no callbacks for its ops; it reports one rectangle, the
  whole screen, once it has been applied.

A viewer that copies the moved area on its canvas and then repaints the
reported rectangles from the decoder's screen ends up with the decoder's
screen exactly (the round-trip test checks this).

## Encoder

The encoder keeps a **model**: the screen as the decoder has it. Each call
encodes "model -> new frame", and applies every op it writes to the model.
So anything it cannot send now simply stays different and goes next time,
and nothing ever drifts.

1. **Row compare.** Each row of the frame is compared with the model, four
   words at a time (an `LDM` loop in ARM assembly on the Psion, C
   elsewhere). A dirty row gets its first and last dirty word and a mask
   of dirty 16-pixel tiles. An unchanged frame costs only this pass, and
   `pc_enc_frame` then returns 1 with nothing written.
2. **Moves** (not in a keyframe; at most 2 an update; only with 6 or more
   dirty rows). The columns are the dirty rows' extent, in words.
   - Every row in the dirty band is hashed, old (model) and new (frame), on
     every fourth word of those columns.
   - Each dirty new row votes for `dy = new row - old row` for each old row
     with the same hash (rows with more than 4 copies, such as blank rows,
     do not vote). The offset with the most votes wins; it needs 3.
   - The best band of rows for that offset is found exactly (word compare,
     Kadane): a row scores +1 if it is dirty and matches after the move,
     and -1 if it is clean and would not. The band must save 3 rows.
   - Columns that do not follow the move are left out: if the band misses
     dirty rows, the codec tries without the edge words (a pane border or a
     scroll bar next to the list); and edge words that miss in more than a
     quarter of the band's rows (a scroll bar thumb) are left out too.
   - The MOVE is written and applied to the model. Rows that now match are
     clean; the others are compared again.
3. **Rectangles.** Tiles are 16 x 8 pixels. In each band of 8 rows the dirty
   tiles form runs (a gap of one clean tile is bridged). A run with the
   same columns as one in the band above extends its rectangle; otherwise
   that rectangle is closed. A closed rectangle is trimmed to its first and
   last dirty rows and to the dirty words at its row ends.
4. **Each rectangle** is scanned for its greys: 1 grey is a FILL, 2 a PIX1,
   3 or 4 a PIX2, more a PIX4. The pixels are packed (through a 256-entry
   table, kept while the greys stay the same) in strips of at most 4 KB,
   and each strip is RLE-coded if that is shorter.
5. **Space.** Before each op the encoder checks that the op list so far,
   plus this op, fits in the output buffer even if deflate cannot compress
   it at all (zlib's own bound). If it would not, it syncs deflate to learn
   the real size and checks again, then halves the strip until it fits. If
   nothing more fits, it ends the update there and `pc_enc_pending()`
   becomes true: the rest goes in the next update. With the 65,532 bytes an
   UPDATE can carry, this only happens for a screen of noise.

The encoder needs a keyframe first, and again only if deflate itself ever
fails (it then returns `PC_ERR_SPACE`, and `PC_ERR_ARG` until it gets
one).

## Memory

| | Psion (encoder) | Atom (decoder) |
|---|---|---|
| Screen copy | 76,800 (the model) | 76,800 (153,600 at 640 x 480) |
| zlib | deflate: window 8 KB, prev 8 KB, hash 8 KB, pending 8 KB, state 5.8 KB = 37.8 KB | inflate: state 7.1 KB, window 4 KB = 11.2 KB |
| Codec | about 12 KB (staging 2 KB, packing 4 + 4 KB, tables) + 7.4 KB of row tables | about 1.1 KB |
| **Total** | **137,792 bytes** (measured on the ARM build) | **89,280 bytes** (measured on the host) |
| Stack | 1.4 KB at most, deflate included (measured on the ARM build) | under 1 KB plus inflate's |

The Psion's frame buffer copy (76,800 bytes) belongs to the caller.

## Results

### Bytes

`make -C cast/test bench` (`bench.py`) builds the sequences and runs them.
The first frame is a keyframe; every later frame is one update. "Ratio" is
the changed frames x 76,800 over the bytes sent. Moves are the MOVE ops in
the whole sequence.

Synthetic sequences, made from `docs/screenshots` with text drawn without
anti-aliasing (like Psion fonts):

| Sequence | What | Updates | Keyframe | Median | Max | Mean | Ratio | Moves |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| type | a paragraph typed in a page, a key a frame | 160 | 3,578 | 26 | 56 | 25 | 2972:1 | 0 |
| term | a terminal scrolling a line a frame | 59 | 6,010 | 403 | 509 | 324 | 236:1 | 59 |
| doc | a reader pane scrolled 3 lines a frame, thumb moving | 29 | 7,474 | 1,404 | 2,781 | 1,610 | 48:1 | 29 |
| list | a list pane beside a fixed pane, a line a frame | 49 | 10,148 | 649 | 1,208 | 654 | 117:1 | 79 |
| menu | a menu, the highlight down 4 items, a dialog, close (x3) | 24 | 3,536 | 658 | 1,875 | 805 | 95:1 | 0 |
| switch | another app every frame | 19 | 7,537 | 4,919 | 7,758 | 4,985 | 15:1 | 0 |

Real sequences: the emulator's screenshots of PsiMail, PsiTerm and PsiWeb
runs (`build/emu/*/s-*.png`, one a second of emulated time, repeats left
out), from boot (so the keyframe is the blank screen, and the largest
update is the first app screen):

| Run | Updates | Median | Max | Mean | Ratio |
|---|---:|---:|---:|---:|---:|
| pmreader (PsiMail reader) | 21 | 1,413 | 9,356 | 2,205 | 35:1 |
| pmail | 29 | 2,442 | 9,356 | 2,523 | 30:1 |
| pterm (PsiTerm) | 25 | 1,475 | 9,356 | 2,363 | 32:1 |
| ptmenu | 15 | 2,757 | 9,356 | 3,123 | 25:1 |
| ptcal | 21 | 1,516 | 9,356 | 2,562 | 30:1 |
| pweb (PsiWeb) | 22 | 3,095 | 9,356 | 3,098 | 25:1 |
| pwview | 12 | 3,849 | 9,356 | 3,453 | 22:1 |

At about 10 KB/s (115,200 baud) that is: a key in under 3 ms of the link,
a terminal line in 30 ms, a list line in 65 ms, a menu in 80 ms, a 3-line
document scroll in 160 ms and a new screen in 0.5 to 0.9 s.

The first working version (dirty tiles as 4bpp rectangles with RLE, and
moves) sent 6,254 / 28,256 / 124,021 / 25,028 / 113,998 bytes for type,
term, doc, menu and switch; the palette ops took that to 4,135 / 19,175 /
48,158 / 19,334 / 94,719, and leaving the columns that do not follow a move
out of it took doc to 46,703 and list from 62,500 to 32,087.

### Tuning (total bytes after the keyframe)

| Change from the defaults | type | term | doc | list | menu | switch | pmreader | pweb |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **defaults** (level 1, 2 moves, RLE, gap 1, tiles 16 x 8) | 4,135 | 19,174 | 46,703 | 32,087 | 19,334 | 94,719 | 46,318 | 68,170 |
| deflate level 3 | 3,903 | 18,966 | 46,131 | 31,427 | 19,000 | 92,268 | 45,599 | 66,615 |
| deflate level 6 | 3,734 | 18,355 | 42,566 | 30,227 | 18,405 | 88,133 | 44,406 | 64,698 |
| no moves | 4,135 | 195,934 | 75,869 | 176,412 | 19,334 | 94,719 | 48,875 | 68,145 |
| one move an update | 4,135 | 19,174 | 46,667 | 32,020 | 19,334 | 94,719 | 46,314 | 68,153 |
| no RLE | 4,102 | 19,821 | 60,978 | 32,839 | 22,238 | 98,637 | 46,306 | 69,395 |
| no gap bridged | 4,135 | 19,145 | 46,915 | 32,086 | 19,361 | 95,809 | 46,498 | 68,493 |
| gap of 4 tiles | 4,135 | 19,149 | 49,098 | 32,087 | 19,301 | 95,285 | 46,205 | 68,970 |
| tiles 16 x 4 | 4,180 | 21,600 | 49,012 | 34,310 | 19,776 | 99,651 | 47,066 | 69,823 |
| tiles 16 x 16 | 4,120 | 18,980 | 45,871 | 35,808 | 18,584 | 93,492 | 46,046 | 68,959 |
| `Z_RLE` / `Z_HUFFMAN_ONLY` | 6,779 | 34,511 | 80,104 | 42,383 | 27,324 | 139,584 | 61,027 | 92,614 |
| memLevel 4 | 4,137 | 19,259 | 48,169 | 32,520 | 19,683 | 96,241 | 47,093 | 69,339 |
| memLevel 6 | 4,130 | 19,183 | 46,876 | 31,904 | 19,275 | 93,406 | 45,894 | 67,286 |

- Moves are worth 10 times on a terminal and 5 on a list.
- Level 6 saves 5 to 9 %, but costs up to 30 % more time on the Psion
  (below), and for a new screen the time saved on the link (about 35 ms) is
  less than the time lost encoding (about 70 ms). Level 1 stays.
- The other knobs are within a few per cent either way; the defaults are
  the best overall.

### Time on the Psion

`cast/test/arm/arm.mk` builds the encoder and deflate with the Psion's
compiler and flags (gcc 3.0, `-O2 -mcpu=arm710`), and `armbench.py` runs
that exact ARM code under unicorn on every sequence, counts the
instructions of each update, and checks that the updates it made decode to
the frames. Time assumes 15 MIPS, the same figure as the PsiWeb ARM
harness for the 36 MHz ARM710T with its cache and memory. It is an
estimate: the real figure depends on cache misses (the model and the frame
are 150 KB between them; the cache is 8 KB).

| Sequence | Median instructions | Median | Max | Keyframe |
|---|---:|---:|---:|---:|
| type | 133,000 | 8.9 ms | 15.6 ms | 129 ms |
| term | 724,000 | 48 ms | 60 ms | 222 ms |
| list | 930,000 | 62 ms | 89 ms | 365 ms |
| menu | 899,000 | 60 ms | 100 ms | 133 ms |
| doc | 1,358,000 | 91 ms | 128 ms | 256 ms |
| switch | 3,296,000 | 220 ms | 312 ms | 288 ms |
| emulator runs | 0.87 to 2.2 M | 58 to 147 ms | 287 to 292 ms | |
| an unchanged frame (`pc_enc_changed`) | 63,900 | 4.3 ms | | |

- Typing is well under 20 ms. Most of it is the compare of the whole
  screen.
- A new screen takes 220 to 310 ms, about twice the 150 ms aimed for. Over
  half of that is deflate itself (its hashing, matching and Huffman trees
  for about 10 KB of packed pixels), the rest packing, RLE and copying.
  The link needs 0.5 to 0.9 s to carry that update anyway, and with two
  updates in flight the next capture overlaps it.
- Scrolls cost 50 to 90 ms: the hashes, the exact compare of the band, the
  move on the model, and then the new lines.

On host x86 (i7-7700) the whole encode is 12 us for a key and 0.4 to 0.6 ms
for a new screen.

## Tests

On the host (gcc; the tests are built with ASan and UBSan):

```
make -C cast/test test        # round trip, decoder fuzz, both halves built alone
make -C cast/test bench       # the tables above (python3 with Pillow)
python3 cast/test/bench.py build/cast-test --sweep    # and the tuning table
```

The ARM measurement (needs the psibuild image and unicorn, pefile):

```
tools/docker/psibuild "make -f cast/test/arm/arm.mk"
python3 cast/test/arm/armbench.py [--profile] [--stack] [--level N]
```

- `rt` (round trip): 9 runs, from 8 x 1 to 640 x 480, strides wider than
  the row, deflate levels 1 to 9, output buffers from 1,024 bytes (so most
  updates are split) to 65,532. Frames get noise, fills, text-like specks,
  single pixels, dithered areas, and scrolls of the whole width or a column
  range by many offsets, with other changes at the same time; keyframes
  come mid-stream. Each decoded screen must equal the frame, and so must a
  viewer's copy kept only through the callbacks.
- `fuzz`: real updates truncated at every length and with bytes flipped,
  replaced, inserted and removed; 12,000 hostile op lists, deflated
  correctly, with fields at and just beyond the edges and RLE tokens that
  nearly cover their op; and random bytes. The results must be `PC_OK` or
  `PC_ERR_DATA`, an update after an error must fail until a keyframe, and a
  good keyframe must then decode exactly.
- `memuse`: the heap each side takes.
