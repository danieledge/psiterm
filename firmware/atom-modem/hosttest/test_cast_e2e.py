#!/usr/bin/env python3
# test_cast_e2e.py - PsiCast end to end on the PC: hostmodem (the firmware's
# modem and PsiCast code, HOSTMODEM_CAST=1), a fake Psion that encodes its
# screen with the real codec (libpccodec.so, through ctypes) and speaks link
# A, and WebSocket viewers that decode FULL / RECTS / SCROLL and compare
# their picture with the Psion's, pixel for pixel. Both routes: the modem
# route (ATDT192.168.7.1:5961 on the serial line, here hostmodem's unix
# socket) and the TCP end that stands in for PPP. Also: the token rules,
# input from a viewer with control reaching the Psion, a slow viewer, and a
# KEYREQ after a damaged update.
#   make -C hosttest hostmodem libpccodec.so && python3 hosttest/test_cast_e2e.py
# MIT licence.
import base64, ctypes, json, os, random, socket, struct, subprocess, sys, tempfile, threading, time, zlib
import urllib.request, urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
W, H, STRIDE = 640, 240, 320
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAIL", what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ----- the codec ------------------------------------------------------------------
lib = ctypes.CDLL(os.path.join(HERE, "libpccodec.so"))
lib.pc_enc_new.restype = ctypes.c_void_p
lib.pc_enc_new.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
lib.pc_enc_frame.restype = ctypes.c_int
lib.pc_enc_frame.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                             ctypes.POINTER(ctypes.c_int)]
lib.pc_enc_free.argtypes = [ctypes.c_void_p]


def frame(t, payload=b"", flags=0):
    head = b"PC" + bytes([t, flags]) + struct.pack("<I", len(payload)) + payload
    return head + struct.pack("<I", zlib.crc32(head) & 0xffffffff)


class Parser:
    def __init__(self):
        self.buf = b""

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(b"PC")
            if i < 0:
                self.buf = self.buf[-1:] if self.buf.endswith(b"P") else b""
                return out
            self.buf = self.buf[i:]
            if len(self.buf) < 12:
                return out
            n = struct.unpack("<I", self.buf[4:8])[0]
            if n > 65536:
                self.buf = self.buf[1:]
                continue
            if len(self.buf) < n + 12:
                return out
            if struct.unpack("<I", self.buf[8 + n:12 + n])[0] != zlib.crc32(self.buf[:8 + n]) & 0xffffffff:
                self.buf = self.buf[1:]
                continue
            out.append((self.buf[2], self.buf[8:8 + n]))
            self.buf = self.buf[12 + n:]


class Psion:
    """A Psion running psicast.exe: the screen, the encoder, link A."""

    def __init__(self, sock, name="Test 5mx"):
        self.sock = sock
        self.enc = lib.pc_enc_new(W, H, 3)
        self.fb = bytearray(b"\xff" * (STRIDE * H))
        self.seq = self.acked = 0
        self.window = 2
        self.need_key = True
        self.welcome = None
        self.viewers = (0, 0)
        self.inputs = []
        self.keyreqs = 0
        self.lock = threading.Lock()
        self.alive = True
        self.raw = b""
        self.name = name
        self.rand = random.Random(5)
        threading.Thread(target=self.reader, daemon=True).start()

    def send(self, data):
        with self.lock:
            self.sock.sendall(data)

    def reader(self):
        p = Parser()
        while self.alive:
            try:
                d = self.sock.recv(4096)
            except OSError:
                break
            if not d:
                break
            self.raw += d
            for t, pl in p.feed(d):
                if t == 0x81:
                    self.welcome = pl
                    self.window = struct.unpack("<H", pl[2:4])[0]
                elif t == 0x82:
                    self.acked = max(self.acked, struct.unpack("<I", pl[:4])[0])
                elif t == 0x83:
                    self.need_key = True
                    self.keyreqs += 1
                elif t == 0x84:
                    self.viewers = (pl[0], pl[1])
                elif t == 0x85:
                    for i in range(0, len(pl), 8):
                        k, a, b = pl[i], struct.unpack("<h", pl[i + 2:i + 4])[0], struct.unpack("<h", pl[i + 4:i + 6])[0]
                        self.inputs.append((k, a, b))
                elif t == 0x86:
                    self.status()
        self.alive = False

    def hello(self, caps=3):
        nm = self.name.encode()
        self.send(frame(0x01, struct.pack("<HHHBBI", 1, W, H, 0, 0, caps) + nm))

    def status(self, ctl=True):
        self.send(frame(0x03, struct.pack("<BBIHHBB", 77, 0, 3 << 20, 50, 120, 1 if ctl else 0, 0)))

    def update(self, damage=False):
        """Encode and send the screen as it is now (waits for room in the window)."""
        t0 = time.time()
        while self.seq - self.acked >= self.window:
            if time.time() - t0 > 5:
                raise RuntimeError("no ACK from the Atom")
            time.sleep(0.002)
        out = ctypes.create_string_buffer(65536)
        n = ctypes.c_int(0)
        key = 1 if self.need_key else 0
        r = lib.pc_enc_frame(self.enc, bytes(self.fb), STRIDE, key, out, 65532, ctypes.byref(n))
        if r == 1:
            return False
        check(r == 0, "pc_enc_frame %d" % r)
        self.need_key = False
        self.seq += 1
        f = bytearray(frame(0x02, struct.pack("<I", self.seq) + out.raw[:n.value], key))
        if damage:
            f[20] ^= 0x10
        self.send(bytes(f))
        return True

    # drawing
    def px(self, x, y, g):
        i = y * STRIDE + x // 2
        b = self.fb[i]
        self.fb[i] = (b & 0x0f) | (g << 4) if x & 1 else (b & 0xf0) | g

    def box(self, x, y, w, h, g):
        for yy in range(y, min(H, y + h)):
            row = yy * STRIDE
            for xx in range(x, min(W, x + w)):
                i = row + xx // 2
                b = self.fb[i]
                self.fb[i] = (b & 0x0f) | (g << 4) if xx & 1 else (b & 0xf0) | g

    def text(self, y):
        x = 4
        while x < 600:
            if self.rand.random() > 0.2:
                self.box(x, y + 2, 5, 9, self.rand.choice((0, 5, 10)))
            x += 7

    def scroll(self):
        self.fb[20 * STRIDE:216 * STRIDE] = self.fb[32 * STRIDE:228 * STRIDE]
        self.box(0, 216, W, 12, 15)
        self.text(216)


# ----- a viewer ----------------------------------------------------------------------
class Viewer:
    def __init__(self, http, control=False, small=False):
        self.http = http
        r = json.load(urllib.request.urlopen("http://127.0.0.1:%d/screen/token%s" % (http, "?control=1" if control else "")))
        self.token = r["t"]
        self.port = r["port"]
        self.sock = socket.socket()
        if small:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)   # (a phone on a poor link)
        self.sock.connect(("127.0.0.1", self.port))
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(("GET /cast?t=%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (self.token, self.port, key)).encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            d = self.sock.recv(1)
            if not d:
                break
            resp += d
        self.status_line = resp.split(b"\r\n")[0].decode()
        import hashlib
        want = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.ok = self.status_line.startswith("HTTP/1.1 101") and ("Sec-WebSocket-Accept: " + want).encode() in resp
        self.grey = bytearray(b"\x0f" * (W * H))
        self.w = W
        self.h = H
        self.buf = b""
        self.texts = []
        self.counts = {1: 0, 2: 0, 3: 0}
        self.bytes = 0
        self.last = time.time()
        self.paused = False
        self.sock.setblocking(False)

    def say(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        hdr = bytes([0x81, 0x80 | len(data)]) if len(data) < 126 else bytes([0x81, 0xfe]) + struct.pack(">H", len(data))
        self.sock.setblocking(True)
        self.sock.sendall(hdr + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))
        self.sock.setblocking(False)

    def pump(self, limit=1 << 30):
        got = 0
        while got < limit and not self.paused:
            try:
                d = self.sock.recv(min(65536, limit - got))
            except BlockingIOError:
                break
            if not d:
                break
            got += len(d)
            self.bytes += len(d)
            self.buf += d
            self.last = time.time()
        while True:
            if len(self.buf) < 2:
                return
            op, ln = self.buf[0] & 15, self.buf[1] & 127
            hl = 2
            if ln == 126:
                if len(self.buf) < 4:
                    return
                ln, hl = struct.unpack(">H", self.buf[2:4])[0], 4
            elif ln == 127:
                if len(self.buf) < 10:
                    return
                ln, hl = struct.unpack(">Q", self.buf[2:10])[0], 10
            if len(self.buf) < hl + ln:
                return
            pl = self.buf[hl:hl + ln]
            self.buf = self.buf[hl + ln:]
            if op == 1:
                self.texts.append(json.loads(pl))
            elif op == 2:
                self.binary(pl)

    def binary(self, p):
        t = p[0]
        self.counts[t] = self.counts.get(t, 0) + 1
        if t == 1:
            self.w, self.h = struct.unpack("<HH", p[1:5])
            for i, b in enumerate(p[5:]):
                self.grey[2 * i] = b & 15
                self.grey[2 * i + 1] = b >> 4
        elif t == 2:
            n = struct.unpack("<H", p[1:3])[0]
            o = 3 + 8 * n
            for k in range(n):
                x, y, w, h = struct.unpack("<HHHH", p[3 + 8 * k:11 + 8 * k])
                for r in range(h):
                    g = (y + r) * W + x
                    for c in range(0, w, 2):
                        b = p[o]
                        o += 1
                        self.grey[g] = b & 15
                        self.grey[g + 1] = b >> 4
                        g += 2
            check(o == len(p), "RECTS length")
        elif t == 3:
            y0, y1, dy = struct.unpack("<HHh", p[1:7])
            x, cw = struct.unpack("<HH", p[7:11]) if len(p) >= 11 else (0, W)
            d0, d1 = y0 + dy, y1 + dy
            if d0 < 0:
                y0 -= d0
                d0 = 0
            if d1 > H:
                y1 -= d1 - H
                d1 = H
            band = [bytes(self.grey[r * W + x:r * W + x + cw]) for r in range(y0, y1)]
            for r in range(d0, d1):
                self.grey[r * W + x:r * W + x + cw] = band[r - d0]

    def same(self, fb):
        for i in range(STRIDE * H):
            b = fb[i]
            if self.grey[2 * i] != b & 15 or self.grey[2 * i + 1] != b >> 4:
                return False
        return True

    def settle(self, ms=250, timeout=6):
        t0 = time.time()
        while time.time() - t0 < timeout:
            self.pump()
            if time.time() - self.last > ms / 1000:
                return
            time.sleep(0.01)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def http_status(http, path):
    try:
        return urllib.request.urlopen("http://127.0.0.1:%d%s" % (http, path)).status
    except urllib.error.HTTPError as e:
        return e.code


def session(psion, http, label):
    # HELLO, WELCOME (with the viewer's address), a keyframe
    psion.hello()
    t0 = time.time()
    while psion.welcome is None and time.time() - t0 < 5:
        time.sleep(0.01)
    check(psion.welcome is not None, label + ": WELCOME")
    if psion.welcome:
        check(struct.unpack("<I", psion.welcome[8:12])[0] == 65536, label + ": WELCOME's largest UPDATE")
        url = psion.welcome[12:].decode()
        check(url == "http://127.0.0.1:%d/screen" % http, label + ": the viewer's address in WELCOME (%r)" % url)
    psion.box(0, 0, W, 18, 0)
    for y in range(20, 228, 12):
        psion.text(y)
    psion.update()
    v = Viewer(http)
    check(v.ok, label + ": the handshake (%s)" % v.status_line)
    slow = Viewer(http, small=True)
    slow.paused = True                              # (reads nothing for a while)
    time.sleep(0.3)
    check(psion.viewers[0] == 2, label + ": VIEWERS (%r)" % (psion.viewers,))
    # changes, scrolls, a damaged update
    for step in range(60):
        r = psion.rand.random()
        if r < 0.35:
            psion.scroll()
        elif r < 0.8:
            psion.box(psion.rand.randrange(600), psion.rand.randrange(20, 220), psion.rand.randrange(8, 160),
                      psion.rand.randrange(4, 40), psion.rand.randrange(16))
        else:
            psion.text(20 + 12 * psion.rand.randrange(17))
        damage = step == 30
        psion.update(damage=damage)
        if damage:
            time.sleep(0.2)
            check(psion.keyreqs >= 1, label + ": KEYREQ after a damaged update")
            if psion.need_key:
                psion.update()                      # (the keyframe it asked for)
        v.pump()
        time.sleep(0.01)
    time.sleep(0.3)
    v.settle()
    check(v.same(psion.fb), label + ": the fast viewer's picture is the Psion's")
    check(v.counts[3] > 5, label + ": scrolls went as SCROLL (%d)" % v.counts[3])
    slow.paused = False
    slow.settle()
    check(slow.same(psion.fb), label + ": the slow viewer's picture is the Psion's")
    check(slow.bytes < v.bytes, label + ": the slow viewer was sent less (%d against %d)" % (slow.bytes, v.bytes))
    st = [t for t in v.texts if t.get("t") == "status"]
    check(st and st[-1]["linkUp"] and st[-1]["viewers"] == 2, label + ": status JSON")
    print("  %s: fast viewer %d bytes (%r), slow %d bytes (%r)" % (label, v.bytes, v.counts, slow.bytes, slow.counts))
    # control: a viewer that asked for it can tap and type; one that did not cannot
    c = Viewer(http, control=True)
    time.sleep(0.3)
    c.pump()
    v.say({"t": "pen", "e": "down", "x": 5, "y": 5})
    c.say({"t": "pen", "e": "down", "x": 100, "y": 50})
    c.say({"t": "pen", "e": "up", "x": 100, "y": 50})
    c.say({"t": "key", "e": "down", "sc": 0x94})
    c.say({"t": "key", "e": "up", "sc": 0x94})
    c.say({"t": "char", "c": 0xa3})
    t0 = time.time()
    while len(psion.inputs) < 5 and time.time() - t0 < 3:
        time.sleep(0.02)
    check(psion.inputs == [(1, 100, 50), (3, 100, 50), (4, 0x94, 0), (5, 0x94, 0), (6, 0xa3, 0)],
          label + ": input from the viewer with control (%r)" % psion.inputs)
    check(psion.viewers == (3, 1), label + ": VIEWERS with control (%r)" % (psion.viewers,))
    for x in (v, slow, c):
        x.close()
    # a used token is refused
    tok = json.load(urllib.request.urlopen("http://127.0.0.1:%d/screen/token" % http))["t"]
    s1 = socket.create_connection(("127.0.0.1", v.port))
    s1.sendall(("GET /cast?t=%s HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n" % tok).encode())
    first = s1.recv(200)
    s2 = socket.create_connection(("127.0.0.1", v.port))
    s2.sendall(("GET /cast?t=%s HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n" % tok).encode())
    second = s2.recv(200)
    check(first.startswith(b"HTTP/1.1 101") and second.startswith(b"HTTP/1.1 403"), label + ": a token is single use")
    s1.close()
    s2.close()
    psion.send(frame(0x04))                         # BYE
    time.sleep(0.3)


def main():
    hm = os.path.join(HERE, "hostmodem")
    tmp = tempfile.mkdtemp()
    sockpath = os.path.join(tmp, "modem.sock")
    link, view, http = free_port(), free_port(), free_port()
    env = dict(os.environ, HOSTMODEM_CAST="1", HOSTMODEM_CASTPORT=str(link), HOSTMODEM_CASTVIEW=str(view),
               HOSTMODEM_HTTP=str(http), HOSTMODEM_JOIN_MS="10")
    log = open(os.path.join(tmp, "hostmodem.log"), "w")
    proc = subprocess.Popen([hm, sockpath], env=env, stderr=log, stdout=log)
    try:
        for _ in range(100):
            if os.path.exists(sockpath):
                break
            time.sleep(0.05)
        ser = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ser.connect(sockpath)
        time.sleep(0.3)
        check(http_status(http, "/screen") == 200, "the viewer page")
        check(http_status(http, "/screen/manifest.json") == 200 and http_status(http, "/screen/icon.png") == 200, "the Home Screen files")
        # the modem route: ATDT192.168.7.1:5961
        ser.sendall(b"ATE0\r")
        time.sleep(0.2)
        ser.recv(4096)
        ser.sendall(b"ATDT192.168.7.1:5961\r")
        got = b""
        t0 = time.time()
        while b"CONNECT" not in got and time.time() - t0 < 3:
            got += ser.recv(4096)
        check(b"CONNECT" in got, "the modem route answers CONNECT")
        ps = Psion(ser, "Modem 5mx")
        session(ps, http, "modem route")
        t0 = time.time()
        while b"NO CARRIER" not in ps.raw and time.time() - t0 < 3:
            time.sleep(0.05)
        check(b"NO CARRIER" in ps.raw, "BYE ends the modem call (NO CARRIER)")
        # the TCP end (the PPP route's)
        tcp = socket.create_connection(("127.0.0.1", link))
        session(Psion(tcp, "PPP 5mx"), http, "TCP route")
        tcp.close()
    finally:
        proc.terminate()
        proc.wait(3)
        log.close()
    if fails:
        print(open(os.path.join(tmp, "hostmodem.log")).read()[-3000:])
    print("test_cast_e2e: %d checks, %d failed" % (checks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
