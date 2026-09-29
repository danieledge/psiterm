/* pmdoc.cpp - lays out a message for reading: the header (subject, sender
 * with an initials badge, date, recipients), the text with its styles,
 * quotes, lists and links, and the attachments. The result is a list of
 * drawing operations over the message's own text (see pmui.h). */
#include "pmui.h"
#include "pmfonts.h"

/* ------------------------------------------------------------ helpers */

static int grow(PmDoc* d)
	{
	if (d->nops < d->cap)
		return 1;
	int nc = d->cap ? d->cap * 2 : 256;
	PmDocOp* n = (PmDocOp*)ui_alloc(nc * (int)sizeof(PmDocOp));
	if (!n)
		return 0;
	for (int i = 0; i < d->nops; i++) n[i] = d->ops[i];
	ui_free(d->ops);
	d->ops = n;
	d->cap = nc;
	return 1;
	}

static PmDocOp* op(PmDoc* d, int type, int x, int y, int w, int h, int grey)
	{
	if (!grow(d))
		return 0;
	PmDocOp* o = &d->ops[d->nops++];
	o->type = (unsigned char)type;
	o->font = 0;
	o->grey = (unsigned char)grey;
	o->extra = 0;
	o->x = (short)x; o->y = y; o->w = (short)w; o->h = (short)h;
	o->off = 0; o->len = 0; o->link = 0;
	return o;
	}

static void text_op(PmDoc* d, int font, int x, int base, int off, int len, int grey, int link)
	{
	if (len <= 0) return;
	PmDocOp* o = op(d, EOpText, x, base, 0, 0, grey);
	if (!o) return;
	o->font = (unsigned char)font;
	o->off = off;
	o->len = len;
	o->link = (short)link;
	}

static void set_link(PmDoc* d, int n, int off, int len)
	{
	if (n <= 0 || n > 400) return;
	if (n >= d->linkCap)
		{
		int nc = n + 32;
		int* a = (int*)ui_alloc(nc * (int)sizeof(int));
		int* b = (int*)ui_alloc(nc * (int)sizeof(int));
		if (!a || !b) { ui_free(a); ui_free(b); return; }
		for (int i = 0; i < nc; i++) { a[i] = -1; b[i] = 0; }
		for (int j = 0; j < d->linkCap; j++) { a[j] = d->linkOff[j]; b[j] = d->linkLen[j]; }
		ui_free(d->linkOff); ui_free(d->linkLen);
		d->linkOff = a; d->linkLen = b; d->linkCap = nc;
		}
	d->linkOff[n] = off;
	d->linkLen[n] = len;
	if (n > d->nlinks) d->nlinks = n;
	}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int doc_header(const char* t, int len, const char* name, const char** value)
	{
	int nl = gfx_strlen(name);
	int p = 0;
	while (p < len)
		{
		int e = p;
		while (e < len && t[e] != '\n') e++;
		if (e == p) return 0;                      /* end of the header */
		if (e - p > nl && t[p + nl] == ':')
			{
			int k = 0;
			while (k < nl && lower((unsigned char)t[p + k]) == lower((unsigned char)name[k])) k++;
			if (k == nl)
				{
				int v = p + nl + 1;
				while (v < e && t[v] == ' ') v++;
				*value = t + v;
				return e - v;
				}
			}
		p = e + 1;
		}
	return 0;
	}

/* "Name <a@b>" -> Name; "a@b" -> a@b */
static void split_addr(const char* s, int n, const char** name, int* nlen, const char** addr, int* alen)
	{
	int lt = -1, gt = -1;
	for (int i = 0; i < n; i++) { if (s[i] == '<') lt = i; if (s[i] == '>') gt = i; }
	/* only the first address of a list */
	for (int i = 0, q = 0; i < n; i++)
		{
		if (s[i] == '"') q = !q;
		if (s[i] == ',' && !q && (lt < 0 || i > gt)) { n = i; break; }
		}
	if (lt > 0 && gt > lt && gt < n)
		{
		int e = lt;
		while (e > 0 && s[e - 1] == ' ') e--;
		int b = 0;
		if (s[b] == '"') b++;
		if (e > b && s[e - 1] == '"') e--;
		*name = s + b; *nlen = e - b;
		*addr = s + lt + 1; *alen = gt - lt - 1;
		}
	else
		{
		*name = s; *nlen = n;
		*addr = s; *alen = n;
		}
	}

/* ------------------------------------------------------------ text flow */

struct Flow
	{
	PmDoc* d;
	const char* t;
	int left, right;           /* text area */
	int x;                     /* pen */
	int y;                     /* top of the current line */
	int lineH, ascent;
	int font, bold, italic;    /* base font id for this block */
	int grey;
	int link;
	int runOff, runLen, runX, runFont, runLink;
	int any;                   /* something on this line */
	int quote;                 /* quote bars on each line */
	int fill;                  /* background for code lines, -1 none */
	};

static int style_font(Flow* f)
	{
	if (f->font == EF_R13)
		{
		if (f->bold) return EF_S13;
		if (f->italic) return EF_I13;
		}
	if (f->font == EF_R12 && f->bold) return EF_S12;
	if (f->font == EF_R11 && f->bold) return EF_S11;
	return f->font;
	}

static void flush_run(Flow* f)
	{
	if (f->runLen > 0)
		{
		int grey = f->runLink ? 0 : f->grey;
		text_op(f->d, f->runFont, f->runX, f->y + f->ascent, f->runOff, f->runLen, grey, f->runLink);
		if (f->runLink)
			{
			PmDocOp* u = op(f->d, EOpUnderline, f->runX, f->y + f->ascent + 2,
				gfx_text_width(ui_font(f->runFont), f->t + f->runOff, f->runLen), 1, 7);
			if (u) u->link = (short)f->runLink;
			}
		}
	f->runLen = 0;
	}

static void line_decor(Flow* f)
	{
	/* things drawn behind/beside every line of the block */
	if (f->fill >= 0)
		op(f->d, EOpFill, f->left - 4, f->y, f->right - f->left + 8, f->lineH, f->fill);
	for (int q = 0; q < f->quote; q++)
		op(f->d, EOpFill, 14 + q * 10, f->y, 2, f->lineH, q == 0 ? 10 : 12);
	}

static void new_line(Flow* f)
	{
	flush_run(f);
	f->y += f->lineH;
	f->x = f->left;
	f->any = 0;
	line_decor(f);
	}

static void put_word(Flow* f, int off, int len, int space)
	{
	const PmFont* font = ui_font(style_font(f));
	int sw = space && f->any ? gfx_text_width(font, " ", 1) : 0;
	int ww = gfx_text_width(font, f->t + off, len);
	if (f->any && f->x + sw + ww > f->right)
		{
		new_line(f);
		sw = 0;
		}
	/* a word longer than the line: break it */
	while (!f->any && ww > f->right - f->left && len > 1)
		{
		int k = gfx_fit(font, f->t + off, len, f->right - f->left);
		if (k < 1) k = 1;
		f->runOff = off; f->runLen = k; f->runX = f->x; f->runFont = style_font(f); f->runLink = f->link;
		new_line(f);
		off += k; len -= k;
		ww = gfx_text_width(font, f->t + off, len);
		}
	int fnt = style_font(f);
	if (f->runLen > 0 && (f->runFont != fnt || f->runLink != f->link || f->runOff + f->runLen + (sw ? 1 : 0) != off))
		flush_run(f);
	if (f->runLen == 0)
		{
		f->runOff = sw ? off - 1 : off;        /* the space belongs to the run */
		f->runLen = 0;
		f->runX = f->x;
		f->runFont = fnt;
		f->runLink = f->link;
		if (sw && f->t[off - 1] != ' ') { f->runOff = off; f->runX = f->x + sw; }
		}
	f->runLen = off + len - f->runOff;
	f->x += sw + ww;
	f->any = 1;
	}

/* flows the text of one block (inline codes included) */
static void flow_text(Flow* f, int p, int e)
	{
	int space = 0;
	while (p < e)
		{
		unsigned char c = (unsigned char)f->t[p];
		if (c == 0x11 || c == 0x12) { flush_run(f); f->bold = c == 0x11; p++; continue; }
		if (c == 0x13 || c == 0x14) { flush_run(f); f->italic = c == 0x13; p++; continue; }
		if (c == 0x15)
			{
			int n = 0;
			p++;
			while (p < e && f->t[p] >= '0' && f->t[p] <= '9') n = n * 10 + (f->t[p++] - '0');
			if (p < e && f->t[p] == 0x16) p++;
			flush_run(f);
			f->link = n;
			continue;
			}
		if (c == 0x17) { flush_run(f); f->link = 0; p++; continue; }
		if (c == ' ' || c < 0x20)
			{
			space = 1;
			p++;
			continue;
			}
		int w = p;
		while (w < e && (unsigned char)f->t[w] > ' ' && !((unsigned char)f->t[w] >= 0x11 && (unsigned char)f->t[w] <= 0x17)) w++;
		put_word(f, p, w - p, space);
		space = 0;
		p = w;
		}
	flush_run(f);
	}

static void start_block(Flow* f, int font, int grey, int left, int lineH, int quote, int fill)
	{
	const PmFont* pf = ui_font(font);
	f->font = font;
	f->grey = grey;
	f->bold = f->italic = 0;
	f->link = 0;
	f->left = left;
	f->x = left;
	f->lineH = lineH;
	f->ascent = pf->ascent + (lineH - pf->ascent - pf->descent) / 2;
	f->quote = quote;
	f->fill = fill;
	f->any = 0;
	f->runLen = 0;
	line_decor(f);
	}

static void end_block(Flow* f)
	{
	flush_run(f);
	f->y += f->lineH;
	}

/* ------------------------------------------------------------ the header */

static void fmt_int(char* out, int* k, int v)
	{
	char b[12];
	int n = 0;
	if (v == 0) b[n++] = '0';
	while (v > 0 && n < 11) { b[n++] = (char)('0' + v % 10); v /= 10; }
	while (n) out[(*k)++] = b[--n];
	}

void doc_free(PmDoc* d)
	{
	ui_free(d->ops);
	ui_free(d->linkOff);
	ui_free(d->linkLen);
	d->ops = 0; d->nops = d->cap = 0;
	d->linkOff = d->linkLen = 0; d->linkCap = 0;
	d->nlinks = 0;
	}

int doc_link_url(const PmDoc* d, const char* text, int link, const char** url)
	{
	if (link <= 0 || link >= d->linkCap || d->linkOff[link] < 0) return 0;
	*url = text + d->linkOff[link];
	return d->linkLen[link];
	}

/* header + body; text is kept by the caller */
int doc_build(PmDoc* d, const char* t, int len, int width,
              const PmUiAttachment* att, int natt, int truncatedKb)
	{
	Flow f;
	const int M = 16;                              /* margins */
	const char* v;
	int n, p;

	d->ops = 0; d->nops = d->cap = 0; d->height = 0; d->nlinks = 0;
	d->linkOff = d->linkLen = 0; d->linkCap = 0; d->bodyTop = 0; d->natt = natt;

	f.d = d; f.t = t; f.right = width - M; f.y = 10; f.runLen = 0;

	/* subject, large */
	n = doc_header(t, len, "Subject", &v);
	start_block(&f, EF_S16, 0, M, 21, 0, -1);
	if (n) flow_text(&f, (int)(v - t), (int)(v - t) + n);
	else
		{
		/* "(no subject)" isn't in the text: use a text op on a constant? no -
		   leave the line empty */
		}
	end_block(&f);
	f.y += 6;

	/* sender: initials badge, name, address, date */
	const char *nm = 0, *ad = 0;
	int nml = 0, adl = 0;
	n = doc_header(t, len, "From", &v);
	if (n) split_addr(v, n, &nm, &nml, &ad, &adl);
	{
		int cy = f.y + 15;
		/* badge colour from the name */
		unsigned int h = 0;
		for (int i = 0; i < nml; i++) h = h * 31 + (unsigned char)nm[i];
		int shade = 3 + (int)(h % 5);
		PmDocOp* o = op(d, EOpCircle, M + 15, cy, 15, 15, shade);
		if (o) o->extra = 0;
		/* initials: first letters of the first and last words */
		int i0 = -1, i1 = -1;
		for (int i = 0; i < nml; i++)
			{
			unsigned char ch = (unsigned char)nm[i];
			int start = i == 0 || nm[i - 1] == ' ' || nm[i - 1] == '"';
			if (start && ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch >= 0xc0))
				{
				if (i0 < 0) i0 = i; else i1 = i;
				}
			}
		/* the initials are drawn from the text itself (upper-cased when drawn) */
		if (i0 >= 0)
			{
			PmDocOp* a = op(d, EOpText, 0, cy + 4, 0, 0, 15);
			if (a) { a->font = EF_S13; a->off = (int)(nm - t) + i0; a->len = 1; a->extra = 1; a->x = (short)(M + 15); }
			if (i1 >= 0)
				{
				PmDocOp* b = op(d, EOpText, 0, cy + 4, 0, 0, 15);
				if (b) { b->font = EF_S13; b->off = (int)(nm - t) + i1; b->len = 1; b->extra = 2; b->x = (short)(M + 15); }
				}
			}
		int tx = M + 38;
		int dl = doc_header(t, len, "Date", &v);
		int dw = 0;
		if (dl)
			{
			/* "Tue, 29 Sep 2026 19:55:46 +0100" -> "Tue, 29 Sep 2026 19:55" */
			int k = dl;
			for (int i = 0; i + 2 < dl; i++)
				if (v[i] == ':' && i + 3 < dl && v[i + 3] == ':') { k = i + 3; break; }
			const char* dv = v;
			dw = gfx_text_width(ui_font(EF_R11), dv, k);
			text_op(d, EF_R11, f.right - dw, f.y + 11, (int)(dv - t), k, 6, 0);
			}
		if (nml)
			{
			const PmFont* sf = ui_font(EF_S13);
			int k = gfx_fit(sf, nm, nml, f.right - tx - dw - 10);
			text_op(d, EF_S13, tx, f.y + 11, (int)(nm - t), k, 0, 0);
			}
		if (adl && ad != nm)
			{
			int k = gfx_fit(ui_font(EF_R11), ad, adl, f.right - tx);
			text_op(d, EF_R11, tx, f.y + 25, (int)(ad - t), k, 6, 0);
			}
		f.y += 32;
	}
	/* recipients */
	for (int r = 0; r < 2; r++)
		{
		n = doc_header(t, len, r == 0 ? "To" : "Cc", &v);
		if (!n) continue;
		start_block(&f, EF_R11, 6, M + 38, 14, 0, -1);
		/* label in bold: flow "To" from the header name in the text */
		const char* lab = v;
		while (lab > t && lab[-1] != '\n') lab--;
		text_op(d, EF_S11, M + 38, f.y + f.ascent, (int)(lab - t), r == 0 ? 2 : 2, 5, 0);
		f.left = M + 38 + gfx_text_width(ui_font(EF_S11), lab, 2) + 5;
		f.x = f.left;
		/* list only names, to keep it short: whole text, clipped to 2 lines */
		int e = (int)(v - t) + n;
		int y0 = f.y;
		flow_text(&f, (int)(v - t), e);
		end_block(&f);
		if (f.y - y0 > 2 * f.lineH)
			{
			/* drop what went past two lines */
			while (d->nops > 0 && d->ops[d->nops - 1].y > y0 + 2 * f.lineH) d->nops--;
			f.y = y0 + 2 * f.lineH;
			}
		}
	f.y += 6;
	op(d, EOpFill, M, f.y, width - 2 * M, 1, 12);
	f.y += 10;
	d->bodyTop = f.y;

	/* ---- the body */
	p = 0;
	while (p < len && !(t[p] == '\n' && (p == 0 || t[p - 1] == '\n'))) p++;
	p++;
	int gap = 0;
	while (p < len)
		{
		int e = p;
		while (e < len && t[e] != '\n') e++;
		if (e == p)
			{
			if (!gap) { f.y += 7; gap = 1; }
			p = e + 1;
			continue;
			}
		gap = 0;
		if ((unsigned char)t[p] == 0x01 && p + 1 < e)
			{
			char k = t[p + 1];
			int q = p + 2;
			switch (k)
				{
			case 'p':
				while (q < e && t[q] == ' ') q++;
				start_block(&f, EF_R13, 0, M, 17, 0, -1);
				flow_text(&f, q, e);
				end_block(&f);
				break;
			case 'h':
				{
				int lvl = q < e ? t[q] - '0' : 2;
				q++;
				while (q < e && t[q] == ' ') q++;
				f.y += lvl == 1 ? 6 : 4;
				if (lvl <= 1) start_block(&f, EF_S20, 0, M, 26, 0, -1);
				else if (lvl == 2) start_block(&f, EF_S16, 0, M, 21, 0, -1);
				else start_block(&f, EF_S13, 0, M, 18, 0, -1);
				flow_text(&f, q, e);
				end_block(&f);
				f.y += 2;
				break;
				}
			case 'l':
				{
				int depth = q < e ? t[q] - '0' : 1;
				if (depth < 1) depth = 1;
				if (depth > 6) depth = 6;
				q++;
				int m0 = q;
				while (q < e && t[q] != 0x02) q++;
				int ml = q - m0;
				if (q < e) q++;
				int ind = M + depth * 16;
				start_block(&f, EF_R13, 0, ind, 17, 0, -1);
				if (ml == 1 && (unsigned char)t[m0] == 0x95)
					op(d, EOpCircle, ind - 9, f.y + 9, 2, 2, 3);
				else
					{
					int mw = gfx_text_width(ui_font(EF_R13), t + m0, ml);
					text_op(d, EF_R13, ind - 5 - mw, f.y + f.ascent, m0, ml, 4, 0);
					}
				flow_text(&f, q, e);
				end_block(&f);
				break;
				}
			case 'q':
				{
				int depth = q < e ? t[q] - '0' : 1;
				if (depth < 1) depth = 1;
				if (depth > 6) depth = 6;
				q++;
				while (q < e && t[q] == ' ') q++;
				start_block(&f, EF_R13, depth > 1 ? 7 : 5, M + depth * 10 + 4, 17, depth, -1);
				flow_text(&f, q, e);
				end_block(&f);
				break;
				}
			case 'c':
				q = p + 2;
				if (q < e && t[q] == ' ') q++;
				start_block(&f, EF_M11, 2, M + 6, 14, 0, 14);
				flow_text(&f, q, e);
				end_block(&f);
				break;
			case 'r':
				f.y += 6;
				op(d, EOpFill, M, f.y, width - 2 * M, 1, 11);
				f.y += 7;
				break;
			case 'i':
				{
				q = p + 2;
				while (q < e && t[q] == ' ') q++;
				const PmFont* sf = ui_font(EF_R11);
				int tw = gfx_text_width(sf, t + q, e - q);
				int bw = tw + 30;
				if (bw > width - 2 * M) bw = width - 2 * M;
				if (bw < 40) bw = 40;
				PmDocOp* o = op(d, EOpFrame, M, f.y + 2, bw, 20, 11);
				if (o) o->extra = 4;
				PmDocOp* ic = op(d, EOpIcon, M + 5, f.y + 5, 14, 14, 7);
				if (ic) ic->extra = EIconImage;
				int k = gfx_fit(sf, t + q, e - q, bw - 30);
				text_op(d, EF_R11, M + 24, f.y + 16, q, k, 6, 0);
				f.y += 24;
				break;
				}
			case 's':
				q = p + 2;
				while (q < e && t[q] == ' ') q++;
				start_block(&f, EF_R11, 7, M, 14, 0, -1);
				flow_text(&f, q, e);
				end_block(&f);
				break;
			case 'u':
				{
				int num = 0;
				while (q < e && t[q] >= '0' && t[q] <= '9') num = num * 10 + (t[q++] - '0');
				while (q < e && t[q] == ' ') q++;
				set_link(d, num, q, e - q);
				break;
				}
			default:
				break;
				}
			}
		else
			{
			/* a plain line */
			start_block(&f, EF_R13, 0, M, 17, 0, -1);
			flow_text(&f, p, e);
			end_block(&f);
			}
		p = e + 1;
		}

	/* ---- what wasn't downloaded */
	if (truncatedKb > 0)
		{
		f.y += 8;
		PmDocOp* o = op(d, EOpRound, M, f.y, width - 2 * M, 24, 14);
		if (o) o->extra = 5;
		PmDocOp* ic = op(d, EOpIcon, M + 8, f.y + 5, 14, 14, 5);
		if (ic) ic->extra = EIconDownload;
		/* the words are drawn by the reader (they aren't in the text) */
		PmDocOp* m = op(d, EOpText, M + 28, f.y + 16, truncatedKb, 0, 5);
		if (m) { m->font = EF_R12; m->extra = 3; m->len = 0; m->off = truncatedKb; }
		f.y += 30;
		}

	/* ---- attachments, as chips */
	if (natt > 0)
		{
		f.y += 10;
		int x = M;
		const PmFont* nf = ui_font(EF_S12);
		const PmFont* zf = ui_font(EF_R11);
		for (int i = 0; i < natt; i++)
			{
			int nw = gfx_text_width(nf, att[i].name, att[i].len);
			int zw = gfx_text_width(zf, att[i].size, att[i].slen);
			if (nw > 260) nw = 260;
			int w = 8 + 14 + 6 + nw + 8 + zw + 10;
			if (x > M && x + w > width - M) { x = M; f.y += 30; }
			PmDocOp* o = op(d, EOpRound, x, f.y, w, 26, 14);
			if (o) { o->extra = 6; o->link = (short)(-1000 - i); }
			PmDocOp* fr = op(d, EOpFrame, x, f.y, w, 26, 11);
			if (fr) { fr->extra = 6; fr->link = (short)(-1000 - i); }
			PmDocOp* ic = op(d, EOpIcon, x + 8, f.y + 6, 14, 14, 3);
			if (ic) { ic->extra = EIconPaperclip; ic->link = (short)(-1000 - i); }
			/* names and sizes aren't in the text: extra 4 = attachment name i, 5 = size i */
			PmDocOp* a = op(d, EOpText, x + 28, f.y + 17, 0, 0, 0);
			if (a) { a->font = EF_S12; a->extra = 4; a->off = i; a->len = nw; a->link = (short)(-1000 - i); }
			PmDocOp* z = op(d, EOpText, x + 28 + nw + 8, f.y + 17, 0, 0, 7);
			if (z) { z->font = EF_R11; z->extra = 5; z->off = i; z->link = (short)(-1000 - i); }
			x += w + 8;
			}
		f.y += 34;
		}
	d->height = f.y + 10;
	return d->nops;
	}

/* ------------------------------------------------------------ plain text */

/* The body without the block codes: for quoting in replies ("> ") and for
   forwarding. Lists keep their markers, quotes get their '>'s back. */
int doc_plain(const char* t, int len, char* out, int max, int quote)
	{
	int p = 0, k = 0;
	while (p < len && !(t[p] == '\n' && (p == 0 || t[p - 1] == '\n'))) p++;
	p++;
	while (p < len && k < max - 8)
		{
		int e = p;
		while (e < len && t[e] != '\n') e++;
		int q = p, depth = 0, skip = 0;
		const char* marker = 0;
		int ml = 0;
		if ((unsigned char)t[p] == 0x01 && p + 1 < e)
			{
			char kd = t[p + 1];
			q = p + 2;
			if (kd == 'h') q++;
			else if (kd == 'q') { depth = t[q] - '0'; q++; }
			else if (kd == 'l')
				{
				q++;
				marker = t + q;
				while (q < e && t[q] != 0x02) q++;
				ml = (int)(t + q - marker);
				if (q < e) q++;
				}
			else if (kd == 'u') skip = 1;
			else if (kd == 'r') { marker = "----"; ml = 4; }
			else if (kd == 'i') { if (k < max - 2) out[k++] = '['; }
			while (q < e && t[q] == ' ' && kd != 'c') q++;
			}
		if (!skip)
			{
			if (quote && k < max - 2) { out[k++] = '>'; if (!depth) out[k++] = ' '; }
			for (int i = 0; i < depth && k < max - 2; i++) out[k++] = '>';
			if (depth && k < max - 1) out[k++] = ' ';
			for (int i = 0; i < ml && k < max - 2; i++) out[k++] = (unsigned char)marker[i] == 0x95 ? '*' : marker[i];
			if (ml && k < max - 1) out[k++] = ' ';
			for (int i = q; i < e && k < max - 2; i++)
				{
				unsigned char c = (unsigned char)t[i];
				if (c == 0x15)
					{
					i++;
					while (i < e && t[i] >= '0' && t[i] <= '9') i++;
					continue;                      /* (i now at \x16: skipped by the loop) */
					}
				if (c >= 0x11 && c <= 0x17) continue;
				if (c == 0x01 || c == 0x02) continue;
				out[k++] = (char)c;
				}
			if ((unsigned char)t[p] == 0x01 && p + 1 < e && t[p + 1] == 'i' && k < max - 2) out[k++] = ']';
			out[k++] = '\n';
			}
		p = e + 1;
		}
	(void)fmt_int;
	return k;
	}
