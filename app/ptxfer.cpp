// PTXFER.CPP - PsiTerm's file transfer and session log (0.74)
//
// File > Send file...   pick a Psion file (the standard Open file dialog),
//                       then the server folder it goes to, then it is sent
// File > Get file...    pick a file in the server's folders, then where it
//                       goes on the Psion (the standard Save as dialog)
// File > Log to file... everything received is written to a file
//
// The transfers are done by psissh.exe (ssh/sftp.c) over SFTP, on the SSH
// connection that is already logged in: this file posts one request at a
// time in the shared memory (psishared.h, xfer_*) and shows the progress
// window (with Stop) until psissh says it has finished.

#include "psiterm.h"
#include "ptxfer.h"

#include <eikcfdlg.h>
#include <eiklabel.h>
#include <eikclb.h>
#include <eikclbd.h>
#include <eikchlst.h>
#include <eikedwin.h>
#include <eikon.rsg>

#ifndef TRAP_IGNORE
#define TRAP_IGNORE(s) { TInt _ignored; TRAP(_ignored, s); }
#endif

_LIT(KDefaultFolder, "C:\\Documents\\");
_LIT(KLogIni, "C:\\System\\Apps\\PsiTerm\\Log.ini");
_LIT(KDefaultLog, "C:\\Documents\\PsiTerm log.txt");

static CPtXferMemory& Mem(CTermView& aView)
	{
	if (!aView.iXferMem)
		aView.iXferMem = new(ELeave) CPtXferMemory;
	return *aView.iXferMem;
	}

// ============================================================================
// Text helpers: the server's names are UTF-8, the Psion's are its code page
// ============================================================================

static void Utf8ToText(const TDesC8& aIn, TDes& aOut, TBool aFileName)
	{
	aOut.Zero();
	TInt i = 0;
	while (i < aIn.Length() && aOut.Length() < aOut.MaxLength())
		{
		TUint c = aIn[i++];
		if (c >= 0xc0 && c < 0xf8)
			{
			TInt more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
			TUint cp = c & (0x3f >> more);
			TInt k = 0;
			while (k < more && i < aIn.Length() && (aIn[i] & 0xc0) == 0x80)
				{
				cp = (cp << 6) | (aIn[i++] & 0x3f);
				k++;
				}
			TInt m = (k == more) ? PsiMapToCodePage(cp) : -1;
			c = m >= 0 ? (TUint)m : '?';
			}
		else if (c >= 0x80)
			c = '?';
		if (aFileName && (c < 32 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?'
			|| c == '"' || c == '<' || c == '>' || c == '|'))
			c = '_';
		aOut.Append((TChar)c);
		}
	}

static void AppendUtf8(TDes8& aOut, TUint aCh)
	{
	if (aCh < 0x80)
		{
		if (aOut.Length() < aOut.MaxLength()) aOut.Append((TUint8)aCh);
		}
	else if (aCh < 0x800)
		{
		if (aOut.Length() + 2 > aOut.MaxLength()) return;
		aOut.Append((TUint8)(0xc0 | (aCh >> 6)));
		aOut.Append((TUint8)(0x80 | (aCh & 0x3f)));
		}
	else
		{
		if (aOut.Length() + 3 > aOut.MaxLength()) return;
		aOut.Append((TUint8)(0xe0 | (aCh >> 12)));
		aOut.Append((TUint8)(0x80 | ((aCh >> 6) & 0x3f)));
		aOut.Append((TUint8)(0x80 | (aCh & 0x3f)));
		}
	}

static void TextToUtf8(const TDesC& aIn, TDes8& aOut)
	{
	for (TInt i = 0; i < aIn.Length(); i++)
		AppendUtf8(aOut, PsiCodePageToUnicode(aIn[i] & 0xff));
	}

// "512 bytes", "1 byte", "12 KB", "3 MB"
static void SizeText(TUint aSize, TDes& aOut)
	{
	if (aSize == 1)
		aOut.Copy(_L("1 byte"));
	else if (aSize < 1024)
		aOut.Format(_L("%d bytes"), aSize);
	else if (aSize < 10 * 1024 * 1024)
		aOut.Format(_L("%d KB"), (aSize + 1023) / 1024);
	else
		aOut.Format(_L("%d MB"), (aSize + 512 * 1024) / (1024 * 1024));
	}

// the end of a long text, "...<end>", in at most aMax characters
static void Tail(const TDesC& aIn, TDes& aOut, TInt aMax)
	{
	if (aMax > aOut.MaxLength()) aMax = aOut.MaxLength();
	if (aIn.Length() <= aMax)
		aOut.Copy(aIn);
	else
		{
		aOut.Copy(_L("..."));
		aOut.Append(aIn.Right(aMax - 3));
		}
	}

// the server's folder above aDir ("/a/b" -> "/a", "/a" -> "/")
static void ParentDir(TDes8& aDir)
	{
	TInt slash = aDir.LocateReverse('/');
	if (slash <= 0)
		aDir.Copy(_L8("/"));
	else
		aDir.SetLength(slash);
	}

// "<dir>/<name>" in aOut. EFalse (and an infoprint) when it does not fit:
// a cut name would send the file under another name, or stat one that is
// not there
static TBool JoinDir(const TDesC8& aDir, const TDesC8& aName, TDes8& aOut)
	{
	TInt slash = (aDir.Length() == 0 || aDir[aDir.Length() - 1] != '/') ? 1 : 0;
	if (aDir.Length() + slash + aName.Length() > aOut.MaxLength())
		{
		CEikonEnv::Static()->InfoMsg(_L("Not available - the name is too long"));
		return EFalse;
		}
	aOut.Copy(aDir);
	if (slash)
		aOut.Append('/');
	aOut.Append(aName);
	return ETrue;
	}

// ============================================================================
// Running one request, with the progress window
// ============================================================================

class CPtXferDialog : public CEikDialog
	{
public:
	CPtXferDialog(CTermView& aView, TUint aReq, const TDesC& aTitle, const TDesC& aWhat, TInt& aResult)
		: iView(aView), iReq(aReq), iTitle(aTitle), iWhat(aWhat), iResult(aResult) {}
	~CPtXferDialog();
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	static TInt TickCallback(TAny* aSelf);
	void Tick();
	void ShowProgressL(TBool aForce);
	CTermView& iView;
	TUint iReq;
	TBuf<40> iTitle;
	TBuf<80> iWhat;
	TInt& iResult;
	CPeriodic* iTimer;
	TBool iStopping;
	TUint iShownDone;
	TUint iShownTotal;
	};

CPtXferDialog::~CPtXferDialog()
	{
	if (iTimer)
		iTimer->Cancel();
	delete iTimer;
	}

void CPtXferDialog::PreLayoutDynInitL()
	{
	SetTitleL(iTitle);
	// (room for the longest lines before the window is laid out)
	SetLabelReserveLengthL(EPtDlgXferWhat, 40);
	SetLabelReserveLengthL(EPtDlgXferHow, 32);
	SetLabelL(EPtDlgXferWhat, iWhat);
	ShowProgressL(ETrue);
	iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
	iTimer->Start(250000, 250000, TCallBack(TickCallback, this));
	}

TInt CPtXferDialog::TickCallback(TAny* aSelf)
	{
	((CPtXferDialog*)aSelf)->Tick();
	return 0;
	}

void CPtXferDialog::Tick()
	{
	PsiShared* s = iView.XferShared();
	if (!s || s->xfer_ack == iReq)
		{
		// finished (or the connection has gone): close the window. Nothing
		// here may touch the dialog after TryExitL, which deletes it.
		iResult = s ? s->xfer_result : PSI_XFER_LINK;
		iTimer->Cancel();
		TRAP_IGNORE(TryExitL(EPtBidXferDone));
		return;
		}
	TRAP_IGNORE(ShowProgressL(EFalse));
	}

void CPtXferDialog::ShowProgressL(TBool aForce)
	{
	PsiShared* s = iView.XferShared();
	if (!s)
		return;
	TUint done = s->xfer_done, total = s->xfer_total;
	if (!aForce && done == iShownDone && total == iShownTotal)
		return;
	iShownDone = done;
	iShownTotal = total;
	TBuf<64> line;
	if (iStopping)
		line.Copy(_L("Stopping..."));
	else if (total > 0)
		{
		TBuf<20> a, b;
		SizeText(done, a);
		SizeText(total, b);
		TInt pc = total >= 0x1000000 ? (TInt)(done / (total / 100)) : (TInt)((done * 100) / total);
		if (pc > 100) pc = 100;
		line.Format(_L("%S of %S - %d%%"), &a, &b, pc);
		}
	else if (done > 0)
		{
		SizeText(done, line);
		}
	else
		line.Copy(_L("Waiting for the server..."));
	SetLabelL(EPtDlgXferHow, line);
	Control(EPtDlgXferHow)->DrawNow();
	}

TBool CPtXferDialog::OkToExitL(TInt aButtonId)
	{
	if (aButtonId == EPtBidXferDone)
		return ETrue;
	if (aButtonId != EEikBidCancel)
		return EFalse;                   // (Enter does not stop a transfer)
	// Stop (or Esc): ask psissh to stop, and wait for it to say it has
	PsiShared* s = iView.XferShared();
	if (s)
		s->xfer_cancel = 1;
	iView.RingSsh();
	iStopping = ETrue;
	ShowProgressL(ETrue);
	return EFalse;
	}

// Posts one request and waits for it, with the progress window if it takes
// more than a moment. Returns PSI_XFER_*.
static TInt RunXferL(CTermView& aView, TInt aOp, const TDesC8& aRemote, const TDesC& aLocal,
	const TDesC& aTitle, const TDesC& aWhat)
	{
	PsiShared* s = aView.XferShared();
	if (!s)
		return PSI_XFER_LINK;
	if (s->xfer_req != s->xfer_ack)
		return PSI_XFER_LINK;            // (one at a time: cannot happen, the window is modal)
	s->xfer_op = aOp;
	TPtr8 local((TUint8*)s->xfer_local, sizeof(s->xfer_local) - 1);
	local.Copy(aLocal.Left(aLocal.Length() < local.MaxLength() ? aLocal.Length() : local.MaxLength()));
	local.ZeroTerminate();
	TPtr8 remote((TUint8*)s->xfer_remote, sizeof(s->xfer_remote) - 1);
	remote.Copy(aRemote.Left(aRemote.Length() < remote.MaxLength() ? aRemote.Length() : remote.MaxLength()));
	remote.ZeroTerminate();
	s->xfer_cancel = 0;
	s->xfer_done = 0;
	s->xfer_total = 0;
	TUint req = s->xfer_req + 1;
	s->xfer_req = req;                   // last: psissh starts now
	aView.RingSsh();
	// a quick request (a folder list on a fast link) needs no window
	for (TInt i = 0; i < 8; i++)
		{
		User::After(50000);
		s = aView.XferShared();
		if (!s)
			return PSI_XFER_LINK;
		if (s->xfer_ack == req)
			return s->xfer_result;
		}
	TInt result = PSI_XFER_LINK;
	CPtXferDialog* dlg = new(ELeave) CPtXferDialog(aView, req, aTitle, aWhat, result);
	dlg->ExecuteLD(R_PT_XFER_DIALOG);
	return result;
	}

// The outcome of a failed request, as an infoprint
static void XferFailed(TInt aResult, CTermView& aView)
	{
	CEikonEnv* env = CEikonEnv::Static();
	PsiShared* s = aView.XferShared();
	switch (aResult)
		{
	case PSI_XFER_CANCELLED:
		env->InfoMsg(_L("Transfer stopped"));
		break;
	case PSI_XFER_NO_SFTP:
		env->InfoMsg(_L("This server does not offer file transfer (SFTP)"));
		break;
	case PSI_XFER_DENIED:
		env->InfoMsg(_L("Not allowed on the server - permission denied"));
		break;
	case PSI_XFER_NOT_FOUND:
		env->InfoMsg(_L("File or folder not found on the server"));
		break;
	case PSI_XFER_LOCAL_WRITE:
		env->InfoMsg(_L("Could not save the file - the disk is full or was removed"));
		break;
	case PSI_XFER_LOCAL_READ:
		env->InfoMsg(_L("Could not read the file - the disk may have been removed"));
		break;
	case PSI_XFER_TIMEOUT:
		env->InfoMsg(_L("Transfer stopped - the server stopped answering"));
		break;
	case PSI_XFER_FAILED:
		{
		TBuf<120> m(_L("The server refused - "));
		TBuf<96> why;
		if (s)
			{
			TPtrC8 w((const TUint8*)s->xfer_msg);
			Utf8ToText(w.Left(w.Length() < 60 ? w.Length() : 60), why, EFalse);
			}
		if (why.Length())
			m.Append(why);
		else
			m.Append(_L("no reason given"));
		env->InfoMsg(m);
		break;
		}
	default:
		env->InfoMsg(_L("Transfer stopped - the SSH connection has gone"));
		break;
		}
	}

static TBool NeedLoginL(CTermView& aView)
	{
	if (aView.XferShared())
		return EFalse;
	CEikonEnv::Static()->InfoMsg(_L("Not available - SSH is not connected"));
	return ETrue;
	}

// ============================================================================
// The server's folders
// ============================================================================

struct TRemoteEntry
	{
	TInt iType;            // 'd' folder, 'f' file, 'l' other (a link: folder or file)
	TUint iSize;
	TInt iOff;             // the name, in the list text
	TInt iLen;
	};

class CRemoteList : public CBase
	{
public:
	~CRemoteList() { delete iText; delete iEntries; }
	void SetL(const TDesC8& aText);
	TInt Count() const { return iEntries ? iEntries->Count() : 0; }
	const TRemoteEntry& At(TInt aIndex) const { return (*iEntries)[aIndex]; }
	TPtrC8 Name(TInt aIndex) const { return iText->Mid(At(aIndex).iOff, At(aIndex).iLen); }
private:
	TInt Compare(const TRemoteEntry& aA, const TRemoteEntry& aB) const;
	void Sort();
	void SiftDown(TInt aRoot, TInt aEnd);
	HBufC8* iText;
	CArrayFixFlat<TRemoteEntry>* iEntries;
	};

// folders first, then names without a leading dot, then A-Z ignoring case
TInt CRemoteList::Compare(const TRemoteEntry& aA, const TRemoteEntry& aB) const
	{
	TInt da = aA.iType == 'd', db = aB.iType == 'd';
	if (da != db) return db - da;
	TInt ha = (*iText)[aA.iOff] == '.', hb = (*iText)[aB.iOff] == '.';
	if (ha != hb) return ha - hb;
	return iText->Mid(aA.iOff, aA.iLen).CompareF(iText->Mid(aB.iOff, aB.iLen));
	}

void CRemoteList::SetL(const TDesC8& aText)
	{
	delete iText;
	iText = NULL;
	delete iEntries;
	iEntries = NULL;
	iText = aText.AllocL();
	iEntries = new(ELeave) CArrayFixFlat<TRemoteEntry>(32);
	TInt pos = 0;
	while (pos < iText->Length())
		{
		TPtrC8 rest(iText->Mid(pos));
		TInt nl = rest.Locate('\n');
		if (nl < 0) break;
		TPtrC8 line(rest.Left(nl));
		TInt tab = line.Locate('\t');
		if (tab >= 1 && tab < line.Length() - 1)
			{
			TRemoteEntry e;
			e.iType = line[0];
			e.iSize = 0;
			for (TInt i = 1; i < tab; i++)
				e.iSize = e.iSize * 10 + (line[i] - '0');
			e.iOff = pos + tab + 1;
			e.iLen = line.Length() - tab - 1;
			iEntries->AppendL(e);
			}
		pos += nl + 1;
		}
	Sort();
	}

// Sorted once, in place, in n log n compares: inserting each entry in order
// as it came was n squared, which on a /usr/bin-sized folder froze the UI
// for seconds at 36 MHz. (A heap sort: no extra memory, no recursion.)
void CRemoteList::Sort()
	{
	TInt n = iEntries->Count();
	for (TInt start = n / 2 - 1; start >= 0; start--)
		SiftDown(start, n);
	for (TInt end = n - 1; end > 0; end--)
		{
		TRemoteEntry t = (*iEntries)[0];
		(*iEntries)[0] = (*iEntries)[end];
		(*iEntries)[end] = t;
		SiftDown(0, end);
		}
	}

void CRemoteList::SiftDown(TInt aRoot, TInt aEnd)
	{
	for (;;)
		{
		TInt child = 2 * aRoot + 1;
		if (child >= aEnd)
			return;
		if (child + 1 < aEnd && Compare((*iEntries)[child], (*iEntries)[child + 1]) < 0)
			child++;
		if (Compare((*iEntries)[aRoot], (*iEntries)[child]) >= 0)
			return;
		TRemoteEntry t = (*iEntries)[aRoot];
		(*iEntries)[aRoot] = (*iEntries)[child];
		(*iEntries)[child] = t;
		aRoot = child;
		}
	}

enum { EActCancel, EActUp, EActOpen, EActGet, EActSendHere };

class CPtRemoteDialog : public CEikDialog, public MEikListBoxObserver
	{
public:
	CPtRemoteDialog(TBool aSend, const TDesC8& aDir, const CRemoteList& aList, TBool aMore,
		TInt& aAction, TInt& aIndex)
		: iSend(aSend), iDir(aDir), iList(aList), iMore(aMore), iAction(aAction), iIndex(aIndex) {}
private:
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void HandleListBoxEventL(CEikListBox* aListBox, TListBoxEvent aEventType);
	void SetSizeAndPositionL(const TSize& aSize);
	TBool AtTop() const { return iDir.Length() <= 1; }
	TInt Row() const;              // -1 = the ".." row, -2 = none, else the entry
	TBool iSend;
	const TDesC8& iDir;
	const CRemoteList& iList;
	TBool iMore;
	TInt& iAction;
	TInt& iIndex;
	};

void CPtRemoteDialog::SetSizeAndPositionL(const TSize& aSize)
	{
	TSize screen = iEikonEnv->ScreenDevice()->SizeInPixels();
	TSize size(aSize.iWidth < screen.iWidth - 8 ? aSize.iWidth : screen.iWidth - 8,
		aSize.iHeight < screen.iHeight - 8 ? aSize.iHeight : screen.iHeight - 8);
	SetCornerAndSizeL(EHCenterVCenter, size);
	}

void CPtRemoteDialog::PreLayoutDynInitL()
	{
	TBuf<44> shown;
	{
	HBufC* dir = HBufC::NewLC(512);         // (on the heap: the stack is small)
	TPtr d = dir->Des();
	Utf8ToText(iDir, d, EFalse);
	Tail(d, shown, 40);
	CleanupStack::PopAndDestroy();          // dir
	}
	if (iMore && shown.Length() < 30)
		shown.Append(_L(" (first part)"));
	SetLabelReserveLengthL(EPtDlgRemoteDir, 40);
	SetLabelL(EPtDlgRemoteDir, shown);

	CEikColumnListBox* list = (CEikColumnListBox*)Control(EPtDlgRemoteList);
	CDesCArrayFlat* rows = new(ELeave) CDesCArrayFlat(32);
	list->Model()->SetItemTextArray(rows);
	list->Model()->SetOwnershipType(ELbmOwnsItemArray);
	CColumnListBoxData* cd = list->Model()->ColumnData();
	cd->SetColumnWidthPixelL(0, 250);
	cd->SetColumnWidthPixelL(1, 90);
	cd->SetColumnFontL(0, iEikonEnv->NormalFont());
	cd->SetColumnFontL(1, iEikonEnv->NormalFont());
	cd->SetColumnAlignmentL(1, CGraphicsContext::ERight);
	if (!AtTop())
		rows->AppendL(_L("..\tUp a folder"));
	TBuf<160> row;
	TBuf<120> name;
	TBuf<20> size;
	for (TInt i = 0; i < iList.Count(); i++)
		{
		const TRemoteEntry& e = iList.At(i);
		Utf8ToText(iList.Name(i), name, EFalse);
		// the row shows at most 100 characters of the name (and a folder's
		// "/" must have room: Append on a full TBuf panics USER 11 - a server
		// name can be 255 bytes)
		if (name.Length() > 100)
			name.SetLength(100);
		if (e.iType == 'd')
			{
			if (name.Length() >= 100)
				name.SetLength(99);
			name.Append('/');
			size.Copy(_L("Folder"));
			}
		else if (e.iType == 'l')
			size.Copy(_L("Link"));
		else
			SizeText(e.iSize, size);
		row.Copy(name);
		row.Append('\t');
		row.Append(size);
		rows->AppendL(row);
		}
	if (!list->ScrollBarFrame())
		list->CreateScrollBarFrameL();
	list->ScrollBarFrame()->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);
	list->HandleItemAdditionL();
	if (rows->Count())
		list->SetCurrentItemIndex(0);
	list->SetListBoxObserver(this);
	}

void CPtRemoteDialog::PostLayoutDynInitL()
	{
	// the first entry, not "..". (Only now: before the layout the list has
	// no height, and a current row past the first panicked EIKON-LISTBOX 4
	// in a short list.)
	CEikColumnListBox* list = (CEikColumnListBox*)Control(EPtDlgRemoteList);
	if (!AtTop() && list->Model()->NumberOfItems() > 1)
		list->SetCurrentItemIndex(1);
	TryChangeFocusToL(EPtDlgRemoteList);
	}

TInt CPtRemoteDialog::Row() const
	{
	CEikColumnListBox* list = (CEikColumnListBox*)Control(EPtDlgRemoteList);
	if (list->Model()->NumberOfItems() == 0)
		return -2;
	TInt cur = list->CurrentItemIndex();
	if (cur < 0)
		return -2;
	if (!AtTop())
		cur--;
	return cur;                       // -1: the ".." row
	}

TBool CPtRemoteDialog::OkToExitL(TInt aButtonId)
	{
	TInt row = Row();
	iIndex = row;
	if (aButtonId == EEikBidCancel)
		{
		iAction = EActCancel;
		return ETrue;
		}
	if (aButtonId == EPtBidUp || ((aButtonId == EPtBidOpen || (aButtonId == EEikBidOk && !iSend)) && row == -1))
		{
		if (AtTop())
			{
			iEikonEnv->InfoMsg(_L("This is the top folder"));
			return EFalse;
			}
		iAction = EActUp;
		return ETrue;
		}
	if (aButtonId == EPtBidOpen || (aButtonId == EEikBidOk && !iSend))
		{
		if (row < 0)
			{
			iEikonEnv->InfoMsg(iSend ? _L("No folder selected") : _L("Nothing to get - the folder is empty"));
			return EFalse;
			}
		TInt type = iList.At(row).iType;
		if (type == 'd' || type == 'l')
			{
			iAction = EActOpen;       // (a link: Open finds out what it points to)
			return ETrue;
			}
		if (iSend)
			{
			iEikonEnv->InfoMsg(_L("That is a file, not a folder"));
			return EFalse;
			}
		iAction = EActGet;
		return ETrue;
		}
	// Send: to the folder shown
	iAction = EActSendHere;
	return ETrue;
	}

TKeyResponse CPtRemoteDialog::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType == EEventKey && !(aKeyEvent.iModifiers & (EModifierCtrl | EModifierFunc)))
		{
		if (aKeyEvent.iCode == EKeyBackspace)
			{
			TryExitL(EPtBidUp);
			return EKeyWasConsumed;
			}
		if (aKeyEvent.iCode == EKeyEnter)
			{
			// Enter: Get / Send (in Get, on a folder, it opens it)
			TryExitL(EEikBidOk);
			return EKeyWasConsumed;
			}
		}
	return CEikDialog::OfferKeyEventL(aKeyEvent, aType);
	}

// a second tap on the highlighted row acts on it: opens a folder (Get and
// Send), gets a file (Get)
void CPtRemoteDialog::HandleListBoxEventL(CEikListBox* /*aListBox*/, TListBoxEvent aEventType)
	{
	if (aEventType != EEventItemDoubleClicked)
		return;
	TryExitL(iSend ? EPtBidOpen : EEikBidOk);
	}

// Lists aDir on the server ("" = the home folder) into aList; aDir becomes
// the folder's full name. Returns PSI_XFER_*.
static TInt ListL(CTermView& aView, TDes8& aDir, CRemoteList& aList, TBool& aMore, const TDesC& aTitle)
	{
	TInt r = RunXferL(aView, PSI_XOP_LIST, aDir, KNullDesC, aTitle, _L("Reading the server's folder..."));
	PsiShared* s = aView.XferShared();
	if (r != PSI_XFER_OK || !s)
		return r;
	TPtrC8 path((const TUint8*)s->xfer_path);
	aDir.Copy(path.Left(path.Length() < aDir.MaxLength() ? path.Length() : aDir.MaxLength()));
	TInt len = s->xfer_list_len;
	if (len < 0 || len > PSI_XFER_LIST_SIZE) len = 0;
	aList.SetL(TPtrC8((const TUint8*)s->xfer_list, len));
	aMore = s->xfer_list_more;
	return r;
	}

// The browsing loop shared by Send (pick a folder) and Get (pick a file).
// Returns ETrue with aPick = the chosen file (Get) or folder (Send).
static TBool BrowseL(CTermView& aView, TBool aSend, TDes8& aPick, TUint& aSize)
	{
	CPtXferMemory& mem = Mem(aView);
	TBuf<100> host;
	host.Copy(aView.Settings().iSshHost);
	if (host.CompareF(mem.iRemoteHost) != 0)
		{
		mem.iRemoteHost = host;
		mem.iRemoteDir.Zero();           // another server: start at its home folder
		}
	TBuf8<512>& dir = mem.iDir;              // (the view's scratch: see CPtXferMemory)
	TBuf8<512>& path = mem.iPath;
	dir = mem.iRemoteDir;
	CRemoteList* list = new(ELeave) CRemoteList;
	CleanupStack::PushL(list);
	TBool chosen = EFalse;
	for (;;)
		{
		TBool more = EFalse;
		TInt r = ListL(aView, dir, *list, more, aSend ? _L("Send file") : _L("Get file"));
		if (r != PSI_XFER_OK && dir.Length() && r != PSI_XFER_CANCELLED && r != PSI_XFER_NO_SFTP
			&& r != PSI_XFER_LINK && r != PSI_XFER_TIMEOUT)
			{
			// that folder has gone (or may not be read): back to the home folder
			XferFailed(r, aView);
			dir.Zero();
			continue;
			}
		if (r != PSI_XFER_OK)
			{
			XferFailed(r, aView);
			break;
			}
		mem.iRemoteDir = dir;
		TInt action = EActCancel, index = -2;
		CPtRemoteDialog* dlg = new(ELeave) CPtRemoteDialog(aSend, dir, *list, more, action, index);
		dlg->ExecuteLD(aSend ? R_PT_REMOTE_SEND_DIALOG : R_PT_REMOTE_GET_DIALOG);
		if (action == EActCancel)
			break;
		if (action == EActUp)
			{
			ParentDir(dir);
			continue;
			}
		if (action == EActSendHere)
			{
			aPick = dir;
			chosen = ETrue;
			break;
			}
		if (index < 0 || index >= list->Count())
			continue;
		if (!JoinDir(dir, list->Name(index), path))
			continue;                        // (too long: it has said so)
		TInt type = list->At(index).iType;
		aSize = list->At(index).iSize;
		if (type == 'l')
			{
			// a link: a folder to open, or a file to get?
			r = RunXferL(aView, PSI_XOP_STAT, path, KNullDesC, aSend ? _L("Send file") : _L("Get file"),
				_L("Looking at the link..."));
			PsiShared* s = aView.XferShared();
			if (r != PSI_XFER_OK || !s)
				{
				XferFailed(r, aView);
				if (r == PSI_XFER_LINK || r == PSI_XFER_NO_SFTP) break;
				continue;
				}
			if (s->xfer_exists == 0)
				{
				CEikonEnv::Static()->InfoMsg(_L("That link points nowhere"));
				continue;
				}
			type = s->xfer_exists == 2 ? 'd' : 'f';
			aSize = s->xfer_total;
			}
		if (type == 'd')
			{
			dir = path;
			continue;
			}
		if (aSend)
			{
			CEikonEnv::Static()->InfoMsg(_L("That is a file, not a folder"));
			continue;
			}
		aPick = path;
		chosen = ETrue;
		break;
		}
	CleanupStack::PopAndDestroy();       // list
	return chosen;
	}

// ============================================================================
// File > Send file...
// ============================================================================

void PtSendFileL(CTermView& aView)
	{
	if (NeedLoginL(aView))
		return;
	CEikonEnv* env = CEikonEnv::Static();
	CPtXferMemory& mem = Mem(aView);
	TFileName name(KDefaultFolder);
	if (mem.iSendFrom.Length())
		name = mem.iSendFrom;
	CEikFileOpenDialog* open = new(ELeave) CEikFileOpenDialog(&name, R_PT_TBUF_SEND_TITLE);
	if (!open->ExecuteLD(R_EIK_DIALOG_FILE_OPEN))
		return;
	mem.iSendFrom = name;
	TParsePtrC parse(name);
	TEntry entry;
	if (env->FsSession().Entry(name, entry) != KErrNone)
		{
		env->InfoMsg(_L("File not found"));
		return;
		}
	TBuf8<512>& dir = mem.iPick;             // (the view's scratch: see CPtXferMemory)
	TBuf8<512>& remote = mem.iRemote;
	TUint dummy = 0;
	if (!BrowseL(aView, ETrue, dir, dummy))
		return;
	TBuf8<300> leaf;
	TextToUtf8(parse.NameAndExt(), leaf);
	if (!JoinDir(dir, leaf, remote))
		return;                              // (too long: it has said so)

	// already there? (a folder of that name cannot be replaced)
	TInt r = RunXferL(aView, PSI_XOP_STAT, remote, KNullDesC, _L("Sending file"), _L("Looking on the server..."));
	PsiShared* s = aView.XferShared();
	if (r != PSI_XFER_OK || !s)
		{
		XferFailed(r, aView);
		return;
		}
	TBuf<60> shortName;
	Tail(parse.NameAndExt(), shortName, 40);
	if (s->xfer_exists == 2)
		{
		env->InfoMsg(_L("The server has a folder of that name"));
		return;
		}
	if (s->xfer_exists == 1)
		{
		TBuf<80> what;
		what.Format(_L("\"%S\" is already in that folder"), &shortName);
		if (!env->QueryWinL(what, _L("Replace it?")))
			return;
		}
	TBuf<512>& dirText = mem.iText;
	Utf8ToText(dir, dirText, EFalse);
	TBuf<30> dirShort;
	Tail(dirText, dirShort, 26);
	TBuf<80> what;
	what.Format(_L("\"%S\" to %S"), &shortName, &dirShort);
	r = RunXferL(aView, PSI_XOP_PUT, remote, name, _L("Sending file"), what);
	if (r != PSI_XFER_OK)
		{
		XferFailed(r, aView);
		return;
		}
	TBuf<20> size;
	SizeText(entry.iSize, size);
	TBuf<100> done;
	done.Format(_L("Sent \"%S\" (%S)"), &shortName, &size);
	env->InfoMsg(done);
	}

// ============================================================================
// File > Get file...
// ============================================================================

void PtGetFileL(CTermView& aView)
	{
	if (NeedLoginL(aView))
		return;
	CEikonEnv* env = CEikonEnv::Static();
	CPtXferMemory& mem = Mem(aView);
	TBuf8<512>& remote = mem.iPick;          // (the view's scratch: see CPtXferMemory)
	TUint size = 0;
	if (!BrowseL(aView, EFalse, remote, size))
		return;
	TInt slash = remote.LocateReverse('/');
	TBuf<256> leaf;
	Utf8ToText(remote.Mid(slash + 1), leaf, ETrue);
	while (leaf.Length() && leaf[0] == '.')
		leaf.Delete(0, 1);               // (".bashrc": EPOC names cannot start with a dot)
	if (leaf.Length() == 0)
		leaf.Copy(_L("File from server"));
	TFileName name;
	if (mem.iSaveTo.Length())
		name = TParsePtrC(mem.iSaveTo).DriveAndPath();
	else
		name = KDefaultFolder;
	if (name.Length() + leaf.Length() > name.MaxLength() - 2)
		leaf.SetLength(name.MaxLength() - 2 - name.Length());
	name.Append(leaf);
	CEikFileSaveAsDialog* save = new(ELeave) CEikFileSaveAsDialog(&name);
	if (!save->ExecuteLD(R_EIK_DIALOG_FILE_SAVEAS))
		return;
	mem.iSaveTo = name;
	RFs& fs = env->FsSession();
	TParsePtrC parse(name);

	// room for it? (it may still run out: psissh notices that too)
	TInt drive;
	TVolumeInfo vol;
	if (RFs::CharToDrive(name[0], drive) == KErrNone && fs.Volume(vol, drive) == KErrNone
		&& vol.iFree < TInt64((TInt)size) + TInt64(4096))
		{
		TBuf<20> need;
		SizeText(size, need);
		TBuf<80> m;
		m.Format(_L("Not enough room on the disk - the file is %S"), &need);
		env->InfoMsg(m);
		return;
		}
	fs.MkDirAll(parse.DriveAndPath());
	// into a temporary file, which replaces the real one only when the
	// whole file has arrived: a failed or stopped transfer leaves an old
	// file of that name as it was
	TFileName part(name);
	if (part.Length() > part.MaxLength() - 1)
		part.SetLength(part.MaxLength() - 1);
	part.Append('~');
	TBuf<60> shortName;
	Tail(parse.NameAndExt(), shortName, 40);
	TBuf<80> what;
	what.Format(_L("\"%S\""), &shortName);
	TInt r = RunXferL(aView, PSI_XOP_GET, remote, part, _L("Getting file"), what);
	if (r != PSI_XFER_OK)
		{
		fs.Delete(part);                 // (psissh has normally done this)
		XferFailed(r, aView);
		return;
		}
	TInt e = fs.Replace(part, name);
	if (e != KErrNone)
		{
		fs.Delete(part);
		env->InfoMsg(e == KErrInUse ? _L("Could not save the file - it is open in another program")
			: _L("Could not save the file"));
		return;
		}
	TEntry entry;
	TBuf<20> got;
	SizeText(fs.Entry(name, entry) == KErrNone ? entry.iSize : size, got);
	TBuf<100> done;
	done.Format(_L("Saved \"%S\" (%S)"), &shortName, &got);
	env->InfoMsg(done);
	}

// ============================================================================
// The session log
// ============================================================================

CPtLog* CPtLog::NewL(RFs& aFs)
	{
	return new(ELeave) CPtLog(aFs);
	}

CPtLog::~CPtLog()
	{
	if (iOpen)
		{
		DoFlush();
		if (iOpen)
			iFile.Close();
		}
	}

TInt CPtLog::Start(const TDesC& aFile, TBool aRaw, TBool aAppend)
	{
	if (iOpen)
		Stop();
	TParsePtrC parse(aFile);
	iFs.MkDirAll(parse.DriveAndPath());
	TInt r = KErrNotFound;
	if (aAppend)
		{
		r = iFile.Open(iFs, aFile, EFileWrite | EFileShareExclusive);
		if (r == KErrNone)
			{
			TInt pos = 0;
			r = iFile.Seek(ESeekEnd, pos);
			if (r != KErrNone)
				iFile.Close();
			}
		}
	if (r == KErrNotFound || (!aAppend && r != KErrInUse))
		r = iFile.Replace(iFs, aFile, EFileWrite | EFileShareExclusive);
	if (r != KErrNone)
		return r;
	iOpen = ETrue;
	iRaw = aRaw;
	iName = aFile;
	iBuf.Zero();
	iBytes = 0;
	iState = 0;
	iUtfLeft = 0;
	iFailErr = 0;
	return KErrNone;
	}

TBool CPtLog::Stop()
	{
	TBool wasOpen = iOpen;
	Flush();
	if (!iOpen)
		return !wasOpen;                 // (Flush failed and has said so)
	iFile.Close();
	iOpen = EFalse;
	return ETrue;
	}

// A write failed (disk full, card taken out): the log stops. Said later, by
// Flush: this may be in the middle of the terminal's painting, when another
// window must not draw (WSERV 10).
void CPtLog::Fail(TInt aErr)
	{
	iFile.Close();
	iOpen = EFalse;
	iBuf.Zero();
	iFailErr = aErr;
	}

void CPtLog::Flush()
	{
	DoFlush();
	TInt err = iFailErr;
	if (!err)
		return;
	iFailErr = 0;
	CEikonEnv* env = CEikonEnv::Static();
	if (err == KErrDiskFull)
		env->InfoMsg(_L("Log stopped - the disk is full"));
	else if (err == KErrNotReady || err == KErrDisMounted || err == KErrCorrupt)
		env->InfoMsg(_L("Log stopped - the disk was removed"));
	else
		{
		TBuf<60> m;
		m.Format(_L("Log stopped - could not write the file (%d)"), err);
		env->InfoMsg(m);
		}
	}

void CPtLog::DoFlush()
	{
	if (!iOpen || iBuf.Length() == 0)
		return;
	TInt r = iFile.Write(iBuf);
	if (r == KErrNone)
		r = iFile.Flush();
	if (r != KErrNone)
		{
		Fail(r);
		return;
		}
	iBytes += iBuf.Length();
	iBuf.Zero();
	}

void CPtLog::Put(TUint8 aByte)
	{
	if (iBuf.Length() == iBuf.MaxLength())
		{
		DoFlush();
		if (!iOpen)
			return;
		}
	iBuf.Append(aByte);
	}

// one character for the plain-text log, in the Psion's code page
void CPtLog::PutChar(TUint aCh)
	{
	if (aCh < 0x80)
		{
		Put((TUint8)aCh);
		return;
		}
	TInt m = PsiMapToCodePage(aCh);
	if (m >= 0)
		{
		Put((TUint8)m);
		return;
		}
	TInt seg = PsiBoxSegments(aCh);        // box drawing: + - |
	if (seg >= 0)
		{
		TBool h = (seg & (KBoxLeft | KBoxRight)) != 0, v = (seg & (KBoxUp | KBoxDown)) != 0;
		Put((TUint8)(h && v ? '+' : h ? '-' : '|'));
		return;
		}
	Put('?');
	}

// Plain text: the terminal's control sequences (colours, cursor moves,
// titles...) are taken out; what is left is the text as it arrived.
enum { ELogText, ELogEsc, ELogCsi, ELogString, ELogStringEsc, ELogEscMore };

void CPtLog::Write(const TUint8* aData, TInt aLen)
	{
	if (!iOpen)
		return;
	if (iRaw)
		{
		for (TInt i = 0; i < aLen && iOpen; i++)
			Put(aData[i]);
		return;
		}
	for (TInt i = 0; i < aLen && iOpen; i++)
		{
		TUint c = aData[i];
		switch (iState)
			{
		case ELogEsc:
			if (c == '[') iState = ELogCsi;
			else if (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') iState = ELogString;
			else if (c >= 0x20 && c <= 0x2f) iState = ELogEscMore;
			else iState = ELogText;      // a two-character sequence (ESC 7, ESC M...)
			continue;
		case ELogCsi:
			if (c >= 0x40 && c <= 0x7e) iState = ELogText;
			else if (c == 0x1b) iState = ELogEsc;
			continue;
		case ELogString:                 // OSC etc: until BEL or ESC backslash
			if (c == 7) iState = ELogText;
			else if (c == 0x1b) iState = ELogStringEsc;
			continue;
		case ELogStringEsc:
			iState = (c == '\\') ? ELogText : ELogString;
			continue;
		case ELogEscMore:                // ESC ( B and the like
			if (c >= 0x30 && c <= 0x7e) iState = ELogText;
			continue;
		default:
			break;
			}
		if (iUtfLeft > 0)
			{
			if ((c & 0xc0) == 0x80)
				{
				iUtf = (iUtf << 6) | (c & 0x3f);
				if (--iUtfLeft == 0)
					PutChar(iUtf);
				continue;
				}
			iUtfLeft = 0;
			Put('?');                     // a broken character: carry on with this byte
			}
		if (c == 0x1b)
			iState = ELogEsc;
		else if (c == '\n')
			{
			Put('\r');
			Put('\n');
			}
		else if (c == '\t')
			Put('\t');
		else if (c == 8)
			{
			// backspace: take back the last character, if it is still here
			if (iBuf.Length() && iBuf[iBuf.Length() - 1] != '\n')
				iBuf.SetLength(iBuf.Length() - 1);
			}
		else if (c < 0x20 || c == 0x7f)
			;                            // CR, BEL and other controls: nothing
		else if (c >= 0xc0 && c < 0xf8)
			{
			iUtfLeft = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
			iUtf = c & (0x3f >> iUtfLeft);
			}
		else if (c >= 0x80)
			Put((TUint8)c);              // not UTF-8 (a Latin-1 server): as it is
		else
			Put((TUint8)c);
		}
	}

// Log to file: the file, plain or raw, add to it or replace it
class CPtLogDialog : public CEikDialog
	{
public:
	CPtLogDialog(TDes& aFile, TInt& aRaw, TInt& aReplace) : iFile(aFile), iRaw(aRaw), iReplace(aReplace) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TDes& iFile;
	TInt& iRaw;
	TInt& iReplace;
	};

void CPtLogDialog::PreLayoutDynInitL()
	{
	SetEdwinTextL(EPtDlgLogFile, &iFile);
	SetChoiceListCurrentItem(EPtDlgLogText, iRaw ? 1 : 0);
	SetChoiceListCurrentItem(EPtDlgLogExisting, iReplace ? 1 : 0);
	}

TBool CPtLogDialog::OkToExitL(TInt /*aButtonId*/)
	{
	TFileName f;
	GetEdwinText(f, EPtDlgLogFile);
	f.Trim();
	if (f.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No filename entered"));
		TryChangeFocusToL(EPtDlgLogFile);
		return EFalse;
		}
	// a name alone goes in \Documents\ on the internal disk
	TParse p;
	if (p.Set(f, &KDefaultLog, NULL) != KErrNone || !iEikonEnv->FsSession().IsValidName(p.FullName())
		|| p.NameAndExt().Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("Invalid filename"));
		TryChangeFocusToL(EPtDlgLogFile);
		return EFalse;
		}
	iFile = p.FullName();
	iRaw = ChoiceListCurrentItem(EPtDlgLogText);
	iReplace = ChoiceListCurrentItem(EPtDlgLogExisting);
	return ETrue;
	}

TBool PtLogging(const CTermView& aView)
	{
	return aView.iLog && aView.iLog->Active();
	}

void PtLogCommandL(CTermView& aView)
	{
	CEikonEnv* env = CEikonEnv::Static();
	RFs& fs = env->FsSession();
	if (PtLogging(aView))
		{
		// (the tick box is on: this stops the log)
		if (!aView.iLog->Stop())
			return;                       // (it has said why)
		TBuf<20> size;
		SizeText(aView.iLog->Bytes(), size);
		TParsePtrC parse(aView.iLog->FileName());
		TBuf<40> leaf;
		Tail(parse.NameAndExt(), leaf, 30);
		TBuf<100> m;
		m.Format(_L("Log stopped - %S in \"%S\""), &size, &leaf);
		env->InfoMsg(m);
		return;
		}
	// last time's choices (Log.ini: raw, replace, then the file name)
	TFileName file(KDefaultLog);
	TInt raw = 0, replace = 0;
	RFile ini;
	if (ini.Open(fs, KLogIni, EFileRead) == KErrNone)
		{
		TBuf8<260> d;
		if (ini.Read(d) == KErrNone && d.Length() > 3 && d[0] == 'L')
			{
			raw = d[1] == '1';
			replace = d[2] == '1';
			TPtrC8 n(d.Mid(3));
			if (n.Length() && n.Length() <= file.MaxLength())
				file.Copy(n);
			}
		ini.Close();
		}
	CPtLogDialog* dlg = new(ELeave) CPtLogDialog(file, raw, replace);
	if (!dlg->ExecuteLD(R_PT_LOG_DIALOG))
		return;
	TBuf8<260> d;
	d.Append('L');
	d.Append(raw ? '1' : '0');
	d.Append(replace ? '1' : '0');
	d.Append(file.Left(file.Length() < 256 ? file.Length() : 256));
	SafeWrite(fs, KLogIni, d);           // (into a temporary, then swapped in, as every data file)
	if (!aView.iLog)
		aView.iLog = CPtLog::NewL(fs);
	TInt r = aView.iLog->Start(file, raw, !replace);
	TParsePtrC parse(file);
	TBuf<40> leaf;
	Tail(parse.NameAndExt(), leaf, 30);
	TBuf<100> m;
	if (r == KErrNone)
		m.Format(_L("Logging to \"%S\""), &leaf);
	else if (r == KErrInUse)
		m.Copy(_L("Could not log - the file is open in another program"));
	else if (r == KErrDiskFull)
		m.Copy(_L("Could not log - the disk is full"));
	else if (r == KErrNotReady || r == KErrPathNotFound)
		m.Copy(_L("Could not log - that disk is not there"));
	else
		m.Format(_L("Could not log to that file (%d)"), r);
	env->InfoMsg(m);
	}
