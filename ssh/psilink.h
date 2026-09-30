// psilink.h - the connection settings PsiTerm, PsiMail and PsiWeb share.
//
// How the Psion reaches the network is the same for every app, so it is set
// once: change it in any app's Connection settings and the others use it
// too. Stored in C:\System\Data\PsiLink.ini (header-only, so each app's
// build just includes it).
//
//   iBaudIndex  0=9600 1=19200 2=38400 3=57600 4=115200
//   iRtsCts     hardware flow control
//   iNetMode    0 = modem ("ATDT host:port"), 1 = the Psion's own TCP/IP
//   iPppStart   Psion TCP/IP only: sent to the modem first (e.g. ATDT777,
//               which puts a WiRSa into PPP); empty = send nothing
#ifndef PSILINK_H
#define PSILINK_H

#include <f32file.h>

_LIT(KPsiLinkFile, "C:\\System\\Data\\PsiLink.ini");

struct TPsiLink
	{
	TInt iBaudIndex;
	TInt iRtsCts;
	TInt iNetMode;
	TBuf<40> iPppStart;

	void SetDefaults()
		{
		iBaudIndex = 4;
		iRtsCts = 0;
		iNetMode = 0;
		iPppStart.Copy(_L("ATDT777"));
		}

	// ETrue if the shared file was there and readable
	TBool Load(RFs& aFs)
		{
		RFile f;
		if (f.Open(aFs, KPsiLinkFile, EFileRead | EFileShareReadersOnly) != KErrNone)
			return EFalse;
		TBuf8<64> d;
		TInt r = f.Read(d);
		f.Close();
		if (r != KErrNone || d.Length() < 6 || d[0] != 'P' || d[1] != 'L' || d[2] < 1)
			return EFalse;
		iBaudIndex = d[3] <= 4 ? d[3] : 4;
		iRtsCts = d[4] ? 1 : 0;
		iNetMode = d[5] ? 1 : 0;
		iPppStart.Zero();
		if (d.Length() >= 7)
			{
			TInt len = d[6];
			if (len <= 40 && 7 + len <= d.Length())
				iPppStart.Copy(d.Mid(7, len));
			}
		return ETrue;
		}

	// Writes it safely (to a temporary name, then renamed over the old one)
	void Save(RFs& aFs) const
		{
		TBuf8<64> d;
		d.Append('P');
		d.Append('L');
		d.Append(1);                           // version
		d.Append((TUint8)iBaudIndex);
		d.Append((TUint8)(iRtsCts ? 1 : 0));
		d.Append((TUint8)(iNetMode ? 1 : 0));
		TBuf8<40> p;
		p.Copy(iPppStart);
		d.Append((TUint8)p.Length());
		d.Append(p);
		aFs.MkDirAll(KPsiLinkFile);
		TFileName tmp(KPsiLinkFile);
		tmp.Append('~');
		RFile f;
		if (f.Replace(aFs, tmp, EFileWrite) != KErrNone)
			return;
		TInt r = f.Write(d);
		if (r == KErrNone)
			r = f.Flush();
		f.Close();
		if (r == KErrNone)
			aFs.Replace(tmp, KPsiLinkFile);
		else
			aFs.Delete(tmp);
		}
	};

#endif
