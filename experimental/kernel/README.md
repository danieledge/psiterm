# PsiKern: an experimental kernel driver for the 5mx

**Experimental.** This is not part of PsiTerm, PsiMail or PsiWeb, and is never released or loaded by the apps. It is item 2 of the shortlist in [`docs/experimental-hacks.md`](../../docs/experimental-hacks.md): can we run our own code in the EPOC R5 kernel, using only the 2002 SDK?

The answer is yes. In the emulator, with the real 5mx ROM 1.05(260), the driver loads, opens a channel and runs in a privileged CPU mode. It reads hardware registers, and changes the LCD palette and puts it back. **PsiKernTest** is the harness that tries the same on a real 5mx.

## What is here

| File | What it is |
|---|---|
| `ldd/psikern.cpp` | `PSIKERN.LDD`, the driver. Everything goes through `DoControl`. |
| `ldd/kern.h` | The kernel classes `DLogicalDevice` and `DLogicalChannel`, rebuilt by hand, because the SDK has no kernel headers. |
| `psikern.h` | `RPsiKern`, the user-side handle, and the register offsets. |
| `ekern_rom.def` | The EKERN exports the driver needs, with the **5mx ROM's** ordinals. |
| `test/psikt.*` | PsiKernTest (`PSIKT.APP`), the test harness. |
| `psikt.pkg` | Its installer. |
| `build.sh` | Builds all of it into `build/kernel-pkg/`, including `PsiKernTest.sis`. |

## What the driver does

All requests go through `DoControl`, and every answer is its return value, so the driver never touches user memory.

- **Version and CPU mode.** Version returns the magic number `0x504B` and 0.2. CPU mode returns the CPSR mode bits.
- **Register reads.** It reads a fixed list of Windermere registers at virtual `0x58000000` and up: memory, LCD, power, interrupt status, UART2, timers, RTC and port data. Only registers without read side effects are on the list: no data, FIFO or end-of-interrupt registers. Any other offset gets `KErrArgument`.
- **Palette reads.** It reads the 16 entries of the LCD palette, which is the 32 bytes of RAM just before the frame buffer. The LCD controller reads it every frame.
- **(0.3) One register write,** to one register and from a short list of values. Each is saved first and put back by `ERestore` and when the channel closes:
  - `EUbrcrSet`: UART2's divider, only 1 (230400) or 3 (115200), and only while the UART is on.
  - (0.6) The ROM wait-state probe of 0.3–0.5 has been removed: on the 5mx it reset the machine.
- **Palette writes.** It inverts the palette or sets single entries. Before the first write it checks that the 32 bytes look like a palette, and refuses if they don't:
  - only the level bits are set, plus the bits-per-pixel code in entry 0;
  - the levels of the entries in use run one way only.

  It keeps a copy of the palette and puts it back on request and when the channel closes. It writes RAM only, never a register.

## PsiKernTest, the harness

Nothing runs by itself. Each test is a menu command.

| Menu | Test | What it does |
|---|---|---|
| Read | All read tests | The four below, in turn. |
| Read | Machine and driver | Shows the machine, ROM, processor clock, display and RAM. Loads the driver, then shows its version and the CPU mode. |
| Read | Registers | Reads every register on the list, then again a second later, and shows what changed. |
| Read | Palette | Shows the screen address and the palette, and whether the palette passes the check. |
| Read | CPU and memory speed | Times a loop and a memory copy. Doesn't use the driver. |
| Read | Serial port speeds | (0.3) Opens COMM::0 at 9600, 57600 and 115200 and reads UART2's divider (`UBRCR`) each time: 47, 7 and 3 are expected. |
| Write | Invert the screen (3 s) | Asks first, then inverts the screen and puts it back. |
| Write | Grey curves (35 s) | Asks first, then shows a 16-grey ramp under six palettes for 5 s each, and puts the palette back. |
| Write | Keep a grey curve | (0.3) Keys 1–6 choose a curve, 0 gives EPOC's own back, and Enter keeps it on while PsiKernTest stays open, in front or not. Every 2 s it checks whether EPOC has set its own palette again (a 4- or 16-grey screen, contrast, switch-on), puts the curve back if so, and logs it. |
| Write | 230400 with the Atom modem | (0.3) Asks first. It sends `AT$SB=230400` at 115200, sets `UBRCR` to 1, then sends `AT` and ten `ATI`s at 230400 and checks the answers are clean. Then it goes back to 115200 at both ends. If the Atom doesn't answer, it waits for the Atom's 15 s fallback. |

**Built-in safety**

- **ROM check.** The driver is loaded only on a 5mx with ROM 1.05(260), checked with `UserHal::MachineInfo`, because its kernel imports are by ordinal.
- **The log.** It is `PsiKern.log` on D: (the CF card) if there is one, otherwise C:. Before each step a line `> step` is written and flushed to the disk, and after it `< step: result`. If a step takes the machine down, the next start says which step it was, and asks before running it again. The previous run's log is kept as `PsiKern.old`.
- **Write tests ask first,** and run only if the palette check passes.

### On the 5mx

1. Back up C: (PsiWin, or copy it to the CF card).
2. Install `build/kernel-pkg/PsiKernTest.sis`, preferably to D:.
3. Open PsiKernTest from Extras and run Read > All read tests. Send back `D:\PsiKern.log`.
4. Only if the palette check passes: Write > Invert the screen, then Write > Grey curves.
5. To remove it: restart the Psion (the driver stays loaded until then), then remove PsiKernTest with Control panel > Add/remove.

## Results in the emulator (3 October 2026)

**Machine and driver**

```
Machine: SERIES5 MX, ROM 1.05(260)
Processor: ARM 710T, 36864 kHz, speed factor 2000
Display 640x240, 16 colours;  RAM 16384 KB;  ROM 17408 KB
Load driver: 0   Open channel: 0   Driver version 0.2
CPU mode in the driver: 1b (UND)
```

**Registers and the screen**
- Registers:
  - `MEMCFG1 = 00921010`, `DRAMCFG = 00000081`, `PWRCNT = 00000004`.
  - From one read to the next a second later, `PWRSR`, `TC2 VALUE` and `RTC LOW` change.
- Screen: the frame buffer is at virtual `58003020`, so the palette is at `58003000`.
- Not modelled by the emulator: `LCDST`, `LCDDBAR1`, `LCDT0`–`2` and `UART2 UBRCR` (the serial speed, 0x708) read `0` or `ffffffff`. On a real 5mx they should hold real values.

**The palette**
- **The System screen uses 4 greys.** Its palette is `100f 000a 0005 0000`, then zeros: bits-per-pixel code 1, with 4 entries in use.
- **16-grey windows** (PsiTerm, PsiMail, this harness) use `200e 000d 000c … 0008 0007 0007 0006 … 0000`.
  - EPOC's palette leaves out one of the 16 hardware levels and uses level 7 twice.
  - On this hardware the high levels are dark: black is level 14, and level 15 is never used.
  - With all 16 levels, black gets darker and greys 7 and 8 become different. The "All 16 levels" curve shows this in the emulator.
  - Whether the 5mx's panel shows the difference is one of the questions for the device.
- **Invert and every grey curve worked,** and the palette was put back each time.

**Timing**
- The tick is 15625 µs (64 a second), as the HAL says. The apps' "64 ticks a second" holds.
- The emulator's own timing is not real time: it counted about 300 ticks in a 1 s wait. Its loop and memory speeds mean nothing, so only the 5mx can give real figures.

## How the kernel contract was recovered

The SDK ships `ekern.lib`, but it is built for the **Series 5** kernel, `EKERN[100000b9].EXE`. The 5mx ROM's kernel is `EKERN[100000ba].EXE`, and its export ordinals are different. A driver linked against the SDK's library loads but calls the wrong functions.

- **Ordinals.** The ordinals in `ekern_rom.def` were read from the export table of the kernel in the ROM image. `build.sh` turns them into `psiekern.lib` with `dlltool`. (No ROM bytes are in this repo.) EUSER's ordinals match the SDK, so its `euser.lib` is used as it is.
- **Class layouts.** The SDK has no kernel headers. `DLogicalDevice` (0x28 bytes) and `DLogicalChannel` were rebuilt from:
  - the ROM's own `Video.ldd`, which derives from both;
  - the EKERN functions it imports;
  - the mangled names in the SDK's `ekern.lib`.

  `kern.h` says which facts are proven and which are not. The fields we use are `iVersion`, `iParseMask`, `iUnitsMask` and `iDevice`.
- **Checking the build.** The import table of `psikern.ldd` should show imports from `EKERN[100000ba]` at ordinals 63, 141, 302, 321, 339, 557, 558, 565 and 566, and nothing else from EKERN. `ekern_rom.def` also lists 99 and 101 (`DoControl` and `DoCreateL` of `DLogicalChannel`); the current driver doesn't import them.

## Building and running in the emulator

```
tools/docker/psibuild "experimental/kernel/build.sh"
node --experimental-strip-types tools/emu/mkcard.ts psikern build/emu/kcard.img
EMU_CARD=build/emu/kcard.img EMU_SECS=130 tools/emu/run.sh kern \
    "46 tap 335 215" "54 148" "55 15" "56 3"
```

- **Starting it:** the tap at 46 s starts PsiKernTest from the Extras bar.
- **Running a test:** Menu (148), then Right (15) to reach Read, and Enter (3) for All read tests.
- **The menu bar reopens on the last pane used,** so later key sequences have to allow for that.

`build.sh` links `ldd/` and `test/` into the SDK's `ptproj/` as `psikernldd` and `psikerntest`. The `.marm` makefiles it generates there are not committed. `PsiKernTest.sis` goes to `build/kernel-pkg/` only, never to `dist/`.

## Risks on a real 5mx

- **A fault in the driver takes the whole machine down.** Nothing in the kernel catches it.
- **C: is a RAM disk.** A hard reset can lose it, so back up C: first. The log goes on D: so that it survives.
- **A driver can't be unloaded** once it is loaded. To remove it, restart the machine.
- **Never write registers.** The driver writes only the palette, which is RAM, and only after the palette check. Writing power, clock, memory or UART registers could hang the machine or corrupt C:, and the emulator can't show what they do to real hardware.
- **The ROM version must be 1.05(260).** The harness checks it. For any other ROM, the ordinals in `ekern_rom.def` must be read again first.
