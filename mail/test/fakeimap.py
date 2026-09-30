#!/usr/bin/env python3
"""fakeimap.py - a tiny plaintext IMAP server for the modem-mode tests

  fakeimap.py PORT [--drop-at N[,N...]] [--drop-every N] [--slow MS_PER_KB]
              [--mute-noop]

Serves an INBOX of a few messages: one whose text has "NO CARRIER" on a
line of its own (as an email about modems would), one of 30 KB (several
8 KB pieces), and small ones. --drop-at closes the TCP connection abruptly
once the given byte counts have been sent (over the whole run), the way a
modem line dropping looks to the client; --drop-every does it every N
bytes sent on each connection. --mute-noop never answers a NOOP (nor
anything after it) but keeps the connection open: what an idle connection
looks like once a router has quietly forgotten it.
"""
import socket, sys, threading, time

PORT = int(sys.argv[1])
DROP_AT = []
DROP_EVERY = 0
SLOW_MS = 0
MUTE_NOOP = False
args = sys.argv[2:]
while args:
    a = args.pop(0)
    if a == '--drop-at': DROP_AT = [int(x) for x in args.pop(0).split(',')]
    elif a == '--drop-every': DROP_EVERY = int(args.pop(0))
    elif a == '--slow': SLOW_MS = int(args.pop(0))      # per 1000 bytes (87 = 115200 baud)
    elif a == '--mute-noop': MUTE_NOOP = True

def msg(uid, subject, text):
    return dict(uid=uid, subject=subject, text=text.replace('\n', '\r\n'))

MSGS = [
    msg(101, 'hello', 'Just a short note.\n'),
    msg(102, 'modem log', 'My WiRSa said:\nCONNECT 115200\n...\nNO CARRIER\nand then it went quiet.\nMore text after.\n'),
    msg(103, 'big one', ''.join('line %05d of the big message, padding padding padding padding\n' % i for i in range(480))),
    msg(104, 'another', 'Second short note.\n'),
    msg(105, 'last', 'Third short note.\n'),
]

sent_total = 0
lock = threading.Lock()

class Drop(Exception): pass

def envelope(m):
    return '("Mon, 1 Jan 2024 10:00:00 +0000" "%s" (("Ann" NIL "ann" "example.com")) (("Ann" NIL "ann" "example.com")) (("Ann" NIL "ann" "example.com")) (("Dan" NIL "dan" "example.com")) NIL NIL NIL "<%d@example.com>")' % (m['subject'], m['uid'])

def bodystructure(m):
    return '("TEXT" "PLAIN" ("CHARSET" "us-ascii") NIL NIL "7BIT" %d %d)' % (len(m['text']), m['text'].count('\n'))

def fetch_items(m, seq, items):
    out = []
    if 'UID' in items or True: out.append('UID %d' % m['uid'])
    if 'FLAGS' in items: out.append('FLAGS ()')
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
    try:
        send('* OK [CAPABILITY IMAP4rev1 LITERAL+ UIDPLUS MOVE] fakeimap ready\r\n')
        while True:
            line = f.readline()
            if not line: break
            line = line.decode('latin-1').rstrip('\r\n')
            log('<', line[:100])
            parts = line.split(' ', 2)
            if len(parts) < 2: continue
            tag, cmd = parts[0], parts[1].upper()
            rest = parts[2] if len(parts) > 2 else ''
            if cmd == 'LOGIN': send('%s OK [CAPABILITY IMAP4rev1 LITERAL+ UIDPLUS MOVE] logged in\r\n' % tag)
            elif cmd == 'CAPABILITY': send('* CAPABILITY IMAP4rev1 LITERAL+ UIDPLUS MOVE\r\n%s OK done\r\n' % tag)
            elif cmd == 'NOOP':
                if MUTE_NOOP:
                    log('mute: not answering the NOOP, or anything else')
                    while f.readline(): pass
                    break
                send('%s OK nothing\r\n' % tag)
            elif cmd == 'LOGOUT': send('* BYE bye\r\n%s OK out\r\n' % tag); break
            elif cmd == 'LIST':
                send('* LIST (\\HasNoChildren) "/" "INBOX"\r\n* LIST (\\HasNoChildren \\Trash) "/" "Trash"\r\n%s OK done\r\n' % tag)
            elif cmd == 'STATUS':
                send('* STATUS %s (MESSAGES %d UNSEEN 0)\r\n%s OK done\r\n' % (rest.split()[0], len(MSGS), tag))
            elif cmd == 'SELECT':
                send('* %d EXISTS\r\n* 0 RECENT\r\n* OK [UIDVALIDITY 1] ok\r\n* OK [UIDNEXT %d] ok\r\n* FLAGS (\\Seen)\r\n%s OK [READ-WRITE] selected\r\n' % (len(MSGS), MSGS[-1]['uid'] + 1, tag))
            elif cmd == 'FETCH' or (cmd == 'UID' and rest.upper().startswith('FETCH')):
                if cmd == 'UID': rest = rest[6:]
                rng, items = rest.split(' ', 1)
                items = items.upper()
                if 'BODY.PEEK[' in items:
                    uid = int(rng)
                    m = [x for x in MSGS if x['uid'] == uid][0]
                    seq = MSGS.index(m) + 1
                    part = items[items.index('<') + 1:items.index('>')]
                    off, ln = [int(x) for x in part.split('.')]
                    data = m['text'][off:off + ln]
                    send('* %d FETCH (UID %d BODY[1]<%d> {%d}\r\n' % (seq, uid, off, len(data)))
                    send(data)
                    send(')\r\n%s OK fetched\r\n' % tag)
                else:
                    lo, hi = rng.split(':') if ':' in rng else (rng, rng)
                    for seq, m in enumerate(MSGS, 1):
                        key = m['uid'] if cmd == 'UID' else seq
                        lo_v = int(lo)
                        hi_v = 10 ** 9 if hi == '*' else int(hi)
                        if lo_v <= key <= hi_v: send(fetch_items(m, seq, items))
                    send('%s OK fetched\r\n' % tag)
            elif cmd == 'UID' and rest.upper().startswith('STORE'):
                send('%s OK stored\r\n' % tag)
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
