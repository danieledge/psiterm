// PMVCARD.CPP - contact cards (vCard) in messages, and your own card
//
// The engine reads a message's .vcf part (vCard 2.1, 3.0 or 4.0) into
// <uid>.vcd (../engine/invite.h). The reader shows a box at the top with
// the name, the addresses and numbers, and "Add to Contacts" (also Edit >
// Add to Contacts > Contact card). Each card becomes a Contacts entry with
// the fields the Contacts program's own template has, so they appear under
// its labels: names, Company and Job title, Home / Work tel, Mobile, fax and
// pager, Home / Work email, Home / Work address, Web page, Notes. A card
// whose email address is in Contacts already is left alone.
//
// Writing a message, Attach > Add my contact card attaches your own card
// (the account's name and address) as "<name>.vcf".

#include <eikenv.h>
#include <cntdb.h>
#include <cntitem.h>
#include <cntfield.h>
#include <cntfldst.h>
#include <cntdef.h>
#include <txtetext.h>
#include "pmapp.h"
#include "pmcontacts.h"

TPtrC PmKeyValue(const TDesC& aText, const TDesC& aKey);     // pminvite.cpp

TBool CPmView::HasCard() const
	{
	return iMode == EMessage && iCardText != NULL;
	}

// the n-th card's lines (between "begin" and "end")
static TPtrC CardBlock(const TDesC& aText, TInt aIndex)
	{
	TPtrC rest(aText);
	TInt n = -1;
	while (rest.Length())
		{
		TInt at = rest.Find(_L("\nbegin\t"));
		if (at < 0)
			break;
		rest.Set(rest.Mid(at + 1));
		if (++n == aIndex)
			{
			TInt end = rest.Find(_L("\nend"));
			return end >= 0 ? rest.Left(end + 1) : rest;
			}
		}
	return TPtrC();
	}

static TInt CardCount(const TDesC& aText)
	{
	TInt n = 0;
	while (n < 8 && CardBlock(aText, n).Length())
		n++;
	return n;
	}

// each value of a kind of line in a card: "email2 TAB a@b" -> calls back
// with the kind bits (VCF_* in invite.h) and the value
struct TPmCardLine { TInt iKind; TPtrC iValue; };
static TInt CardLines(const TDesC& aBlock, const TDesC& aPrefix, TPmCardLine* aOut, TInt aMax)
	{
	TPtrC rest(aBlock);
	TInt n = 0;
	while (rest.Length() && n < aMax)
		{
		TInt nl = rest.Locate('\n');
		TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
		TInt tab = line.Locate('\t');
		if (tab <= aPrefix.Length() || line.Left(aPrefix.Length()).Compare(aPrefix) != 0)
			continue;
		TLex lex(line.Mid(aPrefix.Length(), tab - aPrefix.Length()));
		TInt kind = 0;
		if (lex.Val(kind) != KErrNone)
			continue;
		aOut[n].iKind = kind;
		aOut[n].iValue.Set(line.Mid(tab + 1));
		n++;
		}
	return n;
	}

// the contact card's box (after the invitation's, if any): see InviteBannerL
void CPmView::CardBannerL(TDes& aText, CArrayFixFlat<TInt>& aStyles)
	{
	const TChar KPara(CEditableText::EParagraphDelimiter);
	TInt n = CardCount(*iCardText);
	if (!n)
		return;
	TInt p0 = aText.Length();
	aText.Append(n == 1 ? _L("Contact card: ") : _L("Contact cards: "));
	for (TInt i = 0; i < n && aText.Length() < aText.MaxLength() - 300; i++)
		{
		TPtrC b = CardBlock(*iCardText, i);
		if (i) aText.Append(_L(", "));
		aText.Append(Clip(PmKeyValue(b, _L("fn")), 60));
		TPtrC org = PmKeyValue(b, _L("org"));
		if (n == 1 && org.Length() && org.Compare(PmKeyValue(b, _L("fn"))) != 0)
			{
			aText.Append(_L(" ("));
			aText.Append(Clip(org, 60));
			aText.Append(')');
			}
		}
	aStyles.AppendL(p0); aStyles.AppendL(aText.Length() - p0); aStyles.AppendL(0);
	aStyles.AppendL(p0); aStyles.AppendL(aText.Length() - p0); aStyles.AppendL(2);
	aText.Append(KPara);
	if (n == 1)
		{
		// the addresses and numbers on one line
		TPtrC b = CardBlock(*iCardText, 0);
		TPmCardLine lines[VCF_LINES];
		TInt k, m = CardLines(b, _L("email"), lines, VCF_LINES), any = 0;
		for (k = 0; k < m; k++, any++) { if (any) aText.Append(_L(", ")); aText.Append(Clip(lines[k].iValue, 90)); }
		m = CardLines(b, _L("tel"), lines, VCF_LINES);
		for (k = 0; k < m; k++, any++) { if (any) aText.Append(_L(", ")); aText.Append(Clip(lines[k].iValue, 40)); }
		if (any)
			aText.Append(KPara);
		}
	TPmLinkRange lr;
	lr.iPos = aText.Length();
	if (n == 1) aText.Append(_L("Add to Contacts"));
	else if (n == 2) aText.Append(_L("Add both to Contacts"));
	else aText.AppendFormat(_L("Add all %d to Contacts"), n);
	lr.iLen = aText.Length() - lr.iPos;
	lr.iLink = KPmLinkAddCard;
	iLinks->AppendL(lr);
	aStyles.AppendL(lr.iPos); aStyles.AppendL(lr.iLen); aStyles.AppendL(1);
	aStyles.AppendL(lr.iPos); aStyles.AppendL(lr.iLen); aStyles.AppendL(0);
	aStyles.AppendL(lr.iPos); aStyles.AppendL(lr.iLen); aStyles.AppendL(4);
	aText.Append(KPara);
	}

// ---------------------------------------------------------------- into Contacts

// The fields go in as the Contacts file's own template has them: a
// template field with the same label (the Contacts program shows a card's
// fields by the template's) is copied, so its exact type comes along -
// on a 5mx that includes the separate town, region, postcode and country
// lines of an address. Without one (another language, an older template)
// the field is made from the standard types, the vCard mapping and the
// HOME / WORK / CELL... extras, with the label given.
static const CContactItemField* TemplateField(const CContactItem* aTemplate, const TDesC& aLabel)
	{
	if (!aTemplate)
		return NULL;
	const CContactItemFieldSet& fs = aTemplate->CardFields();
	for (TInt k = 0; k < fs.Count(); k++)
		if (fs[k].StorageType() == KStorageTypeText && fs[k].Label().CompareF(aLabel) == 0)
			return &fs[k];
	return NULL;
	}

static void AddField(CContactItem& aItem, const CContactItem* aTemplate, const TDesC& aText, TFieldType aType,
	TUid aMapping, const TDesC& aLabel, TUid aExtra1 = KNullUid, TUid aExtra2 = KNullUid)
	{
	if (!aText.Length())
		return;
	const CContactItemField* tf = TemplateField(aTemplate, aLabel);
	CContactItemField* f;
	if (tf)
		f = CContactItemField::NewLC(*tf);
	else
		{
		f = aType == KUidContactFieldNone ? CContactItemField::NewLC(KStorageTypeText)
			: CContactItemField::NewLC(KStorageTypeText, aType);
		f->SetMapping(aMapping);
		if (aExtra1 != KNullUid) f->AddFieldTypeL(aExtra1);
		if (aExtra2 != KNullUid) f->AddFieldTypeL(aExtra2);
		f->SetLabelL(aLabel);
		}
	f->TextStorage()->SetTextL(aText.Left(aText.Length() < 240 ? aText.Length() : 240));
	aItem.AddFieldL(*f);
	CleanupStack::Pop();                     // f: the item's now
	}

// part n of an address ("\x01" between the seven)
static TPtrC AdrPart(const TDesC& aAdr, TInt aIndex)
	{
	TPtrC rest(aAdr);
	for (TInt i = 0; i < aIndex; i++)
		{
		TInt sep = rest.Locate(0x01);
		if (sep < 0) return TPtrC();
		rest.Set(rest.Mid(sep + 1));
		}
	TInt sep = rest.Locate(0x01);
	return sep >= 0 ? rest.Left(sep) : rest;
	}

// an address: where the template has town / region / postcode / country
// lines, each part in its own; else all of it in the address, one part a line
static void AddAddress(CContactItem& aItem, const CContactItem* aTemplate, const TDesC& aAdr, TBool aWork)
	{
	TPtrC side = aWork ? _L("Work ") : _L("Home ");
	TBuf<20> city(side), region(side), code(side), country(side), label(side);
	city.Append(_L("city"));
	region.Append(_L("region"));
	code.Append(_L("p'code"));
	country.Append(_L("country"));
	label.Append(_L("address"));
	TBool split = TemplateField(aTemplate, city) != NULL;
	TBuf<240> street;
	for (TInt k = 0; k < (split ? 3 : 7); k++)
		{
		TPtrC part = AdrPart(aAdr, k);
		if (!part.Length() || street.Length() + part.Length() + 1 > street.MaxLength())
			continue;
		if (street.Length())
			street.Append(TChar(CEditableText::ELineBreak));
		street.Append(part);
		}
	TUid s = aWork ? KUidContactFieldVCardMapWORK : KUidContactFieldVCardMapHOME;
	AddField(aItem, aTemplate, street, KUidContactFieldAddress, KUidContactFieldVCardMapADR, label, s);
	if (split)
		{
		AddField(aItem, aTemplate, AdrPart(aAdr, 3), KUidContactFieldAddress, KUidContactFieldVCardMapADR, city, s);
		AddField(aItem, aTemplate, AdrPart(aAdr, 4), KUidContactFieldAddress, KUidContactFieldVCardMapADR, region, s);
		AddField(aItem, aTemplate, AdrPart(aAdr, 5), KUidContactFieldAddress, KUidContactFieldVCardMapADR, code, s);
		AddField(aItem, aTemplate, AdrPart(aAdr, 6), KUidContactFieldAddress, KUidContactFieldVCardMapADR, country, s);
		}
	}

// one card into the database
static TContactItemId AddCardL(CContactDatabase& aDb, const TDesC& aBlock)
	{
	CContactItem* tmpl = NULL;
	TRAPD(terr, tmpl = aDb.ReadContactL(aDb.TemplateId()));
	if (terr != KErrNone) tmpl = NULL;
	CleanupStack::PushL(tmpl);
	CContactCard* card = CContactCard::NewLC();
	TPtrC given = PmKeyValue(aBlock, _L("given")), family = PmKeyValue(aBlock, _L("family"));
	TBuf<80> fn;
	fn.Copy(Clip(PmKeyValue(aBlock, _L("fn")), 80));
	if (!given.Length() && !family.Length() && fn.Length())
		{
		// only a display name: the last word is the family name
		TInt sp = fn.LocateReverse(' ');
		AddField(*card, tmpl, sp > 0 ? fn.Left(sp) : TPtrC(), KUidContactFieldGivenName, KUidContactFieldVCardMapUnusedN, _L("First name"));
		AddField(*card, tmpl, sp > 0 ? fn.Mid(sp + 1) : TPtrC(fn), KUidContactFieldFamilyName, KUidContactFieldVCardMapUnusedN, _L("Last name"));
		}
	AddField(*card, tmpl, PmKeyValue(aBlock, _L("prefix")), KUidContactFieldPrefixName, KUidContactFieldVCardMapUnusedN, _L("Title"));
	AddField(*card, tmpl, given, KUidContactFieldGivenName, KUidContactFieldVCardMapUnusedN, _L("First name"));
	AddField(*card, tmpl, PmKeyValue(aBlock, _L("middle")), KUidContactFieldAdditionalName, KUidContactFieldVCardMapUnusedN, _L("Middle name"));
	AddField(*card, tmpl, family, KUidContactFieldFamilyName, KUidContactFieldVCardMapUnusedN, _L("Last name"));
	AddField(*card, tmpl, PmKeyValue(aBlock, _L("suffix")), KUidContactFieldSuffixName, KUidContactFieldVCardMapUnusedN, _L("Suffix"));
	TPtrC org = PmKeyValue(aBlock, _L("org"));
	// a card from a company: what it doesn't mark home or work is work
	TBool work = org.Length() > 0 || PmKeyValue(aBlock, _L("title")).Length() > 0;
	AddField(*card, tmpl, org, KUidContactFieldCompanyName, KUidContactFieldVCardMapORG, _L("Company"));
	AddField(*card, tmpl, PmKeyValue(aBlock, _L("title")), KUidContactFieldNone, KUidContactFieldVCardMapTITLE, _L("Job title"));

	TPmCardLine lines[VCF_LINES];
	TInt k, m = CardLines(aBlock, _L("tel"), lines, VCF_LINES);
	for (k = 0; k < m; k++)
		{
		TInt kind = lines[k].iKind;
		TBool w = (kind & VCF_WORK) || (!(kind & VCF_HOME) && work);
		TUid side = w ? KUidContactFieldVCardMapWORK : KUidContactFieldVCardMapHOME;
		if (kind & VCF_FAX)
			AddField(*card, tmpl, lines[k].iValue, KUidContactFieldFax, KUidContactFieldVCardMapTEL, w ? _L("Work fax") : _L("Home fax"), side, KUidContactFieldVCardMapFAX);
		else if (kind & VCF_PAGER)
			AddField(*card, tmpl, lines[k].iValue, KUidContactFieldPhoneNumber, KUidContactFieldVCardMapTEL, w ? _L("Work pager") : _L("Pager"), KUidContactFieldVCardMapPAGER, side);
		else if (kind & VCF_CELL)
			{
			TBool wm = (kind & VCF_WORK) != 0;     // a mobile is a personal one unless it says work
			AddField(*card, tmpl, lines[k].iValue, KUidContactFieldPhoneNumber, KUidContactFieldVCardMapTEL, wm ? _L("Work mobile") : _L("Mobile"),
				wm ? KUidContactFieldVCardMapCELL : KUidContactFieldVCardMapHOME, wm ? KUidContactFieldVCardMapWORK : KUidContactFieldVCardMapCELL);
			}
		else
			AddField(*card, tmpl, lines[k].iValue, KUidContactFieldPhoneNumber, KUidContactFieldVCardMapTEL, w ? _L("Work tel") : _L("Home tel"), side);
		}
	m = CardLines(aBlock, _L("email"), lines, VCF_LINES);
	for (k = 0; k < m; k++)
		{
		TBool w = (lines[k].iKind & VCF_WORK) || (!(lines[k].iKind & VCF_HOME) && work);
		AddField(*card, tmpl, lines[k].iValue, KUidContactFieldEMail, KUidContactFieldVCardMapEMAILINTERNET, w ? _L("Work email") : _L("Home email"),
			w ? KUidContactFieldVCardMapWORK : KUidContactFieldVCardMapHOME);
		}
	m = CardLines(aBlock, _L("adr"), lines, VCF_LINES);
	for (k = 0; k < m; k++)
		{
		TBool w = (lines[k].iKind & VCF_WORK) || (!(lines[k].iKind & VCF_HOME) && work);
		AddAddress(*card, tmpl, lines[k].iValue, w);
		}
	AddField(*card, tmpl, PmKeyValue(aBlock, _L("url")), KUidContactFieldUrl, KUidContactFieldVCardMapURL, _L("Web page"));
	AddField(*card, tmpl, PmKeyValue(aBlock, _L("note")), KUidContactFieldNote, KUidContactFieldVCardMapNOTE, _L("Notes"));
	TContactItemId id = aDb.AddNewContactL(*card);
	CleanupStack::PopAndDestroy(2);          // card, tmpl
	return id;
	}

// Add to Contacts: every card in the message not there already
void CPmView::AddCardsL()
	{
	if (!HasCard())
		{
		Toast(_L("This message has no contact card"));
		return;
		}
	TInt n = CardCount(*iCardText);
	if (!n)
		{
		Toast(_L("The contact card could not be read"));
		return;
		}
	CPmContacts* contacts = ((CPmAppUi*)iEikonEnv->EikAppUi())->Contacts();
	// what's there already (by email address)
	if (!contacts->Loaded())
		{
		TInt have = 0;
		TRAPD(err, have = contacts->LoadL());
		if (err == KErrNone && have < 0 &&
			!iEikonEnv->QueryWinL(_L("There is no Contacts file yet"), _L("Create one?")))
			return;
		if (err != KErrNone)
			{
			TBuf<80> t;
			if (err == KErrInUse || err == KErrLocked) t = _L("The Contacts file is in use - close Contacts and try again");
			else t.Format(_L("The Contacts file can't be read (%d)"), err);
			Toast(t);
			return;
			}
		}
	CContactDatabase* db = NULL;
	TRAPD(err, db = CContactDatabase::OpenL());
	if (err == KErrNotFound)
		TRAP(err, db = CContactDatabase::CreateL());
	if (err != KErrNone || !db)
		{
		TBuf<80> t;
		if (err == KErrInUse || err == KErrLocked) t = _L("The Contacts file is in use - close Contacts and try again");
		else t.Format(_L("The Contacts file can't be opened (%d)"), err);
		Toast(t);
		return;
		}
	CleanupStack::PushL(db);
	TInt added = 0, there = 0;
	TBuf<80> lastName, thereName;
	for (TInt i = 0; i < n; i++)
		{
		TPtrC b = CardBlock(*iCardText, i);
		TPmCardLine lines[VCF_LINES];
		TInt m = CardLines(b, _L("email"), lines, VCF_LINES), k;
		TBuf<80> who;
		TBool known = EFalse;
		for (k = 0; k < m && !known; k++)
			known = contacts->NameFor(lines[k].iValue, who);
		if (known)
			{
			there++;
			thereName = who.Length() ? who : TBuf<80>(Clip(PmKeyValue(b, _L("fn")), 80));
			continue;
			}
		AddCardL(*db, b);
		added++;
		lastName.Copy(Clip(PmKeyValue(b, _L("fn")), 80));
		}
	CleanupStack::PopAndDestroy();           // db
	if (added)
		{
		// the list PsiMail keeps (for Tab completion and the picker) again
		TRAP(err, contacts->LoadL());
		}
	TBuf<120> msg;
	if (added == 1 && !there)
		{
		msg = _L("Added to Contacts");
		if (lastName.Length()) { msg.Append(_L(" - ")); msg.Append(lastName); }
		}
	else if (added)
		{
		msg.Format(_L("%d added to Contacts"), added);
		if (there) msg.AppendFormat(_L(" - %d there already"), there);
		}
	else if (there == 1)
		{
		msg = _L("Already in Contacts");
		if (thereName.Length()) { msg.Append(_L(" - ")); msg.Append(thereName); }
		}
	else
		msg = _L("Already in Contacts");
	Toast(msg);
	}

// ---------------------------------------------------------------- your card

// cp1252 -> UTF-8 (vCard 3.0 is UTF-8)
static void AppendUtf8(TDes8& aOut, const TDesC& aText)
	{
	static const TUint16 K80[32] = { 0x20ac, 0, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17d, 0,
		0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0, 0x17e, 0x178 };
	for (TInt i = 0; i < aText.Length() && aOut.Length() < aOut.MaxLength() - 3; i++)
		{
		TUint c = aText[i];
		if (c >= 0x80 && c < 0xa0) c = K80[c - 0x80] ? K80[c - 0x80] : '?';
		if (c == ';' || c == ',' || c == '\\') { aOut.Append('\\'); aOut.Append((TChar)c); }
		else if (c < 0x80) aOut.Append((TChar)c);
		else if (c < 0x800) { aOut.Append((TChar)(0xc0 | (c >> 6))); aOut.Append((TChar)(0x80 | (c & 0x3f))); }
		else { aOut.Append((TChar)(0xe0 | (c >> 12))); aOut.Append((TChar)(0x80 | ((c >> 6) & 0x3f))); aOut.Append((TChar)(0x80 | (c & 0x3f))); }
		}
	}

// writes the account's own card (name and address, vCard 3.0) as
// <store>A<n>\card\<name>.vcf, for attaching; leaves if it can't
void CPmView::MyCardFileL(TDes& aPath)
	{
	const PmAccount& a = iSettings->iAccounts[iSettings->iAcct];
	TPtrC8 n8((const TUint8*)a.fullname), e8((const TUint8*)a.email);
	TBuf<64> name;
	name.Copy(n8.Left(n8.Length() < 64 ? n8.Length() : 64));
	name.Trim();
	TBuf<96> email;
	email.Copy(e8.Left(e8.Length() < 96 ? e8.Length() : 96));
	if (!email.Length())
		User::Leave(KErrNotFound);
	// a file name the Psion (and the other end) can take
	TBuf<64> file(name.Length() ? name : TBuf<64>(_L("My card")));
	for (TInt i = 0; i < file.Length(); i++)
		{
		TText c = file[i];
		if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c < 32)
			file[i] = '_';
		}
	StoreDir(aPath);
	aPath.AppendFormat(_L("A%d\\card\\"), iSettings->iAcct);
	RFs& fs = iCoeEnv->FsSession();
	fs.MkDirAll(aPath);
	aPath.Append(file);
	aPath.Append(_L(".vcf"));
	HBufC8* vb = HBufC8::NewLC(600);         // (on the heap: 600 bytes is a lot of an app thread's stack)
	TPtr8 v = vb->Des();
	v.Append(_L8("BEGIN:VCARD\r\nVERSION:3.0\r\nPRODID:-//PsiMail//Psion Series 5mx//EN\r\nN:"));
	TInt sp = name.LocateReverse(' ');
	AppendUtf8(v, sp > 0 ? name.Mid(sp + 1) : TPtrC(name));
	v.Append(';');
	AppendUtf8(v, sp > 0 ? name.Left(sp) : TPtrC());
	v.Append(_L8(";;;\r\nFN:"));
	AppendUtf8(v, name.Length() ? TPtrC(name) : TPtrC(email));
	v.Append(_L8("\r\nEMAIL;TYPE=INTERNET:"));
	AppendUtf8(v, email);
	v.Append(_L8("\r\nEND:VCARD\r\n"));
	RFile f;
	User::LeaveIfError(f.Replace(fs, aPath, EFileWrite));
	TInt r = f.Write(v);
	f.Close();
	CleanupStack::PopAndDestroy();           // vb
	User::LeaveIfError(r);
	}

void CPmAppUi::MyCardL(TDes& aPath)
	{
	iView->MyCardFileL(aPath);
	}
