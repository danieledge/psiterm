#!/usr/bin/env python3
"""Run the Psion-compiled crypto (bench.pe) in an ARM emulator.

Checks each primitive against its official test vector and estimates how
long it would take on the Psion 5mx's 36 MHz ARM710 from the executed
instruction mix (ARM7 core timings, no long-multiply)."""
import re, sys, time
import pefile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK
from unicorn.arm_const import *
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

MAP = open("bench/bench.map").read()
def sym(name):
    m = re.search(r"0x([0-9a-f]+)\s+%s\s*$" % re.escape(name), MAP, re.M)
    return int(m.group(1), 16)

pe = pefile.PE("bench/bench.pe")
base = pe.OPTIONAL_HEADER.ImageBase
cs = Cs(CS_ARCH_ARM, CS_MODE_ARM)

# ARM710 (ARM7, no M extension) approximate cycle costs
def insn_cycles(i):
    m = i.mnemonic.split(".")[0]
    ops = i.op_str
    base_m = re.sub(r"(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)$", "", m)
    writes_pc = ops.startswith("pc")
    if base_m in ("b", "bl", "bx"):
        return 3
    if base_m.startswith("ldm") or base_m.startswith("pop"):
        n = ops.count("r") + ops.count("lr") + ops.count("pc") + ops.count("ip") + ops.count("sb") + ops.count("sl") + ops.count("fp")
        n = max(1, ops.count(",") + 1)
        return n + 2 + (2 if "pc" in ops else 0)
    if base_m.startswith("stm") or base_m.startswith("push"):
        n = max(1, ops.count(",") + 1)
        return n + 1
    if base_m.startswith("ldr"):
        return 5 if writes_pc else 3
    if base_m.startswith("str"):
        return 2
    if base_m in ("mul", "mla", "muls", "mlas"):
        return 9          # 16-bit-ish operands on average (Booth, 2 bits/cycle)
    c = 1
    if re.search(r"(lsl|lsr|asr|ror) r\d", ops):
        c += 1            # register-specified shift
    if writes_pc:
        c += 2
    return c

block_cost = {}
counts = {"insns": 0, "cycles": 0, "blocks": 0}

def hook_block(uc, addr, size, _):
    c = block_cost.get(addr)
    if c is None:
        code = uc.mem_read(addr, size)
        n = cyc = 0
        for i in cs.disasm(bytes(code), addr):
            n += 1
            cyc += insn_cycles(i)
        c = (n, cyc)
        block_cost[addr] = c
    counts["insns"] += c[0]
    counts["cycles"] += c[1]
    counts["blocks"] += 1

def run(which):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try:
        from unicorn import UC_CPU_ARM_926
        uc.ctl_set_cpu_model(UC_CPU_ARM_926)   # pre-ARMv6 unaligned-load behaviour
    except Exception:
        pass
    size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xfff) & ~0xfff
    uc.mem_map(base, size + 0x10000)
    for s in pe.sections:
        data = s.get_data()
        uc.mem_write(base + s.VirtualAddress, data)
    stack = 0x20000000
    uc.mem_map(stack, 0x100000)
    uc.reg_write(UC_ARM_REG_SP, stack + 0x100000 - 16)
    halt = sym("bench_halt")
    uc.reg_write(UC_ARM_REG_LR, halt)
    uc.reg_write(UC_ARM_REG_R0, which)
    for k in counts: counts[k] = 0
    uc.hook_add(UC_HOOK_BLOCK, hook_block)
    uc.emu_start(sym("bench_main"), halt)
    return uc.reg_read(UC_ARM_REG_R0)

TESTS = [
    (1, "X25519 key agreement (RFC 7748)", "x2 per connection"),
    (2, "Ed25519 host-key verify (RFC 8032)", "x1 per connection"),
    (7, "RSA-2048 host-key verify (e=65537)", "only if server uses RSA"),
    (3, "SHA-256 test vector", ""),
    (4, "AES-128 test vector (FIPS-197)", ""),
    (5, "ChaCha20 test vector (RFC 8439)", ""),
    (6, "Poly1305 test vector (RFC 8439)", ""),
    (8, "ChaCha20 bulk, 4 KB", "throughput"),
    (9, "Poly1305 bulk, 4 KB", "throughput"),
    (10, "AES-128-CTR bulk, 4 KB", "throughput"),
    (11, "SHA-256 bulk, 4 KB", "throughput"),
]
MHZ = 36.0
only = [int(a) for a in sys.argv[1:]]
print("%-40s %-6s %12s %10s %14s" % ("test", "result", "ARM insns", "est. sec", "note"))
for t, name, note in TESTS:
    if only and t not in only:
        continue
    t0 = time.time()
    rc = run(t)
    lo = counts["cycles"] / (MHZ * 1e6)
    hi = lo * 1.6          # allowance for cache misses / DRAM wait states
    if t >= 8:
        kbs_lo, kbs_hi = 4.0 / hi, 4.0 / lo
        est = "%.0f-%.0f KB/s" % (kbs_lo, kbs_hi)
    else:
        est = "%.2f-%.2f" % (lo, hi)
    print("%-40s %-6s %12d %14s %s" % (name, "PASS" if rc == 0 else "FAIL(%d)" % rc, counts["insns"], est, note), flush=True)
