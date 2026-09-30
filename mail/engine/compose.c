/* compose.c - turns a message written in PsiMail.app into MIME for sending
 *
 * The app saves outbox\<id>.txt in Windows-1252:
 *     #PSIMAIL1
 *     To: a@b.com, "Someone" <c@d.org>
 *     Cc: ...
 *     Bcc: ...
 *     Subject: ...
 *     In-Reply-To: <...>
 *     References: <...> <...>
 *     Attach: D:\Documents\notes.txt        (any number)
 *     Reply-Folder: INBOX                    (mark the original answered)
 *     Reply-Uid: 1234
 *     Format: rich                           (the text has bold/italic/underline)
 *     <blank line>
 *     the text...
 * and this writes <id>.eml: UTF-8, quoted-printable, attachments in base64.
 *
 * Rich text marks where styles change, as the app's editor had them:
 * \x11 bold on, \x12 bold off, \x13 italic on, \x14 italic off, \x18
 * underline on, \x19 underline off. It goes out as multipart/alternative:
 * the plain text (marks removed) and HTML.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "pm.h"

static void set_why(char *why, int whymax, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(why, whymax, fmt, ap);
	va_end(ap);
}

/* addresses from "a@b, Name <c@d>; e@f" -> "a@b\nc@d\ne@f\n" */
static int collect_addrs(const char *list, char *out, int max)
{
	const char *p = list;
	int k = (int)strlen(out), n = 0;
	while (*p) {
		char item[240];
		int i = 0, q = 0, ang = 0;
		while (*p && (q || ang || (*p != ',' && *p != ';'))) {
			if (*p == '"') q = !q;
			else if (*p == '<') ang = 1;
			else if (*p == '>') ang = 0;
			if (i < (int)sizeof(item) - 1) item[i++] = *p;
			p++;
		}
		item[i] = 0;
		if (*p) p++;
		{
			char *lt = strrchr(item, '<'), *at, addr[200];
			int j = 0;
			const char *s;
			if (lt) { s = lt + 1; while (*s && *s != '>' && j < 199) addr[j++] = *s++; }
			else {
				s = item;
				while (*s == ' ' || *s == '\t') s++;
				while (*s && *s != ' ' && *s != '\t' && j < 199) addr[j++] = *s++;
			}
			addr[j] = 0;
			at = strchr(addr, '@');
			if (!at) continue;
			if (k + j + 2 < max) { strcpy(out + k, addr); k += j; out[k++] = '\n'; out[k] = 0; n++; }
		}
	}
	return n;
}

/* a header line, folded and encoded if needed; addresses keep their <...> */
static void header_addr(FILE *f, const char *name, const char *list)
{
	/* encode only the display names, item by item */
	const char *p = list;
	int first = 1;
	fprintf(f, "%s:", name);
	while (*p) {
		char item[240], disp[240], enc[400];
		int i = 0, q = 0, ang = 0;
		while (*p == ' ' || *p == ',' || *p == ';') p++;
		while (*p && (q || ang || (*p != ',' && *p != ';'))) {
			if (*p == '"') q = !q;
			else if (*p == '<') ang = 1;
			else if (*p == '>') ang = 0;
			if (i < (int)sizeof(item) - 1) item[i++] = *p;
			p++;
		}
		while (i > 0 && item[i - 1] == ' ') i--;
		item[i] = 0;
		if (!item[0]) continue;
		{
			char *lt = strrchr(item, '<');
			if (lt && lt > item) {
				int dl = (int)(lt - item);
				memcpy(disp, item, dl); disp[dl] = 0;
				while (dl > 0 && disp[dl - 1] == ' ') disp[--dl] = 0;
				if (disp[0] == '"' && dl > 1 && disp[dl - 1] == '"') { disp[dl - 1] = 0; memmove(disp, disp + 1, dl); }
				cs_encode_header(disp, enc, sizeof(enc));
				if (!strcmp(enc, disp)) fprintf(f, "%s \"%s\" %s", first ? "" : ",\r\n", disp, lt);
				else fprintf(f, "%s %s %s", first ? "" : ",\r\n", enc, lt);
			} else fprintf(f, "%s %s", first ? "" : ",\r\n", item);
		}
		first = 0;
	}
	fprintf(f, "\r\n");
}

static void header_text(FILE *f, const char *name, const char *text)
{
	char enc[700];
	cs_encode_header(text, enc, sizeof(enc));
	fprintf(f, "%s: %s\r\n", name, enc);
}

static const char *mime_type(const char *name)
{
	static const struct { const char *ext, *type; } t[] = {
		{ ".txt", "text/plain" }, { ".htm", "text/html" }, { ".html", "text/html" },
		{ ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".png", "image/png" },
		{ ".gif", "image/gif" }, { ".bmp", "image/bmp" }, { ".pdf", "application/pdf" },
		{ ".zip", "application/zip" }, { ".csv", "text/csv" }, { ".rtf", "application/rtf" },
		{ ".doc", "application/msword" }, { ".xls", "application/vnd.ms-excel" },
		{ ".wav", "audio/wav" }, { ".sis", "application/vnd.symbian.install" },
		{ 0, 0 } };
	const char *dot = strrchr(name, '.');
	int i;
	if (dot) for (i = 0; t[i].ext; i++) if (!pm_strcasecmp(dot, t[i].ext)) return t[i].type;
	return "application/octet-stream";
}

/* quoted-printable UTF-8 body from cp1252 text; LF -> CRLF */
static void write_qp_buf(FILE *f, const char *s, int len)
{
	static const char hex[] = "0123456789ABCDEF";
	int col = 0, k;
	for (k = 0; k < len; k++) {
		char u[4];
		unsigned char ch = (unsigned char)s[k];
		int n, i, ws_at_end = 0;
		if (ch == '\r') continue;
		if (ch == '\n') { fputs("\r\n", f); col = 0; continue; }
		if (ch == ' ' || ch == '\t')
			/* a space or tab just before a line break must be encoded */
			ws_at_end = k + 1 >= len || s[k + 1] == '\n' || s[k + 1] == '\r';
		n = cs_cp1252_to_utf8((const char *)&ch, 1, u, sizeof(u));
		for (i = 0; i < n; i++) {
			unsigned char b = (unsigned char)u[i];
			int enc = b >= 0x80 || b == '=' || (b < 32 && b != '\t') || ws_at_end;
			int w = enc ? 3 : 1;
			if (col + w > 75) { fputs("=\r\n", f); col = 0; }
			if (enc) { fputc('=', f); fputc(hex[b >> 4], f); fputc(hex[b & 15], f); }
			else fputc(b, f);
			col += w;
		}
	}
	if (col) fputs("\r\n", f);
}

/* the style marks (see the top of this file) */
static int is_mark(unsigned char c)
{
	return (c >= 0x11 && c <= 0x14) || c == 0x18 || c == 0x19;
}

/* the text without its marks, in place */
static int strip_marks(char *s, int len)
{
	int i, j = 0;
	for (i = 0; i < len; i++)
		if (!is_mark((unsigned char)s[i])) s[j++] = s[i];
	return j;
}

/* HTML from the marked text: styles as properly nested tags, lines as <br> */
static char *to_html(const char *s, int len, int *outlen)
{
	int cap = len * 8 + 400, n = 0, i, b = 0, it = 0, u = 0, open = 0;
	char *o = (char *)malloc(cap);
	const char *head = "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\"></head>\n<body><div style=\"font-family: Arial, sans-serif\">";
	if (!o) return 0;
#define PUT(str) do { const char *q_ = (str); while (*q_ && n < cap - 1) o[n++] = *q_++; } while (0)
	PUT(head);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (is_mark(c)) {
			int nb = b, ni = it, nu = u;
			if (c == 0x11) nb = 1; else if (c == 0x12) nb = 0;
			else if (c == 0x13) ni = 1; else if (c == 0x14) ni = 0;
			else if (c == 0x18) nu = 1; else nu = 0;
			/* close what's open (innermost first), then open the new set */
			if (open) { if (u) PUT("</u>"); if (it) PUT("</i>"); if (b) PUT("</b>"); open = 0; }
			b = nb; it = ni; u = nu;
			continue;
		}
		if (!open && (b || it || u)) { if (b) PUT("<b>"); if (it) PUT("<i>"); if (u) PUT("<u>"); open = 1; }
		if (c == '\r') continue;
		if (c == '\n') {
			if (open) { if (u) PUT("</u>"); if (it) PUT("</i>"); if (b) PUT("</b>"); open = 0; }
			PUT("<br>\n");
			continue;
		}
		if (c == '<') PUT("&lt;");
		else if (c == '>') PUT("&gt;");
		else if (c == '&') PUT("&amp;");
		else if (c == '"') PUT("&quot;");
		else if (c == ' ' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\n')) PUT("&nbsp;");
		else if (n < cap - 1) o[n++] = (char)c;
	}
	if (open) { if (u) PUT("</u>"); if (it) PUT("</i>"); if (b) PUT("</b>"); }
	PUT("</div></body></html>\n");
#undef PUT
	o[n] = 0;
	*outlen = n;
	return o;
}

/* the rest of a file */
static char *read_rest(FILE *in, int *len)
{
	int cap = 4096, n = 0, c;
	char *b = (char *)malloc(cap);
	if (!b) return 0;
	while ((c = fgetc(in)) != EOF) {
		if (n >= cap - 1) {
			char *nb = (char *)realloc(b, cap * 2);
			if (!nb) { free(b); return 0; }
			b = nb; cap *= 2;
		}
		b[n++] = (char)c;
	}
	b[n] = 0;
	*len = n;
	return b;
}

static int write_b64_file(FILE *f, const char *path)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	unsigned char in[57];
	char out[80];
	FILE *a = fopen(path, "rb");
	size_t n;
	if (!a) return -1;
	while ((n = fread(in, 1, sizeof(in), a)) > 0) {
		size_t i;
		int k = 0;
		for (i = 0; i < n; i += 3) {
			unsigned int v = in[i] << 16;
			if (i + 1 < n) v |= in[i + 1] << 8;
			if (i + 2 < n) v |= in[i + 2];
			out[k++] = t[(v >> 18) & 63];
			out[k++] = t[(v >> 12) & 63];
			out[k++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
			out[k++] = i + 2 < n ? t[v & 63] : '=';
		}
		out[k++] = '\r'; out[k++] = '\n';
		fwrite(out, 1, k, f);
	}
	fclose(a);
	return 0;
}

static const char *k_days = "ThuFriSatSunMonTueWed";   /* 1 Jan 1970 was a Thursday */
static const char *k_mons = "JanFebMarAprMayJunJulAugSepOctNovDec";

static void rfc_date(long t, char *out)
{
	long days = t / 86400, secs = t % 86400;
	int y = 1970, m = 0, dim;
	static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int wd = (int)(days % 7);
	while (days >= ((y % 4 == 0) ? 366 : 365)) { days -= (y % 4 == 0) ? 366 : 365; y++; }
	for (m = 0; m < 12; m++) {
		dim = mdays[m] + (m == 1 && y % 4 == 0);
		if (days < dim) break;
		days -= dim;
	}
	sprintf(out, "%.3s, %d %.3s %d %02ld:%02ld:%02ld +0000", k_days + wd * 3, (int)days + 1, k_mons + m * 3, y,
		secs / 3600, (secs / 60) % 60, secs % 60);
}

int compose_mime(int acct, const char *outbox_path, const char *mime_path,
                 char *from_addr, int famax, char *rcpts, int rmax, char *why, int whymax)
{
	PmAccount *a = &pm_shared()->acct[acct];
	static char line[1100], to[1100], cc[1100], bcc[1100], subject[400], irt[200], refs[1000];
	static char attach[8][150];
	int natt = 0, i, rich = 0, blen = 0;
	char *body;
	FILE *in, *f;
	char date[40], boundary[48], msgid[140], fromh[200];
	const char *dom;
	unsigned char rnd[12];

	to[0] = cc[0] = bcc[0] = subject[0] = irt[0] = refs[0] = 0;
	if (!(in = fopen(outbox_path, "r"))) { set_why(why, whymax, "Could not read %s", outbox_path); return PM_RES_FAILED; }
	while (fgets(line, sizeof(line), in)) {
		char *v;
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0]) break;                 /* end of the headers */
		if (line[0] == '#') continue;
		v = strchr(line, ':');
		if (!v) continue;
		*v++ = 0;
		while (*v == ' ') v++;
		if (!pm_strcasecmp(line, "To")) pm_copy(to, v, sizeof(to));
		else if (!pm_strcasecmp(line, "Cc")) pm_copy(cc, v, sizeof(cc));
		else if (!pm_strcasecmp(line, "Bcc")) pm_copy(bcc, v, sizeof(bcc));
		else if (!pm_strcasecmp(line, "Subject")) pm_copy(subject, v, sizeof(subject));
		else if (!pm_strcasecmp(line, "In-Reply-To")) pm_copy(irt, v, sizeof(irt));
		else if (!pm_strcasecmp(line, "References")) pm_copy(refs, v, sizeof(refs));
		else if (!pm_strcasecmp(line, "Attach") && natt < 8 && *v) pm_copy(attach[natt++], v, sizeof(attach[0]));
		else if (!pm_strcasecmp(line, "Format")) rich = !pm_strcasecmp(v, "rich");
	}
	rcpts[0] = 0;
	if (collect_addrs(to, rcpts, rmax) + collect_addrs(cc, rcpts, rmax) + collect_addrs(bcc, rcpts, rmax) == 0) {
		fclose(in);
		set_why(why, whymax, "No one to send it to");
		return PM_RES_FAILED;
	}
	for (i = 0; i < natt; i++) {
		FILE *t = fopen(attach[i], "rb");
		if (!t) { fclose(in); set_why(why, whymax, "Attachment missing: %s", attach[i]); return PM_RES_FAILED; }
		fclose(t);
	}
	if (!(f = fopen(mime_path, "wb"))) { fclose(in); pm_write_why(why, whymax, "the message to send", mime_path); return PM_RES_FAILED; }

	pm_copy(from_addr, a->email, famax);
	dom = strchr(a->email, '@');
	dom = dom ? dom + 1 : "psion.invalid";
	genrandom(rnd, sizeof(rnd));
	sprintf(boundary, "=_psimail_%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
	snprintf(msgid, sizeof(msgid), "<%08lx.%02x%02x%02x%02x@%s>", (unsigned long)pm_time(), rnd[8], rnd[9], rnd[10], rnd[11], dom);
	rfc_date(pm_time(), date);

	fprintf(f, "Date: %s\r\n", date);
	if (a->fullname[0]) snprintf(fromh, sizeof(fromh), "%s <%s>", a->fullname, a->email);
	else pm_copy(fromh, a->email, sizeof(fromh));
	header_addr(f, "From", fromh);
	if (to[0]) header_addr(f, "To", to);
	if (cc[0]) header_addr(f, "Cc", cc);
	header_text(f, "Subject", subject);
	fprintf(f, "Message-ID: %s\r\n", msgid);
	if (irt[0]) fprintf(f, "In-Reply-To: %s\r\n", irt);
	if (refs[0]) fprintf(f, "References: %s\r\n", refs);
	fprintf(f, "MIME-Version: 1.0\r\nUser-Agent: PsiMail (Psion Series 5mx)\r\n");
	if (natt) {
		fprintf(f, "Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\n", boundary);
		fprintf(f, "This is a message in MIME format.\r\n\r\n--%s\r\n", boundary);
	}
	body = read_rest(in, &blen);
	fclose(in);
	if (!body) { fclose(f); remove(mime_path); set_why(why, whymax, "Not enough memory for the message"); return PM_RES_FAILED; }
	if (rich) {
		/* the plain text and HTML, as alternatives */
		int hlen = 0;
		char *html = to_html(body, blen, &hlen);
		char alt[56];
		sprintf(alt, "%s_alt", boundary);
		if (!html) { free(body); fclose(f); remove(mime_path); set_why(why, whymax, "Not enough memory for the message"); return PM_RES_FAILED; }
		blen = strip_marks(body, blen);
		fprintf(f, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n\r\n", alt);
		if (!natt) fprintf(f, "This is a message in MIME format.\r\n\r\n");
		fprintf(f, "--%s\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n", alt);
		write_qp_buf(f, body, blen);
		fprintf(f, "\r\n--%s\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n", alt);
		write_qp_buf(f, html, hlen);
		fprintf(f, "\r\n--%s--\r\n", alt);
		free(html);
	} else {
		fprintf(f, "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n");
		blen = strip_marks(body, blen);
		write_qp_buf(f, body, blen);
	}
	free(body);
	for (i = 0; i < natt; i++) {
		const char *base = attach[i], *s;
		char encname[300];
		for (s = attach[i]; *s; s++) if (*s == '\\' || *s == '/' || *s == ':') base = s + 1;
		pm_progress("Adding %s...", base);
		fprintf(f, "\r\n--%s\r\nContent-Type: %s\r\nContent-Transfer-Encoding: base64\r\n", boundary, mime_type(base));
		cs_encode_header(base, encname, sizeof(encname));
		if (!strcmp(encname, base))
			fprintf(f, "Content-Disposition: attachment; filename=\"%s\"\r\n\r\n", base);
		else {
			/* RFC 2231 */
			char utf[300];
			int n = cs_cp1252_to_utf8(base, (int)strlen(base), utf, sizeof(utf)), j;
			fprintf(f, "Content-Disposition: attachment; filename*=UTF-8''");
			for (j = 0; j < n; j++) {
				unsigned char c = (unsigned char)utf[j];
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
					fputc(c, f);
				else fprintf(f, "%%%02X", c);
			}
			fprintf(f, "\r\n\r\n");
		}
		if (write_b64_file(f, attach[i]) != 0) {
			fclose(f);
			remove(mime_path);
			set_why(why, whymax, "Could not read %s", attach[i]);
			return PM_RES_FAILED;
		}
	}
	if (natt) fprintf(f, "\r\n--%s--\r\n", boundary);
	if (pm_fclose(f) != 0) { remove(mime_path); pm_write_why(why, whymax, "the message to send", mime_path); return PM_RES_FAILED; }
	return PM_RES_OK;
}
