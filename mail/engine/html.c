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
 *
 * (0.75) Newsletters are mostly layout: nested tables, spacer cells and
 * pictures, linked logos, hidden "preheader" text. So that they don't read
 * as screens of empty space:
 *  - a line is only written once something visible is on it: an empty
 *    <p>, <div>, <td> or <tr>, a cell holding a non-breaking space, a link
 *    round nothing but a picture, all leave nothing behind;
 *  - blank lines come one at a time, never at the start or the end;
 *  - tables, rows and cells are blocks without blank lines between them;
 *  - what the sender hid (display:none, mso-hide:all, max-height:0,
 *    font-size:0, opacity:0, visibility:hidden, the hidden attribute) is
 *    left out, and so are spacer pictures (under 4 pixels either way, or
 *    spacer.gif and the like);
 *  - two <br> in a row make one blank line.
 * A picture's line carries its width and height when the HTML gives them
 * ("\x01i" alt "\x02" src "\x02" w "x" h), for the reader's layout.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "pm.h"

typedef void (*OutFn)(const char *s, int n, void *ctx);

#define TAG_MAX   600
#define PEND_MAX  240         /* codes waiting for a line's first visible character */
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
	/* (0.75) a line goes out only once something visible is on it */
	char pend[PEND_MAX];      /* its codes so far (prefix, styles, a link) */
	int pn;
	int visible;              /* this line has shown something: it goes straight out */
	int blank_pending;        /* a blank line, written before the next visible line */
	int hide_depth;           /* inside an element the sender hid: how deep */
	char hide_tag[16];
	};


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

/* Everything goes through put(): until the line has something visible on
   it, its codes wait in pend (and are dropped if nothing visible comes) */
static HtmlConv *g_cur;      /* (the converter out_s and friends write for) */

static void put(HtmlConv *h, const char *s, int n, OutFn out, void *ctx)
{
	if (h->visible) { out(s, n, ctx); return; }
	if (h->pn + n > PEND_MAX) {
		/* (a pathological run of codes: let them go as they are) */
		out(h->pend, h->pn, ctx);
		h->pn = 0;
		out(s, n, ctx);
		return;
	}
	memcpy(h->pend + h->pn, s, n);
	h->pn += n;
}

/* the line has something visible: the blank line before it, if one is
   owed, then its codes */
static void make_visible(HtmlConv *h, OutFn out, void *ctx)
{
	if (h->visible) return;
	if (h->blank_pending) { out("\n", 1, ctx); h->blank_pending = 0; }
	if (h->pn) out(h->pend, h->pn, ctx);
	h->pn = 0;
	h->visible = 1;
	h->blank = 0;
	h->started = 1;
}

static void out_s(OutFn out, void *ctx, const char *s) { put(g_cur, s, (int)strlen(s), out, ctx); }

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
		h->space_pending = 0;
	}
	if (c != ' ') make_visible(h, out, ctx);
	if (h->space_pending) put(h, " ", 1, out, ctx);
	h->space_pending = 0;
	put(h, &c, 1, out, ctx);
}

static void text_char(HtmlConv *h, char c, OutFn out, void *ctx)
{
	if ((unsigned char)c < 0x20 && c != '\n' && c != '\t' && c != '\r') c = ' ';
	if (h->hide_depth) return;
	if (h->pre) {
		if (c == '\r') return;
		if (c == '\n') {
			/* (an empty line in the text: one blank line, as elsewhere) */
			if (h->at_line_start || !h->visible) { h->pn = 0; h->at_line_start = 1; if (h->started && !h->blank) { h->blank_pending = 1; h->blank = 1; } return; }
			out("\n", 1, ctx);
			h->at_line_start = 1;
			h->visible = 0;
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

/* ends the current line (if anything visible is on it: else its codes are
   dropped); want = 2 also leaves a blank line, written only if something
   visible follows */
static void newline(HtmlConv *h, int want, OutFn out, void *ctx)
{
	h->space_pending = 0;
	if (!h->at_line_start) {
		if (h->visible) {
			if (h->in_link) out_s(out, ctx, "\x17");
			if (h->italic) out_s(out, ctx, "\x14");
			if (h->bold) out_s(out, ctx, "\x12");
			out("\n", 1, ctx);
		}
		h->at_line_start = 1;
		if (h->in_link) h->in_link = 0;     /* a link doesn't cross lines */
	}
	h->pn = 0;
	h->visible = 0;
	if (want == 2 && h->started && !h->blank) {
		h->blank_pending = 1;
		h->blank = 1;
	}
}

/* a line of its own that is visible as it is (a rule, a picture) */
static void whole_line(HtmlConv *h, const char *s, OutFn out, void *ctx)
{
	newline(h, 1, out, ctx);
	make_visible(h, out, ctx);
	out(s, (int)strlen(s), ctx);
	h->visible = 0;
	h->at_line_start = 1;
}

static void put_ucs(HtmlConv *h, unsigned int u, OutFn out, void *ctx)
{
	int c = cs_ucs_to_cp1252(u);
	if (h->hide_depth) return;
	if (u == 0xa0) { text_char(h, ' ', out, ctx); return; }
	if (c > 0) { emit_char(h, (char)c, out, ctx); return; }
	if (u == 0x200b || u == 0x200c || u == 0x200d || u == 0xfeff || u == 0x034f || u == 0xad ||
	    u == 0x2060 || u == 0x180e || u == 0x200e || u == 0x200f) return;
	if (u == 0x2010 || u == 0x2011 || u == 0x2212) { emit_char(h, '-', out, ctx); return; }
	if (u == 0x2192 || u == 0x2794 || u == 0x27a1) { emit_char(h, '-', out, ctx); emit_char(h, '>', out, ctx); return; }
	if (u == 0x2190) { emit_char(h, '<', out, ctx); emit_char(h, '-', out, ctx); return; }
	if ((u >= 0x2000 && u <= 0x200a) || u == 0x202f || u == 0x205f || u == 0x3000) { text_char(h, ' ', out, ctx); return; }
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
	else if (!h->hide_depth) {
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

/* "prop: 0" (0, 0px, 0em, 0%, 0.0) in a style attribute */
static int style_zero(const char *style, const char *prop)
{
	const char *p = style;
	int n = (int)strlen(prop);
	while ((p = pm_stristr(p, prop)) != 0) {
		const char *q = p + n;
		/* (whole property names only: "max-height" is not "height") */
		int whole = p == style || p[-1] == ';' || p[-1] == ' ' || p[-1] == '"' || p[-1] == '\t' || p[-1] == '\'';
		while (*q == ' ' || *q == '\t') q++;
		if (whole && *q == ':') {
			q++;
			while (*q == ' ' || *q == '\t') q++;
			if (*q == '0') {
				q++;
				if (*q == '.') { q++; while (*q == '0') q++; if (*q >= '1' && *q <= '9') { p = q; continue; } }
				if (!(*q >= '0' && *q <= '9')) return 1;
			}
		}
		p = q;
	}
	return 0;
}

/* did the sender hide this element? (preheaders, mobile-only copies) */
static int hidden_tag(const char *tag)
{
	char st[300];
	const char *t = tag;
	while (*t && *t != ' ' && *t != '\t' && *t != '\r' && *t != '\n') t++;
	/* the bare "hidden" attribute */
	{
		const char *p = t;
		while ((p = pm_stristr(p, "hidden")) != 0) {
			char b = p[-1], a = p[6];
			if ((b == ' ' || b == '\t' || b == '\n') && (a == 0 || a == ' ' || a == '>' || a == '/' || a == '=' || a == '\t'))
				return 1;
			p += 6;
		}
	}
	if (!attr(tag, "style", st, sizeof(st)) || !st[0]) return 0;
	{
		/* (spaces out, for "display : none") */
		char c[300];
		int i, k = 0;
		for (i = 0; st[i] && k < (int)sizeof(c) - 1; i++) if (st[i] != ' ' && st[i] != '\t') c[k++] = st[i];
		c[k] = 0;
		if (pm_stristr(c, "display:none") || pm_stristr(c, "mso-hide:all") || pm_stristr(c, "visibility:hidden"))
			return 1;
	}
	return style_zero(st, "max-height") || style_zero(st, "font-size") || style_zero(st, "opacity") ||
		(style_zero(st, "height") && pm_stristr(st, "overflow") && pm_stristr(st, "hidden")) ||
		(style_zero(st, "width") && pm_stristr(st, "overflow") && pm_stristr(st, "hidden"));
}

/* elements that never have an end tag (so can't hide what follows) */
static int is_void(const char *name)
{
	return is(name, "img") || is(name, "br") || is(name, "hr") || is(name, "meta") || is(name, "input") ||
		is(name, "link") || is(name, "area") || is(name, "base") || is(name, "col") || is(name, "wbr") ||
		is(name, "source") || is(name, "param") || is(name, "embed");
}

/* a size attribute in pixels ("600", "600px"; "100%" and the like: 0) */
static int px(const char *v)
{
	int n = 0, any = 0;
	while (*v == ' ') v++;
	while (*v >= '0' && *v <= '9') { n = n * 10 + (*v++ - '0'); any = 1; if (n > 100000) break; }
	if (*v == '%') return 0;
	return any ? n : -1;
}

/* the width or height a style gives ("width:300px"): -1 if none */
static int style_px(const char *style, const char *prop)
{
	const char *p = style;
	int n = (int)strlen(prop);
	while ((p = pm_stristr(p, prop)) != 0) {
		const char *q = p + n;
		int whole = p == style || p[-1] == ';' || p[-1] == ' ' || p[-1] == '\t';
		while (*q == ' ') q++;
		if (whole && *q == ':') {
			int v;
			q++;
			v = px(q);
			while (*q == ' ') q++;
			while (*q >= '0' && *q <= '9') q++;
			if (v >= 0 && (*q == 'p' || *q == 'P' || *q == ';' || !*q)) return v;
			return -1;
		}
		p = q;
	}
	return -1;
}

/* the entities an alt text usually has, decoded (Windows-1252, ASCII for
   numbers above 255) */
static void unent(char *s)
{
	char *r = s, *w = s;
	while (*r) {
		if (*r == '&') {
			static const struct { const char *e; char c; } k[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
				{ "&quot;", '"' }, { "&apos;", '\'' }, { "&nbsp;", ' ' }, { "&#39;", '\'' }, { 0, 0 } };
			int i, done = 0;
			for (i = 0; k[i].e; i++)
				if (!strncmp(r, k[i].e, strlen(k[i].e))) { *w++ = k[i].c; r += strlen(k[i].e); done = 1; break; }
			if (done) continue;
		}
		*w++ = *r++;
	}
	*w = 0;
}

/* "&amp;" in an address is "&" (the only entity addresses usually have) */
static void unamp(char *s)
{
	char *r = s, *w = s;
	while (*r) {
		if (*r == '&' && !pm_strncasecmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; }
		else if (*r == '&' && !strncmp(r, "&#38;", 5)) { *w++ = '&'; r += 5; }
		else *w++ = *r++;
	}
	*w = 0;
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
	h->tn = 0;

	if (!close && (is(name, "script") || is(name, "style") || is(name, "head") || is(name, "title"))) {
		strcpy(h->skip_until, name);
		h->state = S_SKIP;
		return;
	}
	/* inside what the sender hid: only its end matters */
	if (h->hide_depth) {
		if (is(name, h->hide_tag)) {
			int tl = (int)strlen(h->tag);
			int selfclose = tl > 0 && h->tag[tl - 1] == '/';
			if (close) h->hide_depth--;
			else if (!selfclose) h->hide_depth++;
		}
		return;
	}
	if (!close && name[0] && !is_void(name) && hidden_tag(h->tag)) {
		int n = (int)strlen(h->tag);
		if (n && h->tag[n - 1] == '/') return;     /* <div style="display:none"/>: nothing in it */
		pm_copy(h->hide_tag, name, sizeof(h->hide_tag));
		h->hide_depth = 1;
		return;
	}
	if (is(name, "br")) {
		/* a <br> on a line with nothing on it (a second <br>, Gmail's
		   <div><br></div>): a blank line - one at most, as ever */
		newline(h, h->at_line_start || !h->visible ? 2 : 1, out, ctx);
		return;
	}
	if (is(name, "p") || is(name, "form")) { newline(h, 2, out, ctx); return; }
	/* layout tables: blocks, no blank lines between them */
	if (is(name, "table") || is(name, "center") || is(name, "tbody") || is(name, "thead") || is(name, "tfoot")) { newline(h, 1, out, ctx); return; }
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
		whole_line(h, "\x01r\n", out, ctx);
		return;
	}
	if (is(name, "b") || is(name, "strong")) {
		if (close) { if (h->bold) { h->bold--; if (!h->bold && !h->at_line_start) out_s(out, ctx, "\x12"); } }
		else {
			if (!h->bold && !h->at_line_start) {
				if (h->space_pending) { put(h, " ", 1, out, ctx); h->space_pending = 0; }
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
				if (h->space_pending) { put(h, " ", 1, out, ctx); h->space_pending = 0; }
				out_s(out, ctx, "\x13");
			}
			h->italic++;
		}
		return;
	}
	if (is(name, "img") && !close) {
		/* "\x01i" alt "\x02" src ["\x02" w "x" h]: the app shows the picture
		   itself when it is a "cid:" part of the message (or one from the
		   web it has been asked to fetch: webpics.c), else the alt text */
		char alt[100], src[400], w[12], hgt[12], st[200], line[540];
		int k, wi, hi;
		attr(h->tag, "width", w, sizeof(w));
		attr(h->tag, "height", hgt, sizeof(hgt));
		attr(h->tag, "style", st, sizeof(st));
		wi = w[0] ? px(w) : -1;
		hi = hgt[0] ? px(hgt) : -1;
		if (wi < 0) wi = style_px(st, "width");
		if (hi < 0) hi = style_px(st, "height");
		if ((wi >= 0 && wi < 4) || (hi >= 0 && hi < 4)) return;     /* spacers, tracking pixels */
		if (st[0] && hidden_tag(h->tag)) return;
		attr(h->tag, "alt", alt, sizeof(alt));
		attr(h->tag, "src", src, sizeof(src));
		if (!src[0] && !alt[0]) return;
		if (!pm_strncasecmp(src, "data:", 5)) src[0] = 0;               /* (not decoded) */
		if (!src[0] && !alt[0]) return;
		unamp(src);
		unent(alt);
		if (pm_stristr(src, "spacer.gif") || pm_stristr(src, "/spacer.") || pm_stristr(src, "blank.gif") ||
		    pm_stristr(src, "clear.gif") || pm_stristr(src, "transparent.gif") || pm_stristr(src, "pixel.gif")) return;
		/* the text is HTML too: keep it simple */
		for (k = 0; alt[k]; k++) if ((unsigned char)alt[k] < 0x20) alt[k] = ' ';
		for (k = 0; src[k]; k++) if ((unsigned char)src[k] < 0x20) src[k] = ' ';
		if (wi > 0 || hi > 0)
			snprintf(line, sizeof(line), "\x01i%s\x02%s\x02%dx%d\n", alt, src, wi > 0 ? wi : 0, hi > 0 ? hi : 0);
		else
			snprintf(line, sizeof(line), "\x01i%s\x02%s\n", alt, src);
		whole_line(h, line, out, ctx);
		return;
	}
	if (is(name, "a")) {
		if (!close) {
			char href[LINK_MAX];
			if (h->in_link) { out_s(out, ctx, "\x17"); h->in_link = 0; }
			if (attr(h->tag, "href", href, sizeof(href)) && (unamp(href), 1) &&
			    (!pm_strncasecmp(href, "http", 4) || !pm_strncasecmp(href, "mailto:", 7)) &&
			    h->nlinks < MAX_LINKS) {
				char b[12];
				h->links[h->nlinks] = (char *)malloc(strlen(href) + 1);
				if (h->links[h->nlinks]) {
					strcpy(h->links[h->nlinks], href);
					h->nlinks++;
					if (h->at_line_start) { line_prefix(h, out, ctx); restyle(h, out, ctx); h->at_line_start = 0; }
					if (h->space_pending) { put(h, " ", 1, out, ctx); h->space_pending = 0; }
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
	g_cur = h;
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
	g_cur = h;
	if (h->state == S_ENT) flush_entity(h, out, ctx);
	newline(h, 1, out, ctx);
	h->blank_pending = 0;                /* (no blank lines at the end) */
	/* the links' addresses, for the app (not shown) */
	for (i = 0; i < h->nlinks; i++) {
		char n[16];
		sprintf(n, "\x01u%d ", i + 1);
		out(n, (int)strlen(n), ctx);
		out(h->links[i], (int)strlen(h->links[i]), ctx);
		out("\n", 1, ctx);
	}
}
