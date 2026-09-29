/* pmscreens.cpp - PsiMail's screens (see pmui.h)
 *
 * The look: white paper, a light grey folder column, dark selection with
 * rounded corners, Inter type anti-aliased in the 5mx's 16 greys, Lucide
 * line icons. Greys: 0 black .. 15 white. */
#include "pmui.h"
#include "pmfonts.h"

const PmFont* ui_font(int aId)
	{
	switch (aId)
		{
	case EF_R11: return &KFontR11;
	case EF_R12: return &KFontR12;
	case EF_R13: return &KFontR13;
	case EF_S11: return &KFontS11;
	case EF_S12: return &KFontS12;
	case EF_S13: return &KFontS13;
	case EF_S16: return &KFontS16;
	case EF_S20: return &KFontS20;
	case EF_I13: return &KFontI13;
	case EF_M11: return &KFontM11;
		}
	return &KFontR12;
	}

/* layout */
enum
	{
	KSide = 156,               /* folder column */
	KHead = 36,                /* header of the message list */
	KRow = 30,                 /* a message: two lines */
	KFolderTop = 36,
	KFolderRow = 22,
	KBar = 28                  /* the reader's top bar */
	};

static int folder_icon(int kind)
	{
	switch (kind)
		{
	case 'I': return EIconInbox;
	case 'S': return EIconSend;
	case 'D': return EIconPencil;
	case 'A': return EIconArchive;
	case 'T': return EIconTrash2;
	case 'J': return EIconShieldAlert;
	case 'O': return EIconArrowUpFromLine;
	case 'N': return EIconFolderOpen;
		}
	return EIconFolder;
	}

static int num(char* b, int v)
	{
	char t[12];
	int n = 0, k = 0;
	if (v <= 0) { b[0] = '0'; return 1; }
	while (v && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
	while (n) b[k++] = t[--n];
	return k;
	}

/* a spinner: the loader icon, turned by 'phase' (approximated with 3 icons) */
static void spinner(PmCanvas* c, int x, int y, int grey)
	{
	gfx_icon(c, &KFontICON14, EIconLoader, x, y, grey);
	}

/* ------------------------------------------------------------ mailbox */

int ui_mailbox_rows(int aHeight) { return (aHeight - KHead - 1) / KRow; }
int ui_sidebar_rows(int aHeight) { return (aHeight - KFolderTop - 8) / KFolderRow; }

static void draw_sidebar(PmCanvas* c, const PmUiMailbox* m)
	{
	const PmFont* r12 = &KFontR12;
	const PmFont* s11 = &KFontS11;
	gfx_fill(c, 0, 0, KSide, c->h, 14);
	gfx_vline(c, KSide, 0, c->h, 12);

	/* the account, with a dot for the connection */
	gfx_icon(c, &KFontICON14, EIconMail, 12, 11, 3);
	gfx_text_clip(c, &KFontS13, 32, 23, m->account, m->alen, KSide - 50, 1);
	if (m->offline)
		gfx_icon(c, &KFontICON14, EIconCloudOff, KSide - 22, 11, 7);
	else if (m->online)
		gfx_circle(c, 2 * (KSide - 15), 2 * 18, 6, 4);

	int rows = ui_sidebar_rows(c->h);
	gfx_clip(c, 0, KFolderTop - 2, KSide, c->h);
	for (int i = 0; i < rows + 1 && m->folderTop + i < m->nfolders; i++)
		{
		const PmUiFolder* f = &m->folders[m->folderTop + i];
		int y = KFolderTop + i * KFolderRow;
		int sel = m->folderTop + i == m->folderSel;
		int fg = 2, ig = 5, cg = 4, cbg = 12;
		if (f->kind == 'O')
			{
			/* the outbox sits below a rule */
			gfx_hline(c, 12, y + 1, KSide - 24, 12);
			y += 4;
			}
		if (sel)
			{
			if (m->sidebarFocus) { gfx_round(c, 5, y, KSide - 10, KFolderRow - 2, 5, 3); fg = 15; ig = 13; cg = 3; cbg = 13; }
			else { gfx_round(c, 5, y, KSide - 10, KFolderRow - 2, 5, 11); fg = 0; ig = 3; }
			}
		int indent = f->depth * 10;
		gfx_icon(c, &KFontICON14, folder_icon(f->kind), 12 + indent, y + 3, ig);
		char b[12];
		int bn = 0;
		int cw = 0;
		if (f->unread > 0)
			{
			bn = num(b, f->unread);
			cw = gfx_text_width(s11, b, bn) + 10;
			int px = KSide - 12 - cw;
			gfx_round(c, px, y + 4, cw, 13, 6, cbg);
			gfx_text(c, s11, px + 5, y + 14, b, bn, cg);
			cw += 4;
			}
		gfx_text_clip(c, f->unread > 0 ? &KFontS12 : r12, 32 + indent, y + 15, f->name, f->len,
			KSide - 44 - indent - cw, fg);
		}
	gfx_noclip(c);
	/* more folders than fit: a little scroll mark */
	if (m->nfolders > rows)
		{
		int h = c->h - KFolderTop - 6;
		int th = h * rows / m->nfolders;
		if (th < 8) th = 8;
		int ty = KFolderTop + (h - th) * m->folderTop / (m->nfolders - rows > 0 ? m->nfolders - rows : 1);
		gfx_round(c, KSide - 4, ty, 3, th, 1, 10);
		}
	}

static void draw_row(PmCanvas* c, const PmUiRow* r, int y, int sel, int last)
	{
	int x0 = KSide + 6, x1 = c->w - 6;
	int unread = r->flags & KRowUnread;
	int fg = 0, sub = unread ? 1 : 5, dg = 6;
	if (sel)
		{
		gfx_round(c, x0, y + 1, x1 - x0, KRow - 2, 5, 3);
		fg = 15; sub = unread ? 15 : 12; dg = 12;
		}
	else if (!last)
		gfx_hline(c, KSide + 20, y + KRow - 1, c->w - KSide - 30, 13);
	if (unread)
		gfx_circle(c, 2 * (KSide + 13), 2 * (y + 9), 6, sel ? 15 : 2);
	const PmFont* ff = unread ? &KFontS12 : &KFontR12;
	const PmFont* df = unread ? &KFontS11 : &KFontR11;
	int dw = gfx_text_right(c, df, x1 - 8, y + 13, r->date, r->dlen, dg);
	gfx_text_clip(c, ff, KSide + 22, y + 13, r->from, r->flen, x1 - 8 - dw - 10 - (KSide + 22), fg);
	/* the flags at the right of the second line */
	int ix = x1 - 8;
	if (r->flags & KRowFlagged) { ix -= 15; gfx_icon(c, &KFontICON14, EIconFlag, ix, y + 14, sel ? 15 : 1); }
	if (r->flags & KRowAttach) { ix -= 15; gfx_icon(c, &KFontICON14, EIconPaperclip, ix, y + 14, sel ? 13 : 5); }
	if (r->flags & KRowAnswered) { ix -= 15; gfx_icon(c, &KFontICON14, EIconReply, ix, y + 14, sel ? 13 : 6); }
	if (r->flags & KRowError) { ix -= 15; gfx_icon(c, &KFontICON14, EIconCircleAlert, ix, y + 14, sel ? 15 : 1); }
	if (r->flags & KRowDraft) { ix -= 15; gfx_icon(c, &KFontICON14, EIconPencil, ix, y + 14, sel ? 13 : 5); }
	gfx_text_clip(c, &KFontR12, KSide + 22, y + 26, r->subj, r->slen, ix - 6 - (KSide + 22), sub);
	}

static void header_button(PmCanvas* c, int x, int icon)
	{
	gfx_icon(c, &KFontICON18, icon, x, 9, 3);
	}

void ui_mailbox(PmCanvas* c, const PmUiMailbox* m)
	{
	gfx_noclip(c);
	gfx_fill(c, KSide + 1, 0, c->w - KSide - 1, c->h, 15);
	draw_sidebar(c, m);

	/* header: title, what's happening, buttons */
	int bx = c->w - 30;
	header_button(c, bx, EIconSquarePen);
	header_button(c, bx - 30, EIconSearch);
	header_button(c, bx - 60, EIconRefreshCw);
	int tx = KSide + 16;
	int tw = gfx_text_clip(c, &KFontS16, tx, 23, m->title, m->tlen, bx - 70 - tx - 60, 0);
	int sx = tx + tw + 10;
	if (m->busy)
		{
		spinner(c, sx, 11, 5);
		sx += 18;
		}
	if (m->statlen)
		gfx_text_clip(c, &KFontR11, sx, 22, m->status, m->statlen, bx - 66 - sx, 5);
	else if (m->sublen)
		gfx_text_clip(c, &KFontR11, sx, 22, m->subtitle, m->sublen, bx - 66 - sx, 6);
	gfx_hline(c, KSide + 1, KHead - 1, c->w - KSide - 1, 13);

	/* the messages */
	gfx_clip(c, KSide + 1, KHead, c->w, c->h);
	if (m->total == 0)
		{
		const PmFont* f = &KFontR12;
		int w = gfx_text_width(f, m->empty, m->elen);
		int cx = KSide + (c->w - KSide) / 2;
		gfx_icon(c, &KFontICON18, m->busy ? EIconLoader : EIconMailOpen, cx - 9, 100, 10);
		gfx_text(c, f, cx - w / 2, 136, m->empty, m->elen, 6);
		}
	for (int i = 0; i < m->nrows; i++)
		{
		int y = KHead + i * KRow;
		if (y >= c->h) break;
		draw_row(c, &m->rows[i], y, m->top + i == m->sel && !m->sidebarFocus, m->top + i == m->total - 1);
		}
	gfx_noclip(c);
	int rows = ui_mailbox_rows(c->h);
	if (m->total > rows)
		{
		int h = c->h - KHead - 6;
		int th = h * rows / m->total;
		if (th < 8) th = 8;
		int ty = KHead + 3 + (h - th) * m->top / (m->total - rows);
		gfx_round(c, c->w - 4, ty, 3, th, 1, 10);
		}
	}

int ui_mailbox_hit(int aW, int aH, const PmUiMailbox* m, int x, int y, int* aIndex)
	{
	*aIndex = -1;
	if (x < KSide)
		{
		if (y < KFolderTop) return EHitNone;
		int i = m->folderTop + (y - KFolderTop) / KFolderRow;
		if (i >= 0 && i < m->nfolders) { *aIndex = i; return EHitFolder; }
		return EHitNone;
		}
	if (y < KHead)
		{
		int bx = aW - 30;
		if (x >= bx - 4) return EHitNew;
		if (x >= bx - 34) return EHitSearch;
		if (x >= bx - 64) return EHitRefresh;
		return EHitNone;
		}
	int i = m->top + (y - KHead) / KRow;
	(void)aH;
	if (i >= 0 && i < m->total) { *aIndex = i; return EHitRow; }
	return EHitNone;
	}

/* ------------------------------------------------------------ reader */

int ui_reader_body_height(int aHeight) { return aHeight - KBar; }

/* the reader's buttons, from the right */
static int reader_buttons(const PmUiReader* r, int* icons)
	{
	int n = 0;
	icons[n++] = EIconTrash2;
	icons[n++] = EIconArchive;
	icons[n++] = EIconFlag;
	icons[n++] = EIconForward;
	icons[n++] = EIconReplyAll;
	icons[n++] = EIconReply;
	if (r->html) icons[n++] = EIconGlobe;
	return n;
	}

static int button_hit(int icon)
	{
	switch (icon)
		{
	case EIconTrash2: return EHitDelete;
	case EIconArchive: return EHitArchive;
	case EIconFlag: return EHitFlag;
	case EIconForward: return EHitForward;
	case EIconReplyAll: return EHitReplyAll;
	case EIconReply: return EHitReply;
	case EIconGlobe: return EHitWeb;
		}
	return EHitNone;
	}

static int upper(int ch)
	{
	if (ch >= 'a' && ch <= 'z') return ch - 32;
	if (ch >= 0xe0 && ch <= 0xfe && ch != 0xf7) return ch - 32;
	return ch;
	}

static void draw_op(PmCanvas* c, const PmUiReader* r, const PmDocOp* o, int dy, int focus)
	{
	int y = o->y + dy;
	switch (o->type)
		{
	case EOpText:
		{
		const PmFont* f = ui_font(o->font);
		int grey = o->grey;
		if (focus && o->link) grey = 15;
		if (o->extra == 1 || o->extra == 2)
			{
			/* the initials badge: both letters, centred on the circle */
			if (o->extra == 2) break;
			char in[2];
			int n = 0;
			in[n++] = (char)upper((unsigned char)r->text[o->off]);
			if (o + 1 < r->doc->ops + r->doc->nops && o[1].extra == 2)
				in[n++] = (char)upper((unsigned char)r->text[o[1].off]);
			int w = gfx_text_width(f, in, n);
			gfx_text(c, f, o->x - w / 2, y, in, n, 15);
			break;
			}
		if (o->extra == 3)
			{
			char b[80];
			int k = 0;
			const char* a = "Only part of this message is here: ";
			while (*a) b[k++] = *a++;
			k += num(b + k, o->off);
			a = " KB more with Shift+Ctrl+W";
			while (*a) b[k++] = *a++;
			gfx_text(c, f, o->x, y, b, k, grey);
			break;
			}
		if (o->extra == 4 || o->extra == 5)
			{
			if (o->off < 0 || o->off >= r->natt) break;
			const PmUiAttachment* a = &r->att[o->off];
			if (o->extra == 4) gfx_text_clip(c, f, o->x, y, a->name, a->len, o->len, focus ? 15 : grey);
			else gfx_text(c, f, o->x, y, a->size, a->slen, focus ? 13 : grey);
			break;
			}
		gfx_text(c, f, o->x, y, r->text + o->off, o->len, grey);
		break;
		}
	case EOpFill:
		gfx_fill(c, o->x, y, o->w, o->h, o->grey);
		break;
	case EOpRound:
		gfx_round(c, o->x, y, o->w, o->h, o->extra, focus ? 3 : o->grey);
		break;
	case EOpFrame:
		if (!focus) gfx_round_frame(c, o->x, y, o->w, o->h, o->extra, o->grey);
		break;
	case EOpLine:
		gfx_hline(c, o->x, y, o->w, o->grey);
		break;
	case EOpCircle:
		gfx_circle(c, 2 * o->x, 2 * y, 2 * o->w, o->grey);
		break;
	case EOpIcon:
		gfx_icon(c, &KFontICON14, o->extra, o->x, y, focus ? 15 : o->grey);
		break;
	case EOpUnderline:
		gfx_dotted_hline(c, o->x, y, o->w, focus ? 15 : o->grey);
		break;
		}
	}

void ui_reader(PmCanvas* c, const PmUiReader* r)
	{
	gfx_noclip(c);
	gfx_fill(c, 0, 0, c->w, c->h, 15);

	/* top bar: back to the folder, position, actions */
	gfx_fill(c, 0, 0, c->w, KBar, 14);
	gfx_hline(c, 0, KBar - 1, c->w, 12);
	gfx_icon(c, &KFontICON14, EIconChevronLeft, 8, 7, 3);
	int fw = gfx_text_clip(c, &KFontS12, 24, 18, r->folder, r->flen, 140, 3);
	int x = 24 + fw + 12;
	if (r->count > 0)
		{
		char b[24];
		int k = num(b, r->position);
		const char* of = " of ";
		while (*of) b[k++] = *of++;
		k += num(b + k, r->count);
		x += gfx_text(c, &KFontR11, x, 18, b, k, 6) + 12;
		}
	int icons[8];
	int nb = reader_buttons(r, icons);
	int bx = c->w - 28;
	for (int i = 0; i < nb; i++)
		{
		int ic = icons[i];
		if (ic == EIconFlag && r->flagged)
			{
			gfx_round(c, bx - 4, 3, 26, 22, 6, 3);
			gfx_icon(c, &KFontICON18, ic, bx, 5, 15);
			}
		else
			gfx_icon(c, &KFontICON18, ic, bx, 5, 3);
		bx -= 30;
		}
	if (r->busy || r->statlen)
		{
		int sx = x;
		if (r->busy) { spinner(c, sx, 7, 5); sx += 18; }
		gfx_text_clip(c, &KFontR11, sx, 18, r->status, r->statlen, bx + 26 - sx, 5);
		}

	/* the message */
	gfx_clip(c, 0, KBar, c->w, c->h);
	if (r->loading || !r->doc)
		{
		gfx_text_clip(c, &KFontS16, 16, KBar + 26, r->subject, r->sublen, c->w - 32, 0);
		/* a sketch of the text to come */
		for (int i = 0; i < 5; i++)
			{
			int w = (i == 4) ? 180 : 560 - (i * 37) % 90;
			gfx_round(c, 16, KBar + 50 + i * 20, w, 9, 4, 14);
			}
		const char* t = r->statlen ? r->status : "Downloading the message";
		int tl = r->statlen ? r->statlen : gfx_strlen(t);
		gfx_icon(c, &KFontICON14, EIconDownload, 16, c->h - 26, 6);
		gfx_text_clip(c, &KFontR11, 36, c->h - 15, t, tl, c->w - 50, 6);
		gfx_noclip(c);
		return;
		}
	int dy = KBar - r->scroll;
	for (int i = 0; i < r->doc->nops; i++)
		{
		const PmDocOp* o = &r->doc->ops[i];
		int top = o->type == EOpText ? o->y + dy - 20 : o->y + dy;
		int bottom = o->type == EOpText ? o->y + dy + 6 : o->y + dy + o->h + (o->type == EOpCircle ? o->w : 0);
		if (o->type == EOpCircle) top -= o->w;
		if (bottom < KBar || top > c->h) continue;
		int focus = r->focusLink && o->link == r->focusLink;
		if (focus && o->type == EOpText && o->link > 0)
			{
			/* a highlighted link: dark behind its text */
			int w = gfx_text_width(ui_font(o->font), r->text + o->off, o->len);
			gfx_round(c, o->x - 2, o->y + dy - 12, w + 4, 16, 3, 3);
			}
		draw_op(c, r, o, dy, focus);
		}
	gfx_noclip(c);
	/* where we are in it */
	int bh = c->h - KBar;
	if (r->doc->height > bh)
		{
		int h = bh - 6;
		int th = h * bh / r->doc->height;
		if (th < 8) th = 8;
		int maxs = r->doc->height - bh;
		int ty = KBar + 3 + (h - th) * r->scroll / (maxs > 0 ? maxs : 1);
		gfx_round(c, c->w - 4, ty, 3, th, 1, 10);
		}
	}

int ui_reader_hit(int aW, int aH, const PmUiReader* r, int x, int y, int* aLink)
	{
	*aLink = 0;
	(void)aH;
	if (y < KBar)
		{
		if (x < 120) return EHitBack;
		int icons[8];
		int nb = reader_buttons(r, icons);
		int bx = aW - 28;
		for (int i = 0; i < nb; i++, bx -= 30)
			if (x >= bx - 5 && x < bx + 25) return button_hit(icons[i]);
		return EHitNone;
		}
	if (!r->doc)
		return EHitNone;
	int dy = KBar - r->scroll;
	for (int i = 0; i < r->doc->nops; i++)
		{
		const PmDocOp* o = &r->doc->ops[i];
		if (!o->link) continue;
		int ox0 = o->x, ox1, oy0, oy1;
		if (o->type == EOpText)
			{
			int w = o->extra == 4 ? o->len : gfx_text_width(ui_font(o->font), r->text + o->off, o->len);
			ox1 = o->x + w; oy0 = o->y + dy - 13; oy1 = o->y + dy + 4;
			}
		else { ox1 = o->x + o->w; oy0 = o->y + dy; oy1 = oy0 + o->h; }
		if (x >= ox0 - 2 && x < ox1 + 2 && y >= oy0 && y < oy1)
			{
			*aLink = o->link;
			return o->link > 0 ? EHitLink : EHitAttach;
			}
		}
	return y < aH / 2 + KBar / 2 ? EHitTop : EHitBottom;
	}

/* the next link or attachment on screen, in reading order, after 'from' */
int ui_reader_next_link(const PmUiReader* r, int aHeight, int from, int dir)
	{
	if (!r->doc) return 0;
	int top = r->scroll, bottom = r->scroll + aHeight - KBar;
	/* links in order of appearance (ops are in order) */
	int prev = 0, seen = from == 0;
	int last = 0;
	for (int i = 0; i < r->doc->nops; i++)
		{
		const PmDocOp* o = &r->doc->ops[i];
		if (!o->link || o->link == last) continue;
		if (o->y < top + 4 || o->y > bottom) continue;
		last = o->link;
		if (dir > 0)
			{
			if (seen) return o->link;
			if (o->link == from) seen = 1;
			}
		else
			{
			if (o->link == from) return prev;
			prev = o->link;
			}
		}
	return dir < 0 && from == 0 ? prev : 0;
	}

/* ------------------------------------------------------------ others */

void ui_welcome(PmCanvas* c, const char* l1, int n1, const char* l2, int n2)
	{
	gfx_noclip(c);
	gfx_fill(c, 0, 0, c->w, c->h, 15);
	gfx_round(c, c->w / 2 - 26, 36, 52, 52, 14, 3);
	gfx_icon(c, &KFontICON18, EIconMail, c->w / 2 - 9, 53, 15);
	const char* t = "Welcome to PsiMail";
	int tl = gfx_strlen(t);
	int w = gfx_text_width(&KFontS20, t, tl);
	gfx_text(c, &KFontS20, (c->w - w) / 2, 126, t, tl, 0);
	w = gfx_text_width(&KFontR12, l1, n1);
	gfx_text(c, &KFontR12, (c->w - w) / 2, 156, l1, n1, 4);
	w = gfx_text_width(&KFontR12, l2, n2);
	gfx_text(c, &KFontR12, (c->w - w) / 2, 175, l2, n2, 6);
	}

void ui_toast(PmCanvas* c, const char* s, int n)
	{
	const PmFont* f = &KFontR12;
	int w = gfx_text_width(f, s, n);
	if (w > c->w - 80) w = c->w - 80;
	int x = (c->w - w) / 2 - 14, y = c->h - 34;
	gfx_noclip(c);
	gfx_round(c, x, y, w + 28, 24, 12, 2);
	gfx_text_clip(c, f, x + 14, y + 16, s, n, w, 15);
	}
