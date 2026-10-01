#!/usr/bin/env python3
"""fakeimap.py - a tiny plaintext IMAP server for the modem-mode tests

  fakeimap.py PORT [--drop-at N[,N...]] [--drop-every N] [--slow MS_PER_KB]
              [--mute-noop] [--no-uidplus] [--arrive N[,N...]]

Serves an INBOX of a few messages: one whose text has "NO CARRIER" on a
line of its own (as an email about modems would), one of 30 KB (several
8 KB pieces), and small ones. --drop-at closes the TCP connection abruptly
once the given byte counts have been sent (over the whole run), the way a
modem line dropping looks to the client; --drop-every does it every N
bytes sent on each connection. --mute-noop never answers a NOOP (nor
anything after it) but keeps the connection open: what an idle connection
looks like once a router has quietly forgotten it.

Messages can be moved between folders (UID MOVE, or UID COPY + UID STORE
\\Deleted + UID EXPUNGE / EXPUNGE), with COPYUID in the replies (UIDPLUS),
and found by UID SEARCH HEADER Message-ID: what Edit > Undo needs (see
undotest.sh). --no-uidplus leaves MOVE and UIDPLUS out of the capabilities,
as an older server would: no COPYUID, so the client must search.
--arrive puts a new message in the INBOX just before the Nth SELECT of it
(counting from 1, over the whole run): new mail for a later check to find.
"""
import socket, sys, threading, time

PORT = int(sys.argv[1])
DROP_AT = []
DROP_EVERY = 0
SLOW_MS = 0
MUTE_NOOP = False
NO_UIDPLUS = False
ARRIVE = []
args = sys.argv[2:]
while args:
    a = args.pop(0)
    if a == '--drop-at': DROP_AT = [int(x) for x in args.pop(0).split(',')]
    elif a == '--drop-every': DROP_EVERY = int(args.pop(0))
    elif a == '--slow': SLOW_MS = int(args.pop(0))      # per 1000 bytes (87 = 115200 baud)
    elif a == '--mute-noop': MUTE_NOOP = True
    elif a == '--no-uidplus': NO_UIDPLUS = True
    elif a == '--arrive': ARRIVE = [int(x) for x in args.pop(0).split(',')]
CAPS = 'IMAP4rev1 LITERAL+' + ('' if NO_UIDPLUS else ' UIDPLUS MOVE')

def msg(uid, subject, text):
    return dict(uid=uid, subject=subject, text=text.replace('\n', '\r\n'), msgid='<%d@example.com>' % uid, deleted=False)

MSGS = [
    msg(101, 'hello', 'Just a short note.\n'),
    msg(102, 'modem log', 'My WiRSa said:\nCONNECT 115200\n...\nNO CARRIER\nand then it went quiet.\nMore text after.\n'),
    msg(103, 'big one', ''.join('line %05d of the big message, padding padding padding padding\n' % i for i in range(480))),
    msg(104, 'another', 'Second short note.\n'),
    msg(105, 'last', 'Third short note.\n'),
]

# the messages in each folder (others are made empty when first selected)
BOXES = {'INBOX': dict(validity=1, next=MSGS[-1]['uid'] + 1, msgs=MSGS)}
def box(name):
    if name.upper() == 'INBOX': name = 'INBOX'
    if name not in BOXES: BOXES[name] = dict(validity=7, next=1, msgs=[])
    return BOXES[name]

inbox_selects = 0

sent_total = 0
lock = threading.Lock()

# the folders, as the server names them (modified UTF-7): INBOX, the Trash
# and a "Work" tree with a non-ASCII child ("Work/Ideas & Pl&AOQ-ne" is
# "Ideas & Pläne"). CREATE, RENAME and DELETE change this list; DELETE of a
# folder with children is refused (as Dovecot's default is to say NO).
FOLDERS = [
    dict(name='INBOX', attrs='\\HasNoChildren'),
    dict(name='Trash', attrs='\\HasNoChildren \\Trash'),
    dict(name='Work', attrs='\\HasChildren'),
    dict(name='Work/Ideas &- Pl&AOQ-ne', attrs='\\HasNoChildren'),
]
folders_lock = threading.Lock()

def unquote(s):
    s = s.strip()
    if s.startswith('"') and s.endswith('"'): s = s[1:-1]
    return s.replace('\\"', '"').replace('\\\\', '\\')

def list_lines():
    out = ''
    for fo in FOLDERS:
        attrs = fo['attrs']
        kids = any(x['name'].startswith(fo['name'] + '/') for x in FOLDERS)
        attrs = ' '.join(a for a in attrs.split() if a not in ('\\HasChildren', '\\HasNoChildren'))
        attrs = ('\\HasChildren ' if kids else '\\HasNoChildren ') + attrs
        out += '* LIST (%s) "/" "%s"\r\n' % (attrs.strip(), fo['name'])
    return out

class Drop(Exception): pass

def envelope(m):
    return '("Mon, 1 Jan 2024 10:00:00 +0000" "%s" (("Ann" NIL "ann" "example.com")) (("Ann" NIL "ann" "example.com")) (("Ann" NIL "ann" "example.com")) (("Dan" NIL "dan" "example.com")) NIL NIL NIL "%s")' % (m['subject'], m['msgid'])

def bodystructure(m):
    return '("TEXT" "PLAIN" ("CHARSET" "us-ascii") NIL NIL "7BIT" %d %d)' % (len(m['text']), m['text'].count('\n'))

def fetch_items(m, seq, items):
    out = []
    if 'UID' in items or True: out.append('UID %d' % m['uid'])
    if 'FLAGS' in items: out.append('FLAGS (%s)' % ('\\Deleted' if m.get('deleted') else ''))
    if 'INTERNALDATE' in items: out.append('INTERNALDATE "01-Jan-2024 10:00:00 +0000"')
    if 'RFC822.SIZE' in items: out.append('RFC822.SIZE %d' % (len(m['text']) + 200))
    if 'ENVELOPE' in items: out.append('ENVELOPE ' + envelope(m))
    if 'BODYSTRUCTURE' in items: out.append('BODYSTRUCTURE ' + bodystructure(m))
    return '* %d FETCH (%s)\r\n' % (seq, ' '.join(out))

def handle(conn, addr):
    global sent_total
    conn_sent = 0
    log = lambda *a: print('[fakeimap]', *a, file=sys.stderr, flush=True)

    def send(s):
        nonlocal conn_sent
        global sent_total
        b = s.encode('latin-1') if isinstance(s, str) else s
        # send in pieces so a drop can land mid-literal
        i = 0
        while i < len(b):
            k = min(1000, len(b) - i)
            # the modem's own NO CARRIER ends a burst; make the look-alike do
            # the same, so the client's tail check gets the worst case
            nc = b.find(b'NO CARRIER\r\n', i, i + k)
            if nc >= 0 and nc + 12 < i + k: k = nc + 12 - i
            chunk = b[i:i + k]
            with lock:
                for at in list(DROP_AT):
                    if sent_total <= at < sent_total + k:
                        cut = at - sent_total
                        conn.sendall(chunk[:cut])
                        sent_total += cut
                        DROP_AT.remove(at)
                        log('dropping the connection at %d bytes' % at)
                        raise Drop()
                if DROP_EVERY and conn_sent + k >= DROP_EVERY:
                    cut = DROP_EVERY - conn_sent
                    conn.sendall(chunk[:cut])
                    sent_total += cut
                    log('dropping the connection (every %d bytes)' % DROP_EVERY)
                    raise Drop()
                conn.sendall(chunk)
                sent_total += k
                conn_sent += k
            i += k
            if SLOW_MS: time.sleep(SLOW_MS * k / 1000000.0)
            if nc >= 0 and k == nc + 12 - (i - k): time.sleep(0.3)

    f = conn.makefile('rb')
    sel = BOXES['INBOX']
    try:
        send('* OK [CAPABILITY %s] fakeimap ready\r\n' % CAPS)
        while True:
            line = f.readline()
            if not line: break
            line = line.decode('latin-1').rstrip('\r\n')
            log('<', line[:100])
            parts = line.split(' ', 2)
            if len(parts) < 2: continue
            tag, cmd = parts[0], parts[1].upper()
            rest = parts[2] if len(parts) > 2 else ''
            if cmd == 'LOGIN': send('%s OK [CAPABILITY %s] logged in\r\n' % (tag, CAPS))
            elif cmd == 'CAPABILITY': send('* CAPABILITY %s\r\n%s OK done\r\n' % (CAPS, tag))
            elif cmd == 'NOOP':
                if MUTE_NOOP:
                    log('mute: not answering the NOOP, or anything else')
                    while f.readline(): pass
                    break
                send('%s OK nothing\r\n' % tag)
            elif cmd == 'LOGOUT': send('* BYE bye\r\n%s OK out\r\n' % tag); break
            elif cmd == 'LIST':
                with folders_lock: send(list_lines() + '%s OK done\r\n' % tag)
            elif cmd == 'STATUS':
                name = unquote(rest[:rest.rindex('(')])
                n = len(box(name)['msgs'])
                send('* STATUS "%s" (MESSAGES %d UNSEEN 0)\r\n%s OK done\r\n' % (name, n, tag))
            elif cmd == 'SELECT':
                sel = box(unquote(rest))
                if sel is BOXES['INBOX']:
                    global inbox_selects
                    inbox_selects += 1
                    if inbox_selects in ARRIVE:
                        n = sel['next']; sel['next'] += 1
                        MSGS.append(msg(n, 'arrived %d' % n, 'This one came while you were away (%d).\n' % n))
                        log('a new message arrived: %d' % n)
                if sel is not BOXES['INBOX']:
                    send('* %d EXISTS\r\n* 0 RECENT\r\n* OK [UIDVALIDITY %d] ok\r\n* OK [UIDNEXT %d] ok\r\n%s OK [READ-WRITE] selected\r\n' % (len(sel['msgs']), sel['validity'], sel['next'], tag))
                else:
                    send('* %d EXISTS\r\n* 0 RECENT\r\n* OK [UIDVALIDITY 1] ok\r\n* OK [UIDNEXT %d] ok\r\n* FLAGS (\\Seen)\r\n%s OK [READ-WRITE] selected\r\n' % (len(MSGS), sel['next'], tag))
            elif cmd == 'CLOSE': send('%s OK closed\r\n' % tag)
            elif cmd in ('SUBSCRIBE', 'UNSUBSCRIBE'): send('%s OK noted\r\n' % tag)
            elif cmd == 'CREATE':
                name = unquote(rest)
                with folders_lock:
                    if any(fo['name'] == name for fo in FOLDERS): send('%s NO [ALREADYEXISTS] Mailbox already exists\r\n' % tag)
                    elif not name or name.endswith('/'): send('%s BAD Invalid mailbox name\r\n' % tag)
                    else:
                        FOLDERS.append(dict(name=name, attrs='\\HasNoChildren'))
                        send('%s OK created\r\n' % tag)
            elif cmd == 'RENAME':
                # RENAME "old" "new" (both quoted)
                parts2 = [unquote(x) for x in rest.replace('" "', '"\x00"').split('\x00')]
                old, new = parts2[0], parts2[-1]
                with folders_lock:
                    if not any(fo['name'] == old for fo in FOLDERS): send('%s NO [NONEXISTENT] Mailbox doesn\'t exist\r\n' % tag)
                    elif any(fo['name'] == new for fo in FOLDERS): send('%s NO [ALREADYEXISTS] Mailbox already exists\r\n' % tag)
                    elif old.upper() == 'INBOX': send('%s NO Cannot rename INBOX\r\n' % tag)
                    else:
                        for fo in FOLDERS:
                            if fo['name'] == old: fo['name'] = new
                            elif fo['name'].startswith(old + '/'): fo['name'] = new + fo['name'][len(old):]
                        send('%s OK renamed\r\n' % tag)
            elif cmd == 'DELETE':
                name = unquote(rest)
                with folders_lock:
                    if not any(fo['name'] == name for fo in FOLDERS): send('%s NO [NONEXISTENT] Mailbox doesn\'t exist\r\n' % tag)
                    elif name.upper() == 'INBOX': send('%s NO Cannot delete INBOX\r\n' % tag)
                    elif any(fo['name'].startswith(name + '/') for fo in FOLDERS): send('%s NO Mailbox has children\r\n' % tag)
                    else:
                        FOLDERS[:] = [fo for fo in FOLDERS if fo['name'] != name]
                        send('%s OK deleted\r\n' % tag)
            elif cmd == 'FETCH' or (cmd == 'UID' and rest.upper().startswith('FETCH')):
                if cmd == 'UID': rest = rest[6:]
                rng, items = rest.split(' ', 1)
                items = items.upper()
                if 'BODY.PEEK[' in items:
                    uid = int(rng)
                    m = [x for x in sel['msgs'] if x['uid'] == uid][0]
                    seq = sel['msgs'].index(m) + 1
                    part = items[items.index('<') + 1:items.index('>')]
                    off, ln = [int(x) for x in part.split('.')]
                    data = m['text'][off:off + ln]
                    send('* %d FETCH (UID %d BODY[1]<%d> {%d}\r\n' % (seq, uid, off, len(data)))
                    send(data)
                    send(')\r\n%s OK fetched\r\n' % tag)
                else:
                    lo, hi = rng.split(':') if ':' in rng else (rng, rng)
                    for seq, m in enumerate(sel['msgs'], 1):
                        key = m['uid'] if cmd == 'UID' else seq
                        lo_v = int(lo)
                        hi_v = 10 ** 9 if hi == '*' else int(hi)
                        if lo_v <= key <= hi_v: send(fetch_items(m, seq, items))
                    send('%s OK fetched\r\n' % tag)
            elif cmd == 'UID' and rest.upper().startswith('STORE'):
                uid = int(rest.split()[1])
                for m in sel['msgs']:
                    if m['uid'] == uid and '\\DELETED' in rest.upper(): m['deleted'] = '-FLAGS' not in rest.upper()
                send('%s OK stored\r\n' % tag)
            elif cmd == 'UID' and (rest.upper().startswith('MOVE ') or rest.upper().startswith('COPY ')):
                if NO_UIDPLUS and rest.upper().startswith('MOVE '): send('%s BAD no MOVE here\r\n' % tag); continue
                op, uid, dest = rest.split(' ', 2)
                uid = int(uid); dbox = box(unquote(dest))
                ms = [m for m in sel['msgs'] if m['uid'] == uid]
                if not ms: send('%s OK nothing to do\r\n' % tag); continue
                m = ms[0]
                c = dict(m, uid=dbox['next'], deleted=False)
                dbox['next'] += 1
                dbox['msgs'].append(c)
                code = '' if NO_UIDPLUS else '[COPYUID %d %d %d] ' % (dbox['validity'], uid, c['uid'])
                log('%s %d -> %s as %d' % (op.upper(), uid, unquote(dest), c['uid']))
                if op.upper() == 'MOVE':
                    seq = sel['msgs'].index(m) + 1
                    sel['msgs'].remove(m)
                    send('* OK %sMoved\r\n* %d EXPUNGE\r\n%s OK done\r\n' % (code, seq, tag))
                else:
                    send('%s OK %scopied\r\n' % (tag, code))
            elif cmd == 'EXPUNGE' or (cmd == 'UID' and rest.upper().startswith('EXPUNGE')):
                want = int(rest.split()[1]) if cmd == 'UID' else None
                out = ''
                for m in list(sel['msgs']):
                    if m['deleted'] and (want is None or m['uid'] == want):
                        out += '* %d EXPUNGE\r\n' % (sel['msgs'].index(m) + 1)
                        sel['msgs'].remove(m)
                send(out + '%s OK expunged\r\n' % tag)
            elif cmd == 'UID' and rest.upper().startswith('SEARCH HEADER MESSAGE-ID '):
                want = unquote(rest[len('SEARCH HEADER MESSAGE-ID '):])
                hits = [str(m['uid']) for m in sel['msgs'] if m['msgid'] == want]
                send('* SEARCH%s\r\n%s OK searched\r\n' % (''.join(' ' + h for h in hits), tag))
            else:
                send('%s BAD what\r\n' % tag)
    except Drop:
        pass
    except (BrokenPipeError, ConnectionResetError):
        pass
    finally:
        try: conn.shutdown(socket.SHUT_RDWR)
        except OSError: pass
        conn.close()
        log('connection closed')

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', PORT))
srv.listen(5)
print('[fakeimap] listening on %d' % PORT, file=sys.stderr, flush=True)
while True:
    c, a = srv.accept()
    threading.Thread(target=handle, args=(c, a), daemon=True).start()
