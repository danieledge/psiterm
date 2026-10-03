// PTGREY.CPP - Tools > Debug > Display calibration... (docs/display.md)
//
// It opens on "Which looks best?" (0.82): the same test picture and the same
// coloured terminal text drawn four ways, side by side - standard, lighter
// shadows, darker shadows, and calibration off (as before). Keys 1-4 or
// Left/Right choose, Enter uses that one in all three programs, Esc closes,
// and A opens the detailed settings below for fine adjustment.
//
// The detailed settings:
// A full-screen page, as a test card: the 16 levels as bars, then ramps
// drawn the way PsiMail and PsiWeb will draw pictures (error diffusion, or
// the ordered pattern) and plain greys (text, rules, terminal colours) with
// the calibration as it stands. The arrow keys change it and the ramps
// follow at once, so the screen in front of Dan is the judge.
//
//   Tab / Shift+Tab   the next / previous setting
//   Left / Right      the level (on Levels), else the setting down / up
//   Up / Down         lighter / darker (Levels), else the setting up / down
//   Del               the standard settings
//   Enter             save (C:\System\Data\PsiGrey.ini, for all three programs)
//   Esc               close without saving
//   T                 time a frame through the window server against a
//                     direct write to the screen (docs/display.md)
//
// The ramps are right when they look smooth, with even steps from black to
// white, and every one of the 16 bars can be told from its neighbours.
// Saving writes the settings and the table they make (ssh/psigrey.h);
// PsiMail sets out its next picture with it, PsiWeb its next page, and
// PsiTerm's own colours change at once.

#include <e32hal.h>
#include <e32math.h>
#include <e32svr.h>
#include <eikenv.h>
#include <eikappui.h>
#include <fbs.h>
#include "psiterm.h"
#include "psidisp.h"
#include "ptcalpic.h"

enum
	{
	EGreyLevels, EGreyGamma, EGreyCurve, EGreyBright, EGreyOn, EGreyDither,
	EGreyReadContrast, EGreyReadLight, EGreyFields
	};

static double GreyPow(double aX, double aY)
	{
	TReal r = 0;
	if (Math::Pow(r, aX, aY) != KErrNone)
		return aX;
	return r;
	}

CPtGreyScreen* CPtGreyScreen::NewL(MPtGreyObserver& aObserver)
	{
	CPtGreyScreen* self = new(ELeave) CPtGreyScreen(aObserver);
	CleanupStack::PushL(self);
	self->ConstructL();
	CleanupStack::Pop();
	return self;
	}

CPtGreyScreen::CPtGreyScreen(MPtGreyObserver& aObserver)
	: iObserver(aObserver), iField(EGreyLevels), iLevel(7), iSimple(ETrue)
	{
	}

CPtGreyScreen::~CPtGreyScreen()
	{
	delete iRamps;
	for (TInt i = 0; i < KChoices; i++)
		delete iPics[i];
	}

void CPtGreyScreen::ConstructL()
	{
	PsiGreyLoad(iCoeEnv->FsSession(), iGrey);
	CreateBackedUpWindowL(iCoeEnv->RootWin(), EGray16);
	TSize screen = iCoeEnv->ScreenDevice()->SizeInPixels();
	SetExtentL(TPoint(0, 0), screen);
	iRamps = new(ELeave) CFbsBitmap;
	User::LeaveIfError(iRamps->Create(TSize(screen.iWidth, KRampRows), EGray16));
	MakeRampsL();
	// the four versions, and the one in use now chosen
	for (TInt i = 0; i < KChoices; i++)
		Version(iGrey, i, iVersions[i]);
	if (!iGrey.on)
		iChoice = 3;
	else if (iGrey.gamma == iVersions[1].gamma && iGrey.curve == iVersions[1].curve)
		iChoice = 1;
	else if (iGrey.gamma == iVersions[2].gamma && iGrey.curve == iVersions[2].curve)
		iChoice = 2;
	MakeChoicesL();
	ActivateL();
	}

// ----- "Which looks best?" ---------------------------------------------------

_LIT(KChoice0, "Standard");
_LIT(KChoice1, "Lighter shadows");
_LIT(KChoice2, "Darker shadows");
_LIT(KChoice3, "As before (off)");

// Version aWhich of the greys: Dan's other settings (pictures, Reading mode)
// kept, the curve set afresh. Gamma above 1 takes the levels to look darker
// than their numbers, so every grey is drawn a level or so lighter.
void CPtGreyScreen::Version(const PsiGrey& aNow, TInt aWhich, PsiGrey& aOut)
	{
	aOut = aNow;
	aOut.on = 1;
	aOut.bright = 0;
	for (TInt k = 0; k < 16; k++)
		aOut.nudge[k] = 0;
	aOut.curve = PSIGREY_STD_CURVE;
	aOut.gamma = PSIGREY_STD_GAMMA;
	if (aWhich == 1)
		aOut.gamma = 160;
	else if (aWhich == 2)
		aOut.gamma = 60;
	else if (aWhich == 3)
		aOut.on = 0;
	psigrey_compute(&aOut, GreyPow);
	}

// The test picture as each version sets it out (error diffusion or the
// ordered pattern, as chosen in the detailed settings), as PsiMail and
// PsiWeb would
void CPtGreyScreen::MakeChoicesL()
	{
	const TInt w = PtCalPicW, h = PtCalPicH;
	static const TUint8 KBayer[16] = { 8, 136, 40, 168, 200, 72, 232, 104, 56, 184, 24, 152, 248, 120, 216, 88 };
	HBufC8* rowBuf = HBufC8::NewLC((w + 1) / 2 + 4);
	TPtr8 row = rowBuf->Des();
	TInt* err = (TInt*)User::AllocLC(sizeof(TInt) * 2 * (w + 2));
	for (TInt c = 0; c < KChoices; c++)
		{
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
				TInt v = PtCalPic[y * w + x];
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

void CPtGreyScreen::DrawChoices(CWindowGc& aGc) const
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
	_LIT(KKeys, "1-4: choose   Enter: use it   Esc: close   A: detailed settings");
	aGc.DrawText(KKeys, TPoint(w - 4 - small->TextWidthInPixels(KKeys), title->AscentInPixels() + 2));

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
		TInt px = x0 + (panelW - PtCalPicW) / 2;
		aGc.BitBlt(TPoint(px, picY), iPics[c]);
		unsigned char lv[16], cal[256];
		psigrey_levels(&iVersions[c], lv);
		psigrey_nearest(lv, cal);
		const TUint8* tab = iVersions[c].on ? cal : NULL;
		TInt y = picY + PtCalPicH + 3;
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
		// the name, and a frame round the one chosen
		TBuf<32> name;
		name.Num(c + 1);
		name.Append(_L("  "));
		name.Append(c == 0 ? KChoice0() : c == 1 ? KChoice1() : c == 2 ? KChoice2() : KChoice3());
		TBool on = c == iChoice;
		TRect label(x0 + 3, y, x0 + panelW - 3, y + lineH + 1);
		aGc.SetBrushColor(on ? KRgbBlack : KRgbWhite);
		aGc.SetPenColor(on ? KRgbWhite : KRgbBlack);
		aGc.DrawText(name, label, small->AscentInPixels() + 2, CGraphicsContext::ECenter, 0);
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
	aGc.DrawText(_L("Look at the doorway and the tree (dark), the clouds (light) and the ball (smooth)"),
		TPoint(4, hy));
	aGc.DiscardFont();
	}

TKeyResponse CPtGreyScreen::ChoiceKeyL(TInt aCode)
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
void CPtGreyScreen::Changed()
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
void CPtGreyScreen::MakeRampsL()
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

TKeyResponse CPtGreyScreen::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
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
	case 't':
	case 'T':
		TimeBlitsL();
		return EKeyWasConsumed;
	default:
		return EKeyWasConsumed;
		}
	switch (iField)
		{
	case EGreyLevels:
		iGrey.nudge[iLevel] = psigrey_clamp(iGrey.nudge[iLevel] + 2 * delta, -64, 64);
		break;
	case EGreyGamma:
		iGrey.gamma = psigrey_clamp(iGrey.gamma + 5 * delta, 50, 250);
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
void CPtGreyScreen::DrawField(CWindowGc& aGc, TInt aField, const TDesC& aText, TInt& aX, TInt aY) const
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

void CPtGreyScreen::Draw(const TRect& /*aRect*/) const
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
	gc.DrawText(_L("Display calibration"), TPoint(4, title->AscentInPixels() + 2));
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

// T on this screen (docs/display.md, "direct screen access"): how long a
// full 640x240 frame takes through the window server (SetScanLine into a
// bitmap, then BitBlt, as PsiWeb draws its page) against copying the same
// rows straight into the screen's memory (UserSvr::ScreenInfo). The rows
// written directly stay up for 2 s (to see that they land where they
// should), then the screen is drawn again through the window server.
void CPtGreyScreen::TimeBlitsL()
	{
	const TInt KFrames = 10;
	TSize size = Rect().Size();
	TInt stride = CFbsBitmap::ScanLineLength(size.iWidth, EGray16);
	CFbsBitmap* bmp = new(ELeave) CFbsBitmap;
	CleanupStack::PushL(bmp);
	User::LeaveIfError(bmp->Create(size, EGray16));
	HBufC8* rows = HBufC8::NewLC(stride * size.iHeight);
	TPtr8 all = rows->Des();
	all.SetLength(stride * size.iHeight);
	for (TInt i = 0; i < all.Length(); i++)
		all[i] = (TUint8)(((i % stride) * 2 * 16 / size.iWidth) * 0x11);   // vertical bands, 0..15
	RWsSession& ws = iCoeEnv->WsSession();

	// 1. the window server's way: rows into a bitmap, then BitBlt (Flush is
	// a synchronous call: the server has drawn when it returns)
	ws.Flush();
	TUint t0 = User::TickCount();
	for (TInt f = 0; f < KFrames; f++)
		{
		for (TInt y = 0; y < size.iHeight; y++)
			{
			TPtr8 row((TUint8*)all.Ptr() + y * stride, stride, stride);
			bmp->SetScanLine(row, y);
			}
		ActivateGc();
		SystemGc().BitBlt(TPoint(0, 0), bmp);
		DeactivateGc();
		ws.Flush();
		}
	TUint t1 = User::TickCount();

	// 2. straight into the screen's memory
	TPckgBuf<TScreenInfoV01> info;
	UserSvr::ScreenInfo(info);
	TUint t2 = t1, t3 = t1;
	// (only on the screen this assumes: 640x240, 16 greys, 4 bits a pixel)
	if (info().iScreenAddressValid && info().iScreenAddress &&
		iCoeEnv->ScreenDevice()->DisplayMode() == EGray16 &&
		info().iScreenSize == TSize(640, 240) && size == TSize(640, 240))
		{
		TUint8* screen = (TUint8*)info().iScreenAddress;
		t2 = User::TickCount();
		for (TInt f = 0; f < KFrames; f++)
			for (TInt y = 0; y < size.iHeight; y++)
				Mem::Copy(screen + y * stride, all.Ptr() + (size.iHeight - 1 - y) * stride, stride);
		t3 = User::TickCount();
		// (the rows the other way up, so the direct copy shows as such)
		for (TInt y = 0; y < 24; y++)
			Mem::FillZ(screen + y * stride, stride / 4);  // a black corner: where (0, 0) is
		User::After(2000000);
		}
	CleanupStack::PopAndDestroy(2);          // rows, bmp
	DrawNow();
	TBuf<120> t;
	// ticks to ms (the tick period comes from the HAL: not 1/64 s everywhere)
	TTimeIntervalMicroSeconds32 period;
	if (UserHal::TickPeriod(period) != KErrNone || period.Int() <= 0)
		period = 15625;
	TInt tickUs = period.Int();
	t.Format(_L("%d frames: window server %d ms, direct %d ms (screen at %x, %s)"), KFrames,
		(TInt)((t1 - t0) * tickUs / 1000), (TInt)((t3 - t2) * tickUs / 1000), (TUint)info().iScreenAddress,
		info().iScreenAddressValid ? _S("valid") : _S("not valid"));
	iEikonEnv->InfoMsg(t);
	}
