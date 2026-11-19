// (A copy of Coda's psisys/psierr.h, kept the same: one wording and one way of
// writing settings in every Coda program. Change both together.)
// psierr.h: an EPOC error as words for a message (review R105), and the one
// wording for something of Coda's that is on the card (review R103).
//
// Messages say what did not happen and why, in the user's words, never the
// bare number: "Not saved - the disk is full", "Not changed - the card is
// not in". The number goes to the program's log, where there is one.
//
//   TBuf<80> m;
//   PsiErrMsg(m, _L("Not saved"), err);       // "Not saved - the disk is full"
//   iEikonEnv->InfoMsg(m);
//   PsiOnCardMsg(m, _L("Help"));              // "Help is on the Coda card - put the card in"
//
// One table for every Coda program (the card programs keep a copy of the
// same words: CONSISTENCY in CHANGES.md, 19 November 2026). Shared by the
// netBook and the Series 5mx builds; on the 5mx "the card" is the memory
// disk's card all the same. No division, nothing that leaves. (ER5: Left
// past a descriptor's end panics, USER 22: every Left is clipped.)
#ifndef PSIERR_H
#define PSIERR_H
#include <e32std.h>

// the reason, lower case, to follow " - "
inline TPtrC PsiErrWords(TInt aErr)
	{
	switch (aErr)
		{
	case KErrNoMemory:        return _L("not enough memory");
	case KErrDiskFull:        return _L("the disk is full");
	case KErrNotReady:        return _L("the card is not in");
	case KErrInUse:           return _L("it is in use");
	case KErrAccessDenied:    return _L("it is read-only or in use");
	case KErrLocked:          return _L("the card is locked");
	case KErrNotFound:        return _L("not found");
	case KErrPathNotFound:    return _L("folder not found");
	case KErrAlreadyExists:   return _L("it is there already");
	case KErrBadName:         return _L("the name is not allowed");
	case KErrCorrupt:         return _L("the file is damaged");
	case KErrEof:             return _L("the file is cut short");
	case KErrNotSupported:    return _L("not possible here");
	case KErrTooBig:
	case KErrOverflow:        return _L("too big");
	case KErrTimedOut:        return _L("no answer in time");
	case KErrCancel:          return _L("stopped");
	case KErrServerBusy:      return _L("the system is busy - try again");
	case KErrCouldNotConnect: return _L("could not connect");
	case KErrDied:            return _L("a program it needed stopped");
	case KErrDisMounted:      return _L("the card was taken out");
	default:                  return _L("for an unknown reason");
		}
	}

// aWhat + " - " + the reason, cut to the buffer
inline void PsiErrMsg(TDes& aMsg, const TDesC& aWhat, TInt aErr)
	{
	TPtrC why(PsiErrWords(aErr));
	aMsg.Copy(aWhat.Left(Min(aWhat.Length(), Max(0, aMsg.MaxLength() - 3 - why.Length()))));
	aMsg.Append(_L(" - "));
	aMsg.Append(why.Left(Min(why.Length(), aMsg.MaxLength() - aMsg.Length())));
	}

// "<What> is on the Coda card - put the card in" (aWhat starts with a capital)
inline void PsiOnCardMsg(TDes& aMsg, const TDesC& aWhat)
	{
	_LIT(KTail, " is on the Coda card - put the card in");
	aMsg.Copy(aWhat.Left(Min(aWhat.Length(), Max(0, aMsg.MaxLength() - KTail().Length()))));
	aMsg.Append(KTail().Left(Min(KTail().Length(), aMsg.MaxLength() - aMsg.Length())));
	}

#endif
