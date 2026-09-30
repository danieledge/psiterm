/* pmui.h - the parts of PsiMail's screens that are not EIKON: the calendar
 * model (pmcalmodel.cpp) and the plain text of a message (pmquote.cpp).
 *
 * Portable C++ with no static data, so it also builds on a PC. PsiMail.app
 * (app/) draws everything with EIKON's own controls and fonts; these give
 * it the data.
 *
 * The message text format (from the engine, see engine/html.c):
 *   header lines "Name: value", a blank line, then one block per line:
 *     plain line                 a line of a plain-text message
 *     \x01p text                 paragraph
 *     \x01h1 / h2 / h3 text      headings
 *     \x01l<depth><marker>\x02text   list item ("\x01l1\x95\x02Milk")
 *     \x01q<depth>text           quoted text (depth 1..9)
 *     \x01c text                 preformatted / code
 *     \x01r                      horizontal rule
 *     \x01i alt                  an image (not shown: its description)
 *     \x01s text                 signature line
 *     \x01u<n> url               link n's address (not shown)
 *     empty line                 space between paragraphs
 *   inline: \x11 bold on, \x12 bold off, \x13 italic on, \x14 italic off,
 *           \x15<n>\x16 link n begins, \x17 link ends.
 */
#ifndef PMUI_H
#define PMUI_H

/* memory, from the platform (User::Alloc on the Psion, malloc on a PC) */
void* ui_alloc(int aSize);
void ui_free(void* aPtr);

/* ---- a message as plain text (pmquote.cpp): the body without the block
   codes, for quoting in a reply ("> ") or forwarding; lists keep their
   markers and quotes get their '>'s back. Returns the length written. */
int  pm_plain_text(const char* text, int len, char* out, int max, int quote);

/* ---- the calendar (app/pmcalview.cpp draws it): a week strip and the chosen day's events */

enum { KEvAlarm = 1, KEvRepeat = 2, KEvPending = 4, KEvReadOnly = 8 };

struct PmUiEvent
	{
	const char* title; int tlen;
	const char* loc; int llen;
	const char* cal; int clen;         /* which calendar */
	int allday;
	int start, end;                    /* minutes from the day's midnight (may be <0 or >1440) */
	int days;                          /* how many days it covers (1 = just this one) */
	int alarm;                         /* minutes before, -1 */
	int shade;                         /* the calendar's grey (0..3) */
	unsigned int flags;                /* KEv* */
	};

struct PmUiDay
	{
	int mday;                          /* 1..31 */
	int wday;                          /* 0 = Monday */
	int count;                         /* events that day */
	int today;
	int month1;                        /* the first of a month (shows the month's name) */
	int mon;                           /* 0..11 */
	};

struct PmUiCalendar
	{
	const char* title; int tlen;       /* "September 2026" */
	const char* status; int statlen;
	int busy;
	PmUiDay days[7];                   /* Monday .. Sunday */
	int daySel;                        /* 0..6 */
	const char* dayTitle; int dlen;    /* "Tuesday 29 September" */
	int dayIsToday;
	const PmUiEvent* events; int nevents;   /* the chosen day's, all-day ones first */
	int sel;                           /* chosen event, -1 */
	int top;                           /* first event row shown */
	int now;                           /* minutes since midnight if the day is today, else -1 */
	const char* empty; int elen;       /* "Nothing on" */
	const char* next; int nlen;        /* "Next: Thu 1 Oct, Dentist" */
	int focus;                         /* keys are in the event list (not the sidebar) */
	int enabled;                       /* calendar sync set up */
	/* the month instead of the week and day (calm_month fills these) */
	int month;
	PmUiDay mdays[42];                 /* 6 weeks from the Monday on or before the 1st */
	const char* mtitle[42]; int mtlen[42];   /* each day's first event */
	int mSel;                          /* 0..41 */
	int mThis;                         /* mdays[i].mon == mThis: in the month shown */
	};


/* one event, in full */
struct PmUiEventView
	{
	const PmUiEvent* ev;
	const char* date; int dlen;        /* "Thursday 1 October 2026" */
	const char* time; int tmlen;       /* "10:00 - 11:00  (1 hour)" / "All day" */
	const char* alarm; int alen;       /* "15 minutes before" */
	const char* repeat; int rlen;      /* "Repeats: one of a series" */
	const char* note; int nlen;        /* "Waiting to be sent" ... */
	};

/* ---- the calendar's data (pmcalmodel.cpp): the engine's events.txt (and
   new Psion entries still in push.txt), by day. Days are counted from
   1970-01-01; times are the Psion's wall clock. */
struct PmCalItem
	{
	long day0, day1;                   /* first and last day */
	int start, end;                    /* minutes from the first day's midnight / the last's */
	int allday, alarm, shade;
	unsigned int flags;
	const char* title; int tlen;
	const char* loc; int llen;
	const char* cal; int clen;
	};
struct PmCalModel
	{
	char* text;                        /* copies of the files (the items point in) */
	char* cals;
	PmCalItem* items; int n, cap;
	const char* calName[8]; int calLen[8]; const char* calId[8]; int ncal;
	};
long cal_days_from(int y, int m, int d);     /* m 1..12 */
void cal_date_of(long days, int* y, int* m, int* d);
int  cal_weekday(long days);                  /* 0 = Monday */
void calm_init(PmCalModel* m);
void calm_free(PmCalModel* m);
/* events.txt, calendars.txt, push.txt (any may be 0) */
void calm_load(PmCalModel* m, const char* events, int elen, const char* cals, int clen, const char* push, int plen);
int  calm_count(const PmCalModel* m, long day);
/* the day's events, all-day ones first, then by time */
int  calm_day(const PmCalModel* m, long day, PmUiEvent* out, int max);
/* the next event after day/minute: index, -1 */
int  calm_next(const PmCalModel* m, long day, int minute);
/* the words the screens show, kept here */
struct PmCalText { char title[40]; char day[80]; char empty[48]; char next[120]; };
/* fills the week, the chosen day's events (into ev) and the words; the
   caller adds side, status, busy, sel, top, focus, enabled */
void calm_view(const PmCalModel* m, long today, int now, long sel, PmUiCalendar* k,
               PmUiEvent* ev, int max, PmCalText* t);
void calm_event_view(const PmUiEvent* e, long day, PmUiEventView* v, PmCalText* t);
/* the month around day 'sel' (after calm_view) */
void calm_month(const PmCalModel* m, long today, long sel, PmUiCalendar* k);
int  calm_date_text(long day, char* out, int max);   /* "Thursday 1 October 2026" */

#endif
