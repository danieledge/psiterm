# PsiWeb

A modern web browser for the **Psion Series 5mx**: [NetSurf](https://www.netsurf-browser.org/)
(HTML5 parser, CSS 2.1 and parts of CSS 3, no JavaScript) built with the 1999
EPOC R5 toolchain, running on PsiTerm's networking - the same WiFi modem or
dial-up link, and PsiTerm's TLS 1.3 client for HTTPS.

**Status: 0.57, alpha.** The exact ARM code that goes into `psiweb.exe` has
been run in an ARM emulator (see *Testing*): it renders pages, follows links
and redirects, and fetches over HTTP and HTTPS. `PsiWeb.app`, the EIKON front
end, is checked screen by screen in a 5mx emulator against the EIKON style
guide, as PsiTerm and PsiMail are. Bug reports are welcome via
[GitHub issues](https://github.com/danieledge/psiterm/issues).

[![Download PsiWeb.sis](https://img.shields.io/badge/Download-PsiWeb.sis-2ea44f?logo=github)](https://github.com/danieledge/psiterm/raw/main/dist/PsiWeb.sis)

## Using it

Install `dist/PsiWeb.sis` (to D: if you have a CF card). PsiWeb is on the
Extras bar.

| Key | |
|---|---|
| Ctrl+O (or Ctrl+L) | open an address, or type words to search for |
| Ctrl+B / Ctrl+F | back / forward |
| Esc | stop loading |
| Ctrl+R, Ctrl+H | reload, home page |
| Fn+Up / Fn+Down | page up / down |
| Ctrl+M, Shift+Ctrl+M | zoom in / out (they cycle round the sizes) |
| Ctrl+T | show or hide the toolbar (the page takes its room) |
| Ctrl+I | show pictures on/off |
| Shift+Ctrl+Q | page information (title, address, status, free memory, connection) |
| Ctrl+U | disconnect (hang up and free the serial port) |
| Ctrl+K | preferences: home page and proxy |
| Shift+Ctrl+H | help on PsiWeb |
| Shift+Ctrl+A | about PsiWeb |
| pen | tap links, buttons, form fields and the scroll bars |

The toolbar (Open, Back, Home, Zoom) and the menus (File, View, Go, Tools)
follow the EIKON style guide, as PsiTerm's and PsiMail's do. View > Normal
size goes back to 100%; Go > Top of page and End of page jump to the ends.
Tools > Help on PsiWeb has the help topics.

**Tools > Connection settings**: modem (`ATDT host:port`, as PsiTerm) or the
Psion's own TCP/IP (dial-up), baud rate and flow control - the same settings
as PsiTerm and PsiMail. **Test** (Ctrl+T) tries them before OK: whether the
modem answers and at what baud rate, CTS and DCD, or whether the Psion's
Internet connection is up. The serial port can only be used by one program
at a time, so disconnect PsiTerm or PsiMail first (and the Remote link must
be off).

**If the engine stops**: the browser engine runs as its own program, so a
page too big for the memory stops the engine rather than the Psion. PsiWeb
says so on the page, and Tools > Restart browser engine starts it again.

**Tools > Preferences**: the home page, and a proxy - strongly recommended - point PsiWeb at a
[WebOne](https://github.com/atauenis/webone) proxy on your network (for
example on the Mac: `WebOne/webone`, port 8080). The proxy fetches HTTPS
sites for the Psion (no TLS handshake on a 36 MHz CPU), and keeps one
connection open for the whole session, so the modem dials once instead of
once per page. Without a proxy, `http://` goes direct and `https://` uses
PsiTerm's TLS 1.3 client (X25519 + ChaCha20-Poly1305; certificates are
**not** checked, as in PsiTerm's updater).

### Updating

**Tools > Update PsiWeb...** asks where from (GitHub, GitHub - test builds
(dev), or a local server: the same choice as PsiMail's, kept in
`C:\System\Apps\PsiWeb\Update.ini`) and then works like PsiTerm's updater: it reads
`dist/PsiWeb-version.txt` from GitHub, downloads `dist/PsiWeb.sis` in
64 KB pieces (retrying a piece that breaks off) to D: (or C:), checks the
Ed25519 signature in `dist/PsiWeb.sis.sig` against PsiTerm's release key
built into `psiweb.exe`, then offers to run the installer. With a proxy set
it goes through the proxy (no TLS on the Psion); otherwise it uses TLS 1.3
directly. Esc cancels.

To publish a release: set the version in `web/app/psiweb.cpp` (KVersion),
`web/pkg/psiweb.pkg` and `dist/PsiWeb-version.txt`, build, then
`tools/release/sign.py --product PsiWeb dist/PsiWeb.sis <version>` and commit
`dist/PsiWeb.sis`, `.sig` and `-version.txt`. The signed text starts
"PsiWeb update", so a PsiTerm signature can never pass for a PsiWeb one.

### What to expect

- Memory: NetSurf needs roughly 1 MB to start plus 30-60 times the size of
  a page's HTML (a 50 KB page: about 3 MB; a 300 KB page: 15 MB, too much
  for a 16 MB Psion). Light sites work best: 68k.news, FrogFind (which also
  simplifies other pages), DuckDuckGo Lite, text.npr.org, lite.cnn.com.
- Speed (estimated from instruction counts in the emulator, at ~15 MIPS):
  starting NetSurf about 1 second; a small styled page 30-50 million ARM
  instructions (2-3 seconds, including a TLS 1.3 handshake for HTTPS); a
  48 KB modern page 120 million (about 8 seconds). At 115200 baud the
  modem adds roughly 10 KB/s of transfer time (less with gzip).
- Images: GIF and BMP only (NetSurf's own decoders). The page's text uses
  NetSurf's built-in 8x16 bitmap font in all sizes.
- No JavaScript, so sites that need it show their no-script version or
  nothing.

### Opened by PsiMail

PsiMail opens links and "View as web page" in PsiWeb: it starts PsiWeb with
the address on its command line, or hands it to a PsiWeb that is already
running (a message to the app). Messages are opened from PsiMail's store as
`file:///D:/System/Data/PsiMail/...` (or `C:`).

## How it fits together

```
PsiWeb.app  (web/app: EIKON UI - screen, keys, pen, menus, settings)
    |  global chunk "PsiWebShared" (web/psiweb.h): PsiTerm's PsiShared
    |  (serial/dial-up settings) + events, commands, 640x240 16-grey screen
psiweb.exe  (NetSurf's framebuffer front end + libs, C, gcc 3.0)
    - fb/nsfb_epoc.c   a libnsfb surface: RGB565 -> 16 greys (fb/pwgrey.c)
    - engine/pwepoc.cpp  the chunk: events, commands, screen
    - engine/fetch_psi.c  http/https fetcher: one keep-alive connection,
                          chunked, gzip, cookies, redirects, POST
    - engine/pwnet.c     over ssh/psiglue.cpp (modem or TCP/IP) and
                          ssh/tls13.c (with ssh/db's curve25519, libtomcrypt)
    - compat/          what estlib lacks: snprintf, iconv, stdio->log, ...
```

NetSurf is used unchanged except for small patches (`patches/`): the EPOC
settings in `utils/config.h`, the fetcher hook, and a `PSIWEB` section in
the framebuffer front end (menu commands, status/title/URL for the app,
resources built into the program). NetSurf's own log goes to
`psiweb.log` next to the app.

## Building

Linux, wine and the EPOC R5 C++ SDK with the gcc 3.0 toolchain, as for
PsiTerm, plus what NetSurf's build needs on the PC: git, perl, flex, bison,
gperf, pkg-config, zlib and libpng development files.

```sh
PSION_SDK=/path/to/psion_cpp_sdk_linux web/build.sh        # dist/PsiWeb.sis
PSION_SDK=/path/to/psion_cpp_sdk_linux web/build.sh host   # + a PC test build
```

The first run fetches NetSurf and its libraries at fixed commits
(`netsurf.sh`), applies the patches and builds them once for the PC: that
runs NetSurf's code generators and logs its compile commands, which
`gen.py` turns into make rules for the Psion compiler.

## Testing

`build/web-host/psiweb-host` is the same browser for a PC: it renders into
a 640x240 16-grey screen and is driven by a script (`fb/pwhost.c`):

```sh
printf 'open http://example.com/\nidle\nshot page.pgm\nquit\n' > t.txt
PW_SCRIPT=t.txt build/web-host/psiweb-host
```

`emu/run_psiweb.py` runs the **Psion-compiled** ARM code in an emulator
(unicorn), with Python standing in for EPOC and the app, and real network
connections from the PC - the way PsiTerm's `ssh/emu` tested Dropbear:

```sh
make -C web TARGET=epoc emu PSION_SDK=...
python3 web/emu/run_psiweb.py --count t.txt      # (shot NAME.png)
```

`--count` estimates the time on a 5mx, `--proxy host:port` uses a proxy,
`--sample` shows where the time goes, `--trace f1,f2` logs calls.

## Licence

NetSurf and its libraries are GPL v2 (NetSurf) and MIT (the libraries), so
`psiweb.exe` as a whole is **GPL v2**; `PsiWeb.sis` carries the licence
text. PsiWeb's own code and the PsiTerm parts are MIT.
