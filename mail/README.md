# PsiMail

An email client for the Psion Series 5mx, made for Fastmail (any IMAP/SMTP
server with TLS should do), with a calendar that keeps the Psion's own
Agenda in step with Fastmail's (any CalDAV server). It uses PsiTerm's networking - a WiFi modem such
as the WiRSa (`ATDT host:port`) or the Psion's own dial-up TCP/IP - and
PsiTerm's TLS 1.3 client, here with the server's certificate checked.

**Status: 0.2, untested on a real Psion.** The engine has been run against
Dovecot, Radicale and Fastmail both as PC code and as the exact ARM code in
an emulator; the screens have been drawn on a PC from real mail
(`ui/uishot.cpp`); the EIKON app itself has only been compiled.

## What it does

* Laid out like the Psion's built-in Email program, from EIKON's own parts:
  a title band (account\\folder, how many messages or what's happening, the
  connection), column headings (? / From / Subject / Date) you tap to sort,
  a folder tree with pictures, status pictures in the message list, a
  read-only rich text reader with a scroll bar, the standard toolbar (New and
  Reply/f'ward pop up a choice, Check mail, Delete - or Close when reading),
  and View > Show toolbar / title bar / list of folders. Menus, shortcuts,
  wording and messages follow Symbian's EIKON Application Style Guide
  (standard shortcuts, busy messages bottom left, infoprints, a second tap
  opens, zoom goes round three sizes like the built-in Email program). The
  menus are laid out as the built-in Email program's: File, Edit, Message
  (Event in the calendar), View, Tools. The pictures are original pixel
  art, made by `tools/mkicons.py` into PsiMail.mbm.
* Folders with unread counts; the newest 50 messages per folder (more with
  File > Get older messages), kept on the CF card to read offline.
* Message text downloaded when opened, up to 64 KB (the rest on request).
  HTML mail is shown as rich text - headings, bold and italic, lists,
  quotes, links you can move to with Tab and open with Enter - and
  Message > View as web page (Ctrl+P) opens the original in PsiWeb
  (NetSurf), as do links. Plain text gets its quotes and links shown the
  same way.
* Attachments listed, saved to `D:\PsiMail\Attachments\` when asked for.
* New message, reply, reply to all, forward; files from the Psion attached
  (up to 8). Sent as UTF-8; a copy saved to Sent.
* Delete (to Trash), archive, move to a folder, read/unread, flag.
* Search a folder on the server.
* Work offline: changes and new messages are kept and sent next time.
* Up to 4 accounts.
* Calendar: two-way sync between Fastmail's calendars and the Psion's
  Agenda (see below).

## Setting up Fastmail

1. On fastmail.com: Settings > Privacy & Security > Manage app passwords >
   New app password, with access to "Mail, Contacts & Calendars" (or
   IMAP/SMTP only if you don't want the calendar).
2. Install `dist/PsiMail.sis`, start PsiMail. The account dialog starts
   with Fastmail's servers filled in (imap.fastmail.com:993 and
   smtp.fastmail.com:465, both TLS): fill in your address and the app
   password.
3. Tools > Connection settings: as for PsiTerm (modem or Psion TCP/IP, baud
   rate, flow control).
4. Shift+Ctrl+C (Check mail) sends and receives.

If Fastmail already saves messages sent by SMTP in your Sent folder, set
"Copy to Sent folder" to No in the account settings to avoid two copies (and
save the upload time).

## The calendar

Tools > Calendar settings: turn "Sync with the Agenda" on. The server is
caldav.fastmail.com (for another server give its address, with a path if
it needs one: `dav.example.com/cal/`). The mail password is used unless
you give another. Pick your time zone (PsiMail guesses from the Psion's
home city) and the Agenda file (normally `C:\Documents\Agenda`).

After that, Check mail (Shift+Ctrl+C) also syncs the calendar, or Shift+Ctrl+Y
does just that. The Agenda can stay open: PsiMail goes through the Agenda
server as PsiWin does.

* Every event on your calendars from 30 days ago to 180 days ahead (both
  can be changed) becomes an Agenda entry: timed events as appointments,
  all-day ones as day entries, with their location and alarm.
* Events that repeat arrive as one entry per time. Change or delete one on
  the Psion and just that one changes on the server.
* Entries you add on the Psion go to the calendar chosen in the settings
  ("New Psion entries go to", once the first sync has found your
  calendars). Entries already on the Psion stay there unless you choose
  "Copy to the server" before the first sync.
* Changes both ways; if an event changed on both sides, the server's
  version wins. Deleting on either side deletes on the other. Events that
  fall out of the window stay in the Agenda.
* The server keeps what the Psion doesn't show (descriptions, guests,
  other alarms): PsiMail changes only the fields it knows.

PsiMail has its own calendar too (Shift+Ctrl+N, View > Go to > Calendar,
or Calendar in the folder tree), drawn beside the folder tree with the
mail list's fonts, rows, zoom and colours: the week's seven days and the
chosen day's events (times, name, place; marks for an alarm, one of a
series, waiting to be sent; a line where "now" is), or the month with each
day's first event. Today has a frame; the chosen day or event is shown
inverted, as EIKON's lists do. Enter (or a second tap) shows an event in
full; Event > Create new event (Ctrl+N) adds one through a standard dialog
(to the Agenda, and from there to the server). It shows what has been
synced, and new Psion entries waiting to be sent.

Not yet: repeating entries made on the Psion (they stay on the Psion), to-do
lists, anniversaries, more than one calendar account.

## Updating from the Psion

Tools > Update PsiMail asks where to look: `github` for published releases
(raw.githubusercontent.com/danieledge/psiterm/main/dist/, over TLS 1.3), or
`host:port` of PsiTerm's local update server (server/psion-update.sh, port
8686). It fetches PsiMail-version.txt; if that is newer, it downloads
PsiMail.sis in 64 KB pieces (each checked, and fetched again if damaged) to
D:\PsiMail-update.sis (C: without a card), checks the Ed25519 signature in
PsiMail.sis.sig against the release key, and offers to install it.

To put a build on the local server:

    tools/release/publish-local.sh PsiMail 0.3.1

(signs with ~/.psiterm-signing/release.key; tools/release/sign.py works
without the `cryptography` package too). The version must go up each time.

## Keys

| | |
|---|---|
| Up/Down, PgUp/PgDn, Home/End | move / scroll |
| Enter or Right | open |
| Esc or Left | back (Esc stops a download while one is running) |
| Left/Right in a message | previous / next message |
| Del (and Backspace in a list) | delete |
| Shift+Ctrl+C / Ctrl+U / Ctrl+Z | check mail (send & receive) / disconnect / stop |
| Ctrl+N / Ctrl+R / Shift+Ctrl+R / Ctrl+W | new / reply / reply to all / forward |
| Ctrl+D / Ctrl+X / Shift+Ctrl+E | delete / move to folder / archive |
| Shift+Ctrl+U / Shift+Ctrl+F | unread / flagged |
| Ctrl+S | save an attachment |
| Ctrl+I / Ctrl+G / Ctrl+B / Ctrl+F | inbox / go to folder / outbox / find |
| Ctrl+Y / Shift+Ctrl+G | check this folder / get older messages |
| Ctrl+M / Shift+Ctrl+M | zoom in / out (three sizes, going round) |
| Ctrl+T / Shift+Ctrl+T / Shift+Ctrl+L | show the toolbar / title bar / folder list |
| Shift+Ctrl+Q / Shift+Ctrl+B | status information / sort |
| Ctrl+K / Shift+Ctrl+W | preferences / work offline |
| Shift+Ctrl+A / Ctrl+E | about PsiMail / close |
| Tab / Shift+Tab in a message | next / previous link or attachment (Enter opens it) |
| Ctrl+P | view the message as a web page (PsiWeb) |
| Shift+Ctrl+N / Ctrl+Q | the calendar / switch its view (week or month) |
| Shift+Ctrl+Y / Shift+Ctrl+D | sync the calendar / go to today |
| In the calendar: Left/Right, PgUp/PgDn, Home | day, week (month), today |
| In the calendar: Ctrl+N / Enter | new event / the event in full |
| Tab or Left in the message list | the folder column |

Writing a message fills the screen, as in the built-in Email program: To,
CC, BCC, Attachments and Subject, then the text, with the buttons on the
right. Ctrl+B, Ctrl+I and Ctrl+U make the text bold, italic or underlined
(it then goes as HTML email, with a plain text copy for mail programs that
want one). Ctrl+S sends, Ctrl+D saves it as a draft (in the outbox), Ctrl+A
adds or removes attachments, Esc closes (asking before it throws anything
away). A new event (Ctrl+N in the calendar) is a standard dialog: the
event, where, the date, all day or start and end, and an alarm.

## How it works

Like PsiWeb, two programs share a chunk of memory (`psimail.h`):

* `psimail.exe` (`engine/`, C): IMAP (`imap.c`, `imapparse.c`), SMTP
  (`smtp.c`), MIME (`mime.c`, `compose.c`), character sets (`charset.c`:
  the Psion's UI is Windows-1252), HTML to text (`html.c`), the files on the
  card (`store.c`), certificate checks (`certcheck.c`, `pmrsa.c`,
  `roots.h`), on `ssh/psiglue.cpp` and `ssh/tls13.c` (built with
  `TLS_VERIFY`). `pmepoc.cpp` is its EPOC side.
  The calendar is `caldav.c` (CalDAV over `http.c`, with `xmlscan.c`),
  `ics.c` (iCalendar) and `caltz.c` (time zones: servers send UTC, the
  Agenda wants wall-clock times).
* `PsiMail.app` (`app/`, EIKON C++): keys, menus, dialogs, and the Agenda
  side of the calendar (`pmcal.cpp`). It reads the store's text files
  directly and sends the engine commands.
* `ui/` (C++ without EIKON, so it also builds on a PC): `pmcalmodel.cpp`
  (the calendar's events by day, from the engine's files), and the drawing
  of PsiMail's earlier screens, which the app no longer shows (the message
  layout in `pmdoc.cpp` still makes the plain text quoted in a reply) -
  `pmgfx.cpp` (anti-aliased text and shapes into a 4-bit bitmap),
  `pmdoc.cpp` (lays out a message), `pmscreens.cpp` (the screens) and
  `pmfonts.cpp`, made by `tools/mkfonts.py` from the fonts in `fonts/`.

Everything the engine downloads is decoded as it arrives and written to a
file, so no message is held whole in memory (peak heap in the emulator:
~70 KB).

### Security

The Psion has no certificate store and PsiTerm's TLS doesn't check
certificates (its downloads are signed instead). PsiMail sends a password,
so it checks: the server's CertificateVerify signature (RSA-PSS) against its
certificate, the chain up to a root built into PsiMail (ISRG Root X1 -
Fastmail's - and nine other common RSA roots, `tools/mkroots.py`), the
name, and the dates (when the Psion's clock looks set). A server that
fails can be trusted by the user, which pins its key (`pins.txt`).
Only RSA certificates can be checked; ECDSA would be too slow on the Psion.

Passwords are kept scrambled, not encrypted, in `C:\System\Apps\PsiMail\PsiMail.ini`
(and `Calendar.ini`).

## Building

    PSION_SDK=/path/to/psion_cpp_sdk_linux mail/build.sh [host]

builds `psimail.exe`, `PsiMail.app` and `dist/PsiMail.sis` (and with `host`
the PC version of the engine). The SDK is
[psion_cpp_sdk_linux](https://github.com/static-void/psion_cpp_sdk_linux)
(needs wine). Tests: see `test/README.md`.

## Files on the card

See the top of `engine/store.c`. In short `D:\PsiMail\A0\` holds the first
account: `folders.txt`, a folder per mail folder with `index.txt` (one line
per message), `<uid>.txt` for each downloaded message (and `<uid>.htm`, the
HTML original), and `outbox\`. The calendar's files are in
`D:\PsiMail\cal\` (see `engine/caldav.c` and `app/pmcal.h`).

## Licences

The fonts built into PsiMail: Inter (SIL Open Font License,
`fonts/Inter-LICENSE.txt`), DejaVu Sans Mono (Bitstream Vera licence,
`fonts/DejaVu-LICENSE.txt`) and Lucide's icons (ISC,
`fonts/lucide-LICENSE.txt`).

## Not yet

* Forwarding attachments (save them and attach them instead).
* Updating PsiMail from GitHub as PsiWeb does.
* IMAP IDLE (push); checking mail on a timer.
* Server-side drafts (drafts stay in the outbox).
