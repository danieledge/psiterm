/* caltz.c - dates, and the time zones PsiMail knows (see cal.h)
 *
 * Calendar servers send times in UTC; the Psion's Agenda keeps wall-clock
 * times. Each zone is a standard offset and one of a handful of daylight
 * saving rules - enough for everywhere that has used the same rule since
 * 2007. The list matches the app's (psimail.rss, r_pm_zones).
 */
#include <stdio.h>
#include <string.h>
#include "pm.h"
#include "cal.h"

enum { R_NONE, R_EU, R_US, R_AU, R_NZ };
typedef struct { int std; int rule; } Zone;   /* std: minutes east of UTC */
static const Zone zones[] =
	{
	{ 0, R_NONE },           /* UTC */
	{ 0, R_EU },             /* London, Dublin, Lisbon */
	{ 60, R_EU },            /* Paris, Berlin, Rome, Madrid */
	{ 120, R_EU },           /* Athens, Helsinki, Kyiv */
	{ 180, R_NONE },         /* Moscow, Istanbul */
	{ 240, R_NONE },         /* Dubai */
	{ 330, R_NONE },         /* India */
	{ 480, R_NONE },         /* Singapore, Hong Kong, Perth */
	{ 540, R_NONE },         /* Tokyo, Seoul */
	{ 600, R_NONE },         /* Brisbane */
	{ 600, R_AU },           /* Sydney, Melbourne */
	{ 720, R_NZ },           /* Auckland */
	{ -600, R_NONE },        /* Hawaii */
	{ -540, R_US },          /* Alaska */
	{ -480, R_US },          /* Pacific (US, Canada) */
	{ -420, R_NONE },        /* Arizona */
	{ -420, R_US },          /* Mountain (US, Canada) */
	{ -360, R_US },          /* Central (US, Canada) */
	{ -300, R_US },          /* Eastern (US, Canada) */
	{ -240, R_US },          /* Atlantic (Canada) */
	{ -180, R_NONE },        /* Sao Paulo, Buenos Aires */
	};
#define NZONES ((int)(sizeof(zones) / sizeof(zones[0])))

int cal_zone_count(void) { return NZONES; }

/* Howard Hinnant's days_from_civil */
long cal_days(int y, int m, int d)
{
	long era, yoe, doy, doe;
	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

void cal_civil(long z, int *y, int *m, int *d)
{
	long era, doe, yoe, doy, mp;
	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = (int)(yoe + era * 400 + (*m <= 2));
}

int cal_wday(long days) { return (int)((days % 7 + 11) % 7); }   /* 1970-01-01 was a Thursday */

/* the n-th (1..) Sunday of a month, or the last one (n = 0) */
static long sunday(int y, int m, int n)
{
	long d;
	if (n == 0) {
		int ny = m == 12 ? y + 1 : y, nm = m == 12 ? 1 : m + 1;
		d = cal_days(ny, nm, 1) - 1;
		return d - cal_wday(d);
	}
	d = cal_days(y, m, 1);
	d += (7 - cal_wday(d)) % 7;
	return d + (n - 1) * 7;
}

/* is daylight saving on at this UTC time? */
static int dst(const Zone *z, long utc)
{
	int y, m, d;
	long on, off, std = z->std * 60L;
	cal_civil(utc / 86400 - (utc < 0 && utc % 86400), &y, &m, &d);
	switch (z->rule) {
	case R_EU:     /* 01:00 UTC, last Sunday of March to last Sunday of October */
		on = sunday(y, 3, 0) * 86400 + 3600;
		off = sunday(y, 10, 0) * 86400 + 3600;
		return utc >= on && utc < off;
	case R_US:     /* 02:00 local, 2nd Sunday of March to 1st Sunday of November */
		on = sunday(y, 3, 2) * 86400 + 7200 - std;
		off = sunday(y, 11, 1) * 86400 + 7200 - std - 3600;
		return utc >= on && utc < off;
	case R_AU:     /* 02:00 local, 1st Sunday of October to 03:00 (DST) 1st Sunday of April */
		off = sunday(y, 4, 1) * 86400 + 3 * 3600 - std - 3600;
		on = sunday(y, 10, 1) * 86400 + 7200 - std;
		return utc < off || utc >= on;
	case R_NZ:     /* 02:00 local, last Sunday of September to 03:00 (DST) 1st Sunday of April */
		off = sunday(y, 4, 1) * 86400 + 3 * 3600 - std - 3600;
		on = sunday(y, 9, 0) * 86400 + 7200 - std;
		return utc < off || utc >= on;
	}
	return 0;
}

long cal_utc_to_local(int zone, long utc)
{
	const Zone *z = &zones[zone >= 0 && zone < NZONES ? zone : 0];
	return utc + z->std * 60L + (dst(z, utc) ? 3600 : 0);
}

long cal_local_to_utc(int zone, long local)
{
	const Zone *z = &zones[zone >= 0 && zone < NZONES ? zone : 0];
	long u = local - z->std * 60L;
	/* in summer time the clocks are an hour ahead; in the missing hour in
	   spring and the repeated one in autumn this picks standard time */
	if (dst(z, u - 3600)) u -= 3600;
	return u;
}

static int num(const char *s, int n)
{
	int v = 0;
	while (n--) {
		if (*s < '0' || *s > '9') return -1;
		v = v * 10 + (*s++ - '0');
	}
	return v;
}

int cal_parse_time(const char *s, long *t, int *utc, int *date_only)
{
	int y = num(s, 4), m = y < 0 ? -1 : num(s + 4, 2), d = m < 0 ? -1 : num(s + 6, 2), hh = 0, mm = 0, ss = 0;
	/* (a long is 32 bits on the Psion: seconds since 1970 reach 1901..2038,
	   and an event up to 400 days long must still fit after its start) */
	if (y < 1902 || y > 2035 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
	*utc = 0;
	*date_only = 1;
	if (s[8] == 'T') {
		hh = num(s + 9, 2); mm = hh < 0 ? -1 : num(s + 11, 2);
		if (hh < 0 || mm < 0 || hh > 24 || mm > 59) return -1;
		ss = num(s + 13, 2);
		*date_only = 0;
		/* (each read stops at the first character that is not a digit, so
		   none goes past the end of a short value) */
		if (ss < 0) { ss = 0; *utc = s[13] == 'Z'; }
		else *utc = s[15] == 'Z';
		if (ss > 60) ss = 0;
	}
	*t = cal_days(y, m, d) * 86400 + hh * 3600L + mm * 60L + ss;
	return 0;
}

static void split(long t, int *y, int *m, int *d, int *hh, int *mm, int *ss)
{
	long days = t / 86400, sec = t % 86400;
	if (sec < 0) { sec += 86400; days--; }
	cal_civil(days, y, m, d);
	*hh = (int)(sec / 3600); *mm = (int)(sec / 60 % 60); *ss = (int)(sec % 60);
}

void cal_fmt_utc(long t, char *out)
{
	int y, m, d, hh, mm, ss;
	split(t, &y, &m, &d, &hh, &mm, &ss);
	sprintf(out, "%04d%02d%02dT%02d%02d%02dZ", y, m, d, hh, mm, ss);
}

void cal_fmt_date(long t, char *out)
{
	int y, m, d, hh, mm, ss;
	split(t, &y, &m, &d, &hh, &mm, &ss);
	sprintf(out, "%04d%02d%02d", y, m, d);
}

void cal_fmt_local(long t, char *out)
{
	int y, m, d, hh, mm, ss;
	split(t, &y, &m, &d, &hh, &mm, &ss);
	sprintf(out, "%04d%02d%02d%02d%02d", y, m, d, hh, mm);
}

long cal_parse_local(const char *s)
{
	int y = num(s, 4), m = y < 0 ? -1 : num(s + 4, 2), d = m < 0 ? -1 : num(s + 6, 2);
	int hh = d < 0 ? -1 : num(s + 8, 2), mm = hh < 0 ? -1 : num(s + 10, 2);
	if (y < 0 || m < 1 || d < 1) return 0;
	if (hh < 0) hh = 0;
	if (mm < 0) mm = 0;
	return cal_days(y, m, d) * 86400 + hh * 3600L + mm * 60L;
}
