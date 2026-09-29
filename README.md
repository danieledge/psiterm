# PsiTerm

> **Alpha - pre-v1. Expect bugs!** PsiTerm is new and changing fast. It works
> well enough for daily SSH, tmux and Claude Code on a real 5mx, but things
> will break. Bug reports (a photo of the screen is perfect) are very welcome
> via [GitHub issues](https://github.com/danieledge/psiterm/issues).

[![Download PsiTerm.sis](https://img.shields.io/badge/Download-PsiTerm.sis-2ea44f?logo=github)](https://github.com/danieledge/psiterm/raw/main/dist/PsiTerm.sis)
[![Buy me a coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-support-FFDD00?logo=buymeacoffee&logoColor=000)](https://buymeacoffee.com/danedge)

**Download:** [PsiTerm.sis](https://github.com/danieledge/psiterm/raw/main/dist/PsiTerm.sis)
(always the latest version - see [`dist/version.txt`](dist/version.txt)).
Once installed, PsiTerm updates itself: Terminal > Update PsiTerm.

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

- SSH with saved hosts and (optionally) saved passwords; a start screen to
  connect with one key (1-9)
- SSH keys (Settings > SSH keys): make Ed25519 keys on the Psion or import
  your own (OpenSSH or Dropbear, Ed25519 or RSA), each named, with its
  fingerprint; show, rename, regenerate or delete them. Terminal > Install
  login key on server adds one to a server, and each saved host chooses its
  key, a saved password, or to ask
- Snippets: your own commands and prompts on a menu and on Shift+Ctrl
  hotkeys, with escapes for any key sequence (`\n`, `^C`, `\e`)
- tmux windows shown as tabs: tap one to switch, Ctrl+Tab / Shift+Ctrl+Tab
  for the next / previous (works with tmux's default status line)
- Claude Code keys (interrupt, rewind, switch mode, /clear, /compact...) and a
  tmux menu (windows, splits, panes, zoom, scroll mode, detach)
- Themes (classic, inverted, high contrast, soft), cursor styles, and a status
  line with the connection state and clock
- Auto-reconnect when a session drops (Psion switched off, WiFi gone), with an
  optional per-host command on login such as `tmux new -A -s psion` to land back where
  you were
- Terminus font in four sizes (plus Courier), optional bold, 16 greys
- Stylus as a mouse when a program asks for one (tmux with mouse on, vim,
  htop): tap to click - panes, tmux windows, buttons - and drag up or down to
  scroll. Shift+stylus still selects text
- Scrollback, pen selection, copy/paste with the system clipboard
- Box drawing, block and Braille graphics drawn natively
- Signed over-the-air updates straight from GitHub (TLS 1.3 on a 36 MHz ARM)
- An on-device crypto speed test

## Install

Download [`PsiTerm.sis`](https://github.com/danieledge/psiterm/raw/main/dist/PsiTerm.sis),
copy it to the Psion (e.g. via a CF card or PsiWin) and open it - install to D:
if you have a CF card. Then see `docs/FIRSTRUN.TXT` for a step-by-step first run.

The installer also offers the EPOC "Standard C Library" (ESTLIB.DLL), which
PsiTerm needs and the 5mx ROM lacks - say OK if asked.

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

## Support

If PsiTerm brings your Psion back to life and you'd like to say thanks,
you can [buy me a coffee](https://buymeacoffee.com/danedge).

## Licence

MIT for PsiTerm's own code - see `LICENSE`. Bundled components keep their own
licences - see `THIRD-PARTY.md`.

(c) Dan Edge
