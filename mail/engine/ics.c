/* ics.c - iCalendar (RFC 5545) for PsiMail's calendar (see cal.h)
 *
 * Reads the events the CalDAV server sends (already expanded into single
 * instances, times in UTC), writes new events, and edits the server's own
 * text for a change made on the Psion, so everything the Psion doesn't
 * know about (attendees, descriptions, other alarms) is kept.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm.h"
#include "cal.h"

#define LINE_MAX_ICS 1200

/* ------------------------------------------------------------ reading */

/* the next logical (unfolded) line from text[*pos..len) into line; returns
   its length, or -1 at the end */
static int next_line(const char *text, int len, int *pos, char *line, int max)
{
	int i = *pos, n = 0;
	if (i >= len) return -1;
	while (i < len) {
		char c = text[i];
		if (c == '\r' || c == '\n') {
			/* CRLF (or LF) then a space or tab continues the line */
			int j = i + 1;
			if (c == '\r' && j < len && text[j] == '\n') j++;
			if (j < len && (text[j] == ' ' || text[j] == '\t')) { i = j + 1; continue; }
			i = j;
			break;
		}
		if (n < max - 1) line[n++] = c;
		i++;
	}
	line[n] = 0;
	*pos = i;
	return n;
}

/* splits "NAME;P=V;Q=W:value": returns the value, name upper-cased into
   name, the parameters (without the name) into params */
static const char *split_prop(const char *line, char *name, int nmax, char *params, int pmax)
{
	int i = 0, n = 0, p = 0, quote = 0;
	while (line[i] && line[i] != ';' && line[i] != ':') {
		char c = line[i++];
		if (c >= 'a' && c <= 'z') c -= 32;
		if (n < nmax - 1) name[n++] = c;
	}
	name[n] = 0;
	if (line[i] == ';') i++;
	while (line[i] && (quote || line[i] != ':')) {
		if (line[i] == '"') quote = !quote;
		if (p < pmax - 1) params[p++] = line[i];
		i++;
	}
	params[p] = 0;
	return line[i] == ':' ? line + i + 1 : line + i;
}

static int has_param(const char *params, const char *what)
{
	return pm_stristr(params, what) != 0;
}

/* TEXT value -> cp1252, one line */
static void text_value(const char *v, char *out, int max)
{
	char tmp[LINE_MAX_ICS];
	int n = 0;
	while (*v && n < (int)sizeof(tmp) - 1) {
		if (*v == '\\' && v[1]) {
			v++;
			if (*v == 'n' || *v == 'N') tmp[n++] = ' ';
			else tmp[n++] = *v;
			v++;
		} else tmp[n++] = *v++;
	}
	while (n > 0 && tmp[n - 1] == ' ') n--;
	cs_utf8_to_cp1252(tmp, n, out, max);
}

/* "-PT15M", "PT1H30M", "-P1D", "-P1W" -> seconds (sign kept) */
static long duration(const char *v)
{
	long sign = 1, total = 0, n = 0;
	int time = 0;
	if (*v == '-') { sign = -1; v++; } else if (*v == '+') v++;
	if (*v++ != 'P') return 0;
	for (; *v; v++) {
		if (*v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); continue; }
		switch (*v) {
		case 'T': time = 1; break;
		case 'W': total += n * 7 * 86400; break;
		case 'D': total += n * 86400; break;
		case 'H': total += n * 3600; break;
		case 'M': if (time) total += n * 60; break;
		case 'S': total += n; break;
		}
		n = 0;
	}
	return sign * total;
}

/* RECURRENCE-ID as a string we can compare: UTC "YYYYMMDDTHHMMSSZ", or
   "YYYYMMDD" for a date. Times with a TZID are taken to be in 'zone'. */
static void norm_recurid(const char *params, const char *v, int zone, char *out)
{
	long t;
	int utc, date_only;
	out[0] = 0;
	if (cal_parse_time(v, &t, &utc, &date_only) != 0) return;
	if (date_only) { cal_fmt_date(t, out); return; }
	if (!utc) t = cal_local_to_utc(zone, t);
	(void)params;
	cal_fmt_utc(t, out);
}

int ics_each(const char *text, int len, void (*fn)(const IcsEvent *e, void *ctx), void *ctx)
{
	static char line[LINE_MAX_ICS];
	char name[40], params[200];
	IcsEvent e;
	int pos = 0, in_event = 0, in_alarm = 0, other = 0, count = 0, have_end = 0;
	long dur = -1, trig = 0;
	int trig_ok = 0;

	while (next_line(text, len, &pos, line, sizeof(line)) >= 0) {
		const char *v = split_prop(line, name, sizeof(name), params, sizeof(params));
		if (!strcmp(name, "BEGIN")) {
			if (!pm_strcasecmp(v, "VEVENT") && !in_event) {
				memset(&e, 0, sizeof(e));
				e.alarm = -1;
				in_event = 1; in_alarm = 0; other = 0; have_end = 0; dur = -1;
			} else if (in_event && !pm_strcasecmp(v, "VALARM")) { in_alarm = 1; trig_ok = 0; }
			else if (in_event) other++;
			continue;
		}
		if (!strcmp(name, "END")) {
			if (in_alarm && !pm_strcasecmp(v, "VALARM")) {
				in_alarm = 0;
				if (trig_ok && e.alarm < 0 && trig <= 0) e.alarm = (int)(-trig / 60);
			} else if (in_event && other && pm_strcasecmp(v, "VEVENT")) other--;
			else if (in_event && !pm_strcasecmp(v, "VEVENT")) {
				in_event = 0;
				if (!have_end) {
					if (dur >= 0) e.end = e.start + dur;
					else e.end = e.allday ? e.start + 86400 : e.start;
				}
				if (e.end < e.start) e.end = e.start;
				count++;
				if (fn) fn(&e, ctx);
			}
			continue;
		}
		if (!in_event || other) continue;
		if (in_alarm) {
			if (!strcmp(name, "TRIGGER") && !has_param(params, "VALUE=DATE-TIME")) {
				trig = duration(v);
				trig_ok = 1;
			}
			continue;
		}
		if (!strcmp(name, "UID")) pm_copy(e.uid, v, sizeof(e.uid));
		else if (!strcmp(name, "SUMMARY")) text_value(v, e.summary, sizeof(e.summary));
		else if (!strcmp(name, "LOCATION")) text_value(v, e.location, sizeof(e.location));
		else if (!strcmp(name, "RRULE") || !strcmp(name, "RDATE")) e.recurring = 1;
		else if (!strcmp(name, "STATUS")) e.cancelled = !pm_strcasecmp(v, "CANCELLED");
		else if (!strcmp(name, "RECURRENCE-ID")) norm_recurid(params, v, 0, e.recurid);
		else if (!strcmp(name, "DTSTART") || !strcmp(name, "DTEND")) {
			long t;
			int utc, date_only;
			if (cal_parse_time(v, &t, &utc, &date_only) != 0) continue;
			if (!strcmp(name, "DTSTART")) {
				e.start = t;
				e.allday = date_only;
				e.floating = !utc && !date_only;
			} else { e.end = t; have_end = 1; }
		} else if (!strcmp(name, "DURATION")) dur = duration(v);
	}
	return count;
}

/* ------------------------------------------------------------ writing */

typedef struct { char *b; int n, max, bad; } Out;

static void out_raw(Out *o, const char *s, int n)
{
	if (o->n + n >= o->max) { o->bad = 1; return; }
	memcpy(o->b + o->n, s, n);
	o->n += n;
}

/* one content line, folded at 75 octets without splitting a UTF-8 character */
static void out_line(Out *o, const char *s, int n)
{
	int col = 0, i = 0;
	while (i < n) {
		int k = 1;
		unsigned char c = (unsigned char)s[i];
		if (c >= 0xc0) while (i + k < n && ((unsigned char)s[i + k] & 0xc0) == 0x80) k++;
		if (col + k > 75) { out_raw(o, "\r\n ", 3); col = 1; }
		out_raw(o, s + i, k);
		col += k;
		i += k;
	}
	out_raw(o, "\r\n", 2);
}

static void out_str(Out *o, const char *s) { out_line(o, s, (int)strlen(s)); }

/* "NAME:" + cp1252 text, escaped and in UTF-8 */
static void out_text(Out *o, const char *prop, const char *cp)
{
	char esc[600], utf[1200];
	int n = 0;
	n = snprintf(esc, sizeof(esc), "%s:", prop);
	for (; *cp && n < (int)sizeof(esc) - 3; cp++) {
		if (*cp == '\\' || *cp == ';' || *cp == ',') esc[n++] = '\\';
		if (*cp == '\n') { esc[n++] = '\\'; esc[n++] = 'n'; continue; }
		esc[n++] = *cp;
	}
	n = cs_cp1252_to_utf8(esc, n, utf, sizeof(utf));
	out_line(o, utf, n);
}

static void out_times(Out *o, const IcsChange *c, int zone)
{
	char a[24], b[24], l[60];
	if (c->allday) {
		long s = c->start - c->start % 86400, e = c->end - c->end % 86400 + 86400;
		if (e <= s) e = s + 86400;
		cal_fmt_date(s, a);
		cal_fmt_date(e, b);
		snprintf(l, sizeof(l), "DTSTART;VALUE=DATE:%s", a); out_str(o, l);
		snprintf(l, sizeof(l), "DTEND;VALUE=DATE:%s", b); out_str(o, l);
	} else {
		cal_fmt_utc(cal_local_to_utc(zone, c->start), a);
		cal_fmt_utc(cal_local_to_utc(zone, c->end > c->start ? c->end : c->start), b);
		snprintf(l, sizeof(l), "DTSTART:%s", a); out_str(o, l);
		snprintf(l, sizeof(l), "DTEND:%s", b); out_str(o, l);
	}
}

static void out_alarm(Out *o, int minutes)
{
	char l[40];
	out_str(o, "BEGIN:VALARM");
	out_str(o, "ACTION:DISPLAY");
	out_str(o, "DESCRIPTION:Reminder");
	snprintf(l, sizeof(l), "TRIGGER:-PT%dM", minutes);
	out_str(o, l);
	out_str(o, "END:VALARM");
}

static void out_stamp(Out *o, const char *prop)
{
	char t[24], l[40];
	cal_fmt_utc(pm_time(), t);
	snprintf(l, sizeof(l), "%s:%s", prop, t);
	out_str(o, l);
}

int ics_new(const IcsChange *c, int zone, const char *uid, char *buf, int max)
{
	Out o;
	char l[200];
	o.b = buf; o.n = 0; o.max = max; o.bad = 0;
	out_str(&o, "BEGIN:VCALENDAR");
	out_str(&o, "VERSION:2.0");
	out_str(&o, "PRODID:-//PsiMail//Psion Series 5mx//EN");
	out_str(&o, "BEGIN:VEVENT");
	snprintf(l, sizeof(l), "UID:%s", uid);
	out_str(&o, l);
	out_stamp(&o, "DTSTAMP");
	out_stamp(&o, "CREATED");
	out_times(&o, c, zone);
	out_text(&o, "SUMMARY", c->summary);
	if (c->location[0]) out_text(&o, "LOCATION", c->location);
	if (c->alarm >= 0) out_alarm(&o, c->alarm);
	out_str(&o, "END:VEVENT");
	out_str(&o, "END:VCALENDAR");
	if (o.bad) return -1;
	buf[o.n] = 0;
	return o.n;
}

/* ------------------------------------------------------------ editing */

/* The object's lines, unfolded. Each VEVENT is found by its RECURRENCE-ID
   ("" for the master). */
typedef struct
	{
	int start, end;          /* line numbers */
	int has_rid;             /* an exception (has a RECURRENCE-ID) */
	char recurid[24];        /* its RECURRENCE-ID in UTC, or a date */
	int wall;                /* ... or it had a TZID: the wall-clock time */
	long wall_t;
	} Block;
#define MAX_LINES 1500
#define MAX_BLOCKS 60

typedef struct
	{
	char *text;              /* unfolded copy, lines NUL-separated */
	int nlines;
	int zone;
	int off[MAX_LINES];
	int nblocks;
	Block block[MAX_BLOCKS];
	int vcal_end;            /* the END:VCALENDAR line */
	} Obj;

static int load(Obj *ob, const char *buf, int len, int zone)
{
	char name[40], params[200];
	char *line;
	int pos = 0, used = 0, n, cur = -1, depth = 0;
	/* a line can be as long as the whole object (unfolded, never longer) */
	ob->text = (char *)malloc(len + 16);
	line = (char *)malloc(len + 16);
	if (!ob->text || !line) { free(ob->text); free(line); return -1; }
	ob->nlines = ob->nblocks = 0;
	ob->vcal_end = -1;
	ob->zone = zone;
	while ((n = next_line(buf, len, &pos, line, len + 16)) >= 0) {
		const char *v;
		if (n == 0) continue;
		if (ob->nlines >= MAX_LINES || used + n + 1 > len + 16) { free(ob->text); free(line); return -1; }
		ob->off[ob->nlines] = used;
		memcpy(ob->text + used, line, n + 1);
		used += n + 1;
		v = split_prop(line, name, sizeof(name), params, sizeof(params));
		if (!strcmp(name, "BEGIN") && !pm_strcasecmp(v, "VEVENT") && ob->nblocks < MAX_BLOCKS) {
			cur = ob->nblocks++;
			memset(&ob->block[cur], 0, sizeof(Block));
			ob->block[cur].start = ob->nlines;
			depth = 0;
		} else if (cur >= 0 && !strcmp(name, "BEGIN")) depth++;
		else if (cur >= 0 && !strcmp(name, "END")) {
			if (depth > 0) depth--;
			else { ob->block[cur].end = ob->nlines; cur = -1; }
		} else if (cur >= 0 && depth == 0 && !strcmp(name, "RECURRENCE-ID")) {
			Block *b = &ob->block[cur];
			long t;
			int utc, date_only;
			b->has_rid = 1;
			if (cal_parse_time(v, &t, &utc, &date_only) == 0) {
				if (date_only) cal_fmt_date(t, b->recurid);
				else if (utc) cal_fmt_utc(t, b->recurid);
				else { b->wall = 1; b->wall_t = t; }   /* TZID (or floating): see find_block */
			}
		}
		if (!strcmp(name, "END") && !pm_strcasecmp(v, "VCALENDAR")) ob->vcal_end = ob->nlines;
		ob->nlines++;
	}
	free(line);
	return ob->vcal_end >= 0 ? 0 : (free(ob->text), -1);
}

static const char *L(Obj *ob, int i) { return ob->text + ob->off[i]; }

/* The block for an instance: recurid "" is the master; otherwise the
   server's UTC RECURRENCE-ID. An exception written with a TZID is matched
   without knowing that zone: its wall-clock time is the UTC time shifted by
   the zone's offset (at most 14 hours, in quarter hours) - the Psion's own
   zone first, else the nearest such exception. */
static int find_block(Obj *ob, const char *recurid)
{
	int i, best = -1;
	long want, bestd = 15 * 3600L;
	int utc, date_only;
	for (i = 0; i < ob->nblocks; i++) {
		Block *b = &ob->block[i];
		if (!recurid[0]) { if (!b->has_rid) return i; continue; }
		if (b->has_rid && !b->wall && !strcmp(b->recurid, recurid)) return i;
	}
	if (!recurid[0] || cal_parse_time(recurid, &want, &utc, &date_only) != 0 || date_only) return -1;
	for (i = 0; i < ob->nblocks; i++) {
		Block *b = &ob->block[i];
		long d;
		if (!b->has_rid || !b->wall) continue;
		if (b->wall_t == cal_utc_to_local(ob->zone, want)) return i;
		d = b->wall_t - want;
		if (d < 0) d = -d;
		if (d % 900 == 0 && d < bestd) { bestd = d; best = i; }
	}
	return best;
}

static int is_prop(const char *line, const char *name)
{
	int n = (int)strlen(name);
	return !pm_strncasecmp(line, name, n) && (line[n] == ':' || line[n] == ';');
}

/* writes one VEVENT (lines a..b) with the change applied; as_exception
   turns a copy of the master into an exception for recurid */
static void write_event(Out *o, Obj *ob, int a, int b, const IcsChange *c, int zone,
                        int as_exception, const char *recurid)
{
	int i, depth = 0, alarm_done = 0, in_alarm = 0, seq = 0, skip_alarm = 0;
	char l[60];
	for (i = a; i <= b; i++) {
		const char *s = L(ob, i);
		if (i == a) { out_str(o, s); continue; }
		if (is_prop(s, "BEGIN")) {
			depth++;
			if (pm_stristr(s, "VALARM")) {
				in_alarm = 1;
				/* the Psion has one alarm: it becomes the first VALARM;
				   no alarm there removes them all */
				skip_alarm = c->alarm == -1 || (c->alarm >= 0 && alarm_done);
				if (!skip_alarm) out_str(o, s);
				continue;
			}
		}
		if (in_alarm) {
			if (is_prop(s, "END") && pm_stristr(s, "VALARM")) {
				in_alarm = 0;
				depth--;
				if (!skip_alarm) out_str(o, s);
				if (c->alarm >= 0) alarm_done = 1;
				continue;
			}
			if (skip_alarm) continue;
			if (c->alarm >= 0 && is_prop(s, "TRIGGER")) {
				snprintf(l, sizeof(l), "TRIGGER:-PT%dM", c->alarm);
				out_str(o, l);
			} else out_str(o, s);
			continue;
		}
		if (i == b) {
			/* END:VEVENT: add what the event didn't have */
			if (c->alarm >= 0 && !alarm_done) out_alarm(o, c->alarm);
			snprintf(l, sizeof(l), "SEQUENCE:%d", seq + 1);
			out_str(o, l);
			out_stamp(o, "DTSTAMP");
			out_stamp(o, "LAST-MODIFIED");
			out_str(o, s);
			continue;
		}
		if (depth > 0) {             /* some other component inside */
			if (is_prop(s, "END")) depth--;
			out_str(o, s);
			continue;
		}
		if (is_prop(s, "DTSTART")) {
			out_times(o, c, zone);
			if (as_exception) {
				snprintf(l, sizeof(l), strlen(recurid) == 8 ? "RECURRENCE-ID;VALUE=DATE:%s" : "RECURRENCE-ID:%s", recurid);
				out_str(o, l);
			}
			continue;
		}
		if (is_prop(s, "DTEND") || is_prop(s, "DURATION") || is_prop(s, "DTSTAMP") ||
		    is_prop(s, "LAST-MODIFIED")) continue;
		if (is_prop(s, "SEQUENCE")) { seq = atoi(strchr(s, ':') ? strchr(s, ':') + 1 : "0"); continue; }
		if (as_exception && (is_prop(s, "RRULE") || is_prop(s, "RDATE") || is_prop(s, "EXDATE") ||
		                     is_prop(s, "RECURRENCE-ID"))) continue;
		if (is_prop(s, "SUMMARY")) { out_text(o, "SUMMARY", c->summary); continue; }
		if (is_prop(s, "LOCATION")) {
			if (c->location[0]) out_text(o, "LOCATION", c->location);
			continue;
		}
		out_str(o, s);
		if (is_prop(s, "UID") && c->location[0]) {
			/* LOCATION is written here if the event had none */
			int k, has = 0;
			for (k = a; k <= b; k++) if (is_prop(L(ob, k), "LOCATION")) has = 1;
			if (!has) out_text(o, "LOCATION", c->location);
		}
	}
}

int ics_change(char *buf, int len, int max, const char *recurid, const IcsChange *c, int zone)
{
	Obj *ob = (Obj *)malloc(sizeof(Obj));
	Out o;
	int i, k, master, target;
	if (!ob) return -1;
	if (load(ob, buf, len, zone) != 0) { free(ob); return -1; }
	master = find_block(ob, "");
	target = find_block(ob, recurid);
	o.b = (char *)malloc(max + 1); o.n = 0; o.max = max; o.bad = 0;
	if (!o.b || (target < 0 && (master < 0 || !recurid[0]))) {
		free(o.b); free(ob->text); free(ob);
		return -1;
	}
	for (i = 0; i < ob->nlines; i++) {
		int handled = 0;
		for (k = 0; k < ob->nblocks; k++)
			if (k == target && i == ob->block[k].start) {
				write_event(&o, ob, ob->block[k].start, ob->block[k].end, c, zone, 0, recurid);
				i = ob->block[k].end;
				handled = 1;
			}
		if (handled) continue;
		if (i == ob->vcal_end && target < 0)
			write_event(&o, ob, ob->block[master].start, ob->block[master].end, c, zone, 1, recurid);
		out_str(&o, L(ob, i));
	}
	free(ob->text);
	free(ob);
	if (o.bad) { free(o.b); return -1; }
	memcpy(buf, o.b, o.n);
	buf[o.n] = 0;
	free(o.b);
	return o.n;
}

int ics_exclude(char *buf, int len, int max, const char *recurid, int zone)
{
	Obj *ob = (Obj *)malloc(sizeof(Obj));
	Out o;
	int i, master, target;
	char l[60];
	if (!ob) return -1;
	if (load(ob, buf, len, zone) != 0) { free(ob); return -1; }
	master = find_block(ob, "");
	target = find_block(ob, recurid);
	o.b = (char *)malloc(max + 1); o.n = 0; o.max = max; o.bad = 0;
	if (!o.b || master < 0 || !recurid[0]) { free(o.b); free(ob->text); free(ob); return -1; }
	for (i = 0; i < ob->nlines; i++) {
		if (target >= 0 && i >= ob->block[target].start && i <= ob->block[target].end) continue;
		out_str(&o, L(ob, i));
		if (i > ob->block[master].start && i < ob->block[master].end && is_prop(L(ob, i), "DTSTART")) {
			snprintf(l, sizeof(l), strlen(recurid) == 8 ? "EXDATE;VALUE=DATE:%s" : "EXDATE:%s", recurid);
			out_str(&o, l);
		}
	}
	free(ob->text);
	free(ob);
	if (o.bad) { free(o.b); return -1; }
	memcpy(buf, o.b, o.n);
	buf[o.n] = 0;
	free(o.b);
	return o.n;
}
