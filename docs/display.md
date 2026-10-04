# Display: greys, dithering, Reading mode and fonts

Written 3 October 2026. This is about getting the most out of the 5mx's screen (640×240, 16 greys, a passive STN panel) in PsiTerm, PsiMail and PsiWeb, without touching the kernel or the LCD controller's palette (Dan's rule). It covers what each change does, the switches, how to calibrate, what was measured, and what was tried and left out.

## The switches

Every change can be turned off. All standards are as listed.

| Switch | Where | Standard | Stored in |
|---|---|---|---|
| Grey calibration on/off | Any of the three: Tools > Preferences > Screen greys, then A (*Calibration on/off*) | on | `C:\System\Data\PsiGrey.ini`, shared by all three programs |
| Pictures: error diffusion or ordered | the same screen (*Pictures: error diffusion / ordered*) | error diffusion | `PsiGrey.ini` |
| Reading mode: notches of contrast, light on | the same screen (*Reading mode: contrast +N*, *light on / light as set*) | +1, light on | `PsiGrey.ini` |
| Reading mode in PsiTerm | View > Reading mode (tick box) | off | `PsiTerm.ini`, a new byte at the end (v14, bit 0) |
| Reading mode in PsiWeb | View > Reading mode (tick box, Shift+Ctrl+R) | off | `PsiWeb.ini`, a new byte at the end (bit 0) |
| Reading mode in PsiMail | Tools > Preferences > Screen > Reading mode (*Off / While reading a message*) | off | `TPmSettings::iView` bit 20 (0x100000), a spare bit: the struct's size is unchanged |
| PsiWeb text: sharp or scaled | PsiWeb: Tools > Preferences > Text (*Sharp / Scaled (as before)*) | sharp | `PsiWeb.ini` byte, bit 1 |
| PsiTerm coloured text: dark or lighter greys | PsiTerm: Tools > Preferences > Coloured text | dark greys | `PsiTerm.ini` v14 byte, bit 1 |
| Direct screen access | not built: see below | off | – |
| Temporal "extra greys" | not built: see below | off | – |

With calibration off and the ordered dither chosen, PsiWeb draws exactly what it did before, to the pixel (checked in the ARM harness on the picture page, 68k.news and BBC with pictures). PsiMail with calibration off gives the same `.pmi` files as before, to the byte (checked with `imgtest`). With *Scaled* text, PsiWeb's text is the old text to the pixel.

`PsiWeb.ini` is now written to a temporary file and renamed over the old one, as `PsiTerm.ini` and `PsiLink.ini` already were.

## 1. Grey calibration

### The problem

Pictures, web pages and terminal colours arrive as 8-bit greys (0–255) and become one of 16 levels. Until now every program took level = v × 15 / 255, which assumes the 16 levels look evenly spaced. On a passive STN panel they don't. The LCD controller makes the greys by frame-rate control (each pixel is on for a fraction of the frames), and the liquid crystal's response to the resulting RMS voltage is an S-curve: flat near fully off and fully on, steep in between. So the steps next to black and next to white are small, and the middle steps are big. Dark greys in a photo fall into near-black, light greys into white, and the middle looks contrasty.

### The model

One table, `lv[16]`: how light each level *looks*, as an 8-bit grey (`ssh/psigrey.h`). From it:
- **Plain greys** (text and its anti-aliased edges, rules, backgrounds, terminal colours): each value takes the level whose `lv` is nearest.
- **Pictures**: error diffusion picks the nearest level and passes on `v − lv[level]`. The average lightness of an area is then kept as the screen really shows it, not as the level numbers say.

With `lv[k] = 17k` (calibration off) both are exactly the old behaviour. Black and white always stay black and white: `lv` is kept strictly increasing, so the ends always map to levels 0 and 15.

### The standard table

```
lv = 0 12 26 42 60 78 98 118 137 157 177 195 213 229 243 255
```

This is the linear ramp blended 35% towards a smoothstep (`x + 0.35·(x²(3−2x) − x)`), with gamma 1.0. It is a mild S-curve: it assumes level 1 looks like 12 rather than 17, and level 14 like 243 rather than 238. Input greys near black and near white are therefore pushed one level further in, so that they stay visibly different from black and white. The middle is left almost as it was.

It is deliberately mild. The real curve depends on the contrast setting and the temperature, and can't be measured from here. The Screen greys screen exists to fit it to Dan's actual panel. `tools/mkgrey.py` computes the table with the same formula as PsiTerm (and checks it against the header).

### PsiGrey.ini

The file is plain text, so the C engines (through ESTLIB) and the C++ programs read it the same way:

```
PsiGrey 1
calibrate 1
dither 1
gamma 100
curve 35
bright 0
nudge 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
reading 1 1
table 0 12 26 42 60 78 98 118 137 157 177 195 213 229 243 255
```

- It is written only by the Screen greys screen (in any of the three programs): to `PsiGrey.ini~`, then renamed over the old file.
- The readers need only `calibrate`, `dither` and `table`. The writer computes the table (it needs `pow`), so the engines need no floating point.
- If the file is missing, or a line doesn't parse, the standard is used. Every value is clamped and the table is made strictly increasing (`psigrey_parse`, `psigrey_fix`).

### Who reads it, and when

- **PsiMail's engine** reads it again for every picture it sets out (a few hundred bytes), so a saved change applies to the next picture. Pictures already set out keep their greys, because their source `.img` is deleted after decoding.
- **PsiWeb's engine** reads it when it starts and at each page load. A change is redrawn from the next page.
- **PsiTerm** reads it at start, and at once after its own Screen greys screen saves. (A change saved from PsiMail or PsiWeb reaches PsiTerm when it next starts.)

### Where it is applied

| Program | 8-bit grey becomes 4-bit in | How |
|---|---|---|
| PsiMail | `mail/engine/img/pmimg.c`, the shared sink of every decoder (JPEG, progressive JPEG, PNG, GIF) | `PmImgOpts.levels` / `.ordered`, set by `pictures.c` (`pic_greys`) |
| PsiWeb | `web/links/psi_grey.c` (the 565 → 16-grey frame conversion) and `web/links/psi_drv.c` (picture bitmaps) | the tables are built from the file; pictures are dithered once, when packed |
| PsiTerm | `GreyOf` (xterm-256 colours to greys) in `app/psiterm.cpp` | 8-bit luma → nearest calibrated level; calibration off = the old formula |

**Anti-aliased glyphs.** Links' text (and its new pre-drawn fonts) is anti-aliased. Its edge greys now go through the calibrated nearest-level table with no dither pattern, so edges keep their intended weight. PsiTerm and PsiMail draw with EPOC's 1-bit bitmap fonts (Terminus, Courier and the system fonts), so there is nothing to calibrate there.

### How to calibrate

**Where it is (0.84):** in every program, Tools > Preferences... has a **Screen greys...** button (Ctrl+G). It opens the full-screen greys screen on top of the Preferences dialog; when it closes, Preferences is still there. The screen is shared code, compiled into all three programs as `ssh/pglinktest.cpp` is: `ssh/psigreyui.cpp` and `.h` (`CPsiGreyScreen`, and `PsiGreyScreenL`, which runs it with the program's loop running and saves). Its test picture is `ssh/psigreypic.h`, drawn by `tools/mkcalpic.py`. After saving:
- PsiTerm redraws its colours at once ("Greys saved - PsiMail and PsiWeb use them from the next picture or page");
- PsiMail uses them from the next picture it sets out ("Greys saved - used from the next picture");
- PsiWeb from the next page ("Greys saved - used from the next page").

It used to be PsiTerm's Tools > Debug > Display calibration; it moved so that it isn't hidden among the developer tools, and so that every program has it.

**The simple way:** the screen opens on *Which looks best?*. A test picture and a line of coloured terminal text, drawn four ways side by side:

| Key | Version | Settings |
|---|---|---|
| 1 | Standard | gamma 1.00, curve 35 |
| 2 | Lighter shadows | gamma 1.60, curve 35 |
| 3 | Darker shadows | gamma 0.60, curve 35 |
| 4 | As before | calibration off |

Look at the doorway and the tree (dark detail), the clouds (near white) and the ball (smooth shading). 1–4 or Left/Right choose. **Up/Down fine-tune** the chosen version: each press is gamma ±0.10 (Up is lighter: gamma above 1 takes the levels to look darker than their numbers, so `psigrey_compute` maps every grey lighter), within 0.50–2.50. That version's picture and text are drawn again at once, and its label says so ("Standard, lighter +2", "Darker shadows, darker -1"; on two lines when it doesn't fit). *As before (off)* has no fine-tuning (an infoprint says so). **Enter** saves the chosen version with its fine-tuning for all three programs, **Esc** closes, and **A** opens the detailed settings below. The other settings (pictures' dither, Reading mode) are kept. When the screen opens, a saved setting that is one of these (the standard curve, no brightness or level nudges, a gamma a whole number of steps from a version's) shows as that version with its fine-tuning.

**The detailed way:**

1. On the Psion, set the screen contrast as you normally use it (Control panel), and switch the backlight to how you read.
2. In any of the three: Tools > Preferences > **Screen greys...**, then **A**. The screen shows:
   - the 16 levels as bars, each as it is;
   - three ramps, drawn with the settings as they stand:
     - pictures (error diffusion, or the ordered pattern if that is chosen);
     - plain greys (what text and colours get);
     - 16 patches of the inputs 0, 17, 34 … 255 drawn as pictures.
3. Aim for these, in order:
   - the picture ramp looks smooth from black to white, with no band that jumps;
   - the 16 patches step evenly;
   - in the plain ramp, the steps look about equal.
4. Keys:
   - **Tab** picks a setting.
   - **Up/Down** change it. On *Levels*, **Left/Right** pick a bar and **Up** makes the input greys near that level come out lighter.
   - **Gamma** (0.50–2.50) and **Curve** (0–100) shape the whole table. **Brightness** shifts it.
   - **Del** goes back to the standard (Reading mode is kept).
   - **Enter** saves for all three programs. **Esc** closes without saving.
5. Rules of thumb:
   - If the darkest patches merge into black, raise the levels near the dark end (Up on bars 1–3), or raise Curve.
   - If the lightest patches vanish into the white, lower the bars near 13–14 (Down), or raise Curve.
   - If everything looks too dark, raise Brightness or Gamma.
6. **The chart:** `docs/display-chart.png` (made by `tools/mkgrey.py --chart`) is a reference for a PC screen or paper. It shows the 16 levels as a linear screen would show them, the standard table's guess, a smooth ramp, the ramp in 16 levels by error diffusion, and the old plain rounding. On the Psion, the screen is right when its own ramp looks as smooth and even as the chart's row 3.

Help: PsiTerm's Help has a "Screen & greys" topic. PsiWeb has the same topic. PsiMail's "Pictures in messages" covers it.

## 2. Error diffusion for pictures

### Which one

At 16 greys I compared ordered (4×4 Bayer, the old PsiWeb way), Floyd–Steinberg (serpentine) and Atkinson. I used two photographs and a ramp at about 240×180, with both linear and standard levels (scratchpad `display/dither/cmp.py`):

| Image | Ordered | Floyd–Steinberg | Atkinson |
|---|---|---|---|
| 5mx photo, blurred error (what the eye sees at arm's length) | 1.13 | **0.75** | 1.10 |
| Series 7 photo | 1.06 | **0.70** | 0.95 |
| ramp | 0.94 | **0.76** | 1.45 |
| raw error (graininess), 5mx photo | 6.90 | 6.12 | 5.44 |

**Floyd–Steinberg, serpentine, was chosen.**
- It keeps tone best: about a third less visible error than ordered.
- Atkinson diffuses only 6/8 of the error. That is good at 1 bit (crisper, less "wormy"), but at 16 levels the lost quarter brings back the contour bands on smooth gradients: the ramp error is twice FS's.
- At 16 greys the grain is under one level step either way, so FS's slightly higher grain doesn't show.
- PsiMail already used FS. PsiWeb now does too.

### Where

- **PsiMail:** the sink in `pmimg.c` already did FS. It now quantises to the calibrated levels (`cal[v]`, error `v − lv[q]`). *Ordered* is now available there too (the same 4×4 pattern, between calibrated levels). Text in PsiMail is never dithered.
- **PsiWeb:**
  - **Before:** the whole 565 frame (text, rules and backgrounds included) went through a 4×4 ordered dither on every redraw.
  - **Now:** a picture's bitmap is dithered once, by FS, when Links packs it (`psi_drv.c` `fs_rows`, carried across strips while they follow on). It is stored as `lv[level]`, which the frame conversion maps back to that same level with no pattern. Everything else (text, its edges, table backgrounds, scroll bars) takes the nearest calibrated level: no dither texture on text or UI.
  - All 16 threshold tables in `psi_grey.c` are simply the same table, so the hot loop is unchanged. *Ordered* in the calibration screen brings back the old path exactly.

### Cost

**PsiWeb**, ARM harness, `NET=replay`, instructions at 15 MIPS:

| | Before | After (greys only) | After (greys + fonts, final) |
|---|---|---|---|
| start-up | 1.9M | 2.0M | 2.0M |
| local picture page | 26.0M | 30.3M | 28.4M |
| its Page Down | 19.1M | 22.6M | 22.6M |
| Show pictures | 6.1M + 23.0M | | 4.0M + 27.2M |
| BBC, all 117 pictures | 473.8M | | 473.2M |
| 68k.news (text) | 30.6M | 30.6M | 25.0M |

- The FS pass costs about 25 ARM instructions a pixel. That is two specialised loops with word-sized error buffers: the ARM710 has no halfword loads, and a first version took about 40.
- It runs once per decoded picture. On BBC it is lost in the noise.
- Heap peaks are unchanged on every page (the error rows are 2 × (width + 2) words, kept between pictures).
- There were no failed allocations.

**PsiMail:**
- `imgtest.py` passes: no crashes, no sanitizer reports and no leaks over 300 fuzz rounds on every sample. So does `parsefuzz.py`.
- The quantiser now does two table loads per pixel in place of a multiply and a divide-by-255. PC timings are unchanged within the timer (2–3 ms per sample picture).
- Building the level tables costs 256 small divisions per picture.
- With linear levels, the output is byte-identical to the old decoder.

### Screenshots

- `scratchpad/display/shots/final-images-1.png` and `final-images-2.png` (before on top, after below): the flat table background and scroll bar; the sky and the GIF without the 4×4 pattern.
- `dither-ordered-fs-atkinson.png`: the three methods side by side.
- `psimail-decoder-old-new-ordered.png`: PsiMail's decoder before, after, and ordered.
- `psimail-photo-after.png`: the photo set out by the ARM engine in the 5mx emulator.

## 3. Reading mode and contrast

While a page, a message or the terminal is being read:
- the contrast goes up by the number of notches set (standard +1, up to +4; 0 leaves it alone);
- the backlight is switched on and set untimed, if *light on* is chosen.

`ssh/psidisp.h` (`TPsiReading`) does it for all three programs, through `UserHal`.

**When it is on.**
- PsiTerm and PsiWeb: while the program is in front with the tick on.
- PsiMail: while a message is open in the reader, PsiMail is in front, and the preference is on.

**When it goes off.** Everything comes back when the program goes to the background, when it closes, at switch-on (that is, after a switch-off), when the tick or preference is turned off, and in PsiMail when you go back to the list. After a switch-on the next key or tap turns it on again, not the switch-on itself, because an alarm may have woken the Psion with nobody reading.

**Putting things back safely.**
- **Only what it changed:** if the contrast is no longer the value Reading mode set (you changed it meanwhile), your choice stays. The backlight behaviour is restored only if it is still untimed.
- **Crash path:** the user's own settings are written to `C:\System\Data\PsiRead.ini` before anything is changed, and the file is deleted when they are put back. If a program dies in Reading mode (a panic, a kill from the task list, a battery change), the next of the three programs to start finds the file, sees that its owner isn't running (`TApaTaskList`), and restores the settings.
- **Hand-over:** when one program goes to the background as another comes forward, in either order, the file keeps the *original* settings from the first. Whoever owns the file last restores them, so a raised contrast is never taken as the user's own.
- **Engines:** they don't touch the display.

**Contrast nudges** are the notch count. I didn't add separate "contrast up/down" commands:
- the Psion's own contrast control already does that;
- the View menus are full (PsiMail's View has 8 items) and PsiTerm has no free Shift+Ctrl letter left;
- a nudge that the program puts back on exit is exactly what Reading mode is.

**Menus and keys (EIKON guide).**
- PsiTerm: View has 8 items with Reading mode. It has no shortcut: all 26 Shift+Ctrl letters are taken by menus or snippet keys.
- PsiWeb: View has 7 items, with Shift+Ctrl+R (a free shifted letter).
- PsiMail: a Preferences line, because View and Tools are both at 8.
- The greys screen is a button in each program's Preferences dialog (0.84; it was the 8th item in PsiTerm's Tools > Debug cascade).

**Tested in the 5mx emulator:** the menus open and the tick, the infoprint ("Reading mode on") and the preferences lines all work, with no panics. The emulator does not show contrast or the backlight, so the visible effect needs the device.

## 4. Fonts for 4-bit grey (PsiWeb)

**The problem.** Links scaled 40 px masters of a serif face down to each size and sharpened them, so stems fell between pixels and showed as grey smudges, and descenders ran into link underlines.

**The fix.** `web/links/mkstrike.py` now pre-renders DejaVu Sans, Sans Bold and Sans Mono with FreeType's TrueType hinting, in the build container, at the cell heights PsiWeb uses at 100% zoom: 12, 13, 14, 16, 19, 21 and 24 px, measured on the page set.
- Body text is normal 14, headings are bold 16–24, and `<pre>` is mono 13–14.
- The coverage is corrected for Links' linear-light mixing, so the text keeps its weight.
- dip.c uses these glyphs as they are, with no scaling and no sharpening.
- Other zooms, and rare characters, fall back to the scaled masters. A page never mixes the two: at any zoom other than 100% it all uses the masters (`psi_set_font_base`).
- List items get a real bullet (•) rather than `*`.

**Why sans.** At about 12 px to the em, serifs become grey blobs. Sans is also close to the Psion's own UI.

**Results.**
- Text is crisper.
- Pages are 5–40% cheaper to draw:

| Page | Before | After |
|---|---|---|
| info.cern.ch | 4.1M | 2.4M |
| 68k.news | 30.6M | 25.0M |
| 68k.news, Page Down | 4.2M | 2.5M |
| NPR | 15.0M | 12.4M |
| Wikipedia | 32.1M | 30.8M |
| BBC | 78.9M | 76.8M |

- Heap peaks are unchanged.
- The cost is 201 KB of EXE (`psiweb.exe` goes from 1.91 MB to 2.12 MB), which is RAM, since the EXE loads into RAM. The scaled masters stay in the EXE for the switch and for other zooms.

**Switch and licence.**
- *Text: Scaled (as before)* sets `psi_hinted_fonts = 0` at engine start (`PW_DISPLAY_SCALED_TEXT` in the shared chunk). All ten screenshots of the page set then match the old build exactly.
- DejaVu's licence (Bitstream Vera) is now added to PsiWeb's `COPYING.txt` by `web/build.sh`, and listed in `THIRD-PARTY.md`.

**Screenshots:** `shots/compare-npr.png`, `compare-68k.png`, `compare-wikipedia.png` (old above, new below).

## 5. Contrast-aware colours

Each program's text and UI colours were checked:

**PsiTerm.**
- Coloured text on the default (white) background was folded into levels up to 9 (cyan text came out level 9: mid-light grey on white).
- Now (*Coloured text: Dark greys*, the standard) such text keeps its colour's order but stays in levels 0–6, so it never goes mid-grey on the white.
- *Lighter greys* gives the old mapping.
- The existing "never less than 6 levels from the background" rule is kept.
- The status line (white on level 4) and the themes are unchanged. *Soft* is a deliberate low-contrast choice.

**PsiWeb.**
- Body text is black.
- Links are underlined in Links' default blue. That is nearest level 2 (it looks like 26 out of 255), near black.
- Page-set colours (`<font color>`) are the page's own: Links can't tell de-emphasis from decoration, and forcing them would break pages. They are left as they are.
- The grey scroll bar and coloured table cells are now flat greys rather than dither patterns (item 2).

**PsiMail.**
- Body and list text is black on white.
- Grey (85) is used only for secondary text (attachment sizes, a picture's "not shown" reason) and lines.
- No change was needed.

## 6. Direct screen access: measured, not built

**The measurement.** I measured it with PsiTerm's calibration screen: **T** there (a developer tool, removed in 0.84 when the screen became shared). It times 10 full 640×240 frames each way, then shows the directly written rows for 2 s and says where the screen is. Results in the 5mx emulator (emulated time):

| Path | 10 frames | Per frame |
|---|---|---|
| Window server (PsiWeb's way: `SetScanLine` × 240, then `BitBlt` to the window) | 359 ms | 36 ms |
| Direct copy into the screen memory | 31 ms | 3 ms |

**What the test showed about the screen.**
- `UserSvr::ScreenInfo` returns a valid address, `0x58003020`: past the frame buffer's 32-byte palette header.
- The layout is exactly EGray16's: 320 bytes a row, the left pixel in the low nibble, 0 black. The direct rows landed in place (`shots/psiterm-dsa-timing.png`).

So the gain is real: about 33 ms per full frame, roughly 10–25% of a Page Down. **It is not built**, because it cannot be made safe on ER5:
1. **Infoprints and busy messages can't be detected.**
   - EIKON draws them as windows of the program's *own* window group. The group's ordinal position stays 0 with an infoprint and with a busy message up (tested in the emulator).
   - ER5 has no visible-region or "covered" query, and `CEikonEnv` doesn't say when an infoprint is showing.
   - Direct writes would draw over them, and they wouldn't be redrawn.
2. **Frames come exactly when those are up.**
   - PsiWeb draws while a page loads (busy message bottom left) and right after commands (infoprints top right).
   - A guard that skipped those moments would leave almost no frames to speed up.
3. **The window server's copy goes stale.** The page is a backed-up window. After direct writes, a menu or dialog closing would restore old content until the next full redraw.
4. **It isn't a published interface.** The screen address is a kernel mapping that user code happens to be able to write on the 5mx. Other ER5 machines (Revo, netBook) and a future ROM may differ.

PsiWeb therefore keeps the window-server path. (The **T** tool was removed in 0.84; `app/ptgrey.cpp` in git history has it, should the real 5mx's figure be wanted.)

## 7. Temporal "extra greys": evaluated, not built

The idea is to alternate a pixel between two adjacent levels on successive redraws, to show a grey between them. It was rejected without code:
- **The controller already does this.** The 16 greys are themselves frame-rate modulation of the panel. A software alternation at the window server's rate would beat against the panel's refresh (about 70 Hz) and show as flicker or crawl, not as a new grey. A full frame costs 36 ms through the window server (measured above), which is at most about 25 Hz with the CPU fully busy.
- **The panel would smear it.** A passive STN panel responds in roughly 150–300 ms, so at best the alternation blurs into a blend. Spatial error diffusion already gives that blend for free, without motion.
- **It costs while you look.** It needs a constant redraw while a still picture is on the screen, which costs CPU and battery and keeps the engine from sleeping.

## Testing done

- **Builds:**
  - `tools/docker/psibuild all`: all three `.sis` build;
  - `make -f web/links/epoc.mk emu`;
  - `web/links/build_host.sh` (the PC build).
- **PsiMail host tests:** foldertest, undotest, invtest, invitehost, htmltest, imgtest (300 rounds) and parsefuzz (3000 rounds) all pass.
- **PsiMail decoder:** with linear levels, the output is identical to the old decoder.
- **Connection test:** the ssh linktest passes (49 checks, 0 failed).
- **ARM harness** (`NET=replay web/links/emu/run_pages.sh`):
  - every page passes, with no failed allocations;
  - with calibration off and ordered chosen, the picture page, 68k.news and BBC with pictures are identical to the old build;
  - a non-standard `PsiGrey.ini` (`PW_CDISK=… run_links.py`, new: the C: folder the harness uses) changes the output as expected (`shots/calib-variant.png`).
- **5mx emulator, each program starts with no panic:**
  - PsiTerm: View (8 items, Reading mode tick), Tools > Debug (8 items), the calibration screen (keys, save, infoprint), Preferences (the Coloured text line), the **T** timing.
  - PsiWeb: View (Reading mode, Shift+Ctrl+R), Preferences (the Text line fits), the welcome page with sharp text.
  - PsiMail (seeded, then the seed removed and rebuilt): the reader with a JPEG set out by the ARM engine with the calibration.
- **`net.py all`:** passes (at, web, mail, ssh; ssh on its retry).
  - The working tree held someone else's unfinished `firmware/atom-modem` changes, which do not link (`proxy.cpp`).
  - So the modem was built from HEAD's firmware in `build/display-fw` and `net.py` was pointed at it (a copy in `build/emu/net-mine.py`).
  - Their files were not touched.

## For Dan to check on the 5mx

1. **Calibration:** Tools > Preferences > Screen greys (in any of the three). Fine-tune with Up/Down, or press A and adjust until the picture ramp is smooth and the 16 patches step evenly. Save. Then compare a PsiMail picture and a PsiWeb page with Show pictures.
2. **The extremes:** press Del (standard), then turn *Calibration off* and choose *Pictures: ordered*. The old look should come back.
3. **Reading mode:**
   - Turn it on in each program and check the contrast and the light.
   - Switch to another program and back, switch off and on, and close the program. The contrast and backlight should always come back.
   - Change the contrast by hand while in Reading mode: yours should stay.
4. **Fonts:** PsiWeb > Preferences > Text: Sharp against Scaled, on 68k.news and Wikipedia. Zoom in and out (other sizes use the scaled fonts).

## Files

**New:**
- `ssh/psigrey.h`: the calibration's model, file and parser.
- `ssh/psidisp.h`: Reading mode and the PsiGrey.ini load and save, for the apps.
- `ssh/psigreyui.cpp`, `.h`: the Screen greys screen, shared by all three (0.84; before that `app/ptgrey.cpp`, with the T timing).
- `ssh/psigreypic.h`: its test picture (before 0.84 `app/ptcalpic.h`).
- `tools/mkgrey.py`: the standard table and the chart.
- `docs/display-chart.png`.
- `web/links/mkstrike.py`: the pre-drawn fonts.

**Changed:**
- PsiTerm: `app/psiterm.cpp`, `.h`, `.hrh`, `.rss`, `.mmp`, `pthelp.cpp`.
- PsiMail: `mail/engine/img/pmimg.c`, `.h`, `mail/engine/pictures.c`, `mail/test/imgtest.c`, `mail/app/pmapp.h`, `psimail.cpp`, `.hrh`, `.rss`, `pmhelp.cpp`.
- PsiWeb app and shared chunk: `web/app/psiweb.cpp`, `pwapp.h`, `.hrh`, `.rss`, `pwhelp.cpp`, `web/psiweb.h`.
- PsiWeb engine and build: `web/links/psi_grey.c`, `psi_drv.c`, `epoc/psi_os.c`, `epoc.mk`, `Makefile`, `emu/run_links.py`, `patches/links-2.30-psion.diff` (dip.c, links.h, html.c: fonts), `web/build.sh`.
- `THIRD-PARTY.md`.
