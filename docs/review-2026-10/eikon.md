# EIKON style guide audit: PsiTerm and PsiMail (dev branch)

Scope: `app/psiterm.rss`, `app/psiterm.hrh`, `app/psiterm.cpp`, `app/pthelp.cpp`, `app/pttabs.cpp`, `app/ptxfer.cpp`, `ssh/pglinktest.cpp`; `mail/app/psimail.rss`, `mail/app/psimail.hrh`, `mail/app/pmapp.h`, `mail/app/*.cpp`.
Checked against the EIKON Application Style Guide (as distilled in the `epoc-eikon-ui-guide` skill) and the "UI rules" section of `psiterm/CLAUDE.md`.

Severity key: **P** = panic risk, **V** = visible inconsistency, **W** = wording.
All paths are under `/home/daniel/www/psion/psiterm/`.

## 0. Panic-risk sweep (EIKON 8): clean

Every `SetItemDimmed` / `SetItemButtonState` / `SetItemTextL` was matched against the pane it is called for:

| Pane | Code | Items touched | All present? |
|---|---|---|---|
| `r_pt_term_menu` | `app/psiterm.cpp:5660-5668` | Ssh, SshDisconnect, Connect, Hangup, InstallKey | yes |
| `r_pt_files_popup` (toolbar pop-up **and** File > Files cascade) | `:5670-5676` | SendFile, GetFile, Log | yes |
| `r_pt_snippets_menu` / `_popup` | `:5677-5698` | AddMenuItemL only | n/a |
| `r_pt_tmux_menu` / `_win_` / `_pane_` / `r_pt_claude_menu` | `:5700-5724` | KTmux/KWin/KPane/KClaude tables | yes, each table matches its pane |
| `r_pt_tmux_prefix_menu` | `:5725-5730` | PrefixA/B | yes |
| `r_pt_font_menu` | `:5732-5746` | Zoom0..4 | yes |
| `r_pt_view_menu` | `:5747-5752` | Bold, StatusLine, Toolbar | yes |
| `r_pm_file_menu` | `mail/app/psimail.cpp:4573-4579` | Offline, Hangup, Stop, Connect | yes |
| `r_pm_print_menu` | `:4580-4584` + `pminvite.cpp:789-790` | PrintPreview, Print, SaveWord | yes (SaveWord is in the cascade) |
| `r_pm_folder_menu` | `:4585-4594` | Refresh, Older, NewFolder, RenameFolder, DeleteFolder | yes |
| `r_pm_edit_menu` | `:4595-4603` | Delete, Move, Archive, Undo, Search | yes |
| `r_pm_attach_menu` | `:4604-4608` | OpenAttach, SaveAttach | yes |
| `r_pm_message_menu` | `:4609-4621` | Forward, Unread, Flag, Whole, New | yes |
| `r_pm_web_menu` | `:4622-4627` | Web, WebPictures | yes |
| `r_pm_event_menu` | `:4628-4634` | EventDetails, Calendar | yes |
| `r_pm_switch_view_menu` | `:4635-4640` | WeekView, MonthView | yes |
| `r_pm_view_menu` / `r_pm_cal_view_menu` | `:4641-4649` | ToggleToolbar/Title/Folders; Sort only for the mail View | yes |
| `r_pm_goto_menu` | `:4650-4654` | Inbox, Folders | yes |
| `r_pm_reply_menu` / `r_pm_reply_popup` | `:4655-4662` | Reply, ReplyAll; Forward only for the pop-up | yes |
| `r_pm_contacts_menu` | `pminvite.cpp:791-795` | AddSender, AddCard | yes |
| `r_pm_invite_menu` | `pminvite.cpp:796-804` | Accept, Tentative, Decline, RemoveEvent | yes |

Hotkey tables have no duplicate keys (PsiMail Ctrl: e n r w d x u m t k s g i y f b q z p; Shift+Ctrl: c r w e g m t l q b a n y u f d h p v s. PsiTerm Shift+Ctrl: e s d u c v m k t b p h a; snippet keys F G I J L N O Q R W X Y Z and 0-9 do not collide). Every pane has at most 8 static items. Both zooms wrap (`app/psiterm.cpp:486-491`, `mail/app/pmnative.cpp:500-503`). All `BusyMsgL` calls use `EHLeftVBottom`. All ten "No … entered" validations move the focus to the line.

## 1. Findings (most important first)

### 1. Snippets menu can grow to 21 items (V)
- Rule: guide 3 "at most about 8 items, no scrolling menus"; CLAUDE.md "Menus are at most 8 items".
- `app/psiterm.cpp:5681` adds up to `KMaxSnippets` (= 20, `app/psiterm.h:71`) items after "Manage snippets..." in both `r_pt_snippets_menu` and the toolbar's `r_pt_snippets_popup`. With 12 or more the pane no longer fits 240 px.
- Fix: cap the loop at 7 (`i < 7`) and say so in the dialog, or put the snippets in a cascade `Snippets > Send ▶`. The remaining snippets stay reachable through Manage snippets… and their Shift+Ctrl keys.

### 2. Query with the question first (V)
- Rule: guide 5 "Statement first, question last, right before the buttons".
- `mail/app/pmwebpic.cpp:201`: `QueryWinL(_L("Get the web pictures?"), what)` puts the question on the top line and "%d pictures on %d web sites: %d calls on the modem" under it.
- Fix: `if (!iEikonEnv->QueryWinL(what, _L("Get the web pictures?")))`.

### 3. Help still describes the old menus (V)
- Rule: CLAUDE.md "Help text consistency with actual menus"; guide 1 "same term for the same thing everywhere".
- `mail/app/pmhelp.cpp:133`: "File > Save as Word file (Shift+Ctrl+S)" – the command is now File > Printing > Save as Word file (`psimail.rss:199`).
- `app/pthelp.cpp:107, 111, 118`: "File > Send file", "File > Get file", "File > Log to file" – now File > Files > … (`psiterm.rss:196`).
- `app/pthelp.cpp:87`: "tmux > Check tmux tabs" – the command is Tools > Debug > Check tmux tabs (`psiterm.rss:228`).
- `app/pthelp.cpp:53`: "SSH to (End SSH while connected)" is right for the toolbar, but the menu says "Disconnect SSH" (see 6).
- Fix: update the four sentences; add Ctrl+P / Shift+Ctrl+P / Shift+Ctrl+V / Ctrl+Z / Shift+Ctrl+S to the PsiMail "Keyboard shortcuts" topic (`pmhelp.cpp:240-260`).

### 4. Digits as shortcut keys for snippets (V)
- Rule: guide 3 "All shortcuts are Ctrl+letter or Shift+Ctrl+letter; never use numbers".
- `app/psiterm.cpp:4515` `KSnippetKeys[] = "1234567890FGIJLNOQRWXYZ"`; `app/pthelp.cpp:167` "a letter or digit of your own".
- Fix: drop the digits (`"FGIJLNOQRWXYZ"` – 13 letters are plenty for 7 menu snippets) and the word "digit" in the help. Existing snippets saved with a digit key need a one-off migration to "none".

### 5. Yes/No choice lists in two different orders (V)
- Rule: guide 1 "use the same term for the same thing everywhere"; the built-in programs use one order.
- `mail/app/psimail.rss:762-768` `r_pm_yes_no_array` = Yes, No (Preferences > New mail: "Send waiting mail", "Show detailed progress"), while `r_pm_yesno_array` (`:621-624`) and PsiTerm's `r_pt_noyes_array` (`psiterm.rss:456-459`) are No, Yes.
- Fix: make `r_pm_yes_no_array` No, Yes and invert the two index mappings in `pmauto.cpp` (the stored bits in `iSpare[]` must not change meaning). Better still, use EIKON check boxes (`EEikCtCheckBox`) for every yes/no line, as Psion's own dialogs do.

### 6. Three names for one command: "Disconnect SSH" / "End SSH" / "disconnect" (W)
- Rule: guide 1 "Use the same term for the same thing everywhere".
- `app/psiterm.rss:192` menu "Disconnect SSH"; `app/psiterm.cpp:5150` toolbar "End\nSSH" (comment: Disconnect is too wide for the dense font); `app/pthelp.cpp:157` "disconnect SSH".
- Fix: rename the menu item to "End SSH" (and the help) so the toolbar and menu match; or keep "Disconnect SSH" and label the button "Dis-\nconnect" only if it measures as fitting.

### 7. Busy-style text shown as an infoprint (V)
- Rule: guide 6 "Busy messages appear bottom left … ending with '…'; static infoprints top right".
- `mail/app/psimail.cpp:2441` `Toast(_L("Opening in PsiWeb..."))` – a progress message at the top right.
- Fix: `Working(_L("Opening in PsiWeb..."))` (bottom left, through pmstatus.cpp), or drop the dots and the message (PsiWeb appearing is the visible result).

### 8. Separators inside cascades (V)
- Rule: guide 3 "no lines inside cascades".
- `mail/app/psimail.rss:199` (`r_pm_print_menu`, after Save as Word file...), `:214` (`r_pm_folder_menu`, after Get older messages), `app/psiterm.rss:315` (`r_pt_claude_menu`, after Switch mode).
- Fix: remove `EEikMenuItemSeparatorAfter` from those three items.

### 9. Cascades high in the pane / interleaved with commands (V)
- Rule: guide 3 "cascades usually sit best near the bottom; a cascade should not be the first item".
- `mail/app/psimail.rss:182` Folder cascade is the second item of File; `:269-272` Message has Attachments, then "Get whole message", then the Web and Reply to cascades; `:378` Accounts sits in the middle of Tools; `app/psiterm.rss:212` Debug sits between Reset terminal and Update PsiTerm....
- Fix (minimal): in Message move "Get whole message" above Attachments so the three cascades are together at the bottom; in PsiTerm's Tools move Debug below Update PsiTerm... (keep Help/About last). File > Folder and Tools > Accounts can stay if Dan prefers the Email program's grouping; say so in the comment.

### 10. "…" on commands that open no dialog, and none on some that do (W)
- Rule: guide 3 "'…' only on commands that open a dialog; actions that then show progress have none".
- `app/psiterm.rss:193` and `mail/app/psimail.rss:183` "Connect...": `PgLinkConnectL` dials and then shows the result window; nothing is asked. Fix: "Connect".
- `app/psiterm.rss:230` "Send screenshots" opens the Update dialog (with an InfoWin first) when no local server is set (`psiterm.cpp:5874-5882`). Fix: "Send screenshots..." or always skip the dialog once a server is set (current behaviour) and leave as is; the first-run path is the only one that asks.
- `app/psiterm.rss:195` "Install login key on server" opens `r_pt_key_pick_dialog` when several keys exist and none matches (`psiterm.cpp:6323-6329`). Fix: "Install login key on server..." (the pick is the usual case for more than one key).
- `app/psiterm.rss:135` and `:1135` "Log to file..." is a tick box whose second use stops the log without a dialog (`ptxfer.cpp:1161-1174`). Acceptable, but the "…" plus a tick is unusual; consider "Log to file" with the tick and a separate "Log settings..." if it bothers.

### 11. Week / Month as check boxes rather than radio buttons (V)
- Rule: guide 3 tick boxes are for toggles; a mutually exclusive pair is a radio group (as `r_pt_tmux_prefix_menu` and `r_pt_font_menu` already do).
- `mail/app/psimail.rss:355-356`.
- Fix: `flags=EEikMenuItemRadioStart` / `EEikMenuItemRadioEnd`; the code at `psimail.cpp:4638-4639` already sets exactly one on.

### 12. Hyphenated single word on a dialog button (V)
- Rule: guide 5 "Two-word labels go on two lines" – one word is not split.
- `mail/app/psimail.rss:589` "Attach-\nments".
- Fix: "Add\nfile" (and the help's "the Attachments button" at `pmhelp.cpp:88, 129` becomes "the Add file button"), or "Attach" on one line.

### 13. PsiTerm About says "(c)", PsiMail says "©" (V)
- `app/psiterm.rss:678` `"SSH terminal for the Psion Series 5mx - (c) 2026 Dan Edge"` is never replaced at run time (only About1 and AboutStatus are, `psiterm.cpp:4866-4867`); PsiMail builds the line with `TChar(0xa9)` (`psimail.cpp:4558-4560`).
- Fix: set `EPtDlgAbout2` at run time the same way, or put the © character in the resource.

### 14. Key names not as the keyboard prints them (W)
- Rule: guide 7 "Write key names as printed on the keyboard".
- `app/psiterm.rss:289-290` extratxt "Shift+PgUp" / "Shift+PgDn"; `app/pthelp.cpp:35, 38, 161` mix "Pg Up/Pg Dn" and "PgUp/PgDn". The 5mx prints "Pg Up" / "Pg Dn" as Fn legends on the arrow keys.
- Fix: "Shift+Pg Up" / "Shift+Pg Dn" in the Scroll cascade and in the help; `pmhelp.cpp:241` already uses "Pg Up/Pg Dn".

### 15. Dialog title shorter than its command (W)
- Rule: guide 5 "Titles say as much as needed".
- `mail/app/psimail.rss:883` `r_pm_cal_dialog` title "Calendar" for Tools > Calendar settings...; `:826` "Mail account" for Accounts > Settings... / Add....
- Fix: "Calendar settings"; "Mail account" is fine (it serves Add and Settings).

### 16. Full stop inside a message (W)
- Rule: guide 6 "Split two parts with ' - ' rather than a full stop".
- `mail/app/pmrecover.cpp:74` "Not downloaded - the mail engine stopped. Tools > Restart mail engine starts it again".
- Fix: "Not downloaded - the mail engine stopped - Tools > Restart mail engine starts it again" or two lines in the reader.

### 17. Bare error number in an outcome (W)
- Rule: guide 6 "Be meaningful, with a likely cause".
- `mail/app/pmcal.cpp:253` `"Calendar sync failed (%d)"`.
- Fix: map the common codes (timeout, name lookup, TLS, 401) to a cause the way `ptxfer.cpp:305-342` does, keeping the number only for the rest.

### 18. Ctrl+Y (Redo) used for "Check this folder" (W)
- Rule: guide 3 "Don't reuse standard letters where a user pressing the key expecting the standard action could lose anything"; Ctrl+Z is Undo here, so Ctrl+Y reads as Redo.
- `mail/app/psimail.rss:116`. Nothing is lost (it starts a check), so this is a nice-to-have.
- Fix: leave, or move to a free shifted key (Shift+Ctrl+X) and note the change in the help (`pmhelp.cpp:151, 248`).

### 19. Trailing separator when there are no snippets (V)
- `app/psiterm.rss:108, 328`: "Manage snippets..." has `EEikMenuItemSeparatorAfter`; with no snippets the pane ends in a line.
- Fix: no flag in the resource; in `DynInitMenuPaneL` (`psiterm.cpp:5681`) add the separator only when `iSnippets->Count() > 0` (via `SetItemFlags`/a divider item), or leave the first snippet undivided.

### 20. PsiTerm's "Zoom in" has no shortcut shown (V, by design)
- `app/psiterm.rss:158, 275`: only Zoom out carries Shift+Ctrl+M (Ctrl+M must go to the host). The guide pairs Ctrl+M / Shift+Ctrl+M, so this is the right half to keep; the sidebar's zoom icons cover Zoom in (`psiterm.cpp:6045-6054`). No change; just noting it is deliberate and the help (`pthelp.cpp:48, 159`) explains it.

## 2. Things checked and found conforming

- Menus: standard order; Tools ends with Help on… and About…; first-word capitals; "&" not "and"; dimming rather than hiding everywhere (`Reply to` / `Attachments` cascades are never dimmed, only their items).
- Toolbar: name, 4 buttons (icon + text, dense font), clock; Ctrl+T (PsiMail) / Shift+Ctrl+B (PsiTerm, documented) hide it and the view reflows. Unavailable buttons still press and infoprint ("No account - add one with Tools > Accounts", "Nothing to delete", "Events are changed in the Agenda", "Not available - SSH is not connected").
- Shortcuts: PsiMail uses the standard letters for Close, Create new, Delete, Find, Go to, Preferences, Zoom, Switch view, Toolbar, Undo, Print, Print setup, Print preview, Save as, About, Help, and the Email program's for Check mail, Disconnect, Move, Folders, Status, Sort, Title bar. PsiTerm keeps every plain Ctrl for the host and uses Shift+Ctrl throughout, as CLAUDE.md requires.
- Dialogs: all fit 640x240 (largest: Account > Incoming, Event, About PsiTerm at about 205-215 px); Cancel/OK order correct in every `DLG_BUTTONS`; "Test" / "Up" / "Open" / "New" / "Edit" / "Delete" sit above Cancel/OK on the right (`EEikDialogFlagButtonsRight`) or along the bottom where six would not fit; Continue is used for information dialogs; Stop for transfers and tools.
- Validation: "No address entered", "No user name entered", "No password entered", "No key name entered", "No text entered", "No filename entered", "No folder name entered", "No event name entered", "No calendar server entered", "No local server entered", "No email address entered", each followed by `TryChangeFocusToL`.
- Queries: all 20 `QueryWinL` calls except finding 2 are statement-then-question, with user names in double quotes (`"\"%S\""` for servers, snippets, keys, messages, folders, accounts).
- Infoprints: no "error", "please", "!", "abort", "exit", "default", "Are you sure", "application", "directory", "click"; no trailing full stops; "Not available while SSH is connected", "Not available - SSH is not connected", "Not available at this time", "Not available until the message has downloaded", "Nothing to paste/copy/undo/delete/disconnect" follow the guide's forms; singular/plural handled ("This message has an attachment" / "%d attachments", "holds 1 message" / "%d messages").
- Busy messages: all `BusyMsgL` are `EHLeftVBottom` and end in "..." ("Hanging up...", "Saving...", "Paginating...", "Reading Contacts...", "Looking in the Agenda...", "Moving the mail into the System folder..."); PsiMail's quiet progress (pmstatus.cpp) is one steady bottom-left message per command with the outcome as an infoprint.
- Zoom: both cycle round the sizes; PsiMail has three sizes, PsiTerm five with the current one ticked in View > Font.
- British English throughout ("colours", "organiser", "licence", "dialling"); the only "Organizer" is the iTIP header name, not shown.
- Help dialogs share one layout (Topic list + read-only rich text, Continue), and PsiMail's custom scroll bar follows the reader's.
