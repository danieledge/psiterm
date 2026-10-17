#!/usr/bin/env python3
# cast_page_shot.py - PsiCast's viewer page (src/cast/viewer.html, as the
# firmware serves it) in headless Chromium at iPhone sizes, against
# hostmodem and the fake Psion of test_cast_e2e.py: screenshots in portrait
# and landscape, with control and the key bar, in each theme, and checks
# that the page's canvas shows the Psion's picture pixel for pixel and that
# a tap on it reaches the Psion. Needs Python's playwright and its Chromium.
#   make -C hosttest hostmodem libpccodec.so && python3 hosttest/cast_page_shot.py [outdir]
# MIT licence.
import os, socket, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_cast_e2e as e2e
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))


def draw_screen(ps):
    """Something that looks like a Psion: a Word document, the toolbar, a dialog."""
    from PIL import Image, ImageDraw, ImageFont
    im = Image.new("L", (640, 240), 255)
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype("DejaVuSans.ttf", 11)
        fb = ImageFont.truetype("DejaVuSans-Bold.ttf", 11)
    except OSError:
        f = fb = ImageFont.load_default()
    d.rectangle((0, 0, 639, 15), fill=0)
    d.text((6, 1), "Word   File  Edit  View  Insert  Text  Tools", font=fb, fill=255)
    lines = ["Dear reader,", "", "PsiCast shows this screen on the phone, live: the Atom",
             "decodes each update and sends every viewer what changed.",
             "Scrolling text goes as a move, so it costs almost nothing.", "",
             "Yours, the Psion Series 5mx"]
    for i, t in enumerate(lines):
        d.text((14, 24 + i * 15), t, font=f, fill=0)
    # the toolbar on the right, in greys
    d.rectangle((560, 16, 639, 239), fill=200)
    for i, t in enumerate(("Bold", "Italic", "Style", "Print")):
        d.rectangle((566, 24 + i * 48, 633, 64 + i * 48), fill=240, outline=60)
        d.text((574, 36 + i * 48), t, font=f, fill=40)
    d.text((14, 222), "Page 1 of 1   Words 54", font=f, fill=110)
    px = im.load()
    for y in range(240):
        for x in range(0, 640, 2):
            a, b = px[x, y] * 15 // 255, px[x + 1, y] * 15 // 255
            ps.fb[y * 320 + x // 2] = a | (b << 4)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "screens", "cast")
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp()
    sockpath = os.path.join(tmp, "modem.sock")
    link, view, http = e2e.free_port(), e2e.free_port(), e2e.free_port()
    env = dict(os.environ, HOSTMODEM_CAST="1", HOSTMODEM_CASTPORT=str(link), HOSTMODEM_CASTVIEW=str(view),
               HOSTMODEM_HTTP=str(http))
    log = open(os.path.join(tmp, "hostmodem.log"), "w")
    proc = subprocess.Popen([os.path.join(HERE, "hostmodem"), sockpath], env=env, stderr=log, stdout=log)
    shots = []
    try:
        for _ in range(100):
            if os.path.exists(sockpath):
                break
            time.sleep(0.05)
        ser = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ser.connect(sockpath)                      # (the serial line: idle here)
        url = "http://127.0.0.1:%d/screen" % http
        with sync_playwright() as p:
            b = p.chromium.launch()
            iphone = dict(viewport={"width": 390, "height": 844}, device_scale_factor=3, is_mobile=True, has_touch=True,
                          user_agent="Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1")
            ctx = b.new_context(**iphone)
            page = ctx.new_page()
            errors = []
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
            # no Psion yet: "Waiting for the Psion..."
            page.goto(url)
            page.wait_for_timeout(1200)
            page.screenshot(path=os.path.join(out, "1-waiting.png"))
            shots.append("1-waiting.png")
            e2e.check("Waiting for the Psion" in page.inner_text("#veil"), "the page says it is waiting for the Psion")
            # the Psion connects (the TCP route) and shares its screen
            ps = e2e.Psion(socket.create_connection(("127.0.0.1", link)), "My 5mx")
            ps.hello()
            ps.send(e2e.frame(0x05, (695).to_bytes(2, "little") + (280).to_bytes(2, "little")))   # DEVINFO
            time.sleep(0.2)
            draw_screen(ps)
            ps.update()
            ps.status()
            page.wait_for_timeout(1500)
            page.screenshot(path=os.path.join(out, "2-portrait.png"))
            shots.append("2-portrait.png")
            # the canvas, read back: the Psion's picture exactly (5mx LCD theme, the default)
            same = page.evaluate("""() => {
                const c = document.getElementById('model'), d = c.getContext('2d').getImageData(0, 0, 640, 240).data;
                const ink = [27, 36, 26], paper = [186, 196, 164], g = [];
                for (let i = 0; i < 640 * 240; i++) {
                    let best = 0, bd = 1e9;
                    for (let k = 0; k < 16; k++) {
                        let e = 0;
                        for (let j = 0; j < 3; j++) { const v = Math.round(ink[j] + (paper[j] - ink[j]) * k / 15); e += Math.abs(v - d[i * 4 + j]); }
                        if (e < bd) { bd = e; best = k; }
                    }
                    g.push(best);
                }
                return g;
            }""")
            want = []
            for byte in ps.fb:
                want += [byte & 15, byte >> 4]
            e2e.check(same == want, "the page's canvas is the Psion's picture, pixel for pixel")
            # scrolling text on the Psion: the page follows
            for i in range(3):
                ps.fb[24 * 320:200 * 320] = ps.fb[39 * 320:215 * 320]
                ps.update()
                time.sleep(0.15)
            page.wait_for_timeout(600)
            # control, with the key bar
            page.click("#bControl")
            page.wait_for_timeout(1500)
            page.screenshot(path=os.path.join(out, "3-control.png"))
            shots.append("3-control.png")
            box = page.locator("#screen").bounding_box()
            page.mouse.click(box["x"] + box["width"] * 100 / 640, box["y"] + box["height"] * 50 / 240)
            page.click('.k[data-sc="148"]')      # Menu
            page.click('.k[data-app="0"]')       # System, in the digitiser's margins from DEVINFO
            page.wait_for_timeout(500)
            e2e.check((1, -23, 260) in ps.inputs, "the System icon is a tap at -23,260 (%r)" % ps.inputs[-4:])
            e2e.check((1, 100, 50) in ps.inputs and (4, 0x94, 0) in ps.inputs, "a tap and Menu from the page reach the Psion (%r)" % ps.inputs[:6])
            # the themes
            page.click("#bMore")
            page.wait_for_timeout(300)
            page.screenshot(path=os.path.join(out, "4-settings.png"))
            shots.append("4-settings.png")
            for t in ("amber", "green", "dark", "paper"):
                page.click('.sw[data-id="%s"]' % t)
                page.wait_for_timeout(150)
                page.locator("#frame").screenshot(path=os.path.join(out, "5-theme-%s.png" % t))
                shots.append("5-theme-%s.png" % t)
            page.click('.sw[data-id="lcd"]')
            page.click("#bClose")
            # landscape
            page.set_viewport_size({"width": 844, "height": 390})
            page.wait_for_timeout(500)
            page.screenshot(path=os.path.join(out, "6-landscape.png"))
            shots.append("6-landscape.png")
            page.click("#bFull")
            page.wait_for_timeout(500)
            page.screenshot(path=os.path.join(out, "7-landscape-full.png"))
            shots.append("7-landscape-full.png")
            # full screen is the 5mx's face: the silkscreen icons round the screen, each a tap where the digitiser has it
            e2e.check(page.locator('.sk').count() == 14, "full screen shows the 14 silkscreen icons")
            page.click('.sk[data-silk="11"]')     # Zoom in, on the left strip
            page.wait_for_timeout(400)
            e2e.check((1, -23, 172) in ps.inputs, "the Zoom in icon in full screen is a tap at -23,172 (%r)" % ps.inputs[-4:])
            # drawn at the phone's resolution (3x here), not the Psion's
            dims = page.evaluate("() => { const c = document.getElementById('screen'), r = c.getBoundingClientRect(); return [c.width, Math.round(r.width * devicePixelRatio)]; }")
            e2e.check(dims[0] == dims[1] and dims[0] > 640, "the screen is drawn at the phone's own resolution (%r)" % dims)

            # paused on the Psion, then gone
            page.click("#exit")
            page.wait_for_timeout(400)
            ps.send(e2e.frame(0x03, bytes([77, 0]) + (3 << 20).to_bytes(4, "little") + bytes([50, 0, 120, 0, 1, 1])))
            page.wait_for_timeout(800)
            page.set_viewport_size({"width": 390, "height": 844})
            page.wait_for_timeout(400)
            page.screenshot(path=os.path.join(out, "8-paused.png"))
            shots.append("8-paused.png")
            e2e.check("Psion paused" in page.inner_text("#banner"), "the page says the Psion is paused")
            ps.send(e2e.frame(0x04))
            page.wait_for_timeout(1200)
            page.screenshot(path=os.path.join(out, "9-psion-gone.png"))
            shots.append("9-psion-gone.png")
            e2e.check(not errors, "no script errors (%r)" % errors[:3])
            b.close()
    finally:
        proc.terminate()
        proc.wait(3)
        log.close()
    print("cast_page_shot: %s: %s" % (out, ", ".join(shots)))
    print("cast_page_shot: %d checks, %d failed" % (e2e.checks, e2e.fails))
    return 1 if e2e.fails else 0


if __name__ == "__main__":
    sys.exit(main())
