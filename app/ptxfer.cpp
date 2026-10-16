// PTXFER.CPP - PsiTerm's file transfer and session log (0.74; 0.91: the file view)
//
// File > Send file...   pick a Psion file (the standard Open file dialog),
//                       then the server folder it goes to in the file view
// File > Get file...    pick a file in the server's folders in the file view,
//                       then where it goes on the Psion (the standard Save as)
// File > Log to file... everything received is written to a file
//
// The transfers are done by psissh.exe (ssh/sftp.c) over SFTP, on the SSH
// connection that is already logged in: this file posts one request at a
// time in the shared memory (psishared.h, xfer_*). A transfer shows the
// progress window (with Stop) until psissh says it has finished; the file
// view (0.91) is a view of the program's own, not a dialog, and lists
// folders in place, ticking until psissh answers.

#include "psiterm.h"
#include "ptxfer.h"

#include <eikcfdlg.h>
#include <eiklabel.h>
#include <eikclb.h>
#include <eikclbd.h>
#include <eikfctry.h>       // SEikControlInfo
#include <eikchlst.h>
#include <eikprogi.h>
#include <eiktxtut.h>
#include <eikedwin.h>
#include <eikon.rsg>
#include <eikbtpan.h>
#include <eikmover.h>
#include <eikcapc.h>

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

// the short form for lists and gauges: "980 B", "1.2K", "42K", "4.2M", "120M"
static void ShortSize(TUint aSize, TDes& aOut)
	{
	if (aSize < 1024)
		aOut.Format(_L("%u B"), aSize);
	else if (aSize < 10 * 1024)
		aOut.Format(_L("%u.%uK"), aSize / 1024, ((aSize % 1024) * 10) / 1024);
	else if (aSize < 1024 * 1024)
		aOut.Format(_L("%uK"), aSize / 1024);
	else if (aSize < 10 * 1024 * 1024)
		aOut.Format(_L("%u.%uM"), aSize >> 20, (((aSize & 0xfffff) >> 10) * 10) >> 10);
	else
		aOut.Format(_L("%uM"), aSize >> 20);
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

// the first aLevel folders of aDir ("/a/b/c", 2 -> "/a/b"; 0 -> "/")
static void TrimDirToLevel(TDes8& aDir, TInt aLevel)
	{
	TInt i = 0, k = 0;
	while (k < aLevel)
		{
		while (i < aDir.Length() && aDir[i] == '/')
			i++;
		if (i >= aDir.Length())
			break;
		while (i < aDir.Length() && aDir[i] != '/')
			i++;
		k++;
		}
	if (k == 0)
		aDir.Copy(_L8("/"));
	else
		aDir.SetLength(i);
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
	CFont* iBigFont;                 // the large percentage
	TBool iStarted;                  // the first byte has moved
	TTime iStart;                    // ...and when
	};

CPtXferDialog::~CPtXferDialog()
	{
	if (iTimer)
		iTimer->Cancel();
	delete iTimer;
	if (iBigFont)
		iEikonEnv->ScreenDevice()->ReleaseFont(iBigFont);
	}

void CPtXferDialog::PreLayoutDynInitL()
	{
	SetTitleL(iTitle);
	// the percentage in a large bold font (a plain label at the normal size
	// was hard to read at arm's length)
	TFontSpec spec(_L("Arial"), 360);
	spec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	if (iEikonEnv->ScreenDevice()->GetNearestFontInTwips(iBigFont, spec) == KErrNone)
		((CEikLabel*)Control(EPtDlgXferPct))->SetFont(iBigFont);
	// (room for the longest lines before the window is laid out)
	SetLabelReserveLengthL(EPtDlgXferWhat, 52);
	SetLabelReserveLengthL(EPtDlgXferPct, 5);
	SetLabelReserveLengthL(EPtDlgXferHow, 52);
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

// "1.2M / 4.2M - 38K/s - about 1 min left"
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
	if (done > 0 && !iStarted)
		{
		iStarted = ETrue;
		iStart.HomeTime();
		}
	TInt pc = 0;
	if (total > 0)
		{
		pc = total >= 0x1000000 ? (TInt)(done / (total / 100)) : (TInt)((done * 100) / total);
		if (pc > 100) pc = 100;
		}
	TBuf<64> how;
	TBuf<8> big;
	if (iStopping)
		how.Copy(_L("Stopping..."));
	else if (total > 0)
		{
		TBuf<12> a, b;
		ShortSize(done, a);
		ShortSize(total, b);
		how.Format(_L("%S / %S"), &a, &b);
		TTime now;
		now.HomeTime();
		TTimeIntervalSeconds el(0);
		if (iStarted)
			now.SecondsFrom(iStart, el);
		TInt secsGone = el.Int();
		if (secsGone >= 2 && done > 0)
			{
			TUint rate = done / (TUint)secsGone;                    // bytes a second
			TBuf<12> r;
			ShortSize(rate, r);
			how.Append(_L(" - "));
			how.Append(r);
			how.Append(_L("/s"));
			if (rate > 0 && total > done)
				{
				TUint secs = (total - done) / rate;
				TBuf<24> eta;
				if (secs < 90)
					eta.Format(_L(" - %u s left"), secs + 1);
				else
					eta.Format(_L(" - about %u min left"), (secs + 30) / 60);
				how.Append(eta);
				}
			}
		}
	else if (done > 0)
		ShortSize(done, how);
	else
		how.Copy(_L("Waiting for the server..."));
	if (total > 0)
		big.Format(_L("%d%%"), pc);
	else
		big.Copy(_L("..."));
	SetLabelL(EPtDlgXferPct, big);
	SetLabelL(EPtDlgXferHow, how);
	CEikProgressInfo* gauge = (CEikProgressInfo*)Control(EPtDlgXferGauge);
	gauge->SetAndDraw(total > 0 ? pc * 10 : 0);
	Control(EPtDlgXferPct)->DrawNow();
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

// Posts one request for psissh. Returns its number (0: no SSH session).
// (0.91) A listing the file view was still waiting for when it closed is
// stopped there and then: psissh ends a LIST or STAT at its next turn, so
// this waits a moment for that before posting.
static TUint PostXfer(CTermView& aView, TInt aOp, const TDesC8& aRemote, const TDesC& aLocal)
	{
	PsiShared* s = aView.XferShared();
	if (s && s->xfer_req != s->xfer_ack)
		{
		s->xfer_cancel = 1;
		aView.RingSsh();
		for (TInt i = 0; i < 40 && s && s->xfer_req != s->xfer_ack; i++)
			{
			User::After(50000);
			s = aView.XferShared();
			}
		}
	if (!s || s->xfer_req != s->xfer_ack)
		return 0;
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
	if (req == 0)
		req = 1;                         // (0 means "none" here)
	s->xfer_req = req;                   // last: psissh starts now
	aView.RingSsh();
	return req;
	}

// Posts one request and waits for it, with the progress window if it takes
// more than a moment. Returns PSI_XFER_*.
static TInt RunXferL(CTermView& aView, TInt aOp, const TDesC8& aRemote, const TDesC& aLocal,
	const TDesC& aTitle, const TDesC& aWhat)
	{
	TUint req = PostXfer(aView, aOp, aRemote, aLocal);
	if (!req)
		return PSI_XFER_LINK;
	PsiShared* s;
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
	TUint iTime;           // modified, seconds since 1970 (0 = not known)
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
		// <type><size> TAB <mtime> TAB <name>
		TInt tab = line.Locate('\t');
		TInt tab2 = -1;
		if (tab >= 1)
			{
			TPtrC8 after(line.Mid(tab + 1));
			TInt t2 = after.Locate('\t');
			if (t2 >= 0)
				tab2 = tab + 1 + t2;
			}
		if (tab >= 1 && tab2 > tab && tab2 < line.Length() - 1)
			{
			TRemoteEntry e;
			e.iType = line[0];
			e.iSize = 0;
			for (TInt i = 1; i < tab && line[i] >= '0' && line[i] <= '9'; i++)
				e.iSize = e.iSize * 10 + (line[i] - '0');
			e.iTime = 0;
			for (TInt i = tab + 1; i < tab2 && line[i] >= '0' && line[i] <= '9'; i++)
				e.iTime = e.iTime * 10 + (line[i] - '0');
			e.iOff = pos + tab2 + 1;
			e.iLen = line.Length() - tab2 - 1;
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


// the date in figures, in the order and with the separators the Psion is set
// to ("06/10/26"), as EIKON's own file browser shows dates; blank if unknown
static void DateText(TUint aUnix, TDes& aOut)
	{
	aOut.Zero();
	if (aUnix == 0)
		return;
	// days since 1970 to a calendar date
	TInt z = (TInt)(aUnix / 86400) + 719468;
	TInt era = z / 146097;
	TInt doe = z - era * 146097;
	TInt yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	TInt y = yoe + era * 400;
	TInt doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	TInt mp = (5 * doy + 2) / 153;
	TInt d = doy - (153 * mp + 2) / 5 + 1;
	TInt m = mp < 10 ? mp + 3 : mp - 9;
	if (m <= 2) y++;
	if (m < 1 || m > 12)
		return;
	TLocale loc;
	TUint s1 = (TUint)loc.DateSeparator(1), s2 = (TUint)loc.DateSeparator(2);
	if (s1 == 0) s1 = '/';
	if (s2 == 0) s2 = '/';
	TInt a = d, b = m, c = y % 100;
	if (loc.DateFormat() == EDateAmerican)
		{ a = m; b = d; }
	else if (loc.DateFormat() == EDateJapanese)
		{ a = y % 100; c = d; }
	aOut.Format(_L("%02d%c%02d%c%02d"), a, s1, b, s2, c);
	}

// a size for the list's Size column, right-aligned: "312" (bytes), "1.8K",
// "98K", "4.2M", "158M", "1.2G"
static void ListSize(TUint aSize, TDes& aOut)
	{
	if (aSize < 1024)
		aOut.Format(_L("%u"), aSize);
	else if (aSize < 10 * 1024)
		aOut.Format(_L("%u.%uK"), aSize / 1024, ((aSize % 1024) * 10) / 1024);
	else if (aSize < 1024 * 1024)
		aOut.Format(_L("%uK"), aSize / 1024);
	else if (aSize < 10 * 1024 * 1024)
		aOut.Format(_L("%u.%uM"), aSize >> 20, (((aSize & 0xfffff) >> 10) * 10) >> 10);
	else if (aSize < 1024u * 1024u * 1024u)
		aOut.Format(_L("%uM"), aSize >> 20);
	else
		aOut.Format(_L("%u.%uG"), aSize >> 30, (((aSize >> 20) & 1023) * 10) >> 10);
	}

// ----- favourite folders ----------------------------------------------------
// One list per server: "user@host:port", a tab, then the folder (UTF-8), a
// line each, in Folders.dat beside PsiTerm's other files.

_LIT(KFavFile, "C:\\System\\Apps\\PsiTerm\\Folders.dat");
const TInt KMaxFavs = 20;                  // for one server
const TInt KFavFileMax = 16 * 1024;

static void FavServer(CTermView& aView, TDes8& aKey)
	{
	const TPsiSettings& st = aView.Settings();
	TBuf<180> k(st.iSshUser);
	k.Append('@');
	k.Append(st.iSshHost);
	k.Append(':');
	k.AppendNum(st.iSshPort);
	TextToUtf8(k, aKey);
	}

// every line of the file (all the servers'); a missing file is no lines
static void FavLoadL(RFs& aFs, CDesC8ArrayFlat& aLines)
	{
	RFile f;
	if (f.Open(aFs, KFavFile, EFileRead | EFileShareReadersOnly) != KErrNone)
		return;
	CleanupClosePushL(f);
	TInt size = 0;
	f.Size(size);
	if (size > KFavFileMax)
		size = KFavFileMax;
	HBufC8* buf = HBufC8::NewLC(size);
	TPtr8 p = buf->Des();
	f.Read(p, size);
	TPtrC8 rest(p);
	while (rest.Length())
		{
		TInt e = rest.Locate('\n');
		TPtrC8 line = e < 0 ? rest : rest.Left(e);
		rest.Set(e < 0 ? TPtrC8() : rest.Mid(e + 1));
		if (line.Length() && line[line.Length() - 1] == '\r')
			line.Set(line.Left(line.Length() - 1));
		if (line.Locate('\t') > 0)
			aLines.AppendL(line);
		}
	CleanupStack::PopAndDestroy(2);         // buf, f
	}

static TBool FavSave(RFs& aFs, const CDesC8ArrayFlat& aLines)
	{
	RFile f;
	if (f.Replace(aFs, KFavFile, EFileWrite) != KErrNone)
		return EFalse;
	TInt err = KErrNone;
	for (TInt i = 0; i < aLines.Count() && err == KErrNone; i++)
		{
		err = f.Write(aLines[i]);
		if (err == KErrNone)
			err = f.Write(_L8("\n"));
		}
	f.Close();
	return err == KErrNone;
	}

// the folder in a line of aServer's, or an empty pointer
static TPtrC8 FavFolder(const TDesC8& aLine, const TDesC8& aServer)
	{
	TInt tab = aLine.Locate('\t');
	if (tab <= 0 || aLine.Left(tab) != aServer)
		return TPtrC8();
	return aLine.Mid(tab + 1);
	}

// aServer's favourite folders, in the order they were added
static void FavMineL(RFs& aFs, const TDesC8& aServer, CDesC8ArrayFlat& aFolders)
	{
	CDesC8ArrayFlat* lines = new(ELeave) CDesC8ArrayFlat(8);
	CleanupStack::PushL(lines);
	FavLoadL(aFs, *lines);
	for (TInt i = 0; i < lines->Count(); i++)
		{
		TPtrC8 folder = FavFolder((*lines)[i], aServer);
		if (folder.Length())
			aFolders.AppendL(folder);
		}
	CleanupStack::PopAndDestroy();          // lines
	}

// Favourite (Ctrl+F) in the file view: the folder shown goes in aServer's
// list, or comes out of it if it is there already. Says which.
static void FavToggleL(RFs& aFs, const TDesC8& aServer, const TDesC8& aDir)
	{
	CEikonEnv* env = CEikonEnv::Static();
	CDesC8ArrayFlat* lines = new(ELeave) CDesC8ArrayFlat(8);
	CleanupStack::PushL(lines);
	FavLoadL(aFs, *lines);
	TInt found = -1, mine = 0;
	for (TInt i = 0; i < lines->Count(); i++)
		{
		TPtrC8 folder = FavFolder((*lines)[i], aServer);
		if (!folder.Length())
			continue;
		mine++;
		if (folder == aDir)
			found = i;
		}
	if (found >= 0)
		{
		lines->Delete(found);
		env->InfoMsg(FavSave(aFs, *lines) ? _L("Removed from favourites")
			: _L("Not removed - the internal disk is full or in use"));
		}
	else if (mine >= KMaxFavs)
		env->InfoMsg(_L("Not added - 20 favourite folders is the most"));
	else
		{
		HBufC8* line = HBufC8::NewLC(aServer.Length() + 1 + aDir.Length());
		line->Des().Copy(aServer);
		line->Des().Append('\t');
		line->Des().Append(aDir);
		lines->AppendL(*line);
		CleanupStack::PopAndDestroy();       // line
		env->InfoMsg(FavSave(aFs, *lines) ? _L("Added to favourites")
			: _L("Not added - the internal disk is full or in use"));
		}
	CleanupStack::PopAndDestroy();          // lines
	}

// The server's folders as a file view of the program's own (0.91: no longer
// a dialog over the terminal). While browsing, the program's window is the
// file view, laid out as EIKON's own file browser and the System screen's
// folder view: a title band (what this is, and the folder); on the left a
// framed Location pane - the server, its favourite folders (one tap goes
// there), the folders down to this one (tap one to go back to it), and what
// is in this folder; on the right a framed list with its column headings
// (Name, Size, Modified). The app UI gives it its own menus, shortcuts and
// toolbar (Up, Favourite, Get or Send here, Close), and Close puts the
// terminal back. Folders are listed in place: the view stays on screen with
// the folder it shows while psissh lists the next one ("Opening folder..."
// bottom left if that takes more than a moment), then shows it.

#include "pticons.h"

const TInt KBrowseSideW = 150;     // the Location pane
const TInt KBrowseGap = 4;         // round the panes and between them
const TInt KBrowseIconW = 20;      // the picture column
const TInt KBrowseSizeW = 40;      // Size, right-aligned
const TInt KBrowseDateW = 64;      // Modified, right-aligned (room for its heading)
const TInt KBrowseBar = 25;        // room for the scroll bar, at the right
const TInt KBrowseFootRows = 2;    // the Location pane's foot: what is in the folder
const TInt KBrowseIcons = 5;       // folder, open folder, file, up, server (then the star)
const TInt KBrowseTextDy = 0;      // (text baseline adjustment, to match the list's rows)
const TInt KPaneLevel = 1000;      // the Location pane's keyboard choice: favourites 0.., then the folders
const TInt KMenuFavs = 7;          // favourites on the Favourites menu (with its tick box: 8 items)

enum { EBrPicFolder, EBrPicOpen, EBrPicFile, EBrPicUp, EBrPicHost, EBrPicStar };

class MPtBrowseObserver
	{
public:
	virtual void BrowseGoToLevel(TInt aLevel) = 0;
	virtual void BrowseGoToFav(TInt aIndex) = 0;
	virtual void BrowseFavPage() = 0;            // a tap on the Favourites heading: the next ones
	};

// What the column headings and the Location pane draw; the view owns it.
class CPtBrowseData : public CBase
	{
public:
	~CPtBrowseData()
		{
		delete iNames;
		delete iPath;
		delete iFavs;
		delete iFavNames;
		if (iIcons)
			iIcons->ResetAndDestroy();
		delete iIcons;
		}
	CPtBrowseData() : iFavHere(-1), iRowH(15), iPaneSel(-1) {}
	void SetPathL(const TDesC8& aDir);
	TInt Levels() const { return iNames ? iNames->Count() : 0; }   // 0 = "/"
	TInt Favs() const { return iFavs ? iFavs->Count() : 0; }
	TBuf<64> iHost;
	HBufC* iPath;                        // the whole path
	CDesCArrayFlat* iNames;              // the folders, one per level below "/"
	CDesCArrayFlat* iFavs;               // this server's favourite folders (whole paths)
	CDesCArrayFlat* iFavNames;           // ...by their last folder, as the pane shows them
	TInt iFavHere;                       // the one that is the folder shown, or -1
	TInt iFavTop;                        // the first favourite the pane shows
	TBool iShown;                        // a folder is shown (not until the first listing)
	TInt iFolders, iFiles;
	TUint iTotal;                        // the files' bytes
	CArrayPtrFlat<CFbsBitmap>* iIcons;   // picture, mask, picture, mask... (NULL: none)
	const CFont* iBold;                  // headings: the dense font in bold
	TInt iRowH;                          // the list's row height; the pane uses the same rows
	TInt iNameX, iSizeR, iDateR;         // where the list's columns put their text
	TBool iPaneFocus;                    // the keys are in the Location pane (Left arrow)
	TInt iPaneSel;                       // ...on favourite N, or KPaneLevel + a folder's level
	MPtBrowseObserver* iObs;
	};

void CPtBrowseData::SetPathL(const TDesC8& aDir)
	{
	delete iNames;
	iNames = NULL;
	delete iPath;
	iPath = NULL;
	iNames = new(ELeave) CDesCArrayFlat(8);
	iPath = HBufC::NewL(aDir.Length() + 2);
	TPtr p = iPath->Des();
	Utf8ToText(aDir, p, EFalse);
	TInt i = 0;
	TBuf<80> part;
	while (i < aDir.Length())
		{
		while (i < aDir.Length() && aDir[i] == '/')
			i++;
		TInt start = i;
		while (i < aDir.Length() && aDir[i] != '/')
			i++;
		if (i > start)
			{
			Utf8ToText(aDir.Mid(start, i - start), part, EFalse);
			iNames->AppendL(part);
			}
		}
	}

// the end of a path that fits aWidth pixels, from a whole folder: ".../c/d"
static void FitTail(const TDesC& aIn, const CFont& aFont, TInt aWidth, TDes& aOut)
	{
	TInt n = aIn.Length();
	for (;;)
		{
		Tail(aIn, aOut, n);
		if (aIn.Length() > n)
			{
			TInt slash = aOut.Mid(3).Locate('/');           // (after the "...")
			if (slash >= 0 && slash + 3 < aOut.Length() - 1)
				aOut.Delete(3, slash);
			}
		if (n <= 4 || aFont.TextWidthInPixels(aOut) <= aWidth)
			return;
		n -= 2;
		}
	}

// The list, with no margins inside its border, so that its rows sit on the
// same grid as the Location pane's and it fills its frame exactly
class CPtBrowseList : public CEikColumnListBox
	{
public:
	void NoMargins() { SetHorizontalMargin(0); SetVerticalMargin(0); }
	};

// the Location pane's rows, under the server's: the Favourites heading (row
// 0) and the favourites, the folders down to this one, the foot
struct TPtSideLayout
	{
	TInt iRows;          // rows under the server's row
	TInt iFavShown;      // rows for favourites, under their heading
	TInt iTreeRow;       // the first folder's row
	TInt iTreeRows;
	TInt iFirst;         // the first level shown (the top ones go when it is deep)
	TInt iFootRow;
	};

// A part of the file view that draws itself: the column headings over the
// list (kind 0) or the Location pane (kind 1)
class CPtBrowsePart : public CCoeControl
	{
public:
	void Set(TInt aKind, const CPtBrowseData* aData) { iKind = aKind; iData = aData; }
	void Tap(const TPoint& aPos);       // a tap on the pane (the view passes it on)
	void Layout(TPtSideLayout& aL) const;
private:
	void Draw(const TRect& aRect) const;
	void DrawHead(CWindowGc& aGc, const TRect& aRect) const;
	void DrawSide(CWindowGc& aGc, const TRect& aRect) const;
	void Text(CWindowGc& aGc, const CFont* aFont, const TDesC& aText, TInt aLeft, TInt aRight, TInt aRowTop,
		CGraphicsContext::TTextAlign aAlign, TRgb aColour = KRgbBlack) const;
	void Picture(CWindowGc& aGc, TInt aPic, TInt aX, TInt aRowTop) const;
	void Rule(CWindowGc& aGc, TInt aX1, TInt aX2, TInt aY) const;
	void VRule(CWindowGc& aGc, TInt aX, TInt aY1, TInt aY2) const;
	void Highlight(CWindowGc& aGc, TInt aX1, TInt aX2, TInt aRowTop) const;
	void Box(CWindowGc& aGc, TInt aX1, TInt aX2, TInt aRowTop) const;
	TInt RowTop(TInt aRow) const { return Rect().iTl.iY + 2 + iData->iRowH * (aRow + 1); }
	TInt iKind;
	const CPtBrowseData* iData;
	};

// text in a row of the list's height, on the same baseline as the list's text
void CPtBrowsePart::Text(CWindowGc& aGc, const CFont* aFont, const TDesC& aText, TInt aLeft, TInt aRight,
	TInt aRowTop, CGraphicsContext::TTextAlign aAlign, TRgb aColour) const
	{
	TInt h = iData->iRowH;
	aGc.UseFont(aFont);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(aColour);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	TInt base = (h - aFont->HeightInPixels()) / 2 + aFont->AscentInPixels() + KBrowseTextDy;
	aGc.DrawText(aText, TRect(aLeft, aRowTop, aRight, aRowTop + h), base, aAlign, 0);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.DiscardFont();
	}

void CPtBrowsePart::Picture(CWindowGc& aGc, TInt aPic, TInt aX, TInt aRowTop) const
	{
	CArrayPtrFlat<CFbsBitmap>* a = iData ? iData->iIcons : NULL;
	if (!a || aPic * 2 + 1 >= a->Count())
		return;
	CFbsBitmap* bmp = a->At(aPic * 2);
	CFbsBitmap* mask = a->At(aPic * 2 + 1);
	TSize s = bmp->SizeInPixels();
	aGc.BitBltMasked(TPoint(aX, aRowTop + (iData->iRowH - s.iHeight) / 2), bmp, TRect(s), mask, ETrue);
	}

// a grey line, as the panes' frames
void CPtBrowsePart::Rule(CWindowGc& aGc, TInt aX1, TInt aX2, TInt aY) const
	{
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(KRgbDarkGray);
	aGc.DrawLine(TPoint(aX1, aY), TPoint(aX2, aY));
	}

void CPtBrowsePart::VRule(CWindowGc& aGc, TInt aX, TInt aY1, TInt aY2) const
	{
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(KRgbDarkGray);
	aGc.DrawLine(TPoint(aX, aY1), TPoint(aX, aY2));
	}

// the keyboard's place in the pane: black, as the list's highlight
void CPtBrowsePart::Highlight(CWindowGc& aGc, TInt aX1, TInt aX2, TInt aRowTop) const
	{
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(KRgbBlack);
	aGc.DrawRect(TRect(aX1, aRowTop, aX2, aRowTop + iData->iRowH));
	aGc.SetBrushColor(KRgbWhite);      // (BitBltMasked uses the brush: the pictures went black)
	}

// the folder shown: boxed, as EIKON's browser marks the folder of the list
void CPtBrowsePart::Box(CWindowGc& aGc, TInt aX1, TInt aX2, TInt aRowTop) const
	{
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(KRgbBlack);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.DrawRect(TRect(aX1, aRowTop, aX2, aRowTop + iData->iRowH - 1));
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	}

void CPtBrowsePart::Draw(const TRect& /*aRect*/) const
	{
	if (!iData)
		return;
	CWindowGc& gc = SystemGc();
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	if (iKind == 0)
		DrawHead(gc, Rect());
	else
		DrawSide(gc, Rect());
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	}

// The column headings, in bold, over the list's columns: the top of the
// list's frame. (The list's own border is the line under them.)
void CPtBrowsePart::DrawHead(CWindowGc& aGc, const TRect& aRect) const
	{
	TInt x0 = aRect.iTl.iX, y0 = aRect.iTl.iY, x1 = aRect.iBr.iX - 1, y1 = aRect.iBr.iY;
	aGc.SetBrushColor(KRgbWhite);
	aGc.DrawRect(aRect);
	Rule(aGc, x0, x1 + 1, y0);
	VRule(aGc, x0, y0, y1);
	VRule(aGc, x1, y0, y1);
	const CFont* f = iData->iBold;
	Text(aGc, f, _L("Name"), iData->iNameX, iData->iSizeR - KBrowseSizeW, y0 + 1, CGraphicsContext::ELeft);
	Text(aGc, f, _L("Size"), iData->iSizeR - KBrowseSizeW, iData->iSizeR, y0 + 1, CGraphicsContext::ERight);
	Text(aGc, f, _L("Modified"), iData->iDateR - KBrowseDateW - 8, iData->iDateR, y0 + 1, CGraphicsContext::ERight);
	}

// The favourites first (at least one row: the folders or how to add one),
// leaving the folders down to this one at least three rows (all of them if
// they fit), then the foot
void CPtBrowsePart::Layout(TPtSideLayout& aL) const
	{
	TInt rows = (Rect().Height() - 3) / iData->iRowH - 1;
	aL.iRows = rows;
	TInt foot = rows - KBrowseFootRows;
	if (foot < 4)
		foot = 4;
	aL.iFootRow = foot;
	TInt levels = iData->iShown ? iData->Levels() + 1 : 0;
	TInt favs = iData->Favs();
	TInt least = levels < 3 ? levels : 3;
	TInt favShown = favs > 0 ? favs : 1;
	if (favShown > foot - 1 - least)
		favShown = foot - 1 - least;
	if (favShown < 1)
		favShown = 1;
	aL.iFavShown = favShown;
	aL.iTreeRow = 1 + favShown;
	TInt tree = foot - aL.iTreeRow;
	if (tree > levels)
		tree = levels;
	if (tree < 0)
		tree = 0;
	aL.iTreeRows = tree;
	aL.iFirst = levels - tree;
	}

// The Location pane: the server; Favourites, each by its last folder, with a
// star (the folder shown boxed); the folders from the top down to this one
// (this one boxed, with a star when it is a favourite); at the foot what is
// in this folder - or, with the keys in the pane on a favourite, its whole
// path. The keyboard's place, with the keys in the pane, is black.
void CPtBrowsePart::DrawSide(CWindowGc& aGc, const TRect& aRect) const
	{
	TInt x0 = aRect.iTl.iX, y0 = aRect.iTl.iY, w = aRect.Width(), h = aRect.Height();
	TInt x1 = x0 + w - 1;
	TInt rh = iData->iRowH;
	const CFont* f = CEikonEnv::Static()->DenseFont();
	aGc.SetBrushColor(KRgbWhite);
	aGc.DrawRect(aRect);
	Rule(aGc, x0, x1 + 1, y0);
	Rule(aGc, x0, x1 + 1, y0 + h - 1);
	VRule(aGc, x0, y0, y0 + h);
	VRule(aGc, x1, y0, y0 + h);
	// the server, as the pane's heading
	TBuf<64> host(iData->iHost);
	TextUtils::ClipToFit(host, *iData->iBold, w - 28);
	Picture(aGc, EBrPicHost, x0 + 4, y0 + 1);
	Text(aGc, iData->iBold, host, x0 + 23, x1 - 2, y0 + 1, CGraphicsContext::ELeft);
	Rule(aGc, x0, x1, y0 + 1 + rh);
	TPtSideLayout l;
	Layout(l);
	TBool keys = iData->iPaneFocus;

	// Favourites
	TInt fy = RowTop(0);
	TInt favs = iData->Favs();
	Text(aGc, iData->iBold, _L("Favourites"), x0 + 6, x1 - 2, fy, CGraphicsContext::ELeft);
	if (favs > l.iFavShown)
		{
		// (a tap here shows the next ones)
		TInt last = iData->iFavTop + l.iFavShown;
		if (last > favs) last = favs;
		TBuf<24> which;
		which.Format(_L("%d-%d of %d"), iData->iFavTop + 1, last, favs);
		Text(aGc, f, which, x0 + 6, x1 - 4, fy, CGraphicsContext::ERight, KRgbDarkGray);
		}
	if (favs == 0)
		Text(aGc, f, _L("Ctrl+F adds this folder"), x0 + 6, x1 - 2, fy + rh, CGraphicsContext::ELeft, KRgbDarkGray);
	for (TInt k = 0; k < l.iFavShown; k++)
		{
		TInt i = iData->iFavTop + k;
		if (i >= favs)
			break;
		TInt y = RowTop(1 + k);
		TBuf<80> name;
		TPtrC nm((*iData->iFavNames)[i]);
		name.Copy(nm.Left(nm.Length() < 80 ? nm.Length() : 80));
		TInt tx = x0 + 23;
		TextUtils::ClipToFit(name, *f, x1 - 4 - tx);
		TBool sel = keys && iData->iPaneSel == i;
		if (sel)
			Highlight(aGc, x0 + 2, x1 - 1, y);
		Picture(aGc, EBrPicStar, x0 + 4, y);
		Text(aGc, f, name, tx, x1 - 2, y, CGraphicsContext::ELeft, sel ? KRgbWhite : KRgbBlack);
		if (i == iData->iFavHere && !sel)
			Box(aGc, tx - 2, tx + f->TextWidthInPixels(name) + 3, y);
		}

	// the folders: level 0 is "/", the last is this folder
	TInt n = iData->Levels();
	if (l.iTreeRows > 0)
		Rule(aGc, x0, x1, RowTop(l.iTreeRow) - 1);
	for (TInt t = 0; t < l.iTreeRows; t++)
		{
		TInt lev = l.iFirst + t;
		if (lev > n)
			break;
		TInt y = RowTop(l.iTreeRow + t);
		TInt ix = x0 + 4 + (t < 6 ? t : 6) * 10;
		if (t > 0 && t <= 6)
			{
			// "L" from the folder above
			TInt cx = ix - 10 + 5;
			aGc.SetPenStyle(CGraphicsContext::ESolidPen);
			aGc.SetPenColor(KRgbBlack);
			aGc.DrawLine(TPoint(cx, y - rh + (rh - 14) / 2 + 14), TPoint(cx, y + rh / 2));
			aGc.DrawLine(TPoint(cx, y + rh / 2), TPoint(ix, y + rh / 2));
			}
		TBool here = (lev == n);
		TBool star = here && iData->iFavHere >= 0;
		TBuf<80> name;
		if (lev == 0)
			name.Copy(_L("/"));
		else
			{ TPtrC nm((*iData->iNames)[lev - 1]); name.Copy(nm.Left(nm.Length() < 80 ? nm.Length() : 80)); }
		TInt tx = ix + 19;
		TextUtils::ClipToFit(name, *f, x1 - 4 - tx - (star ? 18 : 0));
		TBool sel = keys && iData->iPaneSel == KPaneLevel + lev;
		if (sel)
			Highlight(aGc, tx - 2, x1 - 1, y);
		Picture(aGc, here ? EBrPicOpen : EBrPicFolder, ix, y);
		Text(aGc, f, name, tx, x1 - 2, y, CGraphicsContext::ELeft, sel ? KRgbWhite : KRgbBlack);
		if (here && !sel)
			Box(aGc, tx - 2, tx + f->TextWidthInPixels(name) + 3, y);
		if (star)
			Picture(aGc, EBrPicStar, tx + f->TextWidthInPixels(name) + 6, y);
		}

	// the foot
	TInt ft = RowTop(l.iFootRow);
	Rule(aGc, x0, x1, ft - 1);
	if (keys && iData->iPaneSel >= 0 && iData->iPaneSel < favs)
		{
		TBuf<80> shown;
		FitTail((*iData->iFavs)[iData->iPaneSel], *f, x1 - 4 - (x0 + 6), shown);
		Text(aGc, f, shown, x0 + 6, x1 - 2, ft, CGraphicsContext::ELeft);
		Text(aGc, f, _L("Del removes it"), x0 + 6, x1 - 2, ft + rh, CGraphicsContext::ELeft, KRgbDarkGray);
		return;
		}
	if (!iData->iShown)
		return;
	TBuf<40> line;
	TBuf<20> sz;
	line.Format(iData->iFolders == 1 ? _L("%d folder") : _L("%d folders"), iData->iFolders);
	Text(aGc, f, line, x0 + 6, x1 - 2, ft, CGraphicsContext::ELeft);
	ShortSize(iData->iTotal, sz);
	if (iData->iFiles == 0)
		line.Copy(_L("No files"));
	else
		line.Format(iData->iFiles == 1 ? _L("%d file, %S") : _L("%d files, %S"), iData->iFiles, &sz);
	Text(aGc, f, line, x0 + 6, x1 - 2, ft + rh, CGraphicsContext::ELeft);
	}

// One tap on a favourite goes there; on one of the folders above this one,
// goes back to it; on the Favourites heading, shows the next favourites
// when they do not all fit
void CPtBrowsePart::Tap(const TPoint& aPos)
	{
	if (iKind != 1 || !iData || !iData->iObs)
		return;
	TInt top = RowTop(0);
	if (aPos.iY < top)
		return;                                   // the server's row
	TInt row = (aPos.iY - top) / iData->iRowH;
	TPtSideLayout l;
	Layout(l);
	TInt favs = iData->Favs();
	if (row == 0)
		{
		if (favs > l.iFavShown)
			iData->iObs->BrowseFavPage();
		return;
		}
	if (row <= l.iFavShown)
		{
		TInt i = iData->iFavTop + row - 1;
		if (i < favs)
			iData->iObs->BrowseGoToFav(i);
		return;
		}
	if (row >= l.iTreeRow && row < l.iTreeRow + l.iTreeRows)
		{
		TInt level = l.iFirst + row - l.iTreeRow;
		if (level < iData->Levels())
			iData->iObs->BrowseGoToLevel(level);
		}
	}

// File > Go to folder... (Ctrl+G): a folder typed
class CPtGotoDialog : public CEikDialog
	{
public:
	CPtGotoDialog(TDes& aText) : iText(aText) {}
private:
	void PreLayoutDynInitL()
		{
		SetEdwinTextL(EPtDlgGotoPath, &iText);
		}
	TBool OkToExitL(TInt /*aButtonId*/)
		{
		GetEdwinText(iText, EPtDlgGotoPath);
		iText.Trim();
		if (iText.Length() == 0)
			{
			iEikonEnv->InfoMsg(_L("No folder entered"));
			return EFalse;
			}
		return ETrue;
		}
	TDes& iText;
	};

// a list's scroll bar: there when its rows don't all fit (as PsiMail's)
static void ListScrollBar(CEikListBox* aList)
	{
	CEikScrollBarFrame* frame = aList->ScrollBarFrame();
	if (!frame)
		return;
	TBool need = aList->Model()->NumberOfItems() * aList->ItemHeight() > aList->Rect().Height();
	CEikScrollBar* sb = frame->GetScrollBarHandle(CEikScrollBar::EVertical);
	if (sb && sb->IsVisible() != need)
		sb->MakeVisible(need);
	if (need)
		{
		TRAPD(err, aList->UpdateScrollBarsL());
		(void)err;
		}
	}

// ----- the file view ---------------------------------------------------------

class CPtBrowser : public CPtBrowseView, public MEikListBoxObserver, public MPtBrowseObserver
	{
public:
	CPtBrowser(CTermView& aView, TBool aSend, MPtBrowseHost& aHost) : iView(aView), iSend(aSend), iHost(aHost) {}
	~CPtBrowser();
	void ConstructL(const TRect& aRect);
	// CPtBrowseView: from the app UI
	void CommandL(TInt aCommand);
	void InitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane);
	void SetViewRectL(const TRect& aRect);
	void StartL();
	TBool Sending() const { return iSend; }
private:
	// CCoeControl
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void HandlePointerEventL(const TPointerEvent& aPointerEvent);
	TInt CountComponentControls() const { return 3; }
	CCoeControl* ComponentControl(TInt aIndex) const;
	void SizeChanged();
	void Draw(const TRect& aRect) const;
	// the list's second tap
	void HandleListBoxEventL(CEikListBox* aListBox, TListBoxEvent aEventType);
	// the Location pane's taps
	void BrowseGoToLevel(TInt aLevel);
	void BrowseGoToFav(TInt aIndex);
	void BrowseFavPage();

	void LayoutL();
	void LoadPicturesL();
	void LoadFavouritesL();
	void FillRowsL(TInt aKeep);
	void DoFillRowsL(TInt aKeep);
	void ShowFolderL();
	void DrawTitle(CWindowGc& aGc) const;
	TBool AtTop() const { return iDir.Length() <= 1; }
	TInt Row() const;              // -1 = the ".." row, -2 = none, else the entry
	void JumpToLetter(TInt aChar);

	// the listing, in place
	TBool Busy() const { return iOp != 0; }
	TBool NotNow();                // ETrue (and says so) while a folder is being opened
	void List(const TDesC8& aDir);
	void Post(TInt aOp, const TDesC8& aRemote);
	void SetTimer(TBool aFast);
	static TInt TickCallback(TAny* aSelf);
	void Tick();
	void ListDoneL(TInt aResult, PsiShared* aShared);
	void StatDone(TInt aResult, PsiShared* aShared);
	void StopListing();
	void Finish(TInt aResult);

	// what the commands and keys do
	void OpenL();
	void UpL();
	void GotoL();
	void ToggleFavouriteL();
	void GoFavL(TInt aIndex);
	void RemoveFavL(TInt aIndex);
	void ShowFav(TInt aIndex);
	void SetPaneFocus(TBool aOn);
	void PaneMove(TInt aStep);
	void PaneActL();

	CTermView& iView;
	TBool iSend;
	MPtBrowseHost& iHost;
	TBuf8<200> iServer;            // "user@host:port", whose favourites these are
	TBuf8<512> iDir;               // the folder shown
	TBuf8<512> iWant;              // the folder being listed
	TBuf8<512> iPath;              // an entry's full name
	TBuf8<512> iStatPath;          // the link being looked at
	TBuf8<256> iSelect;            // the folder to highlight once listed (where Up came from)
	CRemoteList* iList;            // what the folder shown holds
	TBool iMore;                   // ...only its first part (it did not all fit)
	CPtBrowseData iData;
	CPtBrowseList* iListCtl;
	CPtBrowsePart* iHead;
	CPtBrowsePart* iSide;
	CArrayPtrFlat<CFbsBitmap>* iListPics;   // the list's pictures (the same bitmaps; not owned)
	CFont* iBoldFont;              // (released when the view goes)
	CFont* iTitleFont;
	TInt iTitleH;
	TInt iNameW;
	CPeriodic* iTimer;
	TInt iOp;                      // PSI_XOP_LIST or _STAT while psissh does it; 0 otherwise
	TUint iReq;
	TBool iStopping;               // Esc: psissh has been asked to stop it
	TBool iFinishing;              // the view has finished: it closes as soon as the app UI can
	};

CPtBrowser::~CPtBrowser()
	{
	if (iTimer)
		iTimer->Cancel();
	delete iTimer;
	if (iOp)
		{
		// (the program closing mid-listing: psissh is told to stop it)
		PsiShared* s = iView.XferShared();
		if (s)
			s->xfer_cancel = 1;
		iView.RingSsh();
		iEikonEnv->BusyMsgCancel();
		}
	// the list is told to forget the pictures, which are deleted with iData
	if (iListCtl && iListPics && iListCtl->Model() && iListCtl->Model()->ColumnData())
		iListCtl->Model()->ColumnData()->SetBitmapArray(NULL);
	delete iListPics;
	delete iListCtl;
	delete iHead;
	delete iSide;
	delete iList;
	if (iBoldFont)
		iCoeEnv->ReleaseScreenFont(iBoldFont);
	if (iTitleFont)
		iCoeEnv->ScreenDevice()->ReleaseFont(iTitleFont);
	}

// the pictures from the program's MBM; without them the rows have no picture
void CPtBrowser::LoadPicturesL()
	{
	TFileName mbm = CEikonEnv::Static()->EikAppUi()->Application()->BitmapStoreName();
	iData.iIcons = new(ELeave) CArrayPtrFlat<CFbsBitmap>(2 * KBrowseIcons + 2);
	for (TInt i = 0; i < 2 * KBrowseIcons + 2; i++)
		{
		TInt id = i < 2 * KBrowseIcons ? EMbmSmFolder + i : EMbmSmStar + i - 2 * KBrowseIcons;
		CFbsBitmap* b = iEikonEnv->CreateBitmapL(mbm, id);
		CleanupStack::PushL(b);
		iData.iIcons->AppendL(b);
		CleanupStack::Pop();
		}
	}

// this server's favourites, for the Location pane: whole, and by their last folder
void CPtBrowser::LoadFavouritesL()
	{
	CDesC8ArrayFlat* folders = new(ELeave) CDesC8ArrayFlat(8);
	CleanupStack::PushL(folders);
	FavMineL(iEikonEnv->FsSession(), iServer, *folders);
	CDesCArrayFlat* favs = new(ELeave) CDesCArrayFlat(8);
	CleanupStack::PushL(favs);
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(8);
	CleanupStack::PushL(names);
	TInt here = -1;
	TBuf<512>& text = Mem(iView).iText;
	for (TInt i = 0; i < folders->Count(); i++)
		{
		TPtrC8 folder((*folders)[i]);
		Utf8ToText(folder.Left(folder.Length() < 500 ? folder.Length() : 500), text, EFalse);
		favs->AppendL(text);
		TInt end = text.Length();
		while (end > 1 && text[end - 1] == '/')
			end--;
		TPtrC whole(text.Left(end));
		TInt slash = whole.LocateReverse('/');
		names->AppendL(slash >= 0 && whole.Length() > 1 ? whole.Mid(slash + 1) : whole);
		if (iData.iShown && folder == iDir)
			here = i;
		}
	CleanupStack::Pop(2);                   // names, favs
	delete iData.iFavs;
	iData.iFavs = favs;
	delete iData.iFavNames;
	iData.iFavNames = names;
	iData.iFavHere = here;
	if (iData.iFavTop >= favs->Count())
		iData.iFavTop = 0;
	if (iData.iPaneSel >= 0 && iData.iPaneSel < KPaneLevel && iData.iPaneSel >= favs->Count())
		iData.iPaneSel = favs->Count() > 0 ? favs->Count() - 1 : KPaneLevel + iData.Levels();
	CleanupStack::PopAndDestroy();          // folders
	}

void CPtBrowser::ConstructL(const TRect& aRect)
	{
	CreateWindowL();
	FavServer(iView, iServer);
	const TPsiSettings& st = iView.Settings();
	iData.iHost.Copy(st.iSshHost.Left(st.iSshHost.Length() < iData.iHost.MaxLength() ? st.iSshHost.Length()
		: iData.iHost.MaxLength()));
	iData.iObs = this;
	TRAPD(perr, LoadPicturesL());
	if (perr != KErrNone && iData.iIcons)
		{
		iData.iIcons->ResetAndDestroy();
		delete iData.iIcons;
		iData.iIcons = NULL;
		}
	TRAP_IGNORE(LoadFavouritesL());
	// the headings and the server in bold; the title band as PsiMail's
	iData.iBold = iEikonEnv->DenseFont();
	TFontSpec spec = iEikonEnv->DenseFont()->FontSpecInTwips();
	spec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	TRAPD(ferr, iBoldFont = iCoeEnv->CreateScreenFontL(spec));
	if (ferr == KErrNone && iBoldFont)
		iData.iBold = iBoldFont;
	TFontSpec tspec(_L("Arial"), 160);
	tspec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	if (iCoeEnv->ScreenDevice()->GetNearestFontInTwips(iTitleFont, tspec) != KErrNone)
		iTitleFont = NULL;

	iHead = new(ELeave) CPtBrowsePart;
	iHead->SetContainerWindowL(*this);
	iHead->Set(0, &iData);
	iSide = new(ELeave) CPtBrowsePart;
	iSide->SetContainerWindowL(*this);
	iSide->Set(1, &iData);

	iListCtl = new(ELeave) CPtBrowseList;
	iListCtl->ConstructL(this, 0);
	iListCtl->NoMargins();
	iListCtl->SetBorder(TEikBorder::ESingleGray);
	CDesCArrayFlat* rows = new(ELeave) CDesCArrayFlat(32);
	iListCtl->Model()->SetItemTextArray(rows);
	iListCtl->Model()->SetOwnershipType(ELbmOwnsItemArray);
	iListCtl->CreateScrollBarFrameL();
	iListCtl->ScrollBarFrame()->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);
	iListCtl->SetListBoxObserver(this);
	CColumnListBoxData* cd = iListCtl->Model()->ColumnData();
	for (TInt c = 0; c < 5; c++)
		cd->SetColumnFontL(c, iEikonEnv->DenseFont());
	cd->SetColumnAlignmentL(1, CGraphicsContext::ELeft);
	cd->SetColumnAlignmentL(2, CGraphicsContext::ERight);
	cd->SetColumnAlignmentL(3, CGraphicsContext::ERight);
	if (iData.iIcons)
		{
		// folder, file, up: the same bitmaps as the pane's
		iListPics = new(ELeave) CArrayPtrFlat<CFbsBitmap>(3);
		iListPics->AppendL(iData.iIcons->At(EBrPicFolder * 2));
		iListPics->AppendL(iData.iIcons->At(EBrPicFile * 2));
		iListPics->AppendL(iData.iIcons->At(EBrPicUp * 2));
		cd->SetGraphicsColumnL(0, ETrue);
		cd->SetBitmapArray(iListPics);
		}
	iListCtl->SetFocus(ETrue);
	iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
	SetRectL(aRect);
	LayoutL();                               // (SetRectL does not always call SizeChanged here)
	}

CCoeControl* CPtBrowser::ComponentControl(TInt aIndex) const
	{
	switch (aIndex)
		{
	case 0: return iHead;
	case 1: return iSide;
	default: return iListCtl;
		}
	}

void CPtBrowser::SizeChanged()
	{
	if (iListCtl)
		TRAP_IGNORE(LayoutL());
	}

void CPtBrowser::SetViewRectL(const TRect& aRect)
	{
	SetRectL(aRect);
	LayoutL();
	DrawNow();
	}

// Under the title band: the Location pane and the list's frame side by side,
// as tall as whole rows allow (the row height chosen so they fill the height)
void CPtBrowser::LayoutL()
	{
	TRect r = Rect();
	iTitleH = (iTitleFont ? iTitleFont->HeightInPixels() : 13) + 6;
	TInt y0 = r.iTl.iY + iTitleH + KBrowseGap - 1;
	TInt avail = r.iBr.iY - KBrowseGap + 1 - y0;
	// pane = frame + heading row + rule + rows + frame: 3 + (rows + 1) * rowH
	TInt best = 15, bestRows = (avail - 3) / 15 - 1, bestLeft = avail;
	for (TInt rh = 15; rh <= 17; rh++)
		{
		TInt rows = (avail - 3) / rh - 1;
		TInt waste = avail - 3 - (rows + 1) * rh;
		if (waste < bestLeft)
			{
			best = rh;
			bestRows = rows;
			bestLeft = waste;
			}
		}
	iData.iRowH = best;
	TInt paneH = 3 + (bestRows + 1) * best;
	y0 += bestLeft / 2;                      // (what whole rows leave: above and below)
	TInt sx = r.iTl.iX + KBrowseGap;
	TInt lx = sx + KBrowseSideW + KBrowseGap;
	TInt rx = r.iBr.iX - KBrowseGap;
	iSide->SetRectL(TRect(sx, y0, sx + KBrowseSideW, y0 + paneH));
	iHead->SetRectL(TRect(lx, y0, rx, y0 + 1 + best));
	iListCtl->SetRectL(TRect(lx, y0 + 1 + best, rx, y0 + paneH));
	iListCtl->SetItemHeightL(best);

	// the columns: picture, Name, Size, Modified, then room for the scroll bar
	TInt inner = rx - lx - 2;
	iNameW = inner - KBrowseIconW - KBrowseSizeW - KBrowseDateW - KBrowseBar;
	CColumnListBoxData* cd = iListCtl->Model()->ColumnData();
	cd->SetColumnWidthPixelL(0, KBrowseIconW);
	cd->SetColumnWidthPixelL(1, iNameW);
	cd->SetColumnWidthPixelL(2, KBrowseSizeW);
	cd->SetColumnWidthPixelL(3, KBrowseDateW);
	cd->SetColumnWidthPixelL(4, KBrowseBar);
	TInt cx = lx + 1;
	iData.iNameX = cx + KBrowseIconW;
	iData.iSizeR = iData.iNameX + iNameW + KBrowseSizeW;
	iData.iDateR = iData.iSizeR + KBrowseDateW;
	if (iData.iShown)
		FillRowsL(iListCtl->CurrentItemIndex());   // (the names are cut to the column)
	}

// The title band, white with a black rule under it as PsiMail's (and the
// built-in programs'): what this is, and the end of the folder that fits
void CPtBrowser::DrawTitle(CWindowGc& aGc) const
	{
	TRect r = Rect();
	TRect band(r.iTl, TSize(r.Width(), iTitleH));
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(KRgbBlack);
	aGc.DrawRect(TRect(band.iTl.iX, band.iBr.iY - 1, band.iBr.iX, band.iBr.iY));
	const CFont* tf = iTitleFont ? iTitleFont : iEikonEnv->TitleFont();
	TBuf<200> title;
	if (iSend)
		{
		TBuf<40> leaf;
		const TDesC& from = iView.iXferMem ? (const TDesC&)iView.iXferMem->iSendFrom : (const TDesC&)KNullDesC;
		Tail(TParsePtrC(from).NameAndExt(), leaf, 24);
		title.Format(_L("Send \"%S\" to "), &leaf);
		}
	else
		title.Copy(_L("Get file - "));
	TBuf<30> more;
	if (iMore)
		more.Copy(_L(" (first part)"));
	if (iData.iShown && iData.iPath)
		{
		TBuf<120> shown;
		FitTail(*iData.iPath, *tf, band.Width() - 16 - tf->TextWidthInPixels(title) - tf->TextWidthInPixels(more), shown);
		title.Append(shown);
		title.Append(more);
		}
	else
		title.SetLength(title.Length() - (iSend ? 4 : 3));   // ("Send "x" to" / "Get file" alone at first)
	aGc.UseFont(tf);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(KRgbBlack);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	TInt base = band.iTl.iY + (iTitleH - 1 - tf->HeightInPixels()) / 2 + tf->AscentInPixels();
	aGc.DrawText(title, TPoint(band.iTl.iX + 6, base));
	aGc.DiscardFont();
	}

void CPtBrowser::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	// only round the panes, which draw themselves (all white first, then
	// the panes, showed as a flash on the 5mx's slow screen)
	TRect r = Rect();
	TRect side = iSide->Rect(), list = iListCtl->Rect();
	gc.DrawRect(TRect(r.iTl.iX, r.iTl.iY, r.iBr.iX, side.iTl.iY));               // the title band and above
	gc.DrawRect(TRect(r.iTl.iX, side.iBr.iY, r.iBr.iX, r.iBr.iY));               // below
	gc.DrawRect(TRect(r.iTl.iX, side.iTl.iY, side.iTl.iX, side.iBr.iY));         // left
	gc.DrawRect(TRect(side.iBr.iX, side.iTl.iY, list.iTl.iX, side.iBr.iY));      // between
	gc.DrawRect(TRect(list.iBr.iX, side.iTl.iY, r.iBr.iX, side.iBr.iY));         // right
	DrawTitle(gc);
	}

// the list's rows: picture, name ("..." where it will not fit), size, date.
// aKeep: the row to stay on (-1: the one Up came from, or the first entry)
void CPtBrowser::FillRowsL(TInt aKeep)
	{
	// (the list does not draw itself meanwhile - an empty list flashed up -
	// the caller draws the whole view once)
	iListCtl->View()->SetDisableRedraw(ETrue);
	TRAPD(err, DoFillRowsL(aKeep));
	iListCtl->View()->SetDisableRedraw(EFalse);
	User::LeaveIfError(err);
	}

void CPtBrowser::DoFillRowsL(TInt aKeep)
	{
	CDesCArray* rows = (CDesCArray*)iListCtl->Model()->ItemTextArray();
	rows->Reset();
	iListCtl->Reset();
	iData.iFolders = 0;
	iData.iFiles = 0;
	iData.iTotal = 0;
	if (iData.iShown && iList)
		{
		const CFont* font = iEikonEnv->DenseFont();
		if (!AtTop())
			rows->AppendL(iData.iIcons ? _L("2\t.. (up a folder)\t\t\t") : _L("\t.. (up a folder)\t\t\t"));
		TBuf<200> row;
		TBuf<120> name;
		TBuf<20> size;
		TBuf<24> date;
		for (TInt i = 0; i < iList->Count(); i++)
			{
			const TRemoteEntry& e = iList->At(i);
			Utf8ToText(iList->Name(i), name, EFalse);
			if (name.Length() > 100)
				name.SetLength(100);
			size.Zero();
			if (e.iType == 'd')
				iData.iFolders++;               // (a folder: no size, as EIKON's browser)
			else if (e.iType == 'l')
				size.Copy(_L("Link"));
			else
				{
				ListSize(e.iSize, size);
				iData.iFiles++;
				iData.iTotal += e.iSize;
				}
			TextUtils::ClipToFit(name, *font, iNameW - 6);     // "..." where it will not fit
			DateText(e.iTime, date);
			if (iData.iIcons)
				row.Copy(e.iType == 'd' ? _L("0\t") : _L("1\t"));
			else
				row.Copy(_L("\t"));
			row.Append(name);
			row.Append('\t');
			row.Append(size);
			row.Append('\t');
			row.Append(date);
			row.Append('\t');
			rows->AppendL(row);
			}
		}
	iListCtl->HandleItemAdditionL();
	TInt n = rows->Count();
	TInt cur = aKeep;
	if (cur < 0)
		{
		// the first entry, not ".."; going up, the folder it came from
		cur = (!AtTop() && n > 1) ? 1 : 0;
		if (iSelect.Length() && iList)
			for (TInt i = 0; i < iList->Count(); i++)
				if (iList->Name(i) == iSelect)
					{
					cur = i + (AtTop() ? 0 : 1);
					break;
					}
		}
	if (cur >= n)
		cur = n - 1;
	if (n > 0)
		{
		iListCtl->SetCurrentItemIndex(cur);
		iListCtl->ScrollToMakeItemVisible(cur);
		}
	ListScrollBar(iListCtl);
	}

// A listing has arrived: the list, the panes and the title, all at once
void CPtBrowser::ShowFolderL()
	{
	iData.SetPathL(iDir);
	iData.iShown = ETrue;
	TRAP_IGNORE(LoadFavouritesL());
	if (iData.iPaneSel >= KPaneLevel)
		iData.iPaneSel = KPaneLevel + iData.Levels();   // (with the keys in the pane: this folder)
	FillRowsL(-1);
	if (iData.iFavHere >= 0)
		ShowFav(iData.iFavHere);
	DrawNow();
	iHost.BrowseFavouriteShown(iData.iFavHere >= 0);
	}

TInt CPtBrowser::Row() const
	{
	if (!iData.iShown || !iList || iListCtl->Model()->NumberOfItems() == 0)
		return -2;
	TInt cur = iListCtl->CurrentItemIndex();
	if (cur < 0)
		return -2;
	if (!AtTop())
		cur--;
	if (cur >= iList->Count())
		return -2;
	return cur;                       // -1: the ".." row
	}

// a letter: on to the next name that starts with it (wrapping round)
void CPtBrowser::JumpToLetter(TInt aChar)
	{
	TInt n = iListCtl->Model()->NumberOfItems();
	TInt cur = iListCtl->CurrentItemIndex();
	TChar want(aChar);
	want.Fold();
	for (TInt k = 1; k <= n; k++)
		{
		TInt i = (cur + k) % n;
		if (!AtTop() && i == 0)
			continue;
		TPtrC t = iListCtl->Model()->ItemText(i);
		TInt tab = t.Locate('\t');            // (the picture's column comes first)
		TChar first(tab >= 0 && tab + 1 < t.Length() ? t[tab + 1] : 0);
		first.Fold();
		if (tab >= 0 && tab + 1 < t.Length() && (TUint)first == (TUint)want)
			{
			iListCtl->SetCurrentItemIndex(i);
			iListCtl->ScrollToMakeItemVisible(i);
			iListCtl->DrawNow();
			return;
			}
		}
	}

// ----- listing in place ---------------------------------------------------------

TBool CPtBrowser::NotNow()
	{
	if (iFinishing)
		return ETrue;
	if (!Busy() && iData.iShown)
		return EFalse;
	iEikonEnv->InfoMsg(_L("Not available while opening a folder"));
	return ETrue;
	}

void CPtBrowser::SetTimer(TBool aFast)
	{
	iTimer->Cancel();
	TInt us = aFast ? 100000 : 1000000;    // (idle: only to notice the SSH session going)
	iTimer->Start(us, us, TCallBack(TickCallback, this));
	}

TInt CPtBrowser::TickCallback(TAny* aSelf)
	{
	((CPtBrowser*)aSelf)->Tick();
	return 0;
	}

// Lists aDir ("" = the home folder) for the view to show when it comes.
// Going up, or back along the path, the folder it came from is highlighted.
void CPtBrowser::List(const TDesC8& aDir)
	{
	iSelect.Zero();
	TInt n = aDir.Length();
	if (iData.iShown && n > 0 && iDir.Length() > n && iDir.Left(n) == aDir)
		{
		TInt start = n;
		if (aDir[n - 1] != '/')
			start = iDir[n] == '/' ? n + 1 : -1;
		if (start > 0)
			{
			TPtrC8 rest(iDir.Mid(start));
			TInt slash = rest.Locate('/');
			TPtrC8 child(slash < 0 ? rest : rest.Left(slash));
			iSelect.Copy(child.Left(child.Length() < iSelect.MaxLength() ? child.Length() : iSelect.MaxLength()));
			}
		}
	iWant.Copy(aDir.Left(n < iWant.MaxLength() ? n : iWant.MaxLength()));
	Post(PSI_XOP_LIST, iWant);
	}

// Posts a LIST or STAT to psissh, and waits for it here, ticking: the view
// stays as it is meanwhile, with a busy message if it takes a moment
void CPtBrowser::Post(TInt aOp, const TDesC8& aRemote)
	{
	TUint req = PostXfer(iView, aOp, aRemote, KNullDesC);
	if (!req)
		{
		XferFailed(PSI_XFER_LINK, iView);
		Finish(EPtBrowseNone);
		return;
		}
	iReq = req;
	iOp = aOp;
	iStopping = EFalse;
	TRAP_IGNORE(iEikonEnv->BusyMsgL(aOp == PSI_XOP_STAT ? _L("Looking at the link...") : _L("Opening folder..."),
		EHLeftVBottom, TTimeIntervalMicroSeconds32(300000)));   // (not for a quick one)
	SetTimer(ETrue);
	}

// Esc while a folder is being opened: stop that, and stay in the folder shown
void CPtBrowser::StopListing()
	{
	if (!Busy() || iStopping)
		return;
	PsiShared* s = iView.XferShared();
	if (s)
		s->xfer_cancel = 1;
	iView.RingSsh();
	iStopping = ETrue;
	iEikonEnv->BusyMsgCancel();
	}

// As RunXferL's progress window: the answer, or the SSH connection gone
void CPtBrowser::Tick()
	{
	if (iFinishing)
		return;
	PsiShared* s = iView.XferShared();
	if (!Busy())
		{
		if (!s)
			{
			iEikonEnv->InfoMsg(_L("The SSH connection has gone"));
			Finish(EPtBrowseNone);
			}
		return;
		}
	if (s && s->xfer_ack != iReq)
		return;                            // (still working)
	TInt r = s ? s->xfer_result : PSI_XFER_LINK;
	TInt op = iOp;
	iOp = 0;
	iEikonEnv->BusyMsgCancel();
	SetTimer(EFalse);
	if (iStopping)
		{
		iStopping = EFalse;
		if (!s)
			{
			XferFailed(PSI_XFER_LINK, iView);
			Finish(EPtBrowseNone);
			}
		else if (!iData.iShown)
			Finish(EPtBrowseNone);         // (stopped before there was a folder to show)
		else
			iEikonEnv->InfoMsg(_L("Stopped"));
		return;
		}
	if (op == PSI_XOP_STAT)
		{
		StatDone(r, s);
		return;
		}
	TRAPD(err, ListDoneL(r, s));
	if (err != KErrNone)
		{
		iEikonEnv->HandleError(err);
		if (!iData.iShown)
			Finish(EPtBrowseNone);
		}
	}

void CPtBrowser::ListDoneL(TInt aResult, PsiShared* aShared)
	{
	if (aResult == PSI_XFER_OK && aShared)
		{
		CRemoteList* list = new(ELeave) CRemoteList;
		CleanupStack::PushL(list);
		TInt len = aShared->xfer_list_len;
		if (len < 0 || len > PSI_XFER_LIST_SIZE) len = 0;
		list->SetL(TPtrC8((const TUint8*)aShared->xfer_list, len));
		CleanupStack::Pop();
		delete iList;
		iList = list;
		TPtrC8 path((const TUint8*)aShared->xfer_path);
		iDir.Copy(path.Left(path.Length() < iDir.MaxLength() ? path.Length() : iDir.MaxLength()));
		iMore = aShared->xfer_list_more;
		Mem(iView).iRemoteDir = iDir;
		ShowFolderL();
		return;
		}
	XferFailed(aResult, iView);
	if (aResult == PSI_XFER_LINK || aResult == PSI_XFER_NO_SFTP || !aShared)
		{
		Finish(EPtBrowseNone);
		return;
		}
	if (!iData.iShown)
		{
		// the first listing: the folder used last has gone (or may not be
		// read) - the home folder instead; nothing at all - close
		if (iWant.Length() && aResult != PSI_XFER_TIMEOUT && aResult != PSI_XFER_CANCELLED)
			List(KNullDesC8);
		else
			Finish(EPtBrowseNone);
		}
	// (otherwise: still in the folder shown)
	}

// A link: a folder to open, or a file to get?
void CPtBrowser::StatDone(TInt aResult, PsiShared* aShared)
	{
	if (aResult != PSI_XFER_OK || !aShared)
		{
		XferFailed(aResult, iView);
		if (aResult == PSI_XFER_LINK || aResult == PSI_XFER_NO_SFTP || !aShared)
			Finish(EPtBrowseNone);
		return;
		}
	TInt exists = aShared->xfer_exists;
	if (exists == 0)
		iEikonEnv->InfoMsg(_L("That link points nowhere"));
	else if (exists == 2)
		List(iStatPath);
	else if (iSend)
		iEikonEnv->InfoMsg(_L("That is a file - Ctrl+S sends to this folder"));
	else
		{
		CPtXferMemory& mem = Mem(iView);
		mem.iPick = iStatPath;
		mem.iPickSize = aShared->xfer_total;
		Finish(EPtBrowseGet);
		}
	}

// Done: the choice is left for PtBrowseFinishedL, and the app UI closes the
// view (soon - not from inside it). A listing still going is stopped.
void CPtBrowser::Finish(TInt aResult)
	{
	if (iFinishing)
		return;
	iFinishing = ETrue;
	if (Busy())
		{
		PsiShared* s = iView.XferShared();
		if (s)
			s->xfer_cancel = 1;
		iView.RingSsh();
		}
	iTimer->Cancel();
	iEikonEnv->BusyMsgCancel();
	if (iView.iXferMem)
		iView.iXferMem->iBrowseResult = aResult;
	iHost.BrowseCloseSoon();
	}

// The first listing, once the view is on screen: the folder used last on
// this server (or its home folder)
void CPtBrowser::StartL()
	{
	CPtXferMemory& mem = Mem(iView);
	TBuf<100> host;
	host.Copy(iView.Settings().iSshHost);
	if (host.CompareF(mem.iRemoteHost) != 0)
		{
		mem.iRemoteHost = host;
		mem.iRemoteDir.Zero();           // another server: start at its home folder
		}
	SetTimer(EFalse);
	List(mem.iRemoteDir);
	}

// ----- what the commands and keys do ------------------------------------------

// Enter, Open, a second tap: a folder opens, ".." goes up, a link is looked
// at, a file is got (Get)
void CPtBrowser::OpenL()
	{
	if (NotNow())
		return;
	TInt row = Row();
	if (row == -1)
		{
		UpL();
		return;
		}
	if (row < 0)
		{
		iEikonEnv->InfoMsg(iSend ? _L("This folder is empty") : _L("Nothing to get - the folder is empty"));
		return;
		}
	if (!JoinDir(iDir, iList->Name(row), iPath))
		return;                          // (too long: it has said so)
	TInt type = iList->At(row).iType;
	if (type == 'd')
		List(iPath);
	else if (type == 'l')
		{
		iStatPath = iPath;
		Post(PSI_XOP_STAT, iStatPath);
		}
	else if (iSend)
		iEikonEnv->InfoMsg(_L("That is a file - Ctrl+S sends to this folder"));
	else
		{
		CPtXferMemory& mem = Mem(iView);
		mem.iPick = iPath;
		mem.iPickSize = iList->At(row).iSize;
		Finish(EPtBrowseGet);
		}
	}

void CPtBrowser::UpL()
	{
	if (NotNow())
		return;
	if (AtTop())
		{
		iEikonEnv->InfoMsg(_L("This is the top folder"));
		return;
		}
	iPath = iDir;
	ParentDir(iPath);
	List(iPath);
	}

// Go to folder...: "/..." from the top, "~" the home folder, anything else
// inside the folder shown
void CPtBrowser::GotoL()
	{
	if (NotNow())
		return;
	TBuf<512>& text = Mem(iView).iText;
	Utf8ToText(iDir, text, EFalse);
	CPtGotoDialog* dlg = new(ELeave) CPtGotoDialog(text);
	if (!dlg->ExecuteLD(R_PT_GOTO_DIALOG))
		return;
	if (iFinishing || Busy())
		return;                          // (the SSH session went meanwhile)
	if (text.Compare(_L("~")) == 0)
		{
		List(KNullDesC8);
		return;
		}
	iStatPath.Zero();                    // (as scratch)
	TextToUtf8(text, iStatPath);
	while (iStatPath.Length() > 1 && iStatPath[iStatPath.Length() - 1] == '/')
		iStatPath.SetLength(iStatPath.Length() - 1);
	if (iStatPath.Length() && iStatPath[0] == '/')
		List(iStatPath);
	else if (JoinDir(iDir, iStatPath, iPath))
		List(iPath);
	}

// Favourite folder (Ctrl+F, the toolbar's Favourite): the folder shown in
// this server's favourites, or out of them
void CPtBrowser::ToggleFavouriteL()
	{
	if (!iData.iShown)
		iEikonEnv->InfoMsg(_L("Not available while opening a folder"));
	else
		{
		FavToggleL(iEikonEnv->FsSession(), iServer, iDir);
		TRAP_IGNORE(LoadFavouritesL());
		if (iData.iFavHere >= 0)
			ShowFav(iData.iFavHere);
		iSide->DrawNow();
		}
	iHost.BrowseFavouriteShown(iData.iFavHere >= 0);    // (the button latched as it is)
	}

void CPtBrowser::GoFavL(TInt aIndex)
	{
	if (NotNow())
		return;
	CDesC8ArrayFlat* favs = new(ELeave) CDesC8ArrayFlat(8);
	CleanupStack::PushL(favs);
	FavMineL(iEikonEnv->FsSession(), iServer, *favs);
	if (aIndex >= 0 && aIndex < favs->Count())
		{
		TPtrC8 folder((*favs)[aIndex]);
		iPath.Copy(folder.Left(folder.Length() < iPath.MaxLength() ? folder.Length() : iPath.MaxLength()));
		List(iPath);
		}
	CleanupStack::PopAndDestroy();      // favs
	}

// Del on a favourite in the Location pane: out of the list, at once
void CPtBrowser::RemoveFavL(TInt aIndex)
	{
	CDesC8ArrayFlat* favs = new(ELeave) CDesC8ArrayFlat(8);
	CleanupStack::PushL(favs);
	FavMineL(iEikonEnv->FsSession(), iServer, *favs);
	if (aIndex >= 0 && aIndex < favs->Count())
		{
		HBufC8* gone = (*favs)[aIndex].AllocLC();
		FavToggleL(iEikonEnv->FsSession(), iServer, *gone);   // (it is there: so it comes out)
		CleanupStack::PopAndDestroy();   // gone
		}
	CleanupStack::PopAndDestroy();      // favs
	TRAP_IGNORE(LoadFavouritesL());
	if (iData.iPaneSel >= 0 && iData.iPaneSel < KPaneLevel)
		ShowFav(iData.iPaneSel);
	iSide->DrawNow();
	iHost.BrowseFavouriteShown(iData.iFavHere >= 0);
	}

// favourite aIndex among those the pane shows
void CPtBrowser::ShowFav(TInt aIndex)
	{
	TPtSideLayout l;
	iSide->Layout(l);
	if (aIndex < iData.iFavTop)
		iData.iFavTop = aIndex;
	else if (aIndex >= iData.iFavTop + l.iFavShown)
		iData.iFavTop = aIndex - l.iFavShown + 1;
	if (iData.iFavTop < 0)
		iData.iFavTop = 0;
	}

// Left arrow (or Tab) puts the keys in the Location pane, Right arrow (or
// Tab) back in the list; a tap in the list puts them back there too
void CPtBrowser::SetPaneFocus(TBool aOn)
	{
	if (iData.iPaneFocus == aOn)
		return;
	iData.iPaneFocus = aOn;
	if (aOn)
		{
		TInt favs = iData.Favs();
		TBool ok = (iData.iPaneSel >= 0 && iData.iPaneSel < favs)
			|| (iData.iPaneSel >= KPaneLevel && iData.iPaneSel <= KPaneLevel + iData.Levels());
		if (!ok)
			iData.iPaneSel = iData.iFavHere >= 0 ? iData.iFavHere : favs > 0 ? 0 : KPaneLevel + iData.Levels();
		if (iData.iPaneSel < KPaneLevel)
			ShowFav(iData.iPaneSel);
		}
	iListCtl->SetFocus(!aOn);
	iSide->DrawNow();
	iListCtl->DrawNow();
	}

// up and down the pane: the favourites, then the folders it shows
void CPtBrowser::PaneMove(TInt aStep)
	{
	TPtSideLayout l;
	iSide->Layout(l);
	TInt favs = iData.Favs();
	TInt total = favs + l.iTreeRows;
	if (total <= 0)
		return;
	TInt sel = iData.iPaneSel;
	TInt k = sel < KPaneLevel ? sel : favs + (sel - KPaneLevel - l.iFirst);
	k += aStep;
	if (k < 0) k = 0;
	if (k >= total) k = total - 1;
	iData.iPaneSel = k < favs ? k : KPaneLevel + l.iFirst + (k - favs);
	if (k < favs)
		ShowFav(k);
	iSide->DrawNow();
	}

// Enter in the pane: there
void CPtBrowser::PaneActL()
	{
	TInt sel = iData.iPaneSel;
	if (sel >= 0 && sel < iData.Favs())
		GoFavL(sel);
	else if (sel >= KPaneLevel && sel - KPaneLevel < iData.Levels())
		BrowseGoToLevel(sel - KPaneLevel);
	}

// a tap on a folder in the pane's path
void CPtBrowser::BrowseGoToLevel(TInt aLevel)
	{
	if (NotNow())
		return;
	iPath = iDir;
	TrimDirToLevel(iPath, aLevel);
	List(iPath);
	}

// a tap on a favourite in the pane: one tap goes there
void CPtBrowser::BrowseGoToFav(TInt aIndex)
	{
	TRAP_IGNORE(GoFavL(aIndex));
	}

// a tap on the Favourites heading, when they do not all fit: the next ones
void CPtBrowser::BrowseFavPage()
	{
	TPtSideLayout l;
	iSide->Layout(l);
	iData.iFavTop += l.iFavShown;
	if (iData.iFavTop >= iData.Favs())
		iData.iFavTop = 0;
	iSide->DrawNow();
	}

void CPtBrowser::CommandL(TInt aCommand)
	{
	if (iFinishing)
		return;
	if (aCommand >= EPtCmdBrFav0 && aCommand < EPtCmdBrFav0 + KMenuFavs)
		{
		GoFavL(aCommand - EPtCmdBrFav0);
		return;
		}
	switch (aCommand)
		{
	case EPtCmdBrOpen:
		if (iData.iPaneFocus)
			PaneActL();
		else
			OpenL();
		break;
	case EPtCmdBrAct:                    // Get file... (as Enter) / Send here
		if (!iSend)
			OpenL();
		else if (!NotNow())
			{
			Mem(iView).iPick = iDir;
			Finish(EPtBrowseSendHere);
			}
		break;
	case EPtCmdBrUp:
		UpL();
		break;
	case EPtCmdBrGoto:
		GotoL();
		break;
	case EPtCmdBrFavourite:
		ToggleFavouriteL();
		break;
	case EPtCmdBrClose:
		Finish(EPtBrowseNone);
		break;
	default:
		break;
		}
	}

// The file view's own menus. (Only items in the pane being opened are dimmed
// or ticked: anything else panics EIKON 8.)
void CPtBrowser::InitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	TBool ready = iData.iShown && !Busy();
	if (aMenuId == R_PT_BROWSE_GET_FILE_MENU || aMenuId == R_PT_BROWSE_SEND_FILE_MENU)
		{
		aMenuPane->SetItemDimmed(EPtCmdBrOpen, !ready);
		aMenuPane->SetItemDimmed(EPtCmdBrAct, !ready);
		aMenuPane->SetItemDimmed(EPtCmdBrUp, !ready || AtTop());
		aMenuPane->SetItemDimmed(EPtCmdBrGoto, !ready);
		return;
		}
	if (aMenuId != R_PT_BROWSE_FAV_MENU)
		return;
	aMenuPane->SetItemButtonState(EPtCmdBrFavourite, iData.iFavHere >= 0 ? EEikMenuItemSymbolOn : 0);
	aMenuPane->SetItemDimmed(EPtCmdBrFavourite, !iData.iShown);
	TInt favs = iData.Favs();
	if (favs > 0)
		aMenuPane->ItemData(EPtCmdBrFavourite).iFlags |= EEikMenuItemSeparatorAfter;
	for (TInt i = 0; i < favs && i < KMenuFavs; i++)
		{
		CEikMenuPane::TItem::SData item;
		item.iCommandId = EPtCmdBrFav0 + i;
		item.iCascadeId = 0;
		item.iFlags = 0;
		TPtrC name((*iData.iFavNames)[i]);
		item.iText = name.Left(name.Length() < 30 ? name.Length() : 30);
		item.iExtraText.Zero();
		aMenuPane->AddMenuItemL(item);
		if (!ready || i == iData.iFavHere)
			aMenuPane->SetItemDimmed(EPtCmdBrFav0 + i, ETrue);
		}
	}

// Keys. Esc closes the view (or, while a folder is being opened, stops
// that); the Ctrl+ shortcuts arrive as commands (the menu bar's hotkeys).
TKeyResponse CPtBrowser::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKey)
		return EKeyWasNotConsumed;
	if (iFinishing)
		return EKeyWasConsumed;
	TUint code = aKeyEvent.iCode;
	TUint mods = aKeyEvent.iModifiers;
	if (code == EKeyEscape)
		{
		if (Busy())
			StopListing();
		else
			Finish(EPtBrowseNone);
		return EKeyWasConsumed;
		}
	if (iData.iPaneFocus)
		{
		switch (code)
			{
		case EKeyUpArrow:    PaneMove(-1); break;
		case EKeyDownArrow:  PaneMove(1); break;
		case EKeyPageUp:     PaneMove(-5); break;
		case EKeyPageDown:   PaneMove(5); break;
		case EKeyEnter:      PaneActL(); break;
		case EKeyBackspace:
		case EKeyDelete:
			if (iData.iPaneSel >= 0 && iData.iPaneSel < iData.Favs())
				{
				RemoveFavL(iData.iPaneSel);
				iEikonEnv->InfoMsg(_L("Removed from favourites"));
				}
			else
				iEikonEnv->InfoMsg(_L("Nothing to remove - only favourites can be"));
			break;
		case EKeyRightArrow:
		case EKeyTab:        SetPaneFocus(EFalse); break;
		default: break;
			}
		return EKeyWasConsumed;
		}
	if (!(mods & (EModifierCtrl | EModifierFunc)))
		{
		if (code == EKeyLeftArrow || code == EKeyTab)
			{
			SetPaneFocus(ETrue);
			return EKeyWasConsumed;
			}
		if (code == EKeyBackspace)
			{
			UpL();
			return EKeyWasConsumed;
			}
		if (code == EKeyEnter)
			{
			OpenL();
			return EKeyWasConsumed;
			}
		if (code > 32 && code < 127)
			{
			JumpToLetter(code);
			return EKeyWasConsumed;
			}
		}
	return iListCtl->OfferKeyEventL(aKeyEvent, aType);
	}

// the pane's taps (a tap goes there at once); a tap in the list puts the
// keys back in it
void CPtBrowser::HandlePointerEventL(const TPointerEvent& aPointerEvent)
	{
	if (iFinishing)
		return;
	if (aPointerEvent.iType == TPointerEvent::EButton1Down)
		{
		if (iSide->Rect().Contains(aPointerEvent.iPosition))
			{
			iSide->Tap(aPointerEvent.iPosition);
			return;
			}
		if (iListCtl->Rect().Contains(aPointerEvent.iPosition))
			SetPaneFocus(EFalse);
		}
	CCoeControl::HandlePointerEventL(aPointerEvent);
	}

// a second tap on the highlighted row acts on it: opens a folder, gets a
// file (Get)
void CPtBrowser::HandleListBoxEventL(CEikListBox* /*aListBox*/, TListBoxEvent aEventType)
	{
	if (aEventType == EEventItemDoubleClicked)
		OpenL();
	}

CPtBrowseView* PtNewBrowseViewL(CTermView& aView, TBool aSend, MPtBrowseHost& aHost, const TRect& aRect)
	{
	CPtBrowser* v = new(ELeave) CPtBrowser(aView, aSend, aHost);
	CleanupStack::PushL(v);
	v->ConstructL(aRect);
	CleanupStack::Pop();
	return v;
	}

static void OpenBrowseL(CTermView& aView, TBool aSend)
	{
	Mem(aView).iBrowseResult = EPtBrowseNone;
	static_cast<CPsiTermAppUi*>(CEikonEnv::Static()->EikAppUi())->OpenBrowseL(aSend);
	}

// ============================================================================
// File > Send file...
// ============================================================================

// the Psion file first (the standard Open file dialog), then the file view
// for the server folder it goes to
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
	TEntry entry;
	if (env->FsSession().Entry(name, entry) != KErrNone)
		{
		env->InfoMsg(_L("File not found"));
		return;
		}
	if (NeedLoginL(aView))
		return;
	OpenBrowseL(aView, ETrue);
	}

// Send here: the file goes to the folder chosen (iPick)
static void SendChosenL(CTermView& aView)
	{
	CEikonEnv* env = CEikonEnv::Static();
	CPtXferMemory& mem = Mem(aView);
	const TFileName& name = mem.iSendFrom;
	TParsePtrC parse(name);
	TEntry entry;
	if (env->FsSession().Entry(name, entry) != KErrNone)
		{
		env->InfoMsg(_L("File not found"));
		return;
		}
	TBuf8<512>& dir = mem.iPick;             // (the view's scratch: see CPtXferMemory)
	TBuf8<512>& remote = mem.iRemote;
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

// the file view, for the file on the server; then where it goes on the
// Psion (the standard Save as dialog)
void PtGetFileL(CTermView& aView)
	{
	if (NeedLoginL(aView))
		return;
	OpenBrowseL(aView, EFalse);
	}

static void GetChosenL(CTermView& aView)
	{
	CEikonEnv* env = CEikonEnv::Static();
	CPtXferMemory& mem = Mem(aView);
	TBuf8<512>& remote = mem.iPick;          // (the view's scratch: see CPtXferMemory)
	TUint size = mem.iPickSize;
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

// After the file view has closed (the terminal back on screen): the Get or
// Send it chose
void PtBrowseFinishedL(CTermView& aView)
	{
	if (!aView.iXferMem)
		return;
	CPtXferMemory& mem = *aView.iXferMem;
	TInt r = mem.iBrowseResult;
	mem.iBrowseResult = EPtBrowseNone;
	if (r == EPtBrowseGet)
		GetChosenL(aView);
	else if (r == EPtBrowseSendHere)
		SendChosenL(aView);
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
