/* imapparse.c - turns one IMAP response into a tree of atoms, strings and
 * lists. Literals ({n}CRLF followed by n bytes) are read in place: the
 * tree points into the response buffer, nothing is copied. */
#include <string.h>
#include <stdlib.h>
#include "pm.h"

#define MAX_NODES 3000
static ImapNode g_nodes[MAX_NODES];
static int g_nn;

static ImapNode *new_node(int type)
{
	ImapNode *n;
	if (g_nn >= MAX_NODES) return 0;
	n = &g_nodes[g_nn++];
	memset(n, 0, sizeof(*n));
	n->type = type;
	return n;
}

static const char *g_p, *g_end;

static void skip_sp(void) { while (g_p < g_end && (*g_p == ' ' || *g_p == '\r' || *g_p == '\n')) g_p++; }

static ImapNode *parse_item(int depth);

static ImapNode *parse_list(int depth, char close)
{
	ImapNode *l = new_node(IT_LIST), *last = 0;
	if (!l) return 0;
	for (;;) {
		ImapNode *c;
		skip_sp();
		if (g_p >= g_end) break;
		if (*g_p == close) { g_p++; break; }
		c = parse_item(depth + 1);
		if (!c) break;
		if (last) last->next = c; else l->child = c;
		last = c;
	}
	return l;
}

static ImapNode *parse_item(int depth)
{
	ImapNode *n;
again:
	skip_sp();
	if (g_p >= g_end || depth > 40) return 0;
	if (*g_p == '(') { g_p++; return parse_list(depth, ')'); }
	if (*g_p == '"') {
		/* quoted string: backslash escapes are left in place (rare in the
		   parts we read); the node spans the raw text */
		const char *s = ++g_p;
		while (g_p < g_end && *g_p != '"') { if (*g_p == '\\' && g_p + 1 < g_end) g_p++; g_p++; }
		n = new_node(IT_STRING);
		if (!n) return 0;
		n->s = s; n->len = (int)(g_p - s);
		if (g_p < g_end) g_p++;
		return n;
	}
	if (*g_p == '{') {
		long len = strtol(g_p + 1, 0, 10);
		const char *q = memchr(g_p, '\n', g_end - g_p);
		if (!q) return 0;
		q++;
		/* (compared as lengths: q + len could wrap round for a huge {n}) */
		if (len < 0 || len > (long)(g_end - q)) len = g_end - q;
		n = new_node(IT_STRING);
		if (!n) return 0;
		n->s = q; n->len = (int)len;
		g_p = q + len;
		return n;
	}
	{
		/* atom - including things like BODY[HEADER.FIELDS (A B)]<0>, whose
		   brackets may hold spaces */
		const char *s = g_p;
		int br = 0;
		while (g_p < g_end) {
			char c = *g_p;
			if (c == '[') br++;
			else if (c == ']') { if (br) br--; }
			else if (!br && (c == ' ' || c == '(' || c == ')' || c == '\r' || c == '\n')) break;
			g_p++;
		}
		if (g_p == s) { g_p++; goto again; }   /* stray ')': a loop, not a recursion - a
		                                          line of them must not eat the stack */
		n = new_node(IT_ATOM);
		if (!n) return 0;
		n->s = s; n->len = (int)(g_p - s);
		if (n->len == 3 && (s[0] == 'N' || s[0] == 'n') && (s[1] == 'I' || s[1] == 'i') && (s[2] == 'L' || s[2] == 'l'))
			n->type = IT_NIL;
		return n;
	}
}

ImapNode *ip_parse(const char *buf, int len)
{
	g_nn = 0;
	g_p = buf; g_end = buf + len;
	return parse_list(0, 0);
}

int ip_eq(const ImapNode *n, const char *s)
{
	int l = (int)strlen(s);
	if (!n || (n->type != IT_ATOM && n->type != IT_STRING) || n->len != l) return 0;
	return pm_strncasecmp(n->s, s, l) == 0;
}

void ip_str(const ImapNode *n, char *out, int max)
{
	int i, k = 0;
	if (!n || (n->type != IT_ATOM && n->type != IT_STRING)) { out[0] = 0; return; }
	for (i = 0; i < n->len && k < max - 1; i++) {
		if (n->type == IT_STRING && n->s[i] == '\\' && i + 1 < n->len && (n->s[i + 1] == '"' || n->s[i + 1] == '\\')) i++;
		out[k++] = n->s[i];
	}
	out[k] = 0;
}

long ip_num(const ImapNode *n)
{
	char b[24];
	ip_str(n, b, sizeof(b));
	return strtoul(b, 0, 10);
}

ImapNode *ip_nth(ImapNode *list, int i)
{
	ImapNode *c;
	if (!list || list->type != IT_LIST) return 0;
	for (c = list->child; c && i > 0; c = c->next) i--;
	return c;
}

ImapNode *ip_get(ImapNode *list, const char *key)
{
	ImapNode *c;
	if (!list || list->type != IT_LIST) return 0;
	for (c = list->child; c && c->next; c = c->next->next)
		if (ip_eq(c, key)) return c->next;
	return 0;
}
