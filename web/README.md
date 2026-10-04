# PsiWeb

A web browser for the **Psion Series 5mx**. Its engine is [Links 2](http://links.twibright.com/) (2.30, graphics mode), built with the 1999 EPOC R5 toolchain. It runs on PsiTerm's networking: the same Wi-Fi modem or dial-up link, and the apps' own TLS 1.3 client for HTTPS.

- Pages are plain documents: Links has **no CSS and no JavaScript**. Simple sites read well; big modern home pages come out as long lists.
- Pictures (JPEG, PNG, GIF) are fetched **only when asked**: View > Show pictures (Ctrl+I) for the page showing, or Tools > Preferences > Pictures > On every page.
- Links are underlined, since blue is all but black in 16 greys.
- PsiWeb starts on a built-in welcome page, without dialling.

Up to 0.61 the engine was NetSurf. Its sources are still in the tree as a fallback (see *The NetSurf engine* below), until Links has been proved on the device. The whole port is described in [`links/PORTING.md`](links/PORTING.md).

**Status: alpha.** The exact ARM code that goes into `psiweb.exe` runs in an ARM emulator harness, and the whole package runs in a 5mx emulator (see *Testing*). It has not yet been timed on a real 5mx. Bug reports are welcome via [GitHub issues](https://github.com/danieledge/psiterm/issues).

[![Download PsiWeb.sis](https://img.shields.io/badge/Download-PsiWeb.sis-2ea44f?logo=github)](https://github.com/danieledge/psiterm/raw/main/dist/PsiWeb.sis)

## Using it

Install `dist/PsiWeb.sis` (to D: if you have a CF card). PsiWeb is on the Extras bar.

| Key | |
|---|---|
| Ctrl+O (or Ctrl+L) | open an address, or type words to search for |
| Ctrl+B / Ctrl+F | back / forward |
| Esc | stop loading (or saving a file) |
| Ctrl+R, Ctrl+H | reload, home page |
| Up / Down, Fn+Up / Fn+Down | scroll a line / a screen |
| Left / Right | scroll a wide page sideways |
| Ctrl+M, Shift+Ctrl+M | zoom in / out (they cycle round the sizes); the Zoom in and Zoom out icons beside the screen do the same |
| Ctrl+T | show or hide the toolbar (the page takes its room) |
| Ctrl+I | show the pictures of this page, or take them away (as the Pictures button) |
| Shift+Ctrl+Q | page information (title, address, status, free memory, connection) |
| Ctrl+U | disconnect (hang up and free the serial port) |
| Ctrl+K | preferences: home page, pictures, proxy |
| Shift+Ctrl+H | help on PsiWeb |
| Shift+Ctrl+A | about PsiWeb |
| pen | tap links, buttons and form fields; the scroll bar beside the page (EIKON's) scrolls it |

The toolbar (Open, Back, Home, Pictures - pressed in while the page's pictures show) and the menus (File, View, Go, Tools) follow the EIKON style guide, as PsiTerm's and PsiMail's do. Links' own menus, dialogs and bars are never shown: everything goes through PsiWeb.app.

**Passwords.** A page (or the proxy) that asks for a user name and password brings up PsiWeb's *User name and password* dialog. HTTP Basic and Digest (MD5 and SHA-256) are both understood. PsiWeb keeps the answer until it closes.

**Files PsiWeb cannot show** (a PDF, a ZIP, a Word file): PsiWeb says what the file is and how big, and asks "Save it to a file?". Yes opens the standard Save as dialog (Documents on the Memory disk if there is one). The bottom left then shows how much has come, "Saved" says when it is done, and Esc stops it. A file bigger than 4 MB is not saved, and a part file is never left behind.

**Tools > Connection settings**: modem (`ATDT host:port`, as PsiTerm) or the Psion's own TCP/IP (dial-up), baud rate and flow control. These are the same settings as PsiTerm and PsiMail use. **Test** (Ctrl+T) tries them before OK. The serial port can be used by one program at a time, so disconnect PsiTerm or PsiMail first (and the Remote link must be off).

**Tools > Preferences**: the home page, pictures, and a proxy. A [WebOne](https://github.com/atauenis/webone) proxy on your network is recommended over a modem: it keeps one connection open, so the modem dials once. Without a proxy, `http://` goes direct and `https://` uses PsiWeb's TLS 1.3 client (X25519 + ChaCha20-Poly1305, with session resumption). Certificates are **not** checked, as in PsiTerm's updater.

**If the engine stops**: the browser engine runs as its own program, so the Psion carries on. PsiWeb says so on the page, and Tools > Restart browser engine starts it again.

### Updating

**Tools > Update PsiWeb...** asks where from: GitHub, GitHub - test builds (dev), or a local server. It is the same choice as PsiMail's, kept in `C:\System\Apps\PsiWeb\Update.ini`. It then works like PsiTerm's updater:
1. It reads `dist/PsiWeb-version.txt`.
2. It downloads `dist/PsiWeb.sis` in 64 KB pieces to D: (or C:).
3. It checks the Ed25519 signature in `dist/PsiWeb.sis.sig` against PsiTerm's release key, built into `psiweb.exe`.
4. It offers to run the installer.

To publish a release: set the version in `web/app/psiweb.cpp` (KVersion), `web/pkg/psiweb.pkg` and `dist/PsiWeb-version.txt`, build, run `tools/release/sign.py --product PsiWeb dist/PsiWeb.sis <version>`, and commit `dist/PsiWeb.sis`, `.sig` and `-version.txt`.

### What to expect

Figures are ARM instructions counted in the emulator harness, at an assumed 15 MIPS. They are CPU only: the transfer over a modem (about 10 KB/s) comes on top.

| Page | Time | Heap |
|---|---|---|
| start-up | 0.1 s | 0.3 MB |
| info.cern.ch | 0.3 s | 0.3 MB |
| 68k.news | 2.0 s (a screen of scrolling 0.3 s) | 0.8 MB |
| text.npr.org (https) | 1.0 s | 0.4 MB |
| en.m.wikipedia.org (https, a redirect) | 2.1 s | 0.5 MB |
| www.bbc.co.uk, no pictures | 5.3 s | 2.2 MB |
| www.bbc.co.uk with all 117 pictures (3.9 MB) | 32 s | 6.3 MB, at most 8.3 MB scrolling to the end |

- The engine's heap is limited to 10 MB. On a long page, pictures away from the screen are let go and decoded again when they come back. A page that still does not fit stops where it is and says "Page too big".
- `psiweb.exe` is about 1.9 MB. EPOC loads an EXE into RAM, so this comes on top of the heap.

### Opened by PsiMail

PsiMail opens links and "View as web page" in PsiWeb. It starts PsiWeb with the address on its command line, or hands it to a PsiWeb that is already running. Messages are opened from PsiMail's store as `file:///D:/System/Data/PsiMail/...` (or `C:`).

## How it fits together

```
PsiWeb.app  (web/app: EIKON UI - screen, keys, pen, menus, dialogs, settings)
    |  global chunk "PsiWebShared" (web/psiweb.h): PsiTerm's PsiShared
    |  (serial/dial-up settings) + events, commands, questions (password,
    |  save to file), a 640x240 16-grey screen
psiweb.exe  (Links 2.30 in graphics mode, C, gcc 3.0)
    - links/psi_drv.c     the Links graphics driver: RGB565 frame -> 16 greys
                          (links/psi_grey.c), keys, pen, commands, questions,
                          picture memory
    - links/epoc/psi_os.c the select loop and pseudo-sockets over pwnet
    - engine/pwepoc.cpp   the chunk, PsiWeb.log, heartbeats
    - engine/pwnet.c      over ssh/psiglue.cpp (modem or TCP/IP) and
                          ssh/tls13.c (web/tls: X25519, ChaCha20-Poly1305)
```

Links is patched (`links/patches/links-2.30-psion.diff`): the `psi` driver, no Links menus or bars, pictures on request, the speed work and the picture memory. The engine writes `C:\System\Data\PsiWeb.log` (and `PsiWeb.old`, the run before), with timings, link messages, infoprints and the heap used by each page.

## Building

Use the `psion-build` Docker image (see `docs/BUILDING.md`):

```sh
tools/docker/psibuild web/build.sh                          # dist/PsiWeb.sis
tools/docker/psibuild "PSIWEB_SIS=$PWD/build/x.sis web/build.sh"   # elsewhere
tools/docker/psibuild "make -f web/links/epoc.mk -j8 exe"   # only psiweb.exe
tools/docker/psibuild "make -f web/links/epoc.mk -j8 emu"   # the harness image
```

The first build fetches Links 2.30, libjpeg 9f and libpng 1.6.43 at pinned checksums (`links/fetch.sh`) into `build/links` and applies the patch.

## Testing

- **ARM harness** (`links/emu/run_links.py`, `links/emu/run_pages.sh`): runs the Psion-compiled engine in unicorn, with Python standing in for EPOC and the app, and a 10 MB heap. It counts instructions, records and replays network traffic (`NET=record|replay`), profiles, and answers the engine's questions (`auth USER PASS`, `save PATH` script lines).
- **5mx emulator** (`tools/emu/`): the real package on the real ROM. `python3 tools/emu/net.py web` loads a page through an emulated modem.
- **PC build** (`links/build_host.sh`, `links/run_host.sh`): Links with the `psi` driver on Linux, for quick checks.

## The NetSurf engine

`PSIWEB_ENGINE=netsurf web/build.sh` still builds the 0.61 NetSurf engine (`web/Makefile`, `web/engine/fetch_psi.c`, `web/fb`, `web/patches`, `web/emu`). The app works with either engine. NetSurf rendered CSS, but needed 15 to 25 MB for a page like BBC's, against the 10 MB heap. `links/PORTING.md` says how to remove it once Links is proved on the device.

## Licence

Links is GPL v2, as NetSurf is, so `psiweb.exe` as a whole is **GPL v2**; `PsiWeb.sis` carries the licence text. libjpeg and libpng have their own permissive licences. PsiWeb's own code and the PsiTerm parts are MIT.
