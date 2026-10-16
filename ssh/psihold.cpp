// psihold.cpp - psihold.exe: keeps the Psion's Internet connection up after
// our programs have finished with it, until the person disconnects.
//
// Why it is needed, and why this is enough, is in psilink.h ("keeping the
// Psion Internet connection up after use"). In short: NIFMAN runs no idle
// timer while some RNif session has DisableTimers(ETrue) in force, and that
// lasts only as long as the session. The engines close theirs when they
// finish (or go idle), so this process holds one instead.
//
// It holds nothing else: no socket, no socket server session, never the
// serial port (PPP has that while the link is up, and lets it go when it
// goes down). One runs per Psion (the KPsiHoldLock semaphore). It ends, and
// with its session closing the timers are NIFMAN's again, as soon as
//  - the link goes down: a hang-up stage or "link down" from NIFMAN's
//    progress, or NetworkActive saying no (looked at each minute as well);
//  - PsiLink.ini says the modem route or "Disconnect when idle" (a change
//    to the file wakes it; it also looks each minute);
//  - anything fails.
// It waits on requests only (no polling), so the Psion can halt between.
// A few lines go to C:\System\Data\PsiHold.log (4 KB at most).
//
// Started by PsiHoldStart (psilink.h): the engines when they have the link
// up (psiglue.cpp), and the apps after Connect or a Test that dialled.

#include <e32std.h>
#include <e32base.h>
#include <f32file.h>
#include <nifman.h>
#include <netdial.h>      // TNetDialProgress (enum only)
#include "psilink.h"

const TInt KHoldCheckUs = 60000000;         // NetworkActive asked this often too
const TInt KHoldLinkDown = 1001;            // EIfProgressLinkDown (in_iface.h)
const TInt KHoldLogMax = 4096;
_LIT(KHoldLog, "C:\\System\\Data\\PsiHold.log");
_LIT(KHoldWatch, "C:\\System\\Data\\PsiLink.ini");   // (and Wanted is asked each minute too)

static void Log(RFs& aFs, const TDesC8& aText)
	{
	RFile f;
	TInt size = 0;
	TInt r = f.Open(aFs, KHoldLog, EFileWrite | EFileShareAny);
	if (r == KErrNone && (f.Size(size) != KErrNone || size > KHoldLogMax))
		{
		f.Close();
		r = KErrNotFound;                    // too long: start it again
		}
	if (r != KErrNone)
		r = f.Replace(aFs, KHoldLog, EFileWrite | EFileShareAny);
	if (r != KErrNone)
		return;
	TInt end = 0;
	f.Seek(ESeekEnd, end);
	TTime now;
	now.HomeTime();
	TDateTime t = now.DateTime();
	TBuf8<120> line;
	line.Format(_L8("%02d/%02d %02d:%02d:%02d "), t.Day() + 1, t.Month() + 1, t.Hour(), t.Minute(), t.Second());
	TInt room = line.MaxLength() - line.Length() - 2;
	line.Append(aText.Left(aText.Length() < room ? aText.Length() : room));
	line.Append(_L8("\r\n"));
	f.Write(line);
	f.Close();
	}

static void LogNum(RFs& aFs, const TDesC8& aText, TInt aNum)
	{
	TBuf8<100> b;
	b.Copy(aText.Left(aText.Length() < 80 ? aText.Length() : 80));
	b.AppendNum(aNum);
	Log(aFs, b);
	}

// Should it still hold? (the setting, as the programs last saved it)
static TBool Wanted(RFs& aFs)
	{
	TPsiLink link;
	link.SetDefaults();
	return link.Load(aFs) && link.iNetMode && link.iKeepUp;
	}

// A progress stage that means the link is going or gone
static TBool StageDown(TInt aStage)
	{
	return aStage == EStartingHangUp || aStage == EFinishedHangUp || aStage == KHoldLinkDown;
	}

static void Hold(RFs& aFs)
	{
	if (!Wanted(aFs))
		{
		Log(aFs, _L8("not started: the setting is the modem route or Disconnect when idle"));
		return;
		}
	RNif nif;
	TInt r = nif.Open();
	if (r != KErrNone)
		{
		LogNum(aFs, _L8("not started: no NIFMAN session, "), r);
		return;
		}
	TBool active = EFalse;
	if (nif.NetworkActive(active) != KErrNone || !active)
		{
		Log(aFs, _L8("not started: the link is not up"));
		nif.Close();
		return;
		}
	r = nif.DisableTimers(ETrue);
	if (r != KErrNone)
		{
		LogNum(aFs, _L8("not started: DisableTimers failed, "), r);
		nif.Close();
		return;
		}
	RTimer timer;
	if (timer.CreateLocal() != KErrNone)
		{
		nif.DisableTimers(EFalse);
		nif.Close();
		return;
		}
	Log(aFs, _L8("holding the link up (idle timers off)"));

	TNifProgressBuf prog;
	TRequestStatus progStat, fileStat, timeStat;
	TBool progOn = EFalse, fileOn = EFalse, timeOn = EFalse;
	const char* why = 0;
	TInt whyNum = 0;
	for (;;)
		{
		if (!progOn)
			{
			nif.ProgressNotification(prog, progStat);
			progOn = ETrue;
			}
		if (!fileOn)
			{
			aFs.NotifyChange(ENotifyAll, fileStat, KHoldWatch);
			fileOn = ETrue;
			}
		if (!timeOn)
			{
			timer.After(timeStat, KHoldCheckUs);
			timeOn = ETrue;
			}
		// one completion handled per signal (each completion signals once)
		User::WaitForAnyRequest();
		if (progOn && progStat != KRequestPending)
			{
			progOn = EFalse;
			if (progStat.Int() != KErrNone)
				{
				why = "progress request ended, ";
				whyNum = progStat.Int();
				break;
				}
			if (StageDown(prog().iStage))
				{
				why = "the link is going down: stage ";
				whyNum = prog().iStage;
				break;
				}
			}
		else if (fileOn && fileStat != KRequestPending)
			{
			fileOn = EFalse;
			if (!Wanted(aFs))
				{
				why = "the setting changed (modem route or Disconnect when idle)";
				break;
				}
			}
		else if (timeOn && timeStat != KRequestPending)
			{
			timeOn = EFalse;
			active = EFalse;
			if (nif.NetworkActive(active) != KErrNone || !active)
				{
				why = "NIFMAN says the link is down";
				break;
				}
			// (in case a change to the file was not notified: a rename onto
			// it, a program that wrote it some other way)
			if (!Wanted(aFs))
				{
				why = "the setting is the modem route or Disconnect when idle";
				break;
				}
			}
		// (else a stray signal: nothing of ours completed)
		}
	// every request still out is cancelled and its completion taken
	if (progOn)
		{
		nif.CancelProgressNotification();
		User::WaitForRequest(progStat);
		}
	if (fileOn)
		{
		aFs.NotifyChangeCancel();
		User::WaitForRequest(fileStat);
		}
	if (timeOn)
		{
		timer.Cancel();
		User::WaitForRequest(timeStat);
		}
	timer.Close();
	nif.DisableTimers(EFalse);              // (closing the session does this too)
	nif.Close();
	TBuf8<100> w;
	w.Copy(_L8("released: "));
	w.Append(TPtrC8((const TUint8*)why));
	if (whyNum)
		w.AppendNum(whyNum);
	Log(aFs, w);
	}

GLDEF_C TInt E32Main()
	{
	RSemaphore lock;
	if (lock.CreateGlobal(KPsiHoldLock, 0) != KErrNone)
		return 0;                            // one runs already (or no memory)
	RFs fs;
	if (fs.Connect() == KErrNone)
		{
		Hold(fs);
		fs.Close();
		}
	lock.Close();
	return 0;
	}
