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

`PM_OVERRUN=60000,120000` (host build) throws away 200 bytes of what
arrives at those points, as a serial overrun without RTS/CTS does: the
body and attachment fetches should log in again and carry on from where
they were, and the result should match a clean run.

(`trustlast` accepts the self-signed test certificate, as the app's
"Trust this server's key?" does.)

## The modem line (no server needed)

`fakeimap.py PORT [--drop-at N,N.. | --drop-every N] [--slow MS_PER_KB]` is
a plaintext IMAP server with five messages: one whose text has "NO CARRIER"
on a line of its own, one of 30 KB (several pieces), three small ones. It
can drop the TCP connection after N bytes (a modem line dropping) and pace
its output (87 = 115200 baud). The host build runs the modem-mode code with
`PM_NETMODE=0` (with `PM_MODEM=1` the fake modem says NO CARRIER when the
server closes), downloads ahead with `PM_PREFETCH=n`, and the `loop` command
runs the engine's own loop (pf_step) until it has been quiet for 3 s.
`PM_INJECT_MS=ms PM_INJECT_UID=uid` queues a BODY that long into `loop`, as
the app would while a download ahead runs: the download ahead should pause
for it and carry on afterwards, all on one connection.

    python3 fakeimap.py 1143 --drop-at 12000 --slow 20 &
    PM_HOST=127.0.0.1 PM_PORT=1143 PM_TLS=0 PM_USER=x PM_PASS=x \
    PM_NETMODE=0 PM_MODEM=1 PM_PREFETCH=5 psimail-host -s /tmp/store sync INBOX , loop

Expected: all five texts in the store, one "lost #1 ... NO CARRIER" and one
"dropped at ... again" in the log, two connections in all. Three drops
(`--drop-at 9000,20000,30000`) end with "download ahead is off for 10
minutes: the line keeps dropping".

## The Psion Internet route (PPP)

The same fake server with `PM_NETMODE=1` (the default) runs the code the
Psion Internet route uses: the connection is a socket, "hang up" only
closes it, and the dial-up itself (psiglue's StartPpp, NetDial) has no
stand-in. What can be checked here:

* a drop is a TCP-level reconnect, not a redial: the run above with
  `PM_NETMODE=1` and no `PM_MODEM` gives "lost #1: The connection was lost",
  "dropped at ... again", two connections and all five texts.
* every close closes the socket. The host's `pg_dial` prints
  `WARNING: dialling with the last connection still open` if a connection
  was left open (as `pmn_close(0)` after SMTP's QUIT once did: on the Psion
  that leaked an ESOCK handle whose stale receive made the next connection
  look dropped, and PPP was then taken down and dialled again). A send
  followed by a sync (`sendrecv` with a message in `A0/outbox/`, against
  any plain SMTP on `PM_SMTP_PORT`) must not print it.
* an idle connection a router has quietly dropped is noticed in 15 s, not
  60, and does not count as a line drop. `--mute-noop` makes the server
  swallow a NOOP and everything after it; `sleep MS` leaves the connection
  idle between commands as the app does while a message is read:

      python3 fakeimap.py 1143 --mute-noop &
      PM_HOST=127.0.0.1 PM_PORT=1143 PM_TLS=0 PM_USER=x PM_PASS=x \
      psimail-host -s /tmp/store sync INBOX , sleep 31000 , body INBOX 101

  Expected: "the connection idle for 31 s had gone (2): connecting again"
  about 15 s after the sleep, then `body: OK` on a second connection.

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
