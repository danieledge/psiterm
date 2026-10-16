#!/usr/bin/env python3
"""tools/emu/net.py - end-to-end network tests in the 5mx emulator.

    python3 tools/emu/net.py [at|web|mail|ssh|proxy|browse|update|all ...] [--keep]

For each test it starts a host modem (firmware/atom-modem's hostmodem) on a
unix socket, the local server the app should reach, builds a card with the
app, boots the emulator with its serial port (COMM::0 = UART2) bridged to the
modem (tools/emu/run.sh with EMU_SERIAL, which also switches the Remote link
off), types at the app, and checks the server's log. Screenshots go to
build/emu/net/<test>/ (end.png is the last frame).

  at    PsiTerm's terminal: ATI and AT, the modem answers OK
  web   PsiWeb opens http://127.0.0.1/ -> tools/emu/nethttp.py
  mail  PsiMail: a new account (Security None), Check mail -> fakeimap.py
  ssh   PsiTerm: imports a throwaway key from the card (D:\\PSIKEY) and
        logs in to a throwaway sshd on 127.0.0.1 (key login only)
  proxy PsiWeb with Preferences > Use a proxy: psiproxy, 8080 - the modem's
        own web proxy (firmware/atom-modem/src/proxy.cpp) - opens a real
        site (EMU_PROXY_URL, default news.ycombinator.com: http:// that
        moves to https://, so the modem does the TLS). Needs the Internet;
        not part of "all".
  (and any local tests in tools/emu/local/, see LOCAL below)

Every server listens on 127.0.0.1 only, and every process it starts is
stopped at the end (it never kills anything it did not start). The sshd
runs as you, with its own host key, config and authorized_keys in the work
folder, and a forced command (a plain sh in the work folder): it never reads
~/.ssh. The modem's HOSTMODEM_REDIRECT sends every dial to the test's server,
so the addresses typed on the Psion can be short.

  EMU_NET_DIR   work folder (default build/emu/net)
  EMU_PKG_TERM, EMU_PKG_MAIL, EMU_PKG_WEB  app files (default: pkg/,
                build/mail-pkg/, build/web-pkg/, as mkcard.ts)
  EMU_RETRIES   runs a failed test again this many times (default 1)
  EMU_PROXY_URL the address the proxy test types (default news.ycombinator.com)
  --keep        leave the test keys in the work folder (logs and
                screenshots always stay)
"""
import os, sys, time, shutil, socket, subprocess, tempfile, getpass

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
WORK = os.path.abspath(os.environ.get('EMU_NET_DIR', os.path.join(REPO, 'build', 'emu', 'net')))
HOSTMODEM_DIR = os.path.join(REPO, 'firmware', 'atom-modem', 'hosttest')

# ----- keys -------------------------------------------------------------------
# EStdKey codes, and how the 5mx's UK keyboard makes each character: a
# modifier plus a matrix key. Checked by typing each into PsiTerm against the
# echoing modem (the emulator's own keymap.ts puts @ on Shift+' and ~ on
# Fn+4, but this ROM has them the other way round).
SHIFT, CTRL, FN = 18, 22, 24
ENTER, ESC, TAB, DEL, MENU = 3, 4, 2, 1, 148
LEFT, RIGHT, UP, DOWN = 14, 15, 16, 17
SYMS = {
    ',': (None, 121), '.': (None, 122), "'": (None, 126), ' ': (None, 5),
    '!': (SHIFT, 49), '"': (SHIFT, 50), '$': (SHIFT, 52), '%': (SHIFT, 53),
    '^': (SHIFT, 54), '&': (SHIFT, 55), '*': (SHIFT, 56), '(': (SHIFT, 57), ')': (SHIFT, 48),
    '~': (SHIFT, 126), '?': (SHIFT, 122), '/': (SHIFT, 121),
    '_': (FN, 49), '#': (FN, 50), '\\': (FN, 51), '@': (FN, 52), '<': (FN, 53),
    '>': (FN, 54), '[': (FN, 55), ']': (FN, 56), '{': (FN, 57), '}': (FN, 48),
    ':': (FN, 126), ';': (FN, 76), '=': (FN, 80), '-': (FN, 79), '+': (FN, 73),
}


class Script:
    """Builds run.sh events: keys at a running simulated time."""

    def __init__(self, t):
        self.t = float(t)
        self.ev = []

    def at(self, t):
        self.t = float(t)
        return self

    def wait(self, s):
        self.t += s
        return self

    def key(self, code, mod=None, gap=0.4):
        if mod is not None:
            self.ev.append('%.2f %d 14' % (self.t, mod))
            self.ev.append('%.2f %d 6' % (self.t + 0.06, code))
        else:
            self.ev.append('%.2f %d 6' % (self.t, code))
        self.t += gap
        return self

    def ctrl(self, ch, shift=False, gap=0.6):
        c = ord(ch.upper())
        self.ev.append('%.2f %d 20' % (self.t, CTRL))
        if shift:
            self.ev.append('%.2f %d 18' % (self.t + 0.03, SHIFT))
        self.ev.append('%.2f %d 6' % (self.t + 0.08, c))
        self.t += gap
        return self

    def type(self, text, gap=0.3):
        for ch in text:
            if ch.isalpha():
                self.key(ord(ch.upper()), SHIFT if ch.isupper() else None, gap)
            elif ch.isdigit():
                self.key(ord(ch), None, gap)
            elif ch in SYMS:
                mod, code = SYMS[ch]
                self.key(code, mod, gap)
            elif ch == '\r':
                self.key(ENTER, None, gap)
            else:
                raise ValueError('no key for %r' % ch)
        return self

    def tap(self, x, y, gap=0.8):
        self.ev.append('%.2f tap %d %d' % (self.t, x, y))
        self.t += gap
        return self


# ----- processes ----------------------------------------------------------------
class Procs:
    """Only ever stops the processes it started itself (by PID)."""

    def __init__(self):
        self.p = []

    def start(self, name, argv, log, env=None, cwd=None):
        f = open(log, 'wb')
        p = subprocess.Popen(argv, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=cwd,
                             start_new_session=True)
        self.p.append((name, p, f))
        return p

    def stop_all(self):
        for name, p, f in reversed(self.p):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(5)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
            f.close()
        self.p = []


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_port(port, secs=10):
    end = time.time() + secs
    while time.time() < end:
        try:
            socket.create_connection(('127.0.0.1', port), 0.5).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def wait_path(path, secs=10):
    end = time.time() + secs
    while time.time() < end and not os.path.exists(path):
        time.sleep(0.05)
    return os.path.exists(path)


def run(argv, **kw):
    return subprocess.run(argv, check=True, **kw)


def build_hostmodem():
    run(['make', '-s', '-C', HOSTMODEM_DIR, 'hostmodem'])
    return os.path.join(HOSTMODEM_DIR, 'hostmodem')


def make_card(app, out, pkg_env, files=()):
    env = dict(os.environ)
    if os.environ.get(pkg_env):
        env['EMU_PKG'] = os.environ[pkg_env]
    argv = ['node', '--no-warnings', '--experimental-strip-types', os.path.join(HERE, 'mkcard.ts'), app]
    for dest, src in files:
        argv += ['--file', '%s=%s' % (dest, src)]
    run(argv + [out], env=env, stdout=subprocess.DEVNULL)


def emulate(name, card, sock, script, secs=6):
    out = os.path.join(WORK, name)
    env = dict(os.environ, EMU_CARD=card, EMU_SERIAL=sock, EMU_OUT=out, EMU_SECS=str(secs))
    run([os.path.join(HERE, 'run.sh'), name] + script.ev, env=env, stdout=subprocess.DEVNULL)
    return out


def grep(path, *needles):
    try:
        s = open(path, 'rb').read().decode('latin-1')
    except OSError:
        return False
    return all(n in s for n in needles)


# ----- tests --------------------------------------------------------------------
def test_at(ctx):
    card = os.path.join(WORK, 'term.img')
    make_card('psiterm', card, 'EMU_PKG_TERM')
    modem = ctx.modem('at')
    # PsiTerm's terminal is up at ~45 s and holds COMM::0 when SSH isn't running
    s = Script(52).type('ati\r').wait(1).type('at\r')
    out = emulate('at', card, ctx.sock, s)
    ok = grep(modem, 'Atom modem') and grep(modem, '-> 6 bytes') and grep(os.path.join(out, 'log'), 'serial-rx UART2')
    return ok, out


def test_web(ctx):
    port = free_port()
    log = os.path.join(WORK, 'http.log')
    ctx.procs.start('http', [sys.executable, os.path.join(HERE, 'nethttp.py'), str(port)], log)
    if not wait_port(port):
        return False, 'nethttp.py did not start'
    card = os.path.join(WORK, 'web.img')
    make_card('psiweb', card, 'EMU_PKG_WEB')
    ctx.modem('web', redirect='127.0.0.1:%d' % port)
    # PsiWeb is up on its welcome page at ~55 s; Ctrl+O, the address, Enter
    s = Script(60).ctrl('o').wait(1).type('127.0.0.1\r', gap=0.5)
    out = emulate('web', card, ctx.sock, s, secs=30)
    return grep(log, 'GET / HTTP') , out


def test_proxy(ctx):
    # The modem is the proxy itself: no local server, no redirect - the
    # page comes from the real site, fetched (over TLS for https) by the
    # modem's code and simplified there.
    url = os.environ.get('EMU_PROXY_URL', 'news.ycombinator.com')
    card = os.path.join(WORK, 'proxy.img')
    make_card('psiweb', card, 'EMU_PKG_WEB')
    modem = ctx.modem('proxy')
    # PsiWeb is up on its welcome page at ~55 s. Preferences (Ctrl+K): Down
    # to "Use a proxy", Right for Yes, Down to the host, psiproxy (the port
    # is 8080 already), Enter. The engine restarts with the proxy.
    s = Script(60).ctrl('k', gap=2).key(DOWN, gap=0.6).key(DOWN, gap=0.6).key(DOWN, gap=0.6)
    s.key(RIGHT, gap=0.8).key(DOWN, gap=0.8).type('psiproxy', gap=0.35).key(ENTER, gap=25)
    # then Ctrl+O, the address, Enter
    s.ctrl('o', gap=1.5).type(url + '\r', gap=0.35)
    out = emulate('proxy', card, ctx.sock, s, secs=int(os.environ.get('EMU_PROXY_SECS', 70)))
    ok = grep(modem, 'proxy: GET ') and grep(modem, '-> 200')
    return ok, out


def test_mail(ctx):
    port = free_port()
    log = os.path.join(WORK, 'imap.log')
    ctx.procs.start('imap', [sys.executable, '-u', os.path.join(REPO, 'mail', 'test', 'fakeimap.py'), str(port)], log)
    if not wait_port(port):
        return False, 'fakeimap.py did not start'
    card = os.path.join(WORK, 'mail.img')
    make_card('psimail', card, 'EMU_PKG_MAIL')
    ctx.modem('mail', redirect='127.0.0.1:%d' % port)
    s = mail_script()
    out = emulate('mail', card, ctx.sock, s, secs=60)
    return grep(log, 'LOGIN') and grep(log, 'FETCH'), out


def mail_script():
    # With no account PsiMail opens "Mail account" on its own (~86 s).
    # Account page: the email address (the servers default from it).
    s = Script(90).key(DOWN).key(DOWN).type('dan@test.lan', gap=0.35)
    # Incoming page: tap its tab, then Down into the fields (IMAP server left
    # blank: imap.test.lan, which the modem's redirect sends to fakeimap).
    # A number editor's value is selected on arrival, so typing replaces it.
    s.tap(170, 36).wait(1.5)
    s.key(DOWN, gap=0.8).key(DOWN, gap=0.8).type('143')        # Port 143
    s.key(DOWN, gap=0.8).key(LEFT, gap=0.8)                   # Security None
    s.key(DOWN, gap=0.8).key(DOWN, gap=0.8).type('secret')    # (User name: the address)
    s.key(ENTER, gap=8)                                       # OK: PsiMail checks at once
    # "Use the Email icon for PsiMail?" comes up a few seconds into the check: No
    s.tap(331, 141)
    return s


def ssh_setup(ctx, sftp_root=None):
    d = os.path.join(WORK, 'sshd')
    os.makedirs(d, exist_ok=True)
    for f in ('host_ed25519', 'host_ed25519.pub', 'id_ed25519', 'id_ed25519.pub'):
        p = os.path.join(d, f)
        if os.path.exists(p):
            os.remove(p)
    run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-C', 'psiemu-host', '-f', os.path.join(d, 'host_ed25519')])
    run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-C', 'psiemu-test', '-f', os.path.join(d, 'id_ed25519')])
    shutil.copy(os.path.join(d, 'id_ed25519.pub'), os.path.join(d, 'authorized_keys'))
    os.chmod(os.path.join(d, 'authorized_keys'), 0o600)
    shell = os.path.join(d, 'shell.sh')
    with open(shell, 'w') as f:
        # (SFTP - Get file / Send file - is the subsystem request, which the
        # forced command sees as SSH_ORIGINAL_COMMAND: OpenSSH's sftp-server,
        # starting in the test's folder)
        f.write('#!/bin/sh\n# the only thing the test sshd runs\ncd "%s"\n'
                'case "$SSH_ORIGINAL_COMMAND" in internal-sftp*|sftp|*sftp-server*)\n'
                '  exec /usr/lib/openssh/sftp-server -d "%s" -e -l INFO 2>>"%s/sftp.log";; esac\n'
                'echo "psiemu sshd: logged in as $(id -un)"\n'
                'exec env -i HOME="%s" PATH=/usr/bin:/bin TERM="${TERM:-vt100}" PS1="psiemu$ " /bin/sh -i\n'
                % (d, sftp_root or d, WORK, d))
    os.chmod(shell, 0o700)
    port = free_port()
    cfg = os.path.join(d, 'sshd_config')
    with open(cfg, 'w') as f:
        f.write('\n'.join([
            'Port %d' % port, 'ListenAddress 127.0.0.1', 'HostKey %s/host_ed25519' % d,
            'PidFile %s/sshd.pid' % d, 'AuthorizedKeysFile %s/authorized_keys' % d,
            'AuthorizedKeysCommand none', 'AuthorizedPrincipalsFile none',
            'AllowUsers %s' % getpass.getuser(),
            'PubkeyAuthentication yes', 'PasswordAuthentication no',
            'KbdInteractiveAuthentication no', 'UsePAM no', 'StrictModes no',
            'PermitUserRC no', 'PermitUserEnvironment no', 'AllowTcpForwarding no',
            'AllowAgentForwarding no', 'X11Forwarding no', 'PermitTunnel no',
            'Subsystem sftp internal-sftp', 'ForceCommand %s' % shell,
            'LogLevel VERBOSE', '']))
    log = os.path.join(WORK, 'sshd.log')
    ctx.procs.start('sshd', ['/usr/sbin/sshd', '-D', '-e', '-f', cfg], log)
    if not wait_port(port):
        return None
    return d, port, log


def test_ssh(ctx):
    r = ssh_setup(ctx)
    if not r:
        return False, 'sshd did not start (see %s/sshd.log)' % WORK
    d, port, log = r
    card = os.path.join(WORK, 'ssh.img')
    make_card('psiterm', card, 'EMU_PKG_TERM', [('PSIKEY', os.path.join(d, 'id_ed25519'))])
    ctx.modem('ssh', redirect='127.0.0.1:%d' % port)
    s = ssh_script(getpass.getuser())
    out = emulate('ssh', card, ctx.sock, s, secs=int(os.environ.get('EMU_SSH_SECS', 25)))
    return grep(log, 'Accepted publickey'), out


def ssh_script(user):
    # Tools > SSH keys... > Import (Ctrl+I): the key file is D:\PSIKEY (the
    # card writer only makes 8.3 names, so not the offered D:\id_ed25519);
    # psissh converts it to the Psion's own key format
    s = Script(52).key(MENU, gap=1).key(LEFT, gap=0.8).key(DOWN, gap=0.8).key(ENTER, gap=2)
    s.ctrl('i', gap=2).key(DOWN, gap=0.8)
    for _ in range(16):
        s.key(DEL, gap=0.15)
    s.type('d:\\psikey').key(ENTER, gap=15)
    s.key(ENTER, gap=2).key(ESC, gap=2)       # Close the public key, then the key list
    # SSH to (Shift+Ctrl+S) > New (Ctrl+N): a server "t" at "t" (the modem's
    # redirect sends any address to the test sshd), the user, then OK
    s.ctrl('s', shift=True, gap=2).ctrl('n', gap=2)
    s.type('t').key(DOWN, gap=0.8).type('t').key(DOWN, gap=0.8).type(user)
    s.key(ENTER, gap=2)
    s.key(ENTER, gap=30)                      # Connect; the host key prompt
    s.type('y').wait(12)                      # (the shell is up ~5 s later)
    s.type('echo PSIEMU-$((6*7)) $(uname -s)\r')
    return s


class Ctx:
    def __init__(self):
        self.procs = Procs()
        self.sockdir = tempfile.mkdtemp(prefix='psiemu-', dir='/tmp')
        self.sock = os.path.join(self.sockdir, 'modem')
        self.hostmodem = build_hostmodem()

    def modem(self, name, redirect=None):
        env = dict(os.environ)
        env.pop('HOSTMODEM_REDIRECT', None)
        if redirect:
            env['HOSTMODEM_REDIRECT'] = redirect
        if os.path.exists(self.sock):
            os.remove(self.sock)
        log = os.path.join(WORK, name + '-modem.log')
        self.procs.start('hostmodem', [self.hostmodem, self.sock], log, env=env)
        if not wait_path(self.sock):
            raise RuntimeError('hostmodem did not start')
        return log

    def wirsa(self, name, mode):
        """tools/emu/fakewirsa.py on the bridge socket instead of hostmodem"""
        if os.path.exists(self.sock):
            os.remove(self.sock)
        log = os.path.join(WORK, name + '-modem.log')
        self.procs.start('fakewirsa', [sys.executable, '-u', os.path.join(HERE, 'fakewirsa.py'), self.sock, mode], log)
        if not wait_path(self.sock):
            raise RuntimeError('fakewirsa did not start')
        return log

    def close(self):
        self.procs.stop_all()
        if os.path.exists(self.sock):
            os.remove(self.sock)
        os.rmdir(self.sockdir)


# PsiTerm's Tools > Update PsiTerm from a local server through the modem:
# ssh/test/chunksrv.py serves UPDSRV_ROOT (version.txt, PsiTerm.sis and its
# .sig, signed by the caller with the release key, never committed) in
# 16 KB pieces, and cuts one short and garbles another, so the retries run.
def test_update(ctx):
    root = os.environ.get('UPDSRV_ROOT')
    if not root or not os.path.isfile(os.path.join(root, 'PsiTerm.sis.sig')):
        return False, 'set UPDSRV_ROOT to a folder with version.txt, PsiTerm.sis and PsiTerm.sis.sig'
    port = free_port()
    log = os.path.join(WORK, 'update-http.log')
    ctx.procs.start('chunksrv', [sys.executable, os.path.join(TOP, 'ssh', 'test', 'chunksrv.py'), str(port)],
                    log, env=dict(os.environ, UPDSRV_ROOT=root))
    if not wait_port(port):
        return False, 'chunksrv.py did not start'
    card = os.path.join(WORK, 'update.img')
    make_card('psiterm', card, 'EMU_PKG_TERM')
    mlog = ctx.modem('update', redirect='127.0.0.1:%d' % port)
    # Menu, Left to Tools, Down four times to Update PsiTerm..., Enter; in the
    # dialog Right twice to Local server, Down to its address, any name, OK
    s = Script(52).key(MENU).key(14).key(17).key(17).key(17).key(17).key(ENTER).wait(2)
    s.key(15).key(15).key(17).type('srv').key(ENTER)
    out = emulate('update', card, ctx.sock, s, secs=int(os.environ.get('UPDATE_SECS', '260')))
    try:
        sent = open(mlog, 'rb').read().count(b'-> ')
    except OSError:
        sent = 0
    size = os.path.getsize(os.path.join(root, 'PsiTerm.sis'))
    ok = grep(mlog, 'X-Total: %d' % size) and grep(os.path.join(out, 'log'), 'serial-rx UART2')
    return ok, out


# PsiKernTest's Write > Serial link with the modem (experimental/kernel): the
# dialogue with each modem. (The bridge has no baud rate, and the emulator
# doesn't model UBRCR, so the speed itself is only tested on the 5mx.)
def kern_serial(ctx, name, modem_log, secs=50):
    card = os.path.join(WORK, 'kern.img')
    make_card('psikern', card, 'EMU_PKG_KERN')
    # PsiKernTest from the Extras bar; Menu, Right twice to Write, Down four
    # times to "Serial link with the modem", Enter, then Yes to the question
    s = Script(46).tap(335, 215).at(54).key(148).key(15).key(15)
    s.key(17).key(17).key(17).key(17).key(ENTER).wait(5).key(ord('Y'))
    return emulate(name, card, ctx.sock, s, secs=secs)


def test_kern(ctx):
    log = ctx.modem('kern')
    out = kern_serial(ctx, 'kern', log)
    # (hostmodem logs what it sends: ATI's first line, ten times at each speed)
    try:
        n = open(log, 'rb').read().count(b'Atom modem ')
    except OSError:
        n = 0
    return n >= 20, out


def test_kernwirsa(ctx):
    log = ctx.wirsa('kernwirsa', 'stock')
    out = kern_serial(ctx, 'kernwirsa', log)
    return grep(log, "AT$SB=230400 -> b'ERROR'") and grep(log, "ATI -> "), out


def test_kernwirsafast(ctx):
    log = ctx.wirsa('kernwirsafast', 'fast')
    out = kern_serial(ctx, 'kernwirsafast', log, secs=100)    # (two 5 s switches)
    return grep(log, 'AT$SB=230400 -> switching') and grep(log, 'AT$SB=115200 -> switching'), out


# PsiTerm's Get file (0.91: the file view, which lists folders in place).
# A folder tree for SFTP (sftp-server starts in it): the file view opens at
# once with "Opening folder..."; a folder is opened, made a favourite, Up
# goes back (highlighting it), a big folder is stopped with Esc while it is
# listed and then opened, the Location pane's keys go to a favourite, a tap
# on one goes there, Del removes one, Close goes back to the terminal; then
# Get file again (the folder used last) gets a file, and Send file sends one
# (the session log) into a folder. The screenshots (one a second) show the
# view never leaving the screen while listing.
def browse_tree():
    root = os.path.join(WORK, 'bt')
    shutil.rmtree(root, ignore_errors=True)
    for sub in ('alpha/deep', 'beta', 'big', 'gamma'):
        os.makedirs(os.path.join(root, sub))
    with open(os.path.join(root, 'hello.txt'), 'w') as f:
        f.write('Hello from the test server\n')
    with open(os.path.join(root, 'notes.md'), 'w') as f:
        f.write('# notes\n' * 200)
    with open(os.path.join(root, 'alpha', 'a.txt'), 'w') as f:
        f.write('a\n')
    os.symlink('alpha', os.path.join(root, 'alink'))     # a link: Open looks at it first (STAT)
    for i in range(400):        # a folder that takes several seconds to list
        with open(os.path.join(root, 'big', 'file-with-a-longish-name-%03d.dat' % i), 'w') as f:
            f.write('x' * i)
    return root


def test_browse(ctx):
    root = browse_tree()
    if os.path.exists(os.path.join(WORK, 'sftp.log')):
        os.remove(os.path.join(WORK, 'sftp.log'))
    r = ssh_setup(ctx, sftp_root=root)
    if not r:
        return False, 'sshd did not start (see %s/sshd.log)' % WORK
    d, port, log = r
    card = os.path.join(WORK, 'browse.img')
    make_card('psiterm', card, 'EMU_PKG_TERM', [('PSIKEY', os.path.join(d, 'id_ed25519'))])
    ctx.modem('browse', redirect='127.0.0.1:%d' % port)
    s = ssh_script(getpass.getuser())
    s.wait(4)
    t0 = s.t
    # File > Files > Get file (the menu opens on Tools, used last)
    s.key(MENU, gap=1.2)
    for _ in range(6):
        s.key(LEFT, gap=0.6)
    for _ in range(5):
        s.key(DOWN, gap=0.5)
    s.key(RIGHT, gap=1).key(DOWN, gap=0.6).key(ENTER, gap=12)
    s.ctrl('f', gap=3)                       # the tree's top a favourite
    s.key(ENTER, gap=6)                      # open alpha (highlighted first)
    s.ctrl('f', gap=3)                       # alpha a favourite too
    s.key(MENU, gap=1.5).key(RIGHT, gap=1).key(RIGHT, gap=2)   # the file view's menus: Favourites
    s.key(ESC, gap=1.5)                      # (one Esc closes the menus)
    s.tap(75, 73, gap=6)                     # one tap on the first favourite (the tree's top)
    s.key(ENTER, gap=6)                      # alpha again
    s.key(DEL, gap=6)                        # Backspace: up, alpha highlighted
    s.key(DOWN, gap=0.6).key(DOWN, gap=0.6)  # big
    s.key(ENTER, gap=1.5).key(ESC, gap=6)    # stopped while it is listed
    s.key(ENTER, gap=25)                     # big, all of it
    s.key(LEFT, gap=1.5).key(DOWN, gap=1.5)  # the pane: the second favourite (alpha)
    s.key(ENTER, gap=6)
    s.key(LEFT, gap=1.5).key(DEL, gap=3)     # Del removes alpha from the favourites
    s.key(RIGHT, gap=1.5)
    s.ctrl('g', gap=2)                       # Go to folder...: ~, the home folder (the tree)
    for _ in range(12):
        s.key(DEL, gap=0.15)
    s.type('~').key(ENTER, gap=6)
    s.ctrl('t', gap=3).ctrl('t', gap=3)      # Show toolbar off and on: the view takes the room
    s.ctrl('e', gap=4)                       # Close: the terminal again
    # Get file again, from the toolbar's Files button this time: the folder
    # used last (the tree's top); the file hello.txt
    s.tap(650, 165, gap=1.5).key(DOWN, gap=0.6).key(ENTER, gap=10)
    s.type('a', gap=1).key(ENTER, gap=6)    # "alink" (after alpha): a link to a folder, opened
    s.key(DEL, gap=6)                        # up again
    s.type('h', gap=1).key(ENTER, gap=4)    # the Save as dialog
    s.key(ENTER, gap=8)                      # Save: the progress window, then the terminal
    # Send file: a Psion file to send first (the Open file dialog offers
    # one in C:\Documents; Files > Log to file makes sure there is one)
    s.tap(650, 165, gap=1.5).key(DOWN, gap=0.5).key(DOWN, gap=0.5).key(ENTER, gap=2).key(ENTER, gap=3)
    s.tap(650, 165, gap=1.5).key(ENTER, gap=3)   # Files > Send file...: the Open file dialog
    s.key(ENTER, gap=8)                      # (the file it offers): the file view, for where it goes
    s.key(DOWN, gap=0.6).key(ENTER, gap=5)   # into beta
    s.ctrl('s', gap=10)                      # Send here (Ctrl+S): sent, the terminal again
    print('browse: Get file from %.0f s, script ends at %.0f s' % (t0, s.t), flush=True)
    out = emulate('browse', card, ctx.sock, s, secs=6)
    # (sftp-server's log: the folders listed, and hello.txt read)
    sl = os.path.join(WORK, 'sftp.log')
    # (the link to alpha is listed as alpha: psissh asks for the real path)
    ok = grep(sl, 'opendir "%s/alpha"' % root, 'opendir "%s/big"' % root, 'hello.txt" flags READ',
              'open "%s/beta/' % root, 'flags WRITE,CREATE')
    return ok, out


TESTS = {'browse': test_browse, 'at': test_at, 'web': test_web, 'mail': test_mail, 'ssh': test_ssh, 'proxy': test_proxy,
         'update': test_update, 'kern': test_kern, 'kernwirsa': test_kernwirsa, 'kernwirsafast': test_kernwirsafast}
ALL = ['at', 'web', 'mail', 'ssh']             # (proxy needs the Internet: asked for by name)

# Local tests (tools/emu/local/*.net.py, not part of the repository): each
# runs with this file's names (Script, make_card, emulate, grep...) and adds
# its own to TESTS. Asked for by name; never part of "all".
LOCAL = os.path.join(HERE, 'local')
if os.path.isdir(LOCAL):
    for _n in sorted(os.listdir(LOCAL)):
        if _n.endswith('.net.py'):
            _p = os.path.join(LOCAL, _n)
            with open(_p) as _f:
                exec(compile(_f.read(), _p, 'exec'), globals())


def main():
    names = [a for a in sys.argv[1:] if not a.startswith('--')] or ['all']
    if 'all' in names:
        names = list(ALL)
    for n in names:
        if n not in TESTS:
            sys.exit('net.py: unknown test %s (have %s)' % (n, ' '.join(TESTS)))
    os.makedirs(WORK, exist_ok=True)
    results = []
    retries = int(os.environ.get('EMU_RETRIES', '1'))
    for n in names:
        for attempt in range(retries + 1):
            ctx = Ctx()
            try:
                ok, where = TESTS[n](ctx)
            except Exception as e:  # report and go on to the next test
                ok, where = False, repr(e)
            finally:
                ctx.close()
            if ok:
                break
            if attempt < retries:   # (see README: a card layout can wedge the boot)
                print('%-5s failed, trying again' % n, flush=True)
        results.append((n, ok, where))
        shot = where + '/end.png' if where.startswith('/') else where
        print('%-5s %s  %s' % (n, 'PASS' if ok else 'FAIL', shot), flush=True)
    if '--keep' not in sys.argv:
        d = os.path.join(WORK, 'sshd')
        for p in [os.path.join(d, f) for f in ('host_ed25519', 'host_ed25519.pub', 'id_ed25519',
                                               'id_ed25519.pub', 'authorized_keys')] + [os.path.join(WORK, 'ssh.img')]:
            if os.path.exists(p):
                os.remove(p)
    sys.exit(0 if all(ok for _, ok, _ in results) else 1)


if __name__ == '__main__':
    main()
