import pexpect, time, os, sys
home = "/tmp/cchome"; os.makedirs(home, exist_ok=True)
env = {"TERM": "xterm-256color", "PATH": os.environ["PATH"], "HOME": home, "LANG": "C.UTF-8", "COLORTERM": ""}
c = pexpect.spawn("claude", dimensions=(30, 106), env=env, encoding=None)
out = bytearray(); marks = []
def drain(t):
    end = time.time() + t
    while time.time() < end:
        try: out.extend(c.read_nonblocking(65536, timeout=0.1))
        except pexpect.TIMEOUT: pass
        except pexpect.EOF: return
for label, key, wait in [("start", None, 6), ("enter1", "\r", 3), ("enter2", "\r", 3), ("idle", None, 10)]:
    if key: c.send(key)
    n0 = len(out); t0 = time.time(); drain(wait)
    marks.append((label, len(out) - n0, round(time.time() - t0, 1)))
c.close(force=True)
open("claude.bin", "wb").write(out)
print(marks, len(out))
