// PMCALVIEW.CPP - PsiMail's calendar: the week's seven days, the chosen
// day's events, or the month, drawn in the pane beside the EIKON folder tree
// with the same fonts, rows and colours as the mail list (black on white,
// the highlight inverted, headings and buttons on EIKON's button face).
// The data comes from ../ui/pmcalmodel.cpp (the engine's events.txt, and
// new Psion entries still in push.txt).

#include "pmapp.h"
#include "pmicons.h"
#include <eiktxlbx.h>
#include <eikmenub.h>

static const TInt KScrollBarW = 23;      // EIKON's scroll bar width
static const TInt KNavButtonW = 24;      // the < and > buttons

static const TText* const KWeekday[7] = { _S("Mon"), _S("Tue"), _S("Wed"), _S("Thu"), _S("Fri"), _S("Sat"), _S("Sun") };
static const TText* const KMonth[12] = { _S("Jan"), _S("Feb"), _S("Mar"), _S("Apr"), _S("May"), _S("Jun"),
	_S("Jul"), _S("Aug"), _S("Sep"), _S("Oct"), _S("Nov"), _S("Dec") };
static const TText* const KMonthName[12] = { _S("January"), _S("February"), _S("March"), _S("April"),
	_S("May"), _S("June"), _S("July"), _S("August"), _S("September"), _S("October"), _S("November"), _S("December") };
static const TText* const KDayName[7] = { _S("Monday"), _S("Tuesday"), _S("Wednesday"), _S("Thursday"),
	_S("Friday"), _S("Saturday"), _S("Sunday") };

// the pen on the pane: what it touched (CalHit)
enum { ECalHitNone, ECalHitPrev, ECalHitToday, ECalHitNext, ECalHitWeek, ECalHitMonth, ECalHitDay,
	ECalHitRow, ECalHitBar, ECalHitCell };

static void Copy8(TDes& aOut, const char* aText, TInt aLen)
	{
	aOut.Copy(Clip(TPtrC8((const TUint8*)aText, aLen < 0 ? 0 : aLen), aOut.MaxLength()));
	}

// the text fitted to a width, with "..." if it had to be cut
static void Fit(const CFont* aFont, TDes& aText, TInt aWidth)
	{
	if (aFont->TextWidthInPixels(aText) <= aWidth)
		return;
	while (aText.Length() > 1 && aFont->TextWidthInPixels(aText) + aFont->TextWidthInPixels(_L("...")) > aWidth)
		aText.SetLength(aText.Length() - 1);
	aText.Append(_L("..."));
	}

static TInt Baseline(const CFont* aFont, const TRect& aRect)
	{
	return aRect.iTl.iY + (aRect.Height() - aFont->HeightInPixels()) / 2 + aFont->AscentInPixels();
	}

// ----- the data ---------------------------------------------------------------------

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
		ev ? (const char*)ev->Ptr() : NULL, ev ? ev->Length() : 0,
		cals ? (const char*)cals->Ptr() : NULL, cals ? cals->Length() : 0,
		push ? (const char*)push->Ptr() : NULL, push ? push->Length() : 0);
	CleanupStack::PopAndDestroy(3);          // (the model keeps copies)
	iCalLoaded = ETrue;
	CalendarToday();
	}

// the week (or month) around the chosen day, and its events
void CPmView::FillCalendar(PmUiCalendar& k) const
	{
	Mem::FillZ(&k, sizeof(k));
	CPmView* self = (CPmView*)this;          // (the model's buffers live here)
	self->CalendarToday();
	calm_view(&iCalModel, iCalToday, iCalNow, iCalDay, &k, self->iCalEvents, 40, &self->iCalText);
	if (self->iCalSel >= k.nevents) self->iCalSel = k.nevents - 1;
	if (self->iCalSel < 0) self->iCalSel = k.nevents ? 0 : -1;
	k.sel = iCalSel;
	k.top = iCalTop;
	k.focus = !iSidebar;
	k.enabled = iCal->iCal.enabled;
	if (iCalMonth)
		calm_month(&iCalModel, iCalToday, iCalDay, &k);
	}

// the menu bar: the calendar has Event where the mail has Message
void CPmView::UseMenus(TBool aCalendar)
	{
	if (iCalMenus == aCalendar)
		return;
	CEikMenuBar* bar = iEikonEnv->EikAppUi()->MenuBar();
	if (!bar)
		return;
	TRAPD(err, bar->ChangeMenuBarL(R_PM_HOTKEYS, aCalendar ? R_PM_CAL_MENUBAR : R_PM_MENUBAR, EFalse));
	if (err == KErrNone)
		iCalMenus = aCalendar;
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
	UseMenus(ETrue);
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

// View > Switch view (Ctrl+Q): the week and the month, round and round
void CPmView::ToggleMonthL()
	{
	if (iMode != ECalendar)
		{
		ShowCalendarL();
		return;
		}
	iCalMonth = !iCalMonth;
	iSidebar = EFalse;
	Render();
	}

void CPmView::CalendarTodayL()
	{
	if (iMode != ECalendar)
		ShowCalendarL();
	CalendarToday();
	if (iCalDay == iCalToday && !iSidebar)
		{
		iEikonEnv->InfoMsg(_L("Today is already shown"));
		return;
		}
	iSidebar = EFalse;
	iCalDay = iCalToday;
	CalGoTo(0);
	}

TBool CPmView::EventSelected() const
	{
	if (iMode != ECalendar || iCalMonth)
		return EFalse;
	return calm_count(&iCalModel, iCalDay) > 0 && iCalSel >= 0;
	}

// the title band's middle: what today holds
void CPmView::CalendarText(TDes& aMid) const
	{
	aMid.Zero();
	TInt n = iCalLoaded ? calm_count(&iCalModel, iCalToday) : 0;
	if (n == 1) aMid = _L("1 event today");
	else if (n) aMid.Format(_L("%d events today"), n);
	else if (!iCal->iCal.enabled && iCalModel.n == 0) aMid = _L("Calendar sync is off");
	else aMid = _L("Nothing on today");
	}

// ----- where things are ---------------------------------------------------------------

TRect CPmView::CalRect() const
	{
	TRect r = Rect();
	TBool folders = !(iSettings->iView & 4);
	return TRect(r.iTl.iX + (folders ? iSplitX + 1 : 0), r.iTl.iY + iTitleH, r.iBr.iX, r.iBr.iY);
	}

TRect CPmView::CalHeadRect() const
	{
	TRect r = CalRect();
	return TRect(r.iTl.iX, r.iTl.iY, r.iBr.iX, r.iTl.iY + iHeadH);
	}

TRect CPmView::CalStripRect() const
	{
	TRect h = CalHeadRect();
	TInt sh = iSmallFont->HeightInPixels() + iListFont->HeightInPixels() + 11;
	return TRect(h.iTl.iX, h.iBr.iY, h.iBr.iX, h.iBr.iY + sh);
	}

TRect CPmView::CalDayRect() const
	{
	TRect s = CalStripRect();
	return TRect(s.iTl.iX, s.iBr.iY + 1, s.iBr.iX, s.iBr.iY + 1 + RowHeight());
	}

TRect CPmView::CalListRect() const
	{
	TRect d = CalDayRect();
	TRect r = CalRect();
	return TRect(d.iTl.iX, d.iBr.iY, r.iBr.iX, r.iBr.iY);
	}

TInt CPmView::CalRows() const
	{
	TInt h = RowHeight();
	return h > 0 ? CalListRect().Height() / h : 0;
	}

// the buttons along the top of the pane: < Today >, then Week | Month at the
// right (the month's name sits between); returns how many
TInt CPmView::CalHeadButtons(TRect* aRects) const
	{
	TRect h = CalHeadRect();
	TInt y0 = h.iTl.iY, y1 = h.iBr.iY;
	TInt x = h.iTl.iX;
	TInt todayW = iBoldFont->TextWidthInPixels(_L("Today")) + 14;
	aRects[0] = TRect(x, y0, x + KNavButtonW, y1);
	x += KNavButtonW;
	aRects[1] = TRect(x, y0, x + todayW, y1);
	x += todayW;
	aRects[2] = TRect(x, y0, x + KNavButtonW, y1);
	TInt weekW = iBoldFont->TextWidthInPixels(_L("Week")) + 14;
	TInt monthW = iBoldFont->TextWidthInPixels(_L("Month")) + 14;
	aRects[4] = TRect(h.iBr.iX - monthW, y0, h.iBr.iX, y1);
	aRects[3] = TRect(h.iBr.iX - monthW - weekW, y0, h.iBr.iX - monthW, y1);
	return 5;
	}

// the month grid: the row of weekday names, then six weeks of cells
void CPmView::MonthGrid(TRect& aNames, TRect& aGrid, TInt& aCellW, TInt& aCellH) const
	{
	TRect r = CalRect();
	TRect h = CalHeadRect();
	TInt nh = iSmallFont->HeightInPixels() + 4;
	aNames = TRect(r.iTl.iX, h.iBr.iY, r.iBr.iX, h.iBr.iY + nh);
	aCellW = (r.Width() - 1) / 7;
	aCellH = (r.iBr.iY - aNames.iBr.iY - 1) / 6;
	aGrid = TRect(r.iTl.iX, aNames.iBr.iY, r.iTl.iX + 7 * aCellW + 1, aNames.iBr.iY + 6 * aCellH + 1);
	}

// ----- drawing ------------------------------------------------------------------------

// a small filled square: how busy a day is (up to three)
static void DrawMarks(CWindowGc& gc, TInt aCount, TInt aCx, TInt aY, const TRgb& aColor)
	{
	TInt n = aCount > 3 ? 3 : aCount;
	if (n <= 0)
		return;
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(aColor);
	TInt x = aCx - (n * 5 - 2) / 2;
	for (TInt i = 0; i < n; i++)
		gc.DrawRect(TRect(x + i * 5, aY, x + i * 5 + 3, aY + 3));
	}

// a solid frame two pixels wide: today
static void DrawTodayFrame(CWindowGc& gc, const TRect& aRect, const TRgb& aColor)
	{
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(aColor);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.DrawRect(aRect);
	TRect in(aRect);
	in.Shrink(1, 1);
	gc.DrawRect(in);
	}

// the highlight is where the keys are: inverted when the pane has them, a
// dotted frame when the folder tree has them (as EIKON's lists do)
static void DrawFocusFrame(CWindowGc& gc, const TRect& aRect)
	{
	gc.SetPenStyle(CGraphicsContext::EDottedPen);
	gc.SetPenColor(KRgbBlack);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.DrawRect(aRect);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	}

// a triangle pointing left or right: the < and > buttons
static void DrawChevron(CWindowGc& gc, const TRect& aRect, TBool aLeft)
	{
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbBlack);
	TInt cx = aRect.iTl.iX + aRect.Width() / 2, cy = aRect.iTl.iY + aRect.Height() / 2;
	for (TInt k = 0; k < 4; k++)
		{
		TInt x = aLeft ? cx - 2 + k : cx + 2 - k;
		gc.DrawLine(TPoint(x, cy - k), TPoint(x, cy + k + 1));
		}
	}

void CPmView::DrawCalHead(CWindowGc& gc) const
	{
	TRect h = CalHeadRect();
	TRect b[5];
	CalHeadButtons(b);
	// the band, as the column headings
	PmDrawButtonFace(gc, h, EFalse);
	PmDrawButtonFace(gc, b[0], iCalPress == ECalHitPrev);
	DrawChevron(gc, b[0], ETrue);
	PmDrawButtonFace(gc, b[1], iCalPress == ECalHitToday);
	PmDrawButtonFace(gc, b[2], iCalPress == ECalHitNext);
	DrawChevron(gc, b[2], EFalse);
	// Week | Month: the one showing is latched down
	PmDrawButtonFace(gc, b[3], !iCalMonth || iCalPress == ECalHitWeek);
	PmDrawButtonFace(gc, b[4], iCalMonth || iCalPress == ECalHitMonth);
	gc.UseFont(iBoldFont);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	TInt base = Baseline(iBoldFont, h);
	gc.SetPenColor(KRgbBlack);
	gc.DrawText(_L("Today"), TPoint(b[1].iTl.iX + 7, base));
	// a latched button reads white on the dark face
	gc.SetPenColor(!iCalMonth ? KRgbWhite : KRgbBlack);
	gc.DrawText(_L("Week"), TPoint(b[3].iTl.iX + 7, base));
	gc.SetPenColor(iCalMonth ? KRgbWhite : KRgbBlack);
	gc.DrawText(_L("Month"), TPoint(b[4].iTl.iX + 7, base));
	// the month and year in the middle (the short name when the long one won't fit)
	int y, m, d;
	cal_date_of(iCalDay, &y, &m, &d);
	TBuf<40> title;
	TInt x0 = b[2].iBr.iX + 8, x1 = b[3].iTl.iX - 8;
	title.Format(_L("%s %d"), KMonthName[(m - 1) % 12], y);
	if (iBoldFont->TextWidthInPixels(title) > x1 - x0)
		title.Format(_L("%s %d"), KMonth[(m - 1) % 12], y);
	Fit(iBoldFont, title, x1 - x0);
	gc.SetPenColor(KRgbBlack);
	gc.DrawText(title, TPoint(x0, base));
	gc.DiscardFont();
	}

// one event row: the times, the name, where, and marks at the right
void CPmView::DrawCalEvent(CWindowGc& gc, const PmUiEvent& e, const TRect& aRect, TBool aSel) const
	{
	TBool inv = aSel && !iSidebar;
	TRgb fg = inv ? KRgbWhite : KRgbBlack;
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(inv ? KRgbBlack : KRgbWhite);
	gc.DrawRect(aRect);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(fg);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	// the times: "09:00-10:00", "All day", "cont.-13:00" over from yesterday
	TBuf<24> when;
	if (e.allday)
		when = _L("All day");
	else
		{
		if (e.start < 0) when = _L("cont.");
		else when.Format(_L("%02d:%02d"), (e.start / 60) % 24, e.start % 60);
		if (e.end > e.start)
			{
			when.Append('-');
			if (e.end > 1440) when.Append(_L("later"));
			else when.AppendFormat(_L("%02d:%02d"), (e.end / 60) % 24, e.end % 60);
			}
		}
	TInt timeW = iListFont->TextWidthInPixels(_L("00:00-00:00")) + 10;
	gc.UseFont(iListFont);
	TInt base = Baseline(iListFont, aRect);
	gc.DrawText(when, TPoint(aRect.iTl.iX + 4, base));
	// marks at the right: waiting to be sent, one of a series, an alarm
	TInt right = aRect.iBr.iX - 4;
	TInt icons[3];
	TInt n = 0;
	if (e.flags & KEvPending) icons[n++] = EMbmEvPending;
	if (e.flags & KEvRepeat) icons[n++] = EMbmEvRepeat;
	if (e.alarm >= 0) icons[n++] = EMbmEvAlarm;
	if (n && iIcons && iIcons->Count())
		{
		// a white tile under them, so they read on the highlight too
		TInt w = n * 14 + 4;
		if (inv)
			{
			gc.SetPenStyle(CGraphicsContext::ENullPen);
			gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
			gc.SetBrushColor(KRgbWhite);
			gc.DrawRect(TRect(right - w, aRect.iTl.iY + 1, right + 2, aRect.iBr.iY - 1));
			gc.SetBrushStyle(CGraphicsContext::ENullBrush);
			gc.SetPenStyle(CGraphicsContext::ESolidPen);
			}
		TInt y = aRect.iTl.iY + (aRect.Height() - 11) / 2;
		for (TInt i = 0; i < n; i++)
			PmDrawIcon(gc, iIcons, icons[i], TPoint(right - w + 2 + i * 14, y));
		right -= w + 4;
		}
	// the name, then where (in the small font)
	TBuf<160> title;
	Copy8(title, e.title, e.tlen);
	if (!title.Length()) title = _L("(no name)");
	if (e.allday && e.days > 1) title.AppendFormat(_L(" (%d days)"), e.days);
	TInt tx = aRect.iTl.iX + 4 + timeW;
	TInt room = right - tx;
	TBuf<120> where;
	Copy8(where, e.loc, e.llen);
	TInt tw = iListFont->TextWidthInPixels(title);
	if (where.Length())
		{
		TInt ww = iSmallFont->TextWidthInPixels(where);
		// the name has first call on the room, the place what is left
		TInt maxTitle = room - 12 - (ww < room / 3 ? ww : room / 3);
		if (tw > maxTitle) { Fit(iListFont, title, maxTitle); tw = iListFont->TextWidthInPixels(title); }
		}
	else
		Fit(iListFont, title, room);
	gc.DrawText(title, TPoint(tx, base));
	if (where.Length())
		{
		gc.DiscardFont();
		gc.UseFont(iSmallFont);
		TInt wx = tx + tw + 12;
		Fit(iSmallFont, where, right - wx);
		if (right - wx > 20)
			gc.DrawText(where, TPoint(wx, Baseline(iSmallFont, aRect)));
		}
	gc.DiscardFont();
	if (aSel && iSidebar)
		{
		TRect f(aRect);
		f.Shrink(1, 1);
		DrawFocusFrame(gc, f);
		}
	}

void CPmView::DrawCalWeek(CWindowGc& gc, const PmUiCalendar& k) const
	{
	TRect strip = CalStripRect();
	TInt cw = strip.Width() / 7;
	TInt x0 = strip.iTl.iX + (strip.Width() - 7 * cw) / 2;
	TInt smallH = iSmallFont->HeightInPixels();
	for (TInt i = 0; i < 7; i++)
		{
		const PmUiDay& d = k.days[i];
		TRect cell(x0 + i * cw, strip.iTl.iY, x0 + (i + 1) * cw, strip.iBr.iY);
		TBool sel = i == k.daySel;
		TBool inv = sel && !iSidebar;
		gc.SetPenStyle(CGraphicsContext::ENullPen);
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		gc.SetBrushColor(inv ? KRgbBlack : KRgbWhite);
		gc.DrawRect(cell);
		TRgb fg = inv ? KRgbWhite : KRgbBlack;
		gc.SetPenStyle(CGraphicsContext::ESolidPen);
		gc.SetBrushStyle(CGraphicsContext::ENullBrush);
		gc.SetPenColor(fg);
		TInt cx = cell.iTl.iX + cw / 2;
		// the weekday
		gc.UseFont(iSmallFont);
		TPtrC name(KWeekday[d.wday % 7]);
		gc.DrawText(name, TPoint(cx - iSmallFont->TextWidthInPixels(name) / 2, cell.iTl.iY + 3 + iSmallFont->AscentInPixels()));
		gc.DiscardFont();
		// the date, bold on the first of a month and today
		TBuf<8> num;
		num.Num(d.mday);
		if (d.month1) num.AppendFormat(_L(" %s"), KMonth[d.mon % 12]);
		const CFont* f = (d.today || d.month1) ? iBoldFont : iListFont;
		gc.UseFont(f);
		gc.DrawText(num, TPoint(cx - f->TextWidthInPixels(num) / 2, cell.iTl.iY + 4 + smallH + f->AscentInPixels()));
		gc.DiscardFont();
		DrawMarks(gc, d.count, cx, cell.iBr.iY - 5, fg);
		if (d.today && !inv)
			DrawTodayFrame(gc, cell, KRgbBlack);
		if (sel && iSidebar)
			{
			TRect fr(cell);
			fr.Shrink(d.today ? 3 : 1, d.today ? 3 : 1);
			DrawFocusFrame(gc, fr);
			}
		}
	// a line under the strip
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbBlack);
	gc.DrawLine(TPoint(strip.iTl.iX, strip.iBr.iY), TPoint(strip.iBr.iX, strip.iBr.iY));

	// the day's name on a heading band, and how much is on
	TRect day = CalDayRect();
	PmDrawButtonFace(gc, day, EFalse);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenColor(KRgbBlack);
	// "Wednesday 30 September - Today", shortened as the room runs out
	TBuf<24> count;
	if (k.nevents == 1) count = _L("1 event");
	else if (k.nevents) count.Format(_L("%d events"), k.nevents);
	gc.UseFont(iListFont);
	TInt cw2 = iListFont->TextWidthInPixels(count);
	gc.DrawText(count, TPoint(day.iBr.iX - 4 - cw2, Baseline(iListFont, day)));
	gc.DiscardFont();
	int y, m, d;
	cal_date_of(iCalDay, &y, &m, &d);
	TInt wd = cal_weekday(iCalDay) % 7;
	TInt room = day.Width() - 12 - cw2 - (cw2 ? 8 : 0);
	TBuf<60> title;
	for (TInt form = 0; form < 3; form++)
		{
		title.Format(_L("%s %d %s"), form == 0 ? KDayName[wd] : KWeekday[wd], d, form < 2 ? KMonthName[(m - 1) % 12] : KMonth[(m - 1) % 12]);
		if (k.dayIsToday) title.Append(_L(" - Today"));
		if (iBoldFont->TextWidthInPixels(title) <= room)
			break;
		}
	gc.UseFont(iBoldFont);
	Fit(iBoldFont, title, room);
	gc.DrawText(title, TPoint(day.iTl.iX + 4, Baseline(iBoldFont, day)));
	gc.DiscardFont();

	// the events
	TRect list = CalListRect();
	TInt rh = RowHeight();
	TInt rows = CalRows();
	TInt top = iCalTop;
	if (top > k.nevents - rows) top = k.nevents - rows;
	if (top < 0) top = 0;
	if (k.sel >= 0 && k.sel < top) top = k.sel;
	if (k.sel >= 0 && rows > 0 && k.sel >= top + rows) top = k.sel - rows + 1;
	((CPmView*)this)->iCalTop = top;
	TBool bar = k.nevents > rows;
	TRect rowsRect(list.iTl.iX, list.iTl.iY, list.iBr.iX - (bar ? KScrollBarW : 0), list.iBr.iY);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.DrawRect(list);
	if (k.nevents == 0)
		{
		// nothing on: say so, and what is next
		gc.UseFont(iListFont);
		gc.SetBrushStyle(CGraphicsContext::ENullBrush);
		gc.SetPenStyle(CGraphicsContext::ESolidPen);
		gc.SetPenColor(KRgbBlack);
		TBuf<60> a;
		TBuf<130> b;
		if (!k.enabled && iCalModel.n == 0)
			{
			a = _L("Calendar sync is off");
			b = _L("Turn it on in Tools > Calendar settings");
			}
		else
			{
			Copy8(a, k.empty, k.elen);
			Copy8(b, k.next, k.nlen);
			}
		TInt lh = iListFont->HeightInPixels() + 4;
		TInt y = list.iTl.iY + (list.Height() - 2 * lh) / 2;
		if (y < list.iTl.iY + 2) y = list.iTl.iY + 2;
		Fit(iListFont, a, list.Width() - 16);
		gc.DrawText(a, TPoint(list.iTl.iX + (list.Width() - iListFont->TextWidthInPixels(a)) / 2, y + iListFont->AscentInPixels()));
		if (b.Length() && y + 2 * lh <= list.iBr.iY)
			{
			Fit(iListFont, b, list.Width() - 16);
			gc.DrawText(b, TPoint(list.iTl.iX + (list.Width() - iListFont->TextWidthInPixels(b)) / 2, y + lh + iListFont->AscentInPixels()));
			}
		gc.DiscardFont();
		return;
		}
	TInt nowY = -1;
	for (TInt i = top; i < k.nevents && i < top + rows; i++)
		{
		const PmUiEvent& e = k.events[i];
		TRect r(rowsRect.iTl.iX, list.iTl.iY + (i - top) * rh, rowsRect.iBr.iX, list.iTl.iY + (i - top + 1) * rh);
		if (k.now >= 0 && nowY < 0 && !e.allday && e.start > k.now) nowY = r.iTl.iY;
		DrawCalEvent(gc, e, r, i == k.sel);
		}
	// now: a line between what is over and what is to come
	if (k.now >= 0)
		{
		TInt shown = k.nevents - top < rows ? k.nevents - top : rows;
		TInt endY = list.iTl.iY + shown * rh;
		if (nowY < 0 && top + shown == k.nevents && endY < list.iBr.iY)
			{
			const PmUiEvent& l = k.events[k.nevents - 1];
			if (!l.allday && l.start <= k.now) nowY = endY;
			}
		if (nowY > list.iTl.iY && nowY < list.iBr.iY)
			{
			gc.SetPenStyle(CGraphicsContext::ESolidPen);
			gc.SetPenColor(KRgbBlack);
			gc.DrawLine(TPoint(rowsRect.iTl.iX, nowY), TPoint(rowsRect.iBr.iX, nowY));
			gc.DrawLine(TPoint(rowsRect.iTl.iX, nowY - 1), TPoint(rowsRect.iBr.iX, nowY - 1));
			for (TInt t = 0; t < 4; t++)
				gc.DrawLine(TPoint(rowsRect.iTl.iX + t, nowY - 4 + t), TPoint(rowsRect.iTl.iX + t, nowY + 4 - t));
			}
		}
	if (bar)
		{
		TRect br(rowsRect.iBr.iX, list.iTl.iY, list.iBr.iX, list.iBr.iY);
		PmDrawScrollBar(gc, br, k.nevents * rh, rows * rh, top * rh, iBarPress);
		}
	}

void CPmView::DrawCalMonth(CWindowGc& gc, const PmUiCalendar& k) const
	{
	TRect names, grid;
	TInt cw, ch;
	MonthGrid(names, grid, cw, ch);
	TRect r = CalRect();
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.DrawRect(TRect(r.iTl.iX, names.iTl.iY, r.iBr.iX, r.iBr.iY));
	// the weekdays over the columns, in bold
	gc.UseFont(iSmallFont);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbBlack);
	for (TInt i = 0; i < 7; i++)
		{
		TPtrC name(KWeekday[i]);
		TInt cx = grid.iTl.iX + i * cw + cw / 2;
		gc.DrawText(name, TPoint(cx - iSmallFont->TextWidthInPixels(name) / 2, names.iTl.iY + 2 + iSmallFont->AscentInPixels()));
		}
	gc.DiscardFont();
	// the date in the list's size when that leaves room for the day's first
	// event underneath (or the cell is too short for two lines anyway), else
	// in the small one so that the event fits
	TInt smallH = iSmallFont->HeightInPixels();
	TBool twoLines = 2 * smallH + 5 <= ch;
	const CFont* nf = (!twoLines || iListFont->HeightInPixels() + smallH + 5 <= ch) && iListFont->HeightInPixels() + 4 <= ch ? iListFont : iSmallFont;
	TBool titles = twoLines;
	for (TInt row = 0; row < 6; row++)
		for (TInt col = 0; col < 7; col++)
			{
			TInt n = row * 7 + col;
			const PmUiDay& d = k.mdays[n];
			TRect cell(grid.iTl.iX + col * cw + 1, grid.iTl.iY + row * ch + 1, grid.iTl.iX + (col + 1) * cw, grid.iTl.iY + (row + 1) * ch);
			TBool in = d.mon == k.mThis;
			TBool sel = n == k.mSel;
			TBool inv = sel && !iSidebar;
			if (inv)
				{
				gc.SetPenStyle(CGraphicsContext::ENullPen);
				gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
				gc.SetBrushColor(KRgbBlack);
				gc.DrawRect(cell);
				}
			// the date: black in this month, dimmed (dark grey) around it
			TRgb fg = inv ? KRgbWhite : in ? KRgbBlack : KPmDarkGrey;
			gc.SetPenStyle(CGraphicsContext::ESolidPen);
			gc.SetBrushStyle(CGraphicsContext::ENullBrush);
			gc.SetPenColor(fg);
			const CFont* f = d.today && nf == iListFont ? iBoldFont : nf;
			TBuf<4> num;
			num.Num(d.mday);
			gc.UseFont(f);
			gc.DrawText(num, TPoint(cell.iTl.iX + 3, cell.iTl.iY + 2 + f->AscentInPixels()));
			if (d.today && f != iBoldFont)
				gc.DrawText(num, TPoint(cell.iTl.iX + 4, cell.iTl.iY + 2 + f->AscentInPixels()));   // (bolder)
			gc.DiscardFont();
			if (d.count)
				{
				DrawMarks(gc, d.count, cell.iBr.iX - 9, cell.iTl.iY + 3, fg);
				if (titles && k.mtitle[n])
					{
					// the first thing on that day
					TBuf<60> t;
					Copy8(t, k.mtitle[n], k.mtlen[n]);
					Fit(iSmallFont, t, cell.Width() - 6);
					gc.UseFont(iSmallFont);
					gc.SetPenColor(fg);
					gc.DrawText(t, TPoint(cell.iTl.iX + 3, cell.iBr.iY - 3 - iSmallFont->DescentInPixels()));
					gc.DiscardFont();
					}
				}
			if (d.today && !inv)
				DrawTodayFrame(gc, cell, KRgbBlack);
			if (sel && iSidebar)
				{
				TRect fr(cell);
				fr.Shrink(d.today ? 3 : 1, d.today ? 3 : 1);
				DrawFocusFrame(gc, fr);
				}
			}
	// the grid's lines
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbBlack);
	for (TInt col = 0; col <= 7; col++)
		gc.DrawLine(TPoint(grid.iTl.iX + col * cw, grid.iTl.iY), TPoint(grid.iTl.iX + col * cw, grid.iBr.iY));
	for (TInt row = 0; row <= 6; row++)
		gc.DrawLine(TPoint(grid.iTl.iX, grid.iTl.iY + row * ch), TPoint(grid.iBr.iX, grid.iTl.iY + row * ch));
	}

void CPmView::DrawCalendar(CWindowGc& gc) const
	{
	if (iMode != ECalendar || !iListFont)
		return;
	PmUiCalendar k;
	FillCalendar(k);
	gc.SetClippingRect(CalRect());
	DrawCalHead(gc);
	if (iCalMonth)
		DrawCalMonth(gc, k);
	else
		DrawCalWeek(gc, k);
	gc.CancelClippingRect();
	}

// ----- keys ---------------------------------------------------------------------------

TKeyResponse CPmView::CalendarKeyL(TUint aCode)
	{
	PmUiCalendar k;
	FillCalendar(k);
	TBool canFolders = !(iSettings->iView & 4);
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
		case EKeyHome: CalendarTodayL(); break;
		case EKeyEnter: iCalMonth = EFalse; Render(); break;
		case EKeyTab:
			if (canFolders) FocusFoldersL();
			break;
		case EKeyEscape:
			if (canFolders) FocusFoldersL();
			else { iCalMonth = EFalse; Render(); }
			break;
		default:
			return EKeyWasNotConsumed;
			}
		return EKeyWasConsumed;
		}
	TInt rows = CalRows();
	if (rows < 1) rows = 1;
	switch (aCode)
		{
	case EKeyLeftArrow: CalGoTo(-1); break;
	case EKeyRightArrow: CalGoTo(1); break;
	case EKeyPageUp:
		if (k.nevents > rows && iCalSel > 0) { iCalSel -= rows; if (iCalSel < 0) iCalSel = 0; Render(); }
		else CalGoTo(-7);
		break;
	case EKeyPageDown:
		if (k.nevents > rows && iCalSel < k.nevents - 1) { iCalSel += rows; if (iCalSel >= k.nevents) iCalSel = k.nevents - 1; Render(); }
		else CalGoTo(7);
		break;
	case EKeyHome: CalendarTodayL(); break;
	case EKeyEnd: if (k.nevents) { iCalSel = k.nevents - 1; Render(); } break;
	case EKeyUpArrow:
		if (iCalSel > 0) { iCalSel--; if (iCalSel < iCalTop) iCalTop = iCalSel; Render(); }
		break;
	case EKeyDownArrow:
		if (iCalSel < k.nevents - 1) { iCalSel++; Render(); }
		break;
	case EKeyEnter:
		if (k.nevents > 0) EventDetailsL();
		else iEikonEnv->InfoMsg(_L("Nothing on this day"));
		break;
	case EKeyTab:
	case EKeyEscape:
		if (canFolders) FocusFoldersL();
		break;
	case EKeyDelete:
	case EKeyBackspace:
		iEikonEnv->InfoMsg(_L("Events are changed in the Agenda"));
		break;
	default:
		return EKeyWasNotConsumed;
		}
	return EKeyWasConsumed;
	}

// ----- the pen ------------------------------------------------------------------------

// what is under the pen: a button, a day, an event row, the scroll bar
TInt CPmView::CalHit(const TPoint& aPos, TInt& aIndex) const
	{
	aIndex = -1;
	TRect r = CalRect();
	if (!r.Contains(aPos))
		return ECalHitNone;
	TRect b[5];
	CalHeadButtons(b);
	if (CalHeadRect().Contains(aPos))
		{
		for (TInt i = 0; i < 5; i++)
			if (b[i].Contains(aPos))
				return ECalHitPrev + i;
		return ECalHitNone;
		}
	if (iCalMonth)
		{
		TRect names, grid;
		TInt cw, ch;
		MonthGrid(names, grid, cw, ch);
		if (!grid.Contains(aPos) || cw < 1 || ch < 1)
			return ECalHitNone;
		TInt col = (aPos.iX - grid.iTl.iX) / cw, row = (aPos.iY - grid.iTl.iY) / ch;
		if (col > 6) col = 6;
		if (row > 5) row = 5;
		aIndex = row * 7 + col;
		return ECalHitCell;
		}
	TRect strip = CalStripRect();
	if (strip.Contains(aPos))
		{
		TInt cw = strip.Width() / 7;
		TInt x0 = strip.iTl.iX + (strip.Width() - 7 * cw) / 2;
		TInt i = cw > 0 ? (aPos.iX - x0) / cw : 0;
		if (i < 0) i = 0;
		if (i > 6) i = 6;
		aIndex = i;
		return ECalHitDay;
		}
	TRect list = CalListRect();
	if (!list.Contains(aPos))
		return ECalHitNone;
	TInt n = calm_count(&iCalModel, iCalDay);
	TInt rows = CalRows();
	if (n > rows && aPos.iX >= list.iBr.iX - KScrollBarW)
		return ECalHitBar;
	TInt rh = RowHeight();
	TInt i = rh > 0 ? iCalTop + (aPos.iY - list.iTl.iY) / rh : -1;
	if (i < 0 || i >= n)
		return ECalHitNone;
	aIndex = i;
	return ECalHitRow;
	}

// the event list's scroll bar
void CPmView::CalScrollL(TInt aMovement)
	{
	TInt n = calm_count(&iCalModel, iCalDay);
	TInt rows = CalRows();
	TInt top = iCalTop + aMovement;
	if (top > n - rows) top = n - rows;
	if (top < 0) top = 0;
	iCalTop = top;
	// the highlight stays on screen
	if (iCalSel < top) iCalSel = top;
	if (iCalSel >= top + rows) iCalSel = top + rows - 1;
	Render();
	}

// the pen on the pane: ETrue if it was ours
TBool CPmView::CalendarPointerL(const TPointerEvent& aEvent)
	{
	if (iMode != ECalendar)
		return EFalse;
	TInt index;
	TInt hit = CalHit(aEvent.iPosition, index);
	if (aEvent.iType == TPointerEvent::EButton1Down)
		{
		if (hit == ECalHitNone)
			return EFalse;
		if (hit == ECalHitBar)
			{
			// the list's scroll bar: a row or a page at a time
			TRect list = CalListRect();
			TRect br(list.iBr.iX - KScrollBarW, list.iTl.iY, list.iBr.iX, list.iBr.iY);
			TInt n = calm_count(&iCalModel, iCalDay), rows = CalRows(), rh = RowHeight();
			TRect shaft, thumb, up, down;
			PmScrollBarParts(br, n * rh, rows * rh, iCalTop * rh, shaft, thumb, up, down);
			iBarPress = 0;
			if (up.Contains(aEvent.iPosition)) { iBarPress = 1; CalScrollL(-1); }
			else if (down.Contains(aEvent.iPosition)) { iBarPress = 2; CalScrollL(1); }
			else if (aEvent.iPosition.iY < thumb.iTl.iY) CalScrollL(-rows);
			else if (aEvent.iPosition.iY >= thumb.iBr.iY) CalScrollL(rows);
			iCalPress = ECalHitBar;
			return ETrue;
			}
		if (hit >= ECalHitPrev && hit <= ECalHitMonth)
			{
			// a button: show it pressed until the pen lifts
			iCalPress = hit;
			ActivateGc();
			DrawCalHead(SystemGc());
			DeactivateGc();
			return ETrue;
			}
		// a day, a cell, an event: one tap selects, a second tap opens
		iSidebar = EFalse;
		switch (hit)
			{
		case ECalHitDay:
			{
			PmUiCalendar k;
			FillCalendar(k);
			if (index != k.daySel) CalGoTo(index - k.daySel);
			else Render();
			break;
			}
		case ECalHitCell:
			{
			PmUiCalendar k;
			FillCalendar(k);
			if (index == k.mSel) { iCalMonth = EFalse; Render(); }
			else CalGoTo(index - k.mSel);
			break;
			}
		case ECalHitRow:
			if (index == iCalSel) { Render(); EventDetailsL(); }
			else { iCalSel = index; Render(); }
			break;
			}
		return ETrue;
		}
	if (!iCalPress)
		return EFalse;
	if (aEvent.iType == TPointerEvent::EButton1Up)
		{
		TInt was = iCalPress;
		iCalPress = 0;
		iBarPress = 0;
		if (was == ECalHitBar)
			{
			Render();
			return ETrue;
			}
		if (hit != was)
			{
			ActivateGc();
			DrawCalHead(SystemGc());
			DeactivateGc();
			return ETrue;
			}
		iSidebar = EFalse;
		switch (was)
			{
		case ECalHitPrev: if (iCalMonth) MonthStep(-1); else CalGoTo(-7); break;
		case ECalHitNext: if (iCalMonth) MonthStep(1); else CalGoTo(7); break;
		case ECalHitToday: CalendarTodayL(); break;
		case ECalHitWeek: if (iCalMonth) iCalMonth = EFalse; Render(); break;
		case ECalHitMonth: if (!iCalMonth) iCalMonth = ETrue; Render(); break;
			}
		return ETrue;
		}
	return ETrue;                                // (a drag: wait for the pen to lift)
	}

// ----- the chosen event, in full --------------------------------------------------------

void CPmView::EventDetailsL()
	{
	PmUiCalendar k;
	FillCalendar(k);
	if (iMode != ECalendar || iCalMonth || iCalSel < 0 || iCalSel >= k.nevents)
		{
		iEikonEnv->InfoMsg(_L("No event selected"));
		return;
		}
	PmUiEventView v;
	calm_event_view(&iCalEvents[iCalSel], iCalDay, &v, &iCalText2);
	const PmUiEvent& e = *v.ev;
	TBuf<60> title;
	Copy8(title, e.title, e.tlen);
	if (!title.Length()) title = _L("Event");
	TBuf<140> lines[6];
	TInt n = 0;
	Copy8(lines[n], v.date, v.dlen);
	n++;
	Copy8(lines[n], v.time, v.tmlen);
	n++;
	if (e.llen)
		{
		lines[n] = _L("Where: ");
		TBuf<120> w;
		Copy8(w, e.loc, e.llen);
		lines[n].Append(w);
		n++;
		}
	if (v.alen) { Copy8(lines[n], v.alarm, v.alen); n++; }
	if (v.rlen) { Copy8(lines[n], v.repeat, v.rlen); n++; }
	if (e.clen && n < 6)
		{
		lines[n] = _L("Calendar: ");
		TBuf<60> c;
		Copy8(c, e.cal, e.clen);
		lines[n].Append(c);
		n++;
		}
	if (v.nlen && n < 6) { Copy8(lines[n], v.note, v.nlen); n++; }
	TPtrC ptrs[6];
	for (TInt i = 0; i < n; i++) ptrs[i].Set(lines[i]);
	CPmInfoDialog* dlg = new(ELeave) CPmInfoDialog(title, ptrs, n);
	dlg->ExecuteLD(R_PM_INFO_DIALOG);
	}
