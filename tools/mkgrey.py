#!/usr/bin/env python3
"""mkgrey.py - the grey calibration's standard table and its chart.

    tools/mkgrey.py                 print the standard table (ssh/psigrey.h)
    tools/mkgrey.py --chart OUT.png the calibration chart (docs/display-chart.png)
    tools/mkgrey.py --table GAMMA CURVE BRIGHT   a table for other settings

The formula is the one in ssh/psigrey.h (psigrey_compute), which PsiTerm's
Display calibration uses:
    x = k / 15;  s = x + c (x^2 (3 - 2x) - x)        (c = curve / 100)
    lv[k] = round(255 s^gamma) - bright + nudge[k], then made increasing
lv[k] is how light level k looks, as an 8-bit grey. See docs/display.md.

The chart (640 x 240, shown 2x) is for a PC screen or paper, to set beside
the Psion's own Display calibration screen:
  1. the 16 levels as they would look if the screen were linear (17k);
  2. the 16 levels as the standard table takes them to look on a 5mx;
  3. a smooth ramp from black to white;
  4. the ramp in 16 levels by error diffusion with the standard table,
     drawn as the table says the Psion shows them;
  5. the same with plain linear rounding (the old way), for comparison.
On the Psion, the calibration screen is right when its own ramp looks as
smooth and even as row 3 here.
"""
import sys
from PIL import Image, ImageDraw, ImageFont


def table(gamma=100, curve=35, bright=0, nudge=None):
    nudge = nudge or [0] * 16
    lv = []
    for k in range(16):
        x = k / 15.0
        c = curve / 100.0
        s = x + c * (x * x * (3.0 - 2.0 * x) - x)
        s = min(1.0, max(0.0, s))
        if gamma != 100 and s > 0:
            s = s ** (gamma / 100.0)
        v = int(255.0 * s + 0.5) - bright - nudge[k]
        lv.append(min(255, max(0, v)))
    for k in range(16):                     # psigrey_fix
        lv[k] = max(k, min(240 + k, lv[k]))
        if k and lv[k] <= lv[k - 1]:
            lv[k] = lv[k - 1] + 1
    return lv


def nearest(lv, v):
    best = 0
    for k in range(16):
        if abs(lv[k] - v) < abs(lv[best] - v):
            best = k
    return best


def fs_row_pixels(width, height, lv):
    """the ramp, Floyd-Steinberg serpentine to the levels (as pmimg.c)"""
    out = [[0] * width for _ in range(height)]
    err = [0.0] * (width + 2)
    for y in range(height):
        nxt = [0.0] * (width + 2)
        xs = range(width) if y % 2 == 0 else range(width - 1, -1, -1)
        step = 1 if y % 2 == 0 else -1
        fwd = 0.0
        for x in xs:
            v = x * 255 // (width - 1) + err[x + 1] + fwd
            v = min(255, max(0, v))
            q = nearest(lv, int(v))
            out[y][x] = q
            e = v - lv[q]
            fwd = e * 7 / 16
            nxt[x + 1 - step] += e * 3 / 16
            nxt[x + 1] += e * 5 / 16
            nxt[x + 1 + step] += e / 16
        err = nxt
    return out


def chart(path):
    lv = table()
    lin = [17 * k for k in range(16)]
    W, H = 640, 240
    im = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype("DejaVuSans.ttf", 10)
    except OSError:
        f = ImageFont.load_default()
    d.text((4, 2), "PsiGrey calibration chart - see docs/display.md", fill=0, font=f)
    rows = [("1 levels if linear (17k)", lin), ("2 standard table (5mx guess)", lv)]
    y = 16
    for label, vals in rows:
        d.text((4, y), label, fill=0, font=f)
        for k in range(16):
            d.rectangle([k * 40, y + 12, k * 40 + 39, y + 41], fill=vals[k])
            d.text((k * 40 + 3, y + 14), str(k), fill=255 if vals[k] < 128 else 0, font=f)
            d.text((k * 40 + 3, y + 28), str(vals[k]), fill=255 if vals[k] < 128 else 0, font=f)
        y += 46
    d.text((4, y), "3 smooth ramp", fill=0, font=f)
    for x in range(W):
        d.line([x, y + 12, x, y + 31], fill=x * 255 // (W - 1))
    y += 34
    for label, levels in (("4 ramp, error diffusion, standard table", lv), ("5 ramp, plain linear rounding (old)", lin)):
        d.text((4, y), label, fill=0, font=f)
        if levels is lv:
            px = fs_row_pixels(W, 20, lv)
            for yy in range(20):
                for x in range(W):
                    im.putpixel((x, y + 12 + yy), lv[px[yy][x]])
        else:
            for x in range(W):
                d.line([x, y + 12, x, y + 31], fill=17 * nearest(lin, x * 255 // (W - 1)))
        y += 34
    im.resize((W * 2, H * 2), Image.NEAREST).save(path)


args = sys.argv[1:]
if args[:1] == ["--chart"]:
    chart(args[1])
elif args[:1] == ["--table"]:
    print(table(int(args[1]), int(args[2]), int(args[3])))
else:
    print("{ " + ", ".join(str(v) for v in table()) + " }")
