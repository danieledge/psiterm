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
const TInt KTabMinChars = 4;     // a name shows at least this much ("buil...") before
                                 // tabs scroll off the strip instead

static TRgb Gray4(TInt aLevel)
	{
	return TRgb::Gray4(aLevel);  // 0 black, 1 dark grey, 2 light grey, 3 white
	}

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
	TInt aBottom, TInt aBase, TBool aCurrent, const TDesC& aText, TRgb aFace, TRgb aInk)
	{
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(aFace);
	aGc.DrawRect(TRect(aL + 1, aTop + 1, aR, aBottom + 1));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(Gray4(0));
	aGc.DrawLine(TPoint(aL, aTop), TPoint(aR + 1, aTop));
	aGc.DrawLine(TPoint(aL, aTop), TPoint(aL, aBottom + 1));
	aGc.DrawLine(TPoint(aR, aTop), TPoint(aR, aBottom + 1));
	aGc.SetPenColor(Gray4(1));
	aGc.DrawLine(TPoint(aR - 1, aTop + 1), TPoint(aR - 1, aBottom + 1));
	if (aCurrent)
		{
		aGc.DrawLine(TPoint(aR - 2, aTop + 2), TPoint(aR - 2, aBottom + 1));
		aGc.SetPenColor(Gray4(2));
		aGc.DrawLine(TPoint(aL + 2, aTop + 2), TPoint(aR - 2, aTop + 2));
		aGc.DrawLine(TPoint(aL + 2, aTop + 2), TPoint(aL + 2, aBottom + 1));
		}
	if (aText.Length() == 0)
		return;
	TBuf<40> shown(aText.Left(Min(aText.Length(), 40)));
	TInt room = aR - aL + 1 - KTabExtraW;
	if (aFont.TextWidthInPixels(shown) > room)
		TextUtils::ClipToFit(shown, aFont, room);
	aGc.SetPenColor(aInk);
	aGc.DrawText(shown, TPoint(aL + KTabTextX, aBase));
	}

// a scroll arrow: an unselected tab with a black triangle
static void DrawArrow(CWindowGc& aGc, TInt aL, TInt aTop, TInt aBottom, TBool aRight, TRgb aFace)
	{
	TInt r = aL + KTabArrowW - 1;
	DrawOneTab(aGc, *TabFont(), aL, r, aTop, aBottom, 0, EFalse, KNullDesC, aFace, aFace);
	TInt cy = (aTop + aBottom) / 2;
	TInt cx = (aL + r) / 2;
	aGc.SetPenColor(Gray4(0));
	for (TInt i = 0; i < 4; i++)
		{
		TInt x = aRight ? cx - 2 + i : cx + 2 - i;   // (the tip points out)
		aGc.DrawLine(TPoint(x, cy - (3 - i)), TPoint(x, cy + (3 - i) + 1));
		}
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
	CEikonEnv* env = CEikonEnv::Static();
	TRgb face = env->ControlColor(EEikColorDialogBackground, *this);
	TRgb ink = env->ControlColor(EEikColorDialogText, *this);
	aGc.SetBrushColor(face);
	aGc.DrawRect(strip);
	const CFont* f = TabFont();
	aGc.UseFont(f);
	CTermView* self = CONST_CAST(CTermView*, this);
	self->LayoutTabs(*f, 2, width - 3);
	TInt top = iOriginY0;                       // the current tab's top
	TInt bottom = iOriginY0 + iTabStripH - 1;   // the page's top line
	TInt base = bottom - 3 - f->DescentInPixels();
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(Gray4(0));
	aGc.DrawLine(TPoint(0, bottom), TPoint(width, bottom));
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
			EFalse, label, face, ink);
		}
	if (iTabArrowL >= 0)
		DrawArrow(aGc, iTabArrowL, top + KTabRaise, bottom - 1, EFalse, face);
	if (iTabArrowR >= 0)
		DrawArrow(aGc, iTabArrowR, top + KTabRaise, bottom - 1, ETrue, face);
	if (curTab >= 0)
		{
		// the terminal's own colours, so the tab joins it as a page
		TabLabel(label, iTabs[curTab]);
		DrawOneTab(aGc, *f, iTabs[curTab].iX0, iTabs[curTab].iX1, top, bottom, base,
			ETrue, label, Grey(15), Grey(0));
		}
	aGc.UseFont(iFont);
	}

// A tap on the strip: a tab goes to that window, an arrow scrolls the strip
TBool CTermView::TabPenDownL(TInt aX)
	{
	if (iTabMsg)
		return EFalse;
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
