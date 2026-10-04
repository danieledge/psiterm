#!/usr/bin/env python3
"""fakewirsa.py SOCKET [stock|fast] - a WiRSa's command mode on a unix socket,
for the emulator's serial bridge (run.sh EMU_SERIAL=SOCKET).

As the WiRSa firmware (github.com/nullvalue0/WiRSa, Firmware/src/modules):
it echoes, answers "\\r\\nOK\\r\\n" and "\\r\\nERROR\\r\\n", ATI gives some network
lines, and AT$SB=n (network.cpp setBaudRate) takes only 300..115200:
 - stock: AT$SB=230400 is ERROR (the WiRSa's own list stops at 115200);
 - fast: as if 230400 were in its list: "SWITCHING SERIAL PORT TO 230400 IN
   5 SECONDS", a 5 s pause, then OK (at the new speed, on the real thing).
The bridge has no baud rate, so the speed itself isn't tested here: only
the dialogue. Each command and answer is logged to stdout."""
import os, socket, sys, time

STOCK = [300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200]


def main():
    path = sys.argv[1]
    fast = len(sys.argv) > 2 and sys.argv[2] == 'fast'
    rates = STOCK + ([230400] if fast else [])
    speed = 115200
    if os.path.exists(path):
        os.remove(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(1)
    print('fakewirsa %s on %s' % ('fast' if fast else 'stock', path), flush=True)
    while True:
        c, _ = srv.accept()
        buf = b''
        while True:
            d = c.recv(1024)
            if not d:
                break
            c.sendall(d)                       # echo, as the WiRSa
            buf += d
            while b'\r' in buf:
                line, buf = buf.split(b'\r', 1)
                cmd = line.strip().decode('latin-1').upper()
                if not cmd:
                    continue
                if cmd == 'AT':
                    out = b'\r\nOK\r\n'
                elif cmd == 'ATI':
                    out = (b'\r\nWIFI STATUS: CONNECTED\r\nSSID.......: test\r\nIP ADDRESS.: 10.0.0.2\r\n'
                           b'BAUD.......: %d\r\n\r\nOK\r\n' % speed)
                elif cmd.startswith('AT$SB?'):
                    out = b'\r\n%d\r\n' % speed
                elif cmd.startswith('AT$SB='):
                    try:
                        want = int(cmd[6:])
                    except ValueError:
                        want = 0
                    if want not in rates:
                        out = b'\r\nERROR\r\n'
                    elif want == speed:
                        out = b'\r\nOK\r\n'
                    else:
                        c.sendall(b'SWITCHING SERIAL PORT TO %d IN 5 SECONDS\r\n' % want)
                        print('%s -> switching in 5 s' % cmd, flush=True)
                        # (5 s on the WiRSa; shorter here because the
                        # emulator runs faster than the clock on the wall,
                        # and PsiKernTest waits 5.3 s of the Psion's time)
                        time.sleep(float(os.environ.get('FAKEWIRSA_SWITCH_S', '0.2')))
                        speed = want
                        out = b'\r\nOK\r\n'
                else:
                    out = b'\r\nOK\r\n'
                c.sendall(out)
                print('%s -> %r' % (cmd, out.strip()), flush=True)
        c.close()


if __name__ == '__main__':
    main()
