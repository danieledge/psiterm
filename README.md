# PsiTerm

An SSH terminal for the **Psion Series 5mx** (EPOC R5, 36 MHz ARM710T).

PsiTerm is a native EPOC terminal app (libvterm-based, xterm-256color) that
wraps a port of the **Dropbear SSH** client. It connects through anything that
behaves like a Hayes modem on the serial port - for example a WiFi RS-232
modem - by dialling `ATDT host:port`, or over the Psion's own dial-up (PPP)
TCP/IP stack.

Modern servers work: curve25519 key exchange, Ed25519/RSA host keys,
chacha20-poly1305, zlib compression. It is fast enough to run tmux, vim, htop -
and Claude Code - on a 1999 palmtop.

## Features

- SSH with saved hosts and (optionally) saved passwords
- Auto-reconnect when a session drops (Psion switched off, WiFi gone), with an
  optional command on login such as `tmux new -A -s psion` to land back where
  you were
- Terminus font in four sizes (plus Courier), optional bold, 16 greys
- Scrollback, pen selection, copy/paste with the system clipboard
- Box drawing, block and Braille graphics drawn natively
- Over-the-air updates and an on-device crypto speed test

## Install

Copy `dist/PsiTerm.sis` to the Psion and open it (install to D: if you have a
CF card). Then see `docs/FIRSTRUN.TXT` for a step-by-step first run.

## Building

You need Linux, wine, and the EPOC R5 C++ SDK with the gcc 3.0 Psion
toolchain (`psion_cpp_sdk_linux`, containing `epoc_cpp_sdk/` and
`gcc-3.0-psion-98r2-9/`).

```sh
PSION_SDK=/path/to/psion_cpp_sdk_linux ./build.sh
```

This builds libvterm (`build_vterm.sh`), `psissh.exe` (`ssh/Makefile`), the
PsiTerm app, and packages `dist/PsiTerm.sis`.

### Layout

| Path | What |
|---|---|
| `app/` | PsiTerm.app - EIKON UI, terminal drawing, settings, dialogs |
| `ssh/` | psissh.exe - Dropbear client plus the EPOC glue (`psiglue.cpp`, `psishim.c`) |
| `ssh/psishared.h` | the shared-memory interface between the two |
| `libvterm/` | terminal emulation |
| `pkg/` | installer definition, font and icon files |
| `server/` | optional local update/debug server (plain HTTP) |
| `tools/` | font and icon converters, screenshot renderer, test harnesses |
| `ssh/test/` | host-side tests (build `ssh/test/Makefile.host`) |

## Updates

**Terminal > Update PsiTerm** downloads new versions over the same link SSH
uses.
Currently it fetches from a small local server (`server/psion-update.sh`).
Fetching straight from this repository over HTTPS is in progress.

## Licence

MIT for PsiTerm's own code - see `LICENSE`. Bundled components keep their own
licences - see `THIRD-PARTY.md`.

(c) Dan Edge
