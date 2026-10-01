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

## Folders (no server needed)

`foldertest.sh` runs the folder commands against `fakeimap.py` (which keeps
a folder list: INBOX, Trash, Work and a child with a non-ASCII name):
`mkfolder PARENT NAME` (PARENT `-` is the top level), `renfolder FOLDER
NAME`, `delfolder FOLDER`. It checks the names go as modified UTF-7 under
the right parent, that folders.txt and the store directories follow, and
that the Inbox, a standard folder and a folder with children are refused.

## Undo (no server needed)

`undotest.sh` runs Edit > Undo's engine side (`undo FOLDER UID`, where the
message was moved from) against `fakeimap.py`, once with MOVE and UIDPLUS
(the uids come from COPYUID) and once with `--no-uidplus` (COPY, STORE
\Deleted and EXPUNGE; the message found again by its Message-ID): a
delete to the Trash and a move undone with the downloaded text coming back
and no "new mail" at the next check; a move made offline undone before it
reached the server (its pending.txt line goes); a move the server made,
undone offline (an UNMOVE line, sent at the next check, the row taking the
server's new uid); "Nothing to undo"; and a message deleted from the Trash
since ("Not undone - the message is no longer in Trash"). `fakeimap.py
--arrive N,M` puts a new message in the Inbox before its Nth SELECT, for
the new mail alert and the timed check.

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

## The picture decoders (no server needed)

`imgtest.py [OUTDIR] [--rounds N] [--pictures DIR]` builds `imgtest.c` with
the decoders in `mail/engine/img` under AddressSanitizer and UBSan, makes
sample pictures with Pillow (baseline JPEG in every subsampling;
progressive JPEG in 4:2:0, 4:4:4 and grey, with restart markers, cut short,
and at each shrink - full size, 1/2, 1/4 and the DC-only 1/8 - for
`pmjprog.c`; PNG in every colour type and interlaced, GIF plain, interlaced
and transparent, a 3000-pixel banner and a 5-megapixel photo for the 1/8
path), decodes each to `OUTDIR/*.pgm` to look at, checks the progressive
ones against Pillow's own decoding (within 10 grey levels of 255 after a
small blur to take out the dithering; they come out within 1) and against
the same picture saved as baseline, checks that a memory limit too small
for the coefficients makes it shrink more (`IMG_MAX_FULL=bytes`) or refuse,
then fuzzes each one (truncated, bit-flipped, overwritten) for N rounds.
Any crash, sanitizer report or leak fails it. `imgtest decode|fuzz|time|pmi`
can also be run by hand; `time` says how long a decode takes on the PC.

## The parsers of network text (no server needed)

`parsefuzz.py [--rounds N] [--seeds N]` builds `parsefuzz.c` with the
IMAP response parser (`imapparse.c`), MIME (`mime.c`: BODYSTRUCTURE, base64
and quoted-printable), HTML (`html.c`), the WebDAV XML reader
(`xmlscan.c`), iCalendar (`ics.c`: reading, editing and EXDATE), the
header charsets (`charset.c`), and invitations and contact cards
(`invite.c`: VTIMEZONE rules, the iTIP reply built from what was found,
vCard 2.1/3.0/4.0) under AddressSanitizer and UBSan, and feeds each one N
rounds of random and mutated input per seed, split at random points; then
again built with `-m32` (UBSan), where a `long` is 32 bits as on the Psion,
which caught times past 2038 and durations overflowing in 0.74. Any crash, sanitizer report or hang fails it (a round over a
second is reported). It found two hangs and an off-by-one in 0.69; add a
word list and a target when adding a parser. See also
`docs/epoc-robustness-best-practices.md`.

## Invitations, contact cards and the store's place (no server needed)

`invtest.py` checks `invite.c` on sample files as Google Calendar,
Outlook/Exchange, Apple and phones send them: times on the Psion's clock
from a described time zone, a named one, UTC and all-day; the organiser and
which attendee is us; the master of a repeating event; a cancellation; a
reply from someone else; the iTIP REPLY PsiMail would send (and that it
refuses headers that aren't a proper reply); vCard 2.1 quoted-printable and
charsets, Apple's 3.0 item groups and photos, 4.0 `tel:` URIs.

`invitehost.py` runs the host engine against `fakeimap.py --invite`: the
invitation and the card are fetched with their messages and summed up, the
Accept reply goes out through `smtp` as multipart/alternative with a
`text/calendar; method=REPLY` part, and a store moved from `PsiMail/` to
`System/Data/PsiMail/` (as PsiMail.app moves it) is used from there with
nothing fetched again.

The same decoders as ARM code: `psimail-host` and `run_psimail.py` take
`pictures F UID PARTS` (the parts of `<uid>.pic` to fetch and decode, as
the app asks with PM_CMD_PICTURES); with `--count` the runner says how many
instructions a decode took - roughly 46 million for an 800x600 baseline
JPEG shown at half size, 12 million for a 320x120 PNG.
`--armclock` makes `pm_ms()` the Psion's time (instructions at ~15 MIPS),
so the decode time limits act as on the device: a 1600x1200 progressive
JPEG takes about 23 s there (340 million instructions), a 5 MP baseline 6 s
at 1/8. (`make TARGET=epoc emu` builds the runner's image again; 0.75 mended
it, and `pictures` sends PM_CMD_PICTURES, 21, not QUIT.)

## Newsletters, web pictures and the engine staying up (0.75)

`htmltest.py` runs `engine/html.c` over the newsletters in `test/html/`
(made up for these tests: a shop's offer, a news digest, an order receipt
with cid: pictures, a product update, a Gmail reply) whole and in odd-sized
pieces, and fails on two blank lines in a row, a blank line at the start or
end, a line with nothing to show, hidden text (each fixture's hidden text
says HIDDEN), spacer or tracking pictures, lost words or pictures, or a
picture without the width and height its HTML gave; with git at hand it
prints the 0.73 converter's line counts beside the new ones. `--show NAME`
prints one as the reader gets it.

`newspics.py DIR` makes photo-like pictures, and `fakeimap.py PORT --news
DIR` serves the fixtures as messages 201-206 with them: inline cid:
pictures (203, 205: a 1600x1200 progressive JPEG, 23 s on a 5mx), attached
photos (206, one a 5 MP picture too big to fetch unasked), and pictures on
the web at `http://pics.example/` (201, 202). Serve `DIR/web` with `python3
-m http.server -d DIR/web 8388` and map the name with `PM_HOSTMAP`:

    PM_HOST=127.0.0.1 PM_PORT=1343 PM_TLS=0 PM_USER=x PM_PASS=x \
    PM_HOSTMAP=pics.example=127.0.0.1:8388 psimail-host -s /tmp/st \
        sync INBOX , body INBOX 201 , webpics INBOX 201 , body INBOX 205 , pictures INBOX 205 2

Expected: "7 web pictures" (the 1x1 tracking picture and the spacers are
never fetched), "1 picture", and `201_W<hash>.pmi` / `205_2.pmi` in the store.

The engine's heartbeat (engine/pmepoc.cpp): it quits on a silent heartbeat
only once PsiMail.app's process has gone, and time spent on a command does
not count. To see the 0.73 failure in the emulator, starve PsiMail.app for
30 s (a busy thread above it, as a busy foreground program would) and then
open a message: 0.73's engine has quit ("The mail engine is not running",
the reader at "Downloading the message..." for ever); 0.75's carries on.
