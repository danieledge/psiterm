# Third-party components

| Component | Where | Licence |
|---|---|---|
| [Dropbear SSH](https://github.com/mkj/dropbear) (client), incl. LibTomCrypt and LibTomMath | `ssh/db/` | MIT-style / public domain - see `ssh/db/LICENSE` |
| [TweetNaCl](https://tweetnacl.cr.yp.to/) (X25519 and Ed25519, as modified in Dropbear) | `ssh/db/src/curve25519.c` | public domain |
| [libvterm](https://www.leonerd.org.uk/code/libvterm/) (upstream `934bc2fb`, Psion changes in `docs/patches/libvterm-psion.diff`) | `libvterm/` | MIT - see `libvterm/LICENSE` |
| [zlib](https://zlib.net/) | `ssh/zlib/` | zlib licence - see `ssh/zlib/LICENSE` |
| [Terminus Font](https://terminus-font.sourceforge.net/) (converted to EPOC format) | `tools/fonts/`, `pkg/psiterm.gdr` | SIL Open Font License 1.1 - see `pkg/terminus-licence.txt` |
| EPOC C Standard Library (`stdlib.sis`, ESTLIB.DLL) from the Psion EPOC R5 C++ SDK | embedded in `dist/PsiTerm.sis` (not in this repository) | Psion's redistributable runtime, shipped with apps that use it as the SDK directs |

Dropbear is vendored from upstream commit `59870ad4` (2026-08-31) with Psion
changes - the diff is in `docs/patches/dropbear-psion.diff` (plus the new file
`ssh/db/src/curve25519_gf32.h`). zlib is upstream commit `767c4c94`.

The TLS 1.3 client used for updates (`ssh/tls13.c`) is PsiTerm's own code
(MIT), built on the LibTomCrypt and TweetNaCl code above.
