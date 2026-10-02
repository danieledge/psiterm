# Building, layout and releases

Developer notes for PsiTerm and PsiMail. For what the apps do and how
to install them, see the [README](../README.md).

## What you need

Linux, wine, and the EPOC R5 C++ SDK with the gcc 3.0 Psion toolchain:
[psion_cpp_sdk_linux](https://github.com/static-void/psion_cpp_sdk_linux)
(containing `epoc_cpp_sdk/` and `gcc-3.0-psion-98r2-9/`).

## PsiTerm

```sh
PSION_SDK=/path/to/psion_cpp_sdk_linux ./build.sh
```

This builds libvterm (`build_vterm.sh`), `psissh.exe` (`ssh/Makefile`), the
PsiTerm app and its toolbar pictures (`tools/mkicons.py`), and packages
`dist/PsiTerm.sis`.

## PsiMail

```sh
PSION_SDK=/path/to/psion_cpp_sdk_linux mail/build.sh [host]   # dist/PsiMail.sis
```

`host` also builds a PC version of the engine for the tests. PsiMail's tests
are described in [mail/test/README.md](../mail/test/README.md).

## The web browser

PsiWeb, a web browser, is in development on the `dev` branch. It does not
work yet, so it is not released from `main`. Its source is in `web/`; build
and test it from `dev` (see `web/README.md` there).

## The Atom modem firmware (dev only)

The Wi-Fi modem firmware for the M5Stack Atom has not been tested on hardware
yet, so it is only on the `dev` branch (`firmware/atom-modem`).

## Layout

| Path | What |
|---|---|
| `app/` | PsiTerm.app - EIKON UI, terminal drawing, settings, dialogs |
| `ssh/` | psissh.exe - Dropbear client plus the EPOC glue (`psiglue.cpp`, `psishim.c`); `psiglue.cpp` is the connection layer all three apps share |
| `ssh/psishared.h` | the shared-memory interface between the two |
| `ssh/sftp.c` | the SFTP v3 client for file transfer, on a second channel of the SSH session (`app/ptxfer.cpp` is its UI and the session log) |
| `ssh/tls13.c` | the TLS 1.3 client (X25519, ChaCha20-Poly1305) used for updates, PsiMail and PsiWeb |
| `ssh/pglinktest.cpp` | Connection settings > Test, shared by the three apps |
| `app/pttabs.cpp` | tmux windows drawn as EIKON page tabs |
| `libvterm/` | terminal emulation |
| `mail/` | PsiMail - IMAP/SMTP over TLS and CalDAV: see [mail/README.md](../mail/README.md) |
| `web/` | PsiWeb, the web browser in development (released from `dev` only) |
| `pkg/` | installer definition, font and icon files |
| `server/` | optional local update/debug server (plain HTTP) |
| `tools/` | font and icon converters, screenshot renderer, release signing, test harnesses |
| `ssh/test/` | host-side tests (build `ssh/test/Makefile.host`; `update-test.key` is a throwaway key only the host test build trusts; `sftp_test.py` tests file transfer against a local OpenSSH server; `linktest.cpp` the Test button) |
| `docs/` | these notes, the robustness review and best practices, patches against upstream, screenshots |

## Updates and signing

**Tools > Update PsiTerm** asks where to look (GitHub, its test builds, or a
local server), then fetches `dist/version.txt` and `dist/PsiTerm.sis`
from this repository over HTTPS. The Psion speaks a minimal TLS 1.3 client
(`ssh/tls13.c`: X25519, ChaCha20-Poly1305) over the same modem or dial-up link
SSH uses. PsiMail does the same with `dist/PsiMail-version.txt`, downloading in 64 KB pieces that are each checked.

Every release is signed. `dist/PsiTerm.sis.sig` holds an Ed25519 signature
over the version and the SHA-256 of the .sis, and PsiTerm checks it against the
public key built into `ssh/psishim.c` before installing anything. The Psion
does not check TLS certificates for updates (no CA store, too slow), so the
signature is what makes updates trustworthy, from GitHub or anywhere else.
PsiMail's signed text names the product ("PsiMail update"), so
one app's signature can never pass for another's.

To make a PsiTerm release: build, run
`tools/release/sign.py dist/PsiTerm.sis <version>` with the release key
(kept outside the repository), set `dist/version.txt`, commit, and **tag the
commit `v<version>`** and push the tag. PsiTerm reads `version.txt` from
`main`, then fetches the signature and the .sis from the tag
(`.../v0.31/dist/`), so GitHub's 5-minute cache can never mix old and new
files. For PsiMail use `tools/release/sign.py --product PsiMail` and set the version in the app's source, its `.pkg` and its
`dist/*-version.txt`.

The dialog's Local server choice points the apps at your own server instead
(`server/psion-update.sh`, plain HTTP, port 8686;
`tools/release/publish-local.sh PsiMail 0.3.1` puts a build there). The same
server receives PsiTerm's Debug screenshots, a developer feature.
