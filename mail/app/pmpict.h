// PMPICT.H - the pictures in a message, shown in the reader's rich text
//
// The engine lists a message's picture parts in <uid>.pic and, when asked
// (PM_CMD_PICTURES), fetches and decodes each to 16 greys in
// <uid>_<part>.pmi (engine/pictures.c). CPmPictures reads those files and
// keeps the bitmaps for the message being read; a CPmPicture is the
// CPicture the rich text holds at each place a picture goes, drawing the
// bitmap, or a small frame with a word while the picture is on its way or
// could not be shown. Word does it this way, so the text flows round the
// pictures, scrolls and lays out with them.

#ifndef __PMPICT_H
#define __PMPICT_H

#include <gdi.h>
#include <fbs.h>
#include <f32file.h>
#include <badesca.h>

class CPmPicture;

// one picture part of the message
struct TPmPicEntry
	{
	enum TState { EUnknown, EWaiting, EReady, EFailed, ETooBig };
	TBuf8<16> iPart;         // "1.2"
	TInt iSize;              // in the message (base64 and all)
	TBuf8<24> iType;
	TBuf8<80> iCid;
	TBuf<80> iName;
	TInt iState;
	TBuf<80> iWhy;           // EFailed: why not
	CFbsBitmap* iBitmap;     // EReady (owned here)
	TInt iSrcW, iSrcH;       // the picture's own size
	TBool iPartial;          // the file was cut short: the rest is grey
	TBool iInline;           // the HTML puts it somewhere (else it goes after the text)
	};

// where a picture sits in the reader's text
struct TPmPicPlace
	{
	TInt iEntry;
	TInt iDocPos;            // the picture character
	CPmPicture* iPicture;    // owned by the rich text; NULL once it has gone
	};

class CPmPictures : public CBase
	{
public:
	static CPmPictures* NewL();
	~CPmPictures();
	// the message's picture list (<uid>.pic); forgets the last message's when the uid differs
	void LoadIndexL(RFs& aFs, const TDesC& aFolderDir, TUint aUid);
	void Reset();
	TInt Count() const { return iEntries->Count(); }
	TPmPicEntry& At(TInt aIndex) { return (*iEntries)[aIndex]; }
	TInt FindCid(const TDesC& aCid) const;              // the part with this Content-ID, -1 if none
	TInt FindPart(const TDesC8& aPart) const;
	// the decoded file, if it is there now: ETrue if the entry's state changed
	TBool LoadReadyL(RFs& aFs, TInt aIndex);
	// places in the text
	void ClearPlaces();
	TPmPicPlace& AddPlaceL(TInt aEntry, TInt aDocPos);
	TInt PlaceCount() const { return iPlaces->Count(); }
	TPmPicPlace& Place(TInt aIndex) { return (*iPlaces)[aIndex]; }
	void PictureGone(CPmPicture* aPicture);            // (a CPmPicture's destructor)
	// bookkeeping for the view
	TUint Uid() const { return iUid; }
	TBool Asked() const { return iAsked; }
	void SetAsked(TBool aAsked) { iAsked = aAsked; }
	TInt LoadedBytes() const { return iLoadedBytes; }
	static void SizeText(TInt aBytes, TDes& aOut);       // "245 KB"
	static void PmiPath(const TDesC& aFolderDir, TUint aUid, const TDesC8& aPart, TDes& aPath);
private:
	CPmPictures() {}
	void ConstructL();
	CArrayFixFlat<TPmPicEntry>* iEntries;
	CArrayFixFlat<TPmPicPlace>* iPlaces;
	TUint iUid;
	TBuf<160> iDir;
	TBool iAsked;            // the engine has been asked for this message's pictures
	TInt iLoadedBytes;
	};

// the CPicture in the rich text: a bitmap, or a frame saying what is up
class CPmPicture : public CPicture
	{
public:
	CPmPicture(CPmPictures& aOwner, const CFont* aFont);
	~CPmPicture();
	// what to show: the bitmap (not owned) drawn at aPixels, or a frame with aText
	void Set(CFbsBitmap* aBitmap, const TSize& aPixels, const TDesC& aText, TBool aFailed, MGraphicsDeviceMap* aMap);
	TSize Pixels() const { return iPixels; }
	CFbsBitmap* Bitmap() const { return iBitmap; }
	// CPicture
	void Draw(CGraphicsContext& aGc, const TPoint& aTopLeft, const TRect& aClipRect, MGraphicsDeviceMap* aMap) const;
	void ExternalizeL(RWriteStream& aStream) const;
	void GetOriginalSizeInTwips(TSize& aSize) const { aSize = iTwips; }
	// a frame's size for a word or two in this font
	static TSize FrameSize(const CFont* aFont, const TDesC& aText, TInt aMaxWidth);
private:
	CPmPictures& iOwner;
	const CFont* iFont;
	CFbsBitmap* iBitmap;
	TSize iPixels;
	TSize iTwips;
	TBuf<90> iText;
	TBool iFailed;
	};

const TUid KUidPmPicture = { 0x01000A7E };

#endif
