// PMBUTTON.CPP - pmbutton.exe: the Email icon below the screen opens PsiMail.
//
// The EIKON server turns a tap on the program icons below the screen into
// key events (RWindow::AddKeyRect in eiksrv.cpp): EEikAppbarSystemKey for the
// System icon, then EEikAppbarApp1Key.. for the rest. On the Series 5mx the
// icons are System, Word, Sheet, Contacts, Agenda, Email, Calc, Jotter,
// Extras, so Email is EEikAppbarApp5Key. The System screen captures those
// keys (RWindowGroup::CaptureKey) and starts the built-in programs; there is
// no setting that changes which program it starts. The most recent capture
// of a key wins, so this program captures the Email key after the System
// screen has (and again whenever a window group comes or goes, so it stays
// the most recent) and opens PsiMail with it instead: brought to the front
// if it is running, started if not. It captures no other key, so every other
// key and icon goes where it always went.
//
// It runs only while PsiMail's "Email icon opens: PsiMail" preference is on,
// which is while C:\System\Apps\PsiMail\Button.ini exists: PsiMail starts it
// when the preference is turned on, and pmbutton.rdl in \System\Recogs
// starts it 30 s after the Psion starts (pmbtnrec.cpp). It stops, and its
// capture goes with it (the Email icon is the System screen's again), when
//  - PsiMail sends it a shutdown event (the preference was turned off);
//  - Button.ini is deleted (by PsiMail, or by the uninstaller's FN line);
//  - PsiMail.app is deleted or replaced (uninstalled or upgraded);
//  - PsiMail is not installed when it starts (it then deletes Button.ini).
// One copy at most (a global mutex). No window, nothing drawn: it sleeps on
// the window server and the file server until something happens.

#include <e32base.h>
#include <f32file.h>
#include <w32std.h>
#include <eikkeys.h>
#include <apgtask.h>
#include <apgwgnam.h>
#include <apacmdln.h>
#include <coedef.h>

const TUid KUidPsiMail = { 0x01000A7C };
const TUid KUidPmButton = { 0x01000A80 };
const TInt KEmailKey = EEikAppbarApp5Key;
const TInt KRecaptureDelay = 3000000;   // and once more 3 s after a window group change

_LIT(KMutexName, "PsiMailButton");
_LIT(KMarker, "C:\\System\\Apps\\PsiMail\\Button.ini");
_LIT(KMarkerDir, "C:\\System\\Apps\\PsiMail\\");
_LIT(KAppName, "PsiMail.app");
_LIT(KAppDir, "\\System\\Apps\\PsiMail\\");
_LIT(KAppRun, "Z:\\System\\Programs\\APPRUN.EXE");
_LIT(KCaption, "PsiMail button");

static TBool Wanted(RFs& aFs)
	{
	TEntry entry;
	return aFs.Entry(KMarker, entry) == KErrNone;
	}

// Capturing the key again a moment after a window group change: the System
// screen captures its keys after its window group is made, which may be
// after this program has seen the change (at boot).
class CPmRecapture : public CTimer
	{
public:
	CPmRecapture(RWindowGroup& aGroup, TInt32& aCapture);
	void ConstructL() { CTimer::ConstructL(); }
	void Capture();
private:
	void RunL() { Capture(); }
private:
	RWindowGroup& iGroup;
	TInt32& iCapture;
	};

CPmRecapture::CPmRecapture(RWindowGroup& aGroup, TInt32& aCapture)
	: CTimer(EPriorityLow), iGroup(aGroup), iCapture(aCapture)
	{
	CActiveScheduler::Add(this);
	}

// Take the Email key, as the most recent capture of it
void CPmRecapture::Capture()
	{
	if (iCapture > 0)
		iGroup.CancelCaptureKey(iCapture);
	iCapture = iGroup.CaptureKey(KEmailKey, 0, 0);   // (< 0: not now; the next change tries again)
	}

// Stops this program when it is no longer wanted: Button.ini deleted
// (PsiMail's preference turned off, or PsiMail uninstalled), or PsiMail.app
// deleted or replaced (uninstalled, or being upgraded - this program's own
// file is installed after it, and is then free to be replaced; the new
// pmbutton.rdl starts the new copy). One of these watches each folder: C:'s
// PsiMail folder, and the one PsiMail.app is in if that is another disk.
class CPmWatch : public CActive
	{
public:
	CPmWatch(const TDesC& aDir, const TDesC& aApp, const TEntry& aAppEntry);
	~CPmWatch();
	void StartL();
private:
	void Start();
	TBool StillWanted();
	void RunL();
	void DoCancel() { iFs.NotifyChangeCancel(); }
private:
	RFs iFs;                     // (its own session: Cancel cancels only this)
	TFileName iDir;
	TFileName iApp;
	TTime iAppModified;
	TInt iAppSize;
	};

CPmWatch::CPmWatch(const TDesC& aDir, const TDesC& aApp, const TEntry& aAppEntry)
	: CActive(EPriorityStandard), iDir(aDir), iApp(aApp), iAppModified(aAppEntry.iModified), iAppSize(aAppEntry.iSize)
	{
	CActiveScheduler::Add(this);
	}

CPmWatch::~CPmWatch()
	{
	Cancel();
	iFs.Close();
	}

void CPmWatch::StartL()
	{
	User::LeaveIfError(iFs.Connect());
	Start();
	}

void CPmWatch::Start()
	{
	iFs.NotifyChange(ENotifyAll, iStatus, iDir);
	SetActive();
	}

TBool CPmWatch::StillWanted()
	{
	if (!Wanted(iFs))
		return EFalse;
	TEntry entry;
	if (iFs.Entry(iApp, entry) != KErrNone)
		return EFalse;
	return entry.iModified == iAppModified && entry.iSize == iAppSize;
	}

void CPmWatch::RunL()
	{
	if (!StillWanted())
		{
		CActiveScheduler::Stop();
		return;
		}
	Start();
	}

// The window server's events: the Email key, window group changes, and
// PsiMail's shutdown event
class CPmButton : public CActive
	{
public:
	CPmButton(RWsSession& aWs, RFs& aFs, CPmRecapture& aRecapture);
	~CPmButton();
	void Start();
private:
	void RunL();
	void DoCancel() { iWs.EventReadyCancel(); }
	TInt RunError(TInt aError);
	void OpenPsiMailL();
private:
	RWsSession& iWs;
	RFs& iFs;
	CPmRecapture& iRecapture;
	RProcess iStarted;          // the last APPRUN started, while it may still be loading
	TBool iStarting;
	};

CPmButton::CPmButton(RWsSession& aWs, RFs& aFs, CPmRecapture& aRecapture)
	: CActive(EPriorityStandard), iWs(aWs), iFs(aFs), iRecapture(aRecapture)
	{
	CActiveScheduler::Add(this);
	}

CPmButton::~CPmButton()
	{
	Cancel();
	if (iStarting)
		iStarted.Close();
	}

void CPmButton::Start()
	{
	iWs.EventReady(&iStatus);
	SetActive();
	}

TInt CPmButton::RunError(TInt /*aError*/)
	{
	// (out of memory while starting PsiMail: the next event is already
	// waited for, and the next tap tries again)
	return KErrNone;
	}

void CPmButton::RunL()
	{
	TWsEvent event;
	iWs.GetEvent(event);
	TInt type = event.Type();
	if (type == EEventUser && *(TApaSystemEvent*)event.EventData() == EApaSystemEventShutdown)
		{
		CActiveScheduler::Stop();      // PsiMail: the preference was turned off
		return;
		}
	Start();
	if (type == EEventWindowGroupsChanged)
		{
		iRecapture.Capture();
		iRecapture.Cancel();
		iRecapture.After(KRecaptureDelay);
		}
	else if (type == EEventKey && event.Key()->iCode == (TUint)KEmailKey)
		OpenPsiMailL();
	}

void CPmButton::OpenPsiMailL()
	{
	TApaTaskList tasks(iWs);
	TApaTask task = tasks.FindApp(KUidPsiMail);
	if (task.Exists())
		{
		task.BringToForeground();
		return;
		}
	if (iStarting)
		{
		// started a moment ago and still opening (its window is not up yet)
		if (iStarted.ExitType() == EExitPending)
			return;
		iStarted.Close();
		iStarting = EFalse;
		}
	TFindFile find(iFs);
	if (find.FindByDir(KAppName, KAppDir) != KErrNone)
		{
		// PsiMail is not installed: the Email icon is the System screen's again
		iFs.Delete(KMarker);
		CActiveScheduler::Stop();
		return;
		}
	CApaCommandLine* cmd = CApaCommandLine::NewLC();
	cmd->SetLibraryNameL(find.File());
	cmd->SetCommandL(EApaCommandRun);
	TInt r = iStarted.Create(KAppRun, cmd->FullCommandLine());
	CleanupStack::PopAndDestroy();     // cmd
	if (r != KErrNone)
		return;
	iStarting = ETrue;
	iStarted.Resume();
	}

static void MainL()
	{
	RFs fs;
	User::LeaveIfError(fs.Connect());
	CleanupClosePushL(fs);
	if (!Wanted(fs))
		{
		CleanupStack::PopAndDestroy();   // fs: the preference is off
		return;
		}
	TFindFile find(fs);
	TEntry appEntry;
	if (find.FindByDir(KAppName, KAppDir) != KErrNone || fs.Entry(find.File(), appEntry) != KErrNone)
		{
		fs.Delete(KMarker);              // PsiMail was removed: so is the preference
		CleanupStack::PopAndDestroy();   // fs
		return;
		}
	TFileName app(find.File());
	TParsePtrC appParse(app);
	CActiveScheduler* scheduler = new(ELeave) CActiveScheduler;
	CleanupStack::PushL(scheduler);
	CActiveScheduler::Install(scheduler);

	RWsSession ws;
	User::LeaveIfError(ws.Connect());
	CleanupClosePushL(ws);
	RWindowGroup group(ws);
	User::LeaveIfError(group.Construct((TUint32)&group));
	CleanupClosePushL(group);
	group.EnableReceiptOfFocus(EFalse);
	group.SetOrdinalPosition(-1, ECoeWinPriorityNeverAtFront);
	// named like a program's, marked "system", so the list of open files and
	// programs does not show it (nor close it) and PsiMail finds it by UID
	CApaWindowGroupName* name = CApaWindowGroupName::NewLC(ws);
	name->SetSystem(ETrue);
	name->SetAppUid(KUidPmButton);
	name->SetCaptionL(KCaption);
	User::LeaveIfError(name->SetWindowGroupName(group));
	CleanupStack::PopAndDestroy();       // name
	User::LeaveIfError(group.EnableGroupChangeEvents());

	TInt32 capture = 0;
	CPmRecapture* recapture = new(ELeave) CPmRecapture(group, capture);
	CleanupStack::PushL(recapture);
	recapture->ConstructL();
	CPmWatch* watch = new(ELeave) CPmWatch(KMarkerDir, app, appEntry);
	CleanupStack::PushL(watch);
	CPmWatch* appWatch = NULL;
	if (appParse.DriveAndPath().CompareF(KMarkerDir) != 0)
		appWatch = new(ELeave) CPmWatch(appParse.DriveAndPath(), app, appEntry);
	CleanupStack::PushL(appWatch);
	CPmButton* button = new(ELeave) CPmButton(ws, fs, *recapture);
	CleanupStack::PushL(button);

	recapture->Capture();
	recapture->After(KRecaptureDelay);
	watch->StartL();
	if (appWatch)
		appWatch->StartL();
	button->Start();
	CActiveScheduler::Start();

	CleanupStack::PopAndDestroy(4);      // button, appWatch, watch, recapture
	if (capture > 0)
		group.CancelCaptureKey(capture);  // (the Email icon is the System screen's again)
	CleanupStack::PopAndDestroy(4);      // group, ws, scheduler, fs
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	if (!cleanup)
		return KErrNoMemory;
	RMutex one;
	if (one.CreateGlobal(KMutexName) != KErrNone)
		{
		delete cleanup;                  // another pmbutton.exe is running
		return KErrNone;
		}
	TRAPD(err, MainL());
	one.Close();
	delete cleanup;
	return err;
	}
