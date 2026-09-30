#!/usr/bin/env python3
"""imgtest.py [OUTDIR] [--rounds N] [--pictures DIR]

The picture decoders (mail/engine/img) on a PC: builds imgtest.c with
AddressSanitizer, makes sample pictures of every kind the Psion will meet
(baseline JPEG in 4:2:0, 4:2:2, 4:4:4 and greyscale, a progressive one that
must be refused; PNG in every colour type, 1-bit, interlaced; GIF plain,
interlaced and transparent; a 3000-pixel banner and a photo big enough for
the 1/8 JPEG path), decodes each to OUTDIR/*.pgm to look at, then fuzzes
each one (truncated, bit-flipped, overwritten) for --rounds rounds. Any
crash, sanitizer report or leak fails the run.

Needs gcc and Pillow. --pictures DIR adds your own JPEG/PNG/GIF files.
"""
import os, subprocess, sys, glob

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
out = None
rounds = 300
extra = None
args = sys.argv[1:]
while args:
    a = args.pop(0)
    if a == "--rounds": rounds = int(args.pop(0))
    elif a == "--pictures": extra = args.pop(0)
    else: out = a
out = out or os.path.join(TOP, "build", "imgtest")
os.makedirs(out, exist_ok=True)

# ---- the decoders, with the sanitizers on
exe = os.path.join(out, "imgtest-asan")
srcs = [os.path.join(HERE, "imgtest.c")] + \
    [os.path.join(TOP, "mail/engine/img", f) for f in ("pmimg.c", "pmjpeg.c", "pmpng.c", "pmgif.c", "picojpeg.c")] + \
    [os.path.join(TOP, "ssh/zlib", f) for f in ("adler32.c", "crc32.c", "inflate.c", "inftrees.c", "inffast.c", "zutil.c")]
cmd = ["gcc", "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined",
       "-I", os.path.join(TOP, "ssh/zlib"), "-o", exe] + srcs
print("building", exe)
subprocess.check_call(cmd)

# ---- the sample pictures
try:
    from PIL import Image, ImageDraw
except ImportError:
    print("pip install Pillow for the sample pictures"); sys.exit(2)

pics = os.path.join(out, "pictures")
os.makedirs(pics, exist_ok=True)

def scene(w, h):
    """a photo-like picture: gradients, shapes, small text (to judge dithering)"""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (x * 255 // max(1, w - 1), y * 255 // max(1, h - 1), 128 + (x + y) % 128)
    d = ImageDraw.Draw(im)
    d.ellipse((w // 8, h // 8, w // 2, h * 3 // 4), fill=(240, 240, 240), outline=(0, 0, 0), width=max(1, w // 200))
    d.rectangle((w // 2, h // 3, w * 7 // 8, h * 7 // 8), fill=(40, 40, 60))
    for i in range(8):
        d.line((0, h * i // 8, w, h * (i + 1) // 8), fill=(255, 255, 255), width=1)
    d.text((w // 2 + 8, h // 3 + 8), "PsiMail pictures", fill=(255, 255, 255))
    return im

def diagram(w, h):
    im = Image.new("RGB", (w, h), (255, 255, 255))
    d = ImageDraw.Draw(im)
    for i, name in enumerate(("PsiMail.app", "psimail.exe", "IMAP server")):
        x = 20 + i * (w - 40) // 3
        d.rectangle((x, h // 4, x + (w - 40) // 3 - 30, h // 4 + h // 5), outline=(0, 0, 0), fill=(230, 230, 230) if i == 1 else None, width=2)
        d.text((x + 8, h // 4 + 6), name, fill=(0, 0, 0))
    d.line((20 + (w - 40) // 3 - 30, h // 4 + h // 10, 20 + (w - 40) // 3, h // 4 + h // 10), fill=(0, 0, 0), width=2)
    d.line((20 + 2 * (w - 40) // 3 - 30, h // 4 + h // 10, 20 + 2 * (w - 40) // 3, h // 4 + h // 10), fill=(0, 0, 0), width=2)
    return im

def logo(w, h):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, w - 1, h - 1), radius=h // 4, fill=(40, 40, 40, 255))
    d.ellipse((h // 8, h // 8, h - h // 8, h - h // 8), fill=(200, 200, 200, 255))
    d.text((h + 10, h // 3), "PsiMail 2026", fill=(255, 255, 255, 255))
    return im

made = []
def save(im, name, **kw):
    p = os.path.join(pics, name)
    im.save(p, **kw)
    made.append(p)

photo = scene(800, 600)
save(photo, "photo800.jpg", quality=85, subsampling=2)             # 4:2:0
save(photo, "photo800_422.jpg", quality=85, subsampling=1)
save(photo, "photo800_444.jpg", quality=85, subsampling=0)
save(photo.convert("L"), "photo800g.jpg", quality=85)
save(photo, "photo800p.jpg", quality=85, progressive=True)         # refused: progressive
save(scene(2576, 1932), "photo2576.jpg", quality=80)                # the 1/8 path
save(scene(3000, 200), "banner.jpg", quality=80)
save(scene(1, 1), "tiny.jpg")
save(photo.resize((400, 300)), "photo400.png")
save(photo.resize((400, 300)), "photo400i.png", interlace=1)
dg = diagram(600, 300)
save(dg, "diagram.png")
save(dg.convert("P", palette=Image.ADAPTIVE, colors=16), "diagram16.png")
save(dg.convert("1"), "diagram1.png")
lg = logo(320, 120)
save(lg, "logo_rgba.png")
save(lg.convert("RGB"), "logo_rgb.png")
save(lg.convert("LA"), "logo_la.png")
save(lg.convert("P", palette=Image.ADAPTIVE, colors=64), "logo_pal.png")
save(lg.convert("L"), "logo_l.png")
save(Image.new("RGB", (1, 1), (0, 0, 0)), "tiny.png")
save(photo.resize((300, 225)), "photo300.gif")
save(photo.resize((300, 225)), "photo300i.gif", interlace=True)
save(dg, "diagram.gif")
save(lg, "logo.gif", transparency=0)
save(Image.new("L", (1, 1), 0), "tiny.gif")
if extra:
    made += [f for f in sorted(glob.glob(os.path.join(extra, "*"))) if f.lower().endswith((".jpg", ".jpeg", ".png", ".gif"))]

# ---- decode each, then fuzz each
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1", UBSAN_OPTIONS="print_stacktrace=1")
failed = 0
for p in made:
    name = os.path.basename(p)
    pgm = os.path.join(out, os.path.splitext(name)[0] + ".pgm")
    r = subprocess.run([exe, "decode", p, pgm], env=env, capture_output=True, text=True)
    line = (r.stdout + r.stderr).strip().splitlines()
    print("%-18s %s" % (name, line[-1] if line else "?"))
    if r.returncode not in (0, 1) or "Sanitizer" in r.stderr or "runtime error" in r.stderr:
        print(r.stderr); failed += 1
    if r.returncode == 1 and "progressive" not in r.stdout:
        print("  unexpected refusal"); failed += 1
    if r.returncode == 0 and "p.jpg" in name:
        print("  a progressive JPEG should be refused"); failed += 1
for p in made:
    name = os.path.basename(p)
    r = subprocess.run([exe, "fuzz", p, str(rounds), "7"], env=env, capture_output=True, text=True)
    line = (r.stdout + r.stderr).strip().splitlines()
    print("%-18s %s" % (name, (line[-1] if line else "?").split(": ", 1)[-1]))
    if r.returncode != 0 or "Sanitizer" in r.stderr or "runtime error" in r.stderr:
        print(r.stderr); failed += 1
print("FAILED (%d)" % failed if failed else "ok: no crashes, no sanitizer reports, no leaks")
sys.exit(1 if failed else 0)
