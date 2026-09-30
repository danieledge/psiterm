#!/usr/bin/env python3
"""calpopulate.py - test events for PsiMail's calendar on a CalDAV server
(Radicale: see test/README.md). Dates are relative to today.

    calpopulate.py [URL] [USER] [PASS]
"""
import sys, datetime, urllib.request, base64

URL = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:5232/dan@example.com/"
USER = sys.argv[2] if len(sys.argv) > 2 else "dan@example.com"
PASS = sys.argv[3] if len(sys.argv) > 3 else "secret"
auth = "Basic " + base64.b64encode(f"{USER}:{PASS}".encode()).decode()
today = datetime.date.today()
def d(n): return (today + datetime.timedelta(days=n)).strftime("%Y%m%d")

TZ = """BEGIN:VTIMEZONE
TZID:Europe/London
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
"""
events = {
 "work/lunch.ics": f"""BEGIN:VEVENT
UID:lunch-1
DTSTAMP:20260101T000000Z
DTSTART;TZID=Europe/London:{d(1)}T123000
DTEND;TZID=Europe/London:{d(1)}T133000
SUMMARY:Lunch with Alice \\, at the café
LOCATION:The Old Bank\; Oxford
BEGIN:VALARM
ACTION:DISPLAY
DESCRIPTION:Lunch
TRIGGER:-PT15M
END:VALARM
DESCRIPTION:Bring the Psion.
END:VEVENT
""",
 "work/standup.ics": f"""BEGIN:VEVENT
UID:standup-1
DTSTAMP:20260101T000000Z
DTSTART;TZID=Europe/London:{d(-14)}T093000
DURATION:PT15M
RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR;COUNT=30
EXDATE;TZID=Europe/London:{d(-14)}T093000
SUMMARY:Stand-up
END:VEVENT
""",
 "work/review.ics": f"""BEGIN:VEVENT
UID:review-1
DTSTAMP:20260101T000000Z
DTSTART:{d(3)}T140000Z
DTEND:{d(3)}T153000Z
SUMMARY:Design review — PsiMail
END:VEVENT
""",
 "home/holiday.ics": f"""BEGIN:VEVENT
UID:holiday-1
DTSTAMP:20260101T000000Z
DTSTART;VALUE=DATE:{d(10)}
DTEND;VALUE=DATE:{d(13)}
SUMMARY:Holiday in Cornwall
END:VEVENT
""",
 "home/birthday.ics": f"""BEGIN:VEVENT
UID:birthday-1
DTSTAMP:20260101T000000Z
DTSTART;VALUE=DATE:{d(5)}
DURATION:P1D
RRULE:FREQ=YEARLY
SUMMARY:Mum's birthday
END:VEVENT
""",
 "home/cancelled.ics": f"""BEGIN:VEVENT
UID:cancel-1
DTSTAMP:20260101T000000Z
DTSTART:{d(2)}T180000Z
DTEND:{d(2)}T190000Z
STATUS:CANCELLED
SUMMARY:Cancelled thing
END:VEVENT
""",
}
for path, ev in events.items():
    body = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//test//EN\r\n" + (TZ if "TZID" in ev else "") + ev + "END:VCALENDAR\r\n"
    body = body.replace("\r\n", "\n").replace("\n", "\r\n")
    req = urllib.request.Request(URL + path, data=body.encode(), method="PUT",
        headers={"Authorization": auth, "Content-Type": "text/calendar; charset=utf-8"})
    print(path, urllib.request.urlopen(req).status)
