#!/usr/bin/env python3
"""Run the Psion-compiled psissh (Dropbear + shim, built by the 1999 EPOC GCC)
inside an ARM emulator, against a real SSH server via fakemodem.py.

Python stands in for: PsiTerm (keyboard/screen rings), psiglue.cpp (serial
port, dialling, entropy) and the user typing. Everything else is the exact
ARM code that will run on the Psion 5mx."""
import os, re, sys, time, socket, select, struct
import pefile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_BLOCK
from unicorn.arm_const import *

HOST, PORT, USER, PASSWORD = "127.0.0.1", int(os.environ.get("PSI_PORT", "2222")), "psitest", os.environ["PSI_TEST_PASS"]
HOME = "/tmp/psiemu"
COUNT = "--count" in sys.argv

MAP = open("emu/psissh-emu.map").read()
def sym(name):
    return int(re.search(r"0x([0-9a-f]+)\s+%s\s*$" % re.escape(name), MAP, re.M).group(1), 16)

pe = pefile.PE("emu/psissh-emu.pe")
base = pe.OPTIONAL_HEADER.ImageBase
uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
try:
    from unicorn import UC_CPU_ARM_926
    uc.ctl_set_cpu_model(UC_CPU_ARM_926)
except Exception:
    pass
size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xfff) & ~0xfff
uc.mem_map(base, size + 0x10000)
for s in pe.sections:
    uc.mem_write(base + s.VirtualAddress, s.get_data())
STACK, SCRATCH = 0x30000000, 0x40000000
uc.mem_map(STACK, 0x40000)
uc.mem_map(SCRATCH, 0x100000)
scratch_top = [SCRATCH]
def salloc(data):
    a = scratch_top[0]
    uc.mem_write(a, data)
    scratch_top[0] += (len(data) + 8) & ~7
    return a
def cstr(addr, maxlen=512):
    b = bytes(uc.mem_read(addr, maxlen))
    return b.split(b"\0", 1)[0].decode("latin-1")

# PsiShared, laid out exactly as the Psion compiler lays it out
SHARED = salloc(b"\0" * 19400)
SAVEDPW = "--savedpw" in sys.argv
if SAVEDPW:
    uc.mem_write(SHARED + 19312, PASSWORD.encode() + b"\0")
uc.mem_write(SHARED + 36, struct.pack("<i", PORT))
uc.mem_write(SHARED + 40, HOST.encode() + b"\0")
uc.mem_write(SHARED + 168, USER.encode() + b"\0")
uc.mem_write(SHARED + 232, b"ATDT\0")
HOME_PTR = salloc(HOME.encode() + b"\0")
if "--bench" in sys.argv:
    uc.mem_write(SHARED + 19308, struct.pack("<i", 1))

# ---------------------------------------------------------------- outside world
ser = None
rx = bytearray()
net_closed = [False]
out_text = []
kbd = bytearray()
env = {}
files = {}
state = {"exit": None, "stage": 0, "t0": time.time(), "quit": False}

stats = {"rx": 0, "out": 0}
def screen(b):
    stats["out"] += len(b)
    s = b.decode("latin-1")
    out_text.append(s)
    sys.stdout.write(s); sys.stdout.flush()

def user_logic():
    """The pretend user watching the Psion's screen."""
    text = "".join(out_text)
    st = state["stage"]
    if st == 0 and "continue connecting" in text:
        kbd.extend(b"y"); state["stage"] = 1
    elif st <= 1 and SAVEDPW and "(saved password)" in text:
        state["stage"] = 2; state["t_login"] = time.time()
    elif st <= 1 and re.search(r"[Pp]assword: ?$", text.rstrip("*")):
        kbd.extend(PASSWORD.encode() + b"\r"); state["stage"] = 2; state["t_login"] = time.time()
    elif st == 2 and text.rstrip().endswith("$"):
        state["t_prompt"] = time.time()
        kbd.extend(b"echo PSION-$((6*7)); tput cols; tput lines; seq 1 1500 | tr '\\n' ' '; echo; echo BULK-END\r"); state["stage"] = 3; stats["rx0"] = stats["rx"]; stats["out0"] = stats["out"]
    elif st == 3 and "PSION-42" in text and "\nBULK-END" in text.replace("\r","") and text.rstrip().endswith("$"):
        stats["rx1"] = stats["rx"]; stats["out1"] = stats["out"]
        kbd.extend(b"exit\r"); state["stage"] = 4

def rx_fill(timeout):
    if rx or ser is None:
        return
    r, _, _ = select.select([ser], [], [], timeout)
    if r:
        d = ser.recv(4096)
        if d: rx.extend(d); stats["rx"] += len(d)
        else: net_closed[0] = True

def pg_wait(ms, want_net, want_kbd):
    t0 = time.time()
    while True:
        user_logic()
        mask = 0
        if want_net and (rx or net_closed[0]): mask |= 1
        if want_kbd and kbd: mask |= 2
        if state["quit"]: mask |= 8
        if mask: return mask
        if ms >= 0 and (time.time() - t0) * 1000 >= ms: return 0
        if want_net: rx_fill(0.03)
        else: time.sleep(0.03)

def read_line(timeout=30):
    line = bytearray(); t0 = time.time()
    while time.time() - t0 < timeout:
        if not rx:
            rx_fill(0.1); continue
        c = rx.pop(0)
        if c in (13, 10):
            if line: return line.decode("latin-1")
            continue
        line.append(c)
    return None

def pg_dial(why_ptr, maxlen):
    ser.sendall(b"\r"); time.sleep(0.3); rx.clear()
    ser.sendall(("ATDT %s:%d\r" % (HOST, PORT)).encode())
    for _ in range(10):
        l = read_line()
        if l is None: msg = "no answer from modem"; break
        if l.startswith("CONNECT"): return 0
        if l.startswith(("NO CARRIER", "ERROR", "BUSY")): msg = l; break
    else:
        msg = "modem did not connect"
    uc.mem_write(why_ptr, msg.encode()[:maxlen - 1] + b"\0")
    return 0xffffffff

def hc(op, a, b, c, d):
    global ser
    if op == 1:                                   # exit
        state["exit"] = a; uc.emu_stop(); return 0
    if op == 3:                                   # gettimeofday(tv)
        t = time.time(); uc.mem_write(a, struct.pack("<ii", int(t), int((t % 1) * 1e6))); return 0
    if op == 4:                                   # fopen
        path = cstr(a).replace("\\", "/"); mode = cstr(b)
        pymode = {"r": "rb", "rb": "rb", "w": "wb", "wb": "wb", "a": "ab", "a+": "a+b", "r+": "r+b"}.get(mode, "rb")
        try:
            f = open(path, pymode)
            if "a+" in pymode: f.seek(0)
        except OSError:
            return 0xffffffff
        h = len(files) + 10; files[h] = f; return h
    if op == 5: files.pop(a).close(); return 0
    if op == 6:                                   # fread(h, buf, n)
        data = files[a].read(c); uc.mem_write(b, data); return len(data)
    if op == 7:                                   # fwrite(h, buf, n)
        data = bytes(uc.mem_read(b, c))
        if a in (-2 & 0xffffffff, -3 & 0xffffffff): screen(data); return c
        files[a].write(data); files[a].flush(); return c
    if op == 8: files[a].seek(struct.unpack("<i", struct.pack("<I", b))[0], c); return 0
    if op == 9:
        ch = files[a].read(1); return ch[0] if ch else 0xffffffff
    if op == 10: os.makedirs(cstr(a).replace("\\", "/"), exist_ok=True); return 0
    if op == 11:                                  # getenv
        v = env.get(cstr(a)); return v if v else 0
    if op == 12: env[cstr(a)] = salloc(cstr(b).encode() + b"\0"); return 0
    pg = op - 100
    if pg == 0: return SHARED
    if pg == 1:                                   # pg_init: open the "serial port"
        ser = socket.create_connection(("127.0.0.1", 7777)); return 0
    if pg in (2, 3, 4, 6, 10, 18):
        if pg == 6: time.sleep(a / 1000.0)
        if pg == 10: net_closed[0] = True
        if pg == 4: state["exit"] = a
        return 0
    if pg == 5: return 1 if state["quit"] else 0
    if pg == 7: ser.sendall(bytes(uc.mem_read(a, b))); return b
    if pg == 8: return len(rx)
    if pg == 9:
        n = min(b, len(rx)); uc.mem_write(a, bytes(rx[:n])); del rx[:n]; return n
    if pg == 11: user_logic(); return len(kbd)
    if pg == 12:
        user_logic(); n = min(b, len(kbd)); uc.mem_write(a, bytes(kbd[:n])); del kbd[:n]; return n
    if pg == 13: screen(bytes(uc.mem_read(a, b))); return 0
    if pg == 14: uc.mem_write(a, struct.pack("<i", 30)); uc.mem_write(b, struct.pack("<i", 106)); return 0
    if pg == 15: return 0
    if pg == 16: return pg_wait(struct.unpack("<i", struct.pack("<I", a))[0], b, c)
    if pg == 17: return pg_dial(a, b)
    if pg == 19: data = os.urandom(min(b, 256)); uc.mem_write(a, data); return len(data)
    if pg == 20: return HOME_PTR
    if op == 0: state["exit"] = a; uc.emu_stop(); return 0   # main() returned
    raise RuntimeError("unknown hypercall %d" % op)

HC_ADDR = sym("emu_hc")
def on_hc(uc_, addr, size, _):
    r = [uc.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    d = struct.unpack("<I", bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_SP), 4)))[0]
    ret = hc(r[0], r[1], r[2], r[3], d)
    uc.reg_write(UC_ARM_REG_R0, ret & 0xffffffff)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
uc.hook_add(UC_HOOK_CODE, on_hc, begin=HC_ADDR, end=HC_ADDR)

insns = [0]
if COUNT:
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
    cs = Cs(CS_ARCH_ARM, CS_MODE_ARM); bc = {}
    sys.path.insert(0, "bench")
    def insn_cycles(i):
        m = i.mnemonic.split(".")[0]; ops = i.op_str
        b = re.sub(r"(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)$", "", m)
        wpc = ops.startswith("pc")
        if b in ("b", "bl", "bx"): return 3
        if b.startswith(("ldm", "pop")): return max(1, ops.count(",") + 1) + 2 + (2 if "pc" in ops else 0)
        if b.startswith(("stm", "push")): return max(1, ops.count(",") + 1) + 1
        if b.startswith("ldr"): return 5 if wpc else 3
        if b.startswith("str"): return 2
        if b in ("mul", "mla", "muls", "mlas"): return 9
        return 1 + (1 if re.search(r"(lsl|lsr|asr|ror) r\d", ops) else 0) + (2 if wpc else 0)
    cyc = [0]
    def on_block(uc_, addr, size, _):
        n = bc.get(addr)
        if n is None:
            l = list(cs.disasm(bytes(uc.mem_read(addr, size)), addr))
            n = bc[addr] = (len(l), sum(insn_cycles(i) for i in l))
        insns[0] += n[0]; cyc[0] += n[1]
    PROF = "--prof" in sys.argv
    if not PROF: uc.hook_add(UC_HOOK_BLOCK, on_block)
    if PROF:
        # per-function cycle histogram: map every block start to the nearest
        # preceding global symbol in the linker map (static functions are
        # attributed to the object file's .text start instead)
        import bisect
        syms = []
        for m in re.finditer(r"^\s+0x([0-9a-f]+)\s+(\S+)\s*$", MAP, re.M):
            a = int(m.group(1), 16)
            if base <= a < base + size and "=" not in m.group(2): syms.append((a, m.group(2)))
        import subprocess
        for m in re.finditer(r"^ \.text\s+0x([0-9a-f]+)\s+0x[0-9a-f]+\s+(\S+)$", MAP, re.M):
            obase, oname = int(m.group(1), 16), m.group(2)
            syms.append((obase, "[" + oname.split("/")[-1] + "]"))
            # static functions from the object file itself (nm 't' symbols)
            if oname.endswith(".o") and os.path.exists(oname):
                try:
                    nm = subprocess.run(["arm-pe-nm", oname], capture_output=True, text=True).stdout
                    for l in nm.splitlines():
                        f = l.split()
                        if len(f) == 3 and f[1] == "t" and not f[2].startswith("."):
                            syms.append((obase + int(f[0], 16), f[2] + "@" + oname.split("/")[-1]))
                except OSError:
                    pass
        syms.sort(); saddr = [a for a, _ in syms]
        prof = {}; bfn = {}
        def fn_of(addr):
            f = bfn.get(addr)
            if f is None:
                i = bisect.bisect_right(saddr, addr) - 1
                f = bfn[addr] = syms[i][1] if i >= 0 else "?"
            return f
        # leaf helpers are charged to their caller ("memset<-mp_init")
        LEAF = set(x for x in ("__muldi3", "memset", "memcpy", "memmove", "__udivsi3", "__divsi3", "__umodsi3", "__modsi3", "malloc", "free", "mp_zero", "mp_clear", "mp_grow", "mp_clamp", "mp_init", "m_mp_init") if any(x == n for _, n in syms))
        leaf_addr = dict((n, a) for a, n in syms if n in LEAF)
        cur = {"leaf": None}
        entry_addr = set(a for a, _ in syms); calls = {}
        def on_block_prof(uc_, addr, size, _):
            n = bc.get(addr)
            if n is None:
                l = list(cs.disasm(bytes(uc.mem_read(addr, size)), addr))
                n = bc[addr] = (len(l), sum(insn_cycles(i) for i in l))
            insns[0] += n[0]; cyc[0] += n[1]
            f = fn_of(addr)
            if addr in entry_addr: calls[f] = calls.get(f, 0) + 1
            if f in LEAF:
                if addr == leaf_addr[f]:
                    cur["leaf"] = f + "<-" + fn_of(uc.reg_read(UC_ARM_REG_LR))
                f = cur["leaf"] or f
            prof[f] = prof.get(f, 0) + n[1]
        uc.hook_add(UC_HOOK_BLOCK, on_block_prof)
        import atexit
        def dump_prof():
            tot = max(1, sum(prof.values()))
            print("\n### cycle profile (top 45), total %d cycles = %.2f s at 36 MHz" % (tot, tot / 36e6))
            for f, c in sorted(prof.items(), key=lambda x: -x[1])[:45]:
                print("  %6.2f%%  %10d  %8d  %s" % (100.0 * c / tot, c, calls.get(f.split("<-")[0], 0), f))
        atexit.register(dump_prof)
    # --cache: crude ARM710 cache model (8 KB unified, 4-way, 16-byte lines,
    # LRU, allocate on read/fetch, write-through). Each line fill is charged
    # CACHE_MISS cycles on top of the core model. Only for comparing
    # code-size/speed trade-offs; absolute numbers are indicative.
    if "--cache" in sys.argv:
        from unicorn import UC_HOOK_MEM_READ
        CACHE_MISS = 10
        SETS = 128            # 8192 / (16 * 4)
        cache = [[] for _ in range(SETS)]   # per set: list of tags, LRU at end
        cst = {"miss": 0, "fetch": 0, "data": 0}
        def touch(line):
            st = cache[line & (SETS - 1)]
            tag = line >> 7
            try:
                st.remove(tag)
            except ValueError:
                cst["miss"] += 1; cyc[0] += CACHE_MISS
                if len(st) >= 4: st.pop(0)
            st.append(tag)
        def on_block_cache(uc_, addr, size, _):
            cst["fetch"] += 1
            for line in range(addr >> 4, (addr + size - 1) // 16 + 1): touch(line)
        def on_read_cache(uc_, access, addr, size, value, _):
            cst["data"] += 1
            touch(addr >> 4)
            if (addr & 15) + size > 16: touch((addr >> 4) + 1)
        uc.hook_add(UC_HOOK_BLOCK, on_block_cache)
        uc.hook_add(UC_HOOK_MEM_READ, on_read_cache)
        import atexit
        atexit.register(lambda: print("### cache: %d line fills (%d blocks, %d data reads)" % (cst["miss"], cst["fetch"], cst["data"])))
    bench_state = {}
    def on_bench_enter(uc_, addr, size, _):
        bench_state["id"] = uc.reg_read(UC_ARM_REG_R0); bench_state["c0"] = cyc[0]
        if "--prof" in sys.argv and "--prof-cases" in sys.argv: prof.clear()
        lr = uc.reg_read(UC_ARM_REG_LR)
        if "hooked" not in bench_state:
            bench_state["hooked"] = 1
            uc.hook_add(UC_HOOK_CODE, on_bench_exit, begin=lr, end=lr)
    def on_bench_exit(uc_, addr, size, _):
        if "id" in bench_state:
            c = cyc[0] - bench_state.pop("id") * 0 - bench_state["c0"]
            lo = c / 36e6; hi = lo * 1.6
            print("\n### [5mx estimate] case: %.3f-%.3f s  (%s)" % (lo, hi, "%.0f-%.0f KB/s if 4 KB" % (4 / hi, 4 / lo)))
            if "--prof" in sys.argv and "--prof-cases" in sys.argv:
                tot = max(1, sum(prof.values()))
                for f, cc in sorted(prof.items(), key=lambda x: -x[1])[:8]:
                    print("      %6.2f%%  %10d  %s" % (100.0 * cc / tot, cc, f))
    uc.hook_add(UC_HOOK_CODE, on_bench_enter, begin=sym("psi_bench_case"), end=sym("psi_bench_case"))

os.makedirs(HOME, exist_ok=True)
argv0 = salloc(b"psissh\0")
argv = salloc(struct.pack("<II", argv0, 0))
uc.reg_write(UC_ARM_REG_SP, STACK + 0x40000 - 64)
uc.reg_write(UC_ARM_REG_R0, 1)
uc.reg_write(UC_ARM_REG_R1, argv)
uc.reg_write(UC_ARM_REG_LR, sym("emu_hc"))    # returning from main -> harmless hypercall
t0 = time.time()
try:
    uc.emu_start(sym("main"), 0xfffffff0)
except Exception as e:
    pc = uc.reg_read(UC_ARM_REG_PC)
    print("\n### EMULATION FAULT: %s at pc=0x%x lr=0x%x" % (e, pc, uc.reg_read(UC_ARM_REG_LR)))
    sys.exit(1)
text = "".join(out_text)
ok = ("PSION-42" in text and state["stage"] >= 4 and (not SAVEDPW or bytes(uc.mem_read(SHARED + 19312, 4)) == b"\0\0\0\0")) or ("--bench" in sys.argv and "self-tests passed" in text)
print("\n### exit=%s stage=%d wall=%.1fs %s" % (state["exit"], state["stage"], time.time() - t0, "PSION-BUILD END-TO-END OK" if ok else "FAILED"))
if "rx1" in stats:
    w = stats["rx1"] - stats["rx0"]; o = stats["out1"] - stats["out0"]
    print("### after login: %d bytes on the wire for %d bytes of screen output (%.1fx)" % (w, o, o / max(w, 1)))
if COUNT:
    print("### ARM instructions executed: %d (~%.0f-%.0f s of 5mx CPU at 36 MHz)" % (insns[0], insns[0] * 1.45 / 36e6, insns[0] * 1.45 * 1.6 / 36e6))
