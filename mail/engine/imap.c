/* imap.c - IMAP4rev1 client (RFC 3501) for PsiMail
 *
 * Kept small for a 36 MHz Psion on a serial line of 11 KB/s at best:
 *  - message lists come from one FETCH of ENVELOPE, FLAGS, size, date and
 *    BODYSTRUCTURE; nothing else is downloaded until a message is opened;
 *  - opening a message downloads just its text part (up to max_body_kb),
 *    decoded, converted to Windows-1252 and HTML flattened on the way to
 *    the file, so no message is ever held whole in memory;
 *  - attachments are only fetched when asked for, streamed to a file.
 * Uses MOVE, UIDPLUS, LIST-STATUS, SPECIAL-USE and LITERAL+ when the server
 * has them (Fastmail has all of them).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "pm.h"

#define RESP_MAX   (48 * 1024)
#define STREAM_MIN 1024        /* literals bigger than this go to the stream callback */
#define TIMEOUT    60000

static int  g_acct = -1;       /* logged-in account, or -1 */
static int  g_conn = -1;       /* pmn_conn_id() of our connection */
static char g_caps[600];
static char g_sel[128];        /* selected folder, "" = none */
static int  g_tagno;
static char g_resp[RESP_MAX];
static int  g_rlen;
static unsigned long g_last_cmd;

/* a literal to stream instead of keeping (message text, attachments) */
typedef void (*StreamFn)(const char *data, int n, void *ctx);
static StreamFn g_stream;
static void *g_stream_ctx;
static long g_stream_total;

typedef void (*UntaggedFn)(ImapNode *r, void *ctx);

static int has_cap(const char *c)
{
	const char *p = g_caps;
	int n = (int)strlen(c);
	while ((p = pm_stristr(p, c)) != 0) {
		if ((p == g_caps || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0 || p[n] == ']')) return 1;
		p += n;
	}
	return 0;
}

static void set_why(char *why, int whymax, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(why, whymax, fmt, ap);
	va_end(ap);
}

static int lost(char *why, int whymax, int r)
{
	if (r == PMN_CANCEL) { set_why(why, whymax, "Stopped"); pmn_close(1); g_acct = -1; return PM_RES_CANCELLED; }
	if (r == PMN_TIMEOUT) set_why(why, whymax, "The server stopped answering");
	else if (strstr(pmn_error(), "damaged"))
		set_why(why, whymax, "The line garbled the data (RTS/CTS flow control would stop that)");
	else set_why(why, whymax, "The connection was lost");
	pm_log("imap: %s (%d) %s", why, r, pmn_error());
	pmn_close(1);
	g_acct = -1;
	g_sel[0] = 0;
	return PM_RES_OFFLINE;
}

/* Reads one whole response (a line plus any literals and continuation
   lines) into g_resp. Returns the length, or <0. */
static int read_response(void)
{
	int r;
	g_rlen = 0;
	for (;;) {
		int start = g_rlen, len;
		r = pmn_readline(g_resp + g_rlen, RESP_MAX - g_rlen - 16, TIMEOUT);
		if (r < 0) return r;
		len = r;
		g_rlen += len;
		/* literal? "... {123}" or "{123+}" */
		if (len > 2 && g_resp[g_rlen - 1] == '}') {
			char *o = g_resp + g_rlen - 2;
			long n;
			while (o > g_resp + start && *o != '{') o--;
			if (*o != '{') { g_resp[g_rlen++] = '\r'; g_resp[g_rlen++] = '\n'; break; }
			n = strtol(o + 1, 0, 10);
			if (n < 0) n = 0;
			if ((g_stream && n > 0) || g_rlen + n + 8 > RESP_MAX) {
				/* stream it (or throw it away if too big to keep) */
				static char chunk[1024];
				long left = n;
				StreamFn fn = g_rlen + n + 8 > RESP_MAX && !g_stream ? 0 : g_stream;
				g_rlen = (int)(o - g_resp);
				memcpy(g_resp + g_rlen, "{0}\r\n", 5);
				g_rlen += 5;
				g_stream_total = n;
				while (left > 0) {
					int k = pmn_read(chunk, left > (long)sizeof(chunk) ? (int)sizeof(chunk) : (int)left, TIMEOUT);
					if (k <= 0) return k == 0 ? -1 : k;
					if (fn) fn(chunk, k, g_stream_ctx);
					left -= k;
				}
			} else {
				long left = n;
				g_resp[g_rlen++] = '\r'; g_resp[g_rlen++] = '\n';
				while (left > 0) {
					int k = pmn_read(g_resp + g_rlen, (int)left, TIMEOUT);
					if (k <= 0) return k == 0 ? -1 : k;
					g_rlen += k;
					left -= k;
				}
			}
			continue;       /* the response goes on after the literal */
		}
		g_resp[g_rlen++] = '\r'; g_resp[g_rlen++] = '\n';
		break;
	}
	g_resp[g_rlen] = 0;
	return g_rlen;
}

static void note_caps(const char *text)
{
	const char *p = pm_stristr(text, "[CAPABILITY ");
	if (p) {
		const char *e = strchr(p, ']');
		int n = e ? (int)(e - p - 12) : (int)strlen(p + 12);
		if (n > (int)sizeof(g_caps) - 1) n = sizeof(g_caps) - 1;
		memcpy(g_caps, p + 12, n);
		g_caps[n] = 0;
	}
}

/* Sends a command and reads until its tagged reply. Untagged responses go
   to fn. Returns PM_RES_OK, PM_RES_FAILED (why = the server's words),
   PM_RES_OFFLINE or PM_RES_CANCELLED. */
static int vcmd(UntaggedFn fn, void *ctx, char *why, int whymax, const char *fmt, va_list ap)
{
	static char line[1100];
	char tag[12];
	int r, n;
	if (!pmn_is_open()) { set_why(why, whymax, "Not connected"); return PM_RES_OFFLINE; }
	sprintf(tag, "P%03d", ++g_tagno % 1000);
	n = sprintf(line, "%s ", tag);
	n += vsnprintf(line + n, sizeof(line) - n - 3, fmt, ap);
	if (n > (int)sizeof(line) - 3) n = sizeof(line) - 3;
	line[n++] = '\r'; line[n++] = '\n';
	g_last_cmd = pm_ms();
	if (pmn_write(line, n) != 0) return lost(why, whymax, -1);
	for (;;) {
		r = read_response();
		if (r < 0) return lost(why, whymax, r);
		if (!strncmp(g_resp, tag, strlen(tag)) && g_resp[strlen(tag)] == ' ') {
			char *s = g_resp + strlen(tag) + 1;
			note_caps(s);
			s[strcspn(s, "\r\n")] = 0;
			if (!pm_strncasecmp(s, "OK", 2)) return PM_RES_OK;
			/* "NO [ALERT] text" -> text */
			s += 2;
			while (*s == ' ') s++;
			if (*s == '[') { char *e = strchr(s, ']'); if (e) s = e + 1; while (*s == ' ') s++; }
			set_why(why, whymax, "%s", *s ? s : "The server said no");
			return PM_RES_FAILED;
		}
		if (g_resp[0] == '*') {
			ImapNode *t;
			note_caps(g_resp);
			if (!pm_strncasecmp(g_resp, "* BYE", 5)) {
				/* the server is closing (maybe after LOGOUT) */
				continue;
			}
			if (!pm_strncasecmp(g_resp, "* CAPABILITY ", 13)) {
				int k = (int)strcspn(g_resp + 13, "\r\n");
				if (k > (int)sizeof(g_caps) - 1) k = sizeof(g_caps) - 1;
				memcpy(g_caps, g_resp + 13, k); g_caps[k] = 0;
			}
			if (fn) {
				t = ip_parse(g_resp + 2, g_rlen - 2);
				if (t) fn(t, ctx);
			}
		}
		/* "+ ..." continuation requests are handled by the caller */
	}
}

static int cmd(UntaggedFn fn, void *ctx, char *why, int whymax, const char *fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vcmd(fn, ctx, why, whymax, fmt, ap);
	va_end(ap);
	return r;
}

/* "string" with IMAP quoting, or a literal+ for 8-bit text */
static void quote(const char *s, char *out, int max)
{
	int k = 0;
	out[k++] = '"';
	for (; *s && k < max - 3; s++) {
		if (*s == '"' || *s == '\\') out[k++] = '\\';
		out[k++] = *s;
	}
	out[k++] = '"';
	out[k] = 0;
}

/* ---------------------------------------------------------------- login */

void imap_logout(void)
{
	char why[40];
	if (g_acct >= 0 && pmn_is_open() && pmn_conn_id() == g_conn) cmd(0, 0, why, sizeof(why), "LOGOUT");
	g_acct = -1;
	g_sel[0] = 0;
}

int imap_open(int acct, char *why, int whymax)
{
	PmShared *s = pm_shared();
	PmAccount *a = &s->acct[acct];
	char qu[200], qp[140];
	int r;

	if (g_acct >= 0 && pmn_conn_id() != g_conn) { g_acct = -1; g_sel[0] = 0; }   /* SMTP took the line */
	if (g_acct == acct && pmn_is_open()) {
		/* still there? (a modem link or the server may have dropped it) */
		if (pm_ms() - g_last_cmd < 30000) return PM_RES_OK;
		if (cmd(0, 0, why, whymax, "NOOP") == PM_RES_OK) return PM_RES_OK;
	}
	if (s->offline) { set_why(why, whymax, "Working offline"); return PM_RES_OFFLINE; }
	if (!a->pass[0]) { set_why(why, whymax, "Password needed for %s", a->name); return PM_RES_NEED_PASS; }
	if (g_acct >= 0 && pmn_is_open()) imap_logout();
	g_acct = -1;
	g_sel[0] = 0;
	g_caps[0] = 0;

	if (pmn_connect(a->imap_host, a->imap_port, a->imap_tls == PM_TLS_ON, why, whymax) != 0) {
		if (tlsv_problem()[0]) {
			snprintf(s->trust_host, sizeof(s->trust_host), "%s:%d", a->imap_host, a->imap_port);
			pm_copy(s->trust_why, tlsv_problem(), sizeof(s->trust_why));
			pm_copy(s->trust_fp, tlsv_fingerprint(), sizeof(s->trust_fp));
			return PM_RES_UNTRUSTED;
		}
		return pm_cancelled() ? PM_RES_CANCELLED : PM_RES_OFFLINE;
	}
	pm_progress("Logging in to %s...", a->imap_host);
	r = read_response();
	if (r < 0) return lost(why, whymax, r);
	if (pm_strncasecmp(g_resp, "* OK", 4) && pm_strncasecmp(g_resp, "* PREAUTH", 9)) {
		g_resp[strcspn(g_resp, "\r\n")] = 0;
		set_why(why, whymax, "Not an IMAP server? %.60s", g_resp);
		pmn_close(1);
		return PM_RES_FAILED;
	}
	note_caps(g_resp);
	if (a->imap_tls == PM_TLS_STARTTLS) {
		if (cmd(0, 0, why, whymax, "STARTTLS") != PM_RES_OK) { pmn_close(1); return PM_RES_FAILED; }
		if (pmn_starttls(a->imap_host, why, whymax) != 0) {
			pmn_close(1);
			if (tlsv_problem()[0]) {
				snprintf(s->trust_host, sizeof(s->trust_host), "%s:%d", a->imap_host, a->imap_port);
				pm_copy(s->trust_why, tlsv_problem(), sizeof(s->trust_why));
				pm_copy(s->trust_fp, tlsv_fingerprint(), sizeof(s->trust_fp));
				return PM_RES_UNTRUSTED;
			}
			return PM_RES_OFFLINE;
		}
		g_caps[0] = 0;
	}
	quote(a->user, qu, sizeof(qu));
	quote(a->pass, qp, sizeof(qp));
	r = cmd(0, 0, why, whymax, "LOGIN %s %s", qu, qp);
	memset(qp, 0, sizeof(qp));
	if (r == PM_RES_FAILED) { pmn_close(1); return PM_RES_LOGIN_FAILED; }
	if (r != PM_RES_OK) return r;
	/* the server's abilities after login (LOGIN's reply usually has them) */
	if (!has_cap("IMAP4rev1") && (r = cmd(0, 0, why, whymax, "CAPABILITY")) != PM_RES_OK) return r;
	g_acct = acct;
	g_conn = pmn_conn_id();
	pm_log("imap: logged in to %s; caps %s", a->imap_host, g_caps);
	return PM_RES_OK;
}

static int select_folder(const char *folder, UntaggedFn fn, void *ctx, char *why, int whymax)
{
	char q[160];
	int r;
	if (fn == 0 && !strcmp(g_sel, folder)) return PM_RES_OK;
	quote(folder, q, sizeof(q));
	r = cmd(fn, ctx, why, whymax, "SELECT %s", q);
	if (r == PM_RES_OK) pm_copy(g_sel, folder, sizeof(g_sel));
	else g_sel[0] = 0;
	return r;
}

/* -------------------------------------------------------------- folders */

typedef struct
	{
	int n;
	struct { char name[128]; char kind; char delim; long total, unseen; int have_status; } f[80];
	} FolderList;

static void on_list(ImapNode *r, void *ctx)
{
	FolderList *fl = (FolderList *)ctx;
	ImapNode *w = r->child;
	if (!w) return;
	if (ip_eq(w, "LIST")) {
		ImapNode *attrs = w->next, *delim = attrs ? attrs->next : 0, *name = delim ? delim->next : 0, *a;
		char kind = '-', d[4];
		if (!name || fl->n >= 80) return;
		for (a = attrs->child; a; a = a->next) {
			if (ip_eq(a, "\\Noselect") || ip_eq(a, "\\NonExistent")) kind = 'N';
			else if (ip_eq(a, "\\Sent")) kind = 'S';
			else if (ip_eq(a, "\\Drafts")) kind = 'D';
			else if (ip_eq(a, "\\Trash")) kind = 'T';
			else if (ip_eq(a, "\\Junk")) kind = 'J';
			else if (ip_eq(a, "\\Archive")) kind = 'A';
		}
		ip_str(delim, d, sizeof(d));
		ip_str(name, fl->f[fl->n].name, sizeof(fl->f[0].name));
		if (!pm_strcasecmp(fl->f[fl->n].name, "INBOX")) { strcpy(fl->f[fl->n].name, "INBOX"); kind = 'I'; }
		fl->f[fl->n].kind = kind;
		fl->f[fl->n].delim = d[0] ? d[0] : '/';
		fl->f[fl->n].total = fl->f[fl->n].unseen = -1;
		fl->n++;
	} else if (ip_eq(w, "STATUS")) {
		char name[128];
		ImapNode *items;
		int i;
		ip_str(w->next, name, sizeof(name));
		items = w->next ? w->next->next : 0;
		for (i = 0; i < fl->n; i++)
			if (!strcmp(fl->f[i].name, name) || (!pm_strcasecmp(name, "INBOX") && fl->f[i].kind == 'I')) {
				ImapNode *v;
				if ((v = ip_get(items, "MESSAGES")) != 0) fl->f[i].total = ip_num(v);
				if ((v = ip_get(items, "UNSEEN")) != 0) fl->f[i].unseen = ip_num(v);
				fl->f[i].have_status = 1;
			}
	}
}

int imap_list_folders(int acct, char *why, int whymax)
{
	static FolderList fl;
	static const char order[] = "IDSAJT-N";
	char dir[160], path[190], tmp[190], disp[128];
	FILE *f;
	int r, i, o;

	if ((r = imap_open(acct, why, whymax)) != PM_RES_OK) return r;
	pm_progress("Getting the folder list...");
	memset(&fl, 0, sizeof(fl));
	if (has_cap("LIST-STATUS") && has_cap("SPECIAL-USE"))
		r = cmd(on_list, &fl, why, whymax, "LIST \"\" \"*\" RETURN (SPECIAL-USE STATUS (MESSAGES UNSEEN))");
	else
		r = cmd(on_list, &fl, why, whymax, "LIST \"\" \"*\"");
	if (r != PM_RES_OK) return r;
	for (i = 0; i < fl.n && i < 30; i++) {
		char q[160];
		if (fl.f[i].have_status || fl.f[i].kind == 'N') continue;
		pm_progress("Counting messages in folder %d of %d...", i + 1, fl.n);
		quote(fl.f[i].name, q, sizeof(q));
		r = cmd(on_list, &fl, why, whymax, "STATUS %s (MESSAGES UNSEEN)", q);
		if (r == PM_RES_OFFLINE || r == PM_RES_CANCELLED) return r;
	}
	st_acct_dir(acct, dir, sizeof(dir));
	snprintf(path, sizeof(path), "%sfolders.txt", dir);
	snprintf(tmp, sizeof(tmp), "%sfolders.new", dir);
	if (!(f = fopen(tmp, "w"))) { pm_write_why(why, whymax, "the folder list", path); return PM_RES_FAILED; }
	fprintf(f, "#PSIMAIL1\n");
	/* INBOX and the special folders first, then the rest as the server lists them */
	for (o = 0; order[o]; o++)
		for (i = 0; i < fl.n; i++) {
			if (fl.f[i].kind != order[o]) continue;
			cs_mutf7_decode(fl.f[i].name, disp, sizeof(disp));
			if (fl.f[i].kind == 'I') strcpy(disp, "Inbox");
			fprintf(f, "%c\t%ld\t%ld\t%s\t%s\t%c\n", fl.f[i].kind, fl.f[i].unseen, fl.f[i].total,
				fl.f[i].name, disp, fl.f[i].delim);
		}
	if (pm_fclose(f) != 0 || pm_replace(tmp, path) != 0) {
		remove(tmp);                          /* the old list stays */
		pm_write_why(why, whymax, "the folder list", path);
		return PM_RES_FAILED;
	}
	st_changed();
	return PM_RES_OK;
}

int imap_special_folder(int acct, char kind, char *out, int max)
{
	char dir[160], path[190];
	static char line[400];
	FILE *f;
	out[0] = 0;
	st_acct_dir(acct, dir, sizeof(dir));
	snprintf(path, sizeof(path), "%sfolders.txt", dir);
	if ((f = fopen(path, "r")) != 0) {
		while (fgets(line, sizeof(line), f)) {
			char *p = line, *t;
			if (line[0] != kind || line[1] != '\t') continue;
			line[strcspn(line, "\r\n")] = 0;
			/* kind unseen total NAME display delim */
			(void)p;
			{
				int tabs = 0;
				for (t = line; *t; t++) if (*t == '\t' && ++tabs == 3) break;
				if (*t) {
					char *e = strchr(t + 1, '\t');
					if (e) *e = 0;
					pm_copy(out, t + 1, max);
				}
			}
			break;
		}
		fclose(f);
	}
	if (!out[0]) {
		const char *d = kind == 'T' ? "Trash" : kind == 'S' ? "Sent" : kind == 'A' ? "Archive" :
			kind == 'D' ? "Drafts" : kind == 'J' ? "Spam" : "INBOX";
		pm_copy(out, d, max);
		return 0;
	}
	return 1;
}

/* ----------------------------------------------------------------- sync */

typedef struct
	{
	PmIndex *ix;
	unsigned int minuid;      /* only messages above this are new */
	int count, total;
	int newunseen;
	unsigned int *seen_uids;  /* flag sync: uids the server still has */
	int nseen, capseen;
	long exists;
	unsigned long uidvalidity, uidnext;
	unsigned int first_seq;   /* UID FETCH x (UID) -> its sequence number */
	} SyncCtx;

static void addr_str(ImapNode *list, char *out, int max, int many)
{
	/* ((name adl mailbox host) ...) -> "Name <box@host>, ..." */
	ImapNode *a;
	int k = 0;
	out[0] = 0;
	if (!list || list->type != IT_LIST) return;
	for (a = list->child; a && k < max - 4; a = a->next) {
		char name[120], raw[160], box[64], host[64];
		ip_str(ip_nth(a, 0), raw, sizeof(raw));
		cs_decode_header(raw, name, sizeof(name));
		ip_str(ip_nth(a, 2), box, sizeof(box));
		ip_str(ip_nth(a, 3), host, sizeof(host));
		if (!box[0] && !host[0]) continue;       /* group syntax */
		if (k) { out[k++] = ','; out[k++] = ' '; }
		if (name[0]) k += snprintf(out + k, max - k, "%s <%s@%s>", name, box, host);
		else k += snprintf(out + k, max - k, "%s@%s", box, host);
		if (k >= max) { k = max - 1; break; }
		if (!many) break;
	}
	out[k] = 0;
}

static const char *k_months = "JanFebMarAprMayJunJulAugSepOctNovDec";

/* "17-Jul-1996 02:44:25 -0700" -> seconds since 1970 (UTC) */
static long num(const char **p, int max)
{
	long v = 0;
	int n = 0;
	while (**p == ' ') (*p)++;
	while (n < max && **p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; n++; }
	return n ? v : -1;
}

static long parse_internaldate(const char *s)
{
	static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	long d, y, hh, mm, ss, tz, days, t;
	int m, sign = 1;
	d = num(&s, 2);
	if (d < 1 || *s++ != '-') return 0;
	for (m = 0; m < 12; m++) if (!pm_strncasecmp(k_months + m * 3, s, 3)) break;
	if (m == 12) return 0;
	s += 3;
	if (*s++ != '-') return 0;
	y = num(&s, 4);
	hh = num(&s, 2); if (*s == ':') s++;
	mm = num(&s, 2); if (*s == ':') s++;
	ss = num(&s, 2);
	while (*s == ' ') s++;
	if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
	tz = num(&s, 4);
	if (y < 1970 || hh < 0 || mm < 0 || ss < 0) return 0;
	if (tz < 0) tz = 0;
	days = (y - 1970) * 365L + (y - 1969) / 4 + cum[m] + (d - 1);
	if (m >= 2 && y % 4 == 0) days++;
	t = days * 86400L + hh * 3600L + mm * 60L + ss;
	t -= sign * ((tz / 100) * 3600L + (tz % 100) * 60L);
	return t;
}

static void flags_from(ImapNode *fl, char *out)
{
	ImapNode *c;
	out[0] = 0;
	if (!fl || fl->type != IT_LIST) return;
	for (c = fl->child; c; c = c->next) {
		if (ip_eq(c, "\\Seen")) st_flag_set(out, 'S', 1);
		else if (ip_eq(c, "\\Flagged")) st_flag_set(out, 'F', 1);
		else if (ip_eq(c, "\\Answered")) st_flag_set(out, 'A', 1);
		else if (ip_eq(c, "\\Deleted")) st_flag_set(out, 'D', 1);
	}
}

static void on_fetch_msg(ImapNode *r, void *ctx)
{
	SyncCtx *sc = (SyncCtx *)ctx;
	ImapNode *w = r->child, *items, *v, *env;
	unsigned int uid;
	PmMsg *m;
	char f[12], raw[400];

	if (!w || !w->next) return;
	if (ip_eq(w->next, "EXISTS")) { sc->exists = ip_num(w); return; }
	if (ip_eq(w, "OK") && w->next && w->next->type == IT_ATOM) {
		/* * OK [UIDVALIDITY 123] / [UIDNEXT 456] (one atom: brackets hold spaces) */
		char t[40];
		ip_str(w->next, t, sizeof(t));
		if (!pm_strncasecmp(t, "[UIDVALIDITY ", 13)) sc->uidvalidity = strtoul(t + 13, 0, 10);
		if (!pm_strncasecmp(t, "[UIDNEXT ", 9)) sc->uidnext = strtoul(t + 9, 0, 10);
		return;
	}
	if (!ip_eq(w->next, "FETCH")) return;
	items = w->next->next;
	if (!(v = ip_get(items, "UID"))) return;
	uid = (unsigned int)ip_num(v);
	if (!sc->first_seq) sc->first_seq = (unsigned int)ip_num(w);

	if (sc->seen_uids) {
		/* flag sync: record that it still exists, update its flags */
		if (sc->nseen >= sc->capseen) {
			int nc = sc->capseen ? sc->capseen * 2 : 256;
			unsigned int *n2 = (unsigned int *)realloc(sc->seen_uids, nc * sizeof(unsigned int));
			if (!n2) return;
			sc->seen_uids = n2; sc->capseen = nc;
		}
		sc->seen_uids[sc->nseen++] = uid;
		if ((m = st_index_find(sc->ix, uid)) != 0 && (v = ip_get(items, "FLAGS")) != 0) {
			int body = st_flag_has(m->flags, 'B');
			flags_from(v, f);
			st_flag_set(f, 'B', body);
			strcpy(m->flags, f);
		}
		return;
	}
	if (!(env = ip_get(items, "ENVELOPE"))) return;
	if (uid <= sc->minuid && sc->minuid) return;
	if (!(m = st_index_add(sc->ix, uid))) return;
	flags_from(ip_get(items, "FLAGS"), m->flags);
	m->size = ip_num(ip_get(items, "RFC822.SIZE"));
	ip_str(ip_get(items, "INTERNALDATE"), raw, sizeof(raw));
	m->date = parse_internaldate(raw);
	ip_str(ip_nth(env, 1), raw, sizeof(raw));
	cs_decode_header(raw, m->subject, sizeof(m->subject));
	addr_str(ip_nth(env, 2), m->from, sizeof(m->from), 0);
	addr_str(ip_nth(env, 5), m->to, sizeof(m->to), 1);
	ip_str(ip_nth(env, 8), m->inreplyto, sizeof(m->inreplyto));
	ip_str(ip_nth(env, 9), m->msgid, sizeof(m->msgid));
	if ((v = ip_get(items, "BODYSTRUCTURE")) != 0) {
		static PmStructure st;
		mime_structure(v, &st);
		m->attach = st.nattach > 0;
	}
	if (!st_flag_has(m->flags, 'S')) sc->newunseen++;
	sc->count++;
	if (sc->total) pm_progress("Fetching message %d of %d...", sc->count, sc->total);
	else pm_progress("Fetching message %d...", sc->count);
}

static int cmp_uint(const void *a, const void *b)
{
	unsigned int x = *(const unsigned int *)a, y = *(const unsigned int *)b;
	return x < y ? -1 : x > y;
}

#define FETCH_ITEMS "(UID FLAGS INTERNALDATE RFC822.SIZE ENVELOPE BODYSTRUCTURE)"

static void remove_cached(int acct, const char *folder, unsigned int uid)
{
	char p[190];
	st_msg_path(acct, folder, uid, "txt", p, sizeof(p)); remove(p);
	st_msg_path(acct, folder, uid, "att", p, sizeof(p)); remove(p);
	st_msg_path(acct, folder, uid, "htm", p, sizeof(p)); remove(p);
}

int imap_sync(int acct, const char *folder, int older, char *why, int whymax)
{
	PmAccount *a = &pm_shared()->acct[acct];
	static PmIndex ix;
	SyncCtx sc;
	int r, i, want = a->sync_count > 0 ? a->sync_count : 50;
	unsigned int maxuid, minuid;

	if ((r = imap_open(acct, why, whymax)) != PM_RES_OK) return r;
	st_index_load(acct, folder, "index.txt", &ix);
	memset(&sc, 0, sizeof(sc));
	sc.ix = &ix;
	pm_progress("Opening %s...", folder);
	r = select_folder(folder, on_fetch_msg, &sc, why, whymax);
	if (r != PM_RES_OK) { st_index_free(&ix); return r; }

	if (ix.uidvalidity && sc.uidvalidity && ix.uidvalidity != sc.uidvalidity) {
		/* the server renumbered the folder: everything we have is stale */
		char dir[160];
		st_folder_dir(acct, folder, dir, sizeof(dir));
		pm_rmtree(dir);
		st_index_free(&ix);
		memset(&ix, 0, sizeof(ix));
	}
	ix.uidvalidity = sc.uidvalidity;
	ix.uidnext = sc.uidnext;
	ix.exists = sc.exists;
	maxuid = ix.n ? ix.m[ix.n - 1].uid : 0;
	minuid = ix.n ? ix.m[0].uid : 0;

	if (sc.exists == 0) {
		for (i = 0; i < ix.n; i++) remove_cached(acct, folder, ix.m[i].uid);
		ix.n = 0;
	} else if (older && ix.n) {
		/* the 'want' messages before the oldest we have */
		SyncCtx s2;
		memset(&s2, 0, sizeof(s2));
		s2.ix = &ix; s2.seen_uids = 0;
		r = cmd(on_fetch_msg, &s2, why, whymax, "UID FETCH %u (UID)", minuid);
		if (r == PM_RES_OK && s2.first_seq > 1) {
			unsigned int to = s2.first_seq - 1, from = to > (unsigned)want ? to - want + 1 : 1;
			memset(&sc, 0, sizeof(sc));
			sc.ix = &ix;
			sc.total = to - from + 1;
			r = cmd(on_fetch_msg, &sc, why, whymax, "FETCH %u:%u " FETCH_ITEMS, from, to);
		} else if (r == PM_RES_OK) {
			set_why(why, whymax, "There are no older messages");
		}
	} else if (!ix.n) {
		/* first look at this folder: the newest 'want' messages */
		long from = sc.exists > want ? sc.exists - want + 1 : 1;
		memset(&sc, 0, sizeof(sc));
		sc.ix = &ix;
		sc.total = (int)(ix.exists - from + 1);
		r = cmd(on_fetch_msg, &sc, why, whymax, "FETCH %ld:* " FETCH_ITEMS, from);
	} else {
		/* 1. what has changed or gone among the ones we have */
		SyncCtx s2;
		memset(&s2, 0, sizeof(s2));
		s2.ix = &ix;
		s2.seen_uids = (unsigned int *)malloc(256 * sizeof(unsigned int));
		s2.capseen = s2.seen_uids ? 256 : 0;
		pm_progress("Checking %s for changes...", folder);
		r = cmd(on_fetch_msg, &s2, why, whymax, "UID FETCH %u:%u (UID FLAGS)", minuid, maxuid);
		if (r == PM_RES_OK && s2.seen_uids) {
			qsort(s2.seen_uids, s2.nseen, sizeof(unsigned int), cmp_uint);
			for (i = ix.n - 1; i >= 0; i--) {
				unsigned int u = ix.m[i].uid;
				if (!bsearch(&u, s2.seen_uids, s2.nseen, sizeof(unsigned int), cmp_uint)) {
					remove_cached(acct, folder, u);
					st_index_remove(&ix, u);
				}
			}
		}
		free(s2.seen_uids);
		/* 2. new messages */
		if (r == PM_RES_OK && (!sc.uidnext || sc.uidnext > maxuid + 1)) {
			memset(&sc, 0, sizeof(sc));
			sc.ix = &ix;
			sc.minuid = maxuid;
			r = cmd(on_fetch_msg, &sc, why, whymax, "UID FETCH %u:* " FETCH_ITEMS, maxuid + 1);
		}
	}
	/* keep the folder from growing without limit on the card */
	while (ix.n > want * 4 && ix.n > 200) {
		remove_cached(acct, folder, ix.m[0].uid);
		st_index_remove(&ix, ix.m[0].uid);
	}
	if (!strcmp(folder, "INBOX") && !older) pm_shared()->new_mail = sc.newunseen;
	if (st_index_save(acct, folder, "index.txt", &ix) != 0 && r == PM_RES_OK) {
		char d[160];
		st_folder_dir(acct, folder, d, sizeof(d));
		pm_write_why(why, whymax, "the message list", d);
		r = PM_RES_FAILED;
	}
	if (r == PM_RES_OK) {
		if (sc.count) set_why(why, whymax, "%d new", sc.count);
		else if (!older) set_why(why, whymax, "No new messages");
	}
	st_index_free(&ix);
	return r;
}

/* ------------------------------------------------ fetching a part in pieces
 *
 * Without RTS/CTS the serial port has only its 16 KB buffer between the
 * modem and us: a long body sent in one go overruns it while the Psion is
 * busy, a TLS record is lost and the connection with it. So a part comes
 * a piece at a time (<offset.length>), each small enough to sit in that
 * buffer, and if the line still drops we log in again and carry on from
 * the byte we had got to. */

extern PsiShared *pg_shared(void);

typedef struct { StreamFn fn; void *ctx; long got; } Piece;

static void piece_stream(const char *d, int n, void *ctx)
{
	Piece *p = (Piece *)ctx;
	p->got += n;
	p->fn(d, n, p->ctx);
}

static ImapNode *body_item(ImapNode *r);

static void on_piece_quoted(ImapNode *r, void *ctx)
{
	ImapNode *v = body_item(r);
	if (v) piece_stream(v->s, v->len, ctx);
}

static long piece_size(void)
{
	PsiShared *s = pg_shared();
	if (s && !s->net_mode && !s->rtscts) return 8192;
	return 65536;
}

/* select the folder, logging in again once if the old connection had gone */
static int open_folder(int acct, const char *folder, char *why, int whymax)
{
	int was = g_acct == acct && pmn_is_open();
	int r = imap_open(acct, why, whymax);
	if (r == PM_RES_OK) r = select_folder(folder, 0, 0, why, whymax);
	if (r == PM_RES_OFFLINE && was && !pm_cancelled() && !pm_shared()->offline) {
		pm_log("imap: connection had gone; logging in again");
		r = imap_open(acct, why, whymax);
		if (r == PM_RES_OK) r = select_folder(folder, 0, 0, why, whymax);
	}
	return r;
}

/* the first 'total' bytes of a part (all of it if total <= 0) to fn */
static int fetch_part(int acct, const char *folder, unsigned int uid, const char *part,
	long total, StreamFn fn, void *ctx, char *why, int whymax)
{
	static Piece p;
	static long s_step;                   /* smaller after a drop, for the next message too */
	long step;
	int again = 0, drops = 0, r;
	p.fn = fn; p.ctx = ctx; p.got = 0;
	if (s_step <= 0 || s_step > piece_size()) s_step = piece_size();
	for (;;) {
		step = s_step;
		long before = p.got, want;
		if (total > 0 && p.got >= total) {
			if (drops) why[0] = 0;
			return PM_RES_OK;
		}
		want = total > 0 && total - p.got < step ? total - p.got : step;
		g_stream = piece_stream;
		g_stream_ctx = &p;
		r = cmd(on_piece_quoted, &p, why, whymax, "UID FETCH %u (BODY.PEEK[%s]<%ld.%ld>)", uid, part, p.got, want);
		g_stream = 0;
		if (r == PM_RES_OFFLINE && !pm_cancelled() && again < 3 && drops < 12) {
			pm_log("imap: %u [%s] dropped at %ld of %ld; again", uid, part, p.got, total);
			again++;
			drops++;
			if (s_step > 2048) s_step /= 2;   /* the line can't take that much at once */
			pm_progress("Reconnecting...");
			if ((r = open_folder(acct, folder, why, whymax)) != PM_RES_OK) return r;
			continue;
		}
		if (r != PM_RES_OK) return r;
		if (p.got > before) again = 0;
		if (p.got - before < want) {                      /* the end of the part */
			if (drops) why[0] = 0;                        /* (not "lost": it came back) */
			return PM_RES_OK;
		}
	}
}

/* ----------------------------------------------------------- message text */

typedef struct
	{
	FILE *f;
	PmDecoder dec;
	int utf8;
	char charset[24];
	char carry[4];            /* an incomplete UTF-8 sequence from the last piece */
	int ncarry;
	HtmlConv *html;
	int flowed, delsp;
	char line[1024];          /* format=flowed: the line being joined */
	int ln;
	int cr;                   /* last char written was \r (to drop it) */
	FILE *hf;                 /* HTML messages: the original, for NetSurf */
	char pl[1600];            /* plain text: the line being built */
	int pln;
	int sig;                  /* after "-- ": the signature */
	int nurls;
	char *urls[60];
	long got;
	long total;
	int percent;
	/* structure / envelope from the first FETCH */
	PmStructure st;
	char hdr[1400];
	int have;
	char label[48];           /* "'Re: hello'" - what the progress messages call it */
	} BodyCtx;

/* "Downloading 'Re: hello'... 40%" (pm_progress adds "Ahead (3 of 10): "
   while downloading ahead) */
static void body_progress(const char *label, const char *doing, int percent)
{
	if (percent >= 0) pm_progress("%s %s... %d%%", doing, label, percent);
	else pm_progress("%s %s...", doing, label);
}

/* ---- plain text to PsiMail's rich text (see ui/pmui.h): quotes ("> ")
   become quote blocks, the signature is marked, web addresses become links */

static void plain_link(BodyCtx *b, const char *u, int n)
{
	char num[12];
	if (b->nurls >= 60) { fwrite(u, 1, n, b->f); return; }
	b->urls[b->nurls] = (char *)malloc(n + 8);
	if (!b->urls[b->nurls]) { fwrite(u, 1, n, b->f); return; }
	if (!pm_strncasecmp(u, "www.", 4)) { strcpy(b->urls[b->nurls], "http://"); memcpy(b->urls[b->nurls] + 7, u, n); b->urls[b->nurls][n + 7] = 0; }
	else { memcpy(b->urls[b->nurls], u, n); b->urls[b->nurls][n] = 0; }
	b->nurls++;
	sprintf(num, "\x15%d\x16", b->nurls);
	fputs(num, b->f);
	fwrite(u, 1, n, b->f);
	fputc('\x17', b->f);
}

static void plain_text(BodyCtx *b, const char *s, int n)
{
	int i = 0;
	while (i < n) {
		int at = -1, k;
		for (k = i; k < n; k++) {
			if ((k == i || s[k - 1] == ' ' || s[k - 1] == '(' || s[k - 1] == '<') &&
			    (!pm_strncasecmp(s + k, "http://", 7) || !pm_strncasecmp(s + k, "https://", 8) ||
			     (!pm_strncasecmp(s + k, "www.", 4) && k + 5 < n))) { at = k; break; }
		}
		if (at < 0) { fwrite(s + i, 1, n - i, b->f); return; }
		fwrite(s + i, 1, at - i, b->f);
		k = at;
		while (k < n && s[k] != ' ' && s[k] != '>' && s[k] != '"' && s[k] != '<') k++;
		while (k > at && (s[k - 1] == '.' || s[k - 1] == ',' || s[k - 1] == ')' || s[k - 1] == ';' || s[k - 1] == ':')) k--;
		plain_link(b, s + at, k - at);
		i = k;
	}
}

static void plain_line(BodyCtx *b, char *s, int n)
{
	int i, depth = 0, p = 0;
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\t') s[i] = ' ';
		else if (c < 0x20) s[i] = ' ';
	}
	if ((n == 3 && !memcmp(s, "-- ", 3)) || (n == 2 && !memcmp(s, "--", 2))) b->sig = 1;
	if (b->sig) {
		fputs("\x01s", b->f);
		plain_text(b, s, n);
		fputc('\n', b->f);
		return;
	}
	/* "> > text" / ">> text" */
	while (p < n && (s[p] == '>' || (s[p] == ' ' && depth > 0 && p + 1 < n && s[p + 1] == '>'))) {
		if (s[p] == '>') depth++;
		p++;
	}
	if (depth > 0) {
		if (p < n && s[p] == ' ') p++;
		fprintf(b->f, "\x01q%d", depth > 9 ? 9 : depth);
		if (p == n) fputc(' ', b->f);           /* an empty quoted line keeps the bar */
		plain_text(b, s + p, n - p);
		fputc('\n', b->f);
		return;
	}
	plain_text(b, s, n);
	fputc('\n', b->f);
}

static void plain_out(BodyCtx *b, const char *s, int n, int end_of_line)
{
	int i;
	for (i = 0; i < n; i++) {
		if (b->pln < (int)sizeof(b->pl)) b->pl[b->pln++] = s[i];
		else { plain_line(b, b->pl, b->pln); b->pln = 0; b->pl[b->pln++] = s[i]; }
	}
	if (end_of_line) { plain_line(b, b->pl, b->pln); b->pln = 0; }
}

/* the HTML converter's output is already rich text */
static void out_rich(const char *s, int n, void *ctx)
{
	BodyCtx *b = (BodyCtx *)ctx;
	fwrite(s, 1, n, b->f);
}

static void out_text(const char *s, int n, void *ctx)
{
	BodyCtx *b = (BodyCtx *)ctx;
	int i;
	for (i = 0; i < n; i++) {
		char c = s[i];
		if (c == '\r') continue;
		if (b->flowed) {
			if (c == '\n') {
				/* a line ending in a space continues on the next (RFC 3676) */
				int soft = b->ln > 0 && b->line[b->ln - 1] == ' ' &&
					!(b->ln == 3 && !memcmp(b->line, "-- ", 3));
				if (soft) {
					if (b->delsp) b->ln--;
					plain_out(b, b->line, b->ln, 0);
				} else
					plain_out(b, b->line, b->ln, 1);
				b->ln = 0;
				continue;
			}
			if (b->ln < (int)sizeof(b->line)) b->line[b->ln++] = c;
			else { plain_out(b, b->line, b->ln, 0); b->ln = 0; b->line[b->ln++] = c; }
			continue;
		}
		if (c == '\n') plain_out(b, 0, 0, 1);
		else plain_out(b, &c, 1, 0);
	}
}

/* the raw text, on its way to a file */
typedef struct { FILE *f; long got, total; int percent, err; const char *label; } Spool;

static void spool_stream(const char *data, int n, void *ctx)
{
	Spool *s = (Spool *)ctx;
	s->got += n;
	if (s->total > 0) {
		int pc = (int)(s->got * 100 / s->total);
		if (pc > 100) pc = 100;
		pc -= pc % 5;                     /* (each update redraws the screen) */
		if (pc != s->percent) { s->percent = pc; body_progress(s->label, "Downloading", pc); }
	}
	if (s->err) return;                       /* (the rest goes by; the failure is reported once) */
	if (fwrite(data, 1, n, s->f) != (size_t)n) s->err = 1;
}

static void body_stream(const char *data, int n, void *ctx)
{
	BodyCtx *b = (BodyCtx *)ctx;
	static char dec[1100], conv[2400];
	int dn, cn, i = 0;
	b->got += n;
	if (b->total > 0) {
		int pc = (int)(b->got * 100 / b->total);
		if (pc != b->percent) { b->percent = pc; body_progress(b->label, "Downloading", pc); }
	}
	dn = dec_feed(&b->dec, data, n, dec + b->ncarry);
	if (b->ncarry) { memcpy(dec, b->carry, b->ncarry); dn += b->ncarry; b->ncarry = 0; }
	if (b->utf8) {
		/* hold back a UTF-8 sequence cut off at the end */
		int e = dn, back = 0;
		while (e > 0 && back < 3 && ((unsigned char)dec[e - 1] & 0xc0) == 0x80) { e--; back++; }
		if (e > 0 && (unsigned char)dec[e - 1] >= 0xc0) {
			unsigned char lead = (unsigned char)dec[e - 1];
			int need = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2 : 1;
			if (back < need) { e--; b->ncarry = dn - e; memcpy(b->carry, dec + e, b->ncarry); dn = e; }
		}
	}
	(void)i;
	if (b->hf && dn) fwrite(dec, 1, dn, b->hf);
	cn = cs_to_cp1252(b->charset, dec, dn, conv, sizeof(conv));
	if (b->html) html_feed(b->html, conv, cn, out_rich, b);
	else out_text(conv, cn, b);
}

/* A body sent as a quoted string rather than a literal (literals are all
   streamed while g_stream is set): find BODY[...] and pass it on. */
static ImapNode *body_item(ImapNode *r)
{
	ImapNode *w = r->child, *c;
	if (!w || !w->next || !ip_eq(w->next, "FETCH") || !w->next->next) return 0;
	for (c = w->next->next->child; c && c->next; c = c->next->next)
		if (c->type == IT_ATOM && c->len > 5 && !pm_strncasecmp(c->s, "BODY[", 5))
			return c->next->type == IT_STRING && c->next->len > 0 ? c->next : 0;
	return 0;
}


static void on_fetch_struct(ImapNode *r, void *ctx)
{
	BodyCtx *b = (BodyCtx *)ctx;
	ImapNode *w = r->child, *items, *v, *env;
	char raw[400], tmp[400];
	int k = 0;
	if (!w || !w->next || !ip_eq(w->next, "FETCH")) return;
	items = w->next->next;
	if ((v = ip_get(items, "BODYSTRUCTURE")) != 0) mime_structure(v, &b->st);
	if ((env = ip_get(items, "ENVELOPE")) != 0) {
		static const struct { int i; const char *h; } hs[] = {
			{ 2, "From" }, { 4, "Reply-To" }, { 5, "To" }, { 6, "Cc" } };
		int j;
		b->have = 1;
		ip_str(ip_nth(env, 0), raw, sizeof(raw));
		k += snprintf(b->hdr + k, sizeof(b->hdr) - k, "Date: %s\n", raw);
		for (j = 0; j < 4; j++) {
			addr_str(ip_nth(env, hs[j].i), tmp, sizeof(tmp), 1);
			if (tmp[0]) k += snprintf(b->hdr + k, sizeof(b->hdr) - k, "%s: %s\n", hs[j].h, tmp);
			if (k >= (int)sizeof(b->hdr)) k = sizeof(b->hdr) - 1;
		}
		ip_str(ip_nth(env, 1), raw, sizeof(raw));
		cs_decode_header(raw, tmp, sizeof(tmp));
		k += snprintf(b->hdr + k, sizeof(b->hdr) - k, "Subject: %s\n", tmp);
		snprintf(b->label, sizeof(b->label), "'%.36s%s'", tmp[0] ? tmp : "(no subject)", strlen(tmp) > 36 ? ".." : "");
		if (k >= (int)sizeof(b->hdr)) k = sizeof(b->hdr) - 1;
		ip_str(ip_nth(env, 9), raw, sizeof(raw));
		if (raw[0]) k += snprintf(b->hdr + k, sizeof(b->hdr) - k, "Message-ID: %s\n", raw);
		if (k >= (int)sizeof(b->hdr)) k = sizeof(b->hdr) - 1;
	}
}

int imap_body(int acct, const char *folder, unsigned int uid, int full, char *why, int whymax)
{
	int keep_unread = (full & 2) != 0;     /* 2: downloaded ahead - not read yet */
	PmAccount *a = &pm_shared()->acct[acct];
	static BodyCtx b;
	char path[190], tmp[196], raw[196], hp[190], apath[190], dir[160];
	int r, i;
	long limit = (a->max_body_kb > 0 ? a->max_body_kb : 64) * 1024L;
	FILE *f;

	full &= 1;

	if ((r = open_folder(acct, folder, why, whymax)) != PM_RES_OK) return r;
	memset(&b, 0, sizeof(b));
	pm_copy(b.label, "the message", sizeof(b.label));
	body_progress(b.label, "Opening", -1);
	r = cmd(on_fetch_struct, &b, why, whymax, "UID FETCH %u (ENVELOPE BODYSTRUCTURE)", uid);
	if (r != PM_RES_OK) return r;
	if (!b.have) { set_why(why, whymax, "The message has gone from the server"); return PM_RES_FAILED; }

	st_folder_dir(acct, folder, dir, sizeof(dir));
	pm_mkdir(dir);
	st_msg_path(acct, folder, uid, "txt", path, sizeof(path));
	st_msg_path(acct, folder, uid, "htm", hp, sizeof(hp));
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	snprintf(raw, sizeof(raw), "%s.raw", path);
	{
		/* room for what is coming? (the raw text and PsiMail's text, plus the
		   HTML original) - better to know now than after the download */
		PmPart *tp = b.st.text >= 0 ? &b.st.part[b.st.text] : 0;
		long need = tp ? ((full || tp->size <= limit) ? tp->size : limit) : 0, freekb;
		need = need * (b.st.html ? 3 : 2) / 1024 + 8;
		if ((freekb = pm_free_kb(dir)) >= 0 && freekb < need) {
			set_why(why, whymax, "No room left on %.*s for the message (%ld KB needed, %ld KB free)",
				dir[1] == ':' ? 2 : 8, dir[1] == ':' ? dir : "the disk", need, freekb);
			return PM_RES_FAILED;
		}
	}

	/* attachments list */
	st_msg_path(acct, folder, uid, "att", apath, sizeof(apath));
	if (b.st.nattach) {
		if ((f = fopen(apath, "w")) != 0) {
			for (i = 0; i < b.st.n; i++) {
				PmPart *p = &b.st.part[i];
				if (i == b.st.text || !p->attachment) continue;
				fprintf(f, "%s\t%ld\t%s\t%s\t%d\n", p->id, p->size, p->name[0] ? p->name : "(no name)", p->type, p->enc);
			}
			if (pm_fclose(f) != 0) remove(apath);          /* (no list rather than a broken one) */
		}
	} else remove(apath);

	if (!(b.f = fopen(tmp, "w"))) { pm_write_why(why, whymax, "the message", path); return PM_RES_FAILED; }
	{
		long rest = 0;
		PmPart *tp = b.st.text >= 0 ? &b.st.part[b.st.text] : 0;
		if (tp && !full && tp->size > limit) rest = tp->size - limit;
		fprintf(b.f, "#PSIMAIL1\t%ld\t%d\n", rest, b.st.html);
		fputs(b.hdr, b.f);
		fputc('\n', b.f);
		if (!tp) {
			fputs("(This message has no text to show.)\n", b.f);
		} else {
			dec_init(&b.dec, tp->enc);
			pm_copy(b.charset, tp->charset[0] ? tp->charset : "us-ascii", sizeof(b.charset));
			b.utf8 = cs_is_utf8(b.charset);
			b.html = b.st.html ? html_new() : 0;
			b.total = (tp->size < limit || full) ? tp->size : limit;
			b.flowed = tp->flowed && !b.st.html;
			b.delsp = tp->delsp;
			b.percent = -1;
			if (b.html) {
				/* keep the HTML as it came, for "View as web page" */
				if ((b.hf = fopen(hp, "wb")) != 0)
					fprintf(b.hf, "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=%s\">\n", b.charset);
			}
			/* first the text as it comes, into a file (quick, so the
			   serial port keeps up); then turned into PsiMail's text */
			{
				static Spool sp;
				memset(&sp, 0, sizeof(sp));
				sp.total = b.total;
				sp.percent = -1;
				sp.label = b.label;
				if (!(sp.f = fopen(raw, "wb"))) { pm_write_why(why, whymax, "the message", path); r = PM_RES_FAILED; }
				else {
					r = fetch_part(acct, folder, uid, tp->id, (full || tp->size <= limit) ? 0 : limit, spool_stream, &sp, why, whymax);
					if (pm_fclose(sp.f) != 0) sp.err = 1;
					if (r == PM_RES_OK && sp.err) { pm_write_why(why, whymax, "the message", path); r = PM_RES_FAILED; }
					if (r == PM_RES_OK && (sp.f = fopen(raw, "rb")) != 0) {
						static char buf[1024];
						size_t n;
						body_progress(b.label, "Setting out", -1);
						b.total = 0;
						while ((n = fread(buf, 1, sizeof(buf), sp.f)) > 0) body_stream(buf, (int)n, &b);
						fclose(sp.f);
					}
				}
				remove(raw);
			}
			if (b.html) { html_end(b.html, out_rich, &b); html_free(b.html); b.html = 0; }
			else {
				int k;
				if (b.ln) plain_out(&b, b.line, b.ln, 0);
				if (b.pln) plain_out(&b, 0, 0, 1);
				for (k = 0; k < b.nurls; k++) { fprintf(b.f, "\x01u%d %s\n", k + 1, b.urls[k]); free(b.urls[k]); }
				b.nurls = 0;
			}
			if (b.hf) {
				/* a web page that did not all arrive is worse than none */
				if (pm_fclose(b.hf) != 0 || r != PM_RES_OK) remove(hp);
				b.hf = 0;
			}
		}
	}
	if (pm_fclose(b.f) != 0 && r == PM_RES_OK) { pm_write_why(why, whymax, "the message", path); r = PM_RES_FAILED; }
	if (r == PM_RES_OK && pm_replace(tmp, path) != 0) {
		pm_log("imap: could not put %s in place", path);
		pm_write_why(why, whymax, "the message", path);
		r = PM_RES_FAILED;
	}
	if (r != PM_RES_OK) { remove(tmp); return r; }

	/* mark it read, here and on the server */
	{
		static PmIndex ix;
		PmMsg *m;
		int was_seen = 1;
		if (st_index_load(acct, folder, "index.txt", &ix) == 0 && (m = st_index_find(&ix, uid)) != 0) {
			was_seen = st_flag_has(m->flags, 'S');
			if (!keep_unread)
				st_flag_set(m->flags, 'S', 1);
			st_flag_set(m->flags, 'B', 1);
			m->attach = b.st.nattach > 0;
			st_index_save(acct, folder, "index.txt", &ix);
		}
		st_index_free(&ix);
		if (!was_seen && !keep_unread) cmd(0, 0, why, whymax, "UID STORE %u +FLAGS.SILENT (\\Seen)", uid);
	}
	st_changed();
	return PM_RES_OK;
}

/* ----------------------------------------------------------- attachments */

typedef struct
	{
	FILE *f;
	PmDecoder dec;
	long got, total;
	int percent;
	int err;
	} AttCtx;

static void att_stream(const char *data, int n, void *ctx)
{
	AttCtx *c = (AttCtx *)ctx;
	static char dec[1100];
	int dn;
	c->got += n;
	if (c->total > 0) {
		int pc = (int)(c->got * 100 / c->total);
		pc -= pc % 5;
		if (pc != c->percent) { c->percent = pc; pm_progress("Downloading the attachment... %d%%", pc); }
	}
	dn = dec_feed(&c->dec, data, n, dec);
	if (c->err) return;
	if (dn && fwrite(dec, 1, dn, c->f) != (size_t)dn) c->err = 1;
}

static void safe_name(const char *in, char *out, int max)
{
	int k = 0;
	for (; *in && k < max - 1; in++) {
		char ch = *in;
		if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|' || (unsigned char)ch < 32)
			ch = '_';
		out[k++] = ch;
	}
	while (k > 0 && (out[k - 1] == '.' || out[k - 1] == ' ')) k--;
	out[k] = 0;
	if (!k) pm_copy(out, "attachment", max);
}

int imap_attach(int acct, const char *folder, unsigned int uid, const char *part, char *why, int whymax)
{
	static BodyCtx b;
	static AttCtx c;
	PmShared *s = pm_shared();
	PmPart *p = 0;
	char name[100], path[200], base[100], ext[20];
	int r, i;
	FILE *t;

	if ((r = open_folder(acct, folder, why, whymax)) != PM_RES_OK) return r;
	memset(&b, 0, sizeof(b));
	r = cmd(on_fetch_struct, &b, why, whymax, "UID FETCH %u (BODYSTRUCTURE)", uid);
	if (r != PM_RES_OK) return r;
	for (i = 0; i < b.st.n; i++) if (!strcmp(b.st.part[i].id, part)) p = &b.st.part[i];
	if (!p) { set_why(why, whymax, "That attachment is not in the message any more"); return PM_RES_FAILED; }

	safe_name(p->name[0] ? p->name : "attachment", name, sizeof(name));
	pm_mkdir(s->attach_dir);
	/* don't overwrite: name.ext, name(1).ext ... */
	{
		char *dot = strrchr(name, '.');
		if (dot && strlen(dot) < sizeof(ext)) { strcpy(ext, dot); *dot = 0; } else ext[0] = 0;
		pm_copy(base, name, sizeof(base));
		snprintf(path, sizeof(path), "%s%s%s", s->attach_dir, base, ext);
		for (i = 1; i < 100 && (t = fopen(path, "r")) != 0; i++) {
			fclose(t);
			snprintf(path, sizeof(path), "%s%s(%d)%s", s->attach_dir, base, i, ext);
		}
	}
	memset(&c, 0, sizeof(c));
	if (!(c.f = fopen(path, "wb"))) { pm_write_why(why, whymax, "the attachment", path); return PM_RES_FAILED; }
	dec_init(&c.dec, p->enc);
	c.total = p->size;
	c.percent = -1;
	r = fetch_part(acct, folder, uid, part, 0, att_stream, &c, why, whymax);
	if (pm_fclose(c.f) != 0) c.err = 1;
	if (r == PM_RES_OK && c.err) { pm_write_why(why, whymax, "the attachment", path); r = PM_RES_FAILED; }
	if (r == PM_RES_OK && c.got == 0 && p->size > STREAM_MIN) { set_why(why, whymax, "The server sent nothing"); r = PM_RES_FAILED; }
	if (r != PM_RES_OK) { remove(path); return r; }
	pm_copy(s->last_file, path, sizeof(s->last_file));
	set_why(why, whymax, "Saved %s", path);
	return PM_RES_OK;
}

/* --------------------------------------------------------- flags, moves */

static const char *flag_name(char f)
{
	switch (f) {
	case 'S': return "\\Seen";
	case 'F': return "\\Flagged";
	case 'A': return "\\Answered";
	case 'D': return "\\Deleted";
	}
	return 0;
}

int imap_flag(int acct, const char *folder, unsigned int uid, const char *op, char *why, int whymax)
{
	const char *fn = flag_name(op[1]);
	int r;
	if (!fn || (op[0] != '+' && op[0] != '-')) { set_why(why, whymax, "Bad flag change"); return PM_RES_FAILED; }
	if ((r = imap_open(acct, why, whymax)) != PM_RES_OK) return r;
	if ((r = select_folder(folder, 0, 0, why, whymax)) != PM_RES_OK) return r;
	return cmd(0, 0, why, whymax, "UID STORE %u %cFLAGS.SILENT (%s)", uid, op[0], fn);
}

int imap_move(int acct, const char *folder, unsigned int uid, const char *dest, char *why, int whymax)
{
	char q[160], trash[128];
	int r;
	if ((r = imap_open(acct, why, whymax)) != PM_RES_OK) return r;
	if ((r = select_folder(folder, 0, 0, why, whymax)) != PM_RES_OK) return r;
	imap_special_folder(acct, 'T', trash, sizeof(trash));
	if (!dest[0]) dest = trash;
	if (!strcmp(dest, folder)) {
		/* deleting from the Trash itself: gone for good */
		r = cmd(0, 0, why, whymax, "UID STORE %u +FLAGS.SILENT (\\Deleted)", uid);
		if (r == PM_RES_OK) r = has_cap("UIDPLUS") ? cmd(0, 0, why, whymax, "UID EXPUNGE %u", uid)
		                                          : cmd(0, 0, why, whymax, "EXPUNGE");
		return r;
	}
	quote(dest, q, sizeof(q));
	if (has_cap("MOVE")) return cmd(0, 0, why, whymax, "UID MOVE %u %s", uid, q);
	r = cmd(0, 0, why, whymax, "UID COPY %u %s", uid, q);
	if (r == PM_RES_OK) r = cmd(0, 0, why, whymax, "UID STORE %u +FLAGS.SILENT (\\Deleted)", uid);
	if (r == PM_RES_OK && has_cap("UIDPLUS")) r = cmd(0, 0, why, whymax, "UID EXPUNGE %u", uid);
	return r;
}

int imap_expunge(int acct, const char *folder, char *why, int whymax)
{
	int r;
	if ((r = imap_open(acct, why, whymax)) != PM_RES_OK) return r;
	if ((r = select_folder(folder, 0, 0, why, whymax)) != PM_RES_OK) return r;
	return cmd(0, 0, why, whymax, "EXPUNGE");
}

/* ---------------------------------------------------------------- search */

typedef struct { unsigned int *u; int n, cap; } UidList;

static void on_search(ImapNode *r, void *ctx)
{
	UidList *l = (UidList *)ctx;
	ImapNode *c = r->child;
	if (!c || !ip_eq(c, "SEARCH")) return;
	for (c = c->next; c; c = c->next) {
		if (l->n >= l->cap) {
			int nc = l->cap ? l->cap * 2 : 128;
			unsigned int *n2 = (unsigned int *)realloc(l->u, nc * sizeof(unsigned int));
			if (!n2) return;
			l->u = n2; l->cap = nc;
		}
		l->u[l->n++] = (unsigned int)ip_num(c);
	}
}

int imap_search(int acct, const char *folder, const char *words, char *why, int whymax)
{
	static PmIndex ix;
	UidList l;
	SyncCtx sc;
	char utf[300], q[400], set[600];
	int r, i, k = 0, ascii = 1, from;

	if ((r = imap_open(acct, why, whymax)) != PM_RES_OK) return r;
	if ((r = select_folder(folder, 0, 0, why, whymax)) != PM_RES_OK) return r;
	cs_cp1252_to_utf8(words, (int)strlen(words), utf, sizeof(utf));
	for (i = 0; utf[i]; i++) if ((unsigned char)utf[i] >= 0x80) ascii = 0;
	memset(&l, 0, sizeof(l));
	pm_progress("Searching %s...", folder);
	if (ascii) {
		quote(utf, q, sizeof(q));
		r = cmd(on_search, &l, why, whymax, "UID SEARCH TEXT %s", q);
	} else if (has_cap("LITERAL+")) {
		r = cmd(on_search, &l, why, whymax, "UID SEARCH CHARSET UTF-8 TEXT {%d+}\r\n%s", (int)strlen(utf), utf);
	} else {
		for (i = 0; utf[i]; i++) if ((unsigned char)utf[i] >= 0x80) utf[i] = '*';
		quote(utf, q, sizeof(q));
		r = cmd(on_search, &l, why, whymax, "UID SEARCH TEXT %s", q);
	}
	if (r != PM_RES_OK) { free(l.u); return r; }
	memset(&ix, 0, sizeof(ix));
	if (l.n) {
		/* the newest 50 matches */
		qsort(l.u, l.n, sizeof(unsigned int), cmp_uint);
		from = l.n > 50 ? l.n - 50 : 0;
		for (i = from; i < l.n && k < (int)sizeof(set) - 12; i++)
			k += sprintf(set + k, "%s%u", k ? "," : "", l.u[i]);
		memset(&sc, 0, sizeof(sc));
		sc.ix = &ix;
		sc.total = l.n - from;
		r = cmd(on_fetch_msg, &sc, why, whymax, "UID FETCH %s " FETCH_ITEMS, set);
	}
	if (r == PM_RES_OK) {
		/* keep what we already know (downloaded text) from the folder's index */
		static PmIndex fx;
		if (st_index_load(acct, folder, "index.txt", &fx) == 0) {
			for (i = 0; i < ix.n; i++) {
				PmMsg *m = st_index_find(&fx, ix.m[i].uid);
				if (m && st_flag_has(m->flags, 'B')) st_flag_set(ix.m[i].flags, 'B', 1);
			}
			st_index_free(&fx);
		}
		if (st_index_save(acct, folder, "search.txt", &ix) != 0) {
			char d[160];
			st_folder_dir(acct, folder, d, sizeof(d));
			pm_write_why(why, whymax, "the search results", d);
			r = PM_RES_FAILED;
		} else if (l.n > 50) set_why(why, whymax, "%d found - showing the newest 50", l.n);
		else set_why(why, whymax, "%d found", l.n);
	}
	st_index_free(&ix);
	free(l.u);
	return r;
}

/* ---------------------------------------------------------------- append */

int imap_append(int acct, const char *folder, const char *path, const char *flags, char *why, int whymax)
{
	static char buf[1024];
	char q[160], tag[12];
	FILE *f;
	long size;
	int r;
	if ((r = imap_open(acct, why, whymax)) != PM_RES_OK) return r;
	if (!(f = fopen(path, "rb"))) { set_why(why, whymax, "Could not read %s", path); return PM_RES_FAILED; }
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	quote(folder, q, sizeof(q));
	sprintf(tag, "P%03d", ++g_tagno % 1000);
	if (has_cap("LITERAL+"))
		r = pmn_printf("%s APPEND %s (%s) {%ld+}\r\n", tag, q, flags, size);
	else {
		r = pmn_printf("%s APPEND %s (%s) {%ld}\r\n", tag, q, flags, size);
		if (r == 0) {
			int k = read_response();
			if (k < 0) { fclose(f); return lost(why, whymax, k); }
			if (g_resp[0] != '+') {
				fclose(f);
				g_resp[strcspn(g_resp, "\r\n")] = 0;
				set_why(why, whymax, "%.100s", g_resp);
				return PM_RES_FAILED;
			}
		}
	}
	if (r != 0) { fclose(f); return lost(why, whymax, -1); }
	{
		long sent = 0;
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
			if (pmn_write(buf, (int)n) != 0) { fclose(f); return lost(why, whymax, -1); }
			sent += (long)n;
			pm_progress("Saving a copy in %s... %d%%", folder, (int)(sent * 100 / (size ? size : 1)));
		}
	}
	fclose(f);
	if (pmn_write("\r\n", 2) != 0) return lost(why, whymax, -1);
	for (;;) {
		int k = read_response();
		if (k < 0) return lost(why, whymax, k);
		if (!strncmp(g_resp, tag, strlen(tag))) {
			char *s = g_resp + strlen(tag) + 1;
			s[strcspn(s, "\r\n")] = 0;
			if (!pm_strncasecmp(s, "OK", 2)) return PM_RES_OK;
			set_why(why, whymax, "%.100s", s);
			return PM_RES_FAILED;
		}
	}
}
