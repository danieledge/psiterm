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

## Phase 2: ARM harness results

Written 2 October 2026. Phase 2 asked whether Links is fast enough and small enough on the real hardware. It built Links for the Psion's ARM with the Psion compiler and ran that exact code in the unicorn harness. This also lays the EPOC foundation: networking, the main loop and the build.

### Verdict

**Yes for text pages; pictures need work before they are usable.**

- Text pages cost 1 to 6 seconds of CPU on a 36 MHz 5mx: info.cern.ch 1.0 s, NPR 2.7 s, Wikipedia 4.7 s, 68k.news 5.5 s.
- The BBC home page (1 MB of HTML, pictures off) costs 20 s. That is close to what the modem needs to bring its 118 KB in.
- Scrolling a screen costs 0.2 to 1.2 s.
- Start-up is 2.5 s.
- The heap never went above **3.4 MB**, against the 10 MB limit. No allocation failed on any page.
- The EXE is **about 1.8 MB**. It started at 5.5 MB.

The figures came after the fixes listed below. Before them, 68k.news took 15 s and BBC 57 s.

These figures are **CPU only**. Transfer time over the link comes on top: at about 10 KB/s, 68k.news needs a further 4 s and BBC 12 s.

The timing rests on one assumption: that the ARM710T runs about 1 instruction per 2.4 cycles, which is **15 MIPS** at 36 MHz. That is the harness's conversion (instructions / 15e6). Soft float and memory waits could make the real figure up to about 1.5 times worse. One run on the device would settle it.

### What was built

All new code is under `web/links/`. The NetSurf build, `web/app`, `dist/` and `web/emu/*` are untouched; the files in `web/emu` are used as they are, or copied where a change was needed.

| File | What it is |
|---|---|
| `epoc.mk` | ARM build: arm-pe-gcc (gcc 3.0 Psion 98r2, `-mcpu=arm710 -msoft-float -O2`), ESTLIB headers. It compiles Links, our driver, libjpeg 9f, read-only libpng 1.6.43 and inflate-only zlib (as archives, so only what is used is linked), `tls13.c`, libtomcrypt and `pwnet.c`. The `emu` target links `build/links/epoc/psiweb-emu.pe` with the emu stand-ins. Run it with `tools/docker/psibuild "make -f web/links/epoc.mk -j8 emu"`. |
| `epoc/config.h` | Hand-written Links configuration for ESTLIB (`PSI_EPOC`). Graphics with only the psi driver; PNG, JPEG and zlib; no IPv6, no threads, no async DNS, no signals. |
| `epoc/psicompat.h` | Force-included compat header. It supplies `ssize_t`/`socklen_t`, the errno values ESTLIB lacks and `snprintf`, and routes `read`, `write`, `close`, `fcntl`, `pipe` and `select` to `psi_os.c`. |
| `epoc/psi_os.c` | The EPOC OS layer. See *Networking* and *Main loop* below. Its `main()` calls Links' `main` (renamed `links_main`) with PsiWeb's built-in settings, so no configuration files are needed. |
| `epoc/pnglibconf.h`, `epoc/float.h` | libpng read-only: no write support, no simplified API and fixed-point arithmetic. `float.h` because ESTLIB has none. |
| `psi_grey.c` | `pw_grey_convert` with output identical to `web/fb/pwgrey.c`, but faster. It reads two pixels as a word, takes white and black pairs straight through, and divides by 255 with a multiply. It is used only by the Links build. |
| `mkfont.py` | Rewrites Links' `font_inc.c`: a Latin subset (1,157 of 2,996 glyphs) at 40 px masters. The gamma is applied offline, so the PNGs say gAMA 1.0 and libpng does no gamma work per glyph. Font data drops from 2.58 MB to 0.47 MB. |
| `mklang.py`, `mksuffix.py` | English-only `language.inc` (380 KB to 17 KB). A public-suffix list of names with one or two labels (188 KB to 102 KB). |
| `emu/links_rt.c` | A copy of `web/emu/emu_rt.c` with these changes: |
| | - a **10 MB heap limit**: `malloc` returns NULL above it, and the harness can change the limit; |
| | - the heap counted as RHeap would count it (size + 4, rounded to 4), live and peak, overall and per page; |
| | - word-at-a-time `memcpy`, `memset` and `memmove`, like EUSER's `Mem::Copy`. NetSurf's byte loops overstated their cost several times. |
| | - `pow`, `log`, `exp` and `frexp` written in C and compiled for the ARM with soft float, so their cost counts as ESTLIB's libm would; |
| | - stubs for the POSIX calls Links links against. |
| `emu/setjmp.s` | `setjmp`/`longjmp` for the emulator. ESTLIB has them on the device. |
| `emu/emu_back.c` | A copy of `web/emu/emu_back.c`: no NetSurf start page, and "load pictures" comes from the harness. |
| `emu/run_links.py` | A copy of `web/emu/run_psiweb.py`. It adds: |
| | - per-page figures: instructions from `open` or `cmd` to the screenshot, heap peak, bytes over the link and dials; |
| | - `--images`, `--heap-limit N` and `--json FILE`; |
| | - `--profile`: instructions per function, static functions included (from `arm-pe-nm`); |
| | - `--callers f1,f2`: who calls a function; |
| | - script line `mark NAME`, which starts a new measured section. |
| `emu/run_pages.sh` | Runs the page set below in parallel. It serves `mktestpage.py`'s picture page on 127.0.0.1:8765. Output goes to `build/links/emu-shots/`. |

The emulator build reuses `web/emu/emu_hc.c` as it is.

### Networking (`psi_os.c` + the `connect.c` patch)

**Connecting**
- `make_connection` calls `psi_sock_connect(host, port, tls)` when `PSI_EPOC` is set.
- That calls `pwn_connect`, which dials or connects through psiglue, lets psiglue resolve the name, and does TLS 1.3 when the URL is https.
- It returns a **pseudo-descriptor** (40 and up). Links' DNS, `socket`/`connect`, `https.c` and OpenSSL are not used. `https_func` is simply `http_func`.

**One connection at a time**
- Only one pseudo-socket is live. A new connection makes the old descriptor stale, and reads or writes on it fail, so Links retries.
- Links runs with `max-connections 1`, `max-connections-to-host 1` and `retries 1`.
- Before each request on a kept-alive connection, `pwn_is_open` checks that the server has not closed it.
- Keep-alive works: Wikipedia with pictures made 7 requests on one connection.

**Reading and writing**
- `read` maps to `pwn_read`.
- A socket is readable when psiglue has bytes, TLS has decrypted data pending, or the link has closed.
- `write` maps to `pwn_write`.
- `READ_SIZE` is 16 KB (it was 64 KB).

**Not done yet:** proxy settings from the shared chunk, and connection errors mapped to messages. `pwn`'s reason is put on the status line.

### Main loop

`select.c` is used as it is, with its timers and bottom halves. `loop_select` becomes `psi_select`:
- It returns at once if a pseudo-socket or file is ready.
- Otherwise it waits in `pg_wait(ms, want_net, 0)` until the network has data or Links' next timer is due.

The psi driver's 20 ms timer still polls `pwb_next_event` for keys, pen and commands, as on the PC. Signals are off (`NO_SIGNAL_HANDLERS`). Links' startup pipe is a dummy pseudo-descriptor.

### UI changes (driver and patches)

**Pictures**
- Pictures are **off by default**: `-html-display-images 0`, then `pwb_load_images()` from the app's setting.
- `PW_CMD_IMAGES "1"` (Show pictures) loads them for the current page. It was tested: see `showpics-1` and `showpics-2`.

**Links' own interface is gone (`PSI_NO_BARS`, set with `GRDRV_PSI`)**
- No title bar and no status bar: the page gets all 240 lines.
- No welcome box.
- A tap at the top of the page no longer opens Links' menu.
- Esc and F1 to F12 are no longer passed to Links, because they open its menus.

**Other**
- `about:` home pages, such as NetSurf's `about:welcome`, are not opened.
- No configuration files are read or written (`init_home` under `PSI_EPOC`).
- Title, URL, status and busy reach the app through `pwb_set_*`, as in phase 1.

### CPU fixes made, and what they saved

These were found with `--profile`. The figures are ARM instructions for the page load.

| Fix | Where | Effect |
|---|---|---|
| 8-bit gamma tables (`-gamma-correction 0`). The 16-bit ones were 196,608 soft-float `pow` evaluations at every start. | `psi_os.c` options | start-up 124M → 44M |
| Glyph sharpening in 16.16 fixed point instead of float (about 450 instructions a pixel of soft float before) | `dip.c` | 68k.news 168M → 90M |
| Fonts with 40 px masters and gamma applied offline (decode and scale of each glyph about 7 times less) | `mkfont.py` | 68k.news 229M → 168M (with 48 px) |
| The search for each form control no longer starts at the top of the page when the page has no `<form>`. This was quadratic: BBC's buttons made 585M instructions. | `html.c`, `html_r.c` | BBC 837M → 290M |
| Colour JPEGs decoded to grey (Y only) with the fast integer IDCT. libjpeg 9 had been doing 16×16 IDCTs for the chroma upsampling, then the colour conversion. | `jpeg.c` | pictures page 388M → 250M |
| The picture gamma table (768 `pow`s, about 20M instructions) is kept for the next picture with the same gamma | `dip.c` | pictures page 250M → 124M |
| Faster `pw_grey_convert`; a glyph lookup cache; remembered `ags_8_to_16` colours | `psi_grey.c`, `dip.c` | about 10% on text pages |

The 32-bit scaler was not needed. The 64-bit divides in `scale_t` did not show in any profile, because JPEGs are now scaled by libjpeg itself and fonts use the grey scaler.

### Results per page (after the fixes)

Each page ran in a fresh process with the 10 MB heap limit, using `web/links/emu/run_pages.sh` plus a 30× Page Down run.

- **Instructions**: counted from the `open` (or command) to the screenshot after the page went idle.
- **Time**: instructions at 15 MIPS.
- **Heap**: the RHeap-equivalent peak during that section.
- **Bytes in**: what came over the link, compressed and with TLS overhead.

| Page | ARM instructions | ~5mx CPU time | Heap peak | Bytes in | Dials | Page Down (one screen) |
|---|---|---|---|---|---|---|
| start-up (no page) | 38M | 2.5 s | 1.07 MB (300 KB after) | | | |
| http://info.cern.ch/ | 14.5M | 1.0 s | 386 KB | 878 | 1 | page fits the screen |
| http://68k.news/ | 83.2M | 5.5 s | 791 KB | 40,089 | 1 | 17.3M, 1.2 s |
| https://text.npr.org/ | 39.8M | 2.7 s | 419 KB | 10,097 | 1 | 5.4M, 0.4 s |
| https://en.m.wikipedia.org/wiki/Psion | 71.1M | 4.7 s | 532 KB | 32,436 | 2 (redirect) | 7.5M, 0.5 s |
| https://www.bbc.co.uk/, pictures off | 299.4M | 20.0 s | 2.25 MB | 118,258 | 1 | 5.4M, 0.4 s |
| local picture page, pictures on | 124.1M | 8.3 s | 1.14 MB | 79,813 | 6 | 64.5M, 4.3 s (decodes the 1600×1040 JPEG) |
| the same, pictures off, then Show pictures | 20.5M + 111.7M | 1.4 s + 7.4 s | 419 KB / 1.16 MB | 890 + 78,923 | 1 + 5 | |
| https://www.bbc.co.uk/, pictures on (first screen) | 2,256M | 150 s | 3.36 MB | 1,365,737 | 87 | 5.3M, 0.4 s |
| the same, then 30× Page Down | 103M for 30 screens | 6.9 s | 2.69 MB | 0 | 0 | 0.23 s a screen |

**Notes**
- The TLS 1.3 handshake costs about **13M instructions (0.9 s)**, mostly x25519.
- **BBC with pictures: 87 connections.** `ichef.bbci.co.uk` closes the idle kept-alive connection while the Psion is busy between pictures, so nearly every picture needs a new TLS connection. About half of the 150 s is handshakes. A run without `--count`, which is several times faster, needed only 3 connections. Over the modem each new connection is also a dial.
- No allocation failed on any page. The heap peaks are far below phase 1's PC figures (9.0 MB for BBC with pictures), for three reasons: there is no OpenSSL, colour JPEGs are decoded to grey, and the page uses smaller read buffers.
- **Scrolling memory is still untested.** 30 Page Downs reached only about 5% of the BBC page (the page is very long without CSS). Phase 1's 17.7 MB after scrolling through every picture has not been retested here.

### Screenshots

In `build/links/emu-shots/`, with copies in this session's scratchpad (`scratchpad/links2/shots/`). Each `NAME-1` is the first screen, and each `NAME-2` is the screen after Page Down or Show pictures.

- `cern-1`, `cern-2`
- `68k-1`, `68k-2`
- `npr-1`, `npr-2`
- `wikipedia-1`, `wikipedia-2`
- `bbc-1`, `bbc-2`: pictures off
- `bbcpics-1`, `bbcpics-2`: pictures on
- `images-1`, `images-2`: pictures on
- `showpics-1`: pictures off, showing the alt text
- `showpics-2`: after Show pictures

Text with the 40 px masters looks the same as with Links' 120 px fonts. Grey JPEG decoding gives the same picture as before.

### Sizes (ARM, gcc 3.0 -O2)

The emulator image is **1.68 MB of .text (code and constant data) plus 95 KB of .data**, and about 155 KB of .bss. About 10 KB of that is the emu stand-ins. On the device, ESTLIB is a DLL, and `pwepoc.cpp` plus psiglue add an estimated 50 to 80 KB. **The EXE will be about 1.8 MB.**

| Part | Code | Data (incl. constants) |
|---|---|---|
| Links (all of it, before the items below) | 619 KB | 127 KB |
| fonts (`font_inc.c`, Latin, 40 px) | – | 475 KB (from 2.58 MB of glyph data / 734 KB at 48 px) |
| public suffixes (one or two labels) | 1 KB | 102 KB (was 188 KB) |
| charsets | 7 KB | 84 KB |
| language (English only) | 2 KB | 17 KB (was 380 KB) |
| libjpeg (decoder) | 121 KB | 9 KB |
| libpng (read only) | 90 KB | 10 KB |
| zlib (inflate) | 16 KB | 14 KB |
| TLS (tls13, x25519, libtomcrypt parts) | 25 KB | 4 KB (+67 KB bss) |
| psi driver, OS layer, grey conversion, pwnet | 15 KB | 1 KB |

**Further trims**, if needed:
- Links' own UI modules (`menu`, `bfu`, `listedit`, `bookmark`: about 130 KB of code) once nothing calls them.
- The unused protocols (`ftp`, `smb`, `finger`, `mailto`, `af_unix`, `doh`).
- `charsets` limited to the common ones.

### Remaining risks

1. **The 15 MIPS assumption.** All times scale with it, and the CPI of gcc 3.0 code on the ARM710T is not measured. A run of the EPOC build on the 5mx is the real test.
2. **Not yet an EPOC EXE.** The objects compile with the device flags and ESTLIB headers, but nothing is linked against `estlib.lib` yet. Still to do:
   - `pwepoc.cpp`'s start-up, heartbeats and quit with Links' `main`;
   - `psi_mem_report` from RHeap;
   - a `petran` step with a 10 MB heap;
   - checking ESTLIB's `setjmp`, maths and `strtod` (only the emulator versions have been run).
   - Expected: 2 to 3 days.
3. **The connection model with pictures.** One connection at a time, and servers drop idle keep-alive connections while the Psion decodes. Two possible fixes:
   - fetch all of a page's pictures before decoding any;
   - send everything through the WebOne proxy, which keeps its own connections.
4. **Picture memory on long scrolls** (phase 1 item 4) is still untested on the ARM. Stores of 4-bit grey bitmaps and eviction are still to do.
5. **Start-up gamma tables** still cost 27M instructions (1.8 s). The fix is to bake the 256-entry tables at build time.
6. **Links' keyboard shortcuts.** Letters typed outside a form field still reach them (`g`, `q`, `/`) and open Links dialogs. Phase 1 item 8 still applies.
7. **Links are not distinguished from text.** Blue becomes near-black in 16 greys. Links should be underlined or set to a grey.

### Updated estimate

Phase 2 completed most of phase 1's items 1, 2, 3, 6 and 10, and part of 5 and 7. What remains:

| Item | Days |
|---|---|
| EPOC EXE: link with ESTLIB, `pwepoc.cpp` integration, petran, first run on the 5mx | 3–4 |
| Network: proxy (WebOne) settings, error messages, pictures fetched before decoding or via the proxy | 3–4 |
| Picture memory: 4-bit bitmaps, eviction, caps, "Page too big" | 5–8 |
| UI integration: keys and dialogs, forms, link styling, pen scrolling, page information | 4–6 |
| CPU: baked gamma tables, further trims | 1–2 |
| Device validation over a modem and over PPP | 3–5 |

**Total: about 19 to 29 days**, against the 31 to 49 days estimated after phase 1.
