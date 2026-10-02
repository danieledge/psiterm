# CLAUDE.md: PsiTerm, PsiMail and PsiWeb for the Psion Series 5mx

This file is the project brief for Claude Code. Read it before changing anything. User-facing docs are in `README.md`, `mail/README.md` and `web/README.md`. Build details are in `docs/BUILDING.md`.

## What this is

A suite of native apps that brings a 1999 **Psion Series 5mx** into 2026:

| App | What it does | UID | Version source |
|---|---|---|---|
| **PsiTerm** (`app/`, `ssh/`) | SSH client and terminal (libvterm, xterm-256color), SFTP, tmux tabs | 0x01000A77 | `app/psiterm.cpp` `KPsiTermVersion`, `pkg/psiterm.pkg` |
| **PsiMail** (`mail/`) | IMAP/SMTP email with TLS, CalDAV calendar synced with the Psion's Agenda, Contacts | 0x01000A7C | `mail/app/psimail.cpp` `KVersion`, `mail/pkg/psimail.pkg` |
| **PsiWeb** (`web/`) | Web browser: NetSurf, patched, with our networking and TLS | 0x01000A7A | `web/app/psiweb.cpp` `KVersion`, `web/pkg/psiweb.pkg` |

The target hardware is very constrained:
- **CPU:** ARM710T at 36 MHz, with no FPU, no Thumb use and no long multiply in the code.
- **Memory:** about 16 MB RAM.
- **Screen:** 640x240, 16 greys.
- **OS:** EPOC Release 5 (ER5), C++ with the EIKON UI framework.
- **Compiler:** gcc 3.0 (Psion 98r2), from the 2002 SDK.

The quality bar is **first-party Psion engineering**: the apps should look and behave like Psion's own built-in programs.

The apps reach the internet in one of two ways, and the user chooses which in the Connection settings dialog:
- **The serial port and a Wi-Fi modem** (WiRSa, or `firmware/atom-modem` on an M5Stack Atom), using Hayes `ATDT host:port` and then raw TCP.
- **The Psion's own "Psion Internet" PPP stack** (ESOCK, NetDial, NIFMAN).

## Open issues

- **PsiWeb never shows a web page** (device and emulator). See `docs/issues/psiweb-no-render.md` before working on PsiWeb.

## Architecture

### Every app is two processes
1. **The UI app** (`*.app`, an EIKON DLL). It draws the screen, handles keys and menus, and owns the settings.
2. **The engine** (`psissh.exe`, `psimail.exe` or `psiweb.exe`). It does all the networking, TLS and parsing in C on ESTLIB (the EPOC C library), so the UI never blocks on the network.

The two talk through a **global shared chunk** (an `RChunk` named e.g. `PsiTermSSH` or `PsiMailShared`; layout in `ssh/psishared.h` and `mail/psimail.h`):
- single-writer ring buffers;
- a command queue (`PM_CMD_*` in `mail/psimail.h`);
- progress strings;
- heartbeats in both directions.

Liveness rules:
- PsiMail's engine quits only if the app's process has actually gone. A missed heartbeat on its own is not enough, because the app can be starved of CPU.
- psissh quits after 45 s with no beat from PsiTerm.

### `ssh/psiglue.cpp`: the shared network and serial layer, linked into all three engines
- **Modem route:**
  - `RComm` on `COMM::0`, with the receive buffer set to 16 KB and a zero-length Read to power up the UART.
  - Handshake is `KConfigObeyCTS` (driver controls RTS) or none. Never XON/XOFF.
  - AT dialogue, then `CONNECT`.
  - DCD is probed after CONNECT and `KConfigFailDCD` is armed only if DCD is high; otherwise the code relies on in-band `NO CARRIER`.
  - Hang-up is `+++`, guard time, `ATH`.
- **PPP route:**
  - ESOCK, with the first `RHostResolver::GetByName` starting the dial-up.
  - `RNif::DisableTimers` while online, re-enabled on close.
  - `NetworkActive()` is checked before a redial.
  - The last DNS answer is cached for 10 minutes.
- **Common to both:**
  - Bounded timeouts: lookup 60 s, connect 30 s, CONNECT 25 s.
  - An async, bounded socket shutdown.
  - A switch-on recheck.
  - `ResetAutoSwitchOffTimer` while data flows.
  - `link:` log lines.
- `ssh/pglinktest.cpp`: the **Test** button in each app's Connection settings, compiled into each app.
- `ssh/tls13.c`: our TLS 1.3 client, used by PsiMail and PsiWeb.
- Connection settings are **shared** by all three apps through `C:\System\Data\PsiLink.ini` (`ssh/psilink.h`).

### PsiTerm (`app/`, `ssh/`)

**UI side** (`app/`):
- `app/psiterm.cpp`: the AppUi and terminal view (libvterm in `app/vterm`, fonts in `glyphs.cpp`), menus, dialogs, toolbar and status line.
- `pttabs.cpp`: tmux windows drawn as EIKON-style tabs.
- `ptxfer.cpp`: Send file / Get file / Log to file.
- `pthelp.cpp`: in-app Help.

**Engine** (`ssh/`):
- `psissh.exe` is Dropbear (`ssh/db`) plus libtomcrypt and libtommath (pure C: `LTC_NO_ASM`, `LTC_NO_FAST`), plus zlib.
- `ssh/psishim.c` is the POSIX shim.
- `ssh/sftp.c` is the SFTP v3 client, run on a second channel of the logged-in session.

**Rules:**
- Shortcuts are **Shift+Ctrl**: plain Ctrl+letter goes to the SSH host.
- Settings live in `C:\System\Apps\PsiTerm\PsiTerm.ini`, a versioned stream to which new fields are appended.
- Signed updates come from GitHub through the release **tag** (`…/v<ver>/dist/`).

### PsiMail (`mail/`)

**App** (`mail/app`):
- `psimail.cpp`: AppUi, menus, commands, settings, compose, dialogs.
- `pmnative.cpp`: the main view, built on EIKON controls: folder tree, message list, reader (`CEikRichTextEditor` with a custom scroll bar), calendar pane.
- One file per feature:

| Feature | Files |
|---|---|
| Calendar | `pmcal.cpp`, `pmcalview.cpp`, `pmwrite.cpp` |
| Contacts | `pmcontacts.cpp` |
| Undo | `pmundo.cpp` |
| Print | `pmprint.cpp` |
| Alerts, timed check, auto-send | `pmauto.cpp` |
| Invitations | `pminvite.cpp` |
| vCards | `pmvcard.cpp` |
| Save as Word | `pmsaveword.cpp` |
| Store location and migration | `pmstore.cpp` |
| Pictures | `pmpict.cpp` |
| Web pictures | `pmwebpic.cpp` |
| Quiet or detailed progress | `pmstatus.cpp` |
| Reader header | `pmheader.cpp` |
| Engine restart | `pmrecover.cpp` |
| Help | `pmhelp.cpp` |

**Engine** (`mail/engine`, plain C):
- `pmmain.c`: the command loop.
- `imap.c` / `imapparse.c`: IMAP. `smtp.c`, `compose.c`: sending. `mime.c`, `charset.c`, `html.c`: message decoding (HTML becomes the reader's line format). `store.c`: the local store.
- `caldav.c`, `ics.c`, `caltz.c`, `xmlscan.c`: calendar.
- `pictures.c`, `img/`: decoders (picojpeg with additions, progressive JPEG, PNG via zlib, GIF) producing 16-grey `.pmi` files.
- `webpics.c`, `http.c`: web pictures. `invite.c`: iTIP. `undo.c`: undo. `pmupdate.c`: updates. `certcheck.c`: certificate checks.
- `pmepoc.cpp`: the EPOC side.

**Other directories:**
- `mail/button/`: `pmbutton.exe`, the optional helper that makes the Email silkscreen icon open PsiMail. `pmbtnrec.cpp`, a boot recogniser, is built but deliberately **not installed**: starting a program during boot hung the emulator.
- `mail/ui/pmquote.cpp`: reply quoting.
- `mail/host/`: the host (PC) build of the engine, used by `mail/test`.

**Data:**
- The store is `<disk>:\System\Data\PsiMail\A<n>\F<folder-hash>\`, holding `<uid>.txt`, index files, `.pic`, `.pmi`, `.inv` and `.vcd`.
- An old `\PsiMail\` is moved there once at start.
- Settings are `TPmSettings`, saved as a **fixed-size struct** in `C:\System\Apps\PsiMail\PsiMail.ini`. **Never change its size.** New options use spare fields and bits, marked in `mail/app/pmapp.h`; most are bits in `iView` and `iSpare[]`.
- Logs: `psimail.log` (lines timestamped mm:ss.t) and `psimail.old` (the previous run).

**Updates:** signed. The source is GitHub main, GitHub dev, or a local server (`Update.ini`). PsiMail and PsiWeb read `dist/` on the branch.

### PsiWeb (`web/`)
- `web/netsurf.sh` fetches NetSurf and its libraries at pinned commits into `build/netsurf`, applies `web/patches/*-psion.diff`, and runs a host build for the code generators.
- `web/gen.py` turns that into Psion build rules.
- `web/app` is the EIKON UI. `web/engine` holds `fetch_psi.c` (HTTP and HTTPS over psiglue) and `pwupdate.c`. `web/fb/nsfb_epoc.c` is the framebuffer surface.

### Firmware (`firmware/atom-modem`)
ESP32 (M5Stack Atom + Atomic RS232 Base, with only TX, RX and GND) running Hayes firmware that is compatible with what psiglue expects. Without RTS/CTS it **paces** output to the Psion so the line doesn't overrun.

Host unit tests: `make -C firmware/atom-modem/hosttest test`.

The pins (G19 TX, G22 RX) are unconfirmed on hardware; `AT$SWAP=1` swaps them.

## Building and testing
- **Toolchain:** the EPOC R5 C++ SDK for Linux, static-void/psion_cpp_sdk_linux @ 7763ee8, plus gcc 3.0 Psion 98r2, wine (for bmconv, makesis, makmake and petran), python3 with Pillow, and perl.
- **On a machine without these installed:** use `tools/docker/psibuild setup` once, then `tools/docker/psibuild all`, or `tools/docker/psibuild "<command>"`. The SDK must sit next to this checkout (`../psion_cpp_sdk_linux`).
- **Builds:**
  - `bash build.sh` → `dist/PsiTerm.sis`
  - `mail/build.sh [host]` → `dist/PsiMail.sis`, plus the host engine
  - `web/build.sh` → `dist/PsiWeb.sis`
- **Stale objects:** the SDK keeps objects for every ptproj app in one directory (`$EPOCROOT/epoc32/build/.../ptproj/marmd/rel`). After switching branches or merging, delete the `*.o` files there, because stale objects have caused panics (USER 19).
- **Host tests** (`mail/test`):
  - `foldertest.sh`, `undotest.sh`;
  - `invtest.py`, `invitehost.py`, `htmltest.py`;
  - `imgtest.py` (ASan/UBSan, fuzzing);
  - `parsefuzz.py` (MIME/IMAP/ICS/XML/HTML fuzzer);
  - `fakeimap.py`, a scriptable fake server with drops, slow links, UIDPLUS and invitations.
  
  `ssh/test` holds the SSH and SFTP end-to-end tests, which need a local sshd and a throwaway user.
- **Emulator** (`tools/emu/`, see its README): uses the psionEmulators checkout next to this repo.
  - `tools/emu/seed.py on|off` adds or removes the TESTSEED test account and mail.
  - `tools/emu/mkcard.ts <app> [--seed]` builds a card image.
  - `tools/emu/run.sh NAME "T KEY" "T tap X Y"…` boots and saves screenshots to `build/emu/NAME/`. Look at them with the Read tool.
  - PsiMail's window appears at about 78 s simulated time.
  - **TESTSEED must never be committed or released.**
  - The emulator can't exercise PPP. The modem route can be tested by bridging serial to a fake modem (`--serial-bridge-socket`, `ssh/test/fakemodem.py`, or the firmware's `hosttest/hostmodem`).

## Releasing
1. Bump the version in **both** the `_LIT` and the `.pkg`. Versions have two digits (0.74 = `0,74,0`), because the EPOC installer only compares major.minor.
2. Build with the seed off, and check that `grep TESTSEED mail/app/psimail.cpp` finds nothing.
3. Sign: `tools/release/sign.py [--product PsiMail|PsiWeb] dist/X.sis <ver>`. The key comes from `--key`, from `$PSITERM_RELEASE_KEY` (hex), or from `~/.psiterm-signing/release.key`. **Never commit the key.**
4. Update `dist/version.txt` (PsiTerm), `dist/PsiMail-version.txt` and `dist/PsiWeb-version.txt`.
5. Commit to **dev** and push. **Never push or merge to `main`**; the owner does that.
6. **Tag PsiTerm releases** with `v<ver>`, because its updater reads the tag.
7. The combined `PsiApps.sis` (made with makesis from the three `.sis` files) and `CloseAll.sis` are local only: **never committed**.

## UI rules: the EIKON Application Style Guide
Use the `epoc-eikon-ui-guide` skill if it is available. The essentials:
- **Menu bars:** PsiMail has File, Edit, Message, View, Tools (Event replaces Message in the calendar). PsiTerm has File, Edit, View, Keys, Snippets, tmux, Tools. PsiWeb has File, View, Go, Tools.
- **Menu panes:**
  - at most **8 items** per pane, with cascades low in the pane;
  - "…" only on commands that open a dialog;
  - tick boxes rather than flip-flop names;
  - dim unavailable items rather than hiding them.
  
  In `DynInitMenuPaneL`, **only dim or tick items that exist in that pane**, or EIKON panics (EIKON 8).
- **Shortcuts:** the standard ones are Ctrl+E Close, Ctrl+K Preferences, Ctrl+M / Shift+Ctrl+M zoom (zoom cycles round), Ctrl+T toolbar, Ctrl+Z Undo, Ctrl+P Print, Shift+Ctrl+A About and Shift+Ctrl+H Help. Don't reuse standard letters for other meanings.
- **Dialogs:**
  - fit 640x240;
  - Cancel/OK placement per the guide;
  - validation messages like "No … entered";
  - queries state the fact first, then ask a question ending in "?", with names in double quotes.
- **Messages:**
  - busy messages go **bottom-left** ("Checking mail…"), outcomes are infoprints;
  - no "error", no "please" and no full stops in infoprints;
  - "Not available …" for things that can't be done.
- **Toolbar:** on the right, with the title, 4 buttons (24x20 pictures drawn by `tools/mkicons.py` / `mail/tools/mkicons.py`, dense font) and the clock. It can be hidden.
- **Look:** 16-grey conventions, with black on white and grey only for secondary text. Text that doesn't fit is clipped with "…" (`TextUtils::ClipToFit`).

## Other rules and gotchas
- **Connection settings:** don't change the dialog, its fields, the formats or the defaults unless the owner asks.
- **Untrusted input:** all network input is untrusted. Parsers must survive garbage, so run `parsefuzz.py` and `imgtest.py` after changing them. Use bounded copies everywhere (`SafeCopy`, `Clip`, `snprintf`).
- **Stack:** psimail.exe has a 64 KB stack; big buffers are static.
- **Long work in the engine:** must not starve the UI. Decode at low priority and keep heartbeats going.
- **Card writes:** the emulator's CF card emulation can wedge on many small writes, which is why `pm_write_whole` exists.
- **`.gitignore`:** has broad rules (`*seed*`, `screenshots/`, vendored `.gitignore`s). Use `git add -f` for real source files they hide.
- **References in `docs/`:** `epoc-robustness-best-practices.md` covers SDK-derived practice for memory, leaves, active objects, files and power. The comms best-practice notes cover serial, ESOCK and PPP. The SDK's own docs are HTML under `$EPOCROOT/sysdoc/`, and its headers are in `$EPOCROOT/epoc32/include`.
- **Writing style:** British English, plain short sentences, and the EIKON glossary terms.
