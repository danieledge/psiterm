#!/usr/bin/env python3
"""PsiTerm's toolbar pictures (original pixel art, in the spirit of the
Psion's own programs) -> BMP files for bmconv, and app/pticons.h with
their ids. As mail/tools/mkicons.py and web/tools/mkicons.py.

    tools/mkicons.py OUTDIR HEADER

Each picture is 24x20 ASCII art: k black, d dark grey, l light grey,
w white, . transparent. Every picture gets a 1-bit mask right after it in
the MBM (black = drawn), so ids go picture, mask, picture, mask...
"""
import sys, os
from PIL import Image

GREY = {'k': 0, 'd': 85, 'l': 170, 'w': 255, '.': 255}

ICONS = [
# SSH to: a terminal screen with a prompt and the cursor
('ToolSsh', """
.kkkkkkkkkkkkkkkkkkkkkk.
.kwwwwwwwwwwwwwwwwwwwwk.
.kwwwwwwwwwwwwwwwwwwwwk.
.kwwkkwwwwwwwwwwwwwwwwk.
.kwwwkkwwwwwwwwwwwwwwwk.
.kwwwwkkwwwwwwwwwwwwwwk.
.kwwwkkwwwwwwwwwwwwwwwk.
.kwwkkwwwkkkkkwwwwwwwwk.
.kwwwwwwwwwwwwwwwwwwwwk.
.kwwwwwwwwwwwwwwwwwwwwk.
.kwwwwwwwwwwwwwwwwwwwwk.
.kwwwwwwwwwwwwwwwwwwwwk.
.kkkkkkkkkkkkkkkkkkkkkk.
.kddddddddddddddddddddk.
.kkkkkkkkkkkkkkkkkkkkkk.
.........kwwwwk.........
........kwwwwwwk........
......kkkkkkkkkkkk......
......kddddddddddk......
......kkkkkkkkkkkk......
"""),
# Disconnect: a plug pulled out of its socket
('ToolDisconnect', """
........................
........................
........................
.........kkkkkk.........
........kwwwwwwk....kkkk
........kwwwwwwk....kwwk
........kwwwwwwkkkk.kkwk
........kwwwwwwkwwk..kwk
........kwwwwwwkkkk.kkwk
kkkkkkkkkwwwwwwk....kwwk
kwwwwwwwwwwwwwwk....kwwk
kkkkkkkkkwwwwwwk....kwwk
........kwwwwwwkkkk.kkwk
........kwwwwwwkwwk..kwk
........kwwwwwwkkkk.kkwk
........kwwwwwwk....kwwk
........kwwwwwwk....kkkk
.........kkkkkk.........
........................
........................
"""),
# Snippets: a sheet with a corner turned down and a few lines of text
('ToolSnippets', """
....kkkkkkkkkkkkkkk.....
....kwwwwwwwwwwwwwkk....
....kwwwwwwwwwwwwwkwk...
....kwwwwwwwwwwwwwkwwk..
....kwwwwwwwwwwwwwkkkkk.
....kwwkkkkkkkwwwwwwwwk.
....kwwwwwwwwwwwwwwwwwk.
....kwwkkkkkkkkkkkwwwwk.
....kwwwwwwwwwwwwwwwwwk.
....kwwkkkkkkkkkwwwwwwk.
....kwwwwwwwwwwwwwwwwwk.
....kwwkkkkkkkkkkkkwwwk.
....kwwwwwwwwwwwwwwwwwk.
....kwwkkkkkkwwwwwwwwwk.
....kwwwwwwwwwwwwwwwwwk.
....kwwkkkkkkkkkkwwwwwk.
....kwwwwwwwwwwwwwwwwwk.
....kwwwwwwwwwwwwwwwwwk.
....kkkkkkkkkkkkkkkkkkk.
........................
"""),
# Keys: two key caps, Esc and a blank one
('ToolKeys', """
........................
..kkkkkkkkkkkkkk........
..kwwwwwwwwwwwwk........
..kkkkwwkkwwkkwk........
..kkwwwkwwwkwwwk........
..kkkkwwkwwkwwwk........
..kkwwwwwkwkwwwk........
..kkkkwkkwwwkkwkkkkkk...
..kwwwwwwwwwwwwkwwwwkk..
..kwwwwwwwwwwwwkwwwwkwk.
..kkkkkkkkkkkkkkwwwwkwk.
...kddddddddddddwwwwkwk.
...kkkkkkkkkkkkkwwwwkwk.
.........kwwwwwwwwwwkwk.
.........kwwwwwwwwwwkwk.
.........kwwwwwwwwwwkwk.
.........kkkkkkkkkkkkwk.
..........kdddddddddddk.
..........kkkkkkkkkkkkk.
........................
"""),
# Files: a folder, with Send (up) and Get (down) arrows beside it
('ToolFiles', """
....................k...
...................kkk..
..................kkkkk.
.................kkkkkkk
.kkkkk.............kkk..
kwwwwwk............kkk..
kwwwwwwkkkkkkkk....kkk..
kwwwwwwwwwwwwwk....kkk..
kwwwwwwwwwwwwwk.........
kkkkkkkkkkkkkkkk........
kwwwwwwwwwwwwwwk........
kwwwwwwwwwwwwwwk...kkk..
kwwwwwwwwwwwwwwk...kkk..
kwwwwwwwwwwwwwwk...kkk..
kwwwwwwwwwwwwwwk...kkk..
kwwwwwwwwwwwwwwk...kkk..
kwwwwwwwwwwwwwwk.kkkkkkk
kddddddddddddddk..kkkkk.
kkkkkkkkkkkkkkkk...kkk..
....................k...
"""),
]


def parse(art):
    rows = [r for r in art.strip('\n').split('\n')]
    w = max(len(r) for r in rows)
    rows = [r.ljust(w, '.') for r in rows]
    return rows, w, len(rows)


def main():
    out, header = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    files = []
    ids = []
    for name, art in ICONS:
        rows, w, h = parse(art)
        if (w, h) != (24, 20):
            raise SystemExit(f'{name}: {w}x{h}, toolbar pictures are 24x20')
        img = Image.new('L', (w, h), 255)
        mask = Image.new('1', (w, h), 1)
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                if c not in GREY:
                    raise SystemExit(f'{name}: bad pixel {c!r}')
                img.putpixel((x, y), GREY[c])
                if c != '.':
                    mask.putpixel((x, y), 0)
        fi = os.path.join(out, f'{name}.bmp')
        fm = os.path.join(out, f'{name}_m.bmp')
        img.convert('RGB').save(fi)
        mask.convert('RGB').save(fm)
        files += [fi, fm]
        ids.append(name)
    with open(header, 'w') as f:
        f.write('/* pticons.h - made by tools/mkicons.py: PsiTerm.mbm ids */\n')
        f.write('#ifndef PTICONS_H\n#define PTICONS_H\nenum TPtIconId\n\t{\n')
        for i, n in enumerate(ids):
            f.write(f'\tEMbm{n} = {2 * i}, EMbm{n}Mask = {2 * i + 1},\n')
        f.write(f'\tEMbmCount = {2 * len(ids)}\n\t}};\n#endif\n')
    with open(os.path.join(out, 'files.txt'), 'w') as f:
        for i, p in enumerate(files):
            f.write(('/2' if i % 2 == 0 else '/1') + os.path.basename(p) + '\n')


if __name__ == '__main__':
    main()
