// pmepoc.cpp - psimail.exe on the Psion: the chunk, the clock, files, and
// the main loop (see pmmain.c). The chunk is created by PsiMail.app and
// opened by psiglue.cpp's pg_attach(): it starts with the PsiShared the
// network code uses.

#include <e32std.h>
#include <e32base.h>
#include <e32hal.h>
#include <f32file.h>

extern "C" {
#include "psimail.h"
#include <stdio.h>
#include <stdarg.h>
#include <sys/reent.h>                   // CloseSTDLIB
int snprintf(char* str, size_t size, const char* fmt, ...);
int vsnprintf(char* str, size_t size, const char* fmt, va_list ap);
void pmn_idle_tick(void);
void pm_log(const char* aFmt, ...);
extern PsiShared* pg_shared();
extern int pg_attach();
extern int pg_bells();
extern int pg_bell_wait(int aMs);
extern void pg_ring_app();
void pm_loop(int (*housekeeping)(void));
extern int pm_replace_busy;              // (pmmain.c) the last pm_replace failed because the file was in use
}

static RFs* gFs = 0;
static PmShared* gPm = 0;
static PmShared* gDummy = 0;

extern "C" PmShared* pm_shared()
	{
	if (gPm)
		return gPm;
	PmShared* s = (PmShared*)pg_shared();
	if (s && s->magic == PM_MAGIC)
		return gPm = s;
	if (!gDummy)
		{
		gDummy = (PmShared*)User::Alloc(sizeof(PmShared));
		if (gDummy)
			Mem::FillZ(gDummy, sizeof(PmShared));
		}
	return gDummy;
	}

extern "C" unsigned long pm_ms()
	{
	TTime t;
	t.UniversalTime();
	TInt64 ms = t.Int64() / TInt64(1000);
	return ms.Low();
	}

extern "C" long pm_time()
	{
	TTime now;
	now.UniversalTime();
	TTime epoch(TDateTime(1970, EJanuary, 0, 0, 0, 0, 0));
	TTimeIntervalSeconds s;
	if (now.SecondsFrom(epoch, s) != KErrNone)
		return 0;
	return s.Int();
	}

static RFs& Fs()
	{
	if (!gFs)
		{
		gFs = new RFs;
		if (gFs && gFs->Connect() != KErrNone)
			{
			delete gFs;
			gFs = 0;
			}
		}
	return *gFs;
	}

// an 8-bit C path -> TFileName
static void ToName(TFileName& aName, const char* aPath)
	{
	TPtrC8 p((const TUint8*)aPath);
	aName.Copy(p.Left(p.Length() < KMaxFileName ? p.Length() : KMaxFileName));
	}

extern "C" int pm_mkdir(const char* aPath)
	{
	if (!gFs && (Fs(), !gFs))
		return -1;
	TFileName n;
	ToName(n, aPath);
	if (n.Length() && n[n.Length() - 1] != '\\')
		n.Append('\\');
	TInt r = Fs().MkDirAll(n);
	return (r == KErrNone || r == KErrAlreadyExists) ? 0 : -1;
	}

extern "C" int pm_list_dir(const char* aDir, const char* aSuffix, void (*aCb)(const char*, void*), void* aCtx)
	{
	if (!gFs && (Fs(), !gFs))
		return -1;
	TFileName n;
	ToName(n, aDir);
	n.Append('*');
	TPtrC8 suf((const TUint8*)aSuffix);
	TBuf<16> s;
	s.Copy(suf.Left(suf.Length() < 16 ? suf.Length() : 16));
	n.Append(s);
	CDir* dir = 0;
	if (Fs().GetDir(n, KEntryAttNormal, ESortByName, dir) != KErrNone || !dir)
		return -1;
	TInt count = dir->Count();
	for (TInt i = 0; i < count; i++)
		{
		TBuf8<KMaxFileName + 1> name;
		name.Copy((*dir)[i].iName);
		name.ZeroTerminate();
		aCb((const char*)name.Ptr(), aCtx);
		}
	delete dir;
	return count;
	}

extern "C" void pm_rmtree(const char* aDir)
	{
	if (!gFs && (Fs(), !gFs))
		return;
	TFileName n;
	ToName(n, aDir);
	if (n.Length() && n[n.Length() - 1] != '\\')
		n.Append('\\');
	CFileMan* fm = 0;
	TRAPD(err, fm = CFileMan::NewL(Fs()));
	if (err == KErrNone && fm)
		{
		fm->RmDir(n);
		delete fm;
		}
	}

extern "C" int pm_write_whole(const char* aPath, const void* aData, long aLen)
	{
	if (!gFs && (Fs(), !gFs))
		return -1;
	TFileName n;
	ToName(n, aPath);
	RFile f;
	TInt r = f.Replace(Fs(), n, EFileWrite | EFileShareExclusive);
	if (r != KErrNone)
		return -1;
	TPtrC8 p((const TUint8*)aData, aLen);
	r = f.Write(p);
	if (r == KErrNone)
		r = f.Flush();
	f.Close();
	if (r != KErrNone)
		Fs().Delete(n);
	return r == KErrNone ? 0 : -1;
	}

// tmp takes path's place in one file-server call (RFs::Replace), so there is
// no moment when neither exists. The app may have the old file open to read
// it (a message being shown, the list): then it is tried again, for up to
// three seconds - parsing a long index.txt from the card on an ARM710 can
// take longer than the 300 ms this used to allow, and a failure here was
// blamed on the card ("is the card in...?"). pm_replace_in_use says whether
// the last failure was that, so pm_write_why can say so instead.
extern "C" int pm_replace(const char* aTmp, const char* aPath)
	{
	pm_replace_busy = 0;
	if (!gFs && (Fs(), !gFs))
		return -1;
	TFileName from, to;
	ToName(from, aTmp);
	ToName(to, aPath);
	TInt r = KErrNone;
	for (TInt i = 0; i < 30; i++)
		{
		r = Fs().Replace(from, to);
		if (r == KErrNone)
			return 0;
		if (r != KErrInUse && r != KErrAccessDenied)
			break;
		User::After(100000);
		}
	pm_replace_busy = (r == KErrInUse || r == KErrAccessDenied);
	pm_log("replace %s -> %s: %d%s", aTmp, aPath, r, pm_replace_busy ? " (the file is in use)" : "");
	return -1;
	}

// free space (KB) on the drive the path is on: "D:\..." -> D
extern "C" long pm_free_kb(const char* aPath)
	{
	if (!aPath || !aPath[0] || aPath[1] != ':' || (!gFs && (Fs(), !gFs)))
		return -1;
	TInt drive;
	if (RFs::CharToDrive(TChar((TUint)(unsigned char)aPath[0]), drive) != KErrNone)
		return -1;
	TVolumeInfo v;
	if (Fs().Volume(v, drive) != KErrNone)
		return -1;
	TInt64 kb = v.iFree / TInt64(1024);
	return kb.High() ? 0x7fffffff : (long)kb.Low();
	}

// ----- log: psimail.log next to the app ------------------------------------
// At most 32 KB, then it starts again with the last 32 KB kept as
// psimail.old; a new engine (a restart after a crash too) moves the last
// one's log there first, so what led up to a crash is still there to read.

static FILE* gLog = 0;
static long gLogLen = 0;
static TUint gLogFailAt = 0;             // when opening the log last failed (0 = never): tried again a minute later

static void LogPath(char* aOut, int aMax, const char* aName)
	{
	PsiShared* s = pg_shared();
	snprintf(aOut, aMax, "%s\\%s", s && s->home[0] ? s->home : "C:\\System\\Apps\\PsiMail", aName);
	}

static void LogKeepOld()
	{
	if (!gFs && (Fs(), !gFs))
		return;
	char a[160], b[160];
	LogPath(a, sizeof(a), "psimail.log");
	LogPath(b, sizeof(b), "psimail.old");
	TFileName from, to;
	ToName(from, a);
	ToName(to, b);
	Fs().Delete(to);
	Fs().Rename(from, to);
	}

extern "C" void pm_log(const char* aFmt, ...)
	{
	char b[400];
	va_list ap;
	va_start(ap, aFmt);
	int n = vsnprintf(b, sizeof(b) - 2, aFmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof(b) - 2) n = sizeof(b) - 2;
	b[n++] = '\n';
	b[n] = 0;
	if (gLog && gLogLen + n > 32 * 1024)
		{
		fclose(gLog);
		gLog = 0;
		LogKeepOld();
		}
	if (!gLog)
		{
		// could not be opened (the card out for a moment, C: full): tried
		// again a minute later rather than never - the sessions that go
		// wrong are the ones whose log is wanted
		TUint now = User::TickCount();
		if (gLogFailAt && now - gLogFailAt < 64 * 60)
			return;
		char path[160];
		LogPath(path, sizeof(path), "psimail.log");
		gLog = fopen(path, gLogFailAt ? "a" : "w");
		if (!gLog)
			{
			gLogFailAt = now ? now : 1;
			return;
			}
		gLogFailAt = 0;
		gLogLen = 0;
		}
	// the time, so a long gap (a decode, a stall) shows
	char t[16];
	TUint ms = (TUint)pm_ms();
	int tn = snprintf(t, sizeof(t), "%02u:%02u.%01u ", (ms / 60000) % 60, (ms / 1000) % 60, (ms / 100) % 10);
	fwrite(t, 1, tn, gLog);
	fwrite(b, 1, n, gLog);
	fflush(gLog);
	gLogLen += n + tn;
	}

// (0.81) Nothing to do. The app's news first: a command has finished,
// files have changed, the line has gone - its doorbell (psibell.h), which
// it waits on when it has nothing going on. Then, with the doorbells, wait
// until the app rings (a command, quit, Stop, a switch-on) for up to 2 s
// (housekeeping), instead of looking 10 times a second.
static TUint gRungDone = 0, gRungChanged = 0;
static TInt gRungOnline = -1, gRungBusy = -1, gRungState = -1;

extern "C" void pm_idle(int aMs)
	{
	PmShared* s = pm_shared();
	if (s->done_seq != gRungDone || s->changed_seq != gRungChanged || s->online != gRungOnline
		|| s->busy != gRungBusy || s->state != gRungState)
		{
		gRungDone = s->done_seq;
		gRungChanged = s->changed_seq;
		gRungOnline = s->online;
		gRungBusy = s->busy;
		gRungState = s->state;
		pg_ring_app();
		}
	if (pg_bells())
		pg_bell_wait(2000);
	else
		User::After(aMs * 1000);
	}

// ----- housekeeping: about once a second ------------------------------------

static TUint gLastCheck = 0;
static TUint gLastBeat = 0;
static TUint gBeatSeen = 0;
static TBool gAppBusyLogged = EFalse;

// (0.75) while the engine works - a command, a download ahead - the app's
// heartbeat may not have been seen (the engine runs above it): only time the
// engine spends idle counts towards "PsiMail.app has gone"
extern "C" void pm_beat_reset()
	{
	gBeatSeen = User::TickCount();
	gLastBeat = pm_shared()->app_beat;
	}

// below the app while a picture is set out (see pm.h)
extern "C" void pm_cpu_low(int aLow)
	{
	RThread().SetPriority(aLow ? EPriorityAbsoluteBackground : EPriorityNormal);
	}

// Is PsiMail.app still there? Only a process that has gone (or ended) is
// "gone": a busy app (a long dialog, printing) is not.
static TBool AppGone(PmShared* s)
	{
	if (!s->app_pid)
		return ETrue;                         // (an older app: the heartbeat alone)
	TProcessId id;
	Mem::Copy(&id, (const void*)&s->app_pid, sizeof(id));
	RProcess p;
	TInt r = p.Open(id);
	if (r == KErrNotFound)
		return ETrue;
	if (r != KErrNone)
		return EFalse;
	TBool gone = p.ExitType() != EExitPending;
	p.Close();
	return gone;
	}

static int Housekeeping()
	{
	PmShared* s = pm_shared();
	TUint now = User::TickCount();          // 1/64 s
	if (now - gLastCheck < 64)
		return 0;
	gLastCheck = now;
	pmn_idle_tick();
	TInt used = 0;
	User::Heap().AllocSize(used);
	s->heap_used = used;
	// quit if PsiMail.app has gone (its heartbeat stopped): never leave the
	// engine holding the serial port
	if (s->app_beat != gLastBeat || gBeatSeen == 0)
		{
		gLastBeat = s->app_beat;
		gBeatSeen = now;
		gAppBusyLogged = EFalse;
		}
	else if (now - gBeatSeen > 64 * 20)
		{
		if (AppGone(s))
			{
			pm_log("PsiMail.app has gone (no heartbeat for %u s): the engine quits", (now - gBeatSeen) / 64);
			return 1;
			}
		if (!gAppBusyLogged)
			pm_log("no heartbeat from PsiMail.app for %u s, but it is still there: carrying on", (now - gBeatSeen) / 64);
		gAppBusyLogged = ETrue;
		}
	return 0;
	}

extern "C" int housekeeping_c()
	{
	return Housekeeping();
	}

int main(int, char**)
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	if (pg_attach() != 0 || pm_shared() != gPm || !gPm)
		{
		// not started by PsiMail.app
		delete cleanup;
		return 1;
		}
	LogKeepOld();                            // (the last engine's log: psimail.old)
	{
	TMemoryInfoV1Buf mi;
	TInt freeRam = UserHal::MemoryInfo(mi) == KErrNone ? mi().iFreeRamInBytes : 0;
	pm_log("psimail.exe %s started (RAM free %d KB)", gPm->net.version, freeRam / 1024);
	}
	if (gPm->app_note[0])
		pm_log("the last mail engine ended: %s", gPm->app_note);
	pm_loop(housekeeping_c);
	pm_log("psimail.exe ends (%s)", gPm->quitting ? "PsiMail asked" : "PsiMail.app has gone or said quit");
	if (gLog)
		fclose(gLog);
	if (gFs)
		{
		gFs->Close();
		delete gFs;
		}
	CloseSTDLIB();                           // the thread's C library state (stdio, errno): robustness notes, section 8
	delete cleanup;
	return 0;
	}
