# PsiKern: an experimental kernel driver for the 5mx

**Emulator only.** This is not part of PsiTerm, PsiMail or PsiWeb. It is never put in a `.sis`, never released and never loaded by the apps. It is item 2 of the shortlist in [`docs/experimental-hacks.md`](../../docs/experimental-hacks.md): can we run our own code in the EPOC R5 kernel, using only the 2002 SDK?

The answer is yes. In the emulator, with the real 5mx ROM 1.05(260), the driver loads, opens a channel, runs in a privileged CPU mode, reads hardware registers, and changes the LCD palette and puts it back.

## What is here

| File | What it is |
|---|---|
| `ldd/psikern.cpp` | `PSIKERN.LDD`, the driver. Everything goes through `DoControl`. |
| `ldd/kern.h` | The kernel classes `DLogicalDevice` and `DLogicalChannel`, rebuilt by hand, because the SDK has no kernel headers. |
| `psikern.h` | `RPsiKern`, the user-side handle, and the register offsets. |
| `ekern_rom.def` | The EKERN exports the driver needs, with the **5mx ROM's** ordinals. |
| `test/psikt.*` | `PSIKT.APP` (PsiKernTest), a bare EIKON app that runs the steps and shows each one on screen. |
| `build.sh` | Builds both into `build/kernel-pkg/`. |

## What the driver does

All requests go through `DoControl`, and every answer is its return value, so the driver never touches user memory.

- **Version and CPU mode.** Version returns the magic number `0x504B` and 0.1. CPU mode returns the CPSR mode bits: in the emulator it is `0x1b` (UND), a privileged mode, not user mode (`0x10`).
- **Register reads.** It reads a fixed list of Windermere registers at virtual `0x58000000` and up: memory, LCD, power, interrupt status, UART2, timers, RTC and port data. Only registers without read side effects are on the list: no data, FIFO or end-of-interrupt registers. Any other offset gets `KErrArgument`.
- **One write: invert the palette.** It inverts the 16 grey levels in the LCD palette. The palette is the 32 bytes of RAM just before the frame buffer, and the LCD controller reads it every frame. The driver saves the bytes first. It puts them back on request and when the channel closes. This writes RAM only, never a register.

The test app runs the invert only if `\PSIKERN.WR` exists on C: or D:.

## Results in the emulator (3 October 2026)

```
LoadLogicalDevice: 0      Open channel: 0
Version: 504b0001         CPU mode in driver: 1b (UND, privileged)
PWRCNT = 00000004         PWRSR = 00000096
LCDCTL = 00000003         MEMCFG1 = 00921010   DRAMCFG = 00000081
UART2 CON = 01  FLG = 10  TC1 VALUE = 0000013f RTC LOW = 0000a4ec
Palette inverted: 0 (3 s) Palette restored: 0  Channel closed. Done.
```

The screen turned white on black for 3 s, then went back to normal.

Some registers read `0` or `ffffffff` (`LCDST`, `LCDDBAR1`, `LCDT0`–`2`, `UART2 LCR`). The emulator does not model them; on a real 5mx they should hold real values. Expect other differences on the device too: the emulator does not model `UBRCR`, for example.

## How the kernel contract was recovered

The SDK ships `ekern.lib`, but it is built for the **Series 5** kernel, `EKERN[100000b9].EXE`. The 5mx ROM's kernel is `EKERN[100000ba].EXE`, and its export ordinals are different. A driver linked against the SDK's library loads but calls the wrong functions.

- **Ordinals.** The ordinals in `ekern_rom.def` were read from the export table of the kernel in the ROM image. `build.sh` turns them into `psiekern.lib` with `dlltool`. (No ROM bytes are in this repo.) EUSER's ordinals match the SDK, so its `euser.lib` is used as it is.
- **Class layouts.** The SDK has no kernel headers. `DLogicalDevice` (0x28 bytes) and `DLogicalChannel` were rebuilt from the ROM's own `Video.ldd`, which derives from both, from the EKERN functions it imports, and from the mangled names in the SDK's `ekern.lib`. `kern.h` says which facts are proven and which are not. The fields we use are `iVersion`, `iParseMask`, `iUnitsMask` and `iDevice`.
- **Checking the build.** The import table of `psikern.ldd` should show imports from `EKERN[100000ba]` at ordinals 63, 141, 302, 321, 339, 557, 558, 565 and 566, and nothing else from EKERN. (`ekern_rom.def` also lists 99 and 101, `DoControl` and `DoCreateL` of `DLogicalChannel`; the current driver does not import them.)

## Building and running

```
tools/docker/psibuild "experimental/kernel/build.sh"
node --experimental-strip-types tools/emu/mkcard.ts psikern build/emu/kcard.img
EMU_CARD=build/emu/kcard.img EMU_SECS=70 tools/emu/run.sh kern "46 tap 335 215"
```

The tap at 46 s starts PSIKT from the Extras bar. Its lines appear from about 47 s and finish by 48 s, in `build/emu/kern/s-04*.png`.

`build.sh` links `ldd/` and `test/` into the SDK's `ptproj/` as `psikernldd` and `psikerntest`. The `.marm` makefiles it generates there are not committed.

## Risks on a real 5mx

Don't do this unless Dan decides to. If he does:

- **A fault in the driver takes the whole machine down.** Nothing in the kernel catches it.
- **C: is a RAM disk.** A hard reset can lose it, so back up C: first.
- **A driver can't be unloaded** once it is loaded. To remove it, restart the machine.
- **Never write registers.** The driver writes only the palette, which is RAM. Writing power, clock, memory or UART registers could hang the machine or corrupt C:, and the emulator can't show what they do to real hardware.
- **The ROM version must be 1.05(260).** On any other ROM, the ordinals in `ekern_rom.def` must be checked again first.
