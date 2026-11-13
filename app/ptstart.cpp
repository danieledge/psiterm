// PTSTART.CPP - the Start view (1.02, from Muxedo's landing page)
//
// PsiTerm opens on the saved servers, and SSH to... (Shift+Ctrl+S, the
// toolbar's first button) comes back to them. Pinned servers first, in their
// saved order, then the rest by when they were last used; each row is the
// name, user@address and when it was last used. Below the list, the
// highlighted server's details (one line on the 5mx; a panel on the
// Series 7, which has the room), with how its last connection went.
//
// Enter (or a tap on the highlighted row) connects; typing a letter goes to
// the next server whose name starts with it. The view goes over the terminal
// as the file view does (ptxfer.cpp), with its own menus, shortcuts and
// toolbar; View > Show terminal (Ctrl+Q) goes back to the terminal.

#include "psiterm.h"
#include "pticons.h"
#include <eikchlst.h>
#include <eiktbar.h>
#include <eikcmbut.h>
#include <eikmenub.h>
#include <eiktxtut.h>
#include <psiterm.rsg>

#ifndef TRAP_IGNORE
#define TRAP_IGNORE(s) { TInt _ignored; TRAP(_ignored, s); }
#endif

static TPtrC Upto(const TDesC& aText, TInt aMax)
	{
	return aText.Left(aText.Length() < aMax ? aText.Length() : aMax);
	}

// a command to the program, as its menus send them
static void Command(TInt aCommand)
	{
	MEikCommandObserver* o = CEikonEnv::Static()->EikAppUi();
	o->ProcessCommandL(aCommand);
	}

static TTime Epoch2000()
	{
	return TTime(TDateTime(2000, EJanuary, 0, 0, 0, 0, 0));
	}

TUint32 PtNowSeconds()
	{
	TTime now;
	now.HomeTime();
	TTimeIntervalSeconds s;
	if (now.SecondsFrom(Epoch2000(), s) != KErrNone || s.Int() < 0)
		return 1;
	return (TUint32)s.Int() | 1;            // (never 0: that is "never")
	}

// "Today 09:14", "Yesterday", "06/11/26", "Never"
static void WhenText(TUint32 aSecs, TDes& aOut)
	{
	aOut.Zero();
	if (!aSecs)
		{
		aOut.Copy(_L("Never"));
		return;
		}
	TTime t = Epoch2000() + TTimeIntervalSeconds((TInt)aSecs);
	TTime now;
	now.HomeTime();
	TDateTime d = t.DateTime(), n = now.DateTime();
	TInt days = now.DaysFrom(t).Int();
	if (d.Year() == n.Year() && d.Month() == n.Month() && d.Day() == n.Day())
		aOut.Format(_L("Today %02d:%02d"), d.Hour(), d.Minute());
	else if (days <= 1 && days >= 0)
		aOut.Copy(_L("Yesterday"));
	else
		aOut.Format(_L("%02d/%02d/%02d"), d.Day() + 1, (TInt)d.Month() + 1, d.Year() % 100);
	}

// "Just now", "12 min ago", "3 h ago", "Yesterday", "4 days ago", "12 Nov"
static void RelativeWhen(TUint32 aSecs, TDes& aOut)
	{
	aOut.Zero();
	if (!aSecs)
		{
		aOut.Copy(_L("New"));
		return;
		}
	TUint32 now = PtNowSeconds();
	TInt ago = now > aSecs ? (TInt)(now - aSecs) : 0;
	TTime t = Epoch2000() + TTimeIntervalSeconds((TInt)aSecs);
	TTime n;
	n.HomeTime();
	TInt days = n.DaysFrom(t).Int();
	TDateTime d = t.DateTime(), nd = n.DateTime();
	TBool today = d.Day() == nd.Day() && d.Month() == nd.Month() && d.Year() == nd.Year();
	if (ago < 60)
		aOut.Copy(_L("Just now"));
	else if (ago < 3600)
		aOut.Format(_L("%d min ago"), ago / 60);
	else if (today)
		aOut.Format(_L("%d h ago"), ago / 3600);
	else if (days <= 1)
		aOut.Copy(_L("Yesterday"));
	else if (days < 7)
		aOut.Format(_L("%d days ago"), days);
	else
		{
		static const TText* const KMon[] = { _S("Jan"), _S("Feb"), _S("Mar"), _S("Apr"), _S("May"), _S("Jun"),
			_S("Jul"), _S("Aug"), _S("Sep"), _S("Oct"), _S("Nov"), _S("Dec") };
		TPtrC mon(KMon[(TInt)d.Month()]);
		aOut.Format(_L("%d %S"), d.Day() + 1, &mon);
		}
	}

static const TText* ResultText(TInt aResult)
	{
	switch (aResult)
		{
	case EPtResultLoggedIn: return _S("logged in");
	case EPtResultNoAnswer: return _S("no answer");
	case EPtResultRefused:  return _S("login refused");
	case EPtResultSecure:   return _S("could not set up encryption");
	case EPtResultLost:     return _S("connection lost");
	default:                return _S("");
		}
	}

static TRgb ResultColour(TInt aResult)
	{
	switch (aResult)
		{
	case EPtResultLoggedIn: return TRgb(0, 153, 0);
	case EPtResultNoAnswer:
	case EPtResultLost:     return TRgb(255, 153, 0);
	default:                return TRgb(204, 0, 0);
		}
	}

// ============================================================================
// The view
// ============================================================================

CPtStartView* CPtStartView::NewL(CHostList& aHosts, CKeyList& aKeys, const TPtTmux& aTmux,
	CTermView& aTerm, const TRect& aRect)
	{
	CPtStartView* v = new(ELeave) CPtStartView(aHosts, aKeys, aTmux, aTerm);
	CleanupStack::PushL(v);
	v->ConstructL(aRect);
	CleanupStack::Pop();
	return v;
	}

CPtStartView::~CPtStartView()
	{
	delete iStar;
	delete iStarMask;
	}

void CPtStartView::ConstructL(const TRect& aRect)
	{
	CreateWindowL();
	iColour = iEikonEnv->ScreenDevice()->DisplayMode() >= EColor16;
	TFileName mbm = iEikonEnv->EikAppUi()->Application()->BitmapStoreName();
	TRAP_IGNORE(
		iStar = iEikonEnv->CreateBitmapL(mbm, EMbmSmStar);
		iStarMask = iEikonEnv->CreateBitmapL(mbm, EMbmSmStarMask);
		);
	EnableDragEvents();
	SetRectL(aRect);
	ReloadL(iHosts.iLast);
	}

void CPtStartView::SetViewRectL(const TRect& aRect)
	{
	SetRectL(aRect);
	DrawNow();
	}

void CPtStartView::SizeChanged()
	{
	Layout();
	}

// Columns and rows for the width and height there are
// Rows and their heights for the room there is
void CPtStartView::Layout()
	{
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	const CFont* title = iEikonEnv->TitleFont();
	TInt h = Rect().Height();
	TBool tall = h >= 400;
	iBandH = title->HeightInPixels() + (tall ? 18 : 8);
	iSecH = small->HeightInPixels() + (tall ? 10 : 4);
	// two lines a server: the name, then user@address
	iRowH = f->HeightInPixels() + small->HeightInPixels() + (tall ? 16 : 6);
	iFootH = tall ? 4 * (small->HeightInPixels() + 4) + 22 : small->HeightInPixels() + 6;
	iListY = iBandH;
	iFootY = h - iFootH;
	iRows = (iFootY - iListY) / iRowH;
	if (iRows < 1)
		iRows = 1;
	}

// Pinned first (in saved order), then the rest, most recently used first
void CPtStartView::ReloadL(TInt aSelectHost)
	{
	TInt keep = aSelectHost >= 0 ? aSelectHost : Current();
	iCount = 0;
	TInt n = iHosts.Count();
	for (TInt i = 0; i < n && iCount < KMaxHosts; i++)
		if (iHosts.At(i).iFlags & KPtHostPinned)
			iOrder[iCount++] = i;
	TInt first = iCount;
	iPinned = iCount;
	for (TInt i = 0; i < n && iCount < KMaxHosts; i++)
		{
		if (iHosts.At(i).iFlags & KPtHostPinned)
			continue;
		// (insertion: at most 16)
		TUint32 t = iHosts.At(i).iLastUsed;
		TInt at = iCount;
		while (at > first && iHosts.At(iOrder[at - 1]).iLastUsed < t)
			{
			iOrder[at] = iOrder[at - 1];
			at--;
			}
		iOrder[at] = i;
		iCount++;
		}
	iSel = 0;
	for (TInt r = 0; r < iCount; r++)
		if (iOrder[r] == keep)
			iSel = r;
	Layout();
	if (iTop >= iCount)
		iTop = 0;
	if (iSel < iTop)
		iTop = iSel;
	while (iTop < iSel && LastVisible(iTop) < iSel)
		iTop++;
	DrawDeferred();
	}

TInt CPtStartView::Current() const
	{
	return iCount > 0 && iSel >= 0 && iSel < iCount ? iOrder[iSel] : -1;
	}

void CPtStartView::Move(TInt aTo)
	{
	if (iCount == 0)
		return;
	if (aTo < 0)
		aTo = 0;
	if (aTo >= iCount)
		aTo = iCount - 1;
	if (aTo == iSel)
		return;
	iSel = aTo;
	if (iSel < iTop)
		iTop = iSel;
	while (iTop < iSel && LastVisible(iTop) < iSel)
		iTop++;
	iHosts.iLast = Current();
	DrawNow();
	Command(EPtCmdStSync);                   // (the toolbar's Pin)
	}

TKeyResponse CPtStartView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKey)
		return EKeyWasNotConsumed;
	if (aKeyEvent.iModifiers & EModifierCtrl)
		return EKeyWasNotConsumed;           // (the shortcuts: the menu bar's hotkeys)
	switch (aKeyEvent.iCode)
		{
	case EKeyUpArrow:   Move(iSel - 1); return EKeyWasConsumed;
	case EKeyDownArrow: Move(iSel + 1); return EKeyWasConsumed;
	case EKeyPageUp:    Move(iSel - iRows); return EKeyWasConsumed;
	case EKeyPageDown:  Move(iSel + iRows); return EKeyWasConsumed;
	case EKeyHome:      Move(0); return EKeyWasConsumed;
	case EKeyEnd:       Move(iCount - 1); return EKeyWasConsumed;
	case EKeyEnter:
		Command(EPtCmdStConnect);
		return EKeyWasConsumed;
	case EKeyEscape:
		Command(EPtCmdStTerminal);
		return EKeyWasConsumed;
	default:
		break;
		}
	// a letter: the next server whose name starts with it
	TChar c(aKeyEvent.iCode);
	if (aKeyEvent.iCode >= 0x20 && aKeyEvent.iCode < 0x100 && c.IsAlphaDigit() && iCount > 0)
		{
		c.Fold();
		for (TInt k = 1; k <= iCount; k++)
			{
			TInt r = (iSel + k) % iCount;
			const TDesC& name = iHosts.At(iOrder[r]).iName;
			if (name.Length() && TChar(name[0]).GetLowerCase() == c.GetLowerCase())
				{
				Move(r);
				break;
				}
			}
		return EKeyWasConsumed;
		}
	return EKeyWasNotConsumed;
	}

void CPtStartView::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	if (aEvent.iType != TPointerEvent::EButton1Down)
		return;
	TInt r = HitTest(aEvent.iPosition.iY - Rect().iTl.iY);
	if (r < 0)
		return;
	if (r == iSel)
		Command(EPtCmdStConnect);   // (a tap on the highlight connects)
	else
		Move(r);
	}

// The headings: "Pinned" over the pinned servers and "Recent" over the
// rest, when there are both
TBool CPtStartView::HeadBefore(TInt aRow) const
	{
	return iPinned > 0 && iPinned < iCount && (aRow == 0 || aRow == iPinned);
	}

TInt CPtStartView::LastVisible(TInt aTop) const
	{
	TInt y = iListY, last = aTop;
	for (TInt r = aTop; r < iCount; r++)
		{
		if (HeadBefore(r))
			y += iSecH;
		if (y + iRowH > iFootY)
			break;
		y += iRowH;
		last = r;
		}
	return last;
	}

TInt CPtStartView::HitTest(TInt aY) const
	{
	TInt y = iListY;
	for (TInt r = iTop; r < iCount; r++)
		{
		if (HeadBefore(r))
			y += iSecH;
		if (y + iRowH > iFootY)
			break;
		if (aY >= y && aY < y + iRowH)
			return r;
		y += iRowH;
		}
	return -1;
	}

// "SSH key "Psion"", and what runs on login
void CPtStartView::Summary(TInt aHost, TDes& aLogin, TDes& aOnLogin) const
	{
	const THostEntry& e = iHosts.At(aHost);
	aLogin.Zero();
	if (e.iAuth == 1)
		aLogin.Copy(_L("password (asked)"));
	else if (e.iAuth == 2)
		aLogin.Copy(_L("saved password"));
	else
		{
		CKeyList& keys = CONST_CAST(CKeyList&, iKeys);
		TInt k = e.iKeyId ? keys.Find(e.iKeyId) : (keys.Count() ? 0 : -1);
		aLogin.Copy(_L("SSH key"));
		if (k >= 0)
			{
			aLogin.Append(_L(" \""));
			aLogin.Append(Upto(keys.At(k).iName, 20));
			aLogin.Append('"');
			}
		if (e.iAuth == 3)
			aLogin.Append(_L(", then password"));
		}
	aOnLogin.Zero();
	if (e.iCommand.Length())
		aOnLogin.Append(Upto(e.iCommand, aOnLogin.MaxLength()));
	else
		{
		TPtTmux t;
		PtTmuxEffective(iTmux, e.iTmux, t);
		if (t.iStart)
			{
			aOnLogin.Copy(_L("tmux, session "));
			aOnLogin.Append(t.iSession);
			if (t.iOwn)
				aOnLogin.Append(_L("-psion"));
			}
		}
	}

// ---- drawing: a light page, as a modern phone's list - a large title,
// two-line rows with a badge, soft rounded highlights. On the 5mx in greys;
// on the Series 7 with colour (each server's badge its own).

// the look's colours: the 5mx's greys, or the Series 7's
struct TStartLook
	{
	TRgb iPaper, iInk, iSoft, iFaint, iRule, iHigh, iAccent;
	};

static void Look(TBool aColour, TStartLook& aLook)
	{
	if (aColour)
		{
		// (the Series 7's 256 colours: the 6x6x6 cube, in steps of 51, and
		// its greys - anything else rounds to the nearest, often white)
		aLook.iPaper = KRgbWhite;
		aLook.iInk = TRgb(0, 0, 0);
		aLook.iSoft = TRgb(102, 102, 153);
		aLook.iFaint = TRgb(153, 153, 153);
		aLook.iRule = TRgb(221, 221, 221);
		aLook.iHigh = TRgb(204, 204, 255);
		aLook.iAccent = TRgb(51, 51, 204);
		}
	else
		{
		aLook.iPaper = KRgbWhite;
		aLook.iInk = KRgbBlack;
		aLook.iSoft = TRgb::Gray16(6);
		aLook.iFaint = TRgb::Gray16(9);
		aLook.iRule = TRgb::Gray16(13);
		aLook.iHigh = TRgb::Gray16(13);
		aLook.iAccent = KRgbBlack;
		}
	}

// a server's badge colour (Series 7): from its name, so it keeps it
static TRgb BadgeColour(const TDesC& aName)
	{
	static const TUint8 KBadge[8][3] = {     // (the palette's own colours)
		{ 51, 51, 204 }, { 0, 153, 153 }, { 204, 102, 0 }, { 204, 0, 51 },
		{ 0, 102, 204 }, { 102, 51, 204 }, { 0, 153, 102 }, { 102, 102, 102 } };
	TUint h = 0;
	for (TInt i = 0; i < aName.Length(); i++)
		h = h * 31 + (aName[i] | 0x20);
	const TUint8* c = KBadge[h % 8];
	return TRgb(c[0], c[1], c[2]);
	}

void CPtStartView::DrawHead(CWindowGc& aGc, TInt aY, TInt aRow) const
	{
	TStartLook l;
	Look(iColour, l);
	const CFont* small = iEikonEnv->AnnotationFont();
	TRect r = Rect();
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(l.iPaper);
	aGc.DrawRect(TRect(r.iTl.iX, r.iTl.iY + aY, r.iBr.iX, r.iTl.iY + aY + iSecH));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.UseFont(small);
	aGc.SetPenColor(l.iFaint);
	TPtrC t(aRow == 0 ? _L("PINNED") : _L("RECENT"));
	aGc.DrawText(t, TPoint(r.iTl.iX + 10, r.iTl.iY + aY + iSecH - 3 - small->DescentInPixels()));
	aGc.DiscardFont();
	}

void CPtStartView::DrawRow(CWindowGc& aGc, TInt aY, TInt aRow) const
	{
	TStartLook l;
	Look(iColour, l);
	TRect r = Rect();
	TRect row(r.iTl.iX, r.iTl.iY + aY, r.iBr.iX, r.iTl.iY + aY + iRowH);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(l.iPaper);
	aGc.DrawRect(row);
	if (aRow >= iCount)
		return;
	const THostEntry& e = iHosts.At(iOrder[aRow]);
	TBool sel = (aRow == iSel);
	TBool tall = iRowH >= 40;
	TInt pad = tall ? 8 : 4;
	// the highlight: a soft rounded card, an accent bar at its left
	TRect card(row.iTl.iX + pad, row.iTl.iY + 1, row.iBr.iX - pad - 4, row.iBr.iY - 1);
	if (sel)
		{
		aGc.SetBrushColor(l.iHigh);
		aGc.DrawRoundRect(card, TSize(6, 6));
		aGc.SetBrushColor(l.iAccent);
		aGc.DrawRect(TRect(card.iTl.iX, card.iTl.iY + 4, card.iTl.iX + 3, card.iBr.iY - 4));
		}
	// the badge: a rounded square with the name's first letter
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	TInt bs = iRowH - (tall ? 16 : 10);
	TInt bx = card.iTl.iX + (tall ? 12 : 9), by = row.iTl.iY + (iRowH - bs) / 2;
	TRect badge(bx, by, bx + bs, by + bs);
	aGc.SetBrushColor(iColour ? BadgeColour(e.iName) : TRgb::Gray16(sel ? 3 : 5));
	aGc.DrawRoundRect(badge, TSize(bs / 4, bs / 4));
	TBuf<2> init;
	if (e.iName.Length())
		{
		TChar c = e.iName[0];
		c.UpperCase();
		init.Append(c);
		}
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.UseFont(f);
	aGc.SetPenColor(KRgbWhite);
	aGc.DrawText(init, TPoint(bx + (bs - f->TextWidthInPixels(init)) / 2, by + (bs + f->AscentInPixels()) / 2 - 1));
	// the name, and user@address under it
	TInt tx = bx + bs + (tall ? 12 : 8);
	TInt right = card.iBr.iX - 8;
	TBuf<40> when;
	RelativeWhen(e.iLastUsed, when);
	// (1.04) connected now (in front or behind): says so in place of when
	TBool on = ((CPsiTermAppUi*)iEikonEnv->EikAppUi())->HostSession(iOrder[aRow]) >= 0;
	if (on)
		when.Copy(_L("Connected"));
	TInt ww = small->TextWidthInPixels(when);
	TInt dot = (on || e.iLastResult != EPtResultNever) ? 8 : 0;
	TInt nameY = row.iTl.iY + (iRowH - f->HeightInPixels() - small->HeightInPixels() - 2) / 2 + f->AscentInPixels();
	TInt subY = nameY + f->DescentInPixels() + 2 + small->AscentInPixels();
	TBuf<120> t(Upto(e.iName, 40));
	TextUtils::ClipToFit(t, *f, right - ww - dot - 16 - tx);
	aGc.SetPenColor(l.iInk);
	aGc.DrawText(t, TPoint(tx, nameY));
	if (iColour && (e.iFlags & KPtHostPinned) && !sel)
		{
		// (pinned: a small accent mark after the name)
		TInt px = tx + f->TextWidthInPixels(t) + 6;
		aGc.SetPenStyle(CGraphicsContext::ENullPen);
		aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		aGc.SetBrushColor(TRgb(255, 153, 0));
		aGc.DrawEllipse(TRect(px, nameY - f->AscentInPixels() / 2 - 3, px + 6, nameY - f->AscentInPixels() / 2 + 3));
		aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
		aGc.SetPenStyle(CGraphicsContext::ESolidPen);
		}
	aGc.UseFont(small);
	t.Copy(Upto(e.iUser, 40));
	t.Append('@');
	t.Append(Upto(e.iHost, 60));
	if (e.iPort > 0 && e.iPort != 22)
		t.AppendFormat(_L(":%d"), e.iPort);
	TextUtils::ClipToFit(t, *small, right - tx);
	aGc.SetPenColor(l.iSoft);
	aGc.DrawText(t, TPoint(tx, subY));
	// when, and how it went (a dot: filled for logged in, a ring if not)
	aGc.SetPenColor(on ? (iColour ? TRgb(0, 153, 0) : l.iInk) : l.iFaint);
	aGc.DrawText(when, TPoint(right - ww, nameY));
	if (dot)
		{
		TInt cx = right - ww - 9, cy = nameY - small->AscentInPixels() / 2 - 1;
		TRect d(cx - 3, cy - 3, cx + 4, cy + 4);
		TBool good = on || e.iLastResult == EPtResultLoggedIn;
		if (iColour)
			{
			aGc.SetPenStyle(CGraphicsContext::ENullPen);
			aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
			aGc.SetBrushColor(ResultColour(on ? EPtResultLoggedIn : e.iLastResult));
			}
		else if (good)
			{
			aGc.SetPenStyle(CGraphicsContext::ENullPen);
			aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
			aGc.SetBrushColor(TRgb::Gray16(5));
			}
		else
			aGc.SetPenColor(TRgb::Gray16(4));
		aGc.DrawEllipse(d);
		aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
		aGc.SetPenStyle(CGraphicsContext::ESolidPen);
		}
	aGc.DiscardFont();
	}

// Nothing saved yet: a picture and the way in, in the middle
void CPtStartView::DrawEmpty(CWindowGc& aGc) const
	{
	TStartLook l;
	Look(iColour, l);
	TRect r = Rect();
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	TInt cx = (r.iTl.iX + r.iBr.iX) / 2;
	TInt mid = r.iTl.iY + (iListY + iFootY) / 2;
	// a small terminal: a rounded screen with a prompt
	TRect scr(cx - 26, mid - 40, cx + 26, mid - 6);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(iColour ? l.iAccent : TRgb::Gray16(4));
	aGc.DrawRoundRect(scr, TSize(8, 8));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(KRgbWhite);
	aGc.UseFont(f);
	aGc.DrawText(_L(">_"), TPoint(scr.iTl.iX + 10, scr.iTl.iY + 6 + f->AscentInPixels()));
	aGc.SetPenColor(l.iInk);
	TPtrC a(_L("No servers yet"));
	aGc.DrawText(a, TPoint(cx - f->TextWidthInPixels(a) / 2, mid + 6 + f->AscentInPixels()));
	aGc.UseFont(small);
	aGc.SetPenColor(l.iSoft);
	TPtrC b(_L("Ctrl+N adds one - Ctrl+G connects to an address"));
	aGc.DrawText(b, TPoint(cx - small->TextWidthInPixels(b) / 2, mid + 10 + f->HeightInPixels() + small->AscentInPixels()));
	aGc.DiscardFont();
	}

// Below the list: the highlighted server (a line on the 5mx; a card on a
// taller screen)
void CPtStartView::DrawDetail(CWindowGc& aGc) const
	{
	TStartLook l;
	Look(iColour, l);
	TRect r = Rect();
	TRect foot(r.iTl.iX, r.iTl.iY + iFootY, r.iBr.iX, r.iBr.iY);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(l.iPaper);
	aGc.DrawRect(foot);
	TInt h = Current();
	if (h < 0)
		return;
	const CFont* f = iEikonEnv->AnnotationFont();
	const THostEntry& e = iHosts.At(h);
	TBuf<80> login;
	TBuf<130> onLogin;
	Summary(h, login, onLogin);
	TBuf<200> t;
	TInt lh = f->HeightInPixels() + 4;
	if (iFootH < 3 * lh)
		{
		// the 5mx: a quiet line - how it logs in, what it runs, how it went
		aGc.SetPenStyle(CGraphicsContext::ESolidPen);
		aGc.SetPenColor(l.iRule);
		aGc.DrawLine(TPoint(foot.iTl.iX + 8, foot.iTl.iY), TPoint(foot.iBr.iX - 8, foot.iTl.iY));
		t.Copy(login);
		if (onLogin.Length())
			{
			t.Append(_L("  -  runs "));
			t.Append(Upto(onLogin, 60));
			}
		if (e.iLastResult != EPtResultNever)
			{
			t.Append(_L("  -  last time "));
			t.Append(TPtrC(ResultText(e.iLastResult)));
			}
		aGc.UseFont(f);
		aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
		aGc.SetPenColor(l.iSoft);
		TextUtils::ClipToFit(t, *f, foot.Width() - 20);
		aGc.DrawText(t, TPoint(foot.iTl.iX + 10, foot.iTl.iY + 3 + f->AscentInPixels()));
		aGc.DiscardFont();
		return;
		}
	// a taller screen: a card of details
	TRect card(foot.iTl.iX + 8, foot.iTl.iY + 6, foot.iBr.iX - 12, foot.iBr.iY - 8);
	aGc.SetBrushColor(iColour ? TRgb(238, 238, 238) : TRgb::Gray16(14));
	aGc.DrawRoundRect(card, TSize(8, 8));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.UseFont(f);
	TInt x = card.iTl.iX + 12, col2 = card.Width() / 2;
	TInt y = card.iTl.iY + 6 + f->AscentInPixels();
	TInt lw = f->TextWidthInPixels(_L("On login")) + 10;
	TBuf<40> when;
	WhenText(e.iLastUsed, when);
	for (TInt i = 0; i < 5; i++)
		{
		TInt lx = x + (i >= 3 ? col2 : 0);
		TInt ly = y + (i >= 3 ? i - 3 : i) * lh;
		TInt room = i == 2 ? card.Width() - lw - 24 : col2 - lw - 16;
		const TText* label = i == 0 ? _S("Server") : i == 1 ? _S("Address") : i == 2 ? _S("On login")
			: i == 3 ? _S("Last") : _S("Login");
		aGc.SetPenColor(l.iFaint);
		aGc.DrawText(TPtrC(label), TPoint(lx, ly));
		t.Zero();
		if (i == 0)
			t.Copy(Upto(e.iName, 40));
		else if (i == 1)
			{
			t.Copy(Upto(e.iUser, 40));
			t.Append('@');
			t.Append(Upto(e.iHost, 60));
			if (e.iPort > 0 && e.iPort != 22)
				t.AppendFormat(_L(":%d"), e.iPort);
			}
		else if (i == 2)
			{
			if (onLogin.Length())
				t.Copy(onLogin);
			else
				t.Copy(_L("a shell"));
			}
		else if (i == 3)
			{
			t.Copy(when);
			if (e.iLastResult != EPtResultNever)
				{
				t.Append(_L(" - "));
				t.Append(TPtrC(ResultText(e.iLastResult)));
				}
			}
		else
			t.Copy(login);
		aGc.SetPenColor(i == 3 && iColour && e.iLastResult != EPtResultNever ? ResultColour(e.iLastResult) : l.iInk);
		TextUtils::ClipToFit(t, *f, room);
		aGc.DrawText(t, TPoint(lx + lw, ly));
		}
	aGc.DiscardFont();
	}

void CPtStartView::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	TStartLook l;
	Look(iColour, l);
	TRect r = Rect();
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(l.iPaper);
	gc.DrawRect(r);
	// the title: large, on the page itself; the count and the connection
	// quietly at the right
	const CFont* title = iEikonEnv->TitleFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.UseFont(title);
	gc.SetPenColor(l.iInk);
	TInt ty = r.iTl.iY + (iBandH - title->HeightInPixels()) / 2 + title->AscentInPixels();
	gc.DrawText(_L("Servers"), TPoint(r.iTl.iX + 12, ty));
	TBuf<80> state;
	CPsiTermAppUi* ui = (CPsiTermAppUi*)iEikonEnv->EikAppUi();
	TInt live = ui->LiveSessions();
	if (live == 1)
		{
		// (1.04) the one connection, whichever it is
		for (TInt i = 0; i < CPsiTermAppUi::KMaxSessions; i++)
			if (ui->Session(i) && ui->Session(i)->Live())
				{
				TBuf<30> name;
				ui->SessionName(*ui->Session(i), name);
				state.Copy(_L("Connected to "));
				state.Append(name);
				}
		}
	else if (live > 1)
		state.Format(_L("%d connections"), live);
	else if (iCount == 1)
		state.Copy(_L("1 saved"));
	else
		state.Format(_L("%d saved"), iCount);
	gc.UseFont(small);
	gc.SetPenColor(live && iColour ? TRgb(0, 150, 0) : l.iFaint);
	gc.DrawText(state, TPoint(r.iBr.iX - 14 - small->TextWidthInPixels(state), ty));
	gc.DiscardFont();
	if (iCount == 0)
		{
		DrawEmpty(gc);
		return;
		}
	// the rows, with their headings
	TInt y = iListY;
	for (TInt row = iTop; row < iCount; row++)
		{
		if (HeadBefore(row))
			{
			if (y + iSecH + iRowH > iFootY)
				break;
			DrawHead(gc, y, row);
			y += iSecH;
			}
		if (y + iRowH > iFootY)
			break;
		DrawRow(gc, y, row);
		y += iRowH;
		}
	// more above or below: a thin bar at the right
	if (iTop > 0 || LastVisible(iTop) < iCount - 1)
		{
		TInt top = r.iTl.iY + iListY + 2, h = iFootY - iListY - 4;
		TInt y0 = top + h * iTop / iCount, y1 = top + h * (LastVisible(iTop) + 1) / iCount;
		gc.SetPenStyle(CGraphicsContext::ENullPen);
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		gc.SetBrushColor(l.iRule);
		gc.DrawRoundRect(TRect(r.iBr.iX - 5, top, r.iBr.iX - 2, top + h), TSize(2, 2));
		gc.SetBrushColor(l.iFaint);
		gc.DrawRoundRect(TRect(r.iBr.iX - 5, y0, r.iBr.iX - 2, y1), TSize(2, 2));
		}
	DrawDetail(gc);
	}

// ============================================================================
// The AppUi's side
// ============================================================================

void CPsiTermAppUi::OpenStartL()
	{
	if (iStart)
		{
		iStart->ReloadL();
		iStart->DrawNow();
		return;
		}
	if (iBrowse)
		return;                              // (the file view first)
	if (!iStartBar && iToolBar)
		{
		CEikToolBar* bar = new(ELeave) CEikToolBar;
		CleanupStack::PushL(bar);
		bar->ConstructL(this, R_PT_START_TOOLBAR, TRect(TPoint(0, 0), iEikonEnv->ScreenDevice()->SizeInPixels()));
		CleanupStack::Pop();
		iStartBar = bar;
		iStartBar->MakeVisible(EFalse);
		// (1.04) Edit server only where there is room (a 5mx's 240 pixels
		// hold four buttons and the clock)
		if (iEikonEnv->ScreenDevice()->SizeInPixels().iHeight < 320)
			{
			TInt at = iStartBar->IndexById(EPtCmdStEdit);
			if (at >= 0)
				{
				iStartBar->DeleteControl(at, 1);
				TRAP_IGNORE(iStartBar->LayoutControlsL());
				}
			}
		TRAP_IGNORE(
			ButtonPictureL(iStartBar, EPtCmdStConnect, EMbmToolSsh);
			ButtonPictureL(iStartBar, EPtCmdStNew, EMbmToolNew);
			ButtonPictureL(iStartBar, EPtCmdStEdit, EMbmToolEditSrv);
			ButtonPictureL(iStartBar, EPtCmdStPin, EMbmToolPin);
			ButtonPictureL(iStartBar, EPtCmdStTerminal, EMbmToolTerm);
			);
		}
	iStart = CPtStartView::NewL(*iHosts, *iKeys, iTmux, *iView, TermRect(iView->Settings().iToolbar));
	AddToStackL(iStart);
	iStartStacked = ETrue;
	UpdateStackedControlFlags(iView, ECoeStackFlagRefusesAllKeys | ECoeStackFlagRefusesFocus,
		ECoeStackFlagRefusesAllKeys | ECoeStackFlagRefusesFocus);
	iMenuBar->ChangeMenuBarL(R_PT_START_HOTKEYS, R_PT_START_MENUBAR, EFalse);
	iStart->ActivateL();
	if (iToolBar && iStartBar && iView->Settings().iToolbar)
		{
		iToolBar->MakeVisible(EFalse);
		TInt h = iStart->Current();
		iStartBar->SetButtonStateOn(EPtCmdStPin, h >= 0 && (iHosts->At(h).iFlags & KPtHostPinned));
		iStartBar->MakeVisible(ETrue);
		iStartBar->DrawNow();
		}
	iView->SetFocus(EFalse);
	iStart->SetFocus(ETrue);
	iStart->DrawNow();
	}

void CPsiTermAppUi::CloseStart()
	{
	if (!iStart)
		return;
	if (iStartStacked)
		RemoveFromStack(iStart);
	iStartStacked = EFalse;
	delete iStart;
	iStart = NULL;
	UpdateStackedControlFlags(iView, 0, ECoeStackFlagRefusesAllKeys | ECoeStackFlagRefusesFocus);
	TRAP_IGNORE(iMenuBar->ChangeMenuBarL(R_PT_HOTKEYS, R_PT_MENUBAR, EFalse));
	if (iStartBar)
		iStartBar->MakeVisible(EFalse);
	if (iToolBar && iView->Settings().iToolbar)
		{
		iToolBar->MakeVisible(ETrue);
		iToolBar->DrawNow();
		}
	iView->SetFocus(ETrue);
	iView->DrawNow();
	}

// (from an idle object) the Start view, after End SSH
void CPsiTermAppUi::StartSoon()
	{
	if (!iStartIdle)
		iStartIdle = CIdle::New(CActive::EPriorityIdle);
	if (iStartIdle && !iStartIdle->IsActive())
		iStartIdle->Start(TCallBack(StartIdleCallback, this));
	}

TInt CPsiTermAppUi::StartIdleCallback(TAny* aSelf)
	{
	CPsiTermAppUi* ui = (CPsiTermAppUi*)aSelf;
	if (!ui->iView->SshActive())
		TRAP_IGNORE(ui->OpenStartL());
	return 0;
	}

// How the connection to the saved server went (CTermView, as it learns)
void CPsiTermAppUi::HostOutcomeL(TInt aHost, TInt aResult)
	{
	if (aHost < 0 || aHost >= iHosts->Count())
		return;
	THostEntry& e = iHosts->At(aHost);
	if (e.iLastResult == aResult)
		return;
	e.iLastResult = aResult;
	iHosts->Save();
	if (iStart)
		iStart->DrawDeferred();
	}

void CPsiTermAppUi::NewHostL()
	{
	if (iHosts->Count() >= KMaxHosts)
		{
		iEikonEnv->InfoMsg(_L("Server list is full - delete one first"));
		return;
		}
	THostEntry e;
	e.iAuth = iKeys->Count() ? 0 : 1;
	TInt cur = iStart ? iStart->Current() : -1;
	if (cur >= 0)
		e.iUser = iHosts->At(cur).iUser;     // most people reuse a user name
	if (EditHostL(e))
		{
		iHosts->AddL(e);
		iHosts->iLast = iHosts->Count() - 1;
		iHosts->Save();
		if (iStart)
			iStart->ReloadL(iHosts->iLast);
		}
	e.iPassword.FillZ();
	}

void CPsiTermAppUi::EditHostAtL(TInt aIndex)
	{
	if (aIndex < 0 || aIndex >= iHosts->Count())
		{
		iEikonEnv->InfoMsg(_L("No server selected"));
		return;
		}
	THostEntry e = iHosts->At(aIndex);
	if (EditHostL(e))
		{
		iHosts->At(aIndex) = e;
		iHosts->iLast = aIndex;
		iHosts->Save();
		if (iStart)
			iStart->ReloadL(aIndex);
		}
	e.iPassword.FillZ();
	}

void CPsiTermAppUi::DeleteHostL(TInt aIndex)
	{
	if (aIndex < 0 || aIndex >= iHosts->Count())
		{
		iEikonEnv->InfoMsg(_L("No server selected"));
		return;
		}
	TBuf<60> what;
	TPtrC name(Upto(iHosts->At(aIndex).iName, 40));
	what.Format(_L("%S will be deleted"), &name);
	if (!iEikonEnv->QueryWinL(what, _L("Delete server?")))
		return;
	iHosts->Delete(aIndex);
	for (TInt i = 0; i < KMaxSessions; i++)   // (1.04) every connection's server
		if (iSessions[i])
			{
			TInt h = iSessions[i]->HostIndex();
			if (h == aIndex)
				iSessions[i]->SetHostIndex(-1);
			else if (h > aIndex)
				iSessions[i]->SetHostIndex(h - 1);
			}
	iHosts->Save();
	if (iStart)
		iStart->ReloadL(aIndex < iHosts->Count() ? aIndex : iHosts->Count() - 1);
	}

// ----- SSH to address... (Ctrl+G): Muxedo's quick connect ---------------------

class CPtQuickDialog : public CEikDialog
	{
public:
	CPtQuickDialog(THostEntry& aEntry, CKeyList& aKeys, TBool& aSave)
		: iEntry(aEntry), iKeys(aKeys), iSave(aSave) {}
private:
	void PreLayoutDynInitL()
		{
		CDesCArrayFlat* a = new(ELeave) CDesCArrayFlat(8);
		CleanupStack::PushL(a);
		iOpts = 0;
		for (TInt i = 0; i < iKeys.Count() && i < KMaxKeys; i++)
			{
			TBuf<48> l(_L("Key: "));
			l.Append(Upto(iKeys.At(i).iName, 24));
			a->AppendL(l);
			iKey[iOpts++] = iKeys.At(i).iId;
			}
		a->AppendL(_L("Password (ask)"));
		iKey[iOpts++] = -1;
		CEikChoiceList* c = (CEikChoiceList*)Control(EPtDlgQuickAuth);
		c->SetArrayL(a);
		c->SetArrayExternalOwnership(EFalse);
		CleanupStack::Pop();
		c->SetCurrentItem(0);
		}
	TBool OkToExitL(TInt /*aButtonId*/)
		{
		TBuf<170> a;
		GetEdwinText(a, EPtDlgQuickAddr);
		a.Trim();
		TInt at = a.Locate('@');
		if (a.Length() == 0)
			{
			iEikonEnv->InfoMsg(_L("No address entered"));
			return EFalse;
			}
		if (at <= 0 || at >= a.Length() - 1)
			{
			iEikonEnv->InfoMsg(_L("Type it as user@address"));
			return EFalse;
			}
		TPtrC host(a.Mid(at + 1));
		TInt port = 22;
		TInt colon = host.LocateReverse(':');
		if (colon > 0 && colon < host.Length() - 1)
			{
			TLex lex(host.Mid(colon + 1));
			TInt p;
			if (lex.Val(p) == KErrNone && lex.Eos() && p > 0 && p < 65536)
				{
				port = p;
				host.Set(host.Left(colon));
				}
			}
		iEntry.iUser = Upto(a.Left(at), iEntry.iUser.MaxLength());
		iEntry.iHost = Upto(host, iEntry.iHost.MaxLength());
		iEntry.iName = Upto(host, iEntry.iName.MaxLength());
		iEntry.iPort = port;
		TInt opt = ((CEikChoiceList*)Control(EPtDlgQuickAuth))->CurrentItem();
		if (opt < 0 || opt >= iOpts)
			opt = iOpts - 1;
		if (iKey[opt] < 0)
			{
			iEntry.iAuth = 1;
			iEntry.iKeyId = 0;
			}
		else
			{
			iEntry.iAuth = 0;
			iEntry.iKeyId = iKey[opt];
			}
		iSave = ((CEikChoiceList*)Control(EPtDlgQuickSave))->CurrentItem() == 1;
		return ETrue;
		}
	THostEntry& iEntry;
	CKeyList& iKeys;
	TBool& iSave;
	TInt iOpts;
	TInt iKey[KMaxKeys + 1];
	};

void CPsiTermAppUi::QuickConnectL()
	{
	THostEntry e;
	TBool save = EFalse;
	CPtQuickDialog* dlg = new(ELeave) CPtQuickDialog(e, *iKeys, save);
	if (!dlg->ExecuteLD(R_PT_QUICK_DIALOG))
		return;
	if (save && iHosts->Count() < KMaxHosts)
		{
		iHosts->AddL(e);
		iHosts->iLast = iHosts->Count() - 1;
		iHosts->Save();
		ConnectHostL(iHosts->iLast);
		return;
		}
	TInt slot = PickSessionL();              // (1.04) a connection of its own, if need be
	if (slot < 0)
		return;
	if (save)
		iEikonEnv->InfoMsg(_L("Server list is full - connecting without saving"));
	CloseStart();
	UseSessionL(slot);
	iView->SetHostIndex(-1);
	ConnectEntryL(e);
	}

// The Start view's commands; EFalse if aCommand is not one
TBool CPsiTermAppUi::StartCommandL(TInt aCommand)
	{
	TInt cur = iStart ? iStart->Current() : iHosts->iLast;
	switch (aCommand)
		{
	case EPtCmdStConnect:
		if (cur < 0 || cur >= iHosts->Count())
			{
			NewHostL();
			return ETrue;
			}
		ConnectHostL(cur);
		return ETrue;
	case EPtCmdStAddress:
		QuickConnectL();
		return ETrue;
	case EPtCmdStNew:
		NewHostL();
		return ETrue;
	case EPtCmdStEdit:
		EditHostAtL(cur);
		return ETrue;
	case EPtCmdStDelete:
		DeleteHostL(cur);
		return ETrue;
	case EPtCmdStPin:
		if (cur < 0 || cur >= iHosts->Count())
			return ETrue;
		iHosts->At(cur).iFlags ^= KPtHostPinned;
		iHosts->Save();
		if (iStart)
			iStart->ReloadL(cur);
		if (iStartBar)
			{
			iStartBar->SetButtonStateOn(EPtCmdStPin, (iHosts->At(cur).iFlags & KPtHostPinned) != 0);
			CCoeControl* b = iStartBar->ControlById(EPtCmdStPin);
			if (b && iStartBar->IsVisible())
				b->DrawNow();
			}
		return ETrue;
	case EPtCmdStSync:
		if (iStartBar && cur >= 0 && cur < iHosts->Count())
			{
			TBool on = (iHosts->At(cur).iFlags & KPtHostPinned) != 0;
			CEikCommandButton* b = (CEikCommandButton*)iStartBar->ControlById(EPtCmdStPin);
			if (b && b->State() != (on ? CEikButtonBase::ESet : CEikButtonBase::EClear))
				{
				iStartBar->SetButtonStateOn(EPtCmdStPin, on);
				if (iStartBar->IsVisible())
					b->DrawNow();
				}
			}
		return ETrue;
	case EPtCmdStTerminal:
		if (iStart)
			CloseStart();
		else
			OpenStartL();
		return ETrue;
	default:
		return EFalse;
		}
	}
