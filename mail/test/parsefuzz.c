/* parsefuzz.c - the engine's parsers of untrusted network text on a PC,
 * built with AddressSanitizer/UBSan by parsefuzz.py:
 *
 *   parsefuzz-asan SEED ROUNDS
 *
 * Every round makes a random or mutated input (a few hundred bytes to a
 * few tens of KB) and feeds it, split at random points, to
 *   imapparse (ip_parse) + mime (mime_structure and the ip_* readers),
 *   mime's base64 / quoted-printable decoder,
 *   html (html_feed / html_end),
 *   xmlscan (xs_feed),
 *   ics (ics_each, ics_change, ics_exclude),
 *   charset (cs_decode_header, cs_to_cp1252, cs_mutf7_decode),
 *   invite (inv_parse with time zones, the iTIP reply built from what it
 *   found, vcf_parse for vCard 2.1/3.0/4.0).
 * A crash or sanitizer report fails the run. Only the engine files are
 * linked: the few pmmain.c helpers the parsers use are copied below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "../engine/pm.h"
#include "../engine/cal.h"
#include "../engine/invite.h"

/* ---- stand-ins for pmmain.c / the platform */
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
long pm_time(void) { return (long)time(0); }
void pm_log(const char *fmt, ...) { (void)fmt; }

/* ---- a small deterministic generator */
static unsigned long g_seed;
static unsigned rnd(void) { g_seed = g_seed * 1103515245UL + 12345UL; return (unsigned)(g_seed >> 8) & 0xffffff; }
static int rnd_n(int n) { return n <= 1 ? 0 : (int)(rnd() % (unsigned)n); }

static const char *k_imap_words[] = {
	"(", ")", "\"", "{", "}", "NIL", "nil", "BODYSTRUCTURE", "ENVELOPE", "FLAGS", "UID", "RFC822.SIZE",
	"INTERNALDATE", "BODY[HEADER.FIELDS (LIST-ID)]", "\"text\"", "\"plain\"", "\"html\"", "\"multipart\"",
	"\"mixed\"", "\"alternative\"", "\"charset\"", "\"utf-8\"", "\"name\"", "\"filename*\"", "\"filename*0*\"",
	"\"filename*1\"", "\"attachment\"", "\"inline\"", "\"base64\"", "\"quoted-printable\"", "\"message\"",
	"\"rfc822\"", "\"image\"", "\"jpeg\"", "\"format\"", "\"flowed\"", "\"delsp\"", "\"yes\"", "\"<id@x>\"",
	"{5}\r\nhello", "{99999999999}\r\n", "{-1}\r\n", "{0}\r\n", "\"a\\\"b\\\\c\"", "\"utf-8''%41%42\"",
	"\"iso-8859-1'en'caf%E9\"", "12345", "0", "4294967295", "99999999999999999999", " ", "\r\n",
	"=?utf-8?q?caf=C3=A9?=", "=?iso-8859-1?b?Y2Fmw6k=?=", "\"=?utf-8?b?" "AAAA" "?=\"", 0 };

static const char *k_html_words[] = {
	"<", ">", "</", "<p>", "</p>", "<br>", "<div>", "<table><tr><td>", "</td></tr></table>", "<ul>", "<ol>",
	"<li>", "</li>", "</ul>", "<h1>", "</h1>", "<h6>", "<blockquote>", "</blockquote>", "<pre>", "</pre>",
	"<b>", "</b>", "<i>", "</i>", "<a href=\"http://x/y\">", "<a href='mailto:a@b'>", "</a>", "<img src=\"cid:a\" alt=\"pic\">",
	"<img width=1 height=1 src=x>", "<script>", "</script>", "<style>", "</style>", "<!--", "-->", "&amp;", "&#x41;", "&#65;",
	"&#xFFFFFFFF;", "&#99999999999999999999;", "&nbsp;", "&bogus;", "&", ";", "<hr>", "text ", "\n", "\t", "  ",
	"<a href=\"", "\">", "<img alt=\"", "<td", "=\"", "'", "\"", "<h", "1>", "</", "<x", 0 };

static const char *k_xml_words[] = {
	"<", ">", "</", "<?xml version=\"1.0\"?>", "<D:multistatus xmlns:D=\"DAV:\">", "</D:multistatus>",
	"<D:response>", "</D:response>", "<D:href>", "</D:href>", "<D:propstat>", "<D:prop>", "<D:getetag>", "</D:getetag>",
	"<C:calendar-data>", "</C:calendar-data>", "<![CDATA[", "]]>", "]", "]]", "<!--", "-->", "&lt;", "&#x41;", "&#999999999999;",
	"&amp;", "&bogus", ";", "/>", "<a b=\"c>d\">", "<a b='c>", "\"", "'", "/cal/x.ics", "BEGIN:VCALENDAR", "\r\n", " ", 0 };

static const char *k_ics_words[] = {
	"BEGIN:VCALENDAR\r\n", "END:VCALENDAR\r\n", "BEGIN:VEVENT\r\n", "END:VEVENT\r\n", "BEGIN:VALARM\r\n", "END:VALARM\r\n",
	"BEGIN:VTIMEZONE\r\n", "END:VTIMEZONE\r\n", "UID:abc@x\r\n", "SUMMARY:Caf\xc3\xa9 \\n \\, \\; x\r\n", "LOCATION:Here\r\n",
	"DTSTART:20260930T101500Z\r\n", "DTEND:20260930T111500Z\r\n", "DTSTART;VALUE=DATE:20260930\r\n", "DTSTART;TZID=Europe/London:20260930T101500\r\n",
	"DURATION:-PT15M\r\n", "DURATION:P99999999999W\r\n", "TRIGGER:-PT15M\r\n", "TRIGGER;VALUE=DATE-TIME:20260930T101500Z\r\n",
	"RRULE:FREQ=DAILY\r\n", "RECURRENCE-ID:20260930T101500Z\r\n", "RECURRENCE-ID;TZID=X:20260930T101500\r\n", "STATUS:CANCELLED\r\n",
	"SEQUENCE:5\r\n", "SEQUENCE:99999999999\r\n", "EXDATE:20260930\r\n", "X-LONG:", "\r\n ", "\r\n\t", "\r\n", "\n", ":", ";", "\"", "=",
	"DTSTART:99999999T999999Z\r\n", "DTSTART:0000000\r\n", "END:VEVENT\r\nEND:VEVENT\r\n",
	/* invitations */
	"METHOD:REQUEST\r\n", "METHOD:CANCEL\r\n", "METHOD:REPLY\r\n", "METHOD:", "TZID:Europe/London\r\n", "TZID:X\r\n",
	"BEGIN:STANDARD\r\n", "END:STANDARD\r\n", "BEGIN:DAYLIGHT\r\n", "END:DAYLIGHT\r\n", "TZOFFSETTO:+0100\r\n",
	"TZOFFSETTO:-9999\r\n", "TZOFFSETTO:+\r\n", "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU\r\n", "RRULE:FREQ=YEARLY;BYMONTH=99;BYDAY=-9XX\r\n",
	"RRULE:BYMONTH=10;BYDAY=5SU\r\n", "DTSTART:16010101T020000\r\n", "DTSTART;TZID=X:20261007T100000\r\n",
	"DTSTART;TZID=\"America/New_York\":20261007T1000\r\n", "DTSTART;TZID=GMT Standard Time:20260329T013000\r\n",
	"ORGANIZER;CN=\"A, B\":mailto:a@b\r\n", "ORGANIZER;CN=\"unterminated:mailto:x\r\n", "ATTENDEE;PARTSTAT=ACCEPTED;CN=Me:mailto:me@x\r\n",
	"ATTENDEE:mailto:\r\n", "SEQUENCE:-5\r\n", "DURATION:PT99999999H\r\n", "TRIGGER:-P1W\r\n", "DTSTART:2026\r\n", "DTSTART:20261007T\r\n",
	"DTSTART:20261007T10\r\n", "DTSTART:20261007T1000Z\r\n", "DTSTART:19001231T000000Z\r\n", "DTSTART:20351231T235959Z\r\n", "DTSTART:20371231T235959Z\r\n",
	/* vCards */
	"BEGIN:VCARD\r\n", "END:VCARD\r\n", "VERSION:2.1\r\n", "VERSION:4.0\r\n", "N:Smith;Alice;;Dr;\r\n", "N:;;;;;;;;;\r\n",
	"FN:Alice\r\n", "item1.EMAIL;type=INTERNET:a@b.c\r\n", "EMAIL:noat\r\n", "TEL;CELL:+44 1\r\n", "TEL;VALUE=uri;TYPE=work:tel:+1\r\n",
	"ADR;WORK:;;1 St;Town;;PC;UK\r\n", "ADR:;;;;;;;;;;;\r\n", "ORG:A;B\r\n", "NOTE:x\\ny\\,z\r\n", "PHOTO;ENCODING=b:AAAA\r\n",
	"FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:J=C3=BC=\r\n", "=\r\n", "=C3", ";CHARSET=iso-8859-15:", 0 };

static int build(char *out, int max, const char **words, int nwords)
{
	int n = 0, kind = rnd_n(3);
	int target = kind == 0 ? 100 + rnd_n(400) : kind == 1 ? 1000 + rnd_n(6000) : 8000 + rnd_n(max - 8100);
	if (target > max - 1) target = max - 1;
	while (n < target) {
		int r = rnd_n(10);
		if (r < 6) {
			const char *w = words[rnd_n(nwords)];
			int l = (int)strlen(w);
			if (n + l >= max - 1) break;
			memcpy(out + n, w, l); n += l;
		} else if (r < 8) out[n++] = (char)(32 + rnd_n(95));
		else if (r < 9) out[n++] = (char)rnd_n(256);
		else { int k = rnd_n(40), c = rnd_n(3) ? ')' : '('; while (k-- && n < max - 1) out[n++] = (char)c; }
	}
	out[n] = 0;
	return n;
}

static void mutate(char *b, int n)
{
	int k, i;
	if (n <= 0) return;
	for (k = rnd_n(8); k >= 0; k--) {
		i = rnd_n(n);
		switch (rnd_n(4)) {
		case 0: b[i] = (char)rnd_n(256); break;
		case 1: b[i] ^= (char)(1 << rnd_n(8)); break;
		case 2: b[i] = "(){}\"\\<>&;:=\r\n"[rnd_n(15)]; break;
		default: if (i + 1 < n) memmove(b + i, b + i + 1, n - i - 1), b[n - 1] = ' '; break;
		}
	}
}

static int nwords(const char **w) { int n = 0; while (w[n]) n++; return n; }

/* ---- targets */
static void sink(const char *s, int n, void *ctx) { (void)ctx; if (n > 0) { volatile char c = s[n - 1]; (void)c; } }

static void fuzz_imap(const char *buf, int n)
{
	ImapNode *t = ip_parse(buf, n), *c;
	PmStructure st;
	char tmp[64];
	int i;
	if (!t) return;
	mime_structure(t, &st);
	for (i = 0; i < 6; i++) {
		c = ip_nth(t, i);
		ip_str(c, tmp, sizeof(tmp));
		ip_num(c);
		if (c && c->type == IT_LIST) mime_structure(c, &st);
	}
	c = ip_get(t, "BODYSTRUCTURE");
	if (c) mime_structure(c, &st);
	c = ip_get(t, "ENVELOPE");
	if (c) { ip_str(ip_nth(c, 1), tmp, sizeof(tmp)); cs_decode_header(tmp, tmp, sizeof(tmp)); }
	ip_eq(t, "x");
}

static void fuzz_dec(const char *buf, int n)
{
	PmDecoder d;
	static char out[70000];
	int enc, pos;
	for (enc = 0; enc <= 2; enc++) {
		dec_init(&d, enc);
		for (pos = 0; pos < n;) {
			int k = 1 + rnd_n(700);
			if (k > n - pos) k = n - pos;
			dec_feed(&d, buf + pos, k, out);
			pos += k;
		}
	}
}

static void fuzz_html(const char *buf, int n)
{
	HtmlConv *h = html_new();
	int pos;
	if (!h) return;
	for (pos = 0; pos < n;) {
		int k = 1 + rnd_n(900);
		if (k > n - pos) k = n - pos;
		html_feed(h, buf + pos, k, sink, 0);
		pos += k;
	}
	html_end(h, sink, 0);
	html_free(h);
}

static void xs_end(XmlScan *x, const char *name, const char *text, int len, void *ctx)
{
	(void)ctx; (void)len; (void)text;
	xs_parent(x, 1); xs_parent(x, 2); xs_parent(x, 50);
	(void)name;
}

static void fuzz_xml(const char *buf, int n)
{
	XmlScan x;
	static char text[3000];
	int pos;
	xs_init(&x, text, rnd_n(2) ? sizeof(text) : 20, xs_end, 0);
	for (pos = 0; pos < n;) {
		int k = 1 + rnd_n(500);
		if (k > n - pos) k = n - pos;
		xs_feed(&x, buf + pos, k);
		pos += k;
	}
}

static void ics_cb(const IcsEvent *e, void *ctx) { (void)ctx; (void)e->start; }

static void fuzz_ics(const char *buf, int n)
{
	IcsChange c;
	static char work[70000];
	int max;
	ics_each(buf, n, ics_cb, 0);
	memset(&c, 0, sizeof(c));
	c.start = 1790000000L; c.end = c.start + 3600; c.alarm = rnd_n(3) - 1; c.allday = rnd_n(2);
	strcpy(c.summary, "Fuzzed \\ ; , summary\nline");
	strcpy(c.location, rnd_n(2) ? "Somewhere" : "");
	max = (int)sizeof(work) - 1;
	memcpy(work, buf, n); work[n] = 0;
	ics_change(work, n, max, "", &c, rnd_n(3));
	memcpy(work, buf, n); work[n] = 0;
	ics_change(work, n, max, "20260930T101500Z", &c, 0);
	memcpy(work, buf, n); work[n] = 0;
	ics_change(work, n, max, "20260930", &c, 0);
	memcpy(work, buf, n); work[n] = 0;
	ics_exclude(work, n, max, "20260930T101500Z", 0);
	memcpy(work, buf, n); work[n] = 0;
	ics_exclude(work, n, n + 40, "20260930", 0);          /* barely any room */
	ics_new(&c, 0, "uid@x", work, 300);
	ics_new(&c, 0, "uid@x", work, max);
}

static void fuzz_invite(const char *buf, int n)
{
	static PmInvite iv;
	static PmCard cards[8];
	static ItipReply r;
	static char out[4096];
	char t[24];
	if (inv_parse(buf, n, rnd_n(22) - 1, rnd_n(2) ? "me@x" : "", &iv) == 0) {
		/* the reply the app would ask for, from what was found */
		memset(&r, 0, sizeof(r));
		itip_header(&r, "Itip", rnd_n(4) ? "ACCEPTED" : "maybe");
		itip_header(&r, "Itip-Uid", iv.uid);
		snprintf(t, sizeof(t), "%d", iv.seq);
		itip_header(&r, "Itip-Seq", t);
		itip_header(&r, "Itip-Recur", iv.recur_line);
		itip_header(&r, "Itip-Organizer", iv.org_line);
		itip_header(&r, "Itip-Attendee", iv.me[0] ? iv.me : "me@x");
		itip_header(&r, "Itip-Name", iv.org_name);
		itip_header(&r, "Itip-Summary", iv.summary);
		cal_fmt_utc(iv.ustart, t);
		itip_header(&r, "Itip-Start", t);
		cal_fmt_utc(iv.uend, t);
		itip_header(&r, "Itip-End", t);
		itip_build(&r, 1790000000L, out, rnd_n(3) ? (int)sizeof(out) : 200);
	}
	vcf_parse(buf, n, cards, 1 + rnd_n(8));
}

static void fuzz_charset(const char *buf, int n)
{
	static char out[4000], in[70000];
	const char *cs[] = { "utf-8", "iso-8859-1", "windows-1252", "iso-8859-15", "us-ascii", "koi8-r", "gb2312", "" };
	memcpy(in, buf, n); in[n] = 0;
	cs_decode_header(in, out, sizeof(out));
	cs_decode_header(in, out, 5);
	cs_to_cp1252(cs[rnd_n(8)], buf, n, out, sizeof(out));
	cs_to_cp1252("utf-8", buf, n, out, 3);
	cs_utf8_to_cp1252(buf, n, out, sizeof(out));
	cs_cp1252_to_utf8(buf, n, out, sizeof(out));
	cs_mutf7_decode(in, out, sizeof(out));
	cs_mutf7_encode(in, out, sizeof(out));
	cs_encode_header(in, out, sizeof(out));
	cs_encode_header(in, out, 8);
}

int main(int argc, char **argv)
{
	static char buf[65536];
	unsigned long seed = argc > 1 ? strtoul(argv[1], 0, 10) : 1;
	int rounds = argc > 2 ? atoi(argv[2]) : 2000, r;
	for (r = 0; r < rounds; r++) {
		int which, n;
		clock_t t0 = clock();
		g_seed = seed * 7919UL + (unsigned long)r * 104729UL;
		which = rnd_n(7);
		switch (which) {
		case 0: n = build(buf, sizeof(buf), k_imap_words, nwords(k_imap_words)); mutate(buf, n); fuzz_imap(buf, n); break;
		case 1: n = build(buf, sizeof(buf), k_imap_words, nwords(k_imap_words)); mutate(buf, n); fuzz_dec(buf, n); break;
		case 2: n = build(buf, sizeof(buf), k_html_words, nwords(k_html_words)); mutate(buf, n); fuzz_html(buf, n); break;
		case 3: n = build(buf, sizeof(buf), k_xml_words, nwords(k_xml_words)); mutate(buf, n); fuzz_xml(buf, n); break;
		case 4: n = build(buf, sizeof(buf), k_ics_words, nwords(k_ics_words)); mutate(buf, n); fuzz_ics(buf, n); break;
		case 5: n = build(buf, sizeof(buf), k_ics_words, nwords(k_ics_words)); mutate(buf, n); fuzz_invite(buf, n); break;
		default: n = build(buf, sizeof(buf), k_imap_words, nwords(k_imap_words)); mutate(buf, n); fuzz_charset(buf, n); break;
		}
		if (clock() - t0 > CLOCKS_PER_SEC) fprintf(stderr, "slow: seed %lu round %d target %d (%d bytes): %ld ms\n", seed, r, which, n, (long)((clock() - t0) * 1000 / CLOCKS_PER_SEC));
		
	}
	fprintf(stderr, "parsefuzz: %d rounds, seed %lu: no crash\n", rounds, seed);
	return 0;
}
