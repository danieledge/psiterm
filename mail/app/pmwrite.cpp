// PMWRITE.CPP - Event > Create new event: a standard EIKON dialog (what,
// where, the day, all day or from and to, an alarm) whose entry goes into
// the Agenda, and from there to the calendar server at the next sync.
// (Writing a message is CPmComposeDialog in psimail.cpp.)

#include <eikchlst.h>
#include <eikmfne.h>
#include <eikedwin.h>
#include "pmapp.h"

// the Alarm line's choices (psimail.rss r_pm_alarm_array), in minutes before
static const TInt16 KAlarmMinutes[7] = { -1, 0, 5, 15, 30, 60, 1440 };

void CPmEventDialog::PreLayoutDynInitL()
	{
	SetEdwinTextL(EPmDlgEvTitle, &iEvent.iTitle);
	SetEdwinTextL(EPmDlgEvLoc, &iEvent.iWhere);
	((CEikDateEditor*)Control(EPmDlgEvDate))->SetDate(iEvent.iDate);
	((CEikTimeEditor*)Control(EPmDlgEvStart))->SetTime(iEvent.iStart);
	((CEikTimeEditor*)Control(EPmDlgEvEnd))->SetTime(iEvent.iEnd);
	SetChoiceListCurrentItem(EPmDlgEvAllDay, iEvent.iAllDay ? 1 : 0);
	SetChoiceListCurrentItem(EPmDlgEvAlarm, iEvent.iAlarm);
	SetLabelL(EPmDlgInfo1, iNote);
	TimesDimmed();
	}

// an all-day event has no times: the lines dim rather than go
void CPmEventDialog::TimesDimmed()
	{
	TBool allDay = ChoiceListCurrentItem(EPmDlgEvAllDay) == 1;
	SetLineDimmedNow(EPmDlgEvStart, allDay);
	SetLineDimmedNow(EPmDlgEvEnd, allDay);
	}

void CPmEventDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId == EPmDlgEvAllDay)
		TimesDimmed();
	}

TBool CPmEventDialog::OkToExitL(TInt /*aButtonId*/)
	{
	GetEdwinText(iEvent.iTitle, EPmDlgEvTitle);
	iEvent.iTitle.Trim();
	if (iEvent.iTitle.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No event name entered"));
		TryChangeFocusToL(EPmDlgEvTitle);
		return EFalse;
		}
	GetEdwinText(iEvent.iWhere, EPmDlgEvLoc);
	iEvent.iWhere.Trim();
	iEvent.iDate = ((CEikDateEditor*)Control(EPmDlgEvDate))->Date();
	iEvent.iAllDay = ChoiceListCurrentItem(EPmDlgEvAllDay) == 1;
	iEvent.iStart = ((CEikTimeEditor*)Control(EPmDlgEvStart))->Time();
	iEvent.iEnd = ((CEikTimeEditor*)Control(EPmDlgEvEnd))->Time();
	iEvent.iAlarm = ChoiceListCurrentItem(EPmDlgEvAlarm);
	if (!iEvent.iAllDay && iEvent.iEnd <= iEvent.iStart)
		{
		iEikonEnv->InfoMsg(_L("The end must be after the start"));
		TryChangeFocusToL(EPmDlgEvEnd);
		return EFalse;
		}
	return ETrue;
	}

// minutes into the day of a time editor's TTime
static TInt MinuteOf(const TTime& aTime)
	{
	TDateTime d = aTime.DateTime();
	return d.Hour() * 60 + d.Minute();
	}

void CPmView::NewEventL()
	{
	CalendarToday();
	TPmNewEvent ev;
	TInt day = iMode == ECalendar ? iCalDay : iCalToday;
	int y, m, d;
	cal_date_of(day, &y, &m, &d);
	ev.iDate = TTime(TDateTime(y, TMonth(m - 1), d - 1, 0, 0, 0, 0));
	// the next whole hour today, else nine o'clock
	TInt hour = day == iCalToday ? iCalNow / 60 + 1 : 9;
	if (hour > 22) hour = 22;
	ev.iStart = TTime(TDateTime(y, TMonth(m - 1), d - 1, hour, 0, 0, 0));
	ev.iEnd = TTime(TDateTime(y, TMonth(m - 1), d - 1, hour + 1, 0, 0, 0));
	ev.iAllDay = EFalse;
	ev.iAlarm = 3;                          // 15 minutes before
	// where it will go
	TBuf<80> note;
	TBuf<100> dir;
	StoreDir(dir);
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(4);
	CleanupStack::PushL(names);
	CDesC8ArrayFlat* ids = new(ELeave) CDesC8ArrayFlat(4);
	CleanupStack::PushL(ids);
	TInt def = -1;
	TRAPD(err, CPmCalSync::CalendarsL(dir, *names, *ids, def));
	if (!iCal->iCal.enabled)
		note = _L("Goes into the Agenda (calendar sync is off)");
	else if (def >= 0 && def < names->Count())
		{
		note = _L("Goes into the Agenda, then to ");
		note.Append(Clip((*names)[def], 40));
		}
	else
		note = _L("Goes into the Agenda, then to the calendar");
	CleanupStack::PopAndDestroy(2);

	CPmEventDialog* dlg = new(ELeave) CPmEventDialog(ev, note);
	if (!dlg->ExecuteLD(R_PM_EVENT_DIALOG))
		return;

	TDateTime dd = ev.iDate.DateTime();
	TTime start(TDateTime(dd.Year(), dd.Month(), dd.Day(), 0, 0, 0, 0));
	TTime end(start);
	if (!ev.iAllDay)
		{
		start += TTimeIntervalMinutes(MinuteOf(ev.iStart));
		end += TTimeIntervalMinutes(MinuteOf(ev.iEnd));
		}
	TRAP(err, CPmCalSync::AddToAgendaL(iCal->iAgendaFile, ev.iTitle, ev.iWhere, start, end,
		ev.iAllDay, KAlarmMinutes[ev.iAlarm]));
	if (err != KErrNone)
		{
		TBuf<80> t;
		if (err == KErrNotFound) t = _L("No Agenda file where Calendar settings say");
		else t.Format(_L("Could not add it to the Agenda (%d)"), err);
		Toast(t);
		return;
		}
	// show it where it is
	iCalDay = cal_days_from(dd.Year(), dd.Month() + 1, dd.Day() + 1);
	if (iMode != ECalendar)
		ShowCalendarL();
	iCalMonth = EFalse;
	iSidebar = EFalse;
	TRAP(err, LoadCalendarL());
	if (iCal->iCal.enabled)
		{
		Toast(_L("Added to the Agenda - sending it to the calendar"));
		CalendarSyncL();
		}
	else
		Toast(_L("Added to the Agenda"));
	Render();
	}
