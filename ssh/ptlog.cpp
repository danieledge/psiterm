// ptlog.cpp - PsiTerm's log, on the Psion's side of psissh.exe
//
// psiglue.cpp reports what the link is doing (dialling, PPP stages, DNS,
// socket shutdown...) through a sink registered with pg_set_link_log. Until
// 0.89 psissh registered none, so PsiTerm had no log. This file is the sink:
// one line per event, "mm:ss.t text", in C:\System\Data\PsiTerm.log, the
// last run's log kept as PsiTerm.old (as PsiWeb.log and psimail.log).
//
// "Tools > Send log to server" runs psissh in mode 6 (psishim.c run_putlog),
// which only appends to the log (pt_log_open(1)) and reads both files with
// pt_log_size / pt_log_read. The log is opened EFileShareAny so it can be
// read while another psissh is writing it. Every file error is ignored: a
// full disk or a missing card must never stop a session.

#include <e32std.h>
#include <f32file.h>

_LIT(KLogFile, "C:\\System\\Data\\PsiTerm.log");
_LIT(KLogOld, "C:\\System\\Data\\PsiTerm.old");

static RFs gLogFs;
static RFile gLog;
static TInt gLogOpen = 0;       // 0 not tried, 1 open, -1 failed
static TInt gLogAppend = 0;     // 1: keep what is there (Send log)
static TUint gLogT0 = 0;
static TInt gLogBytes = 0;

#define PT_LOG_CAP (400 * 1024)   // a runaway session stops logging, not the Psion

static void LogOpen()
	{
	gLogOpen = -1;
	if (gLogFs.Connect() != KErrNone)
		return;
	gLogFs.MkDirAll(_L("C:\\System\\Data\\"));
	if (gLogAppend)
		{
		if (gLog.Open(gLogFs, KLogFile, EFileWrite | EFileShareAny) == KErrNone)
			{
			TInt pos = 0;
			gLog.Seek(ESeekEnd, pos);
			gLogBytes = pos;
			}
		else if (gLog.Create(gLogFs, KLogFile, EFileWrite | EFileShareAny) != KErrNone)
			{
			gLogFs.Close();
			return;
			}
		}
	else
		{
		gLogFs.Delete(KLogOld);
		gLogFs.Rename(KLogFile, KLogOld);
		if (gLog.Replace(gLogFs, KLogFile, EFileWrite | EFileShareAny) != KErrNone)
			{
			gLogFs.Close();
			return;
			}
		}
	gLogT0 = User::TickCount();
	gLogOpen = 1;
	}

// aAppend: 0 = a new log (the old one becomes PsiTerm.old), 1 = add to it
extern "C" void pt_log_open(int aAppend)
	{
	gLogAppend = aAppend;
	}

extern "C" void pt_log(const char* aText)
	{
	if (gLogOpen == 0)
		LogOpen();
	if (gLogOpen != 1 || !aText || gLogBytes > PT_LOG_CAP)
		return;
	TUint t = (User::TickCount() - gLogT0) * 10 / 64;       // 1/10 s
	TBuf8<160> line;
	line.AppendFormat(_L8("%02u:%02u.%u "), (t / 600) % 100, (t / 10) % 60, t % 10);
	TInt n = 0;
	while (aText[n] && n < 120) n++;
	line.Append((const TUint8*)aText, n);
	line.Append(_L8("\r\n"));
	gLog.Write(line);
	gLog.Flush();
	gLogBytes += line.Length();
	}

static TInt LogOpenRead(RFs& aFs, RFile& aFile, TInt aOld)
	{
	TInt err = aFs.Connect();
	if (err == KErrNone)
		{
		err = aFile.Open(aFs, aOld ? KLogOld : KLogFile, EFileRead | EFileShareAny);
		if (err != KErrNone)
			aFs.Close();
		}
	return err;
	}

// the size of PsiTerm.log (aOld: PsiTerm.old); 0 if there is none
extern "C" long pt_log_size(int aOld)
	{
	RFs fs;
	RFile f;
	TInt size = 0;
	if (LogOpenRead(fs, f, aOld) != KErrNone)
		return 0;
	if (f.Size(size) != KErrNone)
		size = 0;
	f.Close();
	fs.Close();
	return size;
	}

// up to aMax bytes of a log from aPos; the number read (0 at the end)
extern "C" long pt_log_read(int aOld, long aPos, void* aBuf, long aMax)
	{
	RFs fs;
	RFile f;
	if (aMax <= 0 || LogOpenRead(fs, f, aOld) != KErrNone)
		return 0;
	TPtr8 p((TUint8*)aBuf, 0, aMax);
	TInt pos = aPos;
	TInt err = f.Read(pos, p);
	f.Close();
	fs.Close();
	return err == KErrNone ? p.Length() : 0;
	}
