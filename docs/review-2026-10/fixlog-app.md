# Fix log: PsiTerm app (app/*, pkg/, tools/) - 3 October 2026

Scope: every finding of recovery.md, bestpractice.md and eikon.md whose code is in `app/`.
Repository `/home/daniel/www/psion/psiterm`, branch `dev`. Nothing committed; versions not bumped.
Line numbers below are in the edited files.

Files changed: `app/psiterm.cpp`, `app/psiterm.h`, `app/psiterm.rss`, `app/psiterm.hrh` (comment),
`app/pthelp.cpp`, `app/ptxfer.cpp`, `app/ptxfer.h`. No change to `pkg/`, `tools/`, `dist/`, README.

## Findings and fixes

### recovery #2 / bestpractice (tool window) - no watchdog after End SSH / Stop (HANG)
- `app/psiterm.cpp` `CTermView::AskQuit`, `EndSshNow`, `QuitCallback` (after `DisconnectSsh`, ~3880-3935);
  `DisconnectSsh` and `StopTool` now call `AskQuit`; `SshProcessEnded` (~3950) cancels the watchdog,
  calls `PsiLinkTimersBack()` when the process was killed, and prints
  `[SSH program did not stop and was ended]` (into the tool window when a tool was running, since
  `LocalMessage` goes to the capture buffer then); `LaunchSshL` resets the flags; `StatusText` shows
  "Ending SSH..." while the quit is pending. `app/psiterm.h`: `iQuitTimer`, `iQuitAsked`, `iKilled`,
  `SshQuitting()`.
- Behaviour: End SSH (menu, Shift+Ctrl+D, toolbar) or Stop in the tool window sets `quit = 1` and arms a
  10 s `CPeriodic`. If `iSshProcess.ExitType()` is still `EExitPending` when it fires, `Kill(0)`; the
  Logon then completes and `SshProcessEnded` updates state, toolbar, tool window (FinishL -> Close),
  NIFMAN timers and the message. A second End SSH / Stop while the first is pending kills at once
  (infoprint "Ending the SSH program now"). The tool dialog's Esc is Stop, so its second Esc ends the job.
- 10 s because psissh's normal close (+++ guard, ATH, bounded socket shutdown) can take 6 s.
- Verification: build; emulator (no session possible there, so only the menus/dialogs). Needs device test:
  end a live session normally (should still say "[SSH program finished]", no kill message), and with a
  wedged link (e.g. pull the modem's power mid-session on the PPP route) End SSH -> after 10 s
  "[SSH program did not stop and was ended]" and SSH to... is available again.

### recovery #7 / bestpractice #5 - Connection settings > Test while SSH is up starves the heartbeat
- App side (the lookup cap and the callback are in `ssh/pglinktest.cpp`, the other agent's):
  `app/psiterm.cpp` `CConnDialog::PreLayoutDynInitL` dims the Test button while `SshActive()`;
  `CConnDialog::TestL` refuses with "Not available while SSH is connected" (the hotkey T still reaches it).
  Connect was already dimmed in the File menu and refused in `HandleCommandL`.
- Verification: build; emulator (dialog opens; Test cannot be exercised without a session).

### recovery #11 - a failed reconnect launch leaves PsiTerm half in "reconnecting"
- `app/psiterm.cpp` `LaunchSshL`: both failure paths (chunk, `RProcess::Create`) call
  `CancelReconnect("[Could not reconnect - use SSH to... to try again]")` when `iReconnecting`;
  `ReconnectNowL` TRAPs `LaunchSshL` and on a leave cancels the reconnect with the error number and
  reopens the serial port; `StartSshL` (user-initiated) cancels any countdown and clears `iReconnecting`.

### bestpractice #7 - USER 11 in the folder list (`Utf8ToText` + `Append('/')`)
- `app/ptxfer.cpp` `CPtRemoteDialog::PreLayoutDynInitL`: the name is clipped to 100 (what the row shows)
  and a folder's name to 99 before the "/" is appended, so `Append` can never overflow the `TBuf<120>`.

### bestpractice #12 - blocking `User::After` waits in the UI thread
- The report accepts these and asks for a busy message where the wait is user-visible (exit, launch):
  `~CTermView` shows "Ending SSH..." (bottom left) during its 3 s wait; `LaunchSshL` shows
  "Stopping the old SSH program..." during the up-to-5.5 s wait for a leftover psissh.
  `WriteToHost` (2 s only when the 2 KB key ring is full), `HangUp` (the Hayes guard times, already under
  "Hanging up..."), `CSerialPort::Write` (3 s driver timeout, only when CTS is low) and ptxfer's 400 ms
  "quick request" probe are unchanged, as the report allows.

### bestpractice #19 - large stack frames (Send file -> BrowseL -> folder dialog, ~4.7 KB)
- `app/ptxfer.h` `CPtXferMemory` gains heap scratch (`iPick`, `iDir`, `iPath`, `iRemote`: `TBuf8<512>`;
  `iText`: `TBuf<512>`, 2.5 KB allocated once on first use); `app/ptxfer.cpp` `BrowseL`, `PtSendFileL`,
  `PtGetFileL` use them (BrowseL uses iDir/iPath, its callers iPick/iRemote/iText, so nothing aliases);
  `CPtRemoteDialog::PreLayoutDynInitL`'s `TBuf<512>` is an `HBufC` on the cleanup stack. About 3 KB
  less stack live at the deepest point.
- `epocstacksize` was NOT added: the SDK's tlmakmak says it sets "a stack size for your executable";
  `psiterm.app` is an app DLL whose thread is made by the launcher, so the statement would not apply.

### bestpractice #21 - `Log.ini` rewritten in place
- `app/ptxfer.cpp` `PtLogCommandL` writes it with `SafeWrite` (temp + `RFs::Replace`), now declared in
  `app/psiterm.h` and no longer `static` in `psiterm.cpp`.

### bestpractice #26 - `JoinDir` truncates a long remote path silently
- `app/ptxfer.cpp` `JoinDir` returns `TBool`; when `dir + "/" + name` does not fit 512 it infoprints
  "Not available - the name is too long" and the callers skip (BrowseL: back to the list; PtSendFileL: return).

### bestpractice #27 - `Format` of a `TFileName` into `TBuf<160>`
- `app/psiterm.cpp` `StartInstallerL`: `TBuf<KMaxFileName + 80>`.

### bestpractice #28 - O(n^2) insertion sort of the folder listing
- `app/ptxfer.cpp` `CRemoteList::SetL` appends, then `Sort()` (an in-place heap sort, `SiftDown`;
  n log n compares, no extra memory, no recursion).

### bestpractice #30 - `EApaSystemEventBackupStarting` not handled (the session log stays open)
- `app/psiterm.cpp` `CPsiTermAppUi::HandleWsEventL` (declared in `psiterm.h`): on an `EEventUser` event
  carrying `EApaSystemEventBackupStarting`, with the log on, it runs the Log command (which stops the log
  and infoprints "Log stopped - ..."), then chains to `CEikAppUi::HandleWsEventL` so EIKON's own
  (private) `HandleSystemEventL` still handles shutdown. (`CEikAppUi::HandleSystemEventL` is private in
  ER5 - `eikappui.h:75` - so it cannot be overridden and chained; `HandleWsEventL` is the protected
  virtual above it.) Not testable in the emulator; needs a PsiWin backup on the device with a log running.

### eikon #1 - Snippets menu could grow to 21 items
- `app/psiterm.h` `KMenuSnippets = 7`; `app/psiterm.cpp` `DynInitMenuPaneL` adds at most 7 after
  "Manage snippets..." (8 items in the pane and in the toolbar pop-up). Every snippet is still sent from
  Manage snippets... (Send) and by its Shift+Ctrl key (`OfferKeyEventL` looks at the whole list).
  Help ("Keys & snippets") says "the first seven are also on the Snippets menu". A cascade was not used:
  it would be limited to 8 too.

### eikon #3 - help describes the old menus
- `app/pthelp.cpp`: "File > Files > Send file / Get file / Log to file"; "Tools > Debug > Check tmux tabs";
  "end SSH" in Keyboard shortcuts; a sentence on End SSH and the watchdog in Getting started.

### eikon #4 - digits as snippet shortcuts
- `app/psiterm.cpp` `KSnippetKeys = "FGIJLNOQRWXYZ"`; the default snippets use L, N, X (cLaude,
  coNtinue, tmuX); `CSnippetList::Load` already drops any key not in the list, so a saved digit loads as
  "none" (no file format change). Help: "Shift+Ctrl+ a letter of your own". Comments in `psiterm.h`.

### eikon #5 - Yes/No order
- PsiTerm's `r_pt_noyes_array` is No, Yes everywhere (Connection settings, Preferences, Snippet, SSH key
  edit). Kept as No/Yes; the PsiMail agent is making PsiMail's lists No/Yes to match.

### eikon #6 - three names for one command
- "End SSH" everywhere: `app/psiterm.rss` File menu item; toolbar button already "End\nSSH"; help;
  the query in `ConfirmDisconnectL` ("SSH is connected" / "End SSH, then continue?"); the infoprint
  "Nothing to end - SSH is not connected"; comments in rss/hrh/h/cpp. (The command id
  `EPtCmdSshDisconnect` and the function names are internal and unchanged.)

### eikon #8 - separator inside a cascade
- `app/psiterm.rss` `r_pt_claude_menu`: `EEikMenuItemSeparatorAfter` removed from "Switch mode".

### eikon #9 - Debug cascade in the middle of Tools
- `app/psiterm.rss` `r_pt_tools_menu`: Reset terminal, Update PsiTerm..., Debug | Help on PsiTerm,
  About PsiTerm. (Debug is now the last item before the Help/About group.)

### eikon #10 - "..." on commands that open no dialog, and none on some that do
- `app/psiterm.rss`: "Connect..." -> "Connect" (it dials and shows the result; nothing is asked);
  "Install login key on server" -> "Install login key on server..." (it can ask which key).
- Left as they are, as the report allows: "Send screenshots" (only asks on first use, when no local
  server is set; afterwards it just sends) and "Log to file..." (a tick box whose dialog opens on the
  first use; the report calls the current form acceptable).

### eikon #13 - "(c)" in About
- `app/psiterm.cpp` `CAboutDialog::PreLayoutDynInitL` sets the three credit lines at run time with
  `TChar(0xa9)`, as PsiMail does (the resource keeps "(c)" as the layout text; the run-time lines are shorter).

### eikon #14 - key names as the keyboard prints them
- `app/psiterm.rss` Scroll cascade: "Shift+Pg Up" / "Shift+Pg Dn"; `app/pthelp.cpp` likewise (two places).

### eikon #19 - trailing separator when there are no snippets
- `app/psiterm.rss`: no `EEikMenuItemSeparatorAfter` on "Manage snippets..." (menu and pop-up);
  `DynInitMenuPaneL` sets it through `CEikMenuPane::ItemData(EPtCmdSnippets).iFlags` only when
  `iSnippets->Count() > 0`.

### eikon #20 - Zoom in without a shortcut: no change (by design, as the report says).

### Found while verifying recovery #2: Esc never reached `OkToExitL` in the tool and transfer windows
- The SDK (eikdials idref: "OkToExitL ... is not called if the Cancel button is activated, unless the
  EEikDialogFlagNotifyEsc flag is set") and the emulator (rv-speed1 frames 078-082: Esc closed the
  Speed test window at once, the job ran on and its output spilled into the terminal) show that the
  report's "Esc just calls StopTool again" was not what happened: Esc closed the window without
  stopping anything. Worse, the transfer window's Stop *is* the Cancel button, so Stop/Esc there never
  set `xfer_cancel`: the window closed, `RunXferL` returned its initial `PSI_XFER_LINK` ("Transfer
  stopped - the SSH connection has gone") and psissh carried on with the transfer.
- `app/psiterm.rss`: `EEikDialogFlagNotifyEsc` added to `r_pt_tool_dialog` and `r_pt_xfer_dialog`.
  Tool window: Esc = Stop (asks, under the watchdog), second Esc = end now, Esc when finished = Close.
  Transfer window: Esc = Stop, as the help already said.

### Checked and found not to need a change
- "anything in TmuxQueryDone/tmux parsing": neither report flags the app side. bestpractice's
  "Checked and found sound" covers tmuxq's output handling; I re-read `TmuxQueryDone`/`TmuxName`/
  `TmuxField`/`ParseTabList` (`app/psiterm.cpp` ~1320-1880): every copy is bounded (`TBuf8<16>` session
  ids are only copied when `sid.Length() < 16`; names are clipped by `TmuxName` and marked with "...";
  `tabs[]` is bounded by `KMaxTabs`; `tq_len` is clamped to `PSI_TQ_OUT_SIZE`). The flagged
  TQ_OPENING/TQ_CLOSING state problems are in `ssh/tmuxq.c`.
- eikon section 0 (EIKON 8 sweep) was clean; the new `ItemData` call names an item that is in both panes
  it is used for (`r_pt_snippets_menu`, `r_pt_snippets_popup`).

## Verification
- Build: `flock .../psibuild.lock tools/docker/psibuild "bash build.sh"` -> `dist/PsiTerm.sis` (twice: the
  second build waited for the ssh agent's in-progress `ssh/pglinktest.cpp` to compile again; not my file).
  Only the pre-existing `arm-pe-ld` interworking warnings. `dist/PsiTerm.sis` restored with
  `git checkout dist/PsiTerm.sis` afterwards.
- Emulator (private card `scratchpad/app-card.img` via `EMU_CARD`, since the shared `build/emu/card.img`
  was remade as a PsiMail card by another agent mid-run). Screenshots in `build/emu/rv-*/`; no panic,
  KERN-, WSERV or USER line in any log:
  - `rv-menus`: File (SSH to..., End SSH dimmed, Connect, Hang up modem, Install login key on server...,
    Files, Close), Edit, View, Keys, Snippets (Manage snippets..., line, then the 5 defaults with
    Shift+Ctrl+L/N/X), tmux, Tools (Preferences..., SSH keys..., Connection settings... | Reset terminal,
    Update PsiTerm..., Debug > | Help on PsiTerm, About PsiTerm).
  - `rv-claude`: Keys > Claude Code cascade with no line inside. `rv-debug`: Tools > Debug cascade opens
    from its new (low) position. `rv-scroll`: View > Scroll shows "Shift+Pg Up" / "Shift+Pg Dn".
  - `rv-about`: About with (c) signs on all three credit lines. `rv-help`: the help dialog opens.
    `rv-conn`: Connection settings opens (Test enabled: no session). `rv-popup`: the toolbar's Send
    snippet pop-up (Manage snippets..., line, 5 snippets, no keys shown).
  - `rv-nosnip3`: all five snippets deleted through the dialog (pen on Delete, Y), then the Snippets menu
    shows "Manage snippets..." alone with no trailing line (frame s-100).
  - `rv-speed1c`: Tools > Debug > Speed test, Esc once: "Stopping..." stays in the window, psissh stops
    at the next test boundary, the button becomes Close (no kill message). `rv-speed2c`: Esc twice:
    "[SSH program did not stop and was ended]" and Close at once. (Before the NotifyEsc fix, `rv-speed1`
    showed the window vanishing on Esc with the rest of the output spilling into the terminal.)
- Not testable in the emulator (no SSH, no backup): the 10 s watchdog on a wedged live session, the
  dimmed Test button with SSH up, the backup event closing the log, the reconnect failure paths, the
  SFTP folder list with a 120+ character name, the long-name refusal and the sort on a big folder.

## For device testing
1. End SSH on a healthy session: "[SSH program finished]" as before; "Ending SSH..." on the status line
   meanwhile. 2. Pull the WiFi mid-session on the PPP route, then End SSH: after at most 10 s
   "[SSH program did not stop and was ended]" and SSH to... works again; or press End SSH twice.
3. Get file on a folder with very long names (no USER 11); a 400+ entry folder lists without a long freeze.
4. Send file / Get file and press Stop (or Esc) mid-transfer: "Transfer stopped" (not "the SSH connection
   has gone") and the next transfer works. 5. A PsiWin backup while File > Files > Log to file is on:
   "Log stopped - ..." infoprint. 6. Old Snippets.dat with digit keys: they load as "None".

## Not changed (with the reason)
- eikon #10 "Send screenshots" and "Log to file...": left as the report allows (see above).
- bestpractice #19 `epocstacksize`: not applicable to an app DLL (see above).
- README.md: its wording already matches ("SSH to (End SSH while connected)", "Tools > Debug").
- `pkg/psiterm.pkg`, versions: untouched, as instructed.
