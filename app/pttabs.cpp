// PTTABS.CPP - tmux's windows, and the connections, as tabs
//
// When PsiTerm finds tmux's window list (psiterm.cpp, ParseTmuxTabs) it puts
// a tab strip at the top of the terminal and hides tmux's own status line
// under it. Since 1.06 a second strip above it (CPtSessionBar) shows the
// connections.
//
// 1.07: both strips are laid out as Muxedo's two rows. Each tab is a pill (a
// rounded box), the one in use outlined:
//   connections:  [Home] [icon Dev x] [icon NAS x] [+]                   [^]
//   tmux:         [main v] [1: psion x] [2: logs x] [+]
// A tap on a tab goes to it; its x closes it (asked first); + is a new
// connection (the Start view) or a new tmux window; ^ hides both strips
// (View > Show tabs, Shift+Ctrl+W, brings them back). The session's name is
// a drop-down list of tmux's sessions. On a taller screen (the netBook's)
// the strips are taller and every window tab has its x; on the 5mx only the
// window in use has one, to keep the tabs wide enough to read and to tap.
// (To 1.06 the tabs were drawn as EIKON's dialog page tabs.)
//
// Names that don't fit end in "..."; when they would be cut to less than 4
// letters, arrows at the ends scroll the strip instead (it follows the
// current window as that changes).

#include "psiterm.h"
#include <eiktxtut.h>

const TInt KTabTextX = 8;        // text inset from the pill's left edge
const TInt KTabExtraW = 16;      // pill width = text + this (the insets)
const TInt KTabGap = 3;          // between pills
const TInt KTabArrowW = 15;      // the scroll arrows' width
const TInt KTabDropArrowW = 12;  // the session drop-down's arrow
const TInt KTabMinChars = 4;     // a name shows at least this much ("buil...") before
                                 // tabs scroll off the strip instead
const TInt KSessIconW = 16 + 4;  // a connection's icon and the gap after it

static TBool Tall()
	{
	return CEikonEnv::Static()->ScreenDevice()->SizeInPixels().iHeight >= 400;
	}

static const CFont* TabFont()
	{
	return CEikonEnv::Static()->NormalFont();     // the dialog font
	}

// a tab's x, and the square buttons (Home, +, ^): wider on the netBook, for a finger
static TInt CloseW()
	{
	return Tall() ? 20 : 14;
	}

static TInt ButtonW()
	{
	return Tall() ? 28 : 21;
	}

// A strip: 26 pixels with the 5mx's dialog font, 32 on a taller screen
static TInt StripHeight()
	{
	const CFont* f = TabFont();
	return f->AscentInPixels() + f->DescentInPixels() + (Tall() ? 17 : 11);
	}

// A pill's colours. On the 5mx, EIKON's greys: the strip is the dialog
// grey, a tab a thin dark outline on it, the one in use white with a black
// outline two lines thick. On a colour screen (Series 7, netBook) the strip
// is navy, as PsiTerm's status line, a tab a lighter blue, the one in use
// lighter still with a white outline. All from the screen's own 216
// colours, so nothing dithers.
struct TPillLook
	{
	TRgb iFace, iInk, iEdge;
	TInt iEdgeW;                 // the outline's thickness (0: none)
	};

static void Looks(TBool aColour, TBool aUpper, const CCoeControl& aControl, TRgb& aBand,
	TPillLook& aTab, TPillLook& aCurrent)
	{
	if (aColour)
		{
		aBand = aUpper ? TRgb(0, 0, 102) : TRgb(0, 0, 153);
		aTab.iFace = aUpper ? TRgb(51, 51, 153) : TRgb(51, 51, 204);
		aTab.iInk = TRgb(204, 204, 255);
		aTab.iEdge = aTab.iFace;
		aTab.iEdgeW = 0;
		aCurrent.iFace = TRgb(102, 102, 255);
		aCurrent.iInk = KRgbWhite;
		aCurrent.iEdge = KRgbWhite;
		aCurrent.iEdgeW = 1;
		}
	else
		{
		CEikonEnv* env = CEikonEnv::Static();
		aBand = env->ControlColor(EEikColorDialogBackground, aControl);
		aTab.iFace = aBand;
		aTab.iInk = env->ControlColor(EEikColorDialogText, aControl);
		aTab.iEdge = TRgb::Gray4(1);
		aTab.iEdgeW = 1;
		aCurrent.iFace = KRgbWhite;
		aCurrent.iInk = KRgbBlack;
		aCurrent.iEdge = KRgbBlack;
		aCurrent.iEdgeW = 2;
		}
	}

// A pill from aL to aR (both drawn), aTop to aBottom
static void DrawPill(CWindowGc& aGc, TInt aL, TInt aR, TInt aTop, TInt aBottom, const TPillLook& aLook)
	{
	TRect r(aL, aTop, aR + 1, aBottom + 1);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	if (aLook.iEdgeW > 0)
		{
		aGc.SetBrushColor(aLook.iEdge);
		aGc.DrawRoundRect(r, TSize(5, 5));
		r.Shrink(aLook.iEdgeW, aLook.iEdgeW);
		}
	aGc.SetBrushColor(aLook.iFace);
	aGc.DrawRoundRect(r, TSize(4, 4));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	}

// the marks on the buttons, centred on (aX, aY), each two lines thick
static void DrawCross(CWindowGc& aGc, TInt aX, TInt aY, TRgb aInk)
	{
	aGc.SetPenColor(aInk);
	for (TInt k = 0; k < 2; k++)
		{
		aGc.DrawLine(TPoint(aX - 3 + k, aY - 3), TPoint(aX + 3 + k, aY + 3));
		aGc.DrawLine(TPoint(aX - 3 + k, aY + 2), TPoint(aX + 3 + k, aY - 4));
		}
	}

static void DrawPlus(CWindowGc& aGc, TInt aX, TInt aY, TRgb aInk)
	{
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(aInk);
	aGc.DrawRect(TRect(aX - 4, aY - 1, aX + 5, aY + 1));
	aGc.DrawRect(TRect(aX - 1, aY - 4, aX + 1, aY + 5));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	}

// ^ (hide the strips)
static void DrawUp(CWindowGc& aGc, TInt aX, TInt aY, TRgb aInk)
	{
	aGc.SetPenColor(aInk);
	for (TInt k = 0; k < 2; k++)
		{
		aGc.DrawLine(TPoint(aX - 4, aY + 2 + k), TPoint(aX + 1, aY - 3 + k));
		aGc.DrawLine(TPoint(aX, aY - 2 + k), TPoint(aX + 5, aY + 3 + k));
		}
	}

// a filled triangle: down (a drop-down list), or left / right (the scroll arrows)
static void DrawTriangle(CWindowGc& aGc, TInt aX, TInt aY, TInt aWay, TRgb aInk)
	{
	aGc.SetPenColor(aInk);
	for (TInt i = 0; i < 4; i++)
		{
		if (aWay == 0)
			aGc.DrawLine(TPoint(aX - 3 + i, aY - 1 + i), TPoint(aX + 4 - i, aY - 1 + i));
		else
			{
			TInt x = aWay > 0 ? aX - 2 + i : aX + 2 - i;   // (the tip points out)
			aGc.DrawLine(TPoint(x, aY - (3 - i)), TPoint(x, aY + (3 - i) + 1));
			}
		}
	}

// a square button: a pill with a mark (0 +, 1 ^, 2 left, 3 right)
static void DrawButton(CWindowGc& aGc, TInt aL, TInt aW, TInt aTop, TInt aBottom, TInt aMark,
	const TPillLook& aLook)
	{
	DrawPill(aGc, aL, aL + aW - 1, aTop, aBottom, aLook);
	TInt cx = aL + aW / 2, cy = (aTop + aBottom + 1) / 2;
	if (aMark == 0)
		DrawPlus(aGc, cx, cy, aLook.iInk);
	else if (aMark == 1)
		DrawUp(aGc, cx, cy, aLook.iInk);
	else
		DrawTriangle(aGc, cx, cy, aMark == 2 ? -1 : 1, aLook.iInk);
	}

static void Command(TInt aCommand)
	{
	MEikCommandObserver* o = CEikonEnv::Static()->EikAppUi();
	o->ProcessCommandL(aCommand);
	}

// ============================================================================
// tmux's windows
// ============================================================================

TInt CTermView::TabStripHeight() const
	{
	return StripHeight();
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

// "1: psion", as Muxedo
static void TabLabel(TDes& aLabel, const CTermView::TTmuxTab& aTab)
	{
	aLabel.Num(aTab.iIndex);
	aLabel.Append(_L(": "));
	aLabel.Append(aTab.iName);
	}

// Which window tabs have an x: all of them where there is room for a
// finger (a taller screen), else the window in use
static TBool TabHasClose(const CTermView::TTmuxTab& aTab)
	{
	return Tall() || aTab.iCurrent;
	}

// Sets each tab's iX0/iX1 (its pill's left and right pixels, 0/0 when not
// shown) and the arrows.
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
		TInt extra = KTabExtraW + (TabHasClose(iTabs[i]) ? CloseW() : 0);
		TabLabel(label, iTabs[i]);
		nat[i] = aFont.TextWidthInPixels(label) + extra;
		minW[i] = nat[i];                   // (a short name is shown whole)
		if (iTabs[i].iName.Length() > KTabMinChars)
			{
			label.Num(iTabs[i].iIndex);
			label.Append(_L(": "));
			label.Append(iTabs[i].iName.Left(KTabMinChars));
			label.Append((TChar)KTextUtilClipEndChar);
			minW[i] = aFont.TextWidthInPixels(label) + extra;
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
		sumMin += minW[k] + (k > 0 ? KTabGap : 0);
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
			TInt used = first > 0 ? KTabArrowW + KTabGap : 0;
			last = first;
			used += minW[first];
			while (last + 1 < n)
				{
				TInt need = used + KTabGap + minW[last + 1];
				if (last + 2 < n)
					need += KTabArrowW + KTabGap;   // the right-hand arrow
				if (need > room)
					break;
				used += KTabGap + minW[last + 1];
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
			aLeft += KTabArrowW + KTabGap;
			}
		if (last < n - 1)
			{
			iTabArrowR = aRight - KTabArrowW + 1;
			aRight -= KTabArrowW + KTabGap;
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
			total += TAB_W(j, mid) + (j > first ? KTabGap : 0);
		if (total <= room)
			lo = mid;
		else
			hi = mid - 1;
		}
	TInt x = aLeft;
	for (TInt t = first; t <= last; t++)
		{
		if (t > first)
			x += KTabGap;
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
	return TPoint(3, iOriginY0 + iTabStripH);
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
	TRgb band;
	TPillLook look, cur;
	Looks(iColour, EFalse, *this, band, look, cur);
	aGc.SetBrushColor(band);
	aGc.DrawRect(strip);
	const CFont* f = TabFont();
	aGc.UseFont(f);
	CTermView* self = CONST_CAST(CTermView*, this);
	TInt top = iOriginY0 + 2, bottom = iOriginY0 + iTabStripH - 4;
	TInt base = (top + bottom + 1) / 2 + (f->AscentInPixels() - f->DescentInPixels()) / 2 + 1;
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	if (!iColour)
		{
		// (the 5mx: a line between the strip and the terminal's white)
		aGc.SetPenColor(TRgb::Gray4(0));
		aGc.DrawLine(TPoint(0, strip.iBr.iY - 1), TPoint(width, strip.iBr.iY - 1));
		}
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
		DrawPill(aGc, 3, 3 + dropW - 1, top, bottom, look);
		aGc.SetPenColor(look.iInk);
		aGc.DrawText(sess, TPoint(3 + KTabTextX, base));
		DrawTriangle(aGc, 3 + dropW - 11, (top + bottom + 1) / 2, 0, look.iInk);
		}
	self->iTabDropW = dropW;
	// at the right: ^ (unless the connections' strip above has it), and
	// room after the last tab for +
	TInt bw = ButtonW();
	CPsiTermAppUi* ui = (CPsiTermAppUi*)iEikonEnv->EikAppUi();
	TInt right = width - 4;
	self->iTabUpX = -1;
	if (!ui->SessBarUp())
		{
		self->iTabUpX = right - bw + 1;
		DrawButton(aGc, iTabUpX, bw, top, bottom, 1, look);
		right -= bw + KTabGap;
		}
	TInt left = 3 + (dropW ? dropW + KTabGap : 0);
	self->LayoutTabs(*f, left, right - bw - KTabGap);
	TBuf<40> label;
	TInt end = left - KTabGap - 1;              // the right edge of what is drawn so far
	if (iTabArrowL >= 0)
		{
		DrawButton(aGc, iTabArrowL, KTabArrowW, top, bottom, 2, look);
		end = iTabArrowL + KTabArrowW - 1;
		}
	for (TInt i = 0; i < iTabCount; i++)
		{
		const TTmuxTab& t = iTabs[i];
		if (t.iX1 <= t.iX0)
			continue;
		const TPillLook& l = t.iCurrent ? cur : look;
		DrawPill(aGc, t.iX0, t.iX1, top, bottom, l);
		TInt closeW = TabHasClose(t) ? CloseW() : 0;
		TabLabel(label, t);
		TInt room = t.iX1 - t.iX0 + 1 - KTabExtraW - closeW;
		if (room > 4)
			{
			if (f->TextWidthInPixels(label) > room)
				TextUtils::ClipToFit(label, *f, room);
			aGc.SetPenColor(l.iInk);
			aGc.DrawText(label, TPoint(t.iX0 + KTabTextX, base));
			}
		if (closeW)
			DrawCross(aGc, t.iX1 - closeW / 2 - 2, (top + bottom + 1) / 2, l.iInk);
		end = t.iX1;
		}
	if (iTabArrowR >= 0)
		{
		DrawButton(aGc, iTabArrowR, KTabArrowW, top, bottom, 3, look);
		end = iTabArrowR + KTabArrowW - 1;
		}
	// + : a new window, straight after the last tab
	self->iTabPlusX = end + KTabGap + 1;
	DrawButton(aGc, iTabPlusX, bw, top, bottom, 0, look);
	aGc.UseFont(iFont);
	}

// A tap on the strip: the session list, a tab (its x closes that window),
// + (a new window), ^ (hide the strips), or an arrow (scrolls the strip)
TBool CTermView::TabPenDownL(TInt aX)
	{
	if (iTabMsg)
		return EFalse;
	CPsiTermAppUi* ui = (CPsiTermAppUi*)iEikonEnv->EikAppUi();
	if (iTabDropW > 0 && aX >= 3 && aX < 3 + iTabDropW)
		{
		ui->TmuxSessPopupL();                // (1.06) the sessions
		return ETrue;
		}
	TInt bw = ButtonW();
	if (iTabUpX >= 0 && aX >= iTabUpX - 1)
		{
		Command(EPtCmdShowTabs);
		return ETrue;
		}
	if (aX >= iTabPlusX - 1 && aX < iTabPlusX + bw + 1)
		{
		Command(EPtCmdTmuxNew);
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
			if (TabHasClose(iTabs[i]) && aX > iTabs[i].iX1 - CloseW() - 2)
				ui->TmuxCloseWindowL(iTabs[i].iIndex, iTabs[i].iName);
			else if (!iTabs[i].iCurrent)
				SelectTmuxWindow(iTabs[i].iIndex);
			return ETrue;
			}
	return EFalse;
	}

// ============================================================================
// (1.06) The connections as tabs: Muxedo's top row. A strip above the
// terminal with Home (the Start view), a tab for each connection (its
// server's icon and name, and an x that ends it), + (another connection)
// and, at the right, ^ (hide the strips). A shade darker than the tmux
// strip under it on a colour screen, so the two rows read apart.
// ============================================================================

CPtSessionBar* CPtSessionBar::NewL(const TRect& aRect)
	{
	CPtSessionBar* b = new(ELeave) CPtSessionBar;
	CleanupStack::PushL(b);
	b->CreateWindowL();
	b->iColour = CEikonEnv::Static()->ScreenDevice()->DisplayMode() >= EColor16;
	b->SetRectL(aRect);
	b->ActivateL();
	CleanupStack::Pop();
	return b;
	}

TInt CPtSessionBar::Height()
	{
	return StripHeight();                    // (as the tmux strip)
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
	TRgb band;
	TPillLook look, cur;
	Looks(iColour, ETrue, *this, band, look, cur);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(band);
	gc.DrawRect(r);
	const CFont* f = TabFont();
	gc.UseFont(f);
	TInt top = r.iTl.iY + 2, bottom = r.iBr.iY - 4;
	TInt mid = (top + bottom + 1) / 2;
	TInt base = mid + (f->AscentInPixels() - f->DescentInPixels()) / 2 + 1;
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	if (!iColour)
		{
		gc.SetPenColor(TRgb::Gray4(1));
		gc.DrawLine(TPoint(r.iTl.iX, r.iBr.iY - 1), TPoint(r.iBr.iX, r.iBr.iY - 1));
		}
	CPtSessionBar* self = CONST_CAST(CPtSessionBar*, this);
	TInt bw = ButtonW(), closeW = CloseW();
	// Home: four squares, as Muxedo's button for its list of servers
	TInt hl = r.iTl.iX + 3;
	self->iHomeW = bw;
	DrawPill(gc, hl, hl + bw - 1, top, bottom, look);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(look.iInk);
	TInt gx = hl + bw / 2 - 6, gy = mid - 6;
	for (TInt q = 0; q < 4; q++)
		gc.DrawRect(TRect(gx + (q & 1) * 7, gy + (q >> 1) * 7, gx + (q & 1) * 7 + 5, gy + (q >> 1) * 7 + 5));
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	// ^ at the right
	self->iUpX = r.iBr.iX - 4 - bw + 1;
	DrawButton(gc, iUpX, bw, top, bottom, 1, look);
	// the tabs: as wide as their names, or an equal share when they do not
	// fit; then +
	TInt left = hl + bw + KTabGap, right = iUpX - KTabGap - 1 - bw - KTabGap;
	TInt share = iCount > 0 ? (right - left + 1 - (iCount - 1) * KTabGap) / iCount : 0;
	TInt x = left;
	for (TInt i = 0; i < iCount; i++)
		{
		TTab& t = self->iTabs[i];
		TInt w = f->TextWidthInPixels(t.iName) + KTabExtraW + closeW + (t.iIcon > 0 ? KSessIconW : 0);
		if (w > share)
			w = share;
		t.iX0 = x;
		t.iX1 = x + w - 1;
		x += w + KTabGap;
		if (w < KTabExtraW + closeW)
			continue;
		const TPillLook& l = t.iFront ? cur : look;
		DrawPill(gc, t.iX0, t.iX1, top, bottom, l);
		TInt tx = t.iX0 + KTabTextX;
		if (t.iIcon > 0 && w > KTabExtraW + closeW + KSessIconW)
			{
			PtDrawHostIcon(gc, t.iIcon, TPoint(tx, mid - 8), l.iInk);
			tx += KSessIconW;
			}
		TBuf<24> name(t.iName);
		TInt room = t.iX1 - closeW - 6 - tx;
		if (room > 8)
			{
			if (f->TextWidthInPixels(name) > room)
				TextUtils::ClipToFit(name, *f, room);
			// (a connection that has ended: its name fainter)
			gc.SetPenColor(t.iLive || t.iFront ? l.iInk : (iColour ? TRgb(153, 153, 204) : TRgb::Gray4(1)));
			gc.DrawText(name, TPoint(tx, base));
			}
		DrawCross(gc, t.iX1 - closeW / 2 - 2, mid, l.iInk);
		}
	self->iPlusX = x;
	DrawButton(gc, iPlusX, bw, top, bottom, 0, look);
	gc.DiscardFont();
	}

void CPtSessionBar::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	if (aEvent.iType != TPointerEvent::EButton1Down)
		return;
	TInt x = aEvent.iPosition.iX;
	CPsiTermAppUi* ui = (CPsiTermAppUi*)iEikonEnv->EikAppUi();
	TInt bw = ButtonW();
	if (x < Rect().iTl.iX + 3 + iHomeW + 1 || (x >= iPlusX - 1 && x < iPlusX + bw + 1))
		{
		ui->SessionTabL(-1);                 // Home, or +: the Start view
		return;
		}
	if (x >= iUpX - 1)
		{
		Command(EPtCmdShowTabs);
		return;
		}
	for (TInt i = 0; i < iCount; i++)
		if (x >= iTabs[i].iX0 && x <= iTabs[i].iX1)
			{
			if (x > iTabs[i].iX1 - CloseW() - 2)
				ui->SessionCloseL(iTabs[i].iSlot);
			else
				ui->SessionTabL(iTabs[i].iSlot);
			return;
			}
	}
