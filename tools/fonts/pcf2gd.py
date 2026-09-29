#!/usr/bin/env python3
"""Terminus (PCF, Unicode) -> EPOC R5 font definition (.gd) for fnttran.
Terminus covers all of code page 1252, so glyphs are copied unchanged."""
import gzip, io
from PIL import PcfFontFile

SIZES = [(12, 16780970), (14, 16780971), (16, 16780972), (18, 16780973)]

def fontbitmap(px, uid):
    f = PcfFontFile.PcfFontFile(io.BytesIO(gzip.open(f"ter-u{px}n_unicode.pcf.gz").read()),
                                charset_encoding="cp1252")
    (W, _), dst, _, _ = f.glyph[ord("M")]
    asc, H = -dst[1], dst[3] - dst[1]
    out = [f"FontBitmap TERMINUS{px}", f"Uid {uid}", f"MaxNormalCharWidth {W}",
           f"CellHeight {H}", f"Ascent {asc}", "CodeSection 32:255"]
    for b in range(32, 256):
        g = f.glyph[b] or f.glyph[ord("?")]
        if b == 0xA0: g = f.glyph[0x20]
        (_, _), d, _, im = g
        im = im.convert("L")
        cell = [["."] * W for _ in range(H)]
        for y in range(im.size[1]):
            for x in range(im.size[0]):
                if im.getpixel((x, y)):
                    cy, cx = asc + d[1] + y, d[0] + x
                    if 0 <= cy < H and 0 <= cx < W: cell[cy][cx] = "*"
        out.append(f"Char {b}")
        out += ["".join(r) for r in cell]
        out.append("EndChar")
    out += ["EndCodeSection", "EndFontBitmap", ""]
    print(f"TERMINUS{px}: {W}x{H} ascent {asc}")
    return "\n".join(out)

parts = [fontbitmap(px, uid) for px, uid in SIZES]
parts.append("Typeface PSITERMTERMINUS\nName \"Terminus\"\nFontBitmaps\n" +
             "\n".join(f"TERMINUS{px}" for px, _ in SIZES) +
             "\nEndFontBitmaps\nEndTypeface\n\nFontStoreFile\nCollectionUid 16780974\n"
             "KPixelAspectRatio 1000\nCopyrightInfo\n"
             "\"Terminus Font (c) 2019 Dimitar Toshkov Zhekov, SIL Open Font License 1.1\"\n"
             "EndCopyrightInfo\nTypefaces\nPSITERMTERMINUS\nEndTypefaces\nEndFontStoreFile\n")
open("psiterm.gd", "w").write("\n".join(parts))
