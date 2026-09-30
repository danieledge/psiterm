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
#include "psilink.h"

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
_LIT(KVersion, "0.6.2");          // also pkg/psimail.pkg
const TInt KTick = 250000;       // look at the engine 4 times a second
const TUint32 KIniMagic = 0x314d5350;   // 'PSM1'

// ============================================================================
// Little helpers
// ============================================================================

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
	StopEngine();
	DestroyNative();
	delete iCalSync;
	calm_free(&iCalModel);
	delete iTimer;
	delete iWatcher;
	delete iFolders;
	delete iRows;
	delete iText;
	if (iDocValid)
		doc_free(&iDoc);
	delete iAttNames;
	delete iAttSizes;
	delete iAttParts;
	delete iCmpNames;
	delete iCmpSizes;
	delete iDraft;
	if (iEdOpen) for (TInt i = 0; i < 4; i++) ed_free(&iEd[i]);
	if (iEvOpen) { ed_free(&iEd[4]); ed_free(&iEd[5]); }
	delete iBitmap;
	User::Free(iBits);
	if (iChunkOpen)
		iChunk.Close();
	}

// the drawing code's memory (see ui/pmui.h)
void* ui_alloc(int aSize) { return User::Alloc(aSize); }
void ui_free(void* aPtr) { User::Free(aPtr); }

void CPmView::ConstructL(const TRect& aRect, TPmSettings& aSettings, TPmCalSettings& aCal)
	{
	iSettings = &aSettings;
	iCal = &aCal;
	iCalSync = CPmCalSync::NewL(*this);
	// the mailbox and the reader are EIKON controls (pmnative.cpp); writing
	// and the calendar are drawn by PsiMail itself, in 16 greys (see ui/)
	CreateWindowL();
	SetRectL(aRect);
	EnableDragEvents();
	iFolders = new(ELeave) CArrayFixFlat<TPmFolder>(16);
	iRows = new(ELeave) CArrayFixFlat<TPmRow>(32);
	iAttNames = new(ELeave) CDesCArrayFlat(4);
	iAttSizes = new(ELeave) CDesCArrayFlat(4);
	iAttParts = new(ELeave) CDesC8ArrayFlat(4);
	iCmpNames = new(ELeave) CDesCArrayFlat(4);
	iCmpSizes = new(ELeave) CDesCArrayFlat(4);

	// the drawn screens use the whole screen (the toolbar is hidden then)
	TSize size = iCoeEnv->ScreenDevice()->SizeInPixels();
	if (size.iWidth > 640) size.iWidth = 640;
	if (size.iHeight > 240) size.iHeight = 240;
	iBitmap = new(ELeave) CFbsBitmap;
	User::LeaveIfError(iBitmap->Create(size, EGray16));
	TInt stride = (size.iWidth + 1) / 2;
	iBits = (TUint8*)User::AllocL(stride * size.iHeight);
	Mem::Fill(iBits, stride * size.iHeight, 0xff);
	gfx_init(&iCanvas, iBits, size.iWidth, size.iHeight, stride);

	TInt r = iChunk.CreateGlobal(_L(PSI_SHARED_NAME), sizeof(PmShared), sizeof(PmShared));
	if (r == KErrAlreadyExists)
		r = iChunk.OpenGlobal(_L(PSI_SHARED_NAME), EFalse);
	User::LeaveIfError(r);
	iChunkOpen = ETrue;
	iShared = (PmShared*)iChunk.Base();
	Mem::FillZ(iShared, sizeof(PmShared));

	CreateNativeL();
	iNativeMode = (TMode)-1;
	iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
	iTimer->Start(KTick, KTick, TCallBack(TickCallback, this));
	ActivateL();
	StartEngineL();
	calm_init(&iCalModel);
	TRAPD(err, LoadCalendarL());
	AccountChangedL();
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
		e.Format(_L("Could not start psimail.exe (error %d)"), r);
		iEikonEnv->InfoWinL(_L("PsiMail"), e);
		return;
		}
	iRunning = ETrue;
	if (!iWatcher)
		iWatcher = new(ELeave) CPmWatcher(*this);
	iWatcher->Watch(iProcess);
	// above this (foreground) app: the engine must drain the serial port
	// while the screen is being drawn, or bytes are lost without RTS/CTS
	iProcess.SetPriority(EPriorityHigh);
	iProcess.Resume();
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
		iProcess.Kill(0);
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
		why.Format(_L("The mail engine stopped: %S %d. Tools > Restart engine."), &cat, reason);
	else
		why.Format(_L("The mail engine closed (%d). Tools > Restart engine."), reason);
	SetStatus(why);
	}

void CPmView::SettingsChanged()
	{
	CopySettingsToShared();
	Render();
	}

TBool CPmView::Busy() const
	{
	return iShared->busy || iShared->cmd_head != iShared->cmd_tail;
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
		Toast(aToast);
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
		iMode = ENoAccount;
		Render();
		return;
		}
	LoadFoldersL();
	iMode = EList;
	iListMode = EList;
	iSel = iTop = 0;
	LoadListL();
	// first time for this account: fetch the folders and the Inbox
	if (iFolders->Count() == 0 && !iSettings->iOffline)
		{
		Cmd(PM_CMD_FOLDERS, KNullDesC8, 0, KNullDesC8);
		Cmd(PM_CMD_SYNC, _L8("INBOX"), 0, KNullDesC8);
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
	if (iDocValid)
		{
		doc_free(&iDoc);
		iDocValid = EFalse;
		}
	iAttNames->Reset();
	iAttSizes->Reset();
	iAttParts->Reset();
	iTruncated = 0;
	iBodyOff = 0;
	iFocusLink = 0;
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
	BuildDoc();
	}

// lays the message out (ui/pmdoc.cpp)
void CPmView::BuildDoc()
	{
	if (iDocValid)
		{
		doc_free(&iDoc);
		iDocValid = EFalse;
		}
	if (!iText)
		return;
	TInt n = iAttNames->Count();
	for (TInt i = 0; i < n; i++)
		{
		iUiAtt[i].name = (const char*)(*iAttNames)[i].Ptr();
		iUiAtt[i].len = (*iAttNames)[i].Length();
		iUiAtt[i].size = (const char*)(*iAttSizes)[i].Ptr();
		iUiAtt[i].slen = (*iAttSizes)[i].Length();
		}
	TPtrC body = iText->Mid(iBodyOff);
	doc_build(&iDoc, (const char*)body.Ptr(), body.Length(), iCanvas.w, iUiAtt, n, (iTruncated + 1023) / 1024);
	iDocValid = ETrue;
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
	case ECalEvent:
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
	if (iMode == ECalEvent)
		iMode = ECalendar;
	if (iMode == ECalendar)
		{
		iSidebar = ETrue;
		iFolderFollow = ETrue;
		iFolderSel = iFolders->Count() + 1;
		Render();
		return;
		}
	if (iMode != EList && iMode != EOutbox)
		return;
	iSidebar = ETrue;
	iFolderFollow = ETrue;
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
	iSel = iTop = 0;
	iRows->Reset();
	LoadListL();
	iSel = 0;
	iTop = 0;
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
	iSel = iTop = 0;
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
	iScroll = 0;
	iFocusLink = 0;
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
	if (iMode == ECalEvent)
		{
		iMode = ECalendar;
		Render();
		return;
		}
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
	Render();
	}

void CPmView::CalSyncDone(TInt aError, const TDesC& aSummary, TBool aPushed)
	{
	iStatus.Zero();
	iStatusUntil = 0;
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
	TInt n = doc_plain((const char*)body.Ptr(), body.Length(), (char*)out, room, aQuote);
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
			Toast(_L("In the outbox - it goes at the next check for mail"));
		else
			{
			Cmd(PM_CMD_SEND, KNullDesC8, 0, KNullDesC8);
			Toast(_L("Sending..."));
			}
		}
	else
		Toast(_L("Saved in the outbox"));
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
	s->app_beat++;                       // "still here": see pmepoc.cpp
	TBool redraw = EFalse;
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
			}
		}
	// messages that have had their time
	TUint now = User::TickCount();
	if (iToast.Length() && now - iToastUntil < 0x80000000u)
		{
		iToast.Zero();
		redraw = ETrue;
		}
	if (iStatus.Length() && iStatusUntil && now - iStatusUntil < 0x80000000u && !s->busy)
		{
		iStatus.Zero();
		iStatusUntil = 0;
		redraw = ETrue;
		}
	// the engine is up: from the start-up screen to the mail
	if (!iSplashDone && s->state != PM_STATE_STARTING)
		{
		iSplashDone = ETrue;
		redraw = ETrue;
		}
	if (redraw)
		Render();
	}

void CPmView::HandleResultL(const PmCmd& aCmd)
	{
	PmShared* s = iShared;
	TBuf<160> msg;
	FromC(msg, s->last_msg);
	TInt res = s->last_res;
	SafeCopy(iStatus, msg);
	iStatusUntil = User::TickCount() + 64 * 6;
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
	switch (res)
		{
	case PM_RES_OK:
		if (aCmd.op == PM_CMD_SEARCH)
			{
			iSearch = ETrue;
			iMode = EList;
			iListMode = EList;
			iFolder.Copy(TPtrC8((const TUint8*)aCmd.folder));
			iSel = iTop = 0;
			LoadListL();
			iSel = 0;
			iTop = 0;
			}
		else if (aCmd.op == PM_CMD_ATTACH)
			{
			TBuf<128> file;
			FromC(file, s->last_file);
			iEikonEnv->InfoWinL(_L("Attachment saved"), file);
			}
		else if ((aCmd.op == PM_CMD_SYNC || aCmd.op == PM_CMD_SENDRECV) && s->new_mail > 0 && aCmd.op == PM_CMD_SENDRECV)
			Toast(msg);
		else if (aCmd.op == PM_CMD_SEND || aCmd.op == PM_CMD_SENDRECV)
			Toast(msg);
		if (aCmd.op == PM_CMD_BODY || aCmd.op == PM_CMD_FULLBODY)
			{
			if (iMode == EMessage && aCmd.uid == iMsgUid)
				{
				TInt top = iScroll;
				LoadMessageL();
				if (aCmd.op == PM_CMD_FULLBODY) iScroll = top;
				}
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
		lines[3] = _L("Trust it only if you expected this (e.g. your own server).");
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
		if (aCmd.op == PM_CMD_FLAG && iSettings->iOffline)
			{
			ReloadL();                         // queued, as expected when offline
			break;
			}
		Toast(msg);
		ReloadL();
		break;
	case PM_RES_CANCELLED:
		Toast(_L("Stopped"));
		break;
	default:
		if ((aCmd.op == PM_CMD_BODY || aCmd.op == PM_CMD_FULLBODY) && aCmd.uid == iMsgUid)
			iBodyError = msg;                    // shown in place of "Downloading..."
		if (aCmd.op == PM_CMD_BODY || aCmd.op == PM_CMD_ATTACH || aCmd.op == PM_CMD_SEND ||
			aCmd.op == PM_CMD_SENDRECV || aCmd.op == PM_CMD_SEARCH)
			iEikonEnv->InfoWinL(_L("PsiMail"), Clip(msg, 120));
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
			lines[0] = _L("The calendar server refused the password.");
			lines[1] = _L("If your provider uses app passwords, make one");
			lines[2] = _L("that can use calendars (CalDAV), and enter it here.");
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
// Drawing - everything through ui/ (pmscreens.cpp), into a 16-grey bitmap
// ============================================================================

TInt CPmView::Rows() const
	{
	return ui_mailbox_rows(iCanvas.h);
	}

void CPmView::Toast(const TDesC& aText)
	{
	if (iNativeShown && NativeMode())
		{
		iEikonEnv->InfoMsg(aText);           // EIKON's own message, as the style guide has it
		return;
		}
	SafeCopy(iToast, aText);
	iToastUntil = User::TickCount() + 64 * 3;      // 3 s (1/64 s ticks)
	Render();
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

static const char* CStr(const TDesC& aDes) { return (const char*)aDes.Ptr(); }

// the folder column: folders, the outbox, the calendar
void CPmView::FillSidebar(PmUiMailbox& m)
	{
	PmAccount& a = iSettings->iAccounts[iSettings->iAcct];
	m.account = a.name;
	m.alen = User::StringLength((const TUint8*)a.name);
	TInt nf = iFolders->Count();
	if (nf > 80) nf = 80;
	TInt k = 0;
	for (TInt i = 0; i < nf; i++)
		{
		const TPmFolder& f = (*iFolders)[i];
		PmUiFolder& u = iUiFolders[k++];
		// "Work/Projects": show "Projects", indented
		TInt depth = 0, last = -1;
		if (f.iKind == '-' || f.iKind == 'N')
			for (TInt j = 0; j < f.iName.Length(); j++)
				if (f.iName[j] == '/' || f.iName[j] == '.') { depth++; last = j; }
		u.name = CStr(f.iName) + last + 1;
		u.len = f.iName.Length() - last - 1;
		u.kind = f.iKind;
		u.unread = f.iKind == 'S' || f.iKind == 'D' || f.iKind == 'T' || f.iKind == 'J' ? 0 : f.iUnread;
		u.depth = depth > 3 ? 3 : depth;
		}
	PmUiFolder& ob = iUiFolders[k++];
	ob.name = "Outbox"; ob.len = 6; ob.kind = 'O'; ob.depth = 0;
	ob.unread = OutboxCount();
	PmUiFolder& cal = iUiFolders[k++];
	cal.name = "Calendar"; cal.len = 8; cal.kind = 'C'; cal.depth = 0;
	cal.unread = iCalLoaded ? calm_count(&iCalModel, iCalToday) : 0;
	m.folders = iUiFolders;
	m.nfolders = k;
	m.folderSel = iFolderSel < k ? iFolderSel : k - 1;
	TInt srows = ui_sidebar_rows(iCanvas.h);
	if (iFolderFollow)
		{
		// (the keyboard moves the highlight: keep it in view; the pen
		// scrolls the column by itself)
		if (iFolderSel < iFolderTop) iFolderTop = iFolderSel;
		if (iFolderSel >= iFolderTop + srows) iFolderTop = iFolderSel - srows + 1;
		iFolderFollow = EFalse;
		}
	if (iFolderTop > k - srows) iFolderTop = k - srows;
	if (iFolderTop < 0) iFolderTop = 0;
	m.folderTop = iFolderTop;
	m.sidebarFocus = iSidebar;
	m.online = iShared->online;
	m.offline = iSettings->iOffline;
	}

void CPmView::RenderMailbox()
	{
	PmUiMailbox m;
	Mem::FillZ(&m, sizeof(m));
	FillSidebar(m);

	TBuf<80> title;
	TBuf<80> sub;
	if (iMode == EOutbox)
		{
		title = _L("Outbox");
		if (iRows->Count()) sub.Format(_L("%d waiting to go"), iRows->Count());
		}
	else
		{
		const TPmFolder* f = CurrentFolder();
		if (iSearch)
			{
			title = _L("Search");
			sub = _L("\x93");
			sub.Append(Clip(iSearchWords, 40));
			sub.Append(_L("\x94 in "));
			sub.Append(f ? Clip(f->iName, 30) : TPtrC(_L("Inbox")));
			}
		else
			{
			if (f) title = Clip(f->iName, 60); else title.Copy(Clip(iFolder, 60));
			TInt unread = 0;
			for (TInt i = 0; i < iRows->Count(); i++)
				if ((*iRows)[i].iFlags.Locate('S') < 0) unread++;
			if (unread) sub.Format(_L("%d unread"), unread);
			else if (iRows->Count()) sub.Format(_L("%d messages"), iRows->Count());
			}
		}
	m.title = CStr(title); m.tlen = title.Length();
	m.subtitle = CStr(sub); m.sublen = sub.Length();

	TInt rows = Rows();
	TInt n = 0;
	for (TInt i = iTop; i < iRows->Count() && n < rows + 1 && n < 12; i++, n++)
		{
		const TPmRow& r = (*iRows)[i];
		PmUiRow& u = iUiRows[n];
		u.from = CStr(r.iFrom); u.flen = r.iFrom.Length();
		u.subj = r.iSubject.Length() ? CStr(r.iSubject) : "(no subject)";
		u.slen = r.iSubject.Length() ? r.iSubject.Length() : 12;
		if (iMode == EOutbox)
			{
			if (r.iFlags.Locate('E') >= 0) iDates[n] = _L("not sent");
			else if (r.iFlags.Locate('D') >= 0) iDates[n] = _L("draft");
			else iDates[n] = _L("to send");
			u.flags = (r.iFlags.Locate('E') >= 0 ? KRowError : 0) | (r.iFlags.Locate('D') >= 0 ? KRowDraft : 0);
			}
		else
			{
			FormatDate(r.iDate, iDates[n]);
			u.flags = 0;
			if (r.iFlags.Locate('S') < 0) u.flags |= KRowUnread;
			if (r.iFlags.Locate('F') >= 0) u.flags |= KRowFlagged;
			if (r.iFlags.Locate('T') >= 0) u.flags |= KRowAttach;
			if (r.iFlags.Locate('A') >= 0) u.flags |= KRowAnswered;
			}
		u.date = CStr(iDates[n]); u.dlen = iDates[n].Length();
		}
	m.rows = iUiRows;
	m.nrows = n;
	m.total = iRows->Count();
	m.top = iTop;
	m.sel = iSel;
	const TDesC& empty = iMode == EOutbox ? _L("Nothing waiting to be sent")
		: iSearch ? _L("Nothing found")
		: (Busy() ? _L("Looking for messages...") : _L("No messages here"));
	m.empty = CStr(empty); m.elen = empty.Length();
	m.busy = Busy() || CalendarBusy();
	if (Busy() && iLastProgress.Length()) { m.status = CStr(iLastProgress); m.statlen = iLastProgress.Length(); }
	else if (Busy()) { m.status = "Working"; m.statlen = 7; }
	else if (iStatus.Length()) { m.status = CStr(iStatus); m.statlen = iStatus.Length(); }
	m.online = iShared->online;
	m.offline = iSettings->iOffline;
	ui_mailbox(&iCanvas, &m);
	}

void CPmView::RenderReader()
	{
	PmUiReader r;
	Mem::FillZ(&r, sizeof(r));
	const TPmRow* row = CurrentRow();
	if (iText && iDocValid)
		{
		r.text = CStr(*iText) + iBodyOff;
		r.len = iText->Length() - iBodyOff;
		r.doc = &iDoc;
		}
	r.scroll = iScroll;
	r.position = iSel + 1;
	r.count = iRows->Count();
	const TPmFolder* f = CurrentFolder();
	if (iListMode == EOutbox) { r.folder = "Outbox"; r.flen = 6; }
	else if (iSearch) { r.folder = "Search"; r.flen = 6; }
	else if (f) { r.folder = CStr(f->iName); r.flen = f->iName.Length(); }
	r.focusLink = iFocusLink;
	r.busy = Busy() || CalendarBusy();
	if (Busy() && iLastProgress.Length()) { r.status = CStr(iLastProgress); r.statlen = iLastProgress.Length(); }
	else if (iStatus.Length()) { r.status = CStr(iStatus); r.statlen = iStatus.Length(); }
	r.loading = iWaitingBody || !iDocValid;
	if (row) { r.subject = CStr(row->iSubject); r.sublen = row->iSubject.Length(); r.flagged = row->iFlags.Locate('F') >= 0; }
	r.html = iHtml;
	r.att = iUiAtt;
	r.natt = iAttNames->Count();
	ui_reader(&iCanvas, &r);
	}

// draws the screen into iBits, then onto the window
void CPmView::Render()
	{
	CPmAppUi* ui = (CPmAppUi*)iEikonEnv->EikAppUi();
	if (NativeMode() && iFolderList)
		{
		if (!iNativeShown)
			{
			iNativeShown = ETrue;
			iNativeMode = (TMode)-1;
			ui->ShowToolBar(ETrue);            // (sets our rect: SizeChanged lays out)
			}
		TRAPD(err, UpdateNativeL());
		(void)err;
		return;
		}
	if (iNativeShown && iFolderList)
		{
		ShowNative(EFalse);
		iNativeMode = (TMode)-1;
		ui->ShowToolBar(EFalse);
		}
	iCanvas.mono = iSettings->iMono;
	TBool splash = !iSplashDone && iRunning && iShared && iShared->state == PM_STATE_STARTING;
	if (splash)
		{
		const TDesC& t = _L("Starting the mail engine...");
		ui_splash(&iCanvas, CStr(t), t.Length());
		}
	else switch (iMode)
		{
	case EMessage:
		RenderReader();
		break;
	case ECalendar:
		RenderCalendar();
		break;
	case ECalEvent:
		RenderEvent();
		break;
	case ECompose:
		RenderCompose();
		break;
	case EEventEdit:
		RenderEventEdit();
		break;
	case ENoAccount:
		{
		const TDesC& a = _L("Set up your mail with Tools > New account (Ctrl+K).");
		const TDesC& b = _L("Most providers want an app password for this.");
		ui_welcome(&iCanvas, CStr(a), a.Length(), CStr(b), b.Length());
		break;
		}
	default:
		RenderMailbox();
		break;
		}
	if (iToast.Length())
		ui_toast(&iCanvas, CStr(iToast), iToast.Length());
	// the buffer has an EGray16 scan line's layout
	for (TInt y = 0; y < iCanvas.h; y++)
		{
		TPtr8 row(iBits + y * iCanvas.stride, iCanvas.stride, iCanvas.stride);
		iBitmap->SetScanLine(row, y);
		}
	DrawNow();
	}

void CPmView::Draw(const TRect& aRect) const
	{
	if (iNativeShown)
		{
		DrawNative(aRect);
		DrawStatus(SystemGc());
		return;
		}
	SystemGc().BitBlt(aRect.iTl, iBitmap, aRect);
	}

// ============================================================================
// Keys and pen
// ============================================================================

void CPmView::EnsureVisible()
	{
	TInt rows = Rows();
	TInt count = iRows->Count();
	if (iSel >= count) iSel = count - 1;
	if (iSel < 0) iSel = 0;
	if (iSel < iTop) iTop = iSel;
	if (iSel >= iTop + rows) iTop = iSel - rows + 1;
	if (iTop > count - rows) iTop = count - rows;
	if (iTop < 0) iTop = 0;
	}

void CPmView::MoveSel(TInt aDelta)
	{
	if (iSidebar)
		{
		iFolderFollow = ETrue;
		iFolderSel += aDelta;
		if (iFolderSel >= SidebarCount()) iFolderSel = SidebarCount() - 1;
		if (iFolderSel < 0) iFolderSel = 0;
		}
	else
		{
		iSel += aDelta;
		EnsureVisible();
		}
	Render();
	}

// the entry in the folder column for what's showing
TInt CPmView::CurrentSidebarItem() const
	{
	if (iMode == ECalendar || iMode == ECalEvent)
		return iFolders->Count() + 1;
	if (iMode == EOutbox)
		return iFolders->Count();
	for (TInt i = 0; i < iFolders->Count(); i++)
		if ((*iFolders)[i].iImap == iFolder)
			return i;
	return 0;
	}

// the pen on the folder column's scroll bar: a page of folders
void CPmView::SidebarPage(TInt aDir)
	{
	TInt rows = ui_sidebar_rows(iCanvas.h);
	iFolderTop += aDir * (rows > 1 ? rows - 1 : 1);
	Render();                              // (FillSidebar keeps it in range)
	}

// Folder > Go to folder...: one of the column's entries
void CPmView::OpenSidebarItemL(TInt aIndex)
	{
	if (iMode == EMessage)
		BackL();
	iSidebar = ETrue;
	iFolderSel = aIndex;
	iFolderFollow = ETrue;
	OpenCurrentL();
	iFolderFollow = ETrue;
	Render();
	}

void CPmView::Scroll(TInt aDelta)
	{
	if (!iDocValid)
		return;
	TInt maxs = iDoc.height - ui_reader_body_height(iCanvas.h);
	if (maxs < 0) maxs = 0;
	iScroll += aDelta;
	if (iScroll > maxs) iScroll = maxs;
	if (iScroll < 0) iScroll = 0;
	// a highlighted link that has gone off screen is let go
	if (iFocusLink)
		{
		PmUiReader r;
		Mem::FillZ(&r, sizeof(r));
		r.doc = &iDoc;
		r.scroll = iScroll;
		r.text = CStr(*iText) + iBodyOff;
		TBool seen = EFalse;
		for (TInt i = 0; i < iDoc.nops && !seen; i++)
			if (iDoc.ops[i].link == iFocusLink && iDoc.ops[i].y > iScroll && iDoc.ops[i].y < iScroll + ui_reader_body_height(iCanvas.h))
				seen = ETrue;
		if (!seen) iFocusLink = 0;
		}
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
	if (iMode == ECompose)
		return ComposeKeyL(code, aKeyEvent.iModifiers);
	if (iMode == EEventEdit)
		return EventEditKeyL(code, aKeyEvent.iModifiers);
	// Esc stops the engine when there is nothing to go back from (or the
	// message being waited for is the thing it's fetching); otherwise it goes
	// back, even while mail downloads ahead in the background
	TBool escStops = ETrue;
	if (iNativeShown && NativeMode())
		escStops = (iMode == EMessage && iWaitingBody) ||
			((iMode == EList || iMode == EOutbox) && iSidebar) || iMode == ENoAccount ||
			(iShared->busy && !iShared->online);   // still connecting: nothing to go back from
	if (code == EKeyEscape && Busy() && escStops)
		{
		StopEngineWork(_L("Stopping..."));  // stop what the engine is doing
		return EKeyWasConsumed;
		}
	if (iNativeShown && NativeMode())
		return NativeKeyL(aKeyEvent, aType);
	if (iMode == EMessage)
		return ReaderKeyL(code, aKeyEvent.iModifiers);
	if (iMode == EList || iMode == EOutbox)
		return MailboxKeyL(code);
	if (iMode == ECalendar)
		return CalendarKeyL(code);
	if (iMode == ECalEvent)
		return EventKeyL(code);
	return EKeyWasNotConsumed;
	}

TKeyResponse CPmView::MailboxKeyL(TUint aCode)
	{
	TInt page = Rows() - 1;
	if (page < 1) page = 1;
	switch (aCode)
		{
	case EKeyEscape:
		BackL();
		break;
	case EKeyUpArrow: MoveSel(-1); break;
	case EKeyDownArrow: MoveSel(1); break;
	case EKeyPageUp: MoveSel(-page); break;
	case EKeyPageDown: MoveSel(page); break;
	case EKeyHome: MoveSel(-100000); break;
	case EKeyEnd: MoveSel(100000); break;
	case EKeyEnter:
	case EKeyRightArrow:
		OpenCurrentL();
		break;
	case EKeyLeftArrow:
		if (!iSidebar) FocusFoldersL();
		break;
	case EKeyTab:
		if (iSidebar) OpenCurrentL(); else FocusFoldersL();
		break;
	case EKeyDelete:
	case EKeyBackspace:
		if (!iSidebar) DeleteCurrentL();
		break;
	default:
		return EKeyWasNotConsumed;
		}
	return EKeyWasConsumed;
	}

TKeyResponse CPmView::ReaderKeyL(TUint aCode, TUint aMods)
	{
	TInt page = ui_reader_body_height(iCanvas.h) - 34;
	PmUiReader r;
	Mem::FillZ(&r, sizeof(r));
	if (iDocValid)
		{
		r.doc = &iDoc;
		r.text = CStr(*iText) + iBodyOff;
		}
	r.scroll = iScroll;
	switch (aCode)
		{
	case EKeyEscape:
		if (iFocusLink) { iFocusLink = 0; Render(); }
		else BackL();
		break;
	case EKeyUpArrow: Scroll(-17); break;
	case EKeyDownArrow: Scroll(17); break;
	case EKeyPageUp: Scroll(-page); break;
	case EKeyPageDown:
	case ' ':
		Scroll(page);
		break;
	case EKeyHome: Scroll(-10000000); break;
	case EKeyEnd: Scroll(10000000); break;
	case EKeyLeftArrow: StepMessageL(-1); break;
	case EKeyRightArrow: StepMessageL(1); break;
	case EKeyTab:
		{
		// the next link or attachment on screen (Shift+Tab: the one before)
		TInt next = ui_reader_next_link(&r, iCanvas.h, iFocusLink, (aMods & EModifierShift) ? -1 : 1);
		iFocusLink = next;
		if (!next && iDocValid)
			Toast(_L("No more links on this page"));
		Render();
		break;
		}
	case EKeyEnter:
		if (iFocusLink) ActivateLinkL(iFocusLink);
		break;
	case EKeyDelete:
		DeleteCurrentL();
		break;
	default:
		return EKeyWasNotConsumed;
		}
	return EKeyWasConsumed;
	}

// a link: open it in PsiWeb; an attachment: save it
void CPmView::ActivateLinkL(TInt aLink)
	{
	if (aLink <= -1000)
		{
		SaveAttachmentL(-aLink - 1000);
		return;
		}
	const char* url = 0;
	TInt n = iDocValid ? doc_link_url(&iDoc, CStr(*iText) + iBodyOff, aLink, &url) : 0;
	if (!n)
		return;
	TPtrC8 u((const TUint8*)url, n);
	if (Clip(u, 7).CompareF(_L8("mailto:")) == 0)
		{
		// write to them
		CPmDraft* d = CPmDraft::NewL();
		CleanupStack::PushL(d);
		TPtrC8 addr = u.Mid(7);
		TInt q = addr.Locate('?');
		if (q >= 0) addr.Set(addr.Left(q));
		SafeCopy(d->iTo, addr);
		CleanupStack::Pop();
		((CPmAppUi*)iEikonEnv->EikAppUi())->ComposeDraftL(d, _L("New message"));
		return;
		}
	TBuf<256> link;
	SafeCopy(link, u);
	OpenWebL(link);
	}

void CPmView::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	TPoint p = aEvent.iPosition;
	AddEntropy(p.iX * 1000 + p.iY);
	if (iNativeShown && NativeMode())
		{
		// the title band and the headings are ours; the list boxes and the
		// reader take the pen themselves
		if (NativePointerL(aEvent))
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
		return;
		}
	// the folder column: the pen drags it up and down; a tap (no drag)
	// opens the folder when the pen lifts
	TBool side = (iMode == EList || iMode == EOutbox || iMode == ECalendar) && p.iX < UI_SIDE_W;
	if (aEvent.iType == TPointerEvent::EButton1Down && side)
		{
		iPenSide = ETrue;
		iPenDragged = EFalse;
		iPenStart = p;
		iPenTop = iFolderTop;
		return;
		}
	if (iPenSide && aEvent.iType == TPointerEvent::EDrag)
		{
		TInt dy = p.iY - iPenStart.iY;
		if (dy > 6 || dy < -6)
			iPenDragged = ETrue;
		if (iPenDragged)
			{
			TInt top = iPenTop - dy / UI_FOLDER_ROW;
			if (top != iFolderTop)
				{
				iFolderTop = top;             // (FillSidebar keeps it in range)
				Render();
				}
			}
		return;
		}
	if (iPenSide && aEvent.iType == TPointerEvent::EButton1Up)
		{
		iPenSide = EFalse;
		if (iPenDragged)
			return;
		p = iPenStart;                        // a tap: handled as before
		}
	else if (aEvent.iType != TPointerEvent::EButton1Down)
		return;
	TInt index = -1;
	if (iMode == ECompose)
		{
		ComposePointerL(p);
		return;
		}
	if (iMode == EEventEdit)
		{
		EventEditPointerL(p);
		return;
		}
	if (iMode == ECalendar || iMode == ECalEvent)
		{
		CalendarPointerL(p);
		return;
		}
	if (iMode == EList || iMode == EOutbox)
		{
		PmUiMailbox m;
		Mem::FillZ(&m, sizeof(m));
		m.nfolders = SidebarCount();
		m.folderTop = iFolderTop;
		m.total = iRows->Count();
		m.top = iTop;
		switch (ui_mailbox_hit(iCanvas.w, iCanvas.h, &m, p.iX, p.iY, &index))
			{
		case EHitFolder:
			iSidebar = ETrue;
			iFolderSel = index;
			OpenCurrentL();
			break;
		case EHitRow:
			if (index == iSel && !iSidebar) OpenCurrentL();
			else { iSidebar = EFalse; iSel = index; EnsureVisible(); Render(); }
			break;
		case EHitTop: SidebarPage(-1); break;
		case EHitBottom: SidebarPage(1); break;
		case EHitRefresh: iEikonEnv->EikAppUi()->HandleCommandL(EPmCmdSendRecv); break;
		case EHitSearch: iEikonEnv->EikAppUi()->HandleCommandL(EPmCmdSearch); break;
		case EHitNew: iEikonEnv->EikAppUi()->HandleCommandL(EPmCmdNew); break;
		default: break;
			}
		return;
		}
	if (iMode == EMessage)
		{
		PmUiReader r;
		Mem::FillZ(&r, sizeof(r));
		if (iDocValid) { r.doc = &iDoc; r.text = CStr(*iText) + iBodyOff; }
		r.scroll = iScroll;
		r.html = iHtml;
		TInt link = 0;
		TInt page = ui_reader_body_height(iCanvas.h) - 34;
		switch (ui_reader_hit(iCanvas.w, iCanvas.h, &r, p.iX, p.iY, &link))
			{
		case EHitBack: BackL(); break;
		case EHitReply: iEikonEnv->EikAppUi()->HandleCommandL(EPmCmdReply); break;
		case EHitReplyAll: iEikonEnv->EikAppUi()->HandleCommandL(EPmCmdReplyAll); break;
		case EHitForward: iEikonEnv->EikAppUi()->HandleCommandL(EPmCmdForward); break;
		case EHitDelete: DeleteCurrentL(); break;
		case EHitArchive: iEikonEnv->EikAppUi()->HandleCommandL(EPmCmdArchive); break;
		case EHitFlag: ToggleFlagL('F'); break;
		case EHitWeb: ViewAsWebPageL(); break;
		case EHitLink:
		case EHitAttach:
			iFocusLink = link;
			Render();
			ActivateLinkL(link);
			break;
		case EHitTop: Scroll(-page); break;
		case EHitBottom: Scroll(page); break;
		default: break;
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
	// PsiWeb needs the serial port for web addresses (not for our own files)
	if (Clip(aUrl, 5).CompareF(_L("file:")) != 0)
		Cmd(PM_CMD_HANGUP, KNullDesC8, 0, KNullDesC8);
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
		if ((iDraft.iBody && iDraft.iBody->Length() > 0) || iDraft.iTo.Length())
			return iEikonEnv->QueryWinL(_L("Save as draft keeps it in the outbox"), _L("Discard this message?"));
		}
	return ETrue;
	}

// ----- preferences ---------------------------------------------------------------

void CPmPrefsDialog::PreLayoutDynInitL()
	{
	SetChoiceListCurrentItem(EPmDlgSort, iSettings.iSort >= 0 && iSettings.iSort <= 6 ? iSettings.iSort : 0);
	SetNumberEditorValue(EPmDlgPrefetch, PrefetchCount(iSettings));
	SetChoiceListCurrentItem(EPmDlgStore, iSettings.iStore ? 1 : 0);
	SetChoiceListCurrentItem(EPmDlgSmooth, iSettings.iMono ? 0 : 1);
	}

TBool CPmPrefsDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iSort = ChoiceListCurrentItem(EPmDlgSort);
	TInt ahead = NumberEditorValue(EPmDlgPrefetch);
	iSettings.iPrefetch = ahead > 0 ? ahead : -1;
	iSettings.iStore = ChoiceListCurrentItem(EPmDlgStore);
	iSettings.iMono = ChoiceListCurrentItem(EPmDlgSmooth) ? 0 : 1;
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

TBool CPmConnDialog::OkToExitL(TInt /*aButtonId*/)
	{
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
	TRAPD(pics, ToolbarPicturesL());
	(void)pics;                              // (no PsiMail.mbm: words only)
	iView = new(ELeave) CPmView;
	iView->ConstructL(ClientRect(), iSettings, iCalSettings);
	AddToStackL(iView);
	if (!iSettings.iAccounts[iSettings.iAcct].used)
		{
		if (EditAccountL(iSettings.iAcct, ETrue))
			iView->AccountChangedL();
		}
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
	CFbsBitmap* bmp = iEikonEnv->CreateBitmapL(mbm, aIcon);
	CleanupStack::PushL(bmp);
	CFbsBitmap* mask = iEikonEnv->CreateBitmapL(mbm, aIcon + 1);
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

// the mailbox and the reader have the standard toolbar; writing and the
// calendar are drawn screens that use all of it
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

CPmAppUi::~CPmAppUi()
	{
	if (iView)
		{
		RemoveFromStack(iView);
		delete iView;
		}
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
			iEikonEnv->InfoMsg(_L("No Agenda file there: open it in Agenda first"));
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
			why = _L("The disk is full. Make room, then try again.");
		else if (err == KErrNotReady || err == KErrPathNotFound)
			why = _L("The disk is not there. Is the card in?");
		else
			why.Format(_L("Not saved (error %d). Try again."), err);
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
		iView->Toast(_L("Wait for the message to download"));
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

CPmDraft* CPmAppUi::ForwardDraftL()
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
		iView->Toast(_L("Wait for the message to download"));
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
	if (iView->AttachmentCount())
		p.Append(_L("\n(The attachments are not forwarded: save them first and attach them.)\n"));
	CleanupStack::Pop();                      // body
	delete d->iBody;
	d->iBody = body;
	CleanupStack::Pop();                      // d
	CleanupStack::PopAndDestroy();            // h
	return d;
	}

void CPmAppUi::ForwardL()
	{
	CPmDraft* d = ForwardDraftL();
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

void CPmAppUi::SaveAttachmentL()
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
	CPmChoiceDialog* dlg = new(ELeave) CPmChoiceDialog(_L("Save attachment"), _L("Attachment"), names, choice);
	if (dlg->ExecuteLD(R_PM_CHOICE_DIALOG))
		iView->SaveAttachmentL(choice);
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
	m.Format(_L("Could not start the installer (%d). Open %S from the System screen."), err, &aFile);
	iEikonEnv->InfoWinL(_L("Update downloaded"), m);
	StartEngineL();
	}

_LIT(KUpdIniFile, "C:\\System\\Apps\\PsiMail\\Update.ini");

// Tools > Update PsiMail: where from, then the engine fetches and checks it
void CPmAppUi::UpdateL()
	{
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
	if (!src.Length())
		src = _L("github");
	CPmTextDialog* dlg = new(ELeave) CPmTextDialog(_L("Update PsiMail"), _L("Get it from"), src);
	if (!dlg->ExecuteLD(R_PM_UPDATE_DIALOG) || !src.Length())
		return;
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

// Folder > Go to folder: every folder, however many the column can show
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

void CPmAppUi::SearchL()
	{
	TBuf<60> words;
	CPmTextDialog* dlg = new(ELeave) CPmTextDialog(_L("Search this folder"), _L("Find"), words);
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
	if (!iEikonEnv->QueryWinL(qn, _L("Delete this account from PsiMail?")))
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
	SetLabelL(EPmDlgInfo3, iStatus);
	}

void CPmAppUi::DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	CPmView::TMode m = iView->Mode();
	TBool msg = (m == CPmView::EList || m == CPmView::EMessage) && iView->CurrentRow() != NULL;
	TBool native = m == CPmView::EList || m == CPmView::EOutbox || m == CPmView::EMessage || m == CPmView::ENoAccount;
	if (aMenuId == R_PM_FILE_MENU)
		{
		aMenuPane->SetItemButtonState(EPmCmdOffline, iSettings.iOffline ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemDimmed(EPmCmdHangup, !iView->Shared()->online);
		aMenuPane->SetItemDimmed(EPmCmdStop, !iView->Busy());
		}
	else if (aMenuId == R_PM_EDIT_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdDelete, !msg && !(m == CPmView::EOutbox && iView->CurrentRow()));
		aMenuPane->SetItemDimmed(EPmCmdMove, !msg);
		aMenuPane->SetItemDimmed(EPmCmdArchive, !msg);
		}
	else if (aMenuId == R_PM_MESSAGE_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdReplyMenu, !msg);
		aMenuPane->SetItemDimmed(EPmCmdForward, !msg);
		aMenuPane->SetItemDimmed(EPmCmdUnread, !msg);
		aMenuPane->SetItemDimmed(EPmCmdFlag, !msg);
		const TPmRow* row = msg ? iView->CurrentRow() : NULL;
		aMenuPane->SetItemButtonState(EPmCmdUnread, row && row->iFlags.Locate('S') < 0 ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemButtonState(EPmCmdFlag, row && row->iFlags.Locate('F') >= 0 ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemDimmed(EPmCmdSaveAttach, iView->AttachmentCount() == 0);
		aMenuPane->SetItemDimmed(EPmCmdWhole, m != CPmView::EMessage);
		aMenuPane->SetItemDimmed(EPmCmdWeb, m != CPmView::EMessage || !iView->HasHtml());
		aMenuPane->SetItemDimmed(EPmCmdNew, m == CPmView::ENoAccount);
		}
	else if (aMenuId == R_PM_VIEW_MENU)
		{
		aMenuPane->SetItemButtonState(EPmCmdToggleToolbar, (iSettings.iView & 1) ? 0 : EEikMenuItemSymbolOn);
		aMenuPane->SetItemButtonState(EPmCmdToggleTitle, (iSettings.iView & 2) ? 0 : EEikMenuItemSymbolOn);
		aMenuPane->SetItemButtonState(EPmCmdToggleFolders, (iSettings.iView & 4) ? 0 : EEikMenuItemSymbolOn);
		aMenuPane->SetItemDimmed(EPmCmdToggleToolbar, !native);
		aMenuPane->SetItemDimmed(EPmCmdToggleTitle, !native);
		aMenuPane->SetItemDimmed(EPmCmdToggleFolders, !native);
		aMenuPane->SetItemDimmed(EPmCmdSort, m != CPmView::EList);
		}
	else if (aMenuId == R_PM_FOLDER_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdOlder, m != CPmView::EList || iView->CurrentIsSearch());
		}
	else if (aMenuId == R_PM_REPLY_MENU || aMenuId == R_PM_REPLY_POPUP)
		{
		aMenuPane->SetItemDimmed(EPmCmdReply, !msg);
		aMenuPane->SetItemDimmed(EPmCmdReplyAll, !msg);
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
	if (iView->ModalCommandL(aCommand))
		return;
	CPmView::TMode m = iView->Mode();
	if (m == CPmView::ENoAccount && aCommand == EPmCmdEditAccount)
		aCommand = EPmCmdNewAccount;
	if (m == CPmView::ENoAccount && aCommand != EEikCmdExit && aCommand != EPmCmdNewAccount &&
		aCommand != EPmCmdConnSettings && aCommand != EPmCmdAbout && aCommand != EPmCmdUpdate &&
		aCommand != EPmCmdToggleToolbar && aCommand != EPmCmdToggleTitle && aCommand != EPmCmdToggleFolders &&
		aCommand != EPmCmdStatusInfo && aCommand != EPmCmdStop && aCommand != EPmCmdPrefs)
		{
		iView->Toast(_L("No account - add one with Tools > Accounts"));
		return;
		}
	switch (aCommand)
		{
	case EEikCmdExit:
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
		iView->Toast(iSettings.iOffline ? _L("Working offline: changes wait until you go online")
			: _L("Online: PsiMail will connect when it needs to"));
		break;
	case EPmCmdHangup:
		iView->Cmd(PM_CMD_HANGUP, KNullDesC8, 0, KNullDesC8);
		iView->Toast(_L("Disconnecting..."));
		break;
	case EPmCmdAbout:
		AboutL();
		break;
	case EPmCmdNew:
		// in the calendar, Ctrl+N makes an event
		if (iView->Mode() == CPmView::ECalendar || iView->Mode() == CPmView::ECalEvent)
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
		break;
	case EPmCmdPrefs:
		{
		TInt sort = iSettings.iSort;
		CPmPrefsDialog* dlg = new(ELeave) CPmPrefsDialog(iSettings, sort);
		if (dlg->ExecuteLD(R_PM_PREFS_DIALOG))
			{
			SaveSettings();
			iView->SettingsChanged();
			if (sort != iSettings.iSort)
				iView->SortL(sort);
			iView->Render();
			}
		break;
		}
	case EPmCmdTool4:
		if (m == CPmView::EMessage)
			iView->BackL();
		else if (m == CPmView::EList || m == CPmView::EOutbox)
			iView->DeleteCurrentL();
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
		SaveAttachmentL();
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
	case EPmCmdNewEvent:
		iView->NewEventL();
		break;
	case EPmCmdCalSettings:
		EditCalendarL();
		break;
	case EPmCmdSmooth:
		iSettings.iMono = !iSettings.iMono;
		SaveSettings();
		iView->Render();
		break;
	case EPmCmdFolders:
		FoldersL();
		break;
	case EPmCmdUpdate:
		UpdateL();
		break;
	case EPmCmdRefresh:
		iView->RefreshL();
		break;
	case EPmCmdOlder:
		iView->OlderL();
		break;
	case EPmCmdSearch:
		SearchL();
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
