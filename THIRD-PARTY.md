# Third-party components

| Component | Where | Licence |
|---|---|---|
| [Dropbear SSH](https://github.com/mkj/dropbear) (client) | `ssh/db/`; its LibTomCrypt, LibTomMath, X25519 (`curve25519.c`, from [TweetNaCl](https://tweetnacl.cr.yp.to/)) and `dbrandom.h` are in [psion-core](https://github.com/danieledge/psion-core) (`core/ssh/db/`), shared with the TLS client | MIT-style / public domain - see `ssh/db/LICENSE` |
| [libvterm](https://www.leonerd.org.uk/code/libvterm/) (upstream `934bc2fb`, Psion changes in `docs/patches/libvterm-psion.diff`) | `libvterm/` | MIT - see `libvterm/LICENSE` |
| [zlib](https://zlib.net/) | `core/ssh/zlib/` (psion-core) | zlib licence - see `core/ssh/zlib/LICENSE` |
| [Terminus Font](https://terminus-font.sourceforge.net/) (converted to EPOC format) | `core/tools/fonts/`, `core/pkg/psiterm.gdr` (psion-core); installed with PsiTerm | SIL Open Font License 1.1 - see `core/pkg/terminus-licence.txt` |
| EPOC C Standard Library (`stdlib.sis`, ESTLIB.DLL) from the Psion EPOC R5 C++ SDK | embedded in `dist/PsiTerm.sis` (not in this repository) | Psion's redistributable runtime, shipped with apps that use it as the SDK directs |

Dropbear is vendored from upstream commit `59870ad4` (2026-08-31) with Psion
changes - the diff is in `docs/patches/dropbear-psion.diff` (plus the new file
`curve25519_gf32.h`, now `core/ssh/db/src/curve25519_gf32.h`). Since the
split into separate repositories (October 2026), `curve25519.c`,
`curve25519.h`, `curve25519_gf32.h`, `dbrandom.h`, `libtomcrypt/` and
`libtommath/` live in psion-core, where PsiMail's and PsiWeb's TLS client uses
them too; `ssh/Makefile` builds them from there.

The TLS 1.3 client used for updates (`core/ssh/tls13.c`) is this project's
own code (MIT), built on the LibTomCrypt and TweetNaCl code above. The
connection layer (`core/ssh/psiglue.cpp`), `psishim.c`, `sftp.c`, `tmuxq.c`
and the app are this project's own code (MIT).

`dist/PsiMail.sis` and `dist/PsiWeb.sis` are published here because the
installed apps fetch their updates from this repository; their own
third-party notices are in the [psimail](https://github.com/danieledge/psimail)
and [psiweb](https://github.com/danieledge/psiweb) repositories (PsiWeb's
engine is Links 2, GPL v2, so `PsiWeb.sis` is GPL v2 as a whole and carries
the licence text).
