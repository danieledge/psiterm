// pmheader.cpp - the header at the top of the reader (0.75)
//
// It was five lines of "Name: value". Now, as a first-party program lays a
// message out, it is rich text in the reader itself, so it scrolls, zooms,
// prints and takes Tab and the pen as the rest of the message does:
//
//   [flag] Re: Minutes of the extraordinary general meeting of the
//          Series 5mx owners club, with the agenda for next…            subject: bold, larger, 2 lines at most
//   Margaret Hamilton-Smythe  margaret@example.ac.uk     30/09/2026 12:59   sender bold, address grey; the date on a right tab
//   To: me, Alice, Bob, +5 others                                     smaller; "+5 others" shows them all
//   [@ Minutes.doc 18 KB] [@ agenda.txt 3 KB] [@ scan.jpg 1.6 MB]     attachments as chips: a tap (or Tab, Enter) opens one
//   ------------------------------------------------------------      a rule, then the invitation or card box and the text
//
// Black on white, grey (EIKON's dark grey) only for what is secondary. The
// chips and the flag are CPictures in the text, as the message's pictures
// are (pmpict.cpp). UpdateReaderL (pmnative.cpp) calls HeaderTextL for the
// paragraphs, then HeaderFormatL once they are in the editor; anything that
// belongs under the header goes in after HeaderTextL (HeaderEnd() is where
// the header stops).

#include "pmapp.h"
#include "pmicons.h"
#include "pmpict.h"
#include <eikrted.h>
#include <txtrich.h>
#include <txtfrmat.h>
#include <frmtview.h>

// the header's fonts (twips, before the zoom): the reader's text is 200
const TInt KHdSubject = 240;
const TInt KHdName = 200;
const TInt KHdSmall = 170;
const TInt KHdChip = 160;

// what HeaderTextL leaves for HeaderFormatL: (position, length, kind, value)
enum { EHdBold, EHdSize, EHdGrey, EHdUnder, EHdTab, EHdRule, EHdSpace, EHdPicture, EHdFill, EHdRight };
// EHdPicture values
enum { EHdPicFlag = -1, EHdPicUnread = -2 };          // 0 up: attachment n's chip

// ----- the pictures in the header ---------------------------------------------

// a picture from PsiMail.mbm (the message list's own, so the two agree)
class CPmHdIcon : public CPicture
	{
public:
	// aHeight: the line's height above the baseline (the picture fills it,
	// white, so nothing is left there from before)
	CPmHdIcon(CFbsBitmap* aBitmap, MGraphicsDeviceMap* aMap, TInt aHeight) : iBitmap(aBitmap)
		{
		iBmp = aBitmap ? aBitmap->SizeInPixels() : TSize(8, 8);
		iPixels = TSize(iBmp.iWidth + 4, iBmp.iHeight > aHeight ? iBmp.iHeight : aHeight);
		iTwips = TSize(aMap->HorizontalPixelsToTwips(iPixels.iWidth), aMap->VerticalPixelsToTwips(iPixels.iHeight));
		}
	void Draw(CGraphicsContext& aGc, const TPoint& aTopLeft, const TRect& aClipRect, MGraphicsDeviceMap* aMap) const
		{
		if (!iBitmap)
			return;
		TSize size(iPixels);
		TSize bmp(iBmp);
		if (aMap)
			{
			// (printed: its size in twips on that device)
			TSize m(aMap->HorizontalTwipsToPixels(iTwips.iWidth), aMap->VerticalTwipsToPixels(iTwips.iHeight));
			if (m.iWidth > 0 && m.iHeight > 0 && (m.iWidth * 4 > iPixels.iWidth * 5 || m.iWidth * 5 < iPixels.iWidth * 4))
				{
				bmp = TSize(iBmp.iWidth * m.iWidth / iPixels.iWidth, iBmp.iHeight * m.iHeight / iPixels.iHeight);
				size = m;
				}
			}
		aGc.SetClippingRect(aClipRect);
		aGc.SetPenStyle(CGraphicsContext::ENullPen);
		aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		aGc.SetBrushColor(KRgbWhite);
		aGc.DrawRect(TRect(aTopLeft, size));
		aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
		// (white where the mask is clear: the reader is white)
		aGc.DrawBitmap(TRect(TPoint(aTopLeft.iX, aTopLeft.iY + (size.iHeight - bmp.iHeight) * 2 / 3), bmp), iBitmap);
		aGc.CancelClippingRect();
		}
	void ExternalizeL(RWriteStream& /*aStream*/) const {}
	void GetOriginalSizeInTwips(TSize& aSize) const { aSize = iTwips; }
private:
	CFbsBitmap* iBitmap;       // (iIcons': the reader goes before them)
	TSize iBmp;
	TSize iPixels;
	TSize iTwips;
	};

// an attachment: a small white box with the paperclip, the name and the size
class CPmHdChip : public CPicture
	{
public:
	static CPmHdChip* NewL(CFbsBitmap* aClip, MGraphicsDeviceMap* aMap, TInt aFontTwips, const TDesC& aName, const TDesC& aSize, TInt aMaxWidth);
	~CPmHdChip();
	void Draw(CGraphicsContext& aGc, const TPoint& aTopLeft, const TRect& aClipRect, MGraphicsDeviceMap* aMap) const;
	void ExternalizeL(RWriteStream& /*aStream*/) const {}
	void GetOriginalSizeInTwips(TSize& aSize) const { aSize = iTwips; }
private:
	void DrawIn(CGraphicsContext& aGc, const TRect& aBox, const CFont* aFont, TInt aClipW, TInt aClipH) const;
	CFbsBitmap* iClip;
	MGraphicsDeviceMap* iMap;  // the reader's zoom (it outlives the reader's text)
	CFont* iFont;              // the screen's, at the zoom
	TInt iFontTwips;
	TBuf<64> iName;            // shortened to fit
	TBuf<16> iSize;
	TSize iPixels;
	TSize iTwips;
	};

const TInt KChipPad = 4;

CPmHdChip* CPmHdChip::NewL(CFbsBitmap* aClip, MGraphicsDeviceMap* aMap, TInt aFontTwips, const TDesC& aName, const TDesC& aSize, TInt aMaxWidth)
	{
	CPmHdChip* c = new(ELeave) CPmHdChip;
	CleanupStack::PushL(c);
	c->iClip = aClip;
	c->iMap = aMap;
	c->iFontTwips = aFontTwips;
	User::LeaveIfError(aMap->GetNearestFontInTwips(c->iFont, TFontSpec(_L("Arial"), aFontTwips)));
	TInt clipW = aClip ? aClip->SizeInPixels().iWidth : 0;
	c->iSize = Clip(aSize, c->iSize.MaxLength());
	TInt fixed = KChipPad + clipW + 2 + 8 + c->iFont->TextWidthInPixels(c->iSize) + KChipPad + 4;
	// the name, shortened in the middle of nowhere: from the end, with "..."
	TPtrC name = Clip(aName, 60);
	TInt room = aMaxWidth - fixed;
	if (room < 40) room = 40;
	if (c->iFont->TextWidthInPixels(name) <= room)
		c->iName = name;
	else
		{
		TInt n = name.Length();
		TBuf<64> t;
		while (n > 1)
			{
			t = name.Left(--n);
			t.Append(TChar(0x85));
			if (c->iFont->TextWidthInPixels(t) <= room)
				break;
			}
		c->iName = t;
		}
	TInt h = c->iFont->HeightInPixels() + 6;
	if (aClip && h < aClip->SizeInPixels().iHeight + 4) h = aClip->SizeInPixels().iHeight + 4;
	c->iPixels = TSize(fixed + c->iFont->TextWidthInPixels(c->iName), h);
	c->iTwips = TSize(aMap->HorizontalPixelsToTwips(c->iPixels.iWidth), aMap->VerticalPixelsToTwips(c->iPixels.iHeight));
	CleanupStack::Pop();
	return c;
	}

CPmHdChip::~CPmHdChip()
	{
	if (iFont)
		iMap->ReleaseFont(iFont);
	}

void CPmHdChip::DrawIn(CGraphicsContext& aGc, const TRect& aBox, const CFont* aFont, TInt aClipW, TInt aClipH) const
	{
	// white behind it all (its line is not otherwise cleared), then the
	// box: white, with EIKON's dark grey edge (as a dialog's edit line)
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(KRgbWhite);
	aGc.DrawRect(aBox);
	TRect box(aBox.iTl.iX, aBox.iTl.iY + 1, aBox.iBr.iX - 4, aBox.iBr.iY - 1);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(KPmDarkGrey);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(KRgbWhite);
	aGc.DrawRect(box);
	TInt x = box.iTl.iX + KChipPad;
	if (iClip && aClipW > 0)
		{
		aGc.DrawBitmap(TRect(TPoint(x, box.iTl.iY + (box.Height() - aClipH) / 2), TSize(aClipW, aClipH)), iClip);
		x += aClipW + 2;
		}
	aGc.UseFont(aFont);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	TInt base = box.iTl.iY + (box.Height() - aFont->HeightInPixels()) / 2 + aFont->AscentInPixels();
	aGc.SetPenColor(KRgbBlack);
	aGc.DrawText(iName, TPoint(x, base));
	x += aFont->TextWidthInPixels(iName) + 8;
	aGc.SetPenColor(KPmDarkGrey);
	aGc.DrawText(iSize, TPoint(x, base));
	aGc.DiscardFont();
	}

void CPmHdChip::Draw(CGraphicsContext& aGc, const TPoint& aTopLeft, const TRect& aClipRect, MGraphicsDeviceMap* aMap) const
	{
	aGc.SetClippingRect(aClipRect);
	TSize size(iPixels);
	TBool screen = ETrue;
	if (aMap && aMap != iMap && iTwips.iWidth > 0)
		{
		TSize m(aMap->HorizontalTwipsToPixels(iTwips.iWidth), aMap->VerticalTwipsToPixels(iTwips.iHeight));
		if (m.iWidth > 0 && m.iHeight > 0 && (m.iWidth * 4 > iPixels.iWidth * 5 || m.iWidth * 5 < iPixels.iWidth * 4))
			{
			size = m;
			screen = EFalse;
			}
		}
	TInt cw = iClip ? iClip->SizeInPixels().iWidth : 0;
	TInt ch = iClip ? iClip->SizeInPixels().iHeight : 0;
	if (screen)
		DrawIn(aGc, TRect(aTopLeft, size), iFont, cw, ch);
	else
		{
		// printed, or the print preview: that device's font and sizes
		CFont* f = NULL;
		if (aMap->GetNearestFontInTwips(f, TFontSpec(_L("Arial"), iFontTwips)) == KErrNone && f)
			{
			DrawIn(aGc, TRect(aTopLeft, size), f, cw * size.iWidth / iPixels.iWidth, ch * size.iHeight / iPixels.iHeight);
			aMap->ReleaseFont(f);
			}
		}
	aGc.CancelClippingRect();
	}

// ----- measuring -----------------------------------------------------------------

// how many lines aText takes at aWidth, wrapped at spaces as the editor
// does it (a word wider than a line is broken where it must be)
static TInt LinesFor(const CFont* aFont, const TDesC& aText, TInt aWidth, TInt aIndent)
	{
	TInt lines = 1;
	TInt x = aIndent;
	TInt space = aFont->TextWidthInPixels(_L(" "));
	TInt i = 0, n = aText.Length();
	while (i < n)
		{
		TInt j = i;
		while (j < n && aText[j] != ' ') j++;
		TPtrC word = aText.Mid(i, j - i);
		TInt w = aFont->TextWidthInPixels(word);
		if (x > 0 && x + w > aWidth)
			{
			lines++;
			x = 0;
			}
		while (w > aWidth && word.Length() > 1)
			{
			// (broken across lines)
			TInt k = aFont->TextCount(word, aWidth - x);
			if (k < 1) k = 1;
			word.Set(word.Mid(k));
			lines++;
			x = 0;
			w = aFont->TextWidthInPixels(word);
			}
		x += w;
		while (j < n && aText[j] == ' ')
			{
			x += space;
			j++;
			}
		i = j;
		}
	return lines;
	}

// aText cut to aLines lines with "..." at the end of the last, if it is longer
static void ClampLines(const CFont* aFont, const TDesC& aText, TInt aWidth, TInt aIndent, TInt aLines, TDes& aOut)
	{
	TPtrC text = Clip(aText, aOut.MaxLength() - 1);
	if (LinesFor(aFont, text, aWidth, aIndent) <= aLines)
		{
		aOut = text;
		return;
		}
	TInt lo = 0, hi = text.Length();       // lo fits (with the dots), hi doesn't
	while (hi - lo > 1)
		{
		TInt mid = (lo + hi) / 2;
		aOut = text.Left(mid);
		aOut.Append(TChar(0x85));
		if (LinesFor(aFont, aOut, aWidth, aIndent) <= aLines)
			lo = mid;
		else
			hi = mid;
		}
	TPtrC keep = text.Left(lo);
	while (keep.Length() && keep[keep.Length() - 1] == ' ')
		keep.Set(keep.Left(keep.Length() - 1));
	aOut = keep;
	aOut.Append(TChar(0x85));
	}

// ----- addresses -------------------------------------------------------------------

// one address of a list: "Name <addr>", "addr" or "\"Name\" <addr>"
static void SplitAddress(const TDesC& aItem, TPtrC& aName, TPtrC& aAddr)
	{
	TPtrC it(aItem);
	while (it.Length() && it[0] == ' ') it.Set(it.Mid(1));
	while (it.Length() && it[it.Length() - 1] == ' ') it.Set(it.Left(it.Length() - 1));
	TInt lt = it.LocateReverse('<');
	if (lt < 0)
		{
		aName.Set(TPtrC());
		aAddr.Set(it);
		return;
		}
	TPtrC addr = it.Mid(lt + 1);
	TInt gt = addr.Locate('>');
	aAddr.Set(gt >= 0 ? addr.Left(gt) : addr);
	TPtrC name = it.Left(lt);
	while (name.Length() && (name[name.Length() - 1] == ' ' || name[name.Length() - 1] == '"'))
		name.Set(name.Left(name.Length() - 1));
	while (name.Length() && (name[0] == ' ' || name[0] == '"'))
		name.Set(name.Mid(1));
	aName.Set(name);
	}

// the next address of a list (commas inside quotes or <> don't count)
static TBool NextAddress(TPtrC& aRest, TPtrC& aItem)
	{
	while (aRest.Length() && (aRest[0] == ' ' || aRest[0] == ','))
		aRest.Set(aRest.Mid(1));
	if (!aRest.Length())
		return EFalse;
	TBool quote = EFalse, angle = EFalse;
	TInt i = 0;
	for (; i < aRest.Length(); i++)
		{
		TText c = aRest[i];
		if (c == '"') quote = !quote;
		else if (c == '<') angle = ETrue;
		else if (c == '>') angle = EFalse;
		else if (c == ',' && !quote && !angle) break;
		}
	aItem.Set(aRest.Left(i));
	aRest.Set(aRest.Mid(i));
	return ETrue;
	}

// one recipient as the collapsed line shows it: the name ("me" for you), or
// the address; aShort: the first name, or the address before the @
static void ShortName(const TDesC& aItem, const TDesC& aMe, TBool aShort, TDes& aOut)
	{
	TPtrC name, addr;
	SplitAddress(aItem, name, addr);
	aOut.Zero();
	if (aMe.Length() && addr.CompareF(aMe) == 0)
		{
		aOut = _L("me");
		return;
		}
	if (name.Length())
		{
		TInt sp = name.Locate(' ');
		aOut.Copy(Clip(aShort && sp > 0 ? name.Left(sp) : name, aOut.MaxLength()));
		}
	else
		{
		TInt at = addr.Locate('@');
		aOut.Copy(Clip(aShort && at > 0 ? addr.Left(at) : addr, aOut.MaxLength()));
		}
	}

static TInt CountAddresses(const TDesC& aList)
	{
	TPtrC rest(aList), item;
	TInt n = 0;
	while (NextAddress(rest, item)) n++;
	return n;
	}

// ----- the header ---------------------------------------------------------------------

static void Run(CArrayFixFlat<TInt>& aRuns, TInt aPos, TInt aLen, TInt aKind, TInt aValue = 0)
	{
	if (aLen <= 0)
		return;
	TRAPD(err, aRuns.AppendL(aPos); aRuns.AppendL(aLen); aRuns.AppendL(aKind); aRuns.AppendL(aValue));
	(void)err;
	}

// a secondary font's twips: aBase, or more at the small zoom, so it is never
// below 150 twips on the screen (the list's small size)
static TInt SmallTwips(TInt aBase, TInt aZoom)
	{
	TInt t = aZoom > 0 ? 150 * 1000 / aZoom : aBase;
	return t > aBase ? t : aBase;
	}

static CFont* ZoomFont(MGraphicsDeviceMap* aMap, TInt aTwips, TBool aBold)
	{
	TFontSpec spec(_L("Arial"), aTwips);
	if (aBold)
		spec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	CFont* f = NULL;
	if (aMap->GetNearestFontInTwips(f, spec) != KErrNone)
		return NULL;
	return f;
	}

// HeaderTextL's working text, on the heap: about 2.5 KB of TBufs, which
// in one frame under CONE's and EIKON's own (the app thread's stack is
// small, and not ours to set) is more than is safe
struct THdScratch
	{
	TBuf<300> iV, iLine, iFrom, iShown, iNext;
	TBuf<500> iTo, iCc;
	TBuf<96> iMe;
	TBuf<80> iNm;
	};

void CPmView::HeaderTextL(TDes& aText, const TPmRow* aRow)
	{
	if (!iHeaderRuns)
		iHeaderRuns = new(ELeave) CArrayFixFlat<TInt>(32);
	iHeaderRuns->Reset();
	if (iHeaderUid != iMsgUid)
		{
		iHeaderUid = iMsgUid;
		iHeaderAllTo = EFalse;
		}
	THdScratch* sc = new(ELeave) THdScratch;
	CleanupStack::PushL(sc);
	TDes& v = sc->iV;
	TDes& line = sc->iLine;
	TDes& from = sc->iFrom;
	TDes& shown = sc->iShown;
	TDes& to = sc->iTo;
	TDes& cc = sc->iCc;
	TDes& me = sc->iMe;
	const TChar KPara(CEditableText::EParagraphDelimiter);
	CArrayFixFlat<TInt>& runs = *iHeaderRuns;
	// the room across the reader (as it is laid out at this zoom), less a
	// little: what is measured here must not wrap there
	TInt width = iReader->TextView() ? iReader->TextView()->ViewRect().Width() : 0;
	if (width < 100)
		width = iReader->Rect().Width();
	if (width < 100)
		width = 500;
	width -= 10;
	CFont* subjFont = ZoomFont(iZoomFactor, KHdSubject, ETrue);
	CFont* nameFont = ZoomFont(iZoomFactor, KHdName, ETrue);
	// (secondary text no smaller than the list's smallest, at the small zoom)
	TInt smallTw = SmallTwips(KHdSmall, iZoomFactor->ZoomFactor());
	CFont* smallFont = ZoomFont(iZoomFactor, smallTw, EFalse);
	if (!subjFont || !nameFont || !smallFont)
		{
		if (subjFont) iZoomFactor->ReleaseFont(subjFont);
		if (nameFont) iZoomFactor->ReleaseFont(nameFont);
		if (smallFont) iZoomFactor->ReleaseFont(smallFont);
		User::Leave(KErrNoMemory);
		}
	TInt p0;

	// the subject: the flag (and unread) in front, two lines at most
	p0 = aText.Length();
	TInt indent = 0;
	TBool flagged = aRow && aRow->iFlags.Locate('F') >= 0;
	TBool unread = aRow && aRow->iFlags.Locate('S') < 0;
	if (flagged && iIcons && iIcons->Count() > EMbmMsgFlag)
		{
		Run(runs, aText.Length(), 1, EHdPicture, EHdPicFlag);
		aText.Append(' ');
		indent += iIcons->At(EMbmMsgFlag)->SizeInPixels().iWidth + 4;
		}
	if (unread && iIcons && iIcons->Count() > EMbmMsgUnread)
		{
		Run(runs, aText.Length(), 1, EHdPicture, EHdPicUnread);
		aText.Append(' ');
		indent += iIcons->At(EMbmMsgUnread)->SizeInPixels().iWidth + 4;
		}
	if (!MessageHeader(_L("Subject"), v) || !v.Length())
		{
		if (aRow && aRow->iSubject.Length()) v.Copy(Clip(aRow->iSubject, v.MaxLength()));
		else v = _L("(no subject)");
		}
	ClampLines(subjFont, v, width, indent, 2, line);
	TInt s0 = aText.Length();
	aText.Append(line);
	Run(runs, s0, aText.Length() - s0, EHdBold);
	Run(runs, s0, aText.Length() - s0, EHdSize, KHdSubject);
	Run(runs, p0, aText.Length() - p0, EHdSpace, 40);
	aText.Append(KPara);
	TInt lastPara = p0;

	// the sender and the date: "Name  address" on the left, the date on the right
	TPtrC name, addr;
	from.Zero();
	if (MessageHeader(_L("From"), from) && from.Length())
		SplitAddress(from, name, addr);
	else if (aRow)
		{
		from.Copy(Clip(aRow->iFrom, from.MaxLength()));   // (the list's: the name, or the address)
		name.Set(from);
		}
	if (!name.Length())
		{
		name.Set(addr);
		addr.Set(TPtrC());
		}
	TBuf<40> date;
	if (aRow && aRow->iDate > 0)
		{
		TTime t(TDateTime(1970, EJanuary, 0, 0, 0, 0, 0));
		t += TTimeIntervalSeconds(aRow->iDate);
		t += TLocale().UniversalTimeOffset();
		TRAPD(err, t.FormatL(date, _L("%D%M%Y%/0%1%/1%2%/2%3%/3 %-B%:0%J%:1%T%+B")));
		if (err) date.Zero();
		}
	if (!date.Length() && MessageHeader(_L("Date"), v))
		date.Copy(Clip(v, date.MaxLength()));
	TInt dateW = date.Length() ? smallFont->TextWidthInPixels(date) + 16 : 0;
	TInt nameW = nameFont->TextWidthInPixels(name);
	TInt addrW = addr.Length() ? smallFont->TextWidthInPixels(_L("  ")) + smallFont->TextWidthInPixels(addr) : 0;
	// as much as fits on the first line: the name, the address, the date
	// (a long name, or a bare address, goes in full: it may wrap)
	TBool dateFirst = nameW + dateW <= width;
	TBool addrFirst = addr.Length() && dateFirst && nameW + addrW + dateW <= width;
	p0 = aText.Length();
	if (name.Length())
		{
		aText.Append(Clip(name, 120));
		Run(runs, p0, aText.Length() - p0, EHdBold);
		}
	if (addrFirst)
		{
		aText.Append(_L("  "));
		TInt a0 = aText.Length();
		aText.Append(Clip(addr, 120));
		Run(runs, a0, aText.Length() - a0, EHdGrey);
		Run(runs, a0, aText.Length() - a0, EHdSize, smallTw);
		}
	TBool dateDone = EFalse;
	for (TInt line = 0; line < 3; line++)
		{
		// line 0: the name's; 1: the address (and the date, if they fit
		// together); 2: the date on its own
		if (line == 1)
			{
			if (!addr.Length() || addrFirst)
				continue;
			aText.Append(Clip(addr, 120));
			Run(runs, p0, aText.Length() - p0, EHdGrey);
			Run(runs, p0, aText.Length() - p0, EHdSize, smallTw);
			}
		if (date.Length() && !dateDone && (line == 2 || (line == 0 && dateFirst) ||
			(line == 1 && addrW + dateW <= width)))
			{
			if (line < 2)
				aText.Append('\t');
			TInt d0 = aText.Length();
			aText.Append(date);
			Run(runs, d0, aText.Length() - d0, EHdGrey);
			Run(runs, d0, aText.Length() - d0, EHdSize, smallTw);
			Run(runs, p0, aText.Length() - p0, line < 2 ? EHdTab : EHdRight, width);   // (on its own: right-aligned)
			dateDone = ETrue;
			}
		if (aText.Length() > p0)
			{
			lastPara = p0;
			aText.Append(KPara);
			}
		p0 = aText.Length();
		}

	// the recipients, smaller: on one line ("To: me, Alice, +3 others"),
	// all of them once "+3 others" is tapped
	to.Zero();
	cc.Zero();
	MessageHeader(_L("To"), to);
	MessageHeader(_L("Cc"), cc);
	me.Zero();
	if (iSettings->iAcct >= 0 && iSettings->iAcct < PM_MAX_ACCOUNTS)
		{
		const char* e = iSettings->iAccounts[iSettings->iAcct].email;
		for (TInt k = 0; k < 95 && e[k]; k++) me.Append((TText)(TUint8)e[k]);
		}
	if (to.Length() || cc.Length())
		{
		const TText* KLabel[2] = { _S("To: "), _S("Cc: ") };
		TDesC* lists[2] = { &to, &cc };
		if (iHeaderAllTo)
			{
			// everyone, in full, a paragraph for To and one for Cc
			for (TInt l = 0; l < 2; l++)
				{
				if (!lists[l]->Length())
					continue;
				p0 = aText.Length();
				aText.Append(TPtrC(KLabel[l]));
				Run(runs, p0, aText.Length() - p0, EHdGrey);
				TPtrC rest(*lists[l]), item;
				TBool first = ETrue;
				while (NextAddress(rest, item) && aText.Length() < aText.MaxLength() - 300)
					{
					if (!first) aText.Append(_L(", "));
					first = EFalse;
					TPtrC nm, ad;
					SplitAddress(item, nm, ad);
					if (nm.Length())
						{
						aText.Append(Clip(nm, 60));
						TInt g0 = aText.Length();
						aText.Append(_L(" <"));
						aText.Append(Clip(ad, 80));
						aText.Append('>');
						Run(runs, g0, aText.Length() - g0, EHdGrey);
						}
					else
						aText.Append(Clip(ad, 80));
					}
				Run(runs, p0, aText.Length() - p0, EHdSize, smallTw);
				lastPara = p0;
				aText.Append(KPara);
				}
			}
		else
			{
			// names only; first names if that is what it takes; then as
			// many as fit, and how many more
			TInt total = CountAddresses(to) + CountAddresses(cc);
			shown.Zero();
			TInt fitted = 0;
			TBool done = EFalse;
			for (TInt pass = 0; pass < 3 && !done; pass++)
				{
				TBool shortNames = pass > 0;
				shown.Zero();
				fitted = 0;
				TBool overflow = EFalse;
				for (TInt l = 0; l < 2 && !overflow; l++)
					{
					if (!lists[l]->Length())
						continue;
					TPtrC rest(*lists[l]), item;
					TBool first = ETrue;
					while (NextAddress(rest, item))
						{
						TDes& nm = sc->iNm;
						ShortName(item, me, shortNames, nm);
						TDes& next = sc->iNext;
						next = shown;
						if (first)
							{
							if (next.Length()) next.Append(_L("   "));
							if (l == 1 || to.Length() == 0 || next.Length()) next.Append(TPtrC(KLabel[l]));
							}
						else
							next.Append(_L(", "));
						if (next.Length() + nm.Length() > next.MaxLength() - 2)
							{
							overflow = ETrue;
							break;
							}
						next.Append(nm);
						// (room for "+N others" while there are more to come)
						TBuf<24> more;
						if (fitted + 1 < total)
							more.Format(_L(", +%d others"), total - fitted - 1);
						TInt w = smallFont->TextWidthInPixels(_L("To: ")) + smallFont->TextWidthInPixels(next) +
							smallFont->TextWidthInPixels(more);
						if (w > width && (pass < 2 || fitted > 0))
							{
							overflow = ETrue;
							break;
							}
						shown = next;
						fitted++;
						first = EFalse;
						}
					}
				done = !overflow || pass == 2;
				}
			p0 = aText.Length();
			if (to.Length())
				{
				aText.Append(TPtrC(KLabel[0]));
				Run(runs, p0, aText.Length() - p0, EHdGrey);
				}
			// (the labels inside are grey too)
			TInt b0 = aText.Length();
			aText.Append(shown);
			TInt cl = shown.Find(_L("Cc: "));
			if (cl >= 0)
				Run(runs, b0 + cl, 4, EHdGrey);
			if (fitted < total)
				{
				aText.Append(_L(", "));
				TPmLinkRange lr;
				lr.iPos = aText.Length();
				TInt more = total - fitted;
				if (more == 1)
					aText.Append(_L("+1 other"));
				else
					aText.AppendFormat(_L("+%d others"), more);
				lr.iLen = aText.Length() - lr.iPos;
				lr.iLink = KPmLinkHeaderTo;
				iLinks->AppendL(lr);
				Run(runs, lr.iPos, lr.iLen, EHdUnder);
				}
			Run(runs, p0, aText.Length() - p0, EHdSize, smallTw);
			lastPara = p0;
			aText.Append(KPara);
			}
		}

	// the attachments: a chip each (a tap, or Tab and Enter, opens it)
	if (iAttNames->Count())
		{
		p0 = aText.Length();
		for (TInt a = 0; a < iAttNames->Count() && a < 16; a++)
			{
			TPmLinkRange lr;
			lr.iPos = aText.Length();
			lr.iLen = 1;
			lr.iLink = -1000 - a;
			iLinks->AppendL(lr);
			Run(runs, aText.Length(), 1, EHdPicture, a);
			aText.Append(' ');
			}
		Run(runs, p0, aText.Length() - p0, EHdSpace, 40);
		Run(runs, p0, aText.Length() - p0, EHdSize, 40);   // (the line no deeper than the chips)
		lastPara = p0;
		aText.Append(KPara);
		}

	// a rule under it all; white behind it all (a picture's line is not
	// otherwise cleared below the picture)
	Run(runs, lastPara, 1, EHdRule);
	iHeaderEnd = aText.Length();
	Run(runs, 0, iHeaderEnd, EHdFill);
	iZoomFactor->ReleaseFont(subjFont);
	iZoomFactor->ReleaseFont(nameFont);
	iZoomFactor->ReleaseFont(smallFont);
	CleanupStack::PopAndDestroy();           // sc
	}

void CPmView::HeaderFormatL(CRichText& aText)
	{
	if (!iHeaderRuns)
		return;
	CArrayFixFlat<TInt>& runs = *iHeaderRuns;
	TInt doc = aText.DocumentLength();
	// the white behind it first (applied after them, it took the tab stops
	// away), pictures last: they are put in place of their stand-in spaces
	for (TInt pass = 0; pass < 3; pass++)
	for (TInt i = 0; i + 3 < runs.Count(); i += 4)
		{
		TInt pos = runs[i], len = runs[i + 1], kind = runs[i + 2], value = runs[i + 3];
		if (pos < 0 || pos + len > doc)
			continue;
		if ((kind == EHdFill ? 0 : kind == EHdPicture ? 2 : 1) != pass)
			continue;
		TCharFormat cf;
		TCharFormatMask cm;
		switch (kind)
			{
		case EHdBold:
			cf.iFontSpec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
			cm.SetAttrib(EAttFontStrokeWeight);
			break;
		case EHdSize:
			cf.iFontSpec.iHeight = value;
			cm.SetAttrib(EAttFontHeight);
			break;
		case EHdGrey:
			cf.iFontPresentation.iTextColor = KPmDarkGrey;
			cm.SetAttrib(EAttColor);
			break;
		case EHdUnder:
			cf.iFontPresentation.iUnderline = EUnderlineOn;
			cm.SetAttrib(EAttFontUnderline);
			break;
		case EHdPicture:
			{
			CPicture* pic = NULL;
			if (value == EHdPicFlag || value == EHdPicUnread)
				{
				TInt id = value == EHdPicFlag ? EMbmMsgFlag : EMbmMsgUnread;
				if (!iIcons || id >= iIcons->Count())
					continue;
				CFont* f = ZoomFont(iZoomFactor, KHdSubject, ETrue);
				TInt h = f ? f->AscentInPixels() : 0;
				if (f) iZoomFactor->ReleaseFont(f);
				pic = new(ELeave) CPmHdIcon(iIcons->At(id), iZoomFactor, h);
				}
			else
				{
				if (value >= iAttNames->Count())
					continue;
				CFbsBitmap* clip = iIcons && EMbmMsgAttach < iIcons->Count() ? iIcons->At(EMbmMsgAttach) : NULL;
				TInt w = iReader->Rect().Width() / 2;
				pic = CPmHdChip::NewL(clip, iZoomFactor, SmallTwips(KHdChip, iZoomFactor->ZoomFactor()),
					(*iAttNames)[value], (*iAttSizes)[value], w);
				}
			CleanupStack::PushL(pic);
			TPictureHeader h;
			h.iPicture = pic;
			h.iPictureType = KUidPmPicture;
			pic->GetOriginalSizeInTwips(h.iSize);
			aText.DeleteL(pos, 1);
			aText.InsertL(pos, h);               // (the text owns it now)
			CleanupStack::Pop();
			continue;
			}
		default:
			{
			// paragraph formats
			CParaFormat* pf = CParaFormat::NewLC();
			TParaFormatMask pm;
			if (kind == EHdTab)
				{
				TTabStop tab;
				tab.iTwipsPosition = iZoomFactor->HorizontalPixelsToTwips(value);
				tab.iType = TTabStop::ERightTab;
				pf->StoreTabL(tab);
				pm.SetAttrib(EAttTabStop);
				}
			else if (kind == EHdRight)
				{
				pf->iHorizontalAlignment = CParaFormat::ERightAlign;
				pm.SetAttrib(EAttAlignment);
				}
			else if (kind == EHdFill)
				{
				pf->iFillColor = KRgbWhite;
				pm.SetAttrib(EAttFillColor);
				}
			else if (kind == EHdRule)
				{
				TParaBorder b;
				b.iLineStyle = TParaBorder::ESolid;
				b.iThickness = 1;
				pf->SetParaBorderL(CParaFormat::EParaBorderBottom, b);
				pf->iSpaceAfterInTwips = 120;
				pf->iBorderMarginInTwips = 40;
				pm.SetAttrib(EAttBottomBorder);
				pm.SetAttrib(EAttSpaceAfter);
				pm.SetAttrib(EAttBorderMargin);
				}
			else
				{
				pf->iSpaceAfterInTwips = value;
				pm.SetAttrib(EAttSpaceAfter);
				}
			aText.ApplyParaFormatL(pf, pm, pos, len);
			CleanupStack::PopAndDestroy();       // pf
			continue;
			}
			}
		aText.ApplyCharFormatL(cf, cm, pos, len);
		}
	}

// a link in the header
void CPmView::HeaderLinkL(TInt aLink)
	{
	if (aLink == KPmLinkHeaderTo)
		{
		iHeaderAllTo = ETrue;
		iHeaderUid = iMsgUid;
		RelayoutReaderL();
		}
	}
