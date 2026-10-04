# Fix log: PsiMail (`mail/`)

Findings from `recovery.md` (R), `bestpractice.md` (B) and `eikon.md` (E) whose code is under `mail/`. Paths are under `/home/daniel/www/psion/psiterm/`. Nothing is committed; `dist/` untouched; versions not bumped.

## Engine (`mail/engine`, `mail/psimail.h`)

| Finding | Change | Verification |
|---|---|---|
| B2 `att_stream` overflow from a quoted BODY[] | `imap.c` `on_piece_quoted` slices a quoted string into `PIECE_SLICE` (1 KB) pieces, the same bound the literal path gives every `StreamFn`; `att_stream` refuses `n > PIECE_SLICE` as a guard. `body_stream` is fed from a spool file in its own buffer, so it was already bounded. | host build; parsefuzz, foldertest, undotest, invitehost pass |
| B25 in-line literal margin | `imap.c` `read_response`: a literal is kept in line only if `KEEP_MARGIN` (512) bytes remain for the rest of the line (was 8, which left `pmn_readline` a non-positive max and dropped the trailing FLAGS/UID). | host tests |
| R8 `pm_replace` KErrInUse blamed on the card | `pmepoc.cpp` `pm_replace`: 30 tries × 100 ms (3 s, was 5 × 60 ms) on KErrInUse/KErrAccessDenied; sets `pm_replace_busy` (defined in `pmmain.c`). `pm_write_why` then says "Could not save … - it is in use (try again in a moment)" instead of the card wording. `imap_body_1`'s ignored `st_index_save` is now logged ("saved but the message list could not be updated"). | Psion engine builds; device: a sync finishing while a big folder is open |
| R17 Seen store failure ignored | `imap.c` `imap_body_1`: a failed `UID STORE +FLAGS.SILENT (\Seen)` queues `FLAG folder uid +S` in pending.txt. | host build |
| R4 duplicate send after a lost 250 | `smtp.c`: `smtp_send` takes `sent_mark`; `mark_sent()` writes `<id>.snt` just before the final `.`; a server refusal (positive code ≠ 250) removes it, a lost line/timeout keeps it. `pmmain.c` `send_outbox`: a message with a `.snt` is never sent again automatically; its `.err` says "May already have been sent - the server did not answer. Delete it, or open it and send again". App side: `SaveDraftL` (re-send by hand) and `DeleteOutboxL` delete the `.snt`. | host build; invitehost (sends through the fake SMTP) passes; device/fakeimap: drop the link after DATA |
| R5 (engine part) MOVE recorded after `local_update` | `pmmain.c` PM_CMD_FLAG / PM_CMD_MOVE: the pending line is written **before** the local files change, and taken out again with the new `st_pending_drop()` (`store.c`) once the server has it. Replaying a change the server already has is harmless. | undotest passes (pending.txt empty after a sent move: "ok: sent: pending.txt is empty") |
| R10 engine side | `psimail.h`: new `PmDone` ring `done[PM_CMDQ]` written in `pm_do_command` beside `last_*`. | see app R10 |
| R13 log dies for the run | `pmepoc.cpp` `pm_log`: a failed open is retried after a minute (`gLogFailAt`), appending. | Psion build |
| R14 `st_pin_save` ignores fclose | `store.c` `st_pin_save`: old pins + the new one to `pins.new`, `pm_fclose` + `pm_replace`; returns -1 on failure; `pmmain.c` PM_CMD_TRUST reports "Could not save the trusted certificate…" instead of "Trusted". | host build |
| B11 `account.txt` in place; partial file wipes the store | `store.c` `st_check_account`: temp + `pm_fclose` + `pm_replace`; `pm_rmtree` only when the old line looks like `user@host`. | foldertest/undotest/invitehost (fresh stores) pass |
| B15 certificate checks | `certcheck.c`: dNSName with an embedded NUL skipped; wildcard needs a dot in its suffix; `der_time` returns -1 for an unreadable date (digits checked) and that fails the certificate; `chain_ok(now)` checks every certificate's dates on the way, remembered intermediates too. | `build/certtest/certtest` on `mail/tools/fastmail-chain.pem`: trusted for imap.fastmail.com, refused for evil.example.com, "expired" with PM_NOW in 2030, trusted with the clock before 2024 (gate) |
| B16 unbounded RSA exponent | `pmrsa.c` `rsa_public`: exponent stripped of leading zeros, must be 1..4 bytes, odd, ≥ 3. | certtest |
| B22 `dec_feed` contract | `mime.c` comment and `pm.h`: output is at most n + 2 bytes. | — |
| B23 `just_path` over-read | `caldav.c`: the scheme skipped by its own length (7 or 8). | host build; calendar paths unchanged |
| B24 xmlscan swallows after `&` | `xmlscan.c` S_ENT: `<`, `&`, whitespace or the cap emits `&` + the text literally and re-reads the byte as text. | parsefuzz (XML target) clean |
| B30 `CloseSTDLIB` | `pmepoc.cpp` `main`: `CloseSTDLIB()` before return (`<sys/reent.h>`). | Psion build |
| B30 pointer-form bounds in certcheck | `tlv()` and `tlsv_certificate` compare lengths (`l > end - q`), never pointers. | certtest |

## App (`mail/app`)

| Finding | Change | Verification |
|---|---|---|
| R3 / B4 settings rewritten in place | `psimail.cpp`: `SafeWriteFile()` (temp `~`, every Write + Flush checked, `RFs::Replace`). `SaveSettings` keeps the old file as `PsiMail.ini.bak` and `LoadSettings` falls back to it; `SaveCalSettings` and Update.ini use it too. A failure shows an infoprint ("Settings not saved - no room left on C:"). `TPmSettings` size unchanged. | Psion app compiles; device: toggle Work offline, close, restart |
| B21 `default.txt`, `push.txt` | `pmcal.cpp`: `WriteFileSafeL` for default.txt; `SavePushL` via `push.tmp` + Replace + Flush; `SaveMapL` now `Replace` in one step (was Delete + Rename). | compiles |
| R5 engine restart loses queued commands / `iAutoCmd` | `StartEngineL`: commands still queued (`cmd_tail..cmd_head`, from `iSent`) are copied into the new chunk **with their sequence numbers**; `cmd_tail`, `cmd_head`, `done_seq` and `iDoneSeen` carry on from `tail`, so every number the app remembers stays right; `iAutoCmd` is cleared unless its check is still queued. Status says "… and was started again - N waiting commands kept" (`pmrecover.cpp`, `iKeptCmds`). | compiles; device: Tools > Restart with a Move queued offline |
| B8 two engines on one chunk | `ConstructL`: a chunk that already exists with `magic == PM_MAGIC` and `state != EXITED` is not zeroed; the old engine gets `quitting`/`net.quit`. `FinishStartL` (tick-driven, no `User::After`) waits up to 6 s with a busy message "Waiting for the last mail engine to stop…", then `KillStrayEngines()` (`TFindProcess("psimail.exe*")`, never our own id) + `PsiLinkTimersBack()`, then zeroes the chunk and goes on. | compiles; device: kill PsiMail from the task list and restart it within 20 s |
| R10 only the last completed command handled | `TickL` walks `iDoneSeen+1 .. done_seq`; for each, the `PmDone` ring entry is copied over `last_*` and `HandleResultL` runs with it. Nothing else reads `last_*` (checked: pmundo, pmrecover, pmauto, psimail.cpp all read inside HandleResultL). | compiles; undotest/foldertest exercise the engine ring |
| B12 `User::After` in the UI thread (StopEngine) | `StopEngine(aWait)` shows "Stopping the mail engine…" (BusyMsgL) while it waits; Close (`EEikCmdExit`, and the shell's shutdown event) goes through `BeginExitL`: the engine is asked to stop, "Closing…" is shown, a `CPeriodic` (100 ms) calls `Exit()` once it has exited or after 6 s (`StopEngine(EFalse)` kills then). Commands and keys are ignored meanwhile (`iExiting`, `CPmView::iClosing`). | compiles; device: Close while a check is running |
| R16 progress window second Esc | `CPmUpdateProgress::OkToExitL`: Stop again 3 s after the first, with the engine still running, asks "The mail engine has not stopped / Restart the mail engine?" → `RestartEngineL()` + `UpdateEnded()` + `FinishL`. Tools > Restart uses the same `RestartEngineL`. | compiles |
| B3 `RunError` dead on ER5 | `pmcal.cpp/.h`: `RunL` = `TRAPD(err, StepL()); if (err) Failed(err);` — `Failed` is the old RunError body (closes the Agenda, saves the map/push, resets, reports). | compiles; device: calendar sync with the Agenda file missing |
| E17 bare `(%d)` | `Failed()` maps KErrPathNotFound, KErrDiskFull, KErrNotReady/DisMounted, KErrAccessDenied, KErrCorrupt/Eof, KErrCancel to causes; the number only for the rest. | — |
| B20 per-call `RFs` | `pmcal.cpp` CalendarsL, SetDefaultCalendarL, ForgetL, AddToAgendaL and `pminvite.cpp` AgendaMatchL use `CEikonEnv::Static()->FsSession()`. | compiles |
| B19 stack frames | `pmheader.cpp` HeaderTextL: the ~2.5 KB of TBufs live in a heap `THdScratch` (cleanup stack); `psimail.cpp` SaveDraftL `TBuf<600>` → `HBufC`; `pmvcard.cpp` `TBuf8<600>` → `HBufC8`. `epocstacksize` **not** added: the SDK (`tlmakmak`) says it sets the stack "for your executable"; PsiMail.app is a DLL run in EIKON's app thread, whose stack is not ours to set, so the statement would be ignored. | compiles |
| B27 Format into a smaller TBuf | `StartInstallerL`: `TBuf<KMaxFileName + 80>`; `pmstore.cpp` log → `TBuf<200>` (the names there are `TBuf<40>`, not TFileName as the report says, but 160 was still short of the worst case: 3×40 + 46). | — |
| B30 `EApaSystemEventBackupStarting` | `CPmAppUi::HandleSystemEventL`: BackupStarting stops the engine (its log and store files close), BackupComplete starts it again; Shutdown runs Close. (`CEikAppUi`'s and `CCoeAppUi`'s own are private on ER5 so cannot be chained to; theirs only handled shutdown.) | compiles; device: PsiWin backup, and Close from the System screen |
| E2 query order | `pmwebpic.cpp`: statement first, "Get the web pictures?" last. | — |
| E3 help | `pmhelp.cpp`: File > Printing > Save as Word file; Check this folder is Shift+Ctrl+X; shortcuts topic gains Ctrl+Z, Shift+Ctrl+S, Ctrl+P, Shift+Ctrl+P, Shift+Ctrl+V; "Add file button" (×2). `mail/README.md` updated to match. | — |
| E5 Yes/No order | `psimail.rss` `r_pm_yes_no_array` = No, Yes; `pmauto.cpp` NewMailInitL/NewMailSave index mappings inverted, so `KPmViewNoAutoSend` and `KPmViewDetailedProgress` keep their meaning. | — |
| E7 Toast with "…" | `Working(_L("Opening in PsiWeb..."))`: bottom left, retired after 5 s by pmstatus. | — |
| E8 separators in cascades | removed from Save as Word file… and Get older messages. | — |
| E9 Message menu cascades | Get whole message moved above Attachments: the three cascades sit together at the bottom. `DynInitMenuPaneL` unchanged (same items). | — |
| E10 "Connect…" | "Connect". | — |
| E11 Week / Month | `EEikMenuItemRadioStart` / `RadioEnd`. | — |
| E12 "Attach-\nments" | "Add\nfile" (hotkey A unchanged). | — |
| E15 dialog title | "Calendar settings". | — |
| E16 full stop mid-message | `pmrecover.cpp`: " - " instead. | — |
| E18 Ctrl+Y | Check this folder is now Shift+Ctrl+X (free: PsiMail's shifted keys are c r w e g m t l q b a n y u f d h p v s); help and README updated. | — |

## Findings judged wrong or not applicable (with evidence)
- B19 `epocstacksize` for `psimail.mmp`: see above; applies to EXEs only.
- B27 `pmstore.cpp:114`: the three names are `TBuf<40>` (`StoreRoot`), not TFileName; widened anyway.
- R1/R2/R6/R7/R9/R11/R12/R15, B1/B5/B6/B7/B9/B10/B13/B14/B17/B18/B26/B28/B29, E1/E4/E6/E13/E14/E19/E20: `ssh/` and `app/` — the other agents'.

## Verification
- `tools/docker/psibuild "mail/build.sh host"` and `"mail/build.sh"`: engine (Psion and host), PsiMail.app, resources and `dist/PsiMail.sis` all build (the final link waited for the other agent's `ssh/pglinktest.cpp` to compile; it did). `dist/` restored with `git checkout dist/` afterwards.
- Emulator (seeded build, then seed off and rebuilt; `build/emu/rvA..rvD/`): File pane shows "Connect" with no dots; File > Printing has no separator after Save as Word file…; File > Folder shows "Check this folder  Shift+Ctrl+X" with no separator; Message pane has Get whole message above the Attachments / Web / Reply to cascades; the compose dialog's button reads "Add file"; Tools > Calendar settings… opens "Calendar settings"; no panics in the logs. (The calendar's View > Switch view radio pair was not screenshotted; the resource compiled with the standard radio flags.)
- `mail/pkg/psimail.pkg`: `PsiMail.ini.bak` and the `~` temporaries added as FN entries so uninstall removes them (no version change).
- `mail/test`: foldertest.sh, undotest.sh, invtest.py, invitehost.py, htmltest.py, imgtest.py (no crashes, no sanitizer reports, no leaks), parsefuzz.py (all clean) — all pass.
- `certtest` (built by hand against `build/mail-host/ltc,ltm`): see B15.
- `grep TESTSEED mail/app/psimail.cpp`: nothing.

## For device testing
- Close PsiMail from the task list, restart within 20 s: "Waiting for the last mail engine to stop…" then normal start; `psimail.log` shows one engine.
- Tools > Restart mail engine with a Move queued offline: "… N waiting commands kept" and the move still happens.
- Pull the modem line after a message's DATA has gone: the Outbox keeps it with "May already have been sent…"; Check mail does not resend it; open + Send does.
- Close while checking mail: "Closing…" bottom left, the app closes within 6 s.
- Tools > Update PsiMail, Stop twice 3 s apart with a stuck line: the restart query.
- Calendar sync with the Agenda file missing: "Agenda file not found", and sync can be started again.
- A full C: when saving settings: "Settings not saved…" and the old PsiMail.ini (or .bak) still loads.
