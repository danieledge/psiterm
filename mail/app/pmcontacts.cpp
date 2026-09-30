// PMCONTACTS.CPP - PsiMail and the Psion's Contacts file (see pmcontacts.h)
//
// Everything goes through the ER5 contacts model (cntmodel.dll), the same
// one the Contacts program uses, so what PsiMail adds shows up there at
// once, and vice versa. The file is read once a session, when first needed:
// on a 5mx that is a second or two for a few hundred entries.

#include <e32keys.h>
#include <eikenv.h>
#include <eikedwin.h>
#include <eikclb.h>
#include <eikclbd.h>
#include <eiklbv.h>
#include <eiksbfrm.h>
#include <cntdb.h>
#include <cntitem.h>
#include <cntfield.h>
#include <cntfldst.h>
#include <psimail.rsg>
#include "psimail.hrh"
#include "pmcontacts.h"

const TInt KPmMaxName = 60;
const TInt KPmMaxAddr = 90;

// ============================================================================
// The list
// ============================================================================

CPmContacts* CPmContacts::NewL()
	{
	CPmContacts* self = new(ELeave) CPmContacts;
	CleanupStack::PushL(self);
	self->ConstructL();
	CleanupStack::Pop();
	return self;
	}

CPmContacts::CPmContacts()
	{
	}

void CPmContacts::ConstructL()
	{
	iNames = new(ELeave) CDesCArrayFlat(32);
	iAddrs = new(ELeave) CDesCArrayFlat(32);
	iIds = new(ELeave) CArrayFixFlat<TInt32>(32);
	}

CPmContacts::~CPmContacts()
	{
	delete iNames;
	delete iAddrs;
	delete iIds;
	}

// the Contacts file: the standard one (C:\System\Data\Contacts.cdb, or
// wherever the Contacts program keeps it). Leaves KErrNotFound if there is
// none, unless aCreate
CContactDatabase* CPmContacts::OpenLC(TBool aCreate)
	{
	CContactDatabase* db = NULL;
	TRAPD(err, db = CContactDatabase::OpenL());
	if (err == KErrNotFound && aCreate)
		{
		db = CContactDatabase::CreateL();
		err = KErrNone;
		}
	User::LeaveIfError(err);
	CleanupStack::PushL(db);
	return db;
	}

TInt CPmContacts::LoadL()
	{
	iNames->Reset();
	iAddrs->Reset();
	iIds->Reset();
	iLoaded = EFalse;
	iNoFile = EFalse;
	CContactDatabase* db = NULL;
	TRAPD(err, db = CContactDatabase::OpenL());
	if (err == KErrNotFound)
		{
		iNoFile = ETrue;
		iLoaded = ETrue;                 // (an empty list, until a file appears)
		return -1;
		}
	User::LeaveIfError(err);
	CleanupStack::PushL(db);
	CEikonEnv* env = CEikonEnv::Static();
	env->BusyMsgL(_L("Reading Contacts..."), EHLeftVBottom, TTimeIntervalMicroSeconds32(300000));
	TRAP(err, ReadAllL(*db));
	env->BusyMsgCancel();
	CleanupStack::PopAndDestroy();       // db
	User::LeaveIfError(err);
	iLoaded = ETrue;
	return iAddrs->Count();
	}

// every card, in the Contacts program's own order, reading only the
// fields wanted (the name, the company and the email addresses)
void CPmContacts::ReadAllL(CContactDatabase& aDb)
	{
	CContactItemViewDef* vd = CContactItemViewDef::NewLC(CContactItemViewDef::EIncludeFields, CContactItemViewDef::EMaskHiddenFields);
	vd->AddL(KUidContactFieldGivenName);
	vd->AddL(KUidContactFieldFamilyName);
	vd->AddL(KUidContactFieldCompanyName);
	vd->AddL(KUidContactFieldEMail);
	const CContactIdArray* ids = aDb.SortedItemsL();     // (the database's)
	TInt n = ids ? ids->Count() : 0;
	TBuf<KPmMaxName> name, given, family, company;
	TBuf<KPmMaxAddr> addr;
	for (TInt i = 0; i < n; i++)
		{
		TContactItemId id = (*ids)[i];
		CContactItem* item = NULL;
		TRAPD(err, item = aDb.ReadContactL(id, *vd));
		if (err != KErrNone || !item)
			continue;                                     // (a damaged entry: skip it)
		CleanupStack::PushL(item);
		if (item->Type() == KUidContactCard)
			{
			given.Zero(); family.Zero(); company.Zero();
			const CContactItemFieldSet& fields = item->CardFields();
			TInt k;
			for (k = 0; k < fields.Count(); k++)
				{
				const CContactItemField& f = fields[k];
				if (f.StorageType() != KStorageTypeText || !f.TextStorage())
					continue;
				const CContentType& ct = f.ContentType();
				TPtrC text = f.TextStorage()->Text();
				TPtrC t = text.Left(text.Length() < KPmMaxName ? text.Length() : KPmMaxName);
				if (ct.ContainsFieldType(KUidContactFieldGivenName)) given = t;
				else if (ct.ContainsFieldType(KUidContactFieldFamilyName)) family = t;
				else if (ct.ContainsFieldType(KUidContactFieldCompanyName)) company = t;
				}
			name = given;
			if (family.Length())
				{
				if (name.Length() + family.Length() + 1 <= name.MaxLength())
					{
					if (name.Length()) name.Append(' ');
					name.Append(family);
					}
				}
			if (!name.Length())
				name = company;
			name.Trim();
			for (k = 0; k < fields.Count(); k++)
				{
				const CContactItemField& f = fields[k];
				if (f.StorageType() != KStorageTypeText || !f.TextStorage() || !f.ContentType().ContainsFieldType(KUidContactFieldEMail))
					continue;
				TPtrC text = f.TextStorage()->Text();
				addr.Copy(text.Left(text.Length() < KPmMaxAddr ? text.Length() : KPmMaxAddr));
				addr.Trim();
				if (addr.Locate('@') < 0)
					continue;
				iNames->AppendL(name);
				iAddrs->AppendL(addr);
				iIds->AppendL(id);
				}
			}
		CleanupStack::PopAndDestroy();               // item
		}
	CleanupStack::PopAndDestroy();                   // vd
	}

TBool CPmContacts::Ready()
	{
	if (iLoaded && !iNoFile)
		return ETrue;
	TInt n = 0;
	TRAPD(err, n = LoadL());
	CEikonEnv* env = CEikonEnv::Static();
	if (err != KErrNone)
		{
		TBuf<80> t;
		if (err == KErrInUse || err == KErrLocked)
			t = _L("The Contacts file is in use - close Contacts and try again");
		else
			t.Format(_L("The Contacts file can't be read (%d)"), err);
		env->InfoMsg(t);
		return EFalse;
		}
	if (n < 0)
		{
		env->InfoMsg(_L("No Contacts file - open the Contacts program to make one"));
		return EFalse;
		}
	if (n == 0)
		{
		env->InfoMsg(_L("No email addresses in Contacts"));
		return EFalse;
		}
	return ETrue;
	}

TInt CPmContacts::FindByAddr(const TDesC& aAddr) const
	{
	for (TInt i = 0; i < iAddrs->Count(); i++)
		if ((*iAddrs)[i].CompareF(aAddr) == 0)
			return i;
	return -1;
	}

TInt CPmContacts::FindByName(const TDesC& aName) const
	{
	if (!aName.Length())
		return -1;
	for (TInt i = 0; i < iNames->Count(); i++)
		if ((*iNames)[i].CompareF(aName) == 0)
			return i;
	return -1;
	}

TBool CPmContacts::NameFor(const TDesC& aAddr, TDes& aName) const
	{
	if (!iLoaded)
		return EFalse;
	TInt i = FindByAddr(aAddr);
	if (i < 0 || !(*iNames)[i].Length())
		return EFalse;
	const TDesC& n = (*iNames)[i];
	aName.Copy(n.Left(n.Length() < aName.MaxLength() ? n.Length() : aName.MaxLength()));
	return ETrue;
	}

// "Name <addr>", the name in quotes if it has a comma or the like in it;
// just the address when there is no name, or it is the address
void CPmContacts::AppendAddress(TDes& aField, const TDesC& aName, const TDesC& aAddr)
	{
	TBuf<KPmMaxName + KPmMaxAddr + 8> item;
	TBool named = aName.Length() && aName.CompareF(aAddr) != 0;
	if (named)
		{
		TBool quote = EFalse;
		for (TInt i = 0; i < aName.Length(); i++)
			{
			TText c = aName[i];
			if (c == ',' || c == ';' || c == '<' || c == '>' || c == '@' || c == ':' || c == '"' || c == '(' || c == ')')
				quote = ETrue;
			}
		if (quote) item.Append('"');
		for (TInt k = 0; k < aName.Length(); k++)
			if (aName[k] != '"') item.Append(aName[k]);
		if (quote) item.Append('"');
		item.Append(_L(" <"));
		item.Append(aAddr);
		item.Append('>');
		}
	else
		item = aAddr;
	// already there? then leave it
	if (aField.FindF(aAddr) >= 0)
		return;
	TInt need = item.Length() + 2;
	if (aField.Length() + need > aField.MaxLength())
		return;
	aField.TrimRight();
	if (aField.Length() && aField[aField.Length() - 1] != ',')
		aField.Append(',');
	if (aField.Length())
		aField.Append(' ');
	aField.Append(item);
	}

TBool CPmContacts::PickL(TDes& aField, const TDesC& aFilter)
	{
	if (!Ready())
		return EFalse;
	CArrayFixFlat<TInt>* chosen = new(ELeave) CArrayFixFlat<TInt>(8);
	CleanupStack::PushL(chosen);
	CPmContactsDialog* dlg = new(ELeave) CPmContactsDialog(*this, aFilter, *chosen);
	TBool ok = dlg->ExecuteLD(R_PM_CONTACTS_DIALOG) && chosen->Count();
	if (ok)
		for (TInt i = 0; i < chosen->Count(); i++)
			{
			TInt k = (*chosen)[i];
			if (k >= 0 && k < iAddrs->Count())
				AppendAddress(aField, (*iNames)[k], (*iAddrs)[k]);
			}
	CleanupStack::PopAndDestroy();       // chosen
	return ok;
	}

// Tab: the text after the last comma is what to complete
TBool CPmContacts::CompleteL(TDes& aField)
	{
	TInt start = 0;
	for (TInt i = aField.Length() - 1; i >= 0; i--)
		if (aField[i] == ',' || aField[i] == ';') { start = i + 1; break; }
	TPtrC part = aField.Mid(start);
	TInt s = 0, e = part.Length();
	while (s < e && part[s] == ' ') s++;
	while (e > s && part[e - 1] == ' ') e--;
	TBuf<KPmMaxName> word;
	TPtrC w = part.Mid(s, e - s);
	word.Copy(w.Left(w.Length() < word.MaxLength() ? w.Length() : word.MaxLength()));
	if (!word.Length())
		{
		// nothing typed yet: the whole list
		return PickL(aField, KNullDesC);
		}
	if (word.Locate('<') >= 0)
		return EFalse;                   // already complete
	if (!Ready())
		return EFalse;
	// one match (a name or address starting with it, or containing it)?
	TInt found = -1, count = 0;
	for (TInt pass = 0; pass < 2 && count != 1; pass++)
		{
		found = -1; count = 0;
		for (TInt k = 0; k < iAddrs->Count(); k++)
			{
			const TDesC& n = (*iNames)[k];
			const TDesC& a = (*iAddrs)[k];
			TBool hit;
			if (pass == 0)
				hit = (n.Length() >= word.Length() && n.Left(word.Length()).CompareF(word) == 0) ||
					(a.Length() >= word.Length() && a.Left(word.Length()).CompareF(word) == 0);
			else
				hit = n.FindF(word) >= 0 || a.FindF(word) >= 0;
			if (hit)
				{
				// the same person twice (two addresses) still counts as several
				if (found < 0) found = k;
				count++;
				}
			}
		}
	if (count == 1)
		{
		aField.SetLength(start);
		AppendAddress(aField, (*iNames)[found], (*iAddrs)[found]);
		return ETrue;
		}
	if (count == 0)
		{
		CEikonEnv::Static()->InfoMsg(_L("Not in Contacts"));
		return EFalse;
		}
	// several: the picker, narrowed to what was typed
	TBuf<KPmMaxName + KPmMaxAddr + 8> typed;
	typed.Copy(part.Left(part.Length() < typed.MaxLength() ? part.Length() : typed.MaxLength()));
	aField.SetLength(start);
	if (!PickL(aField, word))
		{
		aField.SetLength(start);
		aField.Append(typed);            // (put back what was typed)
		return EFalse;
		}
	return ETrue;
	}

// a new entry, or the address added to the entry of the same name
void CPmContacts::AddL(const TDesC& aName, const TDesC& aAddr, TDes& aMsg)
	{
	aMsg.Zero();
	TBuf<KPmMaxName> name;
	name.Copy(aName.Left(aName.Length() < KPmMaxName ? aName.Length() : KPmMaxName));
	name.Trim();
	TBuf<KPmMaxAddr> addr;
	addr.Copy(aAddr.Left(aAddr.Length() < KPmMaxAddr ? aAddr.Length() : KPmMaxAddr));
	addr.Trim();
	if (addr.Locate('@') < 0)
		{
		aMsg = _L("No email address to add");
		return;
		}
	if (name.CompareF(addr) == 0)
		name.Zero();
	// what's there already (this reads the file if it hasn't been)
	if (!iLoaded || iNoFile)
		{
		TInt n = LoadL();
		if (n < 0 && !CEikonEnv::Static()->QueryWinL(_L("There is no Contacts file yet"), _L("Create one?")))
			{
			aMsg = _L("Not added");
			return;
			}
		}
	TInt have = FindByAddr(addr);
	if (have >= 0)
		{
		aMsg = _L("Already in Contacts");
		if ((*iNames)[have].Length())
			{
			aMsg.Append(_L(" - "));
			aMsg.Append((*iNames)[have]);
			}
		return;
		}
	TInt same = FindByName(name);
	CContactDatabase* db = OpenLC(ETrue);
	iNoFile = EFalse;
	CContactItemField* f;
	if (same >= 0)
		{
		// that person is there without this address: add it to them
		TContactItemId id = (*iIds)[same];
		CContactItem* item = db->OpenContactLX(id);
		CleanupStack::PushL(item);
		f = CContactItemField::NewLC(KStorageTypeText, KUidContactFieldEMail);
		f->SetMapping(KUidContactFieldVCardMapEMAILINTERNET);
		f->TextStorage()->SetTextL(addr);
		item->AddFieldL(*f);
		CleanupStack::Pop();             // f: the item's now
		db->CommitContactL(*item);
		CleanupStack::PopAndDestroy(2);  // item, the lock
		iNames->AppendL(name);
		iAddrs->AppendL(addr);
		iIds->AppendL(id);
		aMsg = _L("Address added to ");
		aMsg.Append(name);
		}
	else
		{
		// a new card: first name(s), last name, the address
		CContactCard* card = CContactCard::NewLC();
		TInt sp = name.LocateReverse(' ');
		TPtrC given = sp > 0 ? name.Left(sp) : TPtrC();
		TPtrC family = sp > 0 ? name.Mid(sp + 1) : TPtrC(name);
		if (given.Length())
			{
			f = CContactItemField::NewLC(KStorageTypeText, KUidContactFieldGivenName);
			f->SetMapping(KUidContactFieldVCardMapUnusedN);
			f->TextStorage()->SetTextL(given);
			card->AddFieldL(*f);
			CleanupStack::Pop();
			}
		if (family.Length())
			{
			f = CContactItemField::NewLC(KStorageTypeText, KUidContactFieldFamilyName);
			f->SetMapping(KUidContactFieldVCardMapUnusedN);
			f->TextStorage()->SetTextL(family);
			card->AddFieldL(*f);
			CleanupStack::Pop();
			}
		f = CContactItemField::NewLC(KStorageTypeText, KUidContactFieldEMail);
		f->SetMapping(KUidContactFieldVCardMapEMAILINTERNET);
		f->TextStorage()->SetTextL(addr);
		card->AddFieldL(*f);
		CleanupStack::Pop();
		TContactItemId id = db->AddNewContactL(*card);
		CleanupStack::PopAndDestroy();   // card
		iNames->AppendL(name);
		iAddrs->AppendL(addr);
		iIds->AppendL(id);
		aMsg = _L("Added to Contacts");
		if (name.Length())
			{
			aMsg.Append(_L(" - "));
			aMsg.Append(name);
			}
		}
	CleanupStack::PopAndDestroy();       // db
	}

// ============================================================================
// The picker
// ============================================================================

void CPmContactsDialog::PreLayoutDynInitL()
	{
	iShown = new(ELeave) CArrayFixFlat<TInt>(32);
	TBuf<40> f;
	f.Copy(iFilter.Left(iFilter.Length() < 40 ? iFilter.Length() : 40));
	SetEdwinTextL(EPmDlgFind, &f);
	CEikColumnListBox* list = (CEikColumnListBox*)Control(EPmDlgContactList);
	list->Model()->SetItemTextArray(new(ELeave) CDesCArrayFlat(32));
	list->Model()->SetOwnershipType(ELbmOwnsItemArray);
	CColumnListBoxData* cd = list->Model()->ColumnData();
	cd->SetColumnWidthPixelL(0, 210);
	cd->SetColumnWidthPixelL(1, 290);
	// (the rows' height comes from the columns' fonts: without them, the
	// list would be a line high)
	cd->SetColumnFontL(0, iEikonEnv->NormalFont());
	cd->SetColumnFontL(1, iEikonEnv->NormalFont());
	if (!list->ScrollBarFrame())
		list->CreateScrollBarFrameL();
	list->ScrollBarFrame()->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);
	iFind = f;
	FilterL();
	}

void CPmContactsDialog::PostLayoutDynInitL()
	{
	// with something to find typed already, start in the list
	if (iFind.Length())
		TryChangeFocusToL(EPmDlgContactList);
	}

// the rows: those with the Find text somewhere in the name or address
void CPmContactsDialog::FilterL()
	{
	CEikColumnListBox* list = (CEikColumnListBox*)Control(EPmDlgContactList);
	CDesCArray* items = (CDesCArray*)list->Model()->ItemTextArray();
	items->Reset();
	iShown->Reset();
	list->ClearSelection();
	TBuf<KPmMaxName + KPmMaxAddr + 4> line;
	for (TInt i = 0; i < iContacts.Count(); i++)
		{
		const TDesC& n = iContacts.Name(i);
		const TDesC& a = iContacts.Addr(i);
		if (iFind.Length() && n.FindF(iFind) < 0 && a.FindF(iFind) < 0)
			continue;
		line = n;
		line.Append('\t');
		line.Append(a);
		items->AppendL(line);
		iShown->AppendL(i);
		}
	list->Reset();
	list->HandleItemAdditionL();
	if (items->Count())
		list->SetCurrentItemIndex(0);
	list->UpdateScrollBarsL();
	list->DrawDeferred();
	}

void CPmContactsDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId != EPmDlgFind)
		return;
	TBuf<40> f;
	GetEdwinText(f, EPmDlgFind);
	if (f == iFind)
		return;
	iFind = f;
	FilterL();
	}

// letters typed in the list narrow it too
TKeyResponse CPmContactsDialog::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType == EEventKey && IdOfFocusControl() == EPmDlgContactList && !(aKeyEvent.iModifiers & (EModifierCtrl | EModifierFunc)))
		{
		// (Space marks the row: the list does that itself)
		TUint c = aKeyEvent.iCode;
		if (c == EKeyBackspace || (c >= 0x21 && c < 0x7f))
			{
			if (c == EKeyBackspace)
				{
				if (iFind.Length()) iFind.SetLength(iFind.Length() - 1);
				}
			else if (iFind.Length() < iFind.MaxLength())
				iFind.Append((TText)c);
			SetEdwinTextL(EPmDlgFind, &iFind);
			Control(EPmDlgFind)->DrawDeferred();
			FilterL();
			return EKeyWasConsumed;
			}
		}
	return CEikDialog::OfferKeyEventL(aKeyEvent, aType);
	}

TBool CPmContactsDialog::OkToExitL(TInt /*aButtonId*/)
	{
	CEikColumnListBox* list = (CEikColumnListBox*)Control(EPmDlgContactList);
	iChosen.Reset();
	// the marked rows; with none marked, the highlighted one
	const CListBoxView::CSelectionIndexArray* sel = list->SelectionIndexes();
	if (sel)
		for (TInt i = 0; i < sel->Count(); i++)
			{
			TInt row = (*sel)[i];
			if (row >= 0 && row < iShown->Count())
				iChosen.AppendL((*iShown)[row]);
			}
	if (!iChosen.Count())
		{
		TInt cur = list->CurrentItemIndex();
		if (cur >= 0 && cur < iShown->Count())
			iChosen.AppendL((*iShown)[cur]);
		}
	if (!iChosen.Count())
		{
		CEikonEnv::Static()->InfoMsg(_L("No contact selected"));
		return EFalse;
		}
	return ETrue;
	}

CPmContactsDialog::~CPmContactsDialog()
	{
	delete iShown;
	}
