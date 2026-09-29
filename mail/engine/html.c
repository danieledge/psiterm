/* html.c - HTML mail to PsiMail's rich text, streamed a piece at a time
 *
 * The app lays the result out with real type (see ui/pmui.h for the
 * format): paragraphs, headings, bulleted and numbered lists, quotes,
 * preformatted text, rules, images (as their description), bold, italic
 * and numbered links (addresses listed at the end). Layout tables are
 * flattened into paragraphs - what reads best on a 640x240 screen; the
 * "View as web page" command shows the real thing with NetSurf.
 * <script>, <style>, <head> are dropped, entities decoded, spaces collapsed.
 * Input and output are Windows-1252.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "pm.h"

#define TAG_MAX   600
#define MAX_LINKS 99
#define LINK_MAX  200
#define MAX_DEPTH 8

enum { S_TEXT, S_TAG, S_ENT, S_COMMENT, S_SKIP };

struct HtmlConv
	{
	int state;
	char tag[TAG_MAX];
	int tn;
	char ent[12];
	int en;
	char skip_until[12];
	int space_pending;
	int at_line_start;       /* nothing written on this line yet */
	int blank;               /* a blank line has just been written */
	int started;
	int pre;
	int heading;             /* 1..3 while inside h1..h6 */
	int quote;               /* blockquote depth */
	int list_depth;
	int list_type[MAX_DEPTH];/* 0 ul, 1 ol */
	int list_count[MAX_DEPTH];
	int li_pending;          /* the next line starts a list item: show its marker */
	int bold, italic;        /* nesting counts */
	int in_link;
	int nlinks;
	char *links[MAX_LINKS];
	int dashes;
	};

typedef void (*OutFn)(const char *s, int n, void *ctx);

HtmlConv *html_new(void)
{
	HtmlConv *h = (HtmlConv *)calloc(1, sizeof(HtmlConv));
	if (h) { h->at_line_start = 1; h->blank = 1; }
	return h;
}

void html_free(HtmlConv *h)
{
	int i;
	if (!h) return;
	for (i = 0; i < h->nlinks; i++) free(h->links[i]);
	free(h);
}

static void out_s(OutFn out, void *ctx, const char *s) { out(s, (int)strlen(s), ctx); }

/* the block code that starts a line, from where we are */
static void line_prefix(HtmlConv *h, OutFn out, void *ctx)
{
	char b[24];
	if (h->pre) { out_s(out, ctx, "\x01" "c"); return; }
	if (h->heading) { sprintf(b, "\x01h%d", h->heading); out_s(out, ctx, b); return; }
	if (h->list_depth > 0) {
		int d = h->list_depth > 9 ? 9 : h->list_depth;
		if (h->li_pending) {
			int t = h->list_type[h->list_depth - 1];
			if (t == 1) sprintf(b, "\x01l%d%d.\x02", d, h->list_count[h->list_depth - 1]);
			else sprintf(b, "\x01l%d\x95\x02", d);
			h->li_pending = 0;
		} else sprintf(b, "\x01l%d\x02", d);
		out_s(out, ctx, b);
		return;
	}
	if (h->quote) { sprintf(b, "\x01q%d", h->quote > 9 ? 9 : h->quote); out_s(out, ctx, b); return; }
	out_s(out, ctx, "\x01p");
}

/* re-open styles on a new line (each line is laid out on its own) */
static void restyle(HtmlConv *h, OutFn out, void *ctx)
{
	if (h->bold) out_s(out, ctx, "\x11");
	if (h->italic) out_s(out, ctx, "\x13");
}

static void emit_char(HtmlConv *h, char c, OutFn out, void *ctx)
{
	if (h->at_line_start) {
		line_prefix(h, out, ctx);
		restyle(h, out, ctx);
		h->at_line_start = 0;
		h->blank = 0;
		h->space_pending = 0;
	}
	if (h->space_pending) out(" ", 1, ctx);
	h->space_pending = 0;
	out(&c, 1, ctx);
	h->started = 1;
}

static void text_char(HtmlConv *h, char c, OutFn out, void *ctx)
{
	if ((unsigned char)c < 0x20 && c != '\n' && c != '\t' && c != '\r') c = ' ';
	if (h->pre) {
		if (c == '\r') return;
		if (c == '\n') {
			if (h->at_line_start) { line_prefix(h, out, ctx); h->at_line_start = 0; }
			out("\n", 1, ctx);
			h->at_line_start = 1;
			return;
		}
		if (c == '\t') { int i; for (i = 0; i < 4; i++) emit_char(h, ' ', out, ctx); return; }
		if (c == ' ') { emit_char(h, ' ', out, ctx); return; }
		emit_char(h, c, out, ctx);
		return;
	}
	if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
		if (!h->at_line_start) h->space_pending = 1;
		return;
	}
	emit_char(h, c, out, ctx);
}

static void text_str(HtmlConv *h, const char *s, OutFn out, void *ctx)
{
	while (*s) text_char(h, *s++, out, ctx);
}

/* ends the current line; want = 2 also leaves a blank line */
static void newline(HtmlConv *h, int want, OutFn out, void *ctx)
{
	h->space_pending = 0;
	if (!h->at_line_start) {
		if (h->in_link) out_s(out, ctx, "\x17");
		if (h->italic) out_s(out, ctx, "\x14");
		if (h->bold) out_s(out, ctx, "\x12");
		out("\n", 1, ctx);
		h->at_line_start = 1;
		if (h->in_link) h->in_link = 0;     /* a link doesn't cross lines */
	}
	if (want == 2 && h->started && !h->blank) {
		out("\n", 1, ctx);
		h->blank = 1;
	}
}

static void put_ucs(HtmlConv *h, unsigned int u, OutFn out, void *ctx)
{
	int c = cs_ucs_to_cp1252(u);
	if (u == 0xa0) { text_char(h, ' ', out, ctx); return; }
	if (c > 0) { emit_char(h, (char)c, out, ctx); return; }
	if (u == 0x200b || u == 0x200c || u == 0x200d || u == 0xfeff || u == 0x034f || u == 0xad) return;
	if (u == 0x2010 || u == 0x2011 || u == 0x2212) { emit_char(h, '-', out, ctx); return; }
	if (u == 0x2002 || u == 0x2003 || u == 0x2009) { text_char(h, ' ', out, ctx); return; }
	if (u >= 0x1f000 || (u >= 0x2600 && u < 0x2800)) return;
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
		if (p > tag && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r' || p[-1] == '"' || p[-1] == '\'')) {
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

static int is(const char *name, const char *a) { return !strcmp(name, a); }

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
	h->tn = 0;

	if (!close && (is(name, "script") || is(name, "style") || is(name, "head") || is(name, "title"))) {
		strcpy(h->skip_until, name);
		h->state = S_SKIP;
		return;
	}
	if (is(name, "br")) { newline(h, 1, out, ctx); return; }
	if (is(name, "p") || is(name, "table") || is(name, "center") || is(name, "form")) { newline(h, 2, out, ctx); return; }
	if (is(name, "div") || is(name, "tr") || is(name, "section") || is(name, "article") || is(name, "header") ||
	    is(name, "footer") || is(name, "dt") || is(name, "dd") || is(name, "address") || is(name, "td") ||
	    is(name, "th") || is(name, "caption")) {
		/* table cells: each becomes its own line (layout tables read best so) */
		newline(h, 1, out, ctx);
		return;
	}
	if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
		newline(h, 2, out, ctx);
		h->heading = close ? 0 : (name[1] == '1' ? 1 : name[1] == '2' ? 2 : 3);
		return;
	}
	if (is(name, "blockquote")) {
		newline(h, 2, out, ctx);
		if (close) { if (h->quote) h->quote--; }
		else h->quote++;
		return;
	}
	if (is(name, "pre")) {
		newline(h, 2, out, ctx);
		h->pre = !close;
		return;
	}
	if (is(name, "ul") || is(name, "ol")) {
		if (h->list_depth == 0) newline(h, 2, out, ctx); else newline(h, 1, out, ctx);
		if (close) { if (h->list_depth) h->list_depth--; }
		else if (h->list_depth < MAX_DEPTH) {
			h->list_type[h->list_depth] = name[0] == 'o';
			h->list_count[h->list_depth] = 0;
			h->list_depth++;
		}
		return;
	}
	if (is(name, "li")) {
		newline(h, 1, out, ctx);
		if (!close) {
			if (h->list_depth == 0) { h->list_depth = 1; h->list_type[0] = 0; h->list_count[0] = 0; }
			h->list_count[h->list_depth - 1]++;
			h->li_pending = 1;
		}
		return;
	}
	if (is(name, "hr")) {
		newline(h, 1, out, ctx);
		out_s(out, ctx, "\x01r\n");
		h->blank = 0;
		return;
	}
	if (is(name, "b") || is(name, "strong")) {
		if (close) { if (h->bold) { h->bold--; if (!h->bold && !h->at_line_start) out_s(out, ctx, "\x12"); } }
		else {
			if (!h->bold && !h->at_line_start) {
				if (h->space_pending) { out(" ", 1, ctx); h->space_pending = 0; }
				out_s(out, ctx, "\x11");
			}
			h->bold++;
		}
		return;
	}
	if (is(name, "i") || is(name, "em") || is(name, "cite")) {
		if (close) { if (h->italic) { h->italic--; if (!h->italic && !h->at_line_start) out_s(out, ctx, "\x14"); } }
		else {
			if (!h->italic && !h->at_line_start) {
				if (h->space_pending) { out(" ", 1, ctx); h->space_pending = 0; }
				out_s(out, ctx, "\x13");
			}
			h->italic++;
		}
		return;
	}
	if (is(name, "img") && !close) {
		char alt[100], w[12], hgt[12];
		attr(h->tag, "width", w, sizeof(w));
		attr(h->tag, "height", hgt, sizeof(hgt));
		if ((w[0] && atoi(w) <= 2) || (hgt[0] && atoi(hgt) <= 2)) return;     /* tracking pixels */
		if (!attr(h->tag, "alt", alt, sizeof(alt)) || !alt[0]) return;
		newline(h, 1, out, ctx);
		out_s(out, ctx, "\x01i ");
		{
			/* alt text is HTML too: keep it simple */
			int k;
			for (k = 0; alt[k]; k++) if ((unsigned char)alt[k] < 0x20) alt[k] = ' ';
		}
		out_s(out, ctx, alt);
		out_s(out, ctx, "\n");
		h->at_line_start = 1;
		h->blank = 0;
		return;
	}
	if (is(name, "a")) {
		if (!close) {
			char href[LINK_MAX];
			if (h->in_link) { out_s(out, ctx, "\x17"); h->in_link = 0; }
			if (attr(h->tag, "href", href, sizeof(href)) &&
			    (!pm_strncasecmp(href, "http", 4) || !pm_strncasecmp(href, "mailto:", 7)) &&
			    h->nlinks < MAX_LINKS) {
				char b[12];
				h->links[h->nlinks] = (char *)malloc(strlen(href) + 1);
				if (h->links[h->nlinks]) {
					strcpy(h->links[h->nlinks], href);
					h->nlinks++;
					if (h->at_line_start) { line_prefix(h, out, ctx); restyle(h, out, ctx); h->at_line_start = 0; h->blank = 0; }
					if (h->space_pending) { out(" ", 1, ctx); h->space_pending = 0; }
					sprintf(b, "\x15%d\x16", h->nlinks);
					out_s(out, ctx, b);
					h->in_link = 1;
				}
			}
		} else if (h->in_link) {
			out_s(out, ctx, "\x17");
			h->in_link = 0;
		}
		return;
	}
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
				flush_entity(h, out, ctx);
				h->state = S_TEXT;
				i--;
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
	newline(h, 1, out, ctx);
	/* the links' addresses, for the app (not shown) */
	for (i = 0; i < h->nlinks; i++) {
		char n[16];
		sprintf(n, "\x01u%d ", i + 1);
		out_s(out, ctx, n);
		out_s(out, ctx, h->links[i]);
		out("\n", 1, ctx);
	}
}
