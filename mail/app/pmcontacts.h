// PMCONTACTS.H - PsiMail and the Psion's Contacts file
//
// The names and email addresses in the Contacts program's own file
// (Contacts.cdb, through the ER5 contacts model), read once a session when
// they are first wanted: the compose dialog's Contacts button and Tab
// completion, the reader's Add sender to Contacts, and friendlier names in
// the message list.

#ifndef __PMCONTACTS_H
#define __PMCONTACTS_H

#include <e32base.h>
#include <badesca.h>
#include <eikdialg.h>

class CContactDatabase;
class CEikColumnListBox;

class CPmContacts : public CBase
	{
public:
	static CPmContacts* NewL();
	~CPmContacts();
	// reads the Contacts file (a busy message while it does). Returns the
	// number of addresses, or -1 if there is no Contacts file yet; other
	// trouble leaves
	TInt LoadL();
	// LoadL if needed, saying why when it can't; ETrue when the list is ready
	TBool Ready();
	TBool Loaded() const { return iLoaded; }
	TInt Count() const { return iLoaded ? iAddrs->Count() : 0; }
	TPtrC Name(TInt aIndex) const { return (*iNames)[aIndex]; }
	TPtrC Addr(TInt aIndex) const { return (*iAddrs)[aIndex]; }
	// the picker: appends the chosen addresses to aField as "Name <addr>",
	// separated by ", ". aFilter starts the Find line. ETrue if any were added
	TBool PickL(TDes& aField, const TDesC& aFilter);
	// Tab in an address line: completes the last (partial) name or address
	// from Contacts; when several match, the picker shows them. ETrue if
	// aField changed
	TBool CompleteL(TDes& aField);
	// Add sender to Contacts: a new entry, or the address added to an entry
	// of that name. aMsg says what happened (for an infoprint)
	void AddL(const TDesC& aName, const TDesC& aAddr, TDes& aMsg);
	// the name for an address, when the list has been read: ETrue if found
	TBool NameFor(const TDesC& aAddr, TDes& aName) const;
	// "Name <addr>" (the name quoted if it needs to be), appended to a field
	static void AppendAddress(TDes& aField, const TDesC& aName, const TDesC& aAddr);
private:
	CPmContacts();
	void ConstructL();
	CContactDatabase* OpenLC(TBool aCreate);
	void ReadAllL(CContactDatabase& aDb);
	TInt FindByAddr(const TDesC& aAddr) const;
	TInt FindByName(const TDesC& aName) const;
private:
	CDesCArrayFlat* iNames;
	CDesCArrayFlat* iAddrs;
	CArrayFixFlat<TInt32>* iIds;       // the contact each address belongs to
	TBool iLoaded;
	TBool iNoFile;                     // there was no Contacts file when we looked
	};

// the picker: a Find line and the list (name, address); type to narrow it,
// Space or a tap marks several
class CPmContactsDialog : public CEikDialog
	{
public:
	CPmContactsDialog(CPmContacts& aContacts, const TDesC& aFilter, CArrayFixFlat<TInt>& aChosen)
		: iContacts(aContacts), iFilter(aFilter), iChosen(aChosen) {}
	~CPmContactsDialog();
private:
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void HandleControlStateChangeL(TInt aControlId);
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void FilterL();
	CPmContacts& iContacts;
	TPtrC iFilter;
	CArrayFixFlat<TInt>& iChosen;      // indexes in CPmContacts
	CArrayFixFlat<TInt>* iShown;       // list row -> index in CPmContacts
	TBuf<40> iFind;                    // what the list was last narrowed to
	};

#endif
