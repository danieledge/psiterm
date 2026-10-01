// PMRECOVER.CPP - when the mail engine stops (0.75)
//
// psimail.exe can end without being asked: a crash, or (before 0.75) a
// long picture decode that starved the app of its ticks, after which the
// engine took the silent heartbeat for the app having gone and quit (see
// engine/pmepoc.cpp, engine/pictures.c). The app then said "The mail engine
// is not running" to everything, and a message being read stayed at
// "Downloading the message..." for ever, its pictures at "Getting the
// picture...".
//
// Now nothing waits on an engine that has gone: the reader shows what it
// has, the pictures that were coming say they are not shown, and the
// engine is started again (at once, unless it has stopped three times in
// ten minutes: then Tools > Restart mail engine, as the status line says).
// A message whose text was still to come is asked for again. The reason
// goes to the new engine, which writes it at the top of psimail.log, and
// the old engine's log is kept as psimail.old.

#include "pmapp.h"
#include "pmpict.h"

static TPtrC RecClip(const TDesC& aText, TInt aMax)
	{
	return aText.Left(aText.Length() < aMax ? aText.Length() : aMax);
	}

void CPmView::EngineStoppedL(const TDesC& aWhy)
	{
	iEngineNote = RecClip(aWhy, iEngineNote.MaxLength());
	// what the reader was waiting for won't come from this engine
	TBool reader = iMode == EMessage;
	TBool bodyWaiting = reader && (iWaitingBody || !iText) && !iBodyError.Length();
	if (iPictures && reader)
		{
		for (TInt i = 0; i < iPictures->Count(); i++)
			{
			TPmPicEntry& e = iPictures->At(i);
			if (e.iState == TPmPicEntry::EWaiting)
				{
				e.iState = TPmPicEntry::EFailed;
				e.iWhy = _L("the mail engine stopped");
				}
			}
		}
	if (iWebUid == iMsgUid)
		iWebUid = 0;                       // (asked again by hand, not at once)
	// started again, unless it keeps stopping
	TUint now = User::TickCount();
	TInt recent = 0;
	for (TInt k = 0; k < 3; k++)
		if (iEngineStops[k] && now - iEngineStops[k] < 64 * 600)
			recent++;
	iEngineStops[0] = iEngineStops[1];
	iEngineStops[1] = iEngineStops[2];
	iEngineStops[2] = now ? now : 1;
	if (recent < 2 && !iRestarting)
		{
		iRestarting = ETrue;
		TRAPD(err, StartEngineL());
		iRestarting = EFalse;
		(void)err;
		}
	TBuf<160> status;
	if (iRunning)
		{
		status.Format(_L("The mail engine stopped (%S) and was started again"), &iEngineNote);
		if (bodyWaiting)
			Cmd(PM_CMD_BODY, iFolder, iMsgUid, KNullDesC8);      // the text, again
		}
	else
		{
		status.Format(_L("The mail engine stopped (%S) - use Tools > Restart mail engine"), &iEngineNote);
		if (bodyWaiting)
			iBodyError = _L("Not downloaded - the mail engine stopped. Tools > Restart mail engine starts it again");
		}
	SetStatus(status);
	iReaderUid = 0;                          // the reader laid out again with what it has
	Render();
	}

// Cmd with no engine (it stopped, and wasn't restarted, or failed to start):
// one more try, rather than "The mail engine is not running"
TBool CPmView::EngineBackL()
	{
	if (iRunning)
		return ETrue;
	if (!iShared || iShared->quitting || iRestarting)
		return EFalse;
	iRestarting = ETrue;
	TRAPD(err, StartEngineL());
	iRestarting = EFalse;
	(void)err;
	if (iRunning)
		{
		CopySettingsToShared();
		SetStatus(_L("The mail engine was started again"));
		}
	return iRunning;
	}

// A PICTURES or WEBPICS command has ended: what it decoded goes in, and a
// frame still saying "Getting the picture..." says what happened instead
// (a picture left for next time, the line lost, Stopped)
void CPmView::PicturesDoneL(const PmCmd& aCmd)
	{
	if (iMode != EMessage || aCmd.uid != iMsgUid || !iPictures)
		return;
	TRAPD(err, RefreshPicturesL());
	(void)err;
	TBool web = aCmd.op == PM_CMD_WEBPICS;
	TInt res = iShared ? iShared->last_res : PM_RES_FAILED;
	TBool changed = EFalse;
	for (TInt i = 0; i < iPictures->Count(); i++)
		{
		TPmPicEntry& e = iPictures->At(i);
		if (e.iState != TPmPicEntry::EWaiting || (e.iWeb != 0) != web)
			continue;
		if (res == PM_RES_OK && !web)
			e.iState = TPmPicEntry::EUnknown;          // (left for the next reading)
		else
			{
			e.iState = TPmPicEntry::EFailed;
			e.iWhy = res == PM_RES_CANCELLED ? _L("stopped") : res == PM_RES_OFFLINE ? _L("not connected") :
				res == PM_RES_OK ? _L("over the limit for one message") : _L("not downloaded");
			}
		changed = ETrue;
		}
	if (changed)
		RelayoutReaderL();
	}
