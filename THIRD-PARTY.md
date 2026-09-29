# Third-party components

| Component | Where | Licence |
|---|---|---|
| [Dropbear SSH](https://github.com/mkj/dropbear) (client), incl. LibTomCrypt and LibTomMath | `ssh/db/` | MIT-style / public domain - see `ssh/db/LICENSE` |
| [libvterm](https://www.leonerd.org.uk/code/libvterm/) (upstream `934bc2fb`, Psion changes in `docs/patches/libvterm-psion.diff`) | `libvterm/` | MIT - see `libvterm/LICENSE` |
| [zlib](https://zlib.net/) | `ssh/zlib/` | zlib licence - see `ssh/zlib/LICENSE` |
| [NetSurf](https://www.netsurf-browser.org/) and its libraries (libcss, libdom, hubbub, libparserutils, libwapcaplet, libnsfb, libnsutils, libnslog, libnsgif, libnsbmp, libnspsl), [utf8proc](https://github.com/JuliaStrings/utf8proc) - PsiWeb only | fetched by `web/netsurf.sh`, Psion changes in `web/patches/` | NetSurf GPL v2; libraries MIT-style - so `psiweb.exe` is GPL v2 |
| [Terminus Font](https://terminus-font.sourceforge.net/) (converted to EPOC format) | `tools/fonts/`, `pkg/psiterm.gdr` | SIL Open Font License 1.1 - see `pkg/terminus-licence.txt` |

Dropbear is vendored from upstream commit `59870ad4` (2026-08-31) with Psion
changes - the diff is in `docs/patches/dropbear-psion.diff` (plus the new file
`ssh/db/src/curve25519_gf32.h`). zlib is upstream commit `767c4c94`.
