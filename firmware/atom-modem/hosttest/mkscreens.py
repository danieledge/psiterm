#!/usr/bin/env python3
# mkscreens.py - turns render_screens' screens/*.bmp into PNGs three times
# the size (the LCD is under 1 inch across) and a contact sheet of them all,
# screens/sheet.png. MIT licence (see LICENSE at the top of the repository).
import glob
import os
import sys
from PIL import Image, ImageDraw

DIR = sys.argv[1] if len(sys.argv) > 1 else 'screens'
SCALE = 3
names = sorted(glob.glob(os.path.join(DIR, '*.bmp')))
tiles = []
for p in names:
    im = Image.open(p).convert('RGB')
    big = im.resize((im.width * SCALE, im.height * SCALE), Image.NEAREST)
    big.save(p[:-4] + '.png')
    os.remove(p)
    tiles.append((os.path.basename(p)[:-4], big))
if tiles:
    cols = 4
    tw, th = tiles[0][1].size
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * (tw + 16) + 16, rows * (th + 32) + 16), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for i, (n, t) in enumerate(tiles):
        x = 16 + (i % cols) * (tw + 16)
        y = 16 + (i // cols) * (th + 32)
        sheet.paste(t, (x, y))
        d.text((x, y + th + 4), n, fill=(220, 220, 220))
    sheet.save(os.path.join(DIR, 'sheet.png'))
print('%d screens in %s' % (len(tiles), DIR))
