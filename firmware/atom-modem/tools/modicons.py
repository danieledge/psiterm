#!/usr/bin/env python3
"""modicons.py: build PsiCast's "Modern" icon pack, our own full-colour
versions of the 5mx's pictures.

The viewer's Modern theme paints these over the 5mx's grey pictures on
screen, exactly as the Series 7 theme paints s7icons.json's
(src/cast/iconmatch.js, unchanged). The colour side (a, c) and the palette
are drawn here, from the per-picture designs in tools/modicons/specs.py.
Pictures with no design are left out (the viewer keeps them grey).

The 5mx side of each picture (its name, size, greys and mask: n, w, h, g,
m, encoded as s7icons.py does) comes from s7icons.json, or, for pictures
that are not in it (our own programs', and a few ROM ones the Series 7
lacks), from the spec's sides file (specs.py -> specs.sides.json, kept in
the repository so neither the ROM nor a build is needed). --sides rewrites
that file from the spec's SOURCES: built .aif/.mbm files (paths from the
top of the repository) and, with --rom, files in a 5mx ROM, read in place.
--check says whether the sides still match the built files that exist.

    python3 tools/modicons.py                       # -> src/cast/modicons.json
    python3 tools/modicons.py --sheet sheet.png     # also a contact sheet
    python3 tools/modicons.py --inspect NAME out.png  # a 5mx picture, 10x, with
                                                    # a grid and its regions
    python3 tools/modicons.py --only NAME ...       # build just these
    python3 tools/modicons.py --sides [--rom 5MX.bin]
    python3 tools/modicons.py --check
    python3 tools/modicons.py --spec FILE.py --out PACK.json
                              # another set of designs (its own sides file,
                              # its own pack; the standard materials still apply)

In an .mbm made by our tools each picture is followed by its 1-bit mask
(picture, mask, picture, mask...), so a 2-bit bitmap with a 1-bit one of
its size straight after is a picture drawn through that mask; an .aif's
icons carry their masks.

How a design is drawn (see the header of tools/modicons/specs.py)
- The 5mx picture is read as levels: its 16-grey value, with a checkered
  dither (a pixel whose four sides all differ from it and whose corners
  match it) flattened to the mean of its two greys, so dithers become
  flat tints (12.5, 7.5, 2.5).
- Solid black (level 0) is ink, everything else is fill. The design puts
  materials (named colour ramps, light to dark) into regions: a seed floods
  the 4-connected pixels of its own kind (fill or ink), optionally only of
  some levels, inside an optional box, and the first seed to reach a pixel
  wins. Boxes and pixel lists do the rest. Unreached fill takes the
  design's base material (paper, as standard).
- Each pixel's colour is its material's ramp at an index chosen by its
  level (light to dark, the 5mx's own light from the top left), moved by
  the material's shift. Ink not given a material is line: an outline
  pixel (next to a pixel the 5mx does not draw, or to the shadow) in deep
  slate, an inside line in the darkest step of the material beside it.
- The material "none" is not painted: the viewer draws the 5mx's grey
  there (used for drop shadows, so they sit on whatever background the
  theme gives the screen).
- Pixel overrides (explicit colours, by name or #rrggbb) come last.
"""
import base64, gzip, json, os, re, sys
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(ROOT))
S7 = os.path.join(ROOT, 'src', 'cast', 's7icons.json')
OUT = os.path.join(ROOT, 'src', 'cast', 'modicons.json')
SPEC = os.path.join(HERE, 'modicons', 'specs.py')
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(HERE, 'modicons'))
sys.path.insert(0, HERE)
import specs  # noqa: E402

def load_spec(path):
    import importlib.util
    if os.path.abspath(path) == os.path.abspath(SPEC):
        return specs
    sp = importlib.util.spec_from_file_location('modicons_extra_spec', path)
    mod = importlib.util.module_from_spec(sp); sp.loader.exec_module(mod)
    return mod

# ---- colour ----
def hexrgb(s):
    s = s.lstrip('#')
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))

def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))

WHITE, SLATE = (255, 255, 255), hexrgb('#2b3445')

def ramp(base):
    """seven steps, lightest first; step 3 is the base colour"""
    b = hexrgb(base)
    return [mix(b, WHITE, .72), mix(b, WHITE, .48), mix(b, WHITE, .24), b,
            mix(b, SLATE, .22), mix(b, SLATE, .45), mix(b, SLATE, .7)]

def colour(c):
    if isinstance(c, tuple):
        return c
    if c.startswith('#'):
        return hexrgb(c)
    m = re.match(r'([a-z]+)(\d)$', c)            # "teal2": step 2 of teal's ramp
    if m:
        return MATERIALS[m.group(1)]['ramp'][int(m.group(2))]
    return hexrgb(specs.NAMED[c])

MATERIALS = {}
def add_materials(mats):
    for name, d in mats.items():
        r = [hexrgb(x) for x in d['ramp']] if 'ramp' in d else ramp(d['base'])
        MATERIALS[name] = dict(d, ramp=r)
add_materials(specs.MATERIALS)

# level (0..15, halves for dithers) -> ramp step, before a material's shift
LEVEL_STEP = {15: 1, 12.5: 2, 10: 3, 7.5: 4, 5: 4, 2.5: 5, 0: 6}

# ---- the 5mx side ----
def unpack(e):
    w, h = e['w'], e['h']; n = w * h
    g = base64.b64decode(e['g']); m = base64.b64decode(e['m'])
    G = [(g[i >> 1] >> ((i & 1) * 4)) & 15 for i in range(n)]
    M = [(m[i >> 3] >> (i & 7)) & 1 for i in range(n)]
    return G, M

def levels(G, M, w, h):
    """greys with checkered dithers flattened"""
    L = [float(v) for v in G]
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if not M[i]:
                continue
            g = G[i]
            sides = [G[j] for j in nb4(x, y, w, h) if M[j]]
            corners = [G[(y + dy) * w + x + dx] for dx, dy in ((-1, -1), (1, -1), (-1, 1), (1, 1))
                       if 0 <= x + dx < w and 0 <= y + dy < h and M[(y + dy) * w + x + dx]]
            # (all four sides, so a diagonal line along the mask's edge is
            # not taken for a dither)
            if len(sides) < 4 or any(s == g for s in sides):
                continue
            if len(corners) < 3 or sum(1 for c in corners if c == g) < 3:
                continue
            others = sorted(set(sides), key=sides.count)
            v = others[-1]
            L[i] = (g + v) / 2
    return L

def nb4(x, y, w, h):
    if x > 0: yield y * w + x - 1
    if x < w - 1: yield y * w + x + 1
    if y > 0: yield (y - 1) * w + x
    if y < h - 1: yield (y + 1) * w + x

def nearest_level(v):
    return min(LEVEL_STEP, key=lambda k: abs(k - v))

# ---- drawing a design ----
def draw(e, spec):
    w, h = e['w'], e['h']; n = w * h
    G, M = unpack(e)
    L = levels(G, M, w, h)
    ink = [M[i] and L[i] <= 1 for i in range(n)]
    mat = [None] * n
    # drop shadows: unclaimed fill outside the outline (a region that meets
    # the picture's edge or its undrawn pixels) holding only light greys
    if spec.get('shadow', True):
        seen = [False] * n
        for i0 in range(n):
            if seen[i0] or not M[i0] or ink[i0]:
                continue
            comp, q, edge, light = [], deque([i0]), 0, True
            seen[i0] = True
            while q:
                i = q.popleft(); comp.append(i); x, y = i % w, i // w
                out = x in (0, w - 1) or y in (0, h - 1)
                if nearest_level(L[i]) not in spec.get('shadowlevels', (7.5, 10, 12.5, 15)):
                    light = False
                for j in nb4(x, y, w, h):
                    if not M[j]:
                        out = True
                    elif not seen[j] and not ink[j]:
                        seen[j] = True; q.append(j)
                edge += out
            # (along a good part of it: a fill that meets the outside only
            # through a gap at a corner of an 8-connected outline is not)
            if edge >= max(3, len(comp) // 10) and light:
                for i in comp:
                    mat[i] = 'none'
    for op in spec.get('regions', []):
        kind = op[0]
        if kind == 'seed':                       # ('seed', x, y, material, {levels, box})
            _, sx, sy, mname = op[:4]; o = op[4] if len(op) > 4 else {}
            lv = o.get('levels'); box = o.get('box')
            i0 = sy * w + sx
            if not M[i0]:
                raise SystemExit('%s: seed %d,%d is off the picture' % (e['n'], sx, sy))
            want = ink[i0]
            free = (None, 'none') if o.get('force') else (None,)
            if mat[i0] not in free:
                continue
            q = deque([i0]); mat[i0] = mname
            while q:
                i = q.popleft(); x, y = i % w, i // w
                for j in nb4(x, y, w, h):
                    if mat[j] not in free or not M[j] or ink[j] != want:
                        continue
                    if lv and nearest_level(L[j]) not in lv:
                        continue
                    if box and not (box[0] <= j % w <= box[2] and box[1] <= j // w <= box[3]):
                        continue
                    mat[j] = mname; q.append(j)
        elif kind == 'box':                      # ('box', x0, y0, x1, y1, material, {ink, levels, force})
            _, x0, y0, x1, y1, mname = op[:6]; o = op[6] if len(op) > 6 else {}
            for y in range(max(0, y0), min(h, y1 + 1)):
                for x in range(max(0, x0), min(w, x1 + 1)):
                    i = y * w + x
                    if not M[i] or (mat[i] not in (None, 'none') and not o.get('force')) or (mat[i] == 'none' and not o.get('force')):
                        continue
                    if ink[i] and not o.get('ink'):
                        continue
                    if o.get('levels') and nearest_level(L[i]) not in o['levels']:
                        continue
                    mat[i] = mname
        else:
            raise SystemExit('%s: unknown region op %r' % (e['n'], kind))
    base = spec.get('base', 'paper')
    for i in range(n):
        if M[i] and not ink[i] and mat[i] is None:
            mat[i] = base
    rgb = [None] * n
    for i in range(n):
        if not M[i]:
            continue
        mn = mat[i]
        if mn == 'none':
            continue
        if mn is not None:
            md = MATERIALS[mn]
            lvl = nearest_level(L[i])
            step = md.get('levels', {}).get(lvl, LEVEL_STEP[lvl] + md.get('shift', 0))
            rgb[i] = md['ramp'][max(0, min(6, step))]
            continue
        # unclaimed ink: outline or inside line
        x, y = i % w, i // w
        edge = False; near = {}
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if not dx and not dy:
                    continue
                xx, yy = x + dx, y + dy
                if not (0 <= xx < w and 0 <= yy < h):
                    if dx == 0 or dy == 0: edge = True
                    continue
                j = yy * w + xx
                if (dx == 0 or dy == 0) and (not M[j] or mat[j] == 'none'):
                    edge = True
                if M[j] and mat[j] not in (None, 'none'):
                    near[mat[j]] = near.get(mat[j], 0) + 1
        if edge or not near:
            rgb[i] = SLATE
        else:
            mn = max(near, key=near.get)
            line = MATERIALS[mn].get('line')
            rgb[i] = colour(line) if line else MATERIALS[mn]['ramp'][6]
    # 'outline': claimed pixels on the silhouette (next to an undrawn or a
    # shadow pixel) are drawn as the slate outline
    if spec.get('outline'):
        for i in range(n):
            if rgb[i] is None or mat[i] in (None, 'none'):
                continue
            x, y = i % w, i // w
            if x in (0, w - 1) or y in (0, h - 1) or any(not M[j] or mat[j] == 'none' for j in nb4(x, y, w, h)):
                if ink[i] or spec.get('outline') == 'all':
                    rgb[i] = SLATE
    for (x, y, c) in spec.get('px', []):
        rgb[y * w + x] = None if c is None else colour(c)
    for row0, rows in spec.get('art', []):        # ascii art rows: chars from spec['key']
        key = spec['key']
        for dy, line in enumerate(rows):
            for x, ch in enumerate(line):
                if ch in key:
                    k = key[ch]
                    rgb[(row0 + dy) * w + x] = None if k is None else colour(k)
    return G, M, rgb

# ---- the pack ----
def b64(b):
    return base64.b64encode(bytes(b)).decode()

def pack_bits(bits):
    out = bytearray((len(bits) + 7) >> 3)
    for i, v in enumerate(bits):
        if v: out[i >> 3] |= 1 << (i & 7)
    return out

def pack_nibbles(vals):
    out = bytearray((len(vals) + 1) >> 1)
    for i, v in enumerate(vals):
        out[i >> 1] |= (v & 15) << ((i & 1) * 4)
    return out

def side(name, grey, mask_on):
    """a 5mx side as s7icons.py encodes it (greys 0 where it does not draw)"""
    g = [grey.grey16(x, y) for y in range(grey.h) for x in range(grey.w)]
    return {'n': name, 'w': grey.w, 'h': grey.h,
            'g': b64(pack_nibbles([v if m else 0 for v, m in zip(g, mask_on)])),
            'm': b64(pack_bits(mask_on))}

def decode(path, data):
    """the 5mx sides of one .aif or .mbm (path: its installed name)"""
    import s7icons as s7
    out = []
    if path.lower().endswith('.aif'):
        for g, gm in s7.aif_icons(data):
            out.append(side('%s#%dx%d' % (path, g.w, g.h), g, s7.opaque_from_mask(gm)))
    else:
        bm = s7.mbm_bitmaps(data)
        for k, g in enumerate(bm):
            if g is None or g.colour or g.bpp == 1:
                continue
            mk = bm[k + 1] if k + 1 < len(bm) else None
            if mk is not None and mk.bpp == 1 and (mk.w, mk.h) == (g.w, g.h):
                out.append(side('%s#%d' % (path, k), g, s7.opaque_from_mask(mk)))
            else:
                out.append(side('%s#%d' % (path, k), g, [1] * (g.w * g.h)))
    return out

def sides_path(spec_path):
    return re.sub(r'\.py$', '', spec_path) + '.sides.json'

def read_source(src, rom):
    """a SOURCES value: a file from the top of the repository, or
    'rom:Z:\\...' (a file in the 5mx ROM given with --rom)"""
    if src.startswith('rom:'):
        if not rom:
            return None
        import s7icons as s7
        f = rom.get(src[4:].lower())
        return f[1] if f else None
    p = os.path.join(REPO, src)
    return open(p, 'rb').read() if os.path.exists(p) else None

def make_sides(spec, spec_path, rom_path=None, check=False):
    path = sides_path(spec_path)
    old = json.load(open(path)) if os.path.exists(path) else {}
    rom = None
    if rom_path:
        import s7icons as s7
        rom = s7.rom_files(open(rom_path, 'rb').read())
    new, bad, seen = dict(old), [], 0
    for inst, src in getattr(spec, 'SOURCES', {}).items():
        data = read_source(src, rom)
        if data is None:
            continue
        for e in decode(inst, data):
            seen += 1
            if check and e['n'] in old and old[e['n']] != e:
                bad.append(e['n'])
            if check and e['n'] not in old and e['n'] in spec.ICONS:
                bad.append(e['n'] + ' (not in the sides file)')
            new[e['n']] = e
    if check:
        for n in bad:
            print('modicons --check: %s: the built picture has changed; run --sides' % n)
        print('modicons --check: %d pictures in the built files, %d changed' % (seen, len(bad)))
        return not bad
    with open(path, 'w') as f:
        json.dump(dict(sorted(new.items())), f, indent=0, separators=(',', ':')); f.write('\n')
    print('%d sides -> %s' % (len(new), path))
    return True

def sides_for(spec_path):
    by = {e['n']: e for e in json.load(open(S7))['icons']}
    p = sides_path(spec_path)
    if os.path.exists(p):
        by.update(json.load(open(p)))
    return by

def unfindable(pack):
    """the pictures src/cast/iconmatch.js cannot index (so never finds): it
    is the judge, so ask it (with node; without node, none are left out)"""
    import subprocess
    js = ("const IM=require(process.argv[1]);let d='';process.stdin.on('data',c=>d+=c).on('end',()=>{"
          "const p=JSON.parse(d),m=IM.create(p),got=new Set(m.icons.map(i=>i.name));"
          "console.log(JSON.stringify(p.icons.filter(e=>!got.has(e.n)).map(e=>e.n)))})")
    try:
        r = subprocess.run(['node', '-e', js, os.path.join(ROOT, 'src', 'cast', 'iconmatch.js')],
                           input=json.dumps(pack), capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        print('(no node to ask iconmatch.js which pictures it can find: %s)' % e)
        return set()
    return set(json.loads(r.stdout))

def build(spec, spec_path, only=None):
    by = sides_for(spec_path)
    pal, idx, out, drawn = [], {}, [], []
    for name, design in spec.ICONS.items():
        if only and name not in only:
            continue
        e = by.get(name)
        if not e:
            raise SystemExit('no picture %s in s7icons.json or %s (run --sides)' % (name, sides_path(spec_path)))
        G, M, rgb = draw(e, design)
        a = [1 if c is not None else 0 for c in rgb]
        cs = []
        for c in rgb:
            if c is None: continue
            if c not in idx:
                idx[c] = len(pal); pal.append(c)
            cs.append(idx[c])
        out.append({'n': e['n'], 'w': e['w'], 'h': e['h'], 'g': e['g'], 'm': e['m'],
                    'a': b64(pack_bits(a)), 'c': b64(cs)})
        drawn.append((e, G, M, rgb))
    if len(pal) > 256:
        raise SystemExit('palette has %d colours (256 at most)' % len(pal))
    flat = [v for c in pal for v in c] + [0] * (3 * (256 - len(pal)))
    pack = {'format': 'psicast-iconpack', 'version': 2, 'palette': b64(flat), 'icons': out}
    lost = unfindable(pack)
    if lost:
        for n in sorted(lost):
            print('left out (iconmatch.js cannot index it): %s' % n)
        pack['icons'] = [e for e in out if e['n'] not in lost]
    return pack, drawn, len(pal)

# ---- looking at it ----
def sheet(drawn, path, scale=4):
    from PIL import Image, ImageDraw
    bgs = [(255, 255, 255), hexrgb('#e3e8ef')]
    cols = 4
    cw = max(e['w'] for e, *_ in drawn) * scale * 3 + 4 * scale + 16
    rh = max(e['h'] for e, *_ in drawn) * scale + 14
    rows = (len(drawn) + cols - 1) // cols
    im = Image.new('RGB', (cols * cw, rows * rh), (255, 255, 255))
    d = ImageDraw.Draw(im)
    for k, (e, G, M, rgb) in enumerate(drawn):
        w, h = e['w'], e['h']; ox, oy = (k % cols) * cw, (k // cols) * rh
        d.text((ox + 2, oy), e['n'].split('/')[-1], fill=(200, 0, 0))
        for p, (bg, mode) in enumerate(((bgs[0], 'grey'), (bgs[0], 'col'), (bgs[1], 'col'))):
            px0 = ox + p * (w * scale + 2 * scale)
            d.rectangle([px0, oy + 12, px0 + w * scale - 1, oy + 12 + h * scale - 1], fill=bg)
            for y in range(h):
                for x in range(w):
                    i = y * w + x
                    if not M[i]: continue
                    if mode == 'grey' or rgb[i] is None:
                        v = round(G[i] * 255 / 15); c = (v, v, v)
                        if mode == 'col': c = mix(c, bg, .0)
                    else:
                        c = rgb[i]
                    d.rectangle([px0 + x * scale, oy + 12 + y * scale,
                                 px0 + x * scale + scale - 1, oy + 12 + y * scale + scale - 1], fill=c)
    im.save(path)

def inspect(name, path, spec_path, scale=10):
    """the 5mx picture large, with a grid every pixel (darker every 4,
    labelled every 8) and its levels, to place seeds by"""
    from PIL import Image, ImageDraw
    e = sides_for(spec_path)[name]
    w, h = e['w'], e['h']
    G, M = unpack(e); L = levels(G, M, w, h)
    m = 22
    im = Image.new('RGB', (w * scale + m, h * scale + m), (255, 255, 255))
    d = ImageDraw.Draw(im)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if M[i]:
                v = round(L[i] * 255 / 15); c = (v, v, v)
                if L[i] != G[i]: c = mix(c, (255, 120, 0), .35)   # flattened dither: tinted
            else:
                c = (190, 225, 255)
            d.rectangle([m + x * scale, m + y * scale, m + x * scale + scale - 1, m + y * scale + scale - 1], fill=c)
    for x in range(w + 1):
        col = (255, 0, 0) if x % 8 == 0 else (120, 160, 255) if x % 4 == 0 else (215, 215, 230)
        d.line([m + x * scale, m, m + x * scale, m + h * scale], fill=col)
        if x % 4 == 0 and x < w: d.text((m + x * scale + 1, 2), str(x), fill=(200, 0, 0))
    for y in range(h + 1):
        col = (255, 0, 0) if y % 8 == 0 else (120, 160, 255) if y % 4 == 0 else (215, 215, 230)
        d.line([m, m + y * scale, m + w * scale, m + y * scale], fill=col)
        if y % 4 == 0 and y < h: d.text((1, m + y * scale + 1), str(y), fill=(200, 0, 0))
    im.save(path)

def main():
    import argparse
    ap = argparse.ArgumentParser(description='Build the PsiCast Modern icon pack.')
    ap.add_argument('--spec', default=SPEC, help='the designs (standard tools/modicons/specs.py)')
    ap.add_argument('--out', help='the pack (standard src/cast/modicons.json, for the standard spec)')
    ap.add_argument('--sheet', help='write a contact sheet: 5mx grey | colour on white | colour on light grey')
    ap.add_argument('--match', help='only pictures whose names match this (for the sheet)')
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--inspect', nargs=2, metavar=('NAME', 'PNG'))
    ap.add_argument('--sides', action='store_true', help="rewrite the spec's sides file from its SOURCES")
    ap.add_argument('--rom', help='a 5mx ROM image, for SOURCES in it (read in place)')
    ap.add_argument('--check', action='store_true', help='do the sides still match the built files?')
    o = ap.parse_args()
    spec = load_spec(o.spec)
    if spec is not specs:
        add_materials(getattr(spec, 'MATERIALS', {}))
    if o.sides or o.check:
        sys.exit(0 if make_sides(spec, o.spec, o.rom, o.check) else 1)
    if o.inspect:
        inspect(o.inspect[0], o.inspect[1], o.spec); return
    out = o.out or (OUT if spec is specs else None)
    pack, drawn, ncol = build(spec, o.spec, o.only)
    if not o.only and out:
        with open(out, 'w') as f:
            json.dump(pack, f, separators=(',', ':')); f.write('\n')
        raw = open(out, 'rb').read()
        print('%d pictures, %d colours -> %s (%d bytes, %d gzipped)' % (
            len(pack['icons']), ncol, out, len(raw), len(gzip.compress(raw, 9))))
    if o.sheet:
        if o.match:
            drawn = [d for d in drawn if re.search(o.match, d[0]['n'])]
        sheet(drawn, o.sheet, o.scale)

if __name__ == '__main__':
    main()
