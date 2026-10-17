#!/usr/bin/env python3
"""cast/test/fakeatom.py - the Atom's end of PsiCast's link A, for tests.

Stands in for the Atom modem with PsiCast firmware: a Hayes modem on a unix
socket (what the emulator's serial bridge connects to, see tools/emu/run.sh
EMU_SERIAL) or on a TCP port, which answers ATDT192.168.7.1:5961 with
CONNECT and then speaks link A (cast/docs/PROTOCOL.md): WELCOME (with the
viewer's address if --url), VIEWERS, ACKs, PINGs (with the last update
applied), KEYREQ, and the viewer's INPUT; it checks HELLO and DEVINFO.
Every UPDATE is decoded with the real codec (cast/codec/pccodec.c, built
here as a shared library and used through ctypes) and the screen written
out as PNG, to compare with the emulator's own screenshot.

    python3 cast/test/fakeatom.py --unix /tmp/x/sock --out build/cast-atom
    python3 cast/test/fakeatom.py --tcp 7777 ...

  --out DIR        where screen-NNNN.png, last.png and atom.log go
  --viewers N      the VIEWERS count to report (default 1)
  --control        say the viewer has control (VIEWERS' second count)
  --url TEXT       put TEXT (the viewer's address) after WELCOME's fields
  --max-payload N  the largest UPDATE payload WELCOME allows (0: 65536)
  --keyreq-at N    send KEYREQ after the Nth update (tests keyframes)
  --input FILE     viewer input to send, one event a line:
                     T pen down|move|up X Y     T key down|up SCANCODE
                     T char TEXT
                   T is in seconds from the first STATUS that says control is
                   allowed (the Psion drops input before that anyway)
  --png-every N    write every Nth decoded screen (default 10; 0 only last)
  --corrupt-at N   flip a bit in the Nth message to the Psion (tests resync)
  --old-ping       PING with the token only, as an older Atom (the Psion then
                   falls back on taking a long-stalled window as acknowledged)
  --drop-ack N     leave out the ACK of the Nth update (the next PING, which
                   carries the last update applied, must stand in for it)
  --ping S         PING every S seconds (default 5, as the Atom; the emulator
                   runs up to 10 times faster than real time when idle, so
                   tests there use 1)
  --sim-clock      count --input's T in the Psion's own time (4 s a STATUS)
                   rather than this computer's (for the emulator)
  --answer-status  answer every STATUS with VIEWERS, so the Psion hears from
                   the Atom at least every 5 s of its own time however fast
                   the emulator runs (idle, it runs some 30 times real time)
  --stamps         ask a Psion that can (HELLO bit2) to stamp each UPDATE with
                   its capture time (WELCOME bit1), and log every update's
                   stamp in the Psion's own seconds (PROTOCOL.md "Latency"):
                   in the emulator, against the time a key was pressed

It exits when the Psion says BYE or hangs up (+++ ATH), or on Ctrl+C; the
summary line at the end of atom.log says what came. Plain HTTP, no secrets,
listens on a unix socket or on 127.0.0.1 only.
"""
import argparse, ctypes, os, select, socket, struct, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
CODEC = os.path.join(REPO, 'cast', 'codec')

T_HELLO, T_UPDATE, T_STATUS, T_BYE, T_DEVINFO = 0x01, 0x02, 0x03, 0x04, 0x05
T_WELCOME, T_ACK, T_KEYREQ, T_VIEWERS, T_INPUT, T_PING = 0x81, 0x82, 0x83, 0x84, 0x85, 0x86
NAMES = {1: 'HELLO', 2: 'UPDATE', 3: 'STATUS', 4: 'BYE', 5: 'DEVINFO'}


def codec_lib(outdir):
    """The codec as a shared library (rebuilt when pccodec.c is newer)."""
    so = os.path.join(outdir, 'libpccodec.so')
    src = os.path.join(CODEC, 'pccodec.c')
    if not os.path.exists(so) or os.path.getmtime(so) < os.path.getmtime(src):
        subprocess.check_call(['cc', '-O2', '-shared', '-fPIC', '-fno-strict-aliasing', '-I' + CODEC,
                               src, '-lz', '-o', so])
    lib = ctypes.CDLL(so)
    lib.pc_dec_new.restype = ctypes.c_void_p
    lib.pc_dec_new.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.pc_dec_update.restype = ctypes.c_int
    lib.pc_dec_update.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    lib.pc_dec_screen.restype = ctypes.POINTER(ctypes.c_ubyte)
    lib.pc_dec_screen.argtypes = [ctypes.c_void_p]
    lib.pc_dec_stride.restype = ctypes.c_int
    lib.pc_dec_stride.argtypes = [ctypes.c_void_p]
    lib.pc_dec_free.argtypes = [ctypes.c_void_p]
    return lib


def frame(t, payload=b'', flags=0):
    head = b'PC' + bytes([t, flags]) + struct.pack('<I', len(payload))
    return head + payload + struct.pack('<I', zlib.crc32(head + payload) & 0xffffffff)


def save_png(path, data, w, h, stride):
    from PIL import Image
    im = Image.new('L', (w, h))
    px = im.load()
    for y in range(h):
        row = data[y * stride:(y + 1) * stride]
        for x in range(w):
            b = row[x >> 1]
            v = (b >> 4) if (x & 1) else (b & 15)
            px[x, y] = v * 17
    im.save(path)


class Atom:
    def __init__(self, a, log):
        self.a, self.log = a, log
        self.lib = codec_lib(a.out)
        self.dec = None
        self.w = self.h = 0
        self.rx = b''
        self.updates = self.keyframes = self.bad = self.statuses = 0
        self.bytes_in = 0
        self.sent = 0
        self.welcomed_at = None
        self.control_at = None
        self.sim = 0.0                 # --sim-clock: the Psion's time, from its STATUS
        self.inputs = self.load_input(a.input) if a.input else []
        self.last_ping = time.time()
        self.hello = None
        self.devinfo = None
        self.status = None
        self.bye = False
        self.seq = 0

    @staticmethod
    def load_input(path):
        evs = []
        for line in open(path):
            p = line.split()
            if not p or p[0].startswith('#'):
                continue
            t = float(p[0])
            if p[1] == 'pen':
                kind = {'down': 1, 'move': 2, 'up': 3}[p[2]]
                evs.append((t, [(kind, int(p[3]), int(p[4]))]))
            elif p[1] == 'key':
                evs.append((t, [(4 if p[2] == 'down' else 5, int(p[3], 0), 0)]))
            elif p[1] == 'char':
                text = line.split(None, 2)[2].rstrip('\n')
                text = text.replace('\\n', '\n')
                evs.append((t, [(6, ord(c), 0) for c in text]))
        return sorted(evs, key=lambda e: e[0])

    def send(self, conn, t, payload=b'', flags=0):
        b = frame(t, payload, flags)
        self.sent += 1
        if self.a.corrupt_at and self.sent == self.a.corrupt_at:
            b = bytearray(b)
            b[len(b) // 2] ^= 0x10
            b = bytes(b)
            self.log('corrupted message %d to the Psion' % self.sent)
        # paced, as the real modems: the emulator's UART takes 4 KB at most
        for i in range(0, len(b), 256):
            conn.sendall(b[i:i + 256])
            if len(b) > 256:
                time.sleep(0.05)

    def handle(self, conn, t, flags, p):
        if t == T_HELLO:
            ver, w, h, fmt, _, caps = struct.unpack('<HHHBBI', p[:12])
            name = p[12:].decode('utf-8', 'replace')
            self.hello = (ver, w, h, fmt, caps, name)
            self.log('HELLO version %d, %dx%d, format %d, capabilities %x, "%s"' % (ver, w, h, fmt, caps, name))
            if not (0 < w <= 640 and 0 < h <= 480 and w % 2 == 0):
                self.log('bad size: refusing')
                return False
            if self.dec:
                self.lib.pc_dec_free(self.dec)
            self.w, self.h = w, h
            self.dec = self.lib.pc_dec_new(w, h)
            ext = self.a.url.encode() if self.a.url else b''
            caps = 1 | (2 if self.a.stamps and caps & 4 else 0)
            self.send(conn, T_WELCOME, struct.pack('<HHII', 1, 2, caps, self.a.max_payload) + ext)
            self.send(conn, T_VIEWERS, bytes([self.a.viewers, self.a.viewers if self.a.control else 0]))
            self.welcomed_at = time.time()
        elif t == T_UPDATE:
            seq = struct.unpack('<I', p[:4])[0]
            key = flags & 1
            body = p[4:]
            if flags & 2 and len(p) >= 8:           # (stamped: the capture time, then the body)
                body = p[8:]
                self.log('UPDATE %d captured at %.3f s (Psion), %d bytes' % (seq, struct.unpack('<I', p[4:8])[0] / 1000.0, len(body)))
            r = self.lib.pc_dec_update(self.dec, body, len(body), key, None, None, None) if self.dec else -99
            self.updates += 1
            self.keyframes += key
            if self.a.max_payload and len(p) > self.a.max_payload:
                self.bad += 1
                self.log('UPDATE %d is %d bytes, over the %d allowed' % (seq, len(p), self.a.max_payload))
            if r != 0:
                self.bad += 1
                self.log('UPDATE %d (%d bytes%s) failed to decode: %d - KEYREQ' % (seq, len(body), ', key' if key else '', r))
                self.send(conn, T_KEYREQ)
                return True
            if key or self.updates <= 3 or self.updates % 25 == 0:
                self.log('UPDATE %d: %d bytes%s' % (seq, len(body), ', keyframe' if key else ''))
            self.seq = seq
            if self.a.drop_ack and self.updates == self.a.drop_ack:
                self.log('ACK %d not sent (test: the PING carries it)' % seq)
            else:
                self.send(conn, T_ACK, struct.pack('<I', seq))
            if self.a.png_every and self.updates % self.a.png_every == 0:
                self.png('screen-%04d.png' % self.updates)
            if self.a.keyreq_at and self.updates == self.a.keyreq_at:
                self.log('KEYREQ (test)')
                self.send(conn, T_KEYREQ)
        elif t == T_STATUS:
            batt, mains, ram, ups, cpu, ctl, paused = struct.unpack('<BBIHHBB', p[:12])
            self.statuses += 1
            st = (batt, mains, ram // 1024, ups / 10.0, cpu / 10.0, ctl, paused)
            if st[3:] != (self.status or (0,) * 7)[3:] or self.statuses % 5 == 1:
                self.log('STATUS battery %d%%, mains %d, RAM free %d KB, %.1f updates/s, CPU %.1f%%, control %d, paused %d' % st)
            self.status = st
            if self.a.answer_status:            # (liveness in the emulator: see --answer-status)
                self.send(conn, T_VIEWERS, bytes([self.a.viewers, self.a.viewers if self.a.control else 0]))
            if self.control_at is not None:
                self.sim += 4.0
            if ctl and self.control_at is None:
                self.control_at = time.time()
                self.sim = 0.0
                self.log('control allowed: the input clock starts')
        elif t == T_DEVINFO:
            if len(p) < 4:
                self.log('DEVINFO too short')
                return True
            dw, dh = struct.unpack('<HH', p[:4])
            self.devinfo = (dw, dh)
            self.log('DEVINFO digitiser %dx%d' % (dw, dh))
        elif t == T_BYE:
            self.log('BYE')
            self.bye = True
            return False
        else:
            self.log('unknown message type %02x' % t)
        return True

    def png(self, name):
        if not self.dec:
            return
        stride = self.lib.pc_dec_stride(self.dec)
        data = ctypes.string_at(self.lib.pc_dec_screen(self.dec), stride * self.h)
        save_png(os.path.join(self.a.out, name), data, self.w, self.h, stride)

    def parse(self, conn):
        while len(self.rx) >= 12:
            if self.rx[:2] != b'PC':
                i = self.rx.find(b'PC', 1)
                self.bad += 1
                self.rx = self.rx[i:] if i > 0 else self.rx[-1:]
                continue
            n = struct.unpack('<I', self.rx[4:8])[0]
            if n > 65536:
                self.bad += 1
                self.rx = self.rx[1:]
                continue
            if len(self.rx) < 12 + n:
                return True
            m = self.rx[:12 + n]
            if zlib.crc32(m[:8 + n]) & 0xffffffff != struct.unpack('<I', m[8 + n:])[0]:
                self.bad += 1
                self.log('bad CRC (type %02x, %d bytes): resync' % (m[2], n))
                self.rx = self.rx[1:]
                continue
            self.rx = self.rx[12 + n:]
            if not self.handle(conn, m[2], m[3], m[8:8 + n]):
                return False
        return True

    def online(self, conn, first):
        """link A after CONNECT; returns when the Psion hangs up or says BYE"""
        self.rx = first
        last_rx = time.time()
        while True:
            if not self.parse(conn):
                return
            now = time.time()
            if self.welcomed_at:
                while self.inputs and self.control_at and \
                        (self.sim if self.a.sim_clock else now - self.control_at) >= self.inputs[0][0]:
                    _, evs = self.inputs.pop(0)
                    pl = b''.join(struct.pack('<BBhhH', k, 0, a, b, 0) for k, a, b in evs)
                    self.log('INPUT %s' % ' '.join('%d:%d,%d' % e for e in evs))
                    self.send(conn, T_INPUT, pl)
                if now - self.last_ping > self.a.ping:
                    tok = struct.pack('<I', int(now) & 0xffffffff)
                    # token, then the last UPDATE applied (an older Atom: the token only)
                    self.send(conn, T_PING, tok if self.a.old_ping else tok + struct.pack('<I', self.seq))
                    self.last_ping = now
            if now - last_rx > 20:
                self.log('nothing from the Psion for 20 s: hanging up')
                conn.sendall(b'\r\nNO CARRIER\r\n')
                return
            r, _, _ = select.select([conn], [], [], 0.2)
            if r:
                d = conn.recv(65536)
                if not d:
                    raise EOFError
                self.bytes_in += len(d)
                last_rx = time.time()
                if b'+++' in d and len(d) <= 8:
                    self.log('+++ from the Psion: command mode')
                    conn.sendall(b'\r\nOK\r\n')
                    return
                self.rx += d

    def modem(self, conn):
        """Hayes command mode: echo, OK, ATDT..."""
        buf = b''
        while True:
            d = conn.recv(1024)
            if not d:
                raise EOFError
            conn.sendall(d)                     # echo, like the WiRSa
            buf += d
            while b'\r' in buf:
                line, buf = buf.split(b'\r', 1)
                line = line.strip().decode('latin-1')
                if not line:
                    continue
                self.log('modem: %s' % line)
                u = line.upper()
                if u.startswith('ATDT'):
                    target = line[4:].strip()
                    host, _, port = target.rpartition(':')
                    if port != '5961':
                        conn.sendall(b'\r\nNO CARRIER\r\n')
                        continue
                    conn.sendall(b'\r\nCONNECT 115200\r\n')
                    self.log('CONNECT: link A to %s' % target)
                    self.online(conn, buf)
                    buf = b''
                    if self.bye:
                        return
                elif u.startswith('ATI'):
                    conn.sendall(b'\r\nPsiCast fake Atom\r\nOK\r\n')
                else:
                    conn.sendall(b'\r\nOK\r\n')


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--unix')
    g.add_argument('--tcp', type=int)
    ap.add_argument('--out', default=os.path.join(REPO, 'build', 'cast-atom'))
    ap.add_argument('--viewers', type=int, default=1)
    ap.add_argument('--control', action='store_true')
    ap.add_argument('--url', default='')
    ap.add_argument('--keyreq-at', type=int, default=0)
    ap.add_argument('--input')
    ap.add_argument('--png-every', type=int, default=10)
    ap.add_argument('--corrupt-at', type=int, default=0)
    ap.add_argument('--ping', type=float, default=5.0)
    ap.add_argument('--old-ping', action='store_true')
    ap.add_argument('--max-payload', type=int, default=0)
    ap.add_argument('--drop-ack', type=int, default=0)
    ap.add_argument('--answer-status', action='store_true')
    ap.add_argument('--sim-clock', action='store_true')
    ap.add_argument('--stamps', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    logf = open(os.path.join(a.out, 'atom.log'), 'a')
    t0 = time.time()

    def log(s):
        line = '%7.2f %s' % (time.time() - t0, s)
        print(line, flush=True)
        logf.write(line + '\n')
        logf.flush()

    if a.unix:
        if os.path.exists(a.unix):
            os.unlink(a.unix)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(a.unix)
    else:
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(('127.0.0.1', a.tcp))
    srv.listen(1)
    log('listening on %s' % (a.unix or '127.0.0.1:%d' % a.tcp))
    atom = Atom(a, log)
    try:
        while not atom.bye:
            conn, _ = srv.accept()
            log('serial line connected')
            try:
                atom.modem(conn)
            except (EOFError, ConnectionError):
                log('serial line closed')
            conn.close()
            if atom.bye or a.unix:
                break
    except KeyboardInterrupt:
        pass
    atom.png('last.png')
    log('summary: %d updates (%d keyframes), %d bad, %d STATUS, %d bytes in, hello=%s, devinfo=%s, bye=%s'
        % (atom.updates, atom.keyframes, atom.bad, atom.statuses, atom.bytes_in, atom.hello, atom.devinfo, atom.bye))


if __name__ == '__main__':
    main()
