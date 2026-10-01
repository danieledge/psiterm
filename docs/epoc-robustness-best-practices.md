# EPOC R5 robustness: best practice for PsiTerm, PsiMail and PsiWeb

What the ER5 SDK, Symbian's own ER5-era guidance and the surviving
open-source Psion code say about writing apps that do not panic, leak or
hang, with the rules we apply in this suite. UX/UI rules are in the EIKON
style guide; this is about everything else. Sources are at the end; the
SDK pages are under `SDK/epoc_cpp_sdk/sysdoc/`.

## 1. Memory, leaves and the cleanup stack

**The model.** Any function that can fail to get memory or a resource
*leaves* (`User::Leave`, `new (ELeave)`, `User::LeaveIfError`). A leave
unwinds to the nearest `TRAP`/`TRAPD`; everything pushed on the cleanup
stack since then is destroyed. EIKON wraps every command and event handler
in a trap and shows the error ("Not enough memory"), so an app that leaves
correctly survives OOM in a dialog by default; an app that forgets a
`PushL` leaks, and one that keeps a half-built object on a leave panics
later. *(SDK: e32/euclnp "Cleanup support: TRAP and Leave".)*

Rules:

- **Two-phase construction.** A C++ constructor must not leave (the object
  would be lost: nothing owns it yet). Allocate members in `ConstructL`,
  reached through `NewL`/`NewLC` which does `new (ELeave)`,
  `CleanupStack::PushL`, `ConstructL`, `Pop`. Destructors must cope with
  partially constructed objects (members NULL) because `ConstructL` may have
  left half way. Do not call functions that can leave from a destructor.
- **Push everything you own only locally.** Between allocating an object and
  storing it in a member that a destructor will free, it must be on the
  cleanup stack. Objects owned by `this` are not pushed (the owner's
  destructor frees them). Never push a member variable, and never leave a
  `PushL` unmatched: the `PopAndDestroy` count must agree with the pushes on
  every path (EIKON's debug build panics `E32USER-CBase 71` if it does not).
- **`LD` functions** (`ExecuteLD`, `RunLD`) delete `this`: never touch the
  object afterwards and never push it as well.
- **Non-`CBase` resources** use `CleanupClosePushL` (handles with `Close`),
  `CleanupReleasePushL` or `TCleanupItem` with a static function. *(SDK:
  er5supp/clnuputl.)*
- **Mixing C and C++:** C code cannot leave or be unwound. Call into C from a
  function that has already pushed what it owns, and check C return codes;
  an `estlib` `malloc` returning NULL must be handled, not assumed.
- **TRAP only where you can recover.** `TRAPD(err, ...)` around a unit of work
  (one command, one file load), then report `err` with
  `CEikonEnv::InfoMsg`/`HandleError`. Swallowing errors silently hides OOM.
- **Debug heap checks.** Wrap a test run in `__UHEAP_MARK`/`__UHEAP_MARKEND`
  (debug builds only; a leak panics `ALLOC`). `__UHEAP_FAILNEXT(n)` and
  `__UHEAP_SETFAIL(RHeap::EDeterministic, rate)` make allocations fail so
  every path is tried under OOM. On the debug emulator EIKON's heap failure
  tool does the same interactively: *Ctrl+Alt+Shift+P* opens the dialog
  (app heap, window-server heap and file failure, random or deterministic
  rate), *Ctrl+Alt+Shift+Q* turns it off, *A/B/C* show heap cells, file
  server and window server resources in use. *(SDK: e32/euuser
  `__DbgSetAllocFail`; emul/emulator/emuldebug "Debugging keys".)*
- **EXEs vs DLLs.** `.app` files are DLLs: no writable static data on MARM
  (PETRAN warns "has initialised data"; look for `*(.bss)` in the `.map`).
  Our engines are `.exe`s and may have statics, but global objects with
  constructors are still avoided (`psiglue.cpp` keeps handles on the heap).

## 2. Descriptors

- A `TBuf<N>`'s `Copy`, `Append`, `Format` and `AppendFormat` **panic
  (`USER 11`, "descriptor overflow") rather than truncate** when the result
  exceeds `MaxLength`. Anything of unknown length (file contents, network
  text, C strings from the engine, command tails, resource text) must go
  through a clipping copy: `Left(Min(...))` into the target, as `SafeCopy` /
  `Clip` / `CopyToC` do in `psimail.cpp` and `FromUtf8` in `psiweb.cpp`.
- `TPtrC8((const TUint8*)cstr)` runs `strlen`: the C buffer must be
  NUL-terminated. Our shared-chunk fields are fixed `char[N]` written with
  bounded copies and a terminator; the readers re-terminate before use
  (`s->keyfile[sizeof - 1] = 0`).
- `ZeroTerminate()` needs `Length() < MaxLength()`; `TBuf8<KMaxFileName + 1>`
  for a file name, not `TBuf8<KMaxFileName>`.
- Index operators do not range-check in release builds; `Mid`/`Left` do
  (`USER 10`/`USER 22`). Compute lengths first.
- `HBufC::ReAllocL` returns a *new* pointer; keep it. *(SDK: e32/eudesc.)*

## 3. Active objects

- Every `CActive` must `Cancel()` in its destructor (the base-class
  destructor panics `E32USER-CBase 40` on an outstanding request) and
  implement `DoCancel` to cancel the *service* it asked for.
- A request must be issued at most once before `SetActive()`; two `After()`
  calls on one `RTimer` panic.
- ER5 has **no `RunError`**: a leave from `RunL` goes to the scheduler's
  `Error()`, which in CONE is `CCoeEnv::HandleError` → EIKON's "Not enough
  memory" style dialog. Keep `RunL` leave-safe or trap inside it.
- **Stray signals** (`E32USER-CBase 46`) come from a request completing
  after its object was deleted or its status went out of scope. Keep the
  `TRequestStatus` alive as long as the request may complete. `psiglue.cpp`
  keeps lookup/connect/shutdown statuses in statics and "reaps" orphaned
  completions in its `WaitFor` loop for exactly this reason.
- Synchronous `User::WaitForRequest` in the engines is fine (they are
  separate processes with no UI), but every wait is bounded with a timer
  and checks the app's `quit` flag. *(SDK: e32/euactv, e32/euasyn.)*

## 4. Client-server handles, files

- Every `R` class handle you `Open`/`Connect`/`Create` must be `Close`d on
  all paths, including leaves (`CleanupClosePushL`). A thread's handles are
  reclaimed when it dies, but a long-running app leaks until then; the
  debug key *Ctrl+Alt+Shift+B* counts file server resources per app.
- `RFs` sessions are expensive: use `CEikonEnv::FsSession()` in the app, one
  static session in the engine.
- **Files are not shareable by default.** Open with `EFileShareReadersOnly`
  / `EFileShareAny` where the app reads what the engine writes; otherwise
  `KErrInUse` (-14). `pm_replace` retries `RFs::Replace` a few times for the
  moment when the app has the old file open.
- **Full disk:** `RFile::Write` returns `KErrDiskFull` (-26); stdio's
  `fwrite` may report it only at `fflush`/`fclose`. Check the result of the
  close (`pm_fclose`), delete the partial file, and say which drive is full
  (`pm_write_why`). Write to a temporary and `RFs::Replace` it over the real
  file so a failure never leaves a truncated settings/index file.
- **Card removed:** `KErrNotReady` (-18) means no media; `KErrDisMounted`
  (-13) that the volume changed under an open file; `KErrPathNotFound`
  (-12) if the directory went with it. Treat all three as "the card is not
  there" in messages, close the handles, and re-check the drive before the
  next write (`RFs::Volume`, `RFs::NotifyChange(ENotifyDisk)` to learn of
  the change). Never cache a `TVolumeInfo`.
- **Corrupt files:** anything we read back (settings, `index.txt`,
  `folders.txt`, pending queues) is parsed field by field with bounded
  copies and defaults for missing fields; a version byte first lets a
  newer struct be recognised. Never `Read` straight into a struct without
  checking the size returned.
  *(SDK: f32/fsusing, f32/fssess, f32/fsfile.)*

## 5. Low memory, power, shutdown

- **OOM at startup:** `ConstructL` leaving is handled by EIKON (the app
  exits with an error). Allocate the big things (frame buffers, caches)
  once, early, and keep the steady-state heap small: the Psion has 4–16 MB
  for everything.
- **OOM while running:** commands leave, EIKON tells the user. The window
  server may also fail allocations; `CCoeEnv::HandleError` is where an app
  can override the message. The System screen's "Close" of other apps is the
  user's remedy, so our apps must close *promptly* when asked (below).
- **Switch-off/on:** the machine can be switched off at any moment (lid,
  timer, battery). Nothing in RAM is lost, but open serial ports and sockets
  may be dead on resume: `HandleSwitchOnEventL` is the hook, and our apps bump
  `switch_on` in the shared chunk so the engine re-checks the link.
  `UserHal::ResetAutoSwitchOffTimer()` keeps the machine on while data flows
  (`KeepAwake` in `psiglue.cpp`), never permanently.
- **Being asked to close:** the System screen's task list and the shell send
  `EEikCmdExit` through `HandleCommandL`; `CApaAppUi` also receives
  `EApaSystemEventShutdown` in `HandleSystemEventL` (and
  `EApaSystemEventBackupStarting`, for which files must be closed). The app
  must save state and call `Exit()` without a dialog. Our `HandleCommandL`
  paths save settings, tell the engine to quit (shared `quit` flag), wait a
  bounded time (3–6 s) and `RProcess::Kill` it if it has not exited.
  *(SDK: cone/coappui, apparch/aaappdoc; headers apgtask.h, eikappui.h.)*

## 6. Document and application framework

- `CEikDocument::CreateAppUiL` and `CEikAppUi::ConstructL` are the
  construction path; `BaseConstructL` reads the resource file. Keep the
  model (settings, host list) in the document or app UI, not in controls.
- Settings go to a file of our own, written atomically (section 4), not to
  the document store unless the app is file-based.
- The `.aif` and the UIDs must match the `.mmp` and the `.pkg`.

## 7. Threads and processes

- Our engines run as separate `.exe` processes: a crash in the C code
  kills the engine, not the app. The app `RProcess::Logon`s on it; the
  completion carries `ExitType()` (`EExitPanic` with category and reason,
  e.g. `KERN-EXEC 3` = bad memory access, `USER 11` = descriptor overflow)
  so the status line can say what happened and offer "Restart engine".
- The other way round: an engine must notice its app dying (crash, or
  killed from the task list; an EXE without a window is not in the task
  list, so nobody else can stop it). PsiMail and PsiWeb beat `app_beat`
  in their shared structs and the engines quit after 20 s of silence;
  PsiTerm does the same through `PsiShared::app_beat` (0.69) so psissh
  never keeps the serial port and the chunk until a reset.
- A process the app launched is `Kill`ed on exit if it does not quit; its
  handles (sockets, serial port, NIFMAN session) are reclaimed by the
  kernel, but things it *told* another server (NIFMAN idle timers off) are
  not: undo them from the app (`PsiLinkTimersBack`).
- Priorities: app threads are `EPriorityForeground`/`Background` by the
  window server as they move; engine EXEs default to `EPriorityForeground`
  as processes. Use `RThread().SetPriority(EPriorityLess)` for a busy
  background thread so the UI stays responsive; never `EPriorityRealTime`.
- Panics: `User::Panic` for programming errors only; the user sees a
  "Program closed" dialog with category and number. Every number in our
  code has a meaning in a comment next to the panic.
  *(SDK: e32/euthrd, e32/eupanic.)*

## 8. ESTLIB (the C library) on ER5

- Not in the 5mx ROM: ship `STDLIB.SIS` as an embedded component.
- Each thread has its own `struct _reent` (errno, stdio); `CloseSTDLIB()`
  must be called when the thread is done with it or the debug heap check
  fails. Files opened with stdio are private to the thread.
- `printf("%f")` is non-standard (uses `TDes::Format` and the locale's
  separators); format floats yourself if the text matters.
- `stdio` writes in 512-byte pieces: for a whole file use `RFile::Write`
  once (`pm_write_whole`). `fclose` is where a disk-full error finally
  shows.
- Sockets through estlib are a thin veneer over ESOCK; passing a
  descriptor estlib does not own panics (`USER 22`), hence our `psi_*`
  wrappers for every socket call in Dropbear.
- No `fork`, `exec`, signals or `select` on anything but sockets: stub them.
  *(SDK: stdlib/slimplem, stdlib/slport.)*

## 9. Stack

- The default thread stack is **8 KB** (`makmake` statement
  `epocstacksize`, or PETRAN's `-stack` for an EXE); EIKON app threads get
  more but not much. Our EXEs are built with 64 KB (`psimail.exe`) and
  128 KB (`psissh.exe`) stacks. A stack overflow is a `KERN-EXEC 3` with no
  further clue.
- Keep per-call locals small: no `char buf[4096]` in a leaf you recurse
  into, static scratch buffers in single-threaded engines (`imap.c`'s
  `BodyCtx` and `g_resp`), `TBuf<500>` only where the call chain is shallow.
- Parsers must not recurse on attacker-controlled depth: `imapparse.c`
  caps nesting at 40 and treats a stray `)` with a loop, `html.c`/`mime.c`
  cap list/part depth at 8. *(SDK: stdlib/slport "Stack usage",
  tools/tlmakmak.)*

## 10. Logging

- `RDebug::Print` goes to the serial debug port, which is the modem's port
  on a 5mx: useless in the field. Each app writes a small text log
  (`psimail.log`, capped at 32 KB and rotated) that the user can send; the
  engine's `pm_log` and `pg_set_link_log` feed it. Log the error code with
  every failed call, never passwords.

## 11. Installer (.pkg)

- The package UID is the app's UID3 (`0x1000xxxx` range from Symbian for
  released apps; `0x0100xxxx` is the "unprotected" test range);
  `makesis` `#{"Name"},(UID),major,minor,build` must match the `.aif`.
- One `!:` destination drive variable so the user can choose C: or D:;
  files that must be on C: (`\System\Apps\...`) are not optional.
- Embed dependencies as components (`@"stdlib.sis",(0x...)`) rather than
  copying DLLs so the uninstaller tracks them and never removes a newer
  copy another app needs.
- Mark data the app creates (`FN` flag, "file null") so uninstall deletes
  settings and caches; list every file the installer creates so Add/remove
  leaves nothing behind.
- Languages: `&EN` (plus others) at the top; `({"..."})` strings per language
  in the same order.
  *(SDK: adk/makesis, adk/makesis-ref.)*

## 12. Untrusted input (network and files)

- Every length from the wire is an attacker's number. Compare lengths as
  lengths (`len > end - p`), never as pointers (`p + len > end` wraps on
  32 bits), and never add a wire length to a buffer index in `int` without
  checking the sum (`imap.c`'s `{n}` literal; `tls13.c` record and
  handshake lengths; `http.c` chunk sizes; `pmimg` dimensions).
- Parsers are fuzzed on the host under ASan/UBSan (`mail/test/imgtest.py`
  for the picture decoders, `mail/test/parsefuzz.py` for IMAP, MIME,
  HTML, XML, iCalendar and header charsets). Add a word list and a target
  there when adding a parser.
- Every loop that advances through input must advance on every path (two
  hangs found by the fuzzer did not: `cs_decode_header` on `=?` that was not
  an encoded-word, `ics_change` on a VEVENT without `END`).
- Decoders that stream must document their output bound (`dec_feed` can
  write up to `n + 2` bytes for quoted-printable split across calls).

## 13. ARM alignment (5mx vs netBook)

- The toolchain compiles for `-mcpu=arm710 -mshort-load-bytes`: `short`
  loads are byte-wise, so 16-bit fields are safe anywhere. 32-bit loads
  through a pointer that is not 4-byte aligned are **not** safe: the
  ARM710T rotates the word (wrong value, no trap) and a StrongARM kernel
  with alignment checking raises a data abort (`KERN-EXEC 3`).
- Never cast a byte pointer to `uint32_t*`/`unsigned long*`; use the
  byte-wise `LOAD32H`/`STORE32L` macros (libtomcrypt is built
  `LTC_NO_FAST LTC_NO_ASM`, which selects them) or `memcpy`. Keep zlib's
  `UNALIGNED_OK` off. A quick check is `arm-pe-objdump -d` for `ldr r, [r, #k]`
  with `k % 4 != 0` in real code (none in `psissh.exe` today).

## Sources

SDK (`SDK/epoc_cpp_sdk/sysdoc/`): `cpp/e32/euclnp.html` (cleanup, TRAP),
`cpp/e32/eumem*.html` (memory), `cpp/e32/euuser.html` (`__UHEAP_*`,
`__DbgSetAllocFail`), `cpp/e32/eudesc*.html` (descriptors),
`cpp/e32/euactv.html`, `cpp/e32/euasyn.html` (active objects),
`cpp/e32/euthrd.html` (threads, processes, logon), `cpp/e32/eupanic*.html`
(panic categories), `cpp/f32/fsusing.html`, `cpp/f32/fssess.html`
(files, media), `cpp/cone/coenv.html`, `cpp/cone/coappui.html` (HandleError,
system events, PrepareToExit), `cpp/apparch/aaappdoc.html`,
`cpp/stdlib/slimplem.html`, `cpp/stdlib/slport.html` (ESTLIB, `_reent`,
`CloseSTDLIB`, 8 KB stacks), `cpp/tools/tlmakmak.html` (`epocstacksize`,
`epocheapsize`), `emul/emulator/emuldebug.html` (EIKON debug keys, heap
failure tool), `adk/makesis/`, `adk/makesis-ref/` (packages),
`er5supp/clnuputl/` (CleanupClosePushL and friends); headers
`epoc32/include/apgtask.h` (`EApaSystemEventShutdown`), `eikappui.h`,
`eikenv.h`. Examples: `SDK/epoc_cpp_sdk/epoc32ex/` (stdlib `slsumeik`
for a C engine under an EIKON UI).

Books (ER5 era, from memory of their guidance; not fetched):
*Professional Symbian Programming* (Tasker et al., 1999/2000) - cleanup
stack, two-phase construction, active objects, client-server;
*Symbian OS C++ for Mobile Phones* (Harrison, 2003) - the same idioms, plus
OOM testing with `__UHEAP_FAILNEXT`.

Open source consulted on the web: GitHub topic pages for
[epoc](https://github.com/topics/epoc?o=asc&s=updated) and
[epoc32](https://github.com/topics/epoc32) (VocabBox, Psion-Projects,
lua5.3-epoc32, PsionDoom - C/C++ for ER5), the
[psiconv](https://github.com/kianryan/psiconv) file-format library (host
side; defensive parsing of Psion files), [PsionDoom](https://github.com/doomhack/PsionDoom)
(PrBoom on ER5). The GitHub API and raw file fetches are blocked from this
environment, so their sources were not read; freepoc.org (the Psion
freeware archive, with Macro5 and the EPOC ports) refused the fetch
(robots.txt timeout) and was not worked around. The ARM Architecture
Reference Manual pages on ARMv4 alignment at developer.arm.com redirect to a
page whose content did not load; the alignment behaviour above is from the
ARMv4 architecture (A-bit, rotated loads) and the SA-1100 manual.
