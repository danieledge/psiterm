# EPOC R5 communications: capabilities, best practice, and how PsiTerm, PsiMail and PsiWeb measure up

Research of 2 October 2026. It draws on:
- the SDK documentation (`psion_cpp_sdk_linux/epoc_cpp_sdk/sysdoc/`);
- the SDK headers (`epoc32/include`);
- the SDK examples (`epoc32ex/comms/cmterm.cpp`, `echoeng.cpp`);
- later Symbian documentation, where the API is the same;
- community sources online.

It is a companion to `epoc-robustness-best-practices.md` (memory, leaves, active objects, power).

How reliable each claim is:
- The SDK is the authority for ER5. Online material specific to ER5 comms is thin: Psion's developer site and the EPOC Developer Network have gone.
- Points marked **[inferred]** are not stated anywhere and should be checked on the device.

## 1. What EPOC R5 offers

| Layer | API | Notes |
|---|---|---|
| Serial | C32 comms server: `RCommServ`, `RComm`, with `ECUART.CSY` on the built-in UART | 115200 baud is the most EPOC supports. The port is real ±12 V RS-232 with RTS, CTS, DTR, DCD and DSR. It cannot send or detect BREAK. RING is not available on ARM. |
| Sockets | ESOCK: `RSocketServ`, `RSocket`, `RHostResolver` | TCP and UDP through `TCPIP.PRT`. |
| Dial-up | NIFMAN (`RNif`) and NetDial | "Psion Internet": PPP over the serial port, started implicitly by the first name lookup or TCP connect. |
| Settings | CommDb | ISP and modem records, including the client and route timeouts. |
| Name lookup | Resolver | Tries `C:\System\Data\hosts`, then the DNS cache, then a UDP query. A query starts a dial-up if no link is up. |

There is no `RConnection` (that arrived in Symbian 7) and no TLS anywhere in the ROM. The apps' own `ssh/tls13.c` provides TLS.

## 2. Best-practice rules

Each rule gives its source and how the code complies.
- **SDK sources** are paths under `sysdoc/cpp/`: `commapi/csapi-NNN.html`, `esock/*.html`, `tcpip/*.html` and `e32/*.html`. `er5supp/proginfo/` covers NIFMAN.
- **Status:** ✓ means already done, **gap** means not done, **fixed** means changed with this research.
- **Code references:** psiglue means `ssh/psiglue.cpp`, which all three engines share.

### 2.1 Serial (C32 / RComm)

| # | Rule | Source | Status |
|---|---|---|---|
| S1 | Bring the port up in order: PDD and LDD, then `StartC32`, `RCommServ::Connect`, `LoadCommModule`, `RComm::Open`. Treat `KErrAlreadyExists` at each step as success. | csapi-003; cmterm.cpp | ✓ psiglue `OpenSerial` |
| S2 | Straight after `Open` and `SetConfig`, issue a zero-length `Read` to power the UART. RTS and DTR are not driven until then. | csapi-000, "Serial ports and power management" | ✓ |
| S3 | Never call `SetConfig` or `ResetBuffers` with a read, write or break pending: C32 panics the client. Cancel and wait first. | csapi-003 | ✓ `gComm->Cancel()` before every `SetConfig`; DcdArm/DcdDisarm too |
| S4 | After any cancel, wait on the request status. A cancelled request may have finished normally, with data. | csapi-003; e32/euasyn | ✓ every `Cancel` is followed by `WaitForRequest` |
| S5 | Never use XON/XOFF for binary streams. `ObeyXoff` strips 0x11 and 0x13 out of the data. | csapi-000, "Software flow control and data transparency" | ✓ never set |
| S6 | Hardware flow control: `ObeyCTS` stops sending while CTS is low. The receive high-water mark is fixed at 75 % of the buffer, low-water at 25 %, so size the buffer for baud × latency. | csapi-000 | ✓ selectable; a 16 KB buffer, read back with `ReceiveBufferLength()` |
| S7 | `SetReceiveBufferLength` fails silently if the size is too large: check `ReceiveBufferLength()`. | csapi-003 | ✓ logged |
| S8 | Use `KConfigFailDCD` to end pending I/O when the carrier drops. Many cables and Wi-Fi modems never raise DCD (Psion's own modem cable does not carry it), so arm it only once DCD has been seen high. | csapi-000/017; Psion cable pinouts; Eric Lindsay; PsionARA | ✓ DcdArm probes first, otherwise falls back to in-band `NO CARRIER` |
| S9 | Hayes hang-up: about 1 s of silence, `+++`, about 1 s, then `ATH`. The SDK says nothing about this; it comes from the modem firmware documentation. | Zimodem and WiRSa READMEs | ✓ 1.1 s guards |
| S10 | Open the port exclusive. Expect `KErrInUse` (another program has it) and `KErrAccessDenied` (the Remote link, or another program). | csapi-016; Psion support notes | **fixed:** `KErrAccessDenied` now has its own message ("held by the Remote link - switch it off on the System screen (Ctrl+L)"). It used to show as "could not set up the serial port (error -12)". |
| S11 | C32 can still hold an exclusive port briefly after another process closes it, so retry `Open` on `KErrInUse` before giving up. | Symbian BTComm notes (later OS) **[inferred for ER5]** | **fixed:** 3 retries, 300 ms apart |
| S12 | Call `UserHal::ResetAutoSwitchOffTimer()` while data flows. | csapi-000 | ✓ `KeepAwake` |
| S13 | `RTimer::After` and `User::After` stop counting while the Psion is off. Check the link again after switch-on, rather than trusting a timer that was running. | e32/eutimer | ✓ switch-on recheck |
| S14 | Use large reads; avoid one request per character. | csapi-000, "Optimizing data transfers" | ✓ 16 KB reads |
| S15 | Do not use BREAK or RING on the 5mx. | csapi-003 | ✓ not used |

### 2.2 Sockets (ESOCK / TCP)

| # | Rule | Source | Status |
|---|---|---|---|
| N1 | Read TCP with `RecvOneOrMore`. `Recv` and `Read` complete only when the buffer is full, and lose the last partial piece when the connection closes. | esock/essock; tcpip/tcpsock | ✓ |
| N2 | `Close()` on a connected socket is a synchronous `Shutdown(ENormal)`, which can block over a dead link. Shut down asynchronously, under a timer. A shutdown cannot be cancelled, and `CancelAll` does not cover it. | essock | ✓ async, bounded shutdown (0.68) |
| N3 | Use `EImmediate` (RST) when the link is suspect, `ENormal` (FIN) when it is healthy. | tcpsock | ✓ `NetCloseSocket(aAbort)` |
| N4 | Allow only one request of each kind per socket or resolver: a second one panics rather than returning an error. | essock; eshnres; esprot-008 | ✓ one connection, one request at a time |
| N5 | Give every `Connect`, `GetByName`, `Send` and `Shutdown` its own timer. ESOCK has none, and a dial-up can take about a minute. | echoeng.cpp; proginfo | ✓ lookup 60 s, connect 30 s, CONNECT 25 s |
| N6 | After a TCP error the socket is dead: close it and open a new one. | esprot-009 | ✓ |
| N7 | Closing the `RSocketServ` aborts every socket on it, and further calls on those sockets panic. Close the session last. | esserv; essock | ✓ `NetClose` closes the socket, then the resolver, then the session |
| N8 | An empty host name "resolves" to the Psion's own address. Reject it. | tcpip/tcpdns | **fixed:** `NetConnect` now refuses an empty name |
| N9 | The first lookup or connect can start a dial-up and NetDial's dialogs. Handle `KErrCancel` (the user cancelled), `KErrNetUnreach` (-190), `KErrAccessDenied` (port reserved), and the NetDial codes -3001 to -3006 and -3050 to -3057. | tcpsock; nd_err.h; in_iface.h | ✓ mostly: errors are reported with their number. A friendlier message per code would help (see 3.4). |
| N10 | Check whether a link is already up before dialling: `RNif::NetworkActive`, or enumerate interfaces with `KSoInetEnumInterfaces`. | nifman.h; tcputils | ✓ `NifActive`, then `LinkUp` |
| N11 | If you call `RNif::DisableTimers(ETrue)`, undo it on every exit path, including crashes. Otherwise the link never idles out. | nifman.h **[inferred]** | ✓ `NifRelease`; the apps call `PsiLinkTimersBack()` when an engine has crashed |
| N12 | Keep long, quiet sessions alive. Either set `KSoTcpKeepAlive` (interval undocumented) or run an application keepalive. | tcputils | ✓ SSH uses `-K` (about 30 s). PsiMail and PsiWeb connections are short. |
| N13 | Use one `RSocketServ` with enough message slots. The default of 8 covers one socket plus a resolver. | esserv | ✓ |
| N14 | Make every ESOCK call from the thread that opened the session. | e32/euasyn | ✓ the engine's main thread |

### 2.3 Dial-up (NIFMAN / NetDial)

- **What the docs cover.** `ProgressNotification`, `Progress` and `LastProgressError` are documented. The stages run `EStartingDialling`, …, `EConnectionOpen` (14), then `EIfProgressLinkUp` (1000) and `EIfProgressLinkDown` (1001). This is the only documented way to show what a dial-up is doing, or why it failed.
  - psiglue reads `Progress` while it waits, as "Still connecting (N s, stage)".
- **What the docs leave out.**
  - `RNif::Open`, `Stop`, `DisableTimers` and `NetworkActive` exist only in `nifman.h`, without descriptions.
  - When the link drops after the last client closes is undocumented. It is the user's "If idle, stay online for" setting, together with the CommDb timeouts, whose units are not stated.
  - Nothing says when PPP hands the serial port back.
  - Nothing describes the behaviour across switch-off.
- **What our apps rely on [inferred, observed on the device].**
  - Once `RSocketServ` is closed, the link stays up until the idle time runs out.
  - File > Connect (new in PsiTerm 0.76) depends on this: it dials, then lets go, so later SSH sessions, updates, mail checks and page loads find the link already up.

## 3. Recommendations

### 3.1 Done with this research
- **S10:** the Remote link holding the port is reported as such, in psiglue and in `psissh`.
- **S11:** opening a port that another process has only just closed is retried briefly.
- **N8:** an empty host name is refused before it can "resolve" to the Psion itself.
- **Connect:** File > Connect is in PsiTerm, and is added to PsiMail and PsiWeb in this change. It brings the link up ahead of use, for both routes.

### 3.2 Agreed with Dan and done in 0.78 (2 October 2026)

All four were approved. What was done: `ATNET0` is sent before every `ATDT` and an `ERROR` reply is ignored; the Test suggests RTS/CTS when the modem drives CTS; each dial-up stage is listed as it happens; the NetDial, PPP and TCP/IP error codes come with plain words (`ErrWords` in psiglue).


1. **Telnet handling on Wi-Fi modems (well sourced).**
   - What happens: WiFi232, WiRSa and RetroWiFiModem process telnet IAC bytes (0xFF) unless told not to: `ATNET0` on WiRSa, WiFi232 and RetroWiFiModem, or `ATDT` without the T modifier on Zimodem. In that mode they eat or double 0xFF bytes, which corrupts SSH and TLS. The result is the classic rare, unexplained MAC error.
   - Today: psiglue sends no such command. Our Atom firmware passes data through raw. It is not known whether WiRSa does telnet processing for `ATDT host:port` by default.
   - Suggestion: an optional "modem init" string, or `ATNET0` sent before `ATDT` when the modem accepts it (if the modem answers `ERROR`, ignore it).
2. **Hardware flow control by default for modems that carry CTS.**
   - Several independent reports say 115200 overruns without RTS/CTS.
   - A three-wire modem such as the Atom gives no CTS, so `ObeyCTS` would stop the Psion sending at all.
   - Suggestion: the Test could detect CTS and propose the right setting.
3. **Dial-up progress as busy messages.**
   - Use `RNif::ProgressNotification` in an active object, so each stage shows as it happens ("Dialling", "Logging in", "Connected") instead of a polled "Still connecting (N s)".
4. **A clear message for each NetDial and PPP error code** (-3001 to -3006, -3050 to -3057), instead of the number.

### 3.3 Worth knowing (single sources)
- **TCP port 135 (single source).** The ER5 TCP/IP stack is reported to freeze on unsolicited traffic to TCP port 135. A Psion behind NAT (a WiRSa or Pi gateway) is safe. Don't forward ports to it.
- **The `hosts` file (single source).** `C:\System\Data\hosts` is honoured by the resolver: a fallback when DNS fails, or a way to override a name.
- **OS logs (single source).** Creating the folders `C:\LOGS\Netdial\` and `C:\LOGS\Etel\` makes the OS write NetDial and ETEL logs. This is useful when a dial-up fails before our own `link:` lines can say why.
- **Server-side PPP.** If you run your own `pppd` (a Pi, or `socat` to a WiRSa), set `lcp-echo-interval` / `lcp-echo-failure` so a half-dead link is noticed.

## 4. Where the docs are silent (check on the device)

**Serial**
- What `FreeRTS` really does on ECUART. csapi-017 and csapi-000 disagree; the code's reading is explained in `OpenSerial`.
- Whether one read and one write may be outstanding at once (all real terminals do this).
- Whether `KConfigFailDCD` fires at once if DCD is already low when it is armed.
- What a switched-off Psion does to pending reads, to DTR and RTS, and to the port's settings.

**Sockets and dial-up**
- The error `RComm::Open` returns while PPP holds the port.
- What closing a socket does to a pending `Shutdown`.
- `KErrRoutePending` is documented but defined in no header.
- No default TCP or DNS timeouts are given, and no keepalive interval.

## Sources

**SDK (in this checkout's sibling `psion_cpp_sdk_linux/epoc_cpp_sdk/`)**
- `sysdoc/cpp/commapi/`: csapi-000 (using the comms server), -003 (RComm), -016 (TCommAccess), -017 (configuration constants).
- `sysdoc/cpp/esock/`: essock, esserv, eshnres, esprot-008 and -009.
- `sysdoc/cpp/tcpip/`: tcpsock, tcpdns, tcputils.
- `sysdoc/cpp/e32/`: euasyn, eutimer.
- `sysdoc/er5supp/proginfo/`: NIFMAN progress.
- Headers: `c32comm.h`, `d32comm.h`, `es_sock.h`, `in_sock.h`, `nifman.h`, `netdial.h`, `nd_err.h`, `in_iface.h`.
- Examples: `epoc32ex/comms/cmterm.cpp`, `echoeng.cpp`.

**Later Symbian docs (same APIs)**
- Serial handshaking guide: https://docs.huihoo.com/symbian/s60-3rd-edition-cpp-developers-library-v1.1/GUID-35228542-8C95-4849-A73F-2B4F082F0C44/html/SDL_93/doc_source/guide/Serial-Communications-subsystem-guide/SerialComms/SerialCommsServerClientGuide/SerialCommsServerClientGuide2/SettingUphandshaking.guide.html
- TCP/IP connect and disconnect: https://docs.huihoo.com/symbian/s60-3rd-edition-cpp-developers-library-v1.1/GUID-35228542-8C95-4849-A73F-2B4F082F0C44/html/SDL_93/doc_source/guide/Networking-subsystem-guide/TcpipGuide/TcpipGuide2/HowToConnectAndDisconnect.guide.html
- NIFMAN idle timer: https://docs.huihoo.com/symbian/nokia-symbian3-developers-library-v0.8/GUID-B380482B-CF42-50BF-B09C-F4B3BDAA1679.html
- Port release after Close (BTComm): http://devlib.symbian.slions.net/belle/GUID-3B899F4B-6B3B-5664-A344-021D5BBE76D2.html

**Psion and community**
- Psion, Modem connectivity and communications: https://docs.rs-online.com/2d51/0900766b800305f2.pdf
- Eric Lindsay's comms notes: http://www.ericlindsay.com/epoc/comm5.htm, http://www.ericlindsay.com/epoc/sinet5.htm, http://www.ericlindsay.com/epoc/mhint5.htm
- 5mx serial pinout: https://www.cloudcube.info/2025/03/27/diy-psion-series-5mx-serial-cable-at-a-relatively-low-cost/
- PsionARA settings: https://apps.hci.rwth-aachen.de/borchers-old/personal/psionara.html
- PsionNet: https://github.com/JYewman/PsionNet
- Sidecar (Pi PPP gateway): https://www.theregister.com/2023/01/03/sidecar_getting_psions_online/
- teelsys, 5mx on the Internet: https://teelsys.com/getting-psion-5mx-on-the-internet/
- PPP over a WiFi232: https://jcs.org/2020/09/03/wifi232_ppp
- WiRSa: https://github.com/nullvalue0/WiRSa
- Zimodem: https://github.com/bozimmerman/Zimodem
- RetroWiFiModem: https://github.com/mecparts/RetroWiFiModem
- Steve Litchfield's Psion internet guide: https://www.filesaveas.com/psionconnecting.html
