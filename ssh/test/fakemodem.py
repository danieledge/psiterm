#!/usr/bin/env python3
"""Minimal Hayes/WiRSa emulator: ATDT host:port -> CONNECT, then raw TCP."""
import socket, select, sys, threading
def handle(c):
    buf = b""
    while True:
        d = c.recv(1024)
        if not d: return
        c.sendall(d)                      # modem echo, like the WiRSa
        buf += d
        while b"\r" in buf:
            line, buf = buf.split(b"\r", 1)
            line = line.strip().decode(errors="replace")
            if line.upper().startswith("ATDT"):
                target = line[4:].strip()
                host, _, port = target.rpartition(":")
                try:
                    r = socket.create_connection((host, int(port)), timeout=10)
                except Exception as e:
                    c.sendall(b"\r\nNO CARRIER\r\n"); continue
                c.sendall(b"\r\nCONNECT 9600\r\n")
                if buf: r.sendall(buf); buf = b""
                socks = [c, r]
                online = True
                while online:
                    rd, _, _ = select.select(socks, [], [])
                    for s in rd:
                        x = s.recv(4096)
                        if not x:
                            r.close()
                            if s is r:            # remote hung up: back to command mode
                                c.sendall(b"\r\nNO CARRIER\r\n"); online = False; break
                            return
                        if s is c and x.strip() == b"+++":   # escape to command mode
                            r.close(); c.sendall(b"\r\nOK\r\n"); online = False; break
                        (r if s is c else c).sendall(x)
            elif line:
                c.sendall(b"\r\nOK\r\n")
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 7777)); srv.listen(5)
while True:
    c, _ = srv.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
