#!/usr/bin/env python3
"""PsiWeb icon: a globe (meridians and parallels) in 4 greys, at the three
sizes the Psion 5mx uses, written to PsiWeb.aif with PsiTerm's AIF writer."""
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
    b = [R(3), R(3), n - 1 - R(3), n - 1 - R(3)]
    t = max(1, R(2.5))
    d.ellipse(b, fill=L, outline=K)
    m.ellipse(b, fill=0)
    for k in range(1, t):
        d.ellipse([b[0] + k, b[1] + k, b[2] - k, b[3] - k], outline=K)
    cx, cy = n // 2, n // 2
    rw = (b[2] - b[0]) // 2
    for f in (0.45,):                               # meridian ellipse
        w = int(rw * f)
        d.ellipse([cx - w, b[1], cx + w, b[3]], outline=D)
    d.line([(cx, b[1]), (cx, b[3])], fill=K, width=max(1, t - 1))
    d.line([(b[0], cy), (b[2], cy)], fill=K, width=max(1, t - 1))
    for dy in (-0.5, 0.5):                          # parallels
        y = cy + int(rw * dy)
        half = int(rw * 0.866)
        d.line([(cx - half, y), (cx + half, y)], fill=D, width=max(1, t - 1))
    return im, mk

if __name__ == "__main__":
    icons = [icon(n) for n in (24, 32, 48)]
    out = sys.argv[1] if len(sys.argv) > 1 else "psiweb.aif"
    print(out, write_aif(out, 0x01000A7A, [(1, "PsiWeb"), (10, "PsiWeb")], icons), "bytes")
    if len(sys.argv) > 1:
        sys.exit(0)
    prev = Image.new("L", (24 + 32 + 48 + 20, 48), W)
    x = 0
    for im, _ in icons:
        prev.paste(im, (x, 0)); x += im.size[0] + 10
    prev.resize((prev.size[0] * 3, 144), Image.NEAREST).save("icon-preview.png")
