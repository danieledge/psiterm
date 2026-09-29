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
	void PreLayoutDynInitL();
	TBuf<80> iStatus;
	};

// Update server (host + port)
// Read-only text window for the Debug tools' results
class CDebugDialog : public CEikDialog
	{
public:
	CDebugDialog(const TDesC& aTitle, const TDesC& aText) : iTitle(aTitle), iText(aText) {}
private:
	void PreLayoutDynInitL();
	TPtrC iTitle;
	TPtrC iText;
	};

// Baud rate, flow control, link type
class CConnDialog : public CEikDialog
	{
public:
	CConnDialog(TPsiSettings& aSettings) : iSettings(aSettings) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TPsiSettings& iSettings;
	};

class CUpdateDialog : public CEikDialog
	{
public:
	CUpdateDialog(TDes& aHost, TInt& aPort);
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TDes& iHost;
	TInt& iPort;
	};

// Add / edit one host
class CHostEditDialog : public CEikDialog
	{
public:
	CHostEditDialog(THostEntry& aEntry);
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	THostEntry& iEntry;
	};

// ---------------------------------------------------------------------------
// Terminal view: draws the libvterm screen and turns key presses into bytes
// ---------------------------------------------------------------------------
class CTermView : public CCoeControl, public MSerialObserver
	{
public:
	CTermView();
	~CTermView();
	void ConstructL(const TRect& aRect, const TPsiSettings& aSettings);

	// commands from the app UI
	void SendKey(VTermKey aKey, TInt aMod);
	void SendChar(TUint aChar);
	void SendString(const TDesC8& aText);
	void SendCtrl(TUint aLetter);
	void SendScreenSize();
	void SerialInfo();
	void HangUp();
	void ResetTerminal();
	void ApplySerialSettings();
	void SetFontL(TInt aZoom);
	void ZoomBy(TInt aStep);
	void LocalMessage(const TDesC8& aText);
	// Debug screen: output goes to a text window instead of the terminal
	void BeginDebugL(const TDesC& aTitle);
	void ShowDebugL();                 // show what was collected (if any)
	void ShowDebugIfIdleL() { if (iCapture && !iSshActive) ShowDebugL(); }
	void RunAfterDisconnectL(TInt aCommand);   // disconnect SSH, then run aCommand
	TInt Cols() const { return iCols; }
	TInt Rows() const { return iRows; }
	TPsiSettings& Settings() { return iSettings; }

	// SSH (Dropbear in psissh.exe)
	void StartSshL();
	void SetSshPassword(const TDesC& aPassword) { iSshPassword.Copy(aPassword); }
	void StartSpeedTestL();
	void StartUpdateL();
	void ScreenshotL();                 // after a short delay (menu gone)
	void SendScreenshotsL();
	static TInt ShotCallback(TAny* aSelf);
	void DisconnectSsh();
	void SshProcessEnded();
	TBool SshActive() const { return iSshActive; }
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
	short* iSbCols;
	TInt iLinesPushed;    // absolute number of the top screen row
	TInt iScrollOffset;   // 0 = live; N = viewing N lines back
	TBool iNeedFull;      // repaint everything at EndPaint

	// selection, in absolute line numbers (stable while output scrolls)
	TBool iSelActive;
	TInt iSelLine0, iSelCol0, iSelLine1, iSelCol1;
	enum { EPenNone, EPenSelect, EPenScroll } iPenMode;
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
	TInt iPendingCmd;         // run when the SSH session has ended
	CIdle* iPendingIdle;
	static TInt PendingCallback(TAny* aSelf);
	void AppendDebug(const TDesC8& aText);
	CPeriodic* iShotTimer;    // screenshot a moment after the menu closes
	void TakeScreenshotL();
	void ShotDir(TDes& aDir);
	TInt DeleteShots();
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
	TBool ConfirmDisconnectL(TInt aCommand);
	void LoadSettings(TPsiSettings& aSettings);
	void SshToL();
	TBool EditHostL(THostEntry& aEntry);
	void SaveSettings(const TPsiSettings& aSettings);
private:
	CTermView* iView;
	CHostList* iHosts;
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
