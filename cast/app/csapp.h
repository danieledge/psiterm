// CSAPP.H - PsiCast.app: shares the Psion's screen with a web browser
// through the Atom modem.
//
// The app is small: one view saying how the sharing is going, the commands,
// and the settings. The work - reading the screen, encoding it, sending it,
// injecting the viewer's taps and keys - is psicast.exe's, through the
// chunk in ../engine/psicast.h. The engine runs only while the screen is
// shared, and may go on after PsiCast has closed (Preferences); a PsiCast
// opened again finds it and takes it up.

#ifndef __CSAPP_H
#define __CSAPP_H

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
#include <eikkeys.h>
#include <badesca.h>

extern "C" {
#include <psicast.h>
}
#include "psibell.h"
#include <psicast.rsg>
#include "psicast.hrh"

class CCsAppUi;

const TUid KUidPsiCast = { 0x01000A92 };

// Left() that doesn't mind a short text: EPOC's Left(n) panics (USER 22)
// when n is more than the length
inline TPtrC8 Clip8(const TDesC8& aText, TInt aMax)
	{
	return aText.Left(aMax < 0 ? 0 : aMax < aText.Length() ? aMax : aText.Length());
	}

// a C string from the chunk (always NUL-terminated by its writer), at most aMax long
inline TPtrC8 FromC(const char* aText, TInt aMax)
	{
	TInt n = 0;
	while (n < aMax && aText[n])
		n++;
	return TPtrC8((const TUint8*)aText, n);
	}

// PsiCast.ini: a versioned stream - a version, a count, then that many
// 32-bit fields. New settings are added at the end; an older file simply
// has fewer, and the rest keep their standard values.
struct TCsSettings
	{
	TInt iToolbar;                 // View > Show toolbar
	TInt iZoom;                    // 0 small, 1 medium, 2 large
	TInt iRate;                    // Highest update rate: 0 2, 1 5, 2 10, 3 20 a second
	TInt iControl;                 // Allow control from the viewer
	TInt iBatteryStop;             // Stop sharing when the battery is low
	TInt iKeepOnClose;             // When PsiCast closes: 0 stop sharing, 1 keep sharing
	};
const TInt KCsSettingsFields = 6;
const TInt KCsSettingsVersion = 1;

// ----- the view ---------------------------------------------------------------

class CCsView : public CCoeControl
	{
public:
	CCsView(CCsAppUi& aUi) : iUi(aUi) {}
	~CCsView();
	void ConstructL(const TRect& aRect);
	void ApplyZoomL();
	void SetViewRectL(const TRect& aRect);
private:
	void Draw(const TRect& aRect) const;
	void DrawLine(CWindowGc& aGc, TInt aY, const TDesC& aLabel, const TDesC& aText) const;
	CFont* FontL(TInt aTwips, TBool aBold, CFont*& aOld);
	CCsAppUi& iUi;
	CFont* iBig;                   // the state: Sharing, Not sharing...
	CFont* iLabel;                 // the lines' labels (bold)
	CFont* iText;                  // ...and what they say
	CFont* iSmall;                 // the link's words, at the bottom
	};

// ----- dialogs -----------------------------------------------------------------

class CCsPrefsDialog : public CEikDialog
	{
public:
	CCsPrefsDialog(TCsSettings& aSettings) : iSettings(aSettings) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	TCsSettings& iSettings;
	};

class CCsConnDialog : public CEikDialog
	{
private:
	void PreLayoutDynInitL();
	void HandleControlStateChangeL(TInt aControlId);
	TBool OkToExitL(TInt aButtonId);
	void DimKeepUpLine();
	};

class CCsAboutDialog : public CEikDialog
	{
public:
	CCsAboutDialog(const TDesC& aStatus) : iStatus(aStatus) {}
private:
	void PreLayoutDynInitL();
	const TDesC& iStatus;
	};

class CCsHelpDialog : public CEikDialog
	{
private:
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	void HandleControlStateChangeL(TInt aControlId);
	void ShowTopicL(TInt aTopic);
	TInt iTopic;
	};

// ----- the engine's process -------------------------------------------------------

class CCsWatcher : public CActive
	{
public:
	CCsWatcher(CCsAppUi& aUi);
	~CCsWatcher();
	void Watch(RProcess& aProcess);
private:
	void RunL();
	void DoCancel();
	CCsAppUi& iUi;
	RProcess* iProcess;
	};

// ----- the app -----------------------------------------------------------------

class CCsAppUi : public CEikAppUi
	{
public:
	~CCsAppUi();
	void ConstructL();
	const TCsSettings& Settings() const { return iSettings; }
	TBool Sharing() const { return iEngineOn; }
	TBool Paused() const { return iPaused; }
	const CsShared* Shared() const { return iEngineOn ? iShared : NULL; }
	void StateText(TDes& aText) const;      // Sharing, Not sharing, Paused...
	void EngineEnded();
	void HandleCommandL(TInt aCommand);
private:
	void DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane);
	void HandleSwitchOnEventL(CCoeControl* aDestination);
	void HandleWsEventL(const TWsEvent& aEvent, CCoeControl* aDestination);

	// the engine
	TBool ReattachL();
	void StartL();
	void Stop(TBool aSay);
	void Poke();
	void PassSettings();
	void Detach();

	// the timer
	static TInt TickCallback(TAny* aSelf);
	void Tick();
	void SetTickRate(TBool aFast);

	void PauseL(TBool aPause);
	void ControlL(TBool aAllow);
	void PreviousProgram();
	void PrefsL();
	void ConnSettingsL();
	void AboutL();
	void ZoomL(TInt aStep);
	void ShowToolBarL(TBool aShow);
	TRect ViewRect(TBool aToolbar) const;
	void ButtonPictureL(TInt aId, TInt aIcon);
	void ToolbarPicturesL();
	void ButtonStates();
	void CapturePauseKey(TBool aOn);
	void Say(const TDesC& aText);           // an infoprint, wherever PsiCast is
	void LoadSettings();
	void SaveSettings();

	TCsSettings iSettings;
	CCsView* iView;
	CPeriodic* iTimer;
	TBool iFast;
	RChunk iChunk;
	TBool iChunkOpen;
	CsShared* iShared;
	RProcess iProcess;
	TBool iEngineOn;
	CCsWatcher* iWatcher;
	TPsiBellRinger iRinger;
	TBool iPaused;
	TInt iLastPhase;
	TUint iSeenSeq;
	TBool iBusyShown;
	TBool iForeground;
	TBool iClosing;                // the app is being destroyed
	TBool iSaidSharing;            // "Sharing the screen" said for this start
	TInt32 iPauseCapture;          // Shift+Ctrl+W taken from every program while sharing (0: not)
	};

class CCsDocument : public CEikDocument
	{
public:
	CCsDocument(CEikApplication& aApp) : CEikDocument(aApp) {}
private:
	CEikAppUi* CreateAppUiL();
	};

class CCsApplication : public CEikApplication
	{
private:
	CApaDocument* CreateDocumentL();
	TUid AppDllUid() const;
	};

#endif
