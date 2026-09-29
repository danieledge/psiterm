#!/usr/bin/env python3
"""PsiMail icon: an envelope in 4 greys, at the three sizes the Psion 5mx
uses, written to PsiMail.aif with PsiTerm's AIF writer."""
import os, sys
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../tools/icon"))
from aif import write_aif
K, D, L, W = 0, 85, 170, 255

def icon(n):
    """PsiMail's look: a dark rounded square with a white envelope, drawn
    4x larger and scaled down so the edges come out in the greys between."""
    S = 4
    N = n * S
    s = N / 48.0
    R = lambda v: int(round(v * s))
    big = Image.new("L", (N, N), W)
    d = ImageDraw.Draw(big)
    d.rounded_rectangle([R(2), R(2), N - 1 - R(2), N - 1 - R(2)], radius=R(11), fill=K)
    # the envelope
    x0, y0, x1, y1 = R(10), R(15), N - 1 - R(10), N - 1 - R(15)
    d.rounded_rectangle([x0, y0, x1, y1], radius=R(2.5), fill=W)
    cx, cy = N // 2, y0 + (y1 - y0) * 11 // 20
    d.line([(x0 + R(1), y0 + R(1)), (cx, cy), (x1 - R(1), y0 + R(1))], fill=D, width=max(S, R(2.5)))
    im = big.resize((n, n), Image.LANCZOS)
    # 4 greys, as the Psion shows icons
    im = im.point(lambda v: min((K, D, L, W), key=lambda g: abs(g - v)))
    mk = Image.new("L", (N, N), 255)
    ImageDraw.Draw(mk).rounded_rectangle([R(2), R(2), N - 1 - R(2), N - 1 - R(2)], radius=R(11), fill=0)
    mk = mk.resize((n, n), Image.LANCZOS).point(lambda v: 0 if v < 160 else 255).convert("1")
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
