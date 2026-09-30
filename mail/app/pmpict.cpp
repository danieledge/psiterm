// pmpict.cpp - the pictures in a message (see pmpict.h)
#include "pmpict.h"
#include <eikenv.h>
#include <s32strm.h>

// the .pmi file's header (engine/pictures.c): "PMI1", w, h, src w, src h
// (16-bit, little-endian), status (0 whole, 1 cut short, 2 not shown:
// a reason follows), 3 spare bytes; then the rows, two pixels a byte as
// EGray16 has them, each row ((w + 7) / 8) * 4 bytes
const TInt KPmiHeader = 16;

CPmPictures* CPmPictures::NewL()
	{
	CPmPictures* p = new(ELeave) CPmPictures;
	CleanupStack::PushL(p);
	p->ConstructL();
	CleanupStack::Pop();
	return p;
	}

void CPmPictures::ConstructL()
	{
	iEntries = new(ELeave) CArrayFixFlat<TPmPicEntry>(4);
	iPlaces = new(ELeave) CArrayFixFlat<TPmPicPlace>(4);
	}

CPmPictures::~CPmPictures()
	{
	Reset();
	delete iEntries;
	delete iPlaces;
	}

void CPmPictures::Reset()
	{
	// pictures still in a rich text must not draw bitmaps that are going
	ClearPlaces();
	if (iEntries)
		{
		for (TInt i = 0; i < iEntries->Count(); i++)
			{
			delete (*iEntries)[i].iBitmap;
			(*iEntries)[i].iBitmap = NULL;
			}
		iEntries->Reset();
		}
	iUid = 0;
	iAsked = EFalse;
	iLoadedBytes = 0;
	}

void CPmPictures::ClearPlaces()
	{
	if (!iPlaces)
		return;
	for (TInt i = 0; i < iPlaces->Count(); i++)
		{
		CPmPicture* p = (*iPlaces)[i].iPicture;
		if (p)
			p->Set(NULL, p->Pixels(), KNullDesC, EFalse, NULL);
		}
	iPlaces->Reset();
	}

void CPmPictures::PictureGone(CPmPicture* aPicture)
	{
	if (!iPlaces)
		return;
	for (TInt i = 0; i < iPlaces->Count(); i++)
		if ((*iPlaces)[i].iPicture == aPicture)
			(*iPlaces)[i].iPicture = NULL;
	}

TPmPicPlace& CPmPictures::AddPlaceL(TInt aEntry, TInt aDocPos)
	{
	TPmPicPlace pl;
	pl.iEntry = aEntry;
	pl.iDocPos = aDocPos;
	pl.iPicture = NULL;
	iPlaces->AppendL(pl);
	return (*iPlaces)[iPlaces->Count() - 1];
	}

// (ER5's Left(n) panics when n is over the length: these clip)
static TPtrC8 Clip8(const TDesC8& aText, TInt aMax)
	{
	return aText.Left(aText.Length() < aMax ? aText.Length() : aMax);
	}

static TPtrC Clip16(const TDesC& aText, TInt aMax)
	{
	return aText.Left(aText.Length() < aMax ? aText.Length() : aMax);
	}

static TPtrC8 Field8(TPtrC8& aLine)
	{
	TInt t = aLine.Locate('\t');
	TPtrC8 f = t >= 0 ? aLine.Left(t) : aLine;
	aLine.Set(t >= 0 ? aLine.Mid(t + 1) : TPtrC8());
	return f;
	}

static TInt ToInt8(const TDesC8& aText)
	{
	TLex8 lex(aText);
	TInt v = 0;
	lex.Val(v);
	return v;
	}

// <uid>.pic: "#PSIMAIL1", then part TAB size TAB type TAB enc TAB cid TAB name
void CPmPictures::LoadIndexL(RFs& aFs, const TDesC& aFolderDir, TUint aUid)
	{
	if (aUid != iUid || aFolderDir.Compare(iDir) != 0)
		{
		Reset();
		iUid = aUid;
		iDir = Clip16(aFolderDir, iDir.MaxLength());
		}
	else
		{
		ClearPlaces();                             // (the same message laid out again)
		if (iEntries->Count())
			return;
		}
	TFileName path(aFolderDir);
	path.AppendNum(aUid);
	path.Append(_L(".pic"));
	RFile f;
	if (f.Open(aFs, path, EFileRead | EFileShareReadersOnly) != KErrNone)
		return;
	CleanupClosePushL(f);
	TInt size = 0;
	f.Size(size);
	if (size > 8192) size = 8192;
	HBufC8* buf = HBufC8::NewLC(size);
	TPtr8 p = buf->Des();
	User::LeaveIfError(f.Read(p));
	TPtrC8 rest = *buf;
	while (rest.Length() && iEntries->Count() < 24)
		{
		TInt nl = rest.Locate('\n');
		TPtrC8 line = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC8());
		if (line.Length() && line[line.Length() - 1] == '\r')
			line.Set(line.Left(line.Length() - 1));
		if (!line.Length() || line[0] == '#')
			continue;
		TPmPicEntry e;
		TPtrC8 l = line;
		e.iPart = Clip8(Field8(l), e.iPart.MaxLength());
		e.iSize = ToInt8(Field8(l));
		e.iType = Clip8(Field8(l), e.iType.MaxLength());
		Field8(l);                                 // the encoding: the engine's business
		e.iCid = Clip8(Field8(l), e.iCid.MaxLength());
		TPtrC8 name = Field8(l);
		e.iName.Copy(Clip8(name, e.iName.MaxLength()));
		e.iState = TPmPicEntry::EUnknown;
		e.iBitmap = NULL;
		e.iSrcW = e.iSrcH = 0;
		e.iPartial = EFalse;
		e.iInline = EFalse;
		e.iWhy.Zero();
		if (!e.iPart.Length())
			continue;
		iEntries->AppendL(e);
		}
	CleanupStack::PopAndDestroy(2);                // buf, f
	}

TInt CPmPictures::FindCid(const TDesC& aCid) const
	{
	TPtrC cid = aCid;
	if (cid.Length() && cid[0] == '<') cid.Set(cid.Mid(1));
	if (cid.Length() && cid[cid.Length() - 1] == '>') cid.Set(cid.Left(cid.Length() - 1));
	if (!cid.Length())
		return -1;
	TBuf8<80> c8;
	c8.Copy(Clip16(cid, 80));
	for (TInt i = 0; i < iEntries->Count(); i++)
		if ((*iEntries)[i].iCid.Length() && (*iEntries)[i].iCid.CompareF(c8) == 0)
			return i;
	return -1;
	}

TInt CPmPictures::FindPart(const TDesC8& aPart) const
	{
	for (TInt i = 0; i < iEntries->Count(); i++)
		if ((*iEntries)[i].iPart == aPart)
			return i;
	return -1;
	}

void CPmPictures::PmiPath(const TDesC& aFolderDir, TUint aUid, const TDesC8& aPart, TDes& aPath)
	{
	aPath = aFolderDir;
	aPath.AppendNum(aUid);
	aPath.Append('_');
	for (TInt i = 0; i < aPart.Length(); i++)
		aPath.Append(aPart[i] == '.' ? TChar('_') : TChar(aPart[i]));
	aPath.Append(_L(".pmi"));
	}

void CPmPictures::SizeText(TInt aBytes, TDes& aOut)
	{
	// base64 is 4/3 of the file
	TInt kb = (aBytes * 3 / 4 + 1023) / 1024;
	if (kb >= 1024) aOut.Format(_L("%d.%d MB"), kb / 1024, (kb % 1024) * 10 / 1024);
	else aOut.Format(_L("%d KB"), kb);
	}

static TUint16 Le16(const TUint8* p)
	{
	return (TUint16)(p[0] | (p[1] << 8));
	}

// the decoded picture, if the engine has written it: into a 16-grey bitmap
TBool CPmPictures::LoadReadyL(RFs& aFs, TInt aIndex)
	{
	TPmPicEntry& e = (*iEntries)[aIndex];
	if (e.iState == TPmPicEntry::EReady || e.iState == TPmPicEntry::EFailed)
		return EFalse;
	TFileName path;
	PmiPath(iDir, iUid, e.iPart, path);
	RFile f;
	if (f.Open(aFs, path, EFileRead | EFileShareReadersOnly) != KErrNone)
		return EFalse;
	CleanupClosePushL(f);
	TBuf8<KPmiHeader> h;
	User::LeaveIfError(f.Read(h));
	if (h.Length() < KPmiHeader || h.Left(4) != _L8("PMI1"))
		{
		CleanupStack::PopAndDestroy();             // f
		e.iState = TPmPicEntry::EFailed;
		e.iWhy = _L("the picture file is damaged");
		return ETrue;
		}
	const TUint8* hp = h.Ptr();
	TInt w = Le16(hp + 4), hh = Le16(hp + 6);
	e.iSrcW = Le16(hp + 8);
	e.iSrcH = Le16(hp + 10);
	TInt status = hp[12];
	if (status == 2)
		{
		TBuf8<80> why;
		f.Read(why);
		TInt z = why.Locate(0);
		if (z >= 0) why.SetLength(z);
		e.iWhy.Copy(why);
		e.iState = TPmPicEntry::EFailed;
		CleanupStack::PopAndDestroy();             // f
		return ETrue;
		}
	if (w <= 0 || hh <= 0 || w > 2048 || hh > 2048)
		{
		e.iState = TPmPicEntry::EFailed;
		e.iWhy = _L("the picture file is damaged");
		CleanupStack::PopAndDestroy();
		return ETrue;
		}
	CFbsBitmap* bmp = new(ELeave) CFbsBitmap;
	CleanupStack::PushL(bmp);
	User::LeaveIfError(bmp->Create(TSize(w, hh), EGray16));
	TInt stride = CFbsBitmap::ScanLineLength(w, EGray16);
	TInt fileStride = ((w + 7) / 8) * 4;
	if (stride == fileStride)
		{
		// the rows are laid out as the bitmap wants them: straight in
		TPtr8 all((TUint8*)bmp->DataAddress(), stride * hh);
		User::LeaveIfError(f.Read(all));
		if (all.Length() != stride * hh)
			User::Leave(KErrCorrupt);
		}
	else
		{
		HBufC8* row = HBufC8::NewLC(fileStride);
		TPtr8 rp = row->Des();
		for (TInt y = 0; y < hh; y++)
			{
			User::LeaveIfError(f.Read(rp, fileStride));
			if (rp.Length() != fileStride)
				User::Leave(KErrCorrupt);
			bmp->SetScanLine(rp, y);
			}
		CleanupStack::PopAndDestroy();             // row
		}
	CleanupStack::Pop();                           // bmp
	CleanupStack::PopAndDestroy();                 // f
	delete e.iBitmap;
	e.iBitmap = bmp;
	e.iPartial = status == 1;
	e.iState = TPmPicEntry::EReady;
	iLoadedBytes += stride * hh;
	return ETrue;
	}

// ----- CPmPicture ----------------------------------------------------------------

CPmPicture::CPmPicture(CPmPictures& aOwner, const CFont* aFont)
	: iOwner(aOwner), iFont(aFont)
	{
	}

CPmPicture::~CPmPicture()
	{
	iOwner.PictureGone(this);
	}

void CPmPicture::ExternalizeL(RWriteStream& /*aStream*/) const
	{
	// never stored: the reader is read-only
	}

TSize CPmPicture::FrameSize(const CFont* aFont, const TDesC& aText, TInt aMaxWidth)
	{
	TInt w = (aFont ? aFont->TextWidthInPixels(aText) : 8 * aText.Length()) + 30;
	TInt h = (aFont ? aFont->HeightInPixels() : 12) + 10;
	if (w < 60) w = 60;
	if (w > aMaxWidth && aMaxWidth > 20) w = aMaxWidth;
	return TSize(w, h);
	}

void CPmPicture::Set(CFbsBitmap* aBitmap, const TSize& aPixels, const TDesC& aText, TBool aFailed, MGraphicsDeviceMap* aMap)
	{
	iBitmap = aBitmap;
	iPixels = aPixels;
	iText = Clip16(aText, iText.MaxLength());
	iFailed = aFailed;
	if (aMap)
		iTwips = TSize(aMap->HorizontalPixelsToTwips(aPixels.iWidth), aMap->VerticalPixelsToTwips(aPixels.iHeight));
	}

// a small picture glyph: a frame with a hill and a sun
static void DrawGlyph(CGraphicsContext& aGc, const TPoint& aAt)
	{
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(KRgbBlack);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.DrawRect(TRect(aAt, TSize(14, 12)));
	aGc.DrawLine(TPoint(aAt.iX + 2, aAt.iY + 9), TPoint(aAt.iX + 6, aAt.iY + 4));
	aGc.DrawLine(TPoint(aAt.iX + 6, aAt.iY + 4), TPoint(aAt.iX + 9, aAt.iY + 8));
	aGc.DrawLine(TPoint(aAt.iX + 9, aAt.iY + 8), TPoint(aAt.iX + 12, aAt.iY + 5));
	aGc.DrawLine(TPoint(aAt.iX + 12, aAt.iY + 5), TPoint(aAt.iX + 12, aAt.iY + 10));
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(KRgbBlack);
	aGc.DrawRect(TRect(aAt.iX + 3, aAt.iY + 2, aAt.iX + 5, aAt.iY + 4));
	}

void CPmPicture::Draw(CGraphicsContext& aGc, const TPoint& aTopLeft, const TRect& aClipRect, MGraphicsDeviceMap* /*aMap*/) const
	{
	aGc.SetClippingRect(aClipRect);
	if (iBitmap)
		{
		// (a CGraphicsContext has no BitBlt; DrawBitmap to a rectangle the
		// bitmap's own size copies it as it is)
		aGc.DrawBitmap(TRect(aTopLeft, iPixels), iBitmap);
		aGc.CancelClippingRect();
		return;
		}
	// a frame with the word: light grey face, as EIKON's dimmed things are
	TRect r(aTopLeft, iPixels);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(TRgb(85, 85, 85));
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(TRgb(238, 238, 238));
	aGc.DrawRect(r);
	DrawGlyph(aGc, TPoint(r.iTl.iX + 6, r.iTl.iY + (r.Height() - 12) / 2));
	if (iFont && iText.Length())
		{
		aGc.UseFont(iFont);
		aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
		aGc.SetPenColor(iFailed ? TRgb(85, 85, 85) : KRgbBlack);
		TRect tr(r.iTl.iX + 25, r.iTl.iY, r.iBr.iX - 4, r.iBr.iY);
		TInt base = (tr.Height() - iFont->HeightInPixels()) / 2 + iFont->AscentInPixels();
		aGc.DrawText(iText, tr, base, CGraphicsContext::ELeft, 0);
		aGc.DiscardFont();
		}
	aGc.CancelClippingRect();
	}
