// PMWEBPIC.CPP - pictures from the web in a message, when asked for (0.75)
//
// PsiMail fetches nothing from the web unasked: a newsletter's pictures
// would tell the sender when (and roughly where) it was read. A message
// whose HTML points at pictures on the web gets a line at the top of its
// text, as the first-party mail programs have it:
//
//     Pictures from the web are not shown - Show them
//
// "Show them" (Tab to it and Enter, or a tap) or Message > Web > Show web
// pictures has the engine fetch and set them out (PM_CMD_WEBPICS,
// engine/webpics.c), into <uid>_W<hash>.pmi files the reader then shows in
// place, at the size the HTML gives them where it does. Tools > Preferences
// > Web pictures: Ask (the standard), Always (fetched when the message is
// opened) or Never (no line). Nothing is fetched while working offline. On
// the modem each web site is a call of its own, so with pictures on more
// than one site PsiMail says how many calls it will take first.
//
// The setting is TPmSettings::iSpare[0] bits 8-9 (TPmSettings keeps its size).

#include "pmapp.h"
#include "pmpict.h"

const TInt KPmWebShift = 8;
const TInt KPmWebMask = 0x300;

// ----- the picture entries ------------------------------------------------------

// FNV-1a of the address's bytes, as engine/webpics.c names the files
TUint32 CPmPictures::WebHash(const TDesC& aSrc)
	{
	TUint32 h = 2166136261u;
	for (TInt i = 0; i < aSrc.Length(); i++)
		{
		h ^= (TUint8)aSrc[i];
		h *= 16777619u;
		}
	return h;
	}

TInt CPmPictures::AddWebL(const TDesC& aSrc, TInt aHintW, TInt aHintH)
	{
	TBuf8<16> part;
	part.Format(_L8("W%08X"), WebHash(aSrc));
	TInt e = FindPart(part);
	if (e >= 0)
		return e;
	if (iEntries->Count() >= 64)
		return -1;
	TPmPicEntry n;
	n.iPart = part;
	n.iSize = 0;
	n.iType = _L8("web");
	n.iCid.Zero();
	n.iName.Zero();
	n.iState = TPmPicEntry::EUnknown;
	n.iWhy.Zero();
	n.iBitmap = NULL;
	n.iSrcW = n.iSrcH = 0;
	n.iPartial = EFalse;
	n.iInline = ETrue;
	n.iWeb = ETrue;
	n.iHintW = aHintW;
	n.iHintH = aHintH;
	iEntries->AppendL(n);
	return iEntries->Count() - 1;
	}

// ----- the setting -------------------------------------------------------------------

TInt CPmView::WebPicturesPref() const
	{
	TInt p = (iSettings->iSpare[0] & KPmWebMask) >> KPmWebShift;
	return p > 2 ? 0 : p;
	}

TBool CPmView::WebPicturesOn() const
	{
	if (PicturesPref() != 0 || WebPicturesPref() == 2)
		return EFalse;
	return WebPicturesPref() == 1 || (iWebUid && iWebUid == iMsgUid);
	}

// "600x400" (either may be 0): ETrue if there was one
TInt CPmView::ParsePictureSize(const TDesC& aText, TInt& aW, TInt& aH)
	{
	aW = aH = 0;
	TLex lex(aText);
	if (lex.Val(aW) != KErrNone)
		return EFalse;
	if (lex.Get() != 'x')
		return EFalse;
	lex.Val(aH);
	if (aW < 0 || aW > 4000) aW = 0;
	if (aH < 0 || aH > 4000) aH = 0;
	return ETrue;
	}

// ----- what the message has --------------------------------------------------------

static TBool IsWebSrc(const TDesC& aSrc)
	{
	return (aSrc.Length() > 7 && aSrc.Left(7).CompareF(_L("http://")) == 0) ||
		(aSrc.Length() > 8 && aSrc.Left(8).CompareF(_L("https://")) == 0);
	}

// a spacer or tracking pixel by its address or size (engine/webpics.c's list, in short)
static TBool LooksLikeSpacer(const TDesC& aSrc, TInt aW, TInt aH)
	{
	if ((aW > 0 && aW < 4) || (aH > 0 && aH < 4))
		return ETrue;
	static const TText* const KNames[] = { _S("spacer"), _S("pixel.gif"), _S("pixel.png"), _S("/open.aspx"), _S("/open?"),
		_S("/o.gif"), _S("/track/open"), _S("/wf/open"), _S("beacon"), _S("/e2t/o/"), 0 };
	for (TInt i = 0; KNames[i]; i++)
		if (aSrc.FindF(TPtrC(KNames[i])) >= 0)
			return ETrue;
	return EFalse;
	}

// counts the message's web pictures (and the sites they are on) from its text
void CPmView::CountWebPicturesL()
	{
	if (iWebCountUid == iMsgUid && iText)
		return;
	iWebCountUid = iMsgUid;
	iWebCount = iWebHosts = 0;
	if (!iText)
		return;                              // (picture lines come only from HTML: no need to look for the .htm)
	CDesCArrayFlat* hosts = new(ELeave) CDesCArrayFlat(4);
	CleanupStack::PushL(hosts);
	TPtrC rest = iText->Mid(iBodyOff);
	while (rest.Length())
		{
		TInt nl = rest.Locate('\n');
		TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
		if (line.Length() < 3 || line[0] != 0x01 || line[1] != 'i')
			continue;
		TInt sep = line.Locate(0x02);
		if (sep < 0)
			continue;
		TPtrC src = line.Mid(sep + 1);
		TInt w = 0, h = 0;
		TInt sep2 = src.Locate(0x02);
		if (sep2 >= 0)
			{
			ParsePictureSize(src.Mid(sep2 + 1), w, h);
			src.Set(src.Left(sep2));
			}
		if (src.Length() && src[src.Length() - 1] == '\r')
			src.Set(src.Left(src.Length() - 1));
		if (!IsWebSrc(src) || LooksLikeSpacer(src, w, h))
			continue;
		iWebCount++;
		// the site: between "//" and the next '/'
		TInt s = src.Find(_L("//"));
		TPtrC host = s >= 0 ? src.Mid(s + 2) : src;
		TInt slash = host.Locate('/');
		if (slash >= 0)
			host.Set(host.Left(slash));
		TInt at;
		if (host.Length() && hosts->Find(host, at) != 0)
			hosts->AppendL(host.Left(host.Length() < 80 ? host.Length() : 80));
		}
	iWebHosts = hosts->Count();
	CleanupStack::PopAndDestroy();           // hosts
	}

TBool CPmView::CanShowWebPictures() const
	{
	return iMode == EMessage && iText && iWebCount > 0 && PicturesPref() == 0 && !WebPicturesOn();
	}

// ----- asking for them ----------------------------------------------------------------

void CPmView::ShowWebPicturesL()
	{
	if (iMode != EMessage || !iText)
		return;
	CountWebPicturesL();
	if (!iWebCount)
		{
		Toast(_L("No pictures from the web in this message"));
		return;
		}
	if (PicturesPref() != 0)
		{
		Toast(_L("Pictures are off - Tools > Preferences"));
		return;
		}
	if (iSettings->iOffline)
		{
		Toast(_L("Not downloaded - you are working offline"));
		return;
		}
	// on the modem every web site is a call of its own: say so first
	if (iSettings->iNetMode == 0 && iWebHosts > 1)
		{
		TBuf<120> what;
		what.Format(_L("%d pictures on %d web sites: %d calls on the modem"), iWebCount, iWebHosts, iWebHosts);
		if (!iEikonEnv->QueryWinL(_L("Get the web pictures?"), what))
			return;
		}
	iWebUid = iMsgUid;
	Cmd(PM_CMD_WEBPICS, iFolder, iMsgUid, KNullDesC8);
	// laid out again: the line goes, the pictures' frames say "Getting the
	// picture..." until they come (UpdateReaderL, while the command is in flight)
	RelayoutReaderL();
	}

// Web pictures: Always - asked for once a message, when it is shown
void CPmView::AskForWebPicturesL()
	{
	if (WebPicturesPref() != 1 || PicturesPref() != 0 || iSettings->iOffline || !iRunning)
		return;
	if (iWebUid == iMsgUid || !iWebCount)
		return;
	iWebUid = iMsgUid;
	Cmd(PM_CMD_WEBPICS, iFolder, iMsgUid, KNullDesC8);
	for (TInt i = 0; i < iPictures->Count(); i++)
		{
		TPmPicEntry& e = iPictures->At(i);
		if (e.iWeb && e.iState == TPmPicEntry::EUnknown)
			e.iState = TPmPicEntry::EWaiting;
		}
	}

// The reader's line at the top of a message with web pictures not shown:
// appended to aText (with its paragraph end); aLinkPos/aLinkLen is "Show
// them". aLinePos < 0: no line.
void CPmView::WebPicturesLineL(TDes& aText, CArrayFixFlat<TPmLinkRange>& aLinks, TInt& aLinePos, TInt& aLinkLen)
	{
	aLinePos = -1;
	aLinkLen = 0;
	CountWebPicturesL();
	if (!iWebCount || PicturesPref() != 0 || WebPicturesPref() == 2 || aText.Length() > aText.MaxLength() - 100)
		return;
	aLinePos = aText.Length();
	if (WebPicturesOn())
		{
		// (asked for: no line once they are here; while they come, the frames say so)
		aLinePos = -1;
		return;
		}
	aText.Append(_L("Pictures from the web are not shown - "));
	TPmLinkRange lr;
	lr.iPos = aText.Length();
	aText.Append(_L("Show them"));
	lr.iLen = aText.Length() - lr.iPos;
	lr.iLink = KPmLinkWebPictures;
	aLinks.AppendL(lr);
	aLinkLen = lr.iLen;
	aText.Append(TChar(CEditableText::EParagraphDelimiter));
	}
