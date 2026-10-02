#!/usr/bin/env python3
"""mkfont.py - Links' built-in fonts, cut down for the Psion.

Links draws text from its own fonts: every glyph is a grey PNG about 120
pixels high in font_inc.c (7.4 MB of C, 2.7 MB of data). Each glyph is
decoded, gamma-corrected and scaled down the first time a size is drawn,
which on a 36 MHz ARM with no FPU is most of the cost of showing a page.

This writes a font_inc.c that Links uses unchanged, with:
  - only Latin, punctuation and a few symbol blocks (the rest, e.g. Greek,
    Cyrillic and CJK, falls back to Links' "missing glyph" blotch);
  - glyphs re-sampled to a smaller master height (default 48 pixels), so
    there are fewer pixels to inflate and scale;
  - the gamma applied here: the PNGs hold linear ("photon") values and
    say gAMA 1.0, so libpng does no gamma work per glyph;
  - with --rle, each glyph as simple run-length data instead of a PNG (the
    same size in all, but about 30 times cheaper to unpack than a PNG on the
    ARM: no zlib, CRC or libpng set-up per glyph). Links' dip.c (PSIWEB)
    reads both. The format: 'R', width and height (16-bit, big-endian),
    then tokens: 0x00-0x3f a run of 1-64 zeros (paper), 0x40-0x7f a run of
    1-64 255s (ink), 0x80-0xff 1-128 literal bytes follow.

    mkfont.py SRC_FONT_INC_C OUT.c [--height 48] [--all] [--rle]
"""
import io, re, struct, sys, zlib
from PIL import Image

args = sys.argv[1:]
HEIGHT = 48
if "--height" in args:
    HEIGHT = int(args[args.index("--height") + 1])
ALL = "--all" in args
RLE = "--rle" in args
src, out = [a for a in args if not a.startswith("--") and not (args.index(a) > 0 and args[args.index(a) - 1] == "--height")][:2]

KEEP = [(0x20, 0x17f),        # Basic Latin, Latin-1, Latin Extended-A
        (0x218, 0x21b),       # (Romanian S and T with comma)
        (0x2c6, 0x2dd),       # spacing accents
        (0x2000, 0x206f),     # general punctuation
        (0x20a0, 0x20cf),     # currency
        (0x2100, 0x2122),     # letterlike symbols to the trade mark
        (0x2190, 0x2193),     # arrows
        (0x2212, 0x2212),     # minus
        (0x25a0, 0x25cf),     # geometric shapes (list bullets)
        (0xfb00, 0xfb06)]     # ligatures

def keep(code):
    return ALL or any(a <= code <= b for a, b in KEEP)

text = open(src, encoding="latin-1").read()

ESC = {"n": 10, "r": 13, "t": 9, "b": 8, "f": 12, "v": 11, "a": 7, "\\": 92, "'": 39, '"': 34, "?": 63}
def c_bytes(lit):
    """the bytes of a run of C string literals"""
    outb = bytearray()
    for seg in re.findall(r'"((?:[^"\\]|\\.)*)"', lit, re.S):
        i = 0
        while i < len(seg):
            ch = seg[i]
            if ch != "\\":
                outb.append(ord(ch)); i += 1; continue
            m = re.match(r"[0-7]{1,3}", seg[i + 1:i + 4])
            if m:
                outb.append(int(m.group(0), 8) & 255); i += 1 + len(m.group(0))
            else:
                outb.append(ESC[seg[i + 1]]); i += 2
    return bytes(outb)

letters = {}
for m in re.finditer(r"static_const unsigned char (letter_\d+)\[\] = (.*?);\n", text, re.S):
    letters[m.group(1)] = c_bytes(m.group(2))
table = re.findall(r"\{ (letter_\d+), 0x([0-9a-f]+), 0x([0-9a-f]+),\s*(\d+),\s*(\d+), NULL \}", text)
fonts = [tuple(map(int, f)) for f in re.findall(r"\{ (\d+), (\d+) \},", text.split("font_table")[1])]

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

def png_grey(im):
    """8-bit grey PNG with gAMA 1.0, each row with the filter that packs best"""
    w, h = im.size
    px = im.tobytes()
    raw = bytearray()
    prev = bytes(w)
    for y in range(h):
        row = px[y * w:(y + 1) * w]
        cands = [b"\0" + row,
                 b"\1" + bytes((row[x] - (row[x - 1] if x else 0)) & 255 for x in range(w)),
                 b"\2" + bytes((row[x] - prev[x]) & 255 for x in range(w))]
        raw += min(cands, key=lambda c: sum(v if v < 128 else 256 - v for v in c[1:]))
        prev = row
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
            + chunk(b"gAMA", struct.pack(">I", 100000))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))

def rle_grey(im):
    """the run-length form (see above)"""
    w, h = im.size
    px = im.tobytes()
    out = bytearray(b"R" + struct.pack(">HH", w, h))
    lit = bytearray()
    def flush():
        while lit:
            k = min(len(lit), 128)
            out.append(0x80 + k - 1); out.extend(lit[:k]); del lit[:k]
    i, n = 0, len(px)
    while i < n:
        v = px[i]
        if v in (0, 255):
            j = i
            while j < n and px[j] == v and j - i < 64:
                j += 1
            if j - i >= 2 or not lit:
                flush()
                out.append((0 if v == 0 else 0x40) + (j - i) - 1)
                i = j
                continue
        lit.append(v); i += 1
    flush()
    return bytes(out)

# 2.2 from Links' glyphs (gAMA 0.45455) to linear, as libpng would have done
LIN = [round(255 * (v / 255.0) ** (1 / 0.45455)) for v in range(256)]

new_letters, new_table, new_fonts = [], [], []
orig_bytes = 0
for fi, (begin, length) in enumerate(fonts):
    nb = len(new_table)
    for i in range(begin, begin + length):
        name, ln, code, xs, ys = table[i]
        code = int(code, 16)
        data = letters[name][:int(ln, 16)]
        orig_bytes += len(data)
        if fi > 0 and not keep(code):
            continue
        im = Image.open(io.BytesIO(data))
        im.load()
        if im.mode not in ("L", "RGB", "RGBA", "LA", "P", "I"):
            im = im.convert("RGB")
        im = im.convert("L").point(LIN)
        w, h = im.size
        nh = min(HEIGHT, h)
        nw = max(1, round(w * nh / h))
        if (nw, nh) != (w, h):
            im = im.resize((nw, nh), Image.LANCZOS)
        png = rle_grey(im) if RLE else png_grey(im)
        # Links keeps the master's width and height for its metrics: scale
        # them with the picture, keeping the proportions
        new_letters.append(png)
        new_table.append((len(png), code, nw, nh))
    new_fonts.append((nb, len(new_table) - nb))

with open(out, "w") as f:
    f.write("/* font_inc.c for PsiWeb, written by web/links/mkfont.py from Links' own:\n"
            " * %s, master height %d px, gamma applied (gAMA 1.0), %s */\n" %
            ("all glyphs" if ALL else "Latin subset", HEIGHT, "run-length glyphs" if RLE else "PNG glyphs"))
    f.write('#include "cfg.h"\n\n#ifdef G\n\n#include "links.h"\n\n')
    for i, png in enumerate(new_letters):
        f.write("static_const unsigned char letter_%d[] = {" % i)
        for j, b in enumerate(png):
            f.write(("\n" if j % 24 == 0 else "") + "%d," % b)
        f.write("\n};\n")
    f.write("\nstruct letter letter_data[%d] = {\n" % len(new_table))
    for i, (ln, code, w, h) in enumerate(new_table):
        f.write("\t{ letter_%d, 0x%08x, 0x%08x, %3d, %3d, NULL },\n" % (i, ln, code, w, h))
    f.write("};\n\nstruct font font_table[%d] = {\n" % len(new_fonts))
    for b, l in new_fonts:
        f.write("\t{ %d, %d },\n" % (b, l))
    f.write("};\n\n#endif\n")

tot = sum(len(p) for p in new_letters)
print("%d of %d glyphs, %d KB of %s (was %d KB of PNG), master height %d" %
      (len(new_table), len(table), tot // 1024, "run-length data" if RLE else "PNG", orig_bytes // 1024, HEIGHT))
for (b, l), name in zip(new_fonts, ("system", "normal", "bold", "monospaced")):
    print("  %-10s %4d glyphs, %4d KB" % (name, l, sum(len(p) for p in new_letters[b:b + l]) // 1024))
