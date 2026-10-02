# PsiWeb: making the modern web usable on the 5mx

This plan was drafted on 2 October 2026 by a planning agent that read the code. Line numbers are as of v0.78. The NetSurf paths are under `build/netsurf/` and are patched via `web/patches/*-psion.diff`.

## Goals and constraints

**Target pages**
- News: BBC, Guardian.
- Reference and search: Wikipedia, search, GitHub READMEs, Hacker News.
- Everyday: weather, simple forms and logins.

**Hard limits**
- **Memory:** the `psiweb.exe` heap is capped at **10 MB** (`web/Makefile:131`, `-heap 0x40000 0xa00000`). That sits inside about 16 MB shared with EPOC and PsiWeb.app.
- **CPU:** 36 MHz, no FPU.
- **Link:** about 11 KB/s.
- **Screen:** 640×240, 16 greys.

**UX:** follow the EIKON style guide.

**Gate:** every stage is checked against the harness corpus (stage 0), not by feel.

## Current state

### Memory

**BBC home page: 25.9 MB peak against a 10 MB ceiling**
- The harness allocator overstates this figure (`web/emu/emu_rt.c`): it is a 48 MB arena that never fails, and it rounds small blocks up to powers of two with an 8-byte header.
- EPOC's RHeap uses a 4-byte header and 4-byte granularity, so the real peak is probably 15–20 MB. That is still about twice the ceiling.

**No out-of-memory path**
- NetSurf's `die()` goes to `pwb_fatal` (framebuffer `gui.c`).
- The app then shows "The browser engine has stopped".
- An allocation that fails mid-layout is therefore a crash, not a message.

**NetSurf options**
- Already set (framebuffer `gui.c`):
  - 1 MB memory cache;
  - 6 fetchers, 2 per host;
  - images on or off, and zoom, taken from the shared chunk.
- Not set: `author_level_css` (default true), `block_advertisements`, `animate_images`, `incremental_reflow`, `min_reflow_period`, `font_min_size`.

**`author_level_css=false` is almost a free text mode.** It already skips:
- `<link>` stylesheets (`css.c`);
- `<style>` (`dom_event.c`);
- `style=""` (`box_construct.c`).

**Every author stylesheet is fetched and compiled, with no cap.** BBC ships several hundred KB of CSS. libcss bytecode and selector hashes are a multiple of that, which makes CSS the prime suspect for the memory, ahead of the DOM and the box tree. Computed styles are interned, so they are not the problem.

**The page source is kept for the page's life** (llcache), because a charset change re-parses it. The DOM is also kept until `html_destroy`.

**Images**
- Only GIF and BMP decoders are built (`web/netsurf.sh`, `gen.py`).
- Images are decoded to 32 bpp (framebuffer `bitmap.c`).
- A 300×200 GIF therefore costs 240 KB of heap for a 16-grey display.

### Network and CPU

**Fetcher** (`fetch_psi.c`)
- HTTP/1.1 keep-alive, gzip, one request at a time.
- Reads in 2 KB pieces.
- No cap on body size, and no cache on the card.
- Proxy mode rewrites https:// to http:// for WebOne.

**Open address and search**
- Typing in the address box goes through `search_web_omni` with NetSurf's default provider (`www.duckduckgo.com/html`).
- No `SearchEngines` resource is embedded.
- The welcome page already points at lightweight sites (`web/res/welcome.html`).

**Speed:** about 15 MIPS, so a 48 KB modern page takes roughly 8 s. The clock bug fixed in 0.58 had made every scheduled callback wait a whole second.

### Measurement

`run_psiweb.py` already has:
- `--count`, `--sample` and `--trace`;
- `--allocs`, which reports live memory by the function that allocated it;
- a heap line at exit.

It has no corpus runner, no 10 MB limit and no per-page report.

## Options compared

| Option | Memory saved | Effort | Risk | Needs a server? |
|---|---|---|---|---|
| A1 Enforce a 10 MB arena in the harness; calibrate against RHeap | none (it gives the true figure) | small | low | no |
| A2 Turn `author_level_css` off: a "Text" page style | very large (probably 50–70 % on BBC) | tiny | low (pages look plain) | no |
| A3 Cap stylesheets: skip sheets over N KB or after K sheets, drop `@font-face` and print media, skip `.woff` | large | medium | medium (a libcss/css.c patch) | no |
| A4 A hard heap ceiling and a graceful "Page too big" (counting malloc; abort at about 8.5 MB; an `NSERROR_NOMEM` path) | reliability | medium | medium | no |
| A5 Image caps: maximum pixels per image, skip images over N KB, 8-bit grey bitmaps | medium (with images on) | medium | low | no |
| A6 Free the source after conversion | small (1× the HTML) | medium | medium | no |
| B1 A reader mode on the device (strip nav, aside, footer and script before parsing) | large | large | medium (heuristics in C) | no |
| C1 WebOne proxy (already supported) | TLS and images only | none | the user has to run it | yes |
| C2 PsiProxy: a transcoder made for the Psion (readability, simplified HTML, 16-grey GIFs, TLS, gzip) | very large, and it saves CPU and bandwidth too | large (Python on a PC) | depends on a server | yes |
| C3 Lightweight sites and a lite search engine by default | large for those sites | tiny | sites may vanish | third party |
| D1 A cache for CSS and images on the CF card | bandwidth | medium | the card-write wedging noted in CLAUDE.md | no |
| E1 Tune incremental reflow; draw as the page arrives | how fast it feels | small | low | no |

## Roadmap

### Stage 0: measure (1–2 days)

**Work**
- Add `web/emu/corpus.py`. It runs a fixed list of URLs through `run_psiweb.py --count --allocs`. The list:
  - bbc.co.uk
  - theguardian.com
  - en.m.wikipedia.org/wiki/Psion
  - news.ycombinator.com
  - a GitHub README
  - a lite.duckduckgo.com search
  - a weather page
  - a login form
- For each URL it records peak heap, bytes received, estimated device seconds, instruction count and a screenshot, into `build/web-corpus/<date>.tsv`.
- Add `PW_ARENA_MB` to `emu_rt.c`, with 10 MB as the default.
- Report the bytes actually requested as well as the rounded figure, to calibrate the allocator.

**Done when:** the corpus runs unattended, and BBC fails the 10 MB gate with its top allocator named.

### Stage 1: memory diet and graceful failure (1 week)

**Work**
- **Page style:** View > Page style > Normal / Simple / Text, with ticks.
  - Text: `author_level_css` off.
  - Simple: the A3 caps. Simple is the default.
  - Plumbing: a new `PW_CMD_STYLE` command and a `page_style` field in the shared chunk.
- **A3:** in `css.c`, skip sheets whose media is not screen or all, and stop after K sheets. In `fetch_psi.c`, refuse `text/css` bodies over N KB, so the page still lays out without them.
- **A4:**
  - A counting malloc wrapper in `nscompat.c`. All of NetSurf is compiled with `-include nscompat.h`, so this is one `#define`.
  - Allocations fail above about 8.5 MB.
  - `NSERROR_NOMEM` reaches the status line as "Page too big - try Text style", instead of the engine stopping.
  - Show `heap_used` in Page information.
- **Options:** set `block_advertisements`, `animate_images = false` and `font_min_size`.

**Done when:** every corpus page renders under 10 MB in Simple, and any page that can't gives the infoprint rather than an engine restart.

### Stage 2: lite defaults and search (2 days)

**Work**
- Embed a `SearchEngines` resource that uses `lite.duckduckgo.com/lite/?q=%s`.
- Add a lite-site table in `fetch_psi.c`, applied in Simple and Text and listed in Help:
  - BBC → a text news front end;
  - Wikipedia → `en.m.wikipedia.org`;
  - GitHub → the README.

**Done when:** news, Wikipedia and search in the corpus each come in under 4 MB and under 10 s estimated.

### Stage 3: pictures that fit (3–4 days)

**Work:** A5. Store bitmaps as 8-bit or 4-bit grey in `bitmap.c`, with caps on pixels and bytes for each image.

**Done when:** a Wikipedia article with pictures on stays under 10 MB.

### Stage 4: PsiProxy, an optional transcoder (1–2 weeks, Python)

**Work**
- A sibling of WebOne, run on a PC. It does:
  - TLS termination;
  - readability extraction;
  - HTML rewritten to a small dialect;
  - pictures rescaled to 16-grey GIFs at page width;
  - gzip;
  - a `/search` endpoint.
- On the Psion side, PsiWeb needs only the existing proxy setting and an `X-PsiWeb: style` header.

**Done when:** every corpus page through PsiProxy comes in under 3 MB and 5 s, and the BBC front page is readable with headlines and pictures.

### Stage 5: bandwidth and feel (ongoing)

**Work**
- Larger reads.
- Conditional requests against the memory cache.
- Incremental reflow tuned to the layout cost measured with `--sample`.
- A cache on the card is deferred, because of the card-write wedging.

### The first three things to do
1. Run `--allocs` on BBC. It should name the culprit within an hour.
2. Enforce the 10 MB gate in the harness.
3. Add the `author_level_css` toggle. It costs almost nothing and probably halves BBC's memory on its own.

## Open questions for Dan

1. How much of the 25.9 MB is slack from the power-of-two allocator? Stage 0 answers this before any patching.
2. Would you run a PsiProxy on a PC routinely?
   - If not, Stages 1–3 carry the whole load.
   - If so, Stage 4 could come before Stage 3.
3. Is 10 MB the right size for the engine's heap? It might need trimming to leave room for PsiMail running alongside.
4. Is a larger `netsurf-psion.diff` acceptable for the libcss and css.c caps? The alternative is caps only in `fetch_psi.c`, which is coarser but needs no NetSurf patch.
5. Does 0.58 or later actually render pages on the real 5mx? The whole roadmap assumes the harness and the device agree.
