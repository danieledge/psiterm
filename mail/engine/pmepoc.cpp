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
int snprintf(char* str, size_t size, const char* fmt, ...);
int vsnprintf(char* str, size_t size, const char* fmt, va_list ap);
void pmn_idle_tick(void);
extern PsiShared* pg_shared();
extern int pg_attach();
void pm_loop(int (*housekeeping)(void));
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

// ----- log: psimail.log next to the app, at most 32 KB --------------------

static FILE* gLog = 0;
static long gLogLen = 0;

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
	if (!gLog)
		{
		if (gLogLen < 0)
			return;
		char path[160];
		PsiShared* s = pg_shared();
		snprintf(path, sizeof(path), "%s\\psimail.log",
			s && s->home[0] ? s->home : "C:\\System\\Apps\\PsiMail");
		gLog = fopen(path, "w");
		if (!gLog)
			{
			gLogLen = -1;
			return;
			}
		}
	if (gLogLen + n > 32 * 1024)
		return;
	fwrite(b, 1, n, gLog);
	fflush(gLog);
	gLogLen += n;
	}

extern "C" void pm_idle(int aMs)
	{
	User::After(aMs * 1000);
	}

// ----- housekeeping: about once a second ------------------------------------

static TUint gLastCheck = 0;
static TUint gLastBeat = 0;
static TUint gBeatSeen = 0;

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
		}
	else if (now - gBeatSeen > 64 * 20)
		return 1;
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
	pm_log("psimail.exe started");
	pm_loop(housekeeping_c);
	pm_log("psimail.exe ends");
	if (gLog)
		fclose(gLog);
	if (gFs)
		{
		gFs->Close();
		delete gFs;
		}
	delete cleanup;
	return 0;
	}
