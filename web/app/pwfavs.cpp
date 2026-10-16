// pwfavs.cpp - Favourites and History for PsiWeb (0.69)
//
// Favourites.dat and History.dat live beside PsiWeb.ini in
// C:\System\Apps\PsiWeb\: "PW", a version byte, a count byte, then each
// entry's title and address, each with a 2-byte length. They are kept apart
// from TPwSettings, which is saved whole and must not change size.
//
// Menus: Favourites (between Go and Tools) lists the first five favourites;
// Go > History lists the five latest pages. Both are built in
// DynInitMenuPaneL from the lists. A page is logged when it has loaded
// (CPwView::Tick sees the engine's busy flag fall).
#include "pwapp.h"
#include <eikedwin.h>
#include <eikchlst.h>
#include <eikclb.h>
#include <eikclbd.h>
#include <eiklbv.h>
#include <eiksbfrm.h>
#include <eikmenup.h>

_LIT(KFavsFile, "C:\\System\\Apps\\PsiWeb\\Favourites.dat");
_LIT(KHistFile, "C:\\System\\Apps\\PsiWeb\\History.dat");
const TInt KPwFileVer = 1;
const TInt KPwHistWatch = 6 * 64;      // ticks (64 a second) to wait for a page's title

// cuts aText to aWidth pixels of aFont, ending in the ellipsis character
static void ClipPx(TDes& aText, const CFont& aFont, TInt aWidth)
	{
	if (aFont.TextWidthInPixels(aText) <= aWidth)
		return;
	const TText KEllipsis = 0x85;
	TInt n = aText.Length();
	while (n > 0)
		{
		n--;
		if (aFont.TextWidthInPixels(aText.Left(n)) + aFont.CharWidthInPixels(KEllipsis) <= aWidth)
			break;
		}
	aText.SetLength(n);
	aText.Append(KEllipsis);
	}

// a page worth keeping: not empty, not one of PsiWeb's own about: pages
static TBool RealUrl(const TDesC& aUrl)
	{
	return aUrl.Length() > 0 && aUrl.Left(6).CompareF(_L("about:")) != 0;
	}

// the name an entry is shown by: its title, or its address when it has none
static void ShownName(const TPwEntry& aEntry, TDes& aOut)
	{
	if (aEntry.iTitle.Length())
		aOut = aEntry.iTitle;
	else
		aOut = Clip(aEntry.iUrl, aOut.MaxLength());
	for (TInt i = 0; i < aOut.Length(); i++)
		if (aOut[i] < 0x20)
			aOut[i] = ' ';           // (a tab would start a list's next column)
	}

// ===========================================================================
// CPwList
// ===========================================================================

CPwList* CPwList::NewL(RFs& aFs, const TDesC& aFile, TInt aMax)
	{
	CPwList* l = new(ELeave) CPwList(aFs, aMax);
	CleanupStack::PushL(l);
	l->ConstructL(aFile);
	CleanupStack::Pop();
	return l;
	}

void CPwList::ConstructL(const TDesC& aFile)
	{
	iFile = aFile;
	iItems = new(ELeave) TPwEntry[iMax];
	Load();
	}

CPwList::~CPwList()
	{
	delete[] iItems;
	}

static void PutStr(TDes8& aOut, const TDesC& aText)
	{
	aOut.Append((TUint8)(aText.Length() & 0xff));
	aOut.Append((TUint8)(aText.Length() >> 8));
	aOut.Append(aText);              // (narrow text: a straight copy)
	}

static TBool GetStr(const TDesC8& aIn, TInt& aPos, TDes& aText)
	{
	if (aPos + 2 > aIn.Length())
		return EFalse;
	TInt len = aIn[aPos] | (aIn[aPos + 1] << 8);
	aPos += 2;
	if (len > aText.MaxLength() || aPos + len > aIn.Length())
		return EFalse;
	aText.Copy(aIn.Mid(aPos, len));
	aPos += len;
	return ETrue;
	}

// a missing, old or damaged file is an empty list
void CPwList::Load()
	{
	iCount = 0;
	RFile file;
	if (file.Open(iFs, iFile, EFileRead) != KErrNone)
		return;
	TInt size = 0;
	if (file.Size(size) != KErrNone || size < 4 || size > 4 + iMax * (4 + KPwTitleMax + PW_URL_MAX))
		{
		file.Close();
		return;
		}
	HBufC8* buf = HBufC8::New(size);
	if (!buf)
		{
		file.Close();
		return;
		}
	TPtr8 data(buf->Des());
	TInt r = file.Read(data);
	file.Close();
	if (r == KErrNone && data.Length() >= 4 && data[0] == 'P' && data[1] == 'W' && data[2] == KPwFileVer)
		{
		TInt count = data[3];
		TInt pos = 4;
		TInt n = 0;
		for (; n < count && n < iMax; n++)
			if (!GetStr(data, pos, iItems[n].iTitle) || !GetStr(data, pos, iItems[n].iUrl))
				break;
		if (n < count && n < iMax)
			n = 0;                       // damaged: start again
		iCount = n;
		}
	delete buf;
	}

// to a temporary file, then renamed over the old one (as the settings)
void CPwList::Save()
	{
	iFs.MkDirAll(iFile);
	HBufC8* buf = HBufC8::New(4 + iCount * (4 + KPwTitleMax + PW_URL_MAX));
	if (!buf)
		return;
	TPtr8 d(buf->Des());
	d.Append('P');
	d.Append('W');
	d.Append((TUint8)KPwFileVer);
	d.Append((TUint8)iCount);
	for (TInt i = 0; i < iCount; i++)
		{
		PutStr(d, iItems[i].iTitle);
		PutStr(d, iItems[i].iUrl);
		}
	TFileName tmpName(iFile);
	tmpName.Append('~');
	RFile file;
	TInt r = file.Replace(iFs, tmpName, EFileWrite);
	if (r == KErrNone)
		{
		r = file.Write(d);
		if (r == KErrNone)
			r = file.Flush();
		file.Close();
		if (r == KErrNone)
			r = iFs.Replace(tmpName, iFile);
		if (r != KErrNone)
			iFs.Delete(tmpName);
		}
	delete buf;
	}

TInt CPwList::Find(const TDesC& aUrl) const
	{
	for (TInt i = 0; i < iCount; i++)
		if (iItems[i].iUrl.Compare(aUrl) == 0)
			return i;
	return KErrNotFound;
	}

// History: the newest first; a page already there moves to the front
void CPwList::AddFront(const TDesC& aTitle, const TDesC& aUrl)
	{
	TBuf<KPwTitleMax> title(Clip(aTitle, KPwTitleMax));
	TInt at = Find(aUrl);
	if (at == 0 && (title.Length() == 0 || iItems[0].iTitle == title))
		return;                                  // already the newest, nothing new
	if (at >= 0 && title.Length() == 0)
		title = iItems[at].iTitle;               // (keep the name it had)
	if (at < 0)
		at = iCount < iMax ? iCount++ : iCount - 1;   // (the oldest goes when full)
	for (TInt i = at; i > 0; i--)
		iItems[i] = iItems[i - 1];
	iItems[0].iTitle = title;
	iItems[0].iUrl = Clip(aUrl, PW_URL_MAX);
	Save();
	}

TInt CPwList::AddEnd(const TDesC& aTitle, const TDesC& aUrl)
	{
	if (Find(aUrl) >= 0)
		return KErrAlreadyExists;
	if (iCount >= iMax)
		return KErrOverflow;
	iItems[iCount].iTitle = Clip(aTitle, KPwTitleMax);
	iItems[iCount].iUrl = Clip(aUrl, PW_URL_MAX);
	iCount++;
	Save();
	return KErrNone;
	}

void CPwList::Set(TInt aIndex, const TDesC& aTitle, const TDesC& aUrl)
	{
	if (aIndex < 0 || aIndex >= iCount)
		return;
	iItems[aIndex].iTitle = Clip(aTitle, KPwTitleMax);
	iItems[aIndex].iUrl = Clip(aUrl, PW_URL_MAX);
	Save();
	}

void CPwList::SetTitleOnly(TInt aIndex, const TDesC& aTitle)
	{
	if (aIndex < 0 || aIndex >= iCount)
		return;
	iItems[aIndex].iTitle = Clip(aTitle, KPwTitleMax);
	Save();
	}

void CPwList::Delete(TInt aIndex)
	{
	if (aIndex < 0 || aIndex >= iCount)
		return;
	for (TInt i = aIndex; i < iCount - 1; i++)
		iItems[i] = iItems[i + 1];
	iCount--;
	Save();
	}

void CPwList::Clear()
	{
	iCount = 0;
	Save();
	}

// ===========================================================================
// Dialogs
// ===========================================================================

void CPwFavDialog::PreLayoutDynInitL()
	{
	SetEdwinTextL(EPwDlgFavName, &iName);
	SetEdwinTextL(EPwDlgFavUrl, &iUrl);
	}

TBool CPwFavDialog::OkToExitL(TInt /*aButtonId*/)
	{
	((CEikEdwin*)Control(EPwDlgFavName))->GetText(iName);
	iName.Trim();
	((CEikEdwin*)Control(EPwDlgFavUrl))->GetText(iUrl);
	iUrl.Trim();
	if (iName.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No name entered"));
		TryChangeFocusToL(EPwDlgFavName);
		return EFalse;
		}
	if (iUrl.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No address entered"));
		TryChangeFocusToL(EPwDlgFavUrl);
		return EFalse;
		}
	return ETrue;
	}

// Favourites or History as a list of Name / Address
void CPwListDialog::PreLayoutDynInitL()
	{
	CEikColumnListBox* list = (CEikColumnListBox*)Control(EPwDlgOrgList);
	list->Model()->SetItemTextArray(new(ELeave) CDesCArrayFlat(16));
	list->Model()->SetOwnershipType(ELbmOwnsItemArray);
	CColumnListBoxData* cd = list->Model()->ColumnData();
	cd->SetColumnWidthPixelL(0, 200);
	cd->SetColumnWidthPixelL(1, 250);
	// (the rows' height comes from the columns' fonts)
	cd->SetColumnFontL(0, iEikonEnv->NormalFont());
	cd->SetColumnFontL(1, iEikonEnv->NormalFont());
	if (!list->ScrollBarFrame())
		list->CreateScrollBarFrameL();
	list->ScrollBarFrame()->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);
	list->SetListBoxObserver(this);
	FillL();
	}

void CPwListDialog::PostLayoutDynInitL()
	{
	// the first row only now: before the layout the list has no height, and
	// a current row set then panics EIKON-LISTBOX 4 (as in PsiTerm's browser)
	iReady = ETrue;
	CEikColumnListBox* list = (CEikColumnListBox*)Control(EPwDlgOrgList);
	if (list->Model()->NumberOfItems())
		list->SetCurrentItemIndex(0);
	TryChangeFocusToL(EPwDlgOrgList);
	}

// (re)makes the rows from the list; the highlight stays on the same place
void CPwListDialog::FillL()
	{
	CEikColumnListBox* list = (CEikColumnListBox*)Control(EPwDlgOrgList);
	TInt cur = list->CurrentItemIndex();
	CDesCArray* rows = (CDesCArray*)list->Model()->ItemTextArray();
	rows->Reset();
	const CFont* font = iEikonEnv->NormalFont();
	TBuf<KPwTitleMax> name;
	TBuf<PW_URL_MAX> url;
	for (TInt i = 0; i < iList.Count(); i++)
		{
		const TPwEntry& e = iList.At(i);
		ShownName(e, name);
		ClipPx(name, *font, 190);
		url = e.iUrl;
		for (TInt k = 0; k < url.Length(); k++)
			if (url[k] < 0x20)
				url[k] = ' ';
		ClipPx(url, *font, 240);
		TBuf<KPwTitleMax + 4 + 300> row(name);   // (the URL is cut to 240 px: far fewer than 300 characters)
		row.Append('\t');
		row.Append(url.Left(url.Length() < 300 ? url.Length() : 300));
		rows->AppendL(row);
		}
	list->Reset();
	list->HandleItemAdditionL();
	if (iReady && rows->Count())
		list->SetCurrentItemIndex(cur >= 0 && cur < rows->Count() ? cur : rows->Count() - 1);
	}

TBool CPwListDialog::OkToExitL(TInt aButtonId)
	{
	CEikColumnListBox* list = (CEikColumnListBox*)Control(EPwDlgOrgList);
	TInt cur = list->Model()->NumberOfItems() ? list->CurrentItemIndex() : -1;
	if (aButtonId == EPwBidEdit)
		{
		if (cur < 0 || cur >= iList.Count())
			{
			iEikonEnv->InfoMsg(_L("Nothing to edit"));
			return EFalse;
			}
		TBuf<KPwTitleMax> name(iList.At(cur).iTitle);
		TBuf<PW_URL_MAX> url(iList.At(cur).iUrl);
		CPwFavDialog* dlg = new(ELeave) CPwFavDialog(name, url);
		if (dlg->ExecuteLD(R_PW_FAV_DIALOG))
			{
			TInt other = iList.Find(url);
			if (other >= 0 && other != cur)
				iEikonEnv->InfoMsg(_L("Already in favourites"));
			else
				{
				iList.Set(cur, name, url);
				FillL();
				}
			}
		return EFalse;                       // (the list stays up)
		}
	if (aButtonId == EPwBidDelete)
		{
		if (cur < 0 || cur >= iList.Count())
			{
			iEikonEnv->InfoMsg(_L("Nothing to delete"));
			return EFalse;
			}
		iList.Delete(cur);
		FillL();
		iEikonEnv->InfoMsg(_L("Deleted"));
		return EFalse;
		}
	// Open (or Close, when organising)
	iPick = (!iOrganise && cur >= 0 && cur < iList.Count()) ? cur : -1;
	if (!iOrganise && iPick < 0)
		{
		iEikonEnv->InfoMsg(_L("Nothing to open"));
		return EFalse;
		}
	return ETrue;
	}

// a second tap on the highlighted row opens it (or edits it, when organising)
void CPwListDialog::HandleListBoxEventL(CEikListBox* /*aListBox*/, TListBoxEvent aEventType)
	{
	if (aEventType == EEventItemDoubleClicked)
		TryExitL(iOrganise ? EPwBidEdit : EEikBidOk);
	}

// ===========================================================================
// Open address: the Recent line
// ===========================================================================

void CPwOpenDialog::PreLayoutDynInitL()
	{
	CEikEdwin* ed = (CEikEdwin*)Control(EPwDlgUrl);
	ed->SetTextL(&iUrl);
	ed->SelectAllL();
	// Recent: "-" (nothing chosen), then the latest pages' addresses
	CDesCArrayFlat* items = new(ELeave) CDesCArrayFlat(KPwRecent + 1);
	CleanupStack::PushL(items);
	items->AppendL(_L("-"));
	const CFont* font = iEikonEnv->NormalFont();
	for (TInt i = 0; i < iHistory.Count() && i < KPwRecent; i++)
		{
		TBuf<PW_URL_MAX> u(iHistory.At(i).iUrl);
		for (TInt k = 0; k < u.Length(); k++)
			if (u[k] < 0x20)
				u[k] = ' ';
		ClipPx(u, *font, 330);       // (the line must fit beside its prompt)
		items->AppendL(u.Left(u.Length() < 120 ? u.Length() : 120));
		}
	CleanupStack::Pop();
	CEikChoiceList* cl = (CEikChoiceList*)Control(EPwDlgRecent);
	cl->SetArrayL(items);                    // it owns the array now
	cl->SetCurrentItem(0);
	if (iHistory.Count() == 0)
		SetLineDimmedNow(EPwDlgRecent, ETrue);
	}

// choosing a recent address puts it in the Address line
void CPwOpenDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId != EPwDlgRecent)
		return;
	TInt n = ((CEikChoiceList*)Control(EPwDlgRecent))->CurrentItem() - 1;
	if (n >= 0 && n < iHistory.Count() && n < KPwRecent)
		{
		TBuf<PW_URL_MAX> u(iHistory.At(n).iUrl);
		CEikEdwin* ed = (CEikEdwin*)Control(EPwDlgUrl);
		ed->SetTextL(&u);
		ed->SelectAllL();
		ed->DrawNow();
		}
	}

TBool CPwOpenDialog::OkToExitL(TInt /*aButtonId*/)
	{
	((CEikEdwin*)Control(EPwDlgUrl))->GetText(iUrl);
	iUrl.Trim();
	if (iUrl.Length() == 0)
		{
		// nothing typed: the Recent page chosen, if any
		TInt n = ((CEikChoiceList*)Control(EPwDlgRecent))->CurrentItem() - 1;
		if (n >= 0 && n < iHistory.Count() && n < KPwRecent)
			iUrl = iHistory.At(n).iUrl;
		}
	if (iUrl.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No address entered"));
		TryChangeFocusToL(EPwDlgUrl);
		return EFalse;
		}
	return ETrue;
	}

// ===========================================================================
// The app's side: logging, menus, commands
// ===========================================================================

TBool CPwAppUi::HavePage() const
	{
	PwShared* s = iView->Shared();
	if (!s || !iView->EngineRunning())
		return EFalse;
	TBuf<PW_URL_MAX> url;
	FromUtf8(url, s->url);
	url.Trim();
	return RealUrl(url);
	}

// the engine has finished loading a page: into History (CPwView::Tick)
void CPwAppUi::PageLoaded()
	{
	PwShared* s = iView ? iView->Shared() : NULL;
	iHistUntil = 0;
	if (!s || !iHist)
		return;
	TBuf<PW_URL_MAX> url;
	FromUtf8(url, s->url);
	url.Trim();
	if (!RealUrl(url))
		return;
	TBuf<KPwTitleMax> title;
	FromUtf8(title, s->title);
	title.Trim();
	iHist->AddFront(title, url);
	iHistUntil = User::TickCount() + KPwHistWatch;
	}

// ...the title can reach the shared chunk a moment after the page: for a
// few seconds, a changed title is put right
void CPwAppUi::PageTick()
	{
	if (!iHistUntil || !iHist)
		return;
	if ((TInt)(User::TickCount() - iHistUntil) > 0)
		{
		iHistUntil = 0;
		return;
		}
	PwShared* s = iView->Shared();
	if (!s || iHist->Count() == 0)
		return;
	TBuf<PW_URL_MAX> url;
	FromUtf8(url, s->url);
	url.Trim();
	if (iHist->At(0).iUrl.Compare(url) != 0)
		{
		iHistUntil = 0;
		return;
		}
	TBuf<KPwTitleMax> title;
	FromUtf8(title, s->title);
	title.Trim();
	if (title.Length() && iHist->At(0).iTitle.Compare(title) != 0)
		iHist->SetTitleOnly(0, title);
	}

void CPwAppUi::OpenFromListL(CPwList& aList, TInt aIndex)
	{
	if (aIndex < 0 || aIndex >= aList.Count())
		return;
	TBuf<PW_URL_MAX> url(aList.At(aIndex).iUrl);     // (a copy: the list moves as pages load)
	iView->OpenUrlL(url);
	}

// Favourites > Add to favourites: the name from the page's title
void CPwAppUi::AddFavouriteL()
	{
	if (!HavePage())
		{
		iEikonEnv->InfoMsg(_L("Not available - no page open"));
		return;
		}
	PwShared* s = iView->Shared();
	TBuf<KPwTitleMax> name;
	FromUtf8(name, s->title);
	name.Trim();
	TBuf<PW_URL_MAX> url;
	FromUtf8(url, s->url);
	url.Trim();
	if (name.Length() == 0)
		name = Clip(url, KPwTitleMax);
	CPwFavDialog* dlg = new(ELeave) CPwFavDialog(name, url);
	if (!dlg->ExecuteLD(R_PW_FAV_DIALOG))
		return;
	TInt r = iFavs->AddEnd(name, url);
	if (r == KErrAlreadyExists)
		iEikonEnv->InfoMsg(_L("Already in favourites"));
	else if (r == KErrOverflow)
		iEikonEnv->InfoMsg(_L("Not added - 40 favourites is the most"));
	else
		iEikonEnv->InfoMsg(_L("Added to favourites"));
	}

void CPwAppUi::OrganiseL()
	{
	if (iFavs->Count() == 0)
		{
		iEikonEnv->InfoMsg(_L("No favourites"));
		return;
		}
	TInt pick = -1;
	CPwListDialog* dlg = new(ELeave) CPwListDialog(*iFavs, ETrue, pick);
	dlg->ExecuteLD(R_PW_ORGANISE_DIALOG);
	}

// a list to pick a page from (More favourites, Full history)
void CPwAppUi::PickAndOpenL(CPwList& aList)
	{
	if (aList.Count() == 0)
		{
		iEikonEnv->InfoMsg(&aList == iFavs ? _L("No favourites") : _L("No history"));
		return;
		}
	TInt pick = -1;
	CPwListDialog* dlg = new(ELeave) CPwListDialog(aList, EFalse, pick);
	if (dlg->ExecuteLD(&aList == iFavs ? R_PW_PICK_FAVS_DIALOG : R_PW_PICK_HISTORY_DIALOG) && pick >= 0)
		OpenFromListL(aList, pick);
	}

// Go > History > Clear history: the fact, then the question
void CPwAppUi::ClearHistoryL()
	{
	if (iHist->Count() == 0)
		{
		iEikonEnv->InfoMsg(_L("No history"));
		return;
		}
	if (iEikonEnv->QueryWinL(_L("All the pages in History will be removed"), _L("Clear history?")))
		{
		iHist->Clear();
		iHistUntil = 0;
		iEikonEnv->InfoMsg(_L("History cleared"));
		}
	}

static void AddMenuEntryL(CEikMenuPane* aPane, TInt aCommand, const TPwEntry& aEntry, const CFont& aFont, TInt aFlags)
	{
	CEikMenuPane::TItem::SData item;
	item.iCommandId = aCommand;
	item.iCascadeId = 0;
	item.iFlags = aFlags;
	TBuf<CEikMenuPane::TItem::SData::ENominalTextLength> t;
	ShownName(aEntry, t);
	ClipPx(t, aFont, 300);
	item.iText = t;
	item.iExtraText.Zero();
	aPane->AddMenuItemL(item);
	}

// Favourites: Add, Organise, then the first five (More... for the rest) - at
// most 8 commands
void CPwAppUi::FavsMenuL(CEikMenuPane* aPane)
	{
	TInt n = iFavs->Count();
	aPane->SetItemDimmed(EPwCmdAddFav, !HavePage());
	aPane->SetItemDimmed(EPwCmdOrganiseFav, n == 0);
	if (n == 0)
		return;
	aPane->ItemData(EPwCmdOrganiseFav).iFlags |= EEikMenuItemSeparatorAfter;
	const CFont* font = iEikonEnv->NormalFont();
	TInt shown = n > KPwMenuFavs ? KPwMenuFavs : n;
	for (TInt i = 0; i < shown; i++)
		AddMenuEntryL(aPane, EPwCmdFav0 + i, iFavs->At(i), *font, 0);
	if (n > KPwMenuFavs)
		{
		CEikMenuPane::TItem::SData more;
		more.iCommandId = EPwCmdFavMore;
		more.iCascadeId = 0;
		more.iFlags = 0;
		more.iText = _L("More favourites...");
		more.iExtraText.Zero();
		aPane->AddMenuItemL(more);
		}
	}

// History: the five latest pages, then Full history and Clear history. Items
// can only be added at the end, so the pane's own two are taken out and put
// back after the pages.
void CPwAppUi::HistoryMenuL(CEikMenuPane* aPane)
	{
	aPane->DeleteMenuItem(EPwCmdHistoryFull);
	aPane->DeleteMenuItem(EPwCmdClearHistory);
	TInt n = iHist->Count();
	const CFont* font = iEikonEnv->NormalFont();
	if (n == 0)
		{
		CEikMenuPane::TItem::SData none;
		none.iCommandId = EPwCmdHistNone;
		none.iCascadeId = 0;
		none.iFlags = EEikMenuItemSeparatorAfter;
		none.iText = _L("(No history)");
		none.iExtraText.Zero();
		aPane->AddMenuItemL(none);
		aPane->SetItemDimmed(EPwCmdHistNone, ETrue);     // (only when it is in the pane: EIKON 8 otherwise)
		}
	else
		{
		TInt shown = n > KPwMenuHist ? KPwMenuHist : n;
		for (TInt i = 0; i < shown; i++)
			AddMenuEntryL(aPane, EPwCmdHist0 + i, iHist->At(i), *font,
				i == shown - 1 ? EEikMenuItemSeparatorAfter : 0);
		}
	CEikMenuPane::TItem::SData full;
	full.iCommandId = EPwCmdHistoryFull;
	full.iCascadeId = 0;
	full.iFlags = 0;
	full.iText = _L("Full history...");
	full.iExtraText.Zero();
	aPane->AddMenuItemL(full);
	CEikMenuPane::TItem::SData clear;
	clear.iCommandId = EPwCmdClearHistory;
	clear.iCascadeId = 0;
	clear.iFlags = 0;
	clear.iText = _L("Clear history");
	clear.iExtraText.Zero();
	aPane->AddMenuItemL(clear);
	aPane->SetItemDimmed(EPwCmdHistoryFull, n == 0);
	aPane->SetItemDimmed(EPwCmdClearHistory, n == 0);
	}
