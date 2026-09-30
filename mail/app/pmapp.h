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
#include <fbs.h>
#include <eiklbo.h>
#include <eiksbobs.h>
#include <gdi.h>

class CEikTextListBox;
class CEikColumnListBox;
class CEikRichTextEditor;
class CEikScrollBar;
class CEikLabel;
class CPmFolderListBox;
class CPmMsgListBox;

extern "C" {
#include <psimail.h>
}
#include "pmui.h"
#include "pmcal.h"

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
	TInt iMono;            // 1 = text without anti-aliasing
	TInt iCalSync;         // calendar: sync with the Agenda at Send & receive
	TInt iPrefetch;        // download the newest N messages' text ahead: 0 = the default (10), -1 = off
	TInt iZoom;            // text size: 0 = the default (2), else 1..4
	TInt iSort;            // the message list's order: 0 newest first (see pmnative.cpp)
	TInt iView;            // what's hidden: 1 toolbar, 2 title bar, 4 folder list
	TInt iSpare[2];        // (TPmSettings is saved whole: keep its size)
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

// a link in the reader (native screens): where it is, and what it is
struct TPmLinkRange { TInt iPos; TInt iLen; TInt iLink; };

class CPmView : public CCoeControl, public MPmCalObserver, public MEikListBoxObserver
	{
public:
	enum TMode { EList, EMessage, EOutbox, ENoAccount, ECalendar, ECalEvent, ECompose, EEventEdit };
	~CPmView();
	void ConstructL(const TRect& aRect, TPmSettings& aSettings, TPmCalSettings& aCal);
	void CalendarSyncL();                    // ask the engine, then update the Agenda
	void ShowCalendarL();                    // the calendar screen
	void ToggleMonthL();
	void NewEventL();                        // straight into the Agenda (then synced)
	void ComposeL(CPmDraft* aDraft, const TDesC& aTitle);   // takes the draft
	TBool ModalCommandL(TInt aCommand);      // compose / new event: their menus
	TBool InScreenOfItsOwn() const { return iMode == ECompose || iMode == EEventEdit; }
	TBool CalendarBusy() const { return iCalPending || (iCalSync && iCalSync->Running()); }
	void StoreDirectory(TDes& aDir) const { StoreDir(aDir); }
	// MPmCalObserver
	void CalProgress(const TDesC& aText);
	void CalSyncDone(TInt aError, const TDesC& aSummary, TBool aPushed);
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
	const TPmRow* CurrentRow() const;
	TBool CurrentIsSearch() const { return iSearch; }
	const TPmFolder* CurrentFolder() const;
	const TDesC8& FolderImap() const { return iFolder; }
	TInt FolderCount() const;
	const TPmFolder& FolderAt(TInt aIndex) const;
	void FocusFoldersL();
	void SidebarPage(TInt aDir);
	void StartInstallerL(const TDesC& aFile);
	void SetStatus(const TDesC& aText);
	void OpenSidebarItemL(TInt aIndex);
	TInt CurrentSidebarItem() const;
	void OpenFolderL(const TDesC8& aImap);
	void ShowOutboxL();
	void OpenCurrentL();
	void BackL();
	void StepMessageL(TInt aDir);
	void DeleteCurrentL();
	TBool MoveCurrentL(const TDesC8& aDest);
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
	void PlainBodyL(TDes& aOut, TBool aQuote) const;
	TBool HasHtml() const { return iHtml; }
	void ViewAsWebPageL();
	void OpenWebL(const TDesC& aUrl);
	void DraftFromOutboxL(CPmDraft& aDraft);
	void SaveDraftL(CPmDraft& aDraft, TBool aSend);
	void DeleteOutboxL();
	TInt OutboxCount();
	void Toast(const TDesC& aText);
	void Render();                           // draw the screen again
	void ZoomL(TInt aStep);                  // the sidebar's zoom buttons
	TBool NativeMode() const;                // shown with EIKON controls, not drawn
	// MEikListBoxObserver
	void HandleListBoxEventL(CEikListBox* aListBox, TListBoxEvent aEventType);
private:
	// the native (EIKON) screens: the mailbox and the reader (pmnative.cpp)
	TInt CountComponentControls() const;
	CCoeControl* ComponentControl(TInt aIndex) const;
	void SizeChanged();
	void CreateNativeL();
	void DestroyNative();
	void LayoutNative();
	void ShowNative(TBool aShow);
	void ApplyZoomL();
	void UpdateNativeL();
	void UpdateFolderListL();
	void UpdateMessageListL();
	void UpdateReaderL();
	void UpdateStatusLine();
	void SyncSelectionFromLists();
	TKeyResponse NativeKeyL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void NativeLinkStepL(TInt aDir);
	void NativeActivateLinkL();
	TInt NativeLinkUrl(TInt aLink, TDes& aUrl) const;
	void DrawNative(const TRect& aRect) const;
	void DrawStatus(CWindowGc& aGc) const;
	void DrawTitle(CWindowGc& aGc) const;
	void DrawHeaders(CWindowGc& aGc) const;
	TInt HeaderHit(const TPoint& aPos) const;
	void HeaderActionL(TInt aHit);
	TBool NativePointerL(const TPointerEvent& aEvent);
	void LoadIconsL();
	void UpdateReaderBar();
	void ReaderBarModel(TInt& aTotal, TInt& aShown, TInt& aAbove) const;
	void ReaderBarParts(TRect& aShaft, TRect& aThumb, TRect& aUp, TRect& aDown) const;
	void DrawReaderBar(CWindowGc& aGc) const;
	TBool ReaderBarPointerL(const TPointerEvent& aEvent);
	void ReaderScrollL(TInt aMovement);
	void ReaderToL(TBool aEnd);
	void FormatNativeDate(TInt aDate, TDes& aOut) const;
	void SortRows();
public:
	void SortL(TInt aMode);                  // Tools > Sort, a column heading
	void ToggleViewL(TInt aFlag);            // View > Show toolbar / title bar / folders
	void StatusInfoL();                      // View > Status information
	void ToolbarPopupL(TInt aCommand);       // the toolbar's New and Reply/f'ward
	TInt SortMode() const { return iSettings->iSort; }
private:
	void Draw(const TRect& aRect) const;
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	TKeyResponse MailboxKeyL(TUint aCode);
	TKeyResponse ReaderKeyL(TUint aCode, TUint aMods);
	void HandlePointerEventL(const TPointerEvent& aEvent);
	void ActivateLinkL(TInt aLink);
	void AddEntropy(TUint aValue);
	static TInt TickCallback(TAny* aSelf);
	void Tick();
	void TickL();
	void HandleResultL(const PmCmd& aCmd);
	void HandleCalResultL(const PmCmd& aCmd, TInt aRes, const TDesC& aMsg);
	void CalCmd(const TDesC8& aArg);
	void ReloadL();
	void LoadFoldersL();
	void LoadListL();
	void LoadOutboxL();
	void LoadMessageL();
	void BuildDoc();
	void ReadFileL(const TDesC& aName, HBufC*& aBuf, TInt aMax);
	void StoreDir(TDes& aDir) const;
	void FolderDir(const TDesC8& aImap, TDes& aDir) const;
	void MsgPath(TUint aUid, const TDesC& aExt, TDes& aPath) const;
	void OutboxDir(TDes& aDir) const;
	TInt Rows() const;
	void MoveSel(TInt aDelta);
	void Scroll(TInt aDelta);
	void EnsureVisible();
	void FormatDate(TInt aDate, TDes& aOut) const;
	void CopySettingsToShared();
	void RenderMailbox();
	void RenderReader();
	TInt SidebarCount() const { return iFolders->Count() + 2; }   // + the outbox and the calendar
	void FillSidebar(PmUiMailbox& m);
	// the calendar screen (pmcalview.cpp)
	void LoadCalendarL();
	void CalendarToday();
	void CalGoTo(TInt aDays);
	void MonthStep(TInt aDir);
	void RenderCalendar();
	void RenderEvent();
	void FillCalendar(PmUiCalendar& k);
	TKeyResponse CalendarKeyL(TUint aCode);
	// writing (pmwrite.cpp)
	void UseMenus(TBool aOwn, TInt aMenuBar, TInt aHotKeys);
	void ComposeAttachmentsL();
	void ComposeCollect();
	void EndComposeL(TInt aHow);
	void ComposeAttachL();
	void ComposeRemoveAttachL(TInt aIndex);
	void FillCompose(PmUiCompose& k);
	void RenderCompose();
	TKeyResponse ComposeKeyL(TUint aCode, TUint aMods);
	void ComposePointerL(const TPoint& aPoint);
	void EventEditTexts();
	void FillEventEdit(PmUiEventEdit& k);
	void RenderEventEdit();
	void EndEventEditL(TBool aSave);
	TInt EventNextField(TInt aDir);
	void EventStep(TInt aDir);
	TKeyResponse EventEditKeyL(TUint aCode, TUint aMods);
	void EventEditPointerL(const TPoint& aPoint);
	TKeyResponse EventKeyL(TUint aCode);
	void CalendarPointerL(const TPoint& aPoint);
private:
	TPmSettings* iSettings;
	TPmCalSettings* iCal;
	CPmCalSync* iCalSync;
	TBool iCalSecond;                // sending what the Agenda sync found
	TBuf<120> iCalMsg;
	TBool iCalPending;               // a calendar sync is with the engine
	RChunk iChunk;
	TBool iChunkOpen;
	PmShared* iShared;
	RProcess iProcess;
	TBool iRunning;
	CPmWatcher* iWatcher;
	CPeriodic* iTimer;
	TMode iMode;
	TMode iListMode;                 // EList or EOutbox: where Esc returns from a message
	TBool iSidebar;                  // keys move in the folder column
	TInt iFolderSel;
	TInt iFolderTop;
	TBool iFolderFollow;
	TBool iPenSide, iPenDragged;           // the pen went down in the folder column
	TPoint iPenStart;
	TInt iPenTop;                 // bring the highlighted folder into view
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
	TInt iBodyOff;                   // after the file's first line
	PmDoc iDoc;
	TBool iDocValid;
	TInt iScroll;
	TInt iFocusLink;
	TBool iWaitingBody;
	TBool iHtml;                     // an HTML original is on the card
	TInt iTruncated;                 // bytes not downloaded
	CDesCArrayFlat* iAttNames;
	CDesCArrayFlat* iAttSizes;
	CDesC8ArrayFlat* iAttParts;
	// engine bookkeeping
	PmCmd iSent[PM_CMDQ];
	TUint iDoneSeen;
	TUint iChangedSeen;
	TBuf<128> iStatus;
	TUint iStatusUntil;              // tick count when it goes
	TBuf<128> iLastProgress;
	TUint iLinkSeq;               // link messages (dialling, looking up...) from psiglue
	TBuf<80> iLinkMsg;
	TBuf<128> iLinkProg;           // the engine's progress text when that message came
	TInt iBusyWas;
	TInt iOnlineWas;
	TBool iBusyShown;
	TBool iMsgTapArmed;              // the selected message was tapped: another tap opens it                // EIKON's busy message is up
	TInt iEntropyPos;
	TBuf<100> iToast;
	TUint iToastUntil;
	// drawing
	CFbsBitmap* iBitmap;
	TUint8* iBits;                   // 640x240, 4 bits a pixel
	PmCanvas iCanvas;
	PmUiFolder iUiFolders[84];
	// the calendar screen
	PmCalModel iCalModel;
	TBool iCalLoaded;
	TBool iSplashDone;               // the start-up screen has gone
	TInt iCalToday;                  // days since 1970
	TInt iCalNow;                    // minutes since midnight
	TInt iCalDay;                    // the day shown
	TInt iCalSel;
	TInt iCalTop;
	TBool iCalMonth;                 // the month grid instead of the week
	PmUiEvent iCalEvents[40];
	PmCalText iCalText;
	PmCalText iCalText2;
	// writing
	CPmDraft* iDraft;
	PmEditor iEd[6];                 // To, Cc, Subject, text; event name, place
	TBool iEdOpen, iEvOpen;
	TInt iCmpFocus;
	TBool iCmpChanged, iCmpDiscard;
	TMode iCmpReturn;
	TBuf<30> iCmpTitle;
	CDesCArrayFlat* iCmpNames;
	CDesCArrayFlat* iCmpSizes;
	PmUiAttachment iCmpAtt[8];
	TInt iEvDay, iEvFrom, iEvTo, iEvAlarm, iEvFocus;
	TBool iEvAllDay;
	TBuf<48> iEvDate;
	TBuf<8> iEvFromText, iEvToText;
	TBuf<30> iEvAlarmText;
	TBuf<80> iEvCal;
	PmUiRow iUiRows[12];
	TBuf<16> iDates[12];
	PmUiAttachment iUiAtt[8];
	// native screens
	CPmFolderListBox* iFolderList;
	CPmMsgListBox* iMsgList;
	CEikRichTextEditor* iReader;
	TRect iBarRect;                        // the reader's scroll bar (drawn here)
	TInt iBarPress;                        // the pen on it: 1 up, 2 down, 3 the thumb
	TInt iBarGrab;
	TInt iReaderAbove;                     // how far down the reader is, in pixels
	CArrayPtrFlat<CFbsBitmap>* iIcons;     // PsiMail.mbm: icon, mask, icon, mask... (pmicons.h)
	CArrayFixFlat<TInt>* iTree;            // the folder tree: per row, depth | icon << 4 | lines << 16
	CArrayFixFlat<TInt>* iMsgIcons;        // the message list: per row, icon | attachment << 8
	CFont* iBoldFont;                      // the column headings
	CFont* iTitleFont;                     // the title band
	TInt iTitleH;                          // the title band's height (0: hidden)
	TInt iHeadH;                           // the column headings' height
	TInt iHeadX[5];                        // where the headings start: folders, ?, from, subject, date
	TInt iPenHead;                         // the heading the pen is on (-1 none)
	CEikLabel* iStatusLine;
	CEikLabel* iEmptyLabel;          // "No messages here", the first-run text
	CFont* iListFont;
	CFont* iSmallFont;
	TZoomFactor* iZoomFactor;
	TBool iNativeShown;
	TMode iNativeMode;               // what the controls last showed
	TUint iReaderUid;                // what the reader last showed
	TBool iReaderWaiting;
	CArrayFixFlat<TPmLinkRange>* iLinks;
	TInt iLinkSel;                   // index in iLinks, -1 none
	TInt iSplitX;                    // where the folder list ends
	TInt iStatusH;
	TUint iMsgListSum;               // what the message list shows (to skip rebuilds)
	};

class CPmInfoDialog : public CEikDialog
	{
public:
	// aLines: an array of TPtrC (not TBufs: they differ in size)
	CPmInfoDialog(const TDesC& aTitle, const TPtrC* aLines, TInt aCount)
		: iTitle(aTitle), iLines(aLines), iCount(aCount) {}
private:
	void PreLayoutDynInitL();
	TPtrC iTitle;
	const TPtrC* iLines;
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
	TPtrC iTitle;
	TPtrC iPrompt;
	TDes& iText;
	};

class CPmPasswordDialog : public CEikDialog
	{
public:
	CPmPasswordDialog(const TDesC& aPrompt, TDes& aText) : iPrompt(aPrompt), iText(aText) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TPtrC iPrompt;
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
	TPtrC iTitle;
	TPtrC iPrompt;
	CDesCArray* iItems;              // the dialog's choice list takes it
	TInt& iChoice;
	};

class CPmPrefsDialog : public CEikDialog
	{
public:
	CPmPrefsDialog(TPmSettings& aSettings, TInt& aSort) : iSettings(aSettings), iSort(aSort) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TPmSettings& iSettings;
	TInt& iSort;
	};

class CPmComposeDialog : public CEikDialog
	{
public:
	CPmComposeDialog(CPmDraft& aDraft, const TDesC& aTitle) : iDraft(aDraft), iTitle(aTitle) {}
private:
	void SetSizeAndPositionL(const TSize& aSize);   // never larger than the screen
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void Collect();
	void ShowAttachments();
	CPmDraft& iDraft;
	TPtrC iTitle;
	};

class CPmAccountDialog : public CEikDialog
	{
public:
	CPmAccountDialog(PmAccount& aAccount, TInt& aStore, TInt& aPrefetch) : iAcct(aAccount), iStore(aStore), iPrefetch(aPrefetch) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void SetText(TInt aId, const char* aText);
	void GetText(TInt aId, char* aText, TInt aMax);
	PmAccount& iAcct;
	TInt& iStore;
	TInt& iPrefetch;          // messages: 0 = off
	};

class CPmConnDialog : public CEikDialog
	{
public:
	CPmConnDialog(TPmSettings& aSettings, TDes& aPppStart) : iSettings(aSettings), iPppStart(aPppStart) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TPmSettings& iSettings;
	TDes& iPppStart;          // kept in the shared PsiLink.ini, not in TPmSettings
	};

class CPmCalDialog : public CEikDialog
	{
public:
	CPmCalDialog(TPmCalSettings& aCal, const TDesC& aStoreDir) : iCal(aCal), iStoreDir(aStoreDir) {}
	~CPmCalDialog();
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TPmCalSettings& iCal;
	TPtrC iStoreDir;
	CDesC8ArrayFlat* iIds;
	};

class CPmAppUi : public CEikAppUi
	{
public:
	void ConstructL();
	~CPmAppUi();
	void SaveSettings();
	void SaveCalSettings();
	void ComposeDraftL(CPmDraft* aDraft, const TDesC& aTitle) { ComposeL(aDraft, aTitle); }
	void ShowToolBar(TBool aShow);
	void ToolbarPicturesL();
	void ButtonPictureL(TInt aId, TInt aIcon, const TDesC* aText = NULL);
	void SetTool4L(TBool aClose);
	CCoeControl* ToolBarButton(TInt aId);          // native screens have it; the drawn ones use the whole screen
private:
	void HandleCommandL(TInt aCommand);
	void DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane);
	void LoadSettings();
	TBool EditAccountL(TInt aIndex, TBool aNew);
	CPmDraft* ReplyDraftL(TBool aAll);
	CPmDraft* ForwardDraftL();
	void ComposeL(CPmDraft* aDraft, const TDesC& aTitle);
	void NewMessageL();
	void ReplyL(TBool aAll);
	void ForwardL();
	void MoveL();
	void SaveAttachmentL();
	void SearchL();
	void FoldersL();
	void UpdateL();
	void SwitchAccountL();
	void DeleteAccountL();
	void AboutL();
	void AddSignature(CPmDraft& aDraft, TDes& aBody);
	void LoadCalSettings();
	void EditCalendarL();
	CPmView* iView;
	TBool iTool4Close;                 // the last toolbar button says Close
	TPmSettings iSettings;
	TPmCalSettings iCalSettings;
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
