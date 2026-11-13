"""psissh's SAVE (PsiTerm 1.02's editor) against a real sshd and sftp-server,
through psissh-host: a new file, a save over it (its permissions kept,
nothing left beside it), a conflict (changed on the server since opened), a
forced save, a link written through.

    PSISSH_HOST=path/to/psissh-host python3 test/save_test.py      (in ssh/)

It uses the test sshd of core/tools/emu/net.py (your own user, a key made
for the test, a forced command; nothing else of yours is touched), and a
scratch folder (SAVE_TEST_DIR, else a new one in /tmp)."""
import sys, os, subprocess, time, getpass, shutil, stat
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'core', 'tools', 'emu'))
import net
import tempfile
S = os.environ.get('SAVE_TEST_DIR') or tempfile.mkdtemp(prefix='psisave-')
HOST = os.path.abspath(os.environ.get('PSISSH_HOST', 'psissh-host'))
ctx = net.Ctx()
ok_all = True
def check(name, cond, info=''):
    global ok_all
    print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else '  ' + info))
    ok_all = ok_all and cond
try:
    root = os.path.join(S, 'sroot'); shutil.rmtree(root, ignore_errors=True); os.makedirs(root)
    d, port, log = net.ssh_setup(ctx, sftp_root=root)
    home = os.path.join(S, 'phome'); shutil.rmtree(home, ignore_errors=True); os.makedirs(home)
    env = dict(os.environ, PSI_HOST='127.0.0.1', PSI_PORT=str(port), PSI_USER=getpass.getuser(),
               PSI_HOME=home, PSI_NET='1')
    env.pop('PSI_PASS', None)
    # import the test key
    e = dict(env, PSI_KEYSRC=os.path.join(d, 'id_ed25519'), PSI_KEY=os.path.join(home, 'k'), PSI_KEYNAME='t')
    r = subprocess.run([HOST], env=e, input=b'', capture_output=True, timeout=60)
    print('import:', r.returncode, os.listdir(home))
    a = os.path.join(S, 'a.txt'); open(a, 'w').write('one\ntwo\n')
    b = os.path.join(S, 'b.txt'); open(b, 'w').write('one\ntwo\nthree\n')
    target = os.path.join(root, 'note.txt')
    open(os.path.join(root, 'real.txt'), 'w').write('real\n')
    os.symlink('real.txt', os.path.join(root, 'link.txt'))
    def run(script):
        sp = os.path.join(S, 'x.script'); open(sp, 'w').write(script)
        out = os.path.join(S, 'x.out')
        if os.path.exists(out): os.remove(out)
        for f in os.listdir(home):
            if 'known' in f: os.remove(os.path.join(home, f))
        e = dict(env, PSI_KEY=os.path.join(home, 'k'), PSI_XFER=sp, PSI_XFER_OUT=out)
        import pexpect
        c = pexpect.spawn(HOST, env=e, timeout=60, encoding='latin-1')
        seen = ''
        t0 = time.time()
        while time.time() - t0 < 60:
            i = c.expect(['continue connecting', pexpect.EOF, pexpect.TIMEOUT], timeout=2)
            seen += (c.before or '')
            if i == 0: c.send('y')
            if i == 1: break
            if os.path.exists(out) and 'END' in open(out).read(): break
        c.close(force=True)
        o = open(out).read() if os.path.exists(out) else ''
        if not o: print(seen[-600:])
        return o
    o = run('save %s note.txt 0 0 0\nstat note.txt\nquit\n' % a)
    print(o)
    check('new file saved', 'rc=0' in o.splitlines()[0] and open(target).read() == 'one\ntwo\n', o)
    m = int(os.stat(target).st_mtime)
    os.chmod(target, 0o640)
    o = run('save %s note.txt %d %d 0\nquit\n' % (b, m, 8))
    print(o)
    check('saved over it', 'rc=0' in o.splitlines()[0] and open(target).read() == 'one\ntwo\nthree\n', o)
    check('permissions kept', stat.S_IMODE(os.stat(target).st_mode) == 0o640, oct(os.stat(target).st_mode))
    check('no file left beside it', not os.path.exists(os.path.join(root, '.note.txt.psiterm~')))
    # changed on the server since "opened" (an old time)
    o = run('save %s note.txt %d %d 0\nquit\n' % (a, m - 100, 14))
    print(o)
    check('conflict reported', 'rc=10' in o.splitlines()[0] and open(target).read() == 'one\ntwo\nthree\n', o)
    o = run('save %s note.txt %d %d 1\nquit\n' % (a, m - 100, 14))
    check('forced save', 'rc=0' in o.splitlines()[0] and open(target).read() == 'one\ntwo\n', o)
    lm = int(os.stat(os.path.join(root, 'real.txt')).st_mtime)
    o = run('save %s link.txt %d 5 0\nquit\n' % (b, lm))
    print(o)
    check('link written through', os.path.islink(os.path.join(root, 'link.txt')) and open(os.path.join(root, 'real.txt')).read() == 'one\ntwo\nthree\n', o)
finally:
    ctx.close()
print('ALL OK' if ok_all else 'SOME FAILED')
