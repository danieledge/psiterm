#!/usr/bin/env python3
# mkfontmetrics.py - writes ../src/fontmetrics.h: the widths and heights of
# the M5GFX fonts the status screen uses, so the portable layout code
# (status.cpp) can fit and centre text without M5GFX, and the host tests can
# check that nothing runs off the 128x128 screen. Run it again only if the
# fonts change. MIT licence (see LICENSE at the top of the repository).
#   python3 mkfontmetrics.py [path to M5GFX/src/lgfx/Fonts/GFXFF]
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GFXFF = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '.pio', 'libdeps', 'atoms3r', 'M5GFX', 'src',
                                                           'lgfx', 'Fonts', 'GFXFF')
# (the font id in status.h, the GFXFF name)
FONTS = [('EFaceSans', 'FreeSans24pt7b'), ('EFaceBold', 'FreeSansBold24pt7b'), ('EFaceMono', 'FreeMonoBold24pt7b')]


def glyphs(name):
    text = open(os.path.join(GFXFF, name + '.h')).read()
    body = text[text.index(name + 'Glyphs'):]
    body = body[:body.index('};')]
    rows = re.findall(r'\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}', body)
    return [tuple(int(v) for v in r) for r in rows]


out = ['// fontmetrics.h - made by hosttest/mkfontmetrics.py from M5GFX\'s GFXFF fonts',
       '// (FreeFont, GNU FreeFont licence; only the metrics are here). Do not edit.',
       '#ifndef ATOM_FONTMETRICS_H', '#define ATOM_FONTMETRICS_H', '', '#include <stdint.h>', '',
       'namespace am {', '',
       'struct FontMetric',
       '\t{',
       '\tuint8_t advance[95];     // 0x20..0x7e: the pen moves this far',
       '\tuint8_t ascent;          // the tallest glyph above the baseline',
       '\tuint8_t descent;         // the deepest below it',
       '\tuint8_t cap;             // the height of "H": what the eye reads as the size',
       '\t};', '']
for ident, name in FONTS:
    g = glyphs(name)
    assert len(g) == 95, (name, len(g))
    adv = [r[3] for r in g]
    ascent = max(-r[5] for r in g)
    descent = max(r[2] + r[5] for r in g)
    cap = -g[ord('H') - 0x20][5]
    out.append('// %s' % name)
    out.append('static const FontMetric k%sMetric = { {' % name)
    for i in range(0, 95, 19):
        out.append('\t' + ', '.join('%2d' % a for a in adv[i:i + 19]) + ',')
    out.append('\t}, %d, %d, %d };' % (ascent, descent, cap))
out += ['', '} // namespace am', '', '#endif', '']
dst = os.path.join(HERE, '..', 'src', 'fontmetrics.h')
open(dst, 'w').write('\n'.join(out))
print('wrote', os.path.normpath(dst))
