# PsiWeb never shows a web page

> **Superseded, 3 October 2026.** This issue is about the **NetSurf** engine (PsiWeb up to 0.61). From 0.62 PsiWeb's engine is **Links 2**, and NetSurf is only kept in the tree as a fallback (`PSIWEB_ENGINE=netsurf web/build.sh`). See [`web/links/PORTING.md`](../../web/links/PORTING.md).
>
> The symptom reported here ("Checking the modem…" staying up after a page) was reproduced with Links in the 5mx emulator (`python3 tools/emu/net.py web`) and fixed in phase 5: the modem reports the server closing in-band ("NO CARRIER"), and the engine's main loop did not count that as the connection being readable, so the page stayed "loading" until a time-out. Links pages do render in the 5mx emulator over the emulated modem. What is still to be confirmed is a page on Dan's real 5mx (PORTING.md, phase 5, "What Dan should test").
>
> The NetSurf notes below are left as they were.

Status: **fix in 0.58, to be confirmed on the 5mx**. Logged 2 Oct 2026, last seen in PsiWeb 0.57 (dev c5dcd44).

## Finding (2 Oct 2026)

- The engine and the app's repaint are fine: the host harness renders pages and error pages, and in the emulator NetSurf's own toolbar reaches the screen.
- The Psion's clock (`gettimeofday`, `TTime`) has **whole-second** resolution. NetSurf's scheduler and `pwb_ms()` used it, so every scheduled callback (fetch poll, redraw, layout) waited for the next second. A 30 KB page at 115200 baud took 46 s instead of 3.3 s in the harness (`PW_COARSE=1 web/emu/run_psiweb.py`).
- 0.58: `pwb_gettimeofday()` in `pwepoc.cpp` adds the 64 Hz tick count; NetSurf and libnsutils are built to call it (`web/gen.py`); `pwb_ms()` uses it.
- 0.58 also writes `C:\System\Data\PsiWeb.log` (previous run: `PsiWeb.old`): status text, busy changes, link messages, fetch errors, frames drawn. If pages still don't show, read it.
- The emulator could not confirm the fix: `RComm::Open` on COMM::0 never returns there.

## Symptom

- On Dan's real Psion 5mx, PsiWeb has **never displayed a web page**, in any version from 0.1 to 0.57.
- PsiWeb.app starts fine: the toolbar, menus, dialogs, Help, About and Preferences all work.
- After you enter an address, the page area stays empty.
- The emulator (psionEmulators harness, see `tools/emu/`) shows the same failure:
  - the HTTP reply **does arrive**;
  - the status still says **"Checking the modem…"** and never changes;
  - no page is drawn.

So the problem is somewhere between "the reply arrived" and "the pixels reached the screen". It is not caused by the emulator.

## History that matters

- 0.1 crashed at start with KERN-EXEC 3. Fixed in 0.2.
- 0.2 left every page stuck on "Loading". 0.3 fixed several network bugs:
  - NO CARRIER is handled;
  - keep-alive connections are checked;
  - the fetcher has timeouts;
  - the NetRxFill double wait is fixed;
  - `net.quit` no longer sticks;
  - the libnsutils clock overflow is patched.
- Nobody confirmed that a page rendered after those fixes. "Loading" may simply have turned into "nothing happens".
- PsiTerm and PsiMail use the same `ssh/psiglue.cpp` and connect fine on the device. The serial and PPP layer works for them.

## How the pieces fit

| Piece | Where | Role |
|---|---|---|
| PsiWeb.app (EIKON UI) | `web/app/psiweb.cpp` | Window, menus, toolbar. Copies `fb` to the screen when `frame_seq` changes. |
| psiweb.exe (engine) | `web/engine/pwepoc.cpp`, `web/fb/nsfb_epoc.c` | NetSurf framebuffer front end. Draws into the shared `fb`. |
| Fetcher | `web/engine/fetch_psi.c`, `pwnet.c` | HTTP over psiglue sockets. |
| Network | `ssh/psiglue.cpp` | Modem/PPP/serial. "Checking the modem…" comes from `Say()` at about lines 1609 (Psion Internet start) and 1790 (modem mode). |
| Shared chunk | `web/psiweb.h` (`PsiWebShared`) | `state`, `status[128]`, `busy`, `frame_seq`, `dirty_y0/y1`, and `fb` (4bpp, 16 greys, EGray16 layout). |
| Host harness | `web/emu/run_psiweb.py` | Runs the **exact ARM psiweb.exe** under unicorn on a PC. Python stands in for EPOC, the app, and psiglue (real TCP). Scripts can `open URL`, `wait`, and `shot FILE.png`. |

## Hypotheses, most likely first

1. **The app never repaints.** The engine draws into `fb` and bumps `frame_seq`, but PsiWeb.app misses it:
   - it doesn't poll after the connect stage;
   - its timer stops while `busy` is set;
   - or the `dirty_y0`/`dirty_y1` reset logic leaves an empty range.
   
   The status still saying "Checking the modem…" after the reply has arrived means **nothing written later reaches the screen**. That includes `pwb_set_status`.
2. **The status and UI path is stuck in psiglue.** `Say()` output may go to the status, and the connect step may never report that it finished. The app then stays in a "connecting" state and ignores updates. Check whether `ModemAnswersAt()` or the PPP start blocks the thread the app relies on, and what `state` reads.
3. **NetSurf fetches but never lays out or draws.** Possible reasons:
   - content-type sniffing fails;
   - libcss or libdom hit an error;
   - an allocation fails silently. Memory use is about 30–60× the HTML size, and the heap is small.
   - the scheduler or clock (libnsutils) never fires the reflow callback, so the redraw is scheduled but never runs.
4. **The `fb` format or stride doesn't match** what the app blits, so the page is drawn but appears blank. This is less likely: then nothing would draw, not even the status.

## Suggested plan

1. **Split engine from app with the host harness:**
   - Build psiweb.exe with `tools/docker/psibuild all`.
   - Run `web/emu/run_psiweb.py` with a script such as `open http://example.com/`, `wait 20000`, `shot /tmp/p.png`.
   - If the PNG shows the page, the engine is fine. The bug is then in PsiWeb.app's repaint, or in psiglue/state handling on EPOC (hypotheses 1 and 2).
   - If the PNG is blank, the bug is in NetSurf, the fetcher, or the scheduler (hypothesis 3). Add tracing in `fetch_psi.c` and `nsfb_epoc.c` (flush/update), and in the scheduler.
2. **Add a debug log** in both processes, for example `C:\System\Data\PsiWeb\psiweb.log` in the style of PsiMail's `psimail.log`. Log:
   - state changes;
   - status text;
   - fetch start, headers, bytes and done;
   - content type;
   - each `frame_seq` bump with its dirty range;
   - each app-side blit.
   
   Then one run in the emulator (and later on the device) shows exactly where it stops.
3. **Fix, then verify in the psionEmulators harness:**
   - load a simple page served locally (for example `python3 -m http.server`);
   - take a screenshot with `tools/emu/run.sh`;
   - only then ask Dan to try it on the 5mx.
4. Keep the pages tested small: plain HTML, no CSS or images first.

## Constraints

- Don't change connection settings or their defaults: PsiLink.ini, flow control, first-send, ATDT777.
- Follow the EIKON rules in `CLAUDE.md` and the `epoc-eikon-ui-guide` skill. Any new menu item counts toward the 8-item limit.
- Push to `dev` only. Version numbers have two digits (PsiWeb 0.58 = pkg 0,58,0). Sign with `tools/release/sign.py --product PsiWeb`. PsiWeb updates from the `dist/` branch.
