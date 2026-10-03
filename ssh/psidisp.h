// psidisp.h - Reading mode and the grey calibration file, for the apps
// (PsiTerm, PsiMail, PsiWeb). Header-only, as psilink.h. See docs/display.md.
//
// Reading mode: while a page, a message or the terminal is being read, the
// contrast goes up a notch or two and the backlight stays on; both come
// back as they were when the program goes to the background, closes, the
// Psion is switched off and on, or Reading mode is turned off. How many
// notches, and whether the light stays on, are in PsiGrey.ini ("reading"),
// set in PsiTerm's Display calibration.
//
// Putting things back:
//   - only what Reading mode itself set: if the contrast is no longer what
//     it set (the user changed it meanwhile), the user's choice stays;
//   - the user's own settings are kept in C:\System\Data\PsiRead.ini while
//     Reading mode is on (written before anything is changed). If the
//     program stops without putting them back (a panic, a kill from the
//     task list, a battery change), the next of the three programs to start
//     finds the file, sees that its owner is not running, and restores them.
//   - two of the programs can hand over (one goes to the background as the
//     other comes forward, in either order): the file keeps the user's own
//     settings from the first; whoever owns the file last restores them.
//
// Uses euser (UserHal), efsrv and apgrfx (TApaTaskList, for the start-up
// check). Nothing here leaves.
#ifndef PSIDISP_H
#define PSIDISP_H

#include <e32hal.h>
#include <f32file.h>
#include <apgtask.h>
#include "psigrey.h"

_LIT(KPsiGreyFile, "C:\\System\\Data\\PsiGrey.ini");
_LIT(KPsiReadFile, "C:\\System\\Data\\PsiRead.ini");

// the shared grey settings (PsiGrey.ini), or the standard ones
inline void PsiGreyLoad(RFs& aFs, PsiGrey& aGrey)
	{
	RFile f;
	TBuf8<512> d;
	if (f.Open(aFs, KPsiGreyFile, EFileRead | EFileShareReadersOnly) == KErrNone)
		{
		if (f.Read(d) != KErrNone)
			d.Zero();
		f.Close();
		}
	if (d.Length())
		psigrey_parse(&aGrey, (const char*)d.Ptr(), d.Length());
	else
		psigrey_defaults(&aGrey);
	}

// writes it safely (a temporary, then renamed over the old one). KErrNone if saved
inline TInt PsiGreySave(RFs& aFs, const PsiGrey& aGrey)
	{
	TBuf8<512> d;
	d.SetLength(psigrey_format(&aGrey, (char*)d.Ptr(), d.MaxLength()));
	aFs.MkDirAll(KPsiGreyFile);
	TFileName tmp(KPsiGreyFile);
	tmp.Append('~');
	RFile f;
	TInt r = f.Replace(aFs, tmp, EFileWrite);
	if (r != KErrNone)
		return r;
	r = f.Write(d);
	if (r == KErrNone)
		r = f.Flush();
	f.Close();
	if (r == KErrNone)
		r = aFs.Replace(tmp, KPsiGreyFile);
	if (r != KErrNone)
		aFs.Delete(tmp);
	return r;
	}

// the file's modification time, to see whether another program changed it
inline TTime PsiGreyModified(RFs& aFs)
	{
	TTime t(0);
	TEntry e;
	if (aFs.Entry(KPsiGreyFile, e) == KErrNone)
		t = e.iModified;
	return t;
	}

// what Reading mode keeps while it is on (PsiRead.ini, 16 bytes)
struct TPsiReadState
	{
	TUint32 iOwner;          // the program's UID
	TInt8 iSavedContrast;    // the user's contrast (-1: not changed)
	TInt8 iSetContrast;      // what Reading mode set
	TUint8 iSavedBehavior;   // the user's backlight behaviour
	TUint8 iSetLight;        // 1: Reading mode set the backlight untimed
	};

class TPsiReading
	{
public:
	TPsiReading() : iActive(EFalse) {}
	TBool Active() const { return iActive; }

	// on: contrast up and light on, as PsiGrey.ini says (nothing if already on)
	void Enter(RFs& aFs, TUid aApp)
		{
		if (iActive)
			return;
		PsiGrey g;
		PsiGreyLoad(aFs, g);
		TPsiReadState s;
		TBool inherited = Read(aFs, s);        // another program's (or a crash's)
		if (!inherited)
			{
			s.iSavedContrast = -1;
			s.iSetContrast = -1;
			s.iSavedBehavior = EBacklightBehaviorTimed;
			s.iSetLight = 0;
			}
		s.iOwner = aApp.iUid;
		TInt c, max;
		if (g.read_contrast > 0 && UserHal::DisplayContrast(c) == KErrNone
			&& UserHal::MaxDisplayContrast(max) == KErrNone)
			{
			// from the user's own setting, not one already raised
			TInt base = inherited && s.iSavedContrast >= 0 && c == s.iSetContrast ? s.iSavedContrast : c;
			TInt want = base + g.read_contrast;
			if (want > max)
				want = max;
			if (!inherited || s.iSavedContrast < 0 || c != s.iSetContrast)
				s.iSavedContrast = (TInt8)c;
			s.iSetContrast = (TInt8)want;
			}
		if (g.read_light)
			{
			TBacklightBehavior b;
			if (!s.iSetLight && UserHal::BacklightBehavior(b) == KErrNone)
				s.iSavedBehavior = (TUint8)b;
			s.iSetLight = 1;
			}
		// the user's settings are on the card before anything changes
		if (Write(aFs, s) != KErrNone)
			return;
		if (s.iSetContrast >= 0)
			UserHal::SetDisplayContrast(s.iSetContrast);
		if (s.iSetLight)
			{
			UserHal::SetBacklightBehavior(EBacklightBehaviorUntimed);
			UserHal::SetBacklightOn(ETrue);
			}
		iActive = ETrue;
		}

	// off: the user's settings back (if this program still owns them)
	void Leave(RFs& aFs, TUid aApp)
		{
		if (!iActive)
			return;
		iActive = EFalse;
		TPsiReadState s;
		if (!Read(aFs, s) || s.iOwner != (TUint32)aApp.iUid)
			return;                            // another program has taken over
		Restore(s);
		aFs.Delete(KPsiReadFile);
		}

	// at start-up: settings left behind by a program that stopped without
	// putting them back (its owner not running, or this program itself)
	static void Recover(RFs& aFs, RWsSession& aWs, TUid aApp)
		{
		TPsiReadState s;
		if (!Read(aFs, s))
			return;
		if (s.iOwner != (TUint32)aApp.iUid)
			{
			TApaTaskList tasks(aWs);
			if (tasks.FindApp(TUid::Uid(s.iOwner)).Exists())
				return;                        // still running: its own business
			}
		Restore(s);
		aFs.Delete(KPsiReadFile);
		}

private:
	static void Restore(const TPsiReadState& aState)
		{
		TInt c;
		if (aState.iSetContrast >= 0 && aState.iSavedContrast >= 0
			&& UserHal::DisplayContrast(c) == KErrNone && c == aState.iSetContrast)
			UserHal::SetDisplayContrast(aState.iSavedContrast);
		TBacklightBehavior b;
		if (aState.iSetLight && UserHal::BacklightBehavior(b) == KErrNone
			&& b == EBacklightBehaviorUntimed && aState.iSavedBehavior != EBacklightBehaviorUntimed)
			UserHal::SetBacklightBehavior((TBacklightBehavior)aState.iSavedBehavior);
		}

	static TBool Read(RFs& aFs, TPsiReadState& aState)
		{
		RFile f;
		if (f.Open(aFs, KPsiReadFile, EFileRead | EFileShareAny) != KErrNone)
			return EFalse;
		TBuf8<16> d;
		TInt r = f.Read(d);
		f.Close();
		if (r != KErrNone || d.Length() < 10 || d[0] != 'P' || d[1] != 'R')
			return EFalse;
		aState.iOwner = d[2] | (d[3] << 8) | (d[4] << 16) | ((TUint32)d[5] << 24);
		aState.iSavedContrast = (TInt8)d[6];
		aState.iSetContrast = (TInt8)d[7];
		aState.iSavedBehavior = d[8] <= EBacklightBehaviorUntimed ? d[8] : EBacklightBehaviorTimed;
		aState.iSetLight = d[9] ? 1 : 0;
		return ETrue;
		}

	static TInt Write(RFs& aFs, const TPsiReadState& aState)
		{
		TBuf8<16> d;
		d.Append('P');
		d.Append('R');
		d.Append((TUint8)(aState.iOwner & 0xff));
		d.Append((TUint8)((aState.iOwner >> 8) & 0xff));
		d.Append((TUint8)((aState.iOwner >> 16) & 0xff));
		d.Append((TUint8)(aState.iOwner >> 24));
		d.Append((TUint8)aState.iSavedContrast);
		d.Append((TUint8)aState.iSetContrast);
		d.Append(aState.iSavedBehavior);
		d.Append(aState.iSetLight);
		aFs.MkDirAll(KPsiReadFile);
		TFileName tmp(KPsiReadFile);
		tmp.Append('~');
		RFile f;
		TInt r = f.Replace(aFs, tmp, EFileWrite);
		if (r != KErrNone)
			return r;
		r = f.Write(d);
		if (r == KErrNone)
			r = f.Flush();
		f.Close();
		if (r == KErrNone)
			r = aFs.Replace(tmp, KPsiReadFile);
		if (r != KErrNone)
			aFs.Delete(tmp);
		return r;
		}

	TBool iActive;
	};

#endif
