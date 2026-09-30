// pmnative.cpp - PsiMail's mailbox and reader built from EIKON's own
// controls, so they look and behave like the Psion's built-in programs:
// a folder list and a message list (EIKON list boxes), a read-only rich
// text viewer with a scroll bar, the standard toolbar on the right, and
// the sidebar's zoom buttons. (Writing, the calendar and event screens are
// still PsiMail's own drawn screens - see psimail.cpp's Render().)
#include "pmapp.h"
#include <eiktxlbx.h>
#include <eiktxlbm.h>
#include <eikclb.h>
#include <eikclbd.h>
#include <eiklbi.h>
#include <eiklbv.h>
#include <eikrted.h>
#include <eiksbfrm.h>
#include <txtrich.h>
#include <txtfrmat.h>
#include <frmtlay.h>
#include <frmtview.h>

// ----- a text list box whose font can change (for zoom) ------------------------

class CPmTextListItemDrawer : public CTextListItemDrawer
	{
public:
	CPmTextListItemDrawer(MTextListBoxModel* aModel, const CFont* aFont)
		: CTextListItemDrawer(aModel, aFont) {}
	void SetFont(const CFont* aFont) { iFont = aFont; }
	};

class CPmFolderListBox : public CEikTextListBox
	{
public:
	void SetFontL(const CFont* aFont)
		{
		((CPmTextListItemDrawer*)iItemDrawer)->SetFont(aFont);
		SetItemHeightL(aFont->HeightInPixels() + 4);
		}
protected:
	void CreateItemDrawerL()
		{
		iItemDrawer = new(ELeave) CPmTextListItemDrawer(Model(), CEikonEnv::Static()->NormalFont());
		}
	};

// ----- zoom -----------------------------------------------------------------------

static const TInt KZoomFactors[4] = { 800, 1000, 1250, 1500 };
// the list font at each zoom level (twips): the built-in programs' Arial
static const TInt KListTwips[4] = { 150, 180, 220, 270 };

static TInt ZoomLevel(const TPmSettings& aSettings)
	{
	TInt z = aSettings.iZoom;                  // 0 = the default
	if (z < 1 || z > 4)
		z = 2;
	return z - 1;                              // 0..3
	}

TBool CPmView::NativeMode() const
	{
	return iMode == EList || iMode == EOutbox || iMode == EMessage || iMode == ENoAccount;
	}

void CPmView::CreateNativeL()
	{
	iLinks = new(ELeave) CArrayFixFlat<TPmLinkRange>(16);
	iLinkSel = -1;
	iZoomFactor = new(ELeave) TZoomFactor(iCoeEnv->ScreenDevice());

	iFolderList = new(ELeave) CPmFolderListBox;
	iFolderList->ConstructL(this, 0);
	iFolderList->Model()->SetItemTextArray(new(ELeave) CDesCArrayFlat(16));
	iFolderList->Model()->SetOwnershipType(ELbmOwnsItemArray);
	iFolderList->CreateScrollBarFrameL();
	iFolderList->ScrollBarFrame()->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);
	iFolderList->SetListBoxObserver(this);

	iMsgList = new(ELeave) CEikColumnListBox;
	iMsgList->ConstructL(this, 0);
	iMsgList->Model()->SetItemTextArray(new(ELeave) CDesCArrayFlat(32));
	iMsgList->Model()->SetOwnershipType(ELbmOwnsItemArray);
	iMsgList->CreateScrollBarFrameL();
	iMsgList->ScrollBarFrame()->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);
	iMsgList->SetListBoxObserver(this);

	iReader = new(ELeave) CEikRichTextEditor(TEikBorder(TEikBorder::ENone));
	iReader->SetContainerWindowL(*this);
	iReader->ConstructL(this, 0, 0, CEikEdwin::EReadOnly | CEikEdwin::ENoAutoSelection | CEikEdwin::EAlwaysShowSelection);
	iReader->CreateScrollBarFrameL()->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);

	ApplyZoomL();
	ShowNative(EFalse);
	}

void CPmView::DestroyNative()
	{
	delete iFolderList;
	iFolderList = NULL;
	delete iMsgList;
	iMsgList = NULL;
	delete iReader;
	iReader = NULL;
	if (iListFont)
		iCoeEnv->ScreenDevice()->ReleaseFont(iListFont);
	iListFont = NULL;
	if (iSmallFont)
		iCoeEnv->ScreenDevice()->ReleaseFont(iSmallFont);
	iSmallFont = NULL;
	delete iZoomFactor;
	iZoomFactor = NULL;
	delete iLinks;
	iLinks = NULL;
	}

TInt CPmView::CountComponentControls() const
	{
	return iFolderList ? 3 : 0;
	}

CCoeControl* CPmView::ComponentControl(TInt aIndex) const
	{
	switch (aIndex)
		{
	case 0: return iFolderList;
	case 1: return iMsgList;
	default: return iReader;
		}
	}

void CPmView::SizeChanged()
	{
	if (iFolderList)
		LayoutNative();
	}

// fonts and the reader's zoom from the zoom level
void CPmView::ApplyZoomL()
	{
	TInt z = ZoomLevel(*iSettings);
	CWsScreenDevice* dev = iCoeEnv->ScreenDevice();
	TFontSpec spec(_L("Arial"), KListTwips[z]);
	CFont* font = NULL;
	User::LeaveIfError(dev->GetNearestFontInTwips(font, spec));
	if (iListFont)
		dev->ReleaseFont(iListFont);
	iListFont = font;
	TFontSpec small(_L("Arial"), 150);
	CFont* sf = NULL;
	User::LeaveIfError(dev->GetNearestFontInTwips(sf, small));
	if (iSmallFont)
		dev->ReleaseFont(iSmallFont);
	iSmallFont = sf;
	iStatusH = iSmallFont->HeightInPixels() + 4;

	iFolderList->SetFontL(iListFont);
	CColumnListBoxData* cd = iMsgList->Model()->ColumnData();
	for (TInt c = 0; c < 7; c++)
		cd->SetColumnFontL(c, iListFont);
	cd->SetColumnAlignmentL(6, CGraphicsContext::ERight);
	iMsgList->SetItemHeightL(iListFont->HeightInPixels() + 4);

	iZoomFactor->SetZoomFactor(KZoomFactors[z]);
	iReader->SetZoomFactorL(iZoomFactor);
	LayoutNative();
	}

void CPmView::ZoomL(TInt aStep)
	{
	TInt z = ZoomLevel(*iSettings) + aStep;
	if (z < 0 || z > 3)
		{
		iEikonEnv->InfoMsg(aStep > 0 ? _L("Largest text size") : _L("Smallest text size"));
		return;
		}
	iSettings->iZoom = z + 1;
	((CPmAppUi*)iEikonEnv->EikAppUi())->SaveSettings();
	if (iFolderList)
		{
		ApplyZoomL();
		iReaderUid = 0;                          // lay the message out again
		iNativeMode = (TMode)-1;
		if (NativeMode())
			{
			UpdateNativeL();
			DrawNow();
			}
		}
	}

void CPmView::LayoutNative()
	{
	TRect r = Rect();
	TInt h = r.Height() - iStatusH;
	TInt w = r.Width();
	// the folder list takes about 30% (at least room for "Archive (123)")
	iSplitX = w * 3 / 10;
	if (iSplitX < 120) iSplitX = 120;
	TRAPD(e1,
		iFolderList->SetRectL(TRect(r.iTl, TSize(iSplitX, h)));
		iMsgList->SetRectL(TRect(TPoint(r.iTl.iX + iSplitX + 1, r.iTl.iY), TSize(w - iSplitX - 1, h)));
		iReader->SetRectL(TRect(r.iTl, TSize(w, h)));
		);
	(void)e1;
	// columns: marks, from, subject, date
	TInt lw = w - iSplitX - 1 - 12;             // (room for a scroll bar)
	TInt mark = iListFont->TextWidthInPixels(_L("\x95!")) + 4;
	TInt date = iListFont->TextWidthInPixels(_L("Yesterday")) + 6;
	TInt from = (lw - mark - date) * 2 / 5;
	TInt subj = lw - mark - date - from;
	if (subj < 20) subj = 20;
	CColumnListBoxData* cd = iMsgList->Model()->ColumnData();
	TInt gap = 6;
	from -= gap;
	subj -= gap * 2;
	TRAPD(err,
		cd->SetColumnWidthPixelL(0, mark);
		cd->SetColumnWidthPixelL(1, 2);
		cd->SetColumnWidthPixelL(2, from);
		cd->SetColumnWidthPixelL(3, gap);
		cd->SetColumnWidthPixelL(4, subj);
		cd->SetColumnWidthPixelL(5, gap);
		cd->SetColumnWidthPixelL(6, date);
		);
	(void)err;
	}

static void ShowScrollBar(CEikScrollBarFrame* aFrame, TBool aShow)
	{
	if (!aFrame)
		return;
	CEikScrollBar* sb = aFrame->GetScrollBarHandle(CEikScrollBar::EVertical);
	if (sb && (aShow ? aFrame->VScrollBarVisibility() != CEikScrollBarFrame::EOff : ETrue))
		sb->MakeVisible(aShow);
	}

void CPmView::ShowNative(TBool aShow)
	{
	iNativeShown = aShow;
	TBool list = aShow && (iMode == EList || iMode == EOutbox);
	TBool reader = aShow && iMode == EMessage;
	TBool folders = list || (aShow && iMode == ENoAccount);
	TBool msgs = list && iRows->Count() > 0;
	iFolderList->MakeVisible(folders);
	iMsgList->MakeVisible(msgs);
	iReader->MakeVisible(reader);
	// (the scroll bars are controls of their own: hide them with their owner)
	ShowScrollBar(iFolderList->ScrollBarFrame(), folders);
	ShowScrollBar(iMsgList->ScrollBarFrame(), msgs);
	ShowScrollBar(iReader->ScrollBarFrame(), reader);
	iFolderList->SetFocus(list && iSidebar, ENoDrawNow);
	iMsgList->SetFocus(list && !iSidebar, ENoDrawNow);
	iReader->SetFocus(reader, ENoDrawNow);
	}

// ----- filling the controls ---------------------------------------------------------

static TUint Checksum(TUint aSum, const TDesC& aText)
	{
	for (TInt i = 0; i < aText.Length(); i++)
		aSum = aSum * 31 + aText[i];
	return aSum;
	}

void CPmView::UpdateFolderListL()
	{
	CDesCArray* items = (CDesCArray*)iFolderList->Model()->ItemTextArray();
	CDesCArrayFlat* fresh = new(ELeave) CDesCArrayFlat(16);
	CleanupStack::PushL(fresh);
	TBuf<100> line;
	for (TInt i = 0; i < iFolders->Count(); i++)
		{
		const TPmFolder& f = (*iFolders)[i];
		// "Work/Projects": "Projects", indented
		TInt depth = 0, last = -1;
		if (f.iKind == '-' || f.iKind == 'N')
			for (TInt j = 0; j < f.iName.Length(); j++)
				if (f.iName[j] == '/' || f.iName[j] == '.') { depth++; last = j; }
		line.Zero();
		for (TInt d = 0; d < depth && d < 3; d++)
			line.Append(_L("   "));
		line.Append(Clip(f.iName.Mid(last + 1), 60));
		TBool counts = !(f.iKind == 'S' || f.iKind == 'D' || f.iKind == 'T' || f.iKind == 'J');
		if (counts && f.iUnread > 0)
			line.AppendFormat(_L(" (%d)"), f.iUnread);
		fresh->AppendL(line);
		}
	TInt ob = OutboxCount();
	line = _L("Outbox");
	if (ob) line.AppendFormat(_L(" (%d)"), ob);
	fresh->AppendL(line);
	fresh->AppendL(_L("Calendar"));
	TBool same = items->Count() == fresh->Count();
	for (TInt k = 0; same && k < fresh->Count(); k++)
		same = (*items)[k] == (*fresh)[k];
	if (!same)
		{
		items->Reset();
		for (TInt k = 0; k < fresh->Count(); k++)
			items->AppendL((*fresh)[k]);
		iFolderList->HandleItemAdditionL();
		}
	CleanupStack::PopAndDestroy();          // fresh
	TInt sel = iSidebar ? iFolderSel : CurrentSidebarItem();
	if (sel >= items->Count()) sel = items->Count() - 1;
	TBool moved = sel >= 0 && sel != iFolderList->CurrentItemIndex();
	if (moved)
		iFolderList->SetCurrentItemIndex(sel);
	if ((!same || moved) && iNativeShown && iFolderList->IsVisible())
		iFolderList->DrawNow();
	}

void CPmView::UpdateMessageListL()
	{
	CDesCArray* items = (CDesCArray*)iMsgList->Model()->ItemTextArray();
	// only when something changed: a rebuild moves the list about
	TUint sum = iMode == EOutbox ? 7 : 3;
	for (TInt i = 0; i < iRows->Count(); i++)
		{
		const TPmRow& r = (*iRows)[i];
		sum = sum * 131 + r.iUid;
		sum = Checksum(sum, r.iFlags);
		}
	sum = Checksum(sum, iFolder);
	TBool changed = items->Count() != iRows->Count() || iMsgListSum != sum;
	if (changed)
		{
		iMsgListSum = sum;
		items->Reset();
		TBuf<240> line;
		TBuf<16> date;
		for (TInt i = 0; i < iRows->Count(); i++)
			{
			const TPmRow& r = (*iRows)[i];
			line.Zero();
			if (iMode == EOutbox)
				{
				if (r.iFlags.Locate('E') >= 0) line.Append('!');
				date = r.iFlags.Locate('E') >= 0 ? _L("not sent") : r.iFlags.Locate('D') >= 0 ? _L("draft") : _L("to send");
				}
			else
				{
				if (r.iFlags.Locate('S') < 0) line.Append(TChar(0x95));    // unread: a bullet
				if (r.iFlags.Locate('F') >= 0) line.Append('!');
				else if (r.iFlags.Locate('T') >= 0) line.Append('@');
				FormatDate(r.iDate, date);
				}
			line.Append(_L("\t\t"));
			line.Append(Clip(r.iFrom, 60));
			line.Append(_L("\t\t"));
			line.Append(r.iSubject.Length() ? Clip(r.iSubject, 150) : TPtrC(_L("(no subject)")));
			line.Append(_L("\t\t"));
			line.Append(date);
			items->AppendL(line);
			}
		iMsgList->HandleItemAdditionL();
		}
	if (iRows->Count())
		{
		TInt sel = iSel;
		if (sel >= iRows->Count()) sel = iRows->Count() - 1;
		if (sel < 0) sel = 0;
		if (sel != iMsgList->CurrentItemIndex())
			{
			iMsgList->SetCurrentItemIndex(sel);
			changed = ETrue;
			}
		}
	if (changed && iNativeShown && iMsgList->IsVisible())
		iMsgList->DrawNow();
	}

// ----- the reader -------------------------------------------------------------------

// character and paragraph styles collected while the text is put together
struct TPmSpan { TInt iPos; TInt iLen; TInt iKind; TInt iArg; };
enum { ESpanBold, ESpanItalic, ESpanUnder, ESpanBig, ESpanMono, ESpanIndent, ESpanSpaceBefore, ESpanRuleBelow };

static void AddSpan(CArrayFixFlat<TPmSpan>& aSpans, TInt aPos, TInt aLen, TInt aKind, TInt aArg = 0)
	{
	if (aLen <= 0)
		return;
	TPmSpan s;
	s.iPos = aPos; s.iLen = aLen; s.iKind = aKind; s.iArg = aArg;
	TRAPD(err, aSpans.AppendL(s));
	(void)err;
	}

void CPmView::UpdateReaderL()
	{
	const TPmRow* row = CurrentRow();
	TBool waiting = iWaitingBody || !iText;
	if (iReaderUid == iMsgUid && iReaderWaiting == waiting && iNativeMode == EMessage)
		return;
	iReaderUid = iMsgUid;
	iReaderWaiting = waiting;
	iLinks->Reset();
	iLinkSel = -1;

	HBufC* buf = HBufC::NewLC((iText ? iText->Length() : 0) + 2048);
	TPtr t = buf->Des();
	CArrayFixFlat<TPmSpan>* spans = new(ELeave) CArrayFixFlat<TPmSpan>(64);
	CleanupStack::PushL(spans);
	const TChar KPara(CEditableText::EParagraphDelimiter);
	TBuf<300> v;
	TInt lastHead = 0, lastHeadLen = 1;

	// the header: subject, then From / To / Cc / Date, the attachments
	if (row && row->iSubject.Length()) v = Clip(row->iSubject, 200);
	else if (!MessageHeader(_L("Subject"), v)) v = _L("(no subject)");
	TInt p0 = t.Length();
	t.Append(v);
	AddSpan(*spans, p0, t.Length() - p0, ESpanBold);
	AddSpan(*spans, p0, t.Length() - p0, ESpanBig, 1250);
	lastHead = p0;
	lastHeadLen = t.Length() - p0;
	t.Append(KPara);
	const TText* KNames[] = { _S("From"), _S("To"), _S("Cc"), _S("Date") };
	for (TInt h = 0; h < 4; h++)
		{
		TPtrC name(KNames[h]);
		if (!MessageHeader(name, v) || !v.Length())
			continue;
		p0 = t.Length();
		t.Append(name);
		t.Append(_L(": "));
		AddSpan(*spans, p0, t.Length() - p0, ESpanBold);
		t.Append(Clip(v, 250));
		lastHead = p0;
		lastHeadLen = t.Length() - p0;
		t.Append(KPara);
		}
	if (iAttNames->Count())
		{
		p0 = t.Length();
		t.Append(_L("Attachments: "));
		AddSpan(*spans, p0, t.Length() - p0, ESpanBold);
		for (TInt a = 0; a < iAttNames->Count(); a++)
			{
			if (a) t.Append(_L(",  "));
			TPmLinkRange lr;
			lr.iPos = t.Length();
			t.Append((*iAttNames)[a]);
			t.Append(_L(" ("));
			t.Append((*iAttSizes)[a]);
			t.Append(')');
			lr.iLen = t.Length() - lr.iPos;
			lr.iLink = -1000 - a;
			iLinks->AppendL(lr);
			AddSpan(*spans, lr.iPos, lr.iLen, ESpanUnder);
			}
		lastHead = p0;
		lastHeadLen = t.Length() - p0;
		t.Append(KPara);
		}
	// a rule under the header
	AddSpan(*spans, lastHead, lastHeadLen, ESpanRuleBelow);

	if (waiting)
		{
		t.Append(iSettings->iOffline ? _L("Not downloaded - you are working offline.")
			: _L("Downloading the message..."));
		t.Append(KPara);
		}
	else
		{
		// the body: after the header lines and the blank line
		TPtrC rest = iText->Mid(iBodyOff);
		while (rest.Length())
			{
			TInt nl = rest.Locate('\n');
			TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
			rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
			if (!line.Length())
				break;
			}
		TInt bold = -1, ital = -1, link = -1, linkNo = 0;
		while (rest.Length() && t.Length() < t.MaxLength() - 400)
			{
			TInt nl = rest.Locate('\n');
			TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
			rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
			if (line.Length() && line[line.Length() - 1] == '\r')
				line.Set(line.Left(line.Length() - 1));
			TInt paraStart = t.Length();
			TInt kind = 0, arg = 0;
			if (line.Length() >= 2 && line[0] == 0x01)
				{
				TChar k = line[1];
				TPtrC body = line.Mid(2);
				switch (k)
					{
				case 'u':
					continue;                     // a link's address: not shown
				case 'r':
					for (TInt d = 0; d < 24; d++) t.Append(TChar(0x97));
					t.Append(KPara);
					continue;
				case 'h':
					kind = 'h';
					arg = body.Length() ? body[0] - '0' : 2;
					body.Set(body.Length() ? body.Mid(1) : body);
					break;
				case 'l':
					{
					kind = 'l';
					arg = body.Length() ? body[0] - '0' : 1;
					TInt m = body.Locate(0x02);
					TPtrC marker = m > 1 ? body.Mid(1, m - 1) : TPtrC(_L("\x95"));
					body.Set(m >= 0 ? body.Mid(m + 1) : body.Mid(body.Length() ? 1 : 0));
					t.Append(marker);
					t.Append(' ');
					break;
					}
				case 'q':
					kind = 'q';
					arg = body.Length() ? body[0] - '0' : 1;
					body.Set(body.Length() ? body.Mid(1) : body);
					break;
				case 'c': kind = 'c'; break;
				case 'i':
					kind = 'i';
					t.Append(_L("[Picture"));
					if (body.Length() > 1) { t.Append(_L(": ")); }
					break;
				case 's': kind = 's'; break;
				default: break;                   // 'p': a paragraph
					}
				while (body.Length() && body[0] == ' ' && kind != 'c') body.Set(body.Mid(1));
				line.Set(body);
				}
			// inline styles and links
			for (TInt i = 0; i < line.Length(); i++)
				{
				TText c = line[i];
				switch (c)
					{
				case 0x11: bold = t.Length(); break;
				case 0x12: if (bold >= 0) AddSpan(*spans, bold, t.Length() - bold, ESpanBold); bold = -1; break;
				case 0x13: ital = t.Length(); break;
				case 0x14: if (ital >= 0) AddSpan(*spans, ital, t.Length() - ital, ESpanItalic); ital = -1; break;
				case 0x15:
					{
					linkNo = 0;
					while (i + 1 < line.Length() && line[i + 1] >= '0' && line[i + 1] <= '9')
						linkNo = linkNo * 10 + (line[++i] - '0');
					if (i + 1 < line.Length() && line[i + 1] == 0x16) i++;
					link = t.Length();
					break;
					}
				case 0x17:
					if (link >= 0 && linkNo > 0 && t.Length() > link)
						{
						TPmLinkRange lr;
						lr.iPos = link; lr.iLen = t.Length() - link; lr.iLink = linkNo;
						iLinks->AppendL(lr);
						AddSpan(*spans, link, lr.iLen, ESpanUnder);
						}
					link = -1;
					break;
				default:
					if (c >= 0x20 || c == '\t')
						t.Append(c);
					break;
					}
				}
			if (kind == 'i') t.Append(']');
			// close styles still open at the end of the line
			if (bold >= 0) { AddSpan(*spans, bold, t.Length() - bold, ESpanBold); bold = t.Length() + 1; }
			if (ital >= 0) { AddSpan(*spans, ital, t.Length() - ital, ESpanItalic); ital = t.Length() + 1; }
			TInt plen = t.Length() - paraStart;
			switch (kind)
				{
			case 'h':
				AddSpan(*spans, paraStart, plen, ESpanBold);
				AddSpan(*spans, paraStart, plen, ESpanBig, arg == 1 ? 1500 : arg == 2 ? 1300 : 1150);
				AddSpan(*spans, paraStart, plen, ESpanSpaceBefore, 1);
				break;
			case 'l': AddSpan(*spans, paraStart, plen, ESpanIndent, arg * 2); break;
			case 'q':
				AddSpan(*spans, paraStart, plen, ESpanIndent, arg * 2);
				AddSpan(*spans, paraStart, plen, ESpanItalic);
				break;
			case 'c': AddSpan(*spans, paraStart, plen, ESpanMono); break;
			case 'i':
			case 's':
				AddSpan(*spans, paraStart, plen, ESpanItalic);
				break;
			default: break;
				}
			t.Append(KPara);
			}
		if (iTruncated > 0)
			{
			TBuf<120> m;
			m.Format(_L("[%d KB more not downloaded - Message > Whole message]"), (iTruncated + 1023) / 1024);
			p0 = t.Length();
			t.Append(m);
			AddSpan(*spans, p0, t.Length() - p0, ESpanItalic);
			t.Append(KPara);
			}
		}

	// into the editor, then the styles
	CRichText* rt = iReader->RichText();
	rt->Reset();
	rt->InsertL(0, t);
	// the built-in programs' text: Arial, 10 point (the zoom scales it)
	const TInt baseTwips = 200;
	{
	TCharFormat cf(_L("Arial"), baseTwips);
	TCharFormatMask cm;
	cm.SetAttrib(EAttFontTypeface);
	cm.SetAttrib(EAttFontHeight);
	rt->ApplyCharFormatL(cf, cm, 0, rt->DocumentLength());
	}
	for (TInt s = 0; s < spans->Count(); s++)
		{
		const TPmSpan& sp = (*spans)[s];
		if (sp.iPos + sp.iLen > rt->DocumentLength())
			continue;
		TCharFormat cf;
		TCharFormatMask cm;
		switch (sp.iKind)
			{
		case ESpanBold:
			cf.iFontSpec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
			cm.SetAttrib(EAttFontStrokeWeight);
			break;
		case ESpanItalic:
			cf.iFontSpec.iFontStyle.SetPosture(EPostureItalic);
			cm.SetAttrib(EAttFontPosture);
			break;
		case ESpanUnder:
			cf.iFontPresentation.iUnderline = EUnderlineOn;
			cm.SetAttrib(EAttFontUnderline);
			break;
		case ESpanBig:
			cf.iFontSpec.iHeight = baseTwips * sp.iArg / 1000;
			cm.SetAttrib(EAttFontHeight);
			break;
		case ESpanMono:
			cf.iFontSpec.iTypeface.iName = _L("Courier");
			cf.iFontSpec.iTypeface.SetIsProportional(EFalse);
			cm.SetAttrib(EAttFontTypeface);
			break;
		default:
			{
			CParaFormat* pf = CParaFormat::NewLC();
			TParaFormatMask pm;
			if (sp.iKind == ESpanIndent)
				{
				pf->iLeftMarginInTwips = sp.iArg * 120;
				pm.SetAttrib(EAttLeftMargin);
				}
			else if (sp.iKind == ESpanRuleBelow)
				{
				TParaBorder b;
				b.iLineStyle = TParaBorder::ESolid;
				b.iThickness = 1;
				pf->SetParaBorderL(CParaFormat::EParaBorderBottom, b);
				pf->iSpaceAfterInTwips = 120;
				pm.SetAttrib(EAttBottomBorder);
				pm.SetAttrib(EAttSpaceAfter);
				}
			else
				{
				pf->iSpaceBeforeInTwips = 80;
				pm.SetAttrib(EAttSpaceBefore);
				}
			rt->ApplyParaFormatL(pf, pm, sp.iPos, sp.iLen);
			CleanupStack::PopAndDestroy();   // pf
			continue;
			}
			}
		rt->ApplyCharFormatL(cf, cm, sp.iPos, sp.iLen);
		}
	CleanupStack::PopAndDestroy(2);         // spans, buf
	iReader->HandleTextChangedL();
	iReader->SetCursorPosL(0, EFalse);
	// reading, not writing: no cursor
	iReader->TextView()->SetCursorVisibilityL(TCursor::EFCursorInvisible, TCursor::EFCursorInvisible);
	iReader->UpdateScrollBarsL();
	}

// the address of link n (from the "\x01u<n> url" lines), 0 if none
TInt CPmView::NativeLinkUrl(TInt aLink, TDes& aUrl) const
	{
	aUrl.Zero();
	if (!iText || aLink <= 0)
		return 0;
	TBuf<8> tag;
	tag.Format(_L("\x01u%d "), aLink);
	TInt at = iText->Find(tag);
	if (at < 0)
		return 0;
	TPtrC rest = iText->Mid(at + tag.Length());
	TInt nl = rest.Locate('\n');
	TPtrC url = nl >= 0 ? rest.Left(nl) : rest;
	if (url.Length() && url[url.Length() - 1] == '\r')
		url.Set(url.Left(url.Length() - 1));
	aUrl.Copy(Clip(url, aUrl.MaxLength()));
	return aUrl.Length();
	}

// Tab / Shift+Tab: the next or previous link (or attachment), selected
void CPmView::NativeLinkStepL(TInt aDir)
	{
	TInt n = iLinks->Count();
	if (!n)
		{
		Toast(_L("No links in this message"));
		return;
		}
	iLinkSel += aDir;
	if (iLinkSel >= n) iLinkSel = 0;
	if (iLinkSel < 0) iLinkSel = n - 1;
	const TPmLinkRange& lr = (*iLinks)[iLinkSel];
	iReader->SetSelectionL(lr.iPos + lr.iLen, lr.iPos);
	}

void CPmView::NativeActivateLinkL()
	{
	if (iLinkSel < 0 || iLinkSel >= iLinks->Count())
		return;
	TInt link = (*iLinks)[iLinkSel].iLink;
	if (link <= -1000)
		{
		SaveAttachmentL(-link - 1000);
		return;
		}
	TBuf<256> url;
	if (!NativeLinkUrl(link, url))
		return;
	if (Clip(url, 7).CompareF(_L("mailto:")) == 0)
		{
		CPmDraft* d = CPmDraft::NewL();
		CleanupStack::PushL(d);
		TPtrC addr = url.Mid(7);
		TInt q = addr.Locate('?');
		if (q >= 0) addr.Set(addr.Left(q));
		d->iTo.Copy(Clip(addr, d->iTo.MaxLength()));
		CleanupStack::Pop();
		((CPmAppUi*)iEikonEnv->EikAppUi())->ComposeDraftL(d, _L("New message"));
		return;
		}
	OpenWebL(url);
	}

// ----- the status line (drawn here, under the controls) ----------------------------

void CPmView::UpdateStatusLine()
	{
	if (!iNativeShown || !IsReadyToDraw())
		return;
	ActivateGc();
	DrawStatus(SystemGc());
	DeactivateGc();
	}

void CPmView::DrawStatus(CWindowGc& gc) const
	{
	TRect r = Rect();
	TRect s(r.iTl.iX, r.iBr.iY - iStatusH, r.iBr.iX, r.iBr.iY);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.DrawRect(s);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbBlack);
	gc.DrawLine(s.iTl, TPoint(s.iBr.iX, s.iTl.iY));
	gc.UseFont(iSmallFont);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	TBuf<160> left;
	if (iToast.Length())
		left = iToast;
	else if (!iSplashDone && iRunning && iShared && iShared->state == PM_STATE_STARTING)
		left = _L("Starting the mail engine...");
	else if (Busy() && iLastProgress.Length())
		left = iLastProgress;
	else if (Busy() || CalendarBusy())
		left = _L("Working...");
	else if (iStatus.Length())
		left = iStatus;
	else if (iMode == EMessage)
		{
		if (iRows->Count())
			left.Format(_L("Message %d of %d.  Left / right: the one before / after.  Tab: links"), iSel + 1, iRows->Count());
		}
	else if (iMode == EOutbox)
		left.Format(_L("%d waiting to be sent"), iRows->Count());
	else if (iMode == EList && iRows->Count())
		{
		TInt unread = 0;
		for (TInt i = 0; i < iRows->Count(); i++)
			if ((*iRows)[i].iFlags.Locate('S') < 0) unread++;
		if (iSearch) left.Format(_L("%d found"), iRows->Count());
		else if (unread) left.Format(_L("%d messages, %d unread"), iRows->Count(), unread);
		else left.Format(_L("%d messages"), iRows->Count());
		}
	TBuf<40> right;
	if (iSettings->iOffline) right = _L("Offline");
	else if (iShared && iShared->online) right = _L("Online");
	const PmAccount& a = iSettings->iAccounts[iSettings->iAcct];
	if (a.used && a.name[0])
		{
		if (right.Length()) right.Append(_L("  \x95  "));
		TPtrC8 n((const TUint8*)a.name);
		right.Append(Clip(n, 20));
		}
	TInt base = s.iTl.iY + 2 + iSmallFont->AscentInPixels();
	TInt rw = iSmallFont->TextWidthInPixels(right);
	TInt room = s.Width() - rw - 12;
	while (left.Length() && iSmallFont->TextWidthInPixels(left) > room)
		left.SetLength(left.Length() - 1);
	gc.DrawText(left, TPoint(s.iTl.iX + 3, base));
	gc.DrawText(right, TPoint(s.iBr.iX - rw - 3, base));
	gc.DiscardFont();
	}

void CPmView::DrawNative(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	TRect r = Rect();
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.DrawRect(r);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbBlack);
	TInt h = r.Height() - iStatusH;
	if (iMode != EMessage)
		gc.DrawLine(TPoint(r.iTl.iX + iSplitX, r.iTl.iY), TPoint(r.iTl.iX + iSplitX, r.iTl.iY + h));
	// an empty folder, or no account yet: say so where the list would be
	TBool emptyList = (iMode == EList || iMode == EOutbox) && iRows->Count() == 0;
	if (emptyList || iMode == ENoAccount)
		{
		gc.UseFont(iListFont);
		gc.SetBrushStyle(CGraphicsContext::ENullBrush);
		TRect area(r.iTl.iX + iSplitX + 1, r.iTl.iY, r.iBr.iX, r.iTl.iY + h);
		TBuf<120> a, b;
		if (iMode == ENoAccount)
			{
			a = _L("Set up your mail with Tools > New account (Ctrl+K).");
			b = _L("For Fastmail, make an app password first.");
			}
		else if (iMode == EOutbox) a = _L("Nothing waiting to be sent");
		else if (iSearch) a = _L("Nothing found");
		else a = Busy() ? _L("Looking for messages...") : _L("No messages here");
		TInt lh = iListFont->HeightInPixels() + 4;
		TInt y = area.iTl.iY + area.Height() / 2 - lh;
		TInt w1 = iListFont->TextWidthInPixels(a);
		gc.DrawText(a, TPoint(area.iTl.iX + (area.Width() - w1) / 2, y + iListFont->AscentInPixels()));
		if (b.Length())
			{
			TInt w2 = iListFont->TextWidthInPixels(b);
			gc.DrawText(b, TPoint(area.iTl.iX + (area.Width() - w2) / 2, y + lh + iListFont->AscentInPixels()));
			}
		gc.DiscardFont();
		}
	}

// brings the controls up to date with the view's state (Render() calls this)
void CPmView::UpdateNativeL()
	{
	TBool modeChanged = iNativeMode != iMode;
	if (iMode == EList || iMode == EOutbox)
		{
		UpdateFolderListL();
		UpdateMessageListL();
		}
	else if (iMode == EMessage)
		UpdateReaderL();
	else if (iMode == ENoAccount)
		UpdateFolderListL();
	TBool wantMsgList = (iMode == EList || iMode == EOutbox) && iRows->Count() > 0;
	if (modeChanged || wantMsgList != iMsgList->IsVisible() ||
		iFolderList->IsFocused() != ((iMode == EList || iMode == EOutbox) && iSidebar))
		{
		ShowNative(ETrue);
		iNativeMode = iMode;
		DrawNow();
		}
	else
		UpdateStatusLine();                     // (the lists redraw themselves when they change)
	}

void CPmView::SyncSelectionFromLists()
	{
	if (iMode == EList || iMode == EOutbox)
		{
		if (iSidebar)
			iFolderSel = iFolderList->CurrentItemIndex();
		else if (iRows->Count())
			iSel = iMsgList->CurrentItemIndex();
		}
	}

// ----- keys and the pen -------------------------------------------------------------

static TBool IsListKey(TUint aCode)
	{
	return aCode == EKeyUpArrow || aCode == EKeyDownArrow || aCode == EKeyPageUp ||
		aCode == EKeyPageDown || aCode == EKeyHome || aCode == EKeyEnd;
	}

TKeyResponse CPmView::NativeKeyL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	TUint code = aKeyEvent.iCode;
	if (iMode == EList || iMode == EOutbox)
		{
		if (IsListKey(code))
			{
			if (iSidebar)
				iFolderList->OfferKeyEventL(aKeyEvent, aType);
			else if (iRows->Count())
				iMsgList->OfferKeyEventL(aKeyEvent, aType);
			SyncSelectionFromLists();
			UpdateStatusLine();
			return EKeyWasConsumed;
			}
		switch (code)
			{
		case EKeyEnter:
		case EKeyRightArrow:
			SyncSelectionFromLists();
			OpenCurrentL();
			return EKeyWasConsumed;
		case EKeyTab:
			SyncSelectionFromLists();
			if (iSidebar) OpenCurrentL(); else FocusFoldersL();
			return EKeyWasConsumed;
		case EKeyLeftArrow:
			if (!iSidebar) FocusFoldersL();
			return EKeyWasConsumed;
		case EKeyEscape:
			BackL();
			return EKeyWasConsumed;
		case EKeyDelete:
		case EKeyBackspace:
			if (!iSidebar)
				{
				SyncSelectionFromLists();
				DeleteCurrentL();
				}
			return EKeyWasConsumed;
		default:
			return EKeyWasNotConsumed;
			}
		}
	if (iMode == EMessage)
		{
		switch (code)
			{
		case EKeyUpArrow: iReader->MoveDisplayL(TCursorPosition::EFLineUp); break;
		case EKeyDownArrow: iReader->MoveDisplayL(TCursorPosition::EFLineDown); break;
		case EKeyPageUp: iReader->MoveDisplayL(TCursorPosition::EFPageUp); break;
		case EKeyPageDown:
		case ' ':
			iReader->MoveDisplayL(TCursorPosition::EFPageDown);
			break;
		case EKeyHome: iReader->SetCursorPosL(0, EFalse); break;
		case EKeyEnd: iReader->SetCursorPosL(iReader->TextLength(), EFalse); break;
		case EKeyLeftArrow: StepMessageL(-1); return EKeyWasConsumed;
		case EKeyRightArrow: StepMessageL(1); return EKeyWasConsumed;
		case EKeyTab: NativeLinkStepL((aKeyEvent.iModifiers & EModifierShift) ? -1 : 1); break;
		case EKeyEnter: NativeActivateLinkL(); return EKeyWasConsumed;
		case EKeyEscape:
			if (iLinkSel >= 0)
				{
				iLinkSel = -1;
				iReader->SetCursorPosL(iReader->CursorPos(), EFalse);
				}
			else
				BackL();
			return EKeyWasConsumed;
		case EKeyDelete:
			DeleteCurrentL();
			return EKeyWasConsumed;
		default:
			return EKeyWasNotConsumed;
			}
		iReader->UpdateScrollBarsL();
		return EKeyWasConsumed;
		}
	return EKeyWasNotConsumed;
	}

void CPmView::HandleListBoxEventL(CEikListBox* aListBox, TListBoxEvent aEventType)
	{
	if (aListBox == iFolderList)
		{
		// a tap on a folder opens it
		iSidebar = ETrue;
		iFolderSel = iFolderList->CurrentItemIndex();
		OpenCurrentL();
		return;
		}
	if (aListBox == iMsgList)
		{
		TBool wasSide = iSidebar;
		iSidebar = EFalse;
		iSel = iMsgList->CurrentItemIndex();
		if (aEventType == EEventItemDoubleClicked || aEventType == EEventEnterKeyPressed)
			OpenCurrentL();
		else if (wasSide)
			Render();
		else
			UpdateStatusLine();
		}
	}
