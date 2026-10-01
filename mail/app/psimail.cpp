// PSIMAIL.CPP - PsiMail.app: screen, keyboard, pen, menus and settings for
// the mail engine (psimail.exe). See ../psimail.h and ../engine/store.c for
// the files it reads.
//
// An EPOC app is a DLL: no writable static data here - everything lives in
// the view or the app UI.

#include <e32keys.h>
#include <e32hal.h>
#include <eikchlst.h>
#include <eikedwin.h>
#include <eiklabel.h>
#include <eikimage.h>
#include <eikmfne.h>
#include <eikseced.h>
#include <eikcfdlg.h>
#include <eikon.rsg>
#include <apgcli.h>
#include <txtetext.h>
#include <apacmdln.h>
#include <apgtask.h>
#include <eikdll.h>
#include <eikrted.h>
#include <eikgted.h>
#include <txtrich.h>
#include <eiktbar.h>
#include <eikcmbut.h>
#include "pmicons.h"
#include "pmapp.h"
#include "pmcontacts.h"
#include "psilink.h"
#include "pglinktest.h"

// how many of the newest messages to download ahead after a sync
static TInt PrefetchCount(const TPmSettings& aSettings)
	{
	if (aSettings.iPrefetch < 0)
		return 0;
	if (aSettings.iPrefetch == 0)
		return 10;                           // the default
	return aSettings.iPrefetch > 50 ? 50 : aSettings.iPrefetch;
	}

// The link settings (speed, flow control, modem or Psion Internet, the PPP
// start command) are shared with PsiTerm and PsiWeb in PsiLink.ini. The
// start command lives only there: TPmSettings is saved as a whole struct,
// so it can't grow without losing everyone's accounts.
static void UseSharedLink(RFs& aFs, TPmSettings& aSettings, TDes* aPppStart)
	{
	TPsiLink link;
	if (!link.Load(aFs))
		{
		link.SetDefaults();
		if (aPppStart)
			*aPppStart = link.iPppStart;
		return;
		}
	aSettings.iBaudIndex = link.iBaudIndex;
	aSettings.iRtsCts = link.iRtsCts;
	aSettings.iNetMode = link.iNetMode;
	if (aPppStart)
		*aPppStart = link.iPppStart;
	}

static void SaveSharedLink(RFs& aFs, const TPmSettings& aSettings, const TDesC& aPppStart)
	{
	TPsiLink link, old;
	link.iBaudIndex = aSettings.iBaudIndex;
	link.iRtsCts = aSettings.iRtsCts ? 1 : 0;
	link.iNetMode = aSettings.iNetMode ? 1 : 0;
	link.iPppStart = aPppStart.Left(aPppStart.Length() < 40 ? aPppStart.Length() : 40);   // Left(n) panics if n > length
	if (old.Load(aFs) && old.iBaudIndex == link.iBaudIndex && old.iRtsCts == link.iRtsCts
		&& old.iNetMode == link.iNetMode && old.iPppStart == link.iPppStart)
		return;
	link.Save(aFs);
	}

_LIT(KEngineExe, "psimail.exe");
_LIT(KIniFile, "C:\\System\\Apps\\PsiMail\\PsiMail.ini");
_LIT(KVersion, "0.72");          // also pkg/psimail.pkg
const TInt KTick = 250000;       // look at the engine 4 times a second
const TUint32 KIniMagic = 0x314d5350;   // 'PSM1'

// ============================================================================
// Little helpers
// ============================================================================

static TUint32 Fnv(const TDesC8& aText);     // (below: Paths and files)

static void CopyToC(char* aDst, TInt aMax, const TDesC& aSrc)
	{
	TInt n = aSrc.Length() < aMax - 1 ? aSrc.Length() : aMax - 1;
	Mem::Copy(aDst, aSrc.Ptr(), n);
	aDst[n] = 0;
	}

static void FromC(TDes& aDst, const char* aSrc)
	{
	TPtrC8 p((const TUint8*)aSrc);
	aDst.Copy(p.Left(p.Length() < aDst.MaxLength() ? p.Length() : aDst.MaxLength()));
	}

// the next tab-separated field of aLine
static TPtrC NextField(TPtrC& aLine)
	{
	TInt t = aLine.Locate('\t');
	if (t < 0)
		{
		TPtrC f = aLine;
		aLine.Set(KNullDesC);
		return f;
		}
	TPtrC f = aLine.Left(t);
	aLine.Set(aLine.Mid(t + 1));
	return f;
	}

static TInt ToInt(const TDesC& aText)
	{
	TLex lex(aText);
	TInt v = 0;
	if (aText.Length() && aText[0] == '-')
		{
		lex.Inc();
		lex.Val(v);
		return -v;
		}
	TUint u = 0;
	lex.Val(u);
	return (TInt)u;
	}

static void SafeCopy(TDes& aDst, const TDesC& aSrc)
	{
	aDst.Copy(aSrc.Left(aSrc.Length() < aDst.MaxLength() ? aSrc.Length() : aDst.MaxLength()));
	}

// "Name <a@b>" -> "Name"; "a@b" -> "a@b"
static void DisplayName(TDes& aOut, const TDesC& aAddr)
	{
	TInt lt = aAddr.Locate('<');
	if (lt > 0)
		{
		TPtrC n = aAddr.Left(lt);
		TInt e = n.Length();
		while (e > 0 && n[e - 1] == ' ') e--;
		TInt s = 0;
		if (e > 1 && n[0] == '"' && n[e - 1] == '"') { s = 1; e--; }
		SafeCopy(aOut, n.Mid(s, e - s));
		}
	else
		SafeCopy(aOut, aAddr);
	}

// just the address from "Name <a@b>"
static void AddressOnly(TDes& aOut, const TDesC& aAddr)
	{
	TInt lt = aAddr.LocateReverse('<');
	TInt gt = aAddr.LocateReverse('>');
	if (lt >= 0 && gt > lt)
		SafeCopy(aOut, aAddr.Mid(lt + 1, gt - lt - 1));
	else
		{
		SafeCopy(aOut, aAddr);
		aOut.Trim();
		}
	}

// ============================================================================
// Draft
// ============================================================================

CPmDraft* CPmDraft::NewL()
	{
	CPmDraft* d = new(ELeave) CPmDraft;
	CleanupStack::PushL(d);
	d->iAttach = new(ELeave) CDesCArrayFlat(4);
	d->iBody = HBufC::NewL(16);
	CleanupStack::Pop();
	return d;
	}

CPmDraft::~CPmDraft()
	{
	delete iAttach;
	delete iBody;
	}

// ============================================================================
// Engine watcher
// ============================================================================

CPmWatcher::CPmWatcher(CPmView& aView)
	: CActive(EPriorityStandard), iView(aView)
	{
	CActiveScheduler::Add(this);
	}

CPmWatcher::~CPmWatcher()
	{
	Cancel();
	}

void CPmWatcher::Watch(RProcess& aProcess)
	{
	iProcess = &aProcess;
	aProcess.Logon(iStatus);
	SetActive();
	}

void CPmWatcher::RunL()
	{
	iView.EngineEnded();
	}

void CPmWatcher::DoCancel()
	{
	if (iProcess)
		iProcess->LogonCancel(iStatus);
	}

// ============================================================================
// View: set up, the engine
// ============================================================================

CPmView::~CPmView()
	{
	delete iFwdFiles;
	StopEngine();
	DestroyNative();
	delete iCalSync;
	calm_free(&iCalModel);
	delete iTimer;
	delete iCheckTimer;
	delete iWatcher;
	delete iFolders;
	delete iRows;
	delete iText;
	delete iAttNames;
	delete iAttSizes;
	delete iAttParts;
	if (iChunkOpen)
		iChunk.Close();
	}

// the calendar model's memory (see ui/pmui.h)
void* ui_alloc(int aSize) { return User::Alloc(aSize); }
void ui_free(void* aPtr) { User::Free(aPtr); }

void CPmView::ConstructL(const TRect& aRect, TPmSettings& aSettings, TPmCalSettings& aCal)
	{
	iSettings = &aSettings;
	iCal = &aCal;
	iCalSync = CPmCalSync::NewL(*this);
	// the mailbox, the reader and the calendar are EIKON controls and drawing
	// (pmnative.cpp, pmcalview.cpp)
	CreateWindowL();
	SetRectL(aRect);
	EnableDragEvents();
	iFolders = new(ELeave) CArrayFixFlat<TPmFolder>(16);
	iRows = new(ELeave) CArrayFixFlat<TPmRow>(32);
	iAttNames = new(ELeave) CDesCArrayFlat(4);
	iAttSizes = new(ELeave) CDesCArrayFlat(4);
	iAttParts = new(ELeave) CDesC8ArrayFlat(4);

	TInt r = iChunk.CreateGlobal(_L(PSI_SHARED_NAME), sizeof(PmShared), sizeof(PmShared));
	if (r == KErrAlreadyExists)
		r = iChunk.OpenGlobal(_L(PSI_SHARED_NAME), EFalse);
	User::LeaveIfError(r);
	iChunkOpen = ETrue;
	iShared = (PmShared*)iChunk.Base();
	Mem::FillZ(iShared, sizeof(PmShared));

	CreateNativeL();
	iNativeMode = (TMode)-1;
	calm_init(&iCalModel);
	iFolder = _L8("INBOX");
	if (!iSettings->iAccounts[iSettings->iAcct].used)
		iMode = ENoAccount;
	// the frame, toolbar and headings go up first; the engine, the lists
	// and the calendar follow from the first tick (FinishStartL), so the
	// window is on the screen while they load
	iStartPending = ETrue;
	iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
	iTimer->Start(KTick / 5, KTick, TCallBack(TickCallback, this));
	ActivateL();
	}

// The rest of starting up, once the window is drawn: the engine (started
// at the app's own priority, and raised once it is up - see TickL), the
// folders and the Inbox, and the calendar.
void CPmView::FinishStartL()
	{
	iStartPending = EFalse;
	AccountChangedL();                        // the folders and the list, from the card
	TRAPD(re, ReopenL());                     // where the last session was closed
	(void)re;
	DrawNow();
	StartEngineL();
	TRAPD(err, LoadCalendarL());
	(void)err;
	Render();
	AutoSettingsChanged();                    // the timed check, if it is on (pmauto.cpp)
	((CPmAppUi*)iEikonEnv->EikAppUi())->EmailButtonSoon();
	}

// Where PsiMail is, for the next start (the style guide: a program reopens
// what was in use when it was closed). Kept in a settings word: the folder's
// name hash with 1 in the low bits, 2 the Outbox, 3 the calendar, 0 the Inbox.
TInt CPmView::WhereToken() const
	{
	if (iMode == ECalendar)
		return 3;
	if (iMode == EOutbox)
		return 2;
	if ((iMode == EList || iMode == EMessage) && !iSearch && iFolder.CompareF(_L8("INBOX")) != 0)
		return (TInt)((Fnv(iFolder) & ~3u) | 1);
	return 0;
	}

// ... and back there at the start (nothing is fetched for it: the first
// check for mail does that, as for the Inbox)
void CPmView::ReopenL()
	{
	TInt where = iSettings->iSpare[1];
	if (iMode != EList || !where)
		return;
	TInt kind = where & 3;
	if (kind == 3)
		{
		ShowCalendarL();
		return;
		}
	if (kind == 2)
		{
		ShowOutboxL();
		return;
		}
	for (TInt i = 0; i < iFolders->Count(); i++)
		{
		const TPmFolder& f = (*iFolders)[i];
		if ((TInt)(Fnv(f.iImap) & ~3u) == (where & ~3) && f.iKind != 'N')
			{
			iFolder = f.iImap;
			iFolderSel = i;
			iSel = 0;
			LoadListL();
			Render();
			return;
			}
		}
	}

void CPmView::StoreDir(TDes& aDir) const
	{
	FromC(aDir, iShared->store_dir);
	}

void CPmView::CopySettingsToShared()
	{
	PmShared* s = iShared;
	TBuf<40> ppp;
	UseSharedLink(iCoeEnv->FsSession(), *iSettings, &ppp);   // may have changed in another app
	s->net.baud_index = iSettings->iBaudIndex;
	s->net.rtscts = iSettings->iRtsCts;
	s->net.net_mode = iSettings->iNetMode;
	CopyToC(s->net.ppp_start, sizeof(s->net.ppp_start), ppp);
	s->offline = iSettings->iOffline;
	s->prefetch = PrefetchCount(*iSettings);
	CopyToC(s->net.version, sizeof(s->net.version), KVersion);
	Mem::Copy(s->acct, iSettings->iAccounts, sizeof(s->acct));
	Mem::Copy(&s->cal, &iCal->iCal, sizeof(s->cal));
	if (!s->cal.host[0])
		{
		// not given: "caldav." and the account's domain
		const PmAccount& ca = iSettings->iAccounts[iSettings->iAcct];
		const char* at = ca.email;
		while (*at && *at != '@') at++;
		if (*at && at[1])
			{
			TBuf<64> h(_L("caldav."));
			TPtrC8 d((const TUint8*)at + 1);
			TBuf<60> d16;
			d16.Copy(d.Left(d.Length() < 55 ? d.Length() : 55));
			h.Append(d16);
			CopyToC(s->cal.host, sizeof(s->cal.host), h);
			}
		}
	if (s->cal.acct < 0 || s->cal.acct >= PM_MAX_ACCOUNTS || !s->acct[s->cal.acct].used)
		s->cal.acct = iSettings->iAcct;
	s->acct_seq++;
	// the mail goes on the CF card (D:) if there is one
	TVolumeInfo vol;
	TBool card = iSettings->iStore == 0 && iCoeEnv->FsSession().Volume(vol, EDriveD) == KErrNone;
	const char* root = card ? "D:\\PsiMail\\" : "C:\\PsiMail\\";
	TInt i = 0;
	while (root[i]) { s->store_dir[i] = root[i]; i++; }
	s->store_dir[i] = 0;
	TBuf<96> att;
	att.Copy(TPtrC8((const TUint8*)root));
	att.Append(_L("Attachments\\"));
	CopyToC(s->attach_dir, sizeof(s->attach_dir), att);
	TBuf<100> dir;
	StoreDir(dir);
	iCoeEnv->FsSession().MkDirAll(dir);
	}

void CPmView::StartEngineL()
	{
	if (iRunning)
		return;
	PmShared* s = iShared;
	Mem::FillZ(s, sizeof(PmShared));
	s->magic = PM_MAGIC;
	s->net.magic = PSI_SHARED_MAGIC;
	s->net.port = 993;
	s->net.dial_prefix[0] = 'A'; s->net.dial_prefix[1] = 'T';
	s->net.dial_prefix[2] = 'D'; s->net.dial_prefix[3] = 'T'; s->net.dial_prefix[4] = 0;
	CopySettingsToShared();
	iDoneSeen = 0;

	// the engine lives next to the app
	TParse parse;
	parse.Set(CEikonEnv::Static()->EikAppUi()->Application()->AppFullName(), NULL, NULL);
	TFileName dir(parse.DriveAndPath());
	TPtrC home = dir.Left(dir.Length() - 1);      // without the last '\'
	CopyToC(s->net.home, sizeof(s->net.home), home);
	TFileName exe(dir);
	exe.Append(KEngineExe);
	TInt r = iProcess.Create(exe, KNullDesC);
	if (r != KErrNone)
		{
		TBuf<80> e;
		e.Format(_L("psimail.exe did not start (%d)"), r);
		iEikonEnv->InfoWinL(_L("Could not start the mail engine"), e);
		return;
		}
	iRunning = ETrue;
	if (!iWatcher)
		iWatcher = new(ELeave) CPmWatcher(*this);
	iWatcher->Watch(iProcess);
	// It runs above this (foreground) app once it is up: it must drain the
	// serial port while the screen is being drawn, or bytes are lost without
	// RTS/CTS. While it starts (loading, its own set-up) it stays below the
	// app, so the screen gets drawn first; TickL raises it when it is ready.
	iProcess.SetPriority(EPriorityBackground);
	iEngineLow = ETrue;
	iProcess.Resume();
	if (iFirstFetch)
		{
		// the account's first time (see AccountChangedL): the folders and the Inbox
		iFirstFetch = EFalse;
		Cmd(PM_CMD_FOLDERS, KNullDesC8, 0, KNullDesC8);
		Cmd(PM_CMD_SYNC, _L8("INBOX"), 0, KNullDesC8);
		}
	}

void CPmView::StopEngine()
	{
	if (!iRunning)
		return;
	iShared->quitting = 1;
	iShared->net.quit = 1;
	// give it a few seconds to log out and hang up
	for (TInt i = 0; i < 60 && iShared->state != PM_STATE_EXITED; i++)
		User::After(100000);
	if (iWatcher)
		iWatcher->Cancel();
	if (iShared->state != PM_STATE_EXITED)
		{
		iProcess.Kill(0);
		PsiLinkTimersBack();             // it never got to give NIFMAN its timers back
		}
	iProcess.Close();
	iRunning = EFalse;
	}

void CPmView::EngineEnded()
	{
	iCalPending = EFalse;
	TExitType type = iProcess.ExitType();
	TInt reason = iProcess.ExitReason();
	TExitCategoryName cat = iProcess.ExitCategory();
	iProcess.Close();
	iRunning = EFalse;
	if (iShared->quitting)
		return;
	TBuf<120> why;
	if (type == EExitPanic)
		PsiLinkTimersBack();             // (0.68) the crash skipped the engine's own clean-up
	if (type == EExitPanic)
		why.Format(_L("The mail engine stopped (%S %d) - use Tools > Restart mail engine"), &cat, reason);
	else
		why.Format(_L("The mail engine closed (%d) - use Tools > Restart mail engine"), reason);
	SetStatus(why);
	}

void CPmView::SettingsChanged()
	{
	CopySettingsToShared();
	iReaderUid = 0;                          // (the pictures preference may have changed)
	Render();
	}

TBool CPmView::Busy() const
	{
	return iShared->busy || iShared->cmd_head != iShared->cmd_tail;
	}

// The engine is busy with a download ahead (pf_step: busy with no command
// of ours running). It steps aside by itself for a command we queue, so
// there is nothing for Esc to stop - stopping it would only hang up the
// line, and the next command would dial again.
TBool CPmView::DownloadingAhead() const
	{
	return iShared->busy && iShared->cur_op == PM_CMD_NONE;
	}

// Is a command of this kind running, or waiting in the queue?
TBool CPmView::OpInFlight(TInt aOp) const
	{
	PmShared* s = iShared;
	if (s->busy && s->cur_op == aOp)
		return ETrue;
	for (TUint i = s->cmd_tail; i != s->cmd_head; i++)
		if (iSent[i % PM_CMDQ].op == aOp)
			return ETrue;
	return EFalse;
	}

// Asks the engine to stop what it is doing (a dial-up that will not come up,
// a download). The engine notices within a quarter of a second, even while
// it waits for the Psion's Internet connection, and the command ends with
// "Stopped". (The engine clears net.quit after each command, so a command
// already queued behind it still runs.)
void CPmView::StopEngineWork(const TDesC& aToast)
	{
	iShared->net.quit = 1;
	if (aToast.Length())
		Working(aToast);
	}

void CPmView::Cmd(TInt aOp, const TDesC8& aFolder, TUint aUid, const TDesC8& aArg)
	{
	PmShared* s = iShared;
	if (!iRunning)
		{
		Toast(_L("The mail engine is not running"));
		return;
		}
	if (s->cmd_head - s->cmd_tail >= PM_CMDQ)
		{
		Toast(_L("Not available at this time"));
		return;
		}
	PmCmd& c = s->cmd[s->cmd_head % PM_CMDQ];
	Mem::FillZ(&c, sizeof(c));
	c.op = aOp;
	c.acct = iSettings->iAcct;
	c.uid = aUid;
	TInt n = aFolder.Length() < (TInt)sizeof(c.folder) - 1 ? aFolder.Length() : sizeof(c.folder) - 1;
	Mem::Copy(c.folder, aFolder.Ptr(), n);
	n = aArg.Length() < PM_ARG_MAX - 1 ? aArg.Length() : PM_ARG_MAX - 1;
	Mem::Copy(c.arg, aArg.Ptr(), n);
	iSent[s->cmd_head % PM_CMDQ] = c;
	s->cmd_head++;
	Render();
	}

// ============================================================================
// Paths and files
// ============================================================================

static TUint32 Fnv(const TDesC8& aText)
	{
	TUint32 h = 2166136261u;
	for (TInt i = 0; i < aText.Length(); i++)
		{
		h ^= aText[i];
		h *= 16777619u;
		}
	return h;
	}

void CPmView::FolderDir(const TDesC8& aImap, TDes& aDir) const
	{
	StoreDir(aDir);
	aDir.AppendFormat(_L("A%d\\F%08X\\"), iSettings->iAcct, Fnv(aImap));
	}

void CPmView::OutboxDir(TDes& aDir) const
	{
	StoreDir(aDir);
	aDir.AppendFormat(_L("A%d\\outbox\\"), iSettings->iAcct);
	}

void CPmView::MsgPath(TUint aUid, const TDesC& aExt, TDes& aPath) const
	{
	FolderDir(iFolder, aPath);
	aPath.AppendNum(aUid);
	aPath.Append('.');
	aPath.Append(aExt);
	}

// Reads a whole file (up to aMax bytes). aBuf is NULL if it isn't there.
void CPmView::ReadFileL(const TDesC& aName, HBufC*& aBuf, TInt aMax)
	{
	aBuf = NULL;
	RFile f;
	if (f.Open(iCoeEnv->FsSession(), aName, EFileRead | EFileShareReadersOnly) != KErrNone)
		return;
	TInt size = 0;
	f.Size(size);
	if (size > aMax)
		size = aMax;
	HBufC* b = HBufC::New(size + 1);
	if (!b)
		{
		f.Close();
		User::Leave(KErrNoMemory);
		}
	TPtr p = b->Des();
	TInt r = f.Read(p, size);
	f.Close();
	if (r != KErrNone)
		{
		delete b;
		return;
		}
	aBuf = b;
	}

// ============================================================================
// Loading what to show
// ============================================================================

void CPmView::AccountChangedL()
	{
	CopySettingsToShared();
	iFolder = _L8("INBOX");
	iSearch = EFalse;
	PmAccount& a = iSettings->iAccounts[iSettings->iAcct];
	if (!a.used)
		{
		iFolders->Reset();                    // (not the last account's folders)
		iRows->Reset();
		iMode = ENoAccount;
		iSidebar = EFalse;
		Render();
		return;
		}
	LoadFoldersL();
	iMode = EList;
	iListMode = EList;
	iSel = 0;
	LoadListL();
	// first time for this account: fetch the folders and the Inbox (once the
	// engine is up, if it isn't yet: StartEngineL)
	if (iFolders->Count() == 0 && !iSettings->iOffline)
		{
		if (iRunning)
			{
			Cmd(PM_CMD_FOLDERS, KNullDesC8, 0, KNullDesC8);
			Cmd(PM_CMD_SYNC, _L8("INBOX"), 0, KNullDesC8);
			}
		else
			iFirstFetch = ETrue;
		}
	Render();
	}

void CPmView::LoadFoldersL()
	{
	iFolders->Reset();
	TBuf<120> path;
	StoreDir(path);
	path.AppendFormat(_L("A%d\\folders.txt"), iSettings->iAcct);
	HBufC* buf = NULL;
	ReadFileL(path, buf, 64 * 1024);
	if (!buf)
		return;
	CleanupStack::PushL(buf);
	TPtrC rest = *buf;
	while (rest.Length())
		{
		TInt nl = rest.Locate('\n');
		TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
		if (line.Length() && line[line.Length() - 1] == '\r')
			line.Set(line.Left(line.Length() - 1));
		if (line.Length() < 3 || line[0] == '#')
			continue;
		TPmFolder f;
		TPtrC l = line;
		TPtrC kind = NextField(l);
		f.iKind = kind.Length() ? kind[0] : '-';
		f.iUnread = ToInt(NextField(l));
		f.iTotal = ToInt(NextField(l));
		TPtrC imap = NextField(l);
		if (!imap.Length())
			continue;                    // a damaged line: not a folder
		SafeCopy(f.iImap, imap);
		SafeCopy(f.iName, NextField(l));
		iFolders->AppendL(f);
		}
	CleanupStack::PopAndDestroy();
	}

TInt CPmView::FolderCount() const { return iFolders->Count(); }
const TPmFolder& CPmView::FolderAt(TInt aIndex) const { return (*iFolders)[aIndex]; }

const TPmFolder* CPmView::CurrentFolder() const
	{
	for (TInt i = 0; i < iFolders->Count(); i++)
		if ((*iFolders)[i].iImap == iFolder)
			return &(*iFolders)[i];
	return NULL;
	}

// The folder File > Folder acts on: the highlighted one while the keys are
// in the folder tree (none for the Outbox and the Calendar), otherwise the
// open folder.
const TPmFolder* CPmView::CommandFolder() const
	{
	if (iSidebar && (iMode == EList || iMode == EOutbox || iMode == ECalendar))
		return iFolderSel >= 0 && iFolderSel < iFolders->Count() ? &(*iFolders)[iFolderSel] : NULL;
	if (iMode == EList || iMode == EMessage)
		return CurrentFolder();
	return NULL;
	}

// The engine made, renamed or deleted a folder (and rewrote folders.txt):
// the tree follows, and so does the open folder if it was the one
void CPmView::FolderChangedL(const PmCmd& aCmd)
	{
	TBuf8<128> was;
	was.Copy(TPtrC8((const TUint8*)aCmd.folder));
	TBuf8<128> now;
	now.Copy(TPtrC8((const TUint8*)iShared->last_file));
	LoadFoldersL();
	if (aCmd.op == PM_CMD_MKFOLDER)
		{
		// straight into the new folder (empty, and checked with the server)
		if (now.Length())
			OpenFolderL(now);
		return;
		}
	TBool open = (iMode == EList || iMode == EMessage) && iFolder == was;
	if (aCmd.op == PM_CMD_RENFOLDER)
		{
		if (open && now.Length())
			{
			iFolder = now;                        // the same files, under their new name
			if (iMode == EMessage) { iMode = EList; iSidebar = EFalse; }
			}
		}
	else if (aCmd.op == PM_CMD_DELFOLDER && open)
		{
		iFolder = _L8("INBOX");
		iSearch = EFalse;
		iMode = EList;
		iListMode = EList;
		iSel = 0;
		}
	if (iSidebar)
		FocusFoldersL();                          // (the highlight, now the tree has changed)
	ReloadL();
	}

void CPmView::LoadListL()
	{
	TUint keep = (iSel >= 0 && iSel < iRows->Count()) ? (*iRows)[iSel].iUid : 0;
	iRows->Reset();
	TBuf<140> path;
	FolderDir(iFolder, path);
	path.Append(iSearch ? _L("search.txt") : _L("index.txt"));
	HBufC* buf = NULL;
	ReadFileL(path, buf, 400 * 1024);
	if (buf)
		{
		CleanupStack::PushL(buf);
		TPtrC rest = *buf;
		while (rest.Length())
			{
			TInt nl = rest.Locate('\n');
			TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
			rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
			if (line.Length() < 3 || line[0] == '#')
				continue;
			TPmRow row;
			TPtrC l = line;
			row.iUid = (TUint)ToInt(NextField(l));
			SafeCopy(row.iFlags, NextField(l));
			row.iDate = ToInt(NextField(l));
			row.iSize = ToInt(NextField(l));
			TPtrC from = NextField(l);
			if (iFolders && CurrentFolder() && (CurrentFolder()->iKind == 'S' || CurrentFolder()->iKind == 'D'))
				{
				// in Sent and Drafts, show who it went to
				TPtrC subj = NextField(l);
				TPtrC to = NextField(l);
				TBuf<64> n;
				DisplayName(n, to.Length() ? to : from);
				row.iFrom = _L("To ");
				row.iFrom.Append(n.Left(n.Length() < 61 ? n.Length() : 61));
				SafeCopy(row.iSubject, subj);
				}
			else
				{
				DisplayName(row.iFrom, from);
				SafeCopy(row.iSubject, NextField(l));
				if (row.iFrom.Locate('@') >= 0)
					{
					// only an address: the name from Contacts, when they have been read
					CPmContacts* c = ((CPmAppUi*)iEikonEnv->EikAppUi())->Contacts();
					TBuf<64> n;
					if (c && c->Loaded() && c->NameFor(row.iFrom, n))
						row.iFrom = n;
					}
				}
			iRows->AppendL(row);
			}
		CleanupStack::PopAndDestroy();
		// the file is oldest first; show the newest first
		TInt n = iRows->Count();
		for (TInt i = 0; i < n / 2; i++)
			{
			TPmRow t = (*iRows)[i];
			(*iRows)[i] = (*iRows)[n - 1 - i];
			(*iRows)[n - 1 - i] = t;
			}
		SortRows();
		}
	iSel = 0;
	for (TInt i = 0; i < iRows->Count(); i++)
		if ((*iRows)[i].iUid == keep) { iSel = i; break; }
	EnsureVisible();
	}

void CPmView::LoadOutboxL()
	{
	TUint keep = (iSel >= 0 && iSel < iRows->Count()) ? (*iRows)[iSel].iUid : 0;
	iRows->Reset();
	TBuf<120> dir;
	OutboxDir(dir);
	TBuf<130> spec(dir);
	spec.Append(_L("*.txt"));
	CDir* list = NULL;
	if (iCoeEnv->FsSession().GetDir(spec, KEntryAttNormal, ESortByName, list) == KErrNone && list)
		{
		CleanupStack::PushL(list);
		for (TInt i = 0; i < list->Count(); i++)
			{
			const TEntry& e = (*list)[i];
			TPmRow row;
			row.iUid = (TUint)ToInt(e.iName);
			row.iFlags = _L("S");
			row.iSize = e.iSize;
			row.iDate = 0;
			TFileName path(dir);
			path.Append(e.iName);
			HBufC* buf = NULL;
			ReadFileL(path, buf, 4096);
			if (buf)
				{
				TPtrC rest = *buf;
				while (rest.Length())
					{
					TInt nl = rest.Locate('\n');
					TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
					rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
					if (line.Length() == 0)
						break;
					if (line.Find(_L("To: ")) == 0)
						{
						TBuf<64> n;
						DisplayName(n, line.Mid(4));
						row.iFrom = _L("To ");
						row.iFrom.Append(n.Left(n.Length() < 61 ? n.Length() : 61));
						}
					else if (line.Find(_L("Subject: ")) == 0)
						SafeCopy(row.iSubject, line.Mid(9));
					else if (line.Find(_L("Draft: 1")) == 0)
						row.iFlags.Append('D');
					}
				delete buf;
				}
			TFileName err(dir);
			err.AppendNum(row.iUid);
			err.Append(_L(".err"));
			TEntry ee;
			if (iCoeEnv->FsSession().Entry(err, ee) == KErrNone)
				row.iFlags.Append('E');
			iRows->AppendL(row);
			}
		CleanupStack::PopAndDestroy();
		}
	iSel = 0;
	for (TInt i = 0; i < iRows->Count(); i++)
		if ((*iRows)[i].iUid == keep) { iSel = i; break; }
	EnsureVisible();
	}

TInt CPmView::OutboxCount()
	{
	TBuf<120> dir;
	OutboxDir(dir);
	dir.Append(_L("*.txt"));
	CDir* list = NULL;
	TInt n = 0;
	if (iCoeEnv->FsSession().GetDir(dir, KEntryAttNormal, ESortByName, list) == KErrNone && list)
		{
		n = list->Count();
		delete list;
		}
	return n;
	}

void CPmView::LoadMessageL()
	{
	delete iText;
	iText = NULL;
	iAttNames->Reset();
	iAttSizes->Reset();
	iAttParts->Reset();
	iTruncated = 0;
	iBodyOff = 0;
	TBuf<150> path;
	MsgPath(iMsgUid, _L("htm"), path);
	TEntry he;
	iHtml = iCoeEnv->FsSession().Entry(path, he) == KErrNone;
	MsgPath(iMsgUid, _L("txt"), path);
	HBufC* buf = NULL;
	ReadFileL(path, buf, 300 * 1024);
	iWaitingBody = buf == NULL;
	iText = buf;
	if (!iText)
		return;
	// "#PSIMAIL1 TAB bytes-not-downloaded TAB html" first
	TPtrC t = *iText;
	if (t.Length() && t[0] == '#')
		{
		TInt nl = t.Locate('\n');
		TPtrC l = nl >= 0 ? t.Left(nl) : t;
		NextField(l);
		iTruncated = ToInt(NextField(l));
		iBodyOff = nl >= 0 ? nl + 1 : t.Length();
		}
	// the attachments
	MsgPath(iMsgUid, _L("att"), path);
	HBufC* att = NULL;
	ReadFileL(path, att, 16 * 1024);
	if (att)
		{
		CleanupStack::PushL(att);
		TPtrC rest = *att;
		while (rest.Length() && iAttNames->Count() < 8)
			{
			TInt nl = rest.Locate('\n');
			TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
			rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
			if (!line.Length())
				continue;
			TPtrC l = line;
			TPtrC part = NextField(l);
			TInt size = ToInt(NextField(l));
			TPtrC name = NextField(l);
			iAttNames->AppendL(name.Left(name.Length() < 90 ? name.Length() : 90));
			// base64 is 4/3 of the file
			TInt kb = (size * 3 / 4 + 1023) / 1024;
			TBuf<16> z;
			if (kb >= 1024) z.Format(_L("%d.%d MB"), kb / 1024, (kb % 1024) * 10 / 1024);
			else z.Format(_L("%d KB"), kb);
			iAttSizes->AppendL(z);
			TBuf8<16> p8;
			SafeCopy(p8, part);
			iAttParts->AppendL(p8);
			}
		CleanupStack::PopAndDestroy();
		}
	}

void CPmView::ReloadL()
	{
	switch (iMode)
		{
	case EList:
		LoadFoldersL();
		LoadListL();
		break;
	case EOutbox:
		LoadOutboxL();
		break;
	case ECalendar:
		LoadFoldersL();
		LoadCalendarL();
		break;
	case EMessage:
		LoadFoldersL();
		if (iListMode == EList)
			LoadListL();
		if (iWaitingBody)
			{
			TBuf<150> path;
			MsgPath(iMsgUid, _L("txt"), path);
			TEntry e;
			if (iCoeEnv->FsSession().Entry(path, e) == KErrNone)
				LoadMessageL();
			}
		break;
	default:
		break;
		}
	Render();
	}

// ============================================================================
// Navigation
// ============================================================================

void CPmView::FocusFoldersL()
	{
	if (iMode == EMessage)
		BackL();
	if (iMode == ECalendar)
		{
		iSidebar = ETrue;
		iFolderSel = iFolders->Count() + 1;
		Render();
		return;
		}
	if (iMode != EList && iMode != EOutbox)
		return;
	iSidebar = ETrue;
	// the open folder is the highlighted one
	iFolderSel = iFolders->Count();                // (the outbox)
	if (iMode == EList)
		for (TInt i = 0; i < iFolders->Count(); i++)
			if ((*iFolders)[i].iImap == iFolder) iFolderSel = i;
	Render();
	}

void CPmView::OpenFolderL(const TDesC8& aImap)
	{
	iFolder = aImap;
	iSearch = EFalse;
	iMode = EList;
	iListMode = EList;
	iSidebar = EFalse;
	iSel = 0;
	iRows->Reset();
	LoadListL();
	iSel = 0;
	for (TInt i = 0; i < iFolders->Count(); i++)
		if ((*iFolders)[i].iImap == iFolder) iFolderSel = i;
	Render();
	// always look for changes when a folder is opened
	if (!iSettings->iOffline)
		Cmd(PM_CMD_SYNC, iFolder, 0, KNullDesC8);
	}

void CPmView::ShowOutboxL()
	{
	iMode = EOutbox;
	iListMode = EOutbox;
	iSidebar = EFalse;
	iSearch = EFalse;
	iFolderSel = iFolders->Count();
	iSel = 0;
	LoadOutboxL();
	Render();
	}

const TPmRow* CPmView::CurrentRow() const
	{
	if ((iMode == EList || iMode == EOutbox || iMode == EMessage) && iSel >= 0 && iSel < iRows->Count())
		return &(*iRows)[iSel];
	return NULL;
	}

void CPmView::OpenCurrentL()
	{
	if (iSidebar)
		{
		if (iFolderSel > iFolders->Count())
			ShowCalendarL();
		else if (iFolderSel == iFolders->Count())
			ShowOutboxL();
		else if ((*iFolders)[iFolderSel].iKind == 'N')
			Toast(_L("That folder holds only other folders"));
		else
			OpenFolderL((*iFolders)[iFolderSel].iImap);
		return;
		}
	if (iMode == EOutbox)
		{
		// edited in the app UI (compose dialog)
		iEikonEnv->EikAppUi()->HandleCommandL(EPmCmdOpen);
		return;
		}
	if (iMode != EList || iSel < 0 || iSel >= iRows->Count())
		return;
	TPmRow& row = (*iRows)[iSel];
	TBool wasUnread = row.iFlags.Locate('S') < 0;
	iMsgUid = row.iUid;
	iMode = EMessage;
	if (wasUnread)
		row.iFlags.Append('S');                  // the engine marks it read
	iBodyError.Zero();
	LoadMessageL();
	if (iWaitingBody)
		{
		if (iSettings->iOffline)
			SetStatus(_L("Not downloaded - you are working offline"));
		else
			Cmd(PM_CMD_BODY, iFolder, iMsgUid, KNullDesC8);
		}
	else if (wasUnread)
		{
		// here already (e.g. marked unread again): tell the server it's read
		Cmd(PM_CMD_FLAG, iFolder, iMsgUid, _L8("+S"));
		}
	Render();
	}

void CPmView::BackL()
	{
	if (iMode == EMessage)
		{
		iMode = iListMode;
		if (iMode == EList) LoadListL(); else LoadOutboxL();
		}
	else if (iSidebar)
		iSidebar = EFalse;
	else if (iMode == EList && iSearch)
		{
		iSearch = EFalse;
		LoadListL();
		}
	else if (iMode == EList || iMode == EOutbox)
		{
		FocusFoldersL();
		return;
		}
	Render();
	}

void CPmView::StepMessageL(TInt aDir)
	{
	if (iMode != EMessage)
		return;
	TInt s = iSel + aDir;
	if (s < 0 || s >= iRows->Count())
		{
		Toast(aDir > 0 ? _L("That was the oldest message") : _L("That was the newest message"));
		return;
		}
	iSel = s;
	iMode = EList;
	OpenCurrentL();
	}

void CPmView::DeleteCurrentL()
	{
	if (iMode == EOutbox)
		{
		DeleteOutboxL();
		return;
		}
	const TPmRow* row = CurrentRow();
	if (!row || (iMode != EList && iMode != EMessage))
		return;
	TUint uid = row->iUid;                    // (the list may reload during the query)
	const TPmFolder* f = CurrentFolder();
	TBool forGood = f && f->iKind == 'T';
	if (forGood && !iEikonEnv->QueryWinL(_L("It is in the Trash already"), _L("Delete this message for good?")))
		return;
	Cmd(PM_CMD_MOVE, iFolder, uid, KNullDesC8);
	if (!forGood)
		NoteUndoL(uid);                       // (Edit > Undo: pmundo.cpp)
	for (TInt i = 0; i < iRows->Count(); i++)
		if ((*iRows)[i].iUid == uid) { iSel = i; break; }
	if (iSel < 0 || iSel >= iRows->Count() || (*iRows)[iSel].iUid != uid)
		{
		Render();
		return;
		}
	iRows->Delete(iSel);
	if (iSel >= iRows->Count()) iSel = iRows->Count() - 1;
	if (iSel < 0) iSel = 0;
	if (iMode == EMessage)
		{
		iMode = EList;
		if (iRows->Count())
			OpenCurrentL();
		}
	EnsureVisible();
	Toast(forGood ? _L("Deleted") : _L("Moved to the Trash"));
	Render();
	}

TBool CPmView::MoveCurrentL(const TDesC8& aDest)
	{
	const TPmRow* row = CurrentRow();
	if (!row || (iMode != EList && iMode != EMessage))
		return EFalse;
	if (aDest == iFolder)
		{
		Toast(_L("It is in that folder already"));
		return EFalse;
		}
	Cmd(PM_CMD_MOVE, iFolder, row->iUid, aDest);
	NoteUndoL(row->iUid);                     // (Edit > Undo: pmundo.cpp)
	iRows->Delete(iSel);
	if (iSel >= iRows->Count()) iSel = iRows->Count() - 1;
	if (iSel < 0) iSel = 0;
	if (iMode == EMessage)
		{
		iMode = EList;
		if (iRows->Count())
			OpenCurrentL();
		}
	EnsureVisible();
	Render();
	return ETrue;
	}

void CPmView::ToggleFlagL(TChar aFlag)
	{
	if (iMode != EList && iMode != EMessage)
		return;
	if (iSel < 0 || iSel >= iRows->Count())
		return;
	TPmRow& row = (*iRows)[iSel];
	TInt at = row.iFlags.Locate(aFlag);
	TBuf8<4> op;
	op.Append(at >= 0 ? '-' : '+');
	op.Append(aFlag);
	if (at >= 0)
		row.iFlags.Delete(at, 1);
	else
		row.iFlags.Append(aFlag);
	Cmd(PM_CMD_FLAG, iFolder, row.iUid, op);
	if (aFlag == 'S')
		Toast(at >= 0 ? _L("Marked unread") : _L("Marked read"));
	else
		Toast(at >= 0 ? _L("Flag removed") : _L("Flagged"));
	Render();
	}

void CPmView::RefreshL()
	{
	if (iSidebar)
		Cmd(PM_CMD_FOLDERS, KNullDesC8, 0, KNullDesC8);
	else if (iMode == EOutbox)
		Cmd(PM_CMD_SEND, KNullDesC8, 0, KNullDesC8);
	else
		Cmd(PM_CMD_SYNC, iFolder, 0, KNullDesC8);
	}

void CPmView::OlderL()
	{
	if (iMode != EList || iSearch)
		return;
	Cmd(PM_CMD_OLDER, iFolder, 0, KNullDesC8);
	}

void CPmView::SearchL(const TDesC& aWords)
	{
	if (iMode == EOutbox || iMode == ENoAccount)
		iFolder = _L8("INBOX");
	SafeCopy(iSearchWords, aWords);
	TBuf8<PM_ARG_MAX> w;
	SafeCopy(w, aWords);
	Cmd(PM_CMD_SEARCH, iFolder, 0, w);
	}

void CPmView::SendRecvL()
	{
	Cmd(PM_CMD_SENDRECV, iMode == EList || iMode == EMessage ? TPtrC8(iFolder) : TPtrC8(_L8("INBOX")), 0, KNullDesC8);
	// the calendar sync follows once the mail part is done (HandleResultL):
	// queued alongside, it would dial again after a failed connection and
	// bring the Psion's own connection dialogs straight back
	iCalAfterMail = iCal->iCal.enabled && !iSettings->iOffline;
	}

// ----- calendar: the engine talks to the server, CPmCalSync to the Agenda

void CPmView::CalendarSyncL()
	{
	if (!iCal->iCal.enabled)
		{
		Toast(_L("Calendar sync is off - see Tools > Calendar settings"));
		return;
		}
	if (CalendarBusy())
		return;
	iCalSecond = EFalse;
	CalCmd(KNullDesC8);
	}

// one calendar sync at a time: the engine and CPmCalSync share its files
void CPmView::CalCmd(const TDesC8& aArg)
	{
	iCalPending = ETrue;
	Cmd(PM_CMD_CALSYNC, KNullDesC8, 0, aArg);
	}

void CPmView::CalProgress(const TDesC& aText)
	{
	SafeCopy(iStatus, aText);
	iStatusUntil = User::TickCount() + 64 * 30;
	if (iNativeShown && aText.Length())
		{
		TRAPD(err, iEikonEnv->BusyMsgL(aText, EHLeftVBottom, TTimeIntervalMicroSeconds32(300000)));
		(void)err;
		iBusyShown = ETrue;
		}
	Render();
	}

void CPmView::CalSyncDone(TInt aError, const TDesC& aSummary, TBool aPushed)
	{
	iStatus.Zero();
	iStatusUntil = 0;
	if (iBusyShown && !Busy())
		{
		iEikonEnv->BusyMsgCancel();
		iBusyShown = EFalse;
		}
	if (aError == KErrNone && aPushed && !iCalSecond)
		{
		// send what changed in the Agenda (the engine then fetches again)
		iCalSecond = ETrue;
		iCalMsg = aSummary;
		CalCmd(KNullDesC8);
		Render();
		return;
		}
	if (aError == KErrNone && iCal->iCopyExisting)
		{
		// the Psion's own entries have been sent once: not again
		iCal->iCopyExisting = 0;
		((CPmAppUi*)iEikonEnv->EikAppUi())->SaveCalSettings();
		}
	if (iCalSecond && aError == KErrNone && iCalMsg.Length())
		Toast(iCalMsg);                  // the first pass said what happened
	else
		Toast(aSummary);
	iCalSecond = EFalse;
	iCalMsg.Zero();
	TRAPD(err, LoadCalendarL());
	Render();
	}

void CPmView::WholeMessageL()
	{
	if (iMode != EMessage)
		return;
	if (iTruncated <= 0)
		{
		Toast(_L("You have the whole message"));
		return;
		}
	iWaitingBody = ETrue;
	iBodyError.Zero();
	Cmd(PM_CMD_FULLBODY, iFolder, iMsgUid, KNullDesC8);
	}

TInt CPmView::AttachmentCount() const
	{
	return iMode == EMessage ? iAttNames->Count() : 0;
	}

void CPmView::AttachmentsL(CDesCArray& aNames)
	{
	for (TInt i = 0; i < iAttNames->Count(); i++)
		aNames.AppendL((*iAttNames)[i]);
	}

void CPmView::SaveAttachmentL(TInt aIndex)
	{
	if (aIndex < 0 || aIndex >= iAttParts->Count())
		return;
	Cmd(PM_CMD_ATTACH, iFolder, iMsgUid, (*iAttParts)[aIndex]);
	}

// the engine's name for an attachment file (imap.c's safe_name)
static void SafeFileName(TDes& aOut, const TDesC& aName)
	{
	aOut.Zero();
	for (TInt i = 0; i < aName.Length() && aOut.Length() < aOut.MaxLength(); i++)
		{
		TText c = aName[i];
		if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c < 32)
			c = '_';
		aOut.Append(c);
		}
	while (aOut.Length() && (aOut[aOut.Length() - 1] == '.' || aOut[aOut.Length() - 1] == ' '))
		aOut.SetLength(aOut.Length() - 1);
	if (!aOut.Length())
		aOut = _L("attachment");
	}

// where opened attachments are kept: beside the message, on the mail's
// disk (D:\PsiMail\A0\F...\Open\uid\name), so opening one again is instant
static void AttachCacheDir(TDes& aDir, TUint aUid)
	{
	aDir.Append(_L("Open\\"));
	aDir.AppendNum(aUid);
	aDir.Append('\\');
	}

// Open: a copy already here opens at once; otherwise the engine fetches it
// (the part id, a tab, then where to) and HandleResultL opens it
void CPmView::OpenAttachmentL(TInt aIndex)
	{
	if (aIndex < 0 || aIndex >= iAttParts->Count())
		return;
	TBuf<200> path;
	FolderDir(iFolder, path);
	AttachCacheDir(path, iMsgUid);
	TBuf<100> name;
	SafeFileName(name, (*iAttNames)[aIndex]);
	TBuf8<PM_ARG_MAX> arg;
	arg.Copy((*iAttParts)[aIndex]);
	arg.Append('\t');
	arg.Append(Clip(path, PM_ARG_MAX - arg.Length() - 1));
	path.Append(Clip(name, path.MaxLength() - path.Length()));
	TEntry e;
	if (iCoeEnv->FsSession().Entry(path, e) == KErrNone && e.iSize > 0)
		{
		LaunchFileL(path);
		return;
		}
	Cmd(PM_CMD_ATTACH, iFolder, iMsgUid, arg);
	}

// the file in its own program: the system's recognisers pick it (Word,
// Sheet, Sketch, Record...)
void CPmView::LaunchFileL(const TDesC& aPath)
	{
	RApaLsSession ls;
	User::LeaveIfError(ls.Connect());
	CleanupClosePushL(ls);
	TThreadId id;
	TInt r = ls.StartDocument(aPath, id);
	CleanupStack::PopAndDestroy();          // ls
	if (r == KErrNone)
		{
		TParsePtrC parse(aPath);
		TBuf<100> t(_L("Opening "));
		t.Append(Clip(parse.NameAndExt(), 80));
		t.Append(_L("..."));
		Toast(t);
		return;
		}
	if (r == KErrNotFound || r == KErrNotSupported)
		iEikonEnv->InfoWinL(_L("No program opens this kind of file - it is saved at"), Clip(aPath, 120));
	else
		{
		TBuf<80> t;
		t.Format(_L("The file can't be opened (%d)"), r);
		iEikonEnv->InfoWinL(t, Clip(aPath, 120));
		}
	}

// Forward with attachments: each is fetched to the message's Open folder
// (those already there count at once); the last one in starts the forward
void CPmView::ForwardAttachmentsL(TUint aUid)
	{
	if (!iFwdFiles)
		iFwdFiles = new(ELeave) CDesCArrayFlat(4);
	iFwdFiles->Reset();
	iFwdPending = 0;
	iFwdUid = aUid;
	TBuf<200> dir;
	FolderDir(iFolder, dir);
	AttachCacheDir(dir, aUid);
	for (TInt i = 0; i < iAttParts->Count(); i++)
		{
		TBuf<100> name;
		SafeFileName(name, (*iAttNames)[i]);
		TBuf<200> path(dir);
		path.Append(Clip(name, path.MaxLength() - path.Length()));
		TEntry e;
		if (iCoeEnv->FsSession().Entry(path, e) == KErrNone && e.iSize > 0)
			{
			iFwdFiles->AppendL(path);
			continue;
			}
		TBuf8<PM_ARG_MAX> arg;
		arg.Copy((*iAttParts)[i]);
		arg.Append('\t');
		arg.Append(Clip(dir, PM_ARG_MAX - arg.Length() - 1));
		Cmd(PM_CMD_ATTACH, iFolder, aUid, arg);
		iFwdPending++;
		}
	if (iFwdPending == 0)
		((CPmAppUi*)iEikonEnv->EikAppUi())->ForwardReadyL(aUid, *iFwdFiles);
	else
		Working(_L("Getting the attachments first..."));
	}

// the From of a message in this folder's index, as the server gave it
// ("Name <addr>"): there before the message itself is downloaded
TBool CPmView::IndexFromL(TUint aUid, TDes& aFrom)
	{
	aFrom.Zero();
	TBuf<140> path;
	FolderDir(iFolder, path);
	path.Append(iSearch ? _L("search.txt") : _L("index.txt"));
	HBufC* buf = NULL;
	ReadFileL(path, buf, 400 * 1024);
	if (!buf)
		return EFalse;
	CleanupStack::PushL(buf);
	TPtrC rest = *buf;
	while (rest.Length() && !aFrom.Length())
		{
		TInt nl = rest.Locate('\n');
		TPtrC l = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
		if (l.Length() < 3 || l[0] == '#')
			continue;
		if ((TUint)ToInt(NextField(l)) != aUid)
			continue;
		NextField(l); NextField(l); NextField(l);      // flags, date, size
		SafeCopy(aFrom, NextField(l));
		}
	CleanupStack::PopAndDestroy();                  // buf
	return aFrom.Length() > 0;
	}

// value of a header line in the open message ("From", "Subject"...)
TBool CPmView::MessageHeader(const TDesC& aName, TDes& aValue) const
	{
	aValue.Zero();
	if (!iText)
		return EFalse;
	TPtrC rest = *iText;
	while (rest.Length())
		{
		TInt nl = rest.Locate('\n');
		TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
		if (line.Length() == 0)
			break;
		if (line.Length() > aName.Length() + 1 && line.Left(aName.Length()).CompareF(aName) == 0 && line[aName.Length()] == ':')
			{
			TPtrC v = line.Mid(aName.Length() + 1);
			while (v.Length() && v[0] == ' ') v.Set(v.Mid(1));
			SafeCopy(aValue, v);
			return ETrue;
			}
		}
	return EFalse;
	}

// the message as plain text (for replies, with "> " before each line)
void CPmView::PlainBodyL(TDes& aOut, TBool aQuote) const
	{
	if (!iText)
		return;
	TPtrC body = iText->Mid(iBodyOff);
	TInt room = aOut.MaxLength() - aOut.Length();
	if (room <= 8)
		return;
	TUint8* out = (TUint8*)aOut.Ptr() + aOut.Length();
	TInt n = pm_plain_text((const char*)body.Ptr(), body.Length(), (char*)out, room, aQuote);
	aOut.SetLength(aOut.Length() + n);
	}

// ============================================================================
// Outbox
// ============================================================================

void CPmView::DraftFromOutboxL(CPmDraft& aDraft)
	{
	const TPmRow* row = CurrentRow();
	if (!row || iMode != EOutbox)
		return;
	TBuf<120> dir;
	OutboxDir(dir);
	// outbox files are 0001.txt (older ones may be 1.txt)
	TFileName path(dir);
	path.AppendFormat(_L("%04d.txt"), row->iUid);
	HBufC* buf = NULL;
	ReadFileL(path, buf, 56 * 1024);
	if (!buf)
		{
		path = dir;
		path.AppendNum(row->iUid);
		path.Append(_L(".txt"));
		ReadFileL(path, buf, 56 * 1024);
		}
	if (!buf)
		{
		iEikonEnv->InfoMsg(_L("Could not open that message"));
		User::Leave(KErrNotFound);
		}
	CleanupStack::PushL(buf);
	aDraft.iFileNo = row->iUid;
	TPtrC rest = *buf;
	while (rest.Length())
		{
		TInt nl = rest.Locate('\n');
		TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
		if (line.Length() == 0)
			break;
		TInt c = line.Locate(':');
		if (c < 0)
			continue;
		TPtrC key = line.Left(c);
		TPtrC v = line.Mid(c + 1);
		while (v.Length() && v[0] == ' ') v.Set(v.Mid(1));
		if (key.CompareF(_L("To")) == 0) SafeCopy(aDraft.iTo, v);
		else if (key.CompareF(_L("Cc")) == 0) SafeCopy(aDraft.iCc, v);
		else if (key.CompareF(_L("Bcc")) == 0) SafeCopy(aDraft.iBcc, v);
		else if (key.CompareF(_L("Subject")) == 0) SafeCopy(aDraft.iSubject, v);
		else if (key.CompareF(_L("In-Reply-To")) == 0) SafeCopy(aDraft.iInReplyTo, v);
		else if (key.CompareF(_L("References")) == 0) SafeCopy(aDraft.iReferences, v);
		else if (key.CompareF(_L("Attach")) == 0) aDraft.iAttach->AppendL(v);
		else if (key.CompareF(_L("Reply-Folder")) == 0) SafeCopy(aDraft.iReplyFolder, v);
		else if (key.CompareF(_L("Reply-Uid")) == 0) aDraft.iReplyUid = ToInt(v);
		}
	delete aDraft.iBody;
	aDraft.iBody = NULL;
	aDraft.iBody = rest.AllocL();
	CleanupStack::PopAndDestroy();
	}

// writes a draft to the outbox; aSend = not a draft: the engine will send it
void CPmView::SaveDraftL(CPmDraft& aDraft, TBool aSend)
	{
	RFs& fs = iCoeEnv->FsSession();
	TBuf<120> dir;
	OutboxDir(dir);
	fs.MkDirAll(dir);
	TInt no = aDraft.iFileNo;
	if (!no)
		{
		// one more than the highest number there
		no = 1;
		TBuf<130> spec(dir);
		spec.Append(_L("*.*"));
		CDir* list = NULL;
		if (fs.GetDir(spec, KEntryAttNormal, ESortByName, list) == KErrNone && list)
			{
			for (TInt i = 0; i < list->Count(); i++)
				{
				TInt n = ToInt((*list)[i].iName);
				if (n >= no) no = n + 1;
				}
			delete list;
			}
		}
	TFileName tmp(dir), path(dir), err(dir);
	tmp.AppendFormat(_L("%04d.tmp"), no);
	path.AppendFormat(_L("%04d.txt"), no);
	err.AppendFormat(_L("%04d.err"), no);
	if (aDraft.iFileNo)
		{
		// the old file may have another width of number
		path = dir;
		path.AppendNum(no);
		path.Append(_L(".txt"));
		TEntry e;
		if (fs.Entry(path, e) != KErrNone)
			{
			path = dir;
			path.AppendFormat(_L("%04d.txt"), no);
			}
		err = dir;
		err.AppendNum(no);
		err.Append(_L(".err"));
		}
	RFile f;
	User::LeaveIfError(f.Replace(fs, tmp, EFileWrite));
	CleanupClosePushL(f);
	// every write is checked: on a full card the first one to fail is
	// remembered and reported, and the half-written file thrown away
	TInt r = KErrNone;
#define PMW(d) do { TInt e_ = f.Write(d); if (r == KErrNone) r = e_; } while (0)
	TBuf<600> h;
	h = _L("#PSIMAIL1\n");
	PMW(h);
	h = _L("To: "); h.Append(aDraft.iTo); h.Append('\n'); PMW(h);
	if (aDraft.iCc.Length()) { h = _L("Cc: "); h.Append(aDraft.iCc); h.Append('\n'); PMW(h); }
	if (aDraft.iBcc.Length()) { h = _L("Bcc: "); h.Append(aDraft.iBcc); h.Append('\n'); PMW(h); }
	h = _L("Subject: "); h.Append(aDraft.iSubject); h.Append('\n'); PMW(h);
	if (aDraft.iInReplyTo.Length()) { h = _L("In-Reply-To: "); h.Append(aDraft.iInReplyTo); h.Append('\n'); PMW(h); }
	if (aDraft.iReferences.Length()) { h = _L("References: "); h.Append(aDraft.iReferences); h.Append('\n'); PMW(h); }
	for (TInt i = 0; i < aDraft.iAttach->Count(); i++)
		{
		h = _L("Attach: ");
		h.Append(Clip((*aDraft.iAttach)[i], 500));
		h.Append('\n');
		PMW(h);
		}
	if (aDraft.iReplyFolder.Length())
		{
		h = _L("Reply-Folder: "); h.Append(aDraft.iReplyFolder); h.Append('\n'); PMW(h);
		h = _L("Reply-Uid: "); h.AppendNum(aDraft.iReplyUid); h.Append('\n'); PMW(h);
		}
	if (!aSend)
		PMW(_L("Draft: 1\n"));
	if (aDraft.iBody)
		{
		// bold / italic / underline in the text: the engine sends HTML too
		const TDesC& t = *aDraft.iBody;
		for (TInt i = 0; i < t.Length(); i++)
			if (t[i] == 0x11 || t[i] == 0x13 || t[i] == 0x18)
				{
				PMW(_L("Format: rich\n"));
				break;
				}
		}
	PMW(_L("\n"));
	if (aDraft.iBody)
		PMW(*aDraft.iBody);
	TInt e = f.Flush();
	if (r == KErrNone) r = e;
#undef PMW
	CleanupStack::PopAndDestroy();      // f
	if (r != KErrNone)
		{
		fs.Delete(tmp);
		User::Leave(r);
		}
	// the new file takes the old one's place in one step (RFs::Replace): a
	// crash or a full card between a Delete and a Rename could lose the only
	// copy of the message
	r = fs.Replace(tmp, path);
	if (r != KErrNone)
		{
		fs.Delete(tmp);
		User::Leave(r);
		}
	fs.Delete(err);
	aDraft.iFileNo = no;
	if (aSend)
		{
		if (iSettings->iOffline)
			Toast(_L("In the Outbox - it goes at the next check for mail"));
		else
			{
			Cmd(PM_CMD_SEND, KNullDesC8, 0, KNullDesC8);
			Working(_L("Sending..."));
			}
		}
	else
		Toast(_L("Saved in the Outbox"));
	if (iMode == EOutbox)
		LoadOutboxL();
	Render();
	}

void CPmView::DeleteOutboxL()
	{
	const TPmRow* row = CurrentRow();
	if (!row)
		return;
	TUint no = row->iUid;                     // (the list may reload during the query)
	TBuf<104> subj;
	subj.Append('"');
	subj.Append(row->iSubject.Left(row->iSubject.Length() < 100 ? row->iSubject.Length() : 100));
	subj.Append('"');
	if (!iEikonEnv->QueryWinL(subj, _L("Delete this message?")))
		return;
	TBuf<120> dir;
	OutboxDir(dir);
	RFs& fs = iCoeEnv->FsSession();
	TFileName p(dir);
	p.AppendNum(no); p.Append(_L(".txt"));
	TInt r = fs.Delete(p);
	if (r != KErrNone)
		{
		p = dir; p.AppendFormat(_L("%04d.txt"), no);
		fs.Delete(p);
		}
	p = dir; p.AppendFormat(_L("%04d.err"), no); fs.Delete(p);
	p = dir; p.AppendNum(no); p.Append(_L(".err")); fs.Delete(p);
	LoadOutboxL();
	Render();
	}

// ============================================================================
// The engine's replies
// ============================================================================

TInt CPmView::TickCallback(TAny* aSelf)
	{
	((CPmView*)aSelf)->Tick();
	return 1;
	}

void CPmView::Tick()
	{
	TRAPD(err, TickL());
	(void)err;
	}

void CPmView::TickL()
	{
	PmShared* s = iShared;
	if (!s)
		return;
	if (iStartPending)
		{
		FinishStartL();
		return;
		}
	s->app_beat++;                       // "still here": see pmepoc.cpp
	TBool redraw = EFalse;
	// a picture the engine has just decoded goes into the message being read
	// (whether or not it is still busy with the rest)
	if (iMode == EMessage && s->changed_seq != iPicChangedSeen)
		{
		iPicChangedSeen = s->changed_seq;
		TRAPD(pe, RefreshPicturesL());
		(void)pe;
		}
	if (s->done_seq != iDoneSeen)
		{
		iDoneSeen = s->done_seq;
		TBool changed = s->changed_seq != iChangedSeen;
		iChangedSeen = s->changed_seq;
		PmCmd cmd = iSent[(iDoneSeen - 1) % PM_CMDQ];
		TInt res = s->last_res;
		HandleResultL(cmd);
		// (OK, OFFLINE and failures reload in HandleResultL)
		if (changed && (res == PM_RES_CANCELLED || res == PM_RES_UNTRUSTED ||
			res == PM_RES_NEED_PASS || res == PM_RES_LOGIN_FAILED))
			ReloadL();
		redraw = ETrue;
		}
	else if (s->changed_seq != iChangedSeen && !s->busy)
		{
		// files changed (e.g. a sync saved the list): once the engine is done
		iChangedSeen = s->changed_seq;
		ReloadL();
		}
	// progress
	TBuf<128> prog;
	FromC(prog, s->progress);
	// while it connects, show what the link is doing until the engine has
	// news of its own
	if (s->net.link_seq != iLinkSeq)
		{
		iLinkSeq = s->net.link_seq;
		FromC(iLinkMsg, s->net.link_msg);
		iLinkProg = prog;
		}
	if (!s->busy)
		iLinkMsg.Zero();
	else if (iLinkMsg.Length() && prog == iLinkProg)
		prog = iLinkMsg;
	if (s->online != iOnlineWas)
		{
		iOnlineWas = s->online;
		redraw = ETrue;
		}
	if (prog != iLastProgress || s->busy != iBusyWas)
		{
		iLastProgress = prog;
		iBusyWas = s->busy;
		redraw = ETrue;
		// what the engine is doing: EIKON's busy message, bottom left
		if (iNativeShown && NativeMode() && s->busy && prog.Length())
			{
			TRAPD(err, iEikonEnv->BusyMsgL(prog, EHLeftVBottom, TTimeIntervalMicroSeconds32(300000)));
			(void)err;
			iBusyShown = ETrue;
			}
		else if (iBusyShown && !s->busy)
			{
			iEikonEnv->BusyMsgCancel();
			iBusyShown = EFalse;
			iWorkingSince = 0;
			}
		}
	if (s->busy)
		iWorkingSince = 0;                       // (the engine took the work up)
	else if (iWorkingSince && iBusyShown && User::TickCount() - iWorkingSince > 64 * 5)
		{
		// Working()'s message, and the engine never got busy: take it down
		iEikonEnv->BusyMsgCancel();
		iBusyShown = EFalse;
		iWorkingSince = 0;
		}
	// messages that have had their time
	TUint now = User::TickCount();
	if (iStatus.Length() && iStatusUntil && now - iStatusUntil < 0x80000000u && !s->busy)
		{
		iStatus.Zero();
		iStatusUntil = 0;
		redraw = ETrue;
		}
	// the engine is up: its working priority (see StartEngineL), and from
	// the start-up screen to the mail
	if (iEngineLow && iRunning && s->state != PM_STATE_STARTING)
		{
		iEngineLow = EFalse;
		iProcess.SetPriority(EPriorityHigh);
		}
	if (!iSplashDone && s->state != PM_STATE_STARTING)
		{
		iSplashDone = ETrue;
		redraw = ETrue;
		}
	AutoTickL();                             // a connection came up: send what waits (pmauto.cpp)
	if (redraw)
		Render();
	}

void CPmView::HandleResultL(const PmCmd& aCmd)
	{
	if (aCmd.op == PM_CMD_UNDO)
		{
		UndoResultL(aCmd);                   // (pmundo.cpp)
		return;
		}
	if (AutoResultL(aCmd))                   // a timed check: quiet but for new mail (pmauto.cpp)
		return;
	PmShared* s = iShared;
	TBuf<160> msg;
	FromC(msg, s->last_msg);
	TInt res = s->last_res;
	SafeCopy(iStatus, msg);
	iStatusUntil = User::TickCount() + 64 * 6;
	if (aCmd.op == PM_CMD_PICTURES && res == PM_RES_OK)
		iStatus.Zero();                      // (the pictures themselves say so)
	if (iNativeShown && NativeMode() && iStatus.Length() && aCmd.op != PM_CMD_UPDATE)
		iEikonEnv->InfoMsg(iStatus);         // the outcome, as an infoprint
	if (aCmd.op == PM_CMD_UPDATE)
		{
		iStatus.Zero();
		if (res == PM_RES_OK && s->update_ready)
			{
			TBuf<16> v;
			FromC(v, s->update_version);
			TBuf<64> q;
			q.Format(_L("Install PsiMail %S now?"), &v);
			TBuf<128> file;
			FromC(file, s->last_file);
			if (iEikonEnv->QueryWinL(_L("PsiMail will close while it installs"), q))
				StartInstallerL(file);
			else
				iEikonEnv->InfoWinL(_L("The update is saved"), file);
			}
		else if (res == PM_RES_OK)
			Toast(msg);
		else
			iEikonEnv->InfoWinL(_L("Update PsiMail"), msg);
		return;
		}
	if (aCmd.op == PM_CMD_CALSYNC)
		iCalPending = EFalse;
	if (aCmd.op == PM_CMD_CALSYNC && res != PM_RES_UNTRUSTED && res != PM_RES_CANCELLED)
		{
		HandleCalResultL(aCmd, res, msg);
		return;
		}
	if (aCmd.op == PM_CMD_SENDRECV && iCalAfterMail &&
		res != PM_RES_UNTRUSTED && res != PM_RES_NEED_PASS && res != PM_RES_LOGIN_FAILED)   // (those retry below)
		{
		iCalAfterMail = EFalse;
		// the link came up (the server may still have said no): the calendar's
		// turn. Not after OFFLINE - the connection failed - or Stopped.
		if (res != PM_RES_OFFLINE && res != PM_RES_CANCELLED && !iSettings->iOffline)
			CalendarSyncL();
		}
	if (res == PM_RES_OK)
		NewMailAlertL(aCmd, EFalse);         // the sound (pmauto.cpp)
	switch (res)
		{
	case PM_RES_OK:
		if (aCmd.op == PM_CMD_SEARCH)
			{
			iSearch = ETrue;
			iMode = EList;
			iListMode = EList;
			iFolder.Copy(TPtrC8((const TUint8*)aCmd.folder));
			iSel = 0;
			LoadListL();
			iSel = 0;
			}
		else if (aCmd.op == PM_CMD_ATTACH)
			{
			TBuf<128> file;
			FromC(file, s->last_file);
			TPtrC8 arg((const TUint8*)aCmd.arg);
			if (arg.Locate('\t') < 0)
				iEikonEnv->InfoWinL(_L("Attachment saved"), file);
			else if (iFwdPending > 0 && aCmd.uid == iFwdUid)
				{
				// one of a forward's: the last one in starts it
				iFwdFiles->AppendL(file);
				if (--iFwdPending == 0)
					((CPmAppUi*)iEikonEnv->EikAppUi())->ForwardReadyL(iFwdUid, *iFwdFiles);
				}
			else
				LaunchFileL(file);                     // Open attachment: fetched, now open it
			}
		else if ((aCmd.op == PM_CMD_SYNC || aCmd.op == PM_CMD_SENDRECV) && s->new_mail > 0 && aCmd.op == PM_CMD_SENDRECV)
			Toast(msg);
		else if (aCmd.op == PM_CMD_SEND || aCmd.op == PM_CMD_SENDRECV)
			Toast(msg);
		if (aCmd.op == PM_CMD_BODY || aCmd.op == PM_CMD_FULLBODY)
			{
			if (iMode == EMessage && aCmd.uid == iMsgUid)
				LoadMessageL();
			}
		if (aCmd.op == PM_CMD_MKFOLDER || aCmd.op == PM_CMD_RENFOLDER || aCmd.op == PM_CMD_DELFOLDER)
			{
			FolderChangedL(aCmd);
			break;
			}
		ReloadL();
		break;
	case PM_RES_UNTRUSTED:
		{
		TBuf<80> host;
		FromC(host, s->trust_host);
		TBuf<100> why;
		FromC(why, s->trust_why);
		TBuf<100> fp;
		FromC(fp, s->trust_fp);
		TBuf<200> lines[4];
		lines[0] = _L("PsiMail can't be sure it is talking to");
		lines[1] = host;
		lines[1].Append(_L(": "));
		lines[1].Append(why);
		lines[2] = _L("Key: ");
		lines[2].Append(Clip(fp, 95));
		lines[3] = _L("Trust it only if you expected this (e.g. your own server)");
		TPtrC ptrs[4];
		for (TInt k = 0; k < 4; k++) ptrs[k].Set(lines[k]);
		CPmInfoDialog* dlg = new(ELeave) CPmInfoDialog(_L("Certificate not trusted"), ptrs, 4);
		dlg->ExecuteLD(R_PM_INFO_DIALOG);
		if (iEikonEnv->QueryWinL(host, _L("Trust this server's key from now on?")))
			{
			TBuf8<80> hp;
			hp.Copy(host);
			Cmd(PM_CMD_TRUST, KNullDesC8, 0, hp);
			// and try again
			if (aCmd.op == PM_CMD_CALSYNC) iCalPending = ETrue;
			Cmd(aCmd.op, TPtrC8((const TUint8*)aCmd.folder), aCmd.uid, TPtrC8((const TUint8*)aCmd.arg));
			}
		break;
		}
	case PM_RES_NEED_PASS:
	case PM_RES_LOGIN_FAILED:
		{
		PmAccount& a = iSettings->iAccounts[iSettings->iAcct];
		if (res == PM_RES_LOGIN_FAILED)
			iEikonEnv->InfoWinL(_L("The server refused the login"), Clip(msg, 100));
		TBuf<100> prompt;
		prompt = _L("Password for ");
		TBuf<60> user;
		FromC(user, a.user);
		prompt.Append(user);
		TBuf<32> pw;
		CPmPasswordDialog* dlg = new(ELeave) CPmPasswordDialog(prompt, pw);
		if (dlg->ExecuteLD(R_PM_PASSWORD_DIALOG) && pw.Length())
			{
			CopyToC(a.pass, sizeof(a.pass), pw);
			((CPmAppUi*)iEikonEnv->EikAppUi())->SaveSettings();
			CopySettingsToShared();
			Cmd(aCmd.op, TPtrC8((const TUint8*)aCmd.folder), aCmd.uid, TPtrC8((const TUint8*)aCmd.arg));
			}
		break;
		}
	case PM_RES_OFFLINE:
		if (aCmd.op == PM_CMD_ATTACH)
			iFwdPending = 0;
		if (aCmd.op == PM_CMD_FLAG && iSettings->iOffline)
			{
			ReloadL();                         // queued, as expected when offline
			break;
			}
		Toast(msg);
		ReloadL();
		break;
	case PM_RES_CANCELLED:
		if (aCmd.op == PM_CMD_ATTACH)
			iFwdPending = 0;
		Toast(_L("Stopped"));
		break;
	default:
		if ((aCmd.op == PM_CMD_BODY || aCmd.op == PM_CMD_FULLBODY) && aCmd.uid == iMsgUid)
			iBodyError = msg;                    // shown in place of "Downloading..."
		if (aCmd.op == PM_CMD_ATTACH && iFwdPending > 0)
			iFwdPending = 0;                     // (forward without them: Message > Forward again)
		// what didn't happen, then the engine's reason (the style guide's
		// "say what went wrong", rather than a bare program name)
		if (aCmd.op == PM_CMD_BODY || aCmd.op == PM_CMD_ATTACH || aCmd.op == PM_CMD_SEND ||
			aCmd.op == PM_CMD_SENDRECV || aCmd.op == PM_CMD_SEARCH)
			{
			TPtrC what(aCmd.op == PM_CMD_BODY ? _L("The message did not download") :
				aCmd.op == PM_CMD_ATTACH ? _L("The attachments did not download") :
				aCmd.op == PM_CMD_SEND ? _L("The message was not sent") :
				aCmd.op == PM_CMD_SEARCH ? _L("Find did not finish") : _L("Check mail did not finish"));
			iEikonEnv->InfoWinL(what, Clip(msg, 120));
			}
		else
			Toast(msg);
		ReloadL();
		break;
		}
	}

void CPmView::HandleCalResultL(const PmCmd& aCmd, TInt aRes, const TDesC& aMsg)
	{
	TBool list = aCmd.arg[0] == 'l';
	iCalPending = EFalse;
	if (aRes == PM_RES_OK)
		{
		if (list)
			{
			Toast(aMsg);
			return;
			}
		iStatus.Zero();
		TBuf<100> dir;
		StoreDir(dir);
		iCalSync->StartL(dir, *iCal);
		return;
		}
	if (aRes == PM_RES_OFFLINE && !list)
		{
		// no network: the Agenda half still runs (new Psion entries are
		// kept in push.txt for next time)
		iCalSecond = ETrue;
		iCalMsg.Zero();
		TBuf<100> dir;
		StoreDir(dir);
		iCalSync->StartL(dir, *iCal);
		return;
		}
	iCalSecond = EFalse;
	if (aRes == PM_RES_NEED_PASS || aRes == PM_RES_LOGIN_FAILED)
		{
		if (aRes == PM_RES_LOGIN_FAILED)
			{
			TBuf<200> lines[3];
			lines[0] = _L("The calendar server refused the password");
			lines[1] = _L("If your provider uses app passwords, make one");
			lines[2] = _L("that can use calendars (CalDAV), and enter it here");
			TPtrC ptrs[3];
			for (TInt k = 0; k < 3; k++) ptrs[k].Set(lines[k]);
			CPmInfoDialog* info = new(ELeave) CPmInfoDialog(_L("Calendar"), ptrs, 3);
			info->ExecuteLD(R_PM_INFO_DIALOG);
			}
		TBuf<32> pw;
		CPmPasswordDialog* dlg = new(ELeave) CPmPasswordDialog(_L("Calendar password"), pw);
		if (dlg->ExecuteLD(R_PM_PASSWORD_DIALOG) && pw.Length())
			{
			CopyToC(iCal->iCal.pass, sizeof(iCal->iCal.pass), pw);
			((CPmAppUi*)iEikonEnv->EikAppUi())->SaveCalSettings();
			CopySettingsToShared();
			CalCmd(TPtrC8((const TUint8*)aCmd.arg));
			}
		return;
		}
	TBuf<160> t(_L("Calendar: "));
	t.Append(Clip(aMsg, 140));
	Toast(t);
	}

void CPmView::SetStatus(const TDesC& aText)
	{
	SafeCopy(iStatus, aText);
	iStatusUntil = User::TickCount() + 64 * 6;
	Render();
	}

// ============================================================================
// Drawing - the EIKON screens (pmnative.cpp, pmcalview.cpp)
// ============================================================================

void CPmView::Toast(const TDesC& aText)
	{
	iEikonEnv->InfoMsg(aText);               // EIKON's own message, as the style guide has it
	}

// "Sending...", "Stopping...": the style guide puts what the program is
// doing bottom left, as EIKON's busy message. The engine's own progress
// replaces it, and the tick takes it down when the engine is idle (or after
// a few seconds, should the engine never have picked the work up)
void CPmView::Working(const TDesC& aText)
	{
	if (!iRunning || !iNativeShown)
		{
		Toast(aText);
		return;
		}
	TRAPD(err, iEikonEnv->BusyMsgL(aText, EHLeftVBottom, TTimeIntervalMicroSeconds32(0)));
	if (err != KErrNone)
		return;
	iBusyShown = ETrue;
	iWorkingSince = User::TickCount();
	if (!iWorkingSince) iWorkingSince = 1;
	}

void CPmView::FormatDate(TInt aDate, TDes& aOut) const
	{
	aOut.Zero();
	if (aDate <= 0)
		return;
	TTime t(TDateTime(1970, EJanuary, 0, 0, 0, 0, 0));
	t += TTimeIntervalSeconds(aDate);
	t += TLocale().UniversalTimeOffset();
	TTime now;
	now.HomeTime();
	TDateTime d = t.DateTime();
	TDateTime n = now.DateTime();
	const TText* KMonths[] = { _S("Jan"), _S("Feb"), _S("Mar"), _S("Apr"), _S("May"), _S("Jun"),
		_S("Jul"), _S("Aug"), _S("Sep"), _S("Oct"), _S("Nov"), _S("Dec") };
	const TText* KDays[] = { _S("Mon"), _S("Tue"), _S("Wed"), _S("Thu"), _S("Fri"), _S("Sat"), _S("Sun") };
	// whole days between the two dates (not 24-hour periods)
	TTime dm(TDateTime(d.Year(), d.Month(), d.Day(), 0, 0, 0, 0));
	TTime nm(TDateTime(n.Year(), n.Month(), n.Day(), 0, 0, 0, 0));
	TTimeIntervalDays ago = nm.DaysFrom(dm);
	if (ago.Int() == 0)
		aOut.Format(_L("%02d:%02d"), d.Hour(), d.Minute());
	else if (ago.Int() == 1)
		aOut = _L("Yesterday");
	else if (ago.Int() > 1 && ago.Int() < 7)
		aOut.Format(_L("%s %02d:%02d"), KDays[t.DayNoInWeek()], d.Hour(), d.Minute());
	else if (d.Year() == n.Year())
		aOut.Format(_L("%d %s"), d.Day() + 1, KMonths[d.Month()]);
	else
		aOut.Format(_L("%d %s %02d"), d.Day() + 1, KMonths[d.Month()], d.Year() % 100);
	}

// brings the screen up to date: the EIKON controls, the title band and the
// calendar's pane (pmnative.cpp, pmcalview.cpp)
void CPmView::Render()
	{
	UseMenus(iMode == ECalendar);
	if (!iFolderList)
		return;
	if (!iNativeShown)
		{
		iNativeShown = ETrue;
		iNativeMode = (TMode)-1;
		((CPmAppUi*)iEikonEnv->EikAppUi())->ShowToolBar(ETrue);   // (sets our rect: SizeChanged lays out)
		}
	TRAPD(err, UpdateNativeL());
	(void)err;
	}

void CPmView::Draw(const TRect& aRect) const
	{
	DrawNative(aRect);
	DrawStatus(SystemGc());
	}

// ============================================================================
// Keys and pen
// ============================================================================

// keeps the selection within the list (the list box scrolls to it itself)
void CPmView::EnsureVisible()
	{
	TInt count = iRows->Count();
	if (iSel >= count) iSel = count - 1;
	if (iSel < 0) iSel = 0;
	}

// the entry in the folder column for what's showing
TInt CPmView::CurrentSidebarItem() const
	{
	if (iMode == ECalendar)
		return iFolders->Count() + 1;
	if (iMode == EOutbox)
		return iFolders->Count();
	for (TInt i = 0; i < iFolders->Count(); i++)
		if ((*iFolders)[i].iImap == iFolder)
			return i;
	return 0;
	}

// View > Go to: one of the folder tree's entries
void CPmView::OpenSidebarItemL(TInt aIndex)
	{
	if (iMode == EMessage)
		BackL();
	iSidebar = ETrue;
	iFolderSel = aIndex;
	OpenCurrentL();
	Render();
	}

// Key and pen timings: the randomness behind TLS keys (see psiglue pg_entropy)
void CPmView::AddEntropy(TUint aValue)
	{
	TUint32 t = User::TickCount() ^ (aValue << 16);
	TTime now;
	now.UniversalTime();
	t ^= now.Int64().Low();
	iShared->net.entropy[iEntropyPos % PSI_ENTROPY_SIZE] ^= (TUint8)t;
	iShared->net.entropy[(iEntropyPos + 1) % PSI_ENTROPY_SIZE] ^= (TUint8)(t >> 8);
	iEntropyPos += 2;
	if (iShared->net.entropy_len < PSI_ENTROPY_SIZE)
		iShared->net.entropy_len = iEntropyPos < PSI_ENTROPY_SIZE ? iEntropyPos : PSI_ENTROPY_SIZE;
	}

TKeyResponse CPmView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKey)
		return EKeyWasNotConsumed;
	TUint code = aKeyEvent.iCode;
	AddEntropy(code);
	if (aKeyEvent.iModifiers & EModifierCtrl)
		return EKeyWasNotConsumed;         // the menu's hotkeys
	// Esc stops what the engine is doing (File > Stop shows Esc, as in the
	// built-in programs) in the lists and the calendar, and in the reader
	// while the message is still coming; otherwise it goes back. Download
	// ahead in the background isn't stopped by it
	TBool escStops = (iMode == EMessage && iWaitingBody) ||
		iMode == EList || iMode == EOutbox || iMode == ECalendar || iMode == ENoAccount ||
		(iShared->busy && !iShared->online);   // still connecting: nothing to go back from
	if (code == EKeyEscape && Busy() && escStops && !DownloadingAhead())
		{
		StopEngineWork(_L("Stopping..."));  // stop what the engine is doing
		return EKeyWasConsumed;
		}
	return NativeKeyL(aKeyEvent, aType);
	}

void CPmView::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	TPoint p = aEvent.iPosition;
	AddEntropy(p.iX * 1000 + p.iY);
	// the title band, the headings and the calendar's pane are ours; the
	// list boxes and the reader take the pen themselves
	if (NativePointerL(aEvent))
		return;
	if (iMode == ECalendar && CalendarPointerL(aEvent))
		return;
	CCoeControl::HandlePointerEventL(aEvent);
	if (iMode == EMessage && aEvent.iType == TPointerEvent::EButton1Up && iLinks)
		{
		// a tap on a link or an attachment opens it
		TInt pos = iReader->CursorPos();
		for (TInt i = 0; i < iLinks->Count(); i++)
			if (pos >= (*iLinks)[i].iPos && pos < (*iLinks)[i].iPos + (*iLinks)[i].iLen)
				{
				iLinkSel = i;
				NativeActivateLinkL();
				break;
				}
		}
	}

// ============================================================================
// PsiWeb: links, and HTML mail as a web page (NetSurf)
// ============================================================================

const TUid KUidPsiWeb = { 0x01000A7A };

const TInt PM_WEB_URL_MAX = 500;      // PsiWeb takes up to 511 bytes

void CPmView::OpenWebL(const TDesC& aUrl)
	{
	// On the modem only one program can have the serial line, so PsiWeb
	// needs it for a web address (not for our own files). Over Psion
	// Internet both share the connection: nothing to hang up.
	if (Clip(aUrl, 5).CompareF(_L("file:")) != 0 && !iSettings->iNetMode && iShared->online)
		{
		if (Busy() && !DownloadingAhead() &&
			!iEikonEnv->QueryWinL(_L("PsiMail is using the modem"), _L("Stop and give the line to PsiWeb?")))
			return;
		Cmd(PM_CMD_HANGUP, KNullDesC8, 0, KNullDesC8);
		}
	TBuf8<PM_WEB_URL_MAX> url8;
	url8.Copy(Clip(aUrl, PM_WEB_URL_MAX));
	// already running? then hand it the address (CPwAppUi::ProcessMessageL)
	TApaTaskList tasks(iEikonEnv->WsSession());
	TApaTask task = tasks.FindApp(KUidPsiWeb);
	if (task.Exists())
		{
		task.SendMessage(KUidPsiWeb, url8);
		task.BringToForeground();
		return;
		}
	RApaLsSession ls;
	User::LeaveIfError(ls.Connect());
	CleanupClosePushL(ls);
	TApaAppInfo info;
	TInt r = ls.GetAppInfo(info, KUidPsiWeb);
	CleanupStack::PopAndDestroy();          // ls
	if (r != KErrNone)
		{
		iEikonEnv->InfoWinL(_L("PsiWeb is not installed"), _L("Install PsiWeb.sis to open links and web pages"));
		return;
		}
	CApaCommandLine* cmd = CApaCommandLine::NewLC();
	cmd->SetLibraryNameL(info.iFullName);
	cmd->SetCommandL(EApaCommandRun);
	cmd->SetTailEndL(url8);
	EikDll::StartAppL(*cmd);
	CleanupStack::PopAndDestroy();          // cmd
	Toast(_L("Opening in PsiWeb..."));
	}

void CPmView::ViewAsWebPageL()
	{
	if (iMode != EMessage || !iHtml)
		{
		Toast(_L("This message has no web page version"));
		return;
		}
	TBuf<150> path;
	MsgPath(iMsgUid, _L("htm"), path);
	// file:///D:/PsiMail/A0/F.../12.htm
	TBuf<200> url(_L("file:///"));
	for (TInt i = 0; i < path.Length(); i++)
		url.Append(path[i] == '\\' ? '/' : path[i]);
	OpenWebL(url);
	}

// ============================================================================
// Dialogs
// ============================================================================

void CPmInfoDialog::PreLayoutDynInitL()
	{
	SetTitleL(iTitle);
	for (TInt i = 0; i < 6; i++)
		{
		if (i < iCount)
			SetLabelL(EPmDlgInfo1 + i, iLines[i]);
		else
			MakeLineVisible(EPmDlgInfo1 + i, EFalse);
		}
	}

void CPmTextDialog::PreLayoutDynInitL()
	{
	SetTitleL(iTitle);
	SetEdwinTextL(EPmDlgWords, &iText);
	}

TBool CPmTextDialog::OkToExitL(TInt /*aButtonId*/)
	{
	GetEdwinText(iText, EPmDlgWords);
	iText.Trim();
	return ETrue;
	}

void CPmPasswordDialog::PreLayoutDynInitL()
	{
	SetLabelL(EPmDlgInfo1, iPrompt);
	}

TBool CPmPasswordDialog::OkToExitL(TInt /*aButtonId*/)
	{
	GetSecretEditorText(iText, EPmDlgPass);
	return ETrue;
	}

void CPmChoiceDialog::PreLayoutDynInitL()
	{
	SetTitleL(iTitle);
	if (iPrompt.Length())
		SetControlCaptionL(EPmDlgChoice, iPrompt);
	CEikChoiceList* cl = (CEikChoiceList*)Control(EPmDlgChoice);
	cl->SetArrayL(iItems);              // it owns the array now
	cl->SetCurrentItem(iChoice >= 0 && iChoice < iItems->Count() ? iChoice : 0);
	iItems = NULL;
	}

TBool CPmChoiceDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iChoice = ((CEikChoiceList*)Control(EPmDlgChoice))->CurrentItem();
	return ETrue;
	}

// ----- compose ---------------------------------------------------------------

// EIKON sizes a dialog to fit its contents: keep it on the 640x240 screen
void CPmComposeDialog::SetSizeAndPositionL(const TSize& /*aSize*/)
	{
	// the whole screen, like the built-in Email program's new message
	SetCornerAndSizeL(EHLeftVTop, iEikonEnv->ScreenDevice()->SizeInPixels());
	}

void CPmComposeDialog::PreLayoutDynInitL()
	{
	SetEdwinTextL(EPmDlgTo, &iDraft.iTo);
	SetEdwinTextL(EPmDlgCc, &iDraft.iCc);
	SetEdwinTextL(EPmDlgBcc, &iDraft.iBcc);
	SetEdwinTextL(EPmDlgSubject, &iDraft.iSubject);
	// the text, with its bold / italic / underline marks (see the engine's
	// compose.c) turned back into the editor's formatting
	HBufC* b = HBufC::NewLC(iDraft.iBody->Length() + 1);
	TPtr p = b->Des();
	CArrayFixFlat<TInt>* spans = new(ELeave) CArrayFixFlat<TInt>(16);   // start, length, style
	CleanupStack::PushL(spans);
	TInt start[3] = { -1, -1, -1 };
	const TPtrC src = *iDraft.iBody;
	for (TInt i = 0; i < src.Length(); i++)
		{
		TText c = src[i];
		TInt style = -1, on = 0;
		switch (c)
			{
		case 0x11: style = 0; on = 1; break;
		case 0x12: style = 0; break;
		case 0x13: style = 1; on = 1; break;
		case 0x14: style = 1; break;
		case 0x18: style = 2; on = 1; break;
		case 0x19: style = 2; break;
		default: break;
			}
		if (style >= 0)
			{
			if (on && start[style] < 0)
				start[style] = p.Length();
			else if (!on && start[style] >= 0)
				{
				spans->AppendL(start[style]);
				spans->AppendL(p.Length() - start[style]);
				spans->AppendL(style);
				start[style] = -1;
				}
			continue;
			}
		p.Append(c == '\n' ? (TText)CEditableText::EParagraphDelimiter : c);
		}
	for (TInt st = 0; st < 3; st++)
		if (start[st] >= 0)
			{
			spans->AppendL(start[st]);
			spans->AppendL(p.Length() - start[st]);
			spans->AppendL(st);
			}
	SetEdwinTextL(EPmDlgBody, b);
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(EPmDlgBody);
	for (TInt k = 0; k + 2 < spans->Count(); k += 3)
		{
		TCharFormat cf;
		TCharFormatMask cm;
		switch ((*spans)[k + 2])
			{
		case 0:
			cf.iFontSpec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
			cm.SetAttrib(EAttFontStrokeWeight);
			break;
		case 1:
			cf.iFontSpec.iFontStyle.SetPosture(EPostureItalic);
			cm.SetAttrib(EAttFontPosture);
			break;
		default:
			cf.iFontPresentation.iUnderline = EUnderlineOn;
			cm.SetAttrib(EAttFontUnderline);
			break;
			}
		if ((*spans)[k + 1] > 0)
			ed->RichText()->ApplyCharFormatL(cf, cm, (*spans)[k], (*spans)[k + 1]);
		}
	if (spans->Count())
		ed->HandleTextChangedL();
	CleanupStack::PopAndDestroy(2);           // spans, b
	ShowAttachments();
	}

// Ctrl+B, Ctrl+I, Ctrl+U: bold, italic, underline in the text (EIKON's
// standard shortcuts for them)
TKeyResponse CPmComposeDialog::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType == EEventKey && (aKeyEvent.iModifiers & EModifierCtrl))
		{
		// the buttons' keys first: the editors would take them otherwise
		TUint k = aKeyEvent.iCode;
		if (k == 19 || k == 's' || k == 'S') { TryExitL(EPmBidSend); return EKeyWasConsumed; }
		if (k == 4 || k == 'd' || k == 'D') { TryExitL(EPmBidSave); return EKeyWasConsumed; }
		if (k == 1 || k == 'a' || k == 'A') { TryExitL(EPmBidAttach); return EKeyWasConsumed; }
		if (k == 12 || k == 'l' || k == 'L') { TryExitL(EPmBidContacts); return EKeyWasConsumed; }
		}
	// Tab in an address line: complete the name from Contacts
	if (aType == EEventKey && aKeyEvent.iCode == EKeyTab && !(aKeyEvent.iModifiers & EModifierCtrl) &&
		(IdOfFocusControl() == EPmDlgTo || IdOfFocusControl() == EPmDlgCc || IdOfFocusControl() == EPmDlgBcc))
		{
		ContactsL(ETrue);
		return EKeyWasConsumed;
		}
	if (aType == EEventKey && (aKeyEvent.iModifiers & EModifierCtrl) && IdOfFocusControl() == EPmDlgBody)
		{
		TUint c = aKeyEvent.iCode;
		TInt flag = 0;
		if (c == 2 || c == 'b' || c == 'B') flag = CEikGlobalTextEditor::EBold;
		else if (c == 9 || c == 'i' || c == 'I') flag = CEikGlobalTextEditor::EItalic;
		else if (c == 21 || c == 'u' || c == 'U') flag = CEikGlobalTextEditor::EUnderline;
		if (flag)
			{
			((CEikRichTextEditor*)Control(EPmDlgBody))->BoldItalicUnderlineEventL(flag);
			return EKeyWasConsumed;
			}
		}
	return CEikDialog::OfferKeyEventL(aKeyEvent, aType);
	}

void CPmComposeDialog::PostLayoutDynInitL()
	{
	// replies start in the text; new messages at To
	((CEikEdwin*)Control(EPmDlgBody))->SetCursorPosL(0, EFalse);
	if (iDraft.iTo.Length())
		TryChangeFocusToL(EPmDlgBody);
	// what the message looked like to begin with: Close asks about
	// discarding it only if that has changed
	Collect();
	iSum0 = StateSum();
	}

// a checksum of the message as written (the addresses, the subject, the
// text, the attachments)
TUint CPmComposeDialog::StateSum() const
	{
	TUint sum = 17;
	const TDesC* parts[5] = { &iDraft.iTo, &iDraft.iCc, &iDraft.iBcc, &iDraft.iSubject, iDraft.iBody };
	for (TInt k = 0; k < 5; k++)
		{
		if (!parts[k])
			continue;
		const TDesC& t = *parts[k];
		for (TInt i = 0; i < t.Length(); i++)
			sum = sum * 31 + t[i];
		sum = sum * 7 + 1;
		}
	for (TInt a = 0; a < iDraft.iAttach->Count(); a++)
		{
		const TDesC& t = (*iDraft.iAttach)[a];
		for (TInt i = 0; i < t.Length(); i++)
			sum = sum * 31 + t[i];
		}
	return sum;
	}

// the address line with the focus: To, Cc or Bcc (To from anywhere else)
TInt CPmComposeDialog::AddressLine() const
	{
	TInt id = IdOfFocusControl();
	return (id == EPmDlgCc || id == EPmDlgBcc) ? id : EPmDlgTo;
	}

// the Contacts button: pick from the Psion's Contacts into the address line
// with the focus; Tab: complete what's typed there
void CPmComposeDialog::ContactsL(TBool aComplete)
	{
	CPmContacts* c = ((CPmAppUi*)iEikonEnv->EikAppUi())->Contacts();
	if (!c)
		return;
	TInt id = AddressLine();
	TDes& field = id == EPmDlgCc ? (TDes&)iDraft.iCc : id == EPmDlgBcc ? (TDes&)iDraft.iBcc : (TDes&)iDraft.iTo;
	GetEdwinText(field, id);
	TBool changed = aComplete ? c->CompleteL(field) : c->PickL(field, KNullDesC);
	if (!changed)
		return;
	SetEdwinTextL(id, &field);
	CEikEdwin* ed = (CEikEdwin*)Control(id);
	ed->SetCursorPosL(field.Length(), EFalse);
	ed->DrawDeferred();
	}

void CPmComposeDialog::ShowAttachments()
	{
	TBuf<120> t;
	TInt n = iDraft.iAttach->Count();
	if (n == 0)
		t = _L("none");
	else
		{
		for (TInt i = 0; i < n; i++)
			{
			TParsePtrC parse((*iDraft.iAttach)[i]);
			TPtrC name = Clip(parse.NameAndExt(), 40);
			if (t.Length() + name.Length() + 6 > t.MaxLength())
				{
				t.Append(_L(" ..."));
				break;
				}
			if (i) t.Append(_L(", "));
			t.Append(name);
			}
		}
	TRAPD(err, SetLabelL(EPmDlgAttachments, t));
	}

void CPmComposeDialog::Collect()
	{
	GetEdwinText(iDraft.iTo, EPmDlgTo);
	GetEdwinText(iDraft.iCc, EPmDlgCc);
	GetEdwinText(iDraft.iBcc, EPmDlgBcc);
	GetEdwinText(iDraft.iSubject, EPmDlgSubject);
	// the text, with marks where bold / italic / underline change (the
	// engine sends it as HTML as well as plain text)
	CRichText* rt = ((CEikRichTextEditor*)Control(EPmDlgBody))->RichText();
	TInt len = rt->DocumentLength();
	for (TInt pass = 0; pass < 2; pass++)
		{
		// pass 0 counts the runs, pass 1 writes the text
		TInt runs = 0;
		TPtr out(NULL, 0);
		HBufC* body = NULL;
		if (pass == 1)
			{
			body = HBufC::New(len + iRuns * 6 + 8);
			if (!body)
				return;
			out.Set((TText*)body->Ptr(), 0, len + iRuns * 6 + 8);
			}
		TBool b = EFalse, it = EFalse, u = EFalse;
		TInt pos = 0;
		while (pos < len)
			{
			TPtrC view;
			TCharFormat cf;
			rt->GetChars(view, cf, pos);
			TInt n = view.Length();
			if (pos + n > len) n = len - pos;
			if (n <= 0)
				break;
			runs++;
			TBool nb = cf.iFontSpec.iFontStyle.StrokeWeight() == EStrokeWeightBold;
			TBool ni = cf.iFontSpec.iFontStyle.Posture() == EPostureItalic;
			TBool nu = cf.iFontPresentation.iUnderline == EUnderlineOn;
			if (pass == 1)
				{
				if (u && !nu) out.Append(0x19);
				if (it && !ni) out.Append(0x14);
				if (b && !nb) out.Append(0x12);
				if (!b && nb) out.Append(0x11);
				if (!it && ni) out.Append(0x13);
				if (!u && nu) out.Append(0x18);
				for (TInt i = 0; i < n; i++)
					{
					TText c = view[i];
					if (c == CEditableText::EParagraphDelimiter || c == CEditableText::ELineBreak) out.Append('\n');
					else if (c >= 0x20 || c == '\t') out.Append(c);
					}
				}
			b = nb; it = ni; u = nu;
			pos += n;
			}
		if (pass == 0)
			iRuns = runs;
		else
			{
			if (u) out.Append(0x19);
			if (it) out.Append(0x14);
			if (b) out.Append(0x12);
			body->Des().SetLength(out.Length());   // (written through its own pointer)
			delete iDraft.iBody;
			iDraft.iBody = body;
			}
		}
	}

TBool CPmComposeDialog::OkToExitL(TInt aButtonId)
	{
	Collect();
	if (aButtonId == EPmBidAttach)
		{
		// with some attached already: add another, or take one off
		TInt choice = 0;
		TInt n = iDraft.iAttach->Count();
		if (n)
			{
			CDesCArrayFlat* items = new(ELeave) CDesCArrayFlat(n + 1);
			CleanupStack::PushL(items);
			items->AppendL(_L("Add a file..."));
			for (TInt i = 0; i < n; i++)
				{
				TParsePtrC parse((*iDraft.iAttach)[i]);
				TBuf<60> t(_L("Remove "));
				t.Append(Clip(parse.NameAndExt(), 50));
				items->AppendL(t);
				}
			CleanupStack::Pop();              // the dialog's choice list takes items
			CPmChoiceDialog* cd = new(ELeave) CPmChoiceDialog(_L("Attachments"), _L("Attachments"), items, choice);
			if (!cd->ExecuteLD(R_PM_CHOICE_DIALOG))
				return EFalse;
			}
		if (choice > 0)
			iDraft.iAttach->Delete(choice - 1);
		else
			{
			TFileName name;
			name = _L("C:\\Documents\\");
			CEikFileOpenDialog* dlg = new(ELeave) CEikFileOpenDialog(&name);
			if (dlg->ExecuteLD(R_EIK_DIALOG_FILE_OPEN))
				{
				if (iDraft.iAttach->Count() >= 8)
					iEikonEnv->InfoMsg(_L("At most 8 attachments"));
				else
					iDraft.iAttach->AppendL(name);
				}
			}
		ShowAttachments();
		return EFalse;                  // stay in the dialog
		}
	if (aButtonId == EPmBidContacts)
		{
		ContactsL(EFalse);
		return EFalse;                  // stay in the dialog
		}
	if (aButtonId == EPmBidSend)
		{
		iDraft.iTo.Trim();
		if (iDraft.iTo.Length() == 0 || iDraft.iTo.Locate('@') < 0)
			{
			iEikonEnv->InfoMsg(_L("No address entered"));
			TryChangeFocusToL(EPmDlgTo);
			return EFalse;
			}
		}
	if (aButtonId == EEikBidCancel)
		{
		// (only if something was written: the style guide confirms only
		// what would be lost)
		if (StateSum() != iSum0)
			return iEikonEnv->QueryWinL(_L("Save as draft keeps it in the Outbox"), _L("Discard this message?"));
		}
	return ETrue;
	}

// ----- preferences ---------------------------------------------------------------

void CPmPrefsDialog::PreLayoutDynInitL()
	{
	SetChoiceListCurrentItem(EPmDlgSort, iSettings.iSort >= 0 && iSettings.iSort <= 6 ? iSettings.iSort : 0);
	SetNumberEditorValue(EPmDlgPrefetch, PrefetchCount(iSettings));
	SetChoiceListCurrentItem(EPmDlgStore, iSettings.iStore ? 1 : 0);
	SetChoiceListCurrentItem(EPmDlgPictures, iSettings.iSpare[0] >= 0 && iSettings.iSpare[0] <= 2 ? iSettings.iSpare[0] : 0);
	SetChoiceListCurrentItem(EPmDlgEmailButton, (iSettings.iView & KPmViewEmailButton) ? 0 : 1);
	NewMailInitL();                           // the New mail page (pmauto.cpp)
	}

TBool CPmPrefsDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iSort = ChoiceListCurrentItem(EPmDlgSort);
	TInt ahead = NumberEditorValue(EPmDlgPrefetch);
	iSettings.iPrefetch = ahead > 0 ? ahead : -1;
	iSettings.iStore = ChoiceListCurrentItem(EPmDlgStore);
	iSettings.iSpare[0] = ChoiceListCurrentItem(EPmDlgPictures);   // pictures: 0 shown, 1 only attached files, 2 none
	if (ChoiceListCurrentItem(EPmDlgEmailButton) == 0)
		iSettings.iView |= KPmViewEmailButton;
	else
		iSettings.iView &= ~KPmViewEmailButton;
	NewMailSave();
	return ETrue;
	}

// ----- account ---------------------------------------------------------------

void CPmAccountDialog::SetText(TInt aId, const char* aText)
	{
	TBuf<200> t;
	FromC(t, aText);
	TRAPD(err, SetEdwinTextL(aId, &t));
	}

void CPmAccountDialog::GetText(TInt aId, char* aText, TInt aMax)
	{
	TBuf<200> t;
	GetEdwinText(t, aId);
	t.Trim();
	CopyToC(aText, aMax, t);
	}

void CPmAccountDialog::PreLayoutDynInitL()
	{
	SetText(EPmDlgName, iAcct.name);
	SetText(EPmDlgFullName, iAcct.fullname);
	SetText(EPmDlgEmail, iAcct.email);
	SetText(EPmDlgImapHost, iAcct.imap_host);
	SetNumberEditorValue(EPmDlgImapPort, iAcct.imap_port);
	SetChoiceListCurrentItem(EPmDlgImapTls, iAcct.imap_tls);
	SetText(EPmDlgSmtpHost, iAcct.smtp_host);
	SetNumberEditorValue(EPmDlgSmtpPort, iAcct.smtp_port);
	SetChoiceListCurrentItem(EPmDlgSmtpTls, iAcct.smtp_tls);
	SetText(EPmDlgUser, iAcct.user);
	SetNumberEditorValue(EPmDlgSyncCount, iAcct.sync_count);
	SetNumberEditorValue(EPmDlgBodyKb, iAcct.max_body_kb);
	SetChoiceListCurrentItem(EPmDlgSaveSent, iAcct.save_sent ? 1 : 0);
	// the signature's lines are kept as "\n"
	TBuf<200> sig;
	FromC(sig, iAcct.signature);
	TInt at;
	while ((at = sig.Find(_L("\\n"))) >= 0)
		sig.Replace(at, 2, _L(" / "));
	TRAPD(err, SetEdwinTextL(EPmDlgSignature, &sig));
	}

TBool CPmAccountDialog::OkToExitL(TInt /*aButtonId*/)
	{
	TBuf<100> email;
	GetEdwinText(email, EPmDlgEmail);
	email.Trim();
	if (email.Locate('@') < 1)
		{
		iEikonEnv->InfoMsg(_L("No email address entered"));
		return EFalse;
		}
	GetText(EPmDlgName, iAcct.name, sizeof(iAcct.name));
	GetText(EPmDlgFullName, iAcct.fullname, sizeof(iAcct.fullname));
	GetText(EPmDlgEmail, iAcct.email, sizeof(iAcct.email));
	GetText(EPmDlgImapHost, iAcct.imap_host, sizeof(iAcct.imap_host));
	iAcct.imap_port = NumberEditorValue(EPmDlgImapPort);
	iAcct.imap_tls = ChoiceListCurrentItem(EPmDlgImapTls);
	GetText(EPmDlgSmtpHost, iAcct.smtp_host, sizeof(iAcct.smtp_host));
	iAcct.smtp_port = NumberEditorValue(EPmDlgSmtpPort);
	iAcct.smtp_tls = ChoiceListCurrentItem(EPmDlgSmtpTls);
	GetText(EPmDlgUser, iAcct.user, sizeof(iAcct.user));
	if (!iAcct.user[0])
		CopyToC(iAcct.user, sizeof(iAcct.user), email);
	// servers left empty: the usual names for the address's domain
	TPtrC domain = email.Mid(email.Locate('@') + 1);
	TBuf<80> guess;
	if (!iAcct.imap_host[0])
		{
		guess = _L("imap.");
		guess.Append(domain.Left(domain.Length() < 70 ? domain.Length() : 70));
		CopyToC(iAcct.imap_host, sizeof(iAcct.imap_host), guess);
		}
	if (!iAcct.smtp_host[0])
		{
		guess = _L("smtp.");
		guess.Append(domain.Left(domain.Length() < 70 ? domain.Length() : 70));
		CopyToC(iAcct.smtp_host, sizeof(iAcct.smtp_host), guess);
		}
	if (!iAcct.name[0])
		CopyToC(iAcct.name, sizeof(iAcct.name), domain);
	TBuf<32> pw;
	GetSecretEditorText(pw, EPmDlgPass);
	if (pw.Length())                     // blank keeps the saved password
		CopyToC(iAcct.pass, sizeof(iAcct.pass), pw);
	iAcct.sync_count = NumberEditorValue(EPmDlgSyncCount);
	iAcct.max_body_kb = NumberEditorValue(EPmDlgBodyKb);
	iAcct.save_sent = ChoiceListCurrentItem(EPmDlgSaveSent);
	TBuf<200> sig;
	GetEdwinText(sig, EPmDlgSignature);
	sig.Trim();
	TInt at;
	while ((at = sig.Find(_L(" / "))) >= 0)
		sig.Replace(at, 3, _L("\\n"));
	CopyToC(iAcct.signature, sizeof(iAcct.signature), sig);
	iAcct.used = 1;
	return ETrue;
	}

void CPmConnDialog::PreLayoutDynInitL()
	{
	SetChoiceListCurrentItem(EPmDlgLink, iSettings.iNetMode ? 1 : 0);
	SetChoiceListCurrentItem(EPmDlgBaud, iSettings.iBaudIndex);
	SetChoiceListCurrentItem(EPmDlgFlow, iSettings.iRtsCts ? 1 : 0);
	SetEdwinTextL(EPmDlgPppStart, &iPppStart);
	}

// Test: tries the values shown (not yet saved). If the mail engine has the
// port (mid-connection), the test says the port is in use.
void CPmConnDialog::TestL()
	{
	TBuf<40> ppp;
	GetEdwinText(ppp, EPmDlgPppStart);
	ppp.TrimAll();
	PgLinkTestL(ChoiceListCurrentItem(EPmDlgBaud), ChoiceListCurrentItem(EPmDlgFlow) == 1,
		ChoiceListCurrentItem(EPmDlgLink) == 1, ppp, R_PM_TEST_DIALOG, EPmDlgTest1);
	}

TBool CPmConnDialog::OkToExitL(TInt aButtonId)
	{
	if (aButtonId == EPmBidTest)
		{
		TestL();
		return EFalse;
		}
	iSettings.iNetMode = ChoiceListCurrentItem(EPmDlgLink) == 1;
	iSettings.iBaudIndex = ChoiceListCurrentItem(EPmDlgBaud);
	iSettings.iRtsCts = ChoiceListCurrentItem(EPmDlgFlow) == 1;
	TBuf<40> ppp;
	GetEdwinText(ppp, EPmDlgPppStart);
	ppp.TrimAll();
	iPppStart = ppp;
	return ETrue;
	}

// ============================================================================
// App UI
// ============================================================================

void CPmAppUi::ConstructL()
	{
	BaseConstructL();
	LoadSettings();
	LoadCalSettings();
	TRAPD(mbm, iMbm = CPmMbm::NewL(iCoeEnv->FsSession(), Application()->BitmapStoreName()));
	(void)mbm;                               // (NULL: the pictures load one by one)
	TRAPD(pics, ToolbarPicturesL());
	(void)pics;                              // (no PsiMail.mbm: words only)
	iView = new(ELeave) CPmView;
	iView->ConstructL(ClientRect(), iSettings, iCalSettings);
	AddToStackL(iView);
	iCoeEnv->RootWin().EnableOnEvents(EEventControlAlways);   // (0.68) switch-on events even when in the background
	if (!iSettings.iAccounts[iSettings.iAcct].used)
		{
		iAccountSetup = ETrue;               // (the Email icon question waits for this)
		TBool done = EFalse;
		TRAPD(err, done = EditAccountL(iSettings.iAcct, ETrue));
		iAccountSetup = EFalse;
		User::LeaveIfError(err);
		if (done)
			iView->AccountChangedL();
		EmailButtonSoon();
		}
	}

// ----- the Email icon below the screen ----------------------------------------
//
// The System screen opens the built-in Email program when the Email icon
// below the screen is tapped. With "Email icon opens: PsiMail" in
// Preferences, pmbutton.exe (mail/button) runs in the background and takes
// that key instead: PsiMail is brought to the front, or started. The setting
// is kept in iView, and as the file Button.ini, which is what tells
// pmbutton.exe that it is wanted; PsiMail starts it each time it opens
// with the setting on (no boot hook is installed: see mail/README.md). Turning the setting off removes the file and stops the program:
// the Email icon is the System screen's again. Uninstalling removes the file
// too (an FN line in psimail.pkg), and pmbutton.exe stops when it goes.

_LIT(KButtonMarker, "C:\\System\\Apps\\PsiMail\\Button.ini");
_LIT(KButtonExe, "pmbutton.exe");

void CPmAppUi::ApplyEmailButton()
	{
	RFs& fs = iCoeEnv->FsSession();
	TApaTaskList tasks(iEikonEnv->WsSession());
	TApaTask helper = tasks.FindApp(KUidPmButton);
	if (iSettings.iView & KPmViewEmailButton)
		{
		fs.MkDirAll(KButtonMarker);
		RFile file;
		if (file.Create(fs, KButtonMarker, EFileWrite) == KErrNone)
			file.Close();                        // (there already: fine)
		if (!helper.Exists())
			{
			// it lives next to the app
			TParse parse;
			parse.Set(Application()->AppFullName(), NULL, NULL);
			TFileName exe(parse.DriveAndPath());
			exe.Append(KButtonExe);
			RProcess process;
			if (process.Create(exe, KNullDesC) == KErrNone)
				{
				process.Resume();
				process.Close();
				}
			}
		}
	else
		{
		fs.Delete(KButtonMarker);
		if (helper.Exists())
			helper.SendSystemEvent(EApaSystemEventShutdown);
		}
	}

// Once the window is up (CPmView::FinishStartL): the first run after
// installing asks about the Email icon, once (the answer, either way, is kept;
// a fresh install, whose settings are gone, asks again); then pmbutton.exe
// is started or stopped as the setting says. From an idle callback of its
// own, not the view's tick: a dialog's nested loop inside the tick would
// hold the tick up, and with it the heartbeat the engine watches. When
// PsiMail opens with no account, the account dialog comes first and the
// question when it is closed (ConstructL calls this again).
void CPmAppUi::EmailButtonSoon()
	{
	if (!iSoon)
		iSoon = CIdle::New(CActive::EPriorityIdle);
	if (iSoon && !iSoon->IsActive())
		iSoon->Start(TCallBack(EmailButtonCallback, this));
	}

TInt CPmAppUi::EmailButtonCallback(TAny* aSelf)
	{
	CPmAppUi* self = (CPmAppUi*)aSelf;
	TRAPD(err, self->EmailButtonStartL());
	(void)err;
	return EFalse;                            // (once)
	}

void CPmAppUi::EmailButtonStartL()
	{
	// (not on top of the first account's dialog: asked when that is closed)
	if (!(iSettings.iView & KPmViewEmailButtonAsked) && !iAccountSetup)
		{
		iSettings.iView |= KPmViewEmailButtonAsked;
		if (iEikonEnv->QueryWinL(_L("The Email icon can open PsiMail instead of Email"), _L("Use the Email icon for PsiMail?")))
			iSettings.iView |= KPmViewEmailButton;
		SaveSettings();
		}
	ApplyEmailButton();
	}

// a toolbar button's picture, from PsiMail.mbm (made by tools/mkicons.py)
void CPmAppUi::ButtonPictureL(TInt aId, TInt aIcon, const TDesC* aText)
	{
	CEikCommandButton* b = iToolBar ? (CEikCommandButton*)iToolBar->ControlById(aId) : NULL;
	if (!b)
		return;
	if (aText)
		b->SetTextL(*aText);
	TFileName mbm = Application()->BitmapStoreName();
	CFbsBitmap* bmp = iMbm ? iMbm->CreateBitmapL(aIcon) : iEikonEnv->CreateBitmapL(mbm, aIcon);
	CleanupStack::PushL(bmp);
	CFbsBitmap* mask = iMbm ? iMbm->CreateBitmapL(aIcon + 1) : iEikonEnv->CreateBitmapL(mbm, aIcon + 1);
	CleanupStack::PushL(mask);
	b->SetPictureL(bmp, mask);                // (the button owns them now)
	CleanupStack::Pop(2);
	// the picture in the middle of its side, the words beside it (as the
	// built-in programs' buttons)
	if (b->Picture())
		b->Picture()->SetAlignment(EHCenterVCenter);
	if (b->Label())
		b->Label()->SetAlignment(EHLeftVCenter);
	b->LayoutComponentsL();
	}

void CPmAppUi::ToolbarPicturesL()
	{
	if (!iToolBar)
		return;
	ButtonPictureL(EPmCmdNewPopup, EMbmToolNew);
	ButtonPictureL(EPmCmdReplyPopup, EMbmToolReply);
	ButtonPictureL(EPmCmdSendRecv, EMbmToolCheck);
	ButtonPictureL(EPmCmdTool4, EMbmToolDelete);
	iToolBar->DrawNow();
	}

// the last button: Delete in a list, Close (back to the list) in a message
void CPmAppUi::SetTool4L(TBool aClose)
	{
	if (!iToolBar || aClose == iTool4Close)
		return;
	iTool4Close = aClose;
	TPtrC text(aClose ? _L("Close") : _L("Delete"));
	TRAPD(err, ButtonPictureL(EPmCmdTool4, aClose ? EMbmToolBack : EMbmToolDelete, &text));
	(void)err;
	CCoeControl* b = iToolBar->ControlById(EPmCmdTool4);
	if (b && iToolBar->IsVisible())
		b->DrawNow();
	}

CCoeControl* CPmAppUi::ToolBarButton(TInt aId)
	{
	return iToolBar ? iToolBar->ControlById(aId) : NULL;
	}

// the standard toolbar (View > Show toolbar hides it: the view takes its room)
void CPmAppUi::ShowToolBar(TBool aShow)
	{
	if (iSettings.iView & 1)
		aShow = EFalse;
	if (iToolBar && iToolBar->IsVisible() != aShow)
		iToolBar->MakeVisible(aShow);
	if (iView)
		{
		TRect r = ClientRect();
		if (!aShow)
			r.iBr.iX = iEikonEnv->ScreenDevice()->SizeInPixels().iWidth;   // (its room too)
		TRAPD(err, iView->SetRectL(r));
		(void)err;
		}
	}

// The Psion was switched back on (0.68): the engine re-checks the link
// (PPP up? modem carrier?) rather than wait on a dead connection.
void CPmAppUi::HandleSwitchOnEventL(CCoeControl* aDestination)
	{
	(void)aDestination;                  // (the CONE default does nothing, and is private)
	if (iView && iView->Shared())
		iView->Shared()->net.switch_on++;
	}

CPmAppUi::~CPmAppUi()
	{
	delete iSoon;
	if (iView)
		{
		RemoveFromStack(iView);
		delete iView;
		}
	delete iContacts;
	delete iMbm;
	PmDeletePrinter(iPrinter);
	}

CPmContacts* CPmAppUi::Contacts()
	{
	if (!iContacts)
		{
		TRAPD(err, iContacts = CPmContacts::NewL());
		(void)err;
		}
	return iContacts;
	}

// ----- mailto: links (the reader's, and PsiWeb's) ---------------------------------

// %xx and + in a mailto: part
static void UrlDecode(TDes& aOut, const TDesC& aIn, TBool aPlusIsSpace)
	{
	for (TInt i = 0; i < aIn.Length() && aOut.Length() < aOut.MaxLength(); i++)
		{
		TText c = aIn[i];
		if (c == '%' && i + 2 < aIn.Length())
			{
			TLex lex(aIn.Mid(i + 1, 2));
			TUint v;
			if (lex.Val(v, EHex) == KErrNone)
				{
				c = (TText)v;
				i += 2;
				}
			}
		else if (c == '+' && aPlusIsSpace)
			c = ' ';
		if (c == '\r')
			continue;
		aOut.Append(c);
		}
	}

// mailto:someone@example.com?subject=Hello&cc=...&body=...
void CPmAppUi::MailtoL(const TDesC& aUrl)
	{
	TPtrC u = aUrl;
	if (Clip(u, 7).CompareF(_L("mailto:")) == 0)
		u.Set(u.Mid(7));
	CPmDraft* d = CPmDraft::NewL();
	CleanupStack::PushL(d);
	TInt q = u.Locate('?');
	TPtrC to = q >= 0 ? u.Left(q) : u;
	TPtrC rest = q >= 0 ? u.Mid(q + 1) : TPtrC();
	UrlDecode(d->iTo, to, EFalse);
	HBufC* body = HBufC::NewL(4096);
	TPtr bp = body->Des();
	while (rest.Length())
		{
		TInt amp = rest.Locate('&');
		TPtrC pair = amp >= 0 ? rest.Left(amp) : rest;
		rest.Set(amp >= 0 ? rest.Mid(amp + 1) : TPtrC());
		TInt eq = pair.Locate('=');
		if (eq < 0)
			continue;
		TPtrC key = pair.Left(eq);
		TPtrC val = pair.Mid(eq + 1);
		if (key.CompareF(_L("subject")) == 0) UrlDecode(d->iSubject, val, ETrue);
		else if (key.CompareF(_L("cc")) == 0) UrlDecode(d->iCc, val, EFalse);
		else if (key.CompareF(_L("bcc")) == 0) UrlDecode(d->iBcc, val, EFalse);
		else if (key.CompareF(_L("to")) == 0)
			{
			if (d->iTo.Length()) d->iTo.Append(_L(", "));
			UrlDecode(d->iTo, val, EFalse);
			}
		else if (key.CompareF(_L("body")) == 0) UrlDecode(bp, val, ETrue);
		}
	if (bp.Length() == 0 || bp[bp.Length() - 1] != '\n')
		bp.Append('\n');
	AddSignature(*d, bp);
	delete d->iBody;
	d->iBody = body;
	CleanupStack::Pop();                      // d
	ComposeL(d, _L("New message"));
	}

// PsiWeb (or anyone) hands PsiMail a mailto: link as a message ...
void CPmAppUi::ProcessMessageL(TUid aUid, const TDesC8& aParams)
	{
	if (aUid != KUidPsiMail)
		{
		CEikAppUi::ProcessMessageL(aUid, aParams);
		return;
		}
	TBuf<500> url;
	url.Copy(Clip(aParams, url.MaxLength()));
	url.Trim();
	if (Clip(url, 7).CompareF(_L("mailto:")) == 0 && iView && iView->Mode() != CPmView::ENoAccount)
		MailtoL(url);
	}

// ... or starts it with one on the command line
TBool CPmAppUi::ProcessCommandParametersL(TApaCommand aCommand, TFileName& aDocumentName, const TDesC8& aTail)
	{
	TBuf<500> url;
	url.Copy(Clip(aTail, url.MaxLength()));
	url.Trim();
	if (Clip(url, 7).CompareF(_L("mailto:")) == 0 && iView && iView->Mode() != CPmView::ENoAccount)
		{
		TRAPD(err, MailtoL(url));
		(void)err;
		return EFalse;
		}
	return CEikAppUi::ProcessCommandParametersL(aCommand, aDocumentName, aTail);
	}

// passwords are kept scrambled (not encrypted: anyone with the Psion can
// read them, as with PsiTerm's saved passwords)
static void Scramble(PmAccount& aAcct)
	{
	for (TInt i = 0; i < (TInt)sizeof(aAcct.pass); i++)
		aAcct.pass[i] ^= (char)(0x5a + i * 7);
	}

void CPmAppUi::LoadSettings()
	{
	Mem::FillZ(&iSettings, sizeof(iSettings));
	iSettings.iBaudIndex = 4;           // 115200, as PsiTerm recommends
	RFile file;
	if (file.Open(iCoeEnv->FsSession(), KIniFile, EFileRead) != KErrNone)
		{
		UseSharedLink(iCoeEnv->FsSession(), iSettings, NULL);
		return;
		}
	TPckgBuf<TUint32> magic;
	TPckgBuf<TInt> size;
	if (file.Read(magic) == KErrNone && magic() == KIniMagic &&
		file.Read(size) == KErrNone && size() == (TInt)sizeof(TPmSettings))
		{
		TPckg<TPmSettings> s(iSettings);
		if (file.Read(s) != KErrNone || s.Length() != (TInt)sizeof(TPmSettings))
			Mem::FillZ(&iSettings, sizeof(iSettings));
		else
			for (TInt i = 0; i < PM_MAX_ACCOUNTS; i++)
				Scramble(iSettings.iAccounts[i]);
		}
	file.Close();
	if (iSettings.iAcct < 0 || iSettings.iAcct >= PM_MAX_ACCOUNTS)
		iSettings.iAcct = 0;
	UseSharedLink(iCoeEnv->FsSession(), iSettings, NULL);
	}

void CPmAppUi::SaveSettings()
	{
	RFs& fs = iCoeEnv->FsSession();
	fs.MkDirAll(KIniFile);
	RFile file;
	if (file.Replace(fs, KIniFile, EFileWrite) != KErrNone)
		return;
	TPckgBuf<TUint32> magic(KIniMagic);
	TPckgBuf<TInt> size(sizeof(TPmSettings));
	file.Write(magic);
	file.Write(size);
	for (TInt i = 0; i < PM_MAX_ACCOUNTS; i++)
		Scramble(iSettings.iAccounts[i]);
	file.Write(TPckgC<TPmSettings>(iSettings));
	for (TInt i2 = 0; i2 < PM_MAX_ACCOUNTS; i2++)
		Scramble(iSettings.iAccounts[i2]);
	file.Close();
	}

// ----- calendar settings (a file of their own, so mail settings stay put)

_LIT(KCalIniFile, "C:\\System\\Apps\\PsiMail\\Calendar.ini");
const TUint32 KCalIniMagic = 0x31435350;   // 'PSC1'

static void ScrambleCal(PmCalendar& aCal)
	{
	for (TInt i = 0; i < (TInt)sizeof(aCal.pass); i++)
		aCal.pass[i] ^= (char)(0x3c + i * 5);
	}

// the time zone list's entry for the Psion's own home city (a guess)
static TInt GuessZone()
	{
	static const TInt16 KStd[] = { 0, 0, 60, 120, 180, 240, 330, 480, 540, 600, 600, 720,
		-600, -540, -480, -420, -420, -360, -300, -240, -180 };
	TLocale loc;
	TInt minutes = loc.UniversalTimeOffset().Int() / 60;
	if (loc.QueryHomeHasDaylightSavingOn())
		minutes -= 60;
	if (minutes == 0)
		return 1;                              // London, rather than UTC
	for (TInt i = 0; i < (TInt)(sizeof(KStd) / sizeof(KStd[0])); i++)
		if (KStd[i] == minutes)
			return i;
	return 0;
	}

// zeroes the settings - but a TBuf keeps its size in the object, so the
// Agenda file name is made again afterwards (zeroed, it could hold nothing:
// USER 23 on the first copy into it)
static void ClearCalSettings(TPmCalSettings& aCal)
	{
	Mem::FillZ(&aCal, sizeof(aCal));
	new(&aCal.iAgendaFile) TBuf<128>;
	}

void CPmAppUi::LoadCalSettings()
	{
	TPmCalSettings& c = iCalSettings;
	ClearCalSettings(c);
	RFile file;
	TBool ok = EFalse;
	if (file.Open(iCoeEnv->FsSession(), KCalIniFile, EFileRead) == KErrNone)
		{
		TPckgBuf<TUint32> magic;
		TPckgBuf<TInt> size;
		TPckg<TPmCalSettings> p(c);
		if (file.Read(magic) == KErrNone && magic() == KCalIniMagic &&
			file.Read(size) == KErrNone && size() == (TInt)sizeof(TPmCalSettings) &&
			file.Read(p) == KErrNone && p.Length() == (TInt)sizeof(TPmCalSettings))
			{
			ScrambleCal(c.iCal);
			// the file holds the descriptor's own header too: rebuild it
			// rather than trust it
			TBuf<128> agenda;
			TInt n = c.iAgendaFile.Length();
			if (n >= 0 && n <= 128 && c.iAgendaFile.MaxLength() == 128)
				agenda.Copy(TPtrC((const TText*)c.iAgendaFile.Ptr(), n));
			new(&c.iAgendaFile) TBuf<128>(agenda);
			ok = agenda.Length() > 0;
			}
		file.Close();
		}
	if (!ok)
		{
		ClearCalSettings(c);
		c.iCal.port = 443;
		c.iCal.zone = GuessZone();
		c.iCal.days_back = 30;
		c.iCal.days_ahead = 180;
		c.iCal.acct = iSettings.iAcct;
		CPmCalSync::DefaultAgendaFile(c.iAgendaFile);
		c.iAlarms = 1;
		}
	}

void CPmAppUi::SaveCalSettings()
	{
	RFs& fs = iCoeEnv->FsSession();
	fs.MkDirAll(KCalIniFile);
	RFile file;
	if (file.Replace(fs, KCalIniFile, EFileWrite) != KErrNone)
		return;
	TPckgBuf<TUint32> magic(KCalIniMagic);
	TPckgBuf<TInt> size(sizeof(TPmCalSettings));
	file.Write(magic);
	file.Write(size);
	ScrambleCal(iCalSettings.iCal);
	file.Write(TPckgC<TPmCalSettings>(iCalSettings));
	ScrambleCal(iCalSettings.iCal);
	file.Close();
	}

void CPmAppUi::EditCalendarL()
	{
	TPmCalSettings c = iCalSettings;
	TBuf<100> dir;
	iView->StoreDirectory(dir);
	CPmCalDialog* dlg = new(ELeave) CPmCalDialog(c, dir);
	if (!dlg->ExecuteLD(R_PM_CAL_DIALOG))
		return;
	TBool turnedOn = c.iCal.enabled && !iCalSettings.iCal.enabled;
	TBuf<128> oldFile(iCalSettings.iAgendaFile);
	if (c.iAgendaFile.CompareF(oldFile) != 0)
		CPmCalSync::ForgetL(dir);            // another file: start the links again
	// (a new time zone needs nothing: the engine fetches everything again
	// and the entries are corrected like any other server change)
	if (turnedOn)
		c.iCal.acct = iSettings.iAcct;
	iCalSettings = c;
	SaveCalSettings();
	iView->SettingsChanged();
	if (turnedOn && iEikonEnv->QueryWinL(_L("Calendar sync is on"), _L("Sync the calendar with the Agenda now?")))
		iView->CalendarSyncL();
	}

CPmCalDialog::~CPmCalDialog()
	{
	delete iIds;
	}

void CPmCalDialog::PreLayoutDynInitL()
	{
	SetChoiceListCurrentItem(EPmDlgCalOn, iCal.iCal.enabled ? 1 : 0);
	// host[:port][/path]
	TBuf<200> host;
	FromC(host, iCal.iCal.host);
	if (iCal.iCal.port && iCal.iCal.port != 443)
		{
		host.Append(':');
		host.AppendNum(iCal.iCal.port);
		}
	TBuf<130> path;
	FromC(path, iCal.iCal.path);
	host.Append(path);
	SetEdwinTextL(EPmDlgCalHost, &host);
	SetEdwinTextL(EPmDlgCalFile, &iCal.iAgendaFile);
	SetChoiceListCurrentItem(EPmDlgCalZone, iCal.iCal.zone);
	SetNumberEditorValue(EPmDlgCalBack, iCal.iCal.days_back);
	SetNumberEditorValue(EPmDlgCalAhead, iCal.iCal.days_ahead);
	SetChoiceListCurrentItem(EPmDlgCalAlarms, iCal.iAlarms ? 1 : 0);
	SetChoiceListCurrentItem(EPmDlgCalExisting, iCal.iCopyExisting ? 1 : 0);
	// the calendars found at the last sync
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(4);
	CleanupStack::PushL(names);
	iIds = new(ELeave) CDesC8ArrayFlat(4);
	TInt def = 0;
	CPmCalSync::CalendarsL(iStoreDir, *names, *iIds, def);
	if (names->Count() > 0)
		{
		CEikChoiceList* cl = (CEikChoiceList*)Control(EPmDlgCalDefault);
		CleanupStack::Pop();                 // names: the list owns it now
		cl->SetArrayL(names);
		cl->SetCurrentItem(def >= 0 ? def : 0);
		}
	else
		CleanupStack::PopAndDestroy();       // names
	}

TBool CPmCalDialog::OkToExitL(TInt /*aButtonId*/)
	{
	TBuf<200> host;
	GetEdwinText(host, EPmDlgCalHost);
	host.Trim();
	TInt on = ChoiceListCurrentItem(EPmDlgCalOn);
	if (on && host.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No calendar server entered"));
		return EFalse;
		}
	TBuf<128> file;
	GetEdwinText(file, EPmDlgCalFile);
	file.Trim();
	if (on)
		{
		TEntry e;
		if (file.Length() == 0 || iEikonEnv->FsSession().Entry(file, e) != KErrNone)
			{
			iEikonEnv->InfoMsg(_L("No Agenda file there - open it in Agenda first"));
			return EFalse;
			}
		}
	// "https://host:port/path" is allowed: the path is where to look for
	// calendars. (Always TLS: a calendar password shouldn't go in the clear.)
	TInt scheme = host.Find(_L("://"));
	if (scheme >= 0) host.Delete(0, scheme + 3);
	TInt slash = host.Locate('/');
	iCal.iCal.path[0] = 0;
	if (slash >= 0)
		{
		CopyToC(iCal.iCal.path, sizeof(iCal.iCal.path), host.Mid(slash));
		host.SetLength(slash);
		}
	iCal.iCal.port = 443;
	TInt colon = host.Locate(':');
	if (colon >= 0)
		{
		TLex lex(host.Mid(colon + 1));
		TInt port = 443;
		if (lex.Val(port) == KErrNone && port > 0 && port < 65536)
			iCal.iCal.port = port;
		host.SetLength(colon);
		}
	CopyToC(iCal.iCal.host, sizeof(iCal.iCal.host), host);
	iCal.iCal.enabled = on;
	TBuf<32> pw;
	GetSecretEditorText(pw, EPmDlgCalPass);
	if (pw.Length())
		CopyToC(iCal.iCal.pass, sizeof(iCal.iCal.pass), pw);
	iCal.iAgendaFile = file;
	iCal.iCal.zone = ChoiceListCurrentItem(EPmDlgCalZone);
	iCal.iCal.days_back = NumberEditorValue(EPmDlgCalBack);
	iCal.iCal.days_ahead = NumberEditorValue(EPmDlgCalAhead);
	iCal.iAlarms = ChoiceListCurrentItem(EPmDlgCalAlarms);
	iCal.iCopyExisting = ChoiceListCurrentItem(EPmDlgCalExisting);
	if (iIds && iIds->Count() > 0)
		{
		TInt k = ChoiceListCurrentItem(EPmDlgCalDefault);
		if (k >= 0 && k < iIds->Count())
			CPmCalSync::SetDefaultCalendarL(iStoreDir, (*iIds)[k]);
		}
	return ETrue;
	}

TBool CPmAppUi::EditAccountL(TInt aIndex, TBool aNew)
	{
	PmAccount a = iSettings.iAccounts[aIndex];
	if (aNew)
		{
		// the usual ports; the servers come from the address if left empty
		Mem::FillZ(&a, sizeof(a));
		a.imap_port = 993;
		a.imap_tls = PM_TLS_ON;
		a.smtp_port = 465;
		a.smtp_tls = PM_TLS_ON;
		a.sync_count = 50;
		a.max_body_kb = 64;
		a.save_sent = 1;
		}
	TInt store = iSettings.iStore;
	TInt ahead = PrefetchCount(iSettings);
	CPmAccountDialog* dlg = new(ELeave) CPmAccountDialog(a, store, ahead);
	if (!dlg->ExecuteLD(R_PM_ACCOUNT_DIALOG))
		return EFalse;
	iSettings.iAccounts[aIndex] = a;
	iSettings.iStore = store;
	iSettings.iPrefetch = ahead > 0 ? ahead : -1;
	SaveSettings();
	iView->SettingsChanged();
	return ETrue;
	}

void CPmAppUi::AddSignature(CPmDraft& /*aDraft*/, TDes& aBody)
	{
	PmAccount& a = iSettings.iAccounts[iSettings.iAcct];
	if (!a.signature[0])
		return;
	TBuf<200> sig;
	FromC(sig, a.signature);
	TInt at;
	while ((at = sig.Find(_L("\\n"))) >= 0)
		sig.Replace(at, 2, _L("\n"));
	if (aBody.Length() + sig.Length() + 5 < aBody.MaxLength())
		{
		aBody.Append(_L("\n-- \n"));
		aBody.Append(sig);
		aBody.Append('\n');
		}
	}

void CPmAppUi::ComposeL(CPmDraft* aDraft, const TDesC& aTitle)
	{
	// the standard EIKON dialog: To, Cc, Subject, the attachments, the text,
	// and Send / Save / Attach buttons on the right, like the built-in programs
	CleanupStack::PushL(aDraft);
	for (;;)
		{
		CPmComposeDialog* dlg = new(ELeave) CPmComposeDialog(*aDraft, aTitle);
		TInt r = dlg->ExecuteLD(R_PM_COMPOSE_DIALOG);
		if (r != EPmBidSend && r != EPmBidSave)
			break;
		TRAPD(err, iView->SaveDraftL(*aDraft, r == EPmBidSend));
		if (err == KErrNone)
			break;
		// not saved (the card full, or out): the message is still here -
		// say so and show it again, so nothing written is lost
		TBuf<100> why;
		if (err == KErrDiskFull)
			why = _L("The disk is full - make room, then try again");
		else if (err == KErrNotReady || err == KErrPathNotFound)
			why = _L("The disk is not present - is the card in?");
		else
			why.Format(_L("Not saved (%d) - try again"), err);
		iEikonEnv->InfoWinL(_L("Message not saved"), why);
		}
	CleanupStack::PopAndDestroy();          // the draft
	}

void CPmAppUi::NewMessageL()
	{
	CPmDraft* d = CPmDraft::NewL();
	CleanupStack::PushL(d);
	HBufC* body = HBufC::NewL(400);
	TPtr p = body->Des();
	p.Append('\n');
	AddSignature(*d, p);
	delete d->iBody;
	d->iBody = body;
	CleanupStack::Pop();
	ComposeL(d, _L("New message"));
	}

// The open message's headers, on the heap: the app's stack is only 8 KB and
// the compose dialog still has to run on it.
struct THdrs
	{
	TBuf<500> iFrom, iReplyTo, iTo, iCc, iSubject, iMsgId, iDate;
	TBuf<200> iAddr, iAddr2, iItem;
	};

static THdrs* HeadersLC(CPmView& aView)
	{
	THdrs* h = new(ELeave) THdrs;
	CleanupStack::PushL(h);
	aView.MessageHeader(_L("From"), h->iFrom);
	aView.MessageHeader(_L("Reply-To"), h->iReplyTo);
	aView.MessageHeader(_L("To"), h->iTo);
	aView.MessageHeader(_L("Cc"), h->iCc);
	aView.MessageHeader(_L("Subject"), h->iSubject);
	aView.MessageHeader(_L("Message-ID"), h->iMsgId);
	aView.MessageHeader(_L("Date"), h->iDate);
	return h;
	}

// the next address of a list, skipping commas inside quotes and <>
static TPtrC NextAddress(TPtrC& aRest)
	{
	TInt i = 0, q = 0, ang = 0;
	while (i < aRest.Length())
		{
		TText c = aRest[i];
		if (c == '"') q = !q;
		else if (c == '<') ang = 1;
		else if (c == '>') ang = 0;
		else if ((c == ',' || c == ';') && !q && !ang) break;
		i++;
		}
	TPtrC item = aRest.Left(i);
	aRest.Set(i < aRest.Length() ? aRest.Mid(i + 1) : TPtrC());
	return item;
	}

CPmDraft* CPmAppUi::ReplyDraftL(TBool aAll)
	{
	const TPmRow* row = iView->CurrentRow();
	if (!row || (iView->Mode() != CPmView::EList && iView->Mode() != CPmView::EMessage))
		{
		iView->Toast(_L("No message selected"));
		return NULL;
		}
	if (iView->Mode() == CPmView::EList)
		iView->OpenCurrentL();                  // the text is needed for the reply
	row = iView->CurrentRow();
	if (!row)
		return NULL;
	TUint uid = row->iUid;
	THdrs* h = HeadersLC(*iView);
	if (!h->iFrom.Length())
		{
		iView->Toast(_L("Not available until the message has downloaded"));
		CleanupStack::PopAndDestroy();         // h
		return NULL;
		}
	CPmDraft* d = CPmDraft::NewL();
	CleanupStack::PushL(d);
	SafeCopy(d->iTo, h->iReplyTo.Length() ? TPtrC(h->iReplyTo) : TPtrC(h->iFrom));
	if (aAll)
		{
		// everyone else, not me and not who it goes to already
		TBuf<100> me;
		FromC(me, iSettings.iAccounts[iSettings.iAcct].email);
		AddressOnly(h->iAddr2, d->iTo);
		for (TInt pass = 0; pass < 2; pass++)
			{
			TPtrC rest = pass == 0 ? TPtrC(h->iTo) : TPtrC(h->iCc);
			while (rest.Length())
				{
				TPtrC item = NextAddress(rest);
				AddressOnly(h->iAddr, item);
				if (h->iAddr.Length() == 0 || h->iAddr.CompareF(me) == 0 || h->iAddr.CompareF(h->iAddr2) == 0)
					continue;
				SafeCopy(h->iItem, item);
				h->iItem.Trim();
				if (d->iCc.Length() + h->iItem.Length() + 2 < d->iCc.MaxLength())
					{
					if (d->iCc.Length()) d->iCc.Append(_L(", "));
					d->iCc.Append(h->iItem);
					}
				}
			}
		}
	if (Clip(h->iSubject, 3).CompareF(_L("Re:")) != 0)
		d->iSubject = _L("Re: ");
	d->iSubject.Append(Clip(h->iSubject, d->iSubject.MaxLength() - d->iSubject.Length()));
	SafeCopy(d->iInReplyTo, h->iMsgId);
	SafeCopy(d->iReferences, h->iMsgId);
	SafeCopy(d->iReplyFolder, iView->FolderImap());
	d->iReplyUid = uid;
	HBufC* body = HBufC::NewL(24 * 1024);
	TPtr p = body->Des();
	p.Append('\n');
	AddSignature(*d, p);
	TBuf<120> who;
	DisplayName(who, h->iFrom);
	p.Append('\n');
	p.Append(_L("On "));
	p.Append(Clip(h->iDate, 40));
	p.Append(_L(", "));
	p.Append(Clip(who, 60));
	p.Append(_L(" wrote:\n"));
	iView->PlainBodyL(p, ETrue);
	delete d->iBody;
	d->iBody = body;
	CleanupStack::Pop();                      // d
	CleanupStack::PopAndDestroy();            // h
	return d;
	}

void CPmAppUi::ReplyL(TBool aAll)
	{
	CPmDraft* d = ReplyDraftL(aAll);
	if (d)
		ComposeL(d, aAll ? _L("Reply to all") : _L("Reply"));
	}

CPmDraft* CPmAppUi::ForwardDraftL(CDesCArray* aFiles)
	{
	if (iView->Mode() == CPmView::EList && iView->CurrentRow())
		iView->OpenCurrentL();
	if (iView->Mode() != CPmView::EMessage)
		{
		iView->Toast(_L("No message selected"));
		return NULL;
		}
	THdrs* h = HeadersLC(*iView);
	if (!h->iFrom.Length())
		{
		iView->Toast(_L("Not available until the message has downloaded"));
		CleanupStack::PopAndDestroy();         // h
		return NULL;
		}
	CPmDraft* d = CPmDraft::NewL();
	CleanupStack::PushL(d);
	d->iSubject = _L("Fwd: ");
	d->iSubject.Append(Clip(h->iSubject, 190));
	HBufC* body = HBufC::NewL(32 * 1024);
	CleanupStack::PushL(body);
	TPtr p = body->Des();
	p.Append('\n');
	AddSignature(*d, p);
	p.Append(_L("\n---------- Forwarded message ----------\nFrom: "));
	p.Append(Clip(h->iFrom, 200));
	p.Append(_L("\nDate: "));
	p.Append(Clip(h->iDate, 60));
	p.Append(_L("\nSubject: "));
	p.Append(Clip(h->iSubject, 200));
	p.Append(_L("\nTo: "));
	p.Append(Clip(h->iTo, 200));
	p.Append(_L("\n\n"));
	// the text (leaving room for the note below)
	{
		TPtr room((TUint8*)p.Ptr(), p.Length(), p.MaxLength() - 100);
		iView->PlainBodyL(room, EFalse);
		p.SetLength(room.Length());
	}
	if (aFiles)
		for (TInt i = 0; i < aFiles->Count() && d->iAttach->Count() < 8; i++)
			d->iAttach->AppendL((*aFiles)[i]);
	else if (iView->AttachmentCount())
		p.Append(_L("\n(The attachments are not forwarded: save them first and attach them.)\n"));
	CleanupStack::Pop();                      // body
	delete d->iBody;
	d->iBody = body;
	CleanupStack::Pop();                      // d
	CleanupStack::PopAndDestroy();            // h
	return d;
	}

// Forward: with the message's attachments if wanted (they are downloaded
// first; ForwardReadyL follows)
void CPmAppUi::ForwardL()
	{
	if (iView->Mode() == CPmView::EList && iView->CurrentRow())
		iView->OpenCurrentL();
	TInt n = iView->AttachmentCount();
	if (n > 0 && iView->CurrentRow())
		{
		TBuf<60> t;
		if (n == 1) t = _L("This message has an attachment");
		else t.Format(_L("This message has %d attachments"), n);
		if (iEikonEnv->QueryWinL(t, n == 1 ? _L("Forward it too?") : _L("Forward them too?")))
			{
			iView->ForwardAttachmentsL(iView->CurrentRow()->iUid);
			return;
			}
		}
	CPmDraft* d = ForwardDraftL(NULL);
	if (d)
		ComposeL(d, _L("Forward"));
	}

void CPmAppUi::ForwardReadyL(TUint aUid, CDesCArray& aFiles)
	{
	if (iView->Mode() != CPmView::EMessage || !iView->CurrentRow() || iView->CurrentRow()->iUid != aUid)
		{
		iView->Toast(_L("The attachments are saved - forward the message again to send them"));
		return;
		}
	CPmDraft* d = ForwardDraftL(&aFiles);
	if (d)
		ComposeL(d, _L("Forward"));
	}

void CPmAppUi::MoveL()
	{
	if (!iView->CurrentRow() || (iView->Mode() != CPmView::EList && iView->Mode() != CPmView::EMessage))
		{
		iView->Toast(_L("No message selected"));
		return;
		}
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(8);
	CleanupStack::PushL(names);
	RArray<TInt> map;
	CleanupClosePushL(map);
	for (TInt i = 0; i < iView->FolderCount(); i++)
		{
		const TPmFolder& f = iView->FolderAt(i);
		if (f.iKind == 'N')
			continue;
		names->AppendL(f.iName);
		User::LeaveIfError(map.Append(i));
		}
	if (names->Count() == 0)
		{
		CleanupStack::PopAndDestroy(2);    // map, names
		iView->Toast(_L("No folders yet - check mail first"));
		return;
		}
	TInt choice = 0;
	// the dialog's choice list takes the names: pop them from under map
	CleanupStack::Pop(2);
	CleanupClosePushL(map);
	CPmChoiceDialog* dlg = new(ELeave) CPmChoiceDialog(_L("Move to folder"), _L("Folder"), names, choice);
	if (dlg->ExecuteLD(R_PM_CHOICE_DIALOG) && choice >= 0 && choice < map.Count())
		{
		TBuf8<128> dest(iView->FolderAt(map[choice]).iImap);
		iView->MoveCurrentL(dest);
		}
	CleanupStack::PopAndDestroy();          // map
	}

// Message > Attachments > Open / Save: which one
void CPmAppUi::AttachmentL(TBool aOpen)
	{
	if (iView->AttachmentCount() == 0)
		{
		iView->Toast(_L("This message has no attachments"));
		return;
		}
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(4);
	CleanupStack::PushL(names);
	iView->AttachmentsL(*names);
	TInt choice = 0;
	CleanupStack::Pop();
	CPmChoiceDialog* dlg = new(ELeave) CPmChoiceDialog(aOpen ? _L("Open attachment") : _L("Save attachment"), _L("Attachment"), names, choice);
	if (dlg->ExecuteLD(R_PM_CHOICE_DIALOG))
		{
		if (aOpen) iView->OpenAttachmentL(choice);
		else iView->SaveAttachmentL(choice);
		}
	}

// Edit > Add sender to Contacts: the open (or selected) message's From
void CPmAppUi::AddSenderL()
	{
	if (!iView->CurrentRow() || (iView->Mode() != CPmView::EList && iView->Mode() != CPmView::EMessage))
		{
		iView->Toast(_L("No message selected"));
		return;
		}
	// the From line: the open message's, else the folder index's (there
	// before the message is downloaded; the list needn't be left)
	TBuf<500> from;
	if (iView->Mode() == CPmView::EMessage)
		iView->MessageHeader(_L("From"), from);
	if (!from.Length())
		iView->IndexFromL(iView->CurrentRow()->iUid, from);
	if (!from.Length())
		{
		iView->Toast(_L("No sender address"));
		return;
		}
	TBuf<200> name, addr;
	DisplayName(name, from);
	AddressOnly(addr, from);
	TBuf<120> msg;
	Contacts()->AddL(name, addr, msg);
	if (msg.Length())
		iEikonEnv->InfoMsg(msg);
	}

// hands the checked PsiMail.sis to the system installer, and closes
// (PsiMail.app and psimail.exe are among the files it replaces)
void CPmView::StartInstallerL(const TDesC& aFile)
	{
	StopEngine();
	TInt err;
	RApaLsSession ls;
	err = ls.Connect();
	if (err == KErrNone)
		{
		TThreadId tid;
		err = ls.StartDocument(aFile, TUid::Uid(0x10000419), tid);
		ls.Close();
		}
	if (err == KErrNone)
		{
		iEikonEnv->EikAppUi()->HandleCommandL(EEikCmdExit);
		return;
		}
	TBuf<200> m;
	m.Format(_L("Could not start the installer (%d) - open %S from the System screen"), err, &aFile);
	iEikonEnv->InfoWinL(_L("Update downloaded"), m);
	StartEngineL();
	}

_LIT(KUpdIniFile, "C:\\System\\Apps\\PsiMail\\Update.ini");

// Tools > Update PsiMail: where from, then the engine fetches and checks it
void CPmAppUi::UpdateL()
	{
	// Update.ini holds "github", "github-dev" or "host:port"
	RFs& fs = iCoeEnv->FsSession();
	TBuf<60> src;
	RFile f;
	if (f.Open(fs, KUpdIniFile, EFileRead) == KErrNone)
		{
		TBuf8<60> b;
		f.Read(b);
		f.Close();
		src.Copy(b);
		src.Trim();
		}
	TInt source = 0, port = 8686;
	TBuf<50> host;
	if (src.CompareF(_L("github-dev")) == 0)
		source = 1;
	else if (src.Length() && src.CompareF(_L("github")) != 0)
		{
		source = 2;
		TInt c = src.Locate(':');
		host.Copy(src.Left(c >= 0 ? (c < 50 ? c : 50) : (src.Length() < 50 ? src.Length() : 50)));
		if (c >= 0)
			{
			TLex lex(src.Mid(c + 1));
			if (lex.Val(port) != KErrNone || port <= 0) port = 8686;
			}
		}
	CPmUpdateDialog* dlg = new(ELeave) CPmUpdateDialog(source, host, port);
	if (!dlg->ExecuteLD(R_PM_UPDATE_DIALOG))
		return;
	if (source == 0) src = _L("github");
	else if (source == 1) src = _L("github-dev");
	else
		{
		src = host;
		src.AppendFormat(_L(":%d"), port);
		}
	fs.MkDirAll(KUpdIniFile);
	if (f.Replace(fs, KUpdIniFile, EFileWrite) == KErrNone)
		{
		TBuf8<60> b;
		b.Copy(src);
		f.Write(b);
		f.Close();
		}
	// saved to the CF card if there is one: C: is small
	TVolumeInfo vol;
	TBool card = fs.Volume(vol, EDriveD) == KErrNone && vol.iFree > 600 * 1024;
	TBuf8<40> save(card ? _L8("D:\\PsiMail-update.sis") : _L8("C:\\PsiMail-update.sis"));
	TBuf8<60> arg;
	arg.Copy(src);
	iView->SetStatus(_L("Looking for a new PsiMail..."));
	iView->Cmd(PM_CMD_UPDATE, save, 0, arg);
	}

void CPmUpdateDialog::PreLayoutDynInitL()
	{
	SetChoiceListCurrentItem(EPmDlgUpdSource, iSource >= 0 && iSource <= 2 ? iSource : 0);
	SetEdwinTextL(EPmDlgUpdHost, &iHost);
	SetNumberEditorValue(EPmDlgUpdPort, iPort > 0 ? iPort : 8686);
	LocalLinesDimmed();
	}

// the Local server and Port lines belong to the "Local server" source: dimmed
// (not hidden) with the others, as the style guide has dependent lines
void CPmUpdateDialog::LocalLinesDimmed()
	{
	TBool local = ChoiceListCurrentItem(EPmDlgUpdSource) == 2;
	SetLineDimmedNow(EPmDlgUpdHost, !local);
	SetLineDimmedNow(EPmDlgUpdPort, !local);
	}

void CPmUpdateDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId == EPmDlgUpdSource)
		LocalLinesDimmed();
	}

TBool CPmUpdateDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iSource = ChoiceListCurrentItem(EPmDlgUpdSource);
	GetEdwinText(iHost, EPmDlgUpdHost);
	iHost.Trim();
	iPort = NumberEditorValue(EPmDlgUpdPort);
	if (iSource == 2 && iHost.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No local server entered"));
		TryChangeFocusToL(EPmDlgUpdHost);
		return EFalse;
		}
	return ETrue;
	}

// View > Go to > Folder: every folder, however many the tree can show
void CPmAppUi::FoldersL()
	{
	TInt n = iView->FolderCount();
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(n + 2);
	CleanupStack::PushL(names);
	TBuf<120> item;
	for (TInt i = 0; i < n; i++)
		{
		const TPmFolder& f = iView->FolderAt(i);
		item = Clip(f.iName, 100);
		if (f.iUnread > 0 && f.iKind != 'S' && f.iKind != 'D' && f.iKind != 'T' && f.iKind != 'J')
			item.AppendFormat(_L("  (%d)"), f.iUnread);
		names->AppendL(item);
		}
	names->AppendL(_L("Outbox"));
	names->AppendL(_L("Calendar"));
	TInt choice = iView->CurrentSidebarItem();
	CleanupStack::Pop();                    // the dialog takes the names
	CPmChoiceDialog* dlg = new(ELeave) CPmChoiceDialog(_L("Go to folder"), _L("Folder"), names, choice);
	if (dlg->ExecuteLD(R_PM_CHOICE_DIALOG) && choice >= 0 && choice < n + 2)
		iView->OpenSidebarItemL(choice);
	}

// ----- File > Folder ----------------------------------------------------------

// Offline: the folder commands need the server, so ask before going online
// (as Check mail does)
TBool CPmAppUi::GoOnlineL(const TDesC& aQuestion)
	{
	if (!iSettings.iOffline)
		return ETrue;
	if (!iEikonEnv->QueryWinL(_L("You are working offline"), aQuestion))
		return EFalse;
	iSettings.iOffline = 0;
	SaveSettings();
	iView->SettingsChanged();
	return ETrue;
	}

// the folder's name in quotes, for a dialog title or a query
static void QuotedName(TDes& aOut, const TDesC& aName, TInt aMax)
	{
	aOut.Zero();
	aOut.Append('"');
	aOut.Append(Clip(aName, aMax));
	aOut.Append('"');
	}

void CPmAppUi::NewFolderL()
	{
	if (iView->FolderCount() == 0)
		{
		iView->Toast(_L("No folders yet - check mail first"));
		return;
		}
	// where it goes: the top level, or inside one of the folders (the one
	// highlighted or open to begin with)
	CDesCArrayFlat* places = new(ELeave) CDesCArrayFlat(8);
	CleanupStack::PushL(places);
	places->AppendL(_L("Top level"));
	const TPmFolder* at = iView->CommandFolder();
	TInt parent = 0;
	for (TInt i = 0; i < iView->FolderCount(); i++)
		{
		const TPmFolder& f = iView->FolderAt(i);
		places->AppendL(Clip(f.iName, 60));
		if (at == &f)
			parent = i + 1;
		}
	TBuf<60> name;
	CleanupStack::Pop();                    // the dialog's choice list takes places
	CPmFolderDialog* dlg = new(ELeave) CPmFolderDialog(_L("Create new folder"), name, places, parent);
	if (!dlg->ExecuteLD(R_PM_FOLDER_DIALOG))
		return;
	if (!GoOnlineL(_L("Go online and create the folder?")))
		return;
	TBuf8<128> in;
	if (parent > 0 && parent <= iView->FolderCount())
		in = iView->FolderAt(parent - 1).iImap;
	TBuf8<PM_ARG_MAX> n;
	SafeCopy(n, name);
	iView->SetStatus(_L("Creating the folder..."));
	iView->Cmd(PM_CMD_MKFOLDER, in, 0, n);
	}

// Rename and Delete keep away from the Inbox and the standard folders
// (Sent, Drafts, Trash, Junk, Archive), as the built-in Email program does
static TBool StandardFolder(const TPmFolder& aFolder)
	{
	return aFolder.iKind != '-' && aFolder.iKind != 'N';
	}

void CPmAppUi::RenameFolderL()
	{
	const TPmFolder* f = iView->CommandFolder();
	if (!f)
		{
		iView->Toast(_L("No folder selected"));
		return;
		}
	if (StandardFolder(*f))
		{
		iView->Toast(f->iKind == 'I' ? _L("The Inbox can't be renamed") : _L("Standard folders can't be renamed"));
		return;
		}
	// "Work/Projects": only the last part is renamed
	TBuf<60> name;
	TInt last = -1;
	for (TInt j = 0; j < f->iName.Length(); j++)
		if (f->iName[j] == '/' || f->iName[j] == '.') last = j;
	name = Clip(f->iName.Mid(last + 1), 60);
	TBuf<80> title;
	title = _L("Rename folder ");
	TBuf<64> q;
	QuotedName(q, f->iName.Mid(last + 1), 40);
	title.Append(q);
	TBuf8<128> imap(f->iImap);              // (the list may reload during the dialog)
	TInt none = 0;
	CPmFolderDialog* dlg = new(ELeave) CPmFolderDialog(title, name, NULL, none);
	if (!dlg->ExecuteLD(R_PM_RENAME_DIALOG))
		return;
	if (!GoOnlineL(_L("Go online and rename the folder?")))
		return;
	TBuf8<PM_ARG_MAX> n;
	SafeCopy(n, name);
	iView->SetStatus(_L("Renaming the folder..."));
	iView->Cmd(PM_CMD_RENFOLDER, imap, 0, n);
	}

void CPmAppUi::DeleteFolderL()
	{
	const TPmFolder* f = iView->CommandFolder();
	if (!f)
		{
		iView->Toast(_L("No folder selected"));
		return;
		}
	if (StandardFolder(*f))
		{
		iView->Toast(f->iKind == 'I' ? _L("The Inbox can't be deleted") : _L("Standard folders can't be deleted"));
		return;
		}
	// the statement says what goes (the folder, and how many messages), the
	// question comes last
	TBuf<100> q;
	QuotedName(q, f->iName, 60);
	if (f->iTotal == 1) q.Append(_L(" holds 1 message"));
	else if (f->iTotal > 0) q.AppendFormat(_L(" holds %d messages"), f->iTotal);
	TBuf8<128> imap(f->iImap);
	if (!iEikonEnv->QueryWinL(q, f->iTotal > 0 ? _L("Delete the folder and its messages?") : _L("Delete this folder?")))
		return;
	if (!GoOnlineL(_L("Go online and delete the folder?")))
		return;
	iView->SetStatus(_L("Deleting the folder..."));
	iView->Cmd(PM_CMD_DELFOLDER, imap, 0, KNullDesC8);
	}

void CPmFolderDialog::PreLayoutDynInitL()
	{
	SetTitleL(iTitle);
	SetEdwinTextL(EPmDlgFolderName, &iName);
	iHasParents = iParents != NULL;
	if (iParents)
		{
		CEikChoiceList* cl = (CEikChoiceList*)Control(EPmDlgFolderParent);
		cl->SetArrayL(iParents);            // it owns the array now
		cl->SetCurrentItem(iParent >= 0 && iParent < iParents->Count() ? iParent : 0);
		iParents = NULL;
		}
	}

TBool CPmFolderDialog::OkToExitL(TInt /*aButtonId*/)
	{
	GetEdwinText(iName, EPmDlgFolderName);
	iName.Trim();
	if (iName.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No folder name entered"));
		TryChangeFocusToL(EPmDlgFolderName);
		return EFalse;
		}
	// the server would read these as a path
	if (iName.Locate('/') >= 0 || iName.Locate('\\') >= 0)
		{
		iEikonEnv->InfoMsg(_L("A folder name can't contain / or \\"));
		TryChangeFocusToL(EPmDlgFolderName);
		return EFalse;
		}
	if (iHasParents)
		iParent = ((CEikChoiceList*)Control(EPmDlgFolderParent))->CurrentItem();
	return ETrue;
	}

void CPmAppUi::SearchL()
	{
	TBuf<60> words;
	CPmTextDialog* dlg = new(ELeave) CPmTextDialog(_L("Find in this folder"), _L("Search for"), words);
	if (dlg->ExecuteLD(R_PM_TEXT_DIALOG) && words.Length())
		iView->SearchL(words);
	}

void CPmAppUi::SwitchAccountL()
	{
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(4);
	CleanupStack::PushL(names);
	RArray<TInt> map;
	CleanupClosePushL(map);
	TInt current = 0;
	for (TInt i = 0; i < PM_MAX_ACCOUNTS; i++)
		{
		if (!iSettings.iAccounts[i].used)
			continue;
		TBuf<120> n;
		FromC(n, iSettings.iAccounts[i].name);
		TBuf<96> e;
		FromC(e, iSettings.iAccounts[i].email);
		n.Append(_L(" - "));
		n.Append(Clip(e, 80));
		if (i == iSettings.iAcct) current = names->Count();
		names->AppendL(n);
		User::LeaveIfError(map.Append(i));
		}
	if (names->Count() < 2)
		{
		CleanupStack::PopAndDestroy(2);    // map, names
		iView->Toast(_L("There is only one account"));
		return;
		}
	CleanupStack::Pop(2);
	CleanupClosePushL(map);
	TInt choice = current;
	CPmChoiceDialog* dlg = new(ELeave) CPmChoiceDialog(_L("Switch account"), _L("Account"), names, choice);
	if (dlg->ExecuteLD(R_PM_CHOICE_DIALOG) && choice >= 0 && choice < map.Count())
		{
		iSettings.iAcct = map[choice];
		SaveSettings();
		iView->AccountChangedL();
		}
	CleanupStack::PopAndDestroy();          // map
	}

void CPmAppUi::DeleteAccountL()
	{
	PmAccount& a = iSettings.iAccounts[iSettings.iAcct];
	if (!a.used)
		return;
	TBuf<60> n;
	FromC(n, a.name);
	TBuf<64> qn;
	qn.Append('"');
	qn.Append(n.Left(n.Length() < 60 ? n.Length() : 60));
	qn.Append('"');
	if (!iEikonEnv->QueryWinL(qn, _L("Remove this account from PsiMail?")))
		return;
	Mem::FillZ(&a, sizeof(a));
	// its files go when the slot is next used (the engine checks account.txt)
	for (TInt i = 0; i < PM_MAX_ACCOUNTS; i++)
		if (iSettings.iAccounts[i].used) { iSettings.iAcct = i; break; }
	SaveSettings();
	iView->AccountChangedL();
	}

void CPmAppUi::AboutL()
	{
	const TInt KBauds[5] = { 9600, 19200, 38400, 57600, 115200 };
	TInt bi = iSettings.iBaudIndex >= 0 && iSettings.iBaudIndex <= 4 ? iSettings.iBaudIndex : 4;
	TPtrC link(iSettings.iNetMode ? _L("Psion Internet") : _L("modem"));
	TBuf<80> status;
	status.Format(_L("%d baud, mail via %S"), KBauds[bi], &link);
	CPmAboutDialog* dlg = new(ELeave) CPmAboutDialog(status);
	dlg->ExecuteLD(R_PM_ABOUT_DIALOG);
	}

void CPmAboutDialog::PreLayoutDynInitL()
	{
	TBuf<32> title(_L("PsiMail "));
	title.Append(KVersion);
	SetLabelL(EPmDlgInfo1, title);
	((CEikLabel*)Control(EPmDlgInfo1))->SetFont(iEikonEnv->TitleFont());   // as PsiTerm's and PsiWeb's
	TBuf<80> who(_L("Email & calendar for the Psion Series 5mx - "));
	who.Append(TChar(0xa9));                  // (c), as the Psion's fonts have it
	who.Append(_L(" 2026 Dan Edge"));
	SetLabelL(EPmDlgInfo2, who);
	SetLabelL(EPmDlgInfo3, iStatus);
	}

// Each pane is dimmed and ticked on its own: EIKON panics (EIKON 8) when
// asked about an item the pane doesn't hold, so every SetItemDimmed and
// SetItemButtonState here names an item of that one pane.
void CPmAppUi::DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	CPmView::TMode m = iView->Mode();
	TBool msg = (m == CPmView::EList || m == CPmView::EMessage) && iView->CurrentRow() != NULL;
	TBool list = m == CPmView::EList && !iView->CurrentIsSearch();
	if (aMenuId == R_PM_FILE_MENU)
		{
		aMenuPane->SetItemButtonState(EPmCmdOffline, iSettings.iOffline ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemDimmed(EPmCmdHangup, !iView->Shared()->online);
		aMenuPane->SetItemDimmed(EPmCmdStop, !iView->Busy());
		// (dimmed, not gone, while calendar sync is off; Shift+Ctrl+Y then
		// says where to turn it on)
		aMenuPane->SetItemDimmed(EPmCmdCalendar, !iCalSettings.iCal.enabled);
		}
	else if (aMenuId == R_PM_PRINT_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdPrintPreview, !iView->CanPrint());
		aMenuPane->SetItemDimmed(EPmCmdPrint, !iView->CanPrint());
		}
	else if (aMenuId == R_PM_FOLDER_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdRefresh, m != CPmView::EList);
		aMenuPane->SetItemDimmed(EPmCmdOlder, !list);
		const TPmFolder* f = iView->CommandFolder();
		TBool own = f && f->iKind != 'I' && (f->iKind == '-' || f->iKind == 'N');
		aMenuPane->SetItemDimmed(EPmCmdNewFolder, iView->FolderCount() == 0);
		aMenuPane->SetItemDimmed(EPmCmdRenameFolder, !own);
		aMenuPane->SetItemDimmed(EPmCmdDeleteFolder, !own);
		}
	else if (aMenuId == R_PM_EDIT_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdDelete, !msg && !(m == CPmView::EOutbox && iView->CurrentRow()));
		aMenuPane->SetItemDimmed(EPmCmdMove, !msg);
		aMenuPane->SetItemDimmed(EPmCmdArchive, !msg);
		aMenuPane->SetItemDimmed(EPmCmdUndo, !iView->CanUndo());
		aMenuPane->SetItemDimmed(EPmCmdSearch, m == CPmView::ECalendar || m == CPmView::ENoAccount);
		aMenuPane->SetItemDimmed(EPmCmdAddSender, !msg);
		}
	else if (aMenuId == R_PM_ATTACH_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdOpenAttach, iView->AttachmentCount() == 0);
		aMenuPane->SetItemDimmed(EPmCmdSaveAttach, iView->AttachmentCount() == 0);
		}
	else if (aMenuId == R_PM_MESSAGE_MENU)
		{
		// (the Reply to and Attachments cascades are never dimmed: their
		// items are, so the commands can still be seen - style guide 7.1.3)
		aMenuPane->SetItemDimmed(EPmCmdForward, !msg);
		aMenuPane->SetItemDimmed(EPmCmdUnread, !msg);
		aMenuPane->SetItemDimmed(EPmCmdFlag, !msg);
		const TPmRow* row = msg ? iView->CurrentRow() : NULL;
		aMenuPane->SetItemButtonState(EPmCmdUnread, row && row->iFlags.Locate('S') < 0 ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemButtonState(EPmCmdFlag, row && row->iFlags.Locate('F') >= 0 ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemDimmed(EPmCmdWhole, m != CPmView::EMessage);
		aMenuPane->SetItemDimmed(EPmCmdWeb, m != CPmView::EMessage || !iView->HasHtml());
		aMenuPane->SetItemDimmed(EPmCmdNew, m == CPmView::ENoAccount);
		}
	else if (aMenuId == R_PM_EVENT_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdEventDetails, !iView->EventSelected());
		}
	else if (aMenuId == R_PM_SWITCH_VIEW_MENU)
		{
		TBool month = iView->MonthShown();
		aMenuPane->SetItemButtonState(EPmCmdWeekView, month ? 0 : EEikMenuItemSymbolOn);
		aMenuPane->SetItemButtonState(EPmCmdMonthView, month ? EEikMenuItemSymbolOn : 0);
		}
	else if (aMenuId == R_PM_VIEW_MENU || aMenuId == R_PM_CAL_VIEW_MENU)
		{
		aMenuPane->SetItemButtonState(EPmCmdToggleToolbar, (iSettings.iView & 1) ? 0 : EEikMenuItemSymbolOn);
		aMenuPane->SetItemButtonState(EPmCmdToggleTitle, (iSettings.iView & 2) ? 0 : EEikMenuItemSymbolOn);
		aMenuPane->SetItemButtonState(EPmCmdToggleFolders, (iSettings.iView & 4) ? 0 : EEikMenuItemSymbolOn);
		// (only the mail's View has Sort)
		if (aMenuId == R_PM_VIEW_MENU)
			aMenuPane->SetItemDimmed(EPmCmdSort, m != CPmView::EList);
		}
	else if (aMenuId == R_PM_GOTO_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdInbox, iView->FolderCount() == 0);
		aMenuPane->SetItemDimmed(EPmCmdFolders, iView->FolderCount() == 0);
		}
	else if (aMenuId == R_PM_REPLY_MENU || aMenuId == R_PM_REPLY_POPUP)
		{
		aMenuPane->SetItemDimmed(EPmCmdReply, !msg);
		aMenuPane->SetItemDimmed(EPmCmdReplyAll, !msg);
		// (only the toolbar's pop-up has Forward)
		if (aMenuId == R_PM_REPLY_POPUP)
			aMenuPane->SetItemDimmed(EPmCmdForward, !msg);
		}
	}

void CPmAppUi::HandleCommandL(TInt aCommand)
	{
	if (aCommand == EEikCmdZoomIn || aCommand == EEikCmdZoomOut ||
		aCommand == EPmCmdZoomIn || aCommand == EPmCmdZoomOut)
		{
		iView->ZoomL(aCommand == EEikCmdZoomIn || aCommand == EPmCmdZoomIn ? 1 : -1);
		return;
		}
	if (aCommand == EPmCmdNewPopup || aCommand == EPmCmdReplyPopup)
		{
		iView->ToolbarPopupL(aCommand);
		return;
		}
	CPmView::TMode m = iView->Mode();
	if (m == CPmView::ENoAccount && aCommand == EPmCmdEditAccount)
		aCommand = EPmCmdNewAccount;
	if (m == CPmView::ENoAccount && aCommand != EEikCmdExit && aCommand != EPmCmdNewAccount &&
		aCommand != EPmCmdConnSettings && aCommand != EPmCmdAbout && aCommand != EPmCmdUpdate &&
		aCommand != EPmCmdToggleToolbar && aCommand != EPmCmdToggleTitle && aCommand != EPmCmdToggleFolders &&
		aCommand != EPmCmdStatusInfo && aCommand != EPmCmdStop && aCommand != EPmCmdPrefs &&
		aCommand != EPmCmdZoomIn && aCommand != EPmCmdZoomOut && aCommand != EPmCmdCalSettings &&
		aCommand != EPmCmdRestart && aCommand != EPmCmdHelp)
		{
		iView->Toast(_L("No account - add one with Tools > Accounts"));
		return;
		}
	switch (aCommand)
		{
	case EEikCmdExit:
		iSettings.iSpare[1] = iView->WhereToken();   // (reopened here next time)
		SaveSettings();
		iView->StopEngine();
		Exit();
		break;
	case EPmCmdSendRecv:
		if (iView->OpInFlight(PM_CMD_SENDRECV))
			{
			// pressed again while the last check is still connecting (or
			// stuck): the way to try again is to stop it first
			if (iEikonEnv->QueryWinL(_L("Still checking mail"), _L("Stop it, so you can try again?")))
				iView->StopEngineWork(_L("Stopping..."));
			break;
			}
		if (iSettings.iOffline)
			{
			if (!iEikonEnv->QueryWinL(_L("You are working offline"), _L("Go online and send & receive?")))
				break;
			iSettings.iOffline = 0;
			SaveSettings();
			iView->SettingsChanged();
			}
		iView->SendRecvL();
		break;
	case EPmCmdOffline:
		iSettings.iOffline = !iSettings.iOffline;
		SaveSettings();
		iView->SettingsChanged();
		if (iSettings.iOffline)
			iView->Cmd(PM_CMD_HANGUP, KNullDesC8, 0, KNullDesC8);
		iView->Toast(iSettings.iOffline ? _L("Working offline - changes wait until you go online")
			: _L("Online - PsiMail will connect when it needs to"));
		if (!iSettings.iOffline)
			iView->LeftOfflineL();               // send what waits (pmauto.cpp)
		break;
	case EPmCmdHangup:
		iView->Cmd(PM_CMD_HANGUP, KNullDesC8, 0, KNullDesC8);
		iView->Working(_L("Disconnecting..."));
		break;
	case EPmCmdAbout:
		AboutL();
		break;
	case EPmCmdNew:
		// in the calendar, Ctrl+N makes an event
		if (m == CPmView::ECalendar)
			iView->NewEventL();
		else
			NewMessageL();
		break;
	case EPmCmdReply:
		ReplyL(EFalse);
		break;
	case EPmCmdReplyAll:
		ReplyL(ETrue);
		break;
	case EPmCmdForward:
		ForwardL();
		break;
	case EPmCmdDelete:
		if (m == CPmView::EList || m == CPmView::EOutbox || m == CPmView::EMessage)
			iView->DeleteCurrentL();
		else
			iView->Toast(m == CPmView::ECalendar ? _L("Events are changed in the Agenda") : _L("Nothing to delete"));
		break;
	case EPmCmdPrefs:
		{
		TInt sort = iSettings.iSort;
		CPmPrefsDialog* dlg = new(ELeave) CPmPrefsDialog(iSettings, sort);
		if (dlg->ExecuteLD(R_PM_PREFS_DIALOG))
			{
			SaveSettings();
			ApplyEmailButton();
			iView->SettingsChanged();
			iView->AutoSettingsChanged();
			if (sort != iSettings.iSort)
				iView->SortL(sort);
			iView->Render();
			}
		break;
		}
	case EPmCmdUndo:
		iView->UndoL();
		break;
	case EPmCmdPageSetup:
	case EPmCmdPrintSetup:
	case EPmCmdPrintPreview:
	case EPmCmdPrint:
		PrintCommandL(aCommand);              // (pmprint.cpp)
		break;
	case EPmCmdTool4:
		if (m == CPmView::EMessage)
			iView->BackL();
		else if (m == CPmView::EList || m == CPmView::EOutbox)
			iView->DeleteCurrentL();
		else
			iView->Toast(m == CPmView::ECalendar ? _L("Events are changed in the Agenda") : _L("Nothing to delete"));
		break;
	case EPmCmdStop:
		if (iView->Busy())
			iView->StopEngineWork(_L("Stopping..."));   // stop what the engine is doing
		break;
	case EPmCmdToggleToolbar:
		iView->ToggleViewL(1);
		break;
	case EPmCmdToggleTitle:
		iView->ToggleViewL(2);
		break;
	case EPmCmdToggleFolders:
		iView->ToggleViewL(4);
		break;
	case EPmCmdStatusInfo:
		iView->StatusInfoL();
		break;
	case EPmCmdSort:
		{
		if (m != CPmView::EList)
			{
			iView->Toast(_L("Not available here - open a folder to sort its messages"));
			break;
			}
		CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(8);
		CleanupStack::PushL(names);
		names->AppendL(_L("Date, newest first"));
		names->AppendL(_L("Date, oldest first"));
		names->AppendL(_L("From, A to Z"));
		names->AppendL(_L("From, Z to A"));
		names->AppendL(_L("Subject, A to Z"));
		names->AppendL(_L("Subject, Z to A"));
		names->AppendL(_L("Unread first"));
		TInt choice = iView->SortMode();
		CleanupStack::Pop();                  // the dialog's choice list takes names
		CPmChoiceDialog* dlg = new(ELeave) CPmChoiceDialog(_L("Sort messages"), _L("Order"), names, choice);
		if (dlg->ExecuteLD(R_PM_CHOICE_DIALOG))
			iView->SortL(choice);
		break;
		}
	case EPmCmdInbox:
		{
		TInt inbox = 0;
		for (TInt i = 0; i < iView->FolderCount(); i++)
			if (iView->FolderAt(i).iKind == 'I') { inbox = i; break; }
		if (iView->FolderCount())
			iView->OpenSidebarItemL(inbox);
		break;
		}
	case EPmCmdMove:
		MoveL();
		break;
	case EPmCmdArchive:
		{
		TBuf8<128> dest;
		for (TInt i = 0; i < iView->FolderCount(); i++)
			if (iView->FolderAt(i).iKind == 'A')
				dest = iView->FolderAt(i).iImap;
		if (!dest.Length())
			{
			iView->Toast(_L("There is no Archive folder"));
			break;
			}
		if (iView->MoveCurrentL(dest))
			iView->Toast(_L("Archived"));
		break;
		}
	case EPmCmdUnread:
		iView->ToggleFlagL('S');
		break;
	case EPmCmdFlag:
		iView->ToggleFlagL('F');
		break;
	case EPmCmdSaveAttach:
		AttachmentL(EFalse);
		break;
	case EPmCmdOpenAttach:
		AttachmentL(ETrue);
		break;
	case EPmCmdAddSender:
		AddSenderL();
		break;
	case EPmCmdWhole:
		iView->WholeMessageL();
		break;
	case EPmCmdWeb:
		iView->ViewAsWebPageL();
		break;
	case EPmCmdCalendar:
		iView->CalendarSyncL();
		break;
	case EPmCmdShowCalendar:
		iView->ShowCalendarL();
		break;
	case EPmCmdMonth:
		iView->ToggleMonthL();
		break;
	case EPmCmdWeekView:
		iView->ShowMonthL(EFalse);
		break;
	case EPmCmdMonthView:
		iView->ShowMonthL(ETrue);
		break;
	case EPmCmdToday:
		iView->CalendarTodayL();
		break;
	case EPmCmdEventDetails:
		iView->EventDetailsL();
		break;
	case EPmCmdNewEvent:
		iView->NewEventL();
		break;
	case EPmCmdCalSettings:
		EditCalendarL();
		break;
	case EPmCmdFolders:
		FoldersL();
		break;
	case EPmCmdNewFolder:
		NewFolderL();
		break;
	case EPmCmdRenameFolder:
		RenameFolderL();
		break;
	case EPmCmdDeleteFolder:
		DeleteFolderL();
		break;
	case EPmCmdHelp:
		HelpL();
		break;
	case EPmCmdUpdate:
		UpdateL();
		break;
	case EPmCmdRefresh:
		if (m == CPmView::EList) iView->RefreshL();
		else iView->Toast(_L("Not available here - open a folder to check it"));
		break;
	case EPmCmdOlder:
		if (m == CPmView::EList && !iView->CurrentIsSearch()) iView->OlderL();
		else iView->Toast(_L("Not available here - open a folder to get more of it"));
		break;
	case EPmCmdSearch:
		if (m == CPmView::ECalendar) iView->Toast(_L("Not available in the calendar"));
		else SearchL();
		break;
	case EPmCmdOutbox:
		iView->ShowOutboxL();
		break;
	case EPmCmdOpen:
		if (m == CPmView::EOutbox && iView->CurrentRow())
			{
			CPmDraft* d = CPmDraft::NewL();
			CleanupStack::PushL(d);
			TRAPD(err, iView->DraftFromOutboxL(*d));
			if (err != KErrNone)
				{
				CleanupStack::PopAndDestroy();   // (it said why)
				break;
				}
			CleanupStack::Pop();
			ComposeL(d, _L("Edit message"));
			}
		else
			iView->OpenCurrentL();
		break;
	case EPmCmdSwitchAccount:
		SwitchAccountL();
		break;
	case EPmCmdEditAccount:
		if (EditAccountL(iSettings.iAcct, EFalse))
			iView->AccountChangedL();
		break;
	case EPmCmdNewAccount:
		{
		TInt slot = -1;
		for (TInt i = 0; i < PM_MAX_ACCOUNTS; i++)
			if (!iSettings.iAccounts[i].used) { slot = i; break; }
		if (slot < 0)
			{
			iView->Toast(_L("PsiMail has room for only 4 accounts"));
			break;
			}
		if (EditAccountL(slot, ETrue))
			{
			iSettings.iAcct = slot;
			SaveSettings();
			iView->AccountChangedL();
			}
		break;
		}
	case EPmCmdDeleteAccount:
		DeleteAccountL();
		break;
	case EPmCmdConnSettings:
		{
		TBuf<40> ppp;
		UseSharedLink(iCoeEnv->FsSession(), iSettings, &ppp);
		CPmConnDialog* dlg = new(ELeave) CPmConnDialog(iSettings, ppp);
		if (dlg->ExecuteLD(R_PM_CONN_DIALOG))
			{
			SaveSharedLink(iCoeEnv->FsSession(), iSettings, ppp);
			SaveSettings();
			iView->StopEngine();
			iView->StartEngineL();
			iView->SettingsChanged();
			}
		break;
		}
	case EPmCmdRestart:
		iView->StopEngine();
		iView->StartEngineL();
		iView->SettingsChanged();
		break;
	case EPmCmdBack:
		break;
	default:
		break;
		}
	}

// ============================================================================
// Document / Application / entry points
// ============================================================================

CPmDocument::CPmDocument(CEikApplication& aApp)
	: CEikDocument(aApp)
	{
	}

CEikAppUi* CPmDocument::CreateAppUiL()
	{
	return new(ELeave) CPmAppUi;
	}

TUid CPmApplication::AppDllUid() const
	{
	return KUidPsiMail;
	}

CApaDocument* CPmApplication::CreateDocumentL()
	{
	return new(ELeave) CPmDocument(*this);
	}

EXPORT_C CApaApplication* NewApplication()
	{
	return new CPmApplication;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
