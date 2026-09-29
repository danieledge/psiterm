/* uishot.cpp - PsiMail's screens drawn on a PC, from a real store (made by
 * psimail-host or the emulator), to see them without a Psion:
 *
 *   uishot STOREDIR mailbox FOLDER OUT.pgm [SEL] [sidebar]
 *   uishot STOREDIR reader FOLDER UID OUT.pgm [SCROLL] [FOCUSLINK]
 *   uishot STOREDIR calendar OUT.pgm [YYYYMMDD] [EVENT] [sidebar]   (TODAY=, NOW= minutes)
 *   uishot STOREDIR event OUT.pgm YYYYMMDD EVENT
 *   uishot - welcome OUT.pgm
 *
 * Writes a 640x240 16-grey PGM (mail/ui/shot.py turns it into a PNG). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pmui.h"

static unsigned char g_bits[320 * 240];
void* ui_alloc(int n) { return malloc(n); }
void ui_free(void* p) { free(p); }

static char* slurp(const char* path, int* len)
	{
	FILE* f = fopen(path, "rb");
	if (!f) { *len = 0; return 0; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* b = (char*)malloc(n + 1);
	*len = (int)fread(b, 1, n, f);
	b[*len] = 0;
	fclose(f);
	return b;
	}

static void save(const char* out)
	{
	FILE* f = fopen(out, "wb");
	fprintf(f, "P5 640 240 255\n");
	for (int y = 0; y < 240; y++)
		for (int x = 0; x < 640; x++)
			{
			unsigned char v = g_bits[y * 320 + x / 2];
			v = (x & 1) ? (v >> 4) : (v & 15);
			fputc(v * 17, f);
			}
	fclose(f);
	}

static unsigned long fnv(const char* s)
	{
	unsigned long h = 2166136261UL;
	while (*s) { h ^= (unsigned char)*s++; h = (h * 16777619UL) & 0xffffffffUL; }
	return h;
	}

static char* field(char** p)
	{
	char* s = *p;
	if (!s) return (char*)"";
	char* t = strchr(s, '\t');
	if (t) { *t = 0; *p = t + 1; } else *p = 0;
	return s;
	}

static void display_name(const char* from, char* out)
	{
	const char* lt = strchr(from, '<');
	if (lt && lt > from)
		{
		int n = (int)(lt - from);
		while (n > 0 && from[n - 1] == ' ') n--;
		int b = 0;
		if (from[0] == '"') { b = 1; if (n > 1 && from[n - 1] == '"') n--; }
		memcpy(out, from + b, n - b);
		out[n - b] = 0;
		}
	else strcpy(out, from);
	}

static void fmt_date(long t, char* out)
	{
	time_t tt = t;
	struct tm* tm = localtime(&tt);
	time_t now = time(0);
	struct tm n = *localtime(&now);
	static const char* mon[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
	if (tm->tm_year == n.tm_year && tm->tm_yday == n.tm_yday) sprintf(out, "%02d:%02d", tm->tm_hour, tm->tm_min);
	else sprintf(out, "%d %s", tm->tm_mday, mon[tm->tm_mon]);
	}

static void load_folders(const char* store, const char* folder, PmUiFolder* folders, int* pnf, int* pfsel)
	{
	char path[400];
	int nf = 0, fsel = 0;
	sprintf(path, "%s/A0/folders.txt", store);
	int len;
	char* ft = slurp(path, &len);
	char* line = ft ? strtok(ft, "\n") : 0;
	while (line && nf < 37)
		{
		if (line[0] != '#')
			{
			char* p = line;
			char* kind = field(&p); char* un = field(&p); field(&p); char* imap = field(&p); char* disp = field(&p);
			folders[nf].kind = kind[0];
			folders[nf].unread = atoi(un);
			folders[nf].name = disp; folders[nf].len = strlen(disp);
			folders[nf].depth = 0;
			if (!strcmp(imap, folder)) fsel = nf;
			nf++;
			}
		line = strtok(0, "\n");
		}
	folders[nf].kind = 'O'; folders[nf].name = "Outbox"; folders[nf].len = 6; folders[nf].unread = 2; folders[nf].depth = 0; nf++;
	folders[nf].kind = 'C'; folders[nf].name = "Calendar"; folders[nf].len = 8; folders[nf].unread = 0; folders[nf].depth = 0; nf++;
	*pnf = nf;
	*pfsel = fsel;
	}

static long parse_day(const char* s)
	{
	int y, m, d;
	if (!s || sscanf(s, "%4d%2d%2d", &y, &m, &d) != 3) return -1;
	return cal_days_from(y, m, d);
	}

int main(int argc, char** argv)
	{
	PmCanvas c;
	gfx_init(&c, g_bits, 640, 240, 320);
	if (getenv("MONO")) c.mono = 1;
	if (!strcmp(argv[2], "welcome"))
		{
		const char* a = "Set up your mail with Tools > New account (Ctrl+K).";
		const char* b = "For Fastmail, make an app password at Settings > Privacy & Security.";
		ui_welcome(&c, a, strlen(a), b, strlen(b));
		save(argv[3]);
		return 0;
		}
	char path[400];
	const char* store = argv[1];
	const char* folder = argv[3];
	if (!strcmp(argv[2], "mailbox"))
		{
		PmUiFolder folders[40];
		int nf = 0, fsel = 0;
		load_folders(store, folder, folders, &nf, &fsel);
		/* messages */
		int len;
		char* line;
		sprintf(path, "%s/A0/F%08lX/index.txt", store, fnv(folder));
		char* it = slurp(path, &len);
		static PmUiRow rows[600];
		static char from[600][80], date[600][16];
		int nr = 0, unread = 0;
		char* lines[600];
		int nl = 0;
		line = it ? strtok(it, "\n") : 0;
		while (line && nl < 600) { if (line[0] != '#') lines[nl++] = line; line = strtok(0, "\n"); }
		for (int i = nl - 1; i >= 0; i--)
			{
			char* p = lines[i];
			field(&p); char* flags = field(&p); char* d = field(&p); field(&p);
			char* fr = field(&p); char* subj = field(&p);
			PmUiRow* r = &rows[nr];
			display_name(fr, from[nr]);
			fmt_date(atol(d), date[nr]);
			r->from = from[nr]; r->flen = strlen(from[nr]);
			r->subj = subj; r->slen = strlen(subj);
			r->date = date[nr]; r->dlen = strlen(date[nr]);
			r->flags = 0;
			if (!strchr(flags, 'S')) { r->flags |= KRowUnread; unread++; }
			if (strchr(flags, 'F')) r->flags |= KRowFlagged;
			if (strchr(flags, 'T')) r->flags |= KRowAttach;
			if (strchr(flags, 'A')) r->flags |= KRowAnswered;
			nr++;
			}
		int sel = argc > 5 ? atoi(argv[5]) : 0;
		int rowsfit = ui_mailbox_rows(240);
		int top = sel - rowsfit + 1 > 0 ? sel - rowsfit + 1 : 0;
		PmUiMailbox m;
		memset(&m, 0, sizeof(m));
		m.account = "Fastmail"; m.alen = 8;
		m.folders = folders; m.nfolders = nf; m.folderSel = fsel; m.sidebarFocus = argc > 6;
		m.title = folders[fsel].name; m.tlen = folders[fsel].len;
		char sub[40];
		sprintf(sub, "%d unread", unread);
		m.subtitle = sub; m.sublen = strlen(sub);
		m.rows = rows + top; m.nrows = nr - top < rowsfit + 1 ? nr - top : rowsfit + 1;
		m.total = nr; m.top = top; m.sel = sel;
		m.empty = "No messages here"; m.elen = 16;
		m.online = 1;
		if (getenv("STATUS")) { m.status = getenv("STATUS"); m.statlen = strlen(m.status); m.busy = 1; }
		ui_mailbox(&c, &m);
		if (getenv("TOAST")) ui_toast(&c, getenv("TOAST"), strlen(getenv("TOAST")));
		save(argv[argc > 6 ? 4 : 4]);
		return 0;
		}
	if (!strcmp(argv[2], "calendar") || !strcmp(argv[2], "event"))
		{
		/* uishot STORE calendar OUT.pgm [DAY] [EVENT] [side]; TODAY=YYYYMMDD NOW=minutes */
		static PmUiFolder folders[40];
		int nf = 0, fsel = 0;
		load_folders(store, "", folders, &nf, &fsel);
		fsel = nf - 1;
		int len1, len2, len3;
		sprintf(path, "%s/cal/events.txt", store);
		char* ev = slurp(path, &len1);
		sprintf(path, "%s/cal/calendars.txt", store);
		char* cals = slurp(path, &len2);
		sprintf(path, "%s/cal/push.txt", store);
		char* push = slurp(path, &len3);
		PmCalModel m;
		calm_init(&m);
		calm_load(&m, ev, len1, cals, len2, push, len3);
		time_t now = time(0);
		struct tm* lt = localtime(&now);
		long today = getenv("TODAY") ? parse_day(getenv("TODAY")) : cal_days_from(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
		int nowmin = getenv("NOW") ? atoi(getenv("NOW")) : lt->tm_hour * 60 + lt->tm_min;
		long sel = argc > 4 ? parse_day(argv[4]) : today;
		if (sel < 0) sel = today;
		static PmUiEvent evs[64];
		PmCalText t;
		PmUiCalendar k;
		memset(&k, 0, sizeof(k));
		calm_view(&m, today, nowmin, sel, &k, evs, 64, &t);
		PmUiMailbox side;
		memset(&side, 0, sizeof(side));
		side.account = "Fastmail"; side.alen = 8;
		side.folders = folders; side.nfolders = nf; side.folderSel = fsel; side.online = 1;
		side.sidebarFocus = argc > 6;
		k.side = &side;
		k.sel = argc > 5 ? atoi(argv[5]) : (k.nevents ? 0 : -1);
		k.focus = !side.sidebarFocus;
		k.enabled = 1;
		if (getenv("STATUS")) { k.status = getenv("STATUS"); k.statlen = strlen(k.status); k.busy = 1; }
		if (!strcmp(argv[2], "event") && k.sel >= 0 && k.sel < k.nevents)
			{
			PmUiEventView v;
			PmCalText t2;
			calm_event_view(&evs[k.sel], sel, &v, &t2);
			ui_event(&c, &v);
			}
		else
			ui_calendar(&c, &k);
		if (getenv("TOAST")) ui_toast(&c, getenv("TOAST"), strlen(getenv("TOAST")));
		save(argv[3]);
		fprintf(stderr, "%d events that day, %d in all\n", k.nevents, m.n);
		return 0;
		}
	if (!strcmp(argv[2], "reader"))
		{
		int uid = atoi(argv[4]);
		sprintf(path, "%s/A0/F%08lX/%d.txt", store, fnv(folder), uid);
		int len;
		char* t = slurp(path, &len);
		int trunc = 0;
		char* body = t;
		if (t && t[0] == '#')
			{
			char* nl = strchr(t, '\n');
			char* p = strchr(t, '\t');
			trunc = p ? atoi(p + 1) : 0;
			body = nl + 1;
			len -= (int)(body - t);
			}
		/* attachments */
		PmUiAttachment att[8];
		static char an[8][100], as[8][20];
		int na = 0;
		sprintf(path, "%s/A0/F%08lX/%d.att", store, fnv(folder), uid);
		int al;
		char* at = slurp(path, &al);
		char* line = at ? strtok(at, "\n") : 0;
		while (line && na < 8)
			{
			char* p = line;
			field(&p); int size = atoi(field(&p)); char* name = field(&p);
			strcpy(an[na], name);
			sprintf(as[na], "%d KB", (size * 3 / 4 + 1023) / 1024);
			att[na].name = an[na]; att[na].len = strlen(an[na]);
			att[na].size = as[na]; att[na].slen = strlen(as[na]);
			na++;
			line = strtok(0, "\n");
			}
		PmDoc d;
		doc_build(&d, body, len, 640, att, na, (trunc + 1023) / 1024);
		PmUiReader r;
		memset(&r, 0, sizeof(r));
		r.text = body; r.len = len; r.doc = &d;
		r.scroll = argc > 6 ? atoi(argv[6]) : 0;
		r.position = 3; r.count = 67;
		r.folder = "Inbox"; r.flen = 5;
		r.focusLink = argc > 7 ? atoi(argv[7]) : 0;
		r.att = att; r.natt = na;
		r.html = 1;
		if (getenv("LOADING")) { r.loading = 1; r.subject = "Café plans"; r.sublen = 10; }
		ui_reader(&c, &r);
		save(argv[5]);
		fprintf(stderr, "doc: %d ops, height %d, %d links\n", d.nops, d.height, d.nlinks);
		return 0;
		}
	return 1;
	}
