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
	case EPtResultLoggedIn: return TRgb(0, 150, 0);
	case EPtResultNoAnswer:
	case EPtResultLost:     return TRgb(220, 140, 0);
	default:                return TRgb(200, 0, 0);
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
void CPtStartView::Layout()
	{
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	TInt w = Rect().Width(), h = Rect().Height();
	iBandH = f->HeightInPixels() + 6;
	iHeadH = small->HeightInPixels() + 4;
	iRowH = f->HeightInPixels() + 4;
	// the details: a line on the 5mx, four on a taller screen
	iFootH = h >= 400 ? 4 * (small->HeightInPixels() + 4) + 8 : small->HeightInPixels() + 6;
	iListY = iBandH + iHeadH;
	iRows = (h - iListY - iFootH) / iRowH;
	if (iRows < 1)
		iRows = 1;
	iFootY = iListY + iRows * iRowH;
	iColName = 22;
	iColAddr = iColName + w * 26 / 100;
	iColLast = w - w * 22 / 100;
	iColEnd = w - 4;
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
	if (iSel < iTop)
		iTop = iSel;
	if (iSel >= iTop + iRows)
		iTop = iSel - iRows + 1;
	if (iTop > iCount - iRows)
		iTop = iCount - iRows;
	if (iTop < 0)
		iTop = 0;
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
	if (iSel >= iTop + iRows)
		iTop = iSel - iRows + 1;
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
	TInt y = aEvent.iPosition.iY - Rect().iTl.iY;
	if (y < iListY || y >= iFootY)
		return;
	TInt r = iTop + (y - iListY) / iRowH;
	if (r >= iCount)
		return;
	if (r == iSel)
		Command(EPtCmdStConnect);   // (a tap on the highlight connects)
	else
		Move(r);
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

void CPtStartView::DrawRow(CWindowGc& aGc, TInt aRow) const
	{
	TInt r = aRow - iTop;
	if (r < 0 || r >= iRows)
		return;
	TRect row(Rect().iTl.iX, Rect().iTl.iY + iListY + r * iRowH, Rect().iBr.iX, Rect().iTl.iY + iListY + (r + 1) * iRowH);
	TBool sel = (aRow == iSel) && aRow < iCount;
	TRgb paper = KRgbWhite, ink = KRgbBlack, grey = TRgb::Gray16(7);
	if (sel)
		{
		paper = iColour ? TRgb(0, 0, 128) : KRgbBlack;
		ink = KRgbWhite;
		grey = iColour ? TRgb(200, 200, 255) : TRgb::Gray16(12);
		}
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(paper);
	aGc.DrawRect(row);
	if (aRow >= iCount)
		return;
	const THostEntry& e = iHosts.At(iOrder[aRow]);
	const CFont* f = iEikonEnv->NormalFont();
	aGc.UseFont(f);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	TInt base = row.iTl.iY + 2 + f->AscentInPixels();
	TInt x0 = Rect().iTl.iX;
	// the pin
	if ((e.iFlags & KPtHostPinned) && iStar && iStarMask)
		{
		TSize sz = iStar->SizeInPixels();
		TPoint at(x0 + 3, row.iTl.iY + (iRowH - sz.iHeight) / 2);
		if (sel || iColour)
			{
			// (on a highlight, or in colour: a filled mark that reads on it)
			aGc.SetPenStyle(CGraphicsContext::ENullPen);
			aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
			aGc.SetBrushColor(iColour ? TRgb(240, 170, 0) : KRgbWhite);
			TInt cy = row.iTl.iY + iRowH / 2;
			aGc.DrawEllipse(TRect(x0 + 5, cy - 5, x0 + 16, cy + 6));
			aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
			aGc.SetPenStyle(CGraphicsContext::ESolidPen);
			}
		else
			aGc.BitBltMasked(at, iStar, TRect(sz), iStarMask, ETrue);
		}
	TBuf<120> t;
	aGc.SetPenColor(ink);
	t.Copy(Upto(e.iName, 40));
	TextUtils::ClipToFit(t, *f, iColAddr - iColName - 8);
	aGc.DrawText(t, TPoint(x0 + iColName, base));
	aGc.SetPenColor(grey);
	t.Copy(Upto(e.iUser, 40));
	t.Append('@');
	t.Append(Upto(e.iHost, 60));
	if (e.iPort > 0 && e.iPort != 22)
		t.AppendFormat(_L(":%d"), e.iPort);
	TextUtils::ClipToFit(t, *f, iColLast - iColAddr - 8);
	aGc.DrawText(t, TPoint(x0 + iColAddr, base));
	WhenText(e.iLastUsed, t);
	TInt lx = x0 + iColLast;
	if (iColour && e.iLastResult != EPtResultNever)
		{
		// how it went, as a coloured mark before the time
		aGc.SetPenStyle(CGraphicsContext::ENullPen);
		aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		aGc.SetBrushColor(ResultColour(e.iLastResult));
		TInt cy = row.iTl.iY + iRowH / 2;
		aGc.DrawRect(TRect(lx - 12, cy - 4, lx - 4, cy + 4));
		aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
		aGc.SetPenStyle(CGraphicsContext::ESolidPen);
		}
	aGc.SetPenColor(grey);
	TextUtils::ClipToFit(t, *f, iColEnd - lx);
	aGc.DrawText(t, TPoint(lx, base));
	aGc.DiscardFont();
	}

// Below the list: the highlighted server's details
void CPtStartView::DrawDetail(CWindowGc& aGc) const
	{
	TRect foot(Rect().iTl.iX, Rect().iTl.iY + iFootY, Rect().iBr.iX, Rect().iBr.iY);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(KRgbWhite);
	aGc.DrawRect(foot);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(TRgb::Gray16(8));
	aGc.DrawLine(foot.iTl, TPoint(foot.iBr.iX, foot.iTl.iY));
	const CFont* f = iEikonEnv->AnnotationFont();
	aGc.UseFont(f);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	TInt x = foot.iTl.iX + 6, w = foot.Width() - 12;
	TInt lh = f->HeightInPixels() + 4;
	TInt y = foot.iTl.iY + 3 + f->AscentInPixels();
	TInt h = Current();
	if (h < 0)
		{
		aGc.SetPenColor(TRgb::Gray16(5));
		aGc.DrawText(_L("No saved servers - Ctrl+N creates one"), TPoint(x, y));
		aGc.DiscardFont();
		return;
		}
	const THostEntry& e = iHosts.At(h);
	TBuf<80> login;
	TBuf<130> onLogin;
	Summary(h, login, onLogin);
	TBuf<40> when;
	WhenText(e.iLastUsed, when);
	TBuf<200> t;
	if (iFootH < 3 * lh)
		{
		// the 5mx: one line
		t.Copy(Upto(e.iHost, 60));
		if (e.iPort > 0 && e.iPort != 22)
			t.AppendFormat(_L(":%d"), e.iPort);
		t.Append(_L(" - "));
		t.Append(login);
		if (onLogin.Length())
			{
			t.Append(_L(" - runs "));
			t.Append(Upto(onLogin, 60));
			}
		if (e.iLastResult != EPtResultNever)
			{
			t.Append(_L(" - last: "));
			t.Append(TPtrC(ResultText(e.iLastResult)));
			}
		aGc.SetPenColor(TRgb::Gray16(5));
		TextUtils::ClipToFit(t, *f, w);
		aGc.DrawText(t, TPoint(x, y));
		aGc.DiscardFont();
		return;
		}
	// a taller screen: label and value, in two columns
	TInt col2 = w / 2;
	TInt lw = f->TextWidthInPixels(_L("On login ")) + 4;
	struct TLine { const TText* iLabel; TInt iCol; TInt iRow; };
	const TLine lines[] = {
		{ _S("Server"), 0, 0 }, { _S("Address"), 0, 1 }, { _S("On login"), 0, 2 },
		{ _S("Last"), 1, 0 }, { _S("Login"), 1, 1 } };
	for (TUint i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
		{
		TInt lx = x + lines[i].iCol * col2;
		TInt ly = y + lines[i].iRow * lh;
		TInt room = (lines[i].iCol == 0 && lines[i].iRow == 2) ? w - lw : col2 - lw - 8;
		aGc.SetPenColor(TRgb::Gray16(6));
		aGc.DrawText(TPtrC(lines[i].iLabel), TPoint(lx, ly));
		t.Zero();
		switch (i)
			{
		case 0: t.Copy(Upto(e.iName, 40)); break;
		case 1:
			t.Copy(Upto(e.iUser, 40));
			t.Append('@');
			t.Append(Upto(e.iHost, 60));
			if (e.iPort > 0 && e.iPort != 22)
				t.AppendFormat(_L(":%d"), e.iPort);
			break;
		case 2: if (onLogin.Length()) t.Copy(onLogin); else t.Copy(_L("(a shell)")); break;
		case 3:
			t.Copy(when);
			if (e.iLastResult != EPtResultNever)
				{
				t.Append(_L(" - "));
				t.Append(TPtrC(ResultText(e.iLastResult)));
				}
			break;
		default: t.Copy(login); break;
			}
		aGc.SetPenColor(i == 3 && iColour && e.iLastResult != EPtResultNever ? ResultColour(e.iLastResult) : KRgbBlack);
		TextUtils::ClipToFit(t, *f, room);
		aGc.DrawText(t, TPoint(lx + lw, ly));
		}
	aGc.DiscardFont();
	}

void CPtStartView::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	TRect r = Rect();
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	// the title band: as the file view's
	TRect band(r.iTl, TPoint(r.iBr.iX, r.iTl.iY + iBandH));
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(iColour ? TRgb(0, 0, 128) : TRgb::Gray16(4));
	gc.DrawRect(band);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbWhite);
	gc.UseFont(f);
	TInt by = band.iTl.iY + 3 + f->AscentInPixels();
	gc.DrawText(_L("Saved servers"), TPoint(r.iTl.iX + 6, by));
	TBuf<80> state;
	if (iTerm.SshActive())
		{
		state.Copy(_L("SSH: "));
		state.Append(Upto(iTerm.Settings().iSshHost, 40));
		}
	else
		state.Copy(_L("Not connected"));
	gc.UseFont(small);
	gc.DrawText(state, TPoint(r.iBr.iX - 6 - small->TextWidthInPixels(state), band.iTl.iY + 3 + small->AscentInPixels() + (f->AscentInPixels() - small->AscentInPixels())));
	// the column heads
	TRect head(r.iTl.iX, band.iBr.iY, r.iBr.iX, band.iBr.iY + iHeadH);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.DrawRect(head);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(TRgb::Gray16(6));
	TInt hy = head.iTl.iY + 2 + small->AscentInPixels();
	gc.DrawText(_L("Name"), TPoint(r.iTl.iX + iColName, hy));
	gc.DrawText(_L("Address"), TPoint(r.iTl.iX + iColAddr, hy));
	gc.DrawText(_L("Last used"), TPoint(r.iTl.iX + iColLast, hy));
	gc.SetPenColor(TRgb::Gray16(8));
	gc.DrawLine(TPoint(r.iTl.iX, head.iBr.iY - 1), TPoint(r.iBr.iX, head.iBr.iY - 1));
	gc.DiscardFont();
	for (TInt i = 0; i < iRows; i++)
		DrawRow(gc, iTop + i);
	// a scroll mark when there are more rows than fit
	if (iCount > iRows)
		{
		TInt top = r.iTl.iY + iListY, h = iRows * iRowH;
		TInt y0 = top + h * iTop / iCount, y1 = top + h * (iTop + iRows) / iCount;
		gc.SetPenStyle(CGraphicsContext::ENullPen);
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		gc.SetBrushColor(TRgb::Gray16(10));
		gc.DrawRect(TRect(r.iBr.iX - 4, top, r.iBr.iX, top + h));
		gc.SetBrushColor(TRgb::Gray16(4));
		gc.DrawRect(TRect(r.iBr.iX - 4, y0, r.iBr.iX, y1));
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
		TRAP_IGNORE(
			ButtonPictureL(iStartBar, EPtCmdStConnect, EMbmToolSsh);
			ButtonPictureL(iStartBar, EPtCmdStNew, EMbmToolNew);
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
void CPsiTermAppUi::HostOutcomeL(TInt aResult)
	{
	if (iConnHost < 0 || iConnHost >= iHosts->Count())
		return;
	THostEntry& e = iHosts->At(iConnHost);
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
	if (iConnHost == aIndex)
		iConnHost = -1;
	else if (iConnHost > aIndex)
		iConnHost--;
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
	if (iView->SshActive())
		{
		iEikonEnv->InfoMsg(_L("Not available while SSH is connected"));
		return;
		}
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
	if (save)
		iEikonEnv->InfoMsg(_L("Server list is full - connecting without saving"));
	CloseStart();
	iConnHost = -1;
	ConnectEntryL(e);
	}

// The Start view's commands; EFalse if aCommand is not one
TBool CPsiTermAppUi::StartCommandL(TInt aCommand)
	{
	TInt cur = iStart ? iStart->Current() : iHosts->iLast;
	switch (aCommand)
		{
	case EPtCmdStConnect:
		if (iView->SshActive())
			{
			iEikonEnv->InfoMsg(_L("Not available while SSH is connected"));
			return ETrue;
			}
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
