# PsiMail tests

Everything here runs on a PC. The engine is tested two ways: built for the
PC (`make TARGET=host` → `build/mail-host/psimail-host`) and as the real
Psion ARM code in an emulator (`make TARGET=epoc emu` →
`mail/emu/run_psimail.py`, which needs `pip install unicorn pefile`).
Both take the same commands and read the account from `PM_*` variables.

## A local server

* IMAP: Dovecot with `dovecot.conf` (replace `@DIR@` with a scratch folder
  holding `cert.pem`/`key.pem` from
  `openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 30 -subj /CN=localhost -addext subjectAltName=DNS:localhost`;
  it needs a non-root `mailtest` user). Password `secret`, port 9993 (TLS).
* `populate.py 127.0.0.1 9993 dan@example.com secret` fills the Inbox with
  the awkward cases: UTF-8 headers, HTML-only, multipart/alternative,
  attachments (one with an RFC 2231 name), Latin-1 quoted-printable,
  format=flowed and a 200 KB message.
* SMTP: `smtpserver.py 9465 cert.pem key.pem secret OUTDIR` (implicit TLS),
  add `--starttls` for port-587 style. Messages land in OUTDIR.

Then, for example:

    export PM_HOST=localhost PM_PORT=9993 PM_SMTP=localhost PM_SMTP_PORT=9465 \
           PM_USER=dan@example.com PM_PASS=secret
    psimail-host folders , trustlast , sync INBOX , body INBOX 61 , attach INBOX 64 2
    mail/emu/run_psimail.py --align --stack --count sendrecv , body INBOX 67

(`trustlast` accepts the self-signed test certificate, as the app's
"Trust this server's key?" does.)

## Certificates

`certtest.c` + `certtest.py` check the chain code on a saved chain, e.g.
Fastmail's (`../tools/fastmail-chain.pem`): trusted for imap.fastmail.com,
refused for another name, refused as expired with `PM_NOW` in 2030.

## Against Fastmail

With no password, `PM_HOST=imap.fastmail.com PM_USER=x@fastmail.com
PM_PASS=wrong psimail-host folders` should get as far as the login
("Incorrect username, password or access token"): TLS 1.3, the chain up
to ISRG Root X1 and the server's RSA-PSS signature have all been checked
by then. In the emulator that first connection costs ~108 million ARM
instructions (~7 s on a 5mx); later ones ~33 million (~2 s), because the
checked intermediates are remembered in `certs.txt`.

## Calendar

A local CalDAV server: `pip install radicale`, then run it with a config
like

    [server]
    hosts = 127.0.0.1:5232
    [auth]
    type = htpasswd
    htpasswd_filename = /tmp/radicale/users     (dan@example.com:secret)
    htpasswd_encryption = plain
    [storage]
    filesystem_folder = /tmp/radicale/collections

make two calendars (`curl -u dan@example.com:secret -X MKCALENDAR
http://127.0.0.1:5232/dan@example.com/work/`, and `home`) and fill them with
`calpopulate.py`: a timed event with a TZID and an alarm, a weekly series
with an exception date, an all-day event over three days, a yearly
birthday, a cancelled event. Then

    PM_CAL_HOST=127.0.0.1 PM_CAL_PORT=5232 PM_CAL_PLAIN=1 PM_USER=dan@example.com \
      PM_PASS=secret psimail-host -s /tmp/pmcal cal

writes `/tmp/pmcal/cal/events.txt`; the same in the emulator
(`emu/run_psimail.py cal`) must give the same file. Changes to send go in
`cal/push.txt` (formats at the top of `engine/caldav.c`). `PM_CAL_ZONE`
picks the time zone (1 = London, 2 = Paris...). The Agenda side
(`app/pmcal.cpp`) needs a real Psion.

Against Fastmail: `caldav.fastmail.com`'s chain (`../tools/fastmail-caldav-chain.pem`,
Let's Encrypt YR2 via ISRG Root YR, cross-signed by ISRG Root X1) passes
`certtest`.
