// PMBTNREC.CPP - pmbutton.rdl: starts pmbutton.exe after the Psion starts.
//
// EPOC R5 has no list of programs to run at start-up, but the application
// architecture server loads every file recogniser in ?:\System\Recogs when
// it starts at boot (CApaAppListServer::ConstructL, in the System screen's
// process), and again whenever that folder changes. This is a recogniser
// that recognises nothing. What it does is start pmbutton.exe once, 30 s
// after it is loaded, while PsiMail's "Email icon opens: PsiMail" preference
// is on (C:\System\Apps\PsiMail\Button.ini exists).
//
// Why 30 s later, from a timer, and not at once: a program started from
// here while the Psion is still starting up stops the System screen from
// finishing its start (in the emulator: no icons, or a blank screen). Once
// it has started, a program started from here is harmless. The timer is an
// active object in the server's own thread, so it runs when that thread is
// idle; it starts the program and is done.
//
// Safety: the recogniser is consulted only after the ROM's recognisers have
// passed on a file (it is added at the end of the list), and it always says
// ENotRecognized, so it never changes how a file is opened, and its RunL is
// never called. Nothing here can leave or panic (the server's active
// scheduler faults the server if a RunL leaves): only R-class calls whose
// errors are checked, and allocations that are checked. If it cannot be made,
// CreateRecognizer returns NULL, which apparc allows (the library is closed
// again: CApaScanningFileRecognizer::LoadRecognizerL in apparc/apfile/
// apfrec.cpp). When the file is deleted (uninstalled), apparc deletes the
// recogniser, and with it the timer, before it closes the library. A loaded
// recogniser does not keep its file in use: it can be deleted or replaced.
//
// UIDs: KDynamicLibraryUid, KUidFileRecognizer8 (0x1000013E, the narrow
// file recogniser type that apparc checks on MARM - not the Unicode
// 0x10003A37), and this DLL's own UID, 0x01000A81. Ordinal 1 is
// CreateRecognizer, the only export.

#include <e32base.h>
#include <f32file.h>
#include <apaflrec.h>

_LIT(KMarker, "C:\\System\\Apps\\PsiMail\\Button.ini");
_LIT(KExeName, "pmbutton.exe");
_LIT(KAppDir, "\\System\\Apps\\PsiMail\\");
const TInt KStartDelay = 30000000;     // 30 s

static void StartButton()
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	TEntry entry;
	if (fs.Entry(KMarker, entry) == KErrNone)
		{
		TFindFile find(fs);
		if (find.FindByDir(KExeName, KAppDir) == KErrNone)
			{
			// (pmbutton.exe checks again that PsiMail is installed, and
			// that it is the only copy running)
			RProcess process;
			if (process.Create(find.File(), KNullDesC) == KErrNone)
				{
				process.Resume();
				process.Close();
				}
			}
		}
	fs.Close();
	}

class CPmButtonStarter : public CTimer
	{
public:
	CPmButtonStarter() : CTimer(EPriorityIdle) { CActiveScheduler::Add(this); }
	TInt Construct();
private:
	void RunL() { StartButton(); }     // (once)
	};

TInt CPmButtonStarter::Construct()
	{
	TRAPD(err, ConstructL());
	return err;
	}

class CPmButtonRecognizer : public CApaFileRecognizerType
	{
public:
	CPmButtonRecognizer() {}
	~CPmButtonRecognizer() { delete iStarter; }   // (deleting a CTimer cancels it)
	void Start();
private:
	TThreadId RunL(TApaCommand aCommand, const TDesC* aDocFileName, const TDesC8* aTailEnd) const;
	TRecognizedType DoRecognizeFileL(RFs& aFs, TUidType aUidType);
private:
	CPmButtonStarter* iStarter;
	};

void CPmButtonRecognizer::Start()
	{
	iStarter = new CPmButtonStarter;
	if (!iStarter)
		return;
	if (iStarter->Construct() != KErrNone)
		{
		delete iStarter;
		iStarter = NULL;
		return;
		}
	iStarter->After(KStartDelay);
	}

CApaFileRecognizerType::TRecognizedType CPmButtonRecognizer::DoRecognizeFileL(RFs& /*aFs*/, TUidType /*aUidType*/)
	{
	return ENotRecognized;          // (no file is ours)
	}

TThreadId CPmButtonRecognizer::RunL(TApaCommand /*aCommand*/, const TDesC* /*aDocFileName*/, const TDesC8* /*aTailEnd*/) const
	{
	// never called: apparc runs only a file its recogniser recognised
	TThreadId none;
	Mem::FillZ(&none, sizeof(none));
	User::Leave(KErrNotSupported);
	return none;
	}

EXPORT_C CApaFileRecognizerType* CreateRecognizer()
	{
	CPmButtonRecognizer* rec = new CPmButtonRecognizer;
	if (rec)
		rec->Start();
	return rec;                     // (NULL, out of memory: apparc closes the library)
	}

GLDEF_C TInt E32Dll(TDllReason /*aReason*/)
	{
	return KErrNone;
	}
