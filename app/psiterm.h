// PSITERM.H - PsiTerm: a modern (libvterm-based) serial terminal for the Psion 5mx

#ifndef __PSITERM_H
#define __PSITERM_H

#include <coecntrl.h>
#include <coemain.h>
#include <eikappui.h>
#include <eikapp.h>
#include <eikdoc.h>
#include <eikenv.h>
#include <eikmenup.h>
#include <eikcmds.hrh>
#include <eikmenu.hrh>
#include <c32comm.h>
#include <f32file.h>
#include <eikdialg.h>
#include <eikdialg.hrh>
#include <eikbutb.h>
#include <badesca.h>

extern "C" {
#include "psishared.h"
}

#include "vterm.h"
#include "termsb.h"

#include <psiterm.rsg>
#include "psiterm.hrh"

const TUid KUidPsiTerm = { 0x01000A77 };

// ---------------------------------------------------------------------------
// Persistent settings
// ---------------------------------------------------------------------------
struct TPsiSettings
	{
	TInt iBaudIndex;   // 0=9600 1=19200 2=38400 3=57600 4=115200
	TInt iRtsCts;      // 0=none 1=RTS/CTS
	TInt iZoom;        // 0..4, see KZoomLevels in psiterm.cpp
	TBuf<100> iSshHost;
	TBuf<60> iSshUser;
	TInt iSshPort;
	TInt iNetMode;     // 0=modem "ATDT host:port" (default) 1=Psion TCP/IP (dial-up PPP)
	TBuf<100> iUpdHost; // web server that has version.txt and PsiTerm.sis
	TInt iUpdPort;
	TInt iBold;        // 1 = draw all text bold (easier to read)
	TInt iAutoReconnect;  // 1 = redial if a logged-in session drops
	TBuf<100> iStartCmd;  // optional command run on login, e.g. tmux new -A -s psion
	TInt iUpdSource;      // 0 = GitHub (HTTPS, default), 1 = the local server iUpdHost
	// 0.35 appearance and keys
	TInt iTheme;          // 0 classic, 1 inverted (light on dark), 2 high contrast, 3 soft
	TInt iCursor;         // 0 block, 1 underline, 2 bar
	TInt iBlink;          // 1 = blinking cursor
	TInt iStatus;         // 1 = status line at the bottom
	TInt iTmuxPrefix;     // 0 = Ctrl+B, 1 = Ctrl+A
	TInt iBell;           // 0 = beep, 1 = silent
	TInt iStartScreen;    // 1 = start screen (host menu) when not connected
	TInt iTmuxTabs;       // 1 = draw tmux's window list as tabs
	TBuf<40> iPppStart;   // Psion Internet: sent to the modem first (empty = nothing)
	TInt iToolbar;        // 1 = the toolbar is showing (0.71: saved as one more byte, v13)
	};

// ---------------------------------------------------------------------------
// Snippets (C:\System\Apps\PsiTerm\Snippets.dat): named text to send, each
// optionally on a Shift+Ctrl hotkey. The text understands a few escapes:
//   \n Enter   \e Esc   \t Tab   ^X Ctrl+X   \\ backslash   \^ caret
// so a snippet can also be any key sequence (e.g. ^Bc = tmux new window).
// ---------------------------------------------------------------------------
const TInt KMaxSnippets = 20;

struct TSnippet
	{
	TBuf<24> iName;
	TBuf<120> iText;
	TInt iEnter;          // 1 = press Enter after the text
	TInt iKey;            // Shift+Ctrl hotkey: 0 none, else 'A'..'Z' or '0'..'9'
	};

class CSnippetList : public CBase
	{
public:
	static CSnippetList* NewL(RFs& aFs);
	~CSnippetList();
	void Load();              // or the defaults if there is no file yet
	TInt Save();
	TInt Count() const { return iEntries->Count(); }
	TSnippet& At(TInt aIndex) { return (*iEntries)[aIndex]; }
	void AddL(const TSnippet& aEntry) { iEntries->AppendL(aEntry); }
	void Delete(TInt aIndex) { iEntries->Delete(aIndex); }
	TInt FindKey(TInt aKey) const;   // index of the snippet on this hotkey, or -1
	TInt iLast;
private:
	CSnippetList(RFs& aFs) : iFs(aFs) {}
	void AddDefaultsL();
	RFs& iFs;
	CArrayFixFlat<TSnippet>* iEntries;
	};

// Hotkeys a snippet can use: Shift+Ctrl + a digit or a letter the menus
// don't already use (E H S T C V P are menu shortcuts)
TInt SnippetKeyCount();
TInt SnippetKeyAt(TInt aIndex);          // 0 = none
TInt SnippetKeyIndex(TInt aKey);
void SnippetKeyName(TInt aKey, TDes& aText);

// Manage snippets: pick one, Send it, or add / edit / delete
class CSnippetListDialog : public CEikDialog
	{
public:
	CSnippetListDialog(CSnippetList& aList, TInt& aIndex, TInt& aAction)
		: iList(aList), iIndex(aIndex), iAction(aAction) { iAction = 0; }
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	CSnippetList& iList;
	TInt& iIndex;
	TInt& iAction;
	};

class CSnippetEditDialog : public CEikDialog
	{
public:
	CSnippetEditDialog(TSnippet& aEntry, CSnippetList& aList, TInt aSelf)
		: iEntry(aEntry), iList(aList), iSelf(aSelf) {}
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TSnippet& iEntry;
	CSnippetList& iList;
	TInt iSelf;           // this snippet's index (-1 new): its own hotkey is not a clash
	};

// Appearance: theme, cursor, status line, bold
class CAppearanceDialog : public CEikDialog
	{
public:
	CAppearanceDialog(TPsiSettings& aSettings) : iSettings(aSettings) {}
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TPsiSettings& iSettings;
	};

// ---------------------------------------------------------------------------
// Serial port reader/writer
// ---------------------------------------------------------------------------
class MSerialObserver
	{
public:
	virtual void SerialDataL(const TDesC8& aData) = 0;
	virtual void SerialError(TInt aError) = 0;
	};

class CSerialPort : public CActive
	{
public:
	static CSerialPort* NewL(MSerialObserver& aObserver);
	~CSerialPort();
	TInt Open(TInt aBaudIndex, TBool aRtsCts);
	void Close();
	TInt Write(const TDesC8& aData);
	TBool IsOpen() const { return iOpen; }
	TUint Signals() { return iOpen ? iComm.Signals() : 0; }
	void Probe(TDes8& aOut);
private:
	CSerialPort(MSerialObserver& aObserver);
	void ReadFirst();
	void ReadMore();
	void RunL();
	void DoCancel();
private:
	enum TState { EIdle, EWaitFirst, EWaitMore };
	MSerialObserver& iObserver;
	RCommServ iServer;
	RComm iComm;
	TBool iServerOpen;
	TBool iOpen;
	TState iState;
	TInt iErrorCount;
	TBuf8<1> iFirst;
	TBuf8<1024> iBuf;
	};

// Watches psissh.exe and tells the view when it exits
class CTermView;
class CSshWatcher : public CActive
	{
public:
	CSshWatcher(CTermView& aView);
	~CSshWatcher();
	void Watch(RProcess& aProcess);
private:
	void RunL();
	void DoCancel();
	CTermView& iView;
	RProcess* iProcess;
	};

// ---------------------------------------------------------------------------
// Saved SSH hosts (C:\System\Apps\PsiTerm\Hosts.dat)
// Passwords are scrambled with a key tied to this Psion, which stops casual
// reading of the file but is NOT strong protection: anyone holding the
// Psion can use the saved logins.
// ---------------------------------------------------------------------------
const TInt KMaxHosts = 16;

struct THostEntry
	{
	TBuf<24> iName;
	TBuf<100> iHost;
	TBuf<60> iUser;
	TBuf<63> iPassword;   // empty = ask when connecting
	TInt iPort;
	TBuf<100> iCommand;   // run on login, e.g. tmux new -A -s psion (optional)
	TInt iAuth;           // log in with: 0 = SSH key, 1 = password (ask), 2 = saved password,
	                      // 3 = SSH key, then the saved password
	TInt iKeyId;          // which key (TSshKey::iId); 0 = the first one
	};

// ---------------------------------------------------------------------------
// SSH keys: named, each stored as Keys\k<id>.key/.pub/.fp (made by psissh)
// ---------------------------------------------------------------------------
const TInt KMaxKeys = 10;

struct TSshKey
	{
	TInt iId;
	TBuf<24> iName;
	};

class CKeyList : public CBase
	{
public:
	static CKeyList* NewL(RFs& aFs);
	~CKeyList();
	void Load();
	TInt Save();
	TInt Count() const { return iEntries->Count(); }
	TSshKey& At(TInt aIndex) { return (*iEntries)[aIndex]; }
	TInt Find(TInt aId) const;                     // index, or -1
	TInt AddL(const TDesC& aName);                 // returns the new key's id
	void Delete(TInt aIndex);                      // and its files
	static void Base(TInt aId, TDes& aBase);       // C:\...\Keys\k<id>
	TBool HasFile(TInt aIndex);                    // its .key is there
	void Fingerprint(TInt aIndex, TDes& aFp);      // "SHA256:..." or empty
	void MigrateL();                               // 0.44-0.49's single key
	TInt iLast;
private:
	CKeyList(RFs& aFs) : iFs(aFs) {}
	RFs& iFs;
	CArrayFixFlat<TSshKey>* iEntries;
	};

class CKeyListDialog : public CEikDialog
	{
public:
	CKeyListDialog(CKeyList& aKeys, TInt& aIndex, TInt& aAction)
		: iKeys(aKeys), iIndex(aIndex), iAction(aAction) { iAction = 0; }
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	CKeyList& iKeys;
	TInt& iIndex;
	TInt& iAction;
	};

// New key (name), Import (name + file) or Edit (name + regenerate)
class CKeyEditDialog : public CEikDialog
	{
public:
	CKeyEditDialog(TDes& aName, TDes* aFile, TInt* aRegen)
		: iName(aName), iFile(aFile), iRegen(aRegen) {}
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TDes& iName;
	TDes* iFile;
	TInt* iRegen;
	};

class CKeyPickDialog : public CEikDialog
	{
public:
	CKeyPickDialog(CKeyList& aKeys, TInt& aIndex) : iKeys(aKeys), iIndex(aIndex) {}
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	CKeyList& iKeys;
	TInt& iIndex;
	};

class CHostList : public CBase
	{
public:
	static CHostList* NewL(RFs& aFs);
	~CHostList();
	void Load();
	TInt Save();
	TInt Count() const { return iEntries->Count(); }
	THostEntry& At(TInt aIndex) { return (*iEntries)[aIndex]; }
	void AddL(const THostEntry& aEntry);
	void Delete(TInt aIndex);
	TInt iLast;           // index of the last host used
private:
	CHostList(RFs& aFs) : iFs(aFs) {}
	TUint32 KeyFor(TUint32 aSalt) const;
	RFs& iFs;
	CArrayFixFlat<THostEntry>* iEntries;
	};

// "SSH to" list: choose a saved host, or add / edit / delete one
class CHostListDialog : public CEikDialog
	{
public:
	CHostListDialog(CHostList& aHosts, TInt& aIndex, TInt& aAction);
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	CHostList& iHosts;
	TInt& iIndex;
	TInt& iAction;
	};

// About box: static credits from the resource plus one line of live status
class CAboutDialog : public CEikDialog
	{
public:
	CAboutDialog(const TDesC& aStatus);
private:
	void SetSizeAndPositionL(const TSize& aSize);   // never larger than the screen
	void PreLayoutDynInitL();
	TBuf<80> iStatus;
	};

// Update server (host + port)
// Window for updates and the Debug tools: live output while the job runs
// (with a Stop button), then the full result (with Close)
class CTermView;
class CToolDialog : public CEikDialog
	{
public:
	CToolDialog(CTermView& aView, const TDesC& aTitle, TBool aFinished)
		: iView(aView), iTitle(aTitle), iFinished(aFinished), iLaidOut(EFalse) {}
	void RefreshL();                  // show the latest output
	void FinishL();                   // the job has ended: Stop -> Close
	void CloseL() { TryExitL(EEikBidOk); }
private:
	void SetSizeAndPositionL(const TSize& aSize);   // never larger than the screen
	void SetButtonTextL(const TDesC& aText);
	void PostLayoutDynInitL();
	TBool iLaidOut;
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	CTermView& iView;
	TBuf<40> iTitle;
	TBool iFinished;
	};

// Baud rate, flow control, link type
class CConnDialog : public CEikDialog
	{
public:
	CConnDialog(TPsiSettings& aSettings, CTermView* aView = 0) : iSettings(aSettings), iView(aView) {}
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void TestL();                      // the Test button (0.74)
	TPsiSettings& iSettings;
	CTermView* iView;                  // its terminal holds the port when SSH is not running
	};

// Help on PsiTerm: a topic list over a read-only text (pthelp.cpp)
class CPtHelpDialog : public CEikDialog
	{
public:
	CPtHelpDialog(TInt aTopic) : iTopic(aTopic) {}
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	void HandleControlStateChangeL(TInt aControlId);
	TBool OkToExitL(TInt aButtonId);
	void ShowTopicL(TInt aTopic);
	TInt iTopic;
	};

class CUpdateDialog : public CEikDialog
	{
public:
	CUpdateDialog(TInt& aSource, TDes& aHost, TInt& aPort, TBool aNeedHost)
		: iSource(aSource), iHost(aHost), iPort(aPort), iNeedHost(aNeedHost) {}
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TInt& iSource;
	TDes& iHost;
	TInt& iPort;
	TBool iNeedHost;      // screenshots always need the local server
	};

// Add / edit one host
class CHostEditDialog : public CEikDialog
	{
public:
	CHostEditDialog(THostEntry& aEntry, CKeyList& aKeys);
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	THostEntry& iEntry;
	CKeyList& iKeys;
	TInt iOptCount;
	TInt iOptAuth[2 * KMaxKeys + 2];
	TInt iOptKey[2 * KMaxKeys + 2];
	};

class CPtLog;                     // the session log (ptxfer.h)
class CPtXferMemory;

// ---------------------------------------------------------------------------
// Terminal view: draws the libvterm screen and turns key presses into bytes
// ---------------------------------------------------------------------------
class CTermView : public CCoeControl, public MSerialObserver
	{
public:
	// 0.74: file transfer and the session log (ptxfer.cpp)
	PsiShared* XferShared() const { return SshLoggedIn() ? iShared : NULL; }
	CPtLog* iLog;                 // NULL until File > Log to file... is first used
	CPtXferMemory* iXferMem;      // NULL until a transfer is first used

	struct TTmuxTab { TInt iIndex; TBuf<20> iName; TBool iCurrent; TInt iX0; TInt iX1; };
	enum { KMaxTabs = 20 };
	CTermView();
	~CTermView();
	void ConstructL(const TRect& aRect, const TPsiSettings& aSettings);

	// commands from the app UI
	void SendKey(VTermKey aKey, TInt aMod);
	void SendChar(TUint aChar);
	void SendString(const TDesC8& aText);
	void SendCtrl(TUint aLetter);
	void SendSnippetText(const TDesC& aText, TBool aEnter);   // with \n ^X etc.
	void SetSnippets(CSnippetList* aSnippets) { iSnippets = aSnippets; }
	void SetHosts(CHostList* aHosts) { iHosts = aHosts; }
	void ApplyAppearanceL();           // theme / cursor / status line changed
	void ShowWelcome();                // the start screen with the saved hosts
	void SendScreenSize();
	void SerialInfo();
	void HangUp();
	void ResetTerminal();
	void ApplySerialSettings();
	void SerialClose() { if (iSerial) iSerial->Close(); }   // Connection settings > Test
	void SetFontL(TInt aZoom);
	void ZoomBy(TInt aStep);
	void SetTermRectL(const TRect& aRect);   // the toolbar came or went: lay out again
	void SizeAtZoom(TInt aZoom, TInt& aCols, TInt& aRows) const;   // for the Font menu
	void LocalMessage(const TDesC8& aText);
	// Debug screen: output goes to a text window instead of the terminal
	void BeginDebugL(const TDesC& aTitle);
	void ShowDebugL() { RunToolDialogL(); }
	void RunToolDialogL();             // the window for the job begun with BeginDebugL
	HBufC* DebugTextLC() const;        // collected output as editor text
	void StopTool();
	TBool InstallPending() const { return iInstallPending; }
	void RunAfterDisconnectL(TInt aCommand);   // disconnect SSH, then run aCommand
	TInt Cols() const { return iCols; }
	TInt Rows() const { return iRows; }
	TPsiSettings& Settings() { return iSettings; }

	// SSH (Dropbear in psissh.exe)
	void StartSshL();
	void SetSshPassword(const TDesC& aPassword) { iSshPassword.Copy(aPassword); }
	void SetLoginCommand(const TDesC& aCommand) { iLoginCmd.Copy(aCommand); }
	void SetKeyBase(const TDesC& aBase) { iKeyBase.Copy(aBase); }
	void StartSpeedTestL();
	void StartUpdateL();
	void ScreenshotL();                 // after a short delay (menu gone)
	void SendScreenshotsL();
	static TInt ShotCallback(TAny* aSelf);
	void DisconnectSsh();
	void SshProcessEnded();
	TBool SshActive() const { return iSshActive; }
	TBool ReconnectWaiting() const { return iReconnectWait; }
	// (0.68) the Psion was switched back on: psissh checks the link is still there
	void LinkSwitchedOn() { if (iSshActive && iShared) iShared->switch_on++; }
	TBool ModemOnline() const;
	TBool SshLoggedIn() const;
	TBool InTmux() const { return iTabRow >= 0; }
	void TmuxNextWindow(TBool aBack);
	TBool KeyToolL(TInt aMode, const TDesC& aBase, const TDesC& aName, const TDesC& aSrc);
	void ShowKeyL(const TDesC& aBase, const TDesC& aName);
	const TDesC& KeyBase() const { return iKeyBase; }
	void ParseTmuxTabs();
	static TInt ParseTabList(const TDesC& aText, TTmuxTab* aTabs, TInt& aCurrent);
	TBool RowIsBar(TInt aRow) const;
	void CheckTabsL();              // Debug: what the tab reader sees
	void DrawTabs(CWindowGc& aGc) const;
	void SelectTmuxWindow(TInt aIndex);           // make the key (psissh mode 4)
	void InstallLoginKeyL(const TDesC& aBase);   // type the authorized_keys command into the session
	static TInt PumpCallback(TAny* aSelf);

	// from CCoeControl
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);

	// from MSerialObserver
	void SerialDataL(const TDesC8& aData);
	void SerialError(TInt aError);

	// libvterm callbacks (C linkage is not needed with GCC; plain statics work)
	static int CbDamage(VTermRect aRect, void* aUser);
	static int CbMoveRect(VTermRect aDest, VTermRect aSrc, void* aUser);
	static int CbPushLine(int aCols, const VTermScreenCell* aCells, void* aUser);
	static int CbClearScrollback(void* aUser);

	// scrollback and pen selection
	void HandlePointerEventL(const TPointerEvent& aPointerEvent);
	void ScrollBy(TInt aLines);
	void ScrollTo(TInt aOffset);
	void CopySelectionL();
	void PasteL();
	void SelectScreen();
	void ShowCharInfoL();
	static int CbMoveCursor(VTermPos aPos, VTermPos aOldPos, int aVisible, void* aUser);
	static int CbSetTermProp(VTermProp aProp, VTermValue* aVal, void* aUser);
	static int CbBell(void* aUser);
	static void CbOutput(const char* aBytes, size_t aLen, void* aUser);

private:
	void Draw(const TRect& aRect) const;
	void DrawCells(CWindowGc& aGc, TInt aRow0, TInt aCol0, TInt aRow1, TInt aCol1) const;
	// what one cell on the view looks like (live screen or scrollback)
	struct TLook
		{
		TUint iCh;
		TInt iWidth;
		TInt iFg;
		TInt iBg;
		TUint iFlags;     // KSbBold | KSbUnderline | KSbStrike
		};
	TBool GetLook(TInt aRow, TInt aCol, TLook& aLook) const;
	void DrawOneCell(CWindowGc& aGc, TInt aRow, TInt aCol) const;
	void DrawOneCell(CWindowGc& aGc, TInt aRow, TInt aCol, const TLook& aLook) const;
	void DrawAll(CWindowGc& aGc) const;
	void DrawScrollIndicator(CWindowGc& aGc) const;
	void CellColours(const VTermScreenCell& aCell, TInt& aFg, TInt& aBg) const;
	TInt TextByte(const TLook& aLook) const;
	TInt ViewLine(TInt aRow) const { return iLinesPushed - iScrollOffset + aRow; }
	void RepaintRows(TInt aRow0, TInt aRow1);
	void ClearSelection();
	static int SelLineFn(void* aCtx, int aLine, unsigned int* aChars, int aMaxCols);
	static int SelMapFn(unsigned int aCh);
	void BeginPaint();
	void EndPaint();
	void DrawCursor(CWindowGc& aGc) const;
	void AddDamage(TInt aRow0, TInt aCol0, TInt aRow1, TInt aCol1);
	void FeedTerminal(const TUint8* aBytes, TInt aLen);
	void ReleaseFont();
	void WriteToHost(const TDesC8& aBytes);
	void LaunchSshL(TInt aMode = 0);
	void PumpSsh();
	void AddKeyEntropy(TUint aCode);
	TBool SeedFileExists();

private:
	TPsiSettings iSettings;
	CSerialPort* iSerial;
	VTerm* iVt;
	VTermScreen* iScreen;
	VTermScreenCallbacks iCallbacks;

	CFont* iFont;
	TInt iCellW;
	TInt iCellH;
	TInt iAscent;
	TInt iCols;
	TInt iRows;
	TInt iOriginX;
	TInt iOriginY;
	TBool iMono;          // every glyph is exactly iCellW wide: draw whole runs
	TInt iFontFileId;     // PsiTerm.gdr (Spleen) added to the font server
	TBool iFontFileLoaded;
	CWindowGc* iPaintGc;  // non-NULL while BeginPaint..EndPaint is active

	// scrollback: lines pushed off the top of the screen
	TTermSb iSb;
	TSbCell* iSbCells;
	CSnippetList* iSnippets;  // owned by the app UI; hotkeys look here
	CHostList* iHosts;        // owned by the app UI; the welcome screen lists them
	// appearance
	TInt Theme(TInt aGrey) const;                   // theme mapping of a grey 0..15
	TRgb Grey(TInt aGrey) const { return TRgb::Gray16(Theme(aGrey)); }
	void ThemePair(TInt aFg, TInt aBg, TInt& aThemedFg, TInt& aThemedBg) const;
	TInt iStatusH;            // status line height in pixels (0 = off)
	CPeriodic* iTick;         // 0.5 s: clock, status line, cursor blink
	TBool iBlinkHidden;       // cursor in the "off" half of a blink
	TBuf<120> iStatusDrawn;   // what the status line shows now
	TTime iReconnectAt;       // when the next reconnect attempt starts
	TBool iWelcome;           // the start screen is showing: 1-9 connect
	void StatusText(TDes& aText, TInt& aSplit) const;
	void DrawStatus(CWindowGc& aGc) const;
	void Tick();
	static TInt TickCallback(TAny* aSelf);
	void StartTick();
	void SyncToolbar();       // the first toolbar button: SSH to..., or Disconnect
	TInt iTbBusy;             // what it shows now (-1 = not yet set)
	short* iSbCols;
	TInt iLinesPushed;    // absolute number of the top screen row
	TInt iScrollOffset;   // 0 = live; N = viewing N lines back
	TBool iNeedFull;      // repaint everything at EndPaint

	// selection, in absolute line numbers (stable while output scrolls)
	TBool iSelActive;
	TInt iSelLine0, iSelCol0, iSelLine1, iSelCol1;
	enum { EPenNone, EPenSelect, EPenScroll, EPenMouse } iPenMode;
	TInt iMouseMode;
	// tmux windows as tabs (read from tmux's status line)
	TTmuxTab iTabs[KMaxTabs];
	TInt iTabCount;
	TInt iTabRow;             // the screen row tmux's status line is on; -1 = no tmux seen
	TBool iTabsDrawn;         // PsiTerm draws its tab bar over that row
	TBuf<320> iTabSig;        // what was parsed last, to notice changes
	TBool iPenOnTabs;
	TBool iTabsTop;           // the tab strip is up, at the top (tmux's bar row hidden)
	TInt iOriginY0;           // the top of the terminal (and of the strip, when up)
	void SetTabsTop(TBool aTop);
	// 0.75: the strip is drawn as EIKON's dialog page tabs (pttabs.cpp). It
	// sits over the terminal and takes the place of tmux's status line, which
	// is hidden under it: the terminal has room for (height - strip) / cell
	// rows plus that one, and the SSH window size says so.
	TInt iTabStripH;          // its height in pixels (0 = no strip)
	TInt iTabHideRow;         // tmux's bar row (the last or the first)
	TBool iTabMsg;            // the bar is showing something else (a tmux prompt or
	                          // message): the strip shows that row as text instead
	TInt iTabMiss;            // reads in a row without a tab list, while the strip is up
	enum { KTabMissLimit = 6 };   // that many (3 s of ticks): tmux has gone, the strip goes
	TInt iTabFirst;           // first tab shown when they don't all fit
	TInt iTabLastCur;         // the current window when they were last laid out
	TInt iTabArrowL, iTabArrowR;   // the scroll arrows' left edges (-1 = none)
	void Layout();            // rows, columns and origin for the font and the strip
	TInt TabStripHeight() const;
	TInt RowY(TInt aRow) const;     // the pixel row a terminal row is drawn at
	TBool RowHidden(TInt aRow) const { return iTabsTop && !iTabMsg && aRow == iTabHideRow; }
	void LayoutTabs(const CFont& aFont, TInt aLeft, TInt aRight);
	TBool TabPenDownL(TInt aX);
	TBuf8<240> iTitle;         // the terminal title as it arrives (OSC 0/2)
	TTmuxTab iTitleTabs[KMaxTabs];   // tmux's window list from a PSITABS title
	TInt iTitleTabCount;
	TInt iLastState;          // connection state last tick, and since when
	TUint iStateSince;
	TUint iTickCount;
	TBool iEverLoggedIn;      // this session got as far as a login          // VTERM_PROP_MOUSE_*: the program wants mouse events
	TInt iPenRow0, iPenCol0;
	TBool iPenWheeled;
	TInt iPenY0;
	TInt iPenOffset0;

	// colour cache for GetLook (colours repeat along a line)
	VTermColor iCacheFg, iCacheBg;
	TInt iCacheRev, iCacheF, iCacheB;
	TBool iCacheValid;

	TBool iDamaged;
	TInt iDmgRow0, iDmgCol0, iDmgRow1, iDmgCol1;   // row1/col1 exclusive

	TInt iCurRow;
	TInt iCurCol;
	TBool iCurVisible;
	TInt iDrawnCurRow;
	TInt iDrawnCurCol;
	TBool iDrawnCurVisible;

	// SSH session
	TBool iSshActive;
	TBool iGatheringEntropy;
	TInt iKeyCount;
	RChunk iChunk;
	TBool iChunkOpen;
	PsiShared* iShared;
	RProcess iSshProcess;
	CSshWatcher* iWatcher;
	CPeriodic* iPump;
	TUint8 iEntropy[PSI_ENTROPY_SIZE];
	TInt iEntropyPos;
	TInt iEntropyFill;
	TInt iLaunchMode;         // mode of the running psissh (0 SSH, 1 speed test, 2 update)
	TFileName iUpdateFile;    // where the update SIS is saved
	HBufC8* iDebugText;       // collected Debug tool output
	TBuf<40> iDebugTitle;
	TBool iCapture;           // LocalMessage/psissh output -> iDebugText
	CToolDialog* iToolDlg;    // open while a job runs
	TBool iInstallPending;    // update downloaded: start the installer when the window closes
	void StartInstallerL();
	TInt iPendingCmd;         // run when the SSH session has ended
	CIdle* iPendingIdle;
	static TInt PendingCallback(TAny* aSelf);
	void AppendDebug(const TDesC8& aText);
	CPeriodic* iShotTimer;    // screenshot a moment after the menu closes
	void TakeScreenshotL();
	void ShotDir(TDes& aDir);
	TInt DeleteShots();
	TInt iEntropyMode;        // what to launch once the randomness is gathered
	TBool iModemOnline;       // the terminal saw CONNECT (and no NO CARRIER since)
	TUint iLastRx;            // tick of the last serial data outside SSH
	TUint iLastTx;            // tick of the first key typed since the last reply (0 = none)
	TBool iNoReplyShown;      // the no-reply hint was shown for this burst
	TUint iLastSerialErr;     // tick of the last write-error message
	TBuf8<16> iRxTail;        // end of the last serial data, for split words
	TUint iLastBell;          // tick of the last beep
	TBuf<96> iKeyBase;        // the key offered to this host (base name), or empty
	TInt iPendKeyMode;        // key tool waiting for randomness: 4 make, 5 import
	TBuf<96> iPendKeyBase;
	TBuf<24> iPendKeyName;
	TBuf<96> iPendKeySrc;
	TBuf<100> iLoginCmd;      // the connected host's command on login
	TBuf8<64> iSshPassword;   // saved password for the next launch, then wiped
	// auto-reconnect
	TBuf8<64> iReconnectPw;   // the saved password of this session (RAM only)
	TBool iUserQuit;          // Disconnect was chosen: don't reconnect
	TBool iReconnecting;      // the current/next launch is a reconnect
	TBool iReconnectWait;     // counting down to the next attempt
	TInt iReconnectTries;
	CPeriodic* iReconnectTimer;
	void ScheduleReconnect();
	void CancelReconnect(const TDesC8& aWhy);
	void ReconnectNowL();
	static TInt ReconnectCallback(TAny* aSelf);
	};

// ---------------------------------------------------------------------------
// EIKON application framework classes
// ---------------------------------------------------------------------------
class CPsiTermAppUi : public CEikAppUi
	{
public:
	void ConstructL();
	~CPsiTermAppUi();
private:
	void HandleCommandL(TInt aCommand);
	void DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane);
	void HandleSwitchOnEventL(CCoeControl* aDestination);
	TBool ConfirmDisconnectL(TInt aCommand);
	void LoadSettings(TPsiSettings& aSettings);
	void SshToL();
	TBool EditHostL(THostEntry& aEntry);
	void SaveSettings(const TPsiSettings& aSettings);
public:
	// the toolbar (View > Show toolbar hides it: the terminal takes its room)
	void ShowToolBarL(TBool aShow);
	void SetConnectButton(TBool aBusy);     // SSH to... <-> Disconnect
	TBool ToolbarShown() const;
private:
	void ToolbarPicturesL();
	void ButtonPictureL(TInt aId, TInt aIcon, const TDesC* aText = NULL);
	TRect TermRect(TBool aToolbar) const;
	void ToolbarPopupL(TInt aCommand);
	CTermView* iView;
	CHostList* iHosts;
	CSnippetList* iSnippets;
	CKeyList* iKeys;
	void ManageSnippetsL();
	void ManageKeysL();
	void InstallKeyCmdL();
	void HelpL();                      // Tools > Help on PsiTerm (pthelp.cpp)
	void ConnectHostL(TInt aIndex);
	TBool EditSnippetL(TSnippet& aEntry, TInt aSelf);
	void SendTmux(TUint aKey);
	};

class CPsiTermDocument : public CEikDocument
	{
public:
	CPsiTermDocument(CEikApplication& aApp);
private:
	CEikAppUi* CreateAppUiL();
	};

class CPsiTermApplication : public CEikApplication
	{
private:
	CApaDocument* CreateDocumentL();
	TUid AppDllUid() const;
	};

// Unicode -> Psion glyph helpers (glyphs.cpp)
TInt PsiMapToCodePage(TUint aCodePoint);         // returns 0..255 or -1
TUint PsiCodePageToUnicode(TUint aByte);          // cp1252 byte -> Unicode
TInt PsiBoxSegments(TUint aCodePoint);            // bit mask of line segments, or -1

// segment bits returned by PsiBoxSegments
const TInt KBoxLeft  = 1;
const TInt KBoxRight = 2;
const TInt KBoxUp    = 4;
const TInt KBoxDown  = 8;
const TInt KBoxHeavy = 16;
const TInt KBoxDouble = 32;

#endif
