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
- **BBC with pictures: 87 connections.** `ichef.bbci.co.uk` closes the idle kept-alive connection while the Psion is busy between pictures, so nearly every picture needs a new TLS connection. About half of the 150 s is handshakes. A run without `--count`, which is several times faster, needed only 3 connections. Over the modem each new connection is also a dial. *(Phase 3 found the real cause, a bug in `psi_os.c`: see "The connection bug" there. The server was not closing anything.)*
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

## Phase 3: performance

Written 2 October 2026. Dan asked for the CPU cost to come down "significantly". Everything was profiled in the ARM harness first, the biggest costs were fixed, and each change was measured again. All the changes are in `web/links/`; the NetSurf build, `web/app`, `web/emu` and `dist/` are untouched.

### Verdict

Pages are 1.4 to 4.5 times cheaper (the picture page 4.5 times, cern 3.4, BBC 3, 68k.news 2.6, NPR 1.7, Wikipedia 1.4, where TLS is now half the cost), scrolling 2 to 3 times, start-up 30 times. All the screenshots are **identical to the pixel** with the phase 2 build, at both link speeds tested. No heap peak went up by more than 5 KB, and most went down; start-up's went from 1.07 MB to 313 KB.

| Target | Phase 2 | Phase 3 | |
|---|---|---|---|
| Start-up < 1 s | 2.5 s | **0.08 s** | met |
| 68k.news < 2 s | 5.4 s | **2.1 s** (2.5 s at 10 KB/s) | nearly |
| Wikipedia < 2 s | 4.7 s | **3.5 s** | not met: 1.8 s of it is two TLS key exchanges |
| BBC, pictures off, < 7 s | 20.0 s | **6.8 s** (10.9 s at 10 KB/s) | met on a fast link |
| One screen of scrolling < 0.3 s | 0.4 to 1.0 s | **0.12 to 0.35 s** | met except 68k.news (0.35 s) |
| Pictures decoded and dithered at least 2 times faster | 8.3 s, 4.3 s for Page Down | **1.8 s, 1.4 s** | met (4.6 and 3 times) |
| BBC, pictures on, well under a minute | 158 s, and most pictures never arrived | **48.5 s** with all 117 pictures (3.9 MB) | met |

Times are ARM instructions at 15 MIPS, as in phase 2. The 15 MIPS assumption still stands until a run on the 5mx.

### How it was measured

The phase 2 runs used the live network and the PC's clock, so two runs never did quite the same work, and a page could change between runs. `web/links/emu/run_links.py` (the copy of the harness) now has:

| Option | What it does |
|---|---|
| `--record DIR` | Saves every connection's traffic to `DIR/net.json`. Random numbers are made deterministic, so even TLS can be played back. |
| `--replay DIR` | Plays the traffic back instead of the network, with a **virtual clock**: instructions at 15 MIPS plus the time spent waiting. Runs are repeatable to the instruction, and Links' timers fire as they would on a 15 MIPS Psion. |
| `--rate N` | With `--replay`: the data arrives at N bytes a second of virtual time (10000 for a modem). By default it all arrives at once. |
| `--profile` | Instructions per function now come for each measured section (start-up, page, Page Down), and for the whole run. `PROFILE_FN=f1,f2` adds a breakdown by basic block inside those functions. |
| `--incl f1,f2` | Instructions inside these functions, callees included. |
| `--callers f1,f2` | As before. `CALLSITES=1` shows the exact call sites. |

Start-up (up to `pwb_ready()`) is now measured as its own section.

`web/links/emu/run_pages.sh` takes `NET=record` or `NET=replay` (`NETDIR`, default `build/links/net`). The recordings used here are in `build/links/net/`: the seven pages of phase 2, recorded on 2 October 2026, and `bbcpics` (BBC with pictures, recorded after the connection fix; `bbcpics-phase2` is the old one). For example:

```
NET=replay web/links/emu/run_pages.sh OUT                      # the phase 2 page set
NET=replay RUN_OPTS="--rate 10000" web/links/emu/run_pages.sh OUT
NET=replay web/links/emu/run_pages.sh OUT "bbcpics+=https://www.bbc.co.uk/"
```

The "before" figures come from the phase 2 sources, built unchanged into `build/links-orig/` and replayed on the same recordings. On a fast link they match phase 2's own figures to within a few per cent (the pages had changed a little since).

### Results per page

Fast link (all the data at once, as in phase 2): instructions, time at 15 MIPS, and heap peak.

| Page | Before | After | Heap peak before / after |
|---|---|---|---|
| start-up | 38.1M, 2.54 s | **1.2M, 0.08 s** | 1072 / **313 KB** |
| http://info.cern.ch/ | 14.5M, 0.97 s | **4.3M, 0.29 s** | 386 / 346 KB |
| http://68k.news/ | 81.6M, 5.44 s | **31.5M, 2.10 s** | 807 / 767 KB |
| the same, Page Down | 14.7M, 0.98 s | **5.2M, 0.35 s** | 826 / 786 KB |
| https://text.npr.org/ | 42.2M, 2.82 s | **24.9M, 1.66 s** | 419 / 382 KB |
| the same, Page Down | 5.5M, 0.37 s | **3.0M, 0.20 s** | 421 / 381 KB |
| https://en.m.wikipedia.org/wiki/Psion | 71.1M, 4.74 s | **52.4M, 3.49 s** | 532 / 532 KB |
| the same, Page Down | 7.5M, 0.50 s | **2.3M, 0.15 s** | 574 / 534 KB |
| https://www.bbc.co.uk/, pictures off | 300.6M, 20.0 s | **101.4M, 6.76 s** | 2256 / 2246 KB |
| the same, Page Down | 5.4M, 0.36 s | **1.9M, 0.12 s** | 1714 / 1676 KB |
| local picture page, pictures on | 124.0M, 8.27 s | **27.3M, 1.82 s** | 1142 / 1142 KB |
| the same, Page Down (progressive and 1600×1040 JPEGs) | 64.5M, 4.30 s | **21.3M, 1.42 s** | 1345 / 1345 KB |
| the same, pictures off, then Show pictures | 20.5M + 111.7M, 8.8 s | **6.9M + 24.2M, 2.1 s** | 1163 / 1163 KB |

At 10 KB/s (`--rate 10000`, about a modem link), where Links lays out the half-loaded page while it waits:

| Page | Before | After |
|---|---|---|
| info.cern.ch | 17.0M, 1.13 s | **5.3M, 0.35 s** |
| 68k.news | 109.4M, 7.29 s | **37.6M, 2.51 s** |
| NPR | 42.2M, 2.82 s | **24.9M, 1.66 s** |
| Wikipedia | 71.2M, 4.75 s | **52.5M, 3.50 s** |
| BBC, pictures off | 210.2M, 14.0 s | **163.8M, 10.9 s** |
| local picture page | 277.3M, 18.5 s | **43.7M, 2.91 s** |
| Show pictures | 222.8M, 14.9 s | **41.2M, 2.75 s** |

(At 10 KB/s the transfer itself takes longer than the CPU time: 4 s for 68k.news, 12 s for BBC.)

**BBC with pictures** (`bbcpics`, 117 JPEGs):

| | Before (phase 2 build, old recording) | After (new recording) |
|---|---|---|
| Load | 2366M, 158 s; 100 TLS connections; 1.06 MB in, so most pictures never arrived | **727M, 48.5 s**; 3 connections; 3.9 MB in, every picture |
| Then 30 × Page Down | not comparable | 285M, 19.0 s (pictures decoded as they come into view) |
| Heap peak | 2.8 MB | 6.3 MB at load; **10 MB after 30 screens, with 290 refused allocations** |

The memory figure is the known problem of phase 1 item 4 (the bitmap of every picture drawn is kept), not a new one; it now shows because the pictures really arrive.

### What the profiles showed

Phase 2's estimates were right about the shape but not about the order. With the deterministic runs:

1. **Glyphs** were most of a text page. Each new glyph (letter, size, style and colour) cost about 164,000 instructions, 120,000 of them in libpng and zlib unpacking a 40 px PNG master one row at a time (`inflate` rather than `inflate_fast`, a new Huffman table and two CRCs per glyph). 68k.news draws 240 of them.
2. **Start-up** was 30 million instructions of soft-float `pow()` building dither tables that never change, plus 768 KB of scratch tables.
3. **The HTML parser** went through a big page character by character, several times:
   - the main loop's per-character tests ran through every byte of `<script>` and `<style>` (BBC: 336 KB of script and 181 KB of style);
   - finding each tag in the element table did `strlen` on every name (316,000 calls on BBC);
   - each `get_attr_val` call scanned all the tag's attributes again, and a tag's handler asks for several (BBC's 416 KB of tags were scanned about six times over: long class lists, SVG paths, srcsets);
   - the tags were also parsed in full by `scan_http_equiv` and `find_form_for_input`.
4. **Division.** The ARM710 has no divide instruction: the glyph scaler divided every output pixel, `mix_two_colors` every channel by 255, `compute_width` every character, and the picture scaler every channel.
5. **The C library stand-ins.** `strcspn` called `strchr` for each byte; `strstr` compared the whole needle at each position. On ESTLIB they are probably no better.
6. **TLS bulk decryption.** libtomcrypt saw a little-endian 32-bit target and made every 32-bit load and store a 4-byte `memcpy` call.
7. **Pictures.** A progressive JPEG was decoded again (a full IDCT with block smoothing) after each of its scans. Worse, while a page loaded, every layout pass decoded every picture that had arrived so far in full, only for the `PSI_LAZY_IMAGES` patch to throw the result away; and `header_dimensions_known` filled each picture's buffer with the background first.
8. **The connection bug** (below): 97 of BBC's 100 TLS connections were thrown away unused.
9. **Over a slow link**, the main loop woke for every few bytes: 8,400 reads for 42 KB, about 700 instructions a byte.

### The changes, and what each saved

Figures are for the fast link, measured one change after another (so a gain is on top of those above it). "Same" means the screenshots were compared and are identical.

| # | Change | Where | Gain |
|---|---|---|---|
| 1 | **Run-length glyphs.** `mkfont.py --rle` writes each 40 px master as simple run-length data ('R', width, height, then runs of paper, runs of ink and literal bytes) instead of a PNG. The data is about the same size (446 KB against 440 KB of PNG); `load_char` unpacks it in a few thousand instructions. Same pixels. | `mkfont.py`, `epoc.mk`, `dip.c` | 68k.news 81.6M → 59.9M, cern 14.5M → 7.7M, NPR 42.2M → 31.6M, 68k Page Down 14.7M → 8.2M |
| 2 | **Colour tables made at build time.** `mkdither.py` computes dither.c's tables for PsiWeb's fixed settings (RGB565, gammas 2.2 and 1.0, 8-bit tables) the same way in double precision; `init_dither` copies them. A check build (`-DPSI_DITHER_CHECK`) confirmed all 1,536 entries are the same as those computed on the ARM. | `mkdither.py`, `dither.c` | start-up 38.1M → 2.7M; heap at start-up 1072 → 313 KB |
| 3 | **Parser fast paths.** The main loop skips script and style bodies to the next `<` with `memchr`, and runs of ordinary text in a tight loop (exactly what the per-character code did with them). Element names' lengths are kept, and the first letter is checked before `casecmp`. `get_attr_val` steps over the values of attributes it was not asked for in a tight loop. `scan_http_equiv`, `find_form_for_input` and `skip_element` look for `<` with `memchr`. | `html.c`, `html_tbl.c` | BBC 292M → 233M (`parse_html` itself 43.7M → 7.3M) |
| 4 | **`psi_str.c`**: `strlen`, `strchr` and `memchr` a word at a time; `strcspn` and `strspn` with a 256-bit table; `strstr` that finds the first character first. `psicompat.h` maps Links' calls to them, so the device no longer depends on how good ESTLIB's are. gcc 3.0 rebuilt the 0x01010101 and 0x80808080 constants inside every loop (8 instructions a word): an empty `asm` keeps them in registers. | `psi_str.c`, `epoc/psicompat.h` | part of 3, 5 and 6; 68k.news 49.9M → 44.4M from `strstr` alone |
| 5 | **Word-at-a-time attribute values**: `parse_element` and `get_attr_val` find the closing quote 4 bytes at a time. **No gzip CRC**: `inflateValidate(&z, 0)`; TLS or TCP has already checked the data (15.5M on BBC). | `html.c`, `compress.c` | BBC 233M → 214M |
| 6 | **Attribute memo.** For the element parse_html is handling, `get_attr_val` records where each attribute starts on the first call, then goes straight to the one asked for (or returns NULL). It is used only for that element, and rebuilt for each new one, so it can never point into another document; the result is exactly what the full scan returns. The value asked for is then copied in one go (it was copied a character at a time, with a `realloc` every 32). | `html.c` | BBC 214M → 187M |
| 7 | **No divisions in glyphs and text.** Glyph widths are kept per font size (1 KB a size, the last four sizes). The glyph scaler divides by a reciprocal (exact for its weights, tested for every case). `mix_two_colors` divides by 255 with a shift and one correction (exact over its whole range, tested), and works out grey text once rather than three times. | `dip.c` | 68k.news 44.4M → 35.8M, cern 7.6M → 5.9M, BBC 178M → 172M |
| 8 | **Faster drawing.** ARMv3 has no halfword loads or stores, so a 16-bit pixel was two byte accesses: fills and glyph copies now go a word at a time, joining words when the source and destination are 2 bytes apart. The 565 to 16-grey conversion uses small tables (r, g and b pre-multiplied, and the result for each of the 16 dither thresholds, 4 KB) instead of multiplies, and handles 8 pixels at a time with word stores when they are all paper or all ink. Same output as `pwgrey.c`, to the bit. | `psi_drv.c`, `psi_grey.c` | Page Down: 68k 6.7M → 5.2M, NPR 4.5M → 3.0M, BBC 3.5M → 1.9M, Wikipedia 3.8M → 2.3M |
| 9 | **Progressive JPEGs** take in all the data there is before an output pass, so a picture decoded from the cache is shown once, not once a scan; over a slow link they still show as the data comes. **The sRGB picture gamma table** (768 soft-float `pow()`s) is made at build time too. | `jpeg.c`, `dip.c`, `mkdither.py` | pictures page 108M → 47M, its Page Down 62M → 21M, Show pictures 105M → 44M |
| 10 | **Width lookups** find the size's table once per string, not once per character. | `dip.c` | 68k.news 34.2M → 31.5M |
| 11 | **libtomcrypt's own byte-wise loads and stores** (`-DLTC_NO_ASM` for the TLS objects): no 4-byte `memcpy` calls in ChaCha20, Poly1305 and SHA-256. | `epoc.mk` | BBC with pictures 858M → 727M. BBC without pictures 164M → 101M, but only 4M of that is decryption: the rest is one layout pass fewer, because the page now arrives sooner (see "Timing" below) |
| 12 | **Pictures during layout.** While `insert_image` only wants a picture's size, a JPEG stops once the size is known and its buffer is not filled with the background (it is freed at once). Before, every layout pass while the page loaded decoded each picture that had arrived. | `img.c`, `jpeg.c` | pictures page 47M → 27M, Show pictures 44M → 24M, BBC with pictures 1200M → 858M |
| 13 | **The scheduler** keeps each queued connection's host, port and keepalive key, by its (unique) count. `check_queue` used to parse every queued URL four or five times on every pass: 70,000 `parse_url` calls with BBC's pictures queued. | `sched.c` | part of BBC with pictures |
| 14 | **The connection bug** (below). | `epoc/psi_os.c` | BBC with pictures: 100 TLS connections → 3 |
| 15 | **Fewer wake-ups over a slow link.** When all `psi_select` has to report is a trickle on the connection (under 2 KB waiting), it lets up to 30 ms more collect first, once per call. | `epoc/psi_os.c` | 68k.news at 10 KB/s 85.6M → 37.6M |
| 16 | **The picture scaler divides by an invariant** (Granlund and Montgomery): a 32 × 32 high multiply in 16-bit pieces, as the ARM710 has no long multiply. Exact for every 32-bit dividend (140 million cases tested). | `dip.c` | 8 screens of BBC pictures 135M → 111M |

**The connection bug.** `psi_write` asked `pwn_is_open()` before every write whether the server had closed the connection, and `pwn_is_open` takes any bytes waiting before a request as a sign that it has. On a new TLS 1.3 connection the server sends NewSessionTicket records straight after the handshake, so nearly every new connection was closed again before its first request, and Links retried on another. On BBC with pictures that was 100 TLS handshakes (about 13 million instructions each, and a dial each over the modem), and with `-retries 1` most pictures gave up. Phase 2 put this down to the server closing idle connections; it was this. Now the check is made only on a connection that has already answered, i.e. a kept-alive one being reused. `tls_read` already skips the tickets.

**Compiler flags.** `-O3` was within 1% either way and made the code 200 KB bigger. `-Os` was up to 5% slower on TLS pages, 4% faster on pictures, and only 17 KB smaller. `-O2` stays. Hot loops were tuned by looking at gcc 3.0's output instead (constants kept in registers, no halfword accesses, no divisions, fewer calls).

**Size.** `.text` (code and constant data) grew by 19 KB, from 1.676 MB to 1.695 MB, of which 6 KB is the run-length fonts. There are 9 KB more static variables (the grey tables, the width tables, the attribute memo and the scheduler's cache).

### Timing: why the slow link costs more

Links lays out the page while it loads: after each layout pass it waits 15 times as long as the pass took (at most 1 s before the first), then lays out everything that has arrived so far, decompressing it again from the start. On a fast link BBC is laid out twice; at 10 KB/s four times (124M of BBC's 164M). This is Links' own, sensible, policy (layout takes about 1/16 of the time while loading), and it means the fast-link figures are the floor. The CPU time also overlaps the transfer time, which is longer on a modem.

### What is left

In order of size:

1. **TLS key exchange: 13.5 million instructions (0.9 s) a connection**, in `ssh/db/src/curve25519.c` (x25519 with 16-bit limbs and per-product carries, about 2,000 instructions a field multiply). It is 27M of Wikipedia's 52M (the redirect to en.wikipedia.org means two connections) and half of NPR. A field multiply with 13-bit limbs and no carries inside the column sums, or Karatsuba, or ARM assembly could perhaps halve it. TLS session resumption in `ssh/tls13.c` would avoid it for repeat visits. Both are outside `web/links/`.
2. **TLS bulk decryption: about 120 instructions a byte.** On BBC with pictures, `__muldi3` (Poly1305's 64-bit products, with no long multiply) is 131M and ChaCha20 another 160M. A Poly1305 with 32 × 32 products done in 16-bit pieces, and a ChaCha20 that keeps its state in registers, could save about a third of BBC with pictures.
3. **The HTML parser still parses every tag three times** (`parse_html`, `scan_http_equiv`, `find_form_for_input`): `parse_element` is 28M of BBC's 101M. A cache of tag ends for the current document, or stopping `scan_http_equiv` earlier, would remove two of the passes.
4. **Partial layouts on a slow link** decompress and parse the whole page again each time. An incremental inflate in `compress.c` would save about 6M per pass on BBC.
5. **Pictures:**
   - every picture is decoded when it scrolls into view, and its bitmap kept: after 20 screens of BBC the 10 MB heap is full (phase 1 item 4, still to do: evict bitmaps, 4-bit grey storage);
   - a grey path through `img.c` (1 byte a pixel instead of 3, no `gray_to_rgb`, `agx_24_to_48`, colour scaling and 565 rounding) would roughly halve the cost after the IDCT;
   - photographs cost about 40 instructions a pixel pair in the grey conversion (`grey_pair`).
6. **New glyphs** still cost about 40,000 instructions each (scale, sharpen, mix, round): 68k.news's Page Down is 0.35 s because it brings new sizes into view. Pre-rendering the few sizes PsiWeb uses would remove it.
7. **TLS reads block** until a whole record has arrived (up to 16 KB, 1.6 s at 10 KB/s), during which Links cannot react to the pen or keys. Not a CPU cost, but worth fixing with the network work.
8. **The 15 MIPS assumption**: still to be checked with one run on the 5mx.

## Phase 4: device build

Written 2 October 2026. Phase 4 makes the real thing: `PsiWeb.sis` whose `psiweb.exe` is Links, linked against ESTLIB with PsiWeb's own EPOC backend, so Dan can install it on the 5mx and time it. PsiWeb's version is now **0.62** (`web/app/psiweb.cpp` `KVersion`, `web/pkg/psiweb.pkg`). It is not signed, `dist/` is untouched and nothing is committed: that is the release step.

### Sizes

| | Size |
|---|---|
| `psiweb.exe` (Links, petran'd) | **1,860,884 bytes** (1.77 MB; NetSurf's was 2.6 MB) |
| `PsiWeb.sis` (app, engine, icons, COPYING, and ESTLIB's `STDLIB.SIS`) | **2,021,038 bytes** (NetSurf 0.61: 2,800,785) |

The petran settings are NetSurf's: heap 256 KB to **10 MB**, stack 256 KB. Links' peaks in the ARM harness are 0.3 to 2.3 MB for text pages and up to 6 MB for BBC with all its pictures, so the 10 MB ceiling stays. The EXE is loaded into RAM, so it costs about 1.8 MB on top of the heap.

### How to build

```
tools/docker/psibuild web/build.sh                          # dist/PsiWeb.sis (Links)
tools/docker/psibuild "PSIWEB_SIS=$PWD/build/x.sis web/build.sh"   # elsewhere
tools/docker/psibuild "make -f web/links/epoc.mk -j8 exe"   # only the engine: build/links/epoc/psiweb.exe
```

`web/build.sh` fetches and patches Links the first time (`web/links/fetch.sh`), builds the engine with `web/links/epoc.mk exe`, then PsiWeb.app and the package as before. The log is `build/web-links.log`.

**Switching back to NetSurf:** `PSIWEB_ENGINE=netsurf web/build.sh [host]` builds the 0.61 engine exactly as before (`web/Makefile`, `web/engine/fetch_psi.c`, `web/fb`, `web/patches` are all still there). The app works with either engine: the shared chunk and the commands are the same, and the only new backend call (`pwb_first_url`) is unused by NetSurf. The NetSurf engine would show Show pictures as "pictures from now on" rather than "this page".

### What changed

**The EXE (`web/links/epoc.mk`, target `exe`)**
- The same Links, libjpeg, libpng, zlib, TLS and psi objects as the emulator build, plus:
  - `web/engine/pwepoc.cpp`: the chunk shared with PsiWeb.app, PsiWeb.log, heartbeats, the 64 Hz `pwb_gettimeofday`;
  - `ssh/psiglue.cpp` (unchanged): the modem and Psion Internet routes;
  - `web/engine/pwupdate.c`: Tools > Update PsiWeb;
  - `web/links/epoc/psi_heap.cpp`: `psi_mem_report` from RHeap (`mem <page>: heap … KB` lines in PsiWeb.log);
  - `web/links/epoc/psi_stubs.c`: `kill`, `signal`, `lstat`, `readlink`, `ftruncate`, `execvp`, `remove`, which ESTLIB lacks and nothing on PsiWeb's path calls.
- Linked as `web/Makefile` links NetSurf: `eexe.o`, `ecrt0.o`, `estlib.lib`, `euser`, `efsrv`, `c32`, `esock`, `insock`, `nifman`; then petran with NetSurf's UIDs.
- `psicompat.h` now also:
  - sends `gettimeofday` to `pwb_gettimeofday`: the Psion's clock counts whole seconds, which would make every Links timer (the 20 ms input poll, layout delays) wait for the next second;
  - sends `fprintf`/`vfprintf`/`printf`/`fflush`/`perror` on stdout and stderr to PsiWeb.log (`psi_os.c`), because ESTLIB would open a text console over the app. A Links fatal message ("out of memory" and the like) also becomes what the app says when the engine stops.

**The main loop (`psi_os.c`)**
- psiglue's `pg_wait` times out by `TTime`, which has a one-second step on the Psion. Used for a 20 ms wait, it waited up to a second, and in the emulator keys and taps took 4 to 5 seconds to arrive. Now:
  - with no connection waiting, `psi_select` sleeps exactly (`pg_msleep`, 20 ms at most);
  - with a connection waiting, it still uses `pg_wait`, but PsiWeb.app sets `net.resized` whenever it hands over a key, a tap or a command, and `pg_wait` returns at once on that. (On the modem route; over Psion Internet psiglue's socket wait still runs to the second. A tick-based `NowMicro` in psiglue would fix both, but psiglue is shared with PsiTerm and PsiMail and was not touched.)
- `main()` adds `-http-proxy host:port` when Preferences > Use a proxy is set. `https://` addresses then also go to the proxy as plain requests (`sched.c`), as NetSurf's fetcher did with WebOne.
- On quit the engine hangs up and frees the serial port (`pwn_release_now`).

**The start page.** The first page is the one PsiMail asked for, or else **`about:welcome`**, a page built into the engine (`web/links/welcome.html`, made into `welcome.inc` by `epoc.mk`; served by `psi_about_func`). So starting PsiWeb needs no network and no file. File > Home page still goes to the home page (`http://68k.news/` unless set); an empty home page is the welcome page. Up to 0.61 PsiWeb opened the home page at start, which dialled.

**Pictures (Dan's decision 2)**
- View > **Show pictures** (Ctrl+I) is now a command, not a tick box: it fetches the pictures of the page showing. Following a link goes back to none; going Back to that page shows them again (`psi_show_pictures`, called from `cached_format_html`).
- The old Images setting became Tools > Preferences > **Pictures**: "Only when asked" (the standard) or "On every page". It is kept as a new byte at the end of PsiWeb.ini, so NetSurf's old setting (on by default) does not carry over. The engine restarts when it changes, as for the other preferences.
- On an `about:` page, Show pictures says "No pictures to show"; with pictures on every page, "Pictures are already shown on every page".

**No Links interface (Dan's decision 3)**
- Keys (`psi_filter_key` in `psi_drv.c`):
  - outside a form field, only Up, Down, Page Up/Down, Home, End, Enter, Tab, Space (page down) and Backspace (back) reach Links; Left and Right scroll a wide page sideways (Links would go back and follow a link). Letters, digits, punctuation, Ctrl+letters and Esc are dropped, so `g`, `q`, `/`, `s`, `d` and the rest no longer open Links' dialogs;
  - in a text field, letters, digits, punctuation, the arrows, Backspace, Delete and Ctrl+A/E/U/K/D reach it;
  - if a Links window is open over the page (a `<select>` list), it gets every key, Esc included.
- Dialogs: Links' `msg_box` makes no window. Its text goes to the app as an infoprint (through `net.link_msg`, which the app already shows), and the box is answered as Esc would answer it. In particular:
  - a page that fails says "Page not loaded - …" in plain words (`psi_load_failed`: "the connection broke", "server not found"…), or psiglue's own reason when the connection could not be made;
  - a page that needs a user name and password says "Not available - this page needs a user name and password" and shows the server's own page;
  - a file Links can't show says "Not available - PsiWeb cannot show or save … files".
- `file:///D:/x` is the file `D:/x` (PsiMail's "View as web page"), and the built-in pages may link to files.

**Links visible.** Links are underlined (`html.c`, `set_link_attr`). Their blue is all but black in 16 greys; with the underline they read clearly and the text stays crisp black (see `68k-1.png` and the screenshots below).

**Commands.** Open, Back, Forward, Reload, Stop, Home, Zoom, Top/End of page, Disconnect (Links' connections stopped, then the port freed) and Update (Links' connections stopped, then `pw_update_run`) are all handled in `psi_drv.c`. Connect, Page information and the settings dialogs are the app's own and are unchanged.

**App wording.** About now credits Links; Help describes Links, the welcome page, pictures on request and sideways scrolling.

### Test results

**ARM harness** (`NET=replay web/links/emu/run_pages.sh`, the phase 3 recordings): every page passes with the same figures as phase 3 to within 1 to 4% (the underlines): cern 4.5M, 68k.news 31.7M, NPR 25.3M, Wikipedia 52.6M, BBC 101.6M, the picture page 27.3M, Show pictures 7.0M + 24.2M instructions; heap peaks unchanged; no failed allocations. `about:welcome` and a pen tap on one of its links were also run there.

**The Psion emulator** (`tools/emu`, the real `PsiWeb.sis` contents, ESTLIB, the 5mx ROM). Screenshots are in this session's scratchpad, `scratchpad/links4/shots/`:

| Shot | What it shows |
|---|---|
| `welcome.png` | PsiWeb starts with Links and draws the welcome page about 14 s (simulated) after the tap on its icon, most of it loading the 1.8 MB EXE from the card |
| `file-menu.png`, `view-menu.png` | menus open; Stop and Back/Forward are dimmed only in their own panes (no EIKON panics in any run) |
| `local-nopics.png` | a page on the card (`file:///D:/TEST/PICS.HTM`, reached by a tap on a link): alt text where the pictures are |
| `local-showpics.png` | after View > Show pictures: JPEG, PNG and GIF |
| `local-zoom110.png` | the Zoom button: 110% |
| `form-typing.png` | a tap on a text field, then H, I, G typed into it (G did not open Links' Go to dialog) |
| `back.png` | the Back button |
| `home-no-link.png` | Home with no network: psiglue's reason as an infoprint (too long for the screen: an existing psiglue wording) |
| `preferences.png` | the new Pictures line |
| `page-info.png` | Page information |
| `connect.png` | File > Connect |
| `update-source.png`, `update-progress.png`, `after-update.png` | Tools > Update PsiWeb runs in the Links engine, fails without a serial port, and the engine carries on |
| `toolbar-hidden.png` | View > Show toolbar off: the engine restarts at 640 wide on the same page |

The local page and a few test-only lines needed a test engine: the same build with a link to `D:\TEST\PICS.HTM` on the welcome page (made in the scratchpad, never in the tree).

**The card wedge.** Several runs stalled at "Starting the browser engine..." with the emulator's CF card stuck part way through loading `psiweb.exe` (always at 130 ATA commands). It depends on the card image and the EXE's exact bytes, not on the run: a layout that stalls does so every time, and another PAD.BIN size (`PAD=` in a scratch copy of `mkcard.ts`) loads at once. Restarting the engine (the toolbar test) can hit it too. The emulator also writes to the card image, so each run needs a fresh one.

Not tested: anything over the network on the Psion (the emulator's serial port does not open), a real 5mx, Psion Internet.

### Known issues

1. **Psion Internet waits.** With a connection open over Psion Internet, a key or tap can wait up to a second (psiglue's socket wait; see *The main loop*). The fix belongs in psiglue (`NowMicro` from `User::TickCount()`), which is shared with PsiTerm and PsiMail.
2. **`<select>` lists** still open Links' own pop-up list (the only Links window left). It works with the pen and the arrows.
3. **No page progress in the app.** Links' status line ("Received 12 KB…") goes to Page information and PsiWeb.log only, as NetSurf's did; while a page loads the app shows psiglue's link messages and the busy state.
4. **Pictures memory on long scrolls** (phase 3, item 5) is unchanged: pictures off by default makes it rare.
5. Phase 3's list of remaining CPU work stands.

### What Dan should test on the 5mx

1. Install `PsiWeb.sis` (0.62) over 0.61. PsiWeb should open on the welcome page without dialling. Note the time from the tap to the page.
2. Tap 68k.news on the welcome page; time it to the first screen and to idle (the busy message going). Page Down a few times.
3. Back, Forward, Home, Reload, Esc during a load, Zoom in and out.
4. On a page with pictures (68k.news stories, Wikipedia), View > Show pictures. Then Preferences > Pictures > On every page, and back.
5. A text field: DuckDuckGo Lite (tap the field, type, tap the button).
6. https: text.npr.org; and with the WebOne proxy set, the same pages through it.
7. Both routes: the modem, and Psion Internet (PPP).
8. File > Disconnect, then PsiTerm can use the port; Tools > Update PsiWeb opens and runs (it will say it is current or offer 0.62 again).
9. PsiMail's "View as web page" (a `file:///D:/…` page).

**What to send back:** `C:\System\Data\PsiWeb.log` (and `PsiWeb.old`, the run before) after a session. It has timestamps (`mm:ss.t`) for the start, each status and busy change, the link messages, every infoprint, and a `mem <page>: heap … KB` line when each page finishes, which gives the real timings and memory to set against the 15 MIPS estimates.
