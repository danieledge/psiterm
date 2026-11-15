#!/usr/bin/env python3
"""colouricon.py - colour program icons for a Psi program's .aif, the same
file in every program's repository (it comes from psion-netbook's
psiicons/art/colouricon.py: keep them the same).

The icons are drawn in psion-netbook/psiicons/art/psiapps.py and kept here
as PNG files: colour/<name>24.png, 32, 48 (8 bits a pixel, the palette
index EPOC's own colour number) and colour/<name>24m.png... (the masks:
black is drawn). One .aif serves both machines: the netBook shows the
icons in colour; the Series 5mx shows them in its greys, each colour by
its brightness (seen in the 5mx emulator, psion-netbook/psiicons/README.md).

mkicon.py uses them when they are there:
    from colouricon import colour_icons
    icons = colour_icons("psimail") or [icon(n) for n in (24, 32, 48)]
and psion-core's aif.write_aif writes them: this module teaches its
bitmap_stream the 8-bit colour bitmaps (EColor256, uncompressed), which
it does not otherwise write."""
import os
import struct

from PIL import Image

import aif

HERE = os.path.dirname(os.path.abspath(__file__))
_grey_stream = aif.bitmap_stream


def bitmap_stream(img, bpp=2):
    """CFbsBitmap::ExternalizeL: an 8-bit palette image as EColor256 (the
    palette index as it is), anything else as before (greys)"""
    if img.mode != "P":
        return _grey_stream(img, bpp)
    w, h = img.size
    stride = (w + 3) & ~3                       # rows padded to 32 bits
    px = img.load()
    data = bytearray()
    for y in range(h):
        row = bytearray(stride)
        for x in range(w):
            row[x] = px[x, y]
        data += row
    hdr = struct.pack("<IIiiiiIIII", 40 + len(data), 40, w, h, 0, 0, 8, 1, 0, 0)
    return hdr + bytes(data)


aif.bitmap_stream = bitmap_stream


def colour_icons(name, folder=None):
    """[(icon, mask)] at 24, 32 and 48 from colour/<name>NN.png and
    <name>NNm.png beside this file, or None if any is missing"""
    folder = folder or os.path.join(HERE, "colour")
    out = []
    for n in (24, 32, 48):
        fi = os.path.join(folder, f"{name}{n}.png")
        fm = os.path.join(folder, f"{name}{n}m.png")
        if not (os.path.exists(fi) and os.path.exists(fm)):
            return None
        im = Image.open(fi)
        if im.mode != "P" or im.size != (n, n):
            return None
        out.append((im, Image.open(fm).convert("1")))
    return out
