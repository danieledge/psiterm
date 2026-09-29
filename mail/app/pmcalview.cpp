// PMCALVIEW.CPP - PsiMail's calendar screen: the week, the day's events,
// one event in full (drawn by ../ui/pmcalui.cpp from the engine's
// events.txt, and new Psion entries still in push.txt)

#include "pmapp.h"

static const char* CStr(const TDesC& aDes) { return (const char*)aDes.Ptr(); }

void CPmView::CalendarToday()
	{
	TTime now;
	now.HomeTime();
	TDateTime d = now.DateTime();
	iCalToday = cal_days_from(d.Year(), d.Month() + 1, d.Day() + 1);
	iCalNow = d.Hour() * 60 + d.Minute();
	}

// (re)reads the calendar's files
void CPmView::LoadCalendarL()
	{
	TBuf<100> dir;
	StoreDir(dir);
	dir.Append(_L("cal\\"));
	TBuf<120> path;
	HBufC* ev = NULL;
	HBufC* cals = NULL;
	HBufC* push = NULL;
	path = dir; path.Append(_L("events.txt"));
	ReadFileL(path, ev, 400000);
	CleanupStack::PushL(ev);
	path = dir; path.Append(_L("calendars.txt"));
	ReadFileL(path, cals, 20000);
	CleanupStack::PushL(cals);
	path = dir; path.Append(_L("push.txt"));
	ReadFileL(path, push, 50000);
	CleanupStack::PushL(push);
	calm_load(&iCalModel,
		ev ? CStr(*ev) : NULL, ev ? ev->Length() : 0,
		cals ? CStr(*cals) : NULL, cals ? cals->Length() : 0,
		push ? CStr(*push) : NULL, push ? push->Length() : 0);
	CleanupStack::PopAndDestroy(3);          // (the model keeps copies)
	iCalLoaded = ETrue;
	CalendarToday();
	}

void CPmView::ShowCalendarL()
	{
	if (iMode == EMessage)
		BackL();
	iMode = ECalendar;
	iSidebar = EFalse;
	iFolderSel = iFolders->Count() + 1;
	LoadCalendarL();
	if (iCalDay == 0)
		iCalDay = iCalToday;
	iCalSel = 0;
	iCalTop = 0;
	Render();
	}

// to another day, aDays from the one shown
void CPmView::CalGoTo(TInt aDays)
	{
	iCalDay += aDays;
	iCalSel = 0;
	iCalTop = 0;
	Render();
	}

void CPmView::FillCalendar(PmUiCalendar& k)
	{
	Mem::FillZ(&k, sizeof(k));
	CalendarToday();
	calm_view(&iCalModel, iCalToday, iCalNow, iCalDay, &k, iCalEvents, 40, &iCalText);
	if (iCalSel >= k.nevents) iCalSel = k.nevents - 1;
	if (iCalSel < 0) iCalSel = k.nevents ? 0 : -1;
	k.sel = iCalSel;
	k.top = iCalTop;
	k.focus = !iSidebar;
	k.enabled = iCal->iCal.enabled;
	if (!k.enabled && k.nevents == 0)
		{
		k.empty = "Calendar sync is off"; k.elen = 20;
		k.next = "Turn it on in Tools > Calendar settings"; k.nlen = 39;
		}
	if (iCalMonth)
		calm_month(&iCalModel, iCalToday, iCalDay, &k);
	k.busy = Busy() || CalendarBusy();
	if (Busy() && iLastProgress.Length()) { k.status = CStr(iLastProgress); k.statlen = iLastProgress.Length(); }
	else if (iStatus.Length()) { k.status = CStr(iStatus); k.statlen = iStatus.Length(); }
	}

void CPmView::RenderCalendar()
	{
	PmUiMailbox m;
	Mem::FillZ(&m, sizeof(m));
	FillSidebar(m);
	PmUiCalendar k;
	FillCalendar(k);
	k.side = &m;
	ui_calendar(&iCanvas, &k);
	}

void CPmView::RenderEvent()
	{
	PmUiCalendar k;
	FillCalendar(k);
	if (iCalSel < 0 || iCalSel >= k.nevents)
		{
		iMode = ECalendar;
		RenderCalendar();
		return;
		}
	PmUiEventView v;
	calm_event_view(&iCalEvents[iCalSel], iCalDay, &v, &iCalText2);
	ui_event(&iCanvas, &v);
	}

TKeyResponse CPmView::CalendarKeyL(TUint aCode)
	{
	if (iSidebar)
		{
		// the folder column works as it does beside the mail
		switch (aCode)
			{
		case EKeyUpArrow: MoveSel(-1); return EKeyWasConsumed;
		case EKeyDownArrow: MoveSel(1); return EKeyWasConsumed;
		case EKeyEnter:
		case EKeyRightArrow:
		case EKeyTab:
			OpenCurrentL();
			return EKeyWasConsumed;
		case EKeyEscape:
			iSidebar = EFalse;
			Render();
			return EKeyWasConsumed;
			}
		return EKeyWasNotConsumed;
		}
	PmUiCalendar k;
	FillCalendar(k);
	if (iCalMonth)
		{
		switch (aCode)
			{
		case EKeyLeftArrow: CalGoTo(-1); break;
		case EKeyRightArrow: CalGoTo(1); break;
		case EKeyUpArrow: CalGoTo(-7); break;
		case EKeyDownArrow: CalGoTo(7); break;
		case EKeyPageUp: MonthStep(-1); break;
		case EKeyPageDown: MonthStep(1); break;
		case EKeyHome: iCalDay = iCalToday; CalGoTo(0); break;
		case EKeyEnter: iCalMonth = EFalse; Render(); break;
		case 'm': case 'M': ToggleMonthL(); break;
		case 'n': case 'N': NewEventL(); break;
		case EKeyTab:
		case EKeyEscape:
			FocusFoldersL();
			break;
		default:
			return EKeyWasNotConsumed;
			}
		return EKeyWasConsumed;
		}
	switch (aCode)
		{
	case 'm':
	case 'M':
		ToggleMonthL();
		break;
	case 'n':
	case 'N':
		NewEventL();
		break;
	case EKeyLeftArrow: CalGoTo(-1); break;
	case EKeyRightArrow: CalGoTo(1); break;
	case EKeyPageUp: CalGoTo(-7); break;
	case EKeyPageDown: CalGoTo(7); break;
	case EKeyHome: iCalDay = iCalToday; CalGoTo(0); break;
	case EKeyUpArrow:
		if (iCalSel > 0) { iCalSel--; if (iCalSel < iCalTop) iCalTop = iCalSel; Render(); }
		break;
	case EKeyDownArrow:
		if (iCalSel < k.nevents - 1) { iCalSel++; Render(); }
		break;
	case EKeyEnter:
		if (k.nevents > 0) { iMode = ECalEvent; Render(); }
		break;
	case EKeyTab:
	case EKeyEscape:
		FocusFoldersL();
		break;
	case 't':
	case 'T':
		iCalDay = iCalToday;
		CalGoTo(0);
		break;
	default:
		return EKeyWasNotConsumed;
		}
	// the list scrolls with the selection (the UI worked out the top)
	return EKeyWasConsumed;
	}

TKeyResponse CPmView::EventKeyL(TUint aCode)
	{
	PmUiCalendar k;
	FillCalendar(k);
	switch (aCode)
		{
	case EKeyEscape:
	case EKeyLeftArrow:
	case EKeyEnter:
		iMode = ECalendar;
		Render();
		break;
	case EKeyUpArrow:
		if (iCalSel > 0) { iCalSel--; Render(); }
		break;
	case EKeyDownArrow:
		if (iCalSel < k.nevents - 1) { iCalSel++; Render(); }
		break;
	default:
		return EKeyWasNotConsumed;
		}
	return EKeyWasConsumed;
	}

void CPmView::CalendarPointerL(const TPoint& aPoint)
	{
	if (iMode == ECalEvent)
		{
		iMode = ECalendar;
		Render();
		return;
		}
	PmUiMailbox m;
	Mem::FillZ(&m, sizeof(m));
	FillSidebar(m);
	PmUiCalendar k;
	FillCalendar(k);
	k.side = &m;
	TInt index = -1;
	switch (ui_calendar_hit(iCanvas.w, iCanvas.h, &k, aPoint.iX, aPoint.iY, &index))
		{
	case EHitFolder:
		iSidebar = ETrue;
		iFolderSel = index;
		OpenCurrentL();
		break;
	case EHitDay:
		if (iCalMonth)
			{
			if (index == k.mSel) { iCalMonth = EFalse; Render(); }
			else CalGoTo(index - k.mSel);
			}
		else CalGoTo(index - k.daySel);
		break;
	case EHitMonth: ToggleMonthL(); break;
	case EHitAdd: NewEventL(); break;
	case EHitPrev: if (iCalMonth) MonthStep(-1); else CalGoTo(-7); break;
	case EHitNext: if (iCalMonth) MonthStep(1); else CalGoTo(7); break;
	case EHitToday: iCalDay = iCalToday; CalGoTo(0); break;
	case EHitSync: CalendarSyncL(); break;
	case EHitRow:
		if (index == iCalSel && !iSidebar) iMode = ECalEvent;
		else { iSidebar = EFalse; iCalSel = index; }
		Render();
		break;
	default:
		break;
		}
	}

// the same day a month on (or the month's last day)
void CPmView::MonthStep(TInt aDir)
	{
	int y, m, d;
	cal_date_of(iCalDay, &y, &m, &d);
	m += aDir;
	if (m > 12) { m = 1; y++; }
	if (m < 1) { m = 12; y--; }
	static const TInt8 KLen[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	TInt len = KLen[m - 1] + (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
	if (d > len) d = len;
	iCalDay = cal_days_from(y, m, d);
	CalGoTo(0);
	}

void CPmView::ToggleMonthL()
	{
	if (iMode != ECalendar && iMode != ECalEvent)
		ShowCalendarL();
	iMode = ECalendar;
	iCalMonth = !iCalMonth;
	iSidebar = EFalse;
	Render();
	}
