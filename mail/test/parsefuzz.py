#!/usr/bin/env python3
"""parsefuzz.py [--rounds N] [--seeds N]

Builds mail/test/parsefuzz.c with AddressSanitizer and UBSan against the
engine's parsers (imapparse, mime, html, xmlscan, ics, charset, and the
invitation and vCard readers in invite.c) and runs it for --seeds seeds of
--rounds rounds each; then again built for 32 bits (-m32, UBSan), where a
long is 32 bits as on the Psion, so time arithmetic that would overflow
there is caught. Any crash or sanitizer report fails the run. Needs gcc
(and its 32-bit libraries for the second build; skipped without them).
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
rounds, seeds = 3000, 4
args = sys.argv[1:]
while args:
    a = args.pop(0)
    if a == "--rounds": rounds = int(args.pop(0))
    elif a == "--seeds": seeds = int(args.pop(0))
out = os.path.join(TOP, "build", "parsefuzz")
os.makedirs(out, exist_ok=True)
exe = os.path.join(out, "parsefuzz-asan")
srcs = [os.path.join(HERE, "parsefuzz.c")] + \
    [os.path.join(TOP, "mail/engine", f) for f in ("imapparse.c", "mime.c", "html.c", "xmlscan.c", "ics.c", "caltz.c", "charset.c", "invite.c")]
cmd = ["gcc", "-g", "-O1", "-std=gnu99", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
       "-fno-sanitize-recover=undefined", "-I", os.path.join(TOP, "ssh"), "-o", exe] + srcs
print("building", exe)
subprocess.check_call(cmd)
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1", UBSAN_OPTIONS="print_stacktrace=1")
exes = [exe]
exe32 = os.path.join(out, "parsefuzz-ubsan32")
cmd32 = ["gcc", "-m32", "-g", "-O1", "-std=gnu99", "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
         "-I", os.path.join(TOP, "ssh"), "-o", exe32] + srcs
if subprocess.run(cmd32).returncode == 0:
    exes.append(exe32)
else:
    print("(no 32-bit build: gcc -m32 failed)")
bad = 0
for e in exes:
    for seed in range(1, seeds + 1):
        r = subprocess.run([e, str(seed), str(rounds)], env=env)
        if r.returncode != 0:
            print("FAIL:", os.path.basename(e), "seed", seed, "exit", r.returncode); bad += 1
print("parsefuzz:", "FAILED" if bad else "all clean")
sys.exit(1 if bad else 0)
