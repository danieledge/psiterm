#!/usr/bin/env python3
"""invtest.py [OUTDIR]

The invitation (.ics / iTIP) and contact card (.vcf) parsers in
mail/engine/invite.c on a PC: builds invtest.c with AddressSanitizer and
UBSan, writes sample files as Google Calendar, Outlook, Apple and phones
send them, and checks what the parser makes of each - the times on the
Psion's clock (time zones described in the file, named only, UTC, all
day), the organiser, the reply PsiMail would send, a cancellation, a reply
from someone else; vCard 2.1 (quoted-printable, a charset), 3.0 (Apple's
item groups) and 4.0. Any wrong answer or sanitizer report fails the run.
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(TOP, "build", "invtest")
os.makedirs(out, exist_ok=True)
exe = os.path.join(out, "invtest-asan")
srcs = [os.path.join(HERE, "invtest.c")] + \
    [os.path.join(TOP, "mail/engine", f) for f in ("invite.c", "ics.c", "caltz.c", "charset.c", "mime.c", "imapparse.c")]
subprocess.check_call(["gcc", "-g", "-O1", "-std=gnu99", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       "-fno-sanitize-recover=undefined", "-I", os.path.join(TOP, "ssh"), "-o", exe] + srcs)

def crlf(s): return s.strip("\n").replace("\n", "\r\n") + "\r\n"

SAMPLES = {
"google.ics": crlf("""BEGIN:VCALENDAR
PRODID:-//Google Inc//Google Calendar 70.9054//EN
VERSION:2.0
CALSCALE:GREGORIAN
METHOD:REQUEST
BEGIN:VTIMEZONE
TZID:Europe/London
X-LIC-LOCATION:Europe/London
BEGIN:DAYLIGHT
TZOFFSETFROM:+0000
TZOFFSETTO:+0100
TZNAME:BST
DTSTART:19700329T010000
RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU
END:DAYLIGHT
BEGIN:STANDARD
TZOFFSETFROM:+0100
TZOFFSETTO:+0000
TZNAME:GMT
DTSTART:19701025T020000
RRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU
END:STANDARD
END:VTIMEZONE
BEGIN:VEVENT
DTSTART;TZID=Europe/London:20261007T100000
DTEND;TZID=Europe/London:20261007T113000
DTSTAMP:20260930T120000Z
ORGANIZER;CN=Alice Smith:mailto:alice@example.com
UID:7kukuqrfedlm2f9t7b2e3d9lsa@google.com
ATTENDEE;CUTYPE=INDIVIDUAL;ROLE=REQ-PARTICIPANT;PARTSTAT=ACCEPTED;RSVP=TRUE
 ;CN=Alice Smith;X-NUM-GUESTS=0:mailto:alice@example.com
ATTENDEE;CUTYPE=INDIVIDUAL;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=
 TRUE;CN=dan@example.com;X-NUM-GUESTS=0:mailto:dan@example.com
CREATED:20260930T115900Z
DESCRIPTION:Let's go over the design.
LAST-MODIFIED:20260930T120000Z
LOCATION:Meeting room B\\, 2nd floor
SEQUENCE:0
STATUS:CONFIRMED
SUMMARY:Design review – PsiMail 0.74
TRANSP:OPAQUE
BEGIN:VALARM
ACTION:DISPLAY
DESCRIPTION:This is an event reminder
TRIGGER:-P0DT0H10M0S
END:VALARM
END:VEVENT
END:VCALENDAR
"""),
"outlook.ics": crlf("""BEGIN:VCALENDAR
METHOD:REQUEST
PRODID:Microsoft Exchange Server 2010
VERSION:2.0
BEGIN:VTIMEZONE
TZID:GMT Standard Time
BEGIN:STANDARD
DTSTART:16010101T020000
TZOFFSETFROM:+0100
TZOFFSETTO:+0000
RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10
END:STANDARD
BEGIN:DAYLIGHT
DTSTART:16010101T010000
TZOFFSETFROM:+0000
TZOFFSETTO:+0100
RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=3
END:DAYLIGHT
END:VTIMEZONE
BEGIN:VEVENT
ORGANIZER;CN="Bob Jones":mailto:bob@example.org
ATTENDEE;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE;CN=Dan Edge:
 mailto:Dan@Example.com
DESCRIPTION;LANGUAGE=en-GB:Quarterly figures\\n
UID:040000008200E00074C5B7101A82E00800000000A0B1C2D3E4F5
SUMMARY;LANGUAGE=en-GB:Quarterly figures
DTSTART;TZID=GMT Standard Time:20261102T140000
DTEND;TZID=GMT Standard Time:20261102T150000
CLASS:PUBLIC
PRIORITY:5
DTSTAMP:20260930T101010Z
TRANSP:OPAQUE
STATUS:CONFIRMED
SEQUENCE:2
LOCATION;LANGUAGE=en-GB:Teams
END:VEVENT
END:VCALENDAR
"""),
"newyork.ics": crlf("""BEGIN:VCALENDAR
VERSION:2.0
METHOD:REQUEST
BEGIN:VEVENT
UID:ny-1@example.net
DTSTART;TZID=America/New_York:20261007T090000
DURATION:PT45M
SUMMARY:Call with New York
ORGANIZER:mailto:carol@example.net
ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:dan@example.com
RRULE:FREQ=WEEKLY;COUNT=4
END:VEVENT
BEGIN:VEVENT
UID:ny-1@example.net
RECURRENCE-ID;TZID=America/New_York:20261014T090000
DTSTART;TZID=America/New_York:20261014T100000
DURATION:PT45M
SUMMARY:Call with New York (moved)
ORGANIZER:mailto:carol@example.net
END:VEVENT
END:VCALENDAR
"""),
"allday.ics": crlf("""BEGIN:VCALENDAR
VERSION:2.0
PRODID:-//Example//EN
BEGIN:VEVENT
UID:allday-2@example.com
DTSTART;VALUE=DATE:20261015
DTEND;VALUE=DATE:20261017
SUMMARY:Conference
LOCATION:Birmingham
END:VEVENT
END:VCALENDAR
"""),
"cancel.ics": crlf("""BEGIN:VCALENDAR
VERSION:2.0
METHOD:CANCEL
BEGIN:VEVENT
UID:7kukuqrfedlm2f9t7b2e3d9lsa@google.com
DTSTART:20261007T090000Z
DTEND:20261007T103000Z
SEQUENCE:1
STATUS:CANCELLED
SUMMARY:Design review – PsiMail 0.74
ORGANIZER;CN=Alice Smith:mailto:alice@example.com
ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:dan@example.com
END:VEVENT
END:VCALENDAR
"""),
"reply.ics": crlf("""BEGIN:VCALENDAR
VERSION:2.0
METHOD:REPLY
BEGIN:VEVENT
UID:psion-event-1
DTSTART:20261008T130000Z
SUMMARY:Lunch
ORGANIZER:mailto:dan@example.com
ATTENDEE;PARTSTAT=DECLINED;CN="Eve Adams":mailto:eve@example.com
END:VEVENT
END:VCALENDAR
"""),
"phone21.vcf": crlf("""BEGIN:VCARD
VERSION:2.1
N;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:M=C3=BCller;J=C3=BCrgen;;Dr.;
FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:Dr. J=C3=BCrgen M=C3=BCller
TEL;CELL:+49 170 1234567
TEL;WORK;VOICE:+49 30 123456
TEL;WORK;FAX:+49 30 123457
EMAIL;INTERNET:juergen@example.de
ADR;WORK;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:;;Hauptstra=C3=9Fe 1=0D=0AHof 2;Berlin;;10115;=
Germany
ORG:Beispiel GmbH;Entwicklung
TITLE:Entwickler
END:VCARD
"""),
"apple30.vcf": crlf("""BEGIN:VCARD
VERSION:3.0
PRODID:-//Apple Inc.//macOS 15.0//EN
N:Smith;Alice;;;
FN:Alice Smith
ORG:Example Ltd;
TITLE:Head of Design
item1.EMAIL;type=INTERNET;type=pref:alice@example.com
EMAIL;type=INTERNET;type=HOME:alice.home@example.net
TEL;type=CELL;type=VOICE;type=pref:07700 900123
TEL;type=HOME;type=VOICE:01632 960123
item2.ADR;type=HOME;type=pref:;;1 High Street;Bath;;BA1 1AA;United Kingdom
item2.X-ABADR:gb
URL;type=pref:https://example.com/alice
NOTE:Met at the Psion meetup\\, 2026
PHOTO;ENCODING=b;TYPE=JPEG:/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJ
 C4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/
END:VCARD
BEGIN:VCARD
VERSION:3.0
FN:Bob Jones
EMAIL:bob@example.org
END:VCARD
"""),
"v40.vcf": crlf("""BEGIN:VCARD
VERSION:4.0
FN:Grace Hopper
N:Hopper;Grace;Brewster Murray;Rear Admiral;
EMAIL;TYPE=work:grace@example.mil
TEL;VALUE=uri;TYPE="voice,work":tel:+1-555-555-0100
TEL;VALUE=uri;TYPE=cell:tel:+1-555-555-0199
ADR;TYPE=work:;;1 Navy Way;Arlington;VA;22202;USA
END:VCARD
"""),
}
for name, text in SAMPLES.items():
    with open(os.path.join(out, name), "wb") as f:
        f.write(text.encode("utf-8"))

env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1", UBSAN_OPTIONS="print_stacktrace=1")
def run(*args):
    r = subprocess.run([exe] + [str(a) for a in args], env=env, capture_output=True)
    enc = "utf-8" if args[0] == "reply" else "cp1252"     # (a reply is UTF-8; the summaries cp1252)
    if r.returncode not in (0, 1) or r.stderr:
        print(r.stderr.decode("latin-1")); raise SystemExit("FAIL: invtest crashed on %s" % (args,))
    return r.stdout.decode(enc)

def fields(text):
    d = {}
    for line in text.splitlines():
        k, _, v = line.partition("\t")
        d.setdefault(k, v)
    return d

bad = 0
def expect(what, got, want):
    global bad
    ok = got == want
    print(("ok   " if ok else "FAIL ") + "%s: %r" % (what, got) + ("" if ok else " (wanted %r)" % (want,)))
    if not ok: bad += 1

g = fields(run("ics", os.path.join(out, "google.ics"), 1, "dan@example.com"))
expect("google method", g["method"], "request")
expect("google summary (UTF-8 dash -> cp1252)", g["summary"], "Design review – PsiMail 0.74")
expect("google location (escaped comma)", g["location"], "Meeting room B, 2nd floor")
expect("google start, London in summer", g["start"], "202610071000")
expect("google end", g["end"], "202610071130")
expect("google start in UTC", g["utcstart"], "20261007T090000Z")
expect("google organiser", (g["orgname"], g["orgaddr"]), ("Alice Smith", "alice@example.com"))
expect("google me / my status", (g["me"], g["mystatus"]), ("dan@example.com", "needs-action"))
expect("google alarm", g["alarm"], "10")
g9 = fields(run("ics", os.path.join(out, "google.ics"), 18, "dan@example.com"))
expect("google start on a Psion in New York", g9["start"], "202610070500")

o = fields(run("ics", os.path.join(out, "outlook.ics"), 1, "dan@example.com"))
expect("outlook start (GMT Standard Time, winter)", o["start"], "202611021400")
expect("outlook utc start", o["utcstart"], "20261102T140000Z")
expect("outlook organiser (quoted CN)", (o["orgname"], o["orgaddr"]), ("Bob Jones", "bob@example.org"))
expect("outlook me (address case differs)", o["me"], "Dan@Example.com")
expect("outlook sequence", o["seq"], "2")

n = fields(run("ics", os.path.join(out, "newyork.ics"), 1, "dan@example.com"))
expect("new york by name only, on a London Psion", n["start"], "202610071400")
expect("new york end (DURATION)", n["end"], "202610071445")
expect("new york repeats, master chosen", (n["repeats"], n["summary"]), ("1", "Call with New York"))
expect("new york events", n["events"], "2")

a = fields(run("ics", os.path.join(out, "allday.ics"), 1, "dan@example.com"))
expect("all day (no method: publish)", (a["method"], a["allday"]), ("publish", "1"))
expect("all day: first and last day", (a["start"], a["end"]), ("202610150000", "202610160000"))

c = fields(run("ics", os.path.join(out, "cancel.ics"), 1, "dan@example.com"))
expect("cancel", (c["method"], c["start"]), ("cancel", "202610071000"))
r = fields(run("ics", os.path.join(out, "reply.ics"), 1, "dan@example.com"))
expect("reply from someone else", (r["method"], r["replyname"], r["replyaddr"], r["replystatus"]), ("reply", "Eve Adams", "eve@example.com", "declined"))

rep = run("reply", os.path.join(out, "google.ics"), 1, "dan@example.com", "ACCEPTED", "Dan Edge")
print(rep)
lines = rep.replace("\r\n ", "").split("\r\n")
expect("reply METHOD", "METHOD:REPLY" in lines, True)
expect("reply UID", "UID:7kukuqrfedlm2f9t7b2e3d9lsa@google.com" in lines, True)
expect("reply attendee", 'ATTENDEE;PARTSTAT=ACCEPTED;CN="Dan Edge":mailto:dan@example.com' in lines, True)
expect("reply organiser as it came", "ORGANIZER;CN=Alice Smith:mailto:alice@example.com" in lines, True)
expect("reply start in UTC", "DTSTART:20261007T090000Z" in lines, True)
expect("reply lines folded at 75", max(len(l.encode("utf-8")) for l in rep.split("\r\n")) <= 75, True)
expect("reply summary in UTF-8", any(l.startswith("SUMMARY:Design review –") for l in lines), True)
rep2 = run("reply", os.path.join(out, "newyork.ics"), 1, "dan@example.com", "DECLINED", "")
expect("reply to a repeating event: no RECURRENCE-ID for the master", "RECURRENCE-ID" in rep2, False)
expect("reply refuses a status that isn't one", run("reply", os.path.join(out, "google.ics"), 1, "dan@example.com", "X\r\nATTENDEE:evil", "").strip(), "no reply")

v = run("vcf", os.path.join(out, "phone21.vcf"))
print(v)
vf = fields(v)
expect("vcard 2.1 name (QP, UTF-8)", (vf["given"], vf["family"], vf["prefix"]), ("Jürgen", "Müller", "Dr."))
expect("vcard 2.1 org (first part)", vf["org"], "Beispiel GmbH")
expect("vcard 2.1 phones", [l for l in v.splitlines() if l.startswith("tel")], ["tel4\t+49 170 1234567", "tel2\t+49 30 123456", "tel10\t+49 30 123457"])
expect("vcard 2.1 address (QP soft break, lines)", vf.get("adr2"), "\x01\x01Hauptstraße 1,Hof 2\x01Berlin\x01\x0110115\x01Germany")
v3 = run("vcf", os.path.join(out, "apple30.vcf"))
print(v3)
expect("vcard 3.0: two cards", v3.splitlines()[0], "#PSIVCD1\t2")
vf3 = fields(v3)
expect("vcard 3.0 grouped email", vf3["email0"], "alice@example.com")
expect("vcard 3.0 home email", vf3["email1"], "alice.home@example.net")
expect("vcard 3.0 note unescaped", vf3["note"], "Met at the Psion meetup, 2026")
expect("vcard 3.0 photo left out", "9j" in v3, False)
v4 = fields(run("vcf", os.path.join(out, "v40.vcf")))
expect("vcard 4.0 tel: URI", (v4["tel2"], v4["tel4"]), ("+1-555-555-0100", "+1-555-555-0199"))
expect("vcard 4.0 middle name", v4["middle"], "Brewster Murray")

print("invtest:", "FAILED (%d)" % bad if bad else "all passed")
sys.exit(1 if bad else 0)
