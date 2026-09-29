// PMAPP.H - PsiMail.app: the Psion side of PsiMail
//
// PsiMail.app owns the screen, keyboard, pen, menus and settings. The mail
// engine (psimail.exe) does the network work and keeps the local store; the
// app reads the store's files to show folders, lists and messages, and asks
// the engine for everything else (see ../psimail.h).

#ifndef __PMAPP_H
#define __PMAPP_H

#include <coecntrl.h>
#include <coemain.h>
#include <eikappui.h>
#include <eikapp.h>
#include <eikdoc.h>
#include <eikenv.h>
#include <eikmenup.h>
#include <eikcmds.hrh>
#include <eikmenu.hrh>
#include <f32file.h>
#include <eikdialg.h>
#include <eikdialg.hrh>
#include <badesca.h>

extern "C" {
#include <psimail.h>
}

#include <psimail.rsg>
#include "psimail.hrh"

const TUid KUidPsiMail = { 0x01000A7C };

// app-wide settings (accounts are PmAccount, as the engine uses them)
struct TPmSettings
	{
	TInt iBaudIndex;       // 0=9600 .. 4=115200
	TInt iRtsCts;
	TInt iNetMode;         // 0 = modem "ATDT host:port", 1 = Psion TCP/IP
	TInt iOffline;
	TInt iAcct;            // current account
	TInt iStore;           // 0 = CF card if there is one, 1 = internal disk
	PmAccount iAccounts[PM_MAX_ACCOUNTS];
	};

struct TPmFolder
	{
	TBuf8<128> iImap;      // the server's name for it
	TBuf<64> iName;        // shown
	TUint iKind;           // 'I' inbox, 'S' sent, 'D' drafts, 'T' trash, 'J' junk, 'A' archive, 'N' no messages, '-'
	TInt iUnread;
	TInt iTotal;
	};

struct TPmRow
	{
	TUint iUid;            // (outbox: the file's number)
	TBuf<12> iFlags;       // S seen F flagged A answered T attachment B downloaded; outbox: D draft E error
	TInt iDate;            // seconds since 1970, UTC
	TInt iSize;
	TBuf<64> iFrom;        // shown: the name, or the address
	TBuf<100> iSubject;
	};

// a message being written
class CPmDraft : public CBase
	{
public:
	~CPmDraft();
	static CPmDraft* NewL();
	TBuf<500> iTo;
	TBuf<500> iCc;
	TBuf<300> iBcc;
	TBuf<200> iSubject;
	HBufC* iBody;
	TBuf<120> iInReplyTo;
	TBuf<400> iReferences;
	CDesCArrayFlat* iAttach;   // file names
	TBuf8<128> iReplyFolder;
	TUint iReplyUid;
	TInt iFileNo;              // outbox file it came from, or 0
	};

class CPmView;

class CPmWatcher : public CActive
	{
public:
	CPmWatcher(CPmView& aView);
	~CPmWatcher();
	void Watch(RProcess& aProcess);
private:
	void RunL();
	void DoCancel();
	CPmView& iView;
	RProcess* iProcess;
	};

class CPmView : public CCoeControl
	{
public:
	enum TMode { EFolders, EList, EMessage, EOutbox, ENoAccount };
	~CPmView();
	void ConstructL(const TRect& aRect, TPmSettings& aSettings);
	TMode Mode() const { return iMode; }
	PmShared* Shared() { return iShared; }
	TBool EngineRunning() const { return iRunning; }
	void StartEngineL();
	void StopEngine();
	void EngineEnded();
	void SettingsChanged();                  // accounts or connection edited
	void AccountChangedL();                  // switched to another account
	void Cmd(TInt aOp, const TDesC8& aFolder, TUint aUid, const TDesC8& aArg);
	TBool Busy() const;
	TBool HasSelection() const;
	const TPmRow* CurrentRow() const;
	TBool CurrentIsSearch() const { return iSearch; }
	const TPmFolder* CurrentFolder() const;
	const TDesC8& FolderImap() const { return iFolder; }
	TInt FolderCount() const;
	const TPmFolder& FolderAt(TInt aIndex) const;
	void ShowFoldersL();
	void OpenFolderL(const TDesC8& aImap);
	void ShowOutboxL();
	void OpenCurrentL();
	void BackL();
	void StepMessageL(TInt aDir);
	void DeleteCurrentL();
	void MoveCurrentL(const TDesC8& aDest);
	void ToggleFlagL(TChar aFlag);
	void RefreshL();
	void OlderL();
	void SearchL(const TDesC& aWords);
	void SendRecvL();
	void WholeMessageL();
	void SaveAttachmentL(TInt aIndex);
	TInt AttachmentCount() const;
	void AttachmentsL(CDesCArray& aNames);
	TBool MessageHeader(const TDesC& aName, TDes& aValue) const;
	void QuoteBodyL(TDes& aOut, TInt aMaxLines) const;
	void ToggleHeaders();
	void DraftFromOutboxL(CPmDraft& aDraft);
	void SaveDraftL(CPmDraft& aDraft, TBool aSend);
	void DeleteOutboxL();
	void Title(TDes& aTitle) const;
	TInt OutboxCount();
private:
	void Draw(const TRect& aRect) const;
	void DrawTitle(CWindowGc& aGc) const;
	void DrawStatus(CWindowGc& aGc) const;
	void DrawFolders(CWindowGc& aGc) const;
	void DrawList(CWindowGc& aGc) const;
	void DrawMessage(CWindowGc& aGc) const;
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void HandlePointerEventL(const TPointerEvent& aEvent);
	void AddEntropy(TUint aValue);
	static TInt TickCallback(TAny* aSelf);
	void Tick();
	void TickL();
	void HandleResultL(const PmCmd& aCmd);
	void ReloadL();
	void LoadFoldersL();
	void LoadListL();
	void LoadOutboxL();
	void LoadMessageL();
	void WrapMessageL();
	void ReadFileL(const TDesC& aName, HBufC*& aBuf, TInt aMax);
	void StoreDir(TDes& aDir) const;
	void FolderDir(const TDesC8& aImap, TDes& aDir) const;
	void MsgPath(TUint aUid, const TDesC& aExt, TDes& aPath) const;
	void OutboxDir(TDes& aDir) const;
	TInt Rows() const;
	TInt LineHeight() const { return iLineH; }
	TRect BodyRect() const;
	void MoveSel(TInt aDelta);
	void EnsureVisible();
	void FormatDate(TInt aDate, TDes& aOut) const;
	void CopySettingsToShared();
	void SetStatus(const TDesC& aText);
	void Redraw();
private:
	TPmSettings* iSettings;
	RChunk iChunk;
	TBool iChunkOpen;
	PmShared* iShared;
	RProcess iProcess;
	TBool iRunning;
	CPmWatcher* iWatcher;
	CPeriodic* iTimer;
	TMode iMode;
	TMode iListMode;                 // EList or EOutbox: where Esc returns from a message
	TBool iSearch;                   // the list shows search results
	TBuf<60> iSearchWords;
	TBuf8<128> iFolder;              // IMAP name of the open folder
	CArrayFixFlat<TPmFolder>* iFolders;
	CArrayFixFlat<TPmRow>* iRows;    // newest first
	TInt iSel;
	TInt iTop;
	// message view
	TUint iMsgUid;
	HBufC* iText;                    // the message file (cp1252)
	CArrayFixFlat<TInt>* iLineStart; // wrapped lines: start in iText
	CArrayFixFlat<TInt>* iLineLen;
	TInt iHeaderLines;               // wrapped lines before the body
	TInt iMsgTop;
	TBool iWaitingBody;
	TBool iAllHeaders;
	TInt iTruncated;                 // bytes not downloaded
	CDesCArrayFlat* iAttNames;
	CDesC8ArrayFlat* iAttParts;
	// engine bookkeeping
	PmCmd iSent[PM_CMDQ];
	TUint iDoneSeen;
	TUint iChangedSeen;
	TUint iBeat;
	TBuf<128> iStatus;
	TBuf<128> iLastProgress;
	TInt iBusyWas;
	TInt iEntropyPos;
	// drawing
	const CFont* iFont;
	CFont* iBold;
	TInt iLineH;
	TInt iAscent;
	TBool iShowMsg;
	TBuf<120> iMsg1;
	TBuf<120> iMsg2;
	};

class CPmInfoDialog : public CEikDialog
	{
public:
	CPmInfoDialog(const TDesC& aTitle, const TDesC* aLines, TInt aCount)
		: iTitle(aTitle), iLines(aLines), iCount(aCount) {}
private:
	void PreLayoutDynInitL();
	const TDesC& iTitle;
	const TDesC* iLines;
	TInt iCount;
	};

// one line of text: search words, a password...
class CPmTextDialog : public CEikDialog
	{
public:
	CPmTextDialog(const TDesC& aTitle, const TDesC& aPrompt, TDes& aText)
		: iTitle(aTitle), iPrompt(aPrompt), iText(aText) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	const TDesC& iTitle;
	const TDesC& iPrompt;
	TDes& iText;
	};

class CPmPasswordDialog : public CEikDialog
	{
public:
	CPmPasswordDialog(const TDesC& aPrompt, TDes& aText) : iPrompt(aPrompt), iText(aText) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	const TDesC& iPrompt;
	TDes& iText;
	};

// choose one of a list (folders, attachments, accounts)
class CPmChoiceDialog : public CEikDialog
	{
public:
	CPmChoiceDialog(const TDesC& aTitle, const TDesC& aPrompt, CDesCArray* aItems, TInt& aChoice)
		: iTitle(aTitle), iPrompt(aPrompt), iItems(aItems), iChoice(aChoice) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	const TDesC& iTitle;
	const TDesC& iPrompt;
	CDesCArray* iItems;              // the dialog's choice list takes it
	TInt& iChoice;
	};

class CPmComposeDialog : public CEikDialog
	{
public:
	CPmComposeDialog(CPmDraft& aDraft, const TDesC& aTitle) : iDraft(aDraft), iTitle(aTitle) {}
private:
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void Collect();
	void ShowAttachments();
	CPmDraft& iDraft;
	const TDesC& iTitle;
	};

class CPmAccountDialog : public CEikDialog
	{
public:
	CPmAccountDialog(PmAccount& aAccount, TInt& aStore) : iAcct(aAccount), iStore(aStore) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void SetText(TInt aId, const char* aText);
	void GetText(TInt aId, char* aText, TInt aMax);
	PmAccount& iAcct;
	TInt& iStore;
	};

class CPmConnDialog : public CEikDialog
	{
public:
	CPmConnDialog(TPmSettings& aSettings) : iSettings(aSettings) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TPmSettings& iSettings;
	};

class CPmAppUi : public CEikAppUi
	{
public:
	void ConstructL();
	~CPmAppUi();
	void SaveSettings();
private:
	void HandleCommandL(TInt aCommand);
	void DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane);
	void LoadSettings();
	TBool EditAccountL(TInt aIndex, TBool aNew);
	void ComposeL(CPmDraft* aDraft, const TDesC& aTitle);
	void NewMessageL();
	void ReplyL(TBool aAll);
	void ForwardL();
	void MoveL();
	void SaveAttachmentL();
	void SearchL();
	void SwitchAccountL();
	void DeleteAccountL();
	void AboutL();
	void AddSignature(CPmDraft& aDraft, TDes& aBody);
	CPmView* iView;
	TPmSettings iSettings;
	};

class CPmDocument : public CEikDocument
	{
public:
	CPmDocument(CEikApplication& aApp);
private:
	CEikAppUi* CreateAppUiL();
	};

class CPmApplication : public CEikApplication
	{
private:
	CApaDocument* CreateDocumentL();
	TUid AppDllUid() const;
	};

#endif
