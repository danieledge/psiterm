#!/usr/bin/env python3
"""invitehost.py [OUTDIR]

Invitations and contact cards through the host engine (build/mail-host/
psimail-host, from `mail/build.sh host`), end to end, with no real server:

1. fakeimap.py --invite serves an invitation (text/calendar, base64) and a
   message with a vCard attached; the engine downloads both messages and
   must leave <uid>.ics/.inv and <uid>.vcf/.vcd beside them (invite.h).
2. The reply PsiMail.app writes for Accept (an outbox message with Itip
   headers, as app/pminvite.cpp does) is sent to a capturing SMTP server:
   it must go to the organiser as multipart/alternative with a
   text/calendar; method=REPLY part saying PARTSTAT=ACCEPTED.
3. The store moved as PsiMail.app moves it at start-up (\\PsiMail\\ renamed
   to \\System\\Data\\PsiMail\\, app/pmstore.cpp): the engine, pointed at the
   new place, must find everything there - no message fetched again.
"""
import base64, email, os, shutil, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
HOST = os.path.join(TOP, "build", "mail-host", "psimail-host")
# (a short folder: the engine's store_dir holds 95 characters, plenty for
# D:\System\Data\PsiMail\ but not for a deep build folder)
out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="pminv")
shutil.rmtree(out, ignore_errors=True)
os.makedirs(out)
if not os.path.exists(HOST):
    sys.exit("build the host engine first: PSION_SDK=... mail/build.sh host")

IMAP_PORT, SMTP_PORT = 11743, 11744
imap = subprocess.Popen([sys.executable, os.path.join(HERE, "fakeimap.py"), str(IMAP_PORT), "--invite"],
                        stderr=open(os.path.join(out, "fakeimap.log"), "w"))

# a plaintext SMTP server that keeps what it is sent
captured = []
def smtp_server():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", SMTP_PORT)); s.listen(2)
    while True:
        c, _ = s.accept()
        f = c.makefile("rb")
        c.sendall(b"220 capture ready\r\n")
        rcpts = []
        while True:
            line = f.readline()
            if not line: break
            u = line.decode("latin-1").strip().upper()
            if u.startswith("EHLO"): c.sendall(b"250-capture\r\n250-AUTH PLAIN LOGIN\r\n250 SIZE 1000000\r\n")
            elif u.startswith("AUTH"): c.sendall(b"235 ok\r\n")
            elif u.startswith("RCPT"): rcpts.append(line.decode().strip()); c.sendall(b"250 ok\r\n")
            elif u.startswith("DATA"):
                c.sendall(b"354 go\r\n")
                data = b""
                while True:
                    d = f.readline()
                    if not d or d == b".\r\n": break
                    data += d[1:] if d.startswith(b"..") else d
                captured.append((rcpts, data))
                c.sendall(b"250 queued\r\n")
            elif u.startswith("QUIT"): c.sendall(b"221 bye\r\n"); break
            else: c.sendall(b"250 ok\r\n")
        c.close()
threading.Thread(target=smtp_server, daemon=True).start()
time.sleep(0.5)

env = dict(os.environ, PM_HOST="127.0.0.1", PM_PORT=str(IMAP_PORT), PM_TLS="0", PM_USER="dan@example.com",
           PM_PASS="x", PM_EMAIL="dan@example.com", PM_NAME="Dan Edge", PM_SMTP="127.0.0.1",
           PM_SMTP_PORT=str(SMTP_PORT), PM_SMTP_TLS="0", PM_NETMODE="1")
def engine(store, *cmds):
    r = subprocess.run([HOST, "-s", store] + list(cmds), env=env, capture_output=True, text=True, timeout=120)
    with open(os.path.join(out, "engine.log"), "a") as f: f.write(r.stdout + r.stderr)
    return r.stdout + r.stderr

bad = 0
def expect(what, ok, detail=""):
    global bad
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else "  " + detail))
    if not ok: bad += 1

try:
    # 1. the old layout: <disk>/PsiMail/ (what 0.73 made)
    old = os.path.join(out, "disk", "PsiMail") + "/"
    os.makedirs(old)
    log = engine(old, "sync", "INBOX", ",", "body", "INBOX", "106", ",", "body", "INBOX", "107")
    folder = os.path.join(old, "A0", "F4A1E411B")
    def read(name): return open(os.path.join(folder, name), encoding="cp1252").read()
    have = set(os.listdir(folder))
    expect("invitation fetched and summed up (106.ics, 106.inv)", {"106.ics", "106.inv"} <= have, str(sorted(have)))
    expect("contact card fetched and summed up (107.vcf, 107.vcd)", {"107.vcf", "107.vcd"} <= have, str(sorted(have)))
    inv = dict(l.split("\t", 1) for l in read("106.inv").splitlines()[1:] if "\t" in l)
    expect("invitation: method, summary, organiser",
           (inv.get("method"), inv.get("summary"), inv.get("orgaddr")) == ("request", "Design review", "ann@example.com"), str(inv))
    vcd = read("107.vcd")
    expect("card: name, email, mobile", "fn\tBob Jones" in vcd and "\tbob@example.org" in vcd and "tel4\t07700 900456" in vcd, vcd)

    # 2. Accept: the outbox message PsiMail.app writes (app/pminvite.cpp)
    box = os.path.join(old, "A0", "outbox")
    os.makedirs(box, exist_ok=True)
    with open(os.path.join(box, "0001.txt"), "w", encoding="cp1252", newline="\n") as f:
        f.write("#PSIMAIL1\nTo: Ann <ann@example.com>\nSubject: Accepted: Design review\n"
                "Itip: ACCEPTED\nItip-Uid: %s\nItip-Seq: %s\nItip-Organizer: %s\nItip-Attendee: dan@example.com\n"
                "Itip-Name: Dan Edge\nItip-Summary: %s\nItip-Start: %s\nItip-End: %s\n\n"
                "Dan Edge has accepted this invitation.\n" %
                (inv["uid"], inv["seq"], inv["orgline"], inv["summary"], inv["utcstart"], inv["utcend"]))
    log = engine(old, "send")
    expect("the reply was sent", len(captured) == 1, log[-400:])
    if captured:
        rcpts, data = captured[0]
        msg = email.message_from_bytes(data)
        expect("to the organiser", any("ann@example.com" in r for r in rcpts), str(rcpts))
        expect("multipart/alternative", msg.get_content_type() == "multipart/alternative", msg.get_content_type())
        cal = [p for p in msg.walk() if p.get_content_type() == "text/calendar"]
        expect("a text/calendar part, method=REPLY", cal and cal[0].get_param("method") == "REPLY")
        if cal:
            ics = cal[0].get_payload(decode=True).decode("utf-8")
            open(os.path.join(out, "reply.ics"), "w").write(ics)
            un = ics.replace("\r\n ", "")
            expect("PARTSTAT=ACCEPTED for us", "ATTENDEE;PARTSTAT=ACCEPTED;CN=\"Dan Edge\":mailto:dan@example.com" in un, un)
            expect("the invitation's UID", "UID:fake-invite-1@example.com" in un, un)
        expect("the outbox is empty afterwards", not [n for n in os.listdir(box) if n.endswith(".txt")], str(os.listdir(box)))

    # 3. the store moved as the app moves it, then used from there
    new = os.path.join(out, "disk", "System", "Data", "PsiMail") + "/"
    before = sorted(os.listdir(folder))
    if os.path.isdir(old) and not os.path.isdir(new):
        os.makedirs(os.path.dirname(new.rstrip("/")), exist_ok=True)
        os.rename(old.rstrip("/"), new.rstrip("/"))            # one rename, as RFs::Rename
    expect("old store gone, new one there", not os.path.exists(old) and os.path.isdir(new))
    log = engine(new, "sync", "INBOX")
    expect("the engine finds the moved store: nothing new to fetch", "No new messages" in log, log[-300:])
    after = sorted(os.listdir(os.path.join(new, "A0", "F4A1E411B")))
    expect("every file still there after a sync", set(before) <= set(after), "%s vs %s" % (before, after))
    log = engine(new, "body", "INBOX", "106")
    expect("a message read again from the new place", "body: OK" in log, log[-300:])
finally:
    imap.terminate()

print("invitehost:", "FAILED (%d)" % bad if bad else "all passed")
sys.exit(1 if bad else 0)
