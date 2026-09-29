/* mime.c - base64 / quoted-printable decoding and the message structure
 * (IMAP BODYSTRUCTURE): which part is the text to show, which are
 * attachments. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "pm.h"

int mime_enc_from_name(const char *name)
{
	if (!pm_strcasecmp(name, "base64")) return ENC_BASE64;
	if (!pm_strcasecmp(name, "quoted-printable")) return ENC_QP;
	return ENC_7BIT;
}

void dec_init(PmDecoder *d, int enc)
{
	memset(d, 0, sizeof(*d));
	d->enc = enc;
}

static int b64v(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static int hexv(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/* Decodes a piece of the stream; state carries across calls, so the input
 * can be split anywhere. Output is never longer than the input. */
int dec_feed(PmDecoder *d, const char *in, int n, char *out)
{
	int i, k = 0;
	if (d->enc == ENC_BASE64) {
		for (i = 0; i < n; i++) {
			int v = b64v((unsigned char)in[i]);
			if (v < 0) continue;
			d->acc = (d->acc << 6) | v;
			d->bits += 6;
			if (d->bits >= 8) { d->bits -= 8; out[k++] = (char)(d->acc >> d->bits); }
		}
		return k;
	}
	if (d->enc == ENC_QP) {
		for (i = 0; i < n; i++) {
			char c = in[i];
			if (d->npend) {
				d->pend[d->npend++] = c;
				if (d->npend == 2 && (c == '\n')) { d->npend = 0; continue; }         /* "=\n" soft break */
				if (d->npend == 2 && (c == '\r')) continue;                           /* "=\r" wait for \n */
				if (d->npend == 3) {
					if (d->pend[1] == '\r' && c == '\n') { d->npend = 0; continue; }   /* "=\r\n" */
					if (hexv(d->pend[1]) >= 0 && hexv(c) >= 0)
						out[k++] = (char)(hexv(d->pend[1]) * 16 + hexv(c));
					else { out[k++] = '='; out[k++] = d->pend[1]; out[k++] = c; }
					d->npend = 0;
				}
				continue;
			}
			if (c == '=') { d->pend[0] = c; d->npend = 1; continue; }
			out[k++] = c;
		}
		return k;
	}
	memcpy(out, in, n);
	return n;
}

/* ------------------------------------------------------------ BODYSTRUCTURE */

static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

/* RFC 2231: charset'lang'%XX%YY */
static void decode_2231(const char *v, char *out, int max)
{
	const char *q1 = strchr(v, '\''), *q2 = q1 ? strchr(q1 + 1, '\'') : 0;
	char cs[32], raw[160];
	int k = 0;
	if (!q2) { pm_copy(out, v, max); return; }
	{
		int cl = (int)(q1 - v);
		if (cl > 31) cl = 31;
		memcpy(cs, v, cl); cs[cl] = 0;
	}
	for (v = q2 + 1; *v && k < (int)sizeof(raw) - 1; v++) {
		if (*v == '%' && hexv(v[1]) >= 0 && hexv(v[2]) >= 0) { raw[k++] = (char)(hexv(v[1]) * 16 + hexv(v[2])); v += 2; }
		else raw[k++] = *v;
	}
	cs_to_cp1252(cs, raw, k, out, max);
}

/* looks up a parameter in a ("key" "value" ...) list */
static int param(ImapNode *params, const char *key, char *out, int max)
{
	ImapNode *c;
	char k[40], v[200];
	char star[44];
	out[0] = 0;
	if (!params || params->type != IT_LIST) return 0;
	sprintf(star, "%s*", key);
	for (c = params->child; c && c->next; c = c->next->next) {
		ip_str(c, k, sizeof(k));
		ip_str(c->next, v, sizeof(v));
		if (!pm_strcasecmp(k, star)) { decode_2231(v, out, max); return 1; }
		if (!pm_strcasecmp(k, key)) { cs_decode_header(v, out, max); return 1; }
	}
	/* continuations: filename*0*, filename*0 ... joined (common with long names) */
	{
		char joined[200]; int jl = 0, enc = 0, i;
		joined[0] = 0;
		for (i = 0; i < 10; i++) {
			char k1[44], k2[44]; int found = 0;
			sprintf(k1, "%s*%d", key, i);
			sprintf(k2, "%s*%d*", key, i);
			for (c = params->child; c && c->next; c = c->next->next) {
				ip_str(c, k, sizeof(k));
				if (!pm_strcasecmp(k, k1) || !pm_strcasecmp(k, k2)) {
					if (!pm_strcasecmp(k, k2)) enc = 1;
					ip_str(c->next, v, sizeof(v));
					if (jl + (int)strlen(v) < (int)sizeof(joined) - 1) { strcpy(joined + jl, v); jl += (int)strlen(v); }
					found = 1;
					break;
				}
			}
			if (!found) break;
		}
		if (jl) {
			if (enc) decode_2231(joined, out, max); else cs_decode_header(joined, out, max);
			return 1;
		}
	}
	return 0;
}

static void add_part(PmStructure *st, ImapNode *b, const char *id)
{
	PmPart *p;
	char type[16], sub[24], enc[24], disp[24];
	ImapNode *dispn = 0;
	int is_text, is_msg;
	if (st->n >= PM_MAX_PARTS) return;
	p = &st->part[st->n];
	memset(p, 0, sizeof(*p));
	pm_copy(p->id, id, sizeof(p->id));
	ip_str(ip_nth(b, 0), type, sizeof(type)); lower(type);
	ip_str(ip_nth(b, 1), sub, sizeof(sub)); lower(sub);
	snprintf(p->type, sizeof(p->type), "%s/%s", type, sub);
	param(ip_nth(b, 2), "charset", p->charset, sizeof(p->charset));
	{
		char fl[16];
		param(ip_nth(b, 2), "format", fl, sizeof(fl));
		p->flowed = !pm_strcasecmp(fl, "flowed");
		param(ip_nth(b, 2), "delsp", fl, sizeof(fl));
		p->delsp = !pm_strcasecmp(fl, "yes");
	}
	ip_str(ip_nth(b, 5), enc, sizeof(enc));
	p->enc = mime_enc_from_name(enc);
	p->size = ip_num(ip_nth(b, 6));
	is_text = !strcmp(type, "text");
	is_msg = !strcmp(p->type, "message/rfc822");
	dispn = ip_nth(b, is_text ? 9 : is_msg ? 11 : 8);
	disp[0] = 0;
	if (dispn && dispn->type == IT_LIST) {
		ip_str(ip_nth(dispn, 0), disp, sizeof(disp));
		param(ip_nth(dispn, 1), "filename", p->name, sizeof(p->name));
	}
	if (!p->name[0]) param(ip_nth(b, 2), "name", p->name, sizeof(p->name));
	if (is_msg && !p->name[0]) strcpy(p->name, "message.eml");
	p->attachment = !pm_strcasecmp(disp, "attachment") || p->name[0] ||
		!(is_text && (!strcmp(sub, "plain") || !strcmp(sub, "html")));
	st->n++;
}

static void walk(PmStructure *st, ImapNode *b, const char *prefix, int depth)
{
	char id[16];
	if (!b || b->type != IT_LIST || depth > 8) return;
	if (b->child && b->child->type == IT_LIST) {
		/* multipart: (part)(part)... "subtype" ... */
		ImapNode *c; int i = 1;
		for (c = b->child; c && c->type == IT_LIST; c = c->next, i++) {
			if (prefix[0]) snprintf(id, sizeof(id), "%s.%d", prefix, i);
			else snprintf(id, sizeof(id), "%d", i);
			walk(st, c, id, depth + 1);
		}
		return;
	}
	add_part(st, b, prefix[0] ? prefix : "1");
}

void mime_structure(ImapNode *body, PmStructure *st)
{
	int i;
	memset(st, 0, sizeof(*st));
	st->text = -1;
	walk(st, body, "", 0);
	/* HTML when there is some: PsiMail shows its styles, lists, quotes and
	   links (the plain alternative is often an afterthought) */
	for (i = 0; i < st->n; i++)
		if (!st->part[i].attachment && !strcmp(st->part[i].type, "text/html")) { st->text = i; st->html = 1; break; }
	if (st->text < 0)
		for (i = 0; i < st->n; i++)
			if (!st->part[i].attachment && !strcmp(st->part[i].type, "text/plain")) { st->text = i; break; }
	for (i = 0; i < st->n; i++)
		if (i != st->text && st->part[i].attachment)
			st->nattach++;
}
