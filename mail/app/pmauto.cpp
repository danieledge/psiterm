// PMAUTO.CPP - what PsiMail does by itself (0.74):
//
//  * New mail alert: when a check finds new mail in the Inbox, EIKON's beep
//    and an infoprint ("2 new messages"); from the background the infoprint
//    is the window server's (User::InfoPrint), which shows over whatever
//    program is in front. Preferences > New mail: Sound & message / Message
//    only / Off.
//  * A timed check of the Inbox (Off / every 10, 15, 30 minutes / every
//    hour), only while PsiMail runs. A CPeriodic every 5 minutes counts the
//    time - RTimer::After stops while the Psion is switched off, so it never
//    wakes the machine - and the check waits while the engine is busy
//    (another command, or downloading ahead), is never made while working
//    offline, and connects only if the preference says so (otherwise only
//    while a connection is already open). It is quiet: only new mail is
//    told, not "No new messages" nor a failure.
//  * Sending what waits: when a connection comes up for any reason, or
//    Work offline is turned off, messages waiting in the Outbox go (not
//    drafts, and not ones that failed: those wait for Check mail, so a bad
//    message doesn't go round at every connection). Never while working
//    offline. Preferences > New mail: Send waiting mail when connected.
//
// The settings are bits of TPmSettings::iView (KPmView* in pmapp.h), so
// the settings file keeps its size.

#include <eikdialg.h>
#include <eikchlst.h>
#include "pmapp.h"

const TInt KPmCheckTick = 5 * 60 * 1000000;      // 5 minutes (CPeriodic's interval is 32-bit microseconds)

static TInt CheckMinutes(const TPmSettings& aSettings)
	{
	switch ((aSettings.iView & KPmViewCheckMask) >> KPmViewCheckShift)
		{
	case 1: return 10;
	case 2: return 15;
	case 3: return 30;
	case 4: return 60;
	default: return 0;
		}
	}

TBool CPmView::InBackground() const
	{
	return iCoeEnv->WsSession().GetFocusWindowGroup() != iCoeEnv->RootWin().Identifier();
	}

// ----- the new mail alert ----------------------------------------------------

void CPmView::NewMailAlertL(const PmCmd& aCmd, TBool aAuto)
	{
	TPtrC8 folder((const TUint8*)aCmd.folder);
	if (aCmd.op != PM_CMD_SENDRECV && !(aCmd.op == PM_CMD_SYNC && folder.CompareF(_L8("INBOX")) == 0))
		return;
	TInt n = iShared->new_mail;
	TInt alert = (iSettings->iView & KPmViewAlertMask) >> KPmViewAlertShift;
	if (n <= 0 || alert == 2)
		return;
	if (alert == 0)
		CEikonEnv::Beep();
	TBuf<64> t;
	if (n == 1)
		t = _L("1 new message");
	else
		t.Format(_L("%d new messages"), n);
	if (InBackground())
		{
		// EIKON's infoprint is in PsiMail's window group, behind the program
		// in front: the window server's own shows over it
		t.Append(_L(" - PsiMail"));
		User::InfoPrint(t);
		}
	else if (aAuto)
		iEikonEnv->InfoMsg(t);
	// (in front, a check the user asked for has already said how many came)
	}

// ----- the timed check -------------------------------------------------------

TInt CPmView::CheckCallback(TAny* aSelf)
	{
	TRAPD(err, ((CPmView*)aSelf)->CheckDueL());
	(void)err;
	return 1;
	}

void CPmView::AutoSettingsChanged()
	{
	if (!CheckMinutes(*iSettings))
		{
		delete iCheckTimer;
		iCheckTimer = NULL;
		iCheckTicks = 0;
		return;
		}
	if (iCheckTimer)
		return;                          // (the minutes are looked at each tick)
	TRAPD(err, iCheckTimer = CPeriodic::NewL(CActive::EPriorityIdle));
	if (err == KErrNone)
		iCheckTimer->Start(KPmCheckTick, KPmCheckTick, TCallBack(CheckCallback, this));
	}

void CPmView::CheckDueL()
	{
	TInt minutes = CheckMinutes(*iSettings);
	if (!minutes)
		return;
	if (++iCheckTicks * 5 < minutes)
		return;
	if (iMode == ENoAccount || !iRunning || iSettings->iOffline)
		{
		iCheckTicks = 0;                 // (working offline: never dial)
		return;
		}
	// the engine busy (a command, downloading ahead, the calendar): in 5 minutes
	if (Busy() || iAutoCmd || CalendarBusy())
		return;
	iCheckTicks = 0;
	if (!iShared->online && !(iSettings->iView & KPmViewCheckConnect))
		return;                          // only while connected
	iAutoCmd = iShared->cmd_head + 1;    // (done_seq when it is done)
	Cmd(PM_CMD_SYNC, _L8("INBOX"), 0, KNullDesC8);
	}

// a timed check's result: quiet, except for new mail
TBool CPmView::AutoResultL(const PmCmd& aCmd)
	{
	if (!iAutoCmd || iDoneSeen != iAutoCmd || aCmd.op != PM_CMD_SYNC)
		return EFalse;
	iAutoCmd = 0;
	TInt res = iShared->last_res;
	if (res == PM_RES_OK || res == PM_RES_OFFLINE || res == PM_RES_FAILED)
		ReloadL();
	if (res == PM_RES_OK)
		NewMailAlertL(aCmd, ETrue);
	return ETrue;
	}

// ----- sending what waits ----------------------------------------------------

// Outbox messages to send: not drafts, and not ones that have failed
TInt CPmView::WaitingToSend()
	{
	RFs& fs = iCoeEnv->FsSession();
	TBuf<120> dir;
	OutboxDir(dir);
	TBuf<130> spec(dir);
	spec.Append(_L("*.txt"));
	CDir* list = NULL;
	if (fs.GetDir(spec, KEntryAttNormal, ESortByName, list) != KErrNone || !list)
		return 0;
	TInt n = 0;
	for (TInt i = 0; i < list->Count(); i++)
		{
		const TDesC& name = (*list)[i].iName;
		TFileName p(dir);
		p.Append(name.Left(name.Length() - 4));
		p.Append(_L(".err"));
		TEntry e;
		if (fs.Entry(p, e) == KErrNone)
			continue;                    // failed: it waits for Check mail
		p = dir;
		p.Append(name);
		RFile f;
		if (f.Open(fs, p, EFileRead | EFileShareReadersOnly) != KErrNone)
			continue;
		TBuf8<512> head;
		f.Read(head);
		f.Close();
		TInt end = head.Find(_L8("\n\n"));
		TPtrC8 h = end >= 0 ? head.Left(end + 1) : TPtrC8(head);
		if (h.Find(_L8("\nDraft: 1")) >= 0 || (h.Length() >= 8 && h.Left(8) == _L8("Draft: 1")))
			continue;                    // a draft: not for sending
		n++;
		}
	delete list;
	return n;
	}

void CPmView::SendWaitingL()
	{
	TInt n = WaitingToSend();
	if (!n)
		return;
	Cmd(PM_CMD_SEND, KNullDesC8, 0, KNullDesC8);
	TBuf<60> t;
	if (n == 1)
		t = _L("Sending 1 waiting message...");
	else
		t.Format(_L("Sending %d waiting messages..."), n);
	Working(t);
	}

// from TickL: a connection has come up
void CPmView::AutoTickL()
	{
	PmShared* s = iShared;
	if (iAutoCmd && (TInt)(s->done_seq - iAutoCmd) >= 0)
		iAutoCmd = 0;                    // (its result was passed over in a busy tick)
	TBool up = s->online && !iAutoOnlineWas;
	iAutoOnlineWas = s->online;
	if (!up || iSettings->iOffline || (iSettings->iView & KPmViewNoAutoSend) || iMode == ENoAccount)
		return;
	if (OpInFlight(PM_CMD_SEND) || OpInFlight(PM_CMD_SENDRECV))
		return;                          // (sending already)
	SendWaitingL();
	}

// Work offline has been turned off
void CPmView::LeftOfflineL()
	{
	if (iSettings->iOffline || (iSettings->iView & KPmViewNoAutoSend) || iMode == ENoAccount)
		return;
	if (OpInFlight(PM_CMD_SEND) || OpInFlight(PM_CMD_SENDRECV))
		return;
	SendWaitingL();
	}

// ----- Preferences > New mail ------------------------------------------------

void CPmPrefsDialog::NewMailInitL()
	{
	TInt v = iSettings.iView;
	TInt a = (v & KPmViewAlertMask) >> KPmViewAlertShift;
	SetChoiceListCurrentItem(EPmDlgAlert, a <= 2 ? a : 0);
	TInt c = (v & KPmViewCheckMask) >> KPmViewCheckShift;
	if (c > 4) c = 0;
	SetChoiceListCurrentItem(EPmDlgCheckEvery, c);
	SetChoiceListCurrentItem(EPmDlgCheckConnect, (v & KPmViewCheckConnect) ? 1 : 0);
	// (r_pm_yes_no_array is No, Yes - the one order every yes/no list in
	// the suite has; the stored bits keep their meaning)
	SetChoiceListCurrentItem(EPmDlgAutoSend, (v & KPmViewNoAutoSend) ? 0 : 1);
	SetChoiceListCurrentItem(EPmDlgDetailed, (v & KPmViewDetailedProgress) ? 1 : 0);
	SetLineDimmedNow(EPmDlgCheckConnect, c == 0);
	}

void CPmPrefsDialog::NewMailSave()
	{
	TInt v = iSettings.iView & ~(KPmViewAlertMask | KPmViewCheckMask | KPmViewCheckConnect | KPmViewNoAutoSend | KPmViewDetailedProgress);
	v |= (ChoiceListCurrentItem(EPmDlgAlert) << KPmViewAlertShift) & KPmViewAlertMask;
	v |= (ChoiceListCurrentItem(EPmDlgCheckEvery) << KPmViewCheckShift) & KPmViewCheckMask;
	if (ChoiceListCurrentItem(EPmDlgCheckConnect) == 1)
		v |= KPmViewCheckConnect;
	if (ChoiceListCurrentItem(EPmDlgAutoSend) == 0)      // No: don't send by itself
		v |= KPmViewNoAutoSend;
	if (ChoiceListCurrentItem(EPmDlgDetailed) == 1)      // Yes
		v |= KPmViewDetailedProgress;
	iSettings.iView = v;
	}

// "If not connected" belongs to the timed check: dimmed while it is off
void CPmPrefsDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId == EPmDlgCheckEvery)
		SetLineDimmedNow(EPmDlgCheckConnect, ChoiceListCurrentItem(EPmDlgCheckEvery) == 0);
	}
