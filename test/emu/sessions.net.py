# test/emu/sessions.net.py - PsiTerm 1.09: tmux's session list keeps up
#
#   python3 core/tools/emu/net.py sessions
#
# Run by core/tools/emu/net.py (which gives it Script, ssh_setup, make_card,
# emulate and the key names). A live tmux on the test sshd, with eight
# sessions made on the server ("sh mk"), so there are more than the list
# shows. Then, in PsiTerm: the drop-down (Shift+Ctrl+G) > Manage sessions... >
# New makes session "zz" and goes to it; the drop-down again, second line,
# goes back to an older session; the drop-down a third time is the last
# screenshot (end.png), and "zz" must be in it. To 1.08 it was not: the list
# was tmux's first six by name. Passes when tmux has "zz" and PsiTerm's own
# session is not on it; look at end.png for the list itself.
def test_sessions(ctx):
    tdir = os.path.join(ctx.sockdir, 't')
    os.makedirs(tdir)
    r = ssh_setup(ctx, tmux_dir=tdir)
    if not r:
        return False, 'sshd did not start (see %s/sshd.log)' % WORK
    d, port, log = r
    with open(os.path.join(d, 'mk'), 'w') as f:
        f.write('for i in 1 2 3 4 5 6 7 8; do tmux new-session -d -s old$i; done\n')
    card = os.path.join(WORK, 'sessions.img')
    make_card('psiterm', card, 'EMU_PKG_TERM', [('PSIKEY', os.path.join(d, 'id_ed25519'))])
    ctx.modem('sessions', redirect='127.0.0.1:%d' % port)
    # as the tmux test: the key, tmux settings > Start in tmux, a server, connect
    s = Script(52).key(MENU, gap=1).key(LEFT, gap=1.5).key(DOWN, gap=0.8).key(ENTER, gap=2)
    s.ctrl('i', gap=2).key(DOWN, gap=0.8)
    for _ in range(16):
        s.key(DEL, gap=0.15)
    s.type('d:\\psikey').key(ENTER, gap=15)
    s.key(ENTER, gap=2).key(ESC, gap=2)
    s.key(MENU, gap=2.5).key(LEFT, gap=1.5).key(LEFT, gap=1.5)
    for _ in range(4):
        s.key(DOWN, gap=1)
    s.key(ENTER, gap=3).key(RIGHT, gap=1).key(ENTER, gap=2)
    s.ctrl('s', shift=True, gap=2).ctrl('n', gap=2)
    s.type('t').key(DOWN, gap=0.8).type('t').key(DOWN, gap=0.8).type(getpass.getuser())
    s.key(ENTER, gap=2)
    s.key(ENTER, gap=30)
    s.type('y').wait(25)                      # tmux starts; the tabs come up
    s.type('sh mk\r').wait(10)                # eight more sessions, made on the server
    UP = 16
    # the drop-down; Up from its first line is its last, Manage sessions...
    s.ctrl('g', shift=True, gap=3).key(UP, gap=1).key(ENTER, gap=3)
    s.ctrl('n', gap=3).type('zz').key(ENTER, gap=10)      # New: "zz", and PsiTerm goes to it
    s.ctrl('g', shift=True, gap=3).key(DOWN, gap=1).key(ENTER, gap=10)   # an older session
    s.ctrl('g', shift=True, gap=4)            # the list, for end.png
    out = emulate('sessions', card, ctx.sock, s, secs=int(os.environ.get('EMU_TMUX_SECS', 6)))
    env = dict(os.environ, TMUX_TMPDIR=tdir)
    env.pop('TMUX', None)
    def tmux(*a):
        try:
            return subprocess.run(['tmux'] + list(a), env=env, capture_output=True, text=True, timeout=10).stdout
        except Exception:
            return ''
    sessions = tmux('ls', '-F', '#{session_name} #{session_attached}')
    tmux('kill-server')
    shutil.rmtree(tdir, ignore_errors=True)
    with open(os.path.join(out, 'sessions-result.txt'), 'w') as f:
        f.write(sessions)
    return ('zz 0' in sessions and 'old8' in sessions), out


TESTS['sessions'] = test_sessions
