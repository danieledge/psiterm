#!/usr/bin/env python3
"""File transfer (SFTP) tests for psissh-host against a local OpenSSH server.

Needs (as root, on this machine only - never commit a password):
  - a throwaway user, e.g.  useradd -m psixfer; echo psixfer:<pw> | chpasswd
  - sshd on 127.0.0.1:2222 with "Subsystem sftp internal-sftp"
  - a second sshd on 127.0.0.1:2223 with no Subsystem line
  (other ports: PSI_TEST_PORT, PSI_TEST_PORT_NOSFTP)
Run from ssh/:  PSI_TEST_USER=psixfer PSI_TEST_PASS=<pw> python3 test/sftp_test.py
It logs in over "Psion TCP/IP" (PSI_NET), runs a transfer script through the
shared-memory interface PsiTerm uses, then checks every result and file.
"""
import hashlib, os, pwd, shutil, subprocess, sys, tempfile, time
import pexpect

USER = os.environ.get("PSI_TEST_USER", "psixfer")
PORT = int(os.environ.get("PSI_TEST_PORT", "2222"))           # with sftp
PORT_NOSFTP = int(os.environ.get("PSI_TEST_PORT_NOSFTP", str(PORT + 1)))
PASS = os.environ["PSI_TEST_PASS"]
HOME = pwd.getpwnam(USER).pw_dir
pw = pwd.getpwnam(USER)
work = tempfile.mkdtemp(prefix="psixfer-")
psion = os.path.join(work, "psion")          # the "Psion" side
os.makedirs(psion)
fails = []

def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()

def own(p):
    os.chown(p, pw.pw_uid, pw.pw_gid)

def run(script, port=None, extra_keys=None, label=""):
    port = port or PORT
    sp = os.path.join(work, "script%s.txt" % label)
    out = os.path.join(work, "out%s.txt" % label)
    open(sp, "w").write("\n".join(script) + "\n")
    env = dict(os.environ, PSI_HOST="127.0.0.1", PSI_PORT=str(port), PSI_USER=USER, PSI_NET="1",
               PSI_PASS=PASS, PSI_HOME=os.path.join(work, "home"), PSI_XFER=sp, PSI_XFER_OUT=out)
    os.makedirs(env["PSI_HOME"], exist_ok=True)
    c = pexpect.spawn("./psissh-host", env=env, timeout=120, encoding="latin-1")
    c.logfile_read = open(os.path.join(work, "term%s.log" % label), "w")
    i = c.expect(["continue connecting", r"\$ ", pexpect.EOF, pexpect.TIMEOUT])
    if i == 0:
        c.send("y")
        i = c.expect([r"\$ ", pexpect.EOF, pexpect.TIMEOUT])
        i = 1 if i == 0 else 9
    if i != 1:
        print("### login failed"); sys.exit(1)
    t0 = time.time()
    while time.time() - t0 < 300:              # the script runs in the background
        if os.path.exists(out) and open(out).read().endswith("END\n"):
            break
        try:
            c.expect([pexpect.TIMEOUT], timeout=0.5)
        except pexpect.EOF:
            break
    took = time.time() - t0
    # the shell still works after (and alongside) transfers, and "exit"
    # ends the session: the file transfer channel must not hold it open
    c.send("echo SHELL-$((6*7))\r")
    ok_shell = c.expect(["SHELL-42", pexpect.EOF, pexpect.TIMEOUT], timeout=20) == 0
    c.send("exit\r")
    ok_exit = c.expect([pexpect.EOF, pexpect.TIMEOUT], timeout=20) == 0
    res = {}
    lists = {}
    cur = None
    for line in open(out).read().splitlines():
        if line.startswith("RESULT "):
            cmd = line[7:line.index(" rc=")]
            kv = dict(x.split("=", 1) for x in line[line.index(" rc=") + 1:].split(" ") if "=" in x)
            res[cmd] = kv
            # (msg can have spaces: keep the raw line too)
            kv["raw"] = line
            cur = cmd
            lists[cmd] = []
        elif line != "END" and cur:
            lists[cur].append(line)
    return res, lists, ok_shell, ok_exit, took

def check(name, cond, info=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "   " + str(info)))
    if not cond:
        fails.append(name)

# ---- test files
small = os.path.join(psion, "small.txt")
open(small, "w").write("Hello from the Psion\r\n" * 10)
big = os.path.join(psion, "big.bin")
open(big, "wb").write(os.urandom(3 * 1024 * 1024 + 123))
empty = os.path.join(psion, "empty.txt")
open(empty, "wb").close()
allbytes = os.path.join(HOME, "allbytes.bin")
open(allbytes, "wb").write(bytes(range(256)) * 4099)
own(allbytes)
os.makedirs(os.path.join(HOME, "afolder"), exist_ok=True)
own(os.path.join(HOME, "afolder"))
for n in ("rfile1.txt", "Name with spaces.txt"):
    p = os.path.join(HOME, "afolder", n); open(p, "w").write(n); own(p)
for p in ("big.bin", "small.txt", "empty.txt", "cancelled.bin"):
    try: os.remove(os.path.join(HOME, p))
    except OSError: pass
# a small disk on the "Psion" for disk-full: 256 KB tmpfs
full = os.path.join(work, "full")
os.makedirs(full)
have_tmpfs = subprocess.call(["mount", "-t", "tmpfs", "-o", "size=256k", "tmpfs", full]) == 0

script = [
    "put %s small.txt" % small,
    "put %s big.bin" % big,
    "put %s empty.txt" % empty,
    "get big.bin %s/big.back" % psion,
    "get allbytes.bin %s/allbytes.back" % psion,
    "get empty.txt %s/empty.back" % psion,
    "list .",
    "list afolder",
    "stat big.bin",
    "stat nothere.txt",
    "stat afolder",
    "get nothere.txt %s/nothere.back" % psion,
    "get afolder %s/folder.back" % psion,
    "put %s /root/denied.txt" % small,
    "get /etc/shadow %s/shadow.back" % psion,
    "put %s /no/such/folder/x.txt" % small,
    "put %s/missing.txt missing.txt" % psion,
    "get big.bin /no/such/psion/folder/big.back",
    "cancel 200000",
    "get big.bin %s/cancelled.back" % psion,
    "cancel 200000",
    "put %s cancelled.bin" % big,
    "get small.txt %s/small.back" % psion,
]
if have_tmpfs:
    script.append("get big.bin %s/full.back" % full)
    script.append("get small.txt %s/afterfull.back" % psion)

res, lists, ok_shell, ok_exit, took = run(script)

def rc(cmd):
    return int(res.get(cmd, {}).get("rc", -1))

check("put small", rc(script[0]) == 0 and sha(small) == sha(os.path.join(HOME, "small.txt")), res.get(script[0]))
check("put 3 MB binary, checksum", rc(script[1]) == 0 and sha(big) == sha(os.path.join(HOME, "big.bin")), res.get(script[1]))
check("put empty", rc(script[2]) == 0 and os.path.getsize(os.path.join(HOME, "empty.txt")) == 0)
check("uploads owned by the user", os.stat(os.path.join(HOME, "big.bin")).st_uid == pw.pw_uid)
check("get 3 MB binary, checksum", rc(script[3]) == 0 and sha(big) == sha(psion + "/big.back"), res.get(script[3]))
check("get all byte values", rc(script[4]) == 0 and sha(allbytes) == sha(psion + "/allbytes.back"))
check("get empty", rc(script[5]) == 0 and os.path.getsize(psion + "/empty.back") == 0)
names = [l.split("\t", 1) for l in lists.get("list .", [])]
check("list home", rc("list .") == 0 and ["f%d" % os.path.getsize(big), "big.bin"] in names and ["d" + names[[n[1] for n in names].index("afolder")][0][1:], "afolder"] in names
      and res["list ."]["path"] == HOME, (res.get("list ."), names[:5]))
check("list folder (names with spaces)", rc("list afolder") == 0 and any(l.endswith("\tName with spaces.txt") for l in lists["list afolder"]), lists.get("list afolder"))
check("stat file", rc("stat big.bin") == 0 and res["stat big.bin"]["exists"] == "1" and int(res["stat big.bin"]["total"]) == os.path.getsize(big))
check("stat missing", rc("stat nothere.txt") == 0 and res["stat nothere.txt"]["exists"] == "0")
check("stat folder", rc("stat afolder") == 0 and res["stat afolder"]["exists"] == "2")
check("get missing -> not found", rc(script[11]) == 4 and not os.path.exists(psion + "/nothere.back"), res.get(script[11]))
check("get a folder -> refused", rc(script[12]) == 8 and "folder" in res[script[12]]["raw"] and not os.path.exists(psion + "/folder.back"), res.get(script[12]))
check("put denied", rc(script[13]) == 3, res.get(script[13]))
check("get denied", rc(script[14]) == 3 and not os.path.exists(psion + "/shadow.back"), res.get(script[14]))
check("put into missing folder", rc(script[15]) == 4, res.get(script[15]))
check("put missing Psion file", rc(script[16]) == 6, res.get(script[16]))
check("get to unwritable Psion folder", rc(script[17]) == 5, res.get(script[17]))
check("get cancelled, half file removed", rc(script[19]) == 1 and not os.path.exists(psion + "/cancelled.back"), res.get(script[19]))
time.sleep(0.5)
check("put cancelled, half upload removed", rc(script[21]) == 1 and not os.path.exists(os.path.join(HOME, "cancelled.bin")), res.get(script[21]))
check("transfer after a cancel", rc(script[22]) == 0 and sha(small) == sha(psion + "/small.back"), res.get(script[22]))
if have_tmpfs:
    check("disk full on the Psion", rc(script[23]) == 5 and not os.path.exists(full + "/full.back"), res.get(script[23]))
    check("transfer after disk full", rc(script[24]) == 0, res.get(script[24]))
    subprocess.call(["umount", full])
check("shell works after the transfers", ok_shell)
check("exit ends the session (channel closed)", ok_exit)
print("script took %.1f s" % took)

# ---- a server without the sftp subsystem
res2, _, ok2, ex2, _ = run(["list .", "put %s x.txt" % small], port=PORT_NOSFTP, label="-nosftp")
check("no sftp subsystem: list", rc.__call__ and int(res2.get("list .", {}).get("rc", -1)) == 2, res2)
check("no sftp subsystem: put", int(res2.get("put %s x.txt" % small, {}).get("rc", -1)) == 2, res2)
check("no sftp: shell still fine, exit ends", ok2 and ex2)

# ---- a slow link: 57600 baud (about 5.7 KB/s each way) with PPP-ish delay
import socket, threading
RATE = 5760.0
def pipe(a, b):
    try:
        while True:
            d = a.recv(512)
            if not d: break
            time.sleep(len(d) / RATE + 0.002)
            b.sendall(d)
    except OSError: pass
    for x in (a, b):
        try: x.shutdown(socket.SHUT_RDWR)
        except OSError: pass
def slow_proxy(listen_port, target):
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", listen_port)); srv.listen(4)
    def loop():
        while True:
            c, _ = srv.accept()
            r = socket.create_connection(("127.0.0.1", target))
            threading.Thread(target=pipe, args=(c, r), daemon=True).start()
            threading.Thread(target=pipe, args=(r, c), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()
if not os.environ.get("PSI_SKIP_SLOW"):
    slow_proxy(PORT + 77, PORT)
    mid = os.path.join(psion, "mid.bin")
    open(mid, "wb").write(os.urandom(150 * 1024))
    try: os.remove(os.path.join(HOME, "mid.bin"))
    except OSError: pass
    s3 = ["put %s mid.bin" % mid, "get mid.bin %s/mid.back" % psion, "cancel 30000", "get mid.bin %s/midc.back" % psion,
          "list ."]
    t = time.time()
    res3, l3, ok3, ex3, took3 = run(s3, port=PORT + 77, label="-slow")
    r = lambda c: int(res3.get(c, {}).get("rc", -1))
    check("slow link: put 150 KB", r(s3[0]) == 0 and sha(mid) == sha(os.path.join(HOME, "mid.bin")), res3.get(s3[0]))
    check("slow link: get 150 KB", r(s3[1]) == 0 and sha(mid) == sha(psion + "/mid.back"), res3.get(s3[1]))
    check("slow link: Stop mid-way", r(s3[3]) == 1 and not os.path.exists(psion + "/midc.back"), res3.get(s3[3]))
    check("slow link: list after Stop", r("list .") == 0)
    check("slow link: shell fine, exit ends", ok3 and ex3)
    print("slow link script took %.1f s (150 KB each way at %d B/s)" % (took3, RATE))

if fails:
    print("\n### %d FAILED: %s   (logs in %s)" % (len(fails), ", ".join(fails), work)); sys.exit(1)
shutil.rmtree(work)
print("\n### SFTP TESTS OK")
