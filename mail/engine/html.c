/* html.c - HTML mail to plain text, streamed a piece at a time
 *
 * Enough to read typical HTML-only mail on a 640x240 screen: block tags
 * become line breaks, lists get "* ", links are numbered ("text[3]") and
 * listed at the end, images show their alt text, <script>, <style> and
 * <head> are dropped, entities are decoded, spaces are collapsed.
 * Input and output are Windows-1252.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "pm.h"

#define TAG_MAX   400
#define MAX_LINKS 40
#define LINK_MAX  160

enum { S_TEXT, S_TAG, S_ENT, S_COMMENT, S_SKIP };

struct HtmlConv
	{
	int state;
	char tag[TAG_MAX];
	int tn;
	char ent[12];
	int en;
	char skip_until[12];     /* "script", "style", "head", "title" */
	int space_pending;
	int nl;                  /* newlines just written (0 = mid-line) */
	int started;
	int pre;
	char href[LINK_MAX];
	int in_link;
	int nlinks;
	char *links[MAX_LINKS];
	int dashes;              /* for <!-- ... --> */
	};

typedef void (*OutFn)(const char *s, int n, void *ctx);

HtmlConv *html_new(void)
{
	HtmlConv *h = (HtmlConv *)calloc(1, sizeof(HtmlConv));
	if (h) h->nl = 2;
	return h;
}

void html_free(HtmlConv *h)
{
	int i;
	if (!h) return;
	for (i = 0; i < h->nlinks; i++) free(h->links[i]);
	free(h);
}

static void emit_char(HtmlConv *h, char c, OutFn out, void *ctx)
{
	if (h->space_pending && h->nl == 0 && c != '\n') out(" ", 1, ctx);
	h->space_pending = 0;
	out(&c, 1, ctx);
	h->nl = 0;
	h->started = 1;
}

static void text_char(HtmlConv *h, char c, OutFn out, void *ctx)
{
	if (!h->pre && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
		if (h->started) h->space_pending = 1;
		return;
	}
	if (h->pre && c == '\r') return;
	if (h->pre && c == '\n') { out("\n", 1, ctx); h->nl++; h->space_pending = 0; return; }
	emit_char(h, c, out, ctx);
}

static void text_str(HtmlConv *h, const char *s, OutFn out, void *ctx)
{
	while (*s) text_char(h, *s++, out, ctx);
}

static void newline(HtmlConv *h, int want, OutFn out, void *ctx)
{
	/* want = 1: end the line; 2: leave a blank line */
	h->space_pending = 0;
	if (!h->started) return;
	while (h->nl < want) { out("\n", 1, ctx); h->nl++; }
}

static void put_ucs(HtmlConv *h, unsigned int u, OutFn out, void *ctx)
{
	int c = cs_ucs_to_cp1252(u);
	if (u == 0xa0) { text_char(h, ' ', out, ctx); return; }
	if (c > 0) { emit_char(h, (char)c, out, ctx); return; }
	if (u == 0x200b || u == 0x200c || u == 0x200d || u == 0xfeff || u == 0x034f || u == 0xad) return;
	if (u == 0x2010 || u == 0x2011 || u == 0x2212) { emit_char(h, '-', out, ctx); return; }
	if (u == 0x2002 || u == 0x2003 || u == 0x2009) { text_char(h, ' ', out, ctx); return; }
	if (u >= 0x1f000) return;
	emit_char(h, '?', out, ctx);
}

static const struct { const char *name; unsigned short u; } k_ents[] = {
	{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
	{ "nbsp", 0xa0 }, { "copy", 0xa9 }, { "reg", 0xae }, { "trade", 0x2122 },
	{ "hellip", 0x2026 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
	{ "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201c }, { "rdquo", 0x201d },
	{ "bull", 0x2022 }, { "middot", 0xb7 }, { "euro", 0x20ac }, { "pound", 0xa3 },
	{ "yen", 0xa5 }, { "cent", 0xa2 }, { "deg", 0xb0 }, { "times", 0xd7 }, { "divide", 0xf7 },
	{ "laquo", 0xab }, { "raquo", 0xbb }, { "shy", 0xad }, { "zwnj", 0x200c }, { "zwj", 0x200d },
	{ "eacute", 0xe9 }, { "egrave", 0xe8 }, { "aacute", 0xe1 }, { "agrave", 0xe0 },
	{ "uuml", 0xfc }, { "ouml", 0xf6 }, { "auml", 0xe4 }, { "szlig", 0xdf }, { "ccedil", 0xe7 },
	{ "Uuml", 0xdc }, { "Ouml", 0xd6 }, { "Auml", 0xc4 }, { "ntilde", 0xf1 }, { "iacute", 0xed },
	{ "oacute", 0xf3 }, { "uacute", 0xfa }, { "ecirc", 0xea }, { "sect", 0xa7 }, { "para", 0xb6 },
	{ "frac12", 0xbd }, { "frac14", 0xbc }, { "plusmn", 0xb1 }, { "iexcl", 0xa1 }, { "iquest", 0xbf },
	{ "thinsp", 0x2009 }, { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "rarr", 0x2192 },
	{ 0, 0 }
};

static void flush_entity(HtmlConv *h, OutFn out, void *ctx)
{
	unsigned int u = 0;
	int i, ok = 0;
	h->ent[h->en] = 0;
	if (h->ent[0] == '#') {
		if (h->ent[1] == 'x' || h->ent[1] == 'X') u = (unsigned int)strtoul(h->ent + 2, 0, 16);
		else u = (unsigned int)strtoul(h->ent + 1, 0, 10);
		ok = u > 0;
		/* numeric references in 128..159 mean the cp1252 character */
		if (u >= 0x80 && u < 0xa0) u = cs_cp1252_to_ucs((unsigned char)u);
	} else {
		for (i = 0; k_ents[i].name; i++)
			if (!strcmp(k_ents[i].name, h->ent)) { u = k_ents[i].u; ok = 1; break; }
	}
	if (ok) put_ucs(h, u, out, ctx);
	else {
		emit_char(h, '&', out, ctx);
		for (i = 0; i < h->en; i++) emit_char(h, h->ent[i], out, ctx);
	}
	h->en = 0;
}

/* attribute value from the tag text */
static int attr(const char *tag, const char *name, char *out, int max)
{
	int nl = (int)strlen(name);
	const char *p = tag;
	out[0] = 0;
	while ((p = pm_stristr(p, name)) != 0) {
		const char *q = p + nl;
		if (p > tag && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r')) {
			while (*q == ' ') q++;
			if (*q == '=') {
				int k = 0;
				char quote = 0;
				q++;
				while (*q == ' ') q++;
				if (*q == '"' || *q == '\'') quote = *q++;
				while (*q && k < max - 1 && (quote ? *q != quote : (*q != ' ' && *q != '>'))) {
					if (*q != '\r' && *q != '\n') out[k++] = *q;
					q++;
				}
				out[k] = 0;
				return 1;
			}
		}
		p = q;
	}
	return 0;
}

static void handle_tag(HtmlConv *h, OutFn out, void *ctx)
{
	char name[16];
	int i = 0, close = 0;
	const char *t = h->tag;
	h->tag[h->tn] = 0;
	if (*t == '/') { close = 1; t++; }
	while (*t && *t != ' ' && *t != '>' && *t != '/' && *t != '\t' && *t != '\r' && *t != '\n' && i < 15) {
		char c = *t++;
		if (c >= 'A' && c <= 'Z') c += 32;
		name[i++] = c;
	}
	name[i] = 0;

	if (!close && (!strcmp(name, "script") || !strcmp(name, "style") || !strcmp(name, "head") || !strcmp(name, "title"))) {
		strcpy(h->skip_until, name);
		h->state = S_SKIP;
		h->tn = 0;
		return;
	}
	if (!strcmp(name, "br")) { out("\n", 1, ctx); h->nl++; h->space_pending = 0; h->started = 1; }
	else if (!strcmp(name, "p") || !strcmp(name, "h1") || !strcmp(name, "h2") || !strcmp(name, "h3") ||
	         !strcmp(name, "h4") || !strcmp(name, "h5") || !strcmp(name, "h6") || !strcmp(name, "blockquote") ||
	         !strcmp(name, "table") || !strcmp(name, "ul") || !strcmp(name, "ol"))
		newline(h, 2, out, ctx);
	else if (!strcmp(name, "div") || !strcmp(name, "tr") || !strcmp(name, "section") || !strcmp(name, "article") ||
	         !strcmp(name, "header") || !strcmp(name, "footer") || !strcmp(name, "center") || !strcmp(name, "dt") ||
	         !strcmp(name, "dd") || !strcmp(name, "address") || !strcmp(name, "form"))
		newline(h, 1, out, ctx);
	else if (!strcmp(name, "pre")) { newline(h, 1, out, ctx); h->pre = !close; }
	else if (!strcmp(name, "li") && !close) { newline(h, 1, out, ctx); text_str(h, "* ", out, ctx); }
	else if (!strcmp(name, "hr")) { newline(h, 1, out, ctx); text_str(h, "--------", out, ctx); newline(h, 1, out, ctx); }
	else if ((!strcmp(name, "td") || !strcmp(name, "th")) && close) h->space_pending = 1;
	else if (!strcmp(name, "img") && !close) {
		char alt[80];
		if (attr(h->tag, "alt", alt, sizeof(alt)) && alt[0]) {
			text_char(h, '[', out, ctx);
			for (i = 0; alt[i]; i++) text_char(h, alt[i], out, ctx);
			text_char(h, ']', out, ctx);
		}
	} else if (!strcmp(name, "a")) {
		if (!close) {
			h->in_link = attr(h->tag, "href", h->href, sizeof(h->href)) &&
				(!pm_strncasecmp(h->href, "http", 4) || !pm_strncasecmp(h->href, "mailto:", 7));
		} else if (h->in_link) {
			h->in_link = 0;
			if (h->nlinks < MAX_LINKS) {
				char n[8];
				h->links[h->nlinks] = (char *)malloc(strlen(h->href) + 1);
				if (h->links[h->nlinks]) {
					strcpy(h->links[h->nlinks], h->href);
					h->nlinks++;
					sprintf(n, "[%d]", h->nlinks);
					h->space_pending = 0;
					out(n, (int)strlen(n), ctx);
				}
			}
		}
	}
	h->tn = 0;
}

void html_feed(HtmlConv *h, const char *in, int n, OutFn out, void *ctx)
{
	int i;
	for (i = 0; i < n; i++) {
		char c = in[i];
		switch (h->state) {
		case S_TEXT:
			if (c == '<') { h->state = S_TAG; h->tn = 0; }
			else if (c == '&') { h->state = S_ENT; h->en = 0; }
			else text_char(h, c, out, ctx);
			break;
		case S_ENT:
			if (c == ';') { flush_entity(h, out, ctx); h->state = S_TEXT; }
			else if (h->en < (int)sizeof(h->ent) - 2 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#'))
				h->ent[h->en++] = c;
			else {
				flush_entity(h, out, ctx);           /* "&foo " - not an entity after all */
				h->state = S_TEXT;
				i--;                                  /* look at c again */
			}
			break;
		case S_TAG:
			if (h->tn == 3 && !strncmp(h->tag, "!--", 3)) { h->state = S_COMMENT; h->dashes = 0; i--; break; }
			if (c == '>') { handle_tag(h, out, ctx); if (h->state == S_TAG) h->state = S_TEXT; }
			else if (h->tn < TAG_MAX - 1) h->tag[h->tn++] = c;
			break;
		case S_COMMENT:
			if (c == '>' && h->dashes >= 2) h->state = S_TEXT;
			h->dashes = (c == '-') ? h->dashes + 1 : 0;
			break;
		case S_SKIP:
			/* wait for </name> */
			if (c == '<') { h->tn = 0; h->tag[h->tn++] = c; }
			else if (h->tn > 0 && h->tn < TAG_MAX - 1) {
				h->tag[h->tn++] = c;
				if (c == '>') {
					char want[20];
					h->tag[h->tn] = 0;
					sprintf(want, "</%s", h->skip_until);
					if (!pm_strncasecmp(h->tag, want, (int)strlen(want))) h->state = S_TEXT;
					h->tn = 0;
				}
			}
			break;
		}
	}
}

void html_end(HtmlConv *h, OutFn out, void *ctx)
{
	int i;
	if (h->state == S_ENT) flush_entity(h, out, ctx);
	if (h->nlinks) {
		newline(h, 2, out, ctx);
		out("Links:\n", 7, ctx);
		for (i = 0; i < h->nlinks; i++) {
			char n[12];
			sprintf(n, "[%d] ", i + 1);
			out(n, (int)strlen(n), ctx);
			out(h->links[i], (int)strlen(h->links[i]), ctx);
			out("\n", 1, ctx);
		}
	}
}
