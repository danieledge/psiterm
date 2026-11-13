// PTTMUX.CPP - tmux as Muxedo has it (1.01)
//
// tmux > tmux settings...   the defaults: start in tmux, the session, a
//                           session of the Psion's own, the options set on
//                           attach, the prefix key, a word if tmux is missing
// SSH server > tmux...      one server's own (each "Use default" until set)
// tmux > Sessions...        tmux's sessions: Switch / New / Rename / Close
// Windows, Panes            new, rename and close as tmux commands, not keys
// View > Scroll > tmux history   the pane's own history into the scrollback
//
// The commands go to tmux on the query channel (ssh/tmuxq.c, PSI_TQ_RUN), so
// they work whatever has the keyboard (vim's insert mode, a prompt). When
// the server will not run commands on a channel, the prefix keys are typed
// as before. A server's command on login, if it has one, is used as it is:
// it overrides the tmux settings' start command.

#include "psiterm.h"
#include <eikchlst.h>
#include <eikdialg.h>
#include <eikon.rsg>
#include "psiterm.rsg"

_LIT(KTmuxIni, "C:\\System\\Apps\\PsiTerm\\Tmux.ini");
_LIT(KHistoryFile, "C:\\System\\Apps\\PsiTerm\\History.tmp");
_LIT8(KHistoryRemote, ".psiterm-history.txt");
const TInt KScrollbackColsMax = 128;     // (KScrollbackCols, psiterm.cpp)

static void DeleteCells(TAny* aCells)
	{
	delete[] (VTermScreenCell*)aCells;
	}

// the Psion's text (plain ASCII here: PtTmuxTextOk) onto 8-bit commands
static void Add(TDes8& aOut, const TDesC& aText)
	{
	for (TInt i = 0; i < aText.Length() && aOut.Length() < aOut.MaxLength(); i++)
		aOut.Append((TUint8)aText[i]);
	}

// ============================================================================
// Settings
// ============================================================================

static void Defaults(TPtTmux& aTmux)
	{
	aTmux.iStart = 0;                    // (as before: a server's command on login starts tmux)
	aTmux.iSession.Copy(_L("main"));
	aTmux.iOwn = 1;                      // Muxedo's standard: the Psion's size and window are its own
	aTmux.iOpts = 1;
	aTmux.iOptions.Copy(_L("set -g window-size latest; set -g aggressive-resize on"));
	aTmux.iWarn = 1;
	}

// (Left(n) panics, USER 22, when n is more than the length: this does not)
static TPtrC Upto(const TDesC& aText, TInt aMax)
	{
	return aText.Left(aText.Length() < aMax ? aText.Length() : aMax);
	}

static TPtrC8 Upto8(const TDesC8& aText, TInt aMax)
	{
	return aText.Left(aText.Length() < aMax ? aText.Length() : aMax);
	}

static void Put(TDes8& aOut, const TDesC& aText)
	{
	TBuf8<128> t;
	t.Copy(Upto(aText, 127));
	aOut.Append((TUint8)t.Length());
	aOut.Append(t);
	}

static TBool Get(const TDesC8& aIn, TInt& aPos, TDes& aText)
	{
	if (aPos >= aIn.Length())
		return EFalse;
	TInt len = aIn[aPos++];
	if (aPos + len > aIn.Length() || len > aText.MaxLength())
		return EFalse;
	aText.Copy(aIn.Mid(aPos, len));
	aPos += len;
	return ETrue;
	}

// Tmux.ini: "PX" 1, start, own, opts, warn, session, options (the strings
// length-first). Fields may be appended; an older file keeps the defaults
// for what it lacks.
void PtTmuxLoad(RFs& aFs, TPtTmux& aTmux)
	{
	Defaults(aTmux);
	RFile f;
	if (f.Open(aFs, KTmuxIni, EFileRead) != KErrNone)
		return;
	TBuf8<300> d;
	TInt r = f.Read(d);
	f.Close();
	if (r != KErrNone || d.Length() < 7 || d[0] != 'P' || d[1] != 'X' || d[2] < 1)
		return;
	aTmux.iStart = d[3] ? 1 : 0;
	aTmux.iOwn = d[4] ? 1 : 0;
	aTmux.iOpts = d[5] ? 1 : 0;
	aTmux.iWarn = d[6] ? 1 : 0;
	TInt pos = 7;
	TPtTmux t;
	if (Get(d, pos, t.iSession) && Get(d, pos, t.iOptions))
		{
		if (t.iSession.Length())
			aTmux.iSession = t.iSession;
		aTmux.iOptions = t.iOptions;     // (empty: no options)
		}
	}

TInt PtTmuxSave(RFs& aFs, const TPtTmux& aTmux)
	{
	TBuf8<300> d;
	d.Append('P');
	d.Append('X');
	d.Append(1);
	d.Append((TUint8)(aTmux.iStart ? 1 : 0));
	d.Append((TUint8)(aTmux.iOwn ? 1 : 0));
	d.Append((TUint8)(aTmux.iOpts ? 1 : 0));
	d.Append((TUint8)(aTmux.iWarn ? 1 : 0));
	Put(d, aTmux.iSession);
	Put(d, aTmux.iOptions);
	return SafeWrite(aFs, KTmuxIni, d);
	}

// a server's choice (0 the default, 1 yes, 2 no) over the default's (0/1)
static TInt Pick(TInt aDefault, TInt aServer)
	{
	return aServer == 1 ? 1 : aServer == 2 ? 0 : aDefault;
	}

void PtTmuxEffective(const TPtTmux& aDefaults, const TPtTmux& aServer, TPtTmux& aOut)
	{
	aOut = aDefaults;
	aOut.iStart = Pick(aDefaults.iStart, aServer.iStart);
	aOut.iOwn = Pick(aDefaults.iOwn, aServer.iOwn);
	aOut.iOpts = Pick(aDefaults.iOpts, aServer.iOpts);
	if (aServer.iSession.Length())
		aOut.iSession = aServer.iSession;
	if (aServer.iOptions.Length())
		aOut.iOptions = aServer.iOptions;
	if (aOut.iSession.Length() == 0)
		aOut.iSession.Copy(_L("main"));
	}

// Shared:  tmux new -A -s main
// Own:     tmux new -d -s main 2>/dev/null; exec tmux new -A -s main-psion -t main
//          (a session grouped with main: the same windows, but the Psion's
//          own size and current window; "new -d" makes main if need be)
void PtTmuxStartCommand(const TPtTmux& aTmux, TDes& aCommand)
	{
	const TDesC& s = aTmux.iSession;
	aCommand.Zero();
	if (aTmux.iOwn)
		{
		aCommand.Append(_L("tmux new -d -s "));
		aCommand.Append(s);
		aCommand.Append(_L(" 2>/dev/null; exec tmux new -A -s "));
		aCommand.Append(s);
		aCommand.Append(_L("-psion -t "));
		aCommand.Append(s);
		}
	else
		{
		aCommand.Append(_L("tmux new -A -s "));
		aCommand.Append(s);
		}
	}

TBool PtTmuxTextOk(const TDesC& aText, TBool aName)
	{
	for (TInt i = 0; i < aText.Length(); i++)
		{
		TUint c = aText[i];
		if (c < 0x20 || c > 0x7e || c == '"' || c == '$' || c == '\\' || c == '`')
			return EFalse;
		if (aName && (c == '\'' || c == ':' || c == '.' || c == ';' || c == ' '))
			return EFalse;
		}
	return ETrue;
	}

// ============================================================================
// The settings dialog (the defaults, or one server's own)
// ============================================================================

class CPtTmuxDialog : public CEikDialog
	{
public:
	CPtTmuxDialog(TPtTmux& aTmux, const TPtTmux* aDefaults, TInt* aPrefix)
		: iTmux(aTmux), iDefaults(aDefaults), iPrefix(aPrefix) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void SetChoicesL(TInt aId, TInt aValue, TInt aDefault);
	TInt Choice(TInt aId) const;
	TPtTmux& iTmux;
	const TPtTmux* iDefaults;   // 0: these are the defaults
	TInt* iPrefix;              // (the defaults only) the prefix key setting
	};

// Yes/No, or for a server "Use default (Yes)" / Yes / No
void CPtTmuxDialog::SetChoicesL(TInt aId, TInt aValue, TInt aDefault)
	{
	CDesCArrayFlat* a = new(ELeave) CDesCArrayFlat(3);
	CleanupStack::PushL(a);
	if (iDefaults)
		{
		a->AppendL(aDefault ? _L("Use default (Yes)") : _L("Use default (No)"));
		a->AppendL(_L("Yes"));
		a->AppendL(_L("No"));
		}
	else
		{
		a->AppendL(_L("No"));
		a->AppendL(_L("Yes"));
		}
	CEikChoiceList* c = (CEikChoiceList*)Control(aId);
	c->SetArrayL(a);
	c->SetArrayExternalOwnership(EFalse);
	CleanupStack::Pop();
	c->SetCurrentItem(aValue >= 0 && aValue <= (iDefaults ? 2 : 1) ? aValue : 0);
	}

TInt CPtTmuxDialog::Choice(TInt aId) const
	{
	return ((CEikChoiceList*)Control(aId))->CurrentItem();
	}

void CPtTmuxDialog::PreLayoutDynInitL()
	{
	const TPtTmux& d = iDefaults ? *iDefaults : iTmux;
	SetChoicesL(EPtDlgTmuxStart, iTmux.iStart, d.iStart);
	SetChoicesL(EPtDlgTmuxOwn, iTmux.iOwn, d.iOwn);
	SetChoicesL(EPtDlgTmuxOpts, iTmux.iOpts, d.iOpts);
	SetEdwinTextL(EPtDlgTmuxSession, &iTmux.iSession);
	SetEdwinTextL(EPtDlgTmuxOptions, &iTmux.iOptions);
	if (iDefaults)
		{
		// a server's own: blank text means the default; the prefix and the
		// warning are the same for every server
		SetTitleL(_L("tmux on this server"));
		MakeLineVisible(EPtDlgTmuxPrefix, EFalse);
		MakeLineVisible(EPtDlgTmuxWarn, EFalse);
		}
	else
		{
		SetChoicesL(EPtDlgTmuxWarn, iTmux.iWarn, 1);
		((CEikChoiceList*)Control(EPtDlgTmuxPrefix))->SetCurrentItem(iPrefix && *iPrefix ? 1 : 0);
		}
	}

TBool CPtTmuxDialog::OkToExitL(TInt /*aButtonId*/)
	{
	TBuf<KTmuxSessionLen> session;
	TBuf<120> options;
	GetEdwinText(session, EPtDlgTmuxSession);
	GetEdwinText(options, EPtDlgTmuxOptions);
	session.Trim();
	options.Trim();
	if (!iDefaults && session.Length() == 0)
		{
		CEikonEnv::Static()->InfoMsg(_L("No session name entered"));
		TryChangeFocusToL(EPtDlgTmuxSession);
		return EFalse;
		}
	if (!PtTmuxTextOk(session, ETrue))
		{
		CEikonEnv::Static()->InfoMsg(_L("Session names are letters, digits, - and _"));
		TryChangeFocusToL(EPtDlgTmuxSession);
		return EFalse;
		}
	if (!PtTmuxTextOk(options, EFalse))
		{
		CEikonEnv::Static()->InfoMsg(_L("Options cannot hold \", $, \\ or `"));
		TryChangeFocusToL(EPtDlgTmuxOptions);
		return EFalse;
		}
	iTmux.iSession = session;
	iTmux.iOptions = options;
	iTmux.iStart = Choice(EPtDlgTmuxStart);
	iTmux.iOwn = Choice(EPtDlgTmuxOwn);
	iTmux.iOpts = Choice(EPtDlgTmuxOpts);
	if (!iDefaults)
		{
		iTmux.iWarn = Choice(EPtDlgTmuxWarn);
		if (iPrefix)
			*iPrefix = Choice(EPtDlgTmuxPrefix);
		}
	return ETrue;
	}

TBool PtTmuxSettingsL(TPtTmux& aTmux, const TPtTmux* aDefaults, TInt* aPrefix)
	{
	CPtTmuxDialog* dlg = new(ELeave) CPtTmuxDialog(aTmux, aDefaults, aPrefix);
	return dlg->ExecuteLD(R_PT_TMUX_DIALOG) != 0;
	}

// ============================================================================
// A name (New session, Rename session, Rename window)
// ============================================================================

class CPtTmuxNameDialog : public CEikDialog
	{
public:
	CPtTmuxNameDialog(TDes& aName, const TDesC& aTitle, TBool aWindow)
		: iName(aName), iTitle(aTitle), iWindow(aWindow) {}
private:
	void PreLayoutDynInitL()
		{
		SetTitleL(iTitle);
		SetEdwinTextL(EPtDlgTmuxName, &iName);
		}
	TBool OkToExitL(TInt /*aButtonId*/)
		{
		TBuf<24> n;
		GetEdwinText(n, EPtDlgTmuxName);
		n.Trim();
		if (n.Length() == 0)
			{
			CEikonEnv::Static()->InfoMsg(_L("No name entered"));
			return EFalse;
			}
		// a window's name may have spaces (it goes in single quotes); a
		// session's is a target, so plain
		if (!PtTmuxTextOk(n, !iWindow) || n.Locate('\'') >= 0)
			{
			CEikonEnv::Static()->InfoMsg(iWindow ? _L("Names cannot hold quotes, $, \\ or `")
				: _L("Session names are letters, digits, - and _"));
			return EFalse;
			}
		iName = n;
		return ETrue;
		}
	TDes& iName;
	const TDesC& iTitle;
	TBool iWindow;
	};

static TBool AskName(TDes& aName, const TDesC& aTitle, TBool aWindow)
	{
	CPtTmuxNameDialog* dlg = new(ELeave) CPtTmuxNameDialog(aName, aTitle, aWindow);
	return dlg->ExecuteLD(R_PT_TMUX_NAME_DIALOG) != 0;
	}

// ============================================================================
// tmux > Sessions...
// ============================================================================

class CPtTmuxSessionsDialog : public CEikDialog
	{
public:
	CPtTmuxSessionsDialog(CDesCArray* aLines, TInt aCurrent, TInt& aIndex, TInt& aAction)
		: iLines(aLines), iCurrent(aCurrent), iIndex(aIndex), iAction(aAction) {}
private:
	void PreLayoutDynInitL()
		{
		CEikChoiceList* c = (CEikChoiceList*)Control(EPtDlgTmuxSessList);
		c->SetArrayL(iLines);
		c->SetArrayExternalOwnership(EFalse);
		c->SetCurrentItem(iCurrent);
		}
	TBool OkToExitL(TInt aButtonId)
		{
		iIndex = ((CEikChoiceList*)Control(EPtDlgTmuxSessList))->CurrentItem();
		iAction = aButtonId;
		return ETrue;
		}
	CDesCArray* iLines;
	TInt iCurrent;
	TInt& iIndex;
	TInt& iAction;
	};

void CPsiTermAppUi::TmuxSessionsL()
	{
	if (!iView->TmuxCanRun())
		{
		SendTmux('s');                       // (tmux's own chooser)
		return;
		}
	const TInt KMax = 12;
	TBuf8<12> ids[KMax];
	TBuf<24> names[KMax];
	TInt wins[KMax], att[KMax];
	TInt n = iView->TmuxSessions(ids, names, wins, att, KMax);
	if (n == 0)
		{
		iEikonEnv->InfoMsg(_L("tmux has not answered yet"));
		return;
		}
	CDesCArrayFlat* lines = new(ELeave) CDesCArrayFlat(KMax);
	CleanupStack::PushL(lines);
	TInt current = 0;
	for (TInt i = 0; i < n; i++)
		{
		TBuf<60> l(names[i]);
		l.AppendFormat(wins[i] == 1 ? _L(" (1 window") : _L(" (%d windows"), wins[i]);
		if (ids[i] == iView->TmuxSid())
			{
			l.Append(_L(", this one"));
			current = i;
			}
		else if (att[i])
			l.Append(_L(", in use"));
		l.Append(')');
		lines->AppendL(l);
		}
	TInt index = 0, action = 0;
	CPtTmuxSessionsDialog* dlg = new(ELeave) CPtTmuxSessionsDialog(lines, current, index, action);
	CleanupStack::Pop();                     // (the dialog owns the lines)
	if (!dlg->ExecuteLD(R_PT_TMUX_SESSIONS_DIALOG) && action != EPtBidNew
		&& action != EPtBidTmuxRename && action != EPtBidTmuxClose)
		return;
	if (index < 0 || index >= n)
		index = 0;
	TBuf8<200> cmd;
	TBuf8<16> id(ids[index]);                // '$3': only digits after the $ (tmuxq.c)
	if (action == EPtBidNew)
		{
		TBuf<24> name;
		if (!AskName(name, _L("New session"), EFalse))
			return;
		cmd.Append(_L8("new-session -d -s '"));
		Add(cmd, name);
		cmd.Append(_L8("'; switch-client -t '"));
		Add(cmd, name);
		cmd.Append('\'');
		}
	else if (action == EPtBidTmuxRename)
		{
		TBuf<24> name(names[index]);
		if (!AskName(name, _L("Rename session"), EFalse))
			return;
		cmd.Append(_L8("rename-session -t '"));
		cmd.Append(id);
		cmd.Append(_L8("' '"));
		Add(cmd, name);
		cmd.Append('\'');
		}
	else if (action == EPtBidTmuxClose)
		{
		TBuf<80> q;
		q.Format(_L("Session %S and its windows will be closed"), &names[index]);
		if (!iEikonEnv->QueryWinL(q, _L("Close the session?")))
			return;
		cmd.Append(_L8("kill-session -t '"));
		cmd.Append(id);
		cmd.Append('\'');
		}
	else
		{
		if (id == iView->TmuxSid())
			return;
		cmd.Append(_L8("switch-client -t '"));
		cmd.Append(id);
		cmd.Append('\'');
		}
	iView->TmuxRun(cmd);
	}

// tmux > tmux settings...
void CPsiTermAppUi::TmuxSettingsL()
	{
	TPtTmux t = iTmux;
	TInt prefix = iView->Settings().iTmuxPrefix;
	if (!PtTmuxSettingsL(t, 0, &prefix))
		return;
	iTmux = t;
	PtTmuxSave(iCoeEnv->FsSession(), iTmux);
	if (prefix != iView->Settings().iTmuxPrefix)
		{
		iView->Settings().iTmuxPrefix = prefix;
		SaveSettings(iView->Settings());
		}
	}

// Windows and panes: as tmux commands when it can be asked, else the keys
void CPsiTermAppUi::TmuxCommandL(TInt aCommand)
	{
	TBool run = iView->TmuxCanRun();
	const TDesC8& sid = iView->TmuxSid();
	TBuf8<120> cmd;
	// the target: our session's current window ('$3:'), or tmux's choice
	TBuf8<24> win;
	if (sid.Length() && sid[0] == '$')
		{
		win.Append(_L8(" -t '"));
		win.Append(sid);
		win.Append(_L8(":'"));
		}
	switch (aCommand)
		{
	case EPtCmdTmuxNew:
		if (!run) { SendTmux('c'); return; }
		cmd.Append(_L8("new-window"));
		cmd.Append(win);
		break;
	case EPtCmdTmuxRename:
		{
		TBuf<24> name;
		if (!run || iView->TmuxCurrentWindow(name) < 0) { SendTmux(','); return; }
		if (!AskName(name, _L("Rename window"), ETrue))
			return;
		cmd.Append(_L8("rename-window"));
		cmd.Append(win);
		cmd.Append(_L8(" '"));
		Add(cmd, name);
		cmd.Append('\'');
		break;
		}
	case EPtCmdTmuxCloseWin:
		{
		TBuf<24> name;
		if (!run || iView->TmuxCurrentWindow(name) < 0) { SendTmux('&'); return; }
		TBuf<80> q;
		q.Format(_L("Window %S and what runs in it will be closed"), &name);
		if (!iEikonEnv->QueryWinL(q, _L("Close the window?")))
			return;
		cmd.Append(_L8("kill-window"));
		cmd.Append(win);
		break;
		}
	case EPtCmdTmuxClosePane:
		if (!run) { SendTmux('x'); return; }
		if (!iEikonEnv->QueryWinL(_L("The pane and what runs in it will be closed"), _L("Close the pane?")))
			return;
		cmd.Append(_L8("kill-pane"));
		cmd.Append(win);
		break;
	case EPtCmdTmuxEven:
		if (!run) { SendTmux(' '); return; }    // (tmux's next layout)
		cmd.Append(_L8("select-layout"));
		cmd.Append(win);
		cmd.Append(_L8(" tiled"));
		break;
	case EPtCmdTmuxBreak:
		if (!run) { SendTmux('!'); return; }
		cmd.Append(_L8("break-pane"));
		if (win.Length())
			{
			cmd.Append(_L8(" -s"));
			cmd.Append(win.Mid(3));             // (" -t " -> " -s ")
			}
		break;
	default:
		return;
		}
	iView->TmuxRun(cmd);
	}

// SSH server > tmux...: this server's own settings
void PtHostTmuxL(TPtTmux& aServer, const TPtTmux& aDefaults)
	{
	TPtTmux t = aServer;
	if (PtTmuxSettingsL(t, &aDefaults, 0))
		aServer = t;
	}

// ============================================================================
// The view's side: running commands, the sessions, the history
// ============================================================================

void CTermView::SetTmux(const TPtTmux& aTmux)
	{
	iTmuxCfg = aTmux;
	iTmuxOptsSent = EFalse;
	iTmuxWarned = EFalse;
	iTqRun.Zero();
	iTqAfterPending = EFalse;
	iQSessCount = 0;
	iHistState = 0;
	}

TBool CTermView::TmuxCanRun() const
	{
	return iShared && iSshActive && SshLoggedIn() && iTqState != 2;
	}

// Waits for the channel (TmuxQueryTick posts it, before the next list)
TBool CTermView::TmuxRun(const TDesC8& aCommands)
	{
	if (!TmuxCanRun())
		return EFalse;
	if (iTqRun.Length() && iTqRun.Length() + aCommands.Length() + 2 <= iTqRun.MaxLength())
		iTqRun.Append(_L8("; "));            // (two in a row: run together)
	else
		iTqRun.Zero();
	iTqRun.Append(Upto8(aCommands, iTqRun.MaxLength() - iTqRun.Length()));
	iTqRunOp = PSI_TQ_RUN;
	iTqKickAt = User::TickCount();           // at once
	if (!iTqKickAt)
		iTqKickAt = 1;
	return ETrue;
	}

// A snippet in a new window: tmux makes the window (and goes to it), then
// the text is typed there
void CTermView::TmuxRunThenType(const TDesC8& aCommands, const TDesC& aText, TBool aEnter)
	{
	if (!TmuxRun(aCommands))
		{
		SendSnippetText(aText, aEnter);
		return;
		}
	iTqAfterText = Upto(aText, iTqAfterText.MaxLength());
	iTqAfterEnter = aEnter;
	iTqAfterPending = ETrue;
	}

void CTermView::SendSnippet(const TSnippet& aSnippet)
	{
	if (!aSnippet.iNewWin || !TmuxCanRun())
		{
		SendSnippetText(aSnippet.iText, aSnippet.iEnter);
		return;
		}
	TBuf8<40> cmd(_L8("new-window"));
	if (iQSid.Length() && iQSid[0] == '$')
		{
		cmd.Append(_L8(" -t '"));
		cmd.Append(iQSid);
		cmd.Append(_L8(":'"));
		}
	TmuxRunThenType(cmd, aSnippet.iText, aSnippet.iEnter);
	}

// After a RUN or HISTORY has been answered (TmuxQueryTick)
void CTermView::TmuxRunDone(TInt aOp, TInt aResult)
	{
	if (aOp == PSI_TQ_RUN && iTqAfterPending)
		{
		iTqAfterPending = EFalse;
		if (aResult == PSI_TQ_OK)
			SendSnippetText(iTqAfterText, iTqAfterEnter);
		}
	if (aOp == PSI_TQ_HISTORY && iHistState == 1)
		{
		if (aResult != PSI_TQ_OK)
			{
			iHistState = 0;
			iEikonEnv->InfoMsg(_L("tmux did not answer"));
			return;
			}
		// fetch the file it wrote (SFTP, as Get file)
		PsiShared* s = iShared;
		if (!s || s->xfer_req != s->xfer_ack)
			{
			iHistState = 0;
			iEikonEnv->InfoMsg(_L("Busy with a file - try again"));
			return;
			}
		TPtr8 local((TUint8*)s->xfer_local, sizeof(s->xfer_local) - 1);
		local.Copy(KHistoryFile);
		local.ZeroTerminate();
		TPtr8 remote((TUint8*)s->xfer_remote, sizeof(s->xfer_remote) - 1);
		remote.Copy(KHistoryRemote);
		remote.ZeroTerminate();
		s->xfer_op = PSI_XOP_GET;
		s->xfer_cancel = 0;
		s->xfer_req = s->xfer_req + 1;       // last: psissh starts now
		RingSsh();
		iHistState = 2;
		}
	}

void CTermView::TmuxHistoryL()
	{
	if (!TmuxCanRun() || iQSid.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No tmux session seen"));
		return;
		}
	if (iHistState)
		return;
	iTqRun.Zero();
	iTqRunOp = PSI_TQ_HISTORY;
	iTqRun.Append('h');                      // (anything: something is waiting)
	iTqKickAt = User::TickCount();
	if (!iTqKickAt)
		iTqKickAt = 1;
	iHistState = 1;
	iEikonEnv->BusyMsgL(_L("Fetching tmux history"));
	}

// every tick while fetching: the file has come
void CTermView::TmuxHistoryTick()
	{
	PsiShared* s = iShared;
	if (iHistState != 2 || !s || s->xfer_req != s->xfer_ack)
		return;
	iHistState = 0;
	iEikonEnv->BusyMsgCancel();
	if (s->xfer_result != PSI_XFER_OK)
		{
		iEikonEnv->InfoMsg(s->xfer_result == PSI_XFER_NO_SFTP ? _L("The server has no SFTP")
			: _L("Could not fetch tmux history"));
		return;
		}
	TRAPD(err, TmuxHistoryLoadL());
	iCoeEnv->FsSession().Delete(KHistoryFile);
	if (err != KErrNone)
		iEikonEnv->InfoMsg(_L("Not enough memory for tmux history"));
	}

// The pane's history (UTF-8 lines; capture-pane -J, so long lines are whole)
// replaces the scrollback, wrapped at the screen's width, and the view goes
// back a page into it
void CTermView::TmuxHistoryLoadL()
	{
	RFile f;
	User::LeaveIfError(f.Open(iCoeEnv->FsSession(), KHistoryFile, EFileRead));
	CleanupClosePushL(f);
	TInt size = 0;
	f.Size(size);
	if (size > 256 * 1024)
		size = 256 * 1024;                   // (the newest part matters: kept below)
	HBufC8* buf = HBufC8::NewLC(size + 1);
	TPtr8 d(buf->Des());
	User::LeaveIfError(f.Read(d, size));
	TInt cols = iCols < KScrollbackColsMax ? iCols : KScrollbackColsMax;
	if (cols < 10)
		cols = 10;
	VTermScreenCell* cells = new(ELeave) VTermScreenCell[KScrollbackColsMax];
	CleanupStack::PushL(TCleanupItem(DeleteCells, cells));
	SbClear(&iSb);
	VTermScreenCell blank;
	Mem::FillZ(&blank, sizeof(blank));
	blank.width = 1;
	blank.fg.type = VTERM_COLOR_DEFAULT_FG;
	blank.bg.type = VTERM_COLOR_DEFAULT_BG;
	TInt lines = 0, i = 0, n = d.Length();
	while (i < n)
		{
		// one line, as cells; a long one goes on over more rows
		TInt col = 0;
		TBool any = EFalse;
		while (i < n && d[i] != '\n')
			{
			TUint c = d[i++];
			if (c >= 0xc0 && c < 0xf8)
				{
				TInt more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
				c &= 0x3f >> more;
				for (TInt k = 0; k < more && i < n && (d[i] & 0xc0) == 0x80; k++)
					c = (c << 6) | (d[i++] & 0x3f);
				if (c > 0xffff)
					c = '?';
				}
			else if (c >= 0x80)
				c = '?';
			else if (c == '\t')
				c = ' ';
			else if (c < 0x20)
				continue;
			cells[col] = blank;
			cells[col].chars[0] = c;
			col++;
			any = ETrue;
			if (col == cols)
				{
				CbPushLine(col, cells, this);
				lines++;
				col = 0;
				any = EFalse;
				}
			}
		if (i < n)
			i++;                             // (the \n)
		if (any || col == 0)
			{
			if (col == 0)
				{
				cells[0] = blank;
				col = 1;
				}
			CbPushLine(col, cells, this);
			lines++;
			}
		}
	CleanupStack::PopAndDestroy(3);          // cells, buf, f
	iScrollOffset = 0;
	ScrollTo(iRows > 2 ? iRows - 2 : 1);
	TBuf<60> m;
	m.Format(_L("tmux history: %d lines"), lines < iSb.iCount ? lines : iSb.iCount);
	iEikonEnv->InfoMsg(m);
	}

TInt CTermView::TmuxSessions(TBuf8<12>* aIds, TBuf<24>* aNames, TInt* aWindows, TInt* aAttached, TInt aMax) const
	{
	TInt n = 0;
	for (; n < iQSessCount && n < aMax; n++)
		{
		aIds[n] = iQSessId[n];
		aNames[n] = iQSessName[n];
		aWindows[n] = iQSessWin[n];
		aAttached[n] = iQSessAtt[n];
		}
	return n;
	}

TInt CTermView::TmuxCurrentWindow(TDes& aName) const
	{
	if (!ExactReady())
		return -1;
	for (TInt t = 0; t < iQCount; t++)
		if (iQTabs[t].iCurrent)
			{
			aName = Upto(iQTabs[t].iName, aName.MaxLength());
			return iQTabs[t].iIndex;
			}
	return -1;
	}
