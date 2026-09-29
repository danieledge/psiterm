import pexpect, sys, time
def cap(name, cmd, keys=()):
    c = pexpect.spawn("bash", ["-c", cmd], dimensions=(30, 106), env={"TERM": "xterm-256color", "PATH": "/usr/bin:/bin", "HOME": "/tmp", "LANG": "C.UTF-8"}, encoding=None)
    out = bytearray()
    def drain(t):
        end = time.time() + t
        while time.time() < end:
            try: out.extend(c.read_nonblocking(65536, timeout=0.1))
            except pexpect.TIMEOUT: pass
            except pexpect.EOF: return
    drain(1.0)
    for k in keys:
        c.send(k); drain(0.3)
    drain(0.5)
    c.close(force=True)
    open(name, "wb").write(out)
    print(name, len(out))
seq = "\n".join("line %d  %s" % (i, "x" * (i % 90)) for i in range(400))
open("/tmp/lines.txt", "w").write(seq + "\n")
cap("ls.bin", "ls -l --color=always /usr/bin /usr/lib | head -2000")
cap("seq.bin", "seq 1 3000")
cap("vi.bin", "vi -u NONE /tmp/lines.txt", ["\x06"] * 5 + ["\x02"] * 3 + ["\x04"] * 6 + ["\x15"] * 2 + ["j"] * 60 + ["k"] * 60 + ["G", "gg", "ddddpp", "u", ":q!\r"])
cap("less.bin", "less /tmp/lines.txt", [" "] * 5 + ["b"] * 2 + ["j"] * 40 + ["k"] * 40 + ["G", "g", "q"])
cap("top.bin", "top -d 0.3", ["", "", "", "", "", "q"])
cap("utf.bin", "printf '\\e[1;31mred \\e[7mrev\\e[0m ┌──┐ │██│ └──┘ ⣿⠿ é ü 中文 😀\\n%.0s' $(seq 1 200); printf '\\e[5;20r'; for i in $(seq 1 80); do printf '\\e[20;1Hscroll-region %d\\n' $i; done; printf '\\e[r\\e[2J\\e[H'; seq 1 50")
