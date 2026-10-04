#!/usr/bin/env python3
"""Writes a small local test page with JPEG (baseline and progressive), PNG
(with transparency) and GIF pictures for run_host.sh.
    mktestpage.py DIR
"""
import math
import os
import sys

from PIL import Image, ImageDraw


def photo(w, h):
    """Something photo-like: smooth gradients, a sun, hills and noise."""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            sky = (90 + y * 120 // h, 140 + y * 80 // h, 230 - y * 60 // h)
            hill = h * 0.65 + math.sin(x / w * 6.3) * h * 0.12
            if y > hill:
                g = 90 + int(40 * math.sin(x * 0.21) * math.cos(y * 0.17))
                sky = (40 + g // 3, g + 40, 30)
            px[x, y] = sky
    d = ImageDraw.Draw(im)
    d.ellipse((w * 0.7, h * 0.1, w * 0.85, h * 0.1 + w * 0.15), fill=(255, 220, 90))
    d.rectangle((w * 0.15, h * 0.45, w * 0.3, h * 0.75), fill=(170, 60, 50))
    d.polygon([(w * 0.13, h * 0.45), (w * 0.225, h * 0.3), (w * 0.32, h * 0.45)], fill=(90, 40, 30))
    return im


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    p = photo(400, 260)
    p.save(os.path.join(out, "photo.jpg"), quality=80)
    p.save(os.path.join(out, "photo-prog.jpg"), quality=80, progressive=True)
    p.resize((1600, 1040)).save(os.path.join(out, "big.jpg"), quality=75)

    logo = Image.new("RGBA", (160, 80), (0, 0, 0, 0))
    d = ImageDraw.Draw(logo)
    d.rounded_rectangle((2, 2, 157, 77), 14, fill=(30, 90, 200, 255), outline=(0, 0, 0, 255), width=3)
    d.text((22, 30), "PNG + alpha", fill=(255, 255, 255, 255))
    logo.save(os.path.join(out, "logo.png"))

    g = Image.new("P", (120, 60), 0)
    d = ImageDraw.Draw(g)
    g.putpalette([255, 255, 255, 200, 30, 30, 30, 160, 30, 0, 0, 0] + [0] * 756)
    d.rectangle((0, 0, 119, 59), fill=1, outline=3)
    d.ellipse((30, 10, 90, 50), fill=2)
    d.text((40, 24), "GIF", fill=0)
    g.save(os.path.join(out, "badge.gif"))

    with open(os.path.join(out, "images.html"), "w") as f:
        f.write("""<!DOCTYPE html>
<html><head><title>Pictures test</title></head>
<body>
<h1>Pictures in Links for PsiWeb</h1>
<p>A baseline JPEG (400&times;260), a PNG with transparency on a coloured
cell, and a GIF. Below them a progressive JPEG and a large JPEG scaled to
320 pixels wide with <code>width=</code>.</p>
<table border=1 cellpadding=4><tr>
<td><img src="photo.jpg" alt="photo"></td>
<td bgcolor="#ffcc66"><img src="logo.png" alt="logo"><br>
<img src="badge.gif" alt="badge"></td>
</tr></table>
<p>Progressive JPEG:</p>
<p><img src="photo-prog.jpg" alt="progressive"></p>
<p>Large JPEG (1600&times;1040) shown at 320 wide:</p>
<p><img src="big.jpg" width="320" alt="big"></p>
<p>The end.</p>
</body></html>
""")


if __name__ == "__main__":
    main()
