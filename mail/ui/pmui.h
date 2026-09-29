/* pmui.h - PsiMail's screens, drawn with pmgfx into the 640x240 screen.
 *
 * Portable C++ with no static data: PsiMail.app fills in these structures
 * from the store's files and calls the ui_* functions; mail/ui/uishot.cpp
 * does the same on a PC to make screenshots.
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

#include "pmgfx.h"

/* memory, from the platform (User::Alloc on the Psion, malloc on a PC) */
void* ui_alloc(int aSize);
void ui_free(void* aPtr);

enum TPmFontId { EF_R11, EF_R12, EF_R13, EF_S11, EF_S12, EF_S13, EF_S16, EF_S20, EF_I13, EF_M11, EF_Count };
const PmFont* ui_font(int aId);

/* ---- the mailbox: folders on the left, messages on the right */

enum { KRowUnread = 1, KRowFlagged = 2, KRowAttach = 4, KRowAnswered = 8, KRowDraft = 16, KRowError = 32 };

struct PmUiFolder
	{
	const char* name; int len;
	int kind;                  /* 'I' 'S' 'D' 'T' 'J' 'A' 'N' '-', 'O' outbox */
	int unread;
	int depth;                 /* sub-folder level */
	};

struct PmUiRow
	{
	const char* from; int flen;
	const char* subj; int slen;
	const char* date; int dlen;
	unsigned int flags;
	};

struct PmUiMailbox
	{
	const char* account; int alen;
	const PmUiFolder* folders; int nfolders;
	int folderSel;             /* highlighted folder */
	int folderTop;
	int sidebarFocus;          /* keys move in the folder list */
	const char* title; int tlen;
	const char* subtitle; int sublen;
	const PmUiRow* rows; int nrows;   /* the visible rows, from 'top' */
	int total;                 /* rows in the folder */
	int top;
	int sel;                   /* absolute index */
	const char* empty; int elen;
	const char* status; int statlen;  /* progress or the last result */
	int busy;
	int online;
	int offline;
	};

int  ui_mailbox_rows(int aHeight);           /* message rows that fit */
int  ui_sidebar_rows(int aHeight);
void ui_mailbox(PmCanvas* c, const PmUiMailbox* m);

/* what the pen touched */
enum { EHitNone, EHitFolder, EHitRow, EHitRefresh, EHitNew, EHitSearch, EHitBack, EHitReply,
       EHitReplyAll, EHitForward, EHitDelete, EHitArchive, EHitFlag, EHitLink, EHitAttach,
       EHitWeb, EHitCalendar, EHitTop, EHitBottom, EHitDay, EHitPrev, EHitNext, EHitToday,
       EHitSync, EHitMonth, EHitAdd, EHitSend, EHitSave, EHitField, EHitBody, EHitCancel };
int  ui_mailbox_hit(int aW, int aH, const PmUiMailbox* m, int x, int y, int* aIndex);
/* the folder column alone (also beside the calendar) */
void ui_sidebar(PmCanvas* c, const PmUiMailbox* m);


/* ---- the calendar (pmcalui.cpp): a week strip and the chosen day's events */

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
	const PmUiMailbox* side;           /* the folder column (its folders, account, focus) */
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

int  ui_calendar_rows(int aHeight);          /* event rows that fit */
void ui_calendar(PmCanvas* c, const PmUiCalendar* k);
int  ui_calendar_hit(int aW, int aH, const PmUiCalendar* k, int x, int y, int* aIndex);

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
void ui_event(PmCanvas* c, const PmUiEventView* v);

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



/* ---- a message */

struct PmDocOp
	{
	unsigned char type;        /* EOpText... */
	unsigned char font;
	unsigned char grey;
	unsigned char extra;       /* radius, icon... */
	short x, w, h;
	int y;                     /* text: baseline; others: top */
	int off, len;              /* text: in the message */
	short link;                /* 0 = none; n = link n; -n-1000 = attachment n */
	};
enum { EOpText, EOpFill, EOpRound, EOpFrame, EOpLine, EOpCircle, EOpIcon, EOpUnderline };

struct PmDoc
	{
	PmDocOp* ops; int nops, cap;
	int height;                /* of the whole document */
	int nlinks;
	int* linkOff; int* linkLen;       /* link n's address: linkOff[n], n from 1 */
	int linkCap;
	int bodyTop;               /* where the text begins (after the header) */
	int html;                  /* the message came as HTML (engine said so) */
	int natt;
	};

struct PmUiAttachment { const char* name; int len; const char* size; int slen; };

/* lays out the message (text stays in the caller's buffer) */
int  doc_build(PmDoc* d, const char* text, int len, int width,
               const PmUiAttachment* att, int natt, int truncatedKb);
void doc_free(PmDoc* d);
/* the text of a header line: returns its length, 0 if missing */
int  doc_header(const char* text, int len, const char* name, const char** value);
/* the message as plain text with "> " added (for replies); returns length */
int  doc_plain(const char* text, int len, char* out, int max, int quote);
int  doc_link_url(const PmDoc* d, const char* text, int link, const char** url);

struct PmUiReader
	{
	const char* text; int len;
	const PmDoc* doc;
	int scroll;                /* pixels */
	int position; int count;   /* "3 of 50" */
	const char* folder; int flen;
	int focusLink;             /* highlighted link/attachment (PmDocOp.link), 0 none */
	const char* status; int statlen;
	int busy;
	int loading;               /* the text is being downloaded */
	const char* subject; int sublen;  /* while loading */
	int flagged;
	int html;                  /* offer "view as web page" */
	const PmUiAttachment* att; int natt;
	};

int  ui_reader_body_height(int aHeight);
void ui_reader(PmCanvas* c, const PmUiReader* r);
int  ui_reader_hit(int aW, int aH, const PmUiReader* r, int x, int y, int* aLink);
/* the next/previous link or attachment on screen after 'from' (0 = first) */
int  ui_reader_next_link(const PmUiReader* r, int aHeight, int from, int dir);

/* ---- first run */
void ui_welcome(PmCanvas* c, const char* line1, int l1, const char* line2, int l2);

/* ---- a small dark message box at the bottom (e.g. "Moved to the Trash") */
void ui_toast(PmCanvas* c, const char* s, int n);

/* ---- a text editor in PsiMail's type (pmedit.cpp) */
struct PmEditor
	{
	char* text; int len, cap, max;
	int cur;                           /* the caret */
	int single;                        /* one line (scrolls sideways) */
	const PmFont* font; int width, lineH;
	int* lines; int nlines, lcap;      /* where the wrapped lines start */
	int top;                           /* first line shown (single: x scroll) */
	int wantX;                         /* up/down keep to this x */
	};
enum { EdLeft = 1, EdRight, EdUp, EdDown, EdHome, EdEnd, EdPgUp, EdPgDn, EdBack, EdDel, EdEnter, EdDocStart, EdDocEnd };
void ed_init(PmEditor* e, int single, int max, const PmFont* f, int width, int lineH);
void ed_free(PmEditor* e);
int  ed_set(PmEditor* e, const char* s, int n);
void ed_layout(PmEditor* e, int width);
int  ed_char(PmEditor* e, int ch);
int  ed_insert(PmEditor* e, const char* s, int n);
int  ed_key(PmEditor* e, int key, int page);  /* 1 if it did something */
int  ed_on_first_line(const PmEditor* e);
int  ed_on_last_line(const PmEditor* e);
int  ed_line_of(const PmEditor* e, int pos);
void ed_draw(PmCanvas* c, PmEditor* e, int x, int y, int h, int grey, int focus);
void ed_click(PmEditor* e, int x, int y);
int  ed_height(const PmEditor* e);

/* ---- writing a message (pmcompose.cpp) */
struct PmUiCompose
	{
	const char* title; int tlen;       /* "New message", "Reply" ... */
	const char* from; int flen;        /* the account */
	PmEditor* field[3];                /* To, Cc, Subject */
	PmEditor* body;
	int focus;                         /* 0..2 a field, 3 the text */
	const PmUiAttachment* att; int natt;
	const char* status; int statlen;
	int busy;
	};
/* where the editors go, for this screen size */
struct PmComposeLayout { int fieldX, fieldW, bodyX, bodyW, bodyTop, bodyH, lineH; };
void ui_compose_layout(int w, int h, int natt, PmComposeLayout* l);
void ui_compose(PmCanvas* c, const PmUiCompose* k);
/* index: the field (EHitField), or x|y<<16 inside the text (EHitBody), or the attachment (EHitAttach) */
int  ui_compose_hit(int aW, int aH, const PmUiCompose* k, int x, int y, int* aIndex);

/* ---- a new event (pmcompose.cpp) */
enum { EvTitle, EvDate, EvAllDay, EvFrom, EvTo, EvWhere, EvAlarm, EvCount };
struct PmUiEventEdit
	{
	PmEditor* title;
	PmEditor* where;
	const char* date; int dlen;
	int allday;
	const char* from; int fromLen;
	const char* to; int toLen;
	const char* alarm; int alen;
	const char* cal; int clen;         /* "Added to the Agenda, then Work" */
	int focus;                         /* Ev* */
	const char* status; int statlen;
	};
void ui_event_edit_layout(int w, int* titleX, int* titleW, int* whereX, int* whereW);
void ui_event_edit(PmCanvas* c, const PmUiEventEdit* k);
/* EHitField (index Ev*), EHitPrev/EHitNext (index Ev*), EHitSave, EHitCancel */
int  ui_event_edit_hit(int aW, int aH, const PmUiEventEdit* k, int x, int y, int* aIndex);

/* ---- starting up */
void ui_splash(PmCanvas* c, const char* s, int n);

#endif
