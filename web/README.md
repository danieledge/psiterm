# PsiWeb

A modern web browser for the **Psion Series 5mx**: [NetSurf](https://www.netsurf-browser.org/)
(HTML5 parser, CSS 2.1 and parts of CSS 3, no JavaScript) built with the 1999
EPOC R5 toolchain, running on PsiTerm's networking - the same WiFi modem or
dial-up link, and PsiTerm's TLS 1.3 client for HTTPS.

**Status: 0.1, not yet run on a real Psion.** Everything builds, and the
exact ARM code that goes into `psiweb.exe` has been run in an ARM emulator
(see *Testing*): it renders pages, follows links and redirects, and fetches
over HTTP and HTTPS. `PsiWeb.app` (the EPOC front end) compiles but has
only been checked by reading it.

## Using it

Install `dist/PsiWeb.sis` (to D: if you have a CF card). PsiWeb is on the
Extras bar.

| Key | |
|---|---|
| Ctrl+L (or Ctrl+O) | open an address, or type words to search |
| Ctrl+B / Ctrl+F | back / forward |
| Esc | stop loading |
| Ctrl+R, Ctrl+H | reload, home page |
| Fn+Up / Fn+Down | page up / down |
| Ctrl+M, Shift+Ctrl+M | zoom in / out |
| Ctrl+I | images on/off |
| Ctrl+G | page info (address, title, free memory) |
| pen | tap links, buttons, form fields and the scroll bars |

**Tools > Connection settings**: modem (`ATDT host:port`, as PsiTerm) or the
Psion's own TCP/IP (dial-up), baud rate and flow control. The serial port
can only be used by one program at a time, so disconnect PsiTerm first.

**Tools > Proxy and home page**: strongly recommended - point PsiWeb at a
[WebOne](https://github.com/atauenis/webone) proxy on your network (for
example on the Mac: `WebOne/webone`, port 8080). The proxy fetches HTTPS
sites for the Psion (no TLS handshake on a 36 MHz CPU), and keeps one
connection open for the whole session, so the modem dials once instead of
once per page. Without a proxy, `http://` goes direct and `https://` uses
PsiTerm's TLS 1.3 client (X25519 + ChaCha20-Poly1305; certificates are
**not** checked, as in PsiTerm's updater).

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
