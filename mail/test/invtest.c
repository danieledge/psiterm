/* invtest.c - the invitation and contact card parsers (engine/invite.c)
 * on a PC, for invtest.py:
 *
 *   invtest ics FILE ZONE ME      prints the .inv summary
 *   invtest vcf FILE              prints the .vcd summary
 *   invtest reply FILE ZONE ME PARTSTAT NAME
 *                                 parses FILE, then prints the iTIP REPLY
 *                                 built from it as compose.c would
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "../engine/pm.h"
#include "../engine/cal.h"
#include "../engine/invite.h"

/* ---- stand-ins for pmmain.c */
void pm_copy(char *dst, const char *src, int max)
{
	int i = 0;
	if (max <= 0) return;
	if (src) while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
	dst[i] = 0;
}
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int pm_strcasecmp(const char *a, const char *b)
{
	while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; }
	return lc((unsigned char)*a) - lc((unsigned char)*b);
}
int pm_strncasecmp(const char *a, const char *b, int n)
{
	while (n > 0 && *a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; n--; }
	if (n == 0) return 0;
	return lc((unsigned char)*a) - lc((unsigned char)*b);
}
const char *pm_stristr(const char *hay, const char *needle)
{
	int n = (int)strlen(needle);
	if (!n) return hay;
	for (; *hay; hay++) if (!pm_strncasecmp(hay, needle, n)) return hay;
	return 0;
}
long pm_time(void) { return 1790000000L; }
void pm_log(const char *fmt, ...) { (void)fmt; }

static char *slurp(const char *path, int *len)
{
	FILE *f = fopen(path, "rb");
	static char buf[200000];
	if (!f) { perror(path); exit(2); }
	*len = (int)fread(buf, 1, sizeof(buf) - 1, f);
	buf[*len] = 0;
	fclose(f);
	return buf;
}

int main(int argc, char **argv)
{
	int len;
	char *text;
	if (argc < 3) { fprintf(stderr, "usage: see invtest.c\n"); return 2; }
	text = slurp(argv[2], &len);
	if (!strcmp(argv[1], "ics") || !strcmp(argv[1], "reply")) {
		static PmInvite iv;
		int zone = argc > 3 ? atoi(argv[3]) : 1;
		if (inv_parse(text, len, zone, argc > 4 ? argv[4] : "", &iv) != 0) { printf("no event\n"); return 1; }
		if (!strcmp(argv[1], "ics")) { inv_write(stdout, &iv); return 0; }
		{
			/* the outbox headers the app writes, then compose.c's part */
			static ItipReply r;
			static char out[4096];
			char t[24];
			int n;
			memset(&r, 0, sizeof(r));
			itip_header(&r, "Itip", argc > 5 ? argv[5] : "ACCEPTED");
			itip_header(&r, "Itip-Uid", iv.uid);
			snprintf(t, sizeof(t), "%d", iv.seq);
			itip_header(&r, "Itip-Seq", t);
			itip_header(&r, "Itip-Recur", iv.recur_line);
			itip_header(&r, "Itip-Organizer", iv.org_line);
			itip_header(&r, "Itip-Attendee", iv.me[0] ? iv.me : argv[4]);
			itip_header(&r, "Itip-Name", argc > 6 ? argv[6] : "");
			itip_header(&r, "Itip-Summary", iv.summary);
			if (iv.allday) cal_fmt_date(iv.ustart, t); else cal_fmt_utc(iv.ustart, t);
			itip_header(&r, "Itip-Start", iv.floating ? "" : t);
			if (iv.allday) cal_fmt_date(iv.uend, t); else cal_fmt_utc(iv.uend, t);
			itip_header(&r, "Itip-End", iv.floating ? "" : t);
			n = itip_build(&r, pm_time(), out, sizeof(out));
			if (n < 0) { printf("no reply\n"); return 1; }
			fwrite(out, 1, n, stdout);
			return 0;
		}
	}
	if (!strcmp(argv[1], "vcf")) {
		static PmCard cards[8];
		int n = vcf_parse(text, len, cards, 8);
		vcf_write(stdout, cards, n);
		return n > 0 ? 0 : 1;
	}
	return 2;
}
