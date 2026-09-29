# PsiMail

An email client for the Psion Series 5mx, made for Fastmail (any IMAP/SMTP
server with TLS should do). It uses PsiTerm's networking - a WiFi modem such
as the WiRSa (`ATDT host:port`) or the Psion's own dial-up TCP/IP - and
PsiTerm's TLS 1.3 client, here with the server's certificate checked.

**Status: 0.1, untested on a real Psion.** The engine has been run against
Dovecot and Fastmail both as PC code and as the exact ARM code in an
emulator; the EIKON app has only been compiled.

## What it does

* Folders with unread counts; the newest 50 messages per folder (more with
  Folder > Get older messages), kept on the CF card to read offline.
* Message text downloaded when opened - plain text, or HTML turned into
  text (links numbered at the end) - up to 64 KB, the rest on request.
* Attachments listed, saved to `D:\PsiMail\Attachments\` when asked for.
* New message, reply, reply to all, forward; files from the Psion attached
  (up to 8). Sent as UTF-8; a copy saved to Sent.
* Delete (to Trash), archive, move to a folder, read/unread, flag.
* Search a folder on the server.
* Work offline: changes and new messages are kept and sent next time.
* Up to 4 accounts.

## Setting up Fastmail

1. On fastmail.com: Settings > Privacy & Security > Manage app passwords >
   New app password, with access to IMAP and SMTP.
2. Install `dist/PsiMail.sis`, start PsiMail. The account dialog starts
   with Fastmail's servers filled in (imap.fastmail.com:993 and
   smtp.fastmail.com:465, both TLS): fill in your address and the app
   password.
3. Tools > Connection settings: as for PsiTerm (modem or Psion TCP/IP, baud
   rate, flow control).
4. Ctrl+G sends and receives.

If Fastmail already saves messages sent by SMTP in your Sent folder, set
"Copy to Sent folder" to No in the account settings to avoid two copies (and
save the upload time).

## Keys

| | |
|---|---|
| Up/Down, PgUp/PgDn, Home/End | move / scroll |
| Enter or Right | open |
| Esc or Left | back (Esc stops a download while one is running) |
| Left/Right in a message | previous / next message |
| Del (and Backspace in a list) | delete |
| Ctrl+G | send & receive |
| Ctrl+N / Ctrl+R / Shift+Ctrl+R / Ctrl+W | new / reply / reply to all / forward |
| Ctrl+A / Ctrl+M | archive / move to folder |
| Ctrl+U / Ctrl+T | read-unread / flag |
| Ctrl+S | save an attachment |
| Ctrl+L / Ctrl+B / Ctrl+F | folders / outbox / search |
| Ctrl+Y / Shift+Ctrl+Y | check for new mail / get older messages |
| Ctrl+O / Ctrl+H | work offline / hang up |
| Ctrl+J / Ctrl+K | switch account / account settings |

In the message editor: Ctrl+S sends, Ctrl+D saves to the outbox, Ctrl+A
attaches a file, Ctrl+U removes the last attachment.

## How it works

Like PsiWeb, two programs share a chunk of memory (`psimail.h`):

* `psimail.exe` (`engine/`, C): IMAP (`imap.c`, `imapparse.c`), SMTP
  (`smtp.c`), MIME (`mime.c`, `compose.c`), character sets (`charset.c`:
  the Psion's UI is Windows-1252), HTML to text (`html.c`), the files on the
  card (`store.c`), certificate checks (`certcheck.c`, `pmrsa.c`,
  `roots.h`), on `ssh/psiglue.cpp` and `ssh/tls13.c` (built with
  `TLS_VERIFY`). `pmepoc.cpp` is its EPOC side.
* `PsiMail.app` (`app/`, EIKON C++): screen, keys, menus, dialogs. It reads
  the store's text files directly and sends the engine commands.

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

Passwords are kept scrambled, not encrypted, in `C:\System\Apps\PsiMail\PsiMail.ini`.

## Building

    PSION_SDK=/path/to/psion_cpp_sdk_linux mail/build.sh [host]

builds `psimail.exe`, `PsiMail.app` and `dist/PsiMail.sis` (and with `host`
the PC version of the engine). The SDK is
[psion_cpp_sdk_linux](https://github.com/static-void/psion_cpp_sdk_linux)
(needs wine). Tests: see `test/README.md`.

## Files on the card

See the top of `engine/store.c`. In short `D:\PsiMail\A0\` holds the first
account: `folders.txt`, a folder per mail folder with `index.txt` (one line
per message) and `<uid>.txt` for each downloaded message, and `outbox\`.

## Not yet

* Forwarding attachments (save them and attach them instead).
* Updating PsiMail from GitHub as PsiWeb does.
* IMAP IDLE (push); checking mail on a timer.
* Server-side drafts (drafts stay in the outbox).
