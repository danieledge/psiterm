/* cal.h - PsiMail's calendar: CalDAV (caldav.c) over HTTP (http.c), with
 * iCalendar (ics.c), a little XML (xmlscan.c) and time zones (caltz.c).
 *
 * The engine keeps the server's side in <store>cal\ (see caldav.c for the
 * files); PsiMail.app puts those events into the Psion's Agenda and hands
 * back what changed there (push.txt).
 */
#ifndef PM_CAL_H
#define PM_CAL_H

/* ---- dates and time zones (caltz.c). Times are seconds since 1970 UTC
   ("utc") or the same count on the local wall clock ("local"). */
long cal_days(int y, int m, int d);                  /* days since 1970-01-01 */
void cal_civil(long days, int *y, int *m, int *d);
int  cal_wday(long days);                            /* 0 = Sunday */
long cal_utc_to_local(int zone, long utc);
long cal_local_to_utc(int zone, long local);
int  cal_zone_count(void);
/* "20260930T101500Z" / "20260930T101500" / "20260930" -> seconds; sets
   *utc (Z), *date_only (no time); 0 ok, -1 bad */
int  cal_parse_time(const char *s, long *t, int *utc, int *date_only);
void cal_fmt_utc(long utc, char *out);               /* 20260930T101500Z */
void cal_fmt_date(long t, char *out);                /* 20260930 */
void cal_fmt_local(long t, char *out);               /* 202609301015 (the app's form) */
long cal_parse_local(const char *s);                 /* 202609301015 -> local seconds */

/* ---- a small streaming XML reader (xmlscan.c): element names without
   their namespace prefix, text with entities decoded (UTF-8) */
#define XS_DEPTH 12
typedef struct XmlScan XmlScan;
typedef void (*XsEnd)(XmlScan *x, const char *name, const char *text, int len, void *ctx);
struct XmlScan
	{
	int st;                  /* parser state */
	char tag[160];
	int tlen;
	int quote;
	char last;               /* the tag's last character ("/": empty element) */
	char ent[12];
	int elen;
	char path[XS_DEPTH][48];
	int depth;
	char *text;              /* the current element's text */
	int len, max;
	int over;                /* text was cut short */
	XsEnd end;
	void *ctx;
	};
void xs_init(XmlScan *x, char *textbuf, int textmax, XsEnd end, void *ctx);
void xs_feed(XmlScan *x, const char *in, int n);
/* name of the element 'up' levels above the one ending (1 = its parent) */
const char *xs_parent(XmlScan *x, int up);

/* ---- iCalendar (ics.c) */
typedef struct
	{
	char uid[120];
	char summary[200];       /* cp1252 */
	char location[120];      /* cp1252 */
	char recurid[20];        /* RECURRENCE-ID as UTC (or a date), "" if none */
	long start, end;         /* utc, or local for floating / all-day */
	int allday;
	int floating;            /* times without a zone: already local */
	int alarm;               /* minutes before the start, -1 = none */
	int recurring;           /* has RRULE/RDATE (a master) */
	int cancelled;
	} IcsEvent;
/* calls fn for every VEVENT in the (UTF-8) text; returns how many */
int  ics_each(const char *text, int len, void (*fn)(const IcsEvent *e, void *ctx), void *ctx);

/* an event's details from the Psion, for a new event or a change */
typedef struct
	{
	long start, end;         /* local wall-clock seconds */
	int allday;
	int alarm;               /* minutes before, -1 none */
	char summary[200];       /* cp1252 */
	char location[120];      /* cp1252 */
	} IcsChange;
/* A new VCALENDAR (UTF-8) with one event. Returns length, -1 if too big. */
int  ics_new(const IcsChange *c, int zone, const char *uid, char *out, int max);
/* Edits a calendar object (UTF-8, in place in buf of size max): the master
   event if recurid is "", else that instance (added as an exception if the
   object has none). Returns the new length, -1 if impossible. */
int  ics_change(char *buf, int len, int max, const char *recurid, const IcsChange *c, int zone);
/* Removes one instance of a recurring event (EXDATE). Returns new length. */
int  ics_exclude(char *buf, int len, int max, const char *recurid, int zone);

/* ---- HTTP/1.1 over the engine's connection (http.c) */
typedef struct
	{
	int status;
	char etag[100];
	char location[240];
	long length;             /* body bytes received */
	} HttpResp;
typedef void (*HttpSink)(const char *data, int n, void *ctx);
/* where requests go; user/pass for Basic authentication */
void http_target(const char *host, int port, int tls, const char *user, const char *pass);
/* one request; the response body goes to sink (if any). Returns PM_RES_*. */
int  http_request(const char *method, const char *path, const char *headers,
                  const char *body, int blen, HttpResp *r, HttpSink sink, void *ctx,
                  char *why, int whymax);
void http_close(void);

/* ---- CalDAV (caldav.c) */
int  cal_sync(int list_only, char *why, int whymax);

#endif
