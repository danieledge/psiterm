// psikeep.h - settings kept on the card as well as on C:
//
// The rule (Coda's DESIGN.md, 19 November 2026; the same file in PsiMail,
// PsiTerm, PsiWeb and PsiCast): C: is the home of every setting. Each
// program keeps its own copy on the card, without secrets, at the same path
// below \System\ in one folder:
//   C:\System\Apps\PsiMail\PsiMail.ini -> D:\System\Data\Kept\Apps\PsiMail\PsiMail.ini
// and puts a file back from there when C: has none (a new image, a cold
// start, a flat battery). Passwords, private keys, the random seed and
// cookies never go to the card: they stay on C: only (scrambled, not
// encrypted), and are typed or made again after a new C:.
//
// The copy is written only when it differs from what is there (the card is
// written rarely), through <file>~ and RFs::Replace (never a short file); a
// folder made new on the card is made after a file of zeros, so that a
// computer's disk check reads it right (Coda's psimkdir.h, review R5).
// Nothing here leaves; no writable static data (ER5).
#ifndef __PSIKEEP_H
#define __PSIKEEP_H
#include <e32std.h>
#include <f32file.h>

_LIT(KPsiKeptRoot, "D:\\System\\Data\\Kept\\");
const TInt KPsiKeepMax = 64 * 1024;          // a settings file bigger is not kept

// the card copy's name for a file below C:\System\ -> EFalse if it is not one
static TBool PsiKeptName(const TDesC& aC, TDes& aCard)
	{
	_LIT(KSys, "C:\\System\\");
	TInt n = KSys().Length();
	if (aC.Length() <= n || aC.Left(n).CompareF(KSys) != 0)
		return EFalse;
	if (KPsiKeptRoot().Length() + aC.Length() - n > aCard.MaxLength())
		return EFalse;
	aCard.Copy(KPsiKeptRoot);
	aCard.Append(aC.Mid(n));
	return ETrue;
	}

// a card in D: that can be written
static TBool PsiKeepCard(RFs& aFs)
	{
	TDriveInfo d;
	if (aFs.Drive(d, EDriveD) != KErrNone)
		return EFalse;
	if (d.iType == EMediaNotPresent || d.iType == EMediaUnknown || d.iType == EMediaRom)
		return EFalse;
	if (d.iMediaAtt & KMediaAttWriteProtected)
		return EFalse;
	TVolumeInfo v;
	return aFs.Volume(v, EDriveD) == KErrNone;
	}

// a whole small file -> its bytes (NULL: none, too big, no memory)
static HBufC8* PsiKeepRead(RFs& aFs, const TDesC& aPath)
	{
	RFile f;
	if (f.Open(aFs, aPath, EFileRead | EFileShareReadersOnly) != KErrNone)
		return NULL;
	TInt size = 0;
	HBufC8* b = NULL;
	if (f.Size(size) == KErrNone && size >= 0 && size <= KPsiKeepMax)
		{
		b = HBufC8::New(size > 0 ? size : 1);
		if (b)
			{
			TPtr8 p(b->Des());
			if (f.Read(p, size) != KErrNone || p.Length() != size)
				{
				delete b;
				b = NULL;
				}
			}
		}
	f.Close();
	return b;
	}

// the folders of aPath (a file's) made, those on the card after a file of
// zeros in their parent (EPOC leaves a new folder's cluster as it was)
static void PsiKeepMkDirs(RFs& aFs, const TDesC& aPath)
	{
	TChar drive(aPath.Length() ? aPath[0] : 'C');
	drive.UpperCase();
	for (TInt i = 3; i < aPath.Length(); i++)
		{
		if (aPath[i] != '\\')
			continue;
		TEntry e;
		if (aFs.Entry(aPath.Left(i), e) == KErrNone)
			continue;
		if (drive != 'C')
			{
			TInt p = i - 1;
			while (p > 2 && aPath[p] != '\\')
				p--;
			TFileName tmp(aPath.Left(p + 1));
			tmp.Append(_L("~PsiZero.tmp"));
			RFile z;
			if (z.Replace(aFs, tmp, EFileWrite) == KErrNone)
				{
				TBuf8<512> zeros;
				zeros.FillZ(512);
				TInt err = KErrNone;
				for (TInt n = 0; n < 32 * 1024 && err == KErrNone; n += 512)
					err = z.Write(zeros);
				z.Flush();
				z.Close();
				aFs.Delete(tmp);
				}
			}
		aFs.MkDir(aPath.Left(i + 1));
		}
	}

// aData written whole as aPath: <aPath>~ first, then in its place
static TInt PsiKeepWrite(RFs& aFs, const TDesC& aPath, const TDesC8& aData)
	{
	PsiKeepMkDirs(aFs, aPath);
	TFileName tmp(aPath);
	tmp.Append('~');
	RFile f;
	TInt r = f.Replace(aFs, tmp, EFileWrite);
	if (r != KErrNone)
		return r;
	r = f.Write(aData);
	if (r == KErrNone)
		r = f.Flush();
	f.Close();
	if (r == KErrNone)
		r = aFs.Replace(tmp, aPath);
	if (r != KErrNone)
		aFs.Delete(tmp);
	return r;
	}

// the card's copy of aC made aData, if a card is there and the copy is
// not that already -> KErrNone (or nothing to do), or the error
static TInt PsiKeepData(RFs& aFs, const TDesC& aC, const TDesC8& aData)
	{
	TFileName card;
	if (aData.Length() > KPsiKeepMax || !PsiKeptName(aC, card) || !PsiKeepCard(aFs))
		return KErrNone;
	HBufC8* now = PsiKeepRead(aFs, card);
	TBool same = now && *now == aData;
	delete now;
	return same ? KErrNone : PsiKeepWrite(aFs, card, aData);
	}

// the card's copy of aC made the same as the file on C: (none on C:: the
// copy is left as it is)
static TInt PsiKeepFile(RFs& aFs, const TDesC& aC)
	{
	HBufC8* data = PsiKeepRead(aFs, aC);
	if (!data)
		return KErrNone;
	TInt r = PsiKeepData(aFs, aC, *data);
	delete data;
	return r;
	}

// aC missing on C: and its copy on the card: put back -> ETrue if it was
static TBool PsiKeepRestore(RFs& aFs, const TDesC& aC)
	{
	TEntry e;
	if (aFs.Entry(aC, e) == KErrNone)
		return EFalse;
	TFileName card;
	if (!PsiKeptName(aC, card) || !PsiKeepCard(aFs))
		return EFalse;
	HBufC8* data = PsiKeepRead(aFs, card);
	if (!data)
		return EFalse;
	TInt r = PsiKeepWrite(aFs, aC, *data);
	delete data;
	return r == KErrNone;
	}

#endif
