# PsiTerm icon: a small terminal window showing ">_" with a padlock (SSH).
# 4-grey bitmaps (black, dark grey, light grey, white) + 1-bit masks,
# at the three sizes the Psion 5mx uses (24, 32, 48).
from PIL import Image, ImageDraw
K, D, L, W = 0, 85, 170, 255

def icon(n):
    im = Image.new("L", (n, n), W)
    mk = Image.new("1", (n, n), 1)          # 1 = white = transparent
    d, m = ImageDraw.Draw(im), ImageDraw.Draw(mk)
    s = n / 48.0
    def R(v): return int(round(v * s))
    # window: frame + title bar
    x0, y0, x1, y1 = R(2), R(5), n - 1 - R(4), n - 1 - R(7)
    d.rectangle([x0, y0, x1, y1], fill=K)
    m.rectangle([x0, y0, x1, y1], fill=0)
    tb = y0 + max(2, R(6))
    d.rectangle([x0 + 1, y0 + 1, x1 - 1, tb], fill=L)
    if n >= 32:                            # title bar "buttons"
        for i in range(3):
            cx = x0 + R(4) + i * R(4)
            d.rectangle([cx, y0 + R(2), cx + max(1, R(2)) - 1, tb - R(2)], fill=D)
    # screen
    sx0, sy0, sx1, sy1 = x0 + 1, tb + 1, x1 - 1, y1 - 1
    d.rectangle([sx0, sy0, sx1, sy1], fill=K)
    # ">" prompt
    t = max(1, R(2.5))
    px, py = sx0 + R(4), sy0 + R(5)
    h = R(12)
    d.line([(px, py), (px + h // 2, py + h // 2)], fill=W, width=t)
    d.line([(px + h // 2, py + h // 2), (px, py + h)], fill=W, width=t)
    # "_" cursor
    ux = px + h // 2 + R(5)
    d.rectangle([ux, py + h - t + 1, ux + R(9), py + h], fill=L)
    # padlock bottom-right (SSH), on a white halo so it reads over the window
    lw, lh = max(10, R(16)), max(7, R(11))
    lx1, ly1 = n - 1, n - 1
    lx0, ly0 = lx1 - lw, ly1 - lh
    t2 = max(1, R(2.5))                      # shackle thickness
    shw = lw - 2 * max(2, R(3))              # shackle outer width
    shx0 = lx0 + (lw - shw) // 2
    shx1 = shx0 + shw
    shy0 = ly0 - max(4, R(8))
    # halo
    d.rectangle([shx0 - 1, shy0 - 1, shx1 + 1, ly0], fill=W)
    d.rectangle([lx0 - 1, ly0 - 1, lx1, ly1], fill=W)
    m.rectangle([shx0 - 1, shy0 - 1, shx1 + 1, ly1], fill=0)
    m.rectangle([lx0 - 1, ly0 - 1, lx1, ly1], fill=0)
    # shackle: a thick upside-down U
    d.rectangle([shx0, shy0, shx1, shy0 + t2 - 1], fill=K)
    d.rectangle([shx0, shy0, shx0 + t2 - 1, ly0], fill=K)
    d.rectangle([shx1 - t2 + 1, shy0, shx1, ly0], fill=K)
    # body with keyhole
    d.rectangle([lx0, ly0, lx1, ly1], fill=K)
    d.rectangle([lx0 + 1, ly0 + 1, lx1 - 1, ly1 - 1], fill=L)
    kx = (lx0 + lx1) // 2
    kh = max(1, R(1))
    d.rectangle([kx - kh + 1, ly0 + max(2, R(3)), kx + kh - 1 + (1 if kh == 1 else 0), ly1 - max(2, R(3))], fill=K)
    return im, mk

files = []
for n in (24, 32, 48):
    im, mk = icon(n)
    im = im.point(lambda v: min((K, D, L, W), key=lambda g: abs(g - v)))
    im.save(f"psiterm{n}.bmp")
    mk.save(f"psiterm{n}m.bmp")
    files += [f"psiterm{n}.bmp", f"psiterm{n}m.bmp"]
# preview: 8x zoom, all three side by side
prev = Image.new("L", (8 * (24 + 32 + 48) + 40, 8 * 48 + 20), 200)
x = 10
for n in (24, 32, 48):
    im = Image.open(f"psiterm{n}.bmp").convert("L")
    mk = Image.open(f"psiterm{n}m.bmp").convert("L")
    bg = Image.new("L", (n, n), 200)
    comp = Image.composite(bg, im, mk)
    prev.paste(comp.resize((8 * n, 8 * n), Image.NEAREST), (x, 10))
    x += 8 * n + 10
prev.save("preview.png")
print(files)
