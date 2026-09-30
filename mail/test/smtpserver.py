#!/usr/bin/env python3
"""smtpserver.py - a test SMTP server for PsiMail: implicit TLS (like
Fastmail's port 465) or STARTTLS, AUTH PLAIN/LOGIN with any user and the
password given; saves each message it gets to OUTDIR/<n>.eml.

    smtpserver.py PORT CERT KEY PASSWORD OUTDIR [--starttls]
"""
import ssl, sys, os, asyncio, itertools
from aiosmtpd.controller import Controller
from aiosmtpd.smtp import AuthResult, LoginPassword

port, cert, key, pw, outdir = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
starttls = "--starttls" in sys.argv
os.makedirs(outdir, exist_ok=True)
counter = itertools.count(1)

class Handler:
    async def handle_DATA(self, server, session, envelope):
        n = next(counter)
        with open(os.path.join(outdir, "%d.eml" % n), "wb") as f:
            f.write(b"X-Mail-From: " + envelope.mail_from.encode() + b"\r\n")
            f.write(b"X-Rcpt-To: " + ",".join(envelope.rcpt_tos).encode() + b"\r\n")
            f.write(envelope.original_content)
        return "250 OK queued as %d" % n

def auth(server, session, envelope, mechanism, data):
    if isinstance(data, LoginPassword) and data.password.decode() == pw:
        return AuthResult(success=True)
    return AuthResult(success=False, handled=False)

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
# (implicit TLS: aiosmtpd cannot tell the session is encrypted)
kw = dict(hostname="127.0.0.1", port=port, authenticator=auth, auth_require_tls=starttls)
if starttls:
    c = Controller(Handler(), tls_context=ctx, require_starttls=True, **kw)
else:
    c = Controller(Handler(), server_kwargs={"tls_context": None}, ssl_context=ctx, **kw)
c.start()
print("smtp listening on", port, flush=True)
try:
    asyncio.get_event_loop().run_forever()
except KeyboardInterrupt:
    pass
