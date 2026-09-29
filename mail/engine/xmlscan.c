/* xmlscan.c - just enough XML for WebDAV replies (see cal.h)
 *
 * Fed in pieces as they arrive; calls back at the end of each element with
 * its local name (namespace prefix dropped) and its text. Good for the
 * multistatus documents CalDAV servers send; not a general XML parser
 * (no DTDs, attributes ignored).
 */
#include <string.h>
#include "pm.h"
#include "cal.h"

enum { S_TEXT, S_TAG, S_ENT, S_COMMENT, S_CDATA, S_SKIP };

void xs_init(XmlScan *x, char *textbuf, int textmax, XsEnd end, void *ctx)
{
	memset(x, 0, sizeof(*x));
	x->text = textbuf;
	x->max = textmax;
	x->end = end;
	x->ctx = ctx;
}

const char *xs_parent(XmlScan *x, int up)
{
	int i = x->depth - 1 - up;
	return i >= 0 && i < XS_DEPTH ? x->path[i] : "";
}

static void put(XmlScan *x, const char *s, int n)
{
	if (x->len + n >= x->max) { n = x->max - 1 - x->len; x->over = 1; }
	if (n > 0) { memcpy(x->text + x->len, s, n); x->len += n; }
}

static void put_ucs(XmlScan *x, unsigned long u)
{
	char b[4];
	int n;
	if (u < 0x80) { b[0] = (char)u; n = 1; }
	else if (u < 0x800) { b[0] = (char)(0xc0 | (u >> 6)); b[1] = (char)(0x80 | (u & 63)); n = 2; }
	else if (u < 0x10000) { b[0] = (char)(0xe0 | (u >> 12)); b[1] = (char)(0x80 | ((u >> 6) & 63)); b[2] = (char)(0x80 | (u & 63)); n = 3; }
	else { b[0] = (char)(0xf0 | (u >> 18)); b[1] = (char)(0x80 | ((u >> 12) & 63)); b[2] = (char)(0x80 | ((u >> 6) & 63)); b[3] = (char)(0x80 | (u & 63)); n = 4; }
	put(x, b, n);
}

static void entity(XmlScan *x)
{
	const char *e = x->ent;
	x->ent[x->elen] = 0;
	if (!strcmp(e, "lt")) put(x, "<", 1);
	else if (!strcmp(e, "gt")) put(x, ">", 1);
	else if (!strcmp(e, "amp")) put(x, "&", 1);
	else if (!strcmp(e, "quot")) put(x, "\"", 1);
	else if (!strcmp(e, "apos")) put(x, "'", 1);
	else if (e[0] == '#') {
		unsigned long u = 0;
		const char *p = e + 1;
		int hex = *p == 'x' || *p == 'X';
		if (hex) p++;
		for (; *p; p++) {
			int c = *p, v;
			if (c >= '0' && c <= '9') v = c - '0';
			else if (hex && (c | 32) >= 'a' && (c | 32) <= 'f') v = (c | 32) - 'a' + 10;
			else break;
			u = u * (hex ? 16 : 10) + v;
		}
		put_ucs(x, u);
	}
}

static const char *local(const char *name)
{
	const char *c = strchr(name, ':');
	return c ? c + 1 : name;
}

/* a complete tag in x->tag */
static void tag(XmlScan *x)
{
	char name[64];
	const char *t = x->tag;
	int close = 0, empty, n = 0;
	x->tag[x->tlen] = 0;
	if (t[0] == '?' || t[0] == '!') return;
	if (t[0] == '/') { close = 1; t++; }
	empty = x->last == '/';
	while (t[n] && t[n] != ' ' && t[n] != '\t' && t[n] != '\r' && t[n] != '\n' && t[n] != '/' && n < (int)sizeof(name) - 1) {
		name[n] = t[n];
		n++;
	}
	name[n] = 0;
	if (!close) {
		if (x->depth < XS_DEPTH) pm_copy(x->path[x->depth], local(name), sizeof(x->path[0]));
		x->depth++;
		x->len = 0;
		x->over = 0;
		if (!empty) return;
	}
	/* the end of an element */
	if (x->depth > 0) {
		x->text[x->len] = 0;
		if (x->end) x->end(x, local(name), x->text, x->len, x->ctx);
		x->depth--;
	}
	x->len = 0;
}

void xs_feed(XmlScan *x, const char *in, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		char c = in[i];
		switch (x->st) {
		case S_TEXT:
			if (c == '<') { x->st = S_TAG; x->tlen = 0; x->quote = 0; x->last = 0; }
			else if (c == '&') { x->st = S_ENT; x->elen = 0; }
			else put(x, &c, 1);
			break;
		case S_ENT:
			if (c == ';') { entity(x); x->st = S_TEXT; }
			else if (x->elen < (int)sizeof(x->ent) - 1) x->ent[x->elen++] = c;
			break;
		case S_TAG:
			/* attribute values are kept (xs callers may look at x->tag),
			   and a '>' inside quotes doesn't end the tag */
			if (x->quote) {
				if (c == x->quote) x->quote = 0;
				if (x->tlen < (int)sizeof(x->tag) - 1) x->tag[x->tlen++] = c;
				break;
			}
			if ((c == '"' || c == '\'') && x->tlen && x->tag[0] != '!') x->quote = c;
			else if (c == '>') { tag(x); x->st = S_TEXT; break; }
			x->last = c;
			if (x->tlen < (int)sizeof(x->tag) - 1) x->tag[x->tlen++] = c;
			if (x->tlen == 3 && !memcmp(x->tag, "!--", 3)) { x->st = S_COMMENT; x->tlen = 0; }
			else if (x->tlen == 8 && !memcmp(x->tag, "![CDATA[", 8)) { x->st = S_CDATA; x->tlen = 0; }
			break;
		case S_COMMENT:        /* until --> ; tlen counts the dashes */
			if (c == '-') x->tlen++;
			else if (c == '>' && x->tlen >= 2) x->st = S_TEXT;
			else x->tlen = 0;
			break;
		case S_CDATA:          /* until ]]> */
			if (c == ']') { x->tlen++; break; }
			if (c == '>' && x->tlen >= 2) {
				while (x->tlen > 2) { put(x, "]", 1); x->tlen--; }
				x->st = S_TEXT;
				break;
			}
			while (x->tlen > 0) { put(x, "]", 1); x->tlen--; }
			put(x, &c, 1);
			break;
		}
	}
}
