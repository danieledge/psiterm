# Fix log: ssh/ (psiglue, psishim, sftp, tmuxq, tls13, pglinktest)

Branch dev, working tree only (nothing committed). Files changed:
`ssh/psiglue.cpp`, `ssh/pglinktest.cpp`, `ssh/pglinktest.h`, `ssh/psishim.c`,
`ssh/sftp.c`, `ssh/tmuxq.c`, `ssh/tls13.c`, `ssh/tls13.h`,
`ssh/test/linktest.cpp`, `ssh/test/psiglue_host.c`,
`docs/epoc-comms-best-practices.md`. Line numbers below are as of this log.

**For the other agents (app/, mail/):**
- `PsiShared` (ssh/psishared.h) is unchanged: no rebuild forced by layout.
- `PgLinkTest` (ssh/pglinktest.h) gained fields; the three apps compile
  `pglinktest.cpp` themselves, so any app rebuild picks it up. The
  `PgLinkTestL` / `PgLinkConnectL` API is unchanged: no app change needed.
- `pg_attach` has a new failure code **-20** (no timer: an engine does not start
  without one). psissh words it; PsiMail's/PsiWeb's engines show the number.
- New C calls: `tls_close()` (ssh/tls13.h): the mail and web agents should call
  it where a connection ends (`pmn_close`, `pwn_close`) so the traffic keys do
  not outlive the connection. `tls_connect` wipes them itself before the next
  connection, and on a failed handshake, so this is hygiene, not correctness.
  `pg_take_link_doubt()` (psiglue): 1 once after a switch-on with the modem
  online and no DCD to ask; PsiMail could send a NOOP with a 10 s limit then
  (recovery.md 12 suggests it); psissh already uses it (below).
- `bash build.sh` rewrote `dist/PsiTerm.sis` (it was already modified in the
  tree before my first build, at 07:53, by another build). `git checkout dist/`
  before a release as usual.
- certcheck.c / pmrsa.c (bestpractice 15, 16) were being edited by the mail
  agent when I looked (NUL in dNSName, wildcard suffix, unreadable dates,
  exponent bound were already in their diff). I did not touch mail/engine.
  Still theirs to confirm: `chain_ok` checking `not_after` of every certificate
  in the path (15, last point).

## recovery 1 / bestpractice 1: a receive completing during a two-status wait, then `WaitForRequest(gRecvStat)` blocks for ever

Confirmed as a real path. On EKA1 `User::WaitForRequest(a, b)` loops on the
request semaphore until *a or b* has completed, so the count of any other
request completing meanwhile (the receive pg_wait left outstanding, an
abandoned lookup) is consumed; a later single-status wait on that status then
needs a count that is gone, and with nothing else outstanding never returns.
The file's old "neither completed" branch was unreachable for the same reason
(bestpractice 10's hypothesis), so the orphan flags were never cleared there.

Fix, one rule: **never wait on a status that has already completed**; a
count left over the other way round is harmless because the engines run no
active scheduler and every wait loops until its own status completes.
- `ssh/psiglue.cpp:217` `NoteOrphans()`: orphans that have completed are
  forgotten; called after *every* return of the two-status wait (`:293`), not
  only in the dead branch.
- `:262` `WaitFor`: returns 1 at once for a completed status (no wait); after a
  completion the timer is cancelled and waited for only if still pending.
- `:934` `NetRxFill`: the "completed earlier: take its signal" wait is gone;
  `WaitFor` does the right thing in both cases.
- `:536` `NetCloseSocket`: after `CancelRecv` the wait is bounded (5 s) and
  logged if it does not complete (the status is static, so a late completion is
  safe).
Verified: psissh.exe builds; the host psissh-host (same psishim/sftp/tmuxq
code, Linux stand-in for psiglue) passes `ssh/test/update.py` 6/6 and the TLS
runs below. The EPOC wait logic itself needs the device (PPP route: type while
output streams, SFTP PUT over a slow link, Disconnect afterwards).

## bestpractice 10: orphan-reaping can block for ever
Same fix: `ReapOrphans` (`:320`) goes through `WaitFor`, which never waits on a
completed status, and `NoteOrphans` clears a flag as soon as its status
completes, whichever wait consumed the count. `pg_close` can no longer block
on `gShutStat`.

## recovery 15: psissh's "no timer" path waits without limit
- `ssh/psiglue.cpp:237` `TimerReady()`; `pg_attach` (`:1343`) creates the timer
  at start and returns -20 if it cannot (psishim `:1270` words it). `WaitFor`
  with no timer now returns 0 (timed out) with a log line instead of an
  unbounded wait; every caller already cancels on 0. `aTimeoutUs < 0` is kept
  as "no limit" but still slices 250 ms and checks quit.

## recovery 6 / bestpractice 6: SFTP and tmux channel state after an open timeout; CLOSING for ever
`ssh/sftp.c`, `ssh/tmuxq.c`:
- `opens_pending` (sftp `:89`, tmuxq `:50`) counts CHANNEL_OPENs the server has
  not answered. A timeout while the open is unanswered (`ch == NULL`) now goes
  back to NONE and reports `PSI_XFER_TIMEOUT` with "the server did not answer"
  (sftp `:1035`), not NO_SFTP; the next request may open again. A late
  confirmation for an open that was given up on is closed on arrival
  (`sftp_chan_init :428`, `tq_chan_init :200`), a late refusal only decrements
  the count (`sftp_chan_cleanup :465`, `tq_chan_cleanup :235`), so a refusal of
  the *current* open still gives NO_SFTP / NO_EXEC.
- `last_heard` is refreshed in `close_channel` and `start_op` (sftp `:404`,
  `:604`), so a request after a timeout is not failed at once by the old
  timestamp.
- CLOSING is time-boxed: after REPLY_SECS / TQ_SECS without the server's close
  the channel is left to Dropbear (`detach_channel`, sftp `:417`, tmuxq
  `:190`; its later cleanup is recognised by the `detached[]` list and
  ignored) and a new channel is opened. tmuxq no longer ignores every request
  while CLOSING (`psi_tq_loop`).
- Timeouts in SUBSYS/INIT report TIMEOUT ("the server did not answer") rather
  than "does not offer SFTP".
Note on the report's "for the rest of the session": over a dead link
Dropbear's keepalive (`-K 10`, limit 3) ends the session in about 30 s and
`psi_sftp_session_ended` resets everything, so the old behaviour lasted at
most that long unless the server answered keepalives but not channel opens.
The state fix is still right; the wording was overstated.
Verified: compiles for ARM and host; the state machine was walked by hand for
late confirm, late refusal, double open, CLOSING timeout. Needs a device/sshd
test for the timeout paths (no local sshd + throwaway user on this box).

## bestpractice 17: a download that ends early reported as saved
`ssh/sftp.c:814`: at the CLOSE reply of a GET, `done_bytes < total` (total
known from STAT, not the >4 GB marker) fails with "the file was cut short";
the app then removes the temporary. Compiles; needs a server test.

## bestpractice 18: Stop after the final CLOSE reported as "Transfer stopped"
`ssh/sftp.c:1009`: a cancel is not applied while a CLOSE is outstanding on a
ready channel; its answer decides (OK, or the server's "disk full").

## recovery 7 / bestpractice 5: Connection settings > Test and Connect block the UI thread; `LtLookup` waits without limit
Biggest change. `ssh/pglinktest.cpp` (rewritten around the same API):
- `pg_link_test` runs in a worker `RThread` (`PgLinkThread :146`, 16 KB stack,
  its own C32/ESOCK sessions, N14), while the app's thread waits in a nested
  `CActiveScheduler::Start()` (`PgLinkRunThreadL :174`), as EIKON does for a
  dialog: the app repaints, its timers run (PsiTerm's pump keeps `app_beat`
  going, so psissh's 45 s `AppGone` can no longer end a live session), and a
  `CPeriodic` (`PgLinkTick :117`) shows the test's progress text as the busy
  message, resets the auto switch-off timer every 20 s, caps the whole test at
  180 s and a stop at 15 s.
- Esc stops the test: `CPgLinkKeys :76`, a windowless control on the control
  stack above the dialog (CONE offers keys top-down regardless of focus) that
  takes every key meanwhile and sets `abort_test` on Esc. The worker checks
  `abort_test` at every step (`LtCommand :2629`, the baud scan, `LtWaitSlices`
  1 s slices). Outcome "Stopped" as an infoprint, no result dialog.
- A pen tap on the dialog's Cancel ends our loop from beneath: handled as a
  stop, then the Stop is passed on to the loop it was meant for (`passOn :222`).
  A leave through the loop (the app closing, KLeaveExit) is covered: the key
  catcher comes off the stack by cleanup item, and `PgLinkFree :242` leaves the
  record alone while a thread still runs on it.
- `LtLookup` (`ssh/psiglue.cpp:2918`): its requests live in a block the app's
  thread allocates (`pg_lt_lookup_new :2875`, `TLtLookup :2867`); the wait is
  in 1 s slices (`LtWaitSlices :2891`), then Cancel + 10 s, then close the
  sessions + 10 s, and only then is the request abandoned with the block
  leaked on purpose (`lookup_leaked :2981`); the next Test says "The last test's
  name lookup has not ended yet - test again in a minute" (`:2492`). No
  unbounded wait remains.
- `LtProgress :2532` writes to the record (`busy`, `busy_seq`); the worker
  never touches the screen.
Verified: PsiTerm.app builds with the new pglinktest.cpp (ER5's
`CCoeEnv::AppUi()` returns `CCoeAppUiBase*`: cast added); host `linktest`
49/49 (wording). The threading and Esc need the emulator/device: Test on both
routes, Esc during "Testing the modem...", Cancel tapped during a test, Test
while SSH is connected (session must survive), Connect then Esc.

## bestpractice 30: `LtSetConfig` results ignored
`ssh/psiglue.cpp:2804` (`LtPppStart`): a failed SetConfig is now `ppp = 5`
"Did not send X - could not set the serial port up (err)" and no lookup is
started (`:3027`). In `LtModem` the no-flow-control retry only runs when the
reconfigure succeeded; the reconfigure after the baud scan is commented as
harmless if it fails (nothing after it depends on the rate). Host test added.

## bestpractice 9: `Format` into 160 bytes with a 128-byte host
`ssh/psiglue.cpp:837`, `:854`: the host is clipped to 60 characters and passed
as `%S`.

## recovery 9: updater reports a bad signature when the disk filled at the last write
`ssh/psishim.c:1117`: `fclose` checked; a failure removes the file and says
"could not finish writing ... - disk full?". Verified by the update tests
(normal path).

## recovery 12: modem route cannot tell a dead link from a quiet one after switch-on
- `ssh/psiglue.cpp:1295`: with the modem online (`gModemOnline`, set on
  CONNECT `:2064`, cleared on hang-up/carrier lost/close) and no DCD to ask,
  switch-on now says "The modem link is in doubt after switching on -
  checking..." and sets a flag.
- `ssh/psishim.c:461`: `psi_select` takes the flag (`pg_take_link_doubt`) and
  zeroes Dropbear's keepalive clocks so the next `checktimeouts` sends a
  keepalive at once; a dead line is then found at the keepalive limit (30 s)
  instead of after the next quiet spell plus it. PsiMail's NOOP probe is for
  the mail agent (flag exported). Needs the device (switch off mid-SSH on a
  3-wire modem).

## bestpractice 13: TLS secrets not wiped on failure, traffic keys never
`ssh/tls13.c`: one `done:` block (`:493`) wipes every handshake secret, the
transcript states and the handshake buffers on both paths; `fail:` also calls
`tls_close()`. `tls_close()` (`:282`, exported in tls13.h) zeroes `g_rd`,
`g_wr`, the application/record/output buffers; `tls_connect` calls it first.
psishim calls it through `io_hangup` (`:774`) at every hang-up in TLS mode.

## bestpractice 14: plaintext handshake and alert records accepted after the keys are on
`ssh/tls13.c:244`: once `g_encrypted`, any record type but CCS (20) and
application data (23) is "unexpected plaintext record".
Verified (13+14): the Links ARM harness (`make -f web/links/epoc.mk emu`, then
`web/links/emu/run_pages.sh ... npr=https://text.npr.org/`) fetched the page
through the rebuilt ARM tls13.c against the live server (10070 bytes in, 1
dial, exit 0; screenshot in scratchpad/tls-emu/npr-1.png). psissh-host against
`ssh/test/tlsserver.py` (TLS 1.3 only, ranges): the updater downloaded a
585 KB file byte-identical over "Psion Internet" and over the fake modem, the
signature verified, and "latest version" returned rc 0.

## bestpractice 15, 16: certificate checks, RSA exponent
In the mail agent's files and already in their diff when I looked (see the
note at the top). Not touched here.

## bestpractice 29: `psi_vsnprintf` formats into a fixed static before applying `size`
ESTLIB has no `vsnprintf` (checked `epoc32/include/libc/stdio.h`), so a bounded
formatter would have to be written from scratch. `ssh/psishim.c:125`: the
buffers are 8 KB and 4 KB, and a result that ran past one (memory already
spoilt) now ends the engine with a message (`psi_overrun`) instead of going
on. Every caller is Dropbear's and bounded, as the report says.

## bestpractice 30: `CloseSTDLIB` never called in psissh.exe
`ecrt0.o` (checked with `strings`) references `main`, `exit` and `E32Main`
but not `CloseSTDLIB`, and the SDK says ecrt0's E32Main just calls `main()`.
Calling `CloseSTDLIB` and then returning to ecrt0 would hand a freed `_reent`
to `exit()`, so `ssh/psishim.c:669` `psi_end` calls `CloseSTDLIB()` and ends
the process itself (`pg_exit_process :1466` in psiglue = `User::Exit`), on
every exit path including Dropbear's `psi_session_ended`. The exit reason the
app reads is unchanged (EExitKill, same reason). Needs a device check that
psissh still ends cleanly (PsiTerm would show a panic category otherwise).

## bestpractice 30: documentation drift (S14, N11)
`docs/epoc-comms-best-practices.md`: S14 now says one timed Read of up to 1 KB
per turn with a 16 KB driver buffer; N11 notes that on the Psion Internet
route `pg_hangup` keeps the NIFMAN timers off until `NetClose` (PsiMail's 20
minute idle release), by design.

## bestpractice 30: RNG seeding
Not changed. Stirring the server random into the pool adds nothing against a
network attacker (it is on the wire); the arrival-time idea needs a tick
source tls13.c does not have on the host harnesses. `pg_entropy` already takes
48 spin-count samples between tick edges (several bits each) plus clock,
tick, thread id and free RAM, and the seed file carries the pool across runs.
`pg_entropy` now uses the real clock explicitly (`HomeTime`) since `NowMicro`
is tick-based.

## web/links/PORTING.md phase 4: psiglue waits time out to the second
`ssh/psiglue.cpp:1040` `NowMicro()` is now `User::TickCount()` x
`UserHal::TickPeriod` (15625 us on the 5mx), kept monotonic across the
counter's wrap, never 0. Every wait in psiglue (`pg_wait`, `WaitFor`,
`ReadLine`, the PPP CONNECT wait, the DNS cache age, the fail hold, KeepAwake)
is now exact to a tick; PsiWeb's 20 ms input poll over Psion Internet no
longer waits up to a second. Ticks stop while the Psion is off, as RTimer
does; the switch-on recheck covers that. Shared by all three engines: psissh
built; psimail-host/psiweb use the same source (the mail host build and the
Links ARM build both succeeded; their EPOC `psiglue.o` compiles from the same
file). Needs the device: the first run on real hardware to see the
`TickPeriod` value (the fallback is 15625).

## Checked and left alone
- recovery 2, 3, 4, 5, 8, 10, 11, 13, 14, 16, 17; bestpractice 2, 3, 4, 7, 8,
  11, 12, 19-28: app/ and mail/ (other agents).
- eikon.md: nothing in ssh/ (pglinktest wording was found conforming).
- `EApaSystemEventBackupStarting` (bestpractice 30): app side.

## Verification run
- `ssh/test`: `make -f test/Makefile.host linktest && ./linktest`: 49 checks,
  0 failed (two added for the new wordings and the 80-column limit).
- Docker, under the shared lock: `bash build.sh` (psissh.exe + PsiTerm.app,
  dist/PsiTerm.sis rebuilt); `mail/build.sh host` (ARM + host engines, tls13.c
  in both); `make -f web/links/epoc.mk emu`; all exit 0.
- `mail/test`: `foldertest.sh`, `undotest.sh`: ALL OK.
- psissh-host linked against host-built libtomcrypt/libtommath (in the
  scratchpad: the Makefile's `/home/claude/dropbear-src` does not exist here);
  `ssh/test/update.py` (chunk server with truncated and corrupted pieces, fake
  Hayes modem and direct socket): 6/6 PASS with a 50 KB release (the 594 KB
  dist file needs 36 redials and overruns the test's own 120 s limit, which is
  not new). TLS 1.3 updater runs as above: 3/3.
- Not run: e2e.py / sftp_test.py (need a local sshd and a throwaway user; no
  sudo), the emulator UI for the threaded Test (tools/emu drives PsiMail; a
  Connection settings > Test run with "Esc" and "tap Cancel" steps would be the
  check).
- Temporary servers were started and ended by PID; /tmp/updsrv, /tmp/psihome,
  /tmp/psiupd.sis and the test certificate were removed.

## Left for the device
1. PPP route: an SSH session with typing while output streams, an SFTP PUT of
   a few hundred KB, then Disconnect: no hang, "the abandoned ... has
   completed" lines in the log if any.
2. Connection settings > Test on both routes; Esc during it; the pen on Cancel
   during it; Test while SSH is connected (the session must stay up); Connect.
3. Switch off during SSH on a 3-wire modem: the "in doubt" line, the session
   ending within about 30 s if the modem dropped the call.
4. psissh ending cleanly after CloseSTDLIB (no panic category in PsiTerm's
   "[SSH program ended ...]" line).
5. PsiWeb keys/taps over Psion Internet: no longer up to a second late.
