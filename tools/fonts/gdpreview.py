import re
from PIL import Image
t = open("psiterm.gd").read()
fonts = {}
for m in re.finditer(r"FontBitmap (\w+)\nUid \d+\nMaxNormalCharWidth (\d+)\nCellHeight (\d+)\nAscent (\d+)\nCodeSection 32:255\n(.*?)EndCodeSection", t, re.S):
    name, W, H, A = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))
    g = {}
    for c in re.finditer(r"Char (\d+)\n(.*?)EndChar", m.group(5), re.S):
        g[int(c.group(1))] = c.group(2).split()
    fonts[name] = (W, H, g)
lines = ["psitest@vm:~$ claude  \x95 Read(ptproj/psiterm.cpp)  \x85 'quotes' \x93ok\x94 caf\xe9 \x80 1.2k",
         "The quick brown fox jumps over the lazy dog 0123456789 {}[]()<>|/\\~`!@#$%^&*_+-=:;?,."]
rows = []
for name in ("TERMINUS12", "TERMINUS14", "TERMINUS16", "TERMINUS18"):
    W, H, g = fonts[name]
    for ln in lines:
        img = Image.new("L", (W * len(ln), H), 255)
        for i, ch in enumerate(ln.encode("latin-1") if False else [ord(c) for c in ln]):
            bm = g.get(ch, g[63])
            for y, row in enumerate(bm):
                for x, p in enumerate(row):
                    if p == "*": img.putpixel((i * W + x, y), 0)
        rows.append(img)
width = max(r.size[0] for r in rows); height = sum(r.size[1] + 4 for r in rows)
out = Image.new("L", (width, height), 255); y = 0
for r in rows: out.paste(r, (0, y)); y += r.size[1] + 4
out = out.resize((out.size[0] * 2, out.size[1] * 2), Image.NEAREST); out.save("preview.png"); print(out.size)
