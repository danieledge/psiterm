/* smtp.c - sending mail (RFC 5321) with AUTH PLAIN / LOGIN, over TLS on
 * port 465 or STARTTLS on 587. The message is already in MIME form in a
 * file (compose.c); it is streamed out with dot-stuffing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "pm.h"

#define TIMEOUT 60000

static char g_line[600];
static char g_ext[400];        /* EHLO keywords */

static void set_why(char *why, int whymax, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(why, whymax, fmt, ap);
	va_end(ap);
}

/* Reads a (possibly multi-line) reply; returns its code, or <0 if the
   connection failed. The text of the last line stays in g_line. */
static int reply(int collect_ext)
{
	int code = 0;
	if (collect_ext) g_ext[0] = 0;
	for (;;) {
		int n = pmn_readline(g_line, sizeof(g_line), TIMEOUT);
		if (n < 0) return n;
		if (n < 3) continue;
		code = atoi(g_line);
		if (collect_ext && n > 4 && strlen(g_ext) + n < sizeof(g_ext) - 2) {
			strcat(g_ext, g_line + 4);
			strcat(g_ext, "\n");
		}
		if (g_line[3] != '-') return code;
	}
}

static int command(const char *fmt, ...)
{
	static char b[700];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b) - 2, fmt, ap);
	va_end(ap);
	if (n < 0 || n > (int)sizeof(b) - 3) return -1;
	b[n++] = '\r'; b[n++] = '\n';
	if (pmn_write(b, n) != 0) return -1;
	return reply(0);
}

static void b64(const unsigned char *in, int n, char *out)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i, k = 0;
	for (i = 0; i < n; i += 3) {
		unsigned int v = in[i] << 16;
		if (i + 1 < n) v |= in[i + 1] << 8;
		if (i + 2 < n) v |= in[i + 2];
		out[k++] = t[(v >> 18) & 63];
		out[k++] = t[(v >> 12) & 63];
		out[k++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
		out[k++] = i + 2 < n ? t[v & 63] : '=';
	}
	out[k] = 0;
}

static int ext_has(const char *kw)
{
	const char *p = g_ext;
	int n = (int)strlen(kw);
	while (*p) {
		if (!pm_strncasecmp(p, kw, n) && (p[n] == ' ' || p[n] == '\n' || p[n] == '=')) return 1;
		p = strchr(p, '\n');
		if (!p) break;
		p++;
	}
	return 0;
}

static int auth_has(const char *mech)
{
	const char *p = g_ext;
	while (*p) {
		if (!pm_strncasecmp(p, "AUTH", 4) && (p[4] == ' ' || p[4] == '=')) {
			const char *e = strchr(p, '\n');
			char l[200];
			int n = e ? (int)(e - p) : (int)strlen(p);
			if (n > 199) n = 199;
			memcpy(l, p, n); l[n] = 0;
			if (pm_stristr(l, mech)) return 1;
		}
		p = strchr(p, '\n');
		if (!p) break;
		p++;
	}
	return 0;
}

static int fail(char *why, int whymax, int r, const char *what)
{
	if (r == PMN_CANCEL) { set_why(why, whymax, "Stopped"); pmn_close(1); return PM_RES_CANCELLED; }
	if (r < 0) { set_why(why, whymax, "The connection was lost (%s)", what); pmn_close(1); return PM_RES_OFFLINE; }
	set_why(why, whymax, "%s: %.120s", what, g_line);
	command("QUIT");
	pmn_close(1);
	return PM_RES_FAILED;
}

/* rcpts: addresses separated by newlines */
int smtp_send(int acct, const char *mime_path, const char *from, const char *rcpts, char *why, int whymax)
{
	PmShared *s = pm_shared();
	PmAccount *a = &s->acct[acct];
	static char buf[1100];
	FILE *f;
	int r, bol = 1;
	long size, sent = 0;
	const char *p;

	if (s->offline) { set_why(why, whymax, "Working offline"); return PM_RES_OFFLINE; }
	if (!a->pass[0]) { set_why(why, whymax, "Password needed for %s", a->name); return PM_RES_NEED_PASS; }
	if (!(f = fopen(mime_path, "rb"))) { set_why(why, whymax, "Could not read the message"); return PM_RES_FAILED; }
	fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);

	if (pmn_connect(a->smtp_host, a->smtp_port, a->smtp_tls == PM_TLS_ON, why, whymax) != 0) {
		fclose(f);
		if (tlsv_problem()[0]) {
			snprintf(s->trust_host, sizeof(s->trust_host), "%s:%d", a->smtp_host, a->smtp_port);
			pm_copy(s->trust_why, tlsv_problem(), sizeof(s->trust_why));
			pm_copy(s->trust_fp, tlsv_fingerprint(), sizeof(s->trust_fp));
			return PM_RES_UNTRUSTED;
		}
		return pm_cancelled() ? PM_RES_CANCELLED : PM_RES_OFFLINE;
	}
	pm_progress("Sending: greeting %s...", a->smtp_host);
	if ((r = reply(0)) != 220) { fclose(f); return fail(why, whymax, r, "Server greeting"); }
	pmn_printf("EHLO psion\r\n");
	if ((r = reply(1)) != 250) { fclose(f); return fail(why, whymax, r, "EHLO"); }
	if (a->smtp_tls == PM_TLS_STARTTLS) {
		if ((r = command("STARTTLS")) != 220) { fclose(f); return fail(why, whymax, r, "STARTTLS"); }
		if (pmn_starttls(a->smtp_host, why, whymax) != 0) {
			fclose(f);
			pmn_close(1);
			if (tlsv_problem()[0]) {
				snprintf(s->trust_host, sizeof(s->trust_host), "%s:%d", a->smtp_host, a->smtp_port);
				pm_copy(s->trust_why, tlsv_problem(), sizeof(s->trust_why));
				pm_copy(s->trust_fp, tlsv_fingerprint(), sizeof(s->trust_fp));
				return PM_RES_UNTRUSTED;
			}
			return PM_RES_OFFLINE;
		}
		pmn_printf("EHLO psion\r\n");
		if ((r = reply(1)) != 250) { fclose(f); return fail(why, whymax, r, "EHLO"); }
	}
	pm_progress("Sending: logging in...");
	if (auth_has("PLAIN") || !auth_has("LOGIN")) {
		unsigned char raw[200];
		char enc[280];
		int ul = (int)strlen(a->user), pl = (int)strlen(a->pass);
		if (ul + pl + 2 > (int)sizeof(raw)) { fclose(f); set_why(why, whymax, "User name too long"); return PM_RES_FAILED; }
		raw[0] = 0; memcpy(raw + 1, a->user, ul); raw[1 + ul] = 0; memcpy(raw + 2 + ul, a->pass, pl);
		b64(raw, ul + pl + 2, enc);
		memset(raw, 0, sizeof(raw));
		r = command("AUTH PLAIN %s", enc);
		memset(enc, 0, sizeof(enc));
	} else {
		char enc[200];
		r = command("AUTH LOGIN");
		if (r == 334) { b64((const unsigned char *)a->user, (int)strlen(a->user), enc); r = command("%s", enc); }
		if (r == 334) { b64((const unsigned char *)a->pass, (int)strlen(a->pass), enc); r = command("%s", enc); memset(enc, 0, sizeof(enc)); }
	}
	if (r != 235) { fclose(f); r = fail(why, whymax, r, "Login"); return r == PM_RES_FAILED ? PM_RES_LOGIN_FAILED : r; }

	if (ext_has("SIZE")) r = command("MAIL FROM:<%s> SIZE=%ld", from, size);
	else r = command("MAIL FROM:<%s>", from);
	if (r != 250) { fclose(f); return fail(why, whymax, r, "Sender refused"); }
	for (p = rcpts; *p;) {
		char addr[200];
		int n = (int)strcspn(p, "\n");
		if (n > 199) n = 199;
		memcpy(addr, p, n); addr[n] = 0;
		p += n;
		if (*p) p++;
		if (!addr[0]) continue;
		r = command("RCPT TO:<%s>", addr);
		if (r != 250 && r != 251) {
			char what[240];
			snprintf(what, sizeof(what), "Recipient %s refused", addr);
			fclose(f);
			return fail(why, whymax, r, what);
		}
	}
	if ((r = command("DATA")) != 354) { fclose(f); return fail(why, whymax, r, "DATA"); }
	/* the message, with a '.' doubled at the start of any line */
	for (;;) {
		static char out[2300];
		int n = (int)fread(buf, 1, sizeof(buf), f), i, k = 0;
		if (n <= 0) break;
		for (i = 0; i < n; i++) {
			if (bol && buf[i] == '.') out[k++] = '.';
			out[k++] = buf[i];
			bol = buf[i] == '\n';
		}
		if (pmn_write(out, k) != 0) { fclose(f); return fail(why, whymax, -1, "sending"); }
		sent += n;
		pm_progress("Sending... %d%%", (int)(sent * 100 / (size ? size : 1)));
		if (pm_cancelled()) { fclose(f); return fail(why, whymax, PMN_CANCEL, "sending"); }
	}
	fclose(f);
	if (pmn_write(bol ? ".\r\n" : "\r\n.\r\n", bol ? 3 : 5) != 0) return fail(why, whymax, -1, "sending");
	if ((r = reply(0)) != 250) return fail(why, whymax, r, "Message refused");
	command("QUIT");
	pmn_close(0);
	return PM_RES_OK;
}
