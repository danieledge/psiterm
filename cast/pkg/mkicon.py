#!/usr/bin/env python3
"""PsiCast icon: a Psion's open screen with the waves of it going out, in
4 greys, at the three sizes the 5mx uses, written to PsiCast.aif with
PsiTerm's AIF writer (tools/icon/aif.py).

As the Psion's own icons: mostly white with black outlines and a grey
shadow, and a mask that is black wherever the icon is drawn. The Extras
bar shows only the top 37 rows of the 48-pixel icon (the name is written
over the rest), so the picture keeps to them."""
import os, sys
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../tools/icon"))
from aif import write_aif
K, D, L, W = 0, 85, 170, 255


def icon(n):
    im = Image.new("L", (n, n), W)
    mk = Image.new("1", (n, n), 1)
    d, m = ImageDraw.Draw(im), ImageDraw.Draw(mk)
    s = n / 48.0
    R = lambda v: int(round(v * s))
    # the screen: a landscape rectangle with a grey shadow, lines of "text"
    x0, y0, x1, y1 = R(2), R(6), R(30), R(26)
    sh = max(1, R(2))
    d.rectangle([x0 + sh, y0 + sh, x1 + sh, y1 + sh], fill=L)
    m.rectangle([x0 + sh, y0 + sh, x1 + sh, y1 + sh], fill=0)
    d.rectangle([x0, y0, x1, y1], fill=W, outline=K)
    m.rectangle([x0, y0, x1, y1], fill=0)
    if n >= 32:
        d.rectangle([x0 + 1, y0 + 1, x1 - 1, y1 - 1], outline=K)
    for i, w in enumerate((0.7, 0.5, 0.6)):
        y = y0 + R(5) + i * max(2, R(5))
        if y < y1 - R(2):
            d.line([(x0 + R(4), y), (x0 + R(4) + int((x1 - x0 - R(8)) * w), y)], fill=D, width=max(1, R(1.5)))
    # the hinge under it, as the 5mx's
    d.rectangle([x0 + R(4), y1 + R(2), x1 - R(4), y1 + R(4)], fill=K)
    m.rectangle([x0 + R(4), y1 + R(2), x1 - R(4), y1 + R(4)], fill=0)
    # the waves going out to the right
    cx, cy = x1 + R(1), (y0 + y1) // 2
    t = max(1, R(2))
    for k, r in enumerate((6, 11, 16)):
        rr = R(r)
        box = [cx - rr, cy - rr, cx + rr, cy + rr]
        d.arc(box, -45, 45, fill=K, width=t)
        m.arc(box, -45, 45, fill=0, width=t)
    return im, mk


if __name__ == "__main__":
    icons = [icon(n) for n in (24, 32, 48)]
    out = sys.argv[1] if len(sys.argv) > 1 else "psicast.aif"
    print(out, write_aif(out, 0x01000A92, [(1, "PsiCast"), (10, "PsiCast")], icons), "bytes")
    if len(sys.argv) > 1:
        sys.exit(0)
    prev = Image.new("L", (24 + 32 + 48 + 40, 58), 200)
    x = 10
    for im, mk in icons:
        bg = Image.new("L", im.size, 200)
        prev.paste(Image.composite(bg, im, mk.convert("L")), (x, 5)); x += im.size[0] + 10
    prev.resize((prev.size[0] * 4, prev.size[1] * 4), Image.NEAREST).save("icon-preview.png")
