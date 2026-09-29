/* pmcalmodel.cpp - the calendar's events by day, for the calendar screen
 * (see pmui.h). Reads the engine's files as they are:
 *   calendars.txt  id TAB sync TAB ctag TAB href TAB flags TAB name
 *   events.txt     calid TAB href TAB etag TAB recurid TAB flags TAB start TAB end TAB alarm TAB summary TAB location
 *   push.txt       N TAB key TAB calid TAB start TAB end TAB flags TAB alarm TAB summary TAB location (new on the Psion)
 * Portable: used by PsiMail.app and by uishot on a PC. */
#include "pmui.h"

long cal_days_from(int y, int m, int d)
	{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	long yoe = y - era * 400;
	long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
	}

void cal_date_of(long z, int* y, int* m, int* d)
	{
	z += 719468;
	long era = (z >= 0 ? z : z - 146096) / 146097;
	long doe = z - era * 146097;
	long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	long mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = (int)(yoe + era * 400 + (*m <= 2));
	}

int cal_weekday(long days) { return (int)((days % 7 + 10) % 7); }   /* 1970-01-01: Thursday */

void calm_init(PmCalModel* m)
	{
	char* p = (char*)m;
	for (unsigned int i = 0; i < sizeof(*m); i++) p[i] = 0;
	}

void calm_free(PmCalModel* m)
	{
	ui_free(m->text);
	ui_free(m->cals);
	ui_free(m->items);
	calm_init(m);
	}

/* the fields of a line (tabs), in place */
static int split(char* line, int n, char** f, int* fl, int max)
	{
	int k = 0, start = 0;
	for (int i = 0; i <= n && k < max; i++)
		if (i == n || line[i] == '\t')
			{
			f[k] = line + start;
			fl[k] = i - start;
			k++;
			start = i + 1;
			}
	return k;
	}

static int dig(const char* s, int n, int at, int len)
	{
	int v = 0;
	for (int i = at; i < at + len; i++)
		{
		if (i >= n || s[i] < '0' || s[i] > '9') return -1;
		v = v * 10 + s[i] - '0';
		}
	return v;
	}

/* "YYYYMMDDHHMM" -> day, minute */
static int when(const char* s, int n, long* day, int* minute)
	{
	int y = dig(s, n, 0, 4), mo = dig(s, n, 4, 2), d = dig(s, n, 6, 2), h = dig(s, n, 8, 2), mi = dig(s, n, 10, 2);
	if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
	*day = cal_days_from(y, mo, d);
	*minute = (h < 0 ? 0 : h) * 60 + (mi < 0 ? 0 : mi);
	return 1;
	}

static char* copy(const char* s, int n)
	{
	char* c = (char*)ui_alloc(n + 1);
	if (!c) return 0;
	for (int i = 0; i < n; i++) c[i] = s[i];
	c[n] = 0;
	return c;
	}

static int cal_index(const PmCalModel* m, const char* id, int n)
	{
	for (int i = 0; i < m->ncal; i++)
		{
		const char* a = m->calId[i];
		int k = 0;
		while (k < n && a[k] && a[k] != '\t' && a[k] == id[k]) k++;
		if (k == n && (a[k] == '\t' || a[k] == 0)) return i;
		}
	return -1;
	}

static PmCalItem* add(PmCalModel* m)
	{
	if (m->n >= m->cap)
		{
		int cap = m->cap ? m->cap * 2 : 64;
		PmCalItem* a = (PmCalItem*)ui_alloc(cap * (int)sizeof(PmCalItem));
		if (!a) return 0;
		for (int i = 0; i < m->n; i++) a[i] = m->items[i];
		ui_free(m->items);
		m->items = a;
		m->cap = cap;
		}
	return &m->items[m->n++];
	}

static void item(PmCalModel* m, char** f, int* fl, int calf, int startf, int flagf, int alarmf, int titlef, int pending)
	{
	long d0, d1;
	int s, e;
	if (!when(f[startf], fl[startf], &d0, &s) || !when(f[startf + 1], fl[startf + 1], &d1, &e)) return;
	PmCalItem* it = add(m);
	if (!it) return;
	it->day0 = d0;
	it->day1 = d1 < d0 ? d0 : d1;
	it->start = s;
	it->end = e;
	it->allday = 0;
	it->flags = pending ? KEvPending : 0;
	for (int i = 0; i < fl[flagf]; i++)
		{
		if (f[flagf][i] == 'A') it->allday = 1;
		if (f[flagf][i] == 'R') it->flags |= KEvRepeat;
		}
	/* a timed event that ends at midnight belongs to the day before */
	if (!it->allday && it->day1 > it->day0 && it->end == 0) { it->day1--; it->end = 1440; }
	int a = 0, neg = 0, k = 0;
	if (fl[alarmf] && f[alarmf][0] == '-') { neg = 1; k = 1; }
	for (; k < fl[alarmf]; k++) a = a * 10 + f[alarmf][k] - '0';
	it->alarm = neg ? -1 : a;
	it->title = f[titlef]; it->tlen = fl[titlef];
	it->loc = f[titlef + 1]; it->llen = fl[titlef + 1];
	int ci = cal_index(m, f[calf], fl[calf]);
	it->shade = ci < 0 ? 0 : ci & 3;
	it->cal = ci < 0 ? "" : m->calName[ci];
	it->clen = ci < 0 ? 0 : m->calLen[ci];
	}

static void each_line(char* t, int len, void (*fn)(PmCalModel*, char*, int), PmCalModel* m)
	{
	int start = 0;
	for (int i = 0; i <= len; i++)
		if (i == len || t[i] == '\n')
			{
			int n = i - start;
			if (n > 0 && t[start + n - 1] == '\r') n--;
			if (n > 0 && t[start] != '#') fn(m, t + start, n);
			start = i + 1;
			}
	}

static void cal_line(PmCalModel* m, char* line, int n)
	{
	char* f[6];
	int fl[6];
	if (split(line, n, f, fl, 6) < 6 || m->ncal >= 8) return;
	m->calId[m->ncal] = f[0];
	m->calName[m->ncal] = f[5];
	m->calLen[m->ncal] = fl[5];
	m->ncal++;
	}

static void event_line(PmCalModel* m, char* line, int n)
	{
	char* f[10];
	int fl[10];
	if (split(line, n, f, fl, 10) < 10) return;
	item(m, f, fl, 0, 5, 4, 7, 8, 0);
	}

static void push_line(PmCalModel* m, char* line, int n)
	{
	char* f[9];
	int fl[9];
	if (line[0] != 'N' || split(line, n, f, fl, 9) < 9) return;
	item(m, f, fl, 2, 3, 5, 6, 7, 1);
	}

void calm_load(PmCalModel* m, const char* events, int elen, const char* cals, int clen, const char* push, int plen)
	{
	calm_free(m);
	if (cals && clen > 0 && (m->cals = copy(cals, clen)) != 0)
		each_line(m->cals, clen, cal_line, m);
	/* events and push.txt in one buffer, so both stay alive */
	int total = (events ? elen : 0) + 1 + (push ? plen : 0);
	m->text = (char*)ui_alloc(total + 1);
	if (!m->text) return;
	int k = 0;
	for (int i = 0; events && i < elen; i++) m->text[k++] = events[i];
	m->text[k++] = '\n';
	int pstart = k;
	for (int i = 0; push && i < plen; i++) m->text[k++] = push[i];
	m->text[k] = 0;
	each_line(m->text, pstart - 1, event_line, m);
	each_line(m->text + pstart, k - pstart, push_line, m);
	}

static int on_day(const PmCalItem* it, long day) { return day >= it->day0 && day <= it->day1; }

int calm_count(const PmCalModel* m, long day)
	{
	int n = 0;
	for (int i = 0; i < m->n; i++) if (on_day(&m->items[i], day)) n++;
	return n;
	}

static void to_event(const PmCalItem* it, long day, PmUiEvent* e)
	{
	e->title = it->title; e->tlen = it->tlen;
	e->loc = it->loc; e->llen = it->llen;
	e->cal = it->cal; e->clen = it->clen;
	e->allday = it->allday;
	e->days = (int)(it->day1 - it->day0 + 1);
	e->alarm = it->alarm;
	e->shade = it->shade;
	e->flags = it->flags;
	/* times relative to this day */
	e->start = it->start + (int)(it->day0 - day) * 1440;
	e->end = it->end + (int)(it->day1 - day) * 1440;
	}

static int before(const PmUiEvent* a, const PmUiEvent* b)
	{
	if (a->allday != b->allday) return a->allday;
	if (a->start != b->start) return a->start < b->start;
	return a->end < b->end;
	}

int calm_day(const PmCalModel* m, long day, PmUiEvent* out, int max)
	{
	int n = 0;
	for (int i = 0; i < m->n && n < max; i++)
		{
		if (!on_day(&m->items[i], day)) continue;
		PmUiEvent e;
		to_event(&m->items[i], day, &e);
		/* insertion sort: a day has a handful */
		int j = n++;
		while (j > 0 && before(&e, &out[j - 1])) { out[j] = out[j - 1]; j--; }
		out[j] = e;
		}
	return n;
	}

int calm_next(const PmCalModel* m, long day, int minute)
	{
	int best = -1;
	for (int i = 0; i < m->n; i++)
		{
		const PmCalItem* it = &m->items[i];
		if (it->day0 < day || (it->day0 == day && (it->allday || it->start <= minute))) continue;
		if (best < 0) { best = i; continue; }
		const PmCalItem* b = &m->items[best];
		if (it->day0 < b->day0 || (it->day0 == b->day0 && it->start < b->start)) best = i;
		}
	return best;
	}

/* ------------------------------------------------------------ the screens' words */

static const char* const KDayName[7] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
static const char* const KMonthName[12] = { "January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December" };

struct Str
	{
	char* b; int n, max;
	void s(const char* t) { while (*t && n < max - 1) b[n++] = *t++; b[n] = 0; }
	void s(const char* t, int k) { for (int i = 0; i < k && n < max - 1; i++) b[n++] = t[i]; b[n] = 0; }
	void d(int v)
		{
		char t[12]; int k = 0;
		if (v < 0) { s("-"); v = -v; }
		do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
		while (k && n < max - 1) b[n++] = t[--k];
		b[n] = 0;
		}
	void t(int minutes)
		{
		minutes = ((minutes % 1440) + 1440) % 1440;
		d(minutes / 60);
		s(":");
		if (minutes % 60 < 10) s("0");
		d(minutes % 60);
		}
	};

static void day_title(Str& o, long day, int year)
	{
	int y, m, d;
	cal_date_of(day, &y, &m, &d);
	o.s(KDayName[cal_weekday(day)]);
	o.s(" ");
	o.d(d);
	o.s(" ");
	o.s(KMonthName[m - 1]);
	if (year) { o.s(" "); o.d(y); }
	}

void calm_view(const PmCalModel* m, long today, int now, long sel, PmUiCalendar* k,
               PmUiEvent* ev, int max, PmCalText* t)
	{
	int y, mo, d;
	long monday = sel - cal_weekday(sel);
	cal_date_of(sel, &y, &mo, &d);
	Str o = { t->title, 0, (int)sizeof(t->title) };
	o.s(KMonthName[mo - 1]);
	o.s(" ");
	o.d(y);
	k->title = t->title; k->tlen = o.n;
	for (int i = 0; i < 7; i++)
		{
		long day = monday + i;
		int yy, mm, dd;
		cal_date_of(day, &yy, &mm, &dd);
		PmUiDay* p = &k->days[i];
		p->mday = dd;
		p->wday = i;
		p->mon = mm - 1;
		p->month1 = dd == 1;
		p->today = day == today;
		p->count = calm_count(m, day);
		}
	k->daySel = (int)(sel - monday);
	Str dt = { t->day, 0, (int)sizeof(t->day) };
	day_title(dt, sel, 0);
	k->dayTitle = t->day; k->dlen = dt.n;
	k->dayIsToday = sel == today;
	k->nevents = calm_day(m, sel, ev, max);
	k->events = ev;
	k->now = sel == today ? now : -1;
	/* an empty day: what's next */
	Str e = { t->empty, 0, (int)sizeof(t->empty) };
	e.s(sel == today ? "Nothing on today" : "Nothing on this day");
	k->empty = t->empty; k->elen = e.n;
	Str nx = { t->next, 0, (int)sizeof(t->next) };
	int ni = calm_next(m, sel, sel == today ? now : 1440);
	if (ni >= 0)
		{
		const PmCalItem* it = &m->items[ni];
		nx.s("Next: ");
		if (it->day0 == today + 1) nx.s("tomorrow");
		else
			{
			int yy, mm, dd;
			cal_date_of(it->day0, &yy, &mm, &dd);
			nx.s(KDayName[cal_weekday(it->day0)], 3);
			nx.s(" ");
			nx.d(dd);
			nx.s(" ");
			nx.s(KMonthName[mm - 1], 3);
			}
		if (!it->allday) { nx.s(" at "); nx.t(it->start); }
		nx.s(", ");
		nx.s(it->title, it->tlen);
		}
	k->next = t->next; k->nlen = nx.n;
	}

void calm_event_view(const PmUiEvent* e, long day, PmUiEventView* v, PmCalText* t)
	{
	v->ev = e;
	Str d = { t->day, 0, (int)sizeof(t->day) };
	long first = day + (e->start < 0 ? (e->start - 1439) / 1440 : 0);
	if (e->allday && e->days > 1)
		{
		/* all-day over several days: from - to */
		long d0 = day - (e->start < 0 ? (-e->start + 1439) / 1440 : 0);
		day_title(d, d0, 0);
		d.s(" - ");
		day_title(d, d0 + e->days - 1, 1);
		}
	else day_title(d, first, 1);
	v->date = t->day; v->dlen = d.n;
	Str tm = { t->title, 0, (int)sizeof(t->title) };
	if (e->allday) tm.s(e->days > 1 ? "All day, " : "All day");
	if (e->allday && e->days > 1) { tm.d(e->days); tm.s(" days"); }
	if (!e->allday)
		{
		tm.t(e->start);
		if (e->end > e->start)
			{
			tm.s(" - ");
			tm.t(e->end);
			int mins = e->end - e->start;
			tm.s("   (");
			if (mins >= 60) { tm.d(mins / 60); tm.s(mins >= 120 ? " hours" : " hour"); if (mins % 60) tm.s(" "); }
			if (mins % 60 || mins < 60) { tm.d(mins % 60); tm.s(" min"); }
			tm.s(")");
			}
		}
	v->time = t->title; v->tmlen = tm.n;
	Str a = { t->empty, 0, (int)sizeof(t->empty) };
	if (e->alarm == 0) a.s("Alarm when it starts");
	else if (e->alarm > 0)
		{
		a.s("Alarm ");
		if (e->alarm % 1440 == 0) { a.d(e->alarm / 1440); a.s(e->alarm == 1440 ? " day" : " days"); }
		else if (e->alarm % 60 == 0) { a.d(e->alarm / 60); a.s(e->alarm == 60 ? " hour" : " hours"); }
		else { a.d(e->alarm); a.s(" minutes"); }
		a.s(" before");
		}
	v->alarm = t->empty; v->alen = a.n;
	v->repeat = (e->flags & KEvRepeat) ? "One of a series" : "";
	v->rlen = (e->flags & KEvRepeat) ? 15 : 0;
	v->note = (e->flags & KEvPending) ? "Made on the Psion: goes to the server at the next sync" : "";
	v->nlen = (e->flags & KEvPending) ? 54 : 0;
	}
