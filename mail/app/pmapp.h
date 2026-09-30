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
class CPmContacts;

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

// PsiMail.mbm read once (pmnative.cpp): its pictures are made from the
// file's bytes, which is far quicker on start-up than loading each of the
// 52 through the font and bitmap server. Anything unexpected in the file
// falls back to the ordinary load.
class CPmMbm : public CBase
	{
public:
	static CPmMbm* NewL(RFs& aFs, const TDesC& aFile);
	~CPmMbm();
	CFbsBitmap* CreateBitmapL(TInt aId);
private:
	TFileName iFile;
	HBufC8* iData;
	TInt iCount;
	const TUint32* iOffsets;
	};

class CPmView : public CCoeControl, public MPmCalObserver, public MEikListBoxObserver
	{
public:
	enum TMode { EList, EMessage, EOutbox, ENoAccount, ECalendar };
	~CPmView();
	void ConstructL(const TRect& aRect, TPmSettings& aSettings, TPmCalSettings& aCal);
	void CalendarSyncL();                    // ask the engine, then update the Agenda
	void ShowCalendarL();                    // the calendar screen
	void ToggleMonthL();                     // View > Switch view: the week or the month
	void CalendarTodayL();                   // Event > Go to today
	void NewEventL();                        // the Create new event dialog: into the Agenda (then synced)
	void EventDetailsL();                    // Event > Details: the chosen event in a dialog
	TBool EventSelected() const;             // the calendar shows a day with an event chosen
	TBool CalendarBusy() const { return iCalPending || (iCalSync && iCalSync->Running()); }
	void StoreDirectory(TDes& aDir) const { StoreDir(aDir); }
	// MPmCalObserver
	void CalProgress(const TDesC& aText);
	void CalSyncDone(TInt aError, const TDesC& aSummary, TBool aPushed);
	void FinishStartL();                     // after the first draw: the engine, the lists, the calendar
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
	TBool DownloadingAhead() const;          // the engine's own download ahead is what is running
	TBool OpInFlight(TInt aOp) const;        // running now, or queued
	void StopEngineWork(const TDesC& aToast); // Esc / Stop: net.quit
	const TPmRow* CurrentRow() const;
	TBool CurrentIsSearch() const { return iSearch; }
	const TPmFolder* CurrentFolder() const;
	const TPmFolder* CommandFolder() const;  // the folder File > Folder acts on (highlighted, or open)
	void FolderChangedL(const PmCmd& aCmd);  // a folder was made, renamed or deleted
	const TDesC8& FolderImap() const { return iFolder; }
	TInt FolderCount() const;
	const TPmFolder& FolderAt(TInt aIndex) const;
	void FocusFoldersL();
	void StartInstallerL(const TDesC& aFile);
	void SetStatus(const TDesC& aText);
	void OpenSidebarItemL(TInt aIndex);
	TInt CurrentSidebarItem() const;
	void OpenFolderL(const TDesC8& aImap);
	void ShowOutboxL();
	void Render();                           // bring the screen up to date
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
	void OpenAttachmentL(TInt aIndex);       // in its own program (Word, Sketch...), via a cached copy
	void LaunchFileL(const TDesC& aPath);    // RApaLsSession::StartDocument; says so if no program takes it
	void ForwardAttachmentsL(TUint aUid);    // download the open message's attachments, then CPmAppUi::ForwardReadyL
	TInt AttachmentCount() const;
	void AttachmentsL(CDesCArray& aNames);
	TBool MessageHeader(const TDesC& aName, TDes& aValue) const;
	TBool IndexFromL(TUint aUid, TDes& aFrom);   // a message's From as the folder index has it (before it is downloaded)
	void PlainBodyL(TDes& aOut, TBool aQuote) const;
	TBool HasHtml() const { return iHtml; }
	void ViewAsWebPageL();
	void OpenWebL(const TDesC& aUrl);
	void DraftFromOutboxL(CPmDraft& aDraft);
	void SaveDraftL(CPmDraft& aDraft, TBool aSend);
	void DeleteOutboxL();
	TInt OutboxCount();
	void Toast(const TDesC& aText);
	void ZoomL(TInt aStep);                  // the sidebar's zoom buttons
	TBool NativeMode() const;                // shown with EIKON controls (every mode now)
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
	TInt RowHeight() const;                  // a list row at this zoom
	// the calendar, drawn with EIKON's fonts and colours beside the folder
	// tree (pmcalview.cpp)
	TRect CalRect() const;                   // its pane: right of the folders, under the title band
	TRect CalHeadRect() const;               // the row of buttons at the top of the pane
	TRect CalStripRect() const;              // the week's seven days
	TRect CalDayRect() const;                // the chosen day's name
	TRect CalListRect() const;               // its events
	TInt CalRows() const;                    // event rows that fit
	TInt CalHeadButtons(TRect* aRects) const;   // where the buttons are: prev, today, next, view
	void DrawCalendar(CWindowGc& aGc) const;
	void DrawCalHead(CWindowGc& aGc) const;
	void DrawCalWeek(CWindowGc& aGc, const PmUiCalendar& k) const;
	void DrawCalMonth(CWindowGc& aGc, const PmUiCalendar& k) const;
	void DrawCalEvent(CWindowGc& aGc, const PmUiEvent& e, const TRect& aRect, TBool aSel) const;
	void MonthGrid(TRect& aNames, TRect& aGrid, TInt& aCellW, TInt& aCellH) const;
	TInt CalHit(const TPoint& aPos, TInt& aIndex) const;
	TBool CalendarPointerL(const TPointerEvent& aEvent);
	void CalendarText(TDes& aMid) const;     // the title band's middle
	void CalScrollL(TInt aMovement);         // the event list's scroll bar
public:
	void SortL(TInt aMode);                  // Tools > Sort, a column heading
	void ToggleViewL(TInt aFlag);            // View > Show toolbar / title bar / folders
	void StatusInfoL();                      // View > Status information
	void ToolbarPopupL(TInt aCommand);       // the toolbar's New and Reply/f'ward
	TInt SortMode() const { return iSettings->iSort; }
private:
	void Draw(const TRect& aRect) const;
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void HandlePointerEventL(const TPointerEvent& aEvent);
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
	void ReadFileL(const TDesC& aName, HBufC*& aBuf, TInt aMax);
	void StoreDir(TDes& aDir) const;
	void FolderDir(const TDesC8& aImap, TDes& aDir) const;
	void MsgPath(TUint aUid, const TDesC& aExt, TDes& aPath) const;
	void OutboxDir(TDes& aDir) const;
	void EnsureVisible();                    // keeps iSel within the list
	void FormatDate(TInt aDate, TDes& aOut) const;
	void CopySettingsToShared();
	TInt SidebarCount() const { return iFolders->Count() + 2; }   // + the outbox and the calendar
	// the calendar (pmcalview.cpp)
	void LoadCalendarL();
	void CalendarToday();
	void CalGoTo(TInt aDays);
	void MonthStep(TInt aDir);
	void FillCalendar(PmUiCalendar& k) const;
	void UseMenus(TBool aCalendar);          // the mail or the calendar menu bar
	TKeyResponse CalendarKeyL(TUint aCode);
private:
	TPmSettings* iSettings;
	TPmCalSettings* iCal;
	CPmCalSync* iCalSync;
	TBool iCalSecond;                // sending what the Agenda sync found
	TBuf<120> iCalMsg;
	TBool iCalPending;               // a calendar sync is with the engine
	TBool iCalAfterMail;             // Check mail: sync the calendar once the mail part has connected
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
	TBool iSearch;                   // the list shows search results
	TBuf<60> iSearchWords;
	TBuf8<128> iFolder;              // IMAP name of the open folder
	CArrayFixFlat<TPmFolder>* iFolders;
	CArrayFixFlat<TPmRow>* iRows;    // newest first
	TInt iSel;
	// message view
	TUint iMsgUid;
	HBufC* iText;                    // the message file (cp1252)
	TInt iBodyOff;                   // after the file's first line
	TBool iWaitingBody;
	TBuf<160> iBodyError;            // why the last download of it failed
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
	// the calendar
	PmCalModel iCalModel;
	TBool iCalLoaded;
	TBool iSplashDone;               // the start-up screen has gone
	TBool iStartPending;             // FinishStartL is still to run (from the first tick)
	TBool iFirstFetch;               // a new account's first fetch waits for the engine
	TBool iEngineLow;                // the engine is still starting, below the app's priority
	TInt iCalToday;                  // days since 1970
	TInt iCalNow;                    // minutes since midnight
	TInt iCalDay;                    // the day shown
	TInt iCalSel;
	TInt iCalTop;
	TBool iCalMonth;                 // the month grid instead of the week
	TBool iCalMenus;                 // the calendar's menu bar is up
	TInt iCalPress;                  // the pen on one of the pane's buttons (CalHit), 0 none
	PmUiEvent iCalEvents[40];
	PmCalText iCalText;
	PmCalText iCalText2;
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
	TBuf<160> iReaderError;
	CArrayFixFlat<TPmLinkRange>* iLinks;
	TInt iLinkSel;                   // index in iLinks, -1 none
	TInt iSplitX;                    // where the folder list ends
	TInt iStatusH;
	TUint iMsgListSum;               // what the message list shows (to skip rebuilds)
	// forwarding attachments: they are downloaded first (pmcontacts round)
	TInt iFwdPending;                // attachments still to come
	TUint iFwdUid;                   // the message they are for
	CDesCArrayFlat* iFwdFiles;       // where they landed
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

// File > Folder > New folder / Rename folder: the name, and where a new one goes
class CPmFolderDialog : public CEikDialog
	{
public:
	// aParents (NULL when renaming): the dialog's choice list takes it
	CPmFolderDialog(const TDesC& aTitle, TDes& aName, CDesCArray* aParents, TInt& aParent)
		: iTitle(aTitle), iName(aName), iParents(aParents), iParent(aParent) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TPtrC iTitle;
	TDes& iName;
	CDesCArray* iParents;
	TInt& iParent;
	TBool iHasParents;
	};

// Tools > Help on PsiMail: the topics (pmhelp.cpp) in a dialog
class CPmHelpDialog : public CEikDialog
	{
public:
	CPmHelpDialog(TInt aTopic) : iTopic(aTopic) {}
private:
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	void HandleControlStateChangeL(TInt aControlId);
	TBool OkToExitL(TInt aButtonId);
	void ShowTopicL(TInt aTopic);
	TInt iTopic;
	};

class CPmUpdateDialog : public CEikDialog
	{
public:
	CPmUpdateDialog(TInt& aSource, TDes& aHost, TInt& aPort) : iSource(aSource), iHost(aHost), iPort(aPort) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TInt& iSource;               // 0 GitHub, 1 GitHub test builds, 2 local server
	TDes& iHost;
	TInt& iPort;
	};

class CPmAboutDialog : public CEikDialog
	{
public:
	CPmAboutDialog(const TDesC& aStatus) : iStatus(aStatus) {}
private:
	void PreLayoutDynInitL();
	TPtrC iStatus;
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
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void ContactsL(TBool aComplete);          // the Contacts button (or Tab: complete what's typed)
	TInt AddressLine() const;                 // the To, Cc or Bcc line with the focus (To otherwise)
	CPmDraft& iDraft;
	TPtrC iTitle;
	TInt iRuns;                  // formatting runs in the text (Collect)
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

// Event > Create new event: what, where, when, and an alarm
struct TPmNewEvent
	{
	TBuf<180> iTitle;
	TBuf<110> iWhere;
	TTime iDate;              // midnight of the day
	TBool iAllDay;
	TTime iStart, iEnd;       // times of day (the date part is ignored)
	TInt iAlarm;              // 0 none, 1 when it starts, 2.. minutes before (see pmwrite.cpp)
	};

class CPmEventDialog : public CEikDialog
	{
public:
	CPmEventDialog(TPmNewEvent& aEvent, const TDesC& aNote) : iEvent(aEvent), iNote(aNote) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void HandleControlStateChangeL(TInt aControlId);
	void TimesDimmed();
	TPmNewEvent& iEvent;
	TPtrC iNote;
	};

// EIKON's greys on the 5mx (the workspace is white with black text; button
// faces and headings light grey; dimmed text dark grey), and the pieces the
// native screens share (pmnative.cpp)
#define KPmDarkGrey  TRgb(85, 85, 85)
#define KPmLightGrey TRgb(170, 170, 170)
void PmDrawButtonFace(CWindowGc& aGc, const TRect& aRect, TBool aDown);
void PmDrawIcon(CWindowGc& aGc, CArrayPtr<CFbsBitmap>* aIcons, TInt aId, const TPoint& aPos);
// a scroll bar as EIKON draws its own: the shaft and thumb, the up and down
// buttons at the bottom; aPress 1 up, 2 down
void PmDrawScrollBar(CWindowGc& aGc, const TRect& aRect, TInt aTotal, TInt aShown, TInt aAbove, TInt aPress);
void PmScrollBarParts(const TRect& aRect, TInt aTotal, TInt aShown, TInt aAbove, TRect& aShaft, TRect& aThumb, TRect& aUp, TRect& aDown);

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
	CCoeControl* ToolBarButton(TInt aId);          // (NULL when the toolbar is hidden)
	CPmContacts* Contacts();                       // the Psion's Contacts (pmcontacts.cpp), made when first asked for
	void MailtoL(const TDesC& aUrl);               // a mailto: link: compose, filled in
	void ForwardReadyL(TUint aUid, CDesCArray& aFiles);   // the attachments are here: compose the forward
	CPmMbm* Mbm() { return iMbm; }                 // PsiMail.mbm (NULL if it couldn't be read)
private:
	void HandleCommandL(TInt aCommand);
	void ProcessMessageL(TUid aUid, const TDesC8& aParams);           // PsiWeb's mailto: links
	TBool ProcessCommandParametersL(TApaCommand aCommand, TFileName& aDocumentName, const TDesC8& aTail);
	void AttachmentL(TBool aOpen);                 // Message > Attachments > Open / Save
	void AddSenderL();                             // Edit > Add sender to Contacts
	void DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane);
	void HandleSwitchOnEventL(CCoeControl* aDestination);
	void LoadSettings();
	TBool EditAccountL(TInt aIndex, TBool aNew);
	CPmDraft* ReplyDraftL(TBool aAll);
	CPmDraft* ForwardDraftL(CDesCArray* aFiles);
	void ComposeL(CPmDraft* aDraft, const TDesC& aTitle);
	void NewMessageL();
	void ReplyL(TBool aAll);
	void ForwardL();
	void MoveL();
	void SearchL();
	void FoldersL();
	void NewFolderL();
	void RenameFolderL();
	void DeleteFolderL();
	TBool GoOnlineL(const TDesC& aQuestion);   // offline: asks, and goes online for the command
	void HelpL(TInt aTopic = 0);
	void UpdateL();
	void SwitchAccountL();
	void DeleteAccountL();
	void AboutL();
	void AddSignature(CPmDraft& aDraft, TDes& aBody);
	void LoadCalSettings();
	void EditCalendarL();
	CPmView* iView;
	CPmContacts* iContacts;
	CPmMbm* iMbm;
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
