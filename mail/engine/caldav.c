/* caldav.c - PsiMail's calendar: CalDAV (RFC 4791) - see cal.h
 *
 * The engine's half of calendar sync. It finds the user's calendars, sends
 * the changes made in the Psion's Agenda and fetches every event in a
 * window around today, expanded by the server into single instances with
 * UTC times. PsiMail.app does the other half: it puts those instances into
 * the Agenda and notices what the user changes there.
 *
 *   <store>cal\calendars.txt  "#PSICAL1 TAB home", then per calendar:
 *                             id TAB sync(0/1) TAB ctag TAB href TAB flags TAB name
 *                             (flags: W writable, D where new events go)
 *   <store>cal\events.txt     "#PSIEV1 TAB first day TAB zone", then per instance:
 *                             calid TAB href TAB etag TAB recurid TAB flags TAB
 *                             start TAB end TAB alarm TAB summary TAB location
 *                             (times local, YYYYMMDDHHMM; flags: A all day,
 *                             R one of a series; alarm: minutes before, -1 none)
 *   <store>cal\push.txt       (app) changes to send, one per line:
 *                             N TAB key TAB calid TAB start TAB end TAB flags TAB alarm TAB summary TAB location
 *                             M TAB key TAB href TAB etag TAB recurid TAB start TAB end TAB flags TAB alarm TAB summary TAB location
 *                             D TAB key TAB href TAB etag TAB recurid
 *   <store>cal\pushed.txt     what became of them: key TAB ok|gone|conflict|failed TAB href
 *
 * id is the FNV hash of the calendar's href. All text is Windows-1252.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm.h"
#include "cal.h"

#define MAX_CALS 24
typedef struct
	{
	char id[12];
	int sync;
	char ctag[100];
	char href[200];
	char flags[8];
	char name[64];
	int seen;                /* in the server's latest list */
	int fetch;               /* events to be fetched this time */
	} Cal;

static Cal g_cal[MAX_CALS];
static int g_ncal;
static char g_home[200];
static char g_where[200];            /* host+path the list came from */

static char g_dir[160];
static char *g_text;                 /* XML element text (calendar data can be big) */
#define TEXT_MAX (160 * 1024)
static char *g_obj;                  /* a calendar object being changed */
#define OBJ_MAX (128 * 1024)

/* ------------------------------------------------------------ helpers */

static unsigned long fnv(const char *s)
{
	unsigned long h = 2166136261UL;
	while (*s) { h ^= (unsigned char)*s++; h = (h * 16777619UL) & 0xffffffffUL; }
	return h;
}

static void path_of(const char *name, char *out, int max) { snprintf(out, max, "%s%s", g_dir, name); }

/* "https://host/a/b/" -> "/a/b/" */
static void just_path(const char *href, char *out, int max)
{
	const char *p = href;
	if (!pm_strncasecmp(p, "http://", 7) || !pm_strncasecmp(p, "https://", 8)) {
		p = strchr(p + 8, '/');
		if (!p) p = "/";
	}
	pm_copy(out, p, max);
}

static void trim(char *s)
{
	int n = (int)strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') memmove(s, s + 1, n--);
}

/* tabs and new lines would break our files */
static void clean(char *s)
{
	for (; *s; s++) if (*s == '\t' || *s == '\r' || *s == '\n') *s = ' ';
}

static char *field(char **p)
{
	char *s = *p, *t;
	if (!s) return (char *)"";
	t = strchr(s, '\t');
	if (t) { *t = 0; *p = t + 1; } else *p = 0;
	return s;
}

/* ------------------------------------------------------------ calendars.txt */

static void load_cals(void)
{
	char path[200], line[600];
	FILE *f;
	g_ncal = 0;
	g_home[0] = 0;
	path_of("calendars.txt", path, sizeof(path));
	if (!(f = fopen(path, "r"))) return;
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "#PSICAL1\t", 9)) {
			char *q = line + 9;
			pm_copy(g_home, field(&q), sizeof(g_home));
			if (strcmp(field(&q), g_where)) { g_home[0] = 0; break; }   /* another server */
			continue;
		}
		if (line[0] == '#' || !line[0] || g_ncal >= MAX_CALS) continue;
		{
			Cal *c = &g_cal[g_ncal++];
			memset(c, 0, sizeof(*c));
			pm_copy(c->id, field(&p), sizeof(c->id));
			c->sync = atoi(field(&p));
			pm_copy(c->ctag, field(&p), sizeof(c->ctag));
			pm_copy(c->href, field(&p), sizeof(c->href));
			pm_copy(c->flags, field(&p), sizeof(c->flags));
			pm_copy(c->name, field(&p), sizeof(c->name));
		}
	}
	fclose(f);
}

static int save_cals(void)
{
	char path[200], tmp[200];
	FILE *f;
	int i;
	path_of("calendars.txt", path, sizeof(path));
	path_of("calendars.tmp", tmp, sizeof(tmp));
	if (!(f = fopen(tmp, "w"))) return -1;
	fprintf(f, "#PSICAL1\t%s\t%s\n", g_home, g_where);
	for (i = 0; i < g_ncal; i++) {
		Cal *c = &g_cal[i];
		fprintf(f, "%s\t%d\t%s\t%s\t%s\t%s\n", c->id, c->sync, c->ctag, c->href, c->flags, c->name);
	}
	if (pm_fclose(f) != 0 || pm_replace(tmp, path) != 0) { remove(tmp); return -1; }
	st_changed();
	return 0;
}

static Cal *cal_by_href(const char *href)
{
	int i;
	for (i = 0; i < g_ncal; i++) if (!strcmp(g_cal[i].href, href)) return &g_cal[i];
	return 0;
}

static Cal *cal_by_id(const char *id)
{
	int i;
	for (i = 0; i < g_ncal; i++) if (!strcmp(g_cal[i].id, id)) return &g_cal[i];
	return 0;
}

/* ------------------------------------------------------------ requests */

static const char *XML_HDR = "Content-Type: application/xml; charset=utf-8\r\n";

typedef struct
	{
	XmlScan x;
	/* the response being read */
	char href[200];
	char etag[100];
	char name[64];
	char ctag[100];
	char color[16];
	int is_cal, vevent, comps, writable;
	int got_data;
	int ndata;
	/* discovery */
	char principal[200];
	char home[200];
	/* events */
	FILE *out;
	const char *calid;
	int count;
	} Dav;

static Dav *g_dav;

static void sink(const char *data, int n, void *ctx) { xs_feed(&((Dav *)ctx)->x, data, n); }

static int propfind(const char *path, int depth, const char *props, Dav *d, XsEnd end,
                    HttpResp *r, char *why, int whymax)
{
	static char body[900], hdr[120];
	int n = snprintf(body, sizeof(body),
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<D:propfind xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\" "
		"xmlns:CS=\"http://calendarserver.org/ns/\" xmlns:A=\"http://apple.com/ns/ical/\">"
		"<D:prop>%s</D:prop></D:propfind>", props);
	snprintf(hdr, sizeof(hdr), "%sDepth: %d\r\n", XML_HDR, depth);
	xs_init(&d->x, g_text, TEXT_MAX, end, d);
	return http_request("PROPFIND", path, hdr, body, n, r, sink, d, why, whymax);
}

/* ------------------------------------------------------------ discovery */

static void disc_end(XmlScan *x, const char *name, const char *text, int len, void *ctx)
{
	Dav *d = (Dav *)ctx;
	(void)len;
	if (!strcmp(name, "href")) {
		const char *up = xs_parent(x, 1);
		char t[200];
		pm_copy(t, text, sizeof(t));
		trim(t);
		if (!strcmp(up, "current-user-principal")) just_path(t, d->principal, sizeof(d->principal));
		else if (!strcmp(up, "calendar-home-set")) just_path(t, d->home, sizeof(d->home));
	}
}

static int discover(char *why, int whymax)
{
	PmShared *s = pm_shared();
	HttpResp r;
	char path[200];
	int res, hops, attempt;
	const char *props = "<D:current-user-principal/><C:calendar-home-set/>";

	for (attempt = 0; attempt < 3 && !g_home[0]; attempt++) {
		if (attempt == 0) pm_copy(path, s->cal.path[0] ? s->cal.path : "/.well-known/caldav", sizeof(path));
		else if (attempt == 1) pm_copy(path, "/", sizeof(path));
		else {
			/* Fastmail's layout, if the server wouldn't say */
			const char *user = s->cal.user[0] ? s->cal.user : s->acct[s->cal.acct].user;
			snprintf(g_home, sizeof(g_home), "/dav/calendars/user/%s/", user);
			break;
		}
		for (hops = 0; hops < 4; hops++) {
			memset(g_dav, 0, sizeof(*g_dav));
			res = propfind(path, 0, props, g_dav, disc_end, &r, why, whymax);
			if (res != PM_RES_OK) return res;
			if (r.status >= 300 && r.status < 400 && r.location[0]) { just_path(r.location, path, sizeof(path)); continue; }
			break;
		}
		if (r.status != 207) continue;
		if (g_dav->home[0]) { pm_copy(g_home, g_dav->home, sizeof(g_home)); break; }
		if (g_dav->principal[0]) {
			pm_copy(path, g_dav->principal, sizeof(path));
			memset(g_dav, 0, sizeof(*g_dav));
			res = propfind(path, 0, props, g_dav, disc_end, &r, why, whymax);
			if (res != PM_RES_OK) return res;
			if (r.status == 207 && g_dav->home[0]) { pm_copy(g_home, g_dav->home, sizeof(g_home)); break; }
		}
	}
	pm_log("caldav: calendar home %s", g_home);
	return PM_RES_OK;
}

/* the calendars in the home collection */
static void list_end(XmlScan *x, const char *name, const char *text, int len, void *ctx)
{
	Dav *d = (Dav *)ctx;
	char t[200];
	(void)len;
	if (!strcmp(name, "href") && !strcmp(xs_parent(x, 1), "response")) {
		pm_copy(t, text, sizeof(t)); trim(t);
		just_path(t, d->href, sizeof(d->href));
	} else if (!strcmp(name, "calendar") && !strcmp(xs_parent(x, 1), "resourcetype")) d->is_cal = 1;
	else if (!strcmp(name, "displayname")) { cs_utf8_to_cp1252(text, len, d->name, sizeof(d->name)); trim(d->name); clean(d->name); }
	else if (!strcmp(name, "getctag")) { pm_copy(d->ctag, text, sizeof(d->ctag)); trim(d->ctag); clean(d->ctag); }
	else if (!strcmp(name, "comp") && !strcmp(xs_parent(x, 1), "supported-calendar-component-set")) {
		d->comps = 1;
		if (pm_stristr(x->tag, "VEVENT")) d->vevent = 1;
	} else if ((!strcmp(name, "write") || !strcmp(name, "all") || !strcmp(name, "write-content")) &&
	           !strcmp(xs_parent(x, 1), "privilege")) d->writable = 1;
	else if (!strcmp(name, "response")) {
		if (d->is_cal && (d->vevent || !d->comps) && d->href[0]) {
			Cal *c = cal_by_href(d->href);
			if (!c && g_ncal < MAX_CALS) {
				c = &g_cal[g_ncal++];
				memset(c, 0, sizeof(*c));
				pm_copy(c->href, d->href, sizeof(c->href));
				sprintf(c->id, "%08lX", fnv(d->href));
				c->sync = 1;
			}
			if (c) {
				c->seen = 1;
				if (strcmp(c->ctag, d->ctag) || !d->ctag[0]) c->fetch = 1;
				pm_copy(c->ctag, d->ctag, sizeof(c->ctag));
				pm_copy(c->name, d->name[0] ? d->name : "Calendar", sizeof(c->name));
				c->flags[0] = 0;
				if (d->writable) strcat(c->flags, "W");
			}
		}
		d->href[0] = d->name[0] = d->ctag[0] = 0;
		d->is_cal = d->vevent = d->comps = d->writable = 0;
	}
}

static int list_calendars(char *why, int whymax)
{
	HttpResp r;
	int res, i, def = -1;
	pm_progress("Looking for calendars...");
	if (!g_home[0]) {
		res = discover(why, whymax);
		if (res != PM_RES_OK) return res;
	}
	for (i = 0; i < g_ncal; i++) { g_cal[i].seen = 0; g_cal[i].fetch = 0; }
	memset(g_dav, 0, sizeof(*g_dav));
	res = propfind(g_home, 1,
		"<D:resourcetype/><D:displayname/><CS:getctag/><C:supported-calendar-component-set/>"
		"<D:current-user-privilege-set/>", g_dav, list_end, &r, why, whymax);
	if (res != PM_RES_OK) return res;
	if (r.status != 207) {
		if (r.status == 404) { snprintf(why, whymax, "No calendars found at %.60s", g_home); g_home[0] = 0; save_cals(); }
		else snprintf(why, whymax, "The calendar server said %d", r.status);
		return PM_RES_FAILED;
	}
	/* calendars that have gone */
	for (i = 0; i < g_ncal; ) {
		if (!g_cal[i].seen) { g_cal[i] = g_cal[--g_ncal]; continue; }
		i++;
	}
	/* where new events go: the one marked before, else the first writable */
	for (i = 0; i < g_ncal; i++) if (strchr(g_cal[i].flags, 'W') && def < 0) def = i;
	{
		char path[200], line[300];
		FILE *f;
		path_of("default.txt", path, sizeof(path));
		if ((f = fopen(path, "r")) != 0) {
			if (fgets(line, sizeof(line), f)) {
				Cal *c;
				line[strcspn(line, "\r\n")] = 0;
				c = cal_by_id(line);
				if (c && strchr(c->flags, 'W')) def = (int)(c - g_cal);
			}
			fclose(f);
		}
	}
	if (def >= 0) strcat(g_cal[def].flags, "D");
	save_cals();
	if (!g_ncal) { snprintf(why, whymax, "There are no calendars on %s", pm_shared()->cal.host); return PM_RES_FAILED; }
	return PM_RES_OK;
}

/* ------------------------------------------------------------ events */

typedef struct { Dav *d; } EvCtx;

static void one_event(const IcsEvent *e, void *ctx)
{
	Dav *d = (Dav *)ctx;
	int zone = pm_shared()->cal.zone;
	long s = e->start, en = e->end;
	char a[16], b[16], summary[200], location[120];
	if (e->cancelled) return;
	if (!e->allday && !e->floating) { s = cal_utc_to_local(zone, s); en = cal_utc_to_local(zone, en); }
	if (e->allday && en > s) en -= 86400;          /* DTEND is the day after */
	cal_fmt_local(s, a);
	cal_fmt_local(en, b);
	pm_copy(summary, e->summary[0] ? e->summary : "(no title)", sizeof(summary));
	pm_copy(location, e->location, sizeof(location));
	clean(summary);
	clean(location);
	fprintf(d->out, "%s\t%s\t%s\t%s\t%s%s\t%s\t%s\t%d\t%s\t%s\n", d->calid, d->href, d->etag, e->recurid,
		e->allday ? "A" : "", (e->recurid[0] || e->recurring) ? "R" : "", a, b, e->alarm, summary, location);
	d->count++;
}

static void report_end(XmlScan *x, const char *name, const char *text, int len, void *ctx)
{
	Dav *d = (Dav *)ctx;
	char t[200];
	if (!strcmp(name, "href") && !strcmp(xs_parent(x, 1), "response")) {
		pm_copy(t, text, sizeof(t)); trim(t);
		just_path(t, d->href, sizeof(d->href));
	} else if (!strcmp(name, "getetag")) { pm_copy(d->etag, text, sizeof(d->etag)); trim(d->etag); clean(d->etag); }
	else if (!strcmp(name, "calendar-data")) {
		/* keep the data until the response ends: the href and etag may
		   come after it. The text buffer is ours until the next element. */
		if (x->over) pm_log("caldav: %s is too big, cut short", d->href);
		if (len > 0) {
			memmove(g_obj, text, len < OBJ_MAX - 1 ? len : OBJ_MAX - 1);
			d->ndata = len < OBJ_MAX - 1 ? len : OBJ_MAX - 1;
			g_obj[d->ndata] = 0;
			d->got_data = 1;
		}
	} else if (!strcmp(name, "response")) {
		if (d->got_data && d->href[0]) ics_each(g_obj, d->ndata, one_event, d);
		d->href[0] = d->etag[0] = 0;
		d->got_data = 0;
	}
}

static void window(char *start, char *end, char *first_day)
{
	PmShared *s = pm_shared();
	long now = pm_time(), today = now - now % 86400;
	int back = s->cal.days_back >= 0 ? s->cal.days_back : 30;
	int ahead = s->cal.days_ahead > 0 ? s->cal.days_ahead : 180;
	cal_fmt_utc(today - back * 86400L, start);
	cal_fmt_utc(today + (ahead + 1) * 86400L, end);
	cal_fmt_date(today - back * 86400L, first_day);
}

static int fetch_events(Cal *c, FILE *out, char *why, int whymax)
{
	static char body[1500];
	HttpResp r;
	char a[24], b[24], day[12];
	int n, res;
	window(a, b, day);
	n = snprintf(body, sizeof(body),
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<C:calendar-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
		"<D:prop><D:getetag/><C:calendar-data>"
		"<C:comp name=\"VCALENDAR\"><C:prop name=\"VERSION\"/><C:comp name=\"VEVENT\">"
		"<C:prop name=\"UID\"/><C:prop name=\"SUMMARY\"/><C:prop name=\"LOCATION\"/>"
		"<C:prop name=\"DTSTART\"/><C:prop name=\"DTEND\"/><C:prop name=\"DURATION\"/>"
		"<C:prop name=\"RECURRENCE-ID\"/><C:prop name=\"RRULE\"/><C:prop name=\"RDATE\"/>"
		"<C:prop name=\"STATUS\"/><C:comp name=\"VALARM\"><C:allprop/></C:comp></C:comp></C:comp>"
		"<C:expand start=\"%s\" end=\"%s\"/></C:calendar-data></D:prop>"
		"<C:filter><C:comp-filter name=\"VCALENDAR\"><C:comp-filter name=\"VEVENT\">"
		"<C:time-range start=\"%s\" end=\"%s\"/></C:comp-filter></C:comp-filter></C:filter>"
		"</C:calendar-query>", a, b, a, b);
	pm_progress("Fetching %s...", c->name);
	memset(g_dav, 0, sizeof(*g_dav));
	g_dav->out = out;
	g_dav->calid = c->id;
	xs_init(&g_dav->x, g_text, TEXT_MAX, report_end, g_dav);
	res = http_request("REPORT", c->href, "Content-Type: application/xml; charset=utf-8\r\nDepth: 1\r\n",
		body, n, &r, sink, g_dav, why, whymax);
	if (res != PM_RES_OK) return res;
	if (r.status != 207) { snprintf(why, whymax, "Could not read %s (%d)", c->name, r.status); return PM_RES_FAILED; }
	pm_log("caldav: %s: %d events", c->name, g_dav->count);
	return PM_RES_OK;
}

/* ------------------------------------------------------------ changes from the Psion */

static void body_to_obj(const char *data, int n, void *ctx)
{
	int *len = (int *)ctx;
	if (*len + n >= OBJ_MAX) n = OBJ_MAX - 1 - *len;
	if (n > 0) { memcpy(g_obj + *len, data, n); *len += n; }
}

/* resources changed earlier in this batch: their etags have moved on */
#define MAX_TOUCHED 32
static char g_touched[MAX_TOUCHED][200];
static int g_ntouched;

static int touched(const char *href)
{
	int i;
	for (i = 0; i < g_ntouched; i++) if (!strcmp(g_touched[i], href)) return 1;
	return 0;
}

static void touch(const char *href)
{
	if (!touched(href) && g_ntouched < MAX_TOUCHED) pm_copy(g_touched[g_ntouched++], href, sizeof(g_touched[0]));
}

static void pushed(FILE *res, const char *key, const char *what, const char *href)
{
	if (res) fprintf(res, "%s\t%s\t%s\n", key, what, href);
}

static void read_change(char **p, IcsChange *c)
{
	char *flags;
	memset(c, 0, sizeof(*c));
	c->start = cal_parse_local(field(p));
	c->end = cal_parse_local(field(p));
	flags = field(p);
	c->allday = strchr(flags, 'A') != 0;
	c->alarm = atoi(field(p));
	pm_copy(c->summary, field(p), sizeof(c->summary));
	pm_copy(c->location, field(p), sizeof(c->location));
}

static void mark_fetch(const char *href)
{
	int i;
	for (i = 0; i < g_ncal; i++) {
		int n = (int)strlen(g_cal[i].href);
		if (n && !strncmp(href, g_cal[i].href, n)) g_cal[i].fetch = 1;
	}
}

/* one change; returns PM_RES_OK unless the connection is gone */
static int push_one(char *line, FILE *res, char *why, int whymax)
{
	PmShared *s = pm_shared();
	char *p = line, *op = field(&p), *key = field(&p);
	char hdr[200], href[260];
	HttpResp r;
	IcsChange c;
	int rc, len;

	if (op[0] == 'N') {
		unsigned char rnd[8];
		char uid[40];
		Cal *cal = cal_by_id(field(&p));
		int i;
		read_change(&p, &c);
		if (!cal) { pushed(res, key, "failed", ""); return PM_RES_OK; }
		/* the app names it, so a retry after a lost reply finds it there
		   (If-None-Match) rather than making a second one */
		pm_copy(uid, field(&p), sizeof(uid));
		for (i = 0; uid[i]; i++)
			if (!((uid[i] >= '0' && uid[i] <= '9') || (uid[i] >= 'a' && uid[i] <= 'z') || (uid[i] >= 'A' && uid[i] <= 'Z') || uid[i] == '-')) uid[i] = '-';
		if (!uid[0]) {
			genrandom(rnd, sizeof(rnd));
			for (i = 0; i < 8; i++) sprintf(uid + i * 2, "%02x", rnd[i]);
			strcat(uid, "-psion");
		}
		len = ics_new(&c, s->cal.zone, uid, g_obj, OBJ_MAX);
		if (len < 0) { pushed(res, key, "failed", ""); return PM_RES_OK; }
		snprintf(href, sizeof(href), "%s%s.ics", cal->href, uid);
		rc = http_request("PUT", href, "Content-Type: text/calendar; charset=utf-8\r\nIf-None-Match: *\r\n",
			g_obj, len, &r, 0, 0, why, whymax);
		if (rc != PM_RES_OK) return rc;
		pushed(res, key, r.status / 100 == 2 || r.status == 412 ? "ok" : "failed", href);
		cal->fetch = 1;
		pm_log("caldav: new %s -> %d", href, r.status);
		return PM_RES_OK;
	}
	if (op[0] == 'M' || op[0] == 'D') {
		char *h = field(&p), *etag = field(&p), *recurid = field(&p);
		pm_copy(href, h, sizeof(href));
		mark_fetch(href);
		if (touched(href)) etag = (char *)"";
		if (op[0] == 'D' && !recurid[0]) {
			snprintf(hdr, sizeof(hdr), "If-Match: %s\r\n", etag);
			rc = http_request("DELETE", href, etag[0] ? hdr : "", 0, 0, &r, 0, 0, why, whymax);
			if (rc != PM_RES_OK) return rc;
			pushed(res, key, r.status / 100 == 2 ? "ok" : r.status == 404 ? "gone" : r.status == 412 ? "conflict" : "failed", href);
			pm_log("caldav: delete %s -> %d", href, r.status);
			return PM_RES_OK;
		}
		/* change the server's own text */
		len = 0;
		rc = http_request("GET", href, "", 0, 0, &r, body_to_obj, &len, why, whymax);
		if (rc != PM_RES_OK) return rc;
		if (r.status == 404) { pushed(res, key, "gone", href); return PM_RES_OK; }
		if (r.status != 200 || len >= OBJ_MAX - 1) { pushed(res, key, "failed", href); return PM_RES_OK; }
		if (etag[0] && r.etag[0] && strcmp(etag, r.etag)) { pushed(res, key, "conflict", href); return PM_RES_OK; }
		g_obj[len] = 0;
		if (op[0] == 'M') {
			read_change(&p, &c);
			len = ics_change(g_obj, len, OBJ_MAX - 1, recurid, &c, s->cal.zone);
		} else len = ics_exclude(g_obj, len, OBJ_MAX - 1, recurid, s->cal.zone);
		if (len < 0) { pushed(res, key, "failed", href); return PM_RES_OK; }
		snprintf(hdr, sizeof(hdr), "Content-Type: text/calendar; charset=utf-8\r\n%s%s%s",
			r.etag[0] ? "If-Match: " : "", r.etag, r.etag[0] ? "\r\n" : "");
		rc = http_request("PUT", href, hdr, g_obj, len, &r, 0, 0, why, whymax);
		if (rc != PM_RES_OK) return rc;
		pushed(res, key, r.status / 100 == 2 ? "ok" : r.status == 412 ? "conflict" : "failed", href);
		if (r.status / 100 == 2) touch(href);
		pm_log("caldav: %s %s %s -> %d", op, href, recurid, r.status);
		return PM_RES_OK;
	}
	return PM_RES_OK;
}

static int push_changes(int *n, char *why, int whymax)
{
	char path[200], rpath[200], line[900];
	FILE *f, *res;
	int rc = PM_RES_OK, total = 0, done = 0;
	*n = 0;
	path_of("push.txt", path, sizeof(path));
	if (!(f = fopen(path, "r"))) return PM_RES_OK;
	while (fgets(line, sizeof(line), f)) if (line[0] != '#' && line[0] > ' ') total++;
	if (!total) { fclose(f); remove(path); return PM_RES_OK; }
	fseek(f, 0, SEEK_SET);
	g_ntouched = 0;
	path_of("pushed.txt", rpath, sizeof(rpath));
	res = fopen(rpath, "a");
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || !line[0]) continue;
		pm_progress("Sending calendar changes (%d of %d)...", done + 1, total);
		rc = push_one(line, res, why, whymax);
		if (rc != PM_RES_OK) break;
		done++;
	}
	fclose(f);
	if (res) fclose(res);
	if (rc == PM_RES_OK) remove(path);
	else if (done > 0) {
		/* keep what wasn't sent for next time */
		char tmp[200];
		FILE *in, *out;
		int k = 0;
		path_of("push.tmp", tmp, sizeof(tmp));
		if ((in = fopen(path, "r")) != 0 && (out = fopen(tmp, "w")) != 0) {
			while (fgets(line, sizeof(line), in)) {
				if (line[0] == '#' || line[0] <= ' ') continue;
				if (k++ >= done) fputs(line, out);
			}
			fclose(in);
			/* (if the rest can't be written down, the whole file stays:
			   sending a change twice is harmless, losing one is not) */
			if (pm_fclose(out) != 0 || pm_replace(tmp, path) != 0) remove(tmp);
		} else if (in) fclose(in);
	}
	*n = done;
	st_changed();
	return rc;
}

/* ------------------------------------------------------------ the sync */

/* copies the lines of calendars we are not fetching again from the old
   events.txt; returns 0 if it can't be used (a new day, another zone) */
static int keep_old(FILE *out, const char *first_day)
{
	char path[200], line[900], head[60];
	FILE *f;
	int zone = pm_shared()->cal.zone;
	path_of("events.txt", path, sizeof(path));
	if (!(f = fopen(path, "r"))) return 0;
	snprintf(head, sizeof(head), "#PSIEV1\t%s\t%d", first_day, zone);
	if (!fgets(line, sizeof(line), f) || strncmp(line, head, strlen(head))) { fclose(f); return 0; }
	while (fgets(line, sizeof(line), f)) {
		char id[12];
		Cal *c;
		int n = (int)strcspn(line, "\t");
		if (n >= (int)sizeof(id)) continue;
		memcpy(id, line, n);
		id[n] = 0;
		c = cal_by_id(id);
		if (c && c->sync && !c->fetch) fputs(line, out);
	}
	fclose(f);
	return 1;
}

static int sync_now(int list_only, char *why, int whymax);

/* the buffers are only needed while syncing */
int cal_sync(int list_only, char *why, int whymax)
{
	int r = sync_now(list_only, why, whymax);
	free(g_text); g_text = 0;
	free(g_obj); g_obj = 0;
	free(g_dav); g_dav = 0;
	return r;
}

static int sync_now(int list_only, char *why, int whymax)
{
	PmShared *s = pm_shared();
	PmCalendar *k = &s->cal;
	PmAccount *a = &s->acct[k->acct >= 0 && k->acct < PM_MAX_ACCOUNTS ? k->acct : 0];
	char path[200], tmp[200], a1[24], b1[24], day[12];
	FILE *out;
	int res, i, sent = 0, total = 0, fetched = 0;
	const char *pass = k->pass[0] ? k->pass : a->pass;

	if (!k->host[0]) { snprintf(why, whymax, "No calendar server set up"); return PM_RES_FAILED; }
	if (!pass[0]) { snprintf(why, whymax, "Password needed for the calendar"); return PM_RES_NEED_PASS; }
	snprintf(g_dir, sizeof(g_dir), "%scal%s", s->store_dir, PM_SEP);
	pm_mkdir(g_dir);
	if (!g_text) g_text = (char *)malloc(TEXT_MAX);
	if (!g_obj) g_obj = (char *)malloc(OBJ_MAX);
	if (!g_dav) g_dav = (Dav *)malloc(sizeof(Dav));
	if (!g_text || !g_obj || !g_dav) { snprintf(why, whymax, "Not enough memory for the calendar"); return PM_RES_FAILED; }

	http_target(k->host, k->port ? k->port : 443, !k->plain, k->user[0] ? k->user : a->user, pass);
	snprintf(g_where, sizeof(g_where), "%s%s", k->host, k->path);
	load_cals();
	res = list_calendars(why, whymax);
	if (res != PM_RES_OK) return res;
	if (list_only) {
		snprintf(why, whymax, "%d calendar%s", g_ncal, g_ncal == 1 ? "" : "s");
		return PM_RES_OK;
	}
	res = push_changes(&sent, why, whymax);
	if (res != PM_RES_OK) return res;

	/* fetch what changed; the rest comes from the old file */
	window(a1, b1, day);
	path_of("events.txt", path, sizeof(path));
	path_of("events.tmp", tmp, sizeof(tmp));
	if (!(out = fopen(tmp, "w"))) { pm_write_why(why, whymax, "the calendar", tmp); return PM_RES_FAILED; }
	fprintf(out, "#PSIEV1\t%s\t%d\n", day, k->zone);
	if (!keep_old(out, day)) {
		/* start again */
		fclose(out);
		out = fopen(tmp, "w");
		if (!out) return PM_RES_FAILED;
		fprintf(out, "#PSIEV1\t%s\t%d\n", day, k->zone);
		for (i = 0; i < g_ncal; i++) g_cal[i].fetch = 1;
	}
	for (i = 0; i < g_ncal; i++) {
		Cal *c = &g_cal[i];
		if (!c->sync || !c->fetch) continue;
		res = fetch_events(c, out, why, whymax);
		if (res != PM_RES_OK) {
			fclose(out);
			remove(tmp);
			/* forget the ctags so the next sync fetches again */
			for (i = 0; i < g_ncal; i++) if (g_cal[i].fetch) g_cal[i].ctag[0] = 0;
			save_cals();
			return res;
		}
		total += g_dav->count;
		fetched++;
	}
	if (pm_fclose(out) != 0 || pm_replace(tmp, path) != 0) {
		remove(tmp);
		pm_write_why(why, whymax, "the calendar", path);
		return PM_RES_FAILED;
	}
	save_cals();
	st_changed();
	if (fetched) snprintf(why, whymax, "%s%d event%s", sent ? "Sent changes; " : "", total, total == 1 ? "" : "s");
	else snprintf(why, whymax, sent ? "Sent %d change%s" : "No calendar changes", sent, sent == 1 ? "" : "s");
	return PM_RES_OK;
}
