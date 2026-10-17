#!/usr/bin/env python3
"""s7icons.py: build PsiCast's Series 7 icon pack from a Series 5mx ROM and
a Series 7 ROM.

The PsiCast viewer's "Series 7" theme replaces the 5mx's grey pictures on
screen (application icons, toolbar and dialog pictures) with the Series
7's colour versions of the same pictures (src/cast/iconmatch.js). This
tool reads both sets out of the two ROM images and pairs them.

    python3 tools/s7icons.py 5MX.bin SERIES7.bin src/cast/s7icons.json [--sheet contact.png]

The ROMs are read where they are (pass their paths; never copy them into
the repository). Every English 5mx ROM 1.05 tried (250, 254, 255, 260)
gives the same pack; the committed one is from 5mx 1.05(260) and Series 7
1.05(254) build 756. --sheet draws each pair, 5mx grey beside Series 7
colour, to check the pairing by eye; --everything keeps what LEAVE_OUT
drops; --keep FILE keeps only the pictures named in FILE; --list prints
the names. Then run tools/mkcasticons.py for the firmware's copy.

How the ROMs are read
- An EPOC R5 ROM carries a read-only directory tree. A TRomDir is a
  length then TRomEntry records { TInt iSize; TLinAddr iAddressLin;
  TUint8 iAtt; TUint8 iNameLength; TText8 iName[] }, padded to a word,
  with 0x10 in iAtt for a directory. ER5 (non-Unicode) names are 8-bit.
- The 5mx image starts with its TRomHeader: iRomBase (0x50000000) at
  +0x8c and iRomRootDirectoryList at +0x94 (a count, then {variant,
  address} pairs).
- The Series 7 image has no TRomHeader at offset 0 (those bytes are
  0xff), but it is linked at the same base: file offset = address -
  0x50000000. Its root TRomDir (at 0x82a73c in build 756) is found by
  scanning for the "System" directory entry (att 0x10, length 6), whose
  TRomDir length word sits just before it.

How pictures are stored
- An .aif is a direct file store: the root stream holds the captions and
  an array of {stream id, size} for the icons; each icon is a
  CApaMaskedBitmap, a CFbsBitmap stream for the picture followed straight
  away by one for its mask (see tools/icon/aif.py, which writes them).
- An .mbm is a multi-bitmap file store (UID 0x10000042: a trailer at the
  offset in the header lists each bitmap's stream), or in ROM a ROM image
  (UID 0x10000041: a count and offsets of CBitwiseBitmaps, each with its
  SEpocBitmapHeader at +0x14 and the offset of its pixels at +0x40).
- A bitmap is SEpocBitmapHeader (bitmap.h: size, 40, pixels w h, twips
  w h, bits per pixel, colour flag, palette entries, compression) then
  rows padded to 32 bits, first pixel in the low bits. Compression 1 is
  byte RLE (0..127: next byte n+1 times; 128..255: 256-n literal bytes),
  2 is 12-bit RLE, 3 is 16-bit RLE.
- 5mx pictures are 2 bpp grey (4 greys). Series 7 pictures are 8 bpp
  colour (byte RLE), indexed into the standard EPOC 256-colour palette
  (TRgb, 0x00bbggrr), which this tool reads out of the Series 7 ROM's GDI.
  Their masks stay 2 bpp black and white (a few are 8 bpp).

How the 5mx draws them (so the pack can be matched against its screen)
- Pictures are drawn with BitBltMasked, a binary mask: only the pixels the
  mask lets through reach the screen, the rest keep the background. Black
  in the mask is drawn (apparc and EIKON pass aInvertMask=ETrue). A
  picture without a mask is drawn whole.
- A 4-grey value v reaches the 5mx's 16-grey screen as v*5 (0, 5, 10,
  15). The pack stores screen values (0..15, 0 = black), so the matcher
  does not need to know the bitmap's depth.

Pairing
- AIF icons pair by application and size (24, 32 and 48 pixels on both
  machines, so nothing is resized). Colour is drawn where both masks draw;
  a pixel only the 5mx's mask draws keeps its grey.
- MBMs say nothing about which bitmap masks which, and the Series 7's
  files add masks and pictures and redraw some, so: a black-and-white
  bitmap after a same-sized picture, or one that is mostly black, or one
  whose black covers another picture's shape, is a mask. Each other 5mx
  picture is scored against each Series 7 colour picture of about the
  same size (within 2 pixels; the Series 7 squared some toolbar pictures
  off to 24x24, so the colour one is placed at the best offset, not
  resized): half the overlap of their shapes, half the correlation of
  their brightness, plus a bonus when it is the same picture number in
  its file. The best pairs are taken first and each colour picture is
  used once. Pairs that score badly are left grey. Check the result with
  --sheet: most toolbar pictures are certain, a few redrawn ones are a
  best guess.
"""
import base64, json, os, re, struct, sys

BASE = 0x50000000


# ---------------------------------------------------------------- ROM tree

def rom_root(d):
    if struct.unpack_from('<I', d, 0x8c)[0] == BASE:
        roots = struct.unpack_from('<I', d, 0x94)[0] - BASE
        return struct.unpack_from('<I', d, roots + 8)[0] - BASE
    for m in re.finditer(rb'\x10\x06System', d):
        entry = m.start() - 8
        size = struct.unpack_from('<I', d, entry - 4)[0]
        if 16 <= size < 4096:
            return entry - 4
    raise SystemExit('no ROM directory tree found')


def rom_files(d):
    files = {}

    def walk(off, prefix, depth):
        if depth > 16 or not 0 <= off < len(d) - 4:
            return
        size = struct.unpack_from('<I', d, off)[0]
        p = off + 4
        while p < off + size:
            esize, addr = struct.unpack_from('<iI', d, p)
            att, nlen = d[p + 8], d[p + 9]
            name = d[p + 10:p + 10 + nlen].decode('latin-1')
            path = prefix + '\\' + name
            if att & 0x10:
                walk(addr - BASE, path, depth + 1)
            else:
                files[path.lower()] = (path, d[addr - BASE:addr - BASE + esize])
            p += (10 + nlen + 3) & ~3

    walk(rom_root(d), 'Z:', 0)
    return files


def palette256(d):
    """The EPOC 256-colour palette, from the Series 7's GDI."""
    start = struct.pack('<6I', 0, 0x33, 0x66, 0x99, 0xcc, 0xff)
    i = d.find(start)
    if i < 0:
        raise SystemExit('no 256-colour palette in the Series 7 ROM')
    pal = []
    for v in struct.unpack_from('<256I', d, i):
        pal.append((v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff))
    return pal


# ---------------------------------------------------------------- bitmaps

class Bitmap:
    def __init__(self, w, h, bpp, colour, px):
        self.w, self.h, self.bpp, self.colour, self.px = w, h, bpp, colour, px

    def grey16(self, x, y):
        """Screen grey 0..15 for a grey bitmap."""
        v = self.px[y * self.w + x]
        return v * 15 // ((1 << self.bpp) - 1) if self.bpp <= 4 else v * 15 // 255

    def is_bw(self):
        top = (1 << self.bpp) - 1
        return not self.colour and all(v in (0, top) for v in self.px)


def unrle(data, comp, need):
    out = bytearray()
    p = 0
    if comp == 1:
        while p < len(data) and len(out) < need:
            n = data[p]; p += 1
            if n < 128:
                out += bytes([data[p]]) * (n + 1); p += 1
            else:
                n = 256 - n
                out += data[p:p + n]; p += n
    elif comp == 2:                        # 12-bit: 4-bit count-1, 12-bit value
        while p + 1 < len(data) and len(out) < need:
            v = data[p] | data[p + 1] << 8; p += 2
            out += struct.pack('<H', v & 0xfff) * ((v >> 12) + 1)
    elif comp == 3:                        # 16-bit
        while p < len(data) and len(out) < need:
            n = data[p]; p += 1
            if n < 128:
                out += data[p:p + 2] * (n + 1); p += 2
            else:
                n = 256 - n
                out += data[p:p + 2 * n]; p += 2 * n
    else:
        raise ValueError('compression %d' % comp)
    return bytes(out[:need]).ljust(need, b'\0')


def read_bitmap(buf, off):
    """A CFbsBitmap stream at buf[off]. Returns (Bitmap, bytes used)."""
    (size, ssize, w, h, _tw, _th, bpp, colour, _pal, comp) = \
        struct.unpack_from('<10i', buf, off)
    if ssize != 40 or not (0 < w <= 1024 and 0 < h <= 1024) or bpp not in (1, 2, 4, 8, 12, 16, 24):
        raise ValueError('not a bitmap')
    stride = ((w * bpp + 31) // 32) * 4
    raw = buf[off + 40:off + size]
    if comp:
        raw = unrle(raw, comp, stride * h)
    px = []
    for y in range(h):
        row = raw[y * stride:(y + 1) * stride]
        if bpp < 8:
            mask = (1 << bpp) - 1
            for x in range(w):
                bit = x * bpp
                px.append((row[bit >> 3] >> (bit & 7)) & mask)
        elif bpp == 8:
            px.extend(row[:w])
        elif bpp in (12, 16):
            px.extend(struct.unpack_from('<%dH' % w, row))
        else:
            px.extend(row[3 * x] | row[3 * x + 1] << 8 | row[3 * x + 2] << 16 for x in range(w))
    return Bitmap(w, h, bpp, colour, px), size


def aif_icons(f):
    if struct.unpack_from('<I', f, 4)[0] != 0x1000006A:
        return []
    p = struct.unpack_from('<I', f, 16)[0]
    p += 1 + (f[p] >> 1) * 6               # captions
    n = f[p] >> 1; p += 1
    icons = []
    for i in range(n):
        sid, _side = struct.unpack_from('<Ih', f, p + i * 6)
        bmp, used = read_bitmap(f, sid)
        mask, _ = read_bitmap(f, sid + used)
        icons.append((bmp, mask))
    return icons


def mbm_bitmaps(f):
    uid = struct.unpack_from('<I', f, 0)[0]
    if uid == 0x10000041:                  # ROM image: CBitwiseBitmaps in place
        n = struct.unpack_from('<I', f, 4)[0]
        out = []
        for q in struct.unpack_from('<%dI' % n, f, 8):
            try:
                if struct.unpack_from('<I', f, q)[0] != 0x10000040:
                    raise ValueError('not a CBitwiseBitmap')
                size = struct.unpack_from('<i', f, q + 0x14)[0]
                data = q + struct.unpack_from('<i', f, q + 0x40)[0]
                out.append(read_bitmap(f[q + 0x14:q + 0x14 + 40] + f[data:data + size - 40], 0)[0])
            except (ValueError, struct.error):
                out.append(None)
        return out
    if struct.unpack_from('<II', f, 0) != (0x10000037, 0x10000042):
        return []
    tr = struct.unpack_from('<I', f, 16)[0]
    n = struct.unpack_from('<I', f, tr)[0]
    out = []
    for q in struct.unpack_from('<%dI' % n, f, tr + 4):
        try:
            out.append(read_bitmap(f, q)[0])
        except ValueError:
            out.append(None)
    return out


def to_rgb(b, pal):
    if b.colour and b.bpp == 8:
        return [pal[v] for v in b.px]
    if b.colour and b.bpp == 12:
        return [((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17) for v in b.px]
    if b.colour and b.bpp == 16:
        return [((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31) for v in b.px]
    if b.colour and b.bpp == 24:
        return [(v & 255, v >> 8 & 255, v >> 16) for v in b.px]
    top = (1 << b.bpp) - 1
    return [(v * 255 // top,) * 3 for v in b.px]


# ---------------------------------------------------------------- pairing

def opaque_from_mask(mask):
    """1 where the mask lets the picture through (black in the mask)."""
    top = (1 << mask.bpp) - 1
    return [1 if v * 2 < top else 0 for v in mask.px]


def mask_indices(bitmaps):
    """Which bitmaps of an MBM are masks: a black-and-white bitmap straight
    after a picture of the same size that is not black and white."""
    out = set()
    for k in range(1, len(bitmaps)):
        a, b = bitmaps[k - 1], bitmaps[k]
        if a is None or b is None or k - 1 in out or (a.w, a.h) != (b.w, b.h):
            continue
        if b.is_bw() and (a.colour or not a.is_bw()):
            out.add(k)
    # a mask kept away from its picture (pictures first, then their masks): a
    # black-and-white bitmap whose black covers another picture's shape, or
    # is mostly black (a silhouette; real black-and-white pictures are line
    # drawings)
    for k, b in enumerate(bitmaps):
        if b is None or k in out or not b.is_bw():
            continue
        if sum(1 for v in b.px if v == 0) > 0.4 * len(b.px):
            out.add(k)
            continue
        for j, a in enumerate(bitmaps):
            if j == k or a is None or (a.w, a.h) != (b.w, b.h) or a.is_bw():
                continue
            fg = foreground(a, [1] * (a.w * a.h))
            inside = sum(1 for i, f in enumerate(fg) if f and b.px[i] == 0)
            if sum(fg) and inside >= 0.97 * sum(fg) and inside < sum(1 for v in b.px if v == 0):
                out.add(k)
                break
    return out


def lum(rgb):
    r, g, b = rgb
    return (r * 299 + g * 587 + b * 114) // 1000


def foreground(grey, mask_on):
    """The pixels that make the picture: its mask, or for a picture without
    one, the pixels unlike its background (the commonest grey on its edge)."""
    if not all(mask_on):
        return mask_on
    w, h = grey.w, grey.h
    edge = [grey.px[i] for i in range(w * h) if i % w in (0, w - 1) or i // w in (0, h - 1)]
    bg = max(set(edge), key=edge.count)
    return [1 if v != bg else 0 for v in grey.px]


def fit(grey, mask_on, colour, cmask, pal):
    """How well a colour picture stands for a grey one, 0..1: half the
    overlap of their shapes (intersection over union) and half the
    correlation of their brightness where both draw, at the best offset
    (dx, dy) of the colour picture in the grey one's box, as sizes may
    differ by a pixel or two (the Series 7 squared some toolbar pictures to
    24x24). Brightness alone is no guide: a grey globe is a green and blue
    one on the Series 7, and some toolbar pictures were redrawn."""
    fg = foreground(grey, mask_on)
    cl = [lum(c) for c in to_rgb(colour, pal)]
    top = (1 << grey.bpp) - 1
    best = None
    for dy in range(min(0, grey.h - colour.h), max(0, grey.h - colour.h) + 1):
        for dx in range(min(0, grey.w - colour.w), max(0, grey.w - colour.w) + 1):
            inter = union = 0
            xs, ys = [], []
            for y in range(grey.h):
                cy = y - dy
                for x in range(grey.w):
                    i, cx = y * grey.w + x, x - dx
                    inside = 0 <= cx < colour.w and 0 <= cy < colour.h
                    on7 = inside and (cmask[cy * colour.w + cx] if cmask else fg[i])
                    if fg[i] and on7:
                        inter += 1
                        xs.append(grey.px[i] * 255 // top); ys.append(cl[cy * colour.w + cx])
                    if fg[i] or on7:
                        union += 1
            corr = 0.0
            if len(xs) > 2:
                n = len(xs); mx = sum(xs) / n; my = sum(ys) / n
                sxx = sum((v - mx) ** 2 for v in xs); syy = sum((v - my) ** 2 for v in ys)
                if sxx and syy:
                    corr = sum((a - mx) * (b - my) for a, b in zip(xs, ys)) / (sxx * syy) ** 0.5
            score = 0.5 * inter / max(1, union) + 0.5 * corr
            if best is None or score > best[0]:
                best = (score, dx, dy)
    return best


# Left out of the pack as standard, to keep it small for the Atom's flash:
# Sketch's clip art (pictures inside a drawing, not the interface; nearly
# half the pack), the OPL demo programs' pictures, and the 9- and 12-pixel
# AIF icons (the System screen and the Extras bar draw 24, 32 and 48), and
# EIKON's background texture (Eikon.mbm 0: it is tiled under other controls,
# so only some tiles are whole and the screen comes out patchy).
LEAVE_OUT = [r'/Clpart\.mbm#', r'^System/OPL/', r'\.aif#(9x9|12x12)$', r'/Eikon\.mbm#0$']

MIN_FIT = 0.25           # below this a grey picture keeps its grey
ORDER_BONUS = 0.25       # for the colour picture in the same place in its file


def make_entry(name, kind, grey, mask_on, colour, cmask, dx, dy, pal):
    """One pack entry at the 5mx picture's size: its screen greys (0..15),
    the pixels it draws (to match), and the colour picture's palette
    indices with the pixels to draw in colour (the 5mx's mask and the
    Series 7's, both)."""
    w, h = grey.w, grey.h
    g = [grey.grey16(x, y) for y in range(h) for x in range(w)]
    c = [0] * (w * h)
    a = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            cx, cy = x - dx, y - dy
            if mask_on[i] and 0 <= cx < colour.w and 0 <= cy < colour.h:
                j = cy * colour.w + cx
                if cmask is None or cmask[j]:
                    c[i] = colour.px[j]; a[i] = 1
    return {'name': name, 'kind': kind, 'w': w, 'h': h,
            'g': g, 'm': list(mask_on), 'c': c, 'a': a, 'offset': (dx, dy)}


def build(rom5, rom7):
    d5, d7 = open(rom5, 'rb').read(), open(rom7, 'rb').read()
    f5, f7 = rom_files(d5), rom_files(d7)
    pal = palette256(d7)
    pack, notes = [], []
    for key in sorted(f5):
        if key not in f7:
            continue
        path, b5 = f5[key]
        b7 = f7[key][1]
        short = path[3:].replace('\\', '/')
        if key.endswith('.aif'):
            i5, i7 = aif_icons(b5), aif_icons(b7)
            cands = [(j, c, opaque_from_mask(cm)) for j, (c, cm) in enumerate(i7)
                     if c.colour and c.bpp == 8 and (cm.w, cm.h) == (c.w, c.h)]
            for k, (g, gm) in enumerate(i5):
                mask_on = opaque_from_mask(gm)
                same = [x for x in cands if (x[1].w, x[1].h) == (g.w, g.h)]
                if not same:
                    notes.append('%s %dx%d: the Series 7 has no colour icon of that size' % (short, g.w, g.h))
                    continue
                _, c, cm = same[0]
                pack.append(make_entry('%s#%dx%d' % (short, g.w, g.h), 'aif', g, mask_on, c, cm, 0, 0, pal))
        elif key.endswith('.mbm'):
            m5, m7 = mbm_bitmaps(b5), mbm_bitmaps(b7)
            masks5, masks7 = mask_indices(m5), mask_indices(m7)
            cands = [(j, c, opaque_from_mask(m7[j + 1]) if j + 1 in masks7 and (m7[j + 1].w, m7[j + 1].h) == (c.w, c.h) else None)
                     for j, c in enumerate(m7) if c is not None and c.colour and c.bpp == 8 and j not in masks7]
            # the Series 7 file has the same pictures but not always in the same
            # order, nor all drawn the same (it adds masks and pictures, and
            # redrew some): pair each grey picture with the colour one most
            # like it, best pairs first, each colour picture used once (a grey
            # picture that is in the file twice shares its partner)
            todo, sigs = [], {}
            for k, g in enumerate(m5):
                if g is None or g.colour or k in masks5:
                    continue
                mask_on, kind = [1] * (g.w * g.h), 'mbm'
                if k + 1 in masks5 and (m5[k + 1].w, m5[k + 1].h) == (g.w, g.h):
                    mask_on, kind = opaque_from_mask(m5[k + 1]), 'mbm+mask'
                if sum(mask_on) < 12 or len({g.px[i] for i in range(len(mask_on)) if mask_on[i]}) < 2:
                    continue                    # blank or one-grey: nothing to find
                sig = (g.w, g.h, tuple(g.px), tuple(mask_on))
                if sig in sigs:
                    continue
                sigs[sig] = k
                todo.append((k, g, mask_on, kind))
            # (Psion mostly kept the order: the n-th picture of the 5mx file is
            # often the n-th of the Series 7's, which settles near things)
            ord5 = {k: n for n, k in enumerate(k for k, g in enumerate(m5) if g is not None and k not in masks5)}
            ord7 = {j: n for n, (j, c, cm) in enumerate(cands)}
            scored = []
            for k, g, mask_on, kind in todo:
                for j, c, cm in cands:
                    if abs(c.w - g.w) <= 2 and abs(c.h - g.h) <= 2:
                        f = fit(g, mask_on, c, cm, pal)
                        if f and f[0] >= MIN_FIT:
                            score = f[0] + (ORDER_BONUS if ord5.get(k) == ord7[j] else 0)
                            scored.append((-score, abs(j - k), k, j, f[1], f[2]))
            scored.sort()
            taken_k, taken_j, chosen = set(), set(), {}
            for _, _, k, j, dx, dy in scored:
                if k in taken_k or j in taken_j:
                    continue
                taken_k.add(k); taken_j.add(j); chosen[k] = (j, dx, dy)
            byj = {j: (c, cm) for j, c, cm in cands}
            for k, g, mask_on, kind in todo:
                if k not in chosen:
                    continue
                j, dx, dy = chosen[k]
                c, cm = byj[j]
                rgb = [pal[c.px[(y - dy) * c.w + x - dx]] for y in range(g.h) for x in range(g.w)
                       if 0 <= x - dx < c.w and 0 <= y - dy < c.h]
                if all(r == gg == b for r, gg, b in rgb):
                    continue                    # the Series 7 has no colour here either
                pack.append(make_entry('%s#%d' % (short, k), kind, g, mask_on, c, cm, dx, dy, pal))
    return pack, notes, pal


def pack_bits(bits):
    out = bytearray((len(bits) + 7) // 8)
    for i, v in enumerate(bits):
        if v:
            out[i >> 3] |= 1 << (i & 7)
    return bytes(out)


def pack_nibbles(vals):
    out = bytearray((len(vals) + 1) // 2)
    for i, v in enumerate(vals):
        out[i >> 1] |= (v & 15) << (4 * (i & 1))
    return bytes(out)


def b64(b):
    return base64.b64encode(b).decode()


def encode(pack, pal):
    """The pack as iconmatch.js reads it (version 2): per picture its name,
    size, screen greys (4 bits, first pixel in the low half; 0 where it
    does not draw), the pixels to match (m: a bit each, row by row, first in
    bit 0), the pixels to draw in colour (a: bits likewise) and, for each of
    those in order, its Series 7 palette index (c); the palette once, as
    256 RGB triples."""
    return {
        'format': 'psicast-iconpack', 'version': 2,
        'palette': b64(bytes(v for rgb in pal for v in rgb)),
        'icons': [{'n': e['name'], 'w': e['w'], 'h': e['h'],
                   'g': b64(pack_nibbles([v if m else 0 for v, m in zip(e['g'], e['m'])])),
                   'm': b64(pack_bits(e['m'])), 'a': b64(pack_bits(e['a'])),
                   'c': b64(bytes(c for c, a in zip(e['c'], e['a']) if a))} for e in pack],
    }


def contact_sheet(pack, pal, path):
    from PIL import Image, ImageDraw
    cols, cw, ch = 6, 106, 64
    rows = (len(pack) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * cw, max(1, rows) * ch), (255, 255, 255))
    dr = ImageDraw.Draw(sheet)
    bg = (200, 200, 200)
    for i, e in enumerate(pack):
        x0, y0 = (i % cols) * cw, (i // cols) * ch
        w, h = e['w'], e['h']
        a = Image.new('RGB', (w, h), bg); b = Image.new('RGB', (w, h), bg)
        for p in range(w * h):
            if e['m'][p]:
                v = e['g'][p] * 17
                a.putpixel((p % w, p // w), (v, v, v))
                b.putpixel((p % w, p // w), (v, v, v))
            if e['a'][p]:
                b.putpixel((p % w, p // w), pal[e['c'][p]])
        s = 1 if max(w, h) > 24 else 2
        if max(w, h) > 48:
            a.thumbnail((48, 48)); b.thumbnail((48, 48)); s = 1
        a = a.resize((a.width * s, a.height * s), Image.NEAREST)
        b = b.resize((b.width * s, b.height * s), Image.NEAREST)
        sheet.paste(a, (x0 + 2, y0 + 2)); sheet.paste(b, (x0 + 4 + a.width, y0 + 2))
        dr.text((x0 + 2, y0 + 52), e['name'].split('/')[-1][:17], fill=(0, 0, 0))
    sheet.save(path)


def main():
    import argparse
    ap = argparse.ArgumentParser(description='Build the PsiCast Series 7 icon pack.')
    ap.add_argument('rom5mx'); ap.add_argument('rom7')
    ap.add_argument('out', help='the pack (.json)')
    ap.add_argument('--sheet', help='also write a contact sheet (PNG): 5mx grey beside Series 7 colour')
    ap.add_argument('--keep', help='a file of picture names (one a line) to keep; '
                    'without it every picture that pairs is kept')
    ap.add_argument('--list', action='store_true', help='print the names of the pictures')
    ap.add_argument('--everything', action='store_true',
                    help='keep the pictures that are left out as standard (see LEAVE_OUT)')
    o = ap.parse_args()
    pack, notes, pal = build(o.rom5mx, o.rom7)
    # the same grey picture twice (an app's icon in its .aif and its .mbm): once
    seen, uniq = set(), []
    for e in pack:
        sig = (e['w'], e['h'], tuple(v if m else 99 for v, m in zip(e['g'], e['m'])))
        if sig not in seen:
            seen.add(sig); uniq.append(e)
    pack = uniq
    if not o.everything:
        pack = [e for e in pack if not any(re.search(r, e['name']) for r in LEAVE_OUT)]
    if o.keep:
        want = [l.strip() for l in open(o.keep) if l.strip() and not l.startswith('#')]
        have = {e['name']: e for e in pack}
        for n in want:
            if n not in have:
                notes.append('--keep: no picture %s' % n)
        pack = [have[n] for n in want if n in have]
    if o.list:
        for e in pack:
            print(e['name'], '%dx%d' % (e['w'], e['h']), e['kind'], 'offset %d,%d' % e['offset'])
    os.makedirs(os.path.dirname(os.path.abspath(o.out)), exist_ok=True)
    with open(o.out, 'w') as f:
        json.dump(encode(pack, pal), f, separators=(',', ':'))
        f.write('\n')
    if o.sheet:
        contact_sheet(pack, pal, o.sheet)
    for n in notes:
        print('note:', n, file=sys.stderr)
    raw = os.path.getsize(o.out)
    import gzip
    gz = len(gzip.compress(open(o.out, 'rb').read(), 9))
    print('%d pictures -> %s (%d bytes, %d gzipped)' % (len(pack), o.out, raw, gz))


if __name__ == '__main__':
    main()
