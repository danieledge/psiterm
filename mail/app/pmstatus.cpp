// pmstatus.cpp - what PsiMail says while it works (0.75)
//
// The engine reports every step it takes (psimail.h: progress), and the link
// what it is doing (dialling, looking up...). Shown as they come, they made
// a busy message that changed several times a second - useful when a
// connection won't come up, noisy the rest of the time. So, as the built-in
// Email does it, PsiMail now says one thing per command:
//
//   while it works   one busy message, bottom left: "Checking mail...",
//                    "Sending...", "Getting message..."; nothing for the
//                    work it does by itself (downloading ahead, pictures)
//   when it is done  one infoprint with the outcome: "2 new messages",
//                    "Sent", "No new mail" - and every failure, as before
//
// Preferences > New mail > Show detailed progress (KPmViewDetailedProgress)
// brings back every step. Either way the busy message is steady: it is put
// up only once a command has taken half a second (a quick one shows
// nothing), and changed in place rather than taken down and put up again.
// The message is EIKON's busy message window, without its flashing (see
// CPmBusyWin). Everything that puts up a busy message or an outcome goes through here:
// TickL (BusyTickL), Working(), CalProgress() and HandleResultL (OutcomeText).

#include "pmapp.h"
#include <eikmsg.h>
#include <clock.h>

// EIKON's busy message flashes (RMessageWindow's flash): a black bar every
// half second, which on the 5mx's screen reads as noise while a check takes
// a minute. This is the same window in the same place - bottom left, EIKON's
// font and colours, the same initial delay - shown steady.
// (CEikMsgWin's own constructor isn't exported: an infoprint's window is
// the same thing, and its way of showing the text is replaced here)
class CPmBusyWin : public CEikInfoMsgWin
	{
public:
	CPmBusyWin(CEikonEnv& aEnv) : CEikInfoMsgWin(aEnv) {}
	void SetDelay(TInt aMicroSeconds) { iDelay = aMicroSeconds; }
private:
	void DoStartDisplay(const TDesC& aText)
		{
		iMessageWindow->StartDisplay(EFalse, TTimeIntervalMicroSeconds32(iDelay), aText);
		}
	TInt iDelay;
	};

void CPmView::DeleteBusyWin()
	{
	delete iBusyWin;
	iBusyWin = NULL;
	}

const TInt KPmBusyDelayTicks = 32;          // half a second (User::TickCount is 1/64 s)
const TInt KPmBusyDelay = 500000;           // the same in microseconds

static void FromC8(TDes& aDst, const char* aSrc, TInt aMax)
	{
	aDst.Zero();
	for (TInt i = 0; i < aMax && aSrc[i] && aDst.Length() < aDst.MaxLength(); i++)
		aDst.Append((TText)(TUint8)aSrc[i]);
	}

// one message per kind of command (quiet progress)
static TPtrC QuietText(TInt aOp, const TDesC& aProgress)
	{
	switch (aOp)
		{
	case PM_CMD_FOLDERS:
	case PM_CMD_SYNC:
	case PM_CMD_SENDRECV:
		return _L("Checking mail...");
	case PM_CMD_OLDER:
		return _L("Getting older messages...");
	case PM_CMD_BODY:
	case PM_CMD_FULLBODY:
		return _L("Getting message...");
	case PM_CMD_ATTACH:
		return _L("Getting attachment...");
	case PM_CMD_SEARCH:
		return _L("Finding...");
	case PM_CMD_SEND:
		return _L("Sending...");
	case PM_CMD_CALSYNC:
		return _L("Updating the calendar...");
	case PM_CMD_UPDATE:
		if (aProgress.Length() >= 11 && aProgress.Left(11) == _L("Downloading"))
			return _L("Downloading PsiMail...");
		return _L("Looking for a new PsiMail...");
	case PM_CMD_MKFOLDER:
		return _L("Creating the folder...");
	case PM_CMD_RENFOLDER:
		return _L("Renaming the folder...");
	case PM_CMD_DELFOLDER:
		return _L("Deleting the folder...");
	case PM_CMD_UNDO:
		return _L("Undoing...");
	default:
		// downloading ahead (PM_CMD_NONE), pictures, flags, moves (already
		// said: "Moved to the Trash"), hanging up: nothing to say
		return TPtrC();
		}
	}

// what the busy message should say now
void CPmView::BusyTextNow(TDes& aOut) const
	{
	aOut.Zero();
	PmShared* s = iShared;
	if (!s)
		return;
	TBool detail = DetailedProgress();
	TBool engine = Busy();
	if (detail)
		{
		// every step: the engine's (or the link's) words, as they come;
		// Working()'s until the engine has some
		if (engine && iLastProgress.Length())
			aOut.Copy(Clip(iLastProgress, aOut.MaxLength()));
		else if (iWorkText.Length())
			aOut = iWorkText;
		else if (iCalSync && iCalSync->Running() && iCalBusy.Length())
			aOut = iCalBusy;
		else if (engine && iBusyShown)
			aOut = iBusyText;                    // (between steps: as it was)
		return;
		}
	// quiet: what the user asked for, in a word or two
	if (iWorkText.Length())
		{
		aOut = iWorkText;
		return;
		}
	if (iCalSync && iCalSync->Running())
		{
		aOut = _L("Updating the calendar...");
		return;
		}
	if (!engine)
		return;
	TInt op = PM_CMD_NONE;
	if (s->busy)
		op = s->cur_op;
	else if (s->cmd_tail != s->cmd_head)
		op = iSent[s->cmd_tail % PM_CMDQ].op;    // (about to start)
	TBuf<128> prog;
	FromC8(prog, s->progress, sizeof(s->progress));
	aOut = QuietText(op, prog);
	}

void CPmView::ShowBusy(const TDesC& aText)
	{
	if (!aText.Length() || !iNativeShown)
		{
		HideBusy();
		return;
		}
	TPtrC text = Clip(aText, 80);           // (RMessageWindow::EMaxTextLength)
	if (iBusyShown && text == iBusyText)
		return;                                  // steady: not put up again
	TInt delay = KPmBusyDelay;
	TUint now = User::TickCount();
	if (!iBusyShown)
		iBusyAt = now;
	else
		{
		// changed while up (or while waiting out the delay): in place,
		// still no sooner than half a second after the first was asked for
		TUint gone = now - iBusyAt;
		delay = gone >= (TUint)KPmBusyDelayTicks ? 0 : (KPmBusyDelayTicks - gone) * (1000000 / 64);
		}
	if (!iBusyWin)
		{
		CPmBusyWin* w = new CPmBusyWin(*iEikonEnv);
		if (!w)
			return;
		TRAPD(err, w->ConstructL(iCoeEnv->RootWin()));
		if (err != KErrNone)
			{
			delete w;
			return;
			}
		iBusyWin = w;
		}
	((CPmBusyWin*)iBusyWin)->SetDelay(delay);
	iBusyWin->StartDisplay(text, EHLeftVBottom);
	iBusyText = text;
	iBusyShown = ETrue;
	}

void CPmView::HideBusy()
	{
	if (iBusyShown && iBusyWin)
		iBusyWin->CancelDisplay();
	iBusyShown = EFalse;
	iBusyText.Zero();
	}

// four times a second (TickL)
void CPmView::BusyTickL()
	{
	// Working()'s message stays until the engine has taken the work up and
	// finished it, or for 5 seconds should it never pick it up
	if (iWorkText.Length())
		{
		if (Busy())
			iWorkSeen = ETrue;
		else if (iWorkSeen || !iWorkingSince || User::TickCount() - iWorkingSince > 64 * 5)
			{
			iWorkText.Zero();
			iWorkSeen = EFalse;
			iWorkingSince = 0;
			}
		}
	TBuf<80> t;
	BusyTextNow(t);
	ShowBusy(t);
	}

// "1 new message", "3 new messages"
static void NewText(TDes& aOut, TInt aNew)
	{
	if (aNew == 1)
		aOut.Append(_L("1 new message"));
	else
		aOut.AppendFormat(_L("%d new messages"), aNew);
	}

// a number at the start of the engine's words ("2 sent, 1 new"), -1 if none
static TInt LeadingNumber(const TDesC& aText)
	{
	TLex lex(aText);
	TInt v = -1;
	if (lex.Val(v) != KErrNone)
		return -1;
	return v;
	}

// The infoprint when a command has ended (HandleResultL): ETrue with the
// words in aOut, EFalse for none. Failures are always shown; quiet, a
// command that went as expected says only what the user would want to know.
TBool CPmView::OutcomeText(const PmCmd& aCmd, TInt aRes, const TDesC& aMsg, TDes& aOut) const
	{
	aOut.Zero();
	TInt op = aCmd.op;
	if (op == PM_CMD_UPDATE)
		return EFalse;                           // (its own dialogs)
	if (op == PM_CMD_PICTURES && aRes == PM_RES_OK)
		return EFalse;                           // (the pictures themselves say so)
	if (DetailedProgress() || aRes != PM_RES_OK)
		{
		// changes kept to tell the server later are what was asked for:
		// quiet, they need no note
		if (!DetailedProgress() && aRes == PM_RES_OFFLINE && (op == PM_CMD_FLAG || op == PM_CMD_MOVE))
			return EFalse;
		aOut.Copy(Clip(aMsg, aOut.MaxLength()));
		return aOut.Length() > 0;
		}
	switch (op)
		{
	case PM_CMD_SENDRECV:
	case PM_CMD_SEND:
		{
		// the engine: "2 sent, 1 new", "1 new in the Inbox", "No new mail",
		// "1 sent", "Nothing to send"
		TInt sent = aMsg.Find(_L(" sent")) > 0 ? LeadingNumber(aMsg) : 0;
		TInt got = op == PM_CMD_SENDRECV ? iShared->new_mail : 0;
		if (sent > 0)
			{
			if (sent == 1)
				aOut = _L("Sent");
			else
				aOut.Format(_L("%d messages sent"), sent);
			if (got > 0)
				{
				aOut.Append(_L(" - "));
				NewText(aOut, got);
				}
			}
		else if (got > 0)
			NewText(aOut, got);
		else if (op == PM_CMD_SENDRECV)
			aOut = _L("No new mail");
		else
			aOut.Copy(Clip(aMsg, aOut.MaxLength()));   // "Nothing to send"
		break;
		}
	case PM_CMD_SYNC:
	case PM_CMD_OLDER:
		{
		// "3 new", "No new messages", "There are no older messages"
		TInt n = LeadingNumber(aMsg);
		if (n > 0)
			NewText(aOut, n);
		else
			aOut.Copy(Clip(aMsg, aOut.MaxLength()));
		break;
		}
	case PM_CMD_SEARCH:
	case PM_CMD_MKFOLDER:
	case PM_CMD_RENFOLDER:
	case PM_CMD_DELFOLDER:
		aOut.Copy(Clip(aMsg, aOut.MaxLength()));   // "3 found", "Folder deleted"
		break;
	case PM_CMD_HANGUP:
		aOut = _L("Disconnected");
		break;
	default:
		// a message or an attachment downloaded (it opens), a flag, a
		// move, the folder list: what happened is on the screen
		break;
		}
	return aOut.Length() > 0;
	}
