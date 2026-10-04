# Error-recovery review: PsiTerm and PsiMail (branch dev, read-only)

Scope: ssh/psiglue.cpp, ssh/psishim.c, ssh/sftp.c, ssh/tmuxq.c, ssh/tls13.c, ssh/pglinktest.cpp, ssh/psilink.h, app/psiterm.cpp, app/ptxfer.cpp, mail/engine (pmmain.c, pmepoc.cpp, pmnet.c, imap.c, smtp.c, store.c, http.c, pmupdate.c, webpics.c), mail/app (psimail.cpp, pmrecover.cpp, pmauto.cpp, pmstore.cpp). All paths are under /home/daniel/www/psion/psiterm/. Line numbers are from the current dev checkout.

Severity scale: crash, data loss, hang, misleading, minor. "Needs device test" marks a code-verified path whose trigger timing I could not prove without hardware.

What is done well (so it is not repeated below): atomic temp+Replace writes in the mail store (store.c, imap.c, caldav.c, undo.c, pictures.c), PsiTerm's SafeWrite and TPsiLink::Save, Get file via a `~` temporary (ptxfer.cpp:845-868), psimail.exe quitting only once the app's process has really gone (pmepoc.cpp:296-342), PsiLinkTimersBack after a kill or panic, bounded ESOCK waits with orphan reaping, the NO CARRIER quiet period, `pm_fclose` catching deferred disk-full errors, half-downloads removed in sftp.c and pmupdate.c, and the engine-restart logic in pmrecover.cpp.

---

## 1. HANG (needs device test): a completed receive's signal is eaten as a "stray" and the next wait on it blocks for ever (Psion Internet route)

**Files:** ssh/psiglue.cpp:238-259 (`WaitFor` inner loop), :877-898 (`NetRxFill`), :494-498 (`NetCloseSocket`), :931-945 (`NetWrite`).

**Scenario.** psissh (SSH or SFTP over PPP) has a `RecvOneOrMore` outstanding (`gRecvPending=1`): `pg_wait` posts one in `RxFill` and returns on a keystroke, an SFTP request or a timeout without waiting for it (psishim.c:432-435). Dropbear then writes (`NetWrite`). `WaitFor(stat, 60 s)` loops on `User::WaitForRequest(aStat, timerStat)`. If the server's data arrives while the Write is still pending (a 4 KB SFTP WRITE with the TCP send buffer full on a 115200 link; typing while output streams), the receive completes first: the semaphore is decremented, neither `aStat` nor `timerStat` changed, and the loop at :247-258 only checks the lookup/connect/shutdown orphans, so the receive's signal is consumed and forgotten. Later `NetRxFill` sees `gRecvStat != KRequestPending` and calls `User::WaitForRequest(gRecvStat)` (:897) "to take its signal". On EKA1 that is `do { WaitForAnyRequest(); } while (status==KRequestPending)`: it blocks until *some* request completes, and nothing else is outstanding. psissh never returns to its loop: no quit check, no heartbeat check, the terminal freezes. The same `WaitForRequest(gRecvStat)` after `CancelRecv()` in `NetCloseSocket` (:496-497) blocks the same way. The comment at :887-890 shows the authors know this exact failure mode; the fix there covered the `WaitFor()==1` path but not the stray-in-another-wait path.

**Why it matters.** Together with finding 2 it leaves PsiTerm with a session that cannot be disconnected. For PsiMail the exposure is smaller (writes normally follow a completed read), but SMTP DATA over TLS with a server that sends a KeyUpdate/alert mid-stream has the same shape.

**Fix.** Track "signal already taken" per static request: in the stray branch add `else if (gRecvPending && gRecvStat != KRequestPending) gRecvTaken = 1;` and in `NetRxFill`/`NetCloseSocket` skip the `WaitForRequest` when `gRecvTaken` is set (clear it after). Better still, never call `User::WaitForRequest` on a status that is already completed; keep an explicit per-request "pending/signalled" state.

## 2. HANG / MISLEADING: PsiTerm has no watchdog after Disconnect or Stop

**Files:** app/psiterm.cpp:3828-3835 (`DisconnectSsh`), :792-799 (`StopTool`), :5014-5020 (`CToolDialog::OkToExitL`), :353-363 (`~CTermView`).

**Scenario.** Disconnect and the tool window's Stop only set `iShared->quit = 1`. If psissh is wedged (finding 1; `WaitFor` with no timer at psiglue.cpp:219-222; a KERN-EXEC-free deadlock in ESOCK) nothing ever kills it: `iSshActive` stays true, "SSH to...", Connect and the Debug tools all answer "Not available while SSH is connected" (:5236-5239), the tool dialog cannot be closed (Esc just calls `StopTool` again), and psissh keeps the serial port or the PPP session. The only way out is to exit PsiTerm, whose destructor waits 3 s and kills (:356-362). PsiMail at least has Tools > Restart mail engine (psimail.cpp:472-490, 6 s then Kill).

**Fix.** After setting `quit`, start a one-shot timer (5-10 s); if `iSshProcess.ExitType()==EExitPending` then `Kill(0)`, `PsiLinkTimersBack()`, and run the `SshProcessEnded` path with a "[SSH program did not stop and was ended]" message. Make the tool dialog's second Esc do the same.

## 3. DATA LOSS: PsiMail's settings files are rewritten in place with unchecked writes

**Files:** mail/app/psimail.cpp:3409-3426 (`SaveSettings`), :3505-3514 (`SaveCalSettings`), :4107-4114 (Update.ini).

**Scenario.** `RFile::Replace(KIniFile)` truncates PsiMail.ini first, then three `Write`s whose results are ignored. A `KErrDiskFull` on C: (PsiMail.ini is always on C:, which is small), a switch-off or flat battery between Replace and the last Write, or a card-wedge style stall leaves a short file. `LoadSettings` (:3393-3398) then sees the wrong size and does `Mem::FillZ(&iSettings)`: every account, server, user name and password is gone, and on the next start PsiMail shows "no account". The calendar settings reset the same way. The project's own rule (docs/epoc-robustness-best-practices.md section 4) and PsiTerm's `SafeWrite` (app/psiterm.cpp:102-120) and `TPsiLink::Save` (ssh/psilink.h:61-88) already do it right.

**Fix.** Write to `PsiMail.ini~`, check Write and Flush, Close, then `RFs::Replace(tmp, KIniFile)`; on failure delete the temp and keep the old file. Same for Calendar.ini and Update.ini. Also keep a one-generation backup (`PsiMail.bak`) since this file is the only copy of the account details.

## 4. DATA DUPLICATION / MISLEADING: a message accepted by the SMTP server is sent again after an ambiguous failure

**Files:** mail/engine/smtp.c:221-222, :104-118 (`fail`); mail/engine/pmmain.c:181-191, :208-211; mail/app/pmauto.cpp:141-178.

**Scenario.** The whole message has been streamed and the terminating `CRLF.CRLF` written. The server's `250` is lost: NO CARRIER, PPP drop, or the 60 s `TIMEOUT` on a slow server. `reply()` returns <0, `fail()` returns PM_RES_OFFLINE with "The connection was lost (Message refused)" ... actually "(sending)"; `send_outbox` writes `<id>.err`, keeps `<id>.txt` and breaks. The user reads "not sent". Automatic sending skips files with `.err` (pmauto.cpp:160-161), but Check mail (PM_CMD_SENDRECV -> `send_outbox`) sends every `.txt` in the outbox again: the recipient gets it twice, with a different Message-ID, and the Sent folder gets two copies. A failed `remove(path)` at :208 (card wedge) has the same effect.

**Fix.** Mark the point of no return: before writing the final `.\r\n`, write `<id>.sent` (or append `Sent-Attempt: <time>` to the `.txt`). On an ambiguous failure after that point, do not resend automatically; show "may already have been sent" and let the user choose (or look for the Message-ID in Sent, since the header is in the `.eml`).

## 5. DATA LOSS / MISLEADING: an engine restart silently wipes the queued commands and leaves the timed-check bookkeeping pointing at the old sequence

**Files:** mail/app/psimail.cpp:412-434 (`StartEngineL`: `Mem::FillZ(s)`), :574-599 (`Cmd`), :1851-1864 (`TickL`); mail/app/pmrecover.cpp:56-69; mail/app/pmauto.cpp:116-123, :126-137, :195-199.

**Scenario.** The engine panics with commands still queued (a Move, then a Send). `EngineStoppedL` restarts it and `StartEngineL` zeroes the whole chunk: `cmd_head/tail`, `done_seq` all return to 0. Only a pending BODY is re-asked (pmrecover.cpp:68). The Move had already been applied to index.txt by `local_update` (pmmain.c:428) before `imap_move`; if the crash came before `st_pending_add`, nothing records it, so the message is gone from the local list but not from the server, and comes back at the next sync (possibly counted as new mail). The Send is simply forgotten until the next Check mail. No message says that anything was dropped.

`iAutoCmd` (pmauto.cpp:121) is `cmd_head+1` at the time of the timed check and is compared against `done_seq`. After the reset, `done_seq - iAutoCmd` is negative, so `AutoTickL` never clears it and `CheckDueL` (:116) returns early: timed checking stops silently until `done_seq` has climbed past the old number. When it does, the user's own SYNC that lands on that number is treated as the quiet automatic one (no "No new mail" infoprint).

**Fix.** In `StartEngineL`, do not zero the command ring when restarting: copy the still-queued entries (`iSent[tail..head)`) into the new queue, or at least count them and say "N commands were lost when the engine stopped". Reset `iAutoCmd`, `iCalPending` and other sequence-based state in `EngineStoppedL`. Make MOVE record its pending line before `local_update`, or write both in one step.

## 6. MISLEADING (session-long): a channel-open that never gets an answer leaves sftp.c / tmuxq.c in a state where every later request fails at once

**Files:** ssh/sftp.c:940-945 (`psi_sftp_loop` timeout), :363-377 (`close_channel`), :534-535 (`start_op`); ssh/tmuxq.c:254-261, :143-157, :245.

**Scenario.** `open_channel` sends CHANNEL_OPEN; the server never answers (stalled sshd, a dropped packet on a half-dead link that keepalives have not yet caught). After 60 s the loop calls `finish(PSI_XFER_NO_SFTP)` then `close_channel()`, but `ch` is still NULL so nothing is sent and `ch_state` stays CH_OPENING. The next Get/Send file: `start_op` does not reopen (`ch_state != CH_NONE`), and the timeout test is already true (`last_heard` is old), so the request fails instantly with "This server does not offer file transfer (SFTP)" for the rest of the session. tmuxq has the identical shape (TQ_OPENING stuck -> every query is an instant TIMEOUT; PsiTerm then falls back to screen-scraped tabs without saying why). A CLOSE the server never acknowledges leaves TQ_CLOSING, in which `psi_tq_loop` ignores every new request (:245) for ever.

**Fix.** When timing out with `ch == NULL`, set `ch_state = NONE` and `close_when_open = 1` so a late confirmation is closed immediately; time-box CLOSING (if the server has not closed in N s, treat the channel as gone: `ch = NULL; ch_state = NONE`). Report "the server did not answer" rather than NO_SFTP when the open itself timed out.

## 7. HANG / SESSION LOSS: Connection settings > Test blocks PsiTerm's thread for up to 100 s while psissh runs, and psissh's 45 s heartbeat then ends the session

**Files:** app/psiterm.cpp:6025-6034 (`EPtCmdConnSettings` is allowed while SSH is active), :4893 (`PgLinkTestL` from the dialog); ssh/pglinktest.cpp:84 (synchronous `pg_link_test`); ssh/psiglue.cpp:2706-2767 (`LtLookup`: 90 s + 10 s cancel wait + an unbounded `User::WaitForRequest(look)` at :2747), :1323-1351 (`AppGone`, 45 s).

**Scenario.** Psion Internet mode, SSH connected. The user opens Connection settings and presses Test. `LtNifActive` says up, `LtLookup` runs with a 20 s limit; or the link is down, the user agrees to dial, `LtPppStart` (port busy) then `LtLookup` with 90 s. The UI thread is blocked, so `PumpSsh` stops bumping `app_beat`; after 45 s psissh decides "the app's heartbeat stopped: quitting" and ends the session, hanging up the link the test is trying to bring up. If closing the session does not complete the abandoned lookup, :2747 blocks the app for good. In PsiMail the engine checks the process id so it survives, but the user still has a frozen UI with no Esc and no `KeepAwake`, so the Psion can auto-switch-off mid-dial.

**Fix.** Dim Test and Connect while a session is active (as `EPtCmdConnect` already is at :5665), or keep the heartbeat alive from inside the test's progress callback (the callback already runs between steps: have it bump `app_beat`). Run `pg_link_test` under a bounded wait with Esc, and call `UserHal::ResetAutoSwitchOffTimer` from the callback.

## 8. MISLEADING: PsiMail blames the card when the real reason is that the app had the index open

**Files:** mail/engine/pmepoc.cpp:162-181 (`pm_replace`: 5 tries, 60 ms apart); mail/engine/imap.c:884-889; mail/app/psimail.cpp:641 (the app reads index.txt with EFileShareReadersOnly).

**Scenario (needs device test for the timing).** A sync finishes while the app is reading a big index.txt (several hundred lines parsed on an ARM710 from a CF card can take longer than 300 ms, for example during a Render triggered by `changed_seq`). `RFs::Replace` returns KErrInUse five times; `st_index_save` fails; `imap_sync` reports PM_RES_FAILED with "Could not save the message list on D: - is the card in, and not full or write-protected?" even though the card is fine. The new messages are fetched again at the next sync. `imap_body_1:1401` ignores the same failure, so a message can be downloaded and still show as not downloaded.

**Fix.** Retry for longer on KErrInUse specifically (up to 2-3 s), and word the failure "the list is in use" when the last error was KErrInUse. Check the return at imap.c:1401.

## 9. MISLEADING: PsiTerm's updater reports a bad signature when the disk filled at the last write

**File:** ssh/psishim.c:1054 (`fclose(f)` unchecked), :1056-1059.

**Scenario.** The download fits until the final stdio flush; `fclose` fails with disk full (the project's own note: "fclose is where a disk-full error finally shows"). `verify_file` hashes a truncated file and the user sees "SIGNATURE CHECK FAILED - the download was deleted, nothing installed", which reads as a tampered release. pmupdate.c:370 gets this right.

**Fix.** `if (fclose(f) != 0) { remove(save_as); why = "disk full"; goto fail; }`.

## 10. MINOR / MISLEADING: PsiMail handles only the last of several commands that finished in one tick

**File:** mail/app/psimail.cpp:1851-1864.

**Scenario.** Two quick commands complete between ticks (an offline FLAG then MOVE both answer at once; a SEND that fails fast followed by a SYNC). `iDoneSeen` jumps to `done_seq` and `HandleResultL` runs for the last one only; the first's outcome (its infoprint, `.err` note, `PicturesDoneL`, `UndoResultL`) is never shown or acted on. `AutoResultL` compares `iDoneSeen` exactly, so a timed check can be passed over (covered by `AutoTickL`, but the alert for new mail is lost).

**Fix.** Walk `iSent` from `iDoneSeen+1` to `done_seq`; for all but the last, run the side-effect handlers that do not need `last_res` (reload, picture/undo bookkeeping) and at least log the skipped outcome.

## 11. MINOR: a failed reconnect launch leaves PsiTerm half in "reconnecting"

**File:** app/psiterm.cpp:3883-3899, :3748-3761, :3985-3993.

**Scenario.** `ReconnectNowL` sets `iReconnecting = ETrue` and calls `LaunchSshL`; if that leaves (OOM creating the chunk or the watcher) `TRAP_IGNORE` swallows it: no session, no message, no further retry, the terminal looks idle. If `Create` fails (:3748) the function returns with `iReconnecting` still set; the user's next manual connect that fails to dial is then treated as `iReconnecting && lost == 2` and PsiTerm starts an unasked retry chain.

**Fix.** Clear `iReconnecting`/`iReconnectWait` and print a message in both failure paths; reset `iReconnecting` at the top of a user-initiated `LaunchSshL(0)`.

## 12. MINOR: the modem route cannot tell a dead link from a quiet one after switch-off unless DCD is armed

**Files:** ssh/psiglue.cpp:1204-1228 (`SwitchOnCheck`, modem branch), :1388-1399.

**Scenario.** A WiRSa or Atom without DCD (common: the Atom base has only TX/RX/GND). The Psion is switched off mid-SSH; the modem drops the call. On switch-on nothing is checked ("no carrier detect to check"); SSH sits on a silent line until Dropbear's keepalive (`-K 10`, three misses) gives up, about 30-40 s later, during which typed keys vanish. PsiMail's IMAP read waits the full 60 s TIMEOUT. Documented behaviour, but worth an explicit "link in doubt after switch-on" message so the user does not keep typing.

**Fix.** On switch-on with FailDCD off, send a short probe (an SSH keepalive request at once, or for PsiMail a NOOP with a 10 s limit) and say "Checking the modem link after switching on...".

## 13. MINOR: the engine's log dies for the rest of the run if psimail.log cannot be opened once

**File:** mail/engine/pmepoc.cpp:243-255 (`gLogLen = -1`).

**Scenario.** PsiMail installed on D:, the card out for a moment at start, or the C: drive full: `fopen` fails once and `pm_log` returns for ever, so the one diagnostic the user can send is empty for exactly the sessions that go wrong.

**Fix.** Try again every minute (store the tick of the last failure) instead of latching -1.

## 14. MINOR / DATA: a certificate pin can be lost silently

**File:** mail/engine/store.c:308-318 (`st_pin_save` ignores `fclose`'s result).

**Scenario.** Card full when the user accepts a certificate: the pin is not written; the next connection asks again, and the TRUST command reported "Trusted host". Use `pm_fclose` and return the error so the command says it could not save.

## 15. MINOR: psissh's "no timer" path waits without limit

**File:** ssh/psiglue.cpp:213-222.

If `RTimer::CreateLocal` fails (handle exhaustion after a long session), every `WaitFor` becomes an unbounded `User::WaitForRequest`: a lookup or connect that never completes (NetDial dialog cancelled late) then hangs psissh with no quit check. Fail the operation instead of waiting, or create the timer once in `pg_init` and refuse to start without it.

## 16. MINOR / MISLEADING: PsiMail's progress window cannot be closed if the engine ends without a result

Mostly handled (psimail.cpp:500-508 finishes the dialog on `EngineEnded`), but `StopUpdate` only sets `net.quit`; an engine stuck in a blocking ESOCK wait (finding 1) leaves the dialog on "Stop" for ever. Tools > Restart mail engine is behind the modal dialog. Give the dialog's second Esc a "Restart the mail engine?" query.

## 17. MINOR: `imap_body_1` ignores the failure of the Seen flag store

**File:** mail/engine/imap.c:1404.

If the link drops right after the text is saved, the local index says read, the server says unread; the next sync may flip it back and count it as new. Queue it in pending.txt on failure, as PM_CMD_FLAG does.

---

## Things checked and found sound (for the record)

- App dies while the engine runs: psimail.exe checks `app_pid` (`AppGone`) every second once the heartbeat is 20 s silent and quits, releasing the line; psissh quits after 45 s of no `app_beat`; both close ESOCK/NIFMAN (`pg_close` -> `NetClose` -> `NifRelease`). A crashed PsiTerm's old psissh is asked to quit by the next launch (psiterm.cpp:3593-3602).
- Engine panic: both apps `Logon`, read the exit category/reason, call `PsiLinkTimersBack` and show it (psiterm.cpp:3928-3937; psimail.cpp:512-526 plus the restart logic).
- Serial port held by the Remote link / another app: distinct messages for KErrAccessDenied (-13) and KErrInUse (-10) with a 3x300 ms retry (psiglue.cpp:1015-1026, :1849-1854; psishim.c:1200-1208).
- DNS fail, connect timeout, PPP dialog cancelled: bounded (60/30 s), cancelled requests are reaped, a failed start is held for 8 s to stop dial storms, NetDial codes are worded (psiglue.cpp:692-872, :1783-1796).
- TLS handshake failure: the TCP connection is hung up in every caller (psishim.c:744-747; pmnet.c:117).
- Half-open after switch-off/on on PPP: `SwitchOnCheck` asks NIFMAN/the stack and marks the socket closed.
- Disk full / card out: `pm_write_why` names the drive; message, index, folder list, calendar and undo files go through temp+Replace; Get file and Log to file report the error code class (ptxfer.cpp:944-970, :845-868); compose keeps the draft on screen if it cannot be saved (psimail.cpp:3699-3725); PsiMail's update removes a half file and checks fclose.
- Corrupt settings: PsiMail checks magic, size and the bytes read; PsiTerm parses field by field with bounds; PsiLink.ini likewise.
- Stack: psimail.exe 64 KB with static buffers in imap.c/smtp.c; the SFTP client uses statics.
- IMAP state after a drop: `lost()` resets `g_acct`/`g_sel`, `imap_open` re-logs in and NOOPs an idle connection with a 15 s limit; `pmn_conn_id` guards against SMTP/HTTP having taken the line.
- fetch_small (psishim.c:827-853; pmupdate.c:189-212): a short reply is retried 3 times on a fresh connection, and the signature file is only parsed when complete.
