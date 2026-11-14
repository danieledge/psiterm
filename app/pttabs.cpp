// PTTABS.CPP - tmux's windows as EIKON tabs (0.75)
//
// When PsiTerm finds tmux's window list (psiterm.cpp, ParseTmuxTabs) it puts
// a tab strip at the top of the terminal and hides tmux's own status line
// under it. The tabs are drawn as EIKON draws a multi-page dialog's page
// tabs (CEikPageSelector, e.g. PsiMail's Preferences), measured from the
// 5mx: the dialog font; each tab a black outline with a dark grey shadow
// inside its right edge; the current tab 2 pixels taller, with a light grey
// line inside its top and left edges, and open at the bottom so it joins
// the page (here the terminal); the others recessed behind the page's top
// line, a pixel apart. CEikPageSelector itself only works inside a dialog
// (it builds the dialog's pages from resources), so it is drawn here.
//
// Tap a tab to go to that window. Names that don't fit end in "..."; when
// they would be cut to less than 4 letters, arrows at the ends scroll the
// strip instead (it follows the current window as that changes).

#include "psiterm.h"
#include <eiktxtut.h>

const TInt KTabRaise = 2;        // the current tab stands this much taller
const TInt KTabTextX = 7;        // text inset from the tab's left edge
const TInt KTabExtraW = 13;      // tab width = text + this (insets, outline, shadow)
const TInt KTabArrowW = 15;      // the scroll arrows' width
const TInt KTabDropArrowW = 13;  // (1.06) the session drop-down: its arrow, and the gap after it
const TInt KTabDropGap = 4;
const TInt KTabMinChars = 4;     // a name shows at least this much ("buil...") before
                                 // tabs scroll off the strip instead

static TRgb Gray4(TInt aLevel)
	{
	return TRgb::Gray4(aLevel);  // 0 black, 1 dark grey, 2 light grey, 3 white
	}

// A tab's colours. On the 5mx, EIKON's greys; on a colour screen (Series 7,
// netBook) the strip is the Series 7's title-band navy, as PsiTerm's status
// line, with the current tab in the terminal's own colours: black bevels
// and a grey dialog face looked out of place against a coloured theme.
struct TTabLook
	{
	TRgb iFace, iInk;            // the tab and its text
	TRgb iEdge, iShade, iLight;  // outline, shadow inside the right edge, highlight
	};

static const CFont* TabFont()
	{
	return CEikonEnv::Static()->NormalFont();     // the dialog font
	}

// The strip: the current tab's 2 extra pixels, then a tab as tall as
// EIKON's (26 pixels in all with the 5mx's dialog font)
TInt CTermView::TabStripHeight() const
	{
	const CFont* f = TabFont();
	return f->AscentInPixels() + f->DescentInPixels() + 11;
	}

TInt CTermView::RowY(TInt aRow) const
	{
	if (iTabsTop && iTabMsg && aRow == iTabHideRow)
		return iOriginY0 + (iTabStripH - iCellH) / 2;   // tmux's bar, shown in the strip
	return iOriginY + aRow * iCellH;
	}

// The strip goes up or down: the terminal is laid out again (a row more or
// less, the SSH window size changes and tmux redraws)
void CTermView::SetTabsTop(TBool aTop)
	{
	iTabsTop = aTop;
	if (!aTop)
		iTabMsg = EFalse;
	iTabFirst = 0;
	iTabLastCur = -1;
	iCacheValid = EFalse;
	Layout();
	}

static void TabLabel(TDes& aLabel, const CTermView::TTmuxTab& aTab)
	{
	aLabel.Num(aTab.iIndex);
	aLabel.Append(' ');
	aLabel.Append(aTab.iName);
	}

// Sets each tab's iX0/iX1 (its outline's left and right pixels, 0/0 when
// not shown) and the arrows. Neighbouring tabs are a pixel apart; the
// current tab shares its edges with its neighbours.
void CTermView::LayoutTabs(const CFont& aFont, TInt aLeft, TInt aRight)
	{
	TInt n = iTabCount;
	iTabArrowL = iTabArrowR = -1;
	if (n <= 0)
		return;
	TInt nat[KMaxTabs], minW[KMaxTabs];
	TInt cur = 0;
	TBuf<40> label;
	for (TInt i = 0; i < n; i++)
		{
		TabLabel(label, iTabs[i]);
		nat[i] = aFont.TextWidthInPixels(label) + KTabExtraW;
		minW[i] = nat[i];                   // (a short name is shown whole)
		if (iTabs[i].iName.Length() > KTabMinChars)
			{
			label.Num(iTabs[i].iIndex);
			label.Append(' ');
			label.Append(iTabs[i].iName.Left(KTabMinChars));
			label.Append((TChar)KTextUtilClipEndChar);
			minW[i] = aFont.TextWidthInPixels(label) + KTabExtraW;
			}
		if (minW[i] > nat[i])
			minW[i] = nat[i];
		if (iTabs[i].iCurrent)
			cur = i;
		iTabs[i].iX0 = iTabs[i].iX1 = 0;
		}
	TInt room = aRight - aLeft + 1;

	// what tabs [a, b] take at widths min(nat, max(min, cap))
	#define TAB_W(i, cap) (Max(minW[i], Min(nat[i], (cap))))
	TInt first = 0, last = n - 1;
	TInt sumMin = 0;
	for (TInt k = 0; k < n; k++)
		sumMin += minW[k] + (k > 0 ? ((iTabs[k].iCurrent || iTabs[k - 1].iCurrent) ? -1 : 1) : 0);
	if (sumMin > room)
		{
		// too many: show a run of them with arrows, keeping the current tab
		// in it (unless the arrows were used to look along the strip)
		TBool follow = (cur != iTabLastCur);
		iTabLastCur = cur;
		first = iTabFirst;
		if (first < 0) first = 0;
		if (first > n - 1) first = n - 1;
		if (follow && cur < first)
			first = cur;
		for (;;)
			{
			TInt used = first > 0 ? KTabArrowW + 1 : 0;
			last = first;
			used += minW[first];
			while (last + 1 < n)
				{
				TInt adv = (iTabs[last].iCurrent || iTabs[last + 1].iCurrent) ? -1 : 1;
				TInt need = used + adv + minW[last + 1];
				if (last + 2 < n)
					need += KTabArrowW + 1;   // the right-hand arrow
				if (need > room)
					break;
				used += adv + minW[last + 1];
				last++;
				}
			if (follow && cur > last && first < cur)
				{
				first++;
				continue;
				}
			break;
			}
		iTabFirst = first;
		if (first > 0)
			{
			iTabArrowL = aLeft;
			aLeft += KTabArrowW + 1;
			}
		if (last < n - 1)
			{
			iTabArrowR = aRight - KTabArrowW + 1;
			aRight -= KTabArrowW + 1;
			}
		room = aRight - aLeft + 1;
		}
	// the widest cap at which tabs [first, last] fit
	TInt lo = 0, hi = 0;
	for (TInt j = first; j <= last; j++)
		if (nat[j] > hi)
			hi = nat[j];
	while (lo < hi)
		{
		TInt mid = (lo + hi + 1) / 2;
		TInt total = 0;
		for (TInt j = first; j <= last; j++)
			total += TAB_W(j, mid) + (j > first ? ((iTabs[j].iCurrent || iTabs[j - 1].iCurrent) ? -1 : 1) : 0);
		if (total <= room)
			lo = mid;
		else
			hi = mid - 1;
		}
	TInt x = aLeft;
	for (TInt t = first; t <= last; t++)
		{
		if (t > first)
			x += (iTabs[t].iCurrent || iTabs[t - 1].iCurrent) ? -1 : 1;
		TInt w = TAB_W(t, lo);
		if (x + w - 1 > aRight)
			w = aRight - x + 1;              // (only when a single tab is too wide)
		if (w < KTabExtraW)
			break;
		iTabs[t].iX0 = x;
		iTabs[t].iX1 = x + w - 1;
		x += w;
		}
	#undef TAB_W
	}

// One tab, aTop to aBottom (the current tab runs down over the page's top
// line, to join the page): its outline, shadow and (on the current one)
// highlight, and its text, clipped with "..." to fit
static void DrawOneTab(CWindowGc& aGc, const CFont& aFont, TInt aL, TInt aR, TInt aTop,
	TInt aBottom, TInt aBase, TBool aCurrent, const TDesC& aText, const TTabLook& aLook)
	{
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(aLook.iFace);
	aGc.DrawRect(TRect(aL + 1, aTop + 1, aR, aBottom + 1));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(aLook.iEdge);
	aGc.DrawLine(TPoint(aL, aTop), TPoint(aR + 1, aTop));
	aGc.DrawLine(TPoint(aL, aTop), TPoint(aL, aBottom + 1));
	aGc.DrawLine(TPoint(aR, aTop), TPoint(aR, aBottom + 1));
	aGc.SetPenColor(aLook.iShade);
	aGc.DrawLine(TPoint(aR - 1, aTop + 1), TPoint(aR - 1, aBottom + 1));
	if (aCurrent)
		{
		aGc.DrawLine(TPoint(aR - 2, aTop + 2), TPoint(aR - 2, aBottom + 1));
		aGc.SetPenColor(aLook.iLight);
		aGc.DrawLine(TPoint(aL + 2, aTop + 2), TPoint(aR - 2, aTop + 2));
		aGc.DrawLine(TPoint(aL + 2, aTop + 2), TPoint(aL + 2, aBottom + 1));
		}
	if (aText.Length() == 0)
		return;
	TBuf<40> shown(aText.Left(Min(aText.Length(), 40)));
	TInt room = aR - aL + 1 - KTabExtraW;
	if (aFont.TextWidthInPixels(shown) > room)
		TextUtils::ClipToFit(shown, aFont, room);
	aGc.SetPenColor(aLook.iInk);
	aGc.DrawText(shown, TPoint(aL + KTabTextX, aBase));
	}

// a scroll arrow: an unselected tab with a black triangle
static void DrawArrow(CWindowGc& aGc, TInt aL, TInt aTop, TInt aBottom, TBool aRight, const TTabLook& aLook)
	{
	TInt r = aL + KTabArrowW - 1;
	DrawOneTab(aGc, *TabFont(), aL, r, aTop, aBottom, 0, EFalse, KNullDesC, aLook);
	TInt cy = (aTop + aBottom) / 2;
	TInt cx = (aL + r) / 2;
	aGc.SetPenColor(aLook.iInk);
	for (TInt i = 0; i < 4; i++)
		{
		TInt x = aRight ? cx - 2 + i : cx + 2 - i;   // (the tip points out)
		aGc.DrawLine(TPoint(x, cy - (3 - i)), TPoint(x, cy + (3 - i) + 1));
		}
	}

// (1.06) A drop-down list's button: a raised box (not a tab: it stands clear
// of the page's top line) with its text and a down arrow
static void DrawDropButton(CWindowGc& aGc, const CFont& aFont, TInt aL, TInt aR, TInt aTop,
	TInt aBottom, TInt aBase, const TDesC& aText, const TTabLook& aLook)
	{
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(aLook.iEdge);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(aLook.iFace);
	aGc.DrawRoundRect(TRect(aL, aTop, aR + 1, aBottom + 1), TSize(3, 3));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenColor(aLook.iShade);
	aGc.DrawLine(TPoint(aL + 2, aBottom - 1), TPoint(aR - 1, aBottom - 1));
	aGc.DrawLine(TPoint(aR - 1, aTop + 2), TPoint(aR - 1, aBottom));
	aGc.SetPenColor(aLook.iInk);
	aGc.DrawText(aText, TPoint(aL + KTabTextX, aBase));
	TInt cx = aR - 9, cy = (aTop + aBottom) / 2;
	for (TInt i = 0; i < 4; i++)
		aGc.DrawLine(TPoint(cx - 3 + i, cy - 1 + i), TPoint(cx + 4 - i, cy - 1 + i));
	}

// tmux's session on this connection, by name (EFalse: not known, or no tmux)
TBool CTermView::TmuxSessionName(TDes& aName) const
	{
	aName.Zero();
	if (iQSid.Length() == 0)
		return EFalse;
	for (TInt i = 0; i < iQSessCount; i++)
		if (iQSessId[i] == iQSid)
			{
			TInt n = iQSessName[i].Length();
			aName = iQSessName[i].Left(n < aName.MaxLength() ? n : aName.MaxLength());
			return aName.Length() > 0;
			}
	return EFalse;
	}

// where the drop-down's list opens: under the button, in the view's co-ordinates
TPoint CTermView::TabDropPos() const
	{
	return TPoint(2, iOriginY0 + iTabStripH);
	}

// The whole strip. Under tmux's prompts and messages it shows tmux's bar
// row itself, as text, until the tab list is back.
void CTermView::DrawTabs(CWindowGc& aGc) const
	{
	if (!iTabsTop || iTabStripH <= 0)
		return;
	TInt width = Rect().iBr.iX;
	TRect strip(0, iOriginY0, width, iOriginY0 + iTabStripH);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	if (iTabMsg)
		{
		aGc.SetBrushColor(Grey(15));
		aGc.DrawRect(strip);
		DrawCells(aGc, iTabHideRow, 0, iTabHideRow + 1, iCols);
		return;
		}
	TTabLook look, cur;
	TRgb band;                                  // the strip behind the tabs
	if (iColour)
		{
		band = TRgb(0, 0, 128);
		look.iFace = TRgb(40, 48, 160);
		look.iInk = TRgb(224, 228, 255);
		look.iEdge = TRgb(0, 0, 72);
		look.iShade = TRgb(24, 30, 120);
		look.iLight = look.iFace;
		cur.iFace = Grey(15);                   // the page: the theme's paper
		cur.iInk = Grey(0);
		cur.iEdge = look.iEdge;
		cur.iShade = cur.iLight = cur.iFace;    // (flat: no bevel)
		}
	else
		{
		CEikonEnv* env = CEikonEnv::Static();
		band = look.iFace = env->ControlColor(EEikColorDialogBackground, *this);
		look.iInk = env->ControlColor(EEikColorDialogText, *this);
		look.iEdge = Gray4(0);
		look.iShade = Gray4(1);
		look.iLight = Gray4(2);
		cur = look;
		cur.iFace = Grey(15);                   // the terminal's own colours
		cur.iInk = Grey(0);
		}
	aGc.SetBrushColor(band);
	aGc.DrawRect(strip);
	const CFont* f = TabFont();
	aGc.UseFont(f);
	CTermView* self = CONST_CAST(CTermView*, this);
	// (1.06) tmux's session at the left, as a drop-down list (Muxedo's
	// "main" button): a tap, or Shift+Ctrl+G, lists the sessions
	TBuf<24> sess;
	TInt dropW = 0;
	if (TmuxSessionName(sess))
		{
		TInt room = width / 4 - KTabExtraW - KTabDropArrowW;
		if (f->TextWidthInPixels(sess) > room)
			TextUtils::ClipToFit(sess, *f, room);
		dropW = f->TextWidthInPixels(sess) + KTabExtraW + KTabDropArrowW;
		}
	self->iTabDropW = dropW;
	self->LayoutTabs(*f, 2 + (dropW ? dropW + KTabDropGap : 0), width - 3);
	TInt top = iOriginY0;                       // the current tab's top
	TInt bottom = iOriginY0 + iTabStripH - 1;   // the page's top line
	TInt base = bottom - 3 - f->DescentInPixels();
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(look.iEdge);
	aGc.DrawLine(TPoint(0, bottom), TPoint(width, bottom));
	if (dropW)
		DrawDropButton(aGc, *f, 2, 2 + dropW - 1, top + KTabRaise, bottom - 3, base - 1, sess, look);
	TBuf<40> label;
	TInt curTab = -1;
	for (TInt i = 0; i < iTabCount; i++)
		{
		if (iTabs[i].iX1 <= iTabs[i].iX0)
			continue;
		if (iTabs[i].iCurrent)
			{
			curTab = i;                         // drawn last: in front
			continue;
			}
		TabLabel(label, iTabs[i]);
		DrawOneTab(aGc, *f, iTabs[i].iX0, iTabs[i].iX1, top + KTabRaise, bottom - 1, base,
			EFalse, label, look);
		}
	if (iTabArrowL >= 0)
		DrawArrow(aGc, iTabArrowL, top + KTabRaise, bottom - 1, EFalse, look);
	if (iTabArrowR >= 0)
		DrawArrow(aGc, iTabArrowR, top + KTabRaise, bottom - 1, ETrue, look);
	if (curTab >= 0)
		{
		// the terminal's own colours, so the tab joins it as a page
		TabLabel(label, iTabs[curTab]);
		DrawOneTab(aGc, *f, iTabs[curTab].iX0, iTabs[curTab].iX1, top, bottom, base,
			ETrue, label, cur);
		}
	aGc.UseFont(iFont);
	}

// A tap on the strip: a tab goes to that window, an arrow scrolls the strip
TBool CTermView::TabPenDownL(TInt aX)
	{
	if (iTabMsg)
		return EFalse;
	if (iTabDropW > 0 && aX >= 2 && aX < 2 + iTabDropW)
		{
		((CPsiTermAppUi*)iEikonEnv->EikAppUi())->TmuxSessPopupL();   // (1.06) the sessions
		return ETrue;
		}
	TBool scroll = EFalse;
	if (iTabArrowL >= 0 && aX >= iTabArrowL && aX < iTabArrowL + KTabArrowW)
		{
		iTabFirst--;
		scroll = ETrue;
		}
	else if (iTabArrowR >= 0 && aX >= iTabArrowR && aX < iTabArrowR + KTabArrowW)
		{
		iTabFirst++;
		scroll = ETrue;
		}
	if (scroll)
		{
		if (iTabFirst < 0)
			iTabFirst = 0;
		if (IsActivated() && !iPaintGc)
			{
			ActivateGc();
			DrawTabs(SystemGc());
			DeactivateGc();
			}
		return ETrue;
		}
	for (TInt i = 0; i < iTabCount; i++)
		if (iTabs[i].iX1 > iTabs[i].iX0 && aX >= iTabs[i].iX0 && aX <= iTabs[i].iX1)
			{
			if (!iTabs[i].iCurrent)
				SelectTmuxWindow(iTabs[i].iIndex);
			return ETrue;
			}
	return EFalse;
	}

// ============================================================================
// (1.06) The connections as tabs: Muxedo's top row. A strip above the
// terminal with Home (the Start view) and a tab for each connection, its
// server's icon and name. It looks as the tmux strip under it, a shade
// darker on a colour screen so the two rows read apart.
// ============================================================================

const TInt KSessIconW = 16 + 4;      // a tab's icon and the gap after it
const TInt KSessHomeW = 26;          // the Home button

CPtSessionBar* CPtSessionBar::NewL(const TRect& aRect)
	{
	CPtSessionBar* b = new(ELeave) CPtSessionBar;
	CleanupStack::PushL(b);
	b->CreateWindowL();
	b->iColour = CEikonEnv::Static()->ScreenDevice()->DisplayMode() >= EColor16;
	b->iHomeW = KSessHomeW;
	b->SetRectL(aRect);
	b->ActivateL();
	CleanupStack::Pop();
	return b;
	}

TInt CPtSessionBar::Height()
	{
	const CFont* f = TabFont();
	return f->AscentInPixels() + f->DescentInPixels() + 11;   // (as the tmux strip)
	}

void CPtSessionBar::SetBarRectL(const TRect& aRect)
	{
	if (aRect != Rect())
		SetRectL(aRect);
	}

void CPtSessionBar::SetTabs(const TTab* aTabs, TInt aCount)
	{
	if (aCount > KMaxTabs)
		aCount = KMaxTabs;
	TBool same = (aCount == iCount);
	for (TInt i = 0; i < aCount && same; i++)
		same = aTabs[i].iSlot == iTabs[i].iSlot && aTabs[i].iName == iTabs[i].iName
			&& aTabs[i].iIcon == iTabs[i].iIcon && aTabs[i].iLive == iTabs[i].iLive
			&& aTabs[i].iFront == iTabs[i].iFront;
	if (same)
		return;
	iCount = aCount;
	for (TInt i = 0; i < aCount; i++)
		iTabs[i] = aTabs[i];
	if (IsVisible())
		DrawNow();
	}

void CPtSessionBar::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	TRect r = Rect();
	TTabLook look, cur;
	TRgb band;
	if (iColour)
		{
		// (the palette's own colours: nothing dithers)
		band = TRgb(0, 0, 102);
		look.iFace = TRgb(51, 51, 153);
		look.iInk = TRgb(204, 204, 255);
		look.iEdge = TRgb(0, 0, 51);
		look.iShade = TRgb(0, 0, 102);
		look.iLight = look.iFace;
		cur.iFace = KRgbWhite;
		cur.iInk = KRgbBlack;
		cur.iEdge = look.iEdge;
		cur.iShade = cur.iLight = cur.iFace;
		}
	else
		{
		band = look.iFace = iEikonEnv->ControlColor(EEikColorDialogBackground, *this);
		look.iInk = iEikonEnv->ControlColor(EEikColorDialogText, *this);
		look.iEdge = Gray4(0);
		look.iShade = Gray4(1);
		look.iLight = Gray4(2);
		cur = look;
		cur.iFace = KRgbWhite;
		cur.iInk = KRgbBlack;
		}
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(band);
	gc.DrawRect(r);
	const CFont* f = TabFont();
	gc.UseFont(f);
	TInt top = r.iTl.iY, bottom = r.iBr.iY - 1;
	TInt base = bottom - 3 - f->DescentInPixels();
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(look.iEdge);
	gc.DrawLine(TPoint(r.iTl.iX, bottom), TPoint(r.iBr.iX, bottom));
	// Home: four squares, as Muxedo's button for its list of servers
	TInt hl = r.iTl.iX + 2, hr = hl + iHomeW - 1;
	DrawOneTab(gc, *f, hl, hr, top + KTabRaise, bottom - 1, base, EFalse, KNullDesC, look);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(look.iInk);
	TInt gx = (hl + hr) / 2 - 6, gy = (top + KTabRaise + bottom) / 2 - 5;
	for (TInt q = 0; q < 4; q++)
		gc.DrawRect(TRect(gx + (q & 1) * 7, gy + (q >> 1) * 7, gx + (q & 1) * 7 + 5, gy + (q >> 1) * 7 + 5));
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	// the tabs: as wide as their names, or an equal share when they do not fit
	CPtSessionBar* self = CONST_CAST(CPtSessionBar*, this);
	TInt left = hr + 4, right = r.iBr.iX - 3;
	TInt share = iCount > 0 ? (right - left + 1 - (iCount - 1)) / iCount : 0;
	TInt x = left;
	TInt front = -1;
	for (TInt i = 0; i < iCount; i++)
		{
		TTab& t = self->iTabs[i];
		TInt w = f->TextWidthInPixels(t.iName) + KTabExtraW + (t.iIcon > 0 ? KSessIconW : 0);
		if (w > share)
			w = share;
		t.iX0 = x;
		t.iX1 = x + w - 1;
		x += w + 1;
		if (t.iFront)
			front = i;
		}
	for (TInt pass = 0; pass < 2; pass++)
		for (TInt i = 0; i < iCount; i++)
			{
			const TTab& t = iTabs[i];
			if ((pass == 1) != (i == front) || t.iX1 - t.iX0 < KTabExtraW)
				continue;                    // (the one in front last: over its neighbours)
			const TTabLook& l = (i == front) ? cur : look;
			DrawOneTab(gc, *f, t.iX0, t.iX1, i == front ? top : top + KTabRaise,
				i == front ? bottom : bottom - 1, base, i == front, KNullDesC, l);
			TInt tx = t.iX0 + KTabTextX;
			if (t.iIcon > 0 && t.iX1 - t.iX0 > KTabExtraW + KSessIconW)
				{
				PtDrawHostIcon(gc, t.iIcon, TPoint(tx, (top + KTabRaise + bottom) / 2 - 7), l.iInk);
				tx += KSessIconW;
				}
			TBuf<24> name(t.iName);
			TInt room = t.iX1 - tx - 5;
			if (room > 8)
				{
				if (f->TextWidthInPixels(name) > room)
					TextUtils::ClipToFit(name, *f, room);
				// (a connection that has ended: its name in the shadow colour)
				gc.SetPenColor(t.iLive || i == front ? l.iInk : (iColour ? TRgb(153, 153, 204) : Gray4(1)));
				gc.DrawText(name, TPoint(tx, base));
				}
			}
	gc.DiscardFont();
	}

void CPtSessionBar::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	if (aEvent.iType != TPointerEvent::EButton1Down)
		return;
	TInt x = aEvent.iPosition.iX;
	CPsiTermAppUi* ui = (CPsiTermAppUi*)iEikonEnv->EikAppUi();
	if (x < Rect().iTl.iX + 2 + iHomeW)
		{
		ui->SessionTabL(-1);
		return;
		}
	for (TInt i = 0; i < iCount; i++)
		if (x >= iTabs[i].iX0 && x <= iTabs[i].iX1)
			{
			ui->SessionTabL(iTabs[i].iSlot);
			return;
			}
	}
