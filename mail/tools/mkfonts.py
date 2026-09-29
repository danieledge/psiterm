#!/usr/bin/env python3
"""mkfonts.py - PsiMail's fonts and icons, pre-rendered for the Psion.

The Psion's own fonts are black and white. PsiMail draws its text itself,
anti-aliased in the screen's 16 greys, from glyphs rendered here with
FreeType (light hinting: crisp horizontals) out of:
  Inter (SIL OFL)          - the UI and message text
  DejaVu Sans Mono         - <pre> and code
  Lucide (ISC)             - icons
Each glyph is 4 bits of coverage per pixel. Characters are the Psion's
8-bit set, Windows-1252.

    mail/tools/mkfonts.py > mail/ui/pmfonts.cpp
"""
import os, sys
import freetype

HERE = os.path.dirname(os.path.abspath(__file__))
FD = os.path.join(HERE, "..", "fonts")

# name, file, pixel size
FACES = [
    ("R11", "Inter-Regular.ttf", 11),
    ("R12", "Inter-Regular.ttf", 12),
    ("R13", "Inter-Regular.ttf", 13),
    ("S11", "Inter-SemiBold.ttf", 11),
    ("S12", "Inter-SemiBold.ttf", 12),
    ("S13", "Inter-SemiBold.ttf", 13),
    ("S16", "Inter-SemiBold.ttf", 16),
    ("S20", "Inter-SemiBold.ttf", 20),
    ("I13", "Inter-Italic.ttf", 13),
    ("M11", "DejaVuSansMono.ttf", 11),
]

# icon name, Lucide name (see its codepoints.json)
ICONS = ["inbox", "send", "pencil", "archive", "trash-2", "shield-alert", "folder", "clock",
         "paperclip", "flag", "reply", "refresh-cw", "square-pen", "search", "calendar", "star",
         "mail", "mail-open", "reply-all", "forward", "globe", "chevron-right", "cloud-off",
         "wifi", "arrow-up-from-line", "calendar-sync", "image", "download", "check", "x",
         "circle-user", "users", "bell", "map-pin", "folder-open", "settings", "chevron-left",
         "external-link", "loader", "circle-alert", "file", "plus"]
ICON_SIZES = [14, 18]

GAMMA = 0.85     # a touch darker than linear: thin strokes stay visible on the LCD

def cp1252_chars():
    out = []
    for c in range(32, 256):
        try:
            u = bytes([c]).decode("cp1252")
        except UnicodeDecodeError:
            u = "?"
        out.append(u)
    return out

def render(face, ch, flags):
    face.load_char(ch, flags)
    g = face.glyph
    b = g.bitmap
    rows = []
    for r in range(b.rows):
        row = []
        for c in range(b.width):
            v = b.buffer[r * b.pitch + c] / 255.0
            v = round(15 * (v ** GAMMA)) if v > 0 else 0
            row.append(v)
        rows.append(row)
    # trim empty columns/rows so the data stays small
    left, top = g.bitmap_left, g.bitmap_top
    while rows and not any(rows[0]): rows.pop(0); top -= 1
    while rows and not any(rows[-1]): rows.pop()
    if rows:
        while all(r and r[0] == 0 for r in rows): rows = [r[1:] for r in rows]; left += 1
        while all(r and r[-1] == 0 for r in rows): rows = [r[:-1] for r in rows]
    w = len(rows[0]) if rows else 0
    return (g.advance.x + 32) >> 6, left, top, w, len(rows), rows

def pack(rows):
    px = [v for r in rows for v in r]
    if len(px) % 2: px.append(0)
    return bytes(px[i] | (px[i + 1] << 4) for i in range(0, len(px), 2))

def carr(name, data):
    lines = ["static const unsigned char %s[%d] = {" % (name, max(1, len(data)))]
    for i in range(0, len(data), 24):
        lines.append("\t" + ",".join(str(b) for b in data[i:i + 24]) + ",")
    if not data: lines.append("\t0")
    lines.append("};")
    return "\n".join(lines)

def emit_face(out, name, face, chars, flags):
    data = b""
    glyphs = []
    for ch in chars:
        adv, left, top, w, h, rows = render(face, ch, flags)
        glyphs.append((adv, left, top, w, h, len(data)))
        data += pack(rows)
    m = face.size
    asc = (m.ascender + 32) >> 6
    desc = -((m.descender - 32) >> 6) if m.descender < 0 else 0
    height = (m.height + 32) >> 6
    out.append(carr("k_%s_data" % name, data))
    out.append("static const PmGlyph k_%s_glyphs[%d] = {" % (name, len(glyphs)))
    for (adv, left, top, w, h, off) in glyphs:
        out.append("\t{ %d, %d, %d, %d, %d, %d }," % (adv, left, top, w, h, off))
    out.append("};")
    return asc, desc, height, len(data)

def main():
    out = ['/* pmfonts.cpp - PsiMail\'s fonts and icons. Made by mail/tools/mkfonts.py: do not edit.',
           ' * Inter (SIL Open Font License), DejaVu Sans Mono (Bitstream Vera licence),',
           ' * Lucide icons (ISC licence): see mail/fonts/. */',
           '#include "pmgfx.h"', '#include "pmfonts.h"', ""]
    chars = cp1252_chars()
    defs = []
    total = 0
    for name, fn, px in FACES:
        face = freetype.Face(os.path.join(FD, fn))
        face.set_pixel_sizes(0, px)
        asc, desc, height, n = emit_face(out, name, face, chars, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT)
        total += n
        defs.append((name, asc, desc, height, 32, len(chars)))
    # icons: Lucide codepoints, in ICONS order
    import json
    cps = json.load(open(os.path.join(FD, "lucide-codepoints.json")))
    face = freetype.Face(os.path.join(FD, "lucide.ttf"))
    for px in ICON_SIZES:
        face.set_pixel_sizes(0, px)
        name = "ICON%d" % px
        asc, desc, height, n = emit_face(out, name, face, [chr(cps[i]) for i in ICONS],
                                         freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT)
        total += n
        defs.append((name, asc, desc, height, 0, len(ICONS)))
    out.append("")
    for name, asc, desc, height, first, count in defs:
        out.append("const PmFont KFont%s = { %d, %d, %d, %d, %d, k_%s_glyphs, k_%s_data };" %
                   (name, asc, desc, height, first, count, name, name))
    out.append("/* %d bytes of glyphs */" % total)
    print("\n".join(out))
    # the header: font names and icon numbers
    h = ["/* pmfonts.h - made by mail/tools/mkfonts.py */", "#ifndef PMFONTS_H", "#define PMFONTS_H",
         '#include "pmgfx.h"']
    for name, *_ in defs:
        h.append("extern const PmFont KFont%s;" % name)
    h.append("enum TPmIcon {")
    for i, n in enumerate(ICONS):
        h.append("\tEIcon%s = %d," % ("".join(p.capitalize() for p in n.replace("-", " ").split()), i))
    h.append("\tEIconCount };")
    h.append("#endif")
    open(os.path.join(HERE, "..", "ui", "pmfonts.h"), "w").write("\n".join(h) + "\n")

main()
