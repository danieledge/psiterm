# CLAUDE.md: PsiTerm for the Psion Series 5mx

This file is the project brief for Claude Code. Read it before changing
anything. User-facing docs are in `README.md`; the layout of the suite, the
build and the release steps are in psion-core's `docs/BUILDING.md`
(`core/docs/BUILDING.md` here).

## What this is

**PsiTerm** is an SSH client and terminal (libvterm, xterm-256color) with
SFTP file transfer and tmux windows as tabs, for the 1999 **Psion Series
5mx**. UID 0x01000A77; the version is `KPsiTermVersion` in `app/psiterm.cpp`
and `pkg/psiterm.pkg`. It is one of a suite of separate repositories:
psimail, psiweb, psicast, psilinux, psiconnect (the Atom modem firmware),
psiproxy, and **psion-core**, the shared layer every program carries as the
git submodule `core/`.

The target hardware is very constrained:
- **CPU:** ARM710T at 36 MHz, with no FPU, no Thumb use and no long multiply.
- **Memory:** about 16 MB RAM.
- **Screen:** 640x240, 16 greys.
- **OS:** EPOC Release 5 (ER5), C++ with the EIKON UI framework.
- **Compiler:** gcc 3.0 (Psion 98r2), from the 2002 SDK.

The quality bar is **first-party Psion engineering**: the app should look and
behave like Psion's own built-in programs.

The apps reach the internet in one of two ways, chosen in the Connection
settings dialog (shared by every program through `C:\System\Data\PsiLink.ini`,
`core/ssh/psilink.h`): the serial port and a Wi-Fi modem (Hayes
`ATDT host:port`, then raw TCP), or the Psion's own "Psion Internet" PPP stack.

## Architecture

Two processes:
1. **PsiTerm.app** (`app/`, an EIKON DLL): draws the screen, handles keys
   and menus, owns the settings.
2. **psissh.exe** (`ssh/`): Dropbear (`ssh/db`) plus libtomcrypt and
   libtommath (pure C: `LTC_NO_ASM`, `LTC_NO_FAST`) and zlib, in C on ESTLIB.
   `ssh/psishim.c` is the POSIX shim and the updater; `ssh/sftp.c` the SFTP
   v3 client, on a second channel of the logged-in session; `ssh/tmuxq.c`
   the tmux query channel.

They talk through the global chunk `PsiTermSSH` (layout in
`core/ssh/psishared.h`): single-writer rings, progress strings, heartbeats
both ways. psissh quits after 45 s with no beat from PsiTerm.

**From core** (`core/`, the psion-core submodule; same relative paths as
before the split): `ssh/psiglue.cpp` (the modem and PPP routes), `psishared.h`,
`psibell.h`, `psilink.h`, `pglinktest.cpp` (Connection settings > Test),
`psigreyui.cpp` (Screen greys), `psihold.cpp` (`psihold.exe`, shipped in every
program's folder), `tls13.c` (updates), `ptlog.cpp`, zlib, LibTomCrypt,
LibTomMath and Dropbear's `curve25519.c` (`core/ssh/db/src`, shared with the
TLS client). `ssh/Makefile` builds all of that by relative paths
(`CORE ?= ../core`) so `__FILE__` carries no machine paths. The app's `.mmp`
finds the shared sources through the SDK link `ptproj/psitermssh`, which
`build.sh` points at `core/ssh`.

**UI side** (`app/`): `psiterm.cpp` (AppUi, terminal view with libvterm in
`app/vterm`, fonts in `glyphs.cpp`, menus, dialogs, toolbar, status line),
`pttabs.cpp` (tmux windows as EIKON tabs), `ptxfer.cpp` (Send file, Get file,
Log to file), `pthelp.cpp` (Help).

Rules:
- Shortcuts are **Shift+Ctrl**: plain Ctrl+letter goes to the SSH host.
- Settings live in `C:\System\Apps\PsiTerm\PsiTerm.ini`, a versioned stream
  to which new fields are appended.
- Signed updates from GitHub: the updater reads `version.txt` from the
  branch (`main/dist/`, or `dev/dist/` for test builds), then fetches
  `PsiTerm.sis.sig` and `PsiTerm.sis` from the release **tag**
  (`.../v<ver>/dist/`), which never changes (`app/psiterm.cpp` `KGitHubPath`,
  `ssh/psishim.c`). So push the tag before, or with, the `version.txt` that
  names it. **Do not change the updater URLs.**
- `dist/` also carries `PsiMail.sis`, `PsiWeb.sis`, their `.sig` and
  `-version.txt` files: installed PsiMails and PsiWebs read them from this
  repository's branch `dist/`. psimail's and psiweb's `release.sh` write them
  into a psiterm checkout beside theirs; commit them here as they come.

## Building and testing

```
git clone --recurse-submodules ...        # or: git submodule update --init
core/tools/docker/psibuild setup          # once: image + SDK beside the checkout
core/tools/docker/psibuild "bash build.sh"        # dist/PsiTerm.sis
```

- The SDK keeps objects for every ptproj app in one folder
  (`$EPOCROOT/epoc32/build/.../ptproj/marmd/rel`). After switching branches,
  delete the `*.o` files there: stale objects have caused panics (USER 19).
- `ssh/test`: `Makefile.host` builds `psissh-host` (the real Dropbear client
  on a PC) for `e2e.py`, `scenarios.py`, `keylogin.py`, `savedpw.py`,
  `sftp_test.py` and `update.py` against a throwaway sshd and user; never
  commit credentials. The link tests moved to `core/ssh/test`.
- Emulator: `core/tools/emu/` (read its README). After `build.sh`:
  `node --experimental-strip-types core/tools/emu/mkcard.ts psiterm`, then
  `core/tools/emu/run.sh start` (PsiTerm's window is up at about 45 s).
  `python3 core/tools/emu/net.py at ssh browse update` need the psiconnect
  checkout beside this one (its `hosttest/hostmodem`).
- A core change: commit and push it in psion-core first, then move the
  submodule pointer here.

## Releasing

1. Bump the version in `KPsiTermVersion` and `pkg/psiterm.pkg`. Versions
   have two digits (0.95 = `0,95,0`), because the EPOC installer only
   compares major.minor.
2. Build. Sign: `core/tools/release/sign.py dist/PsiTerm.sis <ver>`. The key
   comes from `--key`, `$PSITERM_RELEASE_KEY` (hex) or
   `~/.psiterm-signing/release.key`. **Never commit the key.**
3. Update `dist/version.txt`, commit to **dev** and push. **Never push or
   merge to `main`**; the owner does that.
4. **Tag** the release `v<ver>` and push the tag before, or with, the commit
   that changes `version.txt`.
5. The combined `PsiApps.sis` and `CloseAll.sis` are local only: never
   committed.

## UI rules: the EIKON Application Style Guide

Use the `epoc-eikon-ui-guide` skill if it is available. The essentials:
- Menu bar: File, Edit, View, Keys, Snippets, tmux, Tools.
- Menu panes: at most **8 items**, cascades low in the pane, "..." only on
  commands that open a dialog, tick boxes rather than flip-flop names, dim
  rather than hide. In `DynInitMenuPaneL`, **only dim or tick items that
  exist in that pane**, or EIKON panics (EIKON 8).
- Standard shortcuts (Ctrl+E Close, Ctrl+K Preferences, Ctrl+M zoom, Ctrl+T
  toolbar, Shift+Ctrl+A About, Shift+Ctrl+H Help): here they are Shift+Ctrl
  because plain Ctrl goes to the host.
- Dialogs fit 640x240; validation messages like "No ... entered"; queries
  state the fact first, then a question ending in "?".
- Busy messages bottom-left, outcomes as infoprints: no "error", no
  "please", no full stops.
- Toolbar on the right: the title, 4 buttons (24x20 pictures drawn by
  `core/tools/mkicons.py`) and the clock.
- 16-grey conventions: black on white, grey only for secondary text; text
  that does not fit is clipped with "..." (`TextUtils::ClipToFit`).

## Other rules

- Don't change the connection settings (the dialog, its fields, formats or
  defaults) unless the owner asks.
- All network input is untrusted: bounded copies everywhere.
- Never commit keys, passwords, the SDK, the emulator or its ROMs, LAN
  addresses, home paths or local headers (`*_local.h`).
- Writing style: British English, plain short sentences, the EIKON glossary
  terms, no em dashes.
