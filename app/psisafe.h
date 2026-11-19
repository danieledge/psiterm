// (A copy of Coda's psisys/psisafe.h, kept the same: one wording and one way of
// writing settings in every Coda program. Change both together.)
// psisafe.h: a settings file written beside itself and put in its place
// only when all of it is written, as PsiPower.dat, the start-up list and the
// Snapshot already were. A full C:, no memory or a reset part way through a
// save leave the old file as it was; written in place (RFile::Replace, then
// Write unchecked), they left a short file that the next start read as
// damaged, and the settings went back to the standard ones.
//
// The temporary is the name with "~" after it ("C:\System\Data\PsiHub.ini~"),
// in the same folder, so the swap is a rename on the same disk. A temporary
// left by a reset is written over by the next save and never read.
//
// Small on the stack (the name is on the heap): Control panel items use it
// on the System screen's 12 KB stack.
//
//   RPsiSafeFile f;
//   if (f.Open(fs, KName) == KErrNone)
//       { f.Write(a); f.Write(b); err = f.Commit(); }
// or, for one buffer, PsiSafeWrite(fs, KName, buf).
//
// Shared by the netBook and the Series 5mx builds (no machine differences).
#ifndef PSISAFE_H
#define PSISAFE_H

#include <f32file.h>

class RPsiSafeFile
	{
public:
	inline RPsiSafeFile() : iFs(NULL), iTemp(NULL), iErr(KErrNone) {}
	// the temporary made (its folder too); an error and nothing open otherwise
	inline TInt Open(RFs& aFs, const TDesC& aName);
	// writes after a failed one do nothing; Commit returns the first failure
	inline void Write(const TDesC8& aData)
		{
		if (iTemp && iErr == KErrNone)
			iErr = iFile.Write(aData);
		}
	// for a caller's own writer (a stream on it): say its failure with SetError
	inline RFile& File() { return iFile; }
	inline void SetError(TInt aErr)
		{
		if (iErr == KErrNone)
			iErr = aErr;
		}
	// flushed, closed and put in the file's place; on any failure the old
	// file stays and the temporary goes
	inline TInt Commit();
	// closed, the temporary deleted, the old file left (safe to call twice)
	inline void Abandon();
	inline ~RPsiSafeFile() { Abandon(); }
private:
	inline TPtrC Target() const { return iTemp->Left(iTemp->Length() - 1); }
	RFs* iFs;
	RFile iFile;
	HBufC* iTemp;
	TInt iErr;
	};

inline TInt RPsiSafeFile::Open(RFs& aFs, const TDesC& aName)
	{
	Abandon();
	iFs = &aFs;
	iErr = KErrNone;
	iTemp = HBufC::New(aName.Length() + 1);
	if (!iTemp)
		return KErrNoMemory;
	TPtr t(iTemp->Des());
	t.Copy(aName);
	t.Append('~');
	aFs.MkDirAll(aName);                    // (KErrAlreadyExists when there)
	TInt err = iFile.Replace(aFs, *iTemp, EFileWrite);
	if (err != KErrNone)
		{
		delete iTemp;
		iTemp = NULL;
		}
	return err;
	}

inline TInt RPsiSafeFile::Commit()
	{
	if (!iTemp)
		return iErr != KErrNone ? iErr : KErrNotReady;
	TInt err = iErr;
	if (err == KErrNone)
		err = iFile.Flush();
	iFile.Close();
	if (err == KErrNone)
		err = iFs->Replace(*iTemp, Target());
	if (err != KErrNone)
		iFs->Delete(*iTemp);
	delete iTemp;
	iTemp = NULL;
	return err;
	}

inline void RPsiSafeFile::Abandon()
	{
	if (!iTemp)
		return;
	iFile.Close();
	iFs->Delete(*iTemp);
	delete iTemp;
	iTemp = NULL;
	}

// one buffer, the whole file
inline TInt PsiSafeWrite(RFs& aFs, const TDesC& aName, const TDesC8& aData)
	{
	RPsiSafeFile f;
	TInt err = f.Open(aFs, aName);
	if (err != KErrNone)
		return err;
	f.Write(aData);
	return f.Commit();
	}

#endif
