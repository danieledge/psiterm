#!/usr/bin/env python3
"""mkpictures.py FOLDER - the test pictures for test_proxy (needs Pillow):
a photo-like JPEG (gradient and shapes, 800x500), the same as PNG and as
GIF, a small GIF (80x50) and a progressive JPEG. test_proxy sends them
through the modem's web proxy with pictures on and checks what comes out."""
import os, sys
from PIL import Image, ImageDraw


def scene(w, h):
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (x * 255 // w, y * 255 // h, (x + y) * 127 // (w + h))
    d = ImageDraw.Draw(im)
    d.ellipse((w // 4, h // 4, 3 * w // 4, 3 * h // 4), fill=(255, 255, 255), outline=(0, 0, 0), width=8)
    d.rectangle((10, 10, w // 5, h // 5), fill=(0, 0, 0))
    d.rectangle((w - w // 5, h - h // 5, w - 10, h - 10), fill=(255, 255, 255))
    return im


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'pictures'
    os.makedirs(out, exist_ok=True)
    big = scene(800, 500)
    big.save(os.path.join(out, 'photo.jpg'), quality=85)
    big.save(os.path.join(out, 'photo_prog.jpg'), quality=85, progressive=True)
    big.save(os.path.join(out, 'photo.png'))
    # (a GIF needs the whole frame in memory in the decoder; 384x240 is within
    # the Atom's limit, a photo-sized one is passed through)
    scene(384, 240).convert('P', palette=Image.ADAPTIVE, colors=64).save(os.path.join(out, 'photo.gif'))
    big.convert('P', palette=Image.ADAPTIVE, colors=64).save(os.path.join(out, 'big.gif'))
    scene(80, 50).convert('P', palette=Image.ADAPTIVE, colors=16).save(os.path.join(out, 'small.gif'))
    # the mean brightness of the big scene, for the test to compare with
    g = big.convert('L')
    with open(os.path.join(out, 'mean.txt'), 'w') as f:
        f.write('%d\n' % (sum(g.getdata()) // (g.width * g.height)))


if __name__ == '__main__':
    main()
