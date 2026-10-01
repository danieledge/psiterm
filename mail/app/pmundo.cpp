// PMUNDO.CPP - Edit > Undo (Ctrl+Z): puts back the last message deleted
// (moved to the Trash), moved to a folder or archived.
//
// The app keeps the last few moves of this session (newest last); the
// engine keeps what each message had here - its line in the folder's
// list, its text and pictures - and moves it back on the server, or takes
// a move that is still waiting (offline) out of the queue (engine/undo.c).
// A delete from the Trash itself is for good, after asking, and is not kept.

#include <txtrich.h>
#include "pmapp.h"

static void UndoFromC(TDes& aDst, const char* aSrc)
	{
	TPtrC8 p((const TUint8*)aSrc);
	aDst.Copy(p.Left(p.Length() < aDst.MaxLength() ? p.Length() : aDst.MaxLength()));
	}

// before a delete or move goes to the engine: what to put back
void CPmView::NoteUndoL(TUint aUid)
	{
	if (iUndoCount == KPmUndoMax)
		{
		for (TInt i = 1; i < KPmUndoMax; i++)
			iUndo[i - 1] = iUndo[i];
		iUndoCount--;
		}
	TPmUndo& u = iUndo[iUndoCount];
	u.iAcct = iSettings->iAcct;
	u.iFolder = iFolder;
	u.iUid = aUid;
	u.iSubject.Zero();
	for (TInt i = 0; i < iRows->Count(); i++)
		if ((*iRows)[i].iUid == aUid)
			{
			const TDesC& s = (*iRows)[i].iSubject;
			u.iSubject.Copy(s.Left(s.Length() < 40 ? s.Length() : 40));
			if (s.Length() > 40)
				u.iSubject.Append(TChar(0x85));   // (an ellipsis in the Psion's character set)
			break;
			}
	iUndoCount++;
	}

TBool CPmView::CanUndo() const
	{
	return iUndoCount > 0 && iUndo[iUndoCount - 1].iAcct == iSettings->iAcct &&
		(iMode == EList || iMode == EMessage || iMode == EOutbox);
	}

void CPmView::UndoL()
	{
	if (iUndoCount > 0 && iUndo[iUndoCount - 1].iAcct != iSettings->iAcct)
		iUndoCount = 0;                      // (another account's: not this one's to undo)
	if (!iUndoCount)
		{
		Toast(_L("Nothing to undo"));
		return;
		}
	if (iMode != EList && iMode != EMessage && iMode != EOutbox)
		{
		Toast(_L("Not available in the calendar"));
		return;
		}
	if (!iRunning)
		{
		Toast(_L("Not available - the mail engine is not running"));
		return;
		}
	iUndoDoing = iUndo[--iUndoCount];
	Cmd(PM_CMD_UNDO, iUndoDoing.iFolder, iUndoDoing.iUid, KNullDesC8);
	Working(_L("Undoing..."));
	}

// the engine's answer: the message is back (last_file: its uid now)
void CPmView::UndoResultL(const PmCmd& aCmd)
	{
	PmShared* s = iShared;
	TBuf<160> msg;
	UndoFromC(msg, s->last_msg);
	TInt res = s->last_res;
	if (!Busy())
		{
		iWorkText.Zero();                    // (pmstatus.cpp)
		iWorkingSince = 0;
		HideBusy();
		}
	TPtrC8 folder((const TUint8*)aCmd.folder);
	if (res != PM_RES_OK && res != PM_RES_OFFLINE)
		{
		Toast(msg.Length() ? (const TDesC&)msg : (const TDesC&)_L("Not undone"));
		ReloadL();
		return;
		}
	TBuf<20> u16;
	UndoFromC(u16, s->last_file);
	TLex lex(u16);
	TUint uid = 0;
	lex.Val(uid);
	// where it is back: the folder's name as the tree shows it
	TBuf<64> name(_L("its folder"));
	for (TInt i = 0; i < iFolders->Count(); i++)
		if ((*iFolders)[i].iImap == folder)
			{
			name = (*iFolders)[i].iName;
			break;
			}
	ReloadL();
	if ((iMode == EList || iMode == EMessage) && !iSearch && iFolder == folder)
		{
		if (iMode == EMessage)
			{
			iMode = EList;
			LoadListL();
			}
		for (TInt i = 0; i < iRows->Count(); i++)
			if ((*iRows)[i].iUid == uid)
				{
				iSel = i;
				break;
				}
		EnsureVisible();
		}
	Render();
	// Undone - "Lunch on Friday?" is back in Inbox
	TBuf<200> t(_L("Undone - "));
	if (iUndoDoing.iSubject.Length() && iUndoDoing.iUid == aCmd.uid)
		{
		t.Append('"');
		t.Append(iUndoDoing.iSubject);
		t.Append(_L("\" is back in "));
		}
	else
		t.Append(_L("the message is back in "));
	t.Append(name);
	iEikonEnv->InfoMsg(t);
	SetStatus(t.Left(t.Length() < 120 ? t.Length() : 120));
	}
