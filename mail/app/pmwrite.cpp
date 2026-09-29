// PMWRITE.CPP - writing a message and making an event, as PsiMail
// screens (../ui/pmcompose.cpp draws them) rather than EPOC dialogs.
//
// While one is open the view is in ECompose or EEventEdit and the menu bar
// and hotkeys are the screen's own (psimail.rss r_pm_compose_menubar,
// r_pm_event_menubar).

#include <eikcfdlg.h>
#include <eikmenub.h>
#include <eikon.rsg>
#include "pmapp.h"
#include "pmfonts.h"

static const char* CStr(const TDesC& aDes) { return (const char*)aDes.Ptr(); }

static const TInt16 KAlarmMinutes[7] = { -1, 0, 5, 15, 30, 60, 1440 };
static const char* const KAlarmText[7] = { "No alarm", "Alarm when it starts", "5 minutes before",
	"15 minutes before", "30 minutes before", "1 hour before", "1 day before" };

// ----- the menus of a screen of its own

void CPmView::UseMenus(TBool aOwn, TInt aMenuBar, TInt aHotKeys)
	{
	CEikMenuBar* bar = iEikonEnv->EikAppUi()->MenuBar();
	if (!bar)
		return;
	if (aOwn)
		{
		TRAPD(err, bar->ChangeMenuBarL(aHotKeys, aMenuBar, EFalse));
		}
	else
		{
		TRAPD(err, bar->ChangeMenuBarL(R_PM_HOTKEYS, R_PM_MENUBAR, EFalse));
		}
	}

// ----- compose

void CPmView::ComposeAttachmentsL()
	{
	iCmpNames->Reset();
	iCmpSizes->Reset();
	TInt n = iDraft->iAttach->Count();
	if (n > 8) n = 8;
	for (TInt i = 0; i < n; i++)
		{
		TParsePtrC parse((*iDraft->iAttach)[i]);
		iCmpNames->AppendL(parse.NameAndExt().Left(60));
		TEntry e;
		TBuf<20> size;
		if (iCoeEnv->FsSession().Entry((*iDraft->iAttach)[i], e) == KErrNone)
			{
			if (e.iSize < 1024) size.Format(_L("%d bytes"), e.iSize);
			else size.Format(_L("%d KB"), (e.iSize + 1023) / 1024);
			}
		iCmpSizes->AppendL(size);
		}
	for (TInt j = 0; j < n; j++)
		{
		iCmpAtt[j].name = CStr((*iCmpNames)[j]);
		iCmpAtt[j].len = (*iCmpNames)[j].Length();
		iCmpAtt[j].size = CStr((*iCmpSizes)[j]);
		iCmpAtt[j].slen = (*iCmpSizes)[j].Length();
		}
	// the text starts lower when there are attachments
	PmComposeLayout l;
	ui_compose_layout(iCanvas.w, iCanvas.h, n, &l);
	}

void CPmView::ComposeL(CPmDraft* aDraft, const TDesC& aTitle)
	{
	if (iMode == ECompose || iMode == EEventEdit)
		{
		delete aDraft;
		return;
		}
	iDraft = aDraft;
	iCmpTitle = aTitle.Left(iCmpTitle.MaxLength());
	iCmpReturn = iMode;
	PmComposeLayout l;
	ui_compose_layout(iCanvas.w, iCanvas.h, 0, &l);
	ed_init(&iEd[0], 1, 500, &KFontR12, l.fieldW, 18);
	ed_init(&iEd[1], 1, 500, &KFontR12, l.fieldW, 18);
	ed_init(&iEd[2], 1, 200, &KFontS12, l.fieldW, 18);
	ed_init(&iEd[3], 0, 64000, &KFontR13, l.bodyW, l.lineH);
	iEdOpen = ETrue;
	ed_set(&iEd[0], CStr(iDraft->iTo), iDraft->iTo.Length());
	ed_set(&iEd[1], CStr(iDraft->iCc), iDraft->iCc.Length());
	ed_set(&iEd[2], CStr(iDraft->iSubject), iDraft->iSubject.Length());
	if (iDraft->iBody)
		ed_set(&iEd[3], CStr(*iDraft->iBody), iDraft->iBody->Length());
	iEd[0].cur = iEd[0].len;
	iEd[2].cur = iEd[2].len;
	// a reply starts in the text; a new message at To
	iCmpFocus = iDraft->iTo.Length() ? 3 : 0;
	iCmpChanged = EFalse;
	iCmpDiscard = EFalse;
	ComposeAttachmentsL();
	iMode = ECompose;
	UseMenus(ETrue, R_PM_COMPOSE_MENUBAR, R_PM_COMPOSE_HOTKEYS);
	Render();
	}

// the screen's text back into the draft
void CPmView::ComposeCollect()
	{
	TBuf<500> t;
	t.Copy(TPtrC8((const TUint8*)iEd[0].text, iEd[0].len).Left(500));
	t.Trim();
	iDraft->iTo = t;
	t.Copy(TPtrC8((const TUint8*)iEd[1].text, iEd[1].len).Left(500));
	t.Trim();
	iDraft->iCc = t;
	iDraft->iSubject.Copy(TPtrC8((const TUint8*)iEd[2].text, iEd[2].len).Left(200));
	iDraft->iSubject.Trim();
	HBufC* body = HBufC::New(iEd[3].len + 1);
	if (body)
		{
		body->Des().Copy(TPtrC8((const TUint8*)iEd[3].text, iEd[3].len));
		delete iDraft->iBody;
		iDraft->iBody = body;
		}
	}

// how: 1 send, 2 save to the outbox, 0 throw away
void CPmView::EndComposeL(TInt aHow)
	{
	if (aHow == 1)
		{
		ComposeCollect();
		if (iDraft->iTo.Length() == 0 || iDraft->iTo.Locate('@') < 0)
			{
			Toast(_L("Who is it to? Enter an address"));
			iCmpFocus = 0;
			Render();
			return;
			}
		}
	else if (aHow == 2)
		ComposeCollect();
	CPmDraft* d = iDraft;
	iDraft = NULL;
	CleanupStack::PushL(d);
	for (TInt i = 0; i < 4; i++) ed_free(&iEd[i]);
	iEdOpen = EFalse;
	iMode = iCmpReturn == EMessage || iCmpReturn == EList || iCmpReturn == EOutbox ||
		iCmpReturn == ECalendar ? iCmpReturn : EList;
	if (iMode == EMessage && !iDocValid) iMode = EList;
	UseMenus(EFalse, 0, 0);
	if (aHow)
		SaveDraftL(*d, aHow == 1);
	else
		Toast(_L("Message thrown away"));
	CleanupStack::PopAndDestroy();       // d
	ReloadL();
	}

void CPmView::ComposeAttachL()
	{
	if (iDraft->iAttach->Count() >= 8)
		{
		Toast(_L("At most 8 attachments"));
		return;
		}
	TFileName name;
	name = _L("C:\\Documents\\");
	CEikFileOpenDialog* dlg = new(ELeave) CEikFileOpenDialog(&name);
	if (dlg->ExecuteLD(R_EIK_DIALOG_FILE_OPEN))
		{
		iDraft->iAttach->AppendL(name);
		iCmpChanged = ETrue;
		ComposeAttachmentsL();
		}
	Render();
	}

void CPmView::ComposeRemoveAttachL(TInt aIndex)
	{
	TInt n = iDraft->iAttach->Count();
	if (n == 0)
		return;
	if (aIndex < 0 || aIndex >= n) aIndex = n - 1;
	TBuf<80> t(_L("Took off "));
	TParsePtrC parse((*iDraft->iAttach)[aIndex]);
	t.Append(parse.NameAndExt().Left(60));
	iDraft->iAttach->Delete(aIndex);
	ComposeAttachmentsL();
	Toast(t);
	}

void CPmView::FillCompose(PmUiCompose& k)
	{
	Mem::FillZ(&k, sizeof(k));
	k.title = CStr(iCmpTitle);
	k.tlen = iCmpTitle.Length();
	for (TInt i = 0; i < 3; i++) k.field[i] = &iEd[i];
	k.body = &iEd[3];
	k.focus = iCmpFocus;
	k.att = iCmpAtt;
	k.natt = iCmpNames->Count();
	if (iStatus.Length()) { k.status = CStr(iStatus); k.statlen = iStatus.Length(); }
	}

void CPmView::RenderCompose()
	{
	// the text's width and place depend on the attachments' row
	PmComposeLayout l;
	ui_compose_layout(iCanvas.w, iCanvas.h, iCmpNames->Count(), &l);
	PmUiCompose k;
	FillCompose(k);
	ui_compose(&iCanvas, &k);
	}

static TInt EdKey(TUint aCode)
	{
	switch (aCode)
		{
	case EKeyLeftArrow: return EdLeft;
	case EKeyRightArrow: return EdRight;
	case EKeyUpArrow: return EdUp;
	case EKeyDownArrow: return EdDown;
	case EKeyHome: return EdHome;
	case EKeyEnd: return EdEnd;
	case EKeyPageUp: return EdPgUp;
	case EKeyPageDown: return EdPgDn;
	case EKeyBackspace: return EdBack;
	case EKeyDelete: return EdDel;
		}
	return 0;
	}

static TBool Printable(TUint aCode)
	{
	return aCode >= 32 && aCode < 256 && aCode != EKeyDelete;
	}

TKeyResponse CPmView::ComposeKeyL(TUint aCode, TUint aMods)
	{
	if (aCode != EKeyEscape && iCmpDiscard)
		iCmpDiscard = EFalse;
	PmComposeLayout l;
	ui_compose_layout(iCanvas.w, iCanvas.h, iCmpNames->Count(), &l);
	TInt page = l.bodyH / l.lineH - 1;
	if (page < 1) page = 1;
	PmEditor* e = &iEd[iCmpFocus];
	switch (aCode)
		{
	case EKeyEscape:
		if (!iCmpChanged || iCmpDiscard)
			EndComposeL(iCmpChanged ? 0 : 0);
		else
			{
			iCmpDiscard = ETrue;
			Toast(_L("Esc again to throw it away - Ctrl+D keeps it"));
			}
		return EKeyWasConsumed;
	case EKeyTab:
		if (aMods & EModifierShift) iCmpFocus = iCmpFocus > 0 ? iCmpFocus - 1 : 3;
		else iCmpFocus = iCmpFocus < 3 ? iCmpFocus + 1 : 0;
		if (iCmpFocus < 3) iEd[iCmpFocus].cur = iEd[iCmpFocus].len;
		Render();
		return EKeyWasConsumed;
	case EKeyEnter:
		if (iCmpFocus < 3)
			{
			iCmpFocus++;
			if (iCmpFocus < 3) iEd[iCmpFocus].cur = iEd[iCmpFocus].len;
			}
		else
			{
			ed_key(e, EdEnter, page);
			iCmpChanged = ETrue;
			}
		Render();
		return EKeyWasConsumed;
	case EKeyUpArrow:
		if (iCmpFocus > 0 && (iCmpFocus < 3 || ed_on_first_line(e)))
			{
			iCmpFocus--;
			Render();
			return EKeyWasConsumed;
			}
		break;
	case EKeyDownArrow:
		if (iCmpFocus < 3)
			{
			iCmpFocus++;
			Render();
			return EKeyWasConsumed;
			}
		break;
		}
	TInt k = EdKey(aCode);
	if (k)
		{
		if (ed_key(e, k, page) && (k == EdBack || k == EdDel))
			iCmpChanged = ETrue;
		Render();
		return EKeyWasConsumed;
		}
	if (Printable(aCode))
		{
		ed_char(e, (TInt)aCode);
		iCmpChanged = ETrue;
		Render();
		return EKeyWasConsumed;
		}
	// the Menu key and the like go on to EIKON; typing doesn't leak anywhere
	return aCode >= (TUint)EKeyPrintScreen ? EKeyWasNotConsumed : EKeyWasConsumed;
	}

void CPmView::ComposePointerL(const TPoint& aPoint)
	{
	PmUiCompose k;
	FillCompose(k);
	TInt index = -1;
	switch (ui_compose_hit(iCanvas.w, iCanvas.h, &k, aPoint.iX, aPoint.iY, &index))
		{
	case EHitCancel:
		ComposeKeyL(EKeyEscape, 0);
		break;
	case EHitSend: EndComposeL(1); break;
	case EHitSave: EndComposeL(2); break;
	case EHitAttach:
		if (index >= 0) ComposeRemoveAttachL(index);
		else ComposeAttachL();
		break;
	case EHitField:
		iCmpFocus = index;
		Render();
		break;
	case EHitBody:
		{
		PmComposeLayout l;
		ui_compose_layout(iCanvas.w, iCanvas.h, iCmpNames->Count(), &l);
		iCmpFocus = 3;
		ed_click(&iEd[3], index & 0xffff, index >> 16);
		Render();
		break;
		}
	default:
		break;
		}
	}

// ----- a new event

void CPmView::EventEditTexts()
	{
	char d[48];
	TInt n = calm_date_text(iEvDay, d, sizeof(d));
	iEvDate.Copy(TPtrC8((const TUint8*)d, n));
	iEvFromText.Format(_L("%02d:%02d"), iEvFrom / 60, iEvFrom % 60);
	iEvToText.Format(_L("%02d:%02d"), (iEvTo / 60) % 24, iEvTo % 60);
	const char* a = KAlarmText[iEvAlarm];
	iEvAlarmText.Copy(TPtrC8((const TUint8*)a, User::StringLength((const TUint8*)a)));
	}

void CPmView::NewEventL()
	{
	if (iMode == ECompose || iMode == EEventEdit)
		return;
	CalendarToday();
	iEvDay = (iMode == ECalendar || iMode == ECalEvent) ? iCalDay : iCalToday;
	// the next whole hour today, else nine o'clock
	TInt hour = iEvDay == iCalToday ? iCalNow / 60 + 1 : 9;
	if (hour > 22) hour = 22;
	iEvFrom = hour * 60;
	iEvTo = iEvFrom + 60;
	iEvAllDay = EFalse;
	iEvAlarm = 3;                          // 15 minutes before
	iEvFocus = EvTitle;
	iCmpDiscard = EFalse;
	iCmpReturn = iMode;
	TInt tx, tw, wx, ww;
	ui_event_edit_layout(iCanvas.w, &tx, &tw, &wx, &ww);
	ed_init(&iEd[4], 1, 180, &KFontS20, tw, 28);
	ed_init(&iEd[5], 1, 110, &KFontR13, ww, 20);
	iEvOpen = ETrue;
	// where it will go
	iEvCal.Zero();
	TBuf<100> dir;
	StoreDir(dir);
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(4);
	CleanupStack::PushL(names);
	CDesC8ArrayFlat* ids = new(ELeave) CDesC8ArrayFlat(4);
	CleanupStack::PushL(ids);
	TInt def = -1;
	TRAPD(err, CPmCalSync::CalendarsL(dir, *names, *ids, def));
	if (!iCal->iCal.enabled)
		iEvCal = _L("Into the Agenda (calendar sync is off)");
	else if (def >= 0 && def < names->Count())
		{
		iEvCal = _L("To the Agenda, then ");
		iEvCal.Append((*names)[def].Left(40));
		}
	else
		iEvCal = _L("To the Agenda, then the calendar");
	CleanupStack::PopAndDestroy(2);
	EventEditTexts();
	iMode = EEventEdit;
	UseMenus(ETrue, R_PM_EVENT_MENUBAR, R_PM_COMPOSE_HOTKEYS);
	Render();
	}

void CPmView::FillEventEdit(PmUiEventEdit& k)
	{
	Mem::FillZ(&k, sizeof(k));
	k.title = &iEd[4];
	k.where = &iEd[5];
	k.date = CStr(iEvDate); k.dlen = iEvDate.Length();
	k.allday = iEvAllDay;
	k.from = CStr(iEvFromText); k.fromLen = iEvFromText.Length();
	k.to = CStr(iEvToText); k.toLen = iEvToText.Length();
	k.alarm = CStr(iEvAlarmText); k.alen = iEvAlarmText.Length();
	k.cal = CStr(iEvCal); k.clen = iEvCal.Length();
	k.focus = iEvFocus;
	if (iStatus.Length()) { k.status = CStr(iStatus); k.statlen = iStatus.Length(); }
	}

void CPmView::RenderEventEdit()
	{
	PmUiEventEdit k;
	FillEventEdit(k);
	ui_event_edit(&iCanvas, &k);
	}

void CPmView::EndEventEditL(TBool aSave)
	{
	if (aSave)
		{
		if (iEd[4].len == 0)
			{
			Toast(_L("Give the event a name"));
			iEvFocus = EvTitle;
			Render();
			return;
			}
		int y, m, d;
		cal_date_of(iEvDay, &y, &m, &d);
		TTime start(TDateTime(y, TMonth(m - 1), d - 1, 0, 0, 0, 0));
		TTime end(start);
		if (!iEvAllDay)
			{
			start += TTimeIntervalMinutes(iEvFrom);
			end += TTimeIntervalMinutes(iEvTo);
			}
		TBuf<180> title;
		title.Copy(TPtrC8((const TUint8*)iEd[4].text, iEd[4].len));
		title.Trim();
		TBuf<110> where;
		where.Copy(TPtrC8((const TUint8*)iEd[5].text, iEd[5].len));
		where.Trim();
		TRAPD(err, CPmCalSync::AddToAgendaL(iCal->iAgendaFile, title, where, start, end,
			iEvAllDay, KAlarmMinutes[iEvAlarm]));
		if (err != KErrNone)
			{
			TBuf<80> t;
			if (err == KErrNotFound) t = _L("No Agenda file where Calendar settings say");
			else t.Format(_L("Could not add it to the Agenda (%d)"), err);
			Toast(t);
			return;
			}
		iCalDay = iEvDay;
		}
	ed_free(&iEd[4]);
	ed_free(&iEd[5]);
	iEvOpen = EFalse;
	UseMenus(EFalse, 0, 0);
	iMode = iCmpReturn == ECalEvent ? ECalendar : iCmpReturn;
	if (iMode == EEventEdit || iMode == ECompose) iMode = ECalendar;
	if (aSave)
		{
		// show it where it is
		iMode = ECalendar;
		iCalMonth = EFalse;
		TRAPD(err, LoadCalendarL());
		if (iCal->iCal.enabled)
			{
			Toast(_L("Added to the Agenda; sending it to the calendar"));
			CalendarSyncL();
			}
		else
			Toast(_L("Added to the Agenda"));
		}
	Render();
	}

// the next or previous stop, skipping the times of an all-day event
TInt CPmView::EventNextField(TInt aDir)
	{
	TInt f = iEvFocus;
	do
		{
		f += aDir;
		if (f < 0) f = EvCount - 1;
		if (f >= EvCount) f = 0;
		}
	while (iEvAllDay && (f == EvFrom || f == EvTo));
	return f;
	}

void CPmView::EventStep(TInt aDir)
	{
	switch (iEvFocus)
		{
	case EvDate:
		iEvDay += aDir;
		break;
	case EvAllDay:
		iEvAllDay = !iEvAllDay;
		break;
	case EvFrom:
		{
		// the end moves with the start: the length stays
		TInt len = iEvTo - iEvFrom;
		iEvFrom += aDir * 15;
		if (iEvFrom < 0) iEvFrom = 0;
		if (iEvFrom > 23 * 60 + 45) iEvFrom = 23 * 60 + 45;
		iEvTo = iEvFrom + len;
		break;
		}
	case EvTo:
		iEvTo += aDir * 15;
		if (iEvTo <= iEvFrom) iEvTo = iEvFrom + 15;
		if (iEvTo > iEvFrom + 24 * 60) iEvTo = iEvFrom + 24 * 60;
		break;
	case EvAlarm:
		iEvAlarm += aDir;
		if (iEvAlarm < 0) iEvAlarm = 6;
		if (iEvAlarm > 6) iEvAlarm = 0;
		break;
		}
	EventEditTexts();
	Render();
	}

TKeyResponse CPmView::EventEditKeyL(TUint aCode, TUint aMods)
	{
	if (aCode != EKeyEscape && iCmpDiscard)
		iCmpDiscard = EFalse;
	TBool text = iEvFocus == EvTitle || iEvFocus == EvWhere;
	PmEditor* e = iEvFocus == EvTitle ? &iEd[4] : &iEd[5];
	switch (aCode)
		{
	case EKeyEscape:
		if (iEd[4].len == 0 || iCmpDiscard)
			EndEventEditL(EFalse);
		else
			{
			iCmpDiscard = ETrue;
			Toast(_L("Esc again to leave without saving"));
			}
		return EKeyWasConsumed;
	case EKeyEnter:
		EndEventEditL(ETrue);
		return EKeyWasConsumed;
	case EKeyTab:
	case EKeyDownArrow:
	case EKeyUpArrow:
		iEvFocus = EventNextField(aCode == EKeyUpArrow || (aMods & EModifierShift) ? -1 : 1);
		Render();
		return EKeyWasConsumed;
	case EKeyLeftArrow:
	case EKeyRightArrow:
		if (!text)
			{
			EventStep(aCode == EKeyLeftArrow ? -1 : 1);
			return EKeyWasConsumed;
			}
		break;
	case ' ':
		if (iEvFocus == EvAllDay)
			{
			EventStep(1);
			return EKeyWasConsumed;
			}
		break;
	case '+':
	case '-':
		if (!text)
			{
			EventStep(aCode == '+' ? 1 : -1);
			return EKeyWasConsumed;
			}
		break;
		}
	if (!text)
		return aCode >= (TUint)EKeyPrintScreen ? EKeyWasNotConsumed : EKeyWasConsumed;
	TInt k = EdKey(aCode);
	if (k)
		{
		ed_key(e, k, 1);
		Render();
		return EKeyWasConsumed;
		}
	if (Printable(aCode))
		{
		ed_char(e, (TInt)aCode);
		Render();
		return EKeyWasConsumed;
		}
	return aCode >= (TUint)EKeyPrintScreen ? EKeyWasNotConsumed : EKeyWasConsumed;
	}

void CPmView::EventEditPointerL(const TPoint& aPoint)
	{
	PmUiEventEdit k;
	FillEventEdit(k);
	TInt index = -1;
	switch (ui_event_edit_hit(iCanvas.w, iCanvas.h, &k, aPoint.iX, aPoint.iY, &index))
		{
	case EHitCancel: EventEditKeyL(EKeyEscape, 0); break;
	case EHitSave: EndEventEditL(ETrue); break;
	case EHitPrev: EventStep(-1); break;
	case EHitNext: EventStep(1); break;
	case EHitField:
		if (index == EvAllDay && iEvFocus == EvAllDay) { EventStep(1); break; }
		iEvFocus = index;
		if (index == EvAllDay && !iEvAllDay) { EventStep(1); break; }
		Render();
		break;
	default:
		break;
		}
	}

// ----- commands from the screens' menus and hotkeys

TBool CPmView::ModalCommandL(TInt aCommand)
	{
	if (iMode == ECompose)
		{
		switch (aCommand)
			{
		case EPmCmdSend: EndComposeL(1); return ETrue;
		case EPmCmdSaveDraft: EndComposeL(2); return ETrue;
		case EPmCmdAttachFile: ComposeAttachL(); return ETrue;
		case EPmCmdRemoveAttach: ComposeRemoveAttachL(-1); return ETrue;
		case EPmCmdDiscard: EndComposeL(0); return ETrue;
		case EEikCmdExit: EndComposeL(2); return EFalse;   // kept in the outbox
			}
		return ETrue;                    // the mail commands don't apply here
		}
	if (iMode == EEventEdit)
		{
		switch (aCommand)
			{
		case EPmCmdSend: EndEventEditL(ETrue); return ETrue;
		case EPmCmdDiscard: EndEventEditL(EFalse); return ETrue;
		case EEikCmdExit: return EFalse;
			}
		return ETrue;
		}
	return EFalse;
	}
