#!/usr/bin/env python3
"""htmltest.py [--show NAME]

The HTML converter (engine/html.c) against the newsletters in test/html/
(written for these tests: a shop's offer, a news digest, an order receipt,
a product update, a personal reply with a picture). Each is converted
whole and in odd-sized pieces (the two must agree), and checked for what
makes a newsletter read badly on a 640x240 screen:

  * no more than one blank line in a row, none at the start or the end;
  * nothing the sender hid (every fixture's hidden text has "HIDDEN" in it);
  * no lines with nothing on them but codes (an empty paragraph);
  * no spacer or tracking pictures (spacer.gif, 1x1, /o/... open pixels);
  * the words that matter still there, and the real pictures, with their
    width and height when the HTML gives them;
  * the reader's line count within a limit (well under the old converter's).

With git at hand the old converter (dev 64646cb, PsiMail 0.73) is built
too, and its line counts are shown beside the new ones. --show NAME prints
one fixture as the reader gets it. Built with AddressSanitizer.
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(TOP, "build", "htmltest")
os.makedirs(OUT, exist_ok=True)
FIX = os.path.join(HERE, "html")

def build(html_c, exe):
    cmd = ["gcc", "-g", "-O1", "-std=gnu99", "-w", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
           "-I", os.path.join(TOP, "ssh"), "-I", os.path.join(TOP, "mail/engine"), "-o", exe, os.path.join(HERE, "htmltest.c"), html_c,
           os.path.join(TOP, "mail/engine/charset.c")]
    subprocess.check_call(cmd)

exe = os.path.join(OUT, "htmltest")
build(os.path.join(TOP, "mail/engine/html.c"), exe)
old_exe = None
try:
    src = subprocess.run(["git", "-C", TOP, "show", "64646cb:mail/engine/html.c"], capture_output=True, check=True).stdout
    old_c = os.path.join(OUT, "html_0_73.c")
    open(old_c, "wb").write(src)
    old_exe = os.path.join(OUT, "htmltest-0.73")
    build(old_c, old_exe)
except Exception:
    old_exe = None

def convert(e, path, split=None):
    r = subprocess.run([e, path] + ([str(split)] if split else []), capture_output=True)
    if r.returncode != 0:
        print(r.stderr.decode("latin-1")); raise SystemExit("htmltest crashed on " + path)
    return r.stdout.decode("latin-1")

def reader_lines(text):
    """the lines the reader shows (link addresses at the end are not shown)"""
    return [l for l in text.split("\n")[:-1] if not l.startswith("\x01u")]

def visible(l):
    """what a line shows, without its codes"""
    if l.startswith("\x01i"):
        return "[picture]"
    s = l[2:] if l.startswith("\x01") else l
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\x15":
            i += 1
            while i < len(s) and s[i].isdigit(): i += 1
            if i < len(s) and s[i] == "\x16": i += 1
            continue
        if c == "\x02" or ord(c) < 0x20: i += 1; continue
        out.append(c); i += 1
    return "".join(out).strip()

# per fixture: words that must be there, pictures (src fragment -> "WxH" or None) that must be, a line limit
# (the newsletters must also come out shorter than 0.73 had them; the personal
# reply gains its blank lines back: Gmail's <div><br></div> is one)
EXPECT = {
    "retail.html": (["The Autumn Edit", "WOOLLY20", "Shop knitwear", "Fisherman jumper", "Unsubscribe", "Ludlow"],
                    {"logo.png": "180x60", "hero-autumn.jpg": "600x400", "jumper.jpg": "290x290", "fb.png": "32x32"}, 40),
    "digest.html": (["Pocket computers are back", "eleven years", "Quick links", "The 1999 handheld round-up", "unsubscribe"],
                    {"masthead.png": "320x48"}, 40),
    "receipt.html": (["Dear Dan", "Programmer's Guide", "20.48", "Falmouth"],
                     {"cid:logo@shop.example": "200x50", "cid:map@shop.example": "560x200"}, 24),
    "update.html": (["Tasklet 4.2 is here", "Dark mode", "Offline sync", "Update now", "Unsubscribe"],
                    {}, 22),
    "personal.html": (["Thanks for the photos", "Are you still coming", "Mum", "wrote:", "Here are the pictures"],
                      {"cid:ii_harbour01": "542x406"}, 24),
}
SPACERS = ["spacer.gif", "open.gif", "/o/", "/e2t/o/"]

if "--show" in sys.argv:
    name = sys.argv[sys.argv.index("--show") + 1]
    for l in convert(exe, os.path.join(FIX, name)).split("\n"):
        print(repr(l))
    sys.exit(0)

bad = 0
def fail(name, why):
    global bad
    bad += 1
    print("FAIL %s: %s" % (name, why))

print("%-14s %8s %8s %8s %8s" % ("fixture", "0.73", "now", "blank", "pictures"))
for name in sorted(os.listdir(FIX)):
    if not name.endswith(".html"): continue
    path = os.path.join(FIX, name)
    text = convert(exe, path)
    for split in (1, 7, 64, 4096):
        if convert(exe, path, split) != text:
            fail(name, "converted in pieces of %d it differs" % split)
    lines = reader_lines(text)
    blanks = [i for i, l in enumerate(lines) if l == ""]
    pics = [l for l in lines if l.startswith("\x01i")]
    old = len(reader_lines(convert(old_exe, path))) if old_exe else -1
    print("%-14s %8s %8d %8d %8d" % (name, old if old >= 0 else "-", len(lines), len(blanks), len(pics)))
    if lines and lines[0] == "": fail(name, "starts with a blank line")
    if lines and lines[-1] == "": fail(name, "ends with a blank line")
    for i in blanks:
        if i + 1 in blanks: fail(name, "two blank lines in a row at line %d" % i); break
    for l in lines:
        if l != "" and not visible(l): fail(name, "a line with nothing to show: %r" % l); break
    if "HIDDEN" in text: fail(name, "hidden text shown: %r" % [l for l in lines if "HIDDEN" in l][:1])
    for s in SPACERS:
        if any(s in p for p in pics): fail(name, "a spacer or tracking picture: %s" % s)
    words, want_pics, limit = EXPECT.get(name, ([], {}, 999))
    flat = " ".join(visible(l) for l in lines)
    for w in words:
        if w not in flat: fail(name, "lost %r" % w)
    for frag, size in want_pics.items():
        hit = [p for p in pics if frag in p]
        if not hit: fail(name, "lost the picture %s" % frag); continue
        if size and not hit[0].endswith("\x02" + size): fail(name, "%s: no size %s in %r" % (frag, size, hit[0]))
    if len(lines) > limit: fail(name, "%d lines, more than %d" % (len(lines), limit))
    if old_exe and old >= 0 and name != "personal.html" and len(lines) >= old: fail(name, "no fewer lines than 0.73 had (%d, %d)" % (len(lines), old))
print("htmltest:", "FAILED (%d)" % bad if bad else "all good")
sys.exit(1 if bad else 0)
