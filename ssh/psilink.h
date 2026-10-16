// psilink.h - the connection settings PsiTerm, PsiMail and PsiWeb share.
//
// How the Psion reaches the network is the same for every app, so it is set
// once: change it in any app's Connection settings and the others use it
// too. Stored in C:\System\Data\PsiLink.ini (header-only, so each app's
// build just includes it).
//
//   iBaudIndex  0=9600 1=19200 2=38400 3=57600 4=115200
//   iRtsCts     hardware flow control
//   iNetMode    0 = modem ("ATDT host:port"), 1 = the Psion's own TCP/IP
//   iPppStart   Psion TCP/IP only: sent to the modem first (e.g. ATDT777,
//               which puts a WiRSa into PPP); empty = send nothing
//   iUseProxy   (file version 2) 1 = use the HTTP proxy below: PsiWeb pages,
//               PsiMail web pictures, and the "Send log to server" commands
//   iProxyHost  proxy host name or address (max 60)
//   iProxyPort  proxy port
//   iModemTls   (file version 3) modem route only: 1 = the Atom modem
//               terminates TLS (AT$TLS), so PsiMail dials the TLS port in
//               plain and the 36 MHz ARM does no crypto. Ignored on the
//               Psion Internet (PPP) route.
//   iKeepUp     (file version 4) Psion Internet only: 1 = once one of our
//               programs has brought the connection up it stays up after
//               use, until the person disconnects (or the Psion, the modem
//               or the service ends it); 0 = the Psion's own idle time ends
//               it, as before. Standard for a new Psion (no file yet): 1.
//               A file from before version 4 loads as 0, so someone who
//               upgrades keeps the idle hang-up they had until they choose
//               otherwise. The modem route always hangs up after use (it
//               holds the port).
//   iLoadedVer  set by Load: the file version that was read (1 = no proxy
//               in it yet: the apps seed it once from their old own setting)
//
// File: 'P' 'L' ver baud rtscts netmode len pppstart[len]
//       v2 then: useproxy len host[len] port-lo port-hi
//       v3 then: modemtls
//       v4 then: keepup
// Each version only appends, so an older program reads a newer file (and
// writes it back without the newer fields, which then load as standard).
//
// PsiDevBox.ini (optional, C:\System\Data): where PsiProxy's own services
// (Send log, a dev tool's builds) are, when the proxy in use is one that has
// none of them. "<host>\r\n<port>\r\n". Nothing writes it but an optional dev
// tool's Dev box... dialog, and nothing is sent anywhere unless the person asks
// (Send log): see DevBox below and pgdevbox.h, the same rule in C.
#ifndef PSILINK_H
#define PSILINK_H

#ifndef PSILINK_STRUCT_ONLY
#include <e32base.h>     // CTrapCleanup, for PsiLinkDisconnect's thread
#include <f32file.h>
#include <nifman.h>      // RNif: PsiLinkTimersBack, PsiLinkActive, PsiLinkDisconnect
#endif

_LIT(KPsiLinkFile, "C:\\System\\Data\\PsiLink.ini");
_LIT(KPsiDevBoxFile, "C:\\System\\Data\\PsiDevBox.ini");

struct TPsiLink
	{
	TInt iBaudIndex;
	TInt iRtsCts;
	TInt iNetMode;
	TBuf<40> iPppStart;
	TInt iUseProxy;
	TBuf<60> iProxyHost;
	TInt iProxyPort;
	TInt iModemTls;
	TInt iKeepUp;
	TInt iLoadedVer;

	TPsiLink() : iLoadedVer(0) { SetProxyDefaults(); iModemTls = 0; iKeepUp = 1; }

	void SetProxyDefaults()
		{
		iUseProxy = 0;
		iProxyHost.Zero();                      // none until the person sets one
		iProxyPort = 8081;                      // PsiProxy's standard port
		}

	// ETrue if the proxy can really be used
	TBool ProxyUsable() const { return iUseProxy && iProxyHost.Length() && iProxyPort > 0 && iProxyPort < 65536; }

	// ETrue if aHost is empty, or the Atom modem's own web proxy (192.168.7.1
	// on its PPP link, or "psiproxy" through the modem), which fetches pages
	// but has none of PsiProxy's own services
	template <class D> static TBool IsAtomProxy(const D& aHost)
		{
		return aHost.Length() == 0 || SameF(aHost, "192.168.7.1") || SameF(aHost, "psiproxy");
		}
	template <class D> static TBool SameF(const D& aA, const char* aB)
		{
		TInt i = 0;
		for (; i < aA.Length() && aB[i]; i++)
			{
			TInt c = aA[i];
			if (c >= 'A' && c <= 'Z')
				c += 'a' - 'A';
			if (c != aB[i])
				return EFalse;
			}
		return i == aA.Length() && !aB[i];
		}

	// Where PsiProxy's own services are (Send log, a dev tool's builds):
	// the proxy, unless that is the Atom's own (or none), when it is the
	// address in PsiDevBox.ini if there is one. EFalse if there is nowhere:
	// then the caller says "Not available - set a proxy in Connection
	// settings". Nothing is built in.
	template <class D> TBool DevBox(RFs& aFs, D& aHost, TInt& aPort) const
		{
		if (!IsAtomProxy(iProxyHost))
			{
			aHost.Zero();
			for (TInt i = 0; i < iProxyHost.Length() && i < aHost.MaxLength(); i++)
				aHost.Append(iProxyHost[i]);
			aPort = iProxyPort > 0 && iProxyPort < 65536 ? iProxyPort : 8081;
			return ETrue;
			}
		return ReadDevBox(aFs, aHost, aPort);
		}

	// PsiDevBox.ini: ETrue and the address if it is there and sound
	template <class D> static TBool ReadDevBox(RFs& aFs, D& aHost, TInt& aPort)
		{
		RFile f;
		if (f.Open(aFs, KPsiDevBoxFile, EFileRead | EFileShareReadersOnly) != KErrNone)
			return EFalse;
		TBuf8<100> d;
		TInt r = f.Read(d);
		f.Close();
		if (r != KErrNone)
			return EFalse;
		TInt n = 0;                              // the host: up to the line end
		while (n < d.Length() && d[n] != '\r' && d[n] != '\n')
			n++;
		if (n == 0 || n > aHost.MaxLength() || n > 60)
			return EFalse;
		for (TInt i = 0; i < n; i++)             // (a name or an address: no spaces or controls)
			if (d[i] <= ' ' || d[i] > '~')
				return EFalse;
		TInt p = n;
		while (p < d.Length() && (d[p] == '\r' || d[p] == '\n'))
			p++;
		TInt port = 0, digits = 0;
		while (p < d.Length() && d[p] >= '0' && d[p] <= '9' && digits < 6)
			{
			port = port * 10 + (d[p++] - '0');
			digits++;
			}
		if (port <= 0 || port >= 65536)
			return EFalse;
		aHost.Zero();
		for (TInt i = 0; i < n; i++)
			aHost.Append(d[i]);
		aPort = port;
		return ETrue;
		}

	void SetDefaults()
		{
		SetProxyDefaults();
		iModemTls = 0;
		iKeepUp = 1;
		iLoadedVer = 0;
		iBaudIndex = 4;
		iRtsCts = 0;
		iNetMode = 0;
		iPppStart.Copy(_L("ATDT777"));
		}

	// ETrue if the shared file was there and readable
	TBool Load(RFs& aFs)
		{
		RFile f;
		if (f.Open(aFs, KPsiLinkFile, EFileRead | EFileShareReadersOnly) != KErrNone)
			return EFalse;
		TBuf8<200> d;
		TInt r = f.Read(d);
		f.Close();
		if (r != KErrNone || d.Length() < 6 || d[0] != 'P' || d[1] != 'L' || d[2] < 1)
			return EFalse;
		iBaudIndex = d[3] <= 4 ? d[3] : 4;
		iRtsCts = d[4] ? 1 : 0;
		iNetMode = d[5] ? 1 : 0;
		iPppStart.Zero();
		iLoadedVer = d[2] >= 4 ? 4 : d[2];
		SetProxyDefaults();
		iModemTls = 0;
		iKeepUp = 0;                           // (older than v4: as it was, the idle hang-up)
		TInt pos = 7;
		if (d.Length() >= 7)
			{
			TInt len = d[6];
			if (len <= 40 && 7 + len <= d.Length())
				{
				iPppStart.Copy(d.Mid(7, len));
				pos = 7 + len;
				}
			else
				pos = d.Length();
			}
		if (iLoadedVer >= 2 && pos + 2 <= d.Length())
			{
			TInt use = d[pos++] ? 1 : 0;
			TInt len = d[pos++];
			if (len <= 60 && pos + len + 2 <= d.Length())
				{
				iUseProxy = use;
				iProxyHost.Copy(d.Mid(pos, len));
				pos += len;
				iProxyPort = d[pos] | (d[pos + 1] << 8);
				pos += 2;
				if (iLoadedVer >= 3 && pos < d.Length())
					iModemTls = d[pos++] ? 1 : 0;
				if (iLoadedVer >= 4 && pos < d.Length())
					iKeepUp = d[pos++] ? 1 : 0;
				}
			}
		return ETrue;
		}

	// Writes it safely (to a temporary name, then renamed over the old one)
	void Save(RFs& aFs) const
		{
		TBuf8<200> d;
		d.Append('P');
		d.Append('L');
		d.Append(4);                           // version
		d.Append((TUint8)iBaudIndex);
		d.Append((TUint8)(iRtsCts ? 1 : 0));
		d.Append((TUint8)(iNetMode ? 1 : 0));
		TBuf8<40> p;
		p.Copy(iPppStart);
		d.Append((TUint8)p.Length());
		d.Append(p);
		d.Append((TUint8)(iUseProxy ? 1 : 0));
		TBuf8<60> h;
		h.Copy(iProxyHost);
		d.Append((TUint8)h.Length());
		d.Append(h);
		d.Append((TUint8)(iProxyPort & 0xff));
		d.Append((TUint8)((iProxyPort >> 8) & 0xff));
		d.Append((TUint8)(iModemTls ? 1 : 0));
		d.Append((TUint8)(iKeepUp ? 1 : 0));
		aFs.MkDirAll(KPsiLinkFile);
		TFileName tmp(KPsiLinkFile);
		tmp.Append('~');
		RFile f;
		if (f.Replace(aFs, tmp, EFileWrite) != KErrNone)
			return;
		TInt r = f.Write(d);
		if (r == KErrNone)
			r = f.Flush();
		f.Close();
		if (r == KErrNone)
			aFs.Replace(tmp, KPsiLinkFile);
		else
			aFs.Delete(tmp);
		}
	};

// Finds a psihold.exe to start (see "keeping the Psion Internet connection
// up" below): the first \System\Apps\<program>\psihold.exe on any disk
// but the ROM, C: first. Each program that can hold the link ships one in
// its own folder, so no list of programs is needed. It reads each disk's
// \System\Apps\ once, and runs only when a psihold.exe should start (the
// link up and none running), never on a poll. ETrue, with aExe, if found.
// (PSILINK_HOLDFIND: also for the host test, ssh/test/psiholdtest.cpp.)
#if !defined(PSILINK_STRUCT_ONLY) || defined(PSILINK_HOLDFIND)
_LIT(KPsiHoldExe, "psihold.exe");

inline TBool PsiHoldFind(RFs& aFs, TFileName& aExe)
	{
	TDriveList drives;
	if (aFs.DriveList(drives) != KErrNone)
		return EFalse;
	const TDesC& hold = KPsiHoldExe;
	TEntry entry;
	for (TInt d = 0; d < KMaxDrives - 1; d++)       // (not Z:, the ROM)
		{
		if (!drives[d])
			continue;
		TBuf<16> apps;
		apps.Append((TUint)('A' + d));
		apps.Append(_L(":\\System\\Apps\\"));
		TBuf<17> wild;
		wild.Copy(apps);
		wild.Append((TUint)'*');
		CDir* dir = NULL;
		if (aFs.GetDir(wild, KEntryAttDir | KEntryAttMatchExclusive, ESortNone, dir) != KErrNone || !dir)
			continue;
		TBool found = EFalse;
		for (TInt i = 0; i < dir->Count() && !found; i++)
			{
			const TEntry& e = (*dir)[i];
			if (!e.IsDir() || apps.Length() + e.iName.Length() + 1 + hold.Length() > aExe.MaxLength())
				continue;
			aExe.Copy(apps);
			aExe.Append(e.iName);
			aExe.Append((TUint)'\\');
			aExe.Append(hold);
			found = aFs.Entry(aExe, entry) == KErrNone;
			}
		delete dir;
		if (found)
			return ETrue;
		}
	return EFalse;
	}
#endif

// (0.68) The engine holds NIFMAN's idle timers off while it is online over
// Psion Internet (psiglue's RNif::DisableTimers), and gives them back as it
// hangs up or exits. If it crashed, or the app had to kill it, that last
// step never ran: the app calls this so the Psion's own idle hang-up works
// again and the link is not left up for ever. Harmless when the timers are
// already on, or no link is up. Needs nifman.lib.
#ifndef PSILINK_STRUCT_ONLY
inline void PsiLinkTimersBack()
	{
	RNif nif;
	if (nif.Open() == KErrNone)
		{
		nif.DisableTimers(EFalse);
		nif.Close();
		}
	}

// ----- keeping the Psion Internet connection up after use (file version 4) --
//
// What NIFMAN does (read from the SDK's own nifman.dll, WINS build, since
// the documentation says nothing; docs/epoc-comms-best-practices.md 2.3):
//  - Its idle timers come from CommDb's Timeout table, read through NetDial
//    as "Timeout\Client" and "Timeout\Route", in seconds. The Client one runs
//    once no network layer (TCP/IP with sockets) is using the link, the Route
//    one while one is but has nothing open on it.
//  - RNif::DisableTimers(ETrue) adds one to a count on the link; while the
//    count is above 0 no idle timer runs at all. It is per session: closing
//    the RNif session (or its process ending, a crash included) takes the
//    session's one off again. So it never outlives the program that asked.
// So a program that closes its sockets and sessions when it has finished -
// the engines do, to free the port on the modem route, and some go when
// idle for power - leaves the link to the Client timeout, which drops
// it within moments. psihold.exe (ssh/psihold.cpp) is the fix: one tiny
// process per Psion, started by whichever of our programs brings the link
// up, that only holds an RNif session with the timers off. It opens no
// socket (so NIFMAN does not take it for a user of the link and redial when
// the link drops), and it ends - giving the timers back - as soon as the
// link goes down (a Disconnect, the modem, the service, or our own hang-up
// of a dead link), or the setting changes to "Disconnect when idle" or to
// the modem route. A global semaphore, KPsiHoldLock, lets only one run; the
// kernel deletes it when that process ends, however it ends.

_LIT(KPsiHoldLock, "PsiLinkHold");

// Is psihold.exe running? (Looking for its semaphore opens nothing.)
inline TBool PsiHoldRunning()
	{
	TFindSemaphore find(_L("*PsiLinkHold"));
	TFullName name;
	return find.Next(name) == KErrNone;
	}

// Is the Psion's Internet connection up, by NIFMAN's account? 1 yes, 0 no,
// -1 it would not say (no socket server yet: then nothing is up either).
inline TInt PsiLinkActive()
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

// Starts psihold.exe when it should run: the Psion Internet route, "Stay
// connected", the link up, and none running yet. aNear is a file in the
// caller's own folder (the app's or the engine's), whose psihold.exe is
// tried first; then any program's folder (PsiHoldFind). Returns
// KErrNone if started, else why not (nothing to do is not a failure).
inline TInt PsiHoldStart(RFs& aFs, const TDesC& aNear)
	{
	TPsiLink link;
	link.SetDefaults();
	if (!link.Load(aFs) || !link.iNetMode || !link.iKeepUp)
		return KErrNotSupported;
	if (PsiHoldRunning())
		return KErrAlreadyExists;
	if (PsiLinkActive() != 1)
		return KErrNotReady;
	TFileName exe;
	TEntry entry;
	TInt found = EFalse;
	TParse parse;
	if (aNear.Length() && parse.Set(aNear, NULL, NULL) == KErrNone)
		{
		exe = parse.DriveAndPath();
		exe.Append(KPsiHoldExe);
		found = aFs.Entry(exe, entry) == KErrNone;
		}
	if (!found)
		found = PsiHoldFind(aFs, exe);
	if (!found)
		return KErrNotFound;
	RProcess p;
	TInt r = p.Create(exe, KNullDesC);
	if (r != KErrNone)
		return r;
	p.Resume();
	p.Close();
	return KErrNone;
	}

// Disconnect: takes the Psion's Internet connection down (RNif::Stop), which
// also ends psihold.exe. RNif::Stop is synchronous with no bound, so it runs
// in a thread of its own and the caller waits at most aWaitUs: the program
// never hangs on it. 1 hung up, 0 nothing was up, 2 still hanging up when
// the wait ended (it goes on by itself), <0 could not ask (an EPOC error).
struct TPsiLinkStop
	{
	volatile TInt iResult;
	volatile TInt iDone;
	};

inline TInt PsiLinkStopThread(TAny* aPtr)
	{
	TPsiLinkStop* st = (TPsiLinkStop*)aPtr;
	CTrapCleanup* cleanup = CTrapCleanup::New();
	TInt result = KErrNoMemory;
	RNif nif;
	if (cleanup && (result = nif.Open()) == KErrNone)
		{
		TBool active = EFalse;
		result = nif.NetworkActive(active);
		if (result == KErrNone)
			{
			result = 0;
			if (active)
				{
				TInt r = KErrNone;
				TRAPD(lr, r = nif.Stop());
				result = lr != KErrNone ? lr : (r != KErrNone ? r : 1);
				}
			}
		nif.Close();
		}
	st->iResult = result;
	st->iDone = 1;
	delete cleanup;
	return 0;
	}

inline TInt PsiLinkDisconnect(TInt aWaitUs)
	{
	// (the record is the app heap's; it stays with a thread that outlives the wait)
	TPsiLinkStop* st = (TPsiLinkStop*)User::Alloc(sizeof(TPsiLinkStop));
	if (!st)
		return KErrNoMemory;
	st->iResult = 0;
	st->iDone = 0;
	RThread thread;
	TBuf<32> name;
	name.Format(_L("PsiLinkStop%u"), User::TickCount());
	TInt r = thread.Create(name, PsiLinkStopThread, 8192, 4096, 32768, st);
	if (r != KErrNone)
		{
		User::Free(st);
		return r;
		}
	RTimer timer;
	if (timer.CreateLocal() != KErrNone)
		{
		thread.Kill(0);
		thread.Close();
		User::Free(st);
		return KErrNoMemory;
		}
	TRequestStatus died, waited;
	thread.Logon(died);
	thread.Resume();
	timer.After(waited, aWaitUs);
	User::WaitForRequest(died, waited);
	TInt result;
	if (died != KRequestPending)
		{
		timer.Cancel();
		User::WaitForRequest(waited);
		result = st->iDone ? st->iResult : KErrDied;
		User::Free(st);
		}
	else
		{
		thread.LogonCancel(died);
		User::WaitForRequest(died);
		result = 2;                      // (st stays with the thread)
		}
	timer.Close();
	thread.Close();
	return result;
	}
#endif // PSILINK_STRUCT_ONLY

#endif
