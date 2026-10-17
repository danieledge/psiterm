// fwtrial.cpp - see fwtrial.h. MIT licence (see LICENSE at the top of the repository).
#include "fwtrial.h"
#include <string.h>
#include <stdio.h>

namespace am {

static const char* const kTrialKey = "fwtrial";
static const char* const kNoteKey = "fwnote";

void FwTrial::Boot(FwOta& aOta)
	{
	int run = aOta.RunningSlot();
	int st = aOta.State(run);
	uint8_t sha[kFwShaLen];
	bool have = aOta.Sha(run, sha);
	FwTrialRec rec;
	memset(&rec, 0, sizeof(rec));
	bool hr = aOta.Load(kTrialKey, &rec, sizeof(rec)) && rec.magic == kFwTrialMagic;
	char m[120];
	if (st == EFwStPending || st == EFwStNew)
		{
		// just written (the bootloader made it "pending" for this start): the
		// trial begins. Marked valid now, so a failed start comes back here
		// and is counted, rather than the bootloader giving up at the first
		memset(&rec, 0, sizeof(rec));
		rec.magic = kFwTrialMagic;
		rec.slot = (uint8_t)run;
		rec.attempts = 1;
		if (have)
			memcpy(rec.sha, sha, kFwShaLen);
		aOta.Save(kTrialKey, &rec, sizeof(rec));
		aOta.MarkValid();
		iTrial = true;
		iAttempt = 1;
		snprintf(m, sizeof(m), "Firmware: new in slot %d, on trial (start 1 of %d)", run, kMaxAttempts);
		aOta.Log(m);
		}
	else if (hr && rec.attempts > 0 && rec.slot == run && have && memcmp(rec.sha, sha, kFwShaLen) == 0
		&& st == EFwStValid)
		{
		// the same image again, and its last start did not get to healthy
		rec.attempts++;
		if (rec.attempts > kMaxAttempts)
			{
			FwNoteRec n;
			memset(&n, 0, sizeof(n));
			n.magic = kFwNoteMagic;
			n.slot = (uint8_t)run;
			n.attempts = (uint8_t)kMaxAttempts;
			memcpy(n.sha, sha, kFwShaLen);
			aOta.Save(kNoteKey, &n, sizeof(n));
			rec.attempts = 0;
			aOta.Save(kTrialKey, &rec, sizeof(rec));
			snprintf(m, sizeof(m), "Firmware: the new firmware failed to start %d times: going back to the previous one",
				kMaxAttempts);
			aOta.Log(m);
			if (aOta.CanRollBack())
				{
				aOta.MarkInvalidAndReboot();     // (does not return on the Atom)
				return;
				}
			// no otadata entry for the other slot (it was flashed over USB):
			// point the bootloader at it directly, if it holds an image
			if (aOta.BootOther())
				{
				aOta.Restart();
				return;
				}
			memset(&n, 0, sizeof(n));
			aOta.Save(kNoteKey, &n, sizeof(n));
			iGaveUp = true;
			aOta.Log("Firmware: there is no previous firmware to go back to: keeping this one");
			}
		else
			{
			aOta.Save(kTrialKey, &rec, sizeof(rec));
			iTrial = true;
			iAttempt = rec.attempts;
			snprintf(m, sizeof(m), "Firmware: still on trial (start %d of %d)", iAttempt, kMaxAttempts);
			aOta.Log(m);
			}
		}
	else if (hr && rec.attempts)
		{
		rec.attempts = 0;                     // for another image (a USB flash over it): dropped
		aOta.Save(kTrialKey, &rec, sizeof(rec));
		}
	Note(aOta, run);
	}

// "Running the previous firmware": a note from a trial that failed, while
// the failed image is still in the other slot (writing a new firmware over
// it ends the note), or the bootloader's own refusal of a slot at its first
// start (it was marked aborted before this code ran)
void FwTrial::Note(FwOta& aOta, int aRunning)
	{
	FwNoteRec n;
	memset(&n, 0, sizeof(n));
	uint8_t sha[kFwShaLen];
	if (aOta.Load(kNoteKey, &n, sizeof(n)) && n.magic == kFwNoteMagic && n.attempts && n.slot != aRunning
		&& aOta.Sha(n.slot, sha) && memcmp(sha, n.sha, kFwShaLen) == 0)
		{
		iShow = EFwShowPrevious;
		iNoteAttempts = n.attempts;
		return;
		}
	int inv = aOta.LastInvalidSlot();
	if (inv >= 0 && inv != aRunning && (aOta.State(inv) == EFwStAborted || aOta.State(inv) == EFwStInvalid))
		{
		iShow = EFwShowPrevious;
		iNoteAttempts = 1;
		return;
		}
	iShow = iTrial ? EFwShowTrial : EFwShowOk;
	}

bool FwTrial::Tick(FwOta& aOta, uint32_t aNowMs, bool aUplink, bool aWeb, bool aApOnly)
	{
	if (!iTrial)
		return false;
	if (!aWeb && !(aNowMs >= kHealthyAfterMs && (aUplink || aApOnly)))
		return false;
	FwTrialRec rec;
	memset(&rec, 0, sizeof(rec));
	if (aOta.Load(kTrialKey, &rec, sizeof(rec)) && rec.magic == kFwTrialMagic)
		{
		rec.attempts = 0;
		aOta.Save(kTrialKey, &rec, sizeof(rec));
		}
	iTrial = false;
	if (iShow == EFwShowTrial)
		iShow = EFwShowOk;
	char m[96];
	snprintf(m, sizeof(m), "Firmware: the new firmware started properly (start %d): kept", iAttempt);
	aOta.Log(m);
	return true;
	}

void FwTrial::Text(char* aOut, size_t aMax) const
	{
	if (!aMax)
		return;
	if (iShow == EFwShowPrevious)
		{
		if (iNoteAttempts > 1)
			snprintf(aOut, aMax, "Running the previous firmware: the update failed to start %d times", iNoteAttempts);
		else
			snprintf(aOut, aMax, "Running the previous firmware: the update failed to start");
		}
	else if (iTrial)
		snprintf(aOut, aMax, "New firmware on trial: start %d of %d (kept once the network or a page works)",
			iAttempt, kMaxAttempts);
	else if (iGaveUp)
		snprintf(aOut, aMax, "The new firmware failed to start %d times, but there is no previous firmware to go back to",
			kMaxAttempts);
	else
		aOut[0] = 0;
	}

} // namespace am
