#!/usr/bin/env python3
"""psiexecd.py - the helper on your LAN for the Atom modem's remote compute
(AT$EXEC=command, and ATDT psiexec from PsiTerm).

    PSIEXEC_TOKEN=your-token python3 psiexecd.py --allow uptime,date,fortune,ls
    PSIEXEC_TOKEN=your-token python3 psiexecd.py --allow uptime --shell

On the modem: AT$XE=1  AT$XH=<this PC>:7777  AT$XK=your-token  AT&W

The protocol (plain TCP on your LAN; not for the Internet):
    modem -> helper   PSIEXEC/1 <token> one\\r\\n       (AT$EXEC=command)
                  or  PSIEXEC/1 <token> channel\\r\\n   (ATDT psiexec)
    helper -> modem   OK\\r\\n          (or ERR <reason>\\r\\n, and the connection closes)
then, for "one", a single command line: it is run, its output sent, and the
connection closes (the modem prints OK). For "channel", a line at a time
for as long as the call lasts: each line is a command, its output comes
back, and "exit" or the modem hanging up ends it (NO CARRIER).

Safety: only commands named with --allow run (the first word of the line;
the rest are its arguments, split as a shell would but with no shell
involved, so no pipes, redirections or substitutions). Each run has a time
limit (--timeout, 30 s) and its output is capped (--max, 64 KB). The token
comes from the environment, never from the command line or a file in the
repository. --shell adds a "sh" command that runs the rest of the line
through /bin/sh, which is a shell: only for a token you trust.
MIT licence (see LICENSE at the top of the repository)."""
import argparse, os, shlex, socket, subprocess, sys, threading


def run(args, allow, shell, timeout, cap):
    if not args:
        return b''
    if args[0] == 'sh' and shell:
        cmd = ' '.join(args[1:])
        popen = dict(args=cmd, shell=True)
    elif args[0] in allow:
        popen = dict(args=args, shell=False)
    else:
        return ('not allowed: %s (allowed: %s)\n' % (args[0], ', '.join(sorted(allow)))).encode()
    try:
        p = subprocess.run(stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, stdin=subprocess.DEVNULL, **popen)
        out = p.stdout
        if p.returncode:
            out += ('[exit %d]\n' % p.returncode).encode()
    except subprocess.TimeoutExpired:
        out = b'[timed out]\n'
    except OSError as e:
        out = ('[%s]\n' % e).encode()
    if len(out) > cap:
        out = out[:cap] + b'\n[output cut]\n'
    return out.replace(b'\n', b'\r\n')


def serve(conn, addr, token, allow, shell, timeout, cap, log):
    try:
        conn.settimeout(10)
        f = conn.makefile('rb')
        hello = f.readline()
        words = hello.decode('latin-1').split()
        if len(words) < 2 or words[0] != 'PSIEXEC/1' or words[1] != token:
            log('%s: refused (bad hello)' % addr[0])
            conn.sendall(b'ERR bad token\r\n')
            return
        one = len(words) < 3 or words[2] == 'one'
        conn.sendall(b'OK\r\n')
        conn.settimeout(None)
        while True:
            line = f.readline()
            if not line:
                break
            text = line.decode('utf-8', 'replace').strip()
            if not text:
                continue
            if text == 'exit':
                break
            try:
                args = shlex.split(text)
            except ValueError as e:
                conn.sendall(('[%s]\r\n' % e).encode())
                continue
            log('%s: %s' % (addr[0], text))
            conn.sendall(run(args, allow, shell, timeout, cap))
            if one:
                break
    except (OSError, ValueError):
        pass
    finally:
        conn.close()
        log('%s: closed' % addr[0])


def main():
    ap = argparse.ArgumentParser(description='the Atom modem\'s remote compute helper')
    ap.add_argument('--port', type=int, default=7777)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--allow', default='uptime,date', help='commands allowed, comma-separated')
    ap.add_argument('--shell', action='store_true', help='also allow "sh ..." through /bin/sh')
    ap.add_argument('--timeout', type=float, default=30)
    ap.add_argument('--max', type=int, default=65536)
    a = ap.parse_args()
    token = os.environ.get('PSIEXEC_TOKEN', '')
    if not token:
        sys.exit('set PSIEXEC_TOKEN in the environment first (the same as AT$XK on the modem)')
    allow = set(x.strip() for x in a.allow.split(',') if x.strip())
    log = lambda m: print(m, flush=True)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.bind, a.port))
    srv.listen(2)
    log('psiexecd on %s:%d, allowed: %s%s' % (a.bind, a.port, ', '.join(sorted(allow)), ' and sh' if a.shell else ''))
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=serve, args=(conn, addr, token, allow, a.shell, a.timeout, a.max, log), daemon=True).start()


if __name__ == '__main__':
    main()
