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
- **Serial:** `EMU_SERIAL=SOCKET tools/emu/run.sh ...` bridges the Psion's serial port to a Unix socket that a modem is listening on (see *Network tests* below).
- **Card layout can wedge loading:** the same build can stall at "Starting the browser engine..." or end with KERN-EXEC 3 on one card image and run cleanly on another with the files shifted (PAD.BIN size). Seen with PsiWeb on 2 Oct 2026: 3 of 11 runs failed on one layout, 0 of 10 with PAD.BIN at 9000 bytes. Try another layout before suspecting the code.
- **Card writer quirks:** the FAT16 writer holds about 16 entries per folder. If a folder grows past one cluster, the card can come out damaged.
- `ESTLIB.DLL` is the EPOC C library, taken from the SDK's redistributable `stdlib.sis`. The `seed/` pictures are synthetic test images.

## Network tests

`tools/emu/net.py` runs the apps' real network code end to end in the
emulator: PsiTerm, PsiMail and PsiWeb dial a modem on the serial port and reach
local test servers through it. It needs g++, python3 with Pillow, node and
`/usr/sbin/sshd`, and the apps built as usual (`pkg/`, `build/mail-pkg/`,
`build/web-pkg/`; no TESTSEED needed).

```
python3 tools/emu/net.py            # all four, about 3 minutes
python3 tools/emu/net.py web mail   # or some of: at web mail ssh
```

| Test | What happens | Passes when |
|---|---|---|
| `at` | PsiTerm's terminal types `ati` and `at` | the modem answered (its log) |
| `web` | PsiWeb: Ctrl+O, `127.0.0.1`, Enter | `nethttp.py` served `GET /` |
| `mail` | PsiMail: a new account (port 143, Security None, a password), which checks at once | `fakeimap.py` saw LOGIN and FETCH |
| `ssh` | PsiTerm: Tools > SSH keys > Import `D:\PSIKEY`, SSH to a new server, accept the host key, run `echo` | the sshd accepted the key |

Screenshots are in `build/emu/net/<test>/` (`end.png` is the last frame), with
the modem's log (`<test>-modem.log`) and the servers' logs (`http.log`,
`imap.log`, `sshd.log`) next to them. `EMU_NET_DIR` moves all of this;
`EMU_PKG_TERM`, `EMU_PKG_MAIL` and `EMU_PKG_WEB` point at other app files (for
example a copy, while someone else is rebuilding). A failed test is run once
more (`EMU_RETRIES`), because of the card wedge described above.

How it works:
- The modem is `firmware/atom-modem/hosttest/hostmodem` (built with `make`),
  listening on a socket in a fresh `/tmp/psiemu-*` folder (a socket path must
  be under 108 bytes). Its `HOSTMODEM_REDIRECT=127.0.0.1:PORT` sends every
  dial to the test's server, so the addresses typed on the Psion are short.
- The servers listen on 127.0.0.1 only. `nethttp.py` serves two pages from
  memory. The sshd runs as you, on a free high port, with its own config, host
  key and `authorized_keys` in `build/emu/net/sshd/`, key login only, and a
  forced command (a plain `sh` in that folder); it never reads `~/.ssh`. The
  throwaway keys and `ssh.img` (which holds the private key) are deleted at the
  end unless you give `--keep`.
- `net.py` stops only the processes it started, by PID.
- `Script` in `net.py` turns text into key events, with the 5mx's modifiers.
  Checked against this ROM: `@` is Fn+4 and `~` is Shift+' (the emulator's
  own `keymap.ts` has them the other way round), `:` is Fn+', `\` Fn+3,
  `/` Shift+, (comma), `-` Fn+O. Dialog buttons such as Import are Ctrl+letter.
- The card writer only makes 8.3 names, which is why the key is `D:\PSIKEY`
  and not the `D:\id_ed25519` that PsiTerm offers.

### Why COMM::0 would not open, and the fix

- On the 5mx, COMM::0 (the RS-232 port) is the Windermere's **UART2**. UART1
  isn't the cable port (probably the infrared one), which is why
  `--serial-attach 1` saw the open hang and then fail.
- The ROM boots with the **Remote link on** (Cable, 115200), and the link
  holds COMM::0. The apps got -21 (KErrAccessDenied), and the "garbage" on
  UART2 was the link's PLP frames (`16 10 02 21 10 03 34 43`, repeated).
- The fix needs no emulator change. With `EMU_SERIAL` set, `run.sh` switches
  the link off the way a user would: at 30 s on the System screen it presses
  Ctrl+L, Left (Link: Cable becomes Off) and Enter. It then attaches the bridge
  to UART2 at 34 s (`EMU_ATTACH`), so the modem never hears the link's frames.
- It uses the harness's ordinary boot-then-card mode, so the card and the
  bridge work together. `--serial-poll-until` skips the card, so don't use it.
- The harness reports CTS, DSR and DCD high whenever the bridge is attached,
  and it doesn't model RTS. So these tests can't cover DCD dropping or flow
  control. A hang-up is seen only as an in-band `NO CARRIER`.
- The harness reads up to 512 bytes per frame from the socket and drops
  anything that doesn't fit the UART's 4 KB receive FIFO. The modem's pacing
  (5500 bytes/s of wall-clock time, while the emulator runs at about 2.5 times
  real time) keeps well below that, but a modem without pacing could lose data.

**Psion Internet (PPP)** isn't covered. The far end would have to be a PPP
peer (pppd), which needs root and a network interface on the host.
