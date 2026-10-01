# PsiMail

An email client for the Psion Series 5mx, made for Fastmail (any IMAP/SMTP
server with TLS should do), with a calendar that keeps the Psion's own
Agenda in step with Fastmail's (any CalDAV server). It uses PsiTerm's networking - a WiFi modem such
as the WiRSa (`ATDT host:port`) or the Psion's own dial-up TCP/IP - and
PsiTerm's TLS 1.3 client, here with the server's certificate checked.

**Status: 0.2, untested on a real Psion.** The engine has been run against
Dovecot, Radicale and Fastmail both as PC code and as the exact ARM code in
an emulator; the EIKON app has been driven in an emulator.

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
* Folders with unread counts (bold while there is unread mail, as are the
  unread messages themselves); the newest 50 messages per folder (more with
  File > Folder > Get older messages), kept on the CF card to read offline.
* File > Folder > Create new / Rename / Delete (IMAP CREATE,
  RENAME and DELETE, with SUBSCRIBE; names go as modified UTF-7, the local
  files follow a rename). The Inbox and the standard folders stay as they
  are. These need the server, so offline they ask to go online.
* Tools > Help on PsiMail (Shift+Ctrl+H): the help topics in a dialog (see
  `app/pmhelp.cpp` for why not a .hlp file).
* Message text downloaded when opened, up to 64 KB (the rest on request).
  HTML mail is shown as rich text - headings, bold and italic, lists,
  quotes, links you can move to with Tab and open with Enter - and
  Message > View as web page opens the original in PsiWeb
  (NetSurf), as do links. Plain text gets its quotes and links shown the
  same way.
* Attachments listed, saved to `D:\Documents\Attachments\` when asked for
  (your files, so in your Documents).
* Invitations (`text/calendar` or an `.ics` file, iTIP `METHOD:REQUEST`):
  the reader shows what, when (on the Psion's clock, whatever time zone the
  invitation was written in), where and who from at the top, with Accept,
  Tentative and Decline as links (Tab, Enter or a tap; also Edit >
  Invitation). Accept and Tentative put the event in the Agenda (and so on
  the CalDAV server, with calendar sync on); each answer goes to the
  organiser as an iTIP `METHOD:REPLY` through the outbox. A cancellation
  (`METHOD:CANCEL`) offers Remove from Agenda.
* Contact cards (`text/vcard` / `.vcf`, vCard 2.1, 3.0 and 4.0): Add to
  Contacts puts the name, company, job title, phone numbers, email
  addresses, address and web page into the Contacts program's own fields
  (as its template labels them). Attach > Add my contact card sends yours.
* File > Save as Word file (Shift+Ctrl+S): the open message as a Psion Word
  file (header, text, bold/italic/underline, headings, lists), through the
  Word engine itself (CWordModel), so the built-in Word opens it.
* Pictures in messages, JPEG (baseline and progressive), PNG and GIF,
  decoded on the Psion to 16 greys.
* New message, reply, reply to all, forward; files from the Psion attached
  (up to 8). Sent as UTF-8; a copy saved to Sent.
* Delete (to Trash), archive, move to a folder, read/unread, flag.
* Search a folder on the server.
* Work offline: changes and new messages are kept and sent next time.
* Up to 4 accounts.
* The Email icon below the screen can open PsiMail instead of the built-in
  Email program: PsiMail asks once after it is installed, and Tools >
  Preferences > Email icon opens (PsiMail / Built-in Email) changes it.
  Built-in Email, or removing PsiMail, gives the icon back to the built-in
  program (see `button/` below).
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

Tools > Calendar settings: turn "Sync with Agenda" on. The server is
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
  ("New entries go to", once the first sync has found your calendars).
  Entries already on the Psion stay there unless you choose
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
| Shift+Ctrl+C / Ctrl+U / Esc | check mail (send & receive) / disconnect / stop |
| Ctrl+N / Ctrl+R / Shift+Ctrl+R / Ctrl+W | new / reply / reply to all / forward |
| Ctrl+D / Ctrl+X / Shift+Ctrl+E | delete / move to folder / archive |
| Shift+Ctrl+U / Shift+Ctrl+F | unread / flagged |
| Ctrl+S | save an attachment (Message > Attachments > Open / Save) |
| Shift+Ctrl+S | save the open message as a Word file (File > Save as Word file) |
| Ctrl+I / Ctrl+G / Ctrl+B / Ctrl+F | inbox / go to folder / outbox / find |
| Ctrl+Y / Shift+Ctrl+G | check this folder / get older messages (File > Folder, with Create new / Rename / Delete) |
| Ctrl+M / Shift+Ctrl+M | zoom in / out (three sizes, going round) |
| Ctrl+T / Shift+Ctrl+T / Shift+Ctrl+L | show the toolbar / title bar / folder list |
| Shift+Ctrl+Q / Shift+Ctrl+B | status information / sort |
| Ctrl+K / Shift+Ctrl+W | preferences / work offline |
| Shift+Ctrl+H / Shift+Ctrl+A / Ctrl+E | help / about PsiMail / close |
| Tab / Shift+Tab in a message | next / previous link or attachment (Enter opens it) |
| Shift+Ctrl+N / Ctrl+Q | the calendar / switch its view (week or month; View > Switch view lists them) |
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
  Agenda wants wall-clock times). Invitations and contact cards in
  messages are `invite.c` (parsing, the iTIP reply) and `invmsg.c`
  (fetching the parts when a message is read). Pictures are `pictures.c`
  over `img/` (picojpeg for baseline JPEG, `pmjprog.c` for progressive).
* `PsiMail.app` (`app/`, EIKON C++): keys, menus, dialogs, and the Agenda
  side of the calendar (`pmcal.cpp`). It reads the store's text files
  directly and sends the engine commands.
* `ui/` (C++ without EIKON, so it also builds on a PC): `pmcalmodel.cpp`
  (the calendar's events by day, from the engine's files) and `pmquote.cpp`
  (a message's plain text, for quoting in a reply). PsiMail's earlier
  hand-drawn screens and their built-in fonts are gone; everything is
  drawn with EIKON's own controls and fonts.

* `button/`: the Email icon. The EIKON server turns a tap on the icons
  below the screen into keys (the Email icon is `EEikAppbarApp5Key`), which
  the System screen captures to start the built-in programs; the newest
  capture of a key wins. `pmbutton.exe` captures that one key (again
  whenever a window group comes or goes, so it stays the newest) and brings
  PsiMail to the front or opens it; it has no window and sleeps on the
  window server. It runs only while `C:\System\Apps\PsiMail\Button.ini`
  exists, and stops when that file goes (Preferences, or the uninstaller's
  FN line), when PsiMail.app is removed or replaced, or when PsiMail tells
  it to; with it gone the System screen's capture is the one that counts
  again. PsiMail starts it each time PsiMail opens with the preference on,
  so after a reset the icon is PsiMail's once PsiMail has been opened.
  `button/pmbtnrec.cpp` is a boot hook (a recogniser that recognises
  nothing and starts `pmbutton.exe` 30 s after boot); it is built but NOT
  installed: in the emulator a program started while the Psion is still
  starting stopped the System screen from finishing, and a recogniser on
  C: that broke booting could only be cleared by a cold reset.

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

See the top of `engine/store.c`. In short `D:\System\Data\PsiMail\A0\`
holds the first account: `folders.txt`, a folder per mail folder with
`index.txt` (one line per message), `<uid>.txt` for each downloaded message
(and `<uid>.htm`, the HTML original; `<uid>.inv` / `.vcd`, an invitation or
contact card summed up - see `engine/invite.h`), and `outbox\`. The
calendar's files are in `D:\System\Data\PsiMail\cal\` (see
`engine/caldav.c` and `app/pmcal.h`). Without a card (or with
Tools > Preferences > Keep mail on set to the internal disk) it is the same
on C:.

The store is the program's working data, so it is under `\System\` as the
style guide asks (10.2.1), in `\System\Data\` - EPOC's folder for programs'
data - rather than `\System\Mail\`, which belongs to the Message Server (it
rebuilds its index there, and can move the whole folder to another disk when
the built-in Email's messages are moved). Before 0.74 it was `\PsiMail\` at the
top of the disk: the first start of a newer PsiMail renames that folder into
place (one rename on the same disk; saved attachments go on to
`\Documents\Attachments\`). If the rename fails PsiMail keeps using
`\PsiMail\`, says so, and writes why to `Store.log` beside PsiMail.app
(`app/pmstore.cpp`). View > Status information shows where the mail is.

Removing PsiMail removes the program and its settings but leaves the mail:
it is the user's.

## Not yet

* Forwarding attachments (save them and attach them instead).
* Updating PsiMail from GitHub as PsiWeb does.
* IMAP IDLE (push); checking mail on a timer.
* Server-side drafts (drafts stay in the outbox).
