// PWAPP.H - PsiWeb.app: the Psion side of the NetSurf web browser
//
// PsiWeb.app owns the screen, keyboard, pen, menus and settings. The browser
// itself (NetSurf) runs in psiweb.exe, started by the app, which draws pages
// into a chunk of memory shared with the app (see ../psiweb.h).

#ifndef __PWAPP_H
#define __PWAPP_H

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
#include <fbs.h>

extern "C" {
#include <psiweb.h>
}

#include <psiweb.rsg>
#include "psiweb.hrh"

const TUid KUidPsiWeb = { 0x01000A7A };

// Left() that doesn't mind a short text: EPOC's Left(n) panics (USER 22)
// when n is more than the length
inline TPtrC Clip(const TDesC& aText, TInt aMax)
	{
	return aText.Left(aMax < 0 ? 0 : aMax < aText.Length() ? aMax : aText.Length());
	}


struct TPwSettings
	{
	TInt iBaudIndex;       // 0=9600 .. 4=115200
	TInt iRtsCts;
	TInt iNetMode;         // 0 = modem "ATDT host:port", 1 = Psion TCP/IP
	TBuf<40> iPppStart;    // Psion TCP/IP: sent to the modem first (shared, see psilink.h)
	TInt iUseProxy;
	TBuf<60> iProxyHost;
	TInt iProxyPort;
	TBuf<200> iHome;
	TInt iImages;
	TInt iZoom;            // percent
	TInt iToolbar;         // View > Show toolbar
	};

class CPwView;

class CPwWatcher : public CActive
	{
public:
	CPwWatcher(CPwView& aView);
	~CPwWatcher();
	void Watch(RProcess& aProcess);
private:
	void RunL();
	void DoCancel();
	CPwView& iView;
	RProcess* iProcess;
	};

class CPwUpdateProgress;
class CPwView : public CCoeControl
	{
public:
	~CPwView();
	void ConstructL(const TRect& aRect, const TPwSettings& aSettings);
	TPwSettings& Settings() { return iSettings; }
	PwShared* Shared() { return iShared; }
	TBool EngineRunning() const { return iRunning; }
	void StartEngineL();
	void StopEngine();
	void StartBusy(const TDesC& aText);      // busy message until the engine draws
	void StartBusyCancel();
	void Command(TInt aCmd, const TDesC& aArg);
	void Key(TUint aCode, TUint aMods);
	void EngineEnded();
	void ShowMessage(const TDesC& aLine1, const TDesC& aLine2);
	void StartUpdateL();
	void OpenUrlL(const TDesC& aUrl);
	void RestartL(TBool aSamePage);
	void SetPageRectL(const TRect& aRect);
	const TDesC& UpdateLog() const { return iUpdLog; }
	TBool Updating() const { return iUpdState == PW_UPD_RUNNING; }
	void StopUpdate() { if (iShared) iShared->net.quit = 1; }   // the updater gives up and says so
private:
	static TInt StartCallback(TAny* aSelf);
	void Draw(const TRect& aRect) const;
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void HandlePointerEventL(const TPointerEvent& aEvent);
	void PushEvent(TInt aType, TInt aCode, TInt aX, TInt aY);
	void AddEntropy(TUint aValue);
	static TInt TickCallback(TAny* aSelf);
	void Tick();
	static TInt AskCallback(TAny* aSelf);    // the engine's questions (Links phase 5)
	void AskAuthL();                         // a user name and password
	void AskSaveL();                         // where to save a file PsiWeb cannot show
	void UpdateTickL();
	void UpdateLine(const TDesC& aLine);        // adds to the progress window's text
	void UpdateDialogL();                       // the progress window, then what the update came to
	void StartInstallerL();
private:
	TPwSettings iSettings;
	CFbsBitmap* iBitmap;
	RChunk iChunk;
	TBool iChunkOpen;
	PwShared* iShared;
	RProcess iProcess;
	TBool iRunning;
	CPwWatcher* iWatcher;
	CPeriodic* iTimer;
	CIdle* iStarter;
	TBuf<PW_URL_MAX> iStartUrl;
	TUint iLastFrame;
	TUint iLinkSeq;               // last link message shown (PsiShared link_seq)
	TBool iLinkBusy;              // a link note is up as a busy message
	TBool iStartBusy;             // "Starting the browser engine..." is up, until its first page loads
	TUint iStartBusyAt;           // (tick count when it went up)
	TInt iEntropyPos;
	TBuf<80> iMsg1;
	TBuf<120> iMsg2;
	TBool iShowMsg;
	TInt iUpdState;            // last PW_UPD_* seen
	TBuf<128> iUpdMsg;
	TBuf<1200> iUpdLog;        // the progress window's text (paragraphs)
	CPwUpdateProgress* iUpdDlg;   // the window, while it is up
	TFileName iUpdateFile;
	CIdle* iAsker;             // shows the engine's questions outside Tick
	TBool iAsking;             // one of its dialogs is up
	};

// A page or the proxy needs a user name and password (HTTP authentication,
// Links phase 5): the engine asks through the shared chunk (psiweb.h auth_*)
class CPwAuthDialog : public CEikDialog
	{
public:
	CPwAuthDialog(const TDesC& aWho, const TDesC& aRealm, TDes& aUser, TDes& aPass)
		: iWho(aWho), iRealm(aRealm), iUser(aUser), iPass(aPass) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	const TDesC& iWho;
	const TDesC& iRealm;
	TDes& iUser;
	TDes& iPass;
	};

// Page information: up to five lines of text (the title is the resource's)
class CPwInfoDialog : public CEikDialog
	{
public:
	// aLines: an array of TPtrC (not TBufs: a TDesC* would step by the
	// wrong size - the old Page info said "Not enough memory" for that)
	CPwInfoDialog(const TPtrC* aLines, TInt aCount)
		: iLines(aLines), iCount(aCount) {}
private:
	void PreLayoutDynInitL();
	const TPtrC* iLines;
	TInt iCount;
	};

// Help on PsiWeb: a topic list over a read-only text (pwhelp.cpp)
class CPwHelpDialog : public CEikDialog
	{
public:
	CPwHelpDialog(TInt aTopic) : iTopic(aTopic) {}
private:
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	void HandleControlStateChangeL(TInt aControlId);
	TBool OkToExitL(TInt aButtonId);
	void ShowTopicL(TInt aTopic);
	TInt iTopic;
	};

class CPwAboutDialog : public CEikDialog
	{
public:
	CPwAboutDialog(const TDesC& aStatus) : iStatus(aStatus) {}
private:
	void PreLayoutDynInitL();
	const TDesC& iStatus;
	};

// Update PsiWeb: where from (as PsiMail's)
class CPwUpdateDialog : public CEikDialog
	{
public:
	CPwUpdateDialog(TInt& aSource, TDes& aHost, TInt& aPort)
		: iSource(aSource), iHost(aHost), iPort(aPort) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TInt& iSource;
	TDes& iHost;
	TInt& iPort;
	};

// Update PsiWeb's progress window, as PsiTerm's: the engine's words line by
// line (a download count replaces the last line), Stop while it works, Close
// when it has ended
class CPwUpdateProgress : public CEikDialog
	{
public:
	CPwUpdateProgress(CPwView& aView) : iView(aView), iFinished(EFalse) {}
	void ShowL(const TDesC& aText);      // the log so far
	void FinishL(const TDesC& aText);    // the job has ended: Stop -> Close
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void SetButtonTextL(const TDesC& aText);
	CPwView& iView;
	TBool iFinished;
	};

class CPwOpenDialog : public CEikDialog
	{
public:
	CPwOpenDialog(TDes& aUrl) : iUrl(aUrl) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TDes& iUrl;
	};

class CPwConnDialog : public CEikDialog
	{
public:
	CPwConnDialog(TPwSettings& aSettings) : iSettings(aSettings) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void TestL();             // the Test button (0.74)
	TPwSettings& iSettings;
	};

class CPwPrefsDialog : public CEikDialog
	{
public:
	CPwPrefsDialog(TPwSettings& aSettings) : iSettings(aSettings) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TPwSettings& iSettings;
	};

class CPwAppUi : public CEikAppUi
	{
public:
	void ConstructL();
	~CPwAppUi();
private:
	void HandleCommandL(TInt aCommand);
	void HandleSwitchOnEventL(CCoeControl* aDestination);
	TBool ProcessCommandParametersL(TApaCommand aCommand, TFileName& aDocumentName, const TDesC8& aTail);
	void ProcessMessageL(TUid aUid, const TDesC8& aParams);
	void DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane);
	void LoadSettings(TPwSettings& aSettings);
	void SaveSettings(const TPwSettings& aSettings);
	void PageInfoL();
	void AboutL();
	void UpdateL();
	void HelpL();                      // Tools > Help on PsiWeb (pwhelp.cpp)
	void ButtonPictureL(TInt aId, TInt aIcon);
	void ToolbarPicturesL();
	void ShowToolBarL(TBool aShow);
	void RestartEngineL();
	TRect PageRect(TBool aToolbar) const;
	CPwView* iView;
	};

class CPwDocument : public CEikDocument
	{
public:
	CPwDocument(CEikApplication& aApp);
private:
	CEikAppUi* CreateAppUiL();
	};

class CPwApplication : public CEikApplication
	{
private:
	CApaDocument* CreateDocumentL();
	TUid AppDllUid() const;
	};

#endif
