# Links 2 for PsiWeb: phase 1 (PC proof)

Written 2 October 2026. This phase asks whether the graphics mode of [Links 2.30](http://links.twibright.com/) could replace NetSurf as PsiWeb's engine on the Series 5mx, which has 16 MB of RAM and gives the engine a 10 MB heap.

## Verdict

On a PC, Links renders real pages into a 640×240 frame that is shown in the Psion's 16 greys, using the existing PsiWeb backend interface (`pwback.h`). It draws text, tables, links and forms, and JPEG, PNG and GIF pictures. Its memory use is several times lower than NetSurf's:

| Page | NetSurf (plan doc, emu harness) | Links (this proof) |
|---|---|---|
| bbc.co.uk, pictures off or not decoded | 25.9 MB | **3.4 MB** |
| bbc.co.uk, JPEG pictures on (first screen) | n/a (no JPEG decoder) | **9.0 MB** with our patches (134 MB without them) |
| 68k.news | 3.4 MB | **1.6 MB** |

The harness figures for NetSurf overstate by an unknown factor; the planning doc guesses its real BBC peak is 15 to 20 MB. Even allowing for that, Links fits the 10 MB budget where NetSurf cannot.

The cost is that Links has **no CSS**. Modern sites come out as plain documents: BBC and Wikipedia start with their navigation lists, and wide pictures make the page wider than the screen.

Pictures are the big memory risk. Two small patches (described below) bring them under control on load. Scrolling all the way down the BBC home page still reaches 17.7 MB, because Links keeps the bitmap of every picture already drawn. Under a 10 MB cap the engine does not crash, but the page degrades to black boxes and missing text. Fixing that is listed in the work items.

## What was built

All new files are in `web/links/`. Nothing in the existing NetSurf build or app was changed: `web/fb/pwhost.c` and `web/fb/pwgrey.c` are reused as they are.

| File | What it is |
|---|---|
| `psi_drv.c` | The Links graphics driver `psi`. See *The driver* below. |
| `psi_mem.c` | PC only. Replaces `malloc` and friends for the whole process and counts live and peak heap. See *How memory was measured*. |
| `patches/links-2.30-psion.diff` | Our changes to Links. See *The patches*. |
| `host/config2.h` | Our additions to the `config.h` that Links' configure writes (Links' `cfg.h` includes it when `HAVE_CONFIG2_H` is set). It turns graphics on with only the `psi` driver, enables PNG and JPEG, sets 32-bit sizes and switches our patches on. |
| `fetch.sh` | Downloads links-2.30, IJG jpeg-9f and libpng-1.6.43 at pinned SHA-256s into `build/links`, unpacks them and applies the patch. zlib is the repo's own `ssh/zlib`. |
| `Makefile`, `build_host.sh` | Build `build/links/host/links-psi` inside the psion-build container. It is **32-bit** (`gcc -m32`), so that struct and pointer sizes match ARM. zlib, libpng and libjpeg are compiled from source as static libraries, as they will be for EPOC. OpenSSL is the container's i386 `libssl.so.3`, used on the PC only. |
| `run_host.sh` | Builds, then runs each test page in a fresh process: open, wait until idle, screenshot, Page Down, screenshot. Writes PNGs and `summary.txt` with the heap peaks. |
| `mktestpage.py` | Writes a local test page with baseline and progressive JPEG, PNG with alpha, GIF, and a 1600×1040 JPEG shown 320 wide. |

### How to run it

```
web/links/run_host.sh                       # standard set -> build/links/shots/
web/links/run_host.sh OUT bbc=https://www.bbc.co.uk/
PSI_HEAP_LIMIT=10485760 web/links/run_host.sh   # refuse allocations above 10 MB
NOBUILD=1 LINKS_OPTS="-html-display-images 0" web/links/run_host.sh
```

- `tools/docker/psibuild web/links/build_host.sh` only builds. The first build takes about 2 minutes, mostly compiling `font_inc.c`.
- `PSI_MEM_BIG=262144` logs every allocation of at least that many bytes.
- Pages run in the container, so a local test server runs there too, on 127.0.0.1:8765.

Two quirks of the script harness:
- After a `key`, scripts need `wait 400` before `idle`. Otherwise `idle` passes before Links has redrawn, because the last draw was long ago.
- The `pen` coordinates for the `nav` test depend on that day's 68k.news headlines.

### Links options used

These are set on the command line in `run_host.sh`; on EPOC they would be built-in defaults.

- `-async-dns 0`: there is no fork or thread for DNS.
- `-html-user-font-size 14 -menu-font-size 12`
- `-html-g-background-color 0xffffff -menu-background-color 0xffffff`: Links' default grey background would dither on 16 greys.
- `-memory-cache-size 262144 -image-cache-size 262144 -font-cache-size 262144 -format-cache-size 1`: small caches, to mimic the device.
- `-dither-images 0 -dither-letters 0`: `pw_grey_convert` already dithers to 16 greys.

## The driver (`psi_drv.c`)

**Drawing**
- Links draws into a 640×240 RGB565 frame in the engine's heap. The driver depth is `2 | 16<<3`, the same as svgalib's 64K mode, so Links' own `get_color_fn(130)` and dithering tables are used.
- `fill_area`, `draw_hline`, `draw_vline`, `draw_bitmap` and `scroll` (a `memmove` within the clip area) are written out simply. `fbcommon.inc` was not used: it is tied to the Linux framebuffer and its mouse-pointer machinery.
- Every operation extends one dirty rectangle. A Links bottom half calls `pwb_present(fb, 640, x0, y0, x1, y1)` once the current event has been handled. Links' `flush` hook is not called reliably, so it is not used.
- Pictures are 2 bytes a pixel (`get_empty_bitmap` allocates `x*y*2`).
- Links' "virtual devices" give a single full-screen device, as for svgalib and fb. `links.h` is patched so that `GRDRV_PSI` enables them.

**Input**
- A Links timer polls `pwb_next_event(..., 0)` every 20 ms. The select loop stays the only place where Links waits.
- NSFB key codes become Links keys:
  - Return, Backspace, Tab, Esc, Delete, the arrows, Home, End, Page Up and Page Down, and F1 to F12 are mapped one to one.
  - `PWB_UNICODE_BASE + ch` becomes the character `ch`.
  - The Shift, Ctrl and Alt key events are tracked as modifiers.
- The pen: `PWB_MOVE` and `NSFB_KEY_MOUSE_1` down/up become `B_LEFT | B_DOWN/B_DRAG/B_UP/B_MOVE`. Mouse 4 and 5 become wheel up and down.
- `PWB_QUIT` ends the select loop cleanly.

**Commands** (`psiweb_cmds.h`)
- OPEN and HOME call `goto_url_utf8`.
- BACK and FORWARD call `go_back(ses, ±1)`. RELOAD calls `reload(ses, -1)`.
- STOP and HANGUP call `abort_all_connections`.
- PAGEUP, PAGEDOWN, TOP and BOTTOM send the matching keys.
- ZOOM sets `ses->ds.font_size` as a percentage of the default and reformats.
- IMAGES sets `ses->ds.display_images` and reformats.
- QUIT ends the select loop.

**State reported to the app**
- The title comes through the driver's `set_title`, with Links' "Links - " prefix removed.
- Every 250 ms the driver reports: the URL (`cur_loc(ses)->url`), the status line (`ses->st`), busy (Links' connection queue is not empty) and back/forward availability.
- Busy is also set as soon as a load is started. Without that, a page that loads within one 250 ms poll never shows as busy, and the harness's `idle` hangs.
- `pwb_ready()` is called once the first session exists. `pwb_home_url()` is opened then, unless it is `about:blank`.

**Tested on the PC:** open, idle, Page Down, a pen tap on a link, Back (`cmd 2`) and Forward (`cmd 3`). See `nav-*.png`.

## The patches (`patches/links-2.30-psion.diff`)

**`drivers.c`, `links.h`, `main.c`**
- Register `psi_driver` first.
- `GRDRV_PSI` turns on virtual devices.
- Graphics mode is forced (`ggr = 1`), so no `-g` is needed and no text terminal is touched.

**`img.c` (`PSI_LAZY_IMAGES`)**
- To lay out the page, Links starts each picture's decoder until it knows the size. From then on the decoder keeps the whole picture decoded (3 to 8 bytes a pixel), plus libjpeg's buffers, until the picture is drawn. Pictures below the first screen are never drawn, so the memory stays held.
- The patch drops the unfinished decoder with Links' own `r3l0ad()` as soon as the size is known. `img_draw_image` restarts it from the cached file when the picture scrolls into view.
- Result on BBC at load: **134 MB → 9.7 MB**.

**`jpeg.c` (`PSI_JPEG_SCALE`)**
- *Decode near the size shown.* libjpeg is told to scale by n/8 while decoding (`scale_num`/`scale_denom`), to the nearest size at or above the one shown.
  - A picture with no size given is capped at 1280 wide (`PSI_JPEG_MAX_W`).
  - This saves memory and most of Links' own scaling work.
- *Buffered-image mode only when needed.* Links put every JPEG in buffered-image mode. That keeps every DCT coefficient of the picture in memory: 3.25 MB for the Y channel of a 1600×1040 photo, before anything is drawn. Now only progressive JPEGs use that mode.
- Result on the local picture page: **11.1 MB → 2.2 MB**.

**`html.c` (`PSIWEB`)**
- No "Link: canonical / alternate / …" lines at the top of the page.
- No `prefetch`, `preload`, `prerender` or `dns-prefetch` fetches. Over a modem these would be pure waste.

## Results

### Memory

`[mem]` lines are written when the page finishes loading. "Peak" is the most heap live at any time during that page.

| Page | Peak | Notes |
|---|---|---|
| (no page: start, `about:blank`, quit) | 1.1 MB | 300 KB of this is the 565 frame |
| http://info.cern.ch/ | 1.2 MB | |
| https://info.cern.ch/ | 2.1 MB | about 0.9 MB is OpenSSL (certificate store, about 16,000 blocks), which tls13.c will not need |
| http://68k.news/ | 1.6 MB | NetSurf: 3.4 MB |
| https://text.npr.org/ | 2.1 MB | includes OpenSSL |
| https://en.m.wikipedia.org/wiki/Psion | 3.3 MB | redirected to the desktop page; includes OpenSSL |
| https://www.bbc.co.uk/, pictures off | 3.4 MB | NetSurf: 25.9 MB (harness) |
| https://www.bbc.co.uk/, pictures on, first screen | 9.0 MB | 117 JPEGs of 480×270. About 4 MB is the compressed pictures and the 964 KB HTML in the file cache, all locked by the page |
| same, without the img.c patch | 134 MB | |
| same, after 30× Page Down | 17.7 MB | bitmaps of every picture drawn so far |
| same, 30× Page Down, `PSI_HEAP_LIMIT=10 MB` | 10.0 MB | no crash: 631 allocations refused; pictures become black boxes and some text is missing |
| first screen, 10 MB limit, before the patches | fatal | "ERROR: out of memory (malloc(8192) returned NULL)" |
| local picture page (JPEG, PNG, GIF, 1600×1040 JPEG) | 2.2 MB | 11.1 MB before the jpeg.c patch |
| 68k.news, tap a story, Back, Forward | 1.7 MB | |

**How memory was measured (`psi_mem.c`)**
- Each live block counts as its `malloc_usable_size` plus 4 bytes, in a 32-bit process. That is within a few bytes per block of RHeap, which has a 4-byte header and 4-byte granularity.
- Every allocation in the process is counted: Links, libpng, libjpeg, zlib, OpenSSL and glibc.

### Sizes (x86-32, -O2)

**The binary**
- `links-psi` is 4.98 MB text, 0.25 MB data and 0.35 MB bss.
- EPOC loads an EXE that lives on C: or D: into RAM. So all of this costs RAM on top of the heap, unless it is trimmed.

**The biggest parts**

| Object | Size | Plan |
|---|---|---|
| `font_inc.o` | 2.69 MB, all PNG glyph data (from 7.4 MB of C source) | see below |
| `language.o` | 273 KB text + 96 KB data | every UI translation: keep English only |
| `https.o` | 161 KB | mostly built-in CA certificates: dropped with OpenSSL |
| `suffix.o` | 137 KB | the public-suffix list, used for cookies: could be trimmed |
| `view.o`, `menu.o`, `html.o`, `charsets.o`, `bfu.o`, `session.o` | 55 to 90 KB each | |
| libpng | 220 KB | includes write support (pulled in by the prebuilt `pnglibconf.h`); a read-only build is about half |
| libjpeg (decoder only) | 131 KB | |
| zlib | 60 KB | includes deflate, again only for libpng's write side |
| `psi_drv.o` + `pwhost.o` + `pwgrey.o` | 19 KB | |
| the rest of Links | about 1.2 MB | |

ARM code from gcc 3.0 will be somewhat larger than x86 code. The data does not change size.

**Fonts.** `font_inc.c` holds four families, each glyph a grey PNG with a 112 to 120 px master height:

| Family | Glyphs | Size | Latin and punctuation only |
|---|---|---|---|
| system | 1 | 4 KB | |
| normal (serif) | 2330 | 2.09 MB | 536 glyphs, 346 KB |
| bold | 327 | 270 KB | 310 glyphs, 261 KB |
| monospaced | 338 | 213 KB | 325 glyphs, 209 KB |

- **Latin-only subset:** about 820 KB.
- **Smaller masters:** re-encoding the subset with 40 to 48 px masters should bring it to roughly 250 to 300 KB. It would also cut the run-time scaling work by about 6 to 9 times.
- **Alternatives:**
  - read glyphs from a file on the card on demand, instead of linking them in;
  - pre-render bitmap fonts at the two or three sizes PsiWeb uses, removing glyph scaling altogether.

FreeType is not needed: Links' built-in fonts are its own rasteriser.

### Rendering quality at 640×240 in 16 greys

Screenshots are in `build/links/shots/`, with copies in this session's scratchpad (`scratchpad/links/`).

**What works well**
- **Text:** anti-aliased grey glyphs come through `pw_grey_convert` crisp. Plain black and white stay undithered.
- **Text size:** 14 px gives about 13 lines of body text.
- **Photographs:** with Links' dithering off and our 4×4 ordered dither, they look good (`bbc-scroll-*.png`, `images-scroll-*.png`).
- **PNG transparency** over a coloured table cell works (`images-1.png`).

**What needs work**
- **Links' own bars.** Links draws its own top bar (← and the URL) and bottom status bar, using about 30 of the 240 lines. PsiWeb.app already has a title and status line, so these should be removed; see work item 8.
- **No CSS.** BBC, Wikipedia and similar pages show "Skip to content" and long navigation lists first. Hidden menus are shown. Pictures in flex grids are stacked one above another.
- **Page width.** The BBC page is wider than the screen (see its horizontal scroll bar), because of picture widths and tables.
- **Simple sites look right:** 68k.news, text.npr.org and info.cern.ch.
- **Links' own scroll bars** are drawn in grey, and pages have a 1-character margin. Both are fine on the Psion.

### CPU (indicative only)

User CPU time for a whole run on an i7-7700 at 3.6 GHz:

| Page | User CPU |
|---|---|
| info.cern.ch | 0.07 s |
| 68k.news | 0.13 s |
| NPR | 0.13 s |
| Wikipedia | 0.16 s |
| BBC, pictures off | 0.16 s |
| BBC, pictures on | 0.39 s |

The ARM710 at 36 MHz is roughly 500 to 1000 times slower for this kind of code, and has no FPU. That puts these pages anywhere from tens of seconds to minutes. It must be measured in the ARM harness (phase 2) before anything is promised.

Known hot spots:
- **Glyph scaling** in `dip.c`, from 120 px masters. It is cached per size, so it is costly the first time a size is used.
- **The scaler** uses `scale_t = unsigned long long` on non-FPU targets, with a 64-bit division per output sample. The ARM710 has no divide instruction at all.
- **Gamma tables** use `pow()` in soft float: 256 entries with `-gamma-correction 0`, but up to 65,536 with the 16-bit setting. Since sRGB and the display gamma are both 2.2, the identity case should skip the tables entirely.
- **PNG decoding through Links** goes via 16-bit-per-channel buffers.

## What matters for EPOC (findings)

**Processes and threads**
- **DNS:** `dns.c` uses fork plus `host`, or a thread, unless `async_dns=0`. With `NO_ASYNC_LOOKUP` it is synchronous (set in `config2.h`).
- **Other fork users:** `os_dep.c` `start_thread` (fork + pipe), `main.c` (detaching), external programs (`exec_on_terminal`, `system`, `popen("uname")`) and `fontconf.c` (pthread, compiled out). None of these is used on the `psi` path once `exec` is NULL and `GD_NO_OS_SHELL` is set. They still need stubbing to link against ESTLIB.

**The select loop**
- `select.c` is built on `select()`/`poll()` over file descriptors.
- Signals arrive through a self-pipe, and `main.c` creates a `terminal_pipe` with `c_pipe()`.
- On EPOC the loop has to wait instead on the network (psiglue) and `pwb_next_event(timeout)`. This needs an `os_psi.c`, as Links already has for DOS, OS/2 and VMS. `select.c` is small and well layered for this.

**Network**
- `connect.c` assumes non-blocking BSD sockets, several at once (`max_connections`), with read and write handlers registered on file descriptors.
- psiglue/pwnet offers **one** connection at a time, with blocking reads and timeouts, and does TLS itself (`pwn_connect(..., tls)`).
- The port therefore needs:
  - a pseudo-socket layer: one pseudo-fd backed by `pwn_*`, read from the loop with a short timeout;
  - `max_connections = 1` and `max_connections_to_host = 1`;
  - https treated as a plain socket whose TLS is done inside `pwn`. This removes `https.c`/OpenSSL entirely.
- Links' own proxy settings (`-http-proxy`) cover WebOne.

**Other system calls**
- **Signals:** SIGINT, SIGTERM, SIGCHLD, SIGWINCH, SIGPIPE, SIGTSTP and SIGCONT are all installed in `os_dep.c`/`select.c`. None is needed on EPOC.
- **Terminal:** termios (`kbd.c`: `tcgetattr`/`tcsetattr`, compiled because `GRDRV_VIRTUAL_DEVICES` brings in the svgalib keyboard code), `save_terminal` and `get_terminal_size` are unused on the `psi` path but are compiled.
- **`mmap`:** only `tiff.c`, which is not built.
- **`setjmp`/`longjmp`:** used by `jpeg.c`, `png.c`, `dip.c`, `select.c` and `terminal.c`. ESTLIB has them.

**Files**
- Links writes `~/.links/links.cfg`, `html.cfg`, `bookmarks.html`, `links.his` and `cookies.txt` (`init_home()` uses `$HOME`, and `getpwuid` as a fallback). On the Psion this should be `C:\System\Apps\PsiWeb\`, or nothing at all except cookies.
- Downloads use `ftruncate` and `fsync`.

**Floating point:** see *CPU* above. dip.c has 80 `float`/`double` uses, img.c 20, and a few elsewhere. Everything compiles with `-msoft-float`, but gamma and glyph work must avoid soft-float in inner loops.

**Memory behaviour**
- Links frees caches on allocation failure (`out_of_memory` → `shrink_memory`). If nothing can be freed, it calls `fatal_exit`.
- With our patches, picture bitmaps and font allocations fail softly, using `mem_alloc_mayfail`. Many other allocations are fatal.
- A device build needs:
  - its own ceiling, at about 8.5 MB of the 10 MB;
  - eviction of bitmaps for pictures that are off the screen;
  - a "Page too big" path, rather than an engine exit.

**Static data and C dialect**
- Links is C89-friendly C, with no C++ and no constructors. It has writable static data, which is fine for an EXE, as for NetSurf.
- `long long` is used, and gcc 3.0 has it.

## EPOC porting work items

Estimates are in working days for one developer who knows this codebase.

1. **EPOC build (3–4 days).**
   - Hand-written `config.h` for ESTLIB and a `compat` header, like `web/compat/nscompat.h`.
   - Build rules for arm-pe-gcc, following `web/Makefile`'s epoc target.
   - libjpeg, libpng with a read-only `pnglibconf.h`, and the repo zlib (inflate only).
   - Stub out what is not used: af_unix, fork/exec, termios keyboard, signals.
2. **`os_psi.c` select loop (4–6 days).**
   - Replace `select()`/signals/`c_pipe` with a wait on `pwb_next_event` and the network pseudo-fd.
   - Timers and bottom halves stay as they are.
   - Engine heartbeats and quit, as `pwepoc.cpp` does for NetSurf.
3. **Network through psiglue (5–8 days).**
   - The pseudo-socket layer over `pwn_*`, with one connection at a time.
   - https through `pwn`'s TLS 1.3, with no OpenSSL; drop `https.c` and the certificates.
   - Proxy (WebOne) settings from the shared chunk, keep-alive, and connection and DNS errors mapped to the status line.
4. **Picture memory (5–8 days).**
   - Evict the bitmaps of off-screen pictures, and re-decode them when they scroll back into view.
   - Store bitmaps as 4-bit grey (a quarter of 565).
   - Fetch pictures lazily, near the view or on request: BBC's 117 pictures are about 4 MB, which is 6 minutes at 11 KB/s.
   - Caps on pixels and bytes per picture.
   - A soft heap ceiling, with "Page too big" in place of `fatal_exit`.
5. **Fonts (2–4 days).**
   - A Latin subset, re-encoded at 40 to 48 px masters (an offline Python step that writes `font_inc.c`), or bitmap fonts pre-rendered at PsiWeb's sizes.
   - Optionally read from a file on the card.
   - Target: under 300 KB.
6. **Trimming (1–2 days).**
   - English-only `language.c`.
   - Drop the certificates, png write and zlib deflate.
   - Optionally a smaller public-suffix list.
   - Goal: an EXE under about 1.5 MB.
7. **CPU (3–5 days, after measurement).**
   - Measure in the ARM emulator harness first.
   - Then: a gamma identity fast path with no `pow` tables, a 32-bit integer scaler, and faster glyph scaling, or none if fonts are pre-rendered.
8. **UI integration with PsiWeb.app (4–6 days).**
   - Remove Links' title and status bars, so the page gets all 240 lines.
   - Route Links' BFU dialogs to the app, or suppress them: errors, authentication, "save file", and the Esc menu. The candidates are `msg_box` and `print_error_dialog`.
   - Form-field text entry from the Psion keyboard.
   - Pen drag scrolling, page information, and the zoom and image toggles. Zoom and images already work through the commands.
9. **Paths and settings (1 day).** Config and cookies under `C:\System\Apps\PsiWeb\`, or no config files at all, with defaults compiled in.
10. **Validation (3–5 days).**
    - An ARM-harness runner like `web/emu/run_psiweb.py` for Links, with the 10 MB arena.
    - The corpus from the modern-web plan, then device tests over a modem and over PPP.

**Total:** about 31 to 49 days. Items 2 and 3 are where most of the risk lies; item 4 decides whether pictures are usable.

## Decisions for Dan

1. **Is "no CSS" acceptable?** Simple sites and news text read well. Modern home pages are long, plain documents, and NetSurf renders them closer to the real thing when it does not run out of memory. It could be offered as a second engine ("Simple") alongside NetSurf rather than as a replacement.
2. **Should pictures be on by default?** Even with lazy fetching, a picture-heavy page costs minutes over a modem.
3. **Links' own UI.** Should its menus and dialogs (the Esc menu, bookmarks, history, download manager) be kept as a fallback, or must everything go through PsiWeb.app's EIKON menus? The style guide suggests the latter.
4. **Licensing.** Links 2.30 is GPL v2, as NetSurf is, so `dist/` packaging and COPYING work the same way. IJG and libpng carry their own permissive licences.

## Dan's decisions (2 October 2026)

1. **Links replaces NetSurf** as PsiWeb's engine, rather than running beside it.
2. **Pictures are off by default.** A "Show pictures" command loads them for the current page.
3. **Everything goes through PsiWeb.app's EIKON menus and dialogs.** Links' own menus, dialogs and bars are not shown.
