// psigreyui.h - Preferences > Screen greys..., shared by PsiTerm, PsiMail
// and PsiWeb (docs/display.md). Each app compiles psigreyui.cpp, as it does
// pglinktest.cpp.
//
// A full-screen page that opens on "Which looks best?": the same test
// picture and coloured terminal text drawn four ways side by side. 1-4 or
// Left/Right choose a version, Up/Down make the chosen one lighter or
// darker in small steps, Enter saves it for all three programs, Esc
// closes, and A opens the detailed settings. The settings are in
// C:\System\Data\PsiGrey.ini (ssh/psigrey.h, ssh/psidisp.h).
//
// The app's side is one call, from its Preferences dialog's button:
//     PsiGrey grey;
//     TInt r = PsiGreyScreenL(grey);   // 1 saved, 0 closed, < 0 not saved
// then it applies grey as it can (PsiTerm at once, PsiMail from the next
// picture, PsiWeb from the next page) and says so in an infoprint.
#ifndef PSIGREYUI_H
#define PSIGREYUI_H

#include <coecntrl.h>
#include <w32std.h>
#include "psigrey.h"

class CFbsBitmap;

class MPsiGreyObserver
	{
public:
	// Enter (aSave, with the settings to save) or Esc
	virtual void GreyScreenDone(TBool aSave, const PsiGrey& aGrey) = 0;
	};

class CPsiGreyScreen : public CCoeControl
	{
public:
	static CPsiGreyScreen* NewL(MPsiGreyObserver& aObserver);
	~CPsiGreyScreen();
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
private:
	enum { KRampRows = 72 };
	enum { KChoices = 4 };             // "Which looks best?": the four versions
	CPsiGreyScreen(MPsiGreyObserver& aObserver);
	void ConstructL();
	void Draw(const TRect& aRect) const;
	void HandlePointerEventL(const TPointerEvent& aPointerEvent);
	void DrawChoices(CWindowGc& aGc) const;
	void ChoiceLabel(TInt aWhich, TDes& aName, TDes& aNudge) const;
	TKeyResponse ChoiceKeyL(TInt aCode);
	void NudgeL(TInt aDelta);
	void MakeChoicesL();
	void MakeVersion(TInt aWhich);
	void DrawField(CWindowGc& aGc, TInt aField, const TDesC& aText, TInt& aX, TInt aY) const;
	void Changed();
	void MakeRampsL();
	MPsiGreyObserver& iObserver;
	PsiGrey iGrey;
	TInt iField;
	TInt iLevel;
	CFbsBitmap* iRamps;
	TBool iSimple;                     // "Which looks best?" (A: the detailed settings)
	TInt iChoice;                      // 0..KChoices-1
	TInt iNudge[KChoices];             // Up/Down: steps lighter (+) or darker (-)
	PsiGrey iVersions[KChoices];
	CFbsBitmap* iPics[KChoices];       // the test picture as each version draws it
	};

// Opens the screen on top of everything (a Preferences dialog included)
// and waits, with the app's loop running, until Enter or Esc. On Enter the
// settings are saved to PsiGrey.ini and aGrey holds them. Returns 1 saved,
// 0 closed without saving, or the file error (< 0) when saving failed.
TInt PsiGreyScreenL(PsiGrey& aGrey);

#endif
