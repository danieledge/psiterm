#!/usr/bin/env python3
"""armbench.py - the encoder's cost on the Psion's ARM, counted in unicorn.

    python3 cast/test/arm/armbench.py [--profile] [--stack] [--level N]
                                      [--moves N] [--strategy N] [--memlevel N] [SEQ.pcs ...]

Runs build/cast-arm/pcbench.pe (arm.mk: the encoder and zlib's deflate as
gcc 3.0 builds them for the Psion) on the benchmark's sequences
(build/cast-test/seq, from bench.py) and counts the ARM instructions each
update takes. Time on the Psion is estimated at 15 MIPS (the same
assumption as the PsiWeb ARM harness: an ARM710T at 36 MHz with its cache
and the 5mx's memory). --profile shows where the instructions go; --stack
the deepest stack. Each sequence's updates are also decoded on the host
(build/cast-test/bench -V) and must end on the last frame.
"""
import os, re, sys, glob, struct
import pefile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, "../../.."))
BUILD = os.path.join(TOP, "build/cast-arm")
MIPS = 15.0
args = sys.argv[1:]
PROFILE = "--profile" in args
LEVEL = int(args[args.index("--level") + 1]) if "--level" in args else 1
MOVES = int(args[args.index("--moves") + 1]) if "--moves" in args else 2
files = [a for a in args if a.endswith(".pcs")] or sorted(glob.glob(os.path.join(TOP, "build/cast-test/seq/*.pcs")))

syms = {}
funcs = []
for l in open(os.path.join(BUILD, "pcbench.syms")):
    p = l.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
        if p[1] in "tT" and not p[2].startswith(".L"):
            funcs.append((int(p[0], 16), p[2]))
funcs.sort()

pe = pefile.PE(os.path.join(BUILD, "pcbench.pe"))
base = pe.OPTIONAL_HEADER.ImageBase
end = max(s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData) for s in pe.sections)
uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
uc.mem_map(base, (end + 0x10fff) & ~0xfff)
for s in pe.sections:
    uc.mem_write(base + s.VirtualAddress, s.get_data())
STACK, STOP = 0x10000000, 0x20000000
uc.mem_map(STACK, 0x100000)
uc.mem_map(STOP, 0x1000)

count = [0]
blocks = {}


STACKUSE = "--stack" in args
minsp = [1 << 32]


def hook_block(uc, addr, size, user):
    count[0] += size >> 2
    if STACKUSE:
        sp = uc.reg_read(UC_ARM_REG_SP)
        if sp < minsp[0]:
            minsp[0] = sp
    if PROFILE:
        blocks[addr] = blocks.get(addr, 0) + (size >> 2)


uc.hook_add(UC_HOOK_BLOCK, hook_block)


def call(name, *a):
    regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
    for r, v in zip(regs, a):
        uc.reg_write(r, v & 0xffffffff)
    uc.reg_write(UC_ARM_REG_SP, STACK + 0xff000)
    uc.reg_write(UC_ARM_REG_LR, STOP)
    count[0] = 0
    uc.emu_start(syms[name], STOP)
    r = uc.reg_read(UC_ARM_REG_R0)
    return (r - (1 << 32) if r & 0x80000000 else r), count[0]


def rd32(name):
    return struct.unpack("<I", uc.mem_read(syms[name], 4))[0]


def func_of(addr):
    lo, hi = 0, len(funcs) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if funcs[mid][0] <= addr:
            lo = mid
        else:
            hi = mid - 1
    return funcs[lo][1]


STRAT = int(args[args.index("--strategy") + 1]) if "--strategy" in args else 0
MEML = int(args[args.index("--memlevel") + 1]) if "--memlevel" in args else 5
r, n = call("drv_init", 640, 240, LEVEL, MOVES)
assert r == 0, "drv_init failed"
if STRAT or MEML != 5:
    call("drv_tune", 5, STRAT)
    call("drv_tune", 6, MEML)
    print("strategy %d, memLevel %d" % (STRAT, MEML))
print("level %d, moves %d; heap after pc_enc_new: %d bytes" % (LEVEL, MOVES, rd32("rt_peak")))
print("| sequence | updates | median instr | max instr | median ms | max ms | keyframe ms |")
print("|---|---:|---:|---:|---:|---:|---:|")
for f in files:
    data = open(f, "rb").read()
    w, h, nf = struct.unpack("<HHI", data[4:12])
    fb = w * h // 2
    uc.reg_write(UC_ARM_REG_R0, 0)
    ins, keyins = [], 0
    rec = open(os.path.join(BUILD, "outs.bin"), "wb")
    for i in range(nf):
        uc.mem_write(syms["drv_fb"], data[12 + i * fb: 12 + (i + 1) * fb])
        key = 1 if i == 0 else 0
        while True:
            r, n = call("drv_frame", key, w // 2)
            if r == 1:
                break
            assert r == 0, "drv_frame returned %d" % r
            ln = rd32("drv_len")
            rec.write(struct.pack("<IB", ln, key) + bytes(uc.mem_read(syms["drv_out"], ln)))
            if key:
                keyins = n
            else:
                ins.append(n)
            key = 0
            if not rd32("drv_pending"):
                break
    rec.close()
    v = __import__("subprocess").run([os.path.join(TOP, "build/cast-test/bench"), "-V", f,
                                      os.path.join(BUILD, "outs.bin")], capture_output=True, text=True)
    if v.returncode:
        print(v.stdout)
        sys.exit(1)
    s = sorted(ins) or [0]
    print("| %s | %d | %d | %d | %.1f | %.1f | %.1f |" % (
        os.path.basename(f)[:-4], len(ins), s[len(s) // 2], s[-1],
        s[len(s) // 2] / MIPS / 1000, s[-1] / MIPS / 1000, keyins / MIPS / 1000))
    sys.stdout.flush()
print("heap peak: %d bytes" % rd32("rt_peak"))
if STACKUSE:
    print("stack used: %d bytes (at block starts)" % (STACK + 0xff000 - minsp[0]))
r, n = call("drv_changed", 320)
print("pc_enc_changed on an unchanged frame: %d instructions (%.1f ms)" % (n, n / MIPS / 1000))

if PROFILE:
    per = {}
    for a, c in blocks.items():
        fn = func_of(a)
        per[fn] = per.get(fn, 0) + c
    tot = sum(per.values())
    print("\nprofile (all sequences):")
    for fn, c in sorted(per.items(), key=lambda x: -x[1])[:20]:
        print("  %5.1f%%  %s" % (100.0 * c / tot, fn))
