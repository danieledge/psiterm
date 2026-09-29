import pexpect, sys, os, signal, time, shutil
BASE = dict(os.environ, PSI_HOST="127.0.0.1", PSI_PORT="2222", PSI_USER="psitest", PSI_HOME="/tmp/psihome")
PW = os.environ["PSI_TEST_PASS"]
results = []
def spawn(**kw):
    env = dict(BASE); env.update(kw)
    return pexpect.spawn("./psissh-host", env=env, timeout=30, encoding="latin-1")
def login(c):
    i = c.expect(["continue connecting", "assword"])
    if i == 0:
        c.send("y"); c.expect("assword")
    c.send(PW + "\r"); c.expect(r"\$ ")
def rec(name, ok, detail=""):
    results.append((name, ok, detail)); print("%-45s %s %s" % (name, "PASS" if ok else "FAIL", detail), flush=True)

# 1 dial failure: nothing listening on port 2299
c = spawn(PSI_PORT="2299")
try:
    c.expect("Could not connect: NO CARRIER"); c.expect(pexpect.EOF); rec("dial failure reports NO CARRIER", True)
except Exception as e: rec("dial failure reports NO CARRIER", False, repr(e)[:80])

# 2 wrong password then right one
c = spawn()
try:
    i = c.expect(["continue connecting", "assword"])
    if i == 0: c.send("y"); c.expect("assword")
    c.send("wrong\r"); c.expect("assword"); c.send(PW + "\r"); c.expect(r"\$ ")
    c.send("exit\r"); c.expect(pexpect.EOF); rec("wrong password re-prompts", True)
except Exception as e: rec("wrong password re-prompts", False, repr(e)[:80])

# 3 window resize mid-session (font change)
c = spawn()
try:
    login(c)
    c.send("tput cols\r"); c.expect("106"); c.expect(r"\$ ")
    os.kill(c.pid, signal.SIGUSR1); time.sleep(0.5)
    c.send("tput cols; tput lines\r"); c.expect("91"); c.expect("21"); c.expect(r"\$ ")
    c.send("exit\r"); c.expect(pexpect.EOF); rec("resize reaches server (106->91 cols)", True)
except Exception as e: rec("resize reaches server (106->91 cols)", False, repr(e)[:80])

# 4 bulk output (ring buffer + flow)
c = spawn()
try:
    login(c)
    c.send("seq 1 30000; head -c 200000 /dev/urandom | base64 | wc -l\r")
    c.expect("30000"); c.expect("3509"); c.expect(r"\$ ")
    c.send("exit\r"); c.expect(pexpect.EOF); rec("bulk output 170KB + 270KB", True)
except Exception as e: rec("bulk output 170KB + 270KB", False, repr(e)[:80])

# 5 full-screen app (alternate screen + key sequences)
c = spawn()
try:
    login(c)
    c.send("TERM=xterm-256color vi -u NONE /tmp/psitest.txt\r"); time.sleep(1.5)
    c.send("iHello from the Psion\x1b:wq\r"); c.expect(r"\$ ")
    c.send("cat /tmp/psitest.txt\r"); c.expect("Hello from the Psion")
    c.send("exit\r"); c.expect(pexpect.EOF); rec("vi edit/save via terminal keys", True)
except Exception as e: rec("vi edit/save via terminal keys", False, repr(e)[:80])

# 6 user disconnect (PsiTerm 'Disconnect SSH' = quit flag; host glue: stdin EOF)
c = spawn()
try:
    login(c)
    c.sendeof(); c.expect(pexpect.EOF, timeout=15); rec("disconnect request ends session", True)
except Exception as e: rec("disconnect request ends session", False, repr(e)[:80])

# 7 server closes connection
c = spawn()
try:
    login(c)
    c.send("kill -HUP $$\r"); c.expect(pexpect.EOF); rec("server-side close handled", True)
except Exception as e: rec("server-side close handled", False, repr(e)[:80])

# 8 host key mismatch must be refused (MITM protection)
kh = "/tmp/psihome/.ssh/known_hosts"
saved = open(kh).read()
bad = saved.split()[0] + " ssh-rsa " + "AAAAB3NzaC1yc2EAAAADAQABAAABAQ" + "C" * 360 + "\n"
open(kh, "w").write(bad)
c = spawn()
try:
    i = c.expect(["mismatch|Mismatch|MISMATCH|doesn't match|did not match|WARNING", "assword", pexpect.EOF])
    rec("changed host key is refused", i != 1, c.before[-120:].replace("\r\n", " ") if i != 0 else "")
except Exception as e: rec("changed host key is refused", False, repr(e)[:80])
open(kh, "w").write(saved)

print("\n%d/%d passed" % (sum(1 for r in results if r[1]), len(results)))
