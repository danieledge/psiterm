// psiglue.cpp - native EPOC side of psissh.exe
//
// Owns the serial port (RComm) while an SSH session runs, and the shared
// chunk through which PsiTerm supplies keystrokes and receives output.
// Everything here is exposed as plain C functions (pg_*) for psishim.c.

#include <e32std.h>
#include <e32base.h>
#include <e32hal.h>
#include <c32comm.h>
#include <f32file.h>
#include <es_sock.h>
#include <in_sock.h>
#include <nifman.h>       // RNif: the Psion's network interface manager (nifman.lib)
#include <netdial.h>      // TNetDialProgress: the dial-up's stages (enum only)

extern "C" {
#include "psishared.h"
}

_LIT(KPddName, "EUART1");
_LIT(KLddName, "ECOMM");
_LIT(KCsyName, "ECUART");
_LIT(KPortName, "COMM::0");

// The apps compile only the Connection settings test (at the end) from this
// file, through pglinktest.cpp: an .app is a DLL and may not have the
// engine's writable statics below.
#ifndef PG_LINK_TEST_ONLY

// EXEs may have writable static data on EPOC R5 (unlike .app DLLs), but
// global objects with constructors are avoided: handles live on the heap.
static RChunk* gChunk = 0;
static PsiShared* gShared = 0;
static RCommServ* gServer = 0;
static RComm* gComm = 0;
static int gCommOpen = 0;
static TUint8 gRx[1024];
static int gRxLen = 0;
static int gRxPos = 0;
static int gNetClosed = 0;
extern "C" int pg_net_closed() { return gNetClosed; }
// serial line errors (overrun, framing, parity) seen by RxFill since the
// port was opened: PsiMail logs them when a connection is lost
static int gRxErrors = 0, gRxLastErr = 0;
extern "C" int pg_rx_errors(int* aLast)
	{
	if (aLast) *aLast = gRxLastErr;
	return gRxErrors;
	}

// Psion TCP/IP mode (off by default): instead of the WiRSa's "ATDT host:port"
// pipe, connect a real socket through EPOC's own networking - normally a
// dial-up (PPP) connection to the WiRSa dialled with "ATDT PPP".
static int gNet = 0;
static int gServerOpen = 0;
static int gNetSent = 0, gNetGotData = 0;   // for the progress messages
static RSocketServ* gSs = 0;
static int gSsOpen = 0;
static RSocket* gSock = 0;
static int gSockOpen = 0;
static RTimer* gTimer = 0;
static int gTimerOpen = 0;
static TRequestStatus gRecvStat;
static TSockXfrLength gRecvLen;
static TPtr8* gRecvDes = 0;
static int gRecvPending = 0;

// How long the Psion Internet steps may take (a 115200 WiFi modem PPP link).
// The first name lookup is what makes EPOC's NetDial open the port, run the
// modem init and bring PPP up, so it gets the longest wait; a TCP connect
// normally finds the link already up.
const TInt KLookupTimeoutUs   = 60000000;  // GetByName incl. the dial-up
const TInt KConnectTimeoutUs  = 30000000;  // RSocket::Connect
const TInt KCancelWaitUs      = 5000000;   // for a Cancel() to complete the request
const TInt KStillWaitingUs    = 15000000;  // "still waiting" notes this often
const int  KPppConnectWaitMs  = 25000;     // StartPpp: CONNECT after the first send
const TInt64 KLinkFailHoldUs  = TInt64(8000000);   // after a failed start: fail fast

// The lookup and connect requests keep their TRequestStatus here, not on the
// stack: if a Cancel() does not complete one (NetDial busy with its own
// dialogs), the request is abandoned and may complete later - into memory
// that must still exist. gLookupOrphan/gConnOrphan say one is still out.
static TRequestStatus gLookupStat;
static TRequestStatus gConnStat;
static int gLookupOrphan = 0, gConnOrphan = 0;
static RHostResolver* gResolver = 0;
static int gResolverOpen = 0;
static TNameEntry* gNameEntry = 0;
static TInt64 gLinkFailAt;                 // when a link start last failed (0 = never)
static int gLinkSuspect = 0;               // a receive or send failed: PPP may be down

// The last name looked up, kept while the session (and so the link) lasts:
// a reconnect to the same server then skips GetByName. Over a dial-up each
// lookup is a round trip to the ISP's DNS, and it is the step that stalls
// when the link has quietly gone.
static char gAddrHost[128];
static TUint32 gAddr = 0;
static TInt64 gAddrAt;
const TInt64 KAddrKeepUs = TInt64(600000000);      // 10 minutes

// An app without a terminal (PsiMail) can have every link message in its
// own log: it sets this and LinkMsg calls it. PsiTerm never sets it.
static void (*gLinkLog)(const char*) = 0;
extern "C" void pg_set_link_log(void (*aFn)(const char*)) { gLinkLog = aFn; }

// (0.68) NIFMAN, the manager of the Psion's Internet connection. While an
// app is online over it, an RNif session holds its idle timers off (the
// Control panel's "disconnect after n minutes idle" would otherwise drop
// PPP in the middle of a long sync), and answers NetworkActive() - is the
// link really up - before a lookup that would otherwise dial again.
static RNif* gNif = 0;
static int gNifOpen = 0;
static int gNifTimersOff = 0;             // DisableTimers(ETrue) is in force
static unsigned int gSwitchOnSeen = 0;    // gShared->switch_on last acted on
static TInt64 gKeepAwakeAt;               // ResetAutoSwitchOffTimer last called
static int gKeptAwake = 0;                // ...and said so in the log (once per connection)
const TInt64 KKeepAwakeEveryUs = TInt64(30000000);
const TInt KShutdownWaitUs    = 3000000;  // RSocket::Shutdown before giving up on it
const TInt KProgressPollUs    = 1000000;  // WaitLink asks NIFMAN for its stage this often

// The socket shutdown's request status lives here for the same reason as
// gLookupStat (see above): a Shutdown() cannot be cancelled.
static TRequestStatus gShutStat;
static int gShutOrphan = 0;

// (0.68) modem route: carrier detect. After CONNECT, if the modem drives
// DCD (it is high), the port is set to fail reads and writes at once
// (KErrCommsLineFail, -29) when DCD drops - the carrier has gone - instead
// of waiting for "NO CARRIER" text or a timeout. A modem that leaves DCD
// low (a WiRSa without &C1, a cable without the wire) gets the old way.
static int gDcdFail = 0;                  // KConfigFailDCD is set on the port
static int gDcdLost = 0;                  // a read or write failed with -29: no carrier
static TUint gCommHandshake = 0;          // iHandshake without the DCD bit
static int gNetClosedWhy = 0;             // the error that set gNetClosed (0: EOF/none)
static int gModemOnline = 0;              // modem route: CONNECT seen, not hung up since
static int gLinkDoubt = 0;                // switched on with no DCD to ask: probe the link (pg_take_link_doubt)

// 1 once after a switch-on left the modem link in doubt (no carrier detect
// to ask): the protocol should send something the server answers, now.
extern "C" int pg_take_link_doubt()
	{
	int d = gLinkDoubt;
	gLinkDoubt = 0;
	return d;
	}

static int LinkOpen()
	{
	return gNet ? gSockOpen : gCommOpen;
	}

static void SetMsg(char* aOut, int aMax, const char* aText)
	{
	if (!aOut || aMax <= 0)
		return;
	int k = 0;
	while (aText[k] && k < aMax - 1) { aOut[k] = aText[k]; k++; }
	aOut[k] = 0;
	}

// Plain words for the errors a connection meets (NetDial nd_err.h, PPP
// in_iface.h, TCP/IP in_sock.h, E32), so a message says what went wrong
// rather than only a number
static const char* ErrWords(TInt aErr)
	{
	switch (aErr)
		{
	case -3:    return "cancelled";
	case -21:   return "the serial port is reserved - is the Remote link on?";
	case -33:   return "timed out";
	case -34:   return "the server refused the connection";
	case -36:   return "the connection was closed";
	case -190:  return "the network cannot be reached - the dial-up failed";
	case -191:  return "the server cannot be reached";
	case -3001: return "the modem did not answer the dial-up";
	case -3002: return "the modem reported an error";
	case -3003: return "the login to the Internet service failed";
	case -3004: return "the login script timed out";
	case -3005: return "the login script failed";
	case -3006: return "no Internet service is set up (Control panel > Internet)";
	case -3050: return "the Internet service refused the user name or password";
	case -3051: return "the Internet service wants a more secure login";
	case -3052: return "the Internet account is disabled";
	case -3053: return "the Internet account may not log in at this time";
	case -3054: return "the Internet account's password has expired";
	case -3055: return "the Internet account may not dial in";
	case -3056: return "the Internet service wants the password changed";
	case -3057: return "the Internet service's call-back was not accepted";
	default:    return 0;
		}
	}

static void SetMsgErr(char* aOut, int aMax, const char* aText, TInt aErr)
	{
	if (!aOut || aMax < 16)
		return;
	TPtr8 p((TUint8*)aOut, 0, aMax - 1);
	p.Copy(TPtrC8((const TUint8*)aText));
	const char* w = ErrWords(aErr);
	if (w && p.Length() + 4 + (TInt)User::StringLength((const TUint8*)w) + 12 < aMax)
		{
		p.Append(_L8(": "));
		p.Append(TPtrC8((const TUint8*)w));
		}
	p.AppendFormat(_L8(" (error %d)"), aErr);
	p.ZeroTerminate();
	}

static TInt64 NowMicro();
static void RxFill(int aTimeoutUs);
static void LinkLog(const char* aText);
extern "C" int pg_quit_requested();

// Abandoned requests (gLookupOrphan and friends) that have completed: note
// it, so that nothing waits on them again (see WaitFor).
static void NoteOrphans()
	{
	if (gLookupOrphan && gLookupStat != KRequestPending)
		{
		gLookupOrphan = 0;
		LinkLog("the abandoned lookup has completed");
		}
	if (gConnOrphan && gConnStat != KRequestPending)
		{
		gConnOrphan = 0;
		LinkLog("the abandoned connect has completed");
		}
	if (gShutOrphan && gShutStat != KRequestPending)
		{
		gShutOrphan = 0;
		LinkLog("the abandoned shutdown has completed");
		}
	}

// Makes the one timer every wait here uses. 1 if there is one.
static int TimerReady()
	{
	if (gTimerOpen)
		return 1;
	if (!gTimer) gTimer = new RTimer;
	if (gTimer && gTimer->CreateLocal() == KErrNone)
		gTimerOpen = 1;
	return gTimerOpen;
	}

// Waits for aStat for at most aTimeoutUs, giving up early if the app asks
// us to quit and aQuitAware is set.
// Returns 1 if aStat has completed, 0 if not (aStat is then still outstanding).
//
// The thread's request semaphore (e32/euasyn): every completion adds one
// count, and every User::WaitForRequest takes counts until the status it was
// given has completed. The two-status wait below therefore swallows the
// count of any *other* request that completes meanwhile - an abandoned
// lookup, or the receive that pg_wait leaves outstanding while NetWrite
// waits. So the rule for this file: never wait on a status that has already
// completed. Its count may be gone, and a wait with nothing else outstanding
// then never returns (the hang after "Securing the connection", seen on the
// device). The other way round - a count left over for a status nobody waits
// on again - does no harm: the engines run no active scheduler, and every
// wait here loops until its own status has completed.
static int WaitFor(TRequestStatus& aStat, TInt aTimeoutUs, int aQuitAware)
	{
	if (aStat != KRequestPending)
		return 1;                        // completed already: never wait on it (above)
	if (!TimerReady())
		{
		// no timer (handles exhausted?): an unbounded wait could hang the
		// engine with no quit check, so this counts as a timeout instead and
		// the caller cancels its request as it would after one
		LinkLog("wait: no timer; treating the wait as timed out");
		return 0;
		}
	TInt64 start = NowMicro();
	for (;;)
		{
		TInt slice = 250000;
		if (aTimeoutUs >= 0)
			{
			TInt64 left = TInt64(aTimeoutUs) - (NowMicro() - start);
			if (left <= 0)
				return 0;
			if (left < slice)
				slice = left.Low();
			if (slice < 1000)
				slice = 1000;
			}
		TRequestStatus timerStat;
		gTimer->After(timerStat, slice);
		for (;;)
			{
			User::WaitForRequest(aStat, timerStat);
			NoteOrphans();               // an orphan done meanwhile: its count went with this wait
			if (aStat != KRequestPending)
				{
				gTimer->Cancel();
				if (timerStat == KRequestPending)
					User::WaitForRequest(timerStat);   // the cancel completes it
				return 1;
				}
			if (timerStat != KRequestPending)
				break;                   // the slice is over
			// neither (a stray count on the semaphore): keep waiting on the
			// same timer - a second After() on a running RTimer would panic
			}
		if (aQuitAware && pg_quit_requested())
			return 0;
		}
	}

// After Cancel(): waits a bounded time for the request to complete. Returns
// 1 if it did, 0 if it is still outstanding (abandoned).
static int TakeCancelled(TRequestStatus& aStat)
	{
	return WaitFor(aStat, KCancelWaitUs, 0);
	}

// Abandoned requests: give each a bounded wait, and forget those that have
// completed. (WaitFor returns at once for a status that has completed.)
static void ReapOrphans(TInt aWaitUs)
	{
	if (gLookupOrphan && WaitFor(gLookupStat, aWaitUs, 0))
		gLookupOrphan = 0;
	if (gConnOrphan && WaitFor(gConnStat, aWaitUs, 0))
		gConnOrphan = 0;
	if (gShutOrphan && WaitFor(gShutStat, aWaitUs, 0))
		gShutOrphan = 0;
	}

// ----- the log --------------------------------------------------------------
// LinkLog: a line for the app's log only (psimail.log "link:" lines), not
// for the status line - the decisions this file takes about the link.
static void LinkLog(const char* aText)
	{
	if (gLinkLog)
		gLinkLog(aText);
	}

static void LinkLogErr(const char* aText, TInt aErr)
	{
	if (!gLinkLog)
		return;
	char m[160];
	SetMsgErr(m, sizeof(m), aText, aErr);
	LinkLog(m);
	}

static void LinkLogNum(const char* aText, TInt aNum)
	{
	if (!gLinkLog)
		return;
	char m[160];
	TPtr8 p((TUint8*)m, 0, sizeof(m) - 1);
	p.Copy(TPtrC8((const TUint8*)aText));
	p.AppendNum(aNum);
	p.ZeroTerminate();
	LinkLog(m);
	}

// ----- NIFMAN ---------------------------------------------------------------

// Opens the session with the network interface manager. RNif::Open only
// connects to the manager: nothing here dials.
static int NifOpen()
	{
	if (gNifOpen)
		return 1;
	if (!gNif) gNif = new RNif;
	if (!gNif)
		return 0;
	TInt r = gNif->Open();
	if (r != KErrNone)
		{
		LinkLogErr("nifman: could not open a session", r);
		return 0;
		}
	gNifOpen = 1;
	return 1;
	}

// The link is up and the app is online: hold NIFMAN's idle timers off, so
// the Control panel's idle disconnect does not drop PPP mid-sync. Undone by
// NifRelease when the app goes offline (NetClose), hangs up or exits.
static void NifHoldLink()
	{
	if (!NifOpen() || gNifTimersOff)
		return;
	TInt r = gNif->DisableTimers(ETrue);
	if (r == KErrNone)
		{
		gNifTimersOff = 1;
		LinkLog("nifman: idle timers disabled while online");
		}
	else
		LinkLogErr("nifman: could not disable the idle timers", r);
	}

// Gives the idle timers back to NIFMAN and closes the session, so the
// Psion's own idle hang-up works again once the app is done with the link.
static void NifRelease()
	{
	if (!gNifOpen)
		return;
	if (gNifTimersOff)
		{
		TInt r = gNif->DisableTimers(EFalse);
		gNifTimersOff = 0;
		if (r == KErrNone)
			LinkLog("nifman: idle timers enabled again");
		else
			LinkLogErr("nifman: could not enable the idle timers again", r);
		}
	gNif->Close();
	gNifOpen = 0;
	}

// Is the Psion's Internet connection up, by NIFMAN's own account?
// 1 yes, 0 no, -1 it would not say (no session).
static int NifActive()
	{
	if (!gNifOpen)
		return -1;
	TBool active = EFalse;
	if (gNif->NetworkActive(active) != KErrNone)
		return -1;
	return active ? 1 : 0;
	}

// NetDial's stages (TNetDialProgress) in plain words; 0 for the rest
static const char* NifStageName(TInt aStage)
	{
	switch (aStage)
		{
	case EStartingSelection:
	case EFinishedSelection:  return "choosing the service";
	case EStartingDialling:   return "dialling";
	case EFinishedDialling:   return "dialled";
	case EScanningScript:
	case EScannedScript:      return "running the login script";
	case EGettingLoginInfo:
	case EGotLoginInfo:       return "getting the login details";
	case EStartingConnect:    return "connecting";
	case EFinishedConnect:    return "connected";
	case EStartingLogIn:      return "logging in";
	case EFinishedLogIn:      return "logged in";
	case EConnectionOpen:     return "connection open";
	case EStartingHangUp:     return "hanging up";
	case EFinishedHangUp:     return "hung up";
	default:                  return 0;
		}
	}

// The dial-up's current stage, if NIFMAN says (else -1)
static TInt NifStage()
	{
	if (!gNifOpen)
		return -1;
	TNifProgress p;
	if (gNif->Progress(p) != KErrNone)
		return -1;
	return p.iStage;
	}

// ----- keeping the Psion awake ---------------------------------------------
// Called whenever data has just flowed: every 30 s of that, the auto
// switch-off timer is reset, so the Psion does not switch off in the middle
// of a download. An idle connection lets it sleep as before.
static void KeepAwake()
	{
	TInt64 now = NowMicro();
	if (gKeepAwakeAt != TInt64(0) && now - gKeepAwakeAt < KKeepAwakeEveryUs)
		return;
	gKeepAwakeAt = now;
	UserHal::ResetAutoSwitchOffTimer();
	if (!gKeptAwake)
		{
		gKeptAwake = 1;
		LinkLog("keeping the Psion awake while data flows (auto switch-off timer reset every 30 s)");
		}
	}

// A short name for the error codes people meet while the dial-up starts
static const char* ErrName(TInt aErr)
	{
	switch (aErr)
		{
	case -3:    return "cancelled";                         // KErrCancel: the Psion's own dialog
	case -33:   return "timed out";
	case -34:   return "could not connect";
	case -36:   return "disconnected";
	case -2003: return "the modem reported NO CARRIER";     // KErrEtelNoCarrier
	case -2004: return "the modem reported BUSY";
	case -2008: return "the modem reported NO ANSWER";      // KErrEtelNoAnswer
	case -2009: return "no dial tone";
	case -2017: return "no modem found";                    // KErrEtelModemNotDetected
	case -3001: return "no reply from the modem";           // KErrExitNoModem
	case -3002: return "modem error";
	case -3003: return "login failed";
	case -3004: return "the dial-up script timed out";
	case -3005: return "dial-up script error";
	default:    return 0;
		}
	}

static void SetMsgNet(char* aOut, int aMax, const char* aText, TInt aErr)
	{
	if (!aOut || aMax < 16)
		return;
	TPtr8 p((TUint8*)aOut, 0, aMax - 1);
	p.Copy(TPtrC8((const TUint8*)aText));
	const char* name = ErrName(aErr);
	if (name)
		{
		p.Append(_L8(" ("));
		p.Append(TPtrC8((const TUint8*)name));
		p.AppendFormat(_L8(", error %d)"), aErr);
		}
	else
		p.AppendFormat(_L8(" (error %d)"), aErr);
	p.ZeroTerminate();
	}

// Closes the TCP connection but keeps the socket server session, and with it
// the Psion's dial-up. An update fetches several files and pieces, one TCP
// connection each: closing the session every time made EPOC take the dial-up
// down and bring it back between pieces, and that churn crashed the socket
// server (KERN-EXEC 3).
//
// (0.68) RSocket::Close() on a connected socket is a Shutdown(ENormal) that
// waits, without limit, for the FIN handshake - with the link gone that
// froze the app until TCP gave up. So: an asynchronous Shutdown first, given
// 3 s; EImmediate (a reset, which needs no reply) when aAbort says the link
// is suspect. A shutdown cannot be cancelled: one that does not complete is
// left behind (its status is static), and the socket with it - closing the
// session (NetClose) takes it down abortively, with no waiting.
static void NetCloseSocket(int aAbort)
	{
	if (!gSockOpen)
		return;
	if (gRecvPending)
		{
		// (WaitFor: no wait if the receive has completed already - its count
		// went with the wait it completed during; a cancel completes it at
		// once otherwise. Its status is static, so even a late completion
		// lands in memory that exists.)
		gSock->CancelRecv();
		if (!WaitFor(gRecvStat, KCancelWaitUs, 0))
			LinkLog("socket: the cancelled receive did not complete in 5 s; going on (its status is static)");
		gRecvPending = 0;
		}
	gSockOpen = 0;
	if (gShutOrphan)
		{
		// the previous socket's shutdown is still out: this one gets no
		// second static status - close it abortively via the session later
		LinkLog("socket: an earlier shutdown is still pending: leaving this socket to the session");
		RSocket* fresh = new RSocket;
		if (fresh) gSock = fresh;
		return;
		}
	gShutStat = KRequestPending;
	gSock->Shutdown(aAbort ? RSocket::EImmediate : RSocket::ENormal, gShutStat);
	if (WaitFor(gShutStat, KShutdownWaitUs, 0))
		{
		if (gShutStat.Int() != KErrNone && gShutStat.Int() != KErrDisconnected && gShutStat.Int() != KErrEof)
			LinkLogErr(aAbort ? "socket: shutdown (immediate) completed" : "socket: shutdown completed", gShutStat.Int());
		gSock->Close();
		return;
		}
	LinkLog(aAbort ? "socket: the immediate shutdown did not complete in 3 s: leaving the socket to the session"
	               : "socket: the shutdown did not complete in 3 s (link down?): leaving the socket to the session");
	gShutOrphan = 1;
	RSocket* fresh = new RSocket;           // the old object stays with its request
	if (fresh) gSock = fresh;
	}

// Closes everything ESOCK: the socket, a resolver still open and the session
// itself. With no client left NetDial stops (or, after a failed start, does
// not try again by itself), and the next pg_dial starts from scratch: StartPpp
// again, a fresh dial-up. Closing the session also completes any request a
// Cancel() could not, so abandoned requests are collected here.
static void NetClose()
	{
	NetCloseSocket(1);              // the link is going anyway: no FIN handshake to wait for
	if (gResolverOpen)
		{
		gResolver->Close();
		gResolverOpen = 0;
		}
	NifRelease();                   // idle timers back on before the session goes
	if (gSsOpen)
		{
		gSs->Close();               // EPOC hangs the dial-up up after its idle time
		gSsOpen = 0;
		}
	ReapOrphans(2000000);
	gLinkSuspect = 0;
	gAddrHost[0] = 0;               // a new link may see the name differently
	}

// A link start (lookup or connect) failed: start afresh next time, and for
// a few seconds fail at once instead of dialling again - PsiWeb's images and
// PsiMail's calendar sync follow a failed connection straight away, and each
// new dial-up brings the Psion's own connection dialogs back.
static void NetFail()
	{
	NetClose();
	gLinkFailAt = NowMicro();
	}

// SSH mode says what the link is doing (updates stay quiet)
extern "C" void pg_out_write(const void* aData, int aLen);
static int gDialVerbose = 0;
extern "C" void pg_dial_verbose(int aOn) { gDialVerbose = aOn; }
static void LinkMsg(const char* aText);
static void Say(const char* aText)
	{
	int n = 0;
	while (aText[n]) n++;
	LinkMsg(aText);
	if (gDialVerbose)
		pg_out_write(aText, n);
	}

// the one-line status only (not in PsiTerm's terminal)
static void LinkMsg(const char* aText)
	{
	if (gShared)
		{
		// also as a one-line status for apps without a terminal
		const char* p = aText;
		while (*p == ' ' || *p == '\r' || *p == '\n') p++;
		int k = 0;
		while (p[k] && p[k] != '\r' && p[k] != '\n' && k < (int)sizeof(gShared->link_msg) - 1)
			{
			gShared->link_msg[k] = p[k];
			k++;
			}
		if (k > 0)
			{
			gShared->link_msg[k] = 0;
			gShared->link_seq++;
			if (gLinkLog)
				gLinkLog(gShared->link_msg);
			}
		}
	}

// Is an IP interface - the PPP link - up on the open socket server session?
// Asks the TCP/IP stack to list its interfaces (KSoInetEnumInterfaces, an
// ER5 API on any socket); nothing here starts a dial-up. Returns 1 if one
// is up with an address, 0 if none is, -1 if the stack would not say.
static int LinkUp()
	{
	if (!gSsOpen)
		return 0;
	RSocket probe;
	if (probe.Open(*gSs, KAfInet, KSockDatagram, KProtocolInetUdp) != KErrNone)
		return -1;
	int up = 0, listed = 0;
	if (probe.SetOpt(KSoInetEnumInterfaces, KSolInetIfCtrl) == KErrNone)
		{
		TPckgBuf<TSoInetInterfaceInfo> info;
		while (probe.GetOpt(KSoInetNextInterface, KSolInetIfCtrl, info) == KErrNone)
			{
			TUint32 a = info().iAddress.Address();
			listed++;
			if ((info().iState == EIfUp || info().iState == EIfBusy) && a != 0 && (a >> 24) != 127)
				up = 1;
			}
		}
	else
		listed = -1;
	probe.Close();
	if (listed < 0)
		return -1;
	return up;
	}

// Explains the dial-up errors people meet when setting this up
static void NetHint(TInt aErr)
	{
	if (aErr == -2017)                   // KErrEtelModemNotDetected
		Say(gShared->ppp_start[0]
			? "  The Psion's dial-up found no modem. PsiTerm has already started PPP,\r\n"
			  "  so set the Internet service's Connection type to Direct.\r\n"
			: "  The Psion's dial-up found no modem: check Control panel > Modems\r\n"
			  "  (speed, flow control, a plain AT init string).\r\n");
	}

// Waits for a lookup/connect request with a hard timeout, noting every so
// often that it is still waiting (the dial-up can take a while and the app
// shows the last note). Returns 1 done, 0 timed out, -1 stopped by the user.
// (0.68) Meanwhile NIFMAN is asked, once a second, what stage the dial-up
// is at (TNetDialProgress): each new stage goes on the status line, and the
// "still waiting" note says where it has got to.
static int WaitLink(TRequestStatus& aStat, TInt aTimeoutUs, const char* aWhat)
	{
	TInt waited = 0, sinceNote = 0;
	TInt lastStage = -1;
	for (;;)
		{
		TInt slice = aTimeoutUs - waited;
		if (slice > KProgressPollUs)
			slice = KProgressPollUs;
		if (slice <= 0)
			return 0;
		if (WaitFor(aStat, slice, 1))
			return 1;
		if (pg_quit_requested())
			return -1;
		waited += slice;
		sinceNote += slice;
		TInt stage = NifStage();
		const char* stageName = NifStageName(stage);
		if (stage >= 0 && stage != lastStage)
			{
			lastStage = stage;
			if (stageName)
				{
				char m[100];
				TPtr8 p((TUint8*)m, 0, sizeof(m) - 1);
				p.Format(_L8("  Psion Internet: %s...\r\n"), stageName);
				p.ZeroTerminate();
				Say(m);                      // (the status line, and the terminal or update window)
				}
			}
		if (sinceNote >= KStillWaitingUs)
			{
			sinceNote = 0;
			char m[160];
			TPtr8 p((TUint8*)m, 0, sizeof(m) - 1);
			if (stageName)
				p.Format(_L8("  Still %s (%d s, %s)...\r\n"), aWhat, waited / 1000000, stageName);
			else
				p.Format(_L8("  Still %s (%d s)...\r\n"), aWhat, waited / 1000000);
			p.ZeroTerminate();
			Say(m);
			}
		}
	}

static int NetConnect(char* aResult, int aMax)
	{
	if (!gSs) gSs = new RSocketServ;
	if (!gSock) gSock = new RSocket;
	if (!gTimer) gTimer = new RTimer;
	if (!gRecvDes) gRecvDes = new TPtr8(gRx, 0, sizeof(gRx));
	if (!gResolver) gResolver = new RHostResolver;
	if (!gNameEntry) gNameEntry = new TNameEntry;
	if (!gSs || !gSock || !gTimer || !gRecvDes || !gResolver || !gNameEntry)
		{
		SetMsg(aResult, aMax, "out of memory");
		return -1;
		}
	if (!gTimerOpen && gTimer->CreateLocal() == KErrNone)
		gTimerOpen = 1;
	TInt r;
	if (!gSsOpen)
		{
		r = gSs->Connect();
		if (r != KErrNone)
			{
			SetMsgErr(aResult, aMax, "Psion networking is not available", r);
			return -1;
			}
		gSsOpen = 1;
		}
	NifOpen();                               // for the stages while it dials; no dial itself

	TBuf<128> host;
	host.Copy(TPtrC8((const TUint8*)gShared->host));
	if (host.Length() == 0)
		{
		// (an empty name "resolves" to the Psion's own address: tcpdns.html)
		SetMsg(aResult, aMax, "no server name given");
		return -1;
		}
	TInetAddr addr;
	int lookedUp = 0, fromCache = 0;
	int sameHost = 1;
	for (int i = 0; ; i++)
		{
		if (gAddrHost[i] != gShared->host[i]) { sameHost = 0; break; }
		if (!gAddrHost[i] || i >= (int)sizeof(gAddrHost) - 1) break;
		}
	if (addr.Input(host) != KErrNone && gAddrHost[0] && sameHost && gAddr != 0
		&& NowMicro() - gAddrAt < KAddrKeepUs)
		{
		// looked up a moment ago on this same link: no need to ask again
		addr.SetAddress(gAddr);
		fromCache = 1;
		Say("  Using the address looked up a moment ago.\r\n");
		}
	else if (addr.Input(host) != KErrNone)
		{
		// a name: look it up (this is usually what starts the dial-up)
		lookedUp = 1;
		r = gResolver->Open(*gSs, KAfInet, KProtocolInetUdp);
		if (r != KErrNone)
			{
			SetMsgErr(aResult, aMax, "no name resolver - is TCP/IP installed?", r);
			NetFail();
			return -1;
			}
		gResolverOpen = 1;
		{
		char m[160];
		int k = 0;
		const char* a = "  Looking up ";
		while (*a) m[k++] = *a++;
		const char* h = (const char*)gShared->host;
		while (*h && k < 120) m[k++] = *h++;
		a = " (this starts the Psion's Internet connection)...\r\n";
		while (*a && k < 158) m[k++] = *a++;
		m[k] = 0;
		Say(m);
		}
		gLookupStat = KRequestPending;
		gResolver->GetByName(host, *gNameEntry, gLookupStat);
		int w = WaitLink(gLookupStat, KLookupTimeoutUs, "waiting for the Psion's Internet connection");
		if (w != 1)
			{
			gResolver->Cancel();
			if (!TakeCancelled(gLookupStat))
				gLookupOrphan = 1;       // NetClose (below) should complete it
			if (w < 0)
				SetMsg(aResult, aMax, "Stopped");
			else
				{
				// (the host is clipped: Format panics rather than truncates,
				// and the host field is 128 bytes)
				char m[160];
				TPtr8 p((TUint8*)m, 0, sizeof(m) - 1);
				TPtrC8 h((const TUint8*)gShared->host);
				if (h.Length() > 60) h.Set(h.Left(60));
				p.Format(_L8("Timed out looking up %S: the Psion's Internet connection did not come up in %d s"),
					&h, KLookupTimeoutUs / 1000000);
				p.ZeroTerminate();
				SetMsg(aResult, aMax, m);
				}
			NetFail();
			return -1;
			}
		gResolver->Close();
		gResolverOpen = 0;
		if (gLookupStat.Int() != KErrNone)
			{
			NetHint(gLookupStat.Int());
			char m[160];
			TPtr8 p((TUint8*)m, 0, sizeof(m) - 1);
			TPtrC8 h((const TUint8*)gShared->host);
			if (h.Length() > 60) h.Set(h.Left(60));
			p.Format(_L8("Could not connect: looking up %S failed"), &h);
			p.ZeroTerminate();
			SetMsgNet(aResult, aMax, m, gLookupStat.Int());
			NetFail();
			return -1;
			}
		addr = TInetAddr((*gNameEntry)().iAddr);
		{
		int i = 0;
		while (gShared->host[i] && i < (int)sizeof(gAddrHost) - 1) { gAddrHost[i] = gShared->host[i]; i++; }
		gAddrHost[i] = 0;
		gAddr = addr.Address();
		gAddrAt = NowMicro();
		}
		}
	addr.SetPort(gShared->port > 0 ? gShared->port : 22);
	{
	TBuf<40> a;
	addr.Output(a);
	char m[80];
	int k = 0;
	const char* t = "  Connecting to ";
	while (*t) m[k++] = *t++;
	for (TInt i = 0; i < a.Length() && k < 60; i++) m[k++] = (char)a[i];
	m[k++] = ':';
	char num[8];
	TInt nd = 0;
	TUint port = addr.Port();
	do { num[nd++] = (char)('0' + port % 10); port /= 10; } while (port && nd < 7);
	while (nd) m[k++] = num[--nd];
	t = "...\r\n";
	while (*t) m[k++] = *t++;
	m[k] = 0;
	Say(m);
	}

	r = gSock->Open(*gSs, KAfInet, KSockStream, KProtocolInetTcp);
	if (r != KErrNone)
		{
		SetMsgErr(aResult, aMax, "could not open a TCP socket", r);
		NetFail();
		return -1;
		}
	gConnStat = KRequestPending;
	gSock->Connect(addr, gConnStat);     // starts the dial-up if it isn't up
	// a numeric host (or a remembered address) skips the lookup, so then
	// this is the step that dials
	int w = WaitLink(gConnStat, lookedUp ? KConnectTimeoutUs : KLookupTimeoutUs, "connecting");
	if (w != 1)
		{
		gSock->CancelConnect();
		if (!TakeCancelled(gConnStat))
			gConnOrphan = 1;
		gSock->Close();
		SetMsg(aResult, aMax, w < 0 ? "Stopped" : "Timed out connecting to the server");
		NetFail();
		return -1;
		}
	if (gConnStat.Int() != KErrNone)
		{
		gSock->Close();
		if (fromCache)
			gAddrHost[0] = 0;            // look the name up afresh next time
		NetHint(gConnStat.Int());
		SetMsgNet(aResult, aMax, "Could not connect to the server", gConnStat.Int());
		NetFail();
		return -1;
		}
	gSockOpen = 1;
	gLinkFailAt = TInt64(0);
	gNetSent = gNetGotData = 0;
	gNetClosedWhy = 0;
	Say("  TCP connection open.\r\n");
	NifHoldLink();                           // online: no idle disconnect until NetClose
	return 0;
	}


// Moves a completed receive into gRx. Keeps one receive outstanding at all
// times instead of cancelling, so no data can be lost to a cancel race.
static void NetRxFill(int aTimeoutUs)
	{
	if (gRxPos < gRxLen || !gSockOpen || gNetClosed)
		return;
	if (!gRecvPending)
		{
		gRecvDes->Set(gRx, 0, sizeof(gRx));
		gSock->RecvOneOrMore(*gRecvDes, 0, gRecvStat, gRecvLen);
		gRecvPending = 1;
		}
	// A receive that completed during another wait (NetWrite's, a lookup's:
	// pg_wait leaves one outstanding when its slice ends) has had its count
	// taken by that wait, so it must not be waited on again - that was the
	// hang after "Securing the connection". WaitFor returns at once for a
	// status that has completed; otherwise it waits, bounded.
	if (!WaitFor(gRecvStat, aTimeoutUs, 0))
		return;                          // still waiting
	gRecvPending = 0;
	gRxPos = 0;
	if (gRecvStat.Int() == KErrNone)
		{
		gRxLen = gRecvDes->Length();
		if (gRxLen > 0)
			KeepAwake();
		if (!gNetGotData && gRxLen > 0)
			{
			gNetGotData = 1;
			LinkMsg("  Receiving the reply...\r\n");
			}
		}
	else
		{
		gRxLen = 0;
		gNetClosed = 1;                  // KErrEof or a link error
		gNetClosedWhy = gRecvStat.Int() == KErrEof ? 0 : gRecvStat.Int();
		if (gRecvStat.Int() == KErrEof)
			LinkMsg("  The server closed the connection.\r\n");
		else
			{
			// not a clean close: PPP itself may have gone. The next
			// pg_dial asks the stack (LinkUp) and, if it has, starts the
			// link afresh (StartPpp and all)
			gLinkSuspect = 1;
			char m[80];
			SetMsgNet(m, sizeof(m), "  The connection dropped", gRecvStat.Int());
			LinkMsg(m);
			}
		}
	}

static int NetWrite(const void* aBuf, int aLen)
	{
	if (!gSockOpen)
		return -1;
	TPtrC8 data((const TUint8*)aBuf, aLen);
	TRequestStatus stat;
	gSock->Write(data, stat);
	if (!WaitFor(stat, 60000000, 0))
		{
		gSock->CancelWrite();
		User::WaitForRequest(stat);
		gLinkSuspect = 1;                // a minute without an ACK: PPP may be gone
		LinkMsg("  Sending to the server timed out.\r\n");
		return -1;
		}
	if (stat.Int() != KErrNone)
		{
		char m[80];
		gLinkSuspect = 1;
		SetMsgNet(m, sizeof(m), "  Could not send to the server", stat.Int());
		LinkMsg(m);
		return -1;
		}
	if (!gNetSent)
		{
		gNetSent = 1;
		LinkMsg("  Request sent, waiting for the reply...\r\n");
		}
	KeepAwake();
	return aLen;
	}

static TBps BaudFromIndex(int aIndex)
	{
	switch (aIndex)
		{
	case 0: return EBps9600;
	case 1: return EBps19200;
	case 2: return EBps38400;
	case 3: return EBps57600;
	default: return EBps115200;
		}
	}

// Elapsed time for every wait and timeout here, from the system tick (64 Hz
// on the 5mx: User::TickCount), not from the clock. TTime has a one-second
// step on the Psion, so a wait measured by it ran on to the next whole
// second - a 20 ms wait (PsiWeb's input poll over Psion Internet) took up to
// a second, and every timeout rounded up. The count is kept monotonic across
// the tick counter's wrap, and the user setting the clock does not move it.
// (Ticks stop while the Psion is off, as RTimer does; SwitchOnCheck covers
// what happened meanwhile.) Never 0: "0 = never" is used by the callers.
static TInt64 gMonoUs;
static TUint gMonoTick = 0;
static TInt gTickUs = 0;

static TInt64 NowMicro()
	{
	if (gTickUs <= 0)
		{
		TTimeIntervalMicroSeconds32 period;
		if (UserHal::TickPeriod(period) != KErrNone || period.Int() <= 0)
			period = 15625;
		gTickUs = period.Int();
		gMonoTick = User::TickCount();
		gMonoUs = TInt64(1);
		}
	TUint now = User::TickCount();
	TUint ticks = now - gMonoTick;       // (unsigned: right across the wrap)
	gMonoTick = now;
	gMonoUs += TInt64(ticks) * TInt64(gTickUs);
	return gMonoUs;
	}

extern "C" PsiShared* pg_shared()
	{
	return gShared;
	}

// Opens COMM::0 for talking to the modem. 0 = open, else a negative code:
// -10 = another program has the port (PsiTerm, PsiMail, PsiWeb, or the
// Psion's own dial-up).
static int OpenSerial()
	{
	TInt r = User::LoadPhysicalDevice(KPddName);
	if (r != KErrNone && r != KErrAlreadyExists)
		return -4;
	r = User::LoadLogicalDevice(KLddName);
	if (r != KErrNone && r != KErrAlreadyExists)
		return -5;
	r = StartC32();
	if (r != KErrNone && r != KErrAlreadyExists)
		return -6;
	if (!gServer) gServer = new RCommServ;
	if (!gComm) gComm = new RComm;
	if (!gServer || !gComm)
		return -7;
	if (!gServerOpen)
		{
		r = gServer->Connect();
		if (r != KErrNone)
			return -8;
		gServerOpen = 1;
		}
	r = gServer->LoadCommModule(KCsyName);
	if (r != KErrNone && r != KErrAlreadyExists)
		return -9;
	// A port another process has just closed can still be held for a moment
	// (C32 releases an exclusive port after Close returns), so "in use" is
	// tried again briefly. KErrAccessDenied is the Remote link (or another
	// program) holding it: its own code, so the message can say what to do.
	r = gComm->Open(*gServer, KPortName, ECommExclusive);
	for (int tries = 0; r == KErrInUse && tries < 3; tries++)
		{
		User::After(300000);
		r = gComm->Open(*gServer, KPortName, ECommExclusive);
		}
	if (r != KErrNone)
		return r == KErrInUse ? -10 : r == KErrAccessDenied ? -13 : -12;
	gCommOpen = 1;

	TCommConfig cfg;
	gComm->Config(cfg);
	cfg().iRate = BaudFromIndex(gShared->baud_index);
	cfg().iDataBits = EData8;
	cfg().iStopBits = EStop1;
	cfg().iParity = EParityNone;
	cfg().iFifo = EFifoEnable;
	cfg().iTerminatorCount = 0;
	// Flow control (0.68). "RTS/CTS on" is KConfigObeyCTS alone:
	//  - ObeyCTS: we stop sending while the modem holds CTS low (csapi-000,
	//    "Flow control when sending").
	//  - RTS, our side of it, is the driver's to lower at the 75 % high-water
	//    mark and raise at 25 %, and it is so unless KConfigFreeRTS is set:
	//    the constants table (csapi-017) says KConfigFreeRTS means "the
	//    output RTS line should NOT be controlled by the driver", and the
	//    port defaults (csapi-000, "Serial Port Defaults": ObeyCTS only,
	//    "DTR and RTS are both held high") are exactly the driver-controlled
	//    case. The guide's prose reads the bit the other way round, but the
	//    name, the table and the defaults agree: Free = hands off. With it
	//    set (as up to 0.67, after the SDK's cmterm example) RTS would be a
	//    fixed high: no receive flow control at all, which fits the overruns
	//    seen at 115200 with "RTS/CTS on". To check on a real 5mx: stop
	//    reading, let the buffer pass 75 %, and watch Signals(KSignalRTS).
	//  - Never KConfigObeyXoff: the driver would eat 0x11/0x13 out of the
	//    SSH/TLS byte stream (csapi-000, "Software flow control and data
	//    transparency").
	// "RTS/CTS off" is 0: no bit obeyed, so a modem or cable that does not
	// drive CTS cannot stall every Write for ever (the port's default is
	// ObeyCTS, so it must be set explicitly).
	cfg().iHandshake = gShared->rtscts ? KConfigObeyCTS : 0;
	gCommHandshake = cfg().iHandshake;
	gComm->Cancel();                         // never SetConfig with I/O pending (it panics)
	r = gComm->SetConfig(cfg);
	if (r != KErrNone)
		{
		gComm->Close();
		gCommOpen = 0;
		return -11;
		}
	// Bigger than Dropbear's 8 KB SSH receive window plus packet overhead,
	// so the server can never send more than fits here: no overruns even
	// at 115200 without working hardware flow control. SetReceiveBufferLength
	// is silently ignored if too big, so the size is read back and logged.
	gComm->SetReceiveBufferLength(16384);
	LinkLogNum(gShared->rtscts ? "serial: handshake ObeyCTS (driver controls RTS), receive buffer "
	                           : "serial: handshake none, receive buffer ", gComm->ReceiveBufferLength());
	gComm->SetSignals(KSignalDTR | KSignalRTS, 0);
	// The UART is unpowered, and DTR not really asserted, until the first
	// read or write: a zero-length read powers it up now (cmterm.cpp does the
	// same), so the modem sees DTR before we talk to it.
		{
		TRequestStatus wake;
		TBuf8<4> none;
		gComm->Read(wake, none, 0);
		User::WaitForRequest(wake);
		}
	gDcdFail = 0;
	gDcdLost = 0;
	gRxErrors = 0;
	gRxLastErr = 0;
	return 0;
	}

// ----- carrier detect (modem route) -----------------------------------------

// After CONNECT: if the modem has raised DCD, make the port fail any read or
// write the moment DCD drops (KConfigFailDCD -> KErrCommsLineFail, -29). A
// modem that has not (a WiRSa without &C1, a 3-wire cable) keeps the old
// in-band NO CARRIER detection only. Every decision is logged.
static void DcdArm()
	{
	if (!gCommOpen || gDcdFail)
		return;
	TCommCaps caps;
	gComm->Caps(caps);
	if (!(caps().iSignals & KCapsSignalDCDSupported))
		{
		LinkLog("carrier detect: this port has no DCD line; FailDCD off, relying on NO CARRIER");
		return;
		}
	// the modem may take a moment after CONNECT to raise DCD
	TUint sig = 0;
	for (int i = 0; i < 4; i++)
		{
		sig = gComm->Signals(KSignalDCD);
		if (sig & KSignalDCD)
			break;
		User::After(100000);
		}
	if (!(sig & KSignalDCD))
		{
		LinkLog("carrier detect: DCD low after CONNECT (modem not set to &C1?); FailDCD off, relying on NO CARRIER");
		return;
		}
	// SetConfig must not have I/O pending, and should not have data waiting
	// in the port either (the server's first bytes): take what fits, and if
	// more is already flowing leave the port alone this time.
	if (gRxPos >= gRxLen && gComm->QueryReceiveBuffer() > 0)
		RxFill(1000);
	if (gComm->QueryReceiveBuffer() > 0)
		{
		LinkLog("carrier detect: DCD high but data is already arriving; FailDCD left off this call");
		return;
		}
	TCommConfig cfg;
	gComm->Config(cfg);
	cfg().iHandshake = gCommHandshake | KConfigFailDCD;
	gComm->Cancel();
	TInt r = gComm->SetConfig(cfg);
	if (r != KErrNone)
		{
		LinkLogErr("carrier detect: DCD high but SetConfig(FailDCD) failed; relying on NO CARRIER", r);
		return;
		}
	gDcdFail = 1;
	LinkLog("carrier detect: DCD high after CONNECT: FailDCD on (reads fail with -29 when the carrier drops)");
	}

// Back to the plain configuration before talking AT to the modem again (with
// FailDCD on and no carrier, every read and write would fail at once).
static void DcdDisarm()
	{
	if (!gCommOpen || !gDcdFail)
		return;
	TCommConfig cfg;
	gComm->Config(cfg);
	cfg().iHandshake = gCommHandshake;
	gComm->Cancel();
	TInt r = gComm->SetConfig(cfg);
	gDcdFail = 0;
	if (r != KErrNone)
		LinkLogErr("carrier detect: could not turn FailDCD off", r);
	else
		LinkLog("carrier detect: FailDCD off (back at the modem's prompt)");
	}

// ----- switched back on -----------------------------------------------------
// PsiTerm/PsiMail/PsiWeb add 1 to switch_on when the Psion is switched back
// on. The UART was unpowered meanwhile and the modem or the ISP may have
// hung up: ask NIFMAN (PPP) or the DCD line (modem) rather than wait on a
// dead socket. A dead link marks the connection closed - the current
// operation ends, and the next pg_dial starts the link again.
static void SwitchOnCheck()
	{
	if (!gShared || gShared->switch_on == gSwitchOnSeen)
		return;
	gSwitchOnSeen = gShared->switch_on;
	if (gNet)
		{
		if (!gSsOpen)
			{
			LinkLog("switch-on: not online (no Internet session open)");
			return;
			}
		int up = NifActive();
		if (up < 0)
			up = LinkUp();
		if (up == 1)
			{
			LinkLog("switch-on: the Psion's Internet connection is still up");
			return;
			}
		gLinkSuspect = 1;
		if (gSockOpen && !gNetClosed)
			{
			gNetClosed = 1;
			gNetClosedWhy = KErrDisconnected;
			LinkMsg(up == 0 ? "  The Psion's Internet connection went while the Psion was off.\r\n"
			                : "  The Psion's Internet connection is in doubt after switching on.\r\n");
			}
		else
			LinkLog(up == 0 ? "switch-on: the Psion's Internet connection has gone; the next connection starts it again"
			                : "switch-on: could not tell if the Psion's Internet connection is up; it is checked at the next connection");
		return;
		}
	if (!gCommOpen)
		{
		LinkLog("switch-on: the serial port is not open");
		return;
		}
	if (!gDcdFail)
		{
		// No DCD to ask (a 3-wire cable, a modem without &C1): whether the
		// modem still has the call shows only when something is sent and
		// answered. Say so, and let the protocol probe it at once (psissh
		// sends an SSH keepalive: pg_take_link_doubt) rather than have the
		// user type into a dead line until the keepalive limit notices.
		if (gModemOnline && !gNetClosed)
			{
			gLinkDoubt = 1;
			LinkMsg("  The modem link is in doubt after switching on - checking...\r\n");
			}
		else
			LinkLog("switch-on: modem port open, no carrier detect to check (FailDCD off)");
		return;
		}
	if (gComm->Signals(KSignalDCD) & KSignalDCD)
		{
		LinkLog("switch-on: the modem still has the carrier (DCD high)");
		return;
		}
	gDcdLost = 1;
	if (!gNetClosed)
		{
		gNetClosed = 1;
		gNetClosedWhy = KErrCommsLineFail;
		LinkMsg("  The modem lost the connection while the Psion was off (no carrier).\r\n");
		}
	else
		LinkLog("switch-on: DCD low, the connection was already closed");
	}

// Opens the chunk the app created. Returns 0, or <0 (see pg_init).
extern "C" int pg_attach()
	{
	if (gShared)
		return 0;
	if (!gChunk)
		gChunk = new RChunk;
	if (!gChunk)
		return -1;
	TInt r = gChunk->OpenGlobal(_L(PSI_SHARED_NAME), EFalse);
	if (r != KErrNone)
		return -2;
	gShared = (PsiShared*)gChunk->Base();
	if (gShared->magic != PSI_SHARED_MAGIC)
		{
		gShared = 0;
		gChunk->Close();
		return -3;
		}
	gNet = gShared->net_mode ? 1 : 0;   // Psion TCP/IP: the socket opens in pg_dial
	// the one timer every bounded wait uses: without it a wait would have no
	// limit (or, now, fail at once), so an engine does not start without it
	if (!TimerReady())
		{
		gShared = 0;
		gChunk->Close();
		return -20;
		}
	return 0;
	}

// Opens and sets up the serial port (modem mode; nothing to do for TCP/IP).
// -10 = another program has the port.
extern "C" int pg_link_open()
	{
	if (!gShared)
		return -2;
	if (gNet || gCommOpen)
		return 0;
	return OpenSerial();
	}

// Lets go of the serial port (or the TCP/IP link) so another program -
// PsiTerm, say - can use it. pg_dial opens it again when needed.
extern "C" void pg_link_close()
	{
	NetClose();
	if (gCommOpen)
		{
		gComm->Close();
		gCommOpen = 0;
		}
	gModemOnline = 0;
	gLinkDoubt = 0;
	gRxPos = gRxLen = 0;
	}

extern "C" int pg_link_is_open()
	{
	return gNet ? gSsOpen : gCommOpen;
	}

extern "C" int pg_init()
	{
	TInt r = pg_attach();
	if (r != 0)
		return r;
	if (gShared->mode == 1)
		return 0;                   // speed test: no serial port needed
	return pg_link_open();
	}

extern "C" void pg_close()
	{
	pg_link_close();
	ReapOrphans(2000000);
	if (gTimerOpen)
		{
		gTimer->Close();
		gTimerOpen = 0;
		}
	if (gServer && gServerOpen)
		{
		gServer->Close();
		gServerOpen = 0;
		}
	}

extern "C" void pg_set_state(int aState)
	{
	if (gShared)
		gShared->state = aState;
	}

extern "C" void pg_set_exit(int aCode)
	{
	if (gShared)
		{
		gShared->exit_code = aCode;
		gShared->state = PSI_STATE_EXITED;
		}
	}

// (0.69) PsiTerm's heartbeat (app_beat) stopped for KAppGoneTicks: the app
// has gone, so psissh must stop too (it would otherwise hold the port and
// the chunk until a reset: an EXE without a window is not in the task list).
// Apps that never beat (PsiMail, PsiWeb, old PsiTerm) are not watched.
const TUint KAppGoneTicks = 64 * 45;
static unsigned int gLastBeat = 0;
static TUint gBeatSeenAt = 0;
static int gAppGone = 0;

static int AppGone()
	{
	if (gAppGone)
		return 1;
	unsigned int beat = gShared->app_beat;
	if (beat == 0 && gLastBeat == 0)
		return 0;                       // an app that never beats
	TUint now = User::TickCount();
	if (beat != gLastBeat || gBeatSeenAt == 0)
		{
		gLastBeat = beat;
		gBeatSeenAt = now;
		return 0;
		}
	if (now - gBeatSeenAt < KAppGoneTicks)
		return 0;
	gAppGone = 1;
	LinkLog("the app's heartbeat stopped: quitting");
	return 1;
	}

extern "C" int pg_quit_requested()
	{
	if (!gShared)
		return 1;
	return gShared->quit || AppGone();
	}

extern "C" void pg_msleep(int aMs)
	{
	User::After(aMs * 1000);
	}

// Ends the engine's process here and now, with aCode as the exit reason the
// app reads (RProcess::ExitReason). psishim calls it after CloseSTDLIB(),
// which frees the C library's per-thread state: returning to ecrt0, whose
// exit() would use that state again, is not an option then.
extern "C" void pg_exit_process(int aCode)
	{
	User::Exit(aCode);
	}

// ----- serial ("network") ---------------------------------------------------

// The carrier went (FailDCD: -29 on a read or write): the connection is
// over. Said once; pg_hangup then skips the +++ ATH the modem no longer needs.
static void CarrierLost(TInt aErr)
	{
	gDcdLost = 1;
	gModemOnline = 0;
	if (gNetClosed)
		return;
	gNetClosed = 1;
	gNetClosedWhy = aErr;
	char m[100];
	SetMsgErr(m, sizeof(m), "  The modem lost the connection: no carrier", aErr);
	LinkMsg(m);
	}

extern "C" int pg_serial_write(const void* aBuf, int aLen)
	{
	SwitchOnCheck();
	if (gNet)
		return aLen > 0 ? NetWrite(aBuf, aLen) : -1;
	if (!gCommOpen || aLen <= 0)
		return -1;
	if (gDcdLost && gDcdFail)
		return -1;                       // no carrier: the write would fail at once anyway
	TPtrC8 data((const TUint8*)aBuf, aLen);
	TRequestStatus stat;
	gComm->Write(stat, TTimeIntervalMicroSeconds32(10000000), data);
	User::WaitForRequest(stat);
	if (stat.Int() == KErrCommsLineFail && gDcdFail)
		CarrierLost(stat.Int());
	if (stat.Int() == KErrNone)
		KeepAwake();
	return (stat.Int() == KErrNone) ? aLen : -1;
	}

// Pull whatever arrives within aTimeoutUs into the receive buffer.
static void RxFill(int aTimeoutUs)
	{
	if (gNet)
		{
		NetRxFill(aTimeoutUs);
		return;
		}
	if (gRxPos < gRxLen || !gCommOpen)
		return;
	if (gDcdLost && gDcdFail)
		return;                          // no carrier: every read fails at once
	gRxPos = 0;
	gRxLen = 0;
	TPtr8 p(gRx, 0, sizeof(gRx));
	TRequestStatus stat;
	gComm->Read(stat, TTimeIntervalMicroSeconds32(aTimeoutUs), p);
	User::WaitForRequest(stat);
	if (stat.Int() == KErrNone || stat.Int() == KErrTimedOut)
		{
		gRxLen = p.Length();
		if (gRxLen > 0)
			KeepAwake();
		}
	else if (stat.Int() == KErrCommsLineFail && gDcdFail)
		CarrierLost(stat.Int());         // DCD dropped: not a line error, the line has gone
	else
		{
		gRxLen = 0;      // line error: drop this burst, SSH will notice
		gRxErrors++;
		gRxLastErr = stat.Int();
		}
	}

extern "C" int pg_net_avail()
	{
	return gRxLen - gRxPos;
	}

extern "C" int pg_net_read(void* aBuf, int aMax)
	{
	int n = gRxLen - gRxPos;
	if (n > aMax)
		n = aMax;
	if (n > 0)
		{
		Mem::Copy(aBuf, gRx + gRxPos, n);
		gRxPos += n;
		}
	return n;
	}

extern "C" void pg_net_set_closed()
	{
	gNetClosed = 1;
	}

// ----- keyboard / screen rings ----------------------------------------------

extern "C" int pg_kbd_avail()
	{
	if (!gShared)
		return 0;
	return (int)(gShared->kbd_head - gShared->kbd_tail);
	}

extern "C" int pg_kbd_read(void* aBuf, int aMax)
	{
	unsigned int tail = gShared->kbd_tail;
	int n = (int)(gShared->kbd_head - tail);
	if (n > aMax)
		n = aMax;
	if (n <= 0)
		return 0;
	unsigned int off = tail % PSI_KBD_SIZE;
	int run = PSI_KBD_SIZE - (int)off;
	if (run > n)
		run = n;
	Mem::Copy(aBuf, gShared->kbd + off, run);
	if (n > run)
		Mem::Copy((unsigned char*)aBuf + run, gShared->kbd, n - run);
	gShared->kbd_tail = tail + n;
	return n;
	}

extern "C" void pg_out_write(const void* aBuf, int aLen)
	{
	const unsigned char* in = (const unsigned char*)aBuf;
	while (aLen > 0)
		{
		// copy as much as fits in at most two runs (the ring may wrap),
		// then publish the new head: one reader (PsiTerm), one writer.
		unsigned int head = gShared->out_head;
		int space = PSI_OUT_SIZE - (int)(head - gShared->out_tail);
		if (space <= 0)
			{
			if (pg_quit_requested())    // (or the app has gone: nobody will drain it)
				return;
			User::After(10000);         // PsiTerm will drain it shortly
			continue;
			}
		int n = aLen < space ? aLen : space;
		unsigned int off = head % PSI_OUT_SIZE;
		int run = PSI_OUT_SIZE - (int)off;
		if (run > n)
			run = n;
		Mem::Copy(gShared->out + off, in, run);
		if (n > run)
			Mem::Copy(gShared->out, in + run, n - run);
		gShared->out_head = head + n;
		in += n;
		aLen -= n;
		}
	}

extern "C" void pg_winsize(int* aRows, int* aCols)
	{
	*aRows = gShared ? gShared->rows : 24;
	*aCols = gShared ? gShared->cols : 80;
	if (*aRows < 5) *aRows = 24;
	if (*aCols < 20) *aCols = 80;
	}

extern "C" int pg_take_resize()
	{
	if (gShared && gShared->resized)
		{
		gShared->resized = 0;
		return 1;
		}
	return 0;
	}

// ----- waiting ----------------------------------------------------------------
// Returns a bit mask: 1 = network data, 2 = keyboard data, 4 = resize, 8 = quit.
// aMs < 0 waits forever.
extern "C" int pg_wait(int aMs, int aWantNet, int aWantKbd)
	{
	TInt64 start = NowMicro();
	for (;;)
		{
		int mask = 0;
		SwitchOnCheck();                    // switched back on? is the link still there
		if (aWantNet && (pg_net_avail() > 0 || gNetClosed))
			mask |= 1;
		if (aWantKbd && pg_kbd_avail() > 0)
			mask |= 2;
		if (gShared && gShared->resized)
			mask |= 4;
		if (pg_quit_requested())
			mask |= 8;
		if (mask)
			return mask;
		int slice = 15625;                  // look at the keyboard every tick (1/64 s)
		if (aMs >= 0)
			{
			TInt64 left = TInt64(aMs) * 1000 - (NowMicro() - start);
			if (left <= 0)
				return 0;
			if (left < slice)
				slice = left.Low();
			if (slice < 1000)
				slice = 1000;
			}
		if (aWantNet && LinkOpen() && !gNetClosed)
			RxFill(slice);
		else
			User::After(slice);
		}
	}

// ----- dialling the WiRSa -----------------------------------------------------

static int ReadLine(char* aLine, int aMax, int aTimeoutMs)
	{
	int n = 0;
	TInt64 start = NowMicro();
	for (;;)
		{
		if (pg_net_avail() == 0)
			{
			if ((NowMicro() - start) > TInt64(aTimeoutMs) * 1000)
				return -1;
			if (pg_quit_requested())
				return -1;
			RxFill(100000);
			continue;
			}
		char c;
		pg_net_read(&c, 1);
		if (c == '\r' || c == '\n')
			{
			if (n == 0)
				continue;
			aLine[n] = 0;
			return n;
			}
		if (n < aMax - 1)
			aLine[n++] = c;
		}
	}

static int StartsWith(const char* aS, const char* aP)
	{
	while (*aP)
		{
		if (*aS++ != *aP++)
			return 0;
		}
	return 1;
	}

// Dials host:port through the WiRSa. Returns 0 on CONNECT.
// Sends AT and waits briefly for OK: is the modem at its command prompt?
// SSH mode says what the modem is doing (updates dial ten times, quietly)

static int ModemAnswersAt()
	{
	char line[160];
	pg_serial_write("AT\r", 3);
	for (int i = 0; i < 4; i++)
		{
		int n = ReadLine(line, sizeof(line), 800);
		if (n < 0)
			return 0;
		if (StartsWith(line, "OK"))
			return 1;
		}
	return 0;
	}

// Sends a command and waits for its result: 1 OK, 0 ERROR, -1 nothing
static int ModemCommand(const char* aCmd)
	{
	char line[160];
	int n = 0;
	while (aCmd[n]) n++;
	pg_serial_write(aCmd, n);
	pg_serial_write("\r", 1);
	for (int i = 0; i < 4; i++)
		{
		if (ReadLine(line, sizeof(line), 800) < 0)
			return -1;
		if (StartsWith(line, "OK"))
			return 1;
		if (StartsWith(line, "ERROR"))
			return 0;
		}
	return -1;
	}

// Psion Internet: before the Psion's TCP/IP starts, send the modem the
// user's "first send" command (e.g. ATDT777, which puts a WiRSa into PPP)
// and wait for CONNECT. Nothing here is specific to one modem: the command
// comes from Connection settings, and an empty one skips this step.
//   - port busy: the Psion's dial-up already has it, so the link is up
//   - no OK to AT: the modem may already be in PPP; carry on and let the
//     TCP/IP connection find out
// Returns 0 to go on, -1 on a definite failure (aResult set).
static int StartPpp(char* aResult, int aResultMax)
	{
	char line[160];
	const char* cmd = gShared->ppp_start;
	gNet = 0;                            // the serial helpers below talk to COMM::0
	int r = OpenSerial();
	if (r != 0)
		{
		if (gCommOpen) { gComm->Close(); gCommOpen = 0; }
		gNet = 1;
		gRxPos = gRxLen = 0;
		if (r == -10)
			Say("  Serial port busy - the Internet connection is probably up already.\r\n");
		else if (r == -13)
			Say("  The serial port is held by the Remote link (System screen, Ctrl+L) or another program - trying anyway.\r\n");
		else
			Say("  Could not open the serial port to start PPP - trying anyway.\r\n");
		return 0;
		}
	int rc = 0;
	pg_serial_write("\r", 1);
	pg_msleep(300);
	gRxPos = gRxLen = 0;
	gComm->ResetBuffers();
	Say("  Checking the modem... ");
	int at = ModemAnswersAt();
	if (!at)
		{
		// still in PPP from a start that the Psion never finished? Escape
		// to the command prompt and hang up, as the modem mode does
		Say("no answer - hanging up an old connection first\r\n");
		pg_msleep(1100);
		pg_serial_write("+++", 3);
		pg_msleep(1100);
		pg_serial_write("ATH\r", 4);
		pg_msleep(500);
		gRxPos = gRxLen = 0;
		gComm->ResetBuffers();
		at = ModemAnswersAt();
		if (!at)
			Say("  The modem does not answer AT - it may already be in PPP; going on.\r\n");
		}
	if (at)
		{
		Say("OK\r\n  Sending ");
		Say(cmd);
		Say("\r\n");
		pg_msleep(100);
		gRxPos = gRxLen = 0;
		gComm->ResetBuffers();
		int clen = 0;
		while (cmd[clen] && clen < (int)sizeof(gShared->ppp_start)) clen++;
		pg_serial_write(cmd, clen);
		pg_serial_write("\r", 1);
		rc = -1;
		SetMsg(aResult, aResultMax, "Could not connect: no CONNECT from the modem after the Psion Internet start command");
		TInt64 start = NowMicro();
		for (;;)
			{
			TInt64 leftUs = TInt64(KPppConnectWaitMs) * 1000 - (NowMicro() - start);
			if (leftUs <= 0)
				{
				Say("  No CONNECT from the modem.\r\n");
				break;
				}
			int n = ReadLine(line, sizeof(line), (leftUs / TInt64(1000)).Low());
			if (n < 0)
				{
				if (pg_quit_requested())
					SetMsg(aResult, aResultMax, "Stopped");
				else
					Say("  No reply from the modem.\r\n");
				break;
				}
			if (StartsWith(line, "AT") || StartsWith(line, "at"))
				continue;                // its echo of the command
			Say("  Modem: ");
			Say(line);
			Say("\r\n");
			if (StartsWith(line, "CONNECT"))
				{
				rc = 0;
				break;
				}
			if (StartsWith(line, "ERROR") || StartsWith(line, "NO CARRIER")
				|| StartsWith(line, "BUSY") || StartsWith(line, "NO ANSWER")
				|| StartsWith(line, "NO DIAL"))
				{
				SetMsg(aResult, aResultMax, "Could not connect: the modem refused the Psion Internet start command (see Connection settings)");
				break;
				}
			}
		}
	// hand the port to the Psion's TCP/IP (closing it drops DTR; set the
	// modem not to hang up on that, e.g. AT&D0, if it is a real modem)
	gComm->Close();
	gCommOpen = 0;
	gNet = 1;
	gRxPos = gRxLen = 0;
	if (rc == 0)
		pg_msleep(200);
	return rc;
	}

extern "C" int pg_dial(char* aResult, int aResultMax)
	{
	char cmd[220];
	char line[160];
	int i;
	SwitchOnCheck();                     // (before gNetClosed is reset below)
	gNetClosed = 0;                      // a fresh connection (update does two)
	gNetClosedWhy = 0;
	gKeptAwake = 0;
	gModemOnline = 0;
	gLinkDoubt = 0;
	gRxPos = gRxLen = 0;
	if (gNet)
		{
		if (pg_quit_requested())
			{
			SetMsg(aResult, aResultMax, "Stopped");
			return -1;
			}
		// a link start failed a moment ago: say so at once rather than
		// dial again (and bring the Psion's connection dialogs back)
		if (gLinkFailAt != TInt64(0) && NowMicro() - gLinkFailAt < KLinkFailHoldUs)
			{
			SetMsg(aResult, aResultMax, "The Psion's Internet connection failed a moment ago - try again in a few seconds");
			return -1;
			}
		ReapOrphans(0);
		if (gLookupOrphan || gConnOrphan)
			{
			// the last attempt's request never completed, even after closing
			// the session: leave ESOCK alone rather than pile a second one on
			// (and no other waits happen while one is out - see WaitFor)
			SetMsg(aResult, aResultMax, "The Psion's Internet connection is still busy with the last attempt - wait a moment and try again");
			return -1;
			}
		// The last TCP connection, if a caller left it open (a close without
		// a hang-up): a second RSocket::Open on the same handle would leak
		// the ESOCK subsession, and its pending receive would be taken for
		// the new connection's - which then looked closed or dropped.
		NetCloseSocket(gLinkSuspect);
		if (gLinkSuspect)
			{
			// a receive or send failed: was it the TCP connection (a server
			// closing, a reset on the way) or the PPP link itself? Ask
			// NIFMAN (NetworkActive), or failing that the stack, before
			// throwing the dial-up away: bringing it back costs half a
			// minute and the Psion's connection dialogs.
			int up = NifActive();
			if (up < 0)
				up = LinkUp();
			else
				LinkLog(up ? "nifman: NetworkActive says the link is up" : "nifman: NetworkActive says the link is down");
			if (up == 1)
				{
				Say("  The Psion's Internet connection is still up.\r\n");
				gLinkSuspect = 0;
				}
			else
				{
				Say(up == 0 ? "  The Psion's Internet connection has gone: starting it again.\r\n"
				            : "  The connection dropped: starting the Internet connection again.\r\n");
				NetClose();              // the link dropped: start it afresh
				}
			}
		else if (gSsOpen && NifActive() == 0)
			{
			// nothing failed, but NIFMAN says the link is down (its idle
			// hang-up, before the timers were held; the ISP's): a lookup
			// now would make NetDial dial by itself, from PPP's leftovers -
			// start it again our way (StartPpp and all) instead.
			LinkLog("nifman: NetworkActive says the link is down before the connection");
			Say("  The Psion's Internet connection has gone: starting it again.\r\n");
			NetClose();
			}
		if (gShared->ppp_start[0] && !gSsOpen && StartPpp(aResult, aResultMax) != 0)
			{
			gLinkFailAt = NowMicro();
			return -1;
			}
		return NetConnect(aResult, aResultMax);
		}
	if (!gCommOpen)
		{
		// PsiWeb and PsiMail open the port only when they need it
		TInt r = pg_link_open();
		if (r != 0)
			{
			if (r == -10)
				SetMsg(aResult, aResultMax, "the serial port is in use by another program (PsiTerm? Remote link?)");
			else if (r == -13)
				SetMsg(aResult, aResultMax, "the serial port is held by the Remote link - switch it off on the System screen (Ctrl+L)");
			else
				SetMsgErr(aResult, aResultMax, "could not set up the serial port", r);   // (pg_link_open's step codes)
			return -1;
			}
		}
	// back to the plain port setup: with FailDCD still on from the last
	// call, and no carrier, the AT dialogue below would fail at once
	DcdDisarm();
	gDcdLost = 0;
	// quick wake-up so the modem is at a command prompt
	pg_serial_write("\r", 1);
	pg_msleep(300);
	gRxPos = gRxLen = 0;
	gComm->ResetBuffers();
	Say("  Checking the modem... ");
	if (ModemAnswersAt())
		Say("OK\r\n");
	else
		{
		Say("no answer - hanging up an old call first\r\n");
		// still online from an old connection (e.g. the Psion was switched
		// off mid-session): escape to command mode and hang up first
		pg_msleep(1100);
		pg_serial_write("+++", 3);
		pg_msleep(1100);
		pg_serial_write("ATH\r", 4);
		pg_msleep(500);
		gRxPos = gRxLen = 0;
		gComm->ResetBuffers();
		if (ModemAnswersAt())
			Say("  Modem OK\r\n");
		else
			Say("  The modem still does not answer AT - is it on, and at this baud rate?\r\n");
		}
	// Wi-Fi modems (WiRSa, WiFi232, RetroWiFiModem) can treat 0xFF bytes as
	// telnet commands, eating or doubling them, which corrupts SSH and TLS.
	// ATNET0 turns that off for the call; a modem without it answers ERROR,
	// which does no harm (the Atom firmware never does telnet).
	pg_msleep(100);
	gRxPos = gRxLen = 0;
	gComm->ResetBuffers();
	{
	int net0 = ModemCommand("ATNET0");
	LinkLog(net0 == 1 ? "modem: ATNET0 OK (telnet handling off)"
	                  : net0 == 0 ? "modem: ATNET0 not supported (ERROR)" : "modem: no answer to ATNET0");
	}
	pg_msleep(100);
	gRxPos = gRxLen = 0;
	gComm->ResetBuffers();

	int len = 0;
	const char* p = gShared->dial_prefix[0] ? gShared->dial_prefix : "ATDT";
	while (*p && len < 20) cmd[len++] = *p++;
	cmd[len++] = ' ';
	p = gShared->host;
	while (*p && len < 190) cmd[len++] = *p++;
	cmd[len++] = ':';
	// port number
	char num[8];
	int port = gShared->port > 0 ? gShared->port : 22;
	int nd = 0;
	do { num[nd++] = (char)('0' + port % 10); port /= 10; } while (port && nd < 7);
	while (nd) cmd[len++] = num[--nd];
	cmd[len] = 0;
	Say("  Sending ");
	Say(cmd);
	Say("\r\n");
	cmd[len++] = '\r';
	pg_serial_write(cmd, len);
	cmd[len - 1] = 0;                    // for spotting the modem's echo of it

	for (i = 0; i < 10; i++)
		{
		int n = ReadLine(line, sizeof(line), 30000);
		if (n < 0)
			{
			const char* m = "no answer from modem";
			if (pg_quit_requested())
				m = "Stopped";                // (ReadLine gives up on quit too)
			else
				Say("  No reply from the modem for 30 seconds\r\n");
			if (aResult) { int k = 0; while (m[k] && k < aResultMax - 1) { aResult[k] = m[k]; k++; } aResult[k] = 0; }
			return -1;
			}
		if (line[0] && !StartsWith(line, cmd) && !StartsWith(line, "AT"))
			{
			Say("  Modem: ");
			Say(line);
			Say("\r\n");
			}
		if (StartsWith(line, "CONNECT"))
			{
			// (0.68) the rest of the modem's line ending ("CONNECT\r\n":
			// ReadLine stopped at the '\r') must not reach the protocol as
			// data - IMAP would read an empty first line, TLS a bad record
			// type. Only CR/LF already here (or arriving within 50 ms) go:
			// SSH, IMAP and TLS servers never start with those.
			if (pg_net_avail() == 0)
				RxFill(50000);
			while (gRxPos < gRxLen && (gRx[gRxPos] == '\r' || gRx[gRxPos] == '\n'))
				gRxPos++;
			DcdArm();                    // carrier detect, if the modem gives us DCD
			gModemOnline = 1;
			return 0;
			}
		if (StartsWith(line, "NO CARRIER") || StartsWith(line, "ERROR") ||
			StartsWith(line, "BUSY") || StartsWith(line, "NO ANSWER") ||
			StartsWith(line, "NO DIALTONE"))
			{
			if (aResult) { int k = 0; while (line[k] && k < aResultMax - 1) { aResult[k] = line[k]; k++; } aResult[k] = 0; }
			return -1;
			}
		// anything else (our own echoed command, status text) is ignored
		}
	if (aResult) { const char* m = "modem did not connect"; int k = 0; while (m[k] && k < aResultMax - 1) { aResult[k] = m[k]; k++; } aResult[k] = 0; }
	return -1;
	}

extern "C" void pg_hangup()
	{
	if (gNet)
		{
		// the dial-up stays up until pg_close; a suspect link gets the
		// abortive shutdown (no reply to wait for)
		NetCloseSocket(gLinkSuspect || (gNetClosed && gNetClosedWhy != 0));
		return;
		}
	if (!gCommOpen)
		return;
	int lost = gDcdLost && gDcdFail;
	gModemOnline = 0;
	gLinkDoubt = 0;
	DcdDisarm();                         // the AT dialogue needs the plain setup
	if (lost)
		{
		// the carrier has already gone: the modem is at its prompt, and
		// the 2.5 s of +++ ATH would buy nothing
		LinkLog("hangup: carrier already lost (DCD), skipping +++ ATH");
		gDcdLost = 0;
		return;
		}
	User::After(1100000);
	pg_serial_write("+++", 3);
	User::After(1100000);
	pg_serial_write("ATH\r", 4);
	User::After(300000);
	}

// ----- entropy ----------------------------------------------------------------
// The Psion has no hardware random source, so collect timing noise:
// PsiTerm's keystroke timings, the system clock, and the jitter between the
// CPU and the tick timer. The result is hashed by Dropbear's own pool.
extern "C" int pg_entropy(unsigned char* aOut, int aMax)
	{
	int n = 0;
	#define PUT(v) do { TUint32 _v = (TUint32)(v); for (int _i = 0; _i < 4 && n < aMax; _i++) { aOut[n++] = (unsigned char)(_v & 0xff); _v >>= 8; } } while (0)
	if (gShared)
		{
		int e = gShared->entropy_len;
		if (e > PSI_ENTROPY_SIZE) e = PSI_ENTROPY_SIZE;
		for (int i = 0; i < e && n < aMax; i++)
			aOut[n++] = gShared->entropy[i];
		}
	TTime now;
	now.HomeTime();                      // (the clock: NowMicro is ticks since the start now)
	TInt64 t = now.Int64();
	PUT(t.Low());
	PUT(t.High());
	PUT(User::TickCount());
	RThread thread;
	TThreadId tid = thread.Id();
	TUint32 raw = 0;
	Mem::Copy(&raw, &tid, sizeof(raw));
	PUT(raw);
	TMemoryInfoV1Buf mem;
	UserHal::MemoryInfo(mem);
	PUT(mem().iFreeRamInBytes);
	// timer/CPU jitter: count spins between tick edges
	for (int s = 0; s < 48 && n < aMax - 4; s++)
		{
		TUint tick = User::TickCount();
		TUint32 spins = 0;
		while (User::TickCount() == tick && spins < 2000000)
			spins++;
		PUT(spins ^ (User::TickCount() << 8));
		}
	#undef PUT
	return n;
	}

extern "C" const char* pg_home()
	{
	return (gShared && gShared->home[0]) ? gShared->home : "C:\\System\\Apps\\PsiTerm";
	}

#endif // PG_LINK_TEST_ONLY

// ----- Connection settings > Test (0.74) ------------------------------------
// Shared by the three apps' Connection settings dialogs (pglinktest.h). Runs
// in the app's own thread, with the settings shown in the dialog: no
// writable statics (an .app is a DLL), no leaves, and every port, session
// and request is closed or completed before it returns.
#ifdef PG_LINK_TEST_ONLY
#include "pglinktest.h"

// --- the pure part: classify replies and word the result (host-tested) ---

static int LtLen(const char* aS)
	{
	int n = 0;
	while (aS[n]) n++;
	return n;
	}

static void LtCat(char* aDst, int aMax, const char* aSrc)
	{
	int n = LtLen(aDst);
	while (*aSrc && n < aMax - 1)
		aDst[n++] = *aSrc++;
	aDst[n] = 0;
	}

static void LtCatNum(char* aDst, int aMax, long aNum)
	{
	char t[16];
	int k = 0, neg = aNum < 0;
	unsigned long v = neg ? (unsigned long)(-aNum) : (unsigned long)aNum;
	do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 14);
	char o[18];
	int j = 0;
	if (neg) o[j++] = '-';
	while (k) o[j++] = t[--k];
	o[j] = 0;
	LtCat(aDst, aMax, o);
	}

static const char* LtBaudName(int aIndex)
	{
	switch (aIndex)
		{
	case 0: return "9600";
	case 1: return "19200";
	case 2: return "38400";
	case 3: return "57600";
	default: return "115200";
		}
	}

static int LtStarts(const char* aS, const char* aP)
	{
	while (*aP)
		{
		char a = *aS++, b = *aP++;
		if (a >= 'a' && a <= 'z') a = (char)(a - 32);
		if (a != b)
			return 0;
		}
	return 1;
	}

// Calls aFn for each non-empty line (CR or LF ends one); stops when it
// returns non-zero, and returns that.
typedef int (*TLtLineFn)(const char* aLine, void* aCtx);
static int LtLines(const unsigned char* aData, int aLen, TLtLineFn aFn, void* aCtx)
	{
	char line[80];
	int n = 0;
	for (int i = 0; i <= aLen; i++)
		{
		int c = i < aLen ? aData[i] : '\n';
		if (c == '\r' || c == '\n')
			{
			while (n > 0 && line[n - 1] == ' ') n--;
			line[n] = 0;
			int s = 0;
			while (line[s] == ' ') s++;
			if (line[s])
				{
				int r = aFn(line + s, aCtx);
				if (r)
					return r;
				}
			n = 0;
			continue;
			}
		if (n < (int)sizeof(line) - 1)
			line[n++] = (char)c;
		}
	return 0;
	}

static int LtIsText(int aC)
	{
	return (aC >= 0x20 && aC < 0x7f) || aC == '\r' || aC == '\n' || aC == '\t';
	}

struct TLtFirst { const char* iSkip; char* iOut; int iMax; };

static int LtFirstFn(const char* aLine, void* aCtx)
	{
	TLtFirst* f = (TLtFirst*)aCtx;
	if (f->iSkip && LtStarts(aLine, f->iSkip))
		return 0;                         // the modem's echo of our command
	if (LtStarts(aLine, "OK") && aLine[2] == 0)
		return 0;
	int k = 0;
	while (aLine[k] && k < f->iMax - 1)
		{
		f->iOut[k] = LtIsText((unsigned char)aLine[k]) ? aLine[k] : '?';
		k++;
		}
	f->iOut[k] = 0;
	return 1;
	}

// The first line of a reply that is not the echo of aSkip and not "OK"
extern "C" void pg_lt_first_line(const unsigned char* aData, int aLen, const char* aSkip, char* aOut, int aMax)
	{
	aOut[0] = 0;
	TLtFirst f;
	f.iSkip = aSkip;
	f.iOut = aOut;
	f.iMax = aMax;
	LtLines(aData, aLen, LtFirstFn, &f);
	}

static int LtAnswerFn(const char* aLine, void* /*aCtx*/)
	{
	if (LtStarts(aLine, "OK") && (aLine[2] == 0 || aLine[2] == ' '))
		return PG_AT_OK;
	if (LtStarts(aLine, "ERROR"))
		return PG_AT_ERROR;
	return 0;
	}

static int LtEchoFn(const char* aLine, void* /*aCtx*/)
	{
	return (LtStarts(aLine, "AT") && aLine[2] == 0) ? 0 : 1;   // 1 = not only echo
	}

// What came back after "AT\r"
extern "C" int pg_lt_classify(const unsigned char* aData, int aLen, char* aFirst, int aFirstMax)
	{
	if (aFirst && aFirstMax > 0)
		aFirst[0] = 0;
	if (aLen <= 0)
		return PG_AT_NOTHING;
	int r = LtLines(aData, aLen, LtAnswerFn, 0);
	if (r)
		return r;
	int bad = 0, text = 0;
	for (int i = 0; i < aLen; i++)
		{
		if (!LtIsText(aData[i]))
			bad++;
		else if (aData[i] > ' ')
			text++;
		}
	// a wrong speed gives mostly bytes that are not text (a stray one in a
	// readable reply is noise); nothing but CR/LF is not an answer either
	if (bad * 4 > aLen || (bad && text < 2))
		return PG_AT_GARBAGE;
	if (text == 0)
		return PG_AT_NOTHING;
	if (!LtLines(aData, aLen, LtEchoFn, 0))
		return PG_AT_ECHO;
	if (aFirst)
		pg_lt_first_line(aData, aLen, "AT", aFirst, aFirstMax);
	return PG_AT_TEXT;
	}

static char* LtNewLine(PgLinkTest* aT)
	{
	if (aT->nlines >= PG_LT_LINES)
		return 0;
	char* l = aT->line[aT->nlines++];
	l[0] = 0;
	return l;
	}

static void LtSay1(PgLinkTest* aT, const char* aA, const char* aB = 0, const char* aC = 0,
	const char* aD = 0, const char* aE = 0)
	{
	char* l = LtNewLine(aT);
	if (!l)
		return;
	LtCat(l, PG_LT_LINE, aA);
	if (aB) LtCat(l, PG_LT_LINE, aB);
	if (aC) LtCat(l, PG_LT_LINE, aC);
	if (aD) LtCat(l, PG_LT_LINE, aD);
	if (aE) LtCat(l, PG_LT_LINE, aE);
	}

static void LtReportPort(PgLinkTest* aT)
	{
	if (aT->port == PG_PORT_BUSY || aT->port == PG_PORT_REMOTE)
		{
		// (C32 says "access denied" both for the Remote link and for a port
		// another program holds - seen in the emulator with PsiWeb's engine
		// mid-dial - so one wording covers both)
		LtSay1(aT, "The serial port is in use by another program or connection");
		LtSay1(aT, "Disconnect it, or close that program, then test again");
		if (aT->port == PG_PORT_REMOTE)
			LtSay1(aT, "Or switch the Remote link off (Ctrl+L on the System screen)");
		}
	else
		{
		char* l = LtNewLine(aT);
		if (l)
			{
			LtCat(l, PG_LT_LINE, "Could not open the serial port (");
			LtCatNum(l, PG_LT_LINE, aT->port_err);
			LtCat(l, PG_LT_LINE, ")");
			}
		}
	}

static void LtReportModem(PgLinkTest* aT)
	{
	const char* baud = LtBaudName(aT->baud_index);
	int at = aT->cts_blocked ? aT->at_noflow : aT->at;
	switch (at)
		{
	case PG_AT_OK:
		if (aT->cts_blocked)
			LtSay1(aT, "The modem answered OK at ", baud, " baud with Flow control None");
		else
			LtSay1(aT, "The modem answered OK at ", baud, " baud");
		if (aT->escaped)
			LtSay1(aT, "It was still in a call - +++ and ATH ended it");
		break;
	case PG_AT_ERROR:
		LtSay1(aT, "A modem is there at ", baud, " baud, but it answered ERROR to AT");
		break;
	case PG_AT_ECHO:
		LtSay1(aT, "Only an echo came back - TX and RX may be joined together");
		break;
	case PG_AT_GARBAGE:
		LtSay1(aT, "Garbled reply at ", baud, " baud - wrong baud rate, or TX and RX swapped");
		break;
	case PG_AT_TEXT:
		LtSay1(aT, "The modem replied '", aT->reply, "' instead of OK");
		break;
	case PG_AT_NOTSENT:
		LtSay1(aT, "Could not send to the modem - CTS is low");
		break;
	default:
		if (aT->found_baud < 0)
			LtSay1(aT, "Nothing came back - check the cable, or try swapping TX and RX");
		else
			LtSay1(aT, "Nothing came back at ", baud, " baud");
		break;
		}
	if (at != PG_AT_OK && at != PG_AT_ERROR && aT->found_baud >= 0)
		{
		const char* f = LtBaudName(aT->found_baud);
		LtSay1(aT, "The modem answers at ", f, " baud - set Baud rate to ", f);
		}
	if (aT->signals)
		{
		const char* cts = aT->cts ? "high" : "low";
		const char* dcd = aT->dcd ? "on" : "off";
		if (aT->rtscts && !aT->cts)
			{
			LtSay1(aT, "CTS is low - set Flow control to None");
			LtSay1(aT, "The modem may not support flow control (DCD is ", dcd, ")");
			}
		else
			LtSay1(aT, "CTS is ", cts, ", DCD is ", dcd);
		// the modem drives CTS: hardware flow control can be used, and at
		// 115200 it is what stops long downloads overrunning the Psion
		if (!aT->rtscts && aT->cts && at == PG_AT_OK)
			LtSay1(aT, "The modem drives CTS - Flow control RTS/CTS is safer at speed");
		}
	if (aT->modem[0])
		LtSay1(aT, "Modem: ", aT->modem);
	}

static void LtReportNet(PgLinkTest* aT)
	{
	if (aT->need_dial)
		{
		LtSay1(aT, "The Psion's Internet connection is not up");
		LtSay1(aT, "It starts when the program next connects");
		return;
		}
	if (aT->dial)
		{
		const char* cmd = aT->ppp_start;
		switch (aT->ppp)
			{
		case 1: LtSay1(aT, "Sent ", cmd, " - the modem answered CONNECT"); break;
		case 2: LtSay1(aT, "Sent ", cmd, " - no CONNECT from the modem"); break;
		case 3: LtSay1(aT, "Did not send ", cmd, " - the serial port is in use"); break;
		case 4: LtSay1(aT, "Did not send ", cmd, " - the modem does not answer AT"); break;
		case 5:
			{
			char* l = LtNewLine(aT);
			if (l)
				{
				LtCat(l, PG_LT_LINE, "Did not send ");
				LtCat(l, PG_LT_LINE, cmd);
				LtCat(l, PG_LT_LINE, " - could not set the serial port up (");
				LtCatNum(l, PG_LT_LINE, aT->port_err);
				LtCat(l, PG_LT_LINE, ")");
				}
			break;
			}
		default: break;
			}
		if (aT->net_after == 1)
			LtSay1(aT, "The Psion's Internet connection is up now");
		else if (aT->net_after == 0)
			LtSay1(aT, "The Psion's Internet connection did not start");
		}
	else if (aT->net_up == 1)
		LtSay1(aT, "The Psion's Internet connection is up");
	else if (aT->net_up < 0)
		LtSay1(aT, "The Psion would not say whether its Internet connection is up");
	if (aT->dns == 0)
		{
		char a[20];
		a[0] = 0;
		for (int i = 3; i >= 0; i--)
			{
			LtCatNum(a, sizeof(a), (long)((aT->addr >> (i * 8)) & 0xff));
			if (i) LtCat(a, sizeof(a), ".");
			}
		LtSay1(aT, "Looked up ", aT->host, ": ", a, " - DNS works");
		}
	else if (aT->dns == -14)                 // KErrInUse: lookup_leaked
		LtSay1(aT, "The last test's name lookup has not ended yet - test again in a minute");
	else if (aT->dns != 1)
		{
		char* l = LtNewLine(aT);
		if (l)
			{
			LtCat(l, PG_LT_LINE, "Could not look up ");
			LtCat(l, PG_LT_LINE, aT->host);
			LtCat(l, PG_LT_LINE, " (");
			LtCatNum(l, PG_LT_LINE, aT->dns);
			LtCat(l, PG_LT_LINE, ")");
			}
		if (aT->dns == -33)
			LtSay1(aT, "No answer in time - the connection may be slow or down");
		else if (aT->dial && aT->net_after != 1)
			LtSay1(aT, "The connection did not come up - the Internet settings may be wrong");
		else
			LtSay1(aT, "The name servers (DNS) in the Internet settings may be wrong");
		}
	}

extern "C" void pg_lt_report(PgLinkTest* aT)
	{
	aT->nlines = 0;
	if (aT->net_mode)
		LtReportNet(aT);
	else if (aT->port != PG_PORT_OK)
		LtReportPort(aT);
	else
		LtReportModem(aT);
	}

// --- the EPOC part ---
#ifndef PG_LINK_TEST_HOST

// The progress text goes into the test's own record (busy, busy_seq): the
// app's thread shows it as the busy message, since this runs in a thread of
// its own (pglinktest.cpp) that may not touch the screen. A callback, if
// set, is called too.
static void LtProgress(PgLinkTest* aT, const char* aText)
	{
	int k = 0;
	while (aText[k] && k < (int)sizeof(aT->busy) - 1) { aT->busy[k] = aText[k]; k++; }
	aT->busy[k] = 0;
	aT->busy_seq++;
	if (aT->progress)
		aT->progress(aT->ctx, aText);
	}

static TBps LtBps(int aIndex)
	{
	switch (aIndex)
		{
	case 0: return EBps9600;
	case 1: return EBps19200;
	case 2: return EBps38400;
	case 3: return EBps57600;
	default: return EBps115200;
		}
	}

static TInt LtSetConfig(RComm& aComm, int aBaud, int aRtsCts)
	{
	TCommConfig cfg;
	aComm.Config(cfg);
	cfg().iRate = LtBps(aBaud);
	cfg().iDataBits = EData8;
	cfg().iStopBits = EStop1;
	cfg().iParity = EParityNone;
	cfg().iFifo = EFifoEnable;
	cfg().iTerminatorCount = 0;
	cfg().iHandshake = aRtsCts ? KConfigObeyCTS : 0;   // as the engines (OpenSerial)
	aComm.Cancel();                          // never SetConfig with I/O pending
	return aComm.SetConfig(cfg);
	}

// Has a line that ends the modem's answer arrived?
static int LtDoneFn(const char* aLine, void* /*aCtx*/)
	{
	return LtStarts(aLine, "OK") || LtStarts(aLine, "ERROR") || LtStarts(aLine, "CONNECT")
		|| LtStarts(aLine, "NO CARRIER") || LtStarts(aLine, "BUSY") || LtStarts(aLine, "NO ANSWER")
		|| LtStarts(aLine, "NO DIAL");
	}

// Sends aCmd and collects the reply in aBuf until a final result line, or
// aWaitMs, or the app asks the test to stop (aT->abort_test). KErrNone,
// KErrTimedOut if the write stalled (CTS low), or the write's error. Line
// errors (framing, overrun: a wrong speed) are kept in the reply as a 0xFF
// byte so they count as garbage.
static TInt LtCommand(RComm& aComm, const TDesC8& aCmd, TDes8& aBuf, TInt aWaitMs, PgLinkTest* aT)
	{
	aBuf.Zero();
	aComm.ResetBuffers();
	TRequestStatus s;
	aComm.Write(s, TTimeIntervalMicroSeconds32(2000000), aCmd);
	User::WaitForRequest(s);
	if (s.Int() != KErrNone)
		{
		aComm.ResetBuffers();
		return s.Int();
		}
	// One timer for the whole wait: a read's own timeout cannot be trusted
	// to end the loop (in the emulator a timed read can run far over)
	RTimer timer;
	if (timer.CreateLocal() != KErrNone)
		return KErrNoMemory;
	TRequestStatus deadline;
	timer.After(deadline, TTimeIntervalMicroSeconds32(aWaitMs * 1000));
	// (each request's signal is taken exactly once: a stray one would panic
	// the app's active scheduler later)
	TInt deadlineDone = 0;
	TBuf8<64> chunk;
	for (;;)
		{
		chunk.Zero();
		aComm.Read(s, TTimeIntervalMicroSeconds32(150000), chunk);
		User::WaitForRequest(s, deadline);
		if (s == KRequestPending)
			{
			deadlineDone = 1;                // its signal was the one taken
			aComm.ReadCancel();
			User::WaitForRequest(s);
			}
		else if (deadline != KRequestPending)
			{
			deadlineDone = 1;                // both done: take the second signal
			User::WaitForRequest(deadline);
			}
		TInt room = aBuf.MaxLength() - aBuf.Length();
		aBuf.Append(chunk.Left(chunk.Length() < room ? chunk.Length() : room));
		if (s.Int() != KErrNone && s.Int() != KErrTimedOut && s.Int() != KErrCancel
			&& aBuf.Length() < aBuf.MaxLength())
			aBuf.Append(0xff);
		if (deadlineDone)
			break;
		if (aT && aT->abort_test)
			break;                           // Esc: the reply so far is what there is
		if (LtLines(aBuf.Ptr(), aBuf.Length(), LtDoneFn, 0))
			break;
		}
	if (!deadlineDone)
		{
		timer.Cancel();
		User::WaitForRequest(deadline);
		}
	timer.Close();
	return KErrNone;
	}

static int LtAt(RComm& aComm, TDes8& aBuf, PgLinkTest* aT, TInt aWaitMs)
	{
	TInt r = LtCommand(aComm, _L8("AT\r"), aBuf, aWaitMs, aT);
	if (r == KErrTimedOut)
		return PG_AT_NOTSENT;
	if (r != KErrNone)
		return PG_AT_NOTHING;
	return pg_lt_classify(aBuf.Ptr(), aBuf.Length(), aT->reply, sizeof(aT->reply));
	}

// Leave a call the modem may still be in: guard time, +++, guard time, ATH
static void LtEscape(RComm& aComm)
	{
	TRequestStatus s;
	User::After(1100000);
	aComm.Write(s, TTimeIntervalMicroSeconds32(2000000), _L8("+++"));
	User::WaitForRequest(s);
	User::After(1100000);
	aComm.Write(s, TTimeIntervalMicroSeconds32(2000000), _L8("ATH\r"));
	User::WaitForRequest(s);
	User::After(500000);
	aComm.ResetBuffers();
	}

// Opens COMM::0 exclusively, as the engines do. PG_PORT_*.
static int LtOpen(RCommServ& aServer, RComm& aComm, int& aServerOpen, PgLinkTest* aT)
	{
	TInt r = User::LoadPhysicalDevice(KPddName);
	if (r == KErrNone || r == KErrAlreadyExists)
		r = User::LoadLogicalDevice(KLddName);
	if (r == KErrNone || r == KErrAlreadyExists)
		r = StartC32();
	if (r == KErrNone || r == KErrAlreadyExists)
		{
		r = aServer.Connect();
		if (r == KErrNone)
			{
			aServerOpen = 1;
			r = aServer.LoadCommModule(KCsyName);
			}
		}
	if (r == KErrNone || r == KErrAlreadyExists)
		r = aComm.Open(aServer, KPortName, ECommExclusive);
	if (r == KErrInUse)
		return PG_PORT_BUSY;
	if (r == KErrAccessDenied)
		return PG_PORT_REMOTE;
	if (r != KErrNone)
		{
		aT->port_err = r;
		return PG_PORT_FAIL;
		}
	return PG_PORT_OK;
	}

// Powers the UART (a zero-length read: DTR is not really raised until the
// first read or write) and lets the lines settle
static void LtWake(RComm& aComm)
	{
	aComm.SetReceiveBufferLength(4096);
	aComm.SetSignals(KSignalDTR | KSignalRTS, 0);
	TRequestStatus s;
	TBuf8<4> none;
	aComm.Read(s, none, 0);
	User::WaitForRequest(s);
	User::After(150000);
	}

static void LtModem(RComm& aComm, PgLinkTest* aT)
	{
	TBuf8<256> buf;
	LtWake(aComm);
	TUint sig = aComm.Signals();
	aT->signals = 1;
	aT->cts = (sig & KSignalCTS) ? 1 : 0;
	aT->dcd = (sig & KSignalDCD) ? 1 : 0;

	// a bare CR first ends any half-typed command line, then AT
	TRequestStatus s;
	aComm.Write(s, TTimeIntervalMicroSeconds32(1000000), _L8("\r"));
	User::WaitForRequest(s);
	User::After(200000);
	int at = LtAt(aComm, buf, aT, 1500);
	if (aT->cts_blocked)
		aT->at_noflow = at;
	else
		aT->at = at;
	if (at == PG_AT_NOTSENT && !aT->cts_blocked)
		{
		// the write stalled although CTS read high: try without flow control
		// (if the port will not take the new setting, the stalled write is
		// the result: "CTS is low")
		if (LtSetConfig(aComm, aT->baud_index, 0) == KErrNone)
			{
			aT->cts_blocked = 1;
			aT->at_noflow = at = LtAt(aComm, buf, aT, 1500);
			}
		}
	if (aT->abort_test)
		return;
	if (at == PG_AT_ECHO || at == PG_AT_NOTHING || at == PG_AT_TEXT)
		{
		// still in a call from an old connection? (as pg_dial does)
		LtProgress(aT, "Testing the modem - ending an old call...");
		LtEscape(aComm);
		int again = LtAt(aComm, buf, aT, 1500);
		if (again == PG_AT_OK || again == PG_AT_ERROR)
			{
			aT->escaped = 1;
			at = again;
			if (aT->cts_blocked) aT->at_noflow = at; else aT->at = at;
			}
		}
	if (at == PG_AT_NOTHING || at == PG_AT_GARBAGE || at == PG_AT_TEXT || at == PG_AT_ECHO)
		{
		// the other speeds, flow control off: does the modem answer at one?
		LtProgress(aT, "Testing the modem at other baud rates...");
		for (int b = 4; b >= 0 && aT->found_baud < 0 && !aT->abort_test; b--)
			{
			if (b == aT->baud_index)
				continue;
			if (LtSetConfig(aComm, b, 0) != KErrNone)
				continue;
			User::After(50000);
			TBuf8<256> other;
			char dummy[8];
			TInt r = LtCommand(aComm, _L8("\rAT\r"), other, 700, aT);
			if (r == KErrNone && pg_lt_classify(other.Ptr(), other.Length(), dummy, sizeof(dummy)) == PG_AT_OK)
				aT->found_baud = b;
			}
		// (back to the chosen rate for the rest; nothing below depends on the
		// rate if this fails: ATI is only asked when the modem answered OK at
		// it, and the signals are read regardless)
		LtSetConfig(aComm, aT->baud_index, aT->cts_blocked ? 0 : aT->rtscts);
		}
	if (at == PG_AT_OK && !aT->abort_test)
		{
		// its name, for the result
		if (LtCommand(aComm, _L8("ATI\r"), buf, 1500, aT) == KErrNone)
			pg_lt_first_line(buf.Ptr(), buf.Length(), "ATI", aT->modem, sizeof(aT->modem));
		}
	// signals again: some modems raise CTS only once they have been spoken to
	sig = aComm.Signals();
	aT->cts = (sig & KSignalCTS) ? 1 : 0;
	aT->dcd = (sig & KSignalDCD) ? 1 : 0;
	}

// Psion Internet, with consent: the user's "first send" command to the modem
// (e.g. ATDT777 puts a WiRSa into PPP), as the engines' StartPpp does
static void LtPppStart(PgLinkTest* aT)
	{
	if (!aT->ppp_start[0])
		return;
	RCommServ server;
	RComm comm;
	int serverOpen = 0;
	int p = LtOpen(server, comm, serverOpen, aT);
	if (p == PG_PORT_OK)
		{
		TInt r = LtSetConfig(comm, aT->baud_index, aT->rtscts);
		if (r != KErrNone)
			{
			aT->ppp = 5;                     // (not sent: the port would be at the wrong rate)
			aT->port_err = r;
			}
		else
			{
			LtWake(comm);
			TRequestStatus s;
			comm.Write(s, TTimeIntervalMicroSeconds32(1000000), _L8("\r"));
			User::WaitForRequest(s);
			User::After(200000);
			TBuf8<256> buf;
			int at = LtAt(comm, buf, aT, 1500);
			if (at != PG_AT_OK && !aT->abort_test)
				{
				LtEscape(comm);
				at = LtAt(comm, buf, aT, 1500);
				}
			if (at != PG_AT_OK)
				aT->ppp = 4;
			else if (!aT->abort_test)
				{
				TBuf8<48> cmd;
				cmd.Copy(TPtrC8((const TUint8*)aT->ppp_start));
				cmd.Append('\r');
				aT->ppp = 2;
				if (LtCommand(comm, cmd, buf, 30000, aT) == KErrNone)
					{
					char first[24];
					pg_lt_first_line(buf.Ptr(), buf.Length(), "AT", first, sizeof(first));
					if (LtStarts(first, "CONNECT"))
						aT->ppp = 1;
					}
				}
			}
		comm.Cancel();
		comm.Close();                        // hand the port to the Psion's TCP/IP
		}
	else
		aT->ppp = 3;
	if (serverOpen)
		server.Close();
	}

// 1 up, 0 down, -1 NIFMAN would not say. Never dials.
static int LtNifActive()
	{
	RNif nif;
	if (nif.Open() != KErrNone)
		return -1;
	TBool active = EFalse;
	TInt r = nif.NetworkActive(active);
	nif.Close();
	if (r != KErrNone)
		return -1;
	return active ? 1 : 0;
	}

// The name lookup's requests live in a block the app's thread made in its
// own heap (pg_lt_lookup_new), not on the stack of the thread running the
// test. A lookup that NetDial completes neither on Cancel nor on closing its
// sessions (it is busy with its own dialogs) is abandoned after a bounded
// wait, and must then complete into memory that still exists: the block is
// left alone for good (lookup_leaked), and the test's thread can end.
struct TLtLookup
	{
	RSocketServ iSs;
	RHostResolver iRes;
	TNameEntry iEntry;
	TRequestStatus iLook;
	};

extern "C" void* pg_lt_lookup_new()
	{
	return new TLtLookup;                    // (0 when there is no memory)
	}

extern "C" void pg_lt_lookup_free(void* aLookup)
	{
	delete (TLtLookup*)aLookup;
	}

// Waits for aStat in slices of a second, so a stop (aT->abort_test, from
// Esc) is seen soon. 1 completed, 0 timed out, -1 stopped. A status that has
// completed is never waited on again: the two-status wait took its count
// with it (see WaitFor in the engine part). A timer count left over, when
// both completed together, does no harm in a thread with no active
// scheduler.
static int LtWaitSlices(RTimer& aTimer, TRequestStatus& aStat, TInt aTimeoutUs, PgLinkTest* aT)
	{
	TInt waited = 0;
	while (aStat == KRequestPending)
		{
		if (waited >= aTimeoutUs)
			return 0;
		if (aT && aT->abort_test)
			return -1;
		TInt slice = aTimeoutUs - waited < 1000000 ? aTimeoutUs - waited : 1000000;
		TRequestStatus tick;
		aTimer.After(tick, slice);
		User::WaitForRequest(aStat, tick);
		if (tick == KRequestPending)
			{
			aTimer.Cancel();
			User::WaitForRequest(tick);      // the cancel completes it
			}
		waited += slice;
		}
	return 1;
	}

// Looks aT->host up (this is what starts the dial-up when the link is
// down), for at most aTimeoutUs. A lookup that does not finish is cancelled,
// then its sessions are closed, each with a bounded wait; one that still
// does not finish is abandoned (above).
static void LtLookup(PgLinkTest* aT, TInt aTimeoutUs)
	{
	TLtLookup* k = (TLtLookup*)aT->lookup;
	if (!k)
		{
		aT->dns = KErrNoMemory;
		return;
		}
	if (aT->lookup_leaked)
		{
		aT->dns = KErrInUse;                 // the last test's lookup is still out
		return;
		}
	TInt r = k->iSs.Connect();
	if (r != KErrNone)
		{
		aT->dns = r;
		return;
		}
	r = k->iRes.Open(k->iSs, KAfInet, KProtocolInetUdp);
	if (r != KErrNone)
		{
		k->iSs.Close();
		aT->dns = r;
		return;
		}
	RTimer timer;
	if (timer.CreateLocal() != KErrNone)
		{
		k->iRes.Close();
		k->iSs.Close();
		aT->dns = KErrNoMemory;
		return;
		}
	TBuf<64> name;
	name.Copy(TPtrC8((const TUint8*)aT->host));
	k->iLook = KRequestPending;
	k->iRes.GetByName(name, k->iEntry, k->iLook);
	int w = LtWaitSlices(timer, k->iLook, aTimeoutUs, aT);
	if (w == 1)
		{
		aT->dns = k->iLook.Int();
		if (aT->dns == KErrNone)
			aT->addr = TInetAddr(k->iEntry().iAddr).Address();
		k->iRes.Close();
		k->iSs.Close();
		}
	else
		{
		aT->dns = w < 0 ? KErrCancel : KErrTimedOut;
		k->iRes.Cancel();
		if (LtWaitSlices(timer, k->iLook, 10000000, 0) == 1)
			{
			k->iRes.Close();
			k->iSs.Close();
			}
		else
			{
			// not even a cancel ends it (NetDial busy with its own dialogs):
			// closing its sessions usually does...
			k->iRes.Close();
			k->iSs.Close();
			if (LtWaitSlices(timer, k->iLook, 10000000, 0) != 1)
				aT->lookup_leaked = 1;       // ...and if not, it is left out: its memory stays
			}
		}
	timer.Close();
	}

extern "C" int pg_link_test(PgLinkTest* aT)
	{
	aT->port = PG_PORT_OK;
	aT->port_err = 0;
	aT->at = aT->at_noflow = PG_AT_NOTHING;
	aT->escaped = aT->cts_blocked = 0;
	aT->found_baud = -1;
	aT->signals = aT->cts = aT->dcd = 0;
	aT->reply[0] = aT->modem[0] = 0;
	aT->net_up = -1;
	aT->need_dial = 0;
	aT->ppp = 0;
	aT->dns = 1;
	aT->addr = 0;
	aT->net_after = -1;
	if (!aT->host[0])
		{
		const char* h = "raw.githubusercontent.com";
		int i = 0;
		while (h[i] && i < (int)sizeof(aT->host) - 1) { aT->host[i] = h[i]; i++; }
		aT->host[i] = 0;
		}
	if (aT->net_mode)
		{
		LtProgress(aT, "Checking the Internet connection...");
		aT->net_up = LtNifActive();
		if (aT->net_up == 1)
			{
			LtProgress(aT, "Looking up a name...");
			LtLookup(aT, 20000000);
			}
		else if (!aT->dial)
			aT->need_dial = 1;
		else
			{
			LtProgress(aT, "Connecting...");
			LtPppStart(aT);
			// (no OK to AT: the modem may be in PPP already - go on, as
			// StartPpp does; a refused command, or a port that could not be
			// set up, is a definite failure)
			if (aT->ppp != 2 && aT->ppp != 5 && !aT->abort_test)
				LtLookup(aT, 90000000);          // starts the Psion's dial-up
			aT->net_after = LtNifActive();
			}
		}
	else
		{
		LtProgress(aT, "Testing the modem...");
		RCommServ server;
		RComm comm;
		int serverOpen = 0;
		aT->port = LtOpen(server, comm, serverOpen, aT);
		if (aT->port == PG_PORT_OK)
			{
			int commOpen = 1;
			TInt r = LtSetConfig(comm, aT->baud_index, aT->rtscts);
			if (r == KErrNone && aT->rtscts)
				{
				LtWake(comm);
				if (!(comm.Signals() & KSignalCTS))
					{
					// RTS/CTS on, but the modem holds CTS low: every write
					// would stall, and changing the handshake on the open
					// port did not free it (emulator). Open it again without.
					aT->cts_blocked = 1;
					comm.Close();
					r = comm.Open(server, KPortName, ECommExclusive);
					if (r == KErrNone)
						r = LtSetConfig(comm, aT->baud_index, 0);
					else
						{
						commOpen = 0;
						aT->port = PG_PORT_BUSY;   // (taken in between)
						}
					}
				}
			if (aT->port != PG_PORT_OK)
				;
			else if (r != KErrNone)
				{
				aT->port = PG_PORT_FAIL;
				aT->port_err = r;
				}
			else
				LtModem(comm, aT);
			if (commOpen)
				{
				comm.Cancel();
				comm.Close();
				}
			}
		if (serverOpen)
			server.Close();
		}
	pg_lt_report(aT);
	return 0;
	}

#endif // PG_LINK_TEST_HOST
#endif // PG_LINK_TEST_ONLY
