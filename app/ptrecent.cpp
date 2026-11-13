// PTRECENT.CPP - Snippets > Recent commands... (1.02, from Muxedo)
//
// PsiTerm remembers the command lines you type at a shell prompt, 20 per
// server, newest first, in C:\System\Apps\PsiTerm\History.dat. A line is
// built from the printable keys typed, Backspace taking one off; Enter keeps
// it. Not kept: anything typed in a full-screen program (vim, less, Claude
// Code: the alternate screen), at a password prompt (the cursor's line
// says password, passphrase or PIN), or a line edited with keys that can't
// be followed here (Tab, the arrows, Ctrl+R...). The dialog sends one again,
// or saves it as a snippet; Remember commands turns it all off.

#include "psiterm.h"
#include <eikchlst.h>
#include <eiklabel.h>
#include <psiterm.rsg>

_LIT(KHistoryFile, "C:\\System\\Apps\\PsiTerm\\History.dat");
const TInt KPerServer = 20;
const TInt KHistoryMax = 60;

static TPtrC Upto(const TDesC& aText, TInt aMax)
	{
	return aText.Left(aText.Length() < aMax ? aText.Length() : aMax);
	}

// ============================================================================
// The list
// ============================================================================

CPtRecent* CPtRecent::NewL(RFs& aFs)
	{
	CPtRecent* r = new(ELeave) CPtRecent(aFs);
	CleanupStack::PushL(r);
	r->iItems = new(ELeave) CArrayFixFlat<TPtRecentItem>(8);
	r->iOn = ETrue;
	r->Load();
	CleanupStack::Pop();
	return r;
	}

CPtRecent::~CPtRecent()
	{
	delete iItems;
	}

// History.dat: "PR" 1, on, count, then per line: server, command (each
// length-first, the Psion's code page). Newest first.
void CPtRecent::Load()
	{
	iItems->Reset();
	RFile f;
	if (f.Open(iFs, KHistoryFile, EFileRead) != KErrNone)
		return;
	HBufC8* buf = HBufC8::New(KHistoryMax * 210 + 8);
	if (!buf)
		{
		f.Close();
		return;
		}
	TPtr8 d(buf->Des());
	TInt r = f.Read(d);
	f.Close();
	if (r == KErrNone && d.Length() >= 5 && d[0] == 'P' && d[1] == 'R' && d[2] == 1)
		{
		iOn = d[3] != 0;
		TInt n = d[4], pos = 5;
		for (TInt i = 0; i < n && i < KHistoryMax; i++)
			{
			TPtRecentItem it;
			if (pos >= d.Length())
				break;
			TInt l = d[pos++];
			if (pos + l > d.Length() || l > it.iServer.MaxLength())
				break;
			it.iServer.Copy(d.Mid(pos, l));
			pos += l;
			if (pos >= d.Length())
				break;
			l = d[pos++];
			if (pos + l > d.Length() || l > it.iCmd.MaxLength())
				break;
			it.iCmd.Copy(d.Mid(pos, l));
			pos += l;
			TRAPD(err, iItems->AppendL(it));
			if (err != KErrNone)
				break;
			}
		}
	delete buf;
	}

TInt CPtRecent::Save()
	{
	HBufC8* buf = HBufC8::New(KHistoryMax * 210 + 8);
	if (!buf)
		return KErrNoMemory;
	TPtr8 d(buf->Des());
	d.Append('P');
	d.Append('R');
	d.Append(1);
	d.Append((TUint8)(iOn ? 1 : 0));
	d.Append((TUint8)iItems->Count());
	TBuf8<128> t;
	for (TInt i = 0; i < iItems->Count(); i++)
		{
		const TPtRecentItem& it = (*iItems)[i];
		t.Copy(it.iServer);
		d.Append((TUint8)t.Length());
		d.Append(t);
		t.Copy(it.iCmd);
		d.Append((TUint8)t.Length());
		d.Append(t);
		}
	TInt r = SafeWrite(iFs, KHistoryFile, d);
	delete buf;
	return r;
	}

// A command typed on aServer: to the top (once), at most 20 for a server
// and 60 in all (the oldest go)
void CPtRecent::AddL(const TDesC& aServer, const TDesC& aCmd)
	{
	if (!iOn || aCmd.Length() == 0)
		return;
	for (TInt i = iItems->Count() - 1; i >= 0; i--)
		if ((*iItems)[i].iServer == aServer && (*iItems)[i].iCmd == aCmd)
			iItems->Delete(i);
	TPtRecentItem it;
	it.iServer = Upto(aServer, it.iServer.MaxLength());
	it.iCmd = Upto(aCmd, it.iCmd.MaxLength());
	iItems->InsertL(0, it);
	TInt mine = 0;
	for (TInt i = 0; i < iItems->Count(); i++)
		if ((*iItems)[i].iServer == aServer && ++mine > KPerServer)
			{
			iItems->Delete(i);
			i--;
			}
	while (iItems->Count() > KHistoryMax)
		iItems->Delete(iItems->Count() - 1);
	Save();
	}

TInt CPtRecent::ForServer(const TDesC& aServer, TInt* aIndices, TInt aMax) const
	{
	TInt n = 0;
	for (TInt i = 0; i < iItems->Count() && n < aMax; i++)
		if ((*iItems)[i].iServer == aServer)
			aIndices[n++] = i;
	return n;
	}

// ============================================================================
// The view's side: following the line being typed
// ============================================================================

// the server this session is on: user@host:port
void CTermView::RecentServer(TDes& aServer) const
	{
	aServer.Zero();
	aServer.Append(Upto(iSettings.iSshUser, 30));
	aServer.Append('@');
	aServer.Append(Upto(iSettings.iSshHost, 40));
	aServer.AppendFormat(_L(":%d"), iSettings.iSshPort);
	}

// the cursor's line looks like a password prompt
TBool CTermView::AtSecretPrompt() const
	{
	TBuf<160> row;
	for (TInt c = 0; c < iCols && c < 160; c++)
		{
		VTermPos pos;
		pos.row = iCurRow;
		pos.col = c;
		VTermScreenCell cell;
		TUint ch = vterm_screen_get_cell(iScreen, pos, &cell) ? cell.chars[0] : 0;
		row.Append((TChar)(ch >= 0x20 && ch < 0x7f ? ch : ' '));
		}
	row.LowerCase();
	return row.Find(_L("assword")) >= 0 || row.Find(_L("assphrase")) >= 0
		|| row.Find(_L("pin:")) >= 0 || row.Find(_L("pin for")) >= 0 || row.Find(_L("passcode")) >= 0;
	}

// Each key that goes to the host (OfferKeyEventL)
void CTermView::RecentKey(TUint aCode, TUint aMods)
	{
	if (!iRecent || !SshLoggedIn())
		return;
	if (iAltScreen)
		{
		iCmdLine.Zero();
		iCmdDirty = EFalse;
		return;
		}
	if (aCode == EKeyEnter)
		{
		TBuf<120> line(iCmdLine);
		line.TrimAll();
		if (!iCmdDirty && line.Length() && !iCmdSecret)
			{
			TBuf<80> server;
			RecentServer(server);
			TRAP_IGNORE(iRecent->AddL(server, line));
			}
		iCmdLine.Zero();
		iCmdDirty = EFalse;
		iCmdSecret = EFalse;
		return;
		}
	if (iCmdLine.Length() == 0 && !iCmdDirty)
		iCmdSecret = AtSecretPrompt();       // (looked at as the line starts)
	if (aCode == EKeyBackspace)
		{
		if (iCmdLine.Length())
			iCmdLine.SetLength(iCmdLine.Length() - 1);
		return;
		}
	if (aMods & EModifierCtrl)
		{
		TUint c = aCode | 0x60;
		if (aCode == 3 || aCode == 21 || c == 'c' || c == 'u')
			{
			iCmdLine.Zero();                 // Ctrl+C, Ctrl+U: the line is gone
			iCmdDirty = EFalse;
			}
		else
			iCmdDirty = ETrue;
		return;
		}
	if (aCode >= 0x20 && aCode < 0x100 && aCode != 0x7f)
		{
		if (iCmdLine.Length() < iCmdLine.MaxLength())
			iCmdLine.Append((TChar)aCode);
		else
			iCmdDirty = ETrue;
		return;
		}
	iCmdDirty = ETrue;                       // Tab, the arrows, Esc...: can't follow it
	}

// A remembered command, typed again: as typed (no snippet escapes), then Enter
void CTermView::SendTyped(const TDesC& aText, TBool aEnter)
	{
	for (TInt i = 0; i < aText.Length(); i++)
		SendChar(PsiCodePageToUnicode(aText[i] & 0xff));
	if (aEnter)
		SendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
	}

// ============================================================================
// Snippets > Recent commands... (Shift+Ctrl+R)
// ============================================================================

class CPtRecentDialog : public CEikDialog
	{
public:
	CPtRecentDialog(CPtRecent& aRecent, const TDesC& aServer, TInt& aIndex, TInt& aAction)
		: iRecent(aRecent), iServer(aServer), iIndex(aIndex), iAction(aAction) {}
private:
	void PreLayoutDynInitL()
		{
		iCount = iRecent.ForServer(iServer, iIdx, KPerServer);
		CDesCArrayFlat* a = new(ELeave) CDesCArrayFlat(8);
		CleanupStack::PushL(a);
		for (TInt i = 0; i < iCount; i++)
			a->AppendL(Upto(iRecent.At(iIdx[i]).iCmd, 60));
		if (iCount == 0)
			a->AppendL(_L("(none yet)"));
		CEikChoiceList* c = (CEikChoiceList*)Control(EPtDlgRecentList);
		c->SetArrayL(a);
		c->SetArrayExternalOwnership(EFalse);
		CleanupStack::Pop();
		c->SetCurrentItem(0);
		((CEikChoiceList*)Control(EPtDlgRecentOn))->SetCurrentItem(iRecent.iOn ? 1 : 0);
		TBuf<80> note(_L("Typed on "));
		note.Append(Upto(iServer, 40));
		note.Append(_L(" (not at password prompts)"));
		((CEikLabel*)Control(EPtDlgRecentNote))->SetTextL(note);
		}
	TBool OkToExitL(TInt aButtonId)
		{
		TBool on = ((CEikChoiceList*)Control(EPtDlgRecentOn))->CurrentItem() == 1;
		if (on != iRecent.iOn)
			{
			iRecent.iOn = on;
			iRecent.Save();
			}
		TInt i = ((CEikChoiceList*)Control(EPtDlgRecentList))->CurrentItem();
		iIndex = (iCount && i >= 0 && i < iCount) ? iIdx[i] : -1;
		iAction = aButtonId;
		if (iIndex < 0 && aButtonId != EEikBidCancel)
			{
			if (aButtonId == EEikBidOk && on == iRecent.iOn)
				iAction = EEikBidCancel;         // (nothing to send: just close)
			else if (aButtonId != EEikBidOk)
				{
				iEikonEnv->InfoMsg(_L("No commands remembered yet"));
				return EFalse;
				}
			}
		return ETrue;
		}
	CPtRecent& iRecent;
	const TDesC& iServer;
	TInt& iIndex;
	TInt& iAction;
	TInt iIdx[KPerServer];
	TInt iCount;
	};

void CPsiTermAppUi::RecentCommandsL()
	{
	CPtRecent* recent = iView->Recent();
	if (!recent)
		return;
	TBuf<80> server;
	iView->RecentServer(server);
	for (;;)
		{
		TInt index = -1, action = 0;
		CPtRecentDialog* dlg = new(ELeave) CPtRecentDialog(*recent, server, index, action);
		dlg->ExecuteLD(R_PT_RECENT_DIALOG);
		if (index < 0 || action == EEikBidCancel || action == 0)
			return;
		TPtRecentItem it = recent->At(index);
		if (action == EPtBidDelete)
			{
			recent->Delete(index);
			recent->Save();
			continue;
			}
		if (action == EPtBidSaveSnippet)
			{
			if (iSnippets->Count() >= KMaxSnippets)
				{
				iEikonEnv->InfoMsg(_L("Snippet list is full - delete one first"));
				return;
				}
			// the snippet's escapes: \ and ^ as themselves
			TSnippet s;
			s.iEnter = 1;
			s.iPinned = 1;
			for (TInt i = 0; i < it.iCmd.Length() && s.iText.Length() < s.iText.MaxLength() - 1; i++)
				{
				TText c = it.iCmd[i];
				if (c == '\\' || c == '^')
					s.iText.Append('\\');
				s.iText.Append(c);
				}
			s.iName = Upto(it.iCmd, s.iName.MaxLength());
			if (EditSnippetL(s, -1))
				{
				iSnippets->AddL(s);
				iSnippets->iLast = iSnippets->Count() - 1;
				iSnippets->Save();
				iEikonEnv->InfoMsg(_L("Saved as a snippet"));
				}
			return;
			}
		// Send
		if (!iView->SshLoggedIn())
			{
			iEikonEnv->InfoMsg(_L("Not available - SSH is not connected"));
			return;
			}
		iView->SendTyped(it.iCmd, ETrue);
		return;
		}
	}
