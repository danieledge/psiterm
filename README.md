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
- Signed over-the-air updates straight from GitHub (TLS 1.3 on a 36 MHz ARM)
- An on-device crypto speed test

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
| `ssh/test/` | host-side tests (build `ssh/test/Makefile.host`; `update-test.key` is a throwaway key only the host test build trusts) |

## Updates

**Terminal > Update PsiTerm** fetches `dist/version.txt` and `dist/PsiTerm.sis`
from this repository over HTTPS. The Psion speaks a minimal TLS 1.3 client
(`ssh/tls13.c`: X25519, ChaCha20-Poly1305) over the same modem or dial-up link
SSH uses.

Every release is signed. `dist/PsiTerm.sis.sig` holds an Ed25519 signature
over the version and the SHA-256 of the .sis, and PsiTerm checks it against the
public key built into `ssh/psishim.c` before installing anything. The Psion
does not check TLS certificates (no CA store, too slow), so the signature is
what makes updates trustworthy, from GitHub or anywhere else.

To make a release: build, run
`tools/release/sign.py dist/PsiTerm.sis <version>` with the release key
(kept outside the repository), set `dist/version.txt`, commit, and **tag the
commit `v<version>`** and push the tag. PsiTerm reads `version.txt` from
`main`, then fetches the signature and the .sis from the tag
(`.../v0.31/dist/`), so GitHub's 5-minute cache can never mix old and new
files.

Settings > Update source can point PsiTerm at a local server instead
(`server/psion-update.sh`, plain HTTP). The same server receives the Debug
screenshots, a developer feature.

## Licence

MIT for PsiTerm's own code - see `LICENSE`. Bundled components keep their own
licences - see `THIRD-PARTY.md`.

(c) Dan Edge
