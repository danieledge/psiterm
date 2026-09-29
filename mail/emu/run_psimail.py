#!/usr/bin/env python3
"""run_psimail.py - run the Psion-compiled psimail engine (ARM code from the
1999 EPOC GCC) in an ARM emulator on a PC. Based on PsiWeb's run_psiweb.py.

Python stands in for EPOC (emu_rt.c's hypercalls: files, time), PsiMail.app
(commands from the command line, results printed) and psiglue.cpp (the
network: real TCP connections from the PC). Everything else is the exact
ARM code that goes into psimail.exe - IMAP, SMTP, MIME, TLS and the
certificate checks.

    run_psimail.py [--count] [--align] [--trace f1,f2] CMD [ARGS] [, CMD [ARGS]] ...

Commands as mail/host/pmhost.c (folders, sync F, body F UID, attach F UID
PART, flag F UID +S, move F UID [DEST], search F WORDS, send, sendrecv,
trustlast, older F, full F UID, hangup). The account comes from PM_*
variables as for psimail-host; files go under $PM_EMU_ROOT
(/tmp/psimailemu): C:\ is $PM_EMU_ROOT/C.
"""
import os, re, sys, time, socket, select, struct, math
import pefile
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_BLOCK
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get("PM_BUILD", os.path.join(HERE, "../../build/mail-epoc"))
args = sys.argv[1:]
COUNT = "--count" in args
OPTVALS = [args[i + 1] for i, a in enumerate(args) if a in ("--trace",) and i + 1 < len(args)]
WORDS = [a for a in args if not a.startswith("--") and a not in OPTVALS]

MAP = open(os.path.join(BUILD, "psimail-emu.map")).read()
def sym(name):
    m = re.search(r"0x([0-9a-f]+)\s+_?%s\s*$" % re.escape(name), MAP, re.M)
    return int(m.group(1), 16)

pe = pefile.PE(os.path.join(BUILD, "psimail-emu.pe"))
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
ROOT = os.environ.get("PM_EMU_ROOT", "/tmp/psimailemu")
state = {"exit": None}
insns = [0]
files, next_fd = {}, [10]
mem = {"in_use": 0, "peak": 0, "arena": 0, "n": 0}
RES = ["OK", "FAILED", "OFFLINE", "CANCELLED", "UNTRUSTED", "NEED_PASS", "LOGIN_FAILED"]
OPS = {"folders": 1, "sync": 2, "older": 3, "body": 4, "full": 5, "attach": 6, "flag": 7, "move": 8,
       "search": 9, "send": 10, "sendrecv": 11, "hangup": 12, "trust": 13, "trustlast": 13, "expunge": 14,
       "cal": 15, "calendars": 15}
cmds, cur = [], []
for w in WORDS + [","]:
    if w == ",":
        if cur: cmds.append(cur)
        cur = []
    else:
        cur.append(w)
results = []
env = os.environ.get
CONFIG = {
    "store_dir": "C:\\PsiMail\\", "attach_dir": "C:\\PsiMail\\Attachments\\",
    "offline": env("PM_OFFLINE", "0"), "name": "Test", "fullname": env("PM_NAME", "Test User"),
    "email": env("PM_EMAIL", env("PM_USER", "test@example.com")),
    "imap_host": env("PM_HOST", "127.0.0.1"), "imap_port": env("PM_PORT", "993"), "imap_tls": env("PM_TLS", "1"),
    "smtp_host": env("PM_SMTP", env("PM_HOST", "127.0.0.1")), "smtp_port": env("PM_SMTP_PORT", "465"),
    "smtp_tls": env("PM_SMTP_TLS", "1"), "user": env("PM_USER", "test@example.com"), "pass": env("PM_PASS", ""),
    "cal_host": env("PM_CAL_HOST", "caldav.fastmail.com"), "cal_port": env("PM_CAL_PORT", "443"),
    "cal_plain": env("PM_CAL_PLAIN", "0"), "cal_path": env("PM_CAL_PATH", ""), "cal_zone": env("PM_CAL_ZONE", "1"),
    "cal_back": env("PM_CAL_BACK", "30"), "cal_ahead": env("PM_CAL_AHEAD", "180"),
    "sync_count": env("PM_COUNT", "50"), "max_body_kb": env("PM_BODY_KB", "64"), "save_sent": env("PM_SAVE_SENT", "1"),
}

def log(*a):
    print("[%6.2fs]" % (time.time() - T0), *a, flush=True)

def host_path(p):
    p = p.replace("\\", "/")
    if len(p) > 1 and p[1] == ":":
        p = os.path.join(ROOT, p[0].upper()) + p[2:]
    return p

# ------------------------------------------------------------------ network
net = {"sock": None, "rx": bytearray(), "closed": False}
def rx_fill(timeout):
    s = net["sock"]
    if net["rx"] or s is None or net["closed"]:
        return
    r, _, _ = select.select([s], [], [], timeout)
    if r:
        d = s.recv(8192)
        if d: net["rx"] += d
        else: net["closed"] = True

def pg_dial(host, port, why, maxlen):
    if net["sock"]:
        net["sock"].close()
    net.update(sock=None, rx=bytearray(), closed=False)
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
        t = time.time(); wr(a, struct.pack("<ii", int(t), int((t % 1) * 1e6))); return 0
    if op == 4:                                      # fopen
        path, mode = host_path(cstr(a)), cstr(b)
        try:
            if "w" in mode or "a" in mode:
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
        else: sys.stdout.write("[out] " + data.decode("latin-1")); sys.stdout.flush()
        return c
    if op == 8:                                      # fseek
        files[a].seek(s32(b), c); return 0
    if op == 9:                                      # fgetc
        ch = files[a].read(1); return ch[0] if ch else u32(-1)
    if op == 26:                                     # ftell
        return files[a].tell()
    if op == 27:                                     # log
        log("[rt]", cstr(a)); return 0
    if op == 10:                                     # mkdir
        try: os.makedirs(host_path(cstr(a)), exist_ok=True); return 0
        except OSError: return u32(-1)
    if op == 11: return 0                            # getenv
    if op == 25:                                     # memory report
        mem.update(in_use=a, peak=b, arena=c, n=d); return 0
    if op == 28:                                     # unlink
        try: os.unlink(host_path(cstr(a))); return 0
        except OSError: return u32(-1)
    if op == 29:                                     # rename
        try: os.rename(host_path(cstr(a)), host_path(cstr(b))); return 0
        except OSError: return u32(-1)
    # ---- the app's side
    if op == 400:                                    # config value
        k = cstr(a)
        if k not in CONFIG: return 0
        wr(b, CONFIG[k].encode("latin-1")[:c - 1] + b"\0"); return 1
    if op == 401:                                    # next command
        if not cmds: return 0
        w = cmds.pop(0)
        op_ = OPS[w[0]]
        rest = w[1:] + ["", "", ""]
        folder, uid, arg = rest[0], 0, ""
        if w[0] in ("search",): arg = rest[1]
        elif w[0] in ("trust",): arg, folder = rest[0], ""
        elif w[0] == "trustlast": folder = ""
        elif w[0] in ("cal", "calendars"): folder, arg = "", ("list" if w[0] == "calendars" else "")
        else:
            uid = int(rest[1] or 0); arg = rest[2]
        log(">", " ".join(w))
        wr(a, struct.pack("<I", uid)); wr(b, folder.encode("latin-1") + b"\0"); wr(c, arg.encode("latin-1") + b"\0")
        state["cmd"] = w
        return op_
    if op == 402:                                    # result
        msg = cstr(b)
        log("%s: %s %s" % (state["cmd"][0], RES[a] if a < len(RES) else a, msg))
        if a == 4: log("  untrusted %s key %s" % (cstr(c), cstr(d)))
        results.append(a); return 0
    if op == 403: return int((time.time() - T0) * 1000) & 0xffffffff
    if op == 404: return int(os.environ.get("PM_NOW", time.time())) & 0xffffffff
    if op == 405:                                    # list dir
        dirp, suf = host_path(cstr(a)), cstr(b)
        try: names = sorted(n for n in os.listdir(dirp) if n.endswith(suf))
        except OSError: names = []
        blob = b"".join(n.encode("latin-1") + b"\0" for n in names)[:d]
        wr(c, blob); return len(names)
    if op == 406:
        import shutil; shutil.rmtree(host_path(cstr(a)), ignore_errors=True); return 0
    if op == 407: log("[log]", cstr(a), ("@%.2fM insns" % (insns[0] / 1e6)) if COUNT else ""); return 0
    if op == 408:
        try: os.makedirs(host_path(cstr(a)), exist_ok=True); return 0
        except OSError: return u32(-1)
    # ---- psiglue
    if op == 300: return pg_dial(cstr(a), b, c, d)
    if op == 301:
        if net["sock"]: net["sock"].close()
        net.update(sock=None, closed=True); log("hang up"); return 0
    if op == 302: rx_fill(0); return len(net["rx"])
    if op == 303:
        n = min(b, len(net["rx"])); wr(a, bytes(net["rx"][:n])); del net["rx"][:n]; return n
    if op == 304:
        if not net["sock"]: return u32(-1)
        try: net["sock"].sendall(rd(a, b))
        except OSError: net["closed"] = True; return u32(-1)
        return b
    if op == 305:                                    # wait(ms, net, kbd)
        ms = s32(a)
        if not b: time.sleep(max(ms, 0) / 1000.0); return 0
        if net["rx"] or net["closed"]: return 1
        rx_fill(60 if ms < 0 else ms / 1000.0)
        return 1 if (net["rx"] or net["closed"]) else 0
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
    PROF = {} if "--prof" in args else None
    def on_block(uc_, addr, size, user):
        insns[0] += size >> 2
        if PROF is not None: PROF[addr] = PROF.get(addr, 0) + (size >> 2)
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

# --stack: the deepest the stack went (psimail.exe gets 64 KB on the Psion)
if "--stack" in args:
    low = [STACK + 0x100000]
    def on_blk(uc_, addr, size, user):
        sp = uc.reg_read(UC_ARM_REG_SP)
        if sp < low[0]: low[0] = sp
    uc.hook_add(UC_HOOK_BLOCK, on_blk)
    import atexit
    atexit.register(lambda: print("[stack] deepest use %d bytes" % (STACK + 0x100000 - 64 - low[0])))

argv0 = SCRATCH + 0x100
wr(argv0, b"psimail\0")
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
if COUNT and PROF:
    syms = sorted((int(m.group(1), 16), m.group(2).split("(")[0]) for m in re.finditer(r"^\s+0x([0-9a-f]+)\s+([A-Za-z_].*?)\s*$", MAP, re.M))
    import bisect
    keys = [a for a, _ in syms]
    per = {}
    for a, n in PROF.items():
        i = bisect.bisect_right(keys, a) - 1
        nm = syms[i][1] if i >= 0 else "?"
        per[nm] = per.get(nm, 0) + n
    for nm, n in sorted(per.items(), key=lambda x: -x[1])[:25]:
        log("%8.2fM %5.1f%% %s" % (n / 1e6, 100.0 * n / insns[0], nm))
sys.exit(0 if state["exit"] == 0 and all(r == 0 for r in results) else 1)
