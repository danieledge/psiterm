// psibell.h - doorbells between an app and its engine (0.81, for power)
//
// The 5mx's CPU halts, and saves the batteries, only while every thread is
// waiting on a request. The apps and engines used to look at the shared
// chunk every tick (1/64 s) to see whether the other side had left keys,
// output or a command there, so an idle SSH session kept the CPU out of
// HALT about 130 times a second. With a doorbell each side waits on a
// request instead, and the other side completes it when it has left
// something: RThread::RequestComplete on a thread of another process, the
// way a server completes a client's request (e32/euthrd, e32/euasyn).
//
// The bell (PsiBell, psishared.h) has one waiter and one ringer.
//  - The waiter sets its TRequestStatus to KRequestPending, writes the
//    status's address and its thread id into the bell, and then armed = 1.
//  - The ringer adds 1 to seq, then swaps armed with 0 (SWP: atomic). Only
//    the side that takes the 1 completes the status, so it completes once.
//  - A waiter that stops waiting swaps armed with 0 too. If it gets the 1,
//    nobody else will touch its status; if it gets 0, the ringer has taken
//    it and completes it at once, so the waiter must take that completion.
//  - The ringer keeps a handle on the waiter's thread, and opens it before
//    it takes the 1, so once it has the 1 nothing can stop it completing.
//    If it cannot open one it leaves armed alone and sets broken, and the
//    waiter falls back to looking every tick or so.
//  - The ringer's process can still die between taking the 1 and
//    completing (an engine killed, an app gone). So a waiter that must take
//    the ringer's completion waits for it in slices, and if the ringer
//    (rtid) has gone, completes its own status (PsiBellTakeBack).
//  - A ring that comes before the waiter has armed is not lost: seq has
//    changed, and the waiter looks at seq after arming.
// Every wait on a bell also has a timer, so a missed ring costs a delay of
// at most that long (a second or two), never a hang.
#ifndef PSIBELL_H
#define PSIBELL_H

#include <e32std.h>
#include <e32base.h>
extern "C" {
#include "psishared.h"
}

// The atomic exchange: ARM's SWP (ARMv3 and later, so the ARM710T has it).
inline int PsiBellSwap(volatile int* aAddr, int aNew)
	{
#ifdef __MARM__
	int old;
	__asm__ __volatile__("swp %0, %2, [%1]" : "=&r"(old) : "r"(aAddr), "r"(aNew) : "memory");
	return old;
#else
	int old = *aAddr;          // (host builds: one thread)
	*aAddr = aNew;
	return old;
#endif
	}

// Thread ids as the bell keeps them (TThreadId has no conversions on ER5)
inline TUint PsiBellMyTid()
	{
	TThreadId id = RThread().Id();
	TUint v = 0;
	Mem::Copy(&v, &id, sizeof(v));
	return v;
	}

// Takes the completion of aStatus after the ringer of aBell has taken
// 'armed' (the waiter's swap got 0). On return aStatus has completed and its
// signal has been taken. Normally the ringer completes it at once; if the
// ringer's thread has died before it could, the status is completed here.
inline void PsiBellTakeBack(PsiBell* aBell, TRequestStatus& aStatus)
	{
	RTimer timer;
	TBool timed = timer.CreateLocal() == KErrNone;
	while (aStatus == KRequestPending)
		{
		if (!timed)
			{
			User::After(100000);             // (no timer: look ten times a second)
			}
		else
			{
			TRequestStatus ts;
			timer.After(ts, 500000);
			User::WaitForRequest(aStatus, ts);
			if (aStatus != KRequestPending)
				{
				timer.Cancel();
				User::WaitForRequest(ts);
				return;                      // the ringer's completion, signal taken
				}
			}
		if (aStatus != KRequestPending)
			break;
		// still nothing: is the ringer alive?
		RThread ringer;
		TThreadId id;
		TUint tid = aBell->rtid;
		Mem::Copy(&id, &tid, sizeof(id));
		TBool alive = ringer.Open(id) == KErrNone;
		if (alive)
			{
			alive = ringer.ExitType() == EExitPending;
			ringer.Close();
			}
		if (!alive && aStatus == KRequestPending)
			{
			TRequestStatus* st = &aStatus;
			User::RequestComplete(st, KErrCancel);
			}
		}
	if (timed)
		timer.Close();
	User::WaitForRequest(aStatus);           // its signal (it has completed)
	}

// The ringer's side. Keep one per bell, for the life of the session.
class TPsiBellRinger
	{
public:
	TPsiBellRinger() : iTid(0), iOpen(EFalse) {}
	void Close()
		{
		if (iOpen)
			iThread.Close();
		iOpen = EFalse;
		iTid = 0;
		}
	// Rings aBell. ETrue if it completed the waiter's request.
	TBool Ring(PsiBell* aBell)
		{
		if (!aBell)
			return EFalse;
		aBell->seq++;
		if (aBell->armed != 1)
			return EFalse;                   // nobody waiting (the usual case: cheap)
		TUint tid = aBell->tid;
		if (!iOpen || iTid != tid)
			{
			Close();
			TThreadId id;
			Mem::Copy(&id, &tid, sizeof(id));
			if (iThread.Open(id) == KErrNone)
				{
				iOpen = ETrue;
				iTid = tid;
				}
			}
		if (!iOpen)
			{
			aBell->broken = 1;               // armed stays: the waiter disarms as usual
			return EFalse;
			}
		aBell->rtid = PsiBellMyTid();
		if (PsiBellSwap(&aBell->armed, 0) != 1)
			return EFalse;                   // the waiter has just stopped waiting
	if (iThread.ExitType() != EExitPending)
			return EFalse;                   // the waiter has gone (killed while waiting): nothing to wake
		TRequestStatus* st = (TRequestStatus*)aBell->status;
		aBell->rings++;
		iThread.RequestComplete(st, KErrNone);
		return ETrue;
		}
private:
	RThread iThread;
	TUint iTid;
	TBool iOpen;
	};

// The waiter's side in an app: an active object whose RunL calls back.
// Arm() when going quiet; the callback runs when the engine rings.
class CPsiBellWaiter : public CActive
	{
public:
	CPsiBellWaiter(TCallBack aCallBack) : CActive(EPriorityStandard), iBell(0), iCallBack(aCallBack), iSeen(0)
		{
		CActiveScheduler::Add(this);
		}
	~CPsiBellWaiter() { Cancel(); }
	// Waits on aBell. ETrue if something was rung since the last wait, and
	// the caller should look now (it is then not armed).
	TBool Arm(PsiBell* aBell)
		{
		if (IsActive())
			return EFalse;
		if (!aBell || aBell->broken)
			return EFalse;
		iBell = aBell;
		iStatus = KRequestPending;
		SetActive();
		aBell->status = &iStatus;
		aBell->tid = PsiBellMyTid();
		aBell->armed = 1;
		if (aBell->seq != iSeen)
			{
			Cancel();                        // rung meanwhile: look now
			return ETrue;
			}
		return EFalse;
		}
	// The bell can't be relied on (no thread handle): poll instead.
	TBool Broken() const { return iBell && iBell->broken; }
private:
	void RunL()
		{
		if (iBell)
			{
			iBell->wakes++;
			iSeen = iBell->seq;
			}
		iCallBack.CallBack();
		}
	void DoCancel()
		{
		if (!iBell)
			{
			TRequestStatus* st = &iStatus;
			User::RequestComplete(st, KErrCancel);
			return;
			}
		if (PsiBellSwap(&iBell->armed, 0) == 1)
			{
			TRequestStatus* st = &iStatus;   // ours again: nobody else completes it
			User::RequestComplete(st, KErrCancel);
			}
		else
			{
			// the ringer has it: take its completion (bounded, in case the
			// engine died meanwhile), then signal again for Cancel() to take
			PsiBellTakeBack(iBell, iStatus);
			TRequestStatus* st = &iStatus;
			User::RequestComplete(st, iStatus.Int());
			}
		iSeen = iBell->seq;
		}
	PsiBell* iBell;
	TCallBack iCallBack;
	TUint iSeen;
	};

#endif
