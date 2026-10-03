#!/usr/bin/env python3
"""mkstrike.py - hinted glyphs for PsiWeb's Links, pre-rendered at the sizes
it actually draws.

Links draws text by scaling a 40 px master of each glyph down to the size
wanted, then sharpening it (dip.c). At 12 to 24 pixels high that gives soft
stems that fall between pixels, which the 16-grey screen shows as grey
smudges. This writes "strikes" instead: every glyph rendered by FreeType
(through Pillow) with the font's own TrueType hinting, at exactly the cell
heights Links asks for at 100% zoom, so stems and x-heights land on whole
pixels. dip.c (PSI_STRIKES) uses a strike when one exists for the style's
height and family, and only while the page's base size is the one the
strikes were made for (psi_strike_base): at any other zoom the whole page
uses the scaled masters, so one page never mixes the two typefaces. A
character a strike does not hold also falls back to its master.

The typeface is DejaVu Sans (Bold, Sans Mono): at about 12 px to the em a
sans with a large x-height and strong hinting reads far better on a
640x240 LCD than a serif, whose serifs and thin strokes become one-pixel
grey blobs; it is also close to the Arial-like faces of the Psion's own
programs. Licence: Bitstream Vera / DejaVu (free, see THIRD-PARTY.md).

Geometry: the em is chosen so the font's whole ascent and descent fit in
the cell (em = height / 1.164 for DejaVu); the baseline sits at the cell's
top slack plus the hinted ascent. Widths are the hinted advances (one
width for every glyph of the monospaced family). Glyphs that overhang
their advance (a negative left bearing, as on 'j') are moved inside it,
since Links draws each glyph as an opaque box.

Pixel values are 0 paper to 255 ink, as in mkfont.py's masters, and dip.c
mixes them in linear light: FreeType's coverage is corrected for that
first (--blend, below), or the text comes out pale.

The output is a C file with the glyphs in mkfont.py's run-length form ('R',
width, height, runs) and a table of strikes (links.h struct psi_strike).

    mkstrike.py OUT.c [--base 14]     (the body size; strikes for all 7 HTML sizes)
                      [--full 0:14]   (slot:height strikes with the whole subset)
                      [--blend 2.2]   (1: raw coverage)
                      [--fontdir /usr/share/fonts/truetype/dejavu] [--preview PNG]
"""
import os, struct, sys
from PIL import Image, ImageDraw, ImageFont

args = sys.argv[1:]
def opt(name, default):
    if name in args:
        v = args[args.index(name) + 1]
        del args[args.index(name):args.index(name) + 2]
        return v
    return default
# Links' seven HTML font sizes for a base (body) size, as html_gr.c's
# get_real_font_size; PsiWeb's base is 14 (psi_os.c -html-user-font-size)
# at 100% zoom: 12, 13, 14, 16, 19, 21 and 24. Headings are 19 to 24 bold.
BASE = int(opt("--base", "14"))
SIZES = [(k * BASE) >> 4 for k in (14, 15, 16, 19, 22, 25, 28)]
# Which of them each family gets. Measured on the page set (68k.news, NPR,
# Wikipedia, BBC, CERN): body text is normal 14, headings bold 16 to 24,
# <pre>/<code> mono 14; normal text at the heading sizes and small
# monospaced text are rare, and use the masters (saving 76 KB).
NORMAL_SIZES, BOLD_SIZES, MONO_SIZES = SIZES[:4], SIZES, SIZES[1:3]
FONTDIR = opt("--fontdir", "/usr/share/fonts/truetype/dejavu")
PREVIEW = opt("--preview", None)
# Links mixes ink and paper in linear light ("photon" space). FreeType's
# coverage is meant to be blended in the screen's own (gamma) space, as
# every desktop renderer does; mixed linearly, a stem that half covers a
# pixel comes out a pale grey, and text looks thin and faint. So the
# coverage c is stored as 1 - (1 - c)^BLEND: mixed linearly, black on
# white then gives the same grey as c blended in sRGB (BLEND = 2.2).
BLEND = float(opt("--blend", "2.2"))
LUT = bytes(round(255 * (1 - (1 - v / 255.0) ** BLEND)) for v in range(256))
FULL = {tuple(int(v) for v in p.split(":")) for p in opt("--full", "0:%d" % BASE).split(",")}  # slot:height
out = args[0]

FAMILIES = [(0, "DejaVuSans.ttf", NORMAL_SIZES),   # normal
            (1, "DejaVuSans-Bold.ttf", BOLD_SIZES),# bold
            (2, "DejaVuSansMono.ttf", MONO_SIZES)] # monospaced

# the same Latin subset as mkfont.py
KEEP = [(0x20, 0x17f), (0x218, 0x21b), (0x2c6, 0x2dd), (0x2000, 0x206f), (0x20a0, 0x20cf),
        (0x2100, 0x2122), (0x2190, 0x2193), (0x2212, 0x2212), (0x25a0, 0x25cf), (0xfb00, 0xfb06)]
CODES = [c for a, b in KEEP for c in range(a, b + 1)]
# Every strike but body text (normal, 14 px) holds only ASCII, Latin-1 and
# the Windows-1252 extras (typographic quotes, dashes, the euro...): the
# rest of Latin Extended-A and the symbols are rare outside body text, and
# fall back to the masters. This keeps the strikes to about a third.
BASIC = set(range(0x20, 0x7f)) | set(range(0xa0, 0x100)) | {
    0x152, 0x153, 0x160, 0x161, 0x178, 0x17d, 0x17e, 0x192, 0x2c6, 0x2dc, 0x2013, 0x2014,
    0x2018, 0x2019, 0x201a, 0x201c, 0x201d, 0x201e, 0x2020, 0x2021, 0x2022, 0x2026,
    0x2030, 0x2039, 0x203a, 0x20ac, 0x2122}
SPACES = {0x20, 0xa0, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007,
          0x2008, 0x2009, 0x200a, 0x202f, 0x205f}

def rle_grey(w, h, px):
    """mkfont.py's run-length form"""
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

def mask_key(font, ch):
    m = font.getmask(ch)
    return (m.size, bytes(m))

strikes = []           # (height, slot, [(code, rle)])
previews = []
for slot, fname, sizes in FAMILIES:
    path = os.path.join(FONTDIR, fname)
    for h in sizes:
        em = int(h / 1.164)
        font = ImageFont.truetype(path, em)
        asc, desc = font.getmetrics()
        while asc + desc > h and em > 4:        # (hinting may round up)
            em -= 1
            font = ImageFont.truetype(path, em)
            asc, desc = font.getmetrics()
        base = (h - asc - desc) + asc           # slack at the top, as Links' masters
        notdef = mask_key(font, "")
        mono_w = round(font.getlength("0")) if slot == 2 else None
        glyphs = []
        for code in CODES:
            if (slot, h) not in FULL and code not in BASIC:
                continue
            ch = chr(code)
            if code not in SPACES and mask_key(font, ch) == notdef:
                continue                        # not in the font: Links' master
            w = mono_w or max(1, round(font.getlength(ch)))
            if code == 0xad:
                continue                        # soft hyphen: width 0 in Links
            im = Image.new("L", (w + 2 * h, h), 0)
            d = ImageDraw.Draw(im)
            d.text((h, base), ch, font=font, fill=255, anchor="ls")
            bb = im.getbbox()
            dx = 0
            if bb:
                l, r = bb[0] - h, bb[2] - h     # ink extent relative to the pen
                if l < 0:
                    dx = -l                     # (any right overhang is then clipped)
                elif r > w:
                    dx = -min(r - w, l)         # move left, but not past the left edge
            g = im.crop((h - dx, 0, h - dx + w, h))
            glyphs.append((code, rle_grey(w, h, g.point(list(LUT)).tobytes())))
        strikes.append((h, slot, glyphs))
        if PREVIEW:
            previews.append((slot, h, font, base))

with open(out, "w") as f:
    f.write("/* Hinted strikes for PsiWeb's Links, written by web/links/mkstrike.py:\n"
            " * DejaVu Sans, Sans Bold and Sans Mono rendered by FreeType at the\n"
            " * cell heights PsiWeb draws (see dip.c, PSI_STRIKES). */\n")
    f.write('#include "cfg.h"\n\n#ifdef G\n\n#include "links.h"\n\n#ifdef PSI_STRIKES\n\n')
    blob = bytearray()
    starts = []
    for si, (h, slot, glyphs) in enumerate(strikes):
        offs = []
        starts.append(len(blob))
        for code, rle in glyphs:
            offs.append(len(blob) - starts[-1])
            blob += rle
        offs.append(len(blob) - starts[-1])
        assert offs[-1] < 65536
        f.write("static const unsigned short strike%d_codes[%d] = {" % (si, len(glyphs)))
        for j, (code, _) in enumerate(glyphs):
            f.write(("\n" if j % 16 == 0 else "") + "%d," % code)
        f.write("\n};\nstatic const unsigned short strike%d_offs[%d] = {" % (si, len(offs)))
        for j, o in enumerate(offs):
            f.write(("\n" if j % 12 == 0 else "") + "%d," % o)
        f.write("\n};\n")
    f.write("static const unsigned char strike_data[%d] = {" % max(1, len(blob)))
    for j, b in enumerate(blob):
        f.write(("\n" if j % 24 == 0 else "") + "%d," % b)
    f.write("\n};\n\nconst struct psi_strike psi_strike_table[%d] = {\n" % len(strikes))
    for si, (h, slot, glyphs) in enumerate(strikes):
        f.write("\t{ %d, %d, %d, strike%d_codes, strike%d_offs, strike_data + %d },\n" % (h, slot, len(glyphs), si, si, starts[si]))
    f.write("};\nconst int psi_strike_count = %d;\nconst int psi_strike_base = %d;\n\n#endif\n\n#endif\n" % (len(strikes), BASE))

print("%d strikes, %d glyphs, %d KB of run-length data" %
      (len(strikes), sum(len(g) for _, _, g in strikes), len(blob) // 1024))

if PREVIEW:
    text = "Stock Market Today: Dow Opens Higher, Yields Fall 1234 gjpqy"
    rows = []
    for slot, h, font, base in previews:
        im = Image.new("L", (640, h), 255)
        ImageDraw.Draw(im).text((2, base), "%d/%d " % (slot, h) + text, font=font, fill=0, anchor="ls")
        rows.append(im.point(lambda v: round(v * 15 / 255) * 17))
    sheet = Image.new("L", (640, sum(r.size[1] + 2 for r in rows)), 255)
    y = 0
    for r in rows:
        sheet.paste(r, (0, y)); y += r.size[1] + 2
    sheet.save(PREVIEW)
