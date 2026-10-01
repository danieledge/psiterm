/* invite.c - invitations (iCalendar, RFC 5545 / iTIP RFC 5546) and
 * contact cards (vCard 2.1, 3.0 and 4.0) that arrive in messages.
 *
 * Only the parsing and building: no files, no network, so it runs on a PC
 * and under the fuzzer (test/parsefuzz.c). invmsg.c fetches the parts and
 * writes what is found here into small files for PsiMail.app.
 *
 * Everything is hostile input: every copy is bounded, lines are read with
 * ics_line (unfolded, cut at the buffer), and nothing is allocated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm.h"
#include "cal.h"
#include "invite.h"

#define LINE_MAX_INV 1200

static void lower_copy(char *dst, const char *src, int max)
{
	int i = 0;
	for (; src[i] && i < max - 1; i++) dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 32 : src[i];
	dst[i] = 0;
}

/* a parameter's value from "A=1;CN=\"Smith, Jo\";B=2" (quotes removed) */
static int get_param(const char *params, const char *key, char *out, int max)
{
	int kl = (int)strlen(key);
	const char *p = params;
	out[0] = 0;
	while (*p) {
		while (*p == ';') p++;
		if (!pm_strncasecmp(p, key, kl) && p[kl] == '=') {
			int k = 0, q = 0;
			p += kl + 1;
			while (*p && (q || *p != ';')) {
				if (*p == '"') { q = !q; p++; continue; }
				if (k < max - 1) out[k++] = *p;
				p++;
			}
			out[k] = 0;
			return 1;
		}
		/* to the next ';' outside quotes */
		{
			int q = 0;
			while (*p && (q || *p != ';')) { if (*p == '"') q = !q; p++; }
		}
	}
	return 0;
}

/* "mailto:a@b" -> "a@b" */
static void mail_addr(const char *v, char *out, int max)
{
	while (*v == ' ') v++;
	if (!pm_strncasecmp(v, "mailto:", 7)) v += 7;
	pm_copy(out, v, max);
}

/* UTF-8 (the iCalendar default) into cp1252, for names in parameters */
static void to_cp(const char *v, char *out, int max)
{
	out[0] = 0;
	cs_utf8_to_cp1252(v, (int)strlen(v), out, max);
}

/* ------------------------------------------------------------ time zones */

/* "+0100", "-0500", "+053000" -> minutes east of UTC */
static int tz_offset(const char *v, int *ok)
{
	int sign = 1, h, m;
	*ok = 0;
	if (*v == '-') sign = -1; else if (*v != '+') return 0;
	v++;
	if (v[0] < '0' || v[0] > '9' || v[1] < '0' || v[1] > '9' || v[2] < '0' || v[2] > '9' || v[3] < '0' || v[3] > '9') return 0;
	h = (v[0] - '0') * 10 + (v[1] - '0');
	m = (v[2] - '0') * 10 + (v[3] - '0');
	if (h > 18 || m > 59) return 0;
	*ok = 1;
	return sign * (h * 60 + m);
}

static int wday_of(const char *s)
{
	static const char *n[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
	int i;
	for (i = 0; i < 7; i++) if (!pm_strncasecmp(s, n[i], 2)) return i;
	return -1;
}

/* RRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU */
static void tz_rrule(TzRule *r, const char *v)
{
	const char *m = pm_stristr(v, "BYMONTH="), *d = pm_stristr(v, "BYDAY=");
	if (m) {
		int mon = atoi(m + 8);
		if (mon >= 1 && mon <= 12) r->month = mon;
	}
	if (d) {
		const char *p = d + 6;
		int sign = 1, n = 0, w;
		if (*p == '-') { sign = -1; p++; } else if (*p == '+') p++;
		while (*p >= '0' && *p <= '9' && n < 10) n = n * 10 + (*p++ - '0');
		w = wday_of(p);
		if (w >= 0) {
			if (n == 0) n = 1;
			if (n > 4) n = -1;              /* the 5th = the last */
			r->week = sign * n;
			if (r->week < -1) r->week = -1;
			r->wday = w;
		}
	}
	r->rule = r->month && r->week;
}

/* the local time a rule changes the clocks in a year */
static long tz_change(const TzRule *r, int y)
{
	long d;
	if (r->week > 0) {
		d = cal_days(y, r->month, 1);
		d += (r->wday - cal_wday(d) + 7) % 7;
		d += (r->week - 1) * 7L;
	} else {
		int ny = r->month == 12 ? y + 1 : y, nm = r->month == 12 ? 1 : r->month + 1;
		d = cal_days(ny, nm, 1) - 1;
		d -= (cal_wday(d) - r->wday + 7) % 7;
	}
	return d * 86400L + r->secs;
}

/* a wall-clock time in a zone the invitation described -> UTC */
static long tz_to_utc(const TzDef *z, long t)
{
	int y, m, d;
	long on, off;
	int dst = 0;
	if (!z->has_std && !z->has_dst) return t;
	if (!z->has_dst || !z->dst.rule || !z->has_std || !z->std.rule)
		return t - (long)(z->has_std ? z->std.offset : z->dst.offset) * 60L;
	cal_civil(t / 86400 - (t < 0 && t % 86400), &y, &m, &d);
	on = tz_change(&z->dst, y);
	off = tz_change(&z->std, y);
	if (on < off) dst = t >= on && t < off;          /* northern summer */
	else dst = t >= on || t < off;                   /* southern */
	return t - (long)(dst ? z->dst.offset : z->std.offset) * 60L;
}

/* well-known zone names (IANA and Windows) when the invitation did not
   describe its zone: the Psion's list (caltz.c) */
static int known_zone(const char *tzid)
{
	static const struct { const char *name; int zone; } k[] = {
		{ "Europe/London", 1 }, { "Europe/Dublin", 1 }, { "Europe/Lisbon", 1 }, { "GMT Standard Time", 1 },
		{ "Europe/Paris", 2 }, { "Europe/Berlin", 2 }, { "Europe/Rome", 2 }, { "Europe/Madrid", 2 },
		{ "Europe/Amsterdam", 2 }, { "Europe/Brussels", 2 }, { "Europe/Vienna", 2 }, { "Europe/Zurich", 2 },
		{ "Europe/Stockholm", 2 }, { "Europe/Oslo", 2 }, { "Europe/Copenhagen", 2 }, { "Europe/Prague", 2 },
		{ "Europe/Warsaw", 2 }, { "Europe/Budapest", 2 }, { "W. Europe Standard Time", 2 },
		{ "Romance Standard Time", 2 }, { "Central Europe Standard Time", 2 }, { "Central European Standard Time", 2 },
		{ "Europe/Athens", 3 }, { "Europe/Helsinki", 3 }, { "Europe/Kiev", 3 }, { "Europe/Kyiv", 3 },
		{ "Europe/Bucharest", 3 }, { "FLE Standard Time", 3 }, { "GTB Standard Time", 3 }, { "E. Europe Standard Time", 3 },
		{ "Europe/Moscow", 4 }, { "Europe/Istanbul", 4 }, { "Asia/Dubai", 5 }, { "Asia/Kolkata", 6 }, { "Asia/Calcutta", 6 },
		{ "Asia/Singapore", 7 }, { "Asia/Hong_Kong", 7 }, { "Asia/Shanghai", 7 }, { "Australia/Perth", 7 },
		{ "Asia/Tokyo", 8 }, { "Asia/Seoul", 8 }, { "Tokyo Standard Time", 8 }, { "Australia/Brisbane", 9 },
		{ "Australia/Sydney", 10 }, { "Australia/Melbourne", 10 }, { "AUS Eastern Standard Time", 10 },
		{ "Pacific/Auckland", 11 }, { "New Zealand Standard Time", 11 }, { "Pacific/Honolulu", 12 },
		{ "America/Anchorage", 13 }, { "America/Los_Angeles", 14 }, { "America/Vancouver", 14 },
		{ "Pacific Standard Time", 14 }, { "America/Phoenix", 15 }, { "US Mountain Standard Time", 15 },
		{ "America/Denver", 16 }, { "America/Edmonton", 16 }, { "Mountain Standard Time", 16 },
		{ "America/Chicago", 17 }, { "America/Winnipeg", 17 }, { "Central Standard Time", 17 },
		{ "America/New_York", 18 }, { "America/Toronto", 18 }, { "America/Detroit", 18 }, { "Eastern Standard Time", 18 },
		{ "America/Halifax", 19 }, { "Atlantic Standard Time", 19 }, { "America/Sao_Paulo", 20 },
		{ "America/Argentina/Buenos_Aires", 20 }, { "UTC", 0 }, { "Etc/UTC", 0 }, { "GMT", 1 }, { "Etc/GMT", 0 },
		{ 0, 0 } };
	int i;
	const char *p = tzid;
	/* "/mozilla.org/20050126_1/Europe/London" and the like: the name at the end */
	for (i = 0; k[i].name; i++) {
		int n = (int)strlen(k[i].name), l = (int)strlen(p);
		if (l >= n && !pm_strcasecmp(p + l - n, k[i].name) && (l == n || p[l - n - 1] == '/')) return k[i].zone;
	}
	return -1;
}

/* "PT1H30M", "-P1W" -> seconds (no sign), each number held under 1000
   and the total under 400 days, so nothing overflows a 32-bit long */
static long dur_secs(const char *p)
{
	long s = 0, n = 0;
	int t = 0;
	if (*p == '-' || *p == '+') p++;
	if (*p != 'P') return -1;
	for (p++; *p; p++) {
		if (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); if (n > 999) n = 999; continue; }
		if (*p == 'T') t = 1;
		else if (*p == 'W') s += n * 7 * 86400L;
		else if (*p == 'D') s += n * 86400L;
		else if (*p == 'H') s += n * 3600L;
		else if (*p == 'M' && t) s += n * 60L;
		else if (*p == 'S') s += n;
		if (s > 400L * 86400L) s = 400L * 86400L;
		n = 0;
	}
	return s;
}

/* ------------------------------------------------------------ invitations */

/* a DTSTART/DTEND/RECURRENCE-ID value -> seconds, UTC when it can be */
static int inv_time(const PmInvite *iv, const char *params, const char *v, long *t, int *utc, int *date_only, int *tzok)
{
	char tzid[80];
	int i, z;
	if (cal_parse_time(v, t, utc, date_only) != 0) return -1;
	*tzok = 1;
	if (*utc || *date_only) return 0;
	if (!get_param(params, "TZID", tzid, sizeof(tzid))) return 0;    /* floating: the Psion's own clock */
	for (i = 0; i < iv->ntz; i++)
		if (!strcmp(iv->tz[i].tzid, tzid)) { *t = tz_to_utc(&iv->tz[i], *t); *utc = 1; return 0; }
	if ((z = known_zone(tzid)) >= 0) { *t = cal_local_to_utc(z, *t); *utc = 1; return 0; }
	*tzok = 0;                                  /* a zone we can't place: taken as the Psion's */
	return 0;
}

/* first pass: the time zones the invitation describes */
static void read_zones(const char *text, int len, PmInvite *iv, char *line)
{
	char name[40], params[200];
	int pos = 0, in_tz = 0, sub = 0;       /* sub: 1 STANDARD, 2 DAYLIGHT */
	TzDef *z = 0;
	TzRule r;
	long rstart = 0;
	memset(&r, 0, sizeof(r));
	while (ics_line(text, len, &pos, line, LINE_MAX_INV) >= 0) {
		const char *v = ics_prop(line, name, sizeof(name), params, sizeof(params));
		if (!strcmp(name, "BEGIN")) {
			if (!pm_strcasecmp(v, "VTIMEZONE") && !in_tz) {
				in_tz = 1; sub = 0;
				z = iv->ntz < INV_MAX_TZ ? &iv->tz[iv->ntz] : 0;
				if (z) memset(z, 0, sizeof(*z));
			} else if (in_tz && (!pm_strcasecmp(v, "STANDARD") || !pm_strcasecmp(v, "DAYLIGHT"))) {
				sub = !pm_strcasecmp(v, "STANDARD") ? 1 : 2;
				memset(&r, 0, sizeof(r));
				rstart = 0;
			}
			continue;
		}
		if (!strcmp(name, "END")) {
			if (in_tz && sub && (!pm_strcasecmp(v, "STANDARD") || !pm_strcasecmp(v, "DAYLIGHT"))) {
				if (z && r.ok) {
					/* the newest of each kind: the rule in force now */
					TzRule *dst = sub == 1 ? &z->std : &z->dst;
					int *has = sub == 1 ? &z->has_std : &z->has_dst;
					if (!*has || rstart >= dst->since) { *dst = r; dst->since = rstart; *has = 1; }
				}
				sub = 0;
			} else if (in_tz && !pm_strcasecmp(v, "VTIMEZONE")) {
				if (z && z->tzid[0] && (z->has_std || z->has_dst)) iv->ntz++;
				in_tz = 0; z = 0;
			}
			continue;
		}
		if (!in_tz || !z) continue;
		if (!strcmp(name, "TZID") && !sub) pm_copy(z->tzid, v, sizeof(z->tzid));
		else if (sub && !strcmp(name, "TZOFFSETTO")) { int ok; r.offset = tz_offset(v, &ok); r.ok = ok; }
		else if (sub && !strcmp(name, "DTSTART")) {
			long t; int u, d;
			if (cal_parse_time(v, &t, &u, &d) == 0) {
				rstart = t;
				r.secs = (int)(((t % 86400) + 86400) % 86400);
			}
		} else if (sub && !strcmp(name, "RRULE")) tz_rrule(&r, v);
	}
}

int inv_parse(const char *text, int len, int zone, const char *me, PmInvite *iv)
{
	static char line[LINE_MAX_INV];
	char name[40], params[200], tmp[200];
	int pos = 0, in_cal = 0, in_ev = 0, depth = 0, have_end = 0, chosen = 0;
	long dur = -1;
	static PmInvite cur;                   /* (static: the engine's stack is small) */
	static TzDef tz[INV_MAX_TZ];

	memset(iv, 0, sizeof(*iv));
	if (!text || len <= 0) return -1;
	read_zones(text, len, iv, line);
	memset(&cur, 0, sizeof(cur));
	while (ics_line(text, len, &pos, line, sizeof(line)) >= 0) {
		const char *v = ics_prop(line, name, sizeof(name), params, sizeof(params));
		if (!strcmp(name, "BEGIN")) {
			if (!pm_strcasecmp(v, "VCALENDAR")) in_cal = 1;
			else if (!pm_strcasecmp(v, "VEVENT") && !in_ev) {
				/* each event starts from the calendar's method and zones */
				pm_copy(cur.method, iv->method, sizeof(cur.method));
				in_ev = 1; depth = 0; have_end = 0; dur = -1;
				cur.alarm = -1;
				cur.tz_ok = 1;
			} else if (in_ev) depth++;
			continue;
		}
		if (!strcmp(name, "END")) {
			if (in_ev && depth > 0) { depth--; continue; }
			if (in_ev && !pm_strcasecmp(v, "VEVENT")) {
				in_ev = 0;
				iv->nevents++;
				if (!have_end) cur.uend = cur.ustart + (dur >= 0 ? dur : cur.allday ? 86400L : 0);
				if (cur.uend < cur.ustart) cur.uend = cur.ustart;
				/* the master (no RECURRENCE-ID) wins over an exception */
				if (cur.have_start && (!chosen || (iv->recur_line[0] && !cur.recur_line[0]))) {
					char method[16];
					int ntz = iv->ntz, nev = iv->nevents;
					memcpy(method, iv->method, sizeof(method));
					memcpy(tz, iv->tz, sizeof(tz));
					*iv = cur;
					memcpy(iv->method, method, sizeof(method));
					memcpy(iv->tz, tz, sizeof(tz));
					iv->ntz = ntz;
					iv->nevents = nev;
					chosen = 1;
				}
				{
					char method[16];
					pm_copy(method, iv->method, sizeof(method));
					memset(&cur, 0, sizeof(cur));
					pm_copy(cur.method, method, sizeof(cur.method));
				}
			} else if (!pm_strcasecmp(v, "VCALENDAR")) in_cal = 0;
			continue;
		}
		if (!in_ev) {
			if (in_cal && !strcmp(name, "METHOD")) { pm_copy(tmp, v, 16); lower_copy(iv->method, tmp, sizeof(iv->method)); }
			continue;
		}
		if (depth > 0) {
			/* a VALARM: its TRIGGER, for the Agenda's alarm */
			if (!strcmp(name, "TRIGGER") && !pm_stristr(params, "VALUE=DATE-TIME") && cur.alarm < 0) {
				long d = dur_secs(v);
				/* before the start (or at it), and not weeks before */
				if (d >= 0 && (*v == '-' || d == 0) && d <= 40L * 86400L) cur.alarm = (int)(d / 60);
			}
			continue;
		}
		if (!strcmp(name, "UID")) pm_copy(cur.uid, v, sizeof(cur.uid));
		else if (!strcmp(name, "SUMMARY")) ics_text(v, cur.summary, sizeof(cur.summary));
		else if (!strcmp(name, "LOCATION")) ics_text(v, cur.location, sizeof(cur.location));
		else if (!strcmp(name, "SEQUENCE")) { long s = atol(v); cur.seq = s < 0 ? 0 : s > 1000000L ? 1000000 : (int)s; }
		else if (!strcmp(name, "STATUS")) cur.cancelled = !pm_strcasecmp(v, "CANCELLED");
		else if (!strcmp(name, "RRULE") || !strcmp(name, "RDATE")) cur.repeats = 1;
		else if (!strcmp(name, "RECURRENCE-ID")) {
			snprintf(cur.recur_line, sizeof(cur.recur_line), "%s", line);
			if ((int)strlen(line) >= (int)sizeof(cur.recur_line)) cur.recur_line[0] = 0;   /* (cut short: useless) */
		} else if (!strcmp(name, "DTSTART") || !strcmp(name, "DTEND")) {
			long t; int utc, date_only, tzok;
			if (inv_time(iv, params, v, &t, &utc, &date_only, &tzok) != 0) continue;
			if (!strcmp(name, "DTSTART")) {
				cur.ustart = t; cur.allday = date_only; cur.floating = !utc && !date_only;
				cur.have_start = 1;
				if (!tzok) cur.tz_ok = 0;
			} else { cur.uend = t; have_end = 1; }
		} else if (!strcmp(name, "DURATION")) {
			if (*v != '-') dur = dur_secs(v);
		} else if (!strcmp(name, "ORGANIZER")) {
			char cn[100];
			mail_addr(v, cur.org_addr, sizeof(cur.org_addr));
			if (get_param(params, "CN", cn, sizeof(cn))) to_cp(cn, cur.org_name, sizeof(cur.org_name));
			snprintf(cur.org_line, sizeof(cur.org_line), "%s", line);
			if ((int)strlen(line) >= (int)sizeof(cur.org_line)) {
				/* too long to send back as it came: just the address */
				snprintf(cur.org_line, sizeof(cur.org_line), "ORGANIZER:mailto:%.*s", (int)sizeof(cur.org_line) - 20, cur.org_addr);
			}
		} else if (!strcmp(name, "ATTENDEE")) {
			char addr[120], ps[24], cn[100];
			mail_addr(v, addr, sizeof(addr));
			get_param(params, "PARTSTAT", ps, sizeof(ps));
			cur.nattendees++;
			if (me && me[0] && !pm_strcasecmp(addr, me)) {
				pm_copy(cur.me, addr, sizeof(cur.me));
				lower_copy(cur.my_partstat, ps, sizeof(cur.my_partstat));
			}
			/* in a reply, the one who answered */
			if (!cur.reply_addr[0]) {
				pm_copy(cur.reply_addr, addr, sizeof(cur.reply_addr));
				lower_copy(cur.reply_partstat, ps, sizeof(cur.reply_partstat));
				cur.reply_name[0] = 0;
				if (get_param(params, "CN", cn, sizeof(cn))) to_cp(cn, cur.reply_name, sizeof(cur.reply_name));
			}
		}
	}
	if (!chosen) return -1;
	if (!iv->method[0]) pm_copy(iv->method, "publish", sizeof(iv->method));
	if (iv->cancelled && !strcmp(iv->method, "publish")) pm_copy(iv->method, "cancel", sizeof(iv->method));
	/* the Psion's own clock: what the Agenda wants */
	if (iv->allday) {
		iv->lstart = iv->ustart;
		iv->lend = iv->uend - 86400L;           /* DTEND is the day after */
		if (iv->lend < iv->lstart) iv->lend = iv->lstart;
	} else if (iv->floating) {
		iv->lstart = iv->ustart;
		iv->lend = iv->uend;
	} else {
		iv->lstart = cal_utc_to_local(zone, iv->ustart);
		iv->lend = cal_utc_to_local(zone, iv->uend);
	}
	if (!iv->summary[0]) pm_copy(iv->summary, "(no title)", sizeof(iv->summary));
	return 0;
}

/* tabs and line ends would break the summary file */
static void put_field(FILE *f, const char *key, const char *v)
{
	fprintf(f, "%s\t", key);
	for (; *v; v++) fputc(*v == '\t' || *v == '\r' || *v == '\n' ? ' ' : *v, f);
	fputc('\n', f);
}

void inv_write(FILE *f, const PmInvite *iv)
{
	char t[24];
	fprintf(f, "#PSIINV1\n");
	put_field(f, "method", iv->method);
	put_field(f, "uid", iv->uid);
	fprintf(f, "seq\t%d\n", iv->seq);
	put_field(f, "summary", iv->summary);
	put_field(f, "location", iv->location);
	cal_fmt_local(iv->lstart, t); put_field(f, "start", t);
	cal_fmt_local(iv->lend, t); put_field(f, "end", t);
	fprintf(f, "allday\t%d\n", iv->allday);
	/* for the reply: UTC (or the date), so no zone needs describing */
	if (iv->allday) cal_fmt_date(iv->ustart, t); else if (iv->floating || !iv->tz_ok) t[0] = 0; else cal_fmt_utc(iv->ustart, t);
	put_field(f, "utcstart", t);
	if (iv->allday) cal_fmt_date(iv->uend, t); else if (iv->floating || !iv->tz_ok) t[0] = 0; else cal_fmt_utc(iv->uend, t);
	put_field(f, "utcend", t);
	fprintf(f, "alarm\t%d\n", iv->alarm);
	fprintf(f, "repeats\t%d\n", iv->repeats);
	fprintf(f, "tzok\t%d\n", iv->tz_ok);
	put_field(f, "recur", iv->recur_line);
	put_field(f, "orgname", iv->org_name);
	put_field(f, "orgaddr", iv->org_addr);
	put_field(f, "orgline", iv->org_line);
	put_field(f, "me", iv->me);
	put_field(f, "mystatus", iv->my_partstat);
	put_field(f, "replyname", iv->reply_name);
	put_field(f, "replyaddr", iv->reply_addr);
	put_field(f, "replystatus", iv->reply_partstat);
	fprintf(f, "events\t%d\n", iv->nevents);
}

/* ------------------------------------------------------------ iTIP replies */

int itip_header(ItipReply *r, const char *name, const char *v)
{
	if (pm_strncasecmp(name, "Itip", 4)) return 0;
	name += 4;
	if (!*name) pm_copy(r->partstat, v, sizeof(r->partstat));
	else if (!pm_strcasecmp(name, "-Uid")) pm_copy(r->uid, v, sizeof(r->uid));
	else if (!pm_strcasecmp(name, "-Seq")) pm_copy(r->seq, v, sizeof(r->seq));
	else if (!pm_strcasecmp(name, "-Recur")) pm_copy(r->recur, v, sizeof(r->recur));
	else if (!pm_strcasecmp(name, "-Organizer")) pm_copy(r->organizer, v, sizeof(r->organizer));
	else if (!pm_strcasecmp(name, "-Attendee")) pm_copy(r->attendee, v, sizeof(r->attendee));
	else if (!pm_strcasecmp(name, "-Name")) pm_copy(r->cn, v, sizeof(r->cn));
	else if (!pm_strcasecmp(name, "-Summary")) pm_copy(r->summary, v, sizeof(r->summary));
	else if (!pm_strcasecmp(name, "-Start")) pm_copy(r->dtstart, v, sizeof(r->dtstart));
	else if (!pm_strcasecmp(name, "-End")) pm_copy(r->dtend, v, sizeof(r->dtend));
	return 1;
}

typedef struct { char *b; int n, max, bad; } Buf;

static void add_raw(Buf *o, const char *s, int n)
{
	if (o->n + n >= o->max) { o->bad = 1; return; }
	memcpy(o->b + o->n, s, n);
	o->n += n;
}

/* one content line (UTF-8), folded at 75 octets */
static void add_line(Buf *o, const char *s)
{
	int n = (int)strlen(s), col = 0, i = 0;
	while (i < n) {
		int k = 1;
		unsigned char c = (unsigned char)s[i];
		if (c >= 0xc0) while (i + k < n && ((unsigned char)s[i + k] & 0xc0) == 0x80) k++;
		if (col + k > 75) { add_raw(o, "\r\n ", 3); col = 1; }
		add_raw(o, s + i, k);
		col += k;
		i += k;
	}
	add_raw(o, "\r\n", 2);
}

/* a value with no line breaks or control characters in it */
static int clean_value(const char *s)
{
	for (; *s; s++) if ((unsigned char)*s < 32) return 0;
	return 1;
}

static int is_status(const char *s)
{
	return !pm_strcasecmp(s, "ACCEPTED") || !pm_strcasecmp(s, "TENTATIVE") || !pm_strcasecmp(s, "DECLINED");
}

int itip_build(const ItipReply *r, long now, char *out, int max)
{
	Buf o;
	char l[1400], utf[1300], esc[400], stamp[24];
	int n, k, i;
	long tt;
	o.b = out; o.n = 0; o.max = max; o.bad = 0;
	if (max <= 0) return -1;
	/* what goes into the calendar must be what we expect, nothing more */
	if (!is_status(r->partstat) || !r->uid[0] || !r->attendee[0] || !r->organizer[0] ||
	    !clean_value(r->uid) || !clean_value(r->attendee) || !clean_value(r->organizer) || !clean_value(r->recur) ||
	    !clean_value(r->dtstart) || !clean_value(r->dtend) || strchr(r->attendee, ':') || strchr(r->attendee, ';'))
		return -1;
	if (pm_strncasecmp(r->organizer, "ORGANIZER", 9) || (r->organizer[9] != ':' && r->organizer[9] != ';')) return -1;
	if (r->recur[0] && (pm_strncasecmp(r->recur, "RECURRENCE-ID", 13) || (r->recur[13] != ':' && r->recur[13] != ';'))) return -1;
	add_line(&o, "BEGIN:VCALENDAR");
	add_line(&o, "VERSION:2.0");
	add_line(&o, "PRODID:-//PsiMail//Psion Series 5mx//EN");
	add_line(&o, "METHOD:REPLY");
	add_line(&o, "BEGIN:VEVENT");
	snprintf(l, sizeof(l), "UID:%s", r->uid); add_line(&o, l);
	snprintf(l, sizeof(l), "SEQUENCE:%d", atoi(r->seq) < 0 ? 0 : atoi(r->seq)); add_line(&o, l);
	cal_fmt_utc(now, stamp);
	snprintf(l, sizeof(l), "DTSTAMP:%s", stamp); add_line(&o, l);
	if (r->recur[0]) add_line(&o, r->recur);
	/* times as UTC (or dates), so the reply needs no time zone of its own */
	if (r->dtstart[0] && cal_parse_time(r->dtstart, &tt, &k, &i) == 0) {
		snprintf(l, sizeof(l), strlen(r->dtstart) == 8 ? "DTSTART;VALUE=DATE:%s" : "DTSTART:%s", r->dtstart); add_line(&o, l);
		if (r->dtend[0] && cal_parse_time(r->dtend, &tt, &k, &i) == 0) {
			snprintf(l, sizeof(l), strlen(r->dtend) == 8 ? "DTEND;VALUE=DATE:%s" : "DTEND:%s", r->dtend); add_line(&o, l);
		}
	}
	add_line(&o, r->organizer);              /* as the invitation had it */
	/* ATTENDEE;PARTSTAT=ACCEPTED;CN="Dan Edge":mailto:dan@example.com */
	n = 0;
	for (i = 0; r->cn[i] && n < (int)sizeof(esc) - 1; i++)
		if (r->cn[i] != '"' && (unsigned char)r->cn[i] >= 32) esc[n++] = r->cn[i];
	esc[n] = 0;
	k = cs_cp1252_to_utf8(esc, n, utf, sizeof(utf) - 1);
	utf[k] = 0;
	{
		char ps[16];
		for (i = 0; r->partstat[i] && i < 15; i++) ps[i] = (r->partstat[i] >= 'a' && r->partstat[i] <= 'z') ? r->partstat[i] - 32 : r->partstat[i];
		ps[i] = 0;
		if (utf[0]) snprintf(l, sizeof(l), "ATTENDEE;PARTSTAT=%s;CN=\"%s\":mailto:%s", ps, utf, r->attendee);
		else snprintf(l, sizeof(l), "ATTENDEE;PARTSTAT=%s:mailto:%s", ps, r->attendee);
		add_line(&o, l);
	}
	if (r->summary[0]) {
		/* SUMMARY, escaped, from cp1252 */
		n = 0;
		for (i = 0; r->summary[i] && n < (int)sizeof(esc) - 3; i++) {
			char c = r->summary[i];
			if ((unsigned char)c < 32) continue;
			if (c == '\\' || c == ';' || c == ',') esc[n++] = '\\';
			esc[n++] = c;
		}
		k = cs_cp1252_to_utf8(esc, n, utf, sizeof(utf) - 1);
		utf[k] = 0;
		snprintf(l, sizeof(l), "SUMMARY:%s", utf);
		add_line(&o, l);
	}
	add_line(&o, "END:VEVENT");
	add_line(&o, "END:VCALENDAR");
	if (o.bad) return -1;
	out[o.n] = 0;
	return o.n;
}

/* ------------------------------------------------------------ vCards */

/* the next physical-line-joined vCard line: ics_line unfolds the RFC way;
   vCard 2.1 quoted-printable values also go on after a line ending in '=' */
static int vcf_line(const char *text, int len, int *pos, char *line, int max)
{
	int n = ics_line(text, len, pos, line, max);
	if (n < 0) return n;
	while (n > 0 && line[n - 1] == '=' && pm_stristr(line, "QUOTED-PRINTABLE") && *pos < len) {
		int m;
		line[--n] = 0;
		m = ics_line(text, len, pos, line + n, max - n);
		if (m < 0) break;
		n += m;
	}
	return n;
}

/* a value into cp1252: quoted-printable and the charset undone, then the
   escapes (\n \, \; \\); component k of a ';'-separated value (k < 0: whole) */
static void vcf_value(const char *params, const char *v, int k, char *out, int max)
{
	static char raw[LINE_MAX_INV], conv[LINE_MAX_INV];
	char cs[32];
	int n = 0, comp = 0, i;
	/* the component */
	for (i = 0; v[i] && n < (int)sizeof(raw) - 1; i++) {
		char c = v[i];
		if (c == '\\' && v[i + 1]) {
			char e = v[++i];
			if (comp == k || k < 0) {
				if (e == 'n' || e == 'N') raw[n++] = '\n';
				else raw[n++] = e;
			}
			continue;
		}
		if (c == ';' && k >= 0) { comp++; if (comp > k) break; continue; }
		if (comp == k || k < 0) raw[n++] = c;
	}
	raw[n] = 0;
	/* quoted-printable (vCard 2.1) */
	if (pm_stristr(params, "QUOTED-PRINTABLE")) {
		PmDecoder d;
		dec_init(&d, ENC_QP);
		n = dec_feed(&d, raw, n, conv);
		memcpy(raw, conv, n);
		raw[n] = 0;
	}
	if (!get_param(params, "CHARSET", cs, sizeof(cs))) pm_copy(cs, "utf-8", sizeof(cs));
	out[0] = 0;
	n = cs_to_cp1252(cs, raw, n, out, max);
	if (n >= 0 && n < max) out[n] = 0;
	/* no CRs (quoted-printable "=0D=0A" line ends), trimmed */
	{
		int j = 0;
		for (i = 0; out[i]; i++) if (out[i] != '\r') out[j++] = out[i];
		out[j] = 0;
	}
	n = (int)strlen(out);
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\n')) out[--n] = 0;
	for (i = 0; out[i] == ' '; i++) ;
	if (i) memmove(out, out + i, strlen(out + i) + 1);
}

/* the kind of a TEL / EMAIL / ADR from its parameters: vCard 2.1 has bare
   words (TEL;WORK;FAX), 3.0 TYPE=WORK,FAX, 4.0 TYPE=work */
static int vcf_kind(const char *params)
{
	int k = 0;
	if (pm_stristr(params, "HOME")) k |= VCF_HOME;
	if (pm_stristr(params, "WORK")) k |= VCF_WORK;
	if (pm_stristr(params, "CELL")) k |= VCF_CELL;
	if (pm_stristr(params, "FAX")) k |= VCF_FAX;
	if (pm_stristr(params, "PAGER")) k |= VCF_PAGER;
	return k;
}

int vcf_parse(const char *text, int len, PmCard *cards, int max)
{
	static char line[LINE_MAX_INV], val[400];
	char name[40], params[200];
	int pos = 0, n = 0, in = 0;
	PmCard *c = 0;
	if (!text || len <= 0 || max <= 0) return 0;
	while (vcf_line(text, len, &pos, line, sizeof(line)) >= 0) {
		const char *v;
		char *dot;
		/* "item1.EMAIL;TYPE=INTERNET:..." - the group goes */
		v = ics_prop(line, name, sizeof(name), params, sizeof(params));
		if ((dot = strchr(name, '.')) != 0) memmove(name, dot + 1, strlen(dot + 1) + 1);
		if (!strcmp(name, "BEGIN") && !pm_strcasecmp(v, "VCARD")) {
			if (n >= max) break;
			c = &cards[n];
			memset(c, 0, sizeof(*c));
			in = 1;
			continue;
		}
		if (!in || !c) continue;
		if (!strcmp(name, "END") && !pm_strcasecmp(v, "VCARD")) {
			in = 0;
			if (!c->fn[0]) {
				/* a name to show: from N, else the organisation */
				snprintf(c->fn, sizeof(c->fn), "%s%s%s", c->given, c->given[0] && c->family[0] ? " " : "", c->family);
				if (!c->fn[0]) pm_copy(c->fn, c->org, sizeof(c->fn));
			}
			if (c->fn[0] || c->nemail || c->ntel) n++;
			c = 0;
			continue;
		}
		/* inline pictures and sounds are not wanted (and can be huge) */
		if (!strcmp(name, "PHOTO") || !strcmp(name, "LOGO") || !strcmp(name, "SOUND") || !strcmp(name, "KEY")) continue;
		if (!strcmp(name, "FN")) vcf_value(params, v, -1, c->fn, sizeof(c->fn));
		else if (!strcmp(name, "N")) {
			vcf_value(params, v, 0, c->family, sizeof(c->family));
			vcf_value(params, v, 1, c->given, sizeof(c->given));
			vcf_value(params, v, 2, c->middle, sizeof(c->middle));
			vcf_value(params, v, 3, c->prefix, sizeof(c->prefix));
			vcf_value(params, v, 4, c->suffix, sizeof(c->suffix));
		} else if (!strcmp(name, "ORG")) vcf_value(params, v, 0, c->org, sizeof(c->org));
		else if (!strcmp(name, "TITLE")) vcf_value(params, v, -1, c->title, sizeof(c->title));
		else if (!strcmp(name, "URL") && !c->url[0]) vcf_value(params, v, -1, c->url, sizeof(c->url));
		else if (!strcmp(name, "NOTE") && !c->note[0]) vcf_value(params, v, -1, c->note, sizeof(c->note));
		else if (!strcmp(name, "EMAIL") && c->nemail < VCF_MAX_EMAIL) {
			vcf_value(params, v, -1, val, sizeof(val));
			if (!pm_strncasecmp(val, "mailto:", 7)) memmove(val, val + 7, strlen(val + 7) + 1);
			if (strchr(val, '@') && strlen(val) < sizeof(c->email[0])) {
				pm_copy(c->email[c->nemail], val, sizeof(c->email[0]));
				c->email_kind[c->nemail++] = vcf_kind(params);
			}
		} else if (!strcmp(name, "TEL") && c->ntel < VCF_MAX_TEL) {
			char *p = val;
			vcf_value(params, v, -1, val, sizeof(val));
			if (!pm_strncasecmp(p, "tel:", 4)) p += 4;       /* vCard 4.0: a URI */
			if (*p && strlen(p) < sizeof(c->tel[0])) {
				pm_copy(c->tel[c->ntel], p, sizeof(c->tel[0]));
				c->tel_kind[c->ntel++] = vcf_kind(params);
			}
		} else if (!strcmp(name, "ADR") && c->nadr < VCF_MAX_ADR) {
			/* PO box; extended; street; town; region; postcode; country:
			   all seven kept in place, \x01 between them, so the app can put
			   each in its own Contacts field where there is one */
			char *a = c->adr[c->nadr];
			int k, at = 0, any = 0;
			a[0] = 0;
			for (k = 0; k < 7; k++) {
				vcf_value(params, v, k, val, sizeof(val));
				{
					char *q;
					for (q = val; *q; q++) if (*q == '\n' || *q == '\t' || *q == '\x01') *q = VCF_ADR_SEP;
				}
				if (val[0]) any = 1;
				at += snprintf(a + at, sizeof(c->adr[0]) - at, "%s%s", k ? "\x01" : "", val);
				if (at >= (int)sizeof(c->adr[0])) { at = (int)sizeof(c->adr[0]) - 1; break; }
			}
			if (!any) a[0] = 0;
			if (a[0]) c->adr_kind[c->nadr++] = vcf_kind(params);
		}
	}
	return n;
}

void vcf_write(FILE *f, const PmCard *cards, int n)
{
	int i, k;
	fprintf(f, "#PSIVCD1\t%d\n", n);
	for (i = 0; i < n; i++) {
		const PmCard *c = &cards[i];
		fprintf(f, "begin\t%d\n", i + 1);
		put_field(f, "fn", c->fn);
		put_field(f, "given", c->given);
		put_field(f, "family", c->family);
		put_field(f, "middle", c->middle);
		put_field(f, "prefix", c->prefix);
		put_field(f, "suffix", c->suffix);
		put_field(f, "org", c->org);
		put_field(f, "title", c->title);
		put_field(f, "url", c->url);
		put_field(f, "note", c->note);
		for (k = 0; k < c->nemail; k++) { fprintf(f, "email%d", c->email_kind[k]); put_field(f, "", c->email[k]); }
		for (k = 0; k < c->ntel; k++) { fprintf(f, "tel%d", c->tel_kind[k]); put_field(f, "", c->tel[k]); }
		for (k = 0; k < c->nadr; k++) { fprintf(f, "adr%d", c->adr_kind[k]); put_field(f, "", c->adr[k]); }
		fprintf(f, "end\n");
	}
}
