#!/usr/bin/env python3
"""PsiMail icon: an envelope in 4 greys, at the three sizes the Psion 5mx
uses, written to PsiMail.aif with PsiTerm's AIF writer."""
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
    t = max(1, R(2.5))
    x0, y0, x1, y1 = R(3), R(10), n - 1 - R(3), n - 1 - R(10)
    d.rectangle([x0, y0, x1, y1], fill=W, outline=K)
    m.rectangle([x0, y0, x1, y1], fill=0)
    for k in range(1, t):
        d.rectangle([x0 + k, y0 + k, x1 - k, y1 - k], outline=K)
    cx, cy = n // 2, y0 + (y1 - y0) * 3 // 5
    d.polygon([(x0 + t, y0 + t), (x1 - t, y0 + t), (cx, cy)], fill=L)
    d.line([(x0, y0), (cx, cy), (x1, y0)], fill=K, width=t)
    d.line([(x0, y1), (cx - R(6), cy - R(4))], fill=D, width=max(1, t - 1))
    d.line([(x1, y1), (cx + R(6), cy - R(4))], fill=D, width=max(1, t - 1))
    return im, mk

if __name__ == "__main__":
    icons = [icon(n) for n in (24, 32, 48)]
    out = sys.argv[1] if len(sys.argv) > 1 else "psimail.aif"
    print(out, write_aif(out, 0x01000A7C, [(1, "PsiMail"), (10, "PsiMail")], icons), "bytes")
    if len(sys.argv) > 1:
        sys.exit(0)
    prev = Image.new("L", (24 + 32 + 48 + 20, 48), W)
    x = 0
    for im, _ in icons:
        prev.paste(im, (x, 0)); x += im.size[0] + 10
    prev.resize((prev.size[0] * 3, 144), Image.NEAREST).save("icon-preview.png")
