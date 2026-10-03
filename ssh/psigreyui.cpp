// psigreyui.cpp - Preferences > Screen greys..., shared by PsiTerm, PsiMail
// and PsiWeb (docs/display.md; the interface is in psigreyui.h)
//
// It opens on "Which looks best?": the same test picture and the same
// coloured terminal text drawn four ways, side by side - standard, lighter
// shadows, darker shadows, and calibration off (as before). Keys 1-4 or
// Left/Right choose, Up/Down make the chosen version lighter or darker in
// small steps (gamma 0.10 a step: gamma above 1 takes the levels to look
// darker than their numbers, so every grey is drawn lighter), Enter uses
// it in all three programs, Esc closes, and A opens the detailed settings
// below for fine adjustment.
//
// The detailed settings:
// A full-screen page, as a test card: the 16 levels as bars, then ramps
// drawn the way PsiMail and PsiWeb will draw pictures (error diffusion, or
// the ordered pattern) and plain greys (text, rules, terminal colours) with
// the calibration as it stands. The arrow keys change it and the ramps
// follow at once, so the screen in front of the user is the judge.
//
//   Tab / Shift+Tab   the next / previous setting
//   Left / Right      the level (on Levels), else the setting down / up
//   Up / Down         lighter / darker (Levels), else the setting up / down
//   Del               the standard settings
//   Enter             save (C:\System\Data\PsiGrey.ini, for all three programs)
//   Esc               close without saving
//
// The ramps are right when they look smooth, with even steps from black to
// white, and every one of the 16 bars can be told from its neighbours.
// Saving writes the settings and the table they make (ssh/psigrey.h);
// PsiMail sets out its next picture with it, PsiWeb its next page, and
// PsiTerm's own colours change at once.

#include <e32math.h>
#include <eikenv.h>
#include <eikdef.h>
#include <coeaui.h>
#include <fbs.h>
#include "psigreyui.h"
#include "psidisp.h"
#include "psigreypic.h"

enum
	{
	EGreyLevels, EGreyGamma, EGreyCurve, EGreyBright, EGreyOn, EGreyDither,
	EGreyReadContrast, EGreyReadLight, EGreyFields
	};

const TInt KGreyGammaMin = 50;          // the limits psigrey_parse keeps to
const TInt KGreyGammaMax = 250;
const TInt KGreyNudgeStep = 10;         // Up/Down on "Which looks best?": gamma 0.10

static double GreyPow(double aX, double aY)
	{
	TReal r = 0;
	if (Math::Pow(r, aX, aY) != KErrNone)
		return aX;
	return r;
	}

// The four versions' gamma before any nudge (the last is calibration off)
static TInt GreyBaseGamma(TInt aWhich)
	{
	return aWhich == 1 ? 160 : aWhich == 2 ? 60 : PSIGREY_STD_GAMMA;
	}

CPsiGreyScreen* CPsiGreyScreen::NewL(MPsiGreyObserver& aObserver)
	{
	CPsiGreyScreen* self = new(ELeave) CPsiGreyScreen(aObserver);
	CleanupStack::PushL(self);
	self->ConstructL();
	CleanupStack::Pop();
	return self;
	}

CPsiGreyScreen::CPsiGreyScreen(MPsiGreyObserver& aObserver)
	: iObserver(aObserver), iField(EGreyLevels), iLevel(7), iSimple(ETrue)
	{
	}

CPsiGreyScreen::~CPsiGreyScreen()
	{
	delete iRamps;
	for (TInt i = 0; i < KChoices; i++)
		delete iPics[i];
	}

void CPsiGreyScreen::ConstructL()
	{
	PsiGreyLoad(iCoeEnv->FsSession(), iGrey);
	CreateBackedUpWindowL(iCoeEnv->RootWin(), EGray16);
	TSize screen = iCoeEnv->ScreenDevice()->SizeInPixels();
	SetExtentL(TPoint(0, 0), screen);
	// in front of the Preferences dialog, and the pen stays here: a tap
	// must not reach the dialog's buttons underneath
	DrawableWindow()->SetOrdinalPosition(0);
	SetPointerCapture(ETrue);
	// (made while the dialog has the windows behind it faded: this one is
	// not behind it, and must show its greys as they are)
	DrawableWindow()->SetFaded(EFalse, RWindowTreeNode::EFadeIncludeChildren);
	iRamps = new(ELeave) CFbsBitmap;
	User::LeaveIfError(iRamps->Create(TSize(screen.iWidth, KRampRows), EGray16));
	MakeRampsL();
	// the version in use now, with its nudge when it is one of ours: the
	// standard curve, no brightness or level nudges, and a gamma that is a
	// whole number of steps from a version's
	if (!iGrey.on)
		iChoice = 3;
	else
		{
		TBool plain = iGrey.curve == PSIGREY_STD_CURVE && iGrey.bright == 0;
		for (TInt k = 0; k < 16; k++)
			if (iGrey.nudge[k])
				plain = EFalse;
		TInt g = iGrey.gamma;
		TInt c = g >= GreyBaseGamma(1) ? 1 : g <= GreyBaseGamma(2) ? 2 : 0;
		TInt d = g - GreyBaseGamma(c);
		if (plain && d % KGreyNudgeStep == 0)
			{
			iChoice = c;
			iNudge[c] = d / KGreyNudgeStep;
			}
		}
	for (TInt i = 0; i < KChoices; i++)
		MakeVersion(i);
	MakeChoicesL();
	ActivateL();
	}

// The pen does nothing here (the keys do it all); the tap is only kept
// from the windows underneath
void CPsiGreyScreen::HandlePointerEventL(const TPointerEvent& /*aPointerEvent*/)
	{
	}

// ----- "Which looks best?" ---------------------------------------------------

_LIT(KChoice0, "Standard");
_LIT(KChoice1, "Lighter shadows");
_LIT(KChoice2, "Darker shadows");
_LIT(KChoice3, "As before (off)");

// Version aWhich of the greys: the user's other settings (pictures, Reading
// mode) kept, the curve set afresh, with its nudge. Gamma above 1 takes the
// levels to look darker than their numbers, so every grey is drawn a level
// or so lighter.
void CPsiGreyScreen::MakeVersion(TInt aWhich)
	{
	PsiGrey& v = iVersions[aWhich];
	v = iGrey;
	v.on = aWhich != 3;
	v.bright = 0;
	for (TInt k = 0; k < 16; k++)
		v.nudge[k] = 0;
	v.curve = PSIGREY_STD_CURVE;
	v.gamma = psigrey_clamp(GreyBaseGamma(aWhich) + KGreyNudgeStep * iNudge[aWhich],
		KGreyGammaMin, KGreyGammaMax);
	psigrey_compute(&v, GreyPow);
	}

// The test picture as each version sets it out (error diffusion or the
// ordered pattern, as chosen in the detailed settings), as PsiMail and
// PsiWeb would. Only the chosen one again after a nudge.
void CPsiGreyScreen::MakeChoicesL()
	{
	const TInt w = PsiGreyPicW, h = PsiGreyPicH;
	static const TUint8 KBayer[16] = { 8, 136, 40, 168, 200, 72, 232, 104, 56, 184, 24, 152, 248, 120, 216, 88 };
	HBufC8* rowBuf = HBufC8::NewLC((w + 1) / 2 + 4);
	TPtr8 row = rowBuf->Des();
	TInt* err = (TInt*)User::AllocLC(sizeof(TInt) * 2 * (w + 2));
	TBool all = !iPics[0];
	for (TInt c = 0; c < KChoices; c++)
		{
		if (!all && c != iChoice)
			continue;
		if (!iPics[c])
			{
			iPics[c] = new(ELeave) CFbsBitmap;
			User::LeaveIfError(iPics[c]->Create(TSize(w, h), EGray16));
			}
		unsigned char lv[16], cal[256];
		psigrey_levels(&iVersions[c], lv);
		psigrey_nearest(lv, cal);
		Mem::FillZ(err, sizeof(TInt) * 2 * (w + 2));
		for (TInt y = 0; y < h; y++)
			{
			TInt* ec = err + ((y & 1) ? (w + 2) : 0);
			TInt* en = err + ((y & 1) ? 0 : (w + 2));
			Mem::FillZ(en, sizeof(TInt) * (w + 2));
			row.SetLength((w + 1) / 2);
			row.FillZ();
			TBool back = (y & 1) != 0;
			TInt fwd = 0;
			for (TInt i = 0; i < w; i++)
				{
				TInt x = back ? w - 1 - i : i;
				TInt step = back ? -1 : 1;
				TInt v = PsiGreyPic[y * w + x];
				TInt q;
				if (!iGrey.dither)
					{
					TInt k = 0;
					while (k < 15 && v >= lv[k + 1])
						k++;
					q = k;
					if (k < 15 && v > lv[k]
						&& (v - lv[k]) * 255 / (lv[k + 1] - lv[k]) + KBayer[(y & 3) * 4 + (x & 3)] >= 255)
						q = k + 1;
					}
				else
					{
					TInt t = v + ec[x + 1] + fwd;
					if (t < 0) t = 0;
					if (t > 255) t = 255;
					q = cal[t];
					TInt e = t - lv[q];
					fwd = (e * 7) >> 4;
					TInt e3 = (e * 3) >> 4, e5 = (e * 5) >> 4;
					en[x + 1 - step] += e3;
					en[x + 1] += e5;
					en[x + 1 + step] += e - fwd - e3 - e5;
					}
				row[x >> 1] = (TUint8)(row[x >> 1] | (q << ((x & 1) * 4)));
				}
			iPics[c]->SetScanLine(row, y);
			}
		}
	CleanupStack::PopAndDestroy(2);          // err, rowBuf
	}

// A terminal colour as PsiTerm shows it (CTermView::CellColours, with the
// standard "dark greys" for coloured text): aCal the version's table, or
// NULL for calibration off
static TInt TermGrey(TInt aR, TInt aG, TInt aB, const TUint8* aCal)
	{
	TUint x = aR * 30 + aG * 59 + aB * 11;
	if (aCal)
		return aCal[(x * 5243u) >> 19];
	return (TInt)((x * 9869u) >> 24);
	}

struct TTermSample { const TText* iText; TUint8 iR, iG, iB; };

// "1  Standard", and its nudge ("lighter +2", "darker -1"), or none
void CPsiGreyScreen::ChoiceLabel(TInt aWhich, TDes& aName, TDes& aNudge) const
	{
	aName.Num(aWhich + 1);
	aName.Append(_L("  "));
	aName.Append(aWhich == 0 ? KChoice0() : aWhich == 1 ? KChoice1() : aWhich == 2 ? KChoice2() : KChoice3());
	aNudge.Zero();
	TInt n = iNudge[aWhich];
	if (n > 0)
		{
		aNudge.Append(_L("lighter +"));
		aNudge.AppendNum(n);
		}
	else if (n < 0)
		{
		aNudge.Append(_L("darker "));
		aNudge.AppendNum(n);
		}
	}

void CPsiGreyScreen::DrawChoices(CWindowGc& aGc) const
	{
	TRect all = Rect();
	TInt w = all.Width();
	const CFont* title = iEikonEnv->LegendFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(KRgbBlack);
	aGc.UseFont(title);
	aGc.DrawText(_L("Which looks best?"), TPoint(4, title->AscentInPixels() + 2));
	aGc.UseFont(small);
	_LIT(KSaved, "For PsiTerm, PsiMail & PsiWeb");
	aGc.DrawText(KSaved, TPoint(w - 4 - small->TextWidthInPixels(KSaved), title->AscentInPixels() + 2));

	// coloured terminal text: on the white, then as backgrounds (a tmux
	// bar, a selection) with light text
	static const TTermSample KOnWhite[] =
		{
		{ _S("red"), 205, 0, 0 }, { _S("green"), 0, 205, 0 }, { _S("blue"), 0, 0, 238 },
		{ _S("cyan"), 0, 205, 205 }, { _S("grey"), 128, 128, 128 },
		};
	static const TTermSample KBacks[] =
		{
		{ _S("blue"), 0, 0, 238 }, { _S("red"), 205, 0, 0 }, { _S("green"), 0, 205, 0 },
		{ _S("yellow"), 205, 205, 0 },
		};
	TInt panelW = w / KChoices;
	TInt picY = 18;
	TInt lineH = small->HeightInPixels() + 3;
	for (TInt c = 0; c < KChoices; c++)
		{
		TInt x0 = c * panelW;
		TInt px = x0 + (panelW - PsiGreyPicW) / 2;
		aGc.BitBlt(TPoint(px, picY), iPics[c]);
		unsigned char lv[16], cal[256];
		psigrey_levels(&iVersions[c], lv);
		psigrey_nearest(lv, cal);
		const TUint8* tab = iVersions[c].on ? cal : NULL;
		TInt y = picY + PsiGreyPicH + 3;
		TInt x = px;
		aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		for (TUint i = 0; i < sizeof(KOnWhite) / sizeof(KOnWhite[0]); i++)
			{
			TPtrC word(KOnWhite[i].iText);
			TInt fg = TermGrey(KOnWhite[i].iR, KOnWhite[i].iG, KOnWhite[i].iB, tab);
			fg = (fg * 6 + 7) / 15;              // coloured text on the white: dark greys
			TInt tw = small->TextWidthInPixels(word) + 4;
			aGc.SetBrushColor(KRgbWhite);
			aGc.SetPenColor(TRgb::Gray16(fg));
			aGc.DrawText(word, TRect(x, y, x + tw, y + lineH), small->AscentInPixels() + 1, CGraphicsContext::ELeft, 1);
			x += tw;
			}
		y += lineH;
		x = px;
		for (TUint j = 0; j < sizeof(KBacks) / sizeof(KBacks[0]); j++)
			{
			TPtrC word(KBacks[j].iText);
			TInt bg = TermGrey(KBacks[j].iR, KBacks[j].iG, KBacks[j].iB, tab);
			TInt fg = 15;
			TInt diff = fg - bg;
			if (diff < 6)
				fg = 0;                          // keep enough contrast to read
			TInt tw = small->TextWidthInPixels(word) + 6;
			aGc.SetBrushColor(TRgb::Gray16(bg));
			aGc.SetPenColor(TRgb::Gray16(fg));
			aGc.DrawText(word, TRect(x, y, x + tw, y + lineH), small->AscentInPixels() + 1, CGraphicsContext::ECenter, 0);
			x += tw;
			}
		y += lineH + 2;
		// the name and its nudge (on one line if it fits, else two), and a
		// frame round the one chosen
		TBuf<40> name, nudge;
		ChoiceLabel(c, name, nudge);
		TBool on = c == iChoice;
		TInt labelW = panelW - 6;
		TInt lines = 1;
		if (nudge.Length())
			{
			TBuf<64> one(name);
			one.Append(_L(", "));
			one.Append(nudge);
			if (small->TextWidthInPixels(one) <= labelW - 4)
				{
				name = one.Left(Min(one.Length(), name.MaxLength()));
				nudge.Zero();
				}
			else
				lines = 2;
			}
		TRect label(x0 + 3, y, x0 + panelW - 3, y + lineH + 1);
		aGc.SetBrushColor(on ? KRgbBlack : KRgbWhite);
		aGc.SetPenColor(on ? KRgbWhite : KRgbBlack);
		aGc.DrawText(name, label, small->AscentInPixels() + 2, CGraphicsContext::ECenter, 0);
		if (lines == 2)
			{
			label.iTl.iY = label.iBr.iY;
			label.iBr.iY += lineH;
			aGc.DrawText(nudge, label, small->AscentInPixels() + 1, CGraphicsContext::ECenter, 0);
			}
		if (on)
			{
			aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
			aGc.SetPenColor(KRgbBlack);
			aGc.DrawRect(TRect(x0 + 1, picY - 3, x0 + panelW - 1, label.iBr.iY + 2));
			aGc.DrawRect(TRect(x0 + 2, picY - 2, x0 + panelW - 2, label.iBr.iY + 1));
			}
		}
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenColor(KRgbBlack);
	TInt hy = all.iBr.iY - 4;
	aGc.DrawText(_L("1-4: choose   Up/Down: lighter/darker   Enter: save   Esc: close   A: detailed settings"),
		TPoint(4, hy));
	aGc.DrawText(_L("Look at the doorway and the tree (dark), the clouds (light) and the ball (smooth)"),
		TPoint(4, hy - small->HeightInPixels() - 3));
	aGc.DiscardFont();
	}

// Up/Down: the chosen version a step lighter or darker (gamma up is
// lighter), within the limits; its picture and text are drawn again
void CPsiGreyScreen::NudgeL(TInt aDelta)
	{
	if (iChoice == 3)
		{
		iEikonEnv->InfoMsg(_L("Not available - calibration is off in version 4"));
		return;
		}
	TInt g = GreyBaseGamma(iChoice) + KGreyNudgeStep * (iNudge[iChoice] + aDelta);
	if (g < KGreyGammaMin || g > KGreyGammaMax)
		{
		iEikonEnv->InfoMsg(aDelta > 0 ? _L("Already the lightest") : _L("Already the darkest"));
		return;
		}
	iNudge[iChoice] += aDelta;
	MakeVersion(iChoice);
	MakeChoicesL();
	DrawNow();
	}

TKeyResponse CPsiGreyScreen::ChoiceKeyL(TInt aCode)
	{
	switch (aCode)
		{
	case EKeyEscape:
		iObserver.GreyScreenDone(EFalse, iGrey);
		break;
	case EKeyEnter:
		iObserver.GreyScreenDone(ETrue, iVersions[iChoice]);
		break;
	case EKeyLeftArrow:
		iChoice = (iChoice + KChoices - 1) % KChoices;
		DrawNow();
		break;
	case EKeyRightArrow:
	case EKeyTab:
		iChoice = (iChoice + 1) % KChoices;
		DrawNow();
		break;
	case EKeyUpArrow:
		NudgeL(1);
		break;
	case EKeyDownArrow:
		NudgeL(-1);
		break;
	case 'a':
	case 'A':
		iSimple = EFalse;                    // the detailed settings
		DrawNow();
		break;
	default:
		if (aCode >= '1' && aCode < '1' + KChoices)
			{
			iChoice = aCode - '1';
			DrawNow();
			}
		break;
		}
	return EKeyWasConsumed;
	}

// the table from the settings, and the ramps drawn with it
void CPsiGreyScreen::Changed()
	{
	psigrey_compute(&iGrey, GreyPow);
	TRAPD(err, MakeRampsL());
	(void)err;
	DrawNow();
	}

// Rows 0-23: a ramp as pictures will be (error diffusion or the ordered
// pattern, as chosen); 24-47: the same ramp as plain greys (each pixel the
// nearest level: text, rules and terminal colours); 48-71: 16 patches,
// v = 0, 17 .. 255, as pictures will be: even steps when it is right.
void CPsiGreyScreen::MakeRampsL()
	{
	TInt w = iRamps->SizeInPixels().iWidth;
	unsigned char lv[16], cal[256];
	psigrey_levels(&iGrey, lv);
	psigrey_nearest(lv, cal);
	HBufC8* rowBuf = HBufC8::NewLC((w + 1) / 2 + 4);
	TPtr8 row = rowBuf->Des();
	TInt* err = (TInt*)User::AllocLC(sizeof(TInt) * 2 * (w + 2));
	Mem::FillZ(err, sizeof(TInt) * 2 * (w + 2));
	static const TUint8 KBayer[16] = { 8, 136, 40, 168, 200, 72, 232, 104, 56, 184, 24, 152, 248, 120, 216, 88 };
	for (TInt y = 0; y < KRampRows; y++)
		{
		TInt part = y / 24;                  // 0 picture ramp, 1 plain ramp, 2 patches
		if (y % 24 == 0)
			Mem::FillZ(err, sizeof(TInt) * 2 * (w + 2));
		TInt* ec = err + ((y & 1) ? (w + 2) : 0);
		TInt* en = err + ((y & 1) ? 0 : (w + 2));
		Mem::FillZ(en, sizeof(TInt) * (w + 2));
		row.SetLength((w + 1) / 2);
		row.FillZ();
		TBool back = (y & 1) != 0;
		TInt fwd = 0;
		for (TInt i = 0; i < w; i++)
			{
			TInt x = back ? w - 1 - i : i;
			TInt step = back ? -1 : 1;
			TInt v = part == 2 ? (x * 16 / w) * 17 : (x * 255) / (w - 1);
			TInt q;
			if (part == 1)
				q = cal[v];
			else if (!iGrey.dither)
				{
				// the ordered pattern between the levels (psi_grey.c's way)
				TInt k = 0;
				while (k < 15 && v >= lv[k + 1])
					k++;
				q = k;
				if (k < 15 && v > lv[k]
					&& (v - lv[k]) * 255 / (lv[k + 1] - lv[k]) + KBayer[(y & 3) * 4 + (x & 3)] >= 255)
					q = k + 1;
				}
			else
				{
				// Floyd-Steinberg, serpentine (pmimg.c's and psi_drv.c's way)
				TInt t = v + ec[x + 1] + fwd;
				if (t < 0) t = 0;
				if (t > 255) t = 255;
				q = cal[t];
				TInt e = t - lv[q];
				fwd = (e * 7) >> 4;
				TInt e3 = (e * 3) >> 4, e5 = (e * 5) >> 4;
				en[x + 1 - step] += e3;
				en[x + 1] += e5;
				en[x + 1 + step] += e - fwd - e3 - e5;
				}
			row[x >> 1] = (TUint8)(row[x >> 1] | (q << ((x & 1) * 4)));
			}
		iRamps->SetScanLine(row, y);
		}
	CleanupStack::PopAndDestroy(2);          // err, rowBuf
	}

TKeyResponse CPsiGreyScreen::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKey)
		return EKeyWasConsumed;              // (everything stops here: the menus too)
	TInt code = aKeyEvent.iCode;
	if (iSimple)
		return ChoiceKeyL(code);
	TBool shift = (aKeyEvent.iModifiers & EModifierShift) != 0;
	TInt delta = 0;
	switch (code)
		{
	case EKeyEscape:
		iObserver.GreyScreenDone(EFalse, iGrey);
		return EKeyWasConsumed;
	case EKeyEnter:
		iObserver.GreyScreenDone(ETrue, iGrey);
		return EKeyWasConsumed;
	case EKeyTab:
		iField = (iField + (shift ? EGreyFields - 1 : 1)) % EGreyFields;
		DrawNow();
		return EKeyWasConsumed;
	case EKeyDelete:
	case EKeyBackspace:
		{
		TInt rc = iGrey.read_contrast, rl = iGrey.read_light;
		psigrey_defaults(&iGrey);
		iGrey.read_contrast = rc;            // (Reading mode is not a grey)
		iGrey.read_light = rl;
		Changed();
		iEikonEnv->InfoMsg(_L("Standard greys"));
		return EKeyWasConsumed;
		}
	case EKeyLeftArrow:
		if (iField == EGreyLevels)
			{
			if (iLevel > 0) iLevel--;
			DrawNow();
			return EKeyWasConsumed;
			}
		delta = -1;
		break;
	case EKeyRightArrow:
		if (iField == EGreyLevels)
			{
			if (iLevel < 15) iLevel++;
			DrawNow();
			return EKeyWasConsumed;
			}
		delta = 1;
		break;
	case EKeyUpArrow:
		delta = 1;
		break;
	case EKeyDownArrow:
		delta = -1;
		break;
	default:
		return EKeyWasConsumed;
		}
	switch (iField)
		{
	case EGreyLevels:
		iGrey.nudge[iLevel] = psigrey_clamp(iGrey.nudge[iLevel] + 2 * delta, -64, 64);
		break;
	case EGreyGamma:
		iGrey.gamma = psigrey_clamp(iGrey.gamma + 5 * delta, KGreyGammaMin, KGreyGammaMax);
		break;
	case EGreyCurve:
		iGrey.curve = psigrey_clamp(iGrey.curve + 5 * delta, 0, 100);
		break;
	case EGreyBright:
		iGrey.bright = psigrey_clamp(iGrey.bright + 2 * delta, -64, 64);
		break;
	case EGreyOn:
		iGrey.on = !iGrey.on;
		break;
	case EGreyDither:
		iGrey.dither = !iGrey.dither;
		break;
	case EGreyReadContrast:
		iGrey.read_contrast = psigrey_clamp(iGrey.read_contrast + delta, 0, 4);
		break;
	case EGreyReadLight:
		iGrey.read_light = !iGrey.read_light;
		break;
		}
	Changed();
	return EKeyWasConsumed;
	}

// "+4", "0", "-6" (TDes::Format has no "+" flag)
static void AppendSigned(TDes& aText, TInt aValue)
	{
	if (aValue > 0)
		aText.Append('+');
	aText.AppendNum(aValue);
	}

// one setting: its name, then its value; the one being changed is inverted
void CPsiGreyScreen::DrawField(CWindowGc& aGc, TInt aField, const TDesC& aText, TInt& aX, TInt aY) const
	{
	const CFont* font = iEikonEnv->AnnotationFont();
	TInt w = font->TextWidthInPixels(aText) + 8;
	TRect r(aX, aY, aX + w, aY + font->HeightInPixels() + 2);
	TBool on = aField == iField;
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(on ? KRgbBlack : KRgbWhite);
	aGc.SetPenColor(on ? KRgbWhite : KRgbBlack);
	aGc.DrawText(aText, r, font->AscentInPixels() + 1, CGraphicsContext::ECenter, 0);
	aX += w + 2;
	}

void CPsiGreyScreen::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	TRect all = Rect();
	TInt w = all.Width();
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.DrawRect(all);
	if (iSimple)
		{
		DrawChoices(gc);
		return;
		}

	// the title
	const CFont* title = iEikonEnv->LegendFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	gc.UseFont(title);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbBlack);
	gc.DrawText(_L("Screen greys"), TPoint(4, title->AscentInPixels() + 2));
	gc.UseFont(small);
	_LIT(KKeys, "Enter: save   Esc: close   Del: standard");
	gc.DrawText(KKeys, TPoint(w - 4 - small->TextWidthInPixels(KKeys), title->AscentInPixels() + 2));

	// the 16 levels, each as it is (no calibration), with its number
	TInt top = 18, barH = 52, barW = w / 16;
	TBuf<4> num;
	for (TInt k = 0; k < 16; k++)
		{
		TRect bar(k * barW, top, (k + 1) * barW, top + barH);
		gc.SetPenStyle(CGraphicsContext::ENullPen);
		gc.SetBrushColor(TRgb::Gray16(k));
		gc.DrawRect(bar);
		num.Num(k);
		gc.SetBrushStyle(CGraphicsContext::ENullBrush);
		gc.SetPenStyle(CGraphicsContext::ESolidPen);
		gc.SetPenColor(k < 8 ? KRgbWhite : KRgbBlack);
		gc.DrawText(num, TPoint(bar.iTl.iX + 3, bar.iTl.iY + small->AscentInPixels() + 2));
		if (iGrey.nudge[k])
			{
			num.Zero();
			if (iGrey.nudge[k] > 0) num.Append('+');
			num.AppendNum(iGrey.nudge[k]);
			gc.DrawText(num, TPoint(bar.iTl.iX + 3, bar.iBr.iY - 3));
			}
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		}
	// the level being nudged: a black mark under its bar
	if (iField == EGreyLevels)
		{
		gc.SetPenStyle(CGraphicsContext::ENullPen);
		gc.SetBrushColor(KRgbBlack);
		gc.DrawRect(TRect(iLevel * barW + 2, top + barH + 1, (iLevel + 1) * barW - 2, top + barH + 4));
		}

	// the ramps
	TInt ry = top + barH + 6;
	gc.BitBlt(TPoint(0, ry), iRamps);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbBlack);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	ry += KRampRows + 2;

	// what each ramp is
	gc.UseFont(small);
	TInt ly = ry + small->AscentInPixels();
	gc.DrawText(iGrey.dither ? _L("Ramps: pictures (error diffusion), plain greys, 16 steps")
		: _L("Ramps: pictures (ordered pattern), plain greys, 16 steps"), TPoint(4, ly));

	// the settings, the one being changed inverted
	TBuf<40> t;
	TInt x = 4, y = ry + small->HeightInPixels() + 4;
	t.Format(_L("Level %d: "), iLevel);
	AppendSigned(t, iGrey.nudge[iLevel]);
	DrawField(gc, EGreyLevels, t, x, y);
	t.Format(_L("Gamma %d.%02d"), iGrey.gamma / 100, iGrey.gamma % 100);
	DrawField(gc, EGreyGamma, t, x, y);
	t.Format(_L("Curve %d"), iGrey.curve);
	DrawField(gc, EGreyCurve, t, x, y);
	t.Copy(_L("Brightness "));
	AppendSigned(t, iGrey.bright);
	DrawField(gc, EGreyBright, t, x, y);
	x = 4;
	y += small->HeightInPixels() + 4;
	DrawField(gc, EGreyOn, iGrey.on ? _L("Calibration on") : _L("Calibration off"), x, y);
	DrawField(gc, EGreyDither, iGrey.dither ? _L("Pictures: error diffusion") : _L("Pictures: ordered"), x, y);
	t.Format(_L("Reading mode: contrast +%d"), iGrey.read_contrast);
	DrawField(gc, EGreyReadContrast, t, x, y);
	DrawField(gc, EGreyReadLight, iGrey.read_light ? _L("light on") : _L("light as set"), x, y);
	y += small->HeightInPixels() + 6;
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.SetPenColor(KRgbBlack);
	gc.DrawText(_L("Tab: next setting   Left/Right: level   Up/Down: lighter/darker or change"),
		TPoint(4, y + small->AscentInPixels()));
	gc.DiscardFont();
	}

// ----- running it from a Preferences dialog ------------------------------------

// Ends the nested wait on Enter or Esc
class TPsiGreyRun : public MPsiGreyObserver
	{
public:
	TPsiGreyRun() : iDone(EFalse), iSave(EFalse) {}
	void GreyScreenDone(TBool aSave, const PsiGrey& aGrey)
		{
		iDone = ETrue;
		iSave = aSave;
		if (aSave)
			iGrey = aGrey;
		CActiveScheduler::Stop();
		}
	TBool iDone;
	TBool iSave;
	PsiGrey iGrey;
	};

// The screen comes off the control stack on every path, a leave through
// the nested loop included (the app closing: KLeaveExit)
struct TPsiGreyStacked
	{
	CCoeAppUi* iAppUi;
	CCoeControl* iControl;
	};

static void PsiGreyUnstack(TAny* aPtr)
	{
	TPsiGreyStacked* s = (TPsiGreyStacked*)aPtr;
	s->iAppUi->RemoveFromStack(s->iControl);
	}

TInt PsiGreyScreenL(PsiGrey& aGrey)
	{
	CEikonEnv* env = CEikonEnv::Static();
	TPsiGreyRun run;
	CPsiGreyScreen* screen = CPsiGreyScreen::NewL(run);
	CleanupStack::PushL(screen);
	TPsiGreyStacked stacked;
	stacked.iAppUi = (CCoeAppUi*)env->AppUi();   // (CCoeAppUiBase*: every EIKON app UI is a CCoeAppUi)
	stacked.iControl = screen;
	// The unstacking first (RemoveFromStack is harmless for a control that
	// never got on), then onto the stack above the dialog's own priority:
	// every key comes here first. (Not TRAP(..PushL..): leaving a TRAP with
	// an item pushed inside it is E32USER-CBase 71.)
	CleanupStack::PushL(TCleanupItem(PsiGreyUnstack, &stacked));
	stacked.iAppUi->AddToStackL(screen, ECoeStackPriorityAlert);
	screen->DrawNow();
	CActiveScheduler::Start();               // until Enter or Esc
	// The loop stopped, but not by the screen: someone else called Stop
	// (the app closing). Close as with Esc, and hand the stop on to the
	// loop it was meant for once this one has tidied up.
	TBool passOn = !run.iDone;
	CleanupStack::PopAndDestroy(2);          // unstack, screen
	if (passOn)
		{
		CActiveScheduler::Stop();
		return 0;
		}
	if (!run.iSave)
		return 0;
	TInt r = PsiGreySave(env->FsSession(), run.iGrey);
	if (r != KErrNone)
		return r < 0 ? r : KErrGeneral;
	aGrey = run.iGrey;
	return 1;
	}
