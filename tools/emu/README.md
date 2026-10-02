# Emulator testing

These scripts run the Psion apps in the
[psionEmulators](https://github.com/joehaines/psionEmulators) harness. They
expect that repository next to this one, at `../psionEmulators`, or wherever
`$PSION_EMU` points. Build its native harness once with
`../psionEmulators/harness/build.sh`.

```
# PsiMail with a seeded offline mailbox and calendar
python3 tools/emu/seed.py on && mail/build.sh && python3 tools/emu/seed.py off
node --experimental-strip-types tools/emu/mkcard.ts psimail --seed
tools/emu/run.sh inbox "88 4"                  # Esc the first-run question
tools/emu/run.sh reader "88 4" "92 17" "94 3" "96 3"
tools/emu/run.sh menus "88 4" "95 148" "97 15" # Menu, then the Edit menu

# PsiTerm (after build.sh, which copies the app into pkg/)
node --experimental-strip-types tools/emu/mkcard.ts psiterm
tools/emu/run.sh start
```

Notes:
- **TESTSEED must never be committed.** Rebuild with `seed.py off` before any release.
- Screenshots go to `build/emu/NAME/` (one per simulated second) and `end.png`.
- **Timing:** PsiMail's first window appears at about 78 s simulated time; PsiTerm's at about 45 s. Keys sent before that are queued or lost.
- **Keys:** Enter 3, Esc 4, Menu 148, arrows 14–17 (left, right, up, down), letters as ASCII capitals. Taps use `X ≈ LCD x + 45`.
- **Serial:** the harness can bridge the Psion's serial port to a Unix socket (`--serial-bridge-socket`). A fake Hayes modem such as `ssh/test/fakemodem.py`, or `firmware/atom-modem/hosttest/hostmodem`, can sit on it to reach local test servers.
- **Card writer quirks:** the FAT16 writer holds about 16 entries per folder. If a folder grows past one cluster, the card can come out damaged.
- `ESTLIB.DLL` is the EPOC C library, taken from the SDK's redistributable `stdlib.sis`. The `seed/` pictures are synthetic test images.
