// pglinktest.cpp - Connection settings > Test, compiled into PsiTerm.app,
// PsiMail.app and PsiWeb.app (each .mmp: SUBPROJECT psitermssh). The test
// is psiglue.cpp's pg_link_test (only that part of the file is compiled
// here); this adds the EIKON side, so each app's dialog only reads its
// lines and calls PgLinkTestL.
#define PG_LINK_TEST_ONLY
#include "psiglue.cpp"

#include <e32base.h>
#include <e32hal.h>
#include <e32keys.h>
#include <coeaui.h>
#include <eikenv.h>
#include <eikdialg.h>
#include <eiklabel.h>

// The result: one label per sentence, unused lines removed (a hidden line
// would still take its height)
class CPgLinkResultDialog : public CEikDialog
	{
public:
	CPgLinkResultDialog(const PgLinkTest& aTest, TInt aFirstLineId, const TDesC* aTitle = 0)
		: iTest(aTest), iFirstLineId(aFirstLineId), iTitle(aTitle) {}
private:
	void PreLayoutDynInitL();
	void SetSizeAndPositionL(const TSize& aSize);
	const PgLinkTest& iTest;
	TInt iFirstLineId;
	const TDesC* iTitle;               // another title than the resource's, or 0
	};

void CPgLinkResultDialog::PreLayoutDynInitL()
	{
	if (iTitle)
		SetTitleL(*iTitle);
	for (TInt i = 0; i < PG_LT_LINES; i++)
		{
		if (i < iTest.nlines)
			{
			TBuf<PG_LT_LINE> t;
			t.Copy(TPtrC8((const TUint8*)iTest.line[i]));
			SetLabelL(iFirstLineId + i, t);
			}
		else
			DeleteLine(iFirstLineId + i);
		}
	}

// Centred, and never wider or taller than the screen
void CPgLinkResultDialog::SetSizeAndPositionL(const TSize& aSize)
	{
	TSize screen = iEikonEnv->ScreenDevice()->SizeInPixels();
	TSize size(aSize.iWidth < screen.iWidth - 8 ? aSize.iWidth : screen.iWidth - 8,
		aSize.iHeight < screen.iHeight - 8 ? aSize.iHeight : screen.iHeight - 8);
	SetCornerAndSizeL(EHCenterVCenter, size);
	}

// ----- running the test off the app's thread --------------------------------
// pg_link_test blocks for seconds, and for a couple of minutes when it
// dials. Run in the app's thread it froze the app: no repaint, no Esc, no
// heartbeat to psissh (which ended a live SSH session after 45 s), and no
// keeping the Psion awake through a dial-up. So it runs in a thread of its
// own, with its own serial and socket sessions (comms notes N14), while the
// app's thread waits in a nested active scheduler loop, as EIKON does for a
// dialog: the app keeps drawing, its timers run, and Esc stops the test.

const TInt KPgLinkTickUs     = 250000;     // the app looks at the test this often
const TInt KPgLinkMaxTicks   = 4 * 180;    // 180 s: past the longest path (PPP start + 90 s lookup + cancels)
const TInt KPgLinkStopTicks  = 4 * 15;     // after a stop, the test gets this long to end
const TInt KPgLinkAwakeTicks = 4 * 20;     // the auto switch-off timer is reset this often

// On the control stack above the dialog while the test runs: it takes every
// key (the dialog beneath must not act on one meanwhile), and Esc stops the
// test. It owns no window: CONE offers keys to every control on the stack,
// top down, whether or not it has focus.
class CPgLinkKeys : public CCoeControl
	{
public:
	CPgLinkKeys(PgLinkTest& aTest) : iTest(aTest) {}
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
		{
		if (aType == EEventKey && aKeyEvent.iCode == EKeyEscape)
			iTest.abort_test = 1;
		return EKeyWasConsumed;
		}
private:
	PgLinkTest& iTest;
	};

// Ends the nested wait when the test's thread ends
class CPgLinkWatch : public CActive
	{
public:
	CPgLinkWatch(RThread& aThread) : CActive(EPriorityStandard), iThread(aThread)
		{ CActiveScheduler::Add(this); }
	~CPgLinkWatch() { Cancel(); }
	void Watch() { iThread.Logon(iStatus); SetActive(); }
private:
	void RunL() { CActiveScheduler::Stop(); }
	void DoCancel() { iThread.LogonCancel(iStatus); }
	RThread& iThread;
	};

struct TPgLinkRun
	{
	PgLinkTest* iTest;
	CEikonEnv* iEnv;
	int iShownSeq;
	TInt iTicks;
	TInt iStopTick;                    // the tick the stop was asked at (-1: not yet)
	int iGaveUp;                       // the test did not end after a stop: the wait ended without it
	};

// Every quarter second while the test runs: the busy message, the auto
// switch-off timer (a dial-up must not be cut by the Psion switching off),
// the overall limit, and the limit after a stop
static TInt PgLinkTick(TAny* aPtr)
	{
	TPgLinkRun* r = (TPgLinkRun*)aPtr;
	PgLinkTest* t = r->iTest;
	r->iTicks++;
	if (t->busy_seq != r->iShownSeq)
		{
		r->iShownSeq = t->busy_seq;
		TBuf<80> b;
		b.Copy(TPtrC8((const TUint8*)t->busy));
		TRAPD(err, r->iEnv->BusyMsgL(b, EHLeftVBottom, TTimeIntervalMicroSeconds32(0)));
		(void)err;
		}
	if (r->iTicks % KPgLinkAwakeTicks == 0)
		UserHal::ResetAutoSwitchOffTimer();
	if (!t->abort_test && r->iTicks >= KPgLinkMaxTicks)
		t->abort_test = 1;
	if (t->abort_test && r->iStopTick < 0)
		r->iStopTick = r->iTicks;
	if (t->abort_test && !t->done && r->iTicks - r->iStopTick >= KPgLinkStopTicks)
		{
		r->iGaveUp = 1;
		CActiveScheduler::Stop();
		}
	return 1;
	}

// The test's thread. (pg_link_test never leaves; the cleanup stack is there
// in case something under it does.)
static TInt PgLinkThread(TAny* aPtr)
	{
	PgLinkTest* t = (PgLinkTest*)aPtr;
	CTrapCleanup* cleanup = CTrapCleanup::New();
	pg_link_test(t);
	t->done = 1;
	delete cleanup;
	return 0;
	}

// The key catcher comes off the control stack on every path, a leave
// through the nested loop included (the app closing: KLeaveExit)
struct TPgLinkStacked
	{
	CCoeAppUi* iAppUi;
	CCoeControl* iControl;
	};

static void PgLinkUnstack(TAny* aPtr)
	{
	TPgLinkStacked* s = (TPgLinkStacked*)aPtr;
	s->iAppUi->RemoveFromStack(s->iControl);
	}

// Runs the test in a thread of its own and waits for it with the app's loop
// running. 0 finished, 1 stopped (Esc, the dialog's Cancel, or the limit),
// 2 the thread did not end even after a stop: aTest and its lookup block
// are then left to it for good (PgLinkFree).
static TInt PgLinkRunThreadL(PgLinkTest* aTest, CEikonEnv* aEnv)
	{
	aTest->abort_test = 0;
	aTest->busy[0] = 0;
	aTest->busy_seq = 0;
	if (!aTest->lookup)
		aTest->lookup = pg_lt_lookup_new();    // (0: the lookup then says "no memory")
	CPgLinkKeys* keys = new(ELeave) CPgLinkKeys(*aTest);
	CleanupStack::PushL(keys);
	CPeriodic* tick = CPeriodic::NewL(CActive::EPriorityStandard);
	CleanupStack::PushL(tick);
	RThread thread;
	TBuf<32> name;
	name.Format(_L("PgLinkTest%u"), User::TickCount());   // (unique even after one that never ended)
	User::LeaveIfError(thread.Create(name, PgLinkThread, 16384, 4096, 65536, aTest));
	CleanupClosePushL(thread);
	CPgLinkWatch* watch = new(ELeave) CPgLinkWatch(thread);
	CleanupStack::PushL(watch);
	TPgLinkStacked stacked;
	stacked.iAppUi = (CCoeAppUi*)aEnv->AppUi();   // (CCoeAppUiBase*: every EIKON app UI is a CCoeAppUi)
	stacked.iControl = keys;
	// (0.84) the cleanup item first, then onto the control stack: pushing it
	// inside a TRAP unbalanced the cleanup stack at the TRAP's end, and EPOC
	// panics then (E32USER-CBase 71). Taking a control off the stack that
	// never got on is harmless.
	CleanupStack::PushL(TCleanupItem(PgLinkUnstack, &stacked));
	TRAPD(err, stacked.iAppUi->AddToStackL(keys, ECoeStackPriorityAlert, ECoeStackFlagRefusesFocus));
	if (err != KErrNone)
		{
		thread.Kill(0);                  // never ran: not left suspended
		User::Leave(err);
		}
	TPgLinkRun run;
	run.iTest = aTest;
	run.iEnv = aEnv;
	run.iShownSeq = 0;
	run.iTicks = 0;
	run.iStopTick = -1;
	run.iGaveUp = 0;
	tick->Start(KPgLinkTickUs, KPgLinkTickUs, TCallBack(PgLinkTick, &run));
	watch->Watch();
	aTest->done = 0;                     // (from here the record belongs to the thread too)
	thread.Resume();
	CActiveScheduler::Start();           // until the thread ends (watch), or a stop gives up
	tick->Cancel();
	TInt rc = aTest->abort_test ? 1 : 0;
	// The loop stopped, but not by the watch: someone else called Stop -
	// the dialog beneath, whose Cancel was tapped with the pen (keys never
	// reach it), or the app closing. Treat it as a stop here, and hand the
	// stop on to the loop it was meant for once this one has tidied up.
	TBool passOn = !run.iGaveUp && watch->IsActive();
	if (!aTest->done)
		{
		aTest->abort_test = 1;
		for (TInt i = 0; i < 40 && !aTest->done; i++)
			User::After(250000);         // the test gives up at its next step
		rc = aTest->done ? 1 : 2;
		}
	watch->Cancel();                     // (takes the logon's completion, or cancels it)
	aEnv->BusyMsgCancel();
	CleanupStack::PopAndDestroy(5);      // unstack, watch, thread (a thread still running goes on), tick, keys
	if (passOn)
		CActiveScheduler::Stop();
	return rc;
	}

// Frees the test's record and its lookup block: not while a thread still
// runs on the record (it stays with the thread), and not the lookup block
// when a lookup was abandoned in it (a late completion would land in freed
// memory)
static void PgLinkFree(TAny* aTest)
	{
	PgLinkTest* t = (PgLinkTest*)aTest;
	if (!t->done)
		return;
	if (!t->lookup_leaked)
		pg_lt_lookup_free(t->lookup);
	delete t;
	}

static void PgLinkRunL(int aBaudIndex, int aRtsCts, int aNetMode, const TDesC8& aPppStart,
	int aResultDialog, int aFirstLineId, TBool aConnect, const TDesC* aTitle)
	{
	CEikonEnv* env = CEikonEnv::Static();
	PgLinkTest* t = new(ELeave) PgLinkTest;
	Mem::FillZ(t, sizeof(PgLinkTest));
	t->done = 1;                         // no thread on it yet (PgLinkFree)
	CleanupStack::PushL(TCleanupItem(PgLinkFree, t));
	t->baud_index = aBaudIndex;
	t->rtscts = aRtsCts ? 1 : 0;
	t->net_mode = aNetMode ? 1 : 0;
	TInt n = aPppStart.Length();
	if (n > (TInt)sizeof(t->ppp_start) - 1)
		n = sizeof(t->ppp_start) - 1;
	Mem::Copy(t->ppp_start, aPppStart.Ptr(), n);
	t->ppp_start[n] = 0;
	t->dial = aConnect ? 1 : 0;         // Connect: dial at once, nothing to ask
	TInt rc = PgLinkRunThreadL(t, env);
	if (rc == 0 && t->need_dial)
		{
		// never dial unasked: statement, then the question
		if (CEikonEnv::QueryWinL(_L("The Psion's Internet connection is not up"), _L("Connect now to test it?")))
			{
			t->dial = 1;
			rc = PgLinkRunThreadL(t, env);
			}
		}
	if (rc == 2)
		{
		// the thread is still inside C32 or ESOCK: its record stays with it
		// (PgLinkFree leaves a record whose thread has not ended)
		CleanupStack::PopAndDestroy();   // t: kept, see above
		env->InfoMsg(aConnect ? _L("Not available - the last attempt has not ended yet")
		                      : _L("Not available - the last test has not ended yet"));
		return;
		}
	if (rc == 1)
		{
		env->InfoMsg(_L("Stopped"));
		CleanupStack::PopAndDestroy();   // t
		return;
		}
	CPgLinkResultDialog* d = new(ELeave) CPgLinkResultDialog(*t, aFirstLineId, aTitle);
	d->ExecuteLD(aResultDialog);
	CleanupStack::PopAndDestroy();       // t
	}

void PgLinkTestL(int aBaudIndex, int aRtsCts, int aNetMode, const TDesC8& aPppStart,
	int aResultDialog, int aFirstLineId)
	{
	PgLinkRunL(aBaudIndex, aRtsCts, aNetMode, aPppStart, aResultDialog, aFirstLineId, EFalse, 0);
	}

void PgLinkConnectL(int aBaudIndex, int aRtsCts, int aNetMode, const TDesC8& aPppStart,
	int aResultDialog, int aFirstLineId, const TDesC8& aTitle)
	{
	PgLinkRunL(aBaudIndex, aRtsCts, aNetMode, aPppStart, aResultDialog, aFirstLineId, ETrue, &aTitle);
	}
