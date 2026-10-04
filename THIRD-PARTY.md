# Third-party components

| Component | Where | Licence |
|---|---|---|
| [Dropbear SSH](https://github.com/mkj/dropbear) (client), incl. LibTomCrypt and LibTomMath | `ssh/db/` | MIT-style / public domain - see `ssh/db/LICENSE` |
| [TweetNaCl](https://tweetnacl.cr.yp.to/) (X25519 and Ed25519, as modified in Dropbear) | `ssh/db/src/curve25519.c` | public domain |
| [libvterm](https://www.leonerd.org.uk/code/libvterm/) (upstream `934bc2fb`, Psion changes in `docs/patches/libvterm-psion.diff`) | `libvterm/` | MIT - see `libvterm/LICENSE` |
| [zlib](https://zlib.net/) | `ssh/zlib/` (its inflate is also built into `psimail.exe` for PNG pictures) | zlib licence - see `ssh/zlib/LICENSE` |
| [picojpeg](https://github.com/richgel999/picojpeg) (baseline JPEG decoder, Rich Geldreich; with a luma-only mode, 1/2 and 1/4 shrink-on-decode and an 8-bit Huffman look-up added for the Psion, marked `PsiMail:` in the source) | `mail/engine/img/picojpeg.c`, `picojpeg.h` | public domain |
| [NetSurf](https://www.netsurf-browser.org/) and its libraries (libcss, libdom, hubbub, libparserutils, libwapcaplet, libnsfb, libnsutils, libnslog, libnsgif, libnsbmp, libnspsl), [utf8proc](https://github.com/JuliaStrings/utf8proc) - PsiWeb only | fetched by `web/netsurf.sh`, Psion changes in `web/patches/` | NetSurf GPL v2; libraries MIT-style - so `psiweb.exe` is GPL v2 |
| [DejaVu fonts](https://dejavu-fonts.github.io/) (Sans, Sans Bold, Sans Mono), pre-rendered by FreeType into PsiWeb's sharp text - PsiWeb only | rendered at build time by `web/links/mkstrike.py` from the build container's `fonts-dejavu-core`; the glyph bitmaps are in `psiweb.exe` | Bitstream Vera licence (DejaVu changes public domain) - the text is added to PsiWeb's `COPYING.txt` |
| [miniz](https://github.com/richgel999/miniz) 1.15, its inflate (tinfl) only - Atom modem host tests | `firmware/atom-modem/hosttest/tinfl/` (the same inflate the ESP32 ROM carries, so the tests run the Atom's code path) | public domain |
| [Terminus Font](https://terminus-font.sourceforge.net/) (converted to EPOC format) | `tools/fonts/`, `pkg/psiterm.gdr` | SIL Open Font License 1.1 - see `pkg/terminus-licence.txt` |
| EPOC C Standard Library (`stdlib.sis`, ESTLIB.DLL) from the Psion EPOC R5 C++ SDK | embedded in `dist/PsiTerm.sis` (not in this repository) | Psion's redistributable runtime, shipped with apps that use it as the SDK directs |

Dropbear is vendored from upstream commit `59870ad4` (2026-08-31) with Psion
changes - the diff is in `docs/patches/dropbear-psion.diff` (plus the new file
`ssh/db/src/curve25519_gf32.h`). zlib is upstream commit `767c4c94`.

The PNG and GIF decoders, the progressive JPEG decoder and the 16-grey
dithering in `mail/engine/img/` (`pmimg.c`, `pmpng.c`, `pmgif.c`, `pmjpeg.c`,
`pmjprog.c`) are PsiMail's own code (MIT). `pmjprog.c` decodes the scans as
the JPEG standard (ITU T.81, Annex G) describes, and its integer IDCT is the
one in [stb_image](https://github.com/nothings/stb) by Sean Barrett (public
domain / MIT, either at your choice), adapted.

The invitation and contact card readers (`mail/engine/invite.c`, iCalendar
RFC 5545 / iTIP RFC 5546 / vCard 2.1, 3.0 and 4.0) are PsiMail's own code
(MIT).

The TLS 1.3 client used for updates (`ssh/tls13.c`) is PsiTerm's own code
(MIT), built on the LibTomCrypt and TweetNaCl code above.
