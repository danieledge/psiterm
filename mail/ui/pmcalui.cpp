/* pmcalui.cpp - PsiMail's calendar screens (see pmui.h)
 *
 * Beside the folder column: the month, a strip with the week's seven days
 * (today in a dark circle, dots for how busy each day is), then the chosen
 * day - all-day events as soft chips, the rest as rows with their times, a
 * bar in the calendar's grey, the place, and a line marking "now". */
#include "pmui.h"
#include "pmfonts.h"

enum
	{
	KSide = 156,
	KHead = 36,
	KStripTop = 36,
	KStripH = 50,
	KDayTop = 87,              /* the day's title */
	KListTop = 108,
	KRowTimed = 32,
	KRowAllDay = 24,
	KBar = 28,
	KTimeX = KSide + 16,       /* the time column */
	KAccentX = KSide + 62,     /* the calendar's bar */
	KTitleX = KSide + 74
	};

static const char* const KWeekday[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static const char* const KMonth[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* the calendars' greys: bars, and (lighter) all-day chips */
static int shade_bar(int s) { static const signed char k[4] = { 2, 6, 9, 4 }; return k[s & 3]; }
static int shade_chip(int s) { static const signed char k[4] = { 12, 13, 14, 13 }; return k[s & 3]; }

static int num2(char* b, int v)
	{
	b[0] = (char)('0' + v / 10 % 10);
	b[1] = (char)('0' + v % 10);
	return 2;
	}

static int num(char* b, int v)
	{
	char t[12];
	int n = 0, k = 0;
	if (v <= 0) { b[0] = '0'; return 1; }
	while (v && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
	while (n) b[k++] = t[--n];
	return k;
	}

static int hhmm(char* b, int minutes)
	{
	minutes %= 1440;
	if (minutes < 0) minutes += 1440;
	num2(b, minutes / 60);
	b[2] = ':';
	num2(b + 3, minutes % 60);
	return 5;
	}

static int text_c(PmCanvas* c, const PmFont* f, int cx, int base, const char* s, int n, int grey)
	{
	int w = gfx_text_width(f, s, n);
	gfx_text(c, f, cx - w / 2, base, s, n, grey);
	return w;
	}

static int row_height(const PmUiEvent* e) { return e->allday ? KRowAllDay : KRowTimed; }

int ui_calendar_rows(int aHeight) { return (aHeight - KListTop) / KRowTimed; }

/* ------------------------------------------------------------ the week */

static int strip_col(int w) { return (w - KSide - 1 - 16) / 7; }

static void draw_strip(PmCanvas* c, const PmUiCalendar* k)
	{
	int cw = strip_col(c->w);
	int x0 = KSide + 1 + 8;
	for (int i = 0; i < 7; i++)
		{
		const PmUiDay* d = &k->days[i];
		int x = x0 + i * cw, cx = x + cw / 2;
		int sel = i == k->daySel;
		int weekend = d->wday >= 5;
		if (sel)
			gfx_round(c, x + 3, KStripTop + 3, cw - 6, KStripH - 5, 8, k->focus ? 12 : 13);
		/* the weekday, or the month on the 1st */
		const char* wl = d->month1 ? KMonth[d->mon % 12] : KWeekday[d->wday % 7];
		text_c(c, d->today || d->month1 || sel ? &KFontS11 : &KFontR11, cx, KStripTop + 15, wl, 3,
			d->today || sel ? 1 : d->month1 ? 2 : weekend ? 8 : 6);
		char b[4];
		int n = num(b, d->mday);
		if (d->today)
			{
			gfx_circle(c, 2 * cx, 2 * (KStripTop + 29), 2 * 12, 2);
			text_c(c, &KFontS16, cx, KStripTop + 35, b, n, 15);
			}
		else
			text_c(c, &KFontS16, cx, KStripTop + 35, b, n, sel ? 0 : weekend ? 5 : 1);
		/* how busy: up to three dots */
		int dots = d->count > 3 ? 3 : d->count;
		for (int j = 0; j < dots; j++)
			gfx_circle(c, 2 * (cx - (dots - 1) * 3 + j * 6), 2 * (KStripTop + 45), 3, sel ? 3 : 6);
		}
	gfx_hline(c, KSide + 1, KStripTop + KStripH, c->w - KSide - 1, 13);
	}

/* ------------------------------------------------------------ the events */

static void draw_allday(PmCanvas* c, const PmUiEvent* e, int y, int sel, int focus)
	{
	int x1 = c->w - 14;
	const char* lab = "All day";
	gfx_text(c, &KFontR11, KTimeX, y + 16, lab, 7, sel && focus ? 3 : 6);
	int bg = sel ? (focus ? 3 : 11) : shade_chip(e->shade);
	int fg = sel && focus ? 15 : 0;
	gfx_round(c, KAccentX - 2, y + 3, x1 - KAccentX + 2, KRowAllDay - 5, 6, bg);
	gfx_round(c, KAccentX + 1, y + 6, 3, KRowAllDay - 11, 1, sel && focus ? 15 : shade_bar(e->shade));
	int right = x1 - 8;
	if (e->days > 1)
		{
		char b[20];
		int n = 0;
		n += num(b, e->days);
		const char* t = " days";
		while (*t) b[n++] = *t++;
		int w = gfx_text_width(&KFontR11, b, n);
		gfx_text(c, &KFontR11, right - w, y + 16, b, n, sel && focus ? 13 : 6);
		right -= w + 8;
		}
	if (e->flags & KEvRepeat)
		{
		right -= 14;
		gfx_icon(c, &KFontICON14, EIconRepeat, right, y + 5, sel && focus ? 13 : 6);
		right -= 4;
		}
	gfx_text_clip(c, &KFontS12, KAccentX + 10, y + 16, e->title, e->tlen, right - KAccentX - 14, fg);
	}

static void draw_timed(PmCanvas* c, const PmUiEvent* e, int y, int sel, int focus, int last)
	{
	int x0 = KSide + 6, x1 = c->w - 6;
	int t1 = 1, t2 = 6, fg = 0, sub = 5, ic = 6, bar = shade_bar(e->shade);
	if (sel)
		{
		if (focus) { gfx_round(c, x0, y + 1, x1 - x0, KRowTimed - 2, 6, 3); t1 = 15; t2 = 12; fg = 15; sub = 12; ic = 12; bar = 15; }
		else gfx_round(c, x0, y + 1, x1 - x0, KRowTimed - 2, 6, 13);
		}
	else if (!last)
		gfx_hline(c, KTitleX, y + KRowTimed - 1, c->w - KTitleX - 12, 14);
	/* times: from and to (or where it came from / goes to) */
	char b[12];
	int n;
	if (e->start < 0) { const char* t = "cont."; n = 5; for (int i = 0; i < n; i++) b[i] = t[i]; }
	else n = hhmm(b, e->start);
	gfx_text(c, &KFontS12, KTimeX, y + 14, b, n, t1);
	if (e->end > e->start)
		{
		if (e->end > 1440) { const char* t = "later"; n = 5; for (int i = 0; i < n; i++) b[i] = t[i]; }
		else n = hhmm(b, e->end);
		gfx_text(c, &KFontR11, KTimeX, y + 27, b, n, t2);
		}
	gfx_round(c, KAccentX, y + 5, 3, KRowTimed - 10, 1, bar);
	/* marks at the right */
	int right = x1 - 10;
	if (e->flags & KEvPending) { right -= 15; gfx_icon(c, &KFontICON14, EIconArrowUpFromLine, right, y + 9, ic); }
	if (e->flags & KEvRepeat) { right -= 15; gfx_icon(c, &KFontICON14, EIconRepeat, right, y + 9, ic); }
	if (e->alarm >= 0) { right -= 15; gfx_icon(c, &KFontICON14, EIconBell, right, y + 9, ic); }
	int tw = right - 8 - KTitleX;
	gfx_text_clip(c, &KFontS12, KTitleX, y + 14, e->title, e->tlen, tw, fg);
	if (e->llen)
		{
		gfx_icon(c, &KFontICON14, EIconMapPin, KTitleX - 2, y + 16, ic);
		gfx_text_clip(c, &KFontR11, KTitleX + 14, y + 27, e->loc, e->llen, tw - 14, sub);
		}
	else if (e->clen)
		gfx_text_clip(c, &KFontR11, KTitleX, y + 27, e->cal, e->clen, tw, sel && focus ? 12 : 7);
	}

/* ------------------------------------------------------------ header */

/* where the header's buttons are (the same for drawing and the pen) */
struct HeadPos { int sync, view, add, next, today, todayW, prev; };

static void head_pos(int w, HeadPos* p)
	{
	p->sync = w - 30;
	p->view = p->sync - 32;
	p->add = p->view - 32;
	p->next = p->add - 38;
	p->todayW = gfx_text_width(&KFontS11, "Today", 5) + 16;
	p->today = p->next - 6 - p->todayW;
	p->prev = p->today - 26;
	}

static void draw_header(PmCanvas* c, const PmUiCalendar* k)
	{
	HeadPos p;
	head_pos(c->w, &p);
	gfx_icon(c, &KFontICON18, EIconCalendarSync, p.sync, 9, 3);
	gfx_icon(c, &KFontICON18, k->month ? EIconCalendar : EIconCalendarDays, p.view, 9, 3);
	gfx_icon(c, &KFontICON18, EIconPlus, p.add, 9, 3);
	gfx_vline(c, p.add - 12, 10, 16, 12);
	gfx_icon(c, &KFontICON18, EIconChevronRight, p.next, 9, 3);
	gfx_round_frame(c, p.today, 8, p.todayW, 20, 6, 10);
	gfx_text(c, &KFontS11, p.today + 8, 22, "Today", 5, 3);
	gfx_icon(c, &KFontICON18, EIconChevronLeft, p.prev, 9, 3);
	int hx = KSide + 16;
	int w = gfx_text_clip(c, &KFontS16, hx, 23, k->title, k->tlen, p.prev - 70 - hx, 0);
	int sx = hx + w + 10;
	if (k->busy) { gfx_icon(c, &KFontICON14, EIconLoader, sx, 11, 5); sx += 18; }
	if (k->statlen) gfx_text_clip(c, &KFontR11, sx, 22, k->status, k->statlen, p.prev - 8 - sx, 5);
	gfx_hline(c, KSide + 1, KHead - 1, c->w - KSide - 1, 13);
	}

/* ------------------------------------------------------------ the month */

enum { KMonthNames = KHead + 13, KGridTop = KHead + 17 };

static int month_row_h(int h) { return (h - KGridTop) / 6; }

static void draw_month(PmCanvas* c, const PmUiCalendar* k)
	{
	int cw = strip_col(c->w);
	int x0 = KSide + 1 + 8;
	int rh = month_row_h(c->h);
	for (int i = 0; i < 7; i++)
		text_c(c, &KFontR11, x0 + i * cw + cw / 2, KMonthNames, KWeekday[i], 3, i >= 5 ? 8 : 6);
	for (int r = 0; r < 6; r++)
		{
		int y = KGridTop + r * rh;
		gfx_hline(c, x0, y, 7 * cw, 13);
		for (int i = 0; i < 7; i++)
			{
			int n = r * 7 + i;
			const PmUiDay* d = &k->mdays[n];
			int x = x0 + i * cw;
			int in = d->mon == k->mThis;
			int sel = n == k->mSel;
			if (sel)
				gfx_round(c, x + 2, y + 2, cw - 4, rh - 3, 6, k->focus ? 3 : 12);
			char b[4];
			int bn = 0;
			int v = d->mday;
			if (v >= 10) b[bn++] = (char)('0' + v / 10);
			b[bn++] = (char)('0' + v % 10);
			int grey = sel && k->focus ? 15 : !in ? 11 : i >= 5 ? 5 : 1;
			if (d->today && !(sel && k->focus))
				{
				gfx_circle(c, 2 * (x + 13), 2 * (y + 9), 2 * 7, 2);
				text_c(c, &KFontS11, x + 13, y + 13, b, bn, 15);
				}
			else
				text_c(c, d->today || sel ? &KFontS11 : &KFontR11, x + 13, y + 13, b, bn, grey);
			/* how busy, and the first thing on */
			if (d->count > 0)
				{
				int dots = d->count > 3 ? 3 : d->count;
				for (int j = 0; j < dots; j++)
					gfx_circle(c, 2 * (x + cw - 10 - j * 6), 2 * (y + 9), 3, sel && k->focus ? 15 : in ? 5 : 11);
				if (k->mtitle[n] && rh >= 26)
					gfx_text_clip(c, &KFontR11, x + 6, y + rh - 3, k->mtitle[n], k->mtlen[n], cw - 10,
						sel && k->focus ? 13 : in ? 4 : 11);
				}
			}
		}
	}

/* the first row to show so that 'sel' is on screen */
static int fix_top(const PmUiCalendar* k, int h)
	{
	int top = k->top < 0 ? 0 : k->top;
	if (k->sel < 0) return top < k->nevents ? top : 0;
	if (k->sel < top) return k->sel;
	for (;;)
		{
		int y = KListTop;
		for (int i = top; i <= k->sel && i < k->nevents; i++) y += row_height(&k->events[i]);
		if (y <= h || top >= k->sel) return top;
		top++;
		}
	}

void ui_calendar(PmCanvas* c, const PmUiCalendar* k)
	{
	gfx_noclip(c);
	gfx_fill(c, KSide + 1, 0, c->w - KSide - 1, c->h, 15);
	ui_sidebar(c, k->side);

	draw_header(c, k);
	if (k->month)
		{
		draw_month(c, k);
		return;
		}
	draw_strip(c, k);

	/* the day */
	int dx = KSide + 16;
	int dw = gfx_text_clip(c, &KFontS13, dx, KDayTop + 15, k->dayTitle, k->dlen, 260, 0);
	if (k->dayIsToday)
		{
		gfx_circle(c, 2 * (dx + dw + 9), 2 * (KDayTop + 10), 3, 6);
		gfx_text(c, &KFontR12, dx + dw + 16, KDayTop + 15, "Today", 5, 5);
		}
	if (k->nevents > 0)
		{
		char b[24];
		int n = num(b, k->nevents);
		const char* t = k->nevents == 1 ? " event" : " events";
		while (*t) b[n++] = *t++;
		gfx_text_right(c, &KFontR11, c->w - 16, KDayTop + 15, b, n, 6);
		}

	gfx_clip(c, KSide + 1, KListTop, c->w, c->h);
	if (k->nevents == 0)
		{
		int cx = KSide + (c->w - KSide) / 2;
		gfx_icon(c, &KFontICON18, k->enabled ? EIconSun : EIconCalendarSync, cx - 9, KListTop + 16, 10);
		int ew = gfx_text_width(&KFontR12, k->empty, k->elen);
		gfx_text(c, &KFontR12, cx - ew / 2, KListTop + 54, k->empty, k->elen, 5);
		if (k->nlen)
			{
			int nw = gfx_text_width(&KFontR11, k->next, k->nlen);
			if (nw > c->w - KSide - 40) nw = c->w - KSide - 40;
			gfx_text_clip(c, &KFontR11, cx - nw / 2, KListTop + 72, k->next, k->nlen, nw, 7);
			}
		gfx_noclip(c);
		return;
		}
	int top = fix_top(k, c->h);
	int y = KListTop;
	int nowY = -1;
	for (int i = top; i < k->nevents && y < c->h; i++)
		{
		const PmUiEvent* e = &k->events[i];
		if (k->now >= 0 && nowY < 0 && !e->allday && e->start > k->now) nowY = y;
		if (e->allday) draw_allday(c, e, y, i == k->sel, k->focus);
		else draw_timed(c, e, y, i == k->sel, k->focus, i == k->nevents - 1);
		y += row_height(e);
		}
	/* now: a line between what's done and what's to come */
	if (k->now >= 0)
		{
		if (nowY < 0 && y < c->h)
			{
			const PmUiEvent* l = &k->events[k->nevents - 1];
			if (!l->allday && l->start <= k->now) nowY = y;
			}
		if (nowY > KListTop && nowY < c->h)
			{
			nowY -= 1;                         /* in the gap between two rows */
			gfx_circle(c, 2 * (KAccentX + 1), 2 * nowY, 7, 1);
			gfx_hline(c, KAccentX + 1, nowY, c->w - KAccentX - 14, 1);
			}
		}
	gfx_noclip(c);
	/* more than fit: a scroll mark */
	int fits = 0;
	for (int i = 0, yy = KListTop; i < k->nevents; i++) { yy += row_height(&k->events[i]); if (yy <= c->h) fits++; }
	if (k->nevents > fits && k->nevents > 0)
		{
		int h = c->h - KListTop - 6;
		int th = h * fits / k->nevents;
		if (th < 8) th = 8;
		int ty = KListTop + 3 + (h - th) * top / (k->nevents - fits > 0 ? k->nevents - fits : 1);
		if (ty + th > c->h - 3) ty = c->h - 3 - th;
		gfx_round(c, c->w - 4, ty, 3, th, 1, 10);
		}
	}

int ui_calendar_hit(int aW, int aH, const PmUiCalendar* k, int x, int y, int* aIndex)
	{
	*aIndex = -1;
	if (x < KSide)
		return ui_sidebar_hit(aH, k->side, x, y, aIndex);
	if (y < KHead)
		{
		HeadPos p;
		head_pos(aW, &p);
		if (x >= p.sync - 4) return EHitSync;
		if (x >= p.view - 4) return EHitMonth;
		if (x >= p.add - 4) return EHitAdd;
		if (x >= p.next - 4) return EHitNext;
		if (x >= p.today) return EHitToday;
		if (x >= p.prev - 4) return EHitPrev;
		return EHitNone;
		}
	if (k->month)
		{
		if (y < KGridTop) return EHitNone;
		int col = (x - KSide - 9) / strip_col(aW);
		int row = (y - KGridTop) / month_row_h(aH);
		if (col >= 0 && col < 7 && row >= 0 && row < 6) { *aIndex = row * 7 + col; return EHitDay; }
		return EHitNone;
		}
	if (y < KStripTop + KStripH)
		{
		int i = (x - KSide - 9) / strip_col(aW);
		if (i >= 0 && i < 7) { *aIndex = i; return EHitDay; }
		return EHitNone;
		}
	if (y < KListTop) return EHitNone;
	int yy = KListTop;
	for (int i = fix_top(k, aH); i < k->nevents; i++)
		{
		int h = row_height(&k->events[i]);
		if (y < yy + h) { *aIndex = i; return EHitRow; }
		yy += h;
		}
	return EHitNone;
	}

/* ------------------------------------------------------------ one event */

static int detail_line(PmCanvas* c, int y, int icon, const char* s, int n, int grey)
	{
	if (n <= 0) return y;
	gfx_icon(c, &KFontICON14, icon, 30, y - 12, 5);
	gfx_text_clip(c, &KFontR13, 54, y, s, n, c->w - 80, grey);
	return y + 24;
	}

void ui_event(PmCanvas* c, const PmUiEventView* v)
	{
	const PmUiEvent* e = v->ev;
	gfx_noclip(c);
	gfx_fill(c, 0, 0, c->w, c->h, 15);
	gfx_hline(c, 12, KBar - 1, c->w - 24, 13);
	gfx_icon(c, &KFontICON14, EIconChevronLeft, 8, 7, 5);
	gfx_text(c, &KFontS12, 24, 18, "Calendar", 8, 4);
	if (e->clen)
		{
		/* which calendar, at the right: a chip in its grey */
		int w = gfx_text_width(&KFontR11, e->cal, e->clen);
		if (w > 200) w = 200;
		int x = c->w - 16 - w - 18;
		gfx_round(c, x, 6, w + 22, 16, 7, shade_chip(e->shade));
		gfx_circle(c, 2 * (x + 9), 2 * 14, 6, shade_bar(e->shade));
		gfx_text_clip(c, &KFontR11, x + 16, 18, e->cal, e->clen, w, 3);
		}

	/* the title, over two lines if it needs them */
	int y = KBar + 34;
	const PmFont* tf = &KFontS20;
	int maxw = c->w - 60;
	int top = y - 22;
	if (gfx_text_width(tf, e->title, e->tlen) <= maxw)
		gfx_text(c, tf, 30, y, e->title, e->tlen, 0);
	else
		{
		int k = gfx_fit(tf, e->title, e->tlen, maxw);
		int b = k;
		while (b > 0 && e->title[b] != ' ') b--;
		if (b == 0) b = k;
		gfx_text(c, tf, 30, y, e->title, b, 0);
		int rest = b;
		while (rest < e->tlen && e->title[rest] == ' ') rest++;
		y += 26;
		gfx_text_clip(c, tf, 30, y, e->title + rest, e->tlen - rest, maxw, 0);
		}
	y += 32;
	y = detail_line(c, y, EIconCalendarDays, v->date, v->dlen, 1);
	y = detail_line(c, y, EIconClock, v->time, v->tmlen, 1);
	y = detail_line(c, y, EIconMapPin, e->loc, e->llen, 1);
	y = detail_line(c, y, EIconBell, v->alarm, v->alen, 3);
	y = detail_line(c, y, EIconRepeat, v->repeat, v->rlen, 3);
	y = detail_line(c, y, EIconArrowUpFromLine, v->note, v->nlen, 5);
	/* the calendar's bar down the left */
	gfx_round(c, 14, top, 4, y - 20 - top, 2, shade_bar(e->shade));
	}
