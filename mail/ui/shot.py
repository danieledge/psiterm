#!/usr/bin/env python3
"""shot.py IN.pgm OUT.png [scale] - a PsiMail screenshot, enlarged, with an
LCD-ish tint (the 5mx's greys are greenish-grey)"""
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
s = int(sys.argv[3]) if len(sys.argv) > 3 else 2
im = im.resize((im.width * s, im.height * s), Image.NEAREST)
if "--plain" not in sys.argv:
    lo, hi = (38, 44, 36), (196, 204, 186)
    im = Image.merge("RGB", [im.point(lambda v, a=a, b=b: a + (b - a) * v // 255) for a, b in zip(lo, hi)])
im.save(sys.argv[2])
