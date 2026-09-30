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

extern "C" {
#include "psishared.h"
}

_LIT(KPddName, "EUART1");
_LIT(KLddName, "ECOMM");
_LIT(KCsyName, "ECUART");
_LIT(KPortName, "COMM::0");

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

static void SetMsgErr(char* aOut, int aMax, const char* aText, TInt aErr)
	{
	if (!aOut || aMax < 16)
		return;
	TPtr8 p((TUint8*)aOut, 0, aMax - 1);
	p.Copy(TPtrC8((const TUint8*)aText));
	p.AppendFormat(_L8(" (error %d)"), aErr);
	p.ZeroTerminate();
	}

static TInt64 NowMicro();
extern "C" int pg_quit_requested();

// Waits for aStat for at most aTimeoutUs (aTimeoutUs < 0: no limit), giving
// up early if PsiTerm asks us to quit and aQuitAware is set.
// Returns 1 if aStat completed, 0 if not (aStat is then still outstanding).
static int WaitFor(TRequestStatus& aStat, TInt aTimeoutUs, int aQuitAware)
	{
	if (aStat != KRequestPending)
		{
		User::WaitForRequest(aStat);     // completed already: take its signal
		return 1;
		}
	if (!gTimerOpen)
		{
		if (!gTimer) gTimer = new RTimer;
		if (gTimer && gTimer->CreateLocal() == KErrNone)
			gTimerOpen = 1;
		}
	if (!gTimerOpen)
		{
		User::WaitForRequest(aStat);     // no timer: nothing better to do
		return 1;
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
			}
		TRequestStatus timerStat;
		gTimer->After(timerStat, slice);
		for (;;)
			{
			User::WaitForRequest(aStat, timerStat);
			if (aStat != KRequestPending)
				{
				gTimer->Cancel();
				User::WaitForRequest(timerStat);
				return 1;
				}
			if (timerStat != KRequestPending)
				break;                   // the slice is over
			// neither: a stray signal - an abandoned request completing (see
			// gLookupOrphan). Its signal has now been taken, so it needs no
			// WaitForRequest of its own. Keep waiting on the same timer: a
			// second After() on a running RTimer would panic.
			if (gLookupOrphan && gLookupStat != KRequestPending)
				gLookupOrphan = 0;
			else if (gConnOrphan && gConnStat != KRequestPending)
				gConnOrphan = 0;
			}
		if (aQuitAware && pg_quit_requested())
			return 0;
		}
	}

// After Cancel(): waits a bounded time for the request to complete and takes
// its signal. Returns 1 if it did, 0 if it is still outstanding (abandoned).
static int TakeCancelled(TRequestStatus& aStat)
	{
	return WaitFor(aStat, KCancelWaitUs, 0);
	}

// Abandoned requests that have since completed: take their signals so the
// thread's request semaphore stays in step with the statuses we wait on.
static void ReapOrphans(TInt aWaitUs)
	{
	if (gLookupOrphan && WaitFor(gLookupStat, aWaitUs, 0))
		gLookupOrphan = 0;
	if (gConnOrphan && WaitFor(gConnStat, aWaitUs, 0))
		gConnOrphan = 0;
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
static void NetCloseSocket()
	{
	if (gSockOpen)
		{
		if (gRecvPending)
			{
			gSock->CancelRecv();
			User::WaitForRequest(gRecvStat);
			gRecvPending = 0;
			}
		gSock->Close();
		gSockOpen = 0;
		}
	}

// Closes everything ESOCK: the socket, a resolver still open and the session
// itself. With no client left NetDial stops (or, after a failed start, does
// not try again by itself), and the next pg_dial starts from scratch: StartPpp
// again, a fresh dial-up. Closing the session also completes any request a
// Cancel() could not, so abandoned requests are collected here.
static void NetClose()
	{
	NetCloseSocket();
	if (gResolverOpen)
		{
		gResolver->Close();
		gResolverOpen = 0;
		}
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
static int WaitLink(TRequestStatus& aStat, TInt aTimeoutUs, const char* aWhat)
	{
	TInt waited = 0;
	for (;;)
		{
		TInt slice = aTimeoutUs - waited;
		if (slice > KStillWaitingUs)
			slice = KStillWaitingUs;
		if (slice <= 0)
			return 0;
		if (WaitFor(aStat, slice, 1))
			return 1;
		if (pg_quit_requested())
			return -1;
		waited += slice;
		char m[120];
		TPtr8 p((TUint8*)m, 0, sizeof(m) - 1);
		p.Format(_L8("  Still %s (%d s)...\r\n"), aWhat, waited / 1000000);
		p.ZeroTerminate();
		Say(m);
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

	TBuf<128> host;
	host.Copy(TPtrC8((const TUint8*)gShared->host));
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
				char m[160];
				TPtr8 p((TUint8*)m, 0, sizeof(m) - 1);
				p.Format(_L8("Timed out looking up %s: the Psion's Internet connection did not come up in %d s"),
					gShared->host, KLookupTimeoutUs / 1000000);
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
			p.Format(_L8("Could not connect: looking up %s failed"), gShared->host);
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
	Say("  TCP connection open.\r\n");
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
	// Take the receive's completion signal exactly once. WaitFor returning 1
	// has already taken it; User::WaitForRequest on a request whose signal is
	// gone blocks until some other request completes - with nothing else
	// outstanding that is forever (the hang after "Securing the connection").
	if (gRecvStat == KRequestPending)
		{
		if (!WaitFor(gRecvStat, aTimeoutUs, 0))
			return;                      // still waiting (signal not taken)
		}
	else
		User::WaitForRequest(gRecvStat); // completed earlier: take its signal
	gRecvPending = 0;
	gRxPos = 0;
	if (gRecvStat.Int() == KErrNone)
		{
		gRxLen = gRecvDes->Length();
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

static TInt64 NowMicro()
	{
	TTime t;
	t.HomeTime();
	return t.Int64();
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
	r = gComm->Open(*gServer, KPortName, ECommExclusive);
	if (r != KErrNone)
		return r == KErrInUse ? -10 : -12;
	gCommOpen = 1;

	TCommConfig cfg;
	gComm->Config(cfg);
	cfg().iRate = BaudFromIndex(gShared->baud_index);
	cfg().iDataBits = EData8;
	cfg().iStopBits = EStop1;
	cfg().iParity = EParityNone;
	cfg().iFifo = EFifoEnable;
	cfg().iTerminatorCount = 0;
	cfg().iHandshake = gShared->rtscts ? (KConfigObeyCTS | KConfigFreeRTS) : 0;
	r = gComm->SetConfig(cfg);
	if (r != KErrNone)
		{
		gComm->Close();
		gCommOpen = 0;
		return -11;
		}
	// Bigger than Dropbear's 8 KB SSH receive window plus packet overhead,
	// so the server can never send more than fits here: no overruns even
	// at 115200 without working hardware flow control.
	gComm->SetReceiveBufferLength(16384);
	gComm->SetSignals(KSignalDTR | KSignalRTS, 0);
	gRxErrors = 0;
	gRxLastErr = 0;
	return 0;
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

extern "C" int pg_quit_requested()
	{
	return gShared ? gShared->quit : 1;
	}

extern "C" void pg_msleep(int aMs)
	{
	User::After(aMs * 1000);
	}

// ----- serial ("network") ---------------------------------------------------

extern "C" int pg_serial_write(const void* aBuf, int aLen)
	{
	if (gNet)
		return aLen > 0 ? NetWrite(aBuf, aLen) : -1;
	if (!gCommOpen || aLen <= 0)
		return -1;
	TPtrC8 data((const TUint8*)aBuf, aLen);
	TRequestStatus stat;
	gComm->Write(stat, TTimeIntervalMicroSeconds32(10000000), data);
	User::WaitForRequest(stat);
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
	gRxPos = 0;
	gRxLen = 0;
	TPtr8 p(gRx, 0, sizeof(gRx));
	TRequestStatus stat;
	gComm->Read(stat, TTimeIntervalMicroSeconds32(aTimeoutUs), p);
	User::WaitForRequest(stat);
	if (stat.Int() == KErrNone || stat.Int() == KErrTimedOut)
		gRxLen = p.Length();
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
			if (gShared->quit)
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
	gNetClosed = 0;                      // a fresh connection (update does two)
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
		NetCloseSocket();
		if (gLinkSuspect)
			{
			// a receive or send failed: was it the TCP connection (a server
			// closing, a reset on the way) or the PPP link itself? Ask the
			// stack before throwing the dial-up away: bringing it back costs
			// half a minute and the Psion's connection dialogs.
			int up = LinkUp();
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
			SetMsg(aResult, aResultMax, r == -10
				? "the serial port is in use by another program (PsiTerm? Remote link?)"
				: "could not set up the serial port");
			return -1;
			}
		}
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
			return 0;
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
		NetCloseSocket();                // the dial-up stays up until pg_close
		return;
		}
	if (!gCommOpen)
		return;
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
	TInt64 t = NowMicro();
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
		PUT(spins ^ (NowMicro().Low() << 8));
		}
	#undef PUT
	return n;
	}

extern "C" const char* pg_home()
	{
	return (gShared && gShared->home[0]) ? gShared->home : "C:\\System\\Apps\\PsiTerm";
	}
