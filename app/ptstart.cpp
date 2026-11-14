// PTSTART.CPP - the Start view (1.02, from Muxedo's landing page)
//
// PsiTerm opens on the saved servers, and SSH to... (Shift+Ctrl+S, the
// toolbar's first button) comes back to them. Pinned servers first, in their
// saved order, then (1.06, as Muxedo's folders) each folder under a heading
// that opens and shuts, then the rest by when they were last used; each row
// is the server's badge (its picture, or its initial), the name,
// user@address and when it was last used. Below the list, the
// highlighted server's details (one line on the 5mx; a panel on the
// Series 7, which has the room), with how its last connection went.
//
// Enter (or a tap on the highlighted row) connects. 1.08: typing goes in
// the field at the top and only the servers with that text stay; each row
// has a "..." button (Tab) for the server's own menu; the sections are
// always Pinned, Recent, the folders and the rest, with "Add server" last.
// The view goes over the terminal as the file view does (ptxfer.cpp), with
// its own menus, shortcuts and toolbar; View > Show terminal (Ctrl+Q) goes
// back to the terminal.

#include "psiterm.h"
#include "pticons.h"
#include "pthosticons.h"
#include <eikfctry.h>       // SEikControlInfo
#include <barsread.h>       // TResourceReader
#include <eikchlst.h>
#include <eiktbar.h>
#include <eikcmbut.h>
#include <eikmenub.h>
#include <eiktxtut.h>
#include <eikcfdlg.h>       // CEikFileOpenDialog
#include <eikon.rsg>
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
// The servers' pictures (1.06): 16 x 16 in one colour, kept in the program
// (pthosticons.h, drawn as text in tools/mkhosticons.py)
// ============================================================================

void PtDrawHostIcon(CGraphicsContext& aGc, TInt aIcon, const TPoint& aAt, TRgb aColour)
	{
	if (aIcon < 1 || aIcon > KPtHostIconCount)
		return;
	const TUint16* rows = KPtHostIconBits[aIcon - 1];
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(aColour);
	for (TInt y = 0; y < 16; y++)
		{
		TUint bits = rows[y];
		TInt x = 0;
		while (x < 16)
			{
			if (!(bits & (0x8000u >> x)))
				{
				x++;
				continue;
				}
			TInt e = x;                      // (a run of dots: one rectangle)
			while (e < 16 && (bits & (0x8000u >> e)))
				e++;
			aGc.DrawRect(TRect(aAt.iX + x, aAt.iY + y, aAt.iX + e, aAt.iY + y + 1));
			x = e;
			}
		}
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	}

// a server's badge colour (Series 7, netBook): from its name, so it keeps it
TRgb PtHostBadgeColour(const TDesC& aName)
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
	delete iFolder;
	delete iFolderMask;
	delete iFolderOpen;
	delete iFolderOpenMask;
	}

void CPtStartView::ConstructL(const TRect& aRect)
	{
	CreateWindowL();
	iColour = iEikonEnv->ScreenDevice()->DisplayMode() >= EColor16;
	TFileName mbm = iEikonEnv->EikAppUi()->Application()->BitmapStoreName();
	TRAP_IGNORE(
		iFolder = iEikonEnv->CreateBitmapL(mbm, EMbmSmFolder);
		iFolderMask = iEikonEnv->CreateBitmapL(mbm, EMbmSmFolderMask);
		iFolderOpen = iEikonEnv->CreateBitmapL(mbm, EMbmSmFolderOpen);
		iFolderOpenMask = iEikonEnv->CreateBitmapL(mbm, EMbmSmFolderOpenMask);
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

// Rows and their heights for the room there is
void CPtStartView::Layout()
	{
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	const CFont* title = iEikonEnv->TitleFont();
	TInt h = Rect().Height();
	TBool tall = h >= 400;
	iBandH = title->HeightInPixels() + (tall ? 18 : 6);
	// a heading: a folder's name in the list's own font
	iSecH = f->HeightInPixels() + (tall ? 12 : 4);
	// two lines a server: the name, then user@address
	iRowH = f->HeightInPixels() + small->HeightInPixels() + (tall ? 16 : 6);
	iFootH = tall ? 4 * (small->HeightInPixels() + 4) + 22 : small->HeightInPixels() + 6;
	iAddH = f->HeightInPixels() + (tall ? 16 : 6);
	iMoreW = tall ? 30 : 22;
	iListY = iBandH;
	iFootY = h - iFootH;
	iRows = (iFootY - iListY) / iRowH;
	if (iRows < 1)
		iRows = 1;
	}

// What CHostList::Shut knows a heading by: a folder's name, or the two
// that are not folders
static void RowKey(TInt aKind, const TDesC& aFolder, TDes& aKey)
	{
	if (aKind == 0)
		aKey = KPtShutPinned;
	else if (aKind == 1)
		aKey = KPtShutRecent;
	else if (aKind == 3)
		aKey = KPtShutRest;
	else
		aKey = Upto(aFolder, aKey.MaxLength());
	}

void CPtStartView::HeadKey(TInt aRow, TDes& aKey) const
	{
	aKey.Zero();
	if (IsHead(aRow))
		RowKey(iRow[aRow].iKind, iHosts.At(iRow[aRow].iRef).iFolder, aKey);
	}

void CPtStartView::HeadName(TInt aRow, TDes& aName) const
	{
	aName.Zero();
	if (!IsHead(aRow))
		return;
	if (iRow[aRow].iKind == EHeadPinned)
		aName = _L("Pinned");
	else if (iRow[aRow].iKind == EHeadRecent)
		aName = _L("Recent");
	else if (iRow[aRow].iKind == EHeadRest)
		aName = iFolders > 0 ? _L("Other servers") : _L("All servers");
	else
		aName = Upto(iHosts.At(iRow[aRow].iRef).iFolder, aName.MaxLength());
	}

// (1.08) Does the server have the text typed in the field at the top: in
// its name, address, user or folder?
TBool CPtStartView::Matches(const THostEntry& aEntry) const
	{
	if (iFilter.Length() == 0)
		return ETrue;
	return aEntry.iName.FindF(iFilter) >= 0 || aEntry.iHost.FindF(iFilter) >= 0
		|| aEntry.iUser.FindF(iFilter) >= 0 || aEntry.iFolder.FindF(iFilter) >= 0;
	}

// One heading and its servers: the pinned ones in their saved order, a
// folder's (or the rest) most recently used first. A pinned server is under
// Pinned only. Recent (1.08) is the few used last, whichever folder they
// are in: they are under their folder as well.
void CPtStartView::AddSectionL(TInt aKind, const TDesC& aFolder, TBool aHeads)
	{
	TInt list[KMaxHosts];
	TInt n = 0;
	TBool finding = iFilter.Length() > 0;
	if (aKind == EHeadRecent && finding)
		return;                              // (what is found is listed once)
	for (TInt i = 0; i < iServers; i++)
		{
		const THostEntry& e = iHosts.At(i);
		TBool pinned = (e.iFlags & KPtHostPinned) != 0;
		if (aKind == EHeadPinned ? !pinned : pinned)
			continue;
		if (!Matches(e))
			continue;
		if (aKind == EHeadRecent && e.iLastUsed == 0)
			continue;
		if (aKind == EHeadFolder && e.iFolder.CompareF(aFolder) != 0)
			continue;
		if (aKind == EHeadRest && e.iFolder.Length() > 0)
			continue;
		TInt at = n;
		if (aKind != EHeadPinned)
			while (at > 0 && iHosts.At(list[at - 1]).iLastUsed < e.iLastUsed)
				{
				list[at] = list[at - 1];
				at--;
				}
		list[at] = i;
		n++;
		}
	const TInt KRecent = Rect().Height() >= 400 ? 4 : 3;
	if (aKind == EHeadRecent && n > KRecent)
		n = KRecent;
	if (n == 0)
		return;
	TBool shut = EFalse;
	if (aHeads && iCount < KMaxRows)
		{
		TBuf<KPtFolderLen> key;
		RowKey(aKind, aFolder, key);
		shut = iHosts.Shut(key) && !finding;
		TRow& h = iRow[iCount++];
		h.iHost = -1;
		h.iKind = aKind;
		h.iRef = list[0];
		h.iCount = n;
		h.iShut = shut;
		}
	for (TInt k = 0; k < n && !shut && iCount < KMaxRows; k++)
		{
		TRow& r = iRow[iCount++];
		r.iHost = list[k];
		r.iKind = aKind;
		r.iRef = list[k];
		r.iCount = 0;
		r.iShut = EFalse;
		}
	}

// Pinned first, then Recent (the few used last), then each folder (in
// alphabetical order), then the servers in no folder ("All servers" when
// there are no folders), and last the "Add server" row. With text typed in
// the field, only the servers that have it.
void CPtStartView::ReloadL(TInt aSelectHost)
	{
	// what stays highlighted: a server, or the heading it was on
	TInt keep = aSelectHost >= 0 ? aSelectHost : Current();
	TBuf<KPtFolderLen> keepKey;
	if (keep < 0)
		HeadKey(iSel, keepKey);
	iCount = 0;
	iServers = iHosts.Count() < KMaxHosts ? iHosts.Count() : KMaxHosts;
	TInt folder[KMaxHosts];                  // a server of each folder in use
	TInt folders = 0;
	TBool pinned = EFalse, rest = EFalse;
	for (TInt i = 0; i < iServers; i++)
		{
		const THostEntry& e = iHosts.At(i);
		if (e.iFlags & KPtHostPinned)
			{
			pinned = ETrue;
			continue;
			}
		if (e.iFolder.Length() == 0)
			{
			rest = ETrue;
			continue;
			}
		if (!Matches(e))
			continue;
		TBool seen = EFalse;
		for (TInt k = 0; k < folders; k++)
			if (iHosts.At(folder[k]).iFolder.CompareF(e.iFolder) == 0)
				seen = ETrue;
		if (seen)
			continue;
		TInt at = folders;
		while (at > 0 && iHosts.At(folder[at - 1]).iFolder.CompareF(e.iFolder) > 0)
			{
			folder[at] = folder[at - 1];
			at--;
			}
		folder[at] = i;
		folders++;
		}
	iFolders = folders;
	(void)pinned;
	(void)rest;
	TBool heads = ETrue;                     // (1.08: always, so the sections read the same every time)
	AddSectionL(EHeadPinned, KNullDesC, heads);
	AddSectionL(EHeadRecent, KNullDesC, heads);
	for (TInt k = 0; k < folders; k++)
		AddSectionL(EHeadFolder, iHosts.At(folder[k]).iFolder, heads);
	AddSectionL(EHeadRest, KNullDesC, heads);
	if (iCount < KMaxRows)
		{
		TRow& add = iRow[iCount++];          // the "Add server" row, always last
		add.iHost = -2;
		add.iKind = add.iRef = add.iCount = 0;
		add.iShut = EFalse;
		}
	// the highlight: the server; its heading if that is shut; the heading
	// it was on; else the first server
	iSel = -1;
	for (TInt r = 0; r < iCount && keep >= 0 && iSel < 0; r++)
		if (iRow[r].iHost == keep)
			iSel = r;
	if (iSel < 0 && keep >= 0 && keep < iServers)
		{
		const THostEntry& e = iHosts.At(keep);
		TBuf<KPtFolderLen> key;
		RowKey((e.iFlags & KPtHostPinned) ? EHeadPinned : e.iFolder.Length() ? EHeadFolder : EHeadRest,
			e.iFolder, key);
		keepKey = key;
		}
	for (TInt r = 0; r < iCount && iSel < 0 && keepKey.Length(); r++)
		{
		TBuf<KPtFolderLen> key;
		HeadKey(r, key);
		if (key.CompareF(keepKey) == 0)
			iSel = r;
		}
	for (TInt r = 0; r < iCount && iSel < 0; r++)
		if (!IsHead(r))
			iSel = r;                        // (the first server; the "Add server" row with none)
	if (iSel < 0)
		iSel = 0;
	Layout();
	if (iTop >= iCount)
		iTop = 0;
	if (iSel < iTop)
		iTop = iSel;
	while (iTop < iSel && LastVisible(iTop) < iSel)
		iTop++;
	if (iTop == 1 && IsHead(0) && LastVisible(0) >= iSel)
		iTop = 0;                            // (the first heading too, when it fits)
	DrawDeferred();
	}

TInt CPtStartView::Current() const
	{
	return iCount > 0 && iSel >= 0 && iSel < iCount && iRow[iSel].iHost >= 0 ? iRow[iSel].iHost : -1;
	}

// The field at the top: what is typed there changed
void CPtStartView::FilterChangedL()
	{
	iTop = 0;
	ReloadL();
	DrawNow();
	Command(EPtCmdStSync);
	}

void CPtStartView::ClearFilter()
	{
	if (iFilter.Length() == 0)
		return;
	iFilter.Zero();
	TRAP_IGNORE(FilterChangedL());
	}

TBool CPtStartView::RowMenuPos(TPoint& aPos) const
	{
	if (Current() < 0)
		return EFalse;
	TInt y = iListY;
	for (TInt r = iTop; r < iSel; r++)
		y += RowHeight(r);
	aPos = PositionRelativeToScreen() + TPoint(Rect().Width() - 14, y + iRowH - 4);
	return ETrue;
	}

TBool CPtStartView::CurrentFolder(TDes& aName) const
	{
	aName.Zero();
	if (iSel < 0 || iSel >= iCount)
		return EFalse;
	if (IsAdd(iSel) || (IsHead(iSel) && iRow[iSel].iKind != EHeadFolder))
		return EFalse;
	aName = Upto(iHosts.At(iRow[iSel].iRef).iFolder, aName.MaxLength());
	return aName.Length() > 0;
	}

// Opens or shuts a heading; remembered with the servers (Hosts.dat)
void CPtStartView::ToggleL(TInt aRow)
	{
	if (!IsHead(aRow))
		return;
	TBuf<KPtFolderLen> key;
	HeadKey(aRow, key);
	iHosts.SetShut(key, !iRow[aRow].iShut);
	iHosts.Save();
	iSel = aRow;
	ReloadL();
	DrawNow();
	Command(EPtCmdStSync);
	}

TBool CPtStartView::ActivateHeadL()
	{
	if (!IsHead(iSel))
		return EFalse;
	ToggleL(iSel);
	return ETrue;
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
	if (iTop == 1 && IsHead(0) && LastVisible(0) >= iSel)
		iTop = 0;
	if (Current() >= 0)
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
	case EKeyLeftArrow:
		// (1.06) as a tree: Left shuts a heading, or goes to a server's
		if (IsHead(iSel))
			{
			if (!iRow[iSel].iShut)
				ToggleL(iSel);
			}
		else
			for (TInt r = iSel - 1; r >= 0; r--)
				if (IsHead(r))
					{
					Move(r);
					break;
					}
		return EKeyWasConsumed;
	case EKeyRightArrow:
		if (IsHead(iSel))
			{
			if (iRow[iSel].iShut)
				ToggleL(iSel);
			else
				Move(iSel + 1);
			}
		return EKeyWasConsumed;
	case EKeyEnter:
		if (IsAdd(iSel))
			Command(EPtCmdStNew);
		else
			Command(EPtCmdStConnect);        // (a heading: opens or shuts it)
		return EKeyWasConsumed;
	case EKeyEscape:
		if (iFilter.Length())
			ClearFilter();                   // (first what was typed goes, then the view)
		else
			Command(EPtCmdStTerminal);
		return EKeyWasConsumed;
	case EKeyTab:
		Command(EPtCmdStRowMenu);            // (1.08) the server's own menu, as its "..." button
		return EKeyWasConsumed;
	case EKeyBackspace:
		if (iFilter.Length())
			{
			iFilter.SetLength(iFilter.Length() - 1);
			FilterChangedL();
			}
		return EKeyWasConsumed;
	default:
		break;
		}
	// (1.08) anything typed goes in the field at the top: only the servers
	// with it in their name, address, user or folder stay in the list
	if (aKeyEvent.iCode >= 0x20 && aKeyEvent.iCode < 0x100 && aKeyEvent.iCode != 0x7f)
		{
		if (iFilter.Length() < iFilter.MaxLength() && !(aKeyEvent.iCode == ' ' && iFilter.Length() == 0))
			{
			iFilter.Append((TText)aKeyEvent.iCode);
			FilterChangedL();
			}
		return EKeyWasConsumed;
		}
	return EKeyWasNotConsumed;
	}

void CPtStartView::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	if (aEvent.iType != TPointerEvent::EButton1Down)
		return;
	if (iServers == 0)
		{
		Command(EPtCmdStNew);       // (nothing saved: the whole page is "Add server")
		return;
		}
	TInt r = HitTest(aEvent.iPosition.iY - Rect().iTl.iY);
	if (r < 0)
		return;
	if (IsHead(r))
		ToggleL(r);                 // (a tap on a heading opens or shuts it)
	else if (IsAdd(r))
		{
		Move(r);
		Command(EPtCmdStNew);
		}
	else if (aEvent.iPosition.iX >= Rect().iBr.iX - iMoreW - 18)
		{
		Move(r);                    // (1.08) its "..." button: the server's menu
		Command(EPtCmdStRowMenu);
		}
	else if (r == iSel)
		Command(EPtCmdStConnect);   // (a tap on the highlight connects)
	else
		Move(r);
	}

TInt CPtStartView::LastVisible(TInt aTop) const
	{
	TInt y = iListY, last = aTop;
	for (TInt r = aTop; r < iCount; r++)
		{
		TInt h = RowHeight(r);
		if (y + h > iFootY)
			break;
		y += h;
		last = r;
		}
	return last;
	}

TInt CPtStartView::HitTest(TInt aY) const
	{
	TInt y = iListY;
	for (TInt r = iTop; r < iCount; r++)
		{
		TInt h = RowHeight(r);
		if (y + h > iFootY)
			break;
		if (aY >= y && aY < y + h)
			return r;
		y += h;
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
// folders as headings, two-line rows with a badge, soft rounded highlights.
// On the 5mx in greys; on the Series 7 and netBook with colour (each
// server's badge its own).

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

// A heading: an arrow (down when open, right when shut), a folder's
// picture, the name, and how many servers it has
void CPtStartView::DrawHead(CWindowGc& aGc, TInt aY, TInt aRow) const
	{
	TStartLook l;
	Look(iColour, l);
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	TRect r = Rect();
	TBool tall = iRowH >= 40;
	TInt pad = tall ? 8 : 4;
	TRect line(r.iTl.iX, r.iTl.iY + aY, r.iBr.iX, r.iTl.iY + aY + iSecH);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(l.iPaper);
	aGc.DrawRect(line);
	TBool sel = (aRow == iSel);
	TRect card(line.iTl.iX + pad, line.iTl.iY + 1, line.iBr.iX - pad - 4, line.iBr.iY - 1);
	if (sel)
		{
		aGc.SetBrushColor(l.iHigh);
		aGc.DrawRoundRect(card, TSize(6, 6));
		}
	const TRow& h = iRow[aRow];
	TBool folder = h.iKind == EHeadFolder;
	TInt cx = card.iTl.iX + (tall ? 14 : 11), cy = (line.iTl.iY + line.iBr.iY) / 2;
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(folder ? l.iInk : l.iSoft);
	for (TInt i = 0; i < 4; i++)
		{
		if (h.iShut)
			aGc.DrawLine(TPoint(cx - 2 + i, cy - 3 + i), TPoint(cx - 2 + i, cy + 4 - i));
		else
			aGc.DrawLine(TPoint(cx - 3 + i, cy - 2 + i), TPoint(cx + 4 - i, cy - 2 + i));
		}
	TInt tx = cx + 10;
	CFbsBitmap* bmp = h.iShut ? iFolder : iFolderOpen;
	CFbsBitmap* mask = h.iShut ? iFolderMask : iFolderOpenMask;
	if (folder && bmp && mask)
		{
		TSize s = bmp->SizeInPixels();
		aGc.SetBrushColor(KRgbWhite);        // (BitBltMasked uses the brush)
		aGc.BitBltMasked(TPoint(tx, cy - s.iHeight / 2), bmp, TRect(s), mask, ETrue);
		tx += s.iWidth + 6;
		}
	TBuf<24> name;
	HeadName(aRow, name);
	TBuf<12> count;
	count.Num(h.iCount);
	TInt right = card.iBr.iX - 8;
	TInt base = line.iTl.iY + (iSecH - f->HeightInPixels()) / 2 + f->AscentInPixels();
	aGc.UseFont(f);
	aGc.SetPenColor(folder ? l.iInk : l.iSoft);
	TextUtils::ClipToFit(name, *f, right - small->TextWidthInPixels(count) - 12 - tx);
	aGc.DrawText(name, TPoint(tx, base));
	aGc.UseFont(small);
	aGc.SetPenColor(l.iFaint);
	aGc.DrawText(count, TPoint(right - small->TextWidthInPixels(count), base));
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
	TInt host = iRow[aRow].iHost;
	const THostEntry& e = iHosts.At(host);
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
	// the badge: a rounded square with the server's picture, or the name's
	// first letter
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	TInt bs = iRowH - (tall ? 16 : 10);
	if (bs < 18)
		bs = 18;                             // (room for a 16-dot picture)
	TInt bx = card.iTl.iX + (tall ? 12 : 9), by = row.iTl.iY + (iRowH - bs) / 2;
	TRect badge(bx, by, bx + bs, by + bs);
	aGc.SetBrushColor(iColour ? PtHostBadgeColour(e.iName) : TRgb::Gray16(sel ? 3 : 5));
	aGc.DrawRoundRect(badge, TSize(bs / 4, bs / 4));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.UseFont(f);
	if (e.iIcon >= 1 && e.iIcon <= KPtHostIconCount)
		PtDrawHostIcon(aGc, e.iIcon, TPoint(bx + (bs - 16) / 2, by + (bs - 16) / 2), KRgbWhite);
	else
		{
		TBuf<2> init;
		if (e.iName.Length())
			{
			TChar c = e.iName[0];
			c.UpperCase();
			init.Append(c);
			}
		aGc.SetPenColor(KRgbWhite);
		aGc.DrawText(init, TPoint(bx + (bs - f->TextWidthInPixels(init)) / 2, by + (bs + f->AscentInPixels()) / 2 - 1));
		}
	// (1.08) its menu's button at the right: three dots in a rounded box
	{
	TRect more(card.iBr.iX - iMoreW - 6, row.iTl.iY + (iRowH - (tall ? 24 : 18)) / 2, card.iBr.iX - 6, 0);
	more.iBr.iY = more.iTl.iY + (tall ? 24 : 18);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(sel ? l.iSoft : l.iRule);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(sel ? l.iPaper : l.iPaper);
	aGc.DrawRoundRect(more, TSize(4, 4));
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushColor(l.iSoft);
	TInt mx = (more.iTl.iX + more.iBr.iX) / 2, my = (more.iTl.iY + more.iBr.iY) / 2;
	for (TInt d = -1; d <= 1; d++)
		aGc.DrawRect(TRect(mx + d * 5 - 1, my - 1, mx + d * 5 + 1, my + 1));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	}
	// the name, and user@address under it
	TInt tx = bx + bs + (tall ? 12 : 8);
	TInt right = card.iBr.iX - iMoreW - 14;
	TBuf<40> when;
	RelativeWhen(e.iLastUsed, when);
	// (1.04) connected now (in front or behind): says so in place of when
	TBool on = ((CPsiTermAppUi*)iEikonEnv->EikAppUi())->HostSession(host) >= 0;
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

// Nothing saved yet: a picture, and "Add server" as a button in the middle
// (Enter, or a tap anywhere, adds one)
void CPtStartView::DrawEmpty(CWindowGc& aGc) const
	{
	TStartLook l;
	Look(iColour, l);
	TRect r = Rect();
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	TInt cx = (r.iTl.iX + r.iBr.iX) / 2;
	TInt mid = r.iTl.iY + (iListY + r.Height()) / 2 - 10;
	// a small terminal: a rounded screen with a prompt
	TRect scr(cx - 26, mid - 54, cx + 26, mid - 20);
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
	aGc.DrawText(a, TPoint(cx - f->TextWidthInPixels(a) / 2, mid - 12 + f->AscentInPixels()));
	// the button
	TPtrC add(_L("+  Add server"));
	TInt bw = f->TextWidthInPixels(add) + 36, bh = f->HeightInPixels() + 10;
	TRect but(cx - bw / 2, mid + 12, cx + bw / 2, mid + 12 + bh);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(iColour ? l.iAccent : KRgbBlack);
	aGc.DrawRoundRect(but, TSize(6, 6));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(KRgbWhite);
	aGc.DrawText(add, TPoint(cx - f->TextWidthInPixels(add) / 2, but.iTl.iY + 5 + f->AscentInPixels()));
	aGc.UseFont(small);
	aGc.SetPenColor(l.iSoft);
	TPtrC b(_L("Enter adds one - Ctrl+G connects to an address - File > Import servers reads a list"));
	aGc.DrawText(b, TPoint(cx - small->TextWidthInPixels(b) / 2, but.iBr.iY + 8 + small->AscentInPixels()));
	aGc.DiscardFont();
	}

// (1.08) The last row: "+ Add server"
void CPtStartView::DrawAdd(CWindowGc& aGc, TInt aY, TInt aRow) const
	{
	TStartLook l;
	Look(iColour, l);
	const CFont* f = iEikonEnv->NormalFont();
	TRect r = Rect();
	TBool tall = iRowH >= 40;
	TInt pad = tall ? 8 : 4;
	TRect line(r.iTl.iX, r.iTl.iY + aY, r.iBr.iX, r.iTl.iY + aY + iAddH);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(l.iPaper);
	aGc.DrawRect(line);
	TRect card(line.iTl.iX + pad, line.iTl.iY + 1, line.iBr.iX - pad - 4, line.iBr.iY - 1);
	if (aRow == iSel)
		{
		aGc.SetBrushColor(l.iHigh);
		aGc.DrawRoundRect(card, TSize(6, 6));
		}
	// a plus in a ring where a server has its badge, then the words
	TInt bs = tall ? 20 : 16;
	TInt bx = card.iTl.iX + (tall ? 16 : 11), cy = (line.iTl.iY + line.iBr.iY) / 2;
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(l.iAccent);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.DrawEllipse(TRect(bx, cy - bs / 2, bx + bs, cy + bs / 2));
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(l.iAccent);
	TInt px = bx + bs / 2;
	aGc.DrawRect(TRect(px - 4, cy - 1, px + 4, cy + 1));
	aGc.DrawRect(TRect(px - 1, cy - 4, px + 1, cy + 4));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.UseFont(f);
	aGc.SetPenColor(l.iAccent);
	aGc.DrawText(_L("Add server"), TPoint(bx + bs + (tall ? 14 : 10),
		line.iTl.iY + (iAddH - f->HeightInPixels()) / 2 + f->AscentInPixels()));
	aGc.DiscardFont();
	}

// (1.08) The field at the top: a magnifying glass, then what was typed, or
// (in grey) what it is for
void CPtStartView::DrawSearch(CWindowGc& aGc, const TRect& aBox) const
	{
	TStartLook l;
	Look(iColour, l);
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	TBool on = iFilter.Length() > 0;
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(on ? l.iAccent : l.iFaint);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(iColour ? TRgb(238, 238, 238) : KRgbWhite);
	aGc.DrawRoundRect(aBox, TSize(aBox.Height() / 2, aBox.Height() / 2));
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	TInt cy = (aBox.iTl.iY + aBox.iBr.iY) / 2;
	TInt gx = aBox.iTl.iX + 9;
	aGc.SetPenColor(l.iSoft);
	aGc.DrawEllipse(TRect(gx, cy - 5, gx + 8, cy + 3));
	aGc.DrawLine(TPoint(gx + 6, cy + 2), TPoint(gx + 11, cy + 7));
	aGc.DrawLine(TPoint(gx + 7, cy + 2), TPoint(gx + 12, cy + 7));
	TInt tx = gx + 17;
	TInt room = aBox.iBr.iX - 10 - tx;
	if (on)
		{
		TBuf<26> t(iFilter);
		aGc.UseFont(f);
		aGc.SetPenColor(l.iInk);
		TextUtils::ClipToFit(t, *f, room - 4);
		TInt base = cy - f->HeightInPixels() / 2 + f->AscentInPixels();
		aGc.DrawText(t, TPoint(tx, base));
		TInt x = tx + f->TextWidthInPixels(t) + 1;   // the cursor
		aGc.DrawLine(TPoint(x, base - f->AscentInPixels() + 2), TPoint(x, base + 2));
		}
	else
		{
		TBuf<40> t(_L("Type to find a server"));
		aGc.UseFont(small);
		aGc.SetPenColor(l.iFaint);
		TextUtils::ClipToFit(t, *small, room);
		aGc.DrawText(t, TPoint(tx, cy - small->HeightInPixels() / 2 + small->AscentInPixels()));
		}
	aGc.DiscardFont();
	}

// Below the list: the highlighted server (a line on the 5mx; a card on a
// taller screen), or what a heading is
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
	const CFont* f = iEikonEnv->AnnotationFont();
	TBuf<200> t;
	TInt lh = f->HeightInPixels() + 4;
	TInt h = Current();
	if (h < 0)
		{
		if (IsAdd(iSel))
			t.Copy(_L("Enter adds a server  -  File > Import servers reads a list of them"));
		else if (!IsHead(iSel))
			return;
		else
			{
			// a heading: what it holds, and how to open it
			TBuf<24> name;
			HeadName(iSel, name);
			if (iRow[iSel].iKind == EHeadFolder)
				t.Copy(_L("Folder "));
			t.Append(name);
			t.AppendFormat(iRow[iSel].iCount == 1 ? _L("  -  %d server") : _L("  -  %d servers"), iRow[iSel].iCount);
			t.Append(iRow[iSel].iShut ? _L("  -  Enter opens it") : _L("  -  Enter shuts it"));
			}
		aGc.SetPenStyle(CGraphicsContext::ESolidPen);
		aGc.SetPenColor(l.iRule);
		aGc.DrawLine(TPoint(foot.iTl.iX + 8, foot.iTl.iY), TPoint(foot.iBr.iX - 8, foot.iTl.iY));
		aGc.UseFont(f);
		aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
		aGc.SetPenColor(l.iSoft);
		TextUtils::ClipToFit(t, *f, foot.Width() - 20);
		aGc.DrawText(t, TPoint(foot.iTl.iX + 10, foot.iTl.iY + 3 + f->AscentInPixels()));
		aGc.DiscardFont();
		return;
		}
	const THostEntry& e = iHosts.At(h);
	TBuf<80> login;
	TBuf<130> onLogin;
	Summary(h, login, onLogin);
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
	for (TInt i = 0; i < 6; i++)
		{
		TInt lx = x + (i >= 3 ? col2 : 0);
		TInt ly = y + (i >= 3 ? i - 3 : i) * lh;
		TInt room = i == 2 ? col2 - lw - 16 : col2 - lw - 16;
		const TText* label = i == 0 ? _S("Server") : i == 1 ? _S("Address") : i == 2 ? _S("On login")
			: i == 3 ? _S("Last") : i == 4 ? _S("Login") : _S("Folder");
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
		else if (i == 4)
			t.Copy(login);
		else if (e.iFolder.Length())
			t.Copy(e.iFolder);
		else
			t.Copy(_L("none"));
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
	TInt titleW = title->TextWidthInPixels(_L("Servers"));
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
	else if (iServers == 1)
		state.Copy(_L("1 server"));
	else
		state.Format(_L("%d servers"), iServers);
	gc.UseFont(small);
	gc.SetPenColor(live && iColour ? TRgb(0, 150, 0) : l.iFaint);
	TInt stateW = small->TextWidthInPixels(state);
	gc.DrawText(state, TPoint(r.iBr.iX - 14 - stateW, ty));
	gc.DiscardFont();
	if (iServers == 0)
		{
		DrawEmpty(gc);
		return;
		}
	// (1.08) the field between the title and the count
	{
	TInt bh = iBandH >= 34 ? 24 : iBandH - 6;
	TInt by = r.iTl.iY + (iBandH - bh) / 2;
	TRect box(r.iTl.iX + 12 + titleW + 20, by, r.iBr.iX - 14 - stateW - 16, by + bh);
	if (box.Width() > 300)
		box.iBr.iX = box.iTl.iX + 300;
	if (box.Width() > 90)
		DrawSearch(gc, box);
	}
	if (iFilter.Length() && iCount <= 1)
		{
		// nothing has what was typed
		const CFont* f = iEikonEnv->NormalFont();
		TBuf<60> none;
		none.Format(_L("No server has \"%S\""), &iFilter);
		gc.UseFont(f);
		gc.SetPenColor(l.iSoft);
		gc.DrawText(none, TPoint(r.iTl.iX + 24, r.iTl.iY + iListY + iAddH + 12 + f->AscentInPixels()));
		gc.DiscardFont();
		}
	// the rows: headings and servers
	TInt y = iListY;
	for (TInt row = iTop; row < iCount; row++)
		{
		TInt h = RowHeight(row);
		if (y + h > iFootY)
			break;
		if (IsHead(row))
			DrawHead(gc, y, row);
		else if (IsAdd(row))
			DrawAdd(gc, y, row);
		else
			DrawRow(gc, y, row);
		y += h;
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
// The server dialog's pictures, and the folders (1.06; 1.08: the grid has
// rows, for the forty-odd icons, and lives on the dialog's Icon page)
// ============================================================================

// The pictures in rows: the server's initial first, then each icon. The
// arrow keys choose; a tap chooses. Its name is under the grid.
class CPtIconGrid : public CCoeControl
	{
public:
	enum { KCell = 22, KCols = 15 };
	TInt iIcon;                              // 0 (the initial) .. KPtHostIconCount
	TBuf<24> iName;                          // the server's (its initial and colour)
	static TInt Rows()
		{
		return (KPtHostIconCount + 1 + KCols - 1) / KCols;
		}
	void ConstructFromResourceL(TResourceReader& aReader)
		{
		aReader.ReadInt8();
		}
	TSize MinimumSize()
		{
		return TSize(KCols * KCell + 2, Rows() * KCell + 5 + iEikonEnv->AnnotationFont()->HeightInPixels());
		}
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
		{
		if (aType != EEventKey)
			return EKeyWasNotConsumed;
		TInt to = iIcon;
		switch (aKeyEvent.iCode)
			{
		case EKeyLeftArrow:  to = iIcon > 0 ? iIcon - 1 : KPtHostIconCount; break;
		case EKeyRightArrow: to = iIcon < KPtHostIconCount ? iIcon + 1 : 0; break;
		case EKeyUpArrow:    to = iIcon - KCols; break;
		case EKeyDownArrow:  to = iIcon + KCols; break;
		default:
			return EKeyWasNotConsumed;
			}
		if (to < 0 || to > KPtHostIconCount)
			return EKeyWasNotConsumed;       // (off the top or bottom: the dialog's own keys)
		iIcon = to;
		DrawNow();
		return EKeyWasConsumed;
		}
	void HandlePointerEventL(const TPointerEvent& aEvent)
		{
		if (aEvent.iType != TPointerEvent::EButton1Down)
			return;
		TInt c = (aEvent.iPosition.iX - Rect().iTl.iX - 1) / KCell;
		TInt r = (aEvent.iPosition.iY - Rect().iTl.iY - 1) / KCell;
		TInt i = r * KCols + c;
		if (c >= 0 && c < KCols && r >= 0 && i <= KPtHostIconCount)
			{
			iIcon = i;
			DrawNow();
			}
		}
	void FocusChanged(TDrawNow aDrawNow)
		{
		if (aDrawNow)
			DrawNow();
		}
	void Draw(const TRect& /*aRect*/) const
		{
		CWindowGc& gc = SystemGc();
		TRect r = Rect();
		TBool colour = iEikonEnv->ScreenDevice()->DisplayMode() >= EColor16;
		TRgb back = iEikonEnv->ControlColor(EEikColorDialogBackground, *this);
		gc.SetPenStyle(CGraphicsContext::ENullPen);
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		gc.SetBrushColor(back);
		gc.DrawRect(r);
		const CFont* f = iEikonEnv->AnnotationFont();
		for (TInt i = 0; i <= KPtHostIconCount; i++)
			{
			TPoint tl(r.iTl.iX + 1 + (i % KCols) * KCell, r.iTl.iY + 1 + (i / KCols) * KCell);
			TRect cell(tl, TSize(KCell, KCell));
			TRect badge(cell.iTl.iX + 1, cell.iTl.iY + 1, cell.iBr.iX - 1, cell.iBr.iY - 1);
			gc.SetPenStyle(CGraphicsContext::ENullPen);
			gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
			gc.SetBrushColor(colour ? PtHostBadgeColour(iName) : TRgb::Gray16(i == iIcon ? 3 : 6));
			gc.DrawRoundRect(badge, TSize(4, 4));
			if (i == 0)
				{
				TBuf<2> init;
				if (iName.Length())
					{
					TChar c = iName[0];
					c.UpperCase();
					init.Append(c);
					}
				else
					init.Append('A');
				gc.SetBrushStyle(CGraphicsContext::ENullBrush);
				gc.SetPenStyle(CGraphicsContext::ESolidPen);
				gc.SetPenColor(KRgbWhite);
				gc.UseFont(f);
				gc.DrawText(init, TPoint(badge.iTl.iX + (badge.Width() - f->TextWidthInPixels(init)) / 2,
					badge.iTl.iY + (badge.Height() + f->AscentInPixels()) / 2 - 1));
				gc.DiscardFont();
				}
			else
				PtDrawHostIcon(gc, i, TPoint(badge.iTl.iX + 2, badge.iTl.iY + 2), KRgbWhite);
			if (i == iIcon)
				{
				// the choice: a black frame (two lines thick with the keys here)
				gc.SetBrushStyle(CGraphicsContext::ENullBrush);
				gc.SetPenStyle(CGraphicsContext::ESolidPen);
				gc.SetPenColor(KRgbBlack);
				gc.DrawRect(cell);
				if (IsFocused())
					gc.DrawRect(TRect(cell.iTl.iX - 1, cell.iTl.iY - 1, cell.iBr.iX + 1, cell.iBr.iY + 1));
				}
			}
		// its name under the grid
		TBuf<40> name;
		if (iIcon >= 1 && iIcon <= KPtHostIconCount)
			name = TPtrC(KPtHostIconNames[iIcon - 1]);
		else
			name = _L("The name's first letter");
		gc.SetBrushStyle(CGraphicsContext::ENullBrush);
		gc.SetPenStyle(CGraphicsContext::ESolidPen);
		gc.SetPenColor(iEikonEnv->ControlColor(EEikColorDialogText, *this));
		gc.UseFont(f);
		gc.DrawText(name, TPoint(r.iTl.iX + 2, r.iTl.iY + Rows() * KCell + 4 + f->AscentInPixels()));
		gc.DiscardFont();
		}
	};

CCoeControl* PtNewIconGridL()
	{
	return new(ELeave) CPtIconGrid;
	}

void PtIconGridSet(CCoeControl* aGrid, TInt aIcon, const TDesC& aName)
	{
	CPtIconGrid* g = (CPtIconGrid*)aGrid;
	g->iIcon = aIcon >= 0 && aIcon <= KPtHostIconCount ? aIcon : 0;
	g->iName = Upto(aName, 24);
	}

TInt PtIconGridIcon(const CCoeControl* aGrid)
	{
	return ((const CPtIconGrid*)aGrid)->iIcon;
	}

// a folder's name: New folder, Rename folder
class CPtFolderNameDialog : public CEikDialog
	{
public:
	CPtFolderNameDialog(TDes& aName, const TDesC& aTitle) : iName(aName), iTitle(aTitle) {}
private:
	void PreLayoutDynInitL()
		{
		SetTitleL(iTitle);
		SetEdwinTextL(EPtDlgFolderName, &iName);
		}
	TBool OkToExitL(TInt /*aButtonId*/)
		{
		TBuf<KPtFolderLen> n;
		GetEdwinText(n, EPtDlgFolderName);
		n.Trim();
		if (n.Length() == 0)
			{
			iEikonEnv->InfoMsg(_L("No name entered"));
			return EFalse;
			}
		iName = n;
		return ETrue;
		}
	TDes& iName;
	const TDesC& iTitle;
	};

TBool PtAskFolderNameL(TDes& aName, const TDesC& aTitle)
	{
	CPtFolderNameDialog* dlg = new(ELeave) CPtFolderNameDialog(aName, aTitle);
	return dlg->ExecuteLD(R_PT_FOLDER_NAME_DIALOG) != 0;
	}

// the folders in use, in alphabetical order
TInt PtFolderNames(const CHostList& aHosts, TBuf<KPtFolderLen>* aNames)
	{
	TInt n = 0;
	for (TInt i = 0; i < aHosts.Count() && i < KMaxHosts; i++)
		{
		const TDesC& f = aHosts.At(i).iFolder;
		if (f.Length() == 0)
			continue;
		TBool seen = EFalse;
		for (TInt k = 0; k < n; k++)
			if (aNames[k].CompareF(f) == 0)
				seen = ETrue;
		if (seen)
			continue;
		TInt at = n;
		while (at > 0 && aNames[at - 1].CompareF(f) > 0)
			{
			aNames[at] = aNames[at - 1];
			at--;
			}
		aNames[at] = Upto(f, KPtFolderLen);
		n++;
		}
	return n;
	}

// Edit > Move to folder... (1.08): None, a folder in use, or a new one
class CPtMoveDialog : public CEikDialog
	{
public:
	CPtMoveDialog(TDes& aFolder, const CHostList& aHosts) : iFolder(aFolder), iHosts(aHosts) {}
private:
	void PreLayoutDynInitL()
		{
		CDesCArrayFlat* a = new(ELeave) CDesCArrayFlat(8);
		CleanupStack::PushL(a);
		a->AppendL(_L("None"));
		iNameCount = PtFolderNames(iHosts, iNames);
		TInt current = 0;
		for (TInt k = 0; k < iNameCount; k++)
			{
			a->AppendL(iNames[k]);
			if (iNames[k].CompareF(iFolder) == 0)
				current = k + 1;
			}
		a->AppendL(_L("New folder..."));
		CEikChoiceList* c = (CEikChoiceList*)Control(EPtDlgLookFolder);
		c->SetArrayL(a);
		c->SetArrayExternalOwnership(EFalse);
		CleanupStack::Pop();
		c->SetCurrentItem(current);
		}
	TBool OkToExitL(TInt /*aButtonId*/)
		{
		TInt at = ((CEikChoiceList*)Control(EPtDlgLookFolder))->CurrentItem();
		TBuf<KPtFolderLen> folder;
		if (at > iNameCount)
			{
			if (!PtAskFolderNameL(folder, _L("New folder")))
				return EFalse;               // (back to this dialog)
			}
		else if (at >= 1)
			folder = iNames[at - 1];
		iFolder = folder;
		return ETrue;
		}
	TDes& iFolder;
	const CHostList& iHosts;
	TBuf<KPtFolderLen> iNames[KMaxHosts];
	TInt iNameCount;
	};

// ============================================================================
// The AppUi's side
// ============================================================================

void CPsiTermAppUi::OpenStartL()
	{
	iHosts->MergeDisk();                     // (1.08) Hosts.dat copied over meanwhile: its servers come in
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
	SessionsSync();                          // (1.06: the connections' tabs go under it)
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
	SessionsSync();
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
	if (iStart)
		iStart->CurrentFolder(e.iFolder);    // (1.08) in the folder the highlight is in
	if (EditHostL(e, ETrue))
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
		if (e.iFolder.Length())
			iHosts->SetShut(e.iFolder, EFalse);   // (its folder opens, so it can be seen)
		iHosts->Save();
		if (iStart)
			iStart->ReloadL(aIndex);
		SessionsSync();                      // (a connection's tab has the name and icon)
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

// ----- Duplicate server, Move to folder..., Rename folder..., the row's menu ----

// (1.08) A copy of the server, to change: "Dev 2", not pinned, never used
void CPsiTermAppUi::DuplicateHostL(TInt aIndex)
	{
	if (aIndex < 0 || aIndex >= iHosts->Count())
		{
		iEikonEnv->InfoMsg(_L("No server selected"));
		return;
		}
	if (iHosts->Count() >= KMaxHosts)
		{
		iEikonEnv->InfoMsg(_L("Server list is full - delete one first"));
		return;
		}
	THostEntry e = iHosts->At(aIndex);
	if (e.iName.Length() > e.iName.MaxLength() - 2)
		e.iName.SetLength(e.iName.MaxLength() - 2);
	e.iName.Append(_L(" 2"));
	e.iFlags &= ~KPtHostPinned;
	e.iLastUsed = 0;
	e.iLastResult = EPtResultNever;
	iHosts->AddL(e);
	e.iPassword.FillZ();
	iHosts->iLast = iHosts->Count() - 1;
	iHosts->Save();
	if (iStart)
		iStart->ReloadL(iHosts->iLast);
	iEikonEnv->InfoMsg(_L("Server duplicated"));
	}

void CPsiTermAppUi::MoveHostL(TInt aIndex)
	{
	if (aIndex < 0 || aIndex >= iHosts->Count())
		{
		iEikonEnv->InfoMsg(_L("No server selected"));
		return;
		}
	TBuf<KPtFolderLen> folder(iHosts->At(aIndex).iFolder);
	CPtMoveDialog* dlg = new(ELeave) CPtMoveDialog(folder, *iHosts);
	if (!dlg->ExecuteLD(R_PT_MOVE_DIALOG))
		return;
	iHosts->At(aIndex).iFolder = folder;
	if (folder.Length())
		iHosts->SetShut(folder, EFalse);     // (its folder opens, so it can be seen)
	iHosts->Save();
	if (iStart)
		iStart->ReloadL(aIndex);
	}

// The highlighted server's own menu: Tab, or its "..." button
void CPsiTermAppUi::RowMenuL(const TPoint& aScreenPos)
	{
	LaunchPopupMenuL(R_PT_ROW_MENU, aScreenPos, EPopupTargetTopRight);
	}

// Hosts.dat changed on the disk while PsiTerm was open (copied there by
// hand, restored): the servers in it that are not here are taken in
void CPsiTermAppUi::HostsFromDiskL()
	{
	TInt added = iHosts->MergeDisk();
	if (added <= 0)
		return;
	if (iStart)
		{
		iStart->ReloadL();
		iStart->DrawNow();
		}
	TBuf<60> m;
	m.Format(added == 1 ? _L("%d server read from the disk") : _L("%d servers read from the disk"), added);
	iEikonEnv->InfoMsg(m);
	}

// ----- File > Import servers... (1.08) -----------------------------------------
// A PsiTerm Hosts.dat (another Psion's, a backup's) or an OpenSSH config
// file. Its servers that are not here already (the same address, port and
// user) are added, with their folder, icon and pin; a saved password comes
// across only from this Psion's own files (it is scrambled with the
// Psion's number), and an SSH key that is not on this Psion becomes the
// first key.
void CPsiTermAppUi::ImportServersL()
	{
	RFs& fs = iCoeEnv->FsSession();
	TFileName name(_L("D:\\Install\\Hosts.dat"));
	TEntry entry;
	if (fs.Entry(name, entry) != KErrNone)
		name = _L("C:\\Documents\\");
	CEikFileOpenDialog* open = new(ELeave) CEikFileOpenDialog(&name, R_PT_TBUF_IMPORT_TITLE);
	if (!open->ExecuteLD(R_EIK_DIALOG_FILE_OPEN))
		return;
	if (fs.Entry(name, entry) != KErrNone)
		{
		iEikonEnv->InfoMsg(_L("File not found"));
		return;
		}
	CHostList* in = CHostList::NewL(fs);
	CleanupStack::PushL(in);
	TInt r = in->ReadFile(name);             // a Hosts.dat?
	if (r != KErrNone)
		{
		// no: an OpenSSH config file (text, up to 32 KB of it)
		RFile file;
		r = file.Open(fs, name, EFileRead | EFileShareReadersOnly);
		if (r == KErrNone)
			{
			HBufC8* text = HBufC8::NewLC(32768);
			TPtr8 p(text->Des());
			r = file.Read(p);
			file.Close();
			TBool looksText = ETrue;
			for (TInt i = 0; i < p.Length() && i < 256; i++)
				if (p[i] == 0)
					looksText = EFalse;
			if (r == KErrNone && looksText)
				in->ReadSshConfig(p, iKeys->Count() > 0);
			CleanupStack::PopAndDestroy();   // text
			}
		}
	TInt found = in->Count();
	TInt before = iHosts->Count();
	TBool full = EFalse;
	TInt added = found ? iHosts->MergeFrom(*in, full) : 0;
	CleanupStack::PopAndDestroy();           // in
	for (TInt i = before; i < iHosts->Count(); i++)
		{
		// an SSH key from another Psion is not here: the first key instead
		THostEntry& e = iHosts->At(i);
		if (e.iKeyId && iKeys->Find(e.iKeyId) < 0)
			e.iKeyId = 0;
		if ((e.iAuth == 0 || e.iAuth == 3) && iKeys->Count() == 0)
			e.iAuth = 1;
		}
	TBuf<80> m;
	if (found == 0)
		m = _L("No servers found in the file");
	else if (added == 0 && full)
		m = _L("Server list is full - delete one first");
	else if (added == 0)
		m.Format(found == 1 ? _L("No new servers - the %d in the file is here already")
			: _L("No new servers - the %d in the file are here already"), found);
	else
		{
		m.Format(added == 1 ? _L("%d server added") : _L("%d servers added"), added);
		if (full)
			m.Append(_L(" - the list is full"));
		else if (found > added)
			m.AppendFormat(_L(" - %d here already"), found - added);
		}
	if (added > 0)
		{
		iHosts->Save();
		if (iStart)
			{
			iStart->ClearFilter();
			iStart->ReloadL(before);
			iStart->DrawNow();
			}
		}
	iEikonEnv->InfoMsg(m);
	}

// The folder the highlight is on or in gets a new name: every server in it moves
void CPsiTermAppUi::RenameFolderL()
	{
	TBuf<KPtFolderLen> old;
	if (!iStart || !iStart->CurrentFolder(old))
		{
		iEikonEnv->InfoMsg(_L("No folder selected"));
		return;
		}
	TBuf<KPtFolderLen> name(old);
	if (!PtAskFolderNameL(name, _L("Rename folder")) || name == old)
		return;
	TBool shut = iHosts->Shut(old);
	TInt cur = iStart->Current();
	for (TInt i = 0; i < iHosts->Count(); i++)
		if (iHosts->At(i).iFolder.CompareF(old) == 0)
			iHosts->At(i).iFolder = name;
	iHosts->SetShut(old, EFalse);
	iHosts->SetShut(name, shut);
	iHosts->Save();
	iStart->ReloadL(cur);
	}

#ifdef PT_TESTSEED
// A test build only (#define PT_TESTSEED at the top of psiterm.h, never committed): with no
// servers saved, a set of them in folders with icons, for the emulator.
// SSH to one then opens a stand-in connection (CTermView::DemoL).
void CPsiTermAppUi::SeedL()
	{
	if (iHosts->Count() > 0)
		return;
	static const struct { const TText* iName; const TText* iHost; const TText* iFolder; TInt iIcon; TInt iPin; TInt iAgo; TInt iResult; } KSeed[] =
		{
		{ _S("Dev"), _S("dev.example.org"), _S("Work"), 2, 1, 600, EPtResultLoggedIn },
		{ _S("Build box"), _S("build.example.org"), _S("Work"), 1, 0, 7200, EPtResultLoggedIn },
		{ _S("Web"), _S("www.example.org"), _S("Work"), 8, 0, 90000, EPtResultNoAnswer },
		{ _S("Home server"), _S("192.0.2.10"), _S("Home"), 3, 0, 3000, EPtResultLoggedIn },
		{ _S("NAS"), _S("192.0.2.20"), _S("Home"), 7, 0, 200000, EPtResultLoggedIn },
		{ _S("Pi-hole"), _S("192.0.2.53"), _S("Home"), 12, 0, 400000, EPtResultRefused },
		{ _S("old-vps"), _S("vps.example.net"), _S(""), 0, 0, 900000, EPtResultLost }
		};
	TUint32 now = PtNowSeconds();
	for (TInt i = 0; i < (TInt)(sizeof(KSeed) / sizeof(KSeed[0])); i++)
		{
		THostEntry e;
		e.iName = TPtrC(KSeed[i].iName);
		e.iHost = TPtrC(KSeed[i].iHost);
		e.iUser = _L("dan");
		e.iAuth = 1;
		e.iFolder = TPtrC(KSeed[i].iFolder);
		e.iIcon = KSeed[i].iIcon;
		e.iFlags = KSeed[i].iPin ? KPtHostPinned : 0;
		e.iLastUsed = now - KSeed[i].iAgo;
		e.iLastResult = KSeed[i].iResult;
		iHosts->AddL(e);
		}
	iHosts->Save();
	iHosts->Load();                          // (the test reads back what it wrote)
	}
#endif

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
	SessionsSync();
	}

// The Start view's commands; EFalse if aCommand is not one
TBool CPsiTermAppUi::StartCommandL(TInt aCommand)
	{
	TInt cur = iStart ? iStart->Current() : iHosts->iLast;
	switch (aCommand)
		{
	case EPtCmdStConnect:
		if (iStart && iStart->ActivateHeadL())
			return ETrue;                    // (1.06) on a heading: it opens or shuts
		if (iStart && iStart->OnAddRow())
			{
			NewHostL();
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
		EditHostAtL(iStart ? cur : -1);
		return ETrue;
	case EPtCmdStDelete:
		DeleteHostL(cur);
		return ETrue;
	case EPtCmdStDuplicate:
		DuplicateHostL(iStart ? cur : -1);
		return ETrue;
	case EPtCmdStMove:
		MoveHostL(iStart ? cur : -1);
		return ETrue;
	case EPtCmdStImport:
		ImportServersL();
		return ETrue;
	case EPtCmdStRowMenu:
		{
		TPoint pos;
		if (iStart && iStart->RowMenuPos(pos))
			RowMenuL(pos);
		return ETrue;
		}
	case EPtCmdStFolderRename:
		RenameFolderL();
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
