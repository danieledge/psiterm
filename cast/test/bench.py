#!/usr/bin/env python3
"""bench.py - frame sequences for the PsiCast codec benchmark, and the run.

    python3 bench.py OUTDIR [--gen-only] [--sweep]

Makes sequences of 640x240 16-grey frames in OUTDIR/seq, then runs
OUTDIR/bench on them and prints a Markdown table.

Synthetic sequences, built from the screenshots in docs/screenshots:
  type     typing a paragraph in a word-processor-like page, a key a frame
  term     a terminal scrolling one line at a time (new line at the bottom)
  doc      a document in a reader pane scrolled 3 lines a frame
  list     a list pane beside a fixed pane, scrolled a line a frame
  menu     a menu opens, the highlight moves down, a dialog opens and closes
  switch   switching between apps (every frame a different screen)
Real sequences, if the emulator has been run here (build/emu/NAME/s-*.png,
one frame a simulated second; tools/emu/README.md): any listed in EMU that
exist, with repeated frames left out.

--sweep also runs the tuning sweep (deflate level, moves, RLE, tile size).
"""
import os, sys, glob, struct, subprocess
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, "../.."))
SHOTS = os.path.join(TOP, "docs/screenshots")
W, H = 640, 240
EMU = ["pmreader", "rv-scroll", "pterm", "ptview", "ptread", "pweb", "pwview",
       "pmail", "ptmenu", "ptcal", "smoke-m", "smoke-t", "showcase", "s26split"]


def grey16(im):
    """A screenshot as 16 greys (0 black, 15 white), 640x240."""
    im = im.convert("L")
    if im.size != (W, H):
        im = im.crop((0, 0, W, H)) if im.size[0] >= W and im.size[1] >= H else im.resize((W, H))
    hi = max(im.getdata()) or 1
    return im.point(lambda v: min(15, (v * 15 + hi // 2) // hi))


def pack(im):
    px = im.tobytes()
    out = bytearray(W * H // 2)
    for i in range(0, W * H, 2):
        out[i >> 1] = (px[i] & 15) | ((px[i + 1] & 15) << 4)
    return bytes(out)


def save(outdir, name, frames):
    path = os.path.join(outdir, name + ".pcs")
    with open(path, "wb") as f:
        f.write(b"PCSQ" + struct.pack("<HHI", W, H, len(frames)))
        for fr in frames:
            f.write(pack(fr))
    return path


def font(size, mono=False):
    names = ["DejaVuSansMono.ttf", "NotoSansMono-Regular.ttf"] if mono else ["DejaVuSans.ttf", "FreeSans.ttf"]
    for n in names:
        for p in glob.glob("/usr/share/fonts/**/" + n, recursive=True):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def text_lines():
    """Ordinary prose and code, for documents and terminals."""
    lines = []
    for p in ["README.md", "docs/BUILDING.md", "mail/README.md", "ssh/psiglue.cpp"]:
        try:
            lines += open(os.path.join(TOP, p), encoding="utf-8", errors="replace").read().splitlines()
        except OSError:
            pass
    return [l.replace("\t", "    ")[:110] for l in lines if l.strip()] or ["hello world"] * 200


def draw_text(d, xy, s, f, fill=0):
    d.fontmode = "1"        # no anti-aliasing: Psion fonts are bitmaps
    d.text(xy, s, font=f, fill=fill)


def seq_type():
    base = grey16(Image.open(os.path.join(SHOTS, "psimail-reader.png")))
    d = ImageDraw.Draw(base)
    d.rectangle((0, 98, 546, 239), fill=15)
    f = font(12)
    words = " ".join(text_lines()[2:12]).split()
    text = " ".join(words)[:160]
    frames = [base.copy()]
    for n in range(1, len(text) + 1):
        im = base.copy()
        dd = ImageDraw.Draw(im)
        x, y, line = 6, 102, ""
        for ch in text[:n]:
            line += ch
            if dd.textlength(line, font=f) > 530:
                draw_text(dd, (x, y), line[:-1], f)
                line = ch.lstrip()
                y += 15
        draw_text(dd, (x, y), line, f)
        cx = x + int(dd.textlength(line, font=f)) + 1
        dd.rectangle((cx, y, cx + 1, y + 13), fill=0)       # the cursor
        frames.append(im)
    return frames


def seq_term():
    base = grey16(Image.open(os.path.join(SHOTS, "psiterm-tmux-tabs.png")))
    f = font(10, mono=True)
    lines = text_lines()[40:200]
    lh, top, rows = 11, 27, 17
    frames = []
    for k in range(60):
        im = base.copy()
        pane = Image.new("L", (566, rows * lh), 15)
        d = ImageDraw.Draw(pane)
        for r in range(rows):
            draw_text(d, (2, r * lh - 1), lines[k + r][:94], f)
        d.rectangle((2, (rows - 1) * lh, 8, rows * lh - 2), fill=0)   # cursor
        im.paste(pane, (0, top))
        frames.append(im)
    return frames


def seq_doc():
    base = grey16(Image.open(os.path.join(SHOTS, "psimail-newsletter.png")))
    f = font(12)
    lines = text_lines()[200:400]
    lh, top, bot = 14, 98, 238
    frames = []
    for k in range(0, 90, 3):
        im = base.copy()
        pane = Image.new("L", (547, 240 - top), 15)
        dp = ImageDraw.Draw(pane)
        y = 2
        for l in lines[k:k + 11]:
            draw_text(dp, (4, y), l[:80], f)
            y += lh
        im.paste(pane, (0, top))
        d = ImageDraw.Draw(im)
        # the scroll bar's thumb moves too
        d.rectangle((549, 44, 565, 200), fill=12)
        ty = 44 + k * 120 // 90
        d.rectangle((550, ty, 564, ty + 30), fill=6)
        frames.append(im)
    return frames


def seq_list():
    """A two-pane list (a folder pane that stays, a message list that
    scrolls a line at a time under a highlight at the bottom)."""
    base = grey16(Image.open(os.path.join(SHOTS, "psimail-mailbox.png")))
    f = font(12)
    lines = text_lines()[400:520]
    lh, top, x0, x1 = 16, 24, 170, 546
    frames = []
    for k in range(50):
        im = base.copy()
        pane = Image.new("L", (x1 - x0 + 1, 240 - top), 15)
        d = ImageDraw.Draw(pane)
        for r in range(13):
            y = r * lh
            sel = r == 12
            if sel:
                d.rectangle((0, y, x1 - x0, y + lh - 1), fill=0)
            draw_text(d, (4, y + 1), lines[k + r][:60], f, fill=15 if sel else 0)
        im.paste(pane, (x0, top))
        frames.append(im)
    return frames


def seq_menu():
    base = grey16(Image.open(os.path.join(SHOTS, "psiterm-tmux-tabs.png")))
    f = font(13)
    items = ["Preferences...", "SSH keys...", "Connection settings...", "Reset terminal",
             "Update PsiTerm...", "Debug", "Help on PsiTerm", "About PsiTerm"]

    def menu(sel):
        im = base.copy()
        d = ImageDraw.Draw(im)
        d.rectangle((0, 0, 480, 26), fill=15, outline=0)
        for i, t in enumerate(["File", "Edit", "View", "Keys", "Snippets", "tmux", "Tools"]):
            draw_text(d, (8 + i * 66, 4), t, f)
        d.rectangle((300, 26, 560, 26 + 8 * 24 + 6), fill=15, outline=0)
        for i, t in enumerate(items):
            y = 30 + i * 24
            if i == sel:
                d.rectangle((302, y - 1, 558, y + 21), fill=0)
            draw_text(d, (310, y), t, f, fill=15 if i == sel else 0)
        return im

    def dialog(im):
        im = im.copy()
        d = ImageDraw.Draw(im)
        d.rectangle((110, 30, 530, 214), fill=13, outline=0)
        d.rectangle((112, 32, 528, 56), fill=0)
        draw_text(d, (120, 36), "Connection settings", f, fill=15)
        for i, t in enumerate(["Connect via", "Host", "Port", "Proxy"]):
            draw_text(d, (124, 70 + i * 28), t, f)
            d.rectangle((260, 68 + i * 28, 510, 88 + i * 28), fill=15, outline=0)
            draw_text(d, (266, 70 + i * 28), ["Psion Internet", "example.org", "22", "Off"][i], f)
        return im

    frames = [base]
    frames += [menu(s) for s in range(0, 5)]
    frames.append(dialog(menu(4)))
    frames.append(menu(4))
    frames.append(base)
    return frames * 3


def seq_switch():
    shots = sorted(p for p in glob.glob(os.path.join(SHOTS, "*.png")) if "banner" not in p)
    ims = [grey16(Image.open(p)) for p in shots]
    return ims + ims[:4]


def seq_emu(name):
    paths = sorted(glob.glob(os.path.join(TOP, "build/emu", name, "s-*.png")))
    frames, last = [], None
    for p in paths:
        try:
            im = Image.open(p)
        except OSError:
            continue
        if im.size != (W, H):
            continue
        g = grey16(im)
        b = g.tobytes()
        if b != last:
            frames.append(g)
            last = b
        if len(frames) >= 150:
            break
    return frames


def generate(outdir):
    sd = os.path.join(outdir, "seq")
    os.makedirs(sd, exist_ok=True)
    paths = []
    for name, fn in [("type", seq_type), ("term", seq_term), ("doc", seq_doc), ("list", seq_list),
                     ("menu", seq_menu), ("switch", seq_switch)]:
        paths.append(save(sd, name, fn()))
    for name in EMU:
        fr = seq_emu(name)
        if len(fr) >= 5:
            paths.append(save(sd, "emu-" + name, fr))
    return paths


def run(outdir, paths, args=()):
    res = subprocess.run([os.path.join(outdir, "bench")] + list(args) + paths,
                         capture_output=True, text=True)
    if res.returncode:
        print(res.stdout, res.stderr)
        sys.exit(1)
    rows = []
    for l in res.stdout.splitlines():
        p = l.split()
        if p and p[0] == "SEQ":
            rows.append(dict(name=p[1].replace(".pcs", ""), n=int(p[2]), key=int(p[3]), med=int(p[4]),
                             max=int(p[5]), mean=int(p[6]), total=int(p[7]), ratio=float(p[8]),
                             tmed=int(p[9]), tmax=int(p[10]), moves=int(p[11])))
        elif p:
            print(l)
    return rows


def table(rows):
    print("| sequence | updates | keyframe B | median B | max B | mean B | ratio | moves | host median us | host max us |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        print("| %(name)s | %(n)d | %(key)d | %(med)d | %(max)d | %(mean)d | %(ratio).0f:1 | %(moves)d | %(tmed)d | %(tmax)d |" % r)


def main():
    outdir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(TOP, "build/cast-test"))
    paths = generate(outdir)
    if "--gen-only" in sys.argv:
        return
    table(run(outdir, paths))
    if "--sweep" in sys.argv:
        configs = [("level 1 (default)", []), ("level 3", ["-l", "3"]), ("level 6", ["-l", "6"]),
                   ("no moves", ["-s", "0"]), ("one move", ["-s", "1"]), ("no RLE", ["-r", "0"]),
                   ("gap 0", ["-g", "0"]), ("gap 2", ["-g", "2"]), ("gap 4", ["-g", "4"]),
                   ("tile h 4", ["-t", "4"]), ("tile h 16", ["-t", "16"]),
                   ("Z_RLE", ["-S", "3"]), ("Z_HUFFMAN_ONLY", ["-S", "2"]), ("Z_FILTERED", ["-S", "1"]),
                   ("memLevel 4", ["-m", "4"]), ("memLevel 6", ["-m", "6"]), ("memLevel 7", ["-m", "7"])]
        print()
        print("| config | " + " | ".join(os.path.basename(p)[:-4] for p in paths) + " |")
        print("|---|" + "---:|" * len(paths))
        for label, a in configs:
            rows = run(outdir, paths, a)
            print("| %s | " % label + " | ".join("%d" % r["total"] for r in rows) + " |")
        print("\n(total bytes of all updates after the keyframe)")


if __name__ == "__main__":
    main()
