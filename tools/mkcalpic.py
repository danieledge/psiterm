#!/usr/bin/env python3
"""mkcalpic.py [OUT.h] - the test picture for PsiTerm's "Which looks best?"
calibration screen (app/ptgrey.cpp): a small scene, drawn here so it needs no
photo and no licence, with what makes greys hard on the 5mx's screen:

 - a sky that runs smoothly from mid grey to near white, with soft clouds in
   the 220-250 range (do they vanish into the white?);
 - a sun disc and its glow;
 - hills in mid greys;
 - a shaded ball: highlight to deep shadow on one surface (is it round?);
 - a dark tree and a doorway, with detail in the 10-45 range (are they
   black blobs?);
 - a white house wall with a shadow side.

Writes a C header: PtCalPicW, PtCalPicH and the 8-bit greys row by row
(0 black .. 255 white). Default OUT: app/ptcalpic.h."""
import math, os, sys

W, H = 150, 110


def clamp(v):
    return max(0, min(255, int(round(v))))


def scene(x, y):
    # sky: mid grey at the top to near white at the horizon
    horizon = 62
    t = y / horizon
    v = 150 + 95 * t
    # soft clouds, near white
    for cx, cy, rx, ry in ((32, 14, 20, 6), (48, 18, 14, 5), (104, 10, 18, 5)):
        d = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
        if d < 1:
            v = max(v, 222 + 28 * (1 - d))
    # the sun and its glow
    d = math.hypot(x - 124, y - 22)
    if d < 7:
        return 255
    if d < 22:
        v = max(v, 255 - (d - 7) * 2.2)
    # far hills, then the near hill with light from the right
    far = 52 + 6 * math.sin(x / 11.0) + 4 * math.sin(x / 5.0 + 1)
    if y > far:
        v = 120 + 20 * math.sin(x / 7.0) - (y - far) * 0.6
    near = 70 + 10 * math.sin(x / 19.0 + 2)
    if y > near:
        v = 92 - (y - near) * 0.9 + 18 * (x / W)
    # the house: a white wall, its shadow side, a roof and a dark doorway
    if 70 <= x < 100 and 60 <= y < 92:
        v = 236 if x < 90 else 132
        if 78 <= x < 86 and 74 <= y < 92:
            v = 18 + (y - 74) * 1.4          # a doorway: 18..43, not black
        if 92 <= x < 97 and 68 <= y < 74:
            v = 70                           # a window in the shadow side
    if 66 <= x < 104 and 48 <= y < 60 and abs(x - 85) < (y - 46) * 1.6:
        v = 60 + (y - 48) * 2                # the roof
    # the tree: dark, but with a trunk and leaves that differ
    d = math.hypot((x - 20) / 1.0, (y - 58) / 1.2)
    if d < 16:
        v = 22 + 18 * (0.5 + 0.5 * math.sin(x * 0.9 + y * 0.7)) * (1 - d / 16)
    if 18 <= x < 23 and 70 <= y < 92:
        v = 38
    # the ball: lit from the top right, highlight to deep shadow
    bx, by, br = 122, 88, 17
    d = math.hypot(x - bx, y - by)
    if d < br:
        nx, ny = (x - bx) / br, (y - by) / br
        nz = math.sqrt(max(0.0, 1 - nx * nx - ny * ny))
        lx, ly, lz = 0.55, -0.55, 0.63
        lam = max(0.0, nx * lx + ny * ly + nz * lz)
        spec = max(0.0, lam) ** 24
        v = 14 + 200 * lam + 45 * spec
    # the ball's shadow on the ground
    if math.hypot((x - 128) / 1.8, (y - 105) / 0.5) < 10 and d >= br:
        v = v * 0.45
    return clamp(v)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'app', 'ptcalpic.h')
    px = [scene(x, y) for y in range(H) for x in range(W)]
    with open(out, 'w') as f:
        f.write('// ptcalpic.h - made by tools/mkcalpic.py: do not edit\n')
        f.write('// The test picture for the calibration screen (app/ptgrey.cpp)\n')
        f.write('const TInt PtCalPicW = %d;\nconst TInt PtCalPicH = %d;\n' % (W, H))
        f.write('static const TUint8 PtCalPic[%d] =\n\t{\n' % (W * H))
        for i in range(0, len(px), 30):
            f.write('\t' + ','.join(str(p) for p in px[i:i + 30]) + ',\n')
        f.write('\t};\n')
    if '--png' in sys.argv:
        from PIL import Image
        im = Image.new('L', (W, H))
        im.putdata(px)
        im.resize((W * 3, H * 3), Image.NEAREST).save(out.replace('.h', '.png'))


if __name__ == '__main__':
    main()
