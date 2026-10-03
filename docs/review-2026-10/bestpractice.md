# PsiTerm and PsiMail: EPOC R5 best-practice review

Read-only review of `/home/daniel/www/psion/psiterm` (branch `dev`), 3 October 2026.
Scope: PsiTerm (`app/`, `ssh/`) and PsiMail (`mail/app`, `mail/engine`), plus the
shared `ssh/psiglue.cpp` and `ssh/tls13.c`. PsiWeb was not reviewed.

Method: the project's own rule lists (`docs/epoc-robustness-best-practices.md`,
`docs/epoc-comms-best-practices.md`) were checked against the code, together with the
ER5 SDK (`sysdoc/cpp/e32/euasyn`, `euactv`, `eudesc`, `commapi/csapi-003`,
`esock/essock`, `f32/fsusing`, `stdlib/slport`; headers `e32base.h`, `e32const.h`,
`coeaui.h`). Every finding below was verified by reading the code; items marked
**UNCERTAIN** depend on behaviour the SDK does not state or on a precondition I could
not confirm. Line numbers are as of commit `ee55128`.

Overall: the suite is well above the usual standard for ER5 code. Cleanup-stack use,
two-phase construction, `Mid`/`Left` guards, shared-chunk termination, the IMAP/MIME/
HTML/ICS parsers and the SFTP wire parsing are sound (details under "Checked and found
sound"). The real problems are concentrated in a few places: the request-semaphore
bookkeeping in `psiglue.cpp`, one unbounded path into `att_stream`, a `RunError` that
ER5 never calls, and PsiMail's settings file being rewritten in place.

Severity scale: **panic** (the app or engine is closed by the kernel), **hang**,
**data loss**, **leak**, **security**, **minor**.

---

## Findings, most severe first

### 1. Engine hang: a receive completion's signal is consumed by a two-status wait, then `WaitForRequest(gRecvStat)` blocks for ever
- **Rule:** SDK `e32/euasyn` (each completion adds one count to the thread's request semaphore; `User::WaitForRequest(s)` always takes one count, so a status whose count was taken elsewhere must not be waited on again). Robustness doc §3.
- **Severity:** hang.
- **Where:** `/home/daniel/www/psion/psiterm/ssh/psiglue.cpp:238-258` (`WaitFor`), `:881-897` (`RxFill`), `:494-498` (`NetCloseSocket`).
- **Scenario (Psion Internet route, SSH or TLS):** `pg_wait` (`:1537`) calls `RxFill`, which leaves a `RecvOneOrMore` outstanding (`gRecvPending = 1`, `:881-885`) when its slice ends. The C code then sends (`NetWrite`, `:~905`), which calls `WaitFor(stat, 60 s)`. `WaitFor` waits with `User::WaitForRequest(aStat, timerStat)` (`:238`). If server data arrives during that wait, the receive's semaphore count is taken by the two-status wait (on ER5 it loops internally until one of *its* two statuses completes; either way the count is gone). The "neither" branch at `:247-258` only looks at the lookup/connect/shutdown orphans, not at `gRecvStat`. The next `RxFill` finds `gRecvStat != KRequestPending` and calls the one-argument `User::WaitForRequest(gRecvStat)` (`:896`), which first does `WaitForAnyRequest` and, with nothing else outstanding, never returns. The file's own comment at `:888-890` describes this exact failure ("the hang after Securing the connection"); the two-status wait is the remaining way in. `NetCloseSocket:494-498` has the same hole (`CancelRecv` finds nothing to cancel, then blocks).
- **Fix:** in `WaitFor`, after every return from the two-status wait, if `gRecvPending && &aStat != &gRecvStat && gRecvStat != KRequestPending` set a `gRecvTaken` flag; `RxFill`/`NetCloseSocket` skip the `WaitForRequest` when it is set. Alternatively, never wait on anything else while a receive is pending (cancel and take it first).

### 2. Engine crash: `att_stream` overflows its 1100-byte buffer when a body arrives as a quoted string
- **Rule:** robustness doc §12 (every length from the wire is an attacker's number; decoders must document and keep their output bound).
- **Severity:** panic (KERN-EXEC 3 or silent .bss corruption in psimail.exe).
- **Where:** `/home/daniel/www/psion/psiterm/mail/engine/imap.c:1422-1436` (`att_stream`, `static char dec[1100]`), reached from `:920-924` (`on_piece_quoted` -> `piece_stream(v->s, v->len, ctx)`) and `:1211-1219` (`body_item`).
- **Scenario:** `fetch_part` streams literals in 1 KB pieces, which is what `dec[1100]` was sized for. If the server answers a `BODY.PEEK[n]<a.b>` fetch with the part as a **quoted string** (`* 1 FETCH (UID 1 BODY[2]<0> "AAAA...")`), `body_item` returns the whole `IT_STRING` node and `piece_stream` hands its full length to `att_stream` in one call. The line can be up to `RESP_MAX - 16` = ~49 KB (`:20`, `:97`), and `dec_feed` in 7-bit mode is a `memcpy` of `n` bytes. Affects attachments (`imap_attach`) and pictures (`imap_part_to_file`); the message-text path only `fwrite`s and is safe. A malicious server, or an unusual one that quotes short parts, triggers it.
- **Fix:** in `on_piece_quoted`, loop over `v->s` in slices of at most 1024 bytes so every `StreamFn` sees the same bound the literal path guarantees; or size-check `n` in `att_stream` and fail the download.

### 3. Calendar sync is left stuck, with the Agenda open, after any leave: `RunError` does not exist on ER5
- **Rule:** SDK `e32base.h:804-834` (ER5's `CActive` declares only `RunL` and `DoCancel`; a leave from `RunL` goes to `CActiveScheduler::Error`, which CONE routes to `CCoeEnv::HandleError`). Robustness doc §3 ("ER5 has no `RunError`... keep `RunL` leave-safe or trap inside it").
- **Severity:** hang (the feature) + leak.
- **Where:** `/home/daniel/www/psion/psiterm/mail/app/pmcal.cpp:229-256` (`CPmCalSync::RunError`), `:258-295` (`RunL`, untrapped), `/home/daniel/www/psion/psiterm/mail/app/pmcal.h:95`.
- **Scenario:** `RunL` calls `OpenL`, `ReadPushedL`, `MappedStepL`, `NewStepL`, `LocalStepL`, `FinishL` with no `TRAP`. Any leave (Agenda file missing: `OpenL:697` leaves `KErrNotFound` deliberately; OOM; a corrupt `agenda.txt`) goes to EIKON, which shows a generic error dialog. `RunError` is never called, so `Close()`/`Reset()` never run: `iPhase` stays non-idle, so `Running()` is true for the rest of the session and `StartL:206` returns silently on every later sync; the `RAgendaServ` session, `CAgnEntryModel` (Agenda file held open), `iFs` session and the event/map arrays stay allocated; `CalSyncDone` is never delivered so `iCalBusy` text persists (`pmstatus.cpp:120-132`). The user sees "Calendar sync" do nothing until PsiMail is restarted.
- **Fix:** rename `RunError` to a private `Failed(TInt)` and call it from `RunL` under a `TRAPD`: `TRAPD(err, StepL()); if (err) { Failed(err); return; }`. Keep the `RunError` name out of the class so the next reader does not assume it works.

### 4. PsiMail's settings (all accounts and passwords) are rewritten in place with unchecked writes
- **Rule:** SDK `f32/fsusing` ("full disk" and media removal), robustness doc §4 ("Write to a temporary and `RFs::Replace` it over the real file so a failure never leaves a truncated settings/index file"); PsiTerm already does this (`app/psiterm.cpp:102-118` `SafeWrite`, fix #8 of the September review).
- **Severity:** data loss.
- **Where:** `/home/daniel/www/psion/psiterm/mail/app/psimail.cpp:3409-3425` (`SaveSettings`), `:3504-3520` (`SaveCalSettings`), `:4107-4113` (`Update.ini`).
- **Scenario:** `file.Replace(fs, KIniFile, ...)` truncates `PsiMail.ini` to zero, then three `Write`s go unchecked. A flat battery, a card removal (if the ini is on C: this is only the battery) or a full C: between the `Replace` and the last `Write` leaves a short file. On the next start `LoadSettings:3393-3398` reads the magic and size correctly, then `file.Read(s)` returns a short descriptor and the code does `Mem::FillZ(&iSettings, ...)`: every account, server, password and preference is gone and the first-run dialog appears. `SaveSettings` is called on many ordinary actions (toolbar toggle, offline toggle, every exit), so the window is hit regularly.
- **Fix:** write to `PsiMail.ini~`, check each `Write` and `Flush`, `Close`, then `fs.Replace(tmp, KIniFile)`; same for `Calendar.ini` and `Update.ini`. Lift `SafeWrite` from `psiterm.cpp` into a shared helper.

### 5. The Connection "Test" and "Connect" block the UI thread for minutes, and one path waits with no limit
- **Rule:** comms doc N5 (every `GetByName` under its own timer), robustness doc §3 (synchronous waits are for the engines; every wait bounded) and §5 (apps must close promptly when the System screen asks).
- **Severity:** hang (UI thread).
- **Where:** `/home/daniel/www/psion/psiterm/ssh/psiglue.cpp:2743-2748` (`LtLookup`), `/home/daniel/www/psion/psiterm/ssh/pglinktest.cpp:84-92` (`pg_link_test` called synchronously from the dialog; `app/psiterm.cpp:4893`, `mail/app/psimail.cpp:3034`).
- **Scenario:** `LtLookup` waits 90 s for the lookup, cancels, waits 10 s more, then closes the resolver and session and does `User::WaitForRequest(look)` with **no timer**. A lookup that has started NetDial (its dialog, or a modem script) does not complete on `Cancel` or session close (psiglue's own experience at `:80-83` and `:1789-1796`), so the app freezes with no repaint and no Esc. Even without that, the worst-case sums are about 24 s on the modem route and about 141 s on Psion Internet with a dial, all in the UI thread with no event processing. In PsiTerm the dialog is not dimmed while SSH is active (`app/psiterm.cpp:6025-6031`), and the app's heartbeat comes from the periodic `iPump` that the Test starves, so a Test longer than 45 s makes psissh's `AppGone` (`psiglue.cpp:1327-1351`) end the live session. Window-server completions arriving during these waits also lose their semaphore count (the apps' tick timers mask this as a delay, not a panic).
- **Fix:** run the test in a worker thread with its own `RCommServ`/`RSocketServ` (N14 is satisfied if that thread opens the sessions) and poll from a `CPeriodic` in the dialog, with Esc cancelling; at minimum cap the lookup at 20 s and keep `look`, `entry`, `res` and `ss` in a heap block that is deliberately leaked when abandoned, so a late completion lands in live memory instead of blocking.

### 6. After an SFTP timeout the next transfer is failed at once as "This server does not offer file transfer"
- **Rule:** comms doc N5/N6 (bounded waits and a clean restart after a failure); general state-machine hygiene.
- **Severity:** hang-like (wrong outcome until the server's late reply arrives).
- **Where:** `/home/daniel/www/psion/psiterm/ssh/sftp.c:940-945` with `:363-377` (`close_channel`) and `:493-535` (`start_op`); same pattern in `/home/daniel/www/psion/psiterm/ssh/tmuxq.c:244-261`.
- **Scenario:** on a stuck link the 60 s test fires, `finish(PSI_XFER_TIMEOUT)`, `close_channel()` sets `CH_CLOSING`. Neither `close_channel` nor `start_op` refreshes `last_heard`, and `start_op` only opens a channel when `ch_state == CH_NONE`. The next request is accepted, `ch_state != CH_READY` with a stale `last_heard` trips the same test on the next loop turn, and `finish(PSI_XFER_NO_SFTP)` is reported, on which `BrowseL` (`app/ptxfer.cpp:646-658`) gives up. It self-heals only when the server's CHANNEL_CLOSE arrives. In `tmuxq.c` a timeout while `ch == NULL` leaves `TQ_OPENING` for ever, so every later tab query returns `PSI_TQ_TIMEOUT`.
- **Fix:** set `last_heard = time(NULL)` in `close_channel()` and `start_op()`; when timing out with `ch == NULL`, reset to `CH_NONE`/`TQ_NONE`; report `PSI_XFER_TIMEOUT`, not `NO_SFTP`, when the state is `CH_CLOSING`.

### 7. A remote folder name of 120 or more characters panics PsiTerm (USER 11)
- **Rule:** SDK `e32/eudesc` (`Append` panics on overflow); robustness doc §2.
- **Severity:** panic.
- **Where:** `/home/daniel/www/psion/psiterm/app/ptxfer.cpp:483-491`, with `Utf8ToText` at `:44-48`.
- **Scenario:** `Utf8ToText` fills `name` until `Length() == MaxLength()` (120). For a directory (`e.iType == 'd'`) the code then does `name.Append('/')`, which panics when the name filled the buffer. SFTP names are up to 255 bytes; one long directory name in a listing closes the app while the list is built.
- **Fix:** `if (name.Length() == name.MaxLength()) name.SetLength(name.MaxLength() - 1);` before the append, or clip to 100 (the row only uses 100).

### 8. PsiMail zeroes the shared chunk without checking for a still-running engine
- **Rule:** robustness doc §7 (an engine must notice its app dying; a process the app launched is killed on exit) and the precedent in PsiTerm (`app/psiterm.cpp:3588-3603`, fix #5 of the September review).
- **Severity:** hang/leak (two engines on one chunk; serial port held by the old one).
- **Where:** `/home/daniel/www/psion/psiterm/mail/app/psimail.cpp:271-277` (`ConstructL`: `OpenGlobal` then `Mem::FillZ`), `:397-401` (`StartEngineL`: `Mem::FillZ` again), engine side `/home/daniel/www/psion/psiterm/mail/engine/pmepoc.cpp:296-311` (`AppGone`) and `:326-340`.
- **Scenario:** PsiMail crashes or is killed from the task list; its engine lives on for up to 20 s of missing heartbeat before `AppGone` says the app's process has gone. If the user restarts PsiMail inside that window, `CreateGlobal` returns `KErrAlreadyExists`, the app opens the engine's chunk and `FillZ`es it, writes its own `app_pid` and starts beating, and launches a second `psimail.exe`. The old engine now sees a live app and a fresh beat (`:326-329`), so it carries on: two engines share one command queue and ring buffers, and the old one holds the serial port (the new one gets `KErrInUse`). PsiTerm handles the same case by asking the old process to quit first.
- **Fix:** in `ConstructL`, after `OpenGlobal`, if `s->magic == PM_MAGIC && s->state != PM_STATE_EXITED` set `s->net.quit = s->quitting = 1`, wait a bounded time for `PM_STATE_EXITED` (ideally from a `CPeriodic`, not `User::After`), then `FillZ`.

### 9. `Format` into 160 bytes with a 128-byte host name panics the engine on the lookup-timeout path
- **Rule:** robustness doc §2 (`Format` panics rather than truncates).
- **Severity:** panic (low likelihood: only a host over 74 characters).
- **Where:** `/home/daniel/www/psion/psiterm/ssh/psiglue.cpp:780-785` and `:795-799`; `PsiShared::host` is `char[128]` (`ssh/psishared.h:45`).
- **Scenario:** the fixed text at `:782` is 83 characters; a host of 75+ characters overflows `m[160]` (120+ at `:797`). PsiTerm and PsiMail clip hosts to 63, so today only PsiWeb can supply one; still an unbounded `Format` of a shared-chunk field.
- **Fix:** clip the host to 60 characters as `:762` does, or use `snprintf`.

### 10. Orphan-reaping can block for ever (UNCERTAIN: depends on ER5's two-status `WaitForRequest`)
- **Rule:** SDK `e32/euasyn`, as finding 1.
- **Severity:** hang.
- **Where:** `/home/daniel/www/psion/psiterm/ssh/psiglue.cpp:247-258`, `:208-211`, `:274-281`.
- **Scenario:** if `User::WaitForRequest(s1, s2)` loops internally until one of *its* statuses completes (EKA1's euser does), the "neither" branch at `:247-258` is dead and an orphaned shutdown that completes during a later `WaitLink` never clears `gShutOrphan` (which, unlike the lookup/connect orphans, `pg_dial` does not refuse on). The next `ReapOrphans` -> `WaitFor(gShutStat)` -> `:210` `User::WaitForRequest` blocks with its count already taken; in `NetClose` that is after the session is closed, so nothing can wake it. Trigger: a FIN unanswered over a dead PPP link, reconnect, TCP gives up during the 30-60 s `WaitLink`, later `pg_close` hangs.
- **Fix:** re-check all three orphan statuses after every two-status wait regardless of which branch ran, and record "signal taken" per status instead of assuming a count exists at `:208`.

### 11. `account.txt` is rewritten in place and unchecked, and a partial file wipes the account's store (UNCERTAIN trigger)
- **Rule:** robustness doc §4 (temp + `pm_replace`, check `pm_fclose`).
- **Severity:** data loss (outbox and pending offline changes).
- **Where:** `/home/daniel/www/psion/psiterm/mail/engine/store.c:71-77` (`st_check_account`), `:308-318` (`st_pin_save`).
- **Scenario:** `fopen(path, "w")` + `fprintf` + unchecked `fclose`. A partial write (card pulled or full mid-write) leaves `have` non-empty and `!= want`, and the next start does `pm_rmtree(dir)` on the whole `A<n>\` tree including `outbox\` and `pending.txt`. An empty file is safe, so this needs a genuinely partial write. `pins.txt` has the milder effect that a truncated line turns a first-time prompt into a "certificate changed" warning.
- **Fix:** write both via temp + `pm_replace` + `pm_fclose` as `st_index_save` does; only `pm_rmtree` when `have` looks like `user@host`.

### 12. Blocking `User::After` loops in the UI thread (accepted by the project, still a violation)
- **Rule:** robustness doc §3 and §5 (the UI thread must not block; close promptly); SDK `cone/coappui`.
- **Severity:** minor (3-6 s freezes, no busy message, no Esc).
- **Where:** `/home/daniel/www/psion/psiterm/mail/app/psimail.cpp:479-480` (`StopEngine`, up to 6 s on every exit and before the installer); `/home/daniel/www/psion/psiterm/app/psiterm.cpp:356-357` (destructor, 3 s), `:1006-1009` (`WriteToHost`, 2 s), `:2680-2682` (`HangUp`, 2.2 s), `:3599-3601` (`LaunchSshL`, 5.5 s), `:266-274` (`CSerialPort::Write`, a synchronous 3 s write per keystroke when CTS is low); `/home/daniel/www/psion/psiterm/app/ptxfer.cpp:282-291` (400 ms).
- **Fix:** where the wait is a user-visible one (exit, launch) show a `BusyMsgL` first; for `StopEngine` on exit, drive the wait from a `CPeriodic` and call `Exit()` from its callback.

### 13. TLS: handshake secrets are not wiped on failure, traffic keys never
- **Rule:** the file's own intent (`memset` at `tls13.c:388, 392, 466, 468`); general key hygiene.
- **Severity:** security/minor (separate process, single-user device).
- **Where:** `/home/daniel/www/psion/psiterm/ssh/tls13.c:277-278, 386-398, 444-476`.
- **Scenario:** every `goto fail` after `:298` skips the memsets, leaving `priv`, `shared`, `hs_secret`, `c_hs`, `s_hs`, `derived`, `master` on the stack; on success `c_hs`, `s_hs`, `derived`, `c_ap`, `s_ap`, `fkey` are not cleared and `g_rd`/`g_wr` keep the application keys after the connection (no `tls_close`).
- **Fix:** one cleanup block reached from both paths; add `tls_close()` zeroing `g_rd`, `g_wr`, `g_app`.

### 14. TLS: plaintext handshake and alert records are accepted after the keys are on
- **Rule:** RFC 8446 §5 (after ServerHello everything but CCS is encrypted).
- **Severity:** security/minor.
- **Where:** `/home/daniel/www/psion/psiterm/ssh/tls13.c:236-240`.
- **Scenario:** `if (!g_encrypted || hdr[0] != 23) { *len = n; return hdr[0]; }` lets an on-path attacker inject a plaintext alert (type 21), which `tls_read` reports as a clean close, or plaintext handshake bytes into the transcript (Finished then fails). No key exposure.
- **Fix:** once `g_encrypted`, treat any record type other than 20 and 23 as a bad record.

### 15. Certificate checks: embedded NUL in dNSName, bare wildcard suffix, unparsable dates, intermediates' validity
- **Rule:** RFC 6125 §6.4.3, RFC 5280 §6.1 (validity of every certificate in the path).
- **Severity:** security/minor (each needs a CA-issued certificate).
- **Where:** `/home/daniel/www/psion/psiterm/mail/engine/certcheck.c:314-330` (name match), `:113-129` (`der_time` returns 0 when unreadable), `:478-486` (`not_after &&` disables the check on 0), `:399-436` (`chain_ok` checks only the leaf's dates; cached intermediates in `certs.txt`, `:384-396`, are trusted by hash for ever).
- **Scenario:** `imap.fastmail.com\0.evil` matches after `memcpy`+NUL; `*.com` matches `fastmail.com`; a date the parser cannot read disables that bound.
- **Fix:** reject names containing NUL, require a dot inside the wildcard suffix, treat an unparsable date as invalid, check `not_after` of every certificate in `chain_ok`.

### 16. Unbounded RSA public exponent: a hostile chain stalls the engine for minutes
- **Rule:** robustness doc §12 (bounded work on attacker input).
- **Severity:** hang (DoS)/minor.
- **Where:** `/home/daniel/www/psion/psiterm/mail/engine/pmrsa.c:12-16`, `/home/daniel/www/psion/psiterm/mail/engine/certcheck.c:189-191`.
- **Scenario:** `nlen` and `siglen` are bounded, `elen` is not. A 4096-bit check with e = 65537 already takes seconds (`:341-342`); a MITM chain with a 4096-bit exponent costs minutes per `verify_pkcs1`, up to six times in `chain_ok`, with no cancel.
- **Fix:** reject `elen > 4` (and even or `< 3` exponents) in `parse_cert`/`rsa_public`.

### 17. A download that ends early is reported as saved (UNCERTAIN how reachable)
- **Severity:** data loss/minor.
- **Where:** `/home/daniel/www/psion/psiterm/ssh/sftp.c:734-738` with `:727`, `:810-811`; `/home/daniel/www/psion/psiterm/app/ptxfer.cpp:862`.
- **Scenario:** for GET, STATUS EOF or a zero-length DATA closes the handle and `finish(PSI_XFER_OK)` without comparing `done_bytes` with the `total` from STAT. A file that shrinks mid-transfer or a server that answers EOF early leaves a short file, which `PtGetFileL` renames over the real one.
- **Fix:** in K_CLOSE, `if (op == PSI_XOP_GET && total && total != 0xffffffffUL && done_bytes < total) set_fail(PSI_XFER_FAILED, "the file was cut short")`.

### 18. Stop pressed after the final CLOSE was sent is reported as "Transfer stopped"
- **Severity:** minor.
- **Where:** `/home/daniel/www/psion/psiterm/ssh/sftp.c:929-935` vs `:595-600`.
- **Scenario:** once `pump()` has sent `FXP_CLOSE`, `have_handle = 0`; a Stop in that window finishes as CANCELLED although every byte went (PUT: the complete file stays on the server but the user is told it stopped; GET: the complete temp file is deleted at `ptxfer.cpp:858`).
- **Fix:** also wait while `count_reqs(K_CLOSE) > 0`.

### 19. Large stack frames in the UI apps (UNCERTAIN: the EIKON app thread's stack size is not stated in the SDK; `e32const.h:46` gives 8 KB as the default thread stack, and neither `.mmp` sets `epocstacksize`)
- **Rule:** SDK `stdlib/slport` "Stack usage", `tools/tlmakmak` (default 8 KB); robustness doc §9.
- **Severity:** panic (KERN-EXEC 3) if the stack is 8 KB; nothing if it is 20 KB.
- **Where:** `/home/daniel/www/psion/psiterm/mail/app/pmheader.cpp:428-429, 465, 545, 598, 617` (`HeaderTextL`: `TBuf<300>` x3, `TBuf<500>` x2, `TBuf<96>`, `TBuf<300>` x2 in nested scopes: about 5.3 KB in one frame, under CONE/EIKON drawing frames); `/home/daniel/www/psion/psiterm/app/ptxfer.cpp:730-779` + `:631-678` + `:461-484` (`PtSendFileL` -> `BrowseL` -> `ExecuteLD` -> `PreLayoutDynInitL`: about 4.7 KB live at once); `/home/daniel/www/psion/psiterm/mail/app/psimail.cpp:1712` (`TBuf<600>`), `/home/daniel/www/psion/psiterm/mail/app/pmvcard.cpp:434` (`TBuf8<600>`).
- **Fix:** move the big scratch descriptors to `HBufC`s or to members of the view (allocated once), and set `epocstacksize` explicitly in both `.mmp`s so the number is known.

### 20. `RFs` session opened per call instead of `CEikonEnv::FsSession()`
- **Rule:** robustness doc §4 ("`RFs` sessions are expensive: use `CEikonEnv::FsSession()` in the app"); SDK `f32/fssess`.
- **Severity:** minor (a server session and a few ms per call; all are closed correctly, `ForgetL` and `AgendaMatchL` have no leaving call between `Connect` and `Close`).
- **Where:** `/home/daniel/www/psion/psiterm/mail/app/pmcal.cpp:557-559, 598-600, 615-623, 1161-1167`; `/home/daniel/www/psion/psiterm/mail/app/pminvite.cpp:358-362`.
- **Fix:** pass `CEikonEnv::Static()->FsSession()` in.

### 21. Other files rewritten in place
- **Rule:** robustness doc §4.
- **Severity:** minor (each read side validates, so the loss is one file's last state).
- **Where:** `/home/daniel/www/psion/psiterm/mail/app/pmcal.cpp:606-609` (`default.txt`, writes unchecked), `:673-680` (`push.txt`: unsent Agenda changes, so this one matters more), `/home/daniel/www/psion/psiterm/app/ptxfer.cpp:1202-1206` (`Log.ini`).
- **Fix:** use the temp + `Replace` helper (`pmcal.cpp:498-530` already does it for `agenda.txt`).

### 22. `dec_feed`'s "output never longer than input" contract is false for quoted-printable across calls
- **Severity:** minor (latent: both streaming callers have headroom today).
- **Where:** `/home/daniel/www/psion/psiterm/mail/engine/mime.c:40-41, 55-75`; `mail/engine/pm.h:118`.
- **Scenario:** a QP stream split as `"="`, `"Z"`, `"Z"` across three calls yields 0, 0, then 3 bytes for 1 input byte: a call can emit `n + 2`. Robustness doc §12 already says `n + 2`; the code comment and `pm.h` say `n`.
- **Fix:** correct the comment and `pm.h`, or hold the pending bytes until a full `=XY` is known.

### 23. `just_path` reads one byte past the terminator for an href of exactly `http://`
- **Severity:** minor (read overrun of an uninitialised `t[200]`).
- **Where:** `/home/daniel/www/psion/psiterm/mail/engine/caldav.c:67-75`.
- **Fix:** skip the scheme by its real length (7 or 8) before `strchr(p, '/')`.

### 24. `xmlscan` swallows the rest of a CalDAV reply after an unterminated `&`
- **Severity:** minor (silent loss of later `response` elements on malformed XML).
- **Where:** `/home/daniel/www/psion/psiterm/mail/engine/xmlscan.c:119-122`.
- **Fix:** on `<`, a space, or the cap, emit the text literally and return to `S_TEXT`, as `html.c:636-640` does.

### 25. Inconsistent margins after a kept in-line IMAP literal drop the rest of the line
- **Severity:** minor (no overflow: `pmnet.c:271-283` handles a non-positive `max` by writing `buf[0] = 0`).
- **Where:** `/home/daniel/www/psion/psiterm/mail/engine/imap.c:97, 112, 127-136`.
- **Scenario:** a literal is kept when `n <= RESP_MAX - g_rlen - 8`, leaving `g_rlen` up to `RESP_MAX - 6`; the next `pmn_readline` gets `max <= 0` and the trailing `FLAGS`/`UID` of the FETCH are thrown away.
- **Fix:** keep only when `n <= RESP_MAX - g_rlen - 18`.

### 26. `JoinDir` truncates a long remote path silently
- **Severity:** minor (a PUT lands under a cut name; a GET stats a name that does not exist).
- **Where:** `/home/daniel/www/psion/psiterm/app/ptxfer.cpp:136-143` (`xfer_remote` is 512).
- **Fix:** infoprint "Not available - the name is too long" and skip when it does not fit.

### 27. `Format` of a `TFileName` into a smaller `TBuf`
- **Rule:** robustness doc §2.
- **Severity:** minor (needs an app installed under a path longer than about 95 characters).
- **Where:** `/home/daniel/www/psion/psiterm/app/psiterm.cpp:821-822` (`TBuf<160>` with `iUpdateFile`, a `TFileName`); `/home/daniel/www/psion/psiterm/mail/app/psimail.cpp:4059-4060` (`TBuf<200>` with `aFile`); `/home/daniel/www/psion/psiterm/mail/app/pmstore.cpp:114` (`TBuf<160>` with three directory names).
- **Fix:** `Clip` the path, or use `TFileName`-sized buffers.

### 28. Folder listing uses an O(n^2) insertion sort
- **Severity:** minor (seconds of frozen UI on `/usr/bin`-sized listings at 36 MHz).
- **Where:** `/home/daniel/www/psion/psiterm/app/ptxfer.cpp:416-420`.
- **Fix:** append, then sort once (`User::QuickSort` over an index array).

### 29. `psi_vsnprintf`/`psi_vfprintf` format into fixed 4 KB statics before applying `size` (UNCERTAIN exploitability)
- **Severity:** minor (every current caller is bounded by Dropbear's own limits, so the bound lives in Dropbear, not here).
- **Where:** `/home/daniel/www/psion/psiterm/ssh/psishim.c:113-124, 135-144`.
- **Fix:** check `n >= sizeof(tmp)` and clip, or use estlib `vsnprintf` if present.

### 30. Smaller points
- `CloseSTDLIB()` is never called in `psimail.exe` (`/home/daniel/www/psion/psiterm/mail/engine/pmepoc.cpp:351-379`) or `psissh.exe`. Robustness doc §8; harmless at process exit apart from debug heap checks. UNCERTAIN whether ESTLIB's `E32Main` wrapper does it.
- `certcheck.c:84, 89, 93, 448, 451, 456` use the pointer form `p + len > end` that robustness doc §12 forbids; the buffers are static arrays well below the top of the address space, so no wrap today.
- `LtSetConfig`'s result is ignored at `psiglue.cpp:2593` and `:2652`, so a failed reconfigure tests at the wrong rate.
- Documentation drift: comms doc S14 says "16 KB reads" but the modem route reads 1 KB (`psiglue.cpp:38, 1415-1417`); N11 should say that on the net route `pg_hangup` (`:1970-1978`) keeps NIFMAN's timers off for up to `IDLE_RELEASE_NET_MS` (20 min, `mail/engine/pmnet.c:51`) after the last activity, by design.
- RNG seeding for PsiMail/PsiWeb on first run (`psishim.c:582-604`, `psiglue.cpp:2002-2036`) rests mostly on spin-count jitter between 64 Hz ticks plus time/tick/free-RAM; PsiTerm adds keystroke timings. UNCERTAIN how much entropy that is; stirring the server random and record arrival times into the pool per connection would help. Dropbear's `dbrandom.c` is fetched at build time and was not read.
- `EApaSystemEventBackupStarting` is not handled by either app (`eikappui.h`, robustness doc §5 says files must be closed for it); PsiTerm's log and the engine's `psimail.log`/store files stay open during a PsiWin backup. Minor, UNCERTAIN what the ER5 backup engine does with an open file.

---

## Checked and found sound

- **Active objects:** `CSerialPort`, `CSshWatcher`, `CPmWatcher` and `CPmCalSync` all `Cancel()` in their destructors and implement `DoCancel` for the service they asked for (`ReadCancel`, `LogonCancel`; `CPmCalSync` self-completes so an empty `DoCancel` is correct). `Watch()` is never called while active. Every `CPeriodic`/`CIdle` is cancelled and deleted by its owner; `CPmView::Tick` traps `TickL`; PsiTerm's `Tick` only calls non-leaving code. Timed checks use a `CPeriodic` against wall-clock time (`pmauto.cpp`), so switch-off does not break them. `HandleSwitchOnEventL` is overridden in both apps (`coeaui.h:73` confirms it is virtual).
- **Cleanup stack and construction:** no `new (ELeave)` or leaving call in any constructor; `NewL`/`NewLC` patterns correct; `PushL`/`PopAndDestroy` counts agree on the paths read (`pmcal.cpp`, `pminvite.cpp`, `pmcontacts.cpp`, `ptxfer.cpp`, `psimail.cpp` draft save); `ExecuteLD` objects are never touched afterwards.
- **Descriptors:** `Clip`/`SafeCopy`/`LeftSafe`/`CopyToC` are used for anything of unknown length; every `Mid`/`Left` I sampled is guarded by a `Locate`/`Length` test (`psiterm.cpp:960-963, 1817-1821, 3354-3355`; `pmnative.cpp:1022-1025`; `pmheader.cpp:296-304`; `psimail.cpp:108-116`); shared-chunk `char[N]` fields are zero-filled by the app and re-terminated by the engines before `strlen`; ring buffers are power-of-two with unsigned head/tail arithmetic.
- **Files:** PsiTerm's settings, hosts, keys and snippets go through `SafeWrite` (temp + `Replace` + `Flush`); PsiMail's drafts (`psimail.cpp:1686-1761`), `agenda.txt`, and the engine's index/folders/pending/events/calendars use temp + `pm_fclose` + `pm_replace`; `.pmi` headers are size-checked before use (`pmpict.cpp:231-233`); settings are read with a magic, a size and a length check.
- **Processes and chunks:** both apps `Logon` to their engine, read `ExitType`/`ExitCategory`/`ExitReason`, `Kill` after a bounded wait, call `PsiLinkTimersBack()` when the engine did not get to undo `DisableTimers`, and close the `RProcess` and `RChunk` in the destructor. The engine checks the app by `RProcess::Open` + `ExitType`, never by heartbeat alone (`pmepoc.cpp:296-311`). Priorities are `EPriorityBackground`/`EPriorityHigh` for the engine process and `EPriorityAbsoluteBackground` for the thread while decoding; nothing uses `EPriorityRealTime`.
- **Serial (S1-S15):** port brought up in the documented order; a zero-length read after `Open`+`SetConfig` (`psiglue.cpp:1079-1084`, Test `:2563-2566`); every `SetConfig`/`ResetBuffers` has no I/O pending (all engine serial I/O is synchronous and the Test cancels and waits first; SDK `csapi-003` wording confirmed); `KConfigObeyCTS` or none, never XON/XOFF; DCD armed only after being seen high; `KErrInUse` retried 3 x 300 ms; `KeepAwake` only while data flows, rate-limited to 30 s.
- **Sockets (N1-N14):** `RecvOneOrMore`; one outstanding request per kind (`gRecvPending`, orphan refusals); async shutdown under a 3 s timer with a static status; close order socket, resolver, NIFMAN, session; `DisableTimers(ETrue)` only after a successful connect and undone in `NetClose` on quit, idle, hang-up, failure and link-dropped paths; an empty host refused; all local `TRequestStatus` objects outlive their requests.
- **Parsers:** `imapparse.c` depth cap 40 on the only recursive path; `mime.c` depth 8; `html.c`, `ics.c`, `xmlscan.c`, `charset.c`, `http.c` have no recursion; `{n}` literals compared as lengths and NUL-terminated; every loop advances; header and address buffers clamped after each `snprintf`; `malloc`/`realloc` checked everywhere; big buffers static, deepest engine chains a few KB against 64 KB (psimail) and 128 KB (psissh). SFTP: `rd32`/`rdstr` cursor with unsigned `n > left`, handle length <= 256, REALPATH length checked, late replies dropped by id, local file closed and temp removed on every `finish`. tmuxq: session ids validated before use in a shell command, output consumed by length not `strlen`.
- **TLS lengths:** record `<= REC_MAX - 5`, handshake accumulation `<= HS_MAX`, extension offsets on ints, certificate list and `CHAIN_MAX` bounded, RSA sizes bounded, AEAD tag compared in constant time, PSS encoding fully checked.

## Sources
- SDK: `epoc_cpp_sdk/sysdoc/cpp/e32/euasyn.html`, `euactv.html`, `eudesc*.html`, `euclnp.html`; `cpp/commapi/csapi-000.html`, `csapi-003.html`; `cpp/esock/essock.html`, `esserv.html`, `eshnres.html`; `cpp/f32/fsusing.html`, `fssess.html`; `cpp/stdlib/slport.html`, `slimplem.html`; `cpp/tools/tlmakmak.html`; headers `epoc32/include/e32base.h` (lines 804-834: `CActive` without `RunError`), `e32const.h:46` (`KDefaultStackSize = 0x2000`), `coeaui.h:72-74`, `coemain.h:97`, `apgtask.h:18-20`, `eikappui.h`.
- Project: `docs/epoc-robustness-best-practices.md`, `docs/epoc-comms-best-practices.md`, `docs/REVIEW-2026-09.md`.
- RFC 8446 §5 (record layer), RFC 6125 §6.4.3 (wildcards), RFC 5280 §6.1 (path validation).
