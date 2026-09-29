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
#include <eikmfne.h>
#include <eikseced.h>
#include <eikcfdlg.h>
#include <eikon.rsg>
#include <apgcli.h>
#include <txtetext.h>
#include "pmapp.h"

_LIT(KEngineExe, "psimail.exe");
_LIT(KIniFile, "C:\\System\\Apps\\PsiMail\\PsiMail.ini");
_LIT(KVersion, "0.1");          // also pkg/psimail.pkg
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
	delete iTimer;
	delete iWatcher;
	delete iFolders;
	delete iRows;
	delete iText;
	delete iLineStart;
	delete iLineLen;
	delete iAttNames;
	delete iAttParts;
	if (iBold)
		iCoeEnv->ReleaseScreenFont(iBold);
	if (iChunkOpen)
		iChunk.Close();
	}

void CPmView::ConstructL(const TRect& aRect, TPmSettings& aSettings)
	{
	iSettings = &aSettings;
	CreateWindowL();
	SetRectL(aRect);
	iFolders = new(ELeave) CArrayFixFlat<TPmFolder>(16);
	iRows = new(ELeave) CArrayFixFlat<TPmRow>(32);
	iLineStart = new(ELeave) CArrayFixFlat<TInt>(256);
	iLineLen = new(ELeave) CArrayFixFlat<TInt>(256);
	iAttNames = new(ELeave) CDesCArrayFlat(4);
	iAttParts = new(ELeave) CDesC8ArrayFlat(4);

	iFont = iEikonEnv->NormalFont();
	TFontSpec spec = iFont->FontSpecInTwips();
	spec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	iBold = iCoeEnv->CreateScreenFontL(spec);
	iLineH = iFont->HeightInPixels() + 3;
	iAscent = iFont->AscentInPixels() + 1;

	TInt r = iChunk.CreateGlobal(_L(PSI_SHARED_NAME), sizeof(PmShared), sizeof(PmShared));
	if (r == KErrAlreadyExists)
		r = iChunk.OpenGlobal(_L(PSI_SHARED_NAME), EFalse);
	User::LeaveIfError(r);
	iChunkOpen = ETrue;
	iShared = (PmShared*)iChunk.Base();
	Mem::FillZ(iShared, sizeof(PmShared));

	iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
	iTimer->Start(KTick, KTick, TCallBack(TickCallback, this));
	ActivateL();
	StartEngineL();
	AccountChangedL();
	}

void CPmView::StoreDir(TDes& aDir) const
	{
	FromC(aDir, iShared->store_dir);
	}

void CPmView::CopySettingsToShared()
	{
	PmShared* s = iShared;
	s->net.baud_index = iSettings->iBaudIndex;
	s->net.rtscts = iSettings->iRtsCts;
	s->net.net_mode = iSettings->iNetMode;
	s->offline = iSettings->iOffline;
	Mem::Copy(s->acct, iSettings->iAccounts, sizeof(s->acct));
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
	Redraw();
	}

TBool CPmView::Busy() const
	{
	return iShared->busy || iShared->cmd_head != iShared->cmd_tail;
	}

void CPmView::Cmd(TInt aOp, const TDesC8& aFolder, TUint aUid, const TDesC8& aArg)
	{
	PmShared* s = iShared;
	if (!iRunning)
		{
		iEikonEnv->InfoMsg(_L("The mail engine is not running"));
		return;
		}
	if (s->cmd_head - s->cmd_tail >= PM_CMDQ)
		{
		iEikonEnv->InfoMsg(_L("Busy - try again in a moment"));
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
	Redraw();
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
		Redraw();
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
	Redraw();
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
			iRows->InsertL(0, row);            // newest first
			}
		CleanupStack::PopAndDestroy();
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
	iAttParts->Reset();
	iTruncated = 0;
	TBuf<150> path;
	MsgPath(iMsgUid, _L("txt"), path);
	HBufC* buf = NULL;
	ReadFileL(path, buf, 300 * 1024);
	iWaitingBody = buf == NULL;
	iText = buf;
	if (iText)
		{
		// "#PSIMAIL1 TAB bytes-not-downloaded TAB html" first
		TPtr t = iText->Des();
		if (t.Length() && t[0] == '#')
			{
			TInt nl = t.Locate('\n');
			TPtrC l = nl >= 0 ? t.Left(nl) : TPtrC(t);
			NextField(l);
			iTruncated = ToInt(NextField(l));
			t.Delete(0, nl >= 0 ? nl + 1 : t.Length());
			}
		// the attachments
		MsgPath(iMsgUid, _L("att"), path);
		HBufC* att = NULL;
		ReadFileL(path, att, 16 * 1024);
		if (att)
			{
			CleanupStack::PushL(att);
			TPtrC rest = *att;
			while (rest.Length())
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
				TBuf<120> shown;
				SafeCopy(shown, name.Left(name.Length() < 90 ? name.Length() : 90));
				// base64 is 4/3 of the file
				TInt kb = (size * 3 / 4 + 1023) / 1024;
				shown.AppendFormat(_L(" (%d KB)"), kb);
				iAttNames->AppendL(shown);
				TBuf8<16> p8;
				SafeCopy(p8, part);
				iAttParts->AppendL(p8);
				}
			CleanupStack::PopAndDestroy();
			}
		}
	WrapMessageL();
	}

// word-wraps the message into screen lines
void CPmView::WrapMessageL()
	{
	iLineStart->Reset();
	iLineLen->Reset();
	iHeaderLines = 0;
	if (!iText)
		return;
	TPtr t = iText->Des();
	for (TInt i = 0; i < t.Length(); i++)
		if (t[i] == '\t') t[i] = ' ';
		else if (t[i] == '\r') t[i] = ' ';
	TInt width = Rect().Width() - 8;
	TInt pos = 0, len = t.Length();
	TBool inHeader = ETrue;
	while (pos < len)
		{
		TInt nl = t.Mid(pos).Locate('\n');
		TInt end = nl >= 0 ? pos + nl : len;
		TPtrC para = t.Mid(pos, end - pos);
		if (inHeader)
			{
			if (para.Length() == 0)
				{
				inHeader = EFalse;
				}
			else if (!iAllHeaders && (para.Find(_L("Reply-To: ")) == 0 || para.Find(_L("Message-ID: ")) == 0))
				{
				pos = end + 1;
				continue;
				}
			}
		if (para.Length() == 0)
			{
			iLineStart->AppendL(pos);
			iLineLen->AppendL(0);
			}
		TInt p = 0;
		while (p < para.Length())
			{
			TPtrC rest = para.Mid(p);
			TInt fit = iFont->TextCount(rest, width);
			if (fit <= 0) fit = 1;
			if (fit < rest.Length())
				{
				TInt sp = fit;
				while (sp > 0 && rest[sp] != ' ') sp--;
				if (sp > fit / 3) fit = sp + 1;
				}
			TInt shown = fit;
			while (shown > 0 && rest[shown - 1] == ' ' && fit < rest.Length()) shown--;
			iLineStart->AppendL(pos + p);
			iLineLen->AppendL(shown);
			p += fit;
			}
		if (inHeader)
			iHeaderLines = iLineStart->Count();
		pos = end + 1;
		}
	}

void CPmView::ReloadL()
	{
	switch (iMode)
		{
	case EFolders:
		LoadFoldersL();
		break;
	case EList:
		LoadFoldersL();
		LoadListL();
		break;
	case EOutbox:
		LoadOutboxL();
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
	Redraw();
	}

// ============================================================================
// Navigation
// ============================================================================

void CPmView::ShowFoldersL()
	{
	LoadFoldersL();
	iMode = EFolders;
	iSel = 0;
	for (TInt i = 0; i < iFolders->Count(); i++)
		if ((*iFolders)[i].iImap == iFolder) iSel = i;
	iTop = 0;
	EnsureVisible();
	Redraw();
	}

void CPmView::OpenFolderL(const TDesC8& aImap)
	{
	iFolder = aImap;
	iSearch = EFalse;
	iMode = EList;
	iListMode = EList;
	iSel = iTop = 0;
	iRows->Reset();
	LoadListL();
	iSel = 0;
	iTop = 0;
	Redraw();
	// always look for changes when a folder is opened
	if (!iSettings->iOffline)
		Cmd(PM_CMD_SYNC, iFolder, 0, KNullDesC8);
	}

void CPmView::ShowOutboxL()
	{
	iMode = EOutbox;
	iListMode = EOutbox;
	iSel = iTop = 0;
	LoadOutboxL();
	Redraw();
	}

TBool CPmView::HasSelection() const
	{
	if (iMode == EMessage) return ETrue;
	if (iMode == EList || iMode == EOutbox) return iSel >= 0 && iSel < iRows->Count();
	return EFalse;
	}

const TPmRow* CPmView::CurrentRow() const
	{
	if ((iMode == EList || iMode == EOutbox || iMode == EMessage) && iSel >= 0 && iSel < iRows->Count())
		return &(*iRows)[iSel];
	return NULL;
	}

void CPmView::OpenCurrentL()
	{
	if (iMode == EFolders)
		{
		if (iSel >= 0 && iSel < iFolders->Count())
			{
			if ((*iFolders)[iSel].iKind == 'N')
				{
				iEikonEnv->InfoMsg(_L("That folder holds only other folders"));
				return;
				}
			OpenFolderL((*iFolders)[iSel].iImap);
			}
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
	iMsgUid = row.iUid;
	iMode = EMessage;
	iMsgTop = 0;
	if (row.iFlags.Locate('S') < 0)
		row.iFlags.Append('S');                  // the engine marks it read
	LoadMessageL();
	if (iWaitingBody)
		{
		if (iSettings->iOffline)
			SetStatus(_L("Not downloaded - you are working offline"));
		else
			Cmd(PM_CMD_BODY, iFolder, iMsgUid, KNullDesC8);
		}
	else if (row.iFlags.Locate('S') >= 0)
		{
		// already here: tell the server it's been read (does nothing if it knew)
		}
	Redraw();
	}

void CPmView::BackL()
	{
	if (iMode == EMessage)
		{
		iMode = iListMode;
		if (iMode == EList) LoadListL(); else LoadOutboxL();
		}
	else if (iMode == EList && iSearch)
		{
		iSearch = EFalse;
		LoadListL();
		}
	else if (iMode == EList || iMode == EOutbox)
		ShowFoldersL();
	Redraw();
	}

void CPmView::StepMessageL(TInt aDir)
	{
	if (iMode != EMessage)
		return;
	TInt s = iSel + aDir;
	if (s < 0 || s >= iRows->Count())
		{
		iEikonEnv->InfoMsg(aDir > 0 ? _L("That was the oldest message") : _L("That was the newest message"));
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
	const TPmFolder* f = CurrentFolder();
	TBool forGood = f && f->iKind == 'T';
	if (forGood && !iEikonEnv->QueryWinL(_L("Delete this message for good?"), _L("It is in the Trash already")))
		return;
	Cmd(PM_CMD_MOVE, iFolder, row->iUid, KNullDesC8);
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
	iEikonEnv->InfoMsg(forGood ? _L("Deleted") : _L("Moved to the Trash"));
	Redraw();
	}

void CPmView::MoveCurrentL(const TDesC8& aDest)
	{
	const TPmRow* row = CurrentRow();
	if (!row || (iMode != EList && iMode != EMessage))
		return;
	if (aDest == iFolder)
		{
		iEikonEnv->InfoMsg(_L("It is in that folder already"));
		return;
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
	Redraw();
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
		iEikonEnv->InfoMsg(at >= 0 ? _L("Marked unread") : _L("Marked read"));
	else
		iEikonEnv->InfoMsg(at >= 0 ? _L("Flag removed") : _L("Flagged"));
	Redraw();
	}

void CPmView::RefreshL()
	{
	if (iMode == EFolders)
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
	if (iMode == EFolders || iMode == EOutbox || iMode == ENoAccount)
		iFolder = _L8("INBOX");
	SafeCopy(iSearchWords, aWords);
	TBuf8<PM_ARG_MAX> w;
	SafeCopy(w, aWords);
	Cmd(PM_CMD_SEARCH, iFolder, 0, w);
	}

void CPmView::SendRecvL()
	{
	Cmd(PM_CMD_SENDRECV, iMode == EList || iMode == EMessage ? TPtrC8(iFolder) : TPtrC8(_L8("INBOX")), 0, KNullDesC8);
	}

void CPmView::WholeMessageL()
	{
	if (iMode != EMessage)
		return;
	if (iTruncated <= 0)
		{
		iEikonEnv->InfoMsg(_L("You have the whole message"));
		return;
		}
	iWaitingBody = ETrue;
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

void CPmView::ToggleHeaders()
	{
	iAllHeaders = !iAllHeaders;
	TRAPD(err, WrapMessageL());
	Redraw();
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

// the message's text with "> " before each line
void CPmView::QuoteBodyL(TDes& aOut, TInt aMaxLines) const
	{
	if (!iText)
		return;
	TPtrC t = *iText;
	TInt blank = t.Find(_L("\n\n"));
	if (blank < 0)
		return;
	TPtrC rest = t.Mid(blank + 2);
	TInt n = 0;
	while (rest.Length() && n < aMaxLines)
		{
		TInt nl = rest.Locate('\n');
		TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
		if (aOut.Length() + line.Length() + 4 > aOut.MaxLength())
			break;
		aOut.Append(line.Length() && line[0] == '>' ? _L(">") : _L("> "));
		aOut.Append(line);
		aOut.Append('\n');
		n++;
		}
	if (rest.Length() && aOut.Length() + 8 < aOut.MaxLength())
		aOut.Append(_L("> ...\n"));
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
	TFileName path(dir);
	path.AppendNum(row->iUid);
	path.Append(_L(".txt"));
	HBufC* buf = NULL;
	ReadFileL(path, buf, 256 * 1024);
	if (!buf)
		return;
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
	TBuf<600> h;
	h = _L("#PSIMAIL1\n");
	f.Write(h);
	h = _L("To: "); h.Append(aDraft.iTo); h.Append('\n'); f.Write(h);
	if (aDraft.iCc.Length()) { h = _L("Cc: "); h.Append(aDraft.iCc); h.Append('\n'); f.Write(h); }
	if (aDraft.iBcc.Length()) { h = _L("Bcc: "); h.Append(aDraft.iBcc); h.Append('\n'); f.Write(h); }
	h = _L("Subject: "); h.Append(aDraft.iSubject); h.Append('\n'); f.Write(h);
	if (aDraft.iInReplyTo.Length()) { h = _L("In-Reply-To: "); h.Append(aDraft.iInReplyTo); h.Append('\n'); f.Write(h); }
	if (aDraft.iReferences.Length()) { h = _L("References: "); h.Append(aDraft.iReferences); h.Append('\n'); f.Write(h); }
	for (TInt i = 0; i < aDraft.iAttach->Count(); i++)
		{
		h = _L("Attach: ");
		h.Append((*aDraft.iAttach)[i].Left(500));
		h.Append('\n');
		f.Write(h);
		}
	if (aDraft.iReplyFolder.Length())
		{
		h = _L("Reply-Folder: "); h.Append(aDraft.iReplyFolder); h.Append('\n'); f.Write(h);
		h = _L("Reply-Uid: "); h.AppendNum(aDraft.iReplyUid); h.Append('\n'); f.Write(h);
		}
	if (!aSend)
		f.Write(_L("Draft: 1\n"));
	f.Write(_L("\n"));
	if (aDraft.iBody)
		f.Write(*aDraft.iBody);
	TInt r = f.Flush();
	CleanupStack::PopAndDestroy();      // f
	User::LeaveIfError(r);
	fs.Delete(path);
	fs.Delete(err);
	User::LeaveIfError(fs.Rename(tmp, path));
	aDraft.iFileNo = no;
	if (aSend)
		{
		if (iSettings->iOffline)
			iEikonEnv->InfoMsg(_L("In the outbox - it goes at the next Send & receive"));
		else
			{
			Cmd(PM_CMD_SEND, KNullDesC8, 0, KNullDesC8);
			iEikonEnv->InfoMsg(_L("Sending..."));
			}
		}
	else
		iEikonEnv->InfoMsg(_L("Saved in the outbox"));
	if (iMode == EOutbox)
		LoadOutboxL();
	Redraw();
	}

void CPmView::DeleteOutboxL()
	{
	const TPmRow* row = CurrentRow();
	if (!row)
		return;
	if (!iEikonEnv->QueryWinL(_L("Delete this message?"), row->iSubject))
		return;
	TBuf<120> dir;
	OutboxDir(dir);
	RFs& fs = iCoeEnv->FsSession();
	TFileName p(dir);
	p.AppendNum(row->iUid); p.Append(_L(".txt"));
	TInt r = fs.Delete(p);
	if (r != KErrNone)
		{
		p = dir; p.AppendFormat(_L("%04d.txt"), row->iUid);
		fs.Delete(p);
		}
	p = dir; p.AppendFormat(_L("%04d.err"), row->iUid); fs.Delete(p);
	p = dir; p.AppendNum(row->iUid); p.Append(_L(".err")); fs.Delete(p);
	LoadOutboxL();
	Redraw();
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
	if (s->changed_seq != iChangedSeen)
		{
		iChangedSeen = s->changed_seq;
		ReloadL();
		}
	if (s->done_seq != iDoneSeen)
		{
		iDoneSeen = s->done_seq;
		PmCmd cmd = iSent[(iDoneSeen - 1) % PM_CMDQ];
		HandleResultL(cmd);
		redraw = ETrue;
		}
	// progress
	TBuf<128> prog;
	FromC(prog, s->progress);
	if (prog != iLastProgress || s->busy != iBusyWas)
		{
		iLastProgress = prog;
		iBusyWas = s->busy;
		redraw = ETrue;
		}
	if (redraw)
		{
		ActivateGc();
		DrawStatus(SystemGc());
		DrawTitle(SystemGc());
		DeactivateGc();
		}
	}

void CPmView::HandleResultL(const PmCmd& aCmd)
	{
	PmShared* s = iShared;
	TBuf<160> msg;
	FromC(msg, s->last_msg);
	TInt res = s->last_res;
	iStatus = msg;
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
			iEikonEnv->InfoMsg(msg);
		else if (aCmd.op == PM_CMD_SEND || aCmd.op == PM_CMD_SENDRECV)
			iEikonEnv->InfoMsg(msg);
		if (aCmd.op == PM_CMD_BODY || aCmd.op == PM_CMD_FULLBODY)
			{
			if (iMode == EMessage && aCmd.uid == iMsgUid)
				{
				TInt top = iMsgTop;
				LoadMessageL();
				if (aCmd.op == PM_CMD_FULLBODY) iMsgTop = top;
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
		lines[2].Append(fp.Left(95));
		lines[3] = _L("Trust it only if you expected this (e.g. your own server).");
		CPmInfoDialog* dlg = new(ELeave) CPmInfoDialog(_L("Certificate not trusted"), lines, 4);
		dlg->ExecuteLD(R_PM_INFO_DIALOG);
		if (iEikonEnv->QueryWinL(_L("Trust this server's key from now on?"), host))
			{
			TBuf8<80> hp;
			hp.Copy(host);
			Cmd(PM_CMD_TRUST, KNullDesC8, 0, hp);
			// and try again
			Cmd(aCmd.op, TPtrC8((const TUint8*)aCmd.folder), aCmd.uid, TPtrC8((const TUint8*)aCmd.arg));
			}
		break;
		}
	case PM_RES_NEED_PASS:
	case PM_RES_LOGIN_FAILED:
		{
		PmAccount& a = iSettings->iAccounts[iSettings->iAcct];
		if (res == PM_RES_LOGIN_FAILED)
			iEikonEnv->InfoWinL(_L("The server refused the login"), msg.Left(100));
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
		if (aCmd.op == PM_CMD_BODY && iMode == EMessage && aCmd.uid == iMsgUid)
			{
			iMsg1 = _L("Could not download this message:");
			iMsg2 = msg;
			}
		iEikonEnv->InfoMsg(msg);
		ReloadL();
		break;
	case PM_RES_CANCELLED:
		iEikonEnv->InfoMsg(_L("Stopped"));
		break;
	default:
		if (aCmd.op == PM_CMD_BODY || aCmd.op == PM_CMD_ATTACH || aCmd.op == PM_CMD_SEND ||
			aCmd.op == PM_CMD_SENDRECV || aCmd.op == PM_CMD_SEARCH)
			iEikonEnv->InfoWinL(_L("PsiMail"), msg.Left(120));
		else
			iEikonEnv->InfoMsg(msg);
		ReloadL();
		break;
		}
	}

void CPmView::SetStatus(const TDesC& aText)
	{
	SafeCopy(iStatus, aText);
	Redraw();
	}

void CPmView::Redraw()
	{
	DrawNow();
	}

// ============================================================================
// Drawing
// ============================================================================

TInt CPmView::Rows() const
	{
	TInt n = BodyRect().Height() / iLineH;
	return n > 0 ? n : 1;
	}

TRect CPmView::BodyRect() const
	{
	TRect r = Rect();
	r.iTl.iY += iLineH + 2;        // title
	r.iBr.iY -= iLineH + 2;        // status
	return r;
	}

void CPmView::Title(TDes& aTitle) const
	{
	aTitle.Zero();
	PmAccount& a = iSettings->iAccounts[iSettings->iAcct];
	TBuf<32> acct;
	FromC(acct, a.name);
	switch (iMode)
		{
	case ENoAccount:
		aTitle = _L("PsiMail");
		break;
	case EFolders:
		aTitle = acct;
		aTitle.Append(_L(" - folders"));
		break;
	case EOutbox:
		aTitle = acct;
		aTitle.AppendFormat(_L(" - outbox (%d)"), iRows->Count());
		break;
	default:
		{
		const TPmFolder* f = CurrentFolder();
		aTitle = acct;
		aTitle.Append(_L(" - "));
		if (f)
			aTitle.Append(f->iName.Left(40));
		else
			{
			TBuf<40> n;
			n.Copy(iFolder.Left(40));
			aTitle.Append(n);
			}
		if (iSearch)
			{
			aTitle.Append(_L(" - search: "));
			aTitle.Append(iSearchWords.Left(30));
			}
		else if (iMode == EList && f && f->iUnread > 0)
			aTitle.AppendFormat(_L(" (%d unread)"), f->iUnread);
		if (iMode == EMessage && iRows->Count())
			aTitle.AppendFormat(_L("   %d of %d"), iSel + 1, iRows->Count());
		break;
		}
		}
	}

void CPmView::DrawTitle(CWindowGc& aGc) const
	{
	TRect r = Rect();
	r.iBr.iY = r.iTl.iY + iLineH + 2;
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(KRgbBlack);
	aGc.SetPenColor(KRgbWhite);
	aGc.UseFont(iBold);
	TBuf<160> t;
	Title(t);
	TBuf<24> right;
	if (iShared->online)
		right = _L("online");
	if (iSettings->iOffline)
		right = _L("offline");
	TInt rw = iBold->TextWidthInPixels(right) + 8;
	TRect left(r.iTl, TPoint(r.iBr.iX - rw, r.iBr.iY));
	aGc.DrawText(t, left, iAscent + 1, CGraphicsContext::ELeft, 4);
	aGc.DrawText(right, TRect(TPoint(r.iBr.iX - rw, r.iTl.iY), r.iBr), iAscent + 1, CGraphicsContext::ERight, 4);
	aGc.DiscardFont();
	}

void CPmView::DrawStatus(CWindowGc& aGc) const
	{
	TRect r = Rect();
	r.iTl.iY = r.iBr.iY - iLineH - 2;
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(TRgb::Gray4(2));
	aGc.SetPenColor(KRgbBlack);
	aGc.UseFont(iFont);
	TBuf<160> t;
	if (iShared->busy && iLastProgress.Length())
		t = iLastProgress;
	else if (iShared->busy || iShared->cmd_head != iShared->cmd_tail)
		t = _L("Working...");
	else
		t = iStatus;
	TBuf<40> right;
	if (iShared->busy)
		right = _L("Esc stops");
	else if (iMode == EMessage && iAttNames->Count())
		right.Format(_L("%d attached - Ctrl+S saves"), iAttNames->Count());
	TInt rw = right.Length() ? iFont->TextWidthInPixels(right) + 8 : 0;
	aGc.DrawText(t, TRect(r.iTl, TPoint(r.iBr.iX - rw, r.iBr.iY)), iAscent + 1, CGraphicsContext::ELeft, 4);
	if (rw)
		aGc.DrawText(right, TRect(TPoint(r.iBr.iX - rw, r.iTl.iY), r.iBr), iAscent + 1, CGraphicsContext::ERight, 4);
	aGc.DiscardFont();
	}

void CPmView::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.DrawRect(BodyRect());
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	DrawTitle(gc);
	DrawStatus(gc);
	gc.SetBrushColor(KRgbWhite);
	gc.SetPenColor(KRgbBlack);
	switch (iMode)
		{
	case EFolders:
		DrawFolders(gc);
		break;
	case EList:
	case EOutbox:
		DrawList(gc);
		break;
	case EMessage:
		DrawMessage(gc);
		break;
	case ENoAccount:
		{
		gc.UseFont(iBold);
		TRect b = BodyRect();
		gc.DrawText(_L("Welcome to PsiMail"), TPoint(b.iTl.iX + 20, b.iTl.iY + 40));
		gc.DiscardFont();
		gc.UseFont(iFont);
		gc.DrawText(_L("Set up your mail account with Tools > New account (Ctrl+K)."), TPoint(b.iTl.iX + 20, b.iTl.iY + 40 + 2 * iLineH));
		gc.DrawText(_L("For Fastmail, make an app password at Settings > Privacy & Security."), TPoint(b.iTl.iX + 20, b.iTl.iY + 40 + 3 * iLineH));
		gc.DiscardFont();
		break;
		}
		}
	}

void CPmView::DrawFolders(CWindowGc& aGc) const
	{
	TRect b = BodyRect();
	TInt rows = Rows();
	if (iFolders->Count() == 0)
		{
		aGc.UseFont(iFont);
		aGc.DrawText(_L("No folders yet: File > Send & receive (Ctrl+G) fetches them."), TPoint(b.iTl.iX + 8, b.iTl.iY + iAscent + 4));
		aGc.DiscardFont();
		return;
		}
	for (TInt i = 0; i < rows && iTop + i < iFolders->Count(); i++)
		{
		const TPmFolder& f = (*iFolders)[iTop + i];
		TRect row(b.iTl.iX, b.iTl.iY + i * iLineH, b.iBr.iX, b.iTl.iY + (i + 1) * iLineH);
		TBool sel = iTop + i == iSel;
		aGc.SetBrushColor(sel ? KRgbBlack : KRgbWhite);
		aGc.SetPenColor(sel ? KRgbWhite : KRgbBlack);
		const CFont* font = f.iUnread > 0 ? iBold : iFont;
		aGc.UseFont(font);
		TBuf<100> name;
		// indent subfolders ("Work/Projects" -> "  Projects")
		TInt depth = 0, last = -1;
		for (TInt k = 0; k < f.iName.Length(); k++)
			if (f.iName[k] == '/' || f.iName[k] == '.') { depth++; last = k; }
		if (f.iKind != '-' && f.iKind != 'N') { depth = 0; last = -1; }
		for (TInt d = 0; d < depth && d < 6; d++) name.Append(_L("   "));
		name.Append(f.iName.Mid(last + 1).Left(60));
		TBuf<32> count;
		if (f.iUnread > 0) count.Format(_L("%d new  "), f.iUnread);
		if (f.iTotal >= 0 && f.iKind != 'N') count.AppendFormat(_L("%d"), f.iTotal);
		TInt cw = font->TextWidthInPixels(count) + 10;
		aGc.DrawText(name, TRect(row.iTl, TPoint(row.iBr.iX - cw, row.iBr.iY)), iAscent + 1, CGraphicsContext::ELeft, 8);
		aGc.DrawText(count, TRect(TPoint(row.iBr.iX - cw, row.iTl.iY), row.iBr), iAscent + 1, CGraphicsContext::ERight, 6);
		aGc.DiscardFont();
		}
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
	TTimeIntervalDays ago = now.DaysFrom(t);
	if (d.Year() == n.Year() && d.Month() == n.Month() && d.Day() == n.Day())
		aOut.Format(_L("%02d:%02d"), d.Hour(), d.Minute());
	else if (ago.Int() >= 0 && ago.Int() < 6)
		aOut.Format(_L("%s %02d:%02d"), KDays[t.DayNoInWeek()], d.Hour(), d.Minute());
	else if (d.Year() == n.Year())
		aOut.Format(_L("%d %s"), d.Day() + 1, KMonths[d.Month()]);
	else
		aOut.Format(_L("%d %s %02d"), d.Day() + 1, KMonths[d.Month()], d.Year() % 100);
	}

void CPmView::DrawList(CWindowGc& aGc) const
	{
	TRect b = BodyRect();
	TInt rows = Rows();
	if (iRows->Count() == 0)
		{
		aGc.UseFont(iFont);
		const TDesC& t = iMode == EOutbox ? _L("The outbox is empty.")
			: iSearch ? _L("Nothing found.")
			: (iShared->busy ? _L("Fetching...") : _L("No messages here. Ctrl+G to send and receive."));
		aGc.DrawText(t, TPoint(b.iTl.iX + 8, b.iTl.iY + iAscent + 4));
		aGc.DiscardFont();
		return;
		}
	TInt w = b.Width();
	TInt flagW = iFont->TextWidthInPixels(_L("@!")) + 10;
	TInt fromW = w * 28 / 100;
	TInt dateW = iFont->TextWidthInPixels(_L("Wed 00:00")) + 10;
	for (TInt i = 0; i < rows && iTop + i < iRows->Count(); i++)
		{
		const TPmRow& row = (*iRows)[iTop + i];
		TInt y0 = b.iTl.iY + i * iLineH, y1 = y0 + iLineH;
		TBool sel = iTop + i == iSel;
		TBool unread = iMode == EList && row.iFlags.Locate('S') < 0;
		aGc.SetBrushColor(sel ? KRgbBlack : KRgbWhite);
		aGc.SetPenColor(sel ? KRgbWhite : KRgbBlack);
		const CFont* font = unread ? iBold : iFont;
		aGc.UseFont(font);
		TBuf<4> flags;
		if (iMode == EOutbox)
			{
			if (row.iFlags.Locate('E') >= 0) flags.Append('!');
			else if (row.iFlags.Locate('D') >= 0) flags.Append('d');
			else flags.Append('>');
			}
		else
			{
			flags.Append(unread ? '*' : row.iFlags.Locate('A') >= 0 ? 'r' : ' ');
			if (row.iFlags.Locate('F') >= 0) flags.Append('!');
			else if (row.iFlags.Locate('T') >= 0) flags.Append('@');
			}
		TInt x = b.iTl.iX;
		aGc.DrawText(flags, TRect(x, y0, x + flagW, y1), iAscent + 1, CGraphicsContext::ELeft, 4);
		x += flagW;
		// cut to fit
		TBuf<64> from(row.iFrom);
		TInt fit = font->TextCount(from, fromW - 8);
		if (fit < from.Length()) from.SetLength(fit);
		aGc.DrawText(from, TRect(x, y0, x + fromW, y1), iAscent + 1, CGraphicsContext::ELeft, 0);
		x += fromW;
		TBuf<16> date;
		if (iMode == EOutbox)
			{
			if (row.iFlags.Locate('E') >= 0) date = _L("failed");
			else if (row.iFlags.Locate('D') >= 0) date = _L("draft");
			else date = _L("to send");
			}
		else
			FormatDate(row.iDate, date);
		TBuf<100> subj(row.iSubject.Length() ? TPtrC(row.iSubject) : TPtrC(_L("(no subject)")));
		TInt sw = b.iBr.iX - dateW - x;
		fit = font->TextCount(subj, sw - 6);
		if (fit < subj.Length()) subj.SetLength(fit);
		aGc.DrawText(subj, TRect(x, y0, x + sw, y1), iAscent + 1, CGraphicsContext::ELeft, 0);
		aGc.DrawText(date, TRect(b.iBr.iX - dateW, y0, b.iBr.iX, y1), iAscent + 1, CGraphicsContext::ERight, 4);
		aGc.DiscardFont();
		}
	// how far down the list we are
	if (iRows->Count() > rows)
		{
		TInt h = b.Height();
		TInt th = h * rows / iRows->Count();
		if (th < 6) th = 6;
		TInt ty = b.iTl.iY + (h - th) * iTop / (iRows->Count() - rows);
		aGc.SetBrushColor(TRgb::Gray4(1));
		aGc.SetPenStyle(CGraphicsContext::ENullPen);
		aGc.DrawRect(TRect(b.iBr.iX - 3, ty, b.iBr.iX, ty + th));
		aGc.SetPenStyle(CGraphicsContext::ESolidPen);
		}
	}

void CPmView::DrawMessage(CWindowGc& aGc) const
	{
	TRect b = BodyRect();
	aGc.SetBrushColor(KRgbWhite);
	aGc.SetPenColor(KRgbBlack);
	if (!iText)
		{
		aGc.UseFont(iBold);
		const TPmRow* row = CurrentRow();
		if (row)
			aGc.DrawText(row->iSubject, TPoint(b.iTl.iX + 8, b.iTl.iY + iAscent + 4));
		aGc.DiscardFont();
		aGc.UseFont(iFont);
		if (iMsg1.Length() && !iShared->busy)
			{
			aGc.DrawText(iMsg1, TPoint(b.iTl.iX + 8, b.iTl.iY + iAscent + 4 + 2 * iLineH));
			aGc.DrawText(iMsg2, TPoint(b.iTl.iX + 8, b.iTl.iY + iAscent + 4 + 3 * iLineH));
			}
		else
			aGc.DrawText(_L("Downloading the message..."), TPoint(b.iTl.iX + 8, b.iTl.iY + iAscent + 4 + 2 * iLineH));
		aGc.DiscardFont();
		return;
		}
	TInt rows = Rows();
	TInt total = iLineStart->Count();
	for (TInt i = 0; i < rows && iMsgTop + i < total; i++)
		{
		TInt k = iMsgTop + i;
		TPtrC line = iText->Mid((*iLineStart)[k], (*iLineLen)[k]);
		TInt y = b.iTl.iY + i * iLineH + iAscent + 1;
		if (k < iHeaderLines)
			{
			// "From: x" with the label in bold
			TInt c = line.Locate(':');
			TInt st = (*iLineStart)[k];
			TBool paraStart = st == 0 || (*iText)[st - 1] == '\n';
			if (c > 0 && c < 14 && paraStart)
				{
				aGc.UseFont(iBold);
				TPtrC label = line.Left(c + 1);
				aGc.DrawText(label, TPoint(b.iTl.iX + 4, y));
				TInt lw = iBold->TextWidthInPixels(label);
				aGc.DiscardFont();
				aGc.UseFont(iFont);
				aGc.DrawText(line.Mid(c + 1), TPoint(b.iTl.iX + 4 + lw, y));
				aGc.DiscardFont();
				continue;
				}
			}
		aGc.UseFont(iFont);
		aGc.DrawText(line, TPoint(b.iTl.iX + 4, y));
		aGc.DiscardFont();
		}
	// a rule under the headers
	if (iHeaderLines > iMsgTop && iHeaderLines - iMsgTop < rows)
		{
		TInt y = b.iTl.iY + (iHeaderLines - iMsgTop) * iLineH + iLineH / 2;
		aGc.SetPenColor(TRgb::Gray4(1));
		aGc.DrawLine(TPoint(b.iTl.iX + 4, y), TPoint(b.iBr.iX - 4, y));
		aGc.SetPenColor(KRgbBlack);
		}
	// attachments after the end of the text
	if (iMsgTop + rows > total && iAttNames->Count())
		{
		TInt i = total - iMsgTop + 1;
		aGc.UseFont(iBold);
		for (TInt a = 0; a < iAttNames->Count() && i < rows; a++, i++)
			{
			TBuf<130> t;
			t.Format(_L("[%d] "), a + 1);
			t.Append((*iAttNames)[a].Left(120));
			aGc.DrawText(t, TPoint(b.iTl.iX + 4, b.iTl.iY + i * iLineH + iAscent + 1));
			}
		aGc.DiscardFont();
		}
	if (total > rows)
		{
		TInt all = total + iAttNames->Count() + 1;
		TInt h = b.Height();
		TInt th = h * rows / all;
		if (th < 6) th = 6;
		TInt maxTop = all - rows;
		if (maxTop < 1) maxTop = 1;
		TInt ty = b.iTl.iY + (h - th) * iMsgTop / maxTop;
		if (ty + th > b.iBr.iY) ty = b.iBr.iY - th;
		aGc.SetBrushColor(TRgb::Gray4(1));
		aGc.SetPenStyle(CGraphicsContext::ENullPen);
		aGc.DrawRect(TRect(b.iBr.iX - 3, ty, b.iBr.iX, ty + th));
		aGc.SetPenStyle(CGraphicsContext::ESolidPen);
		}
	}

// ============================================================================
// Keys and pen
// ============================================================================

void CPmView::EnsureVisible()
	{
	TInt rows = Rows();
	TInt count = iMode == EFolders ? iFolders->Count() : iRows->Count();
	if (iSel >= count) iSel = count - 1;
	if (iSel < 0) iSel = 0;
	if (iSel < iTop) iTop = iSel;
	if (iSel >= iTop + rows) iTop = iSel - rows + 1;
	if (iTop > count - rows) iTop = count - rows;
	if (iTop < 0) iTop = 0;
	}

void CPmView::MoveSel(TInt aDelta)
	{
	if (iMode == EMessage)
		{
		TInt all = iLineStart->Count() + iAttNames->Count() + 1;
		TInt maxTop = all - Rows();
		if (maxTop < 0) maxTop = 0;
		iMsgTop += aDelta;
		if (iMsgTop > maxTop) iMsgTop = maxTop;
		if (iMsgTop < 0) iMsgTop = 0;
		}
	else
		{
		iSel += aDelta;
		EnsureVisible();
		}
	Redraw();
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
	TInt page = Rows() - 1;
	if (page < 1) page = 1;
	switch (code)
		{
	case EKeyEscape:
		if (Busy())
			{
			iShared->net.quit = 1;         // stop what the engine is doing
			iEikonEnv->InfoMsg(_L("Stopping..."));
			}
		else
			BackL();
		return EKeyWasConsumed;
	case EKeyUpArrow:
		MoveSel(-1);
		return EKeyWasConsumed;
	case EKeyDownArrow:
		MoveSel(1);
		return EKeyWasConsumed;
	case EKeyPageUp:
		MoveSel(-page);
		return EKeyWasConsumed;
	case EKeyPageDown:
	case ' ':
		if (iMode == EMessage || code == EKeyPageDown)
			{
			MoveSel(page);
			return EKeyWasConsumed;
			}
		break;
	case EKeyHome:
		MoveSel(-100000);
		return EKeyWasConsumed;
	case EKeyEnd:
		MoveSel(100000);
		return EKeyWasConsumed;
	case EKeyEnter:
		if (iMode != EMessage)
			OpenCurrentL();
		return EKeyWasConsumed;
	case EKeyLeftArrow:
		if (iMode == EMessage)
			StepMessageL(-1);
		else if (iMode == EList || iMode == EOutbox)
			BackL();
		return EKeyWasConsumed;
	case EKeyRightArrow:
		if (iMode == EMessage)
			StepMessageL(1);
		else
			OpenCurrentL();
		return EKeyWasConsumed;
	case EKeyDelete:
	case EKeyBackspace:
		if (iMode == EList || iMode == EOutbox || (iMode == EMessage && code == EKeyDelete))
			DeleteCurrentL();
		return EKeyWasConsumed;
	default:
		break;
		}
	return EKeyWasNotConsumed;
	}

void CPmView::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	TPoint p = aEvent.iPosition;
	AddEntropy(p.iX * 1000 + p.iY);
	if (aEvent.iType != TPointerEvent::EButton1Down)
		return;
	TRect b = BodyRect();
	if (!b.Contains(p))
		return;
	if (iMode == EMessage)
		{
		// top half: back a page, bottom half: on a page
		TInt page = Rows() - 1;
		MoveSel(p.iY < b.Center().iY ? -page : page);
		return;
		}
	TInt i = iTop + (p.iY - b.iTl.iY) / iLineH;
	TInt count = iMode == EFolders ? iFolders->Count() : iRows->Count();
	if (i < 0 || i >= count)
		return;
	if (i == iSel)
		OpenCurrentL();
	else
		{
		iSel = i;
		Redraw();
		}
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

void CPmComposeDialog::PreLayoutDynInitL()
	{
	SetTitleL(iTitle);
	SetEdwinTextL(EPmDlgTo, &iDraft.iTo);
	SetEdwinTextL(EPmDlgCc, &iDraft.iCc);
	SetEdwinTextL(EPmDlgSubject, &iDraft.iSubject);
	// the edwin wants its own paragraph ends (0x06), not '\n'
	HBufC* b = iDraft.iBody->AllocLC();
	TPtr p = b->Des();
	for (TInt i = 0; i < p.Length(); i++)
		if (p[i] == '\n') p[i] = CEditableText::EParagraphDelimiter;
	SetEdwinTextL(EPmDlgBody, b);
	CleanupStack::PopAndDestroy();
	ShowAttachments();
	}

void CPmComposeDialog::PostLayoutDynInitL()
	{
	// replies start in the text; new messages at To
	if (iDraft.iTo.Length())
		TryChangeFocusToL(EPmDlgBody);
	}

void CPmComposeDialog::ShowAttachments()
	{
	TBuf<120> t;
	TInt n = iDraft.iAttach->Count();
	if (n == 0)
		t = _L("none (Ctrl+A to add)");
	else
		{
		for (TInt i = 0; i < n && t.Length() < 100; i++)
			{
			TParsePtrC parse((*iDraft.iAttach)[i]);
			if (i) t.Append(_L(", "));
			t.Append(parse.NameAndExt().Left(40));
			}
		}
	TRAPD(err, SetLabelL(EPmDlgAttachments, t));
	}

void CPmComposeDialog::Collect()
	{
	GetEdwinText(iDraft.iTo, EPmDlgTo);
	GetEdwinText(iDraft.iCc, EPmDlgCc);
	GetEdwinText(iDraft.iSubject, EPmDlgSubject);
	HBufC* body = NULL;
	TRAPD(err, body = ((CEikEdwin*)Control(EPmDlgBody))->GetTextInHBufL());
	if (err == KErrNone)
		{
		delete iDraft.iBody;
		iDraft.iBody = body ? body : HBufC::New(1);
		if (iDraft.iBody)
			{
			TPtr p = iDraft.iBody->Des();
			for (TInt i = 0; i < p.Length(); i++)
				if (p[i] == CEditableText::EParagraphDelimiter || p[i] == CEditableText::ELineBreak) p[i] = '\n';
			}
		}
	}

TBool CPmComposeDialog::OkToExitL(TInt aButtonId)
	{
	Collect();
	if (aButtonId == EPmBidAttach)
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
			ShowAttachments();
			}
		return EFalse;                  // stay in the dialog
		}
	if (aButtonId == EPmBidRemove)
		{
		if (iDraft.iAttach->Count())
			{
			iDraft.iAttach->Delete(iDraft.iAttach->Count() - 1);
			ShowAttachments();
			}
		return EFalse;
		}
	if (aButtonId == EPmBidSend)
		{
		iDraft.iTo.Trim();
		if (iDraft.iTo.Length() == 0 || iDraft.iTo.Locate('@') < 0)
			{
			iEikonEnv->InfoMsg(_L("Who is it to? Enter an address"));
			TryChangeFocusToL(EPmDlgTo);
			return EFalse;
			}
		}
	if (aButtonId == EEikBidCancel)
		{
		if ((iDraft.iBody && iDraft.iBody->Length() > 0) || iDraft.iTo.Length())
			return iEikonEnv->QueryWinL(_L("Discard this message?"), _L("Save keeps it in the outbox"));
		}
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
	SetChoiceListCurrentItem(EPmDlgStore, iStore);
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
		iEikonEnv->InfoMsg(_L("Enter your email address"));
		return EFalse;
		}
	TBuf<64> host;
	GetEdwinText(host, EPmDlgImapHost);
	host.Trim();
	if (host.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("Enter the incoming (IMAP) server"));
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
	if (!iAcct.name[0])
		{
		TInt at = email.Locate('@');
		CopyToC(iAcct.name, sizeof(iAcct.name), email.Mid(at + 1));
		}
	TBuf<32> pw;
	GetSecretEditorText(pw, EPmDlgPass);
	if (pw.Length())                     // blank keeps the saved password
		CopyToC(iAcct.pass, sizeof(iAcct.pass), pw);
	iAcct.sync_count = NumberEditorValue(EPmDlgSyncCount);
	iAcct.max_body_kb = NumberEditorValue(EPmDlgBodyKb);
	iAcct.save_sent = ChoiceListCurrentItem(EPmDlgSaveSent);
	iStore = ChoiceListCurrentItem(EPmDlgStore);
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
	}

TBool CPmConnDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iSettings.iNetMode = ChoiceListCurrentItem(EPmDlgLink) == 1;
	iSettings.iBaudIndex = ChoiceListCurrentItem(EPmDlgBaud);
	iSettings.iRtsCts = ChoiceListCurrentItem(EPmDlgFlow) == 1;
	return ETrue;
	}

// ============================================================================
// App UI
// ============================================================================

void CPmAppUi::ConstructL()
	{
	BaseConstructL();
	LoadSettings();
	iView = new(ELeave) CPmView;
	iView->ConstructL(ClientRect(), iSettings);
	AddToStackL(iView);
	if (!iSettings.iAccounts[iSettings.iAcct].used)
		{
		if (EditAccountL(iSettings.iAcct, ETrue))
			iView->AccountChangedL();
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
		return;
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

TBool CPmAppUi::EditAccountL(TInt aIndex, TBool aNew)
	{
	PmAccount a = iSettings.iAccounts[aIndex];
	if (aNew)
		{
		// Fastmail's settings to start with
		Mem::FillZ(&a, sizeof(a));
		const char* imap = "imap.fastmail.com";
		const char* smtp = "smtp.fastmail.com";
		Mem::Copy(a.imap_host, imap, 18);
		Mem::Copy(a.smtp_host, smtp, 18);
		Mem::Copy(a.name, "Fastmail", 9);
		a.imap_port = 993;
		a.imap_tls = PM_TLS_ON;
		a.smtp_port = 465;
		a.smtp_tls = PM_TLS_ON;
		a.sync_count = 50;
		a.max_body_kb = 64;
		a.save_sent = 1;
		}
	TInt store = iSettings.iStore;
	CPmAccountDialog* dlg = new(ELeave) CPmAccountDialog(a, store);
	if (!dlg->ExecuteLD(R_PM_ACCOUNT_DIALOG))
		return EFalse;
	iSettings.iAccounts[aIndex] = a;
	iSettings.iStore = store;
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
	CleanupStack::PushL(aDraft);
	CPmComposeDialog* dlg = new(ELeave) CPmComposeDialog(*aDraft, aTitle);
	TInt r = dlg->ExecuteLD(R_PM_COMPOSE_DIALOG);
	if (r == EPmBidSend || r == EPmBidSave)
		iView->SaveDraftL(*aDraft, r == EPmBidSend);
	CleanupStack::PopAndDestroy();
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

void CPmAppUi::ReplyL(TBool aAll)
	{
	const TPmRow* row = iView->CurrentRow();
	if (!row || (iView->Mode() != CPmView::EList && iView->Mode() != CPmView::EMessage))
		{
		iEikonEnv->InfoMsg(_L("Choose a message first"));
		return;
		}
	if (iView->Mode() == CPmView::EList)
		{
		// open it (the text is needed for the reply)
		iView->OpenCurrentL();
		}
	CPmDraft* d = CPmDraft::NewL();
	CleanupStack::PushL(d);
	TBuf<500> from, replyTo, to, cc, subject, msgid, date;
	iView->MessageHeader(_L("From"), from);
	iView->MessageHeader(_L("Reply-To"), replyTo);
	iView->MessageHeader(_L("To"), to);
	iView->MessageHeader(_L("Cc"), cc);
	iView->MessageHeader(_L("Subject"), subject);
	iView->MessageHeader(_L("Message-ID"), msgid);
	iView->MessageHeader(_L("Date"), date);
	if (!from.Length())
		{
		iEikonEnv->InfoMsg(_L("Wait for the message to download"));
		CleanupStack::PopAndDestroy();
		return;
		}
	SafeCopy(d->iTo, replyTo.Length() ? replyTo : from);
	if (aAll)
		{
		// everyone else, not me
		TBuf<100> me;
		FromC(me, iSettings.iAccounts[iSettings.iAcct].email);
		TBuf<500> all(to);
		if (cc.Length()) { if (all.Length()) all.Append(_L(", ")); all.Append(cc.Left(all.MaxLength() - all.Length() - 2)); }
		TPtrC rest = all;
		while (rest.Length())
			{
			TInt comma = rest.Locate(',');
			TPtrC item = comma >= 0 ? rest.Left(comma) : rest;
			rest.Set(comma >= 0 ? rest.Mid(comma + 1) : TPtrC());
			TBuf<200> addr;
			AddressOnly(addr, item);
			if (addr.Length() == 0 || addr.CompareF(me) == 0)
				continue;
			TBuf<200> fromAddr;
			AddressOnly(fromAddr, d->iTo);
			if (addr.CompareF(fromAddr) == 0)
				continue;
			TBuf<200> clean(item);
			clean.Trim();
			if (d->iCc.Length() + clean.Length() + 2 < d->iCc.MaxLength())
				{
				if (d->iCc.Length()) d->iCc.Append(_L(", "));
				d->iCc.Append(clean);
				}
			}
		}
	if (subject.Left(3).CompareF(_L("Re:")) != 0)
		d->iSubject = _L("Re: ");
	d->iSubject.Append(subject.Left(d->iSubject.MaxLength() - d->iSubject.Length()));
	SafeCopy(d->iInReplyTo, msgid);
	SafeCopy(d->iReferences, msgid);
	SafeCopy(d->iReplyFolder, iView->FolderImap());
	d->iReplyUid = row->iUid;
	HBufC* body = HBufC::NewL(24 * 1024);
	TPtr p = body->Des();
	p.Append('\n');
	AddSignature(*d, p);
	TBuf<120> who;
	DisplayName(who, from);
	p.Append('\n');
	p.Append(_L("On "));
	p.Append(date.Left(40));
	p.Append(_L(", "));
	p.Append(who.Left(60));
	p.Append(_L(" wrote:\n"));
	iView->QuoteBodyL(p, 300);
	delete d->iBody;
	d->iBody = body;
	CleanupStack::Pop();
	ComposeL(d, aAll ? _L("Reply to all") : _L("Reply"));
	}

void CPmAppUi::ForwardL()
	{
	if (iView->Mode() == CPmView::EList && iView->CurrentRow())
		iView->OpenCurrentL();
	if (iView->Mode() != CPmView::EMessage)
		{
		iEikonEnv->InfoMsg(_L("Choose a message first"));
		return;
		}
	CPmDraft* d = CPmDraft::NewL();
	CleanupStack::PushL(d);
	TBuf<500> from, to, subject, date;
	iView->MessageHeader(_L("From"), from);
	iView->MessageHeader(_L("To"), to);
	iView->MessageHeader(_L("Subject"), subject);
	iView->MessageHeader(_L("Date"), date);
	if (!from.Length())
		{
		iEikonEnv->InfoMsg(_L("Wait for the message to download"));
		CleanupStack::PopAndDestroy();
		return;
		}
	d->iSubject = _L("Fwd: ");
	d->iSubject.Append(subject.Left(190));
	HBufC* body = HBufC::NewL(32 * 1024);
	TPtr p = body->Des();
	p.Append('\n');
	AddSignature(*d, p);
	p.Append(_L("\n---------- Forwarded message ----------\nFrom: "));
	p.Append(from.Left(200));
	p.Append(_L("\nDate: "));
	p.Append(date.Left(60));
	p.Append(_L("\nSubject: "));
	p.Append(subject.Left(200));
	p.Append(_L("\nTo: "));
	p.Append(to.Left(200));
	p.Append(_L("\n\n"));
	// the text without "> "
	HBufC* q = HBufC::NewLC(30 * 1024);
	TPtr qp = q->Des();
	iView->QuoteBodyL(qp, 600);
	TPtrC rest = qp;
	while (rest.Length())
		{
		TInt nl = rest.Locate('\n');
		TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
		if (line.Left(2) == _L("> ")) line.Set(line.Mid(2));
		else if (line.Left(1) == _L(">")) line.Set(line.Mid(1));
		if (p.Length() + line.Length() + 2 > p.MaxLength()) break;
		p.Append(line);
		p.Append('\n');
		}
	CleanupStack::PopAndDestroy();
	if (iView->AttachmentCount())
		p.Append(_L("\n(The attachments are not forwarded: save them first and attach them.)\n"));
	delete d->iBody;
	d->iBody = body;
	CleanupStack::Pop();
	ComposeL(d, _L("Forward"));
	}

void CPmAppUi::MoveL()
	{
	if (!iView->CurrentRow() || (iView->Mode() != CPmView::EList && iView->Mode() != CPmView::EMessage))
		{
		iEikonEnv->InfoMsg(_L("Choose a message first"));
		return;
		}
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(8);
	CleanupStack::PushL(names);
	RArray<TInt> map;
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
		map.Close();
		CleanupStack::PopAndDestroy();
		iEikonEnv->InfoMsg(_L("No folders yet - Send & receive first"));
		return;
		}
	TInt choice = 0;
	CleanupStack::Pop();
	CPmChoiceDialog* dlg = new(ELeave) CPmChoiceDialog(_L("Move to folder"), _L("Folder"), names, choice);
	if (dlg->ExecuteLD(R_PM_CHOICE_DIALOG) && choice >= 0 && choice < map.Count())
		{
		TBuf8<128> dest(iView->FolderAt(map[choice]).iImap);
		iView->MoveCurrentL(dest);
		}
	map.Close();
	}

void CPmAppUi::SaveAttachmentL()
	{
	if (iView->AttachmentCount() == 0)
		{
		iEikonEnv->InfoMsg(_L("This message has no attachments"));
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
		n.Append(e.Left(80));
		if (i == iSettings.iAcct) current = names->Count();
		names->AppendL(n);
		User::LeaveIfError(map.Append(i));
		}
	if (names->Count() < 2)
		{
		map.Close();
		CleanupStack::PopAndDestroy();
		iEikonEnv->InfoMsg(_L("There is only one account (Tools > New account)"));
		return;
		}
	CleanupStack::Pop();
	TInt choice = current;
	CPmChoiceDialog* dlg = new(ELeave) CPmChoiceDialog(_L("Switch account"), _L("Account"), names, choice);
	if (dlg->ExecuteLD(R_PM_CHOICE_DIALOG) && choice >= 0 && choice < map.Count())
		{
		iSettings.iAcct = map[choice];
		SaveSettings();
		iView->AccountChangedL();
		}
	map.Close();
	}

void CPmAppUi::DeleteAccountL()
	{
	PmAccount& a = iSettings.iAccounts[iSettings.iAcct];
	if (!a.used)
		return;
	TBuf<60> n;
	FromC(n, a.name);
	if (!iEikonEnv->QueryWinL(_L("Delete this account from PsiMail?"), n))
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
	TBuf<120> lines[6];
	lines[0] = _L("PsiMail ");
	lines[0].Append(KVersion);
	lines[0].Append(_L(" - email for the Psion Series 5mx"));
	lines[1] = _L("IMAP and SMTP over TLS 1.3, made for Fastmail.");
	lines[2] = _L("Networking and TLS from PsiTerm (MIT).");
	lines[3] = _L("Ctrl+G send & receive, Ctrl+N new, Ctrl+R reply.");
	lines[4] = _L("Esc goes back, or stops a download.");
	TMemoryInfoV1Buf mem;
	UserHal::MemoryInfo(mem);
	lines[5].Format(_L("Free memory: %d KB. Engine: %d KB."), mem().iFreeRamInBytes / 1024,
		iView->Shared()->heap_used / 1024);
	CPmInfoDialog* dlg = new(ELeave) CPmInfoDialog(_L("About PsiMail"), lines, 6);
	dlg->ExecuteLD(R_PM_INFO_DIALOG);
	}

void CPmAppUi::DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	CPmView::TMode m = iView->Mode();
	TBool msg = (m == CPmView::EList || m == CPmView::EMessage) && iView->CurrentRow() != NULL;
	if (aMenuId == R_PM_FILE_MENU)
		{
		aMenuPane->SetItemButtonState(EPmCmdOffline, iSettings.iOffline ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemDimmed(EPmCmdHangup, !iView->Shared()->online);
		}
	else if (aMenuId == R_PM_MESSAGE_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdReply, !msg);
		aMenuPane->SetItemDimmed(EPmCmdReplyAll, !msg);
		aMenuPane->SetItemDimmed(EPmCmdForward, !msg);
		aMenuPane->SetItemDimmed(EPmCmdDelete, !msg && !(m == CPmView::EOutbox && iView->CurrentRow()));
		aMenuPane->SetItemDimmed(EPmCmdMove, !msg);
		aMenuPane->SetItemDimmed(EPmCmdArchive, !msg);
		aMenuPane->SetItemDimmed(EPmCmdUnread, !msg);
		aMenuPane->SetItemDimmed(EPmCmdFlag, !msg);
		aMenuPane->SetItemDimmed(EPmCmdSaveAttach, iView->AttachmentCount() == 0);
		aMenuPane->SetItemDimmed(EPmCmdWhole, m != CPmView::EMessage);
		aMenuPane->SetItemDimmed(EPmCmdShowHeaders, m != CPmView::EMessage);
		aMenuPane->SetItemDimmed(EPmCmdNew, m == CPmView::ENoAccount);
		}
	else if (aMenuId == R_PM_FOLDER_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdOlder, m != CPmView::EList || iView->CurrentIsSearch());
		}
	}

void CPmAppUi::HandleCommandL(TInt aCommand)
	{
	CPmView::TMode m = iView->Mode();
	if (m == CPmView::ENoAccount && aCommand != EEikCmdExit && aCommand != EPmCmdNewAccount &&
		aCommand != EPmCmdConnSettings && aCommand != EPmCmdAbout)
		{
		iEikonEnv->InfoMsg(_L("Set up an account first: Tools > New account"));
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
		iEikonEnv->InfoMsg(iSettings.iOffline ? _L("Working offline: changes wait until you go online")
			: _L("Online: PsiMail will connect when it needs to"));
		break;
	case EPmCmdHangup:
		iView->Cmd(PM_CMD_HANGUP, KNullDesC8, 0, KNullDesC8);
		iEikonEnv->InfoMsg(_L("Hanging up - the serial port will be free"));
		break;
	case EPmCmdAbout:
		AboutL();
		break;
	case EPmCmdNew:
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
		iView->DeleteCurrentL();
		break;
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
			iEikonEnv->InfoMsg(_L("There is no Archive folder"));
			break;
			}
		iView->MoveCurrentL(dest);
		iEikonEnv->InfoMsg(_L("Archived"));
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
	case EPmCmdShowHeaders:
		iView->ToggleHeaders();
		break;
	case EPmCmdFolders:
		iView->ShowFoldersL();
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
			iView->DraftFromOutboxL(*d);
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
			iEikonEnv->InfoMsg(_L("PsiMail has room for 4 accounts"));
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
		CPmConnDialog* dlg = new(ELeave) CPmConnDialog(iSettings);
		if (dlg->ExecuteLD(R_PM_CONN_DIALOG))
			{
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
