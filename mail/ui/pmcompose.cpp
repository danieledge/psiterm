/* pmcompose.cpp - writing a message, making an event, and the start-up
 * screen, in PsiMail's own look (see pmui.h) rather than EPOC dialogs:
 * a quiet top bar with the actions, the fields as labelled lines, and the
 * text filling the rest of the screen. */
#include "pmui.h"
#include "pmfonts.h"

enum
	{
	KBar = 30,
	KField = 26,               /* To, Cc, Subject */
	KLabelX = 16,
	KFieldX = 84,
	KChips = 28,               /* the attachments' row */
	KLine = 19                 /* the text's lines */
	};

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

/* the dark pill with an icon and a word: the screen's main action */
static int pill(PmCanvas* c, int right, int y, int icon, const char* label, int dark)
	{
	int n = slen(label);
	int w = gfx_text_width(&KFontS12, label, n) + (icon >= 0 ? 38 : 24);
	int x = right - w;
	if (dark) gfx_round(c, x, y, w, 22, 11, 2);
	else gfx_round_frame(c, x, y, w, 22, 11, 9);
	int tx = x + 12;
	if (icon >= 0) { gfx_icon(c, &KFontICON14, icon, tx, y + 4, dark ? 15 : 3); tx += 18; }
	gfx_text(c, &KFontS12, tx, y + 15, label, n, dark ? 15 : 2);
	return x;
	}

/* where the bar's pieces are, for drawing and for the pen */
struct Bar { int send, save, attach; };

static void compose_bar(int w, Bar* b)
	{
	int sendW = gfx_text_width(&KFontS12, "Send", 4) + 38;
	int saveW = gfx_text_width(&KFontS12, "Save", 4) + 24;
	b->send = w - 10 - sendW;
	b->save = b->send - 8 - saveW;
	b->attach = b->save - 34;
	}

static void top_bar(PmCanvas* c, const char* title, int tlen)
	{
	gfx_icon(c, &KFontICON18, EIconX, 10, 6, 4);
	gfx_text_clip(c, &KFontS13, 38, 20, title, tlen, 220, 1);
	gfx_hline(c, 12, KBar - 1, c->w - 24, 13);
	}

/* ------------------------------------------------------------ compose */

void ui_compose_layout(int w, int h, int natt, PmComposeLayout* l)
	{
	l->fieldX = KFieldX;
	l->fieldW = w - KFieldX - 20;
	l->bodyX = 16;
	l->bodyW = w - 36;
	l->bodyTop = KBar + 3 * KField + (natt ? KChips : 0) + 8;
	l->bodyH = h - l->bodyTop - 4;
	l->lineH = KLine;
	}

static const char* const KLabel[3] = { "To", "Cc", "Subject" };

void ui_compose(PmCanvas* c, const PmUiCompose* k)
	{
	gfx_noclip(c);
	gfx_fill(c, 0, 0, c->w, c->h, 15);
	top_bar(c, k->title, k->tlen);
	Bar b;
	compose_bar(c->w, &b);
	pill(c, c->w - 10, 4, EIconSend, "Send", 1);
	pill(c, b.send - 8, 4, -1, "Save", 0);
	gfx_icon(c, &KFontICON18, EIconPaperclip, b.attach + 6, 6, 4);
	if (k->busy || k->statlen)
		{
		int sx = 40 + gfx_text_width(&KFontS13, k->title, k->tlen) + 12;
		gfx_text_clip(c, &KFontR11, sx, 19, k->status, k->statlen, b.attach - sx - 8, 6);
		}

	PmComposeLayout l;
	ui_compose_layout(c->w, c->h, k->natt, &l);
	for (int i = 0; i < 3; i++)
		{
		int y = KBar + i * KField;
		int focus = k->focus == i;
		if (focus) gfx_round(c, 8, y + 2, c->w - 16, KField - 3, 6, 14);
		gfx_text(c, focus ? &KFontS12 : &KFontR12, KLabelX, y + 17, KLabel[i], slen(KLabel[i]), focus ? 2 : 7);
		PmEditor* e = k->field[i];
		if (e)
			{
			ed_draw(c, e, l.fieldX, y + 3, KField - 5, i == 2 ? 0 : 1, focus);
			if (e->len == 0 && i == 0)
				gfx_text(c, &KFontR12, l.fieldX + (focus ? 4 : 0), y + 17, "Who is it to?", 13, 10);
			}
		gfx_hline(c, KLabelX, y + KField - 1, c->w - 2 * KLabelX, 13);
		}
	int y = KBar + 3 * KField;
	if (k->natt)
		{
		/* attachment chips, as in the reader */
		int x = KLabelX;
		for (int i = 0; i < k->natt; i++)
			{
			const PmUiAttachment* a = &k->att[i];
			int nw = gfx_text_width(&KFontS11, a->name, a->len);
			if (nw > 150) nw = 150;
			int sw = gfx_text_width(&KFontR11, a->size, a->slen);
			int w = 28 + nw + 6 + sw + 10;
			if (x + w > c->w - 16) { gfx_text(c, &KFontR11, x, y + 18, "...", 3, 6); break; }
			gfx_round(c, x, y + 4, w, 20, 6, 14);
			gfx_round_frame(c, x, y + 4, w, 20, 6, 12);
			gfx_icon(c, &KFontICON14, EIconPaperclip, x + 7, y + 7, 4);
			gfx_text_clip(c, &KFontS11, x + 24, y + 18, a->name, a->len, nw, 1);
			gfx_text(c, &KFontR11, x + 24 + nw + 6, y + 18, a->size, a->slen, 6);
			x += w + 6;
			}
		y += KChips;
		}
	/* the text */
	if (k->body)
		{
		ed_draw(c, k->body, l.bodyX, l.bodyTop, l.bodyH, 0, k->focus == 3);
		if (k->body->len == 0 && k->focus != 3)
			gfx_text(c, &KFontR13, l.bodyX, l.bodyTop + 14, "Write your message", 18, 10);
		int total = ed_height(k->body);
		if (total > l.bodyH)
			{
			int th = l.bodyH * l.bodyH / total;
			if (th < 8) th = 8;
			int rows = l.bodyH / KLine;
			int maxTop = k->body->nlines - rows;
			int ty = l.bodyTop + (l.bodyH - th) * k->body->top / (maxTop > 0 ? maxTop : 1);
			gfx_round(c, c->w - 5, ty, 3, th, 1, 10);
			}
		}
	(void)y;
	}

int ui_compose_hit(int aW, int aH, const PmUiCompose* k, int x, int y, int* aIndex)
	{
	*aIndex = -1;
	if (y < KBar)
		{
		Bar b;
		compose_bar(aW, &b);
		if (x < 34) return EHitCancel;
		if (x >= b.send) return EHitSend;
		if (x >= b.save) return EHitSave;
		if (x >= b.attach) return EHitAttach;
		return EHitNone;
		}
	if (y < KBar + 3 * KField)
		{
		*aIndex = (y - KBar) / KField;
		return EHitField;
		}
	PmComposeLayout l;
	ui_compose_layout(aW, aH, k->natt, &l);
	if (y < l.bodyTop - 8 && k->natt)
		{
		/* which chip */
		int cx = KLabelX;
		for (int i = 0; i < k->natt; i++)
			{
			const PmUiAttachment* a = &k->att[i];
			int nw = gfx_text_width(&KFontS11, a->name, a->len);
			if (nw > 150) nw = 150;
			int w = 28 + nw + 6 + gfx_text_width(&KFontR11, a->size, a->slen) + 10;
			if (x >= cx && x < cx + w) { *aIndex = i; return EHitAttach; }
			cx += w + 6;
			}
		return EHitNone;
		}
	int bx = x - l.bodyX, by = y - l.bodyTop;
	if (bx < 0) bx = 0;
	if (by < 0) by = 0;
	*aIndex = bx | (by << 16);
	return EHitBody;
	}

/* ------------------------------------------------------------ a new event */

enum { KEvTitleY = KBar + 12, KEvRows = KBar + 56, KEvRow = 30, KEvValueX = 52 };

void ui_event_edit_layout(int w, int* titleX, int* titleW, int* whereX, int* whereW)
	{
	*titleX = 28;
	*titleW = w - 56;
	*whereX = KEvValueX;
	*whereW = w - KEvValueX - 40;
	}

/* the rows under the title: which Ev* each shows */
static const int KRowField[5] = { EvDate, EvFrom, EvWhere, EvAlarm, -1 };

static int row_of(int field)
	{
	switch (field)
		{
	case EvDate: return 0;
	case EvAllDay: case EvFrom: case EvTo: return 1;
	case EvWhere: return 2;
	case EvAlarm: return 3;
		}
	return -1;
	}

/* a value you change with Left/Right: < value > when focused */
static void stepper(PmCanvas* c, int x, int y, const char* s, int n, int focus, int bold)
	{
	const PmFont* f = bold ? &KFontS13 : &KFontR13;
	int w = gfx_text_width(f, s, n);
	if (focus)
		{
		gfx_round(c, x - 6, y + 3, w + 44, KEvRow - 7, 6, 3);
		gfx_text(c, f, x, y + 19, s, n, 15);
		gfx_icon(c, &KFontICON14, EIconChevronLeft, x + w + 6, y + 8, 12);
		gfx_icon(c, &KFontICON14, EIconChevronRight, x + w + 20, y + 8, 12);
		}
	else
		gfx_text(c, f, x, y + 19, s, n, 1);
	}

/* the all-day switch */
static int toggle(PmCanvas* c, int x, int y, int on, int focus)
	{
	int w = 30, h = 16;
	gfx_round(c, x, y, w, h, 8, on ? 2 : 12);
	if (focus) gfx_round_frame(c, x - 2, y - 2, w + 4, h + 4, 10, 3);
	gfx_circle(c, 2 * (on ? x + w - 8 : x + 8), 2 * (y + 8), 2 * 6, 15);
	return w;
	}

void ui_event_edit(PmCanvas* c, const PmUiEventEdit* k)
	{
	gfx_noclip(c);
	gfx_fill(c, 0, 0, c->w, c->h, 15);
	top_bar(c, "New event", 9);
	pill(c, c->w - 10, 4, EIconCheck, "Save", 1);
	/* where it goes (or what just happened) beside the title */
	const char* note = k->statlen ? k->status : k->cal;
	int nn = k->statlen ? k->statlen : k->clen;
	if (nn > 0)
		gfx_text_clip(c, &KFontR11, 130, 19, note, nn, c->w - 250, 6);

	/* the title, large, as the event will look */
	int tx, tw, wx, ww;
	ui_event_edit_layout(c->w, &tx, &tw, &wx, &ww);
	gfx_round(c, 14, KEvTitleY + 2, 4, 30, 2, 3);
	if (k->focus == EvTitle) gfx_round(c, tx - 6, KEvTitleY - 2, tw + 12, 36, 7, 14);
	if (k->title)
		{
		ed_draw(c, k->title, tx, KEvTitleY + 3, 28, 0, k->focus == EvTitle);
		if (k->title->len == 0)
			gfx_text(c, &KFontS20, tx + (k->focus == EvTitle ? 4 : 0), KEvTitleY + 25, "Event name", 10, 10);
		}

	static const int KIcon[4] = { EIconCalendarDays, EIconClock, EIconMapPin, EIconBell };
	for (int r = 0; r < 4; r++)
		{
		int y = KEvRows + r * KEvRow;
		int rowFocus = row_of(k->focus) == r;
		gfx_icon(c, &KFontICON14, KIcon[r], 26, y + 8, rowFocus ? 2 : 6);
		switch (r)
			{
		case 0:
			stepper(c, KEvValueX, y, k->date, k->dlen, k->focus == EvDate, 1);
			break;
		case 1:
			{
			int x = KEvValueX;
			x += toggle(c, x, y + 7, k->allday, k->focus == EvAllDay) + 8;
			gfx_text(c, &KFontR12, x, y + 19, "All day", 7, k->focus == EvAllDay ? 1 : 5);
			x += gfx_text_width(&KFontR12, "All day", 7) + 24;
			if (!k->allday)
				{
				stepper(c, x, y, k->from, k->fromLen, k->focus == EvFrom, 1);
				x += gfx_text_width(&KFontS13, k->from, k->fromLen) + (k->focus == EvFrom ? 44 : 10);
				gfx_text(c, &KFontR13, x, y + 19, "to", 2, 6);
				x += 22;
				stepper(c, x, y, k->to, k->toLen, k->focus == EvTo, 1);
				}
			break;
			}
		case 2:
			if (k->focus == EvWhere) gfx_round(c, KEvValueX - 6, y + 3, ww + 12, KEvRow - 7, 6, 14);
			if (k->where)
				{
				ed_draw(c, k->where, wx, y + 5, KEvRow - 10, 1, k->focus == EvWhere);
				if (k->where->len == 0)
					gfx_text(c, &KFontR13, wx + (k->focus == EvWhere ? 4 : 0), y + 19, "Add a place", 11, 10);
				}
			break;
		case 3:
			stepper(c, KEvValueX, y, k->alarm, k->alen, k->focus == EvAlarm, 0);
			break;
			}
		gfx_hline(c, KEvValueX, y + KEvRow - 1, c->w - KEvValueX - 24, 14);
		}
	/* how to drive this */
	const char* help = "Up/Down: next line    Left/Right: change    Enter: save    Esc: cancel";
	gfx_text_clip(c, &KFontR11, KEvValueX, c->h - 10, help, slen(help), c->w - KEvValueX - 16, 9);
	(void)KRowField;
	}

int ui_event_edit_hit(int aW, int aH, const PmUiEventEdit* k, int x, int y, int* aIndex)
	{
	(void)aH;
	*aIndex = -1;
	if (y < KBar)
		{
		int saveW = gfx_text_width(&KFontS12, "Save", 4) + 38;
		if (x < 34) return EHitCancel;
		if (x >= aW - 10 - saveW) return EHitSave;
		return EHitNone;
		}
	if (y < KEvRows) { *aIndex = EvTitle; return EHitField; }
	int r = (y - KEvRows) / KEvRow;
	if (r > 3) return EHitNone;
	int field = r == 0 ? EvDate : r == 2 ? EvWhere : r == 3 ? EvAlarm : EvAllDay;
	if (r == 1)
		{
		int x0 = KEvValueX + 30 + 8 + gfx_text_width(&KFontR12, "All day", 7) + 24;
		if (x < x0 || k->allday) { *aIndex = EvAllDay; return EHitField; }
		int fw = gfx_text_width(&KFontS13, k->from, k->fromLen) + 10;
		field = x < x0 + fw + 20 ? EvFrom : EvTo;
		}
	*aIndex = field;
	/* on a focused stepper: its arrows */
	if (field == k->focus && (field == EvDate || field == EvAlarm || field == EvFrom || field == EvTo))
		{
		int vx = KEvValueX;
		const char* s = field == EvDate ? k->date : field == EvAlarm ? k->alarm : field == EvFrom ? k->from : k->to;
		int n = field == EvDate ? k->dlen : field == EvAlarm ? k->alen : field == EvFrom ? k->fromLen : k->toLen;
		if (field == EvFrom || field == EvTo)
			{
			vx = KEvValueX + 30 + 8 + gfx_text_width(&KFontR12, "All day", 7) + 24;
			if (field == EvTo) vx += gfx_text_width(&KFontS13, k->from, k->fromLen) + 10 + 22;
			}
		int w = gfx_text_width(field == EvAlarm ? &KFontR13 : &KFontS13, s, n);
		if (x >= vx + w + 4 && x < vx + w + 19) return EHitPrev;
		if (x >= vx + w + 19 && x < vx + w + 40) return EHitNext;
		}
	return EHitField;
	}

/* ------------------------------------------------------------ starting up */

void ui_splash(PmCanvas* c, const char* s, int n)
	{
	gfx_noclip(c);
	gfx_fill(c, 0, 0, c->w, c->h, 15);
	int cx = c->w / 2, top = 58;
	/* the icon: a dark rounded square with an envelope and a calendar tab */
	gfx_round(c, cx - 30, top, 60, 60, 16, 2);
	gfx_round(c, cx - 17, top + 19, 34, 24, 3, 15);
	/* the envelope's flap, drawn as two lines of pixels */
	for (int i = 0; i < 17; i++)
		{
		gfx_pixel(c, cx - 17 + i, top + 21 + i * 12 / 17, 2, 15);
		gfx_pixel(c, cx + 16 - i, top + 21 + i * 12 / 17, 2, 15);
		gfx_pixel(c, cx - 17 + i, top + 22 + i * 12 / 17, 2, 8);
		gfx_pixel(c, cx + 16 - i, top + 22 + i * 12 / 17, 2, 8);
		}
	const char* name = "PsiMail";
	int w = gfx_text_width(&KFontS20, name, 7);
	gfx_text(c, &KFontS20, cx - w / 2, top + 92, name, 7, 0);
	const char* tag = "Mail and calendar for the Psion";
	int tl = slen(tag);
	w = gfx_text_width(&KFontR12, tag, tl);
	gfx_text(c, &KFontR12, cx - w / 2, top + 112, tag, tl, 6);
	if (n > 0)
		{
		w = gfx_text_width(&KFontR11, s, n);
		gfx_icon(c, &KFontICON14, EIconLoader, cx - w / 2 - 18, c->h - 34, 7);
		gfx_text(c, &KFontR11, cx - w / 2, c->h - 23, s, n, 7);
		}
	}
