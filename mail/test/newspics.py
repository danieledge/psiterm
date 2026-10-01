#!/usr/bin/env python3
"""newspics.py OUTDIR - the pictures for fakeimap.py --news OUTDIR

Photo-like pictures (gradients, shapes and grain, so they decode as slowly
as real photos do): the attached and inline (cid:) ones the news mailbox
sends, and under OUTDIR/web/ the newsletters' pictures from the web, to be
served by a local web server (python3 -m http.server -d OUTDIR/web PORT)
for Message > Web > Show web pictures. Needs Pillow.
"""
import os, sys, random
from PIL import Image, ImageDraw, ImageFilter

out = sys.argv[1]
os.makedirs(os.path.join(out, "web", "hf"), exist_ok=True)
os.makedirs(os.path.join(out, "web", "icons"), exist_ok=True)
random.seed(75)

def photo(w, h, seed):
    r = random.Random(seed)
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(0, h, 4):
        c = (60 + 120 * y // h, 90 + 100 * y // h, 160 - 60 * y // h)
        d.rectangle([0, y, w, y + 4], fill=c)
    for _ in range(60):
        x, y, s = r.randrange(w), r.randrange(h), r.randrange(10, max(11, w // 6))
        d.ellipse([x, y, x + s, y + s * 2 // 3], fill=(r.randrange(256), r.randrange(256), r.randrange(256)))
    for _ in range(40):
        d.line([r.randrange(w), r.randrange(h), r.randrange(w), r.randrange(h)], fill=(r.randrange(256),) * 3, width=r.randrange(1, 6))
    im = im.filter(ImageFilter.GaussianBlur(1.2))
    # grain: what makes a camera's JPEG big and slow
    noise = Image.effect_noise((w, h), 28).convert("RGB")
    return Image.blend(im, noise, 0.12)

def logo(w, h, text):
    im = Image.new("RGB", (w, h), (255, 255, 255))
    d = ImageDraw.Draw(im)
    d.rectangle([2, 2, w - 3, h - 3], outline=(40, 80, 50), width=3)
    d.text((10, h // 2 - 6), text, fill=(20, 40, 20))
    return im

photo(1600, 1200, 1).save(os.path.join(out, "harbour.jpg"), quality=72, progressive=True)
photo(1200, 900, 2).save(os.path.join(out, "beach.jpg"), quality=75)
photo(2592, 1944, 3).save(os.path.join(out, "cliffs.jpg"), quality=80, progressive=True)   # a 5 MP camera picture (tap to get)
photo(480, 360, 4).quantize(32).save(os.path.join(out, "garden.png"))
logo(200, 50, "Pennywhistle Books").save(os.path.join(out, "shoplogo.png"))
photo(560, 200, 5).save(os.path.join(out, "map.jpg"), quality=70)
w = os.path.join(out, "web")
logo(180, 60, "Hartley & Finch").save(os.path.join(w, "hf", "logo.png"))
photo(600, 400, 6).save(os.path.join(w, "hf", "hero-autumn.jpg"), quality=70)
photo(290, 290, 7).save(os.path.join(w, "hf", "jumper.jpg"), quality=70)
photo(290, 290, 8).save(os.path.join(w, "hf", "jacket.jpg"), quality=70, progressive=True)
for n, c in (("fb", (59, 89, 152)), ("ig", (200, 60, 120)), ("pin", (200, 30, 30))):
    Image.new("RGB", (32, 32), c).save(os.path.join(w, "icons", n + ".png"))
logo(320, 48, "The Weekly Byte").save(os.path.join(w, "masthead.png"))
Image.new("RGB", (1, 1), (255, 255, 255)).save(os.path.join(w, "open.gif"))
for f in sorted(os.listdir(out)):
    p = os.path.join(out, f)
    if os.path.isfile(p): print("%8d %s" % (os.path.getsize(p), f))
