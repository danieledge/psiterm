#!/usr/bin/env python3
"""populate.py - fills a test IMAP account with the kinds of mail PsiMail
has to cope with. Usage: populate.py HOST PORT USER PASS [N]"""
import imaplib, ssl, sys, time
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from email.mime.application import MIMEApplication
from email.header import Header
from email.utils import formatdate, make_msgid, formataddr

host, port, user, pw = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
n_filler = int(sys.argv[5]) if len(sys.argv) > 5 else 60

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
M = imaplib.IMAP4_SSL(host, port, ssl_context=ctx)
M.login(user, pw)

def put(msg, folder="INBOX", flags=""):
    msg["Date"] = msg.get("Date") or formatdate(localtime=True)
    msg["Message-ID"] = msg.get("Message-ID") or make_msgid(domain="example.com")
    M.append(folder, flags, imaplib.Time2Internaldate(time.time()), msg.as_bytes())

for i in range(n_filler):
    m = MIMEText("Filler message number %d.\nSecond line.\n" % i)
    m["From"] = "Filler Bot <filler@example.com>"
    m["To"] = user
    m["Subject"] = "Filler %03d" % i
    put(m, flags="(\\Seen)" if i % 3 else "")

m = MIMEText("Hello Dan,\n\nThis is a plain message with café, €5 and “quotes” — and an emoji \U0001F600.\n\n-- \nAlice\n", "plain", "utf-8")
m["From"] = str(Header("Alice Ångström", "utf-8")) + " <alice@example.com>"
m["To"] = user
m["Cc"] = "Bob <bob@example.org>, carol@example.net"
m["Reply-To"] = "alice-replies@example.com"
m["Subject"] = Header("Café plans – für morgen", "utf-8")
put(m)

html = """<html><head><style>p{color:red}</style><title>x</title></head><body>
<h1>Newsletter</h1><p>Read <a href="https://example.com/article?id=1">the article</a> &amp; enjoy&nbsp;it.</p>
<ul><li>One</li><li>Two &euro;</li></ul><img src="x.png" alt="logo"><script>alert(1)</script>
<table><tr><td>A</td><td>B</td></tr></table>&#8212; end &#x263A;</body></html>"""
m = MIMEText(html, "html", "utf-8")
m["From"] = "News <news@example.com>"
m["To"] = user
m["Subject"] = "HTML only"
put(m)

alt = MIMEMultipart("alternative")
alt.attach(MIMEText("Plain version of the alternative message.\n", "plain", "utf-8"))
alt.attach(MIMEText("<p>HTML version</p>", "html", "utf-8"))
alt["From"] = "Alt <alt@example.com>"
alt["To"] = user
alt["Subject"] = "Alternative"
put(alt)

mix = MIMEMultipart("mixed")
mix.attach(MIMEText("See the attached files.\n", "plain", "utf-8"))
pdf = MIMEApplication(bytes(range(256)) * 40, "pdf")
pdf.add_header("Content-Disposition", "attachment", filename="report.pdf")
mix.attach(pdf)
txt = MIMEText("attached text file\n", "plain", "utf-8")
txt.add_header("Content-Disposition", "attachment", filename=("utf-8", "", "résumé.txt"))
mix.attach(txt)
mix["From"] = "Files <files@example.com>"
mix["To"] = user
mix["Subject"] = "With attachments"
put(mix)

m = MIMEText("Olé! Straße café naïve =equals= " + "long line " * 20 + "\n", "plain", "iso-8859-1")
m.replace_header("Content-Transfer-Encoding", "quoted-printable") if False else None
m["From"] = "latin@example.com"
m["To"] = user
m["Subject"] = "Latin-1 QP"
put(m)

m = MIMEText("This is a flowed paragraph that \nwas wrapped by the sender's \nmail program.\n\nNew paragraph.\n", "plain", "utf-8")
m.set_param("format", "flowed")
m["From"] = "flow@example.com"
m["To"] = user
m["Subject"] = "Flowed"
put(m)

rich = """<html><head><style>.x{}</style></head><body>
<h1>The Weekly Psion</h1>
<p>Hello <b>Dan</b>, here is what's <i>new</i> this week. Read the <a href="https://example.com/full">full story online</a>.</p>
<h2>In this issue</h2>
<ul><li>Series 5mx battery tips</li><li>A <b>new</b> email client</li><li>Readers' letters</li></ul>
<ol><li>First numbered</li><li>Second numbered</li></ol>
<blockquote>The Psion 5mx keyboard is still the best ever made on a small computer.<blockquote>Agreed!</blockquote></blockquote>
<p>Some code:</p><pre>10 PRINT "HELLO"
20 GOTO 10</pre>
<img src="cid:logo" alt="Psion logo"><hr>
<table><tr><td>Price</td><td>&pound;49</td></tr><tr><td>Stock</td><td>3 left</td></tr></table>
<p style="font-size:10px">Unsubscribe: <a href="https://example.com/unsub?id=12345">click here</a></p>
</body></html>"""
m = MIMEMultipart("alternative")
m.attach(MIMEText("plain fallback", "plain", "utf-8"))
m.attach(MIMEText(rich, "html", "utf-8"))
m["From"] = "The Weekly Psion <news@psion.example>"
m["To"] = user
m["Subject"] = "The Weekly Psion - issue 42"
put(m)
m = MIMEText(rich, "html", "utf-8")
m["From"] = "HTML Only <htmlonly@example.com>"
m["To"] = user
m["Subject"] = "Rich HTML only"
put(m)
m = MIMEText("""Sounds good, see you at 7.

Bob

On Mon, Alice wrote:
> Shall we meet at the cafe tomorrow? I can bring the Psion
> and show you the new mail program. It even does HTML now,
> see www.example.com/psimail for details.
>
> On Sun, Bob wrote:
>> Are you free this week?
>> Let me know.

-- 
Bob Smith
https://bob.example.org
""", "plain", "utf-8")
m["From"] = "Bob Smith <bob@example.org>"
m["To"] = user
m["Subject"] = "Re: Tomorrow"
put(m)

big = "".join("Line %05d of a long message, padded to be quite long indeed.\n" % i for i in range(2500))
m = MIMEText(big, "plain", "utf-8")
m["From"] = "big@example.com"
m["To"] = user
m["Subject"] = "Big message"
put(m)

M.logout()
print("ok")
