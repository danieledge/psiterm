# Power: idle wake-ups in PsiTerm, PsiMail and PsiWeb

The 5mx's ARM710T stops in HALT, and saves the batteries, only while every
thread is waiting on a request. Any timer that fires, or any loop that sleeps
for a tick and looks again, takes the CPU out of HALT. This note lists where
the apps and their engines woke when nothing was happening, what was changed
(0.81, not yet released), how it was measured, and what still has to be
checked on a real 5mx.

## Findings (before)

| Where | What woke | How often, idle |
|---|---|---|
| PsiTerm, SSH pump (`CTermView::PumpSsh`) | `CPeriodic` every tick, to look for psissh output and beat | 64 a second while SSH runs |
| psissh, `pg_wait` (`ssh/psiglue.cpp`) | a timed serial read or `User::After` of one tick, to look at the keyboard ring | 64 a second while SSH runs |
| psissh, `psi_select` (`ssh/psishim.c`) | a 250 ms cap once logged in, to see SFTP and tmux requests | 4 a second |
| PsiTerm, status tick | `CPeriodic` 0.5 s: clock, blink, tmux tabs, log flush | 2 a second, also in the background |
| PsiTerm, tmux window list | a channel and a command on the server | every 4 s (tmux), 10 to 30 s (no tmux) |
| PsiMail app, `TickL` | `CPeriodic` 250 ms | 4 a second, always |
| psimail.exe, `pm_loop` | `pm_idle(100)`: `User::After` 100 ms | 10 a second, always |
| PsiWeb app, `Tick` | `CPeriodic` 62.5 ms, to copy frames | 16 a second, always |
| psiweb.exe, Links input poll (`web/links/psi_drv.c`) | a Links timer of 20 ms; `psi_select` sleeps 20 ms with no connection | 50 a second, always |
| `KeepAwake` (`ssh/psiglue.cpp`) | `UserHal::ResetAutoSwitchOffTimer` for any byte, every 30 s | an idle SSH session's keepalive (`-K 10`), tmux's status line and the tmux window list kept the Psion on for ever |

In wake-ups a second, estimated from the code: PsiTerm with an idle SSH
session about 130; PsiWeb about 66; PsiMail about 14; PsiTerm without a
session 2.

Not polling, and left alone: PsiTerm's own serial terminal (`CSerialPort`
waits for the first byte), `pmbutton.exe` (file and window-group
notifications), PsiMail's timed check (a `CPeriodic` every 5 minutes, only
when it is on), the transfer and Test windows (only while open).

## Changes

### Doorbells (`ssh/psibell.h`, `ssh/psishared.h`)

Each side of an app and its engine now waits on a request, and the other side
completes it when it has left something in the shared chunk:
`RThread::RequestComplete` on a thread of another process, as a server
completes a client's request. `PsiShared` (which PsiMail's and PsiWeb's chunks
start with) has two bells: `app_bell` (the engine rings, the app waits) and
`eng_bell` (the app rings, the engine waits), and `bell_magic`, which the app
sets to say that it rings `eng_bell` for everything the engine waits for.

- The protocol is in the header. One waiter and one ringer; `armed` is swapped
  with ARM's `SWP`, so exactly one side completes the request; a ring before
  the waiter has armed is seen through `seq`.
- If the ringer cannot open the waiter's thread it sets `broken`, and the
  waiter polls instead. If the waiter's thread has died it does nothing.
- Every wait on a bell also has a timer, so a missed ring is a delay of a
  second or two, never a hang.
- `rings` and `wakes` count the traffic. A RAM snapshot of the emulator
  showed them equal on every bell: the cross-process completion works on the
  5mx ROM.

### psissh and the other engines (`ssh/psiglue.cpp`, `ssh/psishim.c`)

- `pg_wait` looks every tick, as before, for 1/4 s after keys or data, so a
  burst is still read in slices. Then, if the app rings (`BellsOn`), it waits
  in `QuietWait` for the app's ring, the link's data or a 5 s timer:
  - Psion Internet: the socket receive that is always outstanding;
  - modem: a `ReadOneOrMore`, which completes on the first byte. If the wait
    ends without data it is cancelled (a cancelled `ReadOneOrMore` has taken
    nothing), so no other code ever sees a read outstanding.
- psissh rings PsiTerm's bell after each output (`pg_out_write`).
- `pg_wait` returns 16 when PsiTerm has posted an SFTP or tmux request, to the
  session's wait only. `psi_select` keeps its 250 ms cap only while a request
  is in hand (`psi_sftp_busy`, `psi_tq_busy`), or when there is no doorbell.
- `pg_bell_wait`, `pg_ring_app` and `pg_bells` are for the mail and web
  engines.
- `KeepAwake` resets the switch-off timer only if 2 KB or more moved in the
  last 30 s. A download still keeps the Psion on; a quiet session no longer
  does.

### PsiTerm (`app/psiterm.cpp`, `app/ptxfer.cpp`)

- The SSH pump goes every tick while output flows. After half a second with
  nothing it stops, with the bell armed; psissh's ring starts it again.
- The status tick beats for the pump (psissh still quits 45 s after the last
  beat) and looks at the output ring, in case a ring went astray.
- The status tick goes every 0.5 s only while something on the screen moves
  or waits on it: the cursor blink, a connection being made, a reconnect
  countdown, a tmux query, a modem that has not answered, typing in the last
  10 s. Otherwise every 2 s: the clock in the status line turns within 2 s of
  the minute. Behind another program it is 2 s and draws nothing.
- Keys, a resize, End SSH, a switch-on, and every SFTP and tmux request ring
  psissh's bell.
- The tmux window list is asked for every 4 s while keys are typed, every 14 s
  after a minute without one (still within the 15 s an answer is used for),
  and not at all behind another program (once on coming back).

### PsiMail (`mail/app/psimail.cpp`, `mail/engine/pmepoc.cpp`)

- The app ticks 4 times a second while anything is going on: the engine busy,
  commands queued, results not yet taken, a message that times out, a busy
  message, the update window, start-up. Otherwise every 2 s, with the bell
  armed.
- The engine, idle, rings the app when a command has finished, files have
  changed, the line has come or gone, or its state has changed. It then waits
  on its bell for up to 2 s (housekeeping) instead of 10 times a second.
- Every command, quit, Stop and switch-on rings the engine.
- The engine still quits only when the app's process has gone.

### PsiWeb (`web/app/psiweb.cpp`, `web/links/epoc/psi_os.c`, `web/links/psi_drv.c`)

- The app ticks 16 times a second while a page loads, frames come (and for a
  second after), input is waiting, or a question, a link message, the update
  or start-up is in hand. Otherwise every 2 s, with the bell armed.
- Links' input poll goes every 20 ms while anything happens, and every 2 s
  after a second of nothing with nothing loading. A ring from the app takes
  its input at once (`psi_poll_soon`).
- With no connection the engine waits on its bell, not 20 ms at a time.
- The engine rings the app when there is a new frame, a link message, loading
  starts or ends, a question, an update step, or a change of state.
- Keys, taps, commands, answers, quit and switch-on ring the engine.

### Liveness

- psissh still quits 45 s after PsiTerm's last beat. PsiTerm beats at least
  every 2 s; psissh looks at least every 5 s.
- PsiMail's engine still quits only when PsiMail.app's process has gone. It
  looks every 2 s.
- psiweb.exe still quits after 60 s without a beat. PsiWeb beats at least
  every 2 s.

## Measurements

### Method

The emulator models HALT (`HALT`, 0x408, in `core/windermere.cpp`) and skips
ahead to the next timer while halted. A private copy of the harness, built in
the scratchpad and never committed, counts the cycles spent halted and prints
a line every simulated second:

    PWR t=150 busy=1.11 halts=64

`busy` is the share of the 36 MHz CPU that ran, which is what costs power. It
can also add up the busy cycles in each 64 KB of address space over a window
(ROM, kernel vectors, the apps' RAM code). The patch is about 40 lines: a
counter in the halted branch of `Emulator::executeUntil`, and the report in
`harness/run.cpp`. Runs use `tools/emu/run.sh` and `tools/emu/net.py` with
`PSION_EMU` pointing at the copy.

Each figure is the mean over 50 to 90 simulated seconds of idle. The System
screen alone, with no app, is 0.93%: the 64 Hz system tick and the kernel.
What an app adds is the figure less 0.93.

### Results

| Case | Before | After |
|---|---|---|
| System screen only | 0.93% | 0.93% |
| PsiTerm, not connected | 1.06% | 0.97% |
| PsiTerm, SSH logged in over the modem, idle | 3.12% | 1.09 to 1.11% |
| PsiTerm behind Word | 1.11% | 0.99% |
| PsiMail, main view, idle | 1.09% | 0.96% |
| PsiMail, first-run Account dialog | 1.12% | 0.99% |
| PsiMail, online after a check, idle | 1.07% | 1.00% |
| PsiWeb, welcome page, idle | 2.00% | 0.98% |
| PsiWeb, after loading a page over the modem | 1.98% | 0.94% |

What the apps add over the System screen, idle:

| Case | Before | After |
|---|---|---|
| PsiTerm with an idle SSH session | 2.19% of the CPU | 0.16 to 0.18% (about 12 times less) |
| PsiWeb | 1.07% | 0.05% |
| PsiMail | 0.16% | 0.03% |

Busy cycles over 90 s of an idle SSH session: 103.7 million before, 36.8
million after, of which about 30 million is the System screen's own. The
rest is now mostly the kernel's work for the few wake-ups left, not the apps'
own code.

Wake-ups a second, idle (from the code; the CPU figures agree):

| Case | Before | After |
|---|---|---|
| PsiTerm, idle SSH session | about 130 | about 1 (0.5 status tick, 0.2 psissh, keepalive every 10 s) |
| PsiTerm, not connected | 2 | 0.5 |
| PsiMail | 14 | 1 |
| PsiWeb | 66 | 1 |

### Checks

- Builds: PsiTerm, PsiMail, PsiWeb (`build.sh`, `mail/build.sh`,
  `web/build.sh`), and `mail/build.sh host`.
- Host tests: `foldertest.sh`, `undotest.sh`, `invtest.py`, `invitehost.py`,
  `htmltest.py`, `imgtest.py`, `parsefuzz.py`: all pass. `certtest.py` needs
  Python's `cryptography` module, which the build image lacks.
  `ssh/test/linktest`: 49 checks, 0 failed. `firmware/atom-modem` host tests
  pass.
- `psissh-host` compiles, but does not link here: `ssh/test/Makefile.host`
  expects libtomcrypt at `/home/claude/dropbear-src`. The SFTP and e2e host
  tests need root for their throwaway user, so they were not run.
- `tools/emu/net.py all` passes (at, web, mail, ssh). After an idle spell the
  echo of a typed command comes back as before, and PsiWeb's arrow keys move
  the focus by the next screenshot (they are taken once a simulated second).
- Each app starts in the emulator and reaches its first screen.
- Flakes seen, all also seen with the old build: PsiWeb sometimes stops at
  start with KERN-EXEC 3 or stays at "Starting the browser engine..." (the
  card-layout problem in `tools/emu/README.md`; a RAM snapshot of a stalled
  run shows the engine had not yet reached any of the new code); net.py's
  `at` and `web` tests sometimes pass only on the retry; PsiMail's "download
  ahead" sometimes continues after the test's check and sometimes not.
- Not covered by the emulator: Psion Internet (PPP), carrier detect, and the
  real current drawn.

## Tests on the 5mx

1. **SSH, idle.** Log in over the modem, wait 2 minutes, type. The echo must
   come back at once. Leave it: the Psion should now switch itself off at its
   usual time (it did not before). Switch on: the session is still there, or
   PsiTerm reconnects as before.
2. **SSH over Psion Internet.** The same, plus a large `cat` and an SFTP Get,
   to exercise the receive that stays outstanding in a quiet wait.
3. **tmux.** Tabs appear, a tap changes window, Ctrl+Tab works, after a
   minute of no typing and after coming back from another program.
4. **Background.** Switch to the System screen with a session up for a
   minute, come back: the screen, the clock and the tabs are right.
5. **Close from the task list** with a quiet session: psissh must quit
   within about 50 s (`link:` log, or the serial port free for another app).
6. **PsiMail.** Check mail; Stop during a dial-up (must stop at once); leave
   it open 10 minutes; a timed check still runs.
7. **PsiWeb.** Load a page, wait, then scroll and follow a link; Esc during a
   load.
8. **Current.** With a meter in the battery line, or `UserHal::SupplyInfo`
   (see `docs/experimental-hacks.md`, section 5): the System screen alone,
   then each app idle, before and after. The display and the RS-232 line
   driver draw far more than the CPU, so expect a few mA, not halves.

## What is left

- **The serial port.** PsiTerm keeps COMM::0 open whenever SSH is not
  running, so the RS-232 line driver stays powered. That may now be the
  largest cost of leaving PsiTerm open. Closing it in the background or after
  some idle time changes behaviour (nothing typed by the modem is seen), so
  it is the owner's choice.
- **The SSH keepalive** is every 10 s (`-K 10`). Each one wakes psissh and the
  modem's radio. 30 s would still find a dead link in good time, but it is a
  connection setting, so it was left alone.
- **psissh's quiet wait** could be longer than 5 s; only the heartbeat check
  needs it.
- **PsiTerm with the cursor blink on** still ticks twice a second in front.
