#!/usr/bin/env python3
"""run_psiweb.py - run the Psion-compiled psiweb (NetSurf + PsiWeb, ARM code
from the 1999 EPOC GCC) in an ARM emulator on a PC.

Python stands in for EPOC (emu_rt.c's hypercalls: files, time, maths),
PsiWeb.app (screen, keys, pen, menu commands - driven by a script) and
psiglue.cpp (the network: real TCP connections from the PC). Everything
else is the exact ARM code that goes into psiweb.exe.

    run_psiweb.py [--count] [--proxy host:port] SCRIPT

Script lines: as fb/pwhost.c (open URL, idle [ms], wait ms, key N,
type TEXT, pen X Y, cmd N [arg], shot FILE.png, quit).
--count counts ARM instructions to estimate the time on a 36 MHz Psion.
"""
import os, re, sys, time, socket, select, struct, math
import pefile
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_BLOCK
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get("PW_BUILD", os.path.join(HERE, "../../build/web-epoc"))
args = sys.argv[1:]
COUNT = "--count" in args
# --modem: behave like the WiRSa modem link on the Psion: the far end
# closing shows up only as "NO CARRIER" in the data, never as a closed
# flag, and pg_net_avail counts only bytes already taken from the port
MODEM = "--modem" in args
PROXY = None
if "--proxy" in args:
    PROXY = args[args.index("--proxy") + 1]
OPTVALS = [args[i + 1] for i, a in enumerate(args) if a in ("--proxy", "--trace") and i + 1 < len(args)]
SCRIPT = [a for a in args if not a.startswith("--") and a not in OPTVALS][0]

MAP = open(os.path.join(BUILD, "psiweb-emu.map")).read()
def sym(name):
    m = re.search(r"0x([0-9a-f]+)\s+_?%s\s*$" % re.escape(name), MAP, re.M)
    return int(m.group(1), 16)

pe = pefile.PE(os.path.join(BUILD, "psiweb-emu.pe"))
base = pe.OPTIONAL_HEADER.ImageBase
uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
end = max(s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData) for s in pe.sections)
uc.mem_map(base, (end + 0x10fff) & ~0xfff)
for s in pe.sections:
    uc.mem_write(base + s.VirtualAddress, s.get_data())
STACK, SCRATCH = 0x30000000, 0x40000000
uc.mem_map(STACK, 0x100000)
uc.mem_map(SCRATCH, 0x10000)
DONE = SCRATCH + 0x8000

def rd(addr, n): return bytes(uc.mem_read(addr, n))
def cstr(addr, maxlen=4096):
    out = b""
    while len(out) < maxlen:
        chunk = rd(addr + len(out), 64 - ((addr + len(out)) & 63))
        if b"\0" in chunk:
            return (out + chunk.split(b"\0", 1)[0]).decode("utf-8", "replace")
        out += chunk
    return out.decode("utf-8", "replace")
def wr(addr, data): uc.mem_write(addr, data)
def u32(x): return x & 0xffffffff
def s32(x): x &= 0xffffffff; return x - (1 << 32) if x & 0x80000000 else x
def dbl_from(w0, w1): return struct.unpack(">d", struct.pack(">II", w0 & 0xffffffff, w1 & 0xffffffff))[0]
def dbl_words(v): return struct.unpack(">II", struct.pack(">d", v))

# ------------------------------------------------------------------ state
T0 = time.time()
state = {"exit": None, "busy": 0, "last_draw": time.time(), "fb4": 0, "cmd": 0, "arg": "",
         "wait_until": 0, "idle": None, "queue": [], "script": open(SCRIPT).read().splitlines(),
         "ready": False}
insns = [0]
files, next_fd = {}, [10]
mem = {"in_use": 0, "peak": 0, "arena": 0, "n": 0}

def log(*a):
    print("[%6.2fs]" % (time.time() - T0), *a, flush=True)

def shot(name):
    from PIL import Image
    fb = rd(state["fb4"], 240 * 320)
    im = Image.new("L", (640, 240))
    px = im.load()
    for y in range(240):
        row = fb[y * 320:(y + 1) * 320]
        for x in range(640):
            v = row[x >> 1]
            v = (v >> 4) if x & 1 else (v & 15)
            px[x, y] = v * 17
    im.resize((1280, 480), Image.NEAREST).save(name)
    log("saved", name)

def script_step():
    """runs script lines until one produces input or has to wait"""
    while state["script"] and not state["queue"] and not state["cmd"]:
        t = time.time()
        if state["wait_until"] and t < state["wait_until"]:
            return
        state["wait_until"] = 0
        if state["idle"] is not None:
            if state["busy"] or not state["ready"] or t - state["last_draw"] < state["idle"]:
                return
            state["idle"] = None
        line = state["script"].pop(0).strip()
        if not line or line.startswith("#"):
            continue
        log(">", line)
        cmd, _, rest = line.partition(" ")
        if cmd == "open":
            state["cmd"], state["arg"] = 1, rest
            state["busy"] = 1; state["last_draw"] = t
        elif cmd == "cmd":
            n, _, a = rest.partition(" ")
            state["cmd"], state["arg"] = int(n), a
            state["last_draw"] = t
        elif cmd == "idle":
            state["idle"] = (int(rest) if rest else 1500) / 1000.0
        elif cmd == "wait":
            state["wait_until"] = t + int(rest) / 1000.0
        elif cmd == "key":
            state["queue"] += [(1, int(rest), 0, 0), (2, int(rest), 0, 0)]
        elif cmd == "type":
            for ch in rest:
                state["queue"] += [(1, 0x10000 + ord(ch), 0, 0), (2, 0x10000 + ord(ch), 0, 0)]
        elif cmd == "pen":
            x, y = map(int, rest.split())
            state["queue"] += [(3, 0, x, y), (1, 401, x, y), (2, 401, x, y)]
        elif cmd == "shot":
            shot(rest)
        elif cmd == "quit":
            state["queue"].append((4, 0, 0, 0))
    if not state["script"] and not state["queue"] and not state["cmd"] and state["exit"] is None:
        state["queue"].append((4, 0, 0, 0))

# ------------------------------------------------------------------ network
net = {"sock": None, "rx": bytearray(), "closed": False, "eof": False}
def rx_fill(timeout):
    s = net["sock"]
    if net["rx"] or s is None or net["closed"] or net["eof"]:
        if MODEM and not net["rx"] and timeout > 0:
            time.sleep(min(timeout, 0.05))
        return
    r, _, _ = select.select([s], [], [], timeout)
    if r:
        d = s.recv(8192)
        if d: net["rx"] += d
        elif MODEM:
            net["rx"] += b"\r\nNO CARRIER\r\n"; net["eof"] = True
            log("(modem: NO CARRIER)")
        else: net["closed"] = True

def pg_dial(host, port, why, maxlen):
    if net["sock"]:
        net["sock"].close()
    net.update(sock=None, rx=bytearray(), closed=False, eof=False)
    log("dial %s:%d" % (host, port))
    try:
        net["sock"] = socket.create_connection((host, port), timeout=15)
        net["sock"].settimeout(None)
        return 0
    except OSError as e:
        wr(why, ("could not connect: %s" % e).encode()[:maxlen - 1] + b"\0")
        return u32(-1)

# ------------------------------------------------------------------ hypercalls
def hc(op, a, b, c, d):
    if op == 1:                                      # exit
        state["exit"] = s32(a); uc.emu_stop(); return 0
    if op == 3:                                      # gettimeofday
        t = time.time()
        # PW_COARSE=1: whole seconds only, like the Psion's RTC (TTime and
        # gettimeofday have no finer step there)
        wr(a, struct.pack("<ii", int(t), 0 if os.environ.get("PW_COARSE") else int((t % 1) * 1e6))); return 0
    if op == 4:                                      # fopen
        path, mode = cstr(a), cstr(b)
        path = path.replace("\\", "/")
        if path.startswith("C:"): path = "/tmp/psiwebemu" + path[2:]
        try:
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
            f = open(path, mode.replace("t", "") + ("b" if "b" not in mode else ""))
        except OSError:
            return u32(-1)
        fd = next_fd[0]; next_fd[0] += 1; files[fd] = f; return fd
    if op == 5:                                      # fclose
        f = files.pop(a, None)
        if f: f.close()
        return 0
    if op == 6:                                      # fread
        data = files[a].read(c); wr(b, data); return len(data)
    if op == 7:                                      # fwrite
        data = rd(b, c)
        if a in files: files[a].write(data)
        else: sys.stdout.write("[psiweb] " + data.decode("latin-1")); sys.stdout.flush()
        return c
    if op == 8:                                      # fseek
        files[a].seek(s32(b), c); return 0
    if op == 9:                                      # fgetc
        ch = files[a].read(1); return ch[0] if ch else u32(-1)
    if op == 27:                                     # ftell
        return files[a].tell()
    if op == 10: return 0                            # mkdir
    if op == 11: return 0                            # getenv
    if op == 20:                                     # maths
        x = dbl_from(b, c)
        f = {1: math.sin, 2: math.cos, 3: math.ceil, 4: math.floor, 5: math.sqrt}[a]
        w0, w1 = dbl_words(float(f(x)))
        wr(d + 4, struct.pack("<I", w1)); return w0
    if op == 21:                                     # strtod
        s = cstr(a, 64)
        m = re.match(r"\s*[-+]?(\d+\.?\d*([eE][-+]?\d+)?|\.\d+([eE][-+]?\d+)?)", s)
        v = float(m.group(0)) if m else 0.0
        w0, w1 = dbl_words(v); wr(b, struct.pack("<II", w0, w1))
        return m.end() if m else 0
    if op == 22:                                     # localtime / gmtime
        tm = (time.gmtime if c else time.localtime)(s32(a))
        wr(b, struct.pack("<9i", tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon - 1,
                          tm.tm_year - 1900, (tm.tm_wday + 1) % 7, tm.tm_yday - 1, 0)); return 0
    if op == 23:                                     # mktime
        v = struct.unpack("<9i", rd(a, 36))
        return u32(int(time.mktime((v[5] + 1900, v[4] + 1, v[3], v[2], v[1], v[0], 0, 0, -1))))
    if op == 24:                                     # strftime
        v = struct.unpack("<9i", rd(d, 36))
        t = (v[5] + 1900, v[4] + 1, v[3], v[2], v[1], v[0], (v[6] + 6) % 7, v[7] + 1, 0)
        out = time.strftime(cstr(c), t).encode()[:b - 1]
        wr(a, out + b"\0"); return len(out)
    if op == 25:                                     # memory report
        mem.update(in_use=a, peak=b, arena=c, n=d)
        log("[heap] in use %d KB, peak %d KB" % (a // 1024, b // 1024)); return 0
    # ---- pwback
    if op == 200: state["fb4"] = a; return 0
    if op == 201: state["last_draw"] = time.time(); return 0
    if op == 202:                                    # next event
        end_t = time.time() + (s32(b) if s32(b) >= 0 else 100) / 1000.0
        while True:
            script_step()
            if state["queue"]:
                t, code, x, y = state["queue"].pop(0)
                wr(a, struct.pack("<4i", t, code, x, y)); return 1
            if state["cmd"]:
                wr(a, struct.pack("<4i", 5, 0, 0, 0)); return 1          # PWB_WAKE
            if time.time() >= end_t: return 0
            time.sleep(0.005)
    if op == 203:                                    # take command
        c_ = state["cmd"]
        if c_:
            wr(a, state["arg"].encode()[:b - 1] + b"\0"); state["cmd"] = 0
        return c_
    if op == 204: log("[status]", cstr(a)); return 0
    if op == 205: log("[title]", cstr(a)); return 0
    if op == 206: log("[url]", cstr(a)); return 0
    if op == 207:
        state["busy"] = a; state["last_draw"] = time.time()
        if a:
            state["insn0"] = insns[0]
            log("[busy] 1")
        else:
            extra = ""
            if COUNT and "insn0" in state:
                n = insns[0] - state["insn0"]
                extra = " (page: %.1fM instructions, ~%.1f s on a 5mx)" % (n / 1e6, n / 15e6)
            log("[busy] 0" + extra)
        return 0
    if op == 208: state["ready"] = True; log("ready"); return 0
    if op == 209: return int((time.time() - T0) * 1000) & 0xffffffff
    if op == 210:                                    # config
        if PROXY:
            h, _, p = PROXY.partition(":")
            wr(a, struct.pack("<i", 1)); wr(b, h.encode() + b"\0"); wr(c, struct.pack("<i", int(p or 8080)))
        wr(d, struct.pack("<i", 0 if MODEM else 1))  # modem: in-band NO CARRIER
        return 0
    if op == 211:                                    # update config
        src = os.environ.get("PW_UPD", "")           # "host:port" = local server
        if src:
            h, _, p = src.partition(":")
            wr(a, struct.pack("<i", 1)); wr(b, h.encode() + b"\0"); wr(c, struct.pack("<i", int(p or 8686)))
        wr(d, os.environ.get("PW_VERSION", "0.1").encode() + b"\0")
        return 0
    # ---- psiglue
    if op == 300: return pg_dial(cstr(a), b, c, d)
    if op == 301:
        if net["sock"]: net["sock"].close()
        net.update(sock=None, closed=True); log("hang up"); return 0
    if op == 302:
        if not MODEM: rx_fill(0)                     # EPOC: only what was read already
        return len(net["rx"])
    if op == 303:
        n = min(b, len(net["rx"])); wr(a, bytes(net["rx"][:n])); del net["rx"][:n]; return n
    if op == 304:
        if not net["sock"]: return u32(-1)
        net["sock"].sendall(rd(a, b)); return b
    if op == 305:                                    # wait(ms, net, kbd)
        ms = s32(a)
        if not b: time.sleep(max(ms, 0) / 1000.0); return 0
        if net["rx"] or net["closed"]: return 1
        rx_fill(60 if ms < 0 else ms / 1000.0)
        return 1 if (net["rx"] or net["closed"]) else 0     # (never 'closed' in modem mode)
    if op == 306:
        data = os.urandom(min(b, 64)); wr(a, data); return len(data)
    log("unknown hypercall", op)
    return 0

SYMS = sorted((int(m.group(1), 16), m.group(2)) for m in re.finditer(r"0x([0-9a-f]+)\s+(\S+)\s*$", MAP, re.M))
def where(pc):
    import bisect
    i = bisect.bisect_right(SYMS, (pc, "\xff")) - 1
    return "%s+%x" % (SYMS[i][1], pc - SYMS[i][0]) if i >= 0 else "?"
HC_ADDR = sym("emu_hc")
def on_hc(uc_, addr, size, user):
    r = [uc.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    sp = uc.reg_read(UC_ARM_REG_SP)
    d = struct.unpack("<I", rd(sp, 4))[0]
    ret = hc(r[0], r[1], r[2], r[3], d)
    uc.reg_write(UC_ARM_REG_R0, u32(ret))
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
def on_hc_safe(uc_, addr, size, user):
    try:
        on_hc(uc_, addr, size, user)
    except Exception as e:
        import traceback; traceback.print_exc()
        state["exit"] = -998; uc.emu_stop()
uc.hook_add(UC_HOOK_CODE, on_hc_safe, begin=HC_ADDR, end=HC_ADDR)
def on_done(uc_, addr, size, user):
    state["exit"] = s32(uc.reg_read(UC_ARM_REG_R0)); uc.emu_stop()
uc.hook_add(UC_HOOK_CODE, on_done, begin=DONE, end=DONE)
if COUNT:
    def on_block(uc_, addr, size, user):
        insns[0] += size >> 2
    uc.hook_add(UC_HOOK_BLOCK, on_block)

# --trace f1,f2: log calls and return values of these functions
if "--trace" in args:
    for fn in args[args.index("--trace") + 1].split(","):
        def mk(fn):
            def on_call(uc_, addr, size, user):
                a = [uc.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
                lr = uc.reg_read(UC_ARM_REG_LR)
                log("call %s(%s) from %s" % (fn, ", ".join("%x" % v for v in a), where(lr)))
                h = []
                def on_ret(uc2, addr2, size2, user2):
                    log("  %s returned %d" % (fn, s32(uc.reg_read(UC_ARM_REG_R0))))
                    uc.hook_del(h[0])
                h.append(uc.hook_add(UC_HOOK_CODE, on_ret, begin=lr, end=lr))
            return on_call
        uc.hook_add(UC_HOOK_CODE, mk(fn), begin=sym(fn), end=sym(fn))

# --allocs: which functions allocate the memory that is still in use at exit
if "--allocs" in args:
    live, bysite = {}, {}
    pending = []
    def on_malloc(uc_, addr, size, user):
        n = uc.reg_read(UC_ARM_REG_R0) if addr != sym("calloc") else uc.reg_read(UC_ARM_REG_R0) * uc.reg_read(UC_ARM_REG_R1)
        lr = uc.reg_read(UC_ARM_REG_LR)
        pending.append((n, where(lr).split("+")[0]))
        h = []
        def on_ret(uc2, a2, s2, u2):
            p = uc.reg_read(UC_ARM_REG_R0)
            n_, site = pending.pop()
            live[p] = (n_, site)
            uc.hook_del(h[0])
        h.append(uc.hook_add(UC_HOOK_CODE, on_ret, begin=lr, end=lr))
    def on_free(uc_, addr, size, user):
        live.pop(uc.reg_read(UC_ARM_REG_R0), None)
    uc.hook_add(UC_HOOK_CODE, on_malloc, begin=sym("malloc"), end=sym("malloc"))
    uc.hook_add(UC_HOOK_CODE, on_free, begin=sym("free"), end=sym("free"))
    import atexit
    def report():
        tot = {}
        for n, site in live.values():
            tot[site] = tot.get(site, 0) + n
        print("live memory by allocating function:")
        for site, n in sorted(tot.items(), key=lambda x: -x[1])[:25]:
            print("  %8d KB  %s" % (n // 1024, site))
    atexit.register(report)

# --align: report unaligned word/halfword accesses. The Psion's ARM710T
# does not fault on them - a word load returns rotated data - so such code
# runs wrongly on the Psion while working in this emulator.
if "--align" in args:
    from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
    seen = {}
    def on_mem(uc_, access, address, size, value, user):
        if (size == 4 and address & 3) or (size == 2 and address & 1):
            pc = uc.reg_read(UC_ARM_REG_PC)
            if pc not in seen:
                seen[pc] = 1
                log("UNALIGNED %s size %d at %08x, pc %s" % ("write" if access == 17 else "read", size, address, where(pc)))
    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, on_mem)

argv0 = SCRATCH + 0x100
wr(argv0, b"psiweb\0")
wr(SCRATCH + 0x200, struct.pack("<II", argv0, 0))
wr(DONE, b"\x00\x00\xa0\xe1")                        # nop
uc.reg_write(UC_ARM_REG_SP, STACK + 0x100000 - 64)
uc.reg_write(UC_ARM_REG_R0, 1)
uc.reg_write(UC_ARM_REG_R1, SCRATCH + 0x200)
uc.reg_write(UC_ARM_REG_LR, DONE)
SAMPLE = "--sample" in args
try:
    pc = sym("main")
    while state["exit"] is None:
        # run in slices so --sample can show where the time goes
        uc.emu_start(pc, DONE + 4, timeout=2000000 if SAMPLE else 0)
        if state["exit"] is not None:
            break
        pc = uc.reg_read(UC_ARM_REG_PC)
        if SAMPLE:
            log("sample pc", where(pc), "lr", where(uc.reg_read(UC_ARM_REG_LR)))
except UcError as e:
    pc = uc.reg_read(UC_ARM_REG_PC)
    near = max((int(m.group(1), 16), m.group(2)) for m in re.finditer(r"0x([0-9a-f]+)\s+(\S+)\s*$", MAP, re.M)
               if int(m.group(1), 16) <= pc)
    log("CRASH %s at pc=%08x (%s+%x) lr=%08x" % (e, pc, near[1], pc - near[0], uc.reg_read(UC_ARM_REG_LR)))
    state["exit"] = -999
log("exit %s; heap in use %d KB, peak %d KB, arena %d KB, %d allocations" %
    (state["exit"], mem["in_use"] // 1024, mem["peak"] // 1024, mem["arena"] // 1024, mem["n"]))
if COUNT:
    log("%d million ARM instructions: roughly %.1f s on a 36 MHz Psion 5mx (at ~15 MIPS)" %
        (insns[0] // 1000000, insns[0] / 15e6))
sys.exit(0 if state["exit"] == 0 else 1)
