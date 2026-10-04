// PMCAL.CPP - PsiMail's calendar sync with the Psion's Agenda (see pmcal.h)
//
// One pass, a few entries at a time so the app stays responsive:
//   1. what became of the changes sent last time (pushed.txt)
//   2. every entry we look after: changed on the server? changed or deleted
//      on the Psion? (server wins when both changed)
//   3. new server events -> new Agenda entries
//   4. new Psion entries (made since sync was turned on) -> the server
// Then the links are saved (agenda.txt) and the changes for the server are
// left in push.txt; the app asks the engine to send them.

#include <e32std.h>
#include <s32file.h>
#include <eikenv.h>
#include <txtrich.h>
#include <txtfmlyr.h>
#include <agmmodel.h>
#include <agclient.h>
#include <agmsiter.h>
#include <agmentry.h>
#include "pmcal.h"

_LIT(KMapFile, "agenda.txt");
_LIT(KEventsFile, "events.txt");
_LIT(KPushFile, "push.txt");
_LIT(KPushedFile, "pushed.txt");
_LIT(KCalsFile, "calendars.txt");
_LIT8(KMapMagic, "#PSIAGN1");

const TUint KMapPending = 1;       // made on the Psion, sent, not yet confirmed
const TUint KMapDeleting = 2;      // deleted on the Psion, deletion sent
const TUint KMapForce = 4;         // the server refused a change: put its version back

const TInt KStep = 6;              // entries per step

// ---------------------------------------------------------------- helpers

static TUint32 Fnv(const TDesC8& aText)
	{
	TUint32 h = 2166136261u;
	for (TInt i = 0; i < aText.Length(); i++)
		{
		h ^= aText[i];
		h *= 16777619u;
		}
	return h;
	}

static TInt Num(const TDesC8& aText, TInt aPos, TInt aLen)
	{
	TInt v = 0;
	for (TInt i = aPos; i < aPos + aLen && i < aText.Length(); i++)
		{
		TInt c = aText[i];
		if (c < '0' || c > '9') return v;
		v = v * 10 + c - '0';
		}
	return v;
	}

// "YYYYMMDDHHMM" -> TTime
static TTime ParseTime(const TDesC8& aText)
	{
	if (aText.Length() < 8)
		return Time::NullTTime();
	TInt y = Num(aText, 0, 4), m = Num(aText, 4, 2), d = Num(aText, 6, 2);
	TInt hh = Num(aText, 8, 2), mm = Num(aText, 10, 2);
	if (m < 1 || m > 12 || d < 1 || d > 31)
		return Time::NullTTime();
	return TTime(TDateTime(y, TMonth(m - 1), d - 1, hh, mm, 0, 0));
	}

static void AppendTime(TDes8& aOut, const TTime& aTime)
	{
	TDateTime d = aTime.DateTime();
	aOut.AppendFormat(_L8("%04d%02d%02d%02d%02d"), d.Year(), d.Month() + 1, d.Day() + 1, d.Hour(), d.Minute());
	}

static TTime Midnight(const TTime& aTime)
	{
	TDateTime d = aTime.DateTime();
	return TTime(TDateTime(d.Year(), d.Month(), d.Day(), 0, 0, 0, 0));
	}

static void AppendHex64(TDes8& aOut, const TInt64& aValue)
	{
	aOut.AppendNumFixedWidth((TUint)aValue.High(), EHex, 8);
	aOut.AppendNumFixedWidth(aValue.Low(), EHex, 8);
	}

static TInt64 ParseHex64(const TDesC8& aText)
	{
	TUint hi = 0, lo = 0;
	if (aText.Length() >= 16)
		{
		TLex8 a(aText.Left(8)), b(aText.Mid(8, 8));
		a.Val(hi, EHex);
		b.Val(lo, EHex);
		}
	return TInt64((TInt)hi, lo);
	}

static TUint32 ParseHex(const TDesC8& aText)
	{
	TUint v = 0;
	TLex8 l(aText);
	l.Val(v, EHex);
	return v;
	}

// text for our tab-separated files: no tabs or line ends
static void AppendClean(TDes8& aOut, const TDesC8& aText)
	{
	for (TInt i = 0; i < aText.Length() && aOut.Length() < aOut.MaxLength(); i++)
		{
		TUint c = aText[i];
		aOut.Append(c < 32 ? (TChar)' ' : (TChar)c);
		}
	}

// ---------------------------------------------------------------- setup

CPmCalSync* CPmCalSync::NewL(MPmCalObserver& aObserver)
	{
	CPmCalSync* self = new(ELeave) CPmCalSync(aObserver);
	CActiveScheduler::Add(self);
	return self;
	}

CPmCalSync::CPmCalSync(MPmCalObserver& aObserver)
	: CActive(EPriorityIdle), iObserver(aObserver), iPhase(EIdle)
	{
	}

CPmCalSync::~CPmCalSync()
	{
	Cancel();
	Close();
	Reset();
	}

void CPmCalSync::DefaultAgendaFile(TDes& aFile)
	{
	aFile = _L("C:\\Documents\\Agenda");
	}

void CPmCalSync::Path(const TDesC& aName, TDes& aPath) const
	{
	aPath = iDir;
	aPath.Append(aName);
	}

void CPmCalSync::Reset()
	{
	TInt i;
	if (iEvents)
		{
		for (i = 0; i < iEvents->Count(); i++) delete (*iEvents)[i].iLine;
		delete iEvents;
		iEvents = NULL;
		}
	if (iMap)
		{
		for (i = 0; i < iMap->Count(); i++) delete (*iMap)[i].iKey;
		delete iMap;
		iMap = NULL;
		}
	if (iPush)
		{
		iPush->ResetAndDestroy();
		delete iPush;
		iPush = NULL;
		}
	delete iWritable;
	iWritable = NULL;
	}

void CPmCalSync::Close()
	{
	delete (CAgnSyncIter*)iIter;
	iIter = NULL;
	delete iModel;
	iModel = NULL;
	if (iServ)
		{
		if (iAgendaOpen)
			iServ->CloseAgenda();
		if (iServConnected)
			iServ->Close();
		delete iServ;
		iServ = NULL;
		}
	iAgendaOpen = iServConnected = EFalse;
	delete iPara;
	iPara = NULL;
	delete iChar;
	iChar = NULL;
	if (iFsOpen)
		{
		iFs.Close();
		iFsOpen = EFalse;
		}
	}

void CPmCalSync::StartL(const TDesC& aStoreDir, const TPmCalSettings& aSettings)
	{
	if (Running())
		return;
	iSettings = aSettings;
	iDir = aStoreDir;
	iDir.Append(_L("cal\\"));
	iAdded = iChangedN = iRemoved = iSent = iFailed = 0;
	iPushedRead = EFalse;
	iPhase = EOpen;
	iPos = 0;
	Next();
	}

void CPmCalSync::Next()
	{
	TRequestStatus* s = &iStatus;
	User::RequestComplete(s, KErrNone);
	SetActive();
	}

void CPmCalSync::DoCancel()
	{
	}

// A step left. (This used to be called RunError, which ER5's CActive never
// calls - e32base.h declares only RunL and DoCancel - so a leave went to
// CONE's error dialog and the sync stayed "running" with the Agenda held
// open for the rest of the session. RunL traps StepL and calls this.)
void CPmCalSync::Failed(TInt aError)
	{
	// keep what was done: entries added so far must stay linked
	if (iMap && iPush && iFsOpen)
		{
		TRAPD(err, SaveMapL(); SavePushL());
		if (err == KErrNone && iPushedRead)
			{
			TFileName path;
			Path(KPushedFile, path);
			iFs.Delete(path);
			}
		}
	Close();
	Reset();
	iPhase = EIdle;
	TBuf<80> why;
	// the likely cause, rather than a bare number (style guide 6)
	switch (aError)
		{
	case KErrNotFound: why = _L("Agenda file not found"); break;
	case KErrPathNotFound: why = _L("The Agenda file's folder is not there"); break;
	case KErrInUse:
	case KErrLocked: why = _L("The Agenda file is busy"); break;
	case KErrNoMemory: why = _L("Not enough memory for the calendar"); break;
	case KErrDiskFull: why = _L("Calendar not synced - no room left on the disk"); break;
	case KErrNotReady:
	case KErrDisMounted: why = _L("Calendar not synced - the memory disk is not there"); break;
	case KErrAccessDenied: why = _L("Calendar not synced - the disk is write-protected"); break;
	case KErrCorrupt:
	case KErrEof: why = _L("The Agenda file could not be read"); break;
	case KErrCancel: why = _L("Calendar sync stopped"); break;
	default: why.Format(_L("Calendar sync failed (%d)"), aError); break;
		}
	iObserver.CalSyncDone(aError, why, EFalse);
	}

void CPmCalSync::RunL()
	{
	TRAPD(err, StepL());
	if (err != KErrNone)
		Failed(err);
	}

void CPmCalSync::StepL()
	{
	TBool more = EFalse;
	switch (iPhase)
		{
	case EOpen:
		iObserver.CalProgress(_L("Opening the Agenda..."));
		OpenL();
		iPhase = EPushed;
		more = ETrue;
		break;
	case EPushed:
		ReadPushedL();
		iPhase = EMapped;
		iPos = 0;
		more = ETrue;
		break;
	case EMapped:
		if (!MappedStepL()) { iPhase = ENew; iPos = 0; }
		more = ETrue;
		break;
	case ENew:
		if (!NewStepL()) { iPhase = ELocal; iPos = 0; }
		more = ETrue;
		break;
	case ELocal:
		if (!LocalStepL()) iPhase = EFinish;
		more = ETrue;
		break;
	case EFinish:
		FinishL();
		break;
	default:
		break;
		}
	if (more)
		Next();
	}

// ---------------------------------------------------------------- files

HBufC8* CPmCalSync::ReadFileL(const TDesC& aName)
	{
	TFileName path;
	Path(aName, path);
	RFile f;
	if (f.Open(iFs, path, EFileRead | EFileShareReadersOnly) != KErrNone)
		return NULL;
	CleanupClosePushL(f);
	TInt size = 0;
	User::LeaveIfError(f.Size(size));
	HBufC8* buf = HBufC8::NewLC(size + 1);
	TPtr8 p = buf->Des();
	User::LeaveIfError(f.Read(p, size));
	CleanupStack::Pop();                 // buf
	CleanupStack::PopAndDestroy();       // f
	return buf;
	}

// calls back for each line (without its line end)
class TLines
	{
public:
	TLines(const TDesC8& aText) : iText(aText), iPos(0) {}
	TBool Next(TPtrC8& aLine)
		{
		if (iPos >= iText.Length()) return EFalse;
		TPtrC8 rest = iText.Mid(iPos);
		TInt n = rest.Locate('\n');
		if (n < 0) n = rest.Length();
		aLine.Set(rest.Left(n));
		if (aLine.Length() && aLine[aLine.Length() - 1] == '\r') aLine.Set(aLine.Left(aLine.Length() - 1));
		iPos += n + 1;
		return ETrue;
		}
private:
	const TDesC8& iText;
	TInt iPos;
	};

TPtrC8 CPmCalSync::Field(const TDesC8& aLine, TInt aIndex) const
	{
	TPtrC8 rest(aLine);
	for (TInt i = 0; ; i++)
		{
		TInt t = rest.Locate('\t');
		TPtrC8 f = t < 0 ? rest : rest.Left(t);
		if (i == aIndex) return f;
		if (t < 0) return TPtrC8();
		rest.Set(rest.Mid(t + 1));
		}
	}

// events.txt: calid href etag recurid flags start end alarm summary location
void CPmCalSync::EventKey(const TDesC8& aLine, TDes8& aKey) const
	{
	aKey.Zero();
	aKey.Append(Clip(Field(aLine, 1), aKey.MaxLength() - 30));
	aKey.Append('|');
	aKey.Append(Clip(Field(aLine, 3), 24));
	}

void CPmCalSync::LoadEventsL()
	{
	iEvents = new(ELeave) CArrayFixFlat<TPmCalEvent>(64);
	HBufC8* text = ReadFileL(KEventsFile);
	if (!text)
		return;
	CleanupStack::PushL(text);
	TLines lines(*text);
	TPtrC8 line;
	TBuf8<260> key;
	while (lines.Next(line))
		{
		if (line.Length() == 0)
			continue;
		if (line[0] == '#')
			{
			// "#PSIEV1 TAB first day TAB zone"
			TTime first = ParseTime(Field(line, 1));
			if (first != Time::NullTTime())
				iWinStart = first;
			continue;
			}
		TPmCalEvent e;
		EventKey(line, key);
		e.iKeyHash = Fnv(key);
		TInt skip = Field(line, 0).Length() + Field(line, 1).Length() + Field(line, 2).Length() +
			Field(line, 3).Length() + 4;
		if (skip > line.Length())
			continue;                         // not a whole line
		TPtrC8 hashed = line.Mid(skip);       // flags .. location
		e.iHash = Fnv(hashed);
		e.iMapped = -1;
		e.iLine = line.AllocLC();
		iEvents->AppendL(e);
		CleanupStack::Pop();                  // line
		}
	CleanupStack::PopAndDestroy();            // text
	}

TInt CPmCalSync::FindEvent(const TDesC8& aKey) const
	{
	if (aKey.Length() == 0)
		return -1;
	TUint32 h = Fnv(aKey);
	TBuf8<260> key;
	for (TInt i = 0; i < iEvents->Count(); i++)
		{
		const TPmCalEvent& e = (*iEvents)[i];
		if (e.iKeyHash != h) continue;
		EventKey(*e.iLine, key);
		if (key == aKey) return i;
		}
	return -1;
	}

TInt CPmCalSync::AddMapL(TUint32 aAuid, const TDesC8& aKey, TUint32 aHash, TUint aFlags)
	{
	TPmCalMap m;
	m.iAuid = aAuid;
	m.iChanged = aAuid ? Changed(aAuid) : TInt64(0);
	m.iHash = aHash;
	m.iFlags = aFlags;
	m.iKey = aKey.AllocLC();
	m.iEvent = -1;
	iMap->AppendL(m);
	CleanupStack::Pop();
	return iMap->Count() - 1;
	}

void CPmCalSync::RemoveMap(TInt aIndex)
	{
	delete (*iMap)[aIndex].iKey;
	iMap->Delete(aIndex);
	}

void CPmCalSync::LoadMapL()
	{
	iMap = new(ELeave) CArrayFixFlat<TPmCalMap>(64);
	iSince = TInt64(0);
	iNewFile = ETrue;
	HBufC8* text = ReadFileL(KMapFile);
	TTime now;
	now.HomeTime();
	TBuf8<130> file;
	file.Copy(iSettings.iAgendaFile);
	file.LowerCase();
	if (text)
		{
		CleanupStack::PushL(text);
		TLines lines(*text);
		TPtrC8 line;
		while (lines.Next(line))
			{
			if (line.Length() == 0) continue;
			if (line[0] == '#')
				{
				TBuf8<130> f(Clip(Field(line, 1), 128));
				f.LowerCase();
				if (Field(line, 0) == KMapMagic && f == file)
					{
					iNewFile = EFalse;
					iSince = ParseHex64(Field(line, 2));
					}
				continue;
				}
			if (iNewFile) break;           // another Agenda file: start again
			TPmCalMap m;
			TLex8 l(Field(line, 0));
			TUint auid = 0;
			l.Val(auid);
			m.iAuid = auid;
			m.iChanged = ParseHex64(Field(line, 1));
			m.iHash = ParseHex(Field(line, 2));
			TLex8 lf(Field(line, 3));
			TUint flags = 0;
			lf.Val(flags);
			m.iFlags = flags;
			m.iKey = Field(line, 4).AllocLC();
			m.iEvent = -1;
			iMap->AppendL(m);
			CleanupStack::Pop();
			}
		CleanupStack::PopAndDestroy();       // text
		}
	if (iNewFile)
		{
		for (TInt i = 0; i < iMap->Count(); i++) delete (*iMap)[i].iKey;
		iMap->Reset();
		// Psion entries changed from now on go to the server (or all of
		// them, if the user asked for that)
		iSince = iSettings.iCopyExisting ? TInt64(0) : now.Int64();
		}
	}

void CPmCalSync::SaveMapL()
	{
	TFileName path, tmp;
	Path(KMapFile, path);
	Path(_L("agenda.tmp"), tmp);
	RFile f;
	User::LeaveIfError(f.Replace(iFs, tmp, EFileWrite));
	CleanupClosePushL(f);
	TBuf8<400> line;
	line = KMapMagic;
	line.Append('\t');
	TBuf8<130> file;
	file.Copy(iSettings.iAgendaFile);
	line.Append(file);
	line.Append('\t');
	AppendHex64(line, iSince);
	line.Append(_L8("\t0\n"));
	User::LeaveIfError(f.Write(line));
	for (TInt i = 0; i < iMap->Count(); i++)
		{
		const TPmCalMap& m = (*iMap)[i];
		line.Zero();
		line.AppendNum((TUint)m.iAuid, EDecimal);
		line.Append('\t');
		AppendHex64(line, m.iChanged);
		line.Append('\t');
		line.AppendNumFixedWidth((TUint)m.iHash, EHex, 8);
		line.Append('\t');
		line.AppendNum((TInt)m.iFlags);
		line.Append('\t');
		line.Append(Clip(*m.iKey, line.MaxLength() - line.Length() - 1));
		line.Append('\n');
		User::LeaveIfError(f.Write(line));
		}
	User::LeaveIfError(f.Flush());
	CleanupStack::PopAndDestroy();          // f
	User::LeaveIfError(iFs.Replace(tmp, path));   // (one step: no moment with neither file)
	}

// A small file written whole and put in place in one step (robustness
// notes, section 4): a failure never leaves a short file where the old one was
static void WriteFileSafeL(RFs& aFs, const TDesC& aPath, const TDesC8& aText)
	{
	TFileName tmp(aPath);
	tmp.Append('~');
	RFile f;
	User::LeaveIfError(f.Replace(aFs, tmp, EFileWrite));
	TInt r = f.Write(aText);
	if (r == KErrNone)
		r = f.Flush();
	f.Close();
	if (r == KErrNone)
		r = aFs.Replace(tmp, aPath);
	if (r != KErrNone)
		{
		aFs.Delete(tmp);
		User::Leave(r);
		}
	}

void CPmCalSync::LoadCalendarsL()
	{
	iWritable = new(ELeave) CDesC8ArrayFlat(4);
	iDefaultCal.Zero();
	HBufC8* text = ReadFileL(KCalsFile);
	if (!text)
		return;
	CleanupStack::PushL(text);
	TLines lines(*text);
	TPtrC8 line;
	while (lines.Next(line))
		{
		if (line.Length() == 0 || line[0] == '#') continue;
		// id sync ctag href flags name
		TPtrC8 flags = Field(line, 4);
		if (flags.Locate('W') >= 0) iWritable->AppendL(Field(line, 0));
		if (flags.Locate('D') >= 0) iDefaultCal = Clip(Field(line, 0), 12);
		}
	CleanupStack::PopAndDestroy();
	}

// (the static functions below use the app's own file server session: one
// per app, as the robustness notes have it, not one per call)
void CPmCalSync::CalendarsL(const TDesC& aStoreDir, CDesCArray& aNames, CDesC8Array& aIds, TInt& aDefault)
	{
	aDefault = -1;
	RFs& fs = CEikonEnv::Static()->FsSession();
	TFileName path(aStoreDir);
	path.Append(_L("cal\\calendars.txt"));
	RFile f;
	if (f.Open(fs, path, EFileRead | EFileShareReadersOnly) == KErrNone)
		{
		CleanupClosePushL(f);
		TInt size = 0;
		f.Size(size);
		HBufC8* text = HBufC8::NewLC(size + 1);
		TPtr8 p = text->Des();
		f.Read(p, size);
		TLines lines(*text);
		TPtrC8 line;
		while (lines.Next(line))
			{
			if (line.Length() == 0 || line[0] == '#') continue;
			TPtrC8 rest(line);
			TPtrC8 fields[6];
			for (TInt i = 0; i < 6; i++)
				{
				TInt t = rest.Locate('\t');
				fields[i].Set(t < 0 ? rest : rest.Left(t));
				rest.Set(t < 0 ? TPtrC8() : rest.Mid(t + 1));
				}
			if (fields[4].Locate('W') < 0) continue;     // can't add events there
			TBuf<64> name;
			name.Copy(Clip(fields[5], 64));
			if (fields[4].Locate('D') >= 0) aDefault = aNames.Count();
			aNames.AppendL(name);
			aIds.AppendL(fields[0]);
			}
		CleanupStack::PopAndDestroy(2);   // text, f
		}
	}

void CPmCalSync::SetDefaultCalendarL(const TDesC& aStoreDir, const TDesC8& aId)
	{
	RFs& fs = CEikonEnv::Static()->FsSession();
	TFileName path(aStoreDir);
	path.Append(_L("cal\\"));
	fs.MkDirAll(path);
	path.Append(_L("default.txt"));
	TBuf8<64> text(Clip(aId, 60));
	text.Append('\n');
	WriteFileSafeL(fs, path, text);
	}

void CPmCalSync::ForgetL(const TDesC& aStoreDir)
	{
	RFs& fs = CEikonEnv::Static()->FsSession();
	TFileName path(aStoreDir);
	path.Append(_L("cal\\agenda.txt"));
	fs.Delete(path);
	path = aStoreDir;
	path.Append(_L("cal\\push.txt"));
	fs.Delete(path);
	}

// changes not yet sent (the engine may have been offline): kept, unless
// this pass has a newer one for the same entry
void CPmCalSync::LoadPushL()
	{
	iPush = new(ELeave) CArrayPtrFlat<HBufC8>(8);
	HBufC8* text = ReadFileL(KPushFile);
	if (!text)
		return;
	CleanupStack::PushL(text);
	TLines lines(*text);
	TPtrC8 line;
	while (lines.Next(line))
		{
		if (line.Length() == 0 || line[0] == '#') continue;
		HBufC8* b = line.AllocLC();
		iPush->AppendL(b);
		CleanupStack::Pop();
		}
	CleanupStack::PopAndDestroy();
	}

void CPmCalSync::PushL(const TDesC8& aLine)
	{
	TPtrC8 key = Field(aLine, 1);
	for (TInt i = 0; i < iPush->Count(); i++)
		if (Field(*(*iPush)[i], 1) == key)
			{
			delete (*iPush)[i];
			iPush->Delete(i);
			break;
			}
	HBufC8* b = aLine.AllocLC();
	iPush->AppendL(b);
	CleanupStack::Pop();
	iSent++;
	}

void CPmCalSync::SavePushL()
	{
	TFileName path;
	Path(KPushFile, path);
	if (iPush->Count() == 0)
		{
		iFs.Delete(path);
		return;
		}
	// (these are the user's unsent Agenda changes: written whole to a
	// temporary first, then put in place, as agenda.txt is)
	TFileName tmp;
	Path(_L("push.tmp"), tmp);
	RFile f;
	User::LeaveIfError(f.Replace(iFs, tmp, EFileWrite));
	CleanupClosePushL(f);
	for (TInt i = 0; i < iPush->Count(); i++)
		{
		User::LeaveIfError(f.Write(*(*iPush)[i]));
		User::LeaveIfError(f.Write(_L8("\n")));
		}
	User::LeaveIfError(f.Flush());
	CleanupStack::PopAndDestroy();
	User::LeaveIfError(iFs.Replace(tmp, path));
	}

TBool CPmCalSync::Writable(const TDesC8& aLine) const
	{
	TInt pos;
	return iWritable->Find(Field(aLine, 0), pos) == 0;
	}

// ---------------------------------------------------------------- the Agenda

void CPmCalSync::OpenL()
	{
	User::LeaveIfError(iFs.Connect());
	iFsOpen = ETrue;
	TEntry entry;
	if (iFs.Entry(iSettings.iAgendaFile, entry) != KErrNone)
		User::Leave(KErrNotFound);         // (the Agenda server panics on a missing file)
	iWinStart = Time::NullTTime();
	LoadEventsL();
	LoadMapL();
	LoadCalendarsL();
	LoadPushL();
	TTime now;
	now.HomeTime();
	if (iWinStart == Time::NullTTime())
		iWinStart = Midnight(now) - TTimeIntervalDays(iSettings.iCal.days_back >= 0 ? iSettings.iCal.days_back : 30);
	iWinEnd = Midnight(now) + TTimeIntervalDays((iSettings.iCal.days_ahead > 0 ? iSettings.iCal.days_ahead : 180) + 1);

	iPara = CParaFormatLayer::NewL();
	iChar = CCharFormatLayer::NewL();
	iServ = RAgendaServ::NewL();
	User::LeaveIfError(iServ->Connect());
	iServConnected = ETrue;
	iModel = CAgnEntryModel::NewL();
	iModel->SetServer(iServ);
	iModel->OpenL(iSettings.iAgendaFile, TTimeIntervalMinutes(9 * 60), TTimeIntervalMinutes(9 * 60), TTimeIntervalMinutes(9 * 60));
	iAgendaOpen = ETrue;
	iServ->WaitUntilLoaded();
	if (iServ->FileIsReadOnly())
		User::Leave(KErrAccessDenied);
	}

CAgnEntry* CPmCalSync::FetchL(TUint32 aAuid)
	{
	CAgnEntry* e = NULL;
	TRAPD(err, e = iModel->FetchEntryL(TAgnUniqueId(aAuid)));
	if (err == KErrNoMemory)
		User::Leave(err);
	return err == KErrNone ? e : NULL;
	}

TInt64 CPmCalSync::Changed(TUint32 aAuid)
	{
	TTime t = iServ->UniqueIdLastChangedDate(TAgnUniqueId(aAuid));
	return t.Int64();
	}

// the entry's details as a push.txt line
void CPmCalSync::PushEntryL(TChar aOp, TUint32 aAuid, CAgnEntry* aEntry, const TDesC8& aEventLine)
	{
	HBufC8* buf = HBufC8::NewLC(900);
	TPtr8 line = buf->Des();
	line.Append(aOp);
	line.Append(_L8("\ta"));
	line.AppendNum((TUint)aAuid, EDecimal);
	line.Append('\t');
	if (aOp == 'N')
		line.Append(iDefaultCal);
	else
		{
		line.Append(Field(aEventLine, 1));          // href
		line.Append('\t');
		line.Append(Field(aEventLine, 2));          // etag
		line.Append('\t');
		line.Append(Field(aEventLine, 3));          // recurid
		}
	if (aOp != 'D')
		{
		TTime start, end;
		TBool allday = aEntry->Type() != CAgnEntry::EAppt;
		if (allday)
			{
			start = Midnight(aEntry->CastToEvent()->StartDate());
			end = Midnight(aEntry->CastToEvent()->EndDate());
			}
		else
			{
			start = aEntry->CastToAppt()->StartDateTime();
			end = aEntry->CastToAppt()->EndDateTime();
			}
		if (end < start) end = start;
		TInt alarm = -2;                            // leave the server's alarms alone
		if (iSettings.iAlarms)
			{
			// "no alarm" removes the server's only if it had one we could
			// read (it may have kinds the Psion can't show)
			TLex8 l(Field(aEventLine, 7));
			TInt had = -1;
			if (aOp == 'M') l.Val(had);
			if (aOp == 'N' || had >= 0) alarm = -1;
			if (aEntry->HasAlarm())
				{
				TTimeIntervalMinutes m;
				start.MinutesFrom(aEntry->AlarmInstanceDateTime(), m);
				alarm = m.Int() > 0 ? m.Int() : 0;
				}
			}
		line.Append('\t');
		AppendTime(line, start);
		line.Append('\t');
		AppendTime(line, end);
		line.Append('\t');
		if (allday) line.Append('A');
		line.Append('\t');
		line.AppendNum(alarm);
		line.Append('\t');
		TBuf<200> text;
		CRichText* rt = aEntry->RichTextL();
		rt->Extract(text, 0, Min(rt->DocumentLength(), text.MaxLength()));
		text.Trim();
		if (text.Length() == 0) text = _L("(no title)");
		AppendClean(line, Clip(text, 190));
		line.Append('\t');
		AppendClean(line, Clip(aEntry->Location(), 110));
		if (aOp == 'N')
			{
			// the event's UID on the server: the same if this is sent again
			line.Append(_L8("\tpsion-"));
			AppendHex64(line, iSince);
			line.Append('-');
			line.AppendNum((TUint)aAuid, EDecimal);
			}
		}
	PushL(line);
	CleanupStack::PopAndDestroy();       // buf
	}

void CPmCalSync::FillL(CAgnEntry* aEntry, const TDesC8& aLine, const TTime& aStart)
	{
	CRichText* rt = aEntry->RichTextL();
	rt->Reset();
	TBuf<200> text;
	text.Copy(Clip(Field(aLine, 8), 200));
	rt->InsertL(0, text);
	TBuf<120> loc;
	loc.Copy(Clip(Field(aLine, 9), 120));
	aEntry->SetLocationL(loc);
	if (iSettings.iAlarms)
		{
		TLex8 l(Field(aLine, 7));
		TInt before = -1;
		l.Val(before);
		if (before < 0)
			aEntry->ClearAlarm();
		else
			{
			// the Agenda's alarm: so many days before, at a time of day
			TTime at = aStart - TTimeIntervalMinutes(before);
			TInt days = Midnight(aStart).DaysFrom(Midnight(at)).Int();
			TDateTime d = at.DateTime();
			aEntry->SetAlarm(TTimeIntervalDays(days), TTimeIntervalMinutes(d.Hour() * 60 + d.Minute()));
			}
		}
	}

TUint32 CPmCalSync::AddFromEventL(const TDesC8& aLine)
	{
	TTime start = ParseTime(Field(aLine, 5)), end = ParseTime(Field(aLine, 6));
	TBool allday = Field(aLine, 4).Locate('A') >= 0;
	CAgnEntry* e;
	if (allday)
		{
		CAgnEvent* ev = CAgnEvent::NewL(iPara, iChar);
		e = ev;
		CleanupStack::PushL(e);
		ev->SetStartAndEndDate(start, end);
		}
	else
		{
		CAgnAppt* a = CAgnAppt::NewL(iPara, iChar);
		e = a;
		CleanupStack::PushL(e);
		a->SetStartAndEndDateTime(start, end);
		}
	FillL(e, aLine, start);
	TAgnEntryId id = iModel->AddEntryL(e);
	CleanupStack::PopAndDestroy();       // e
	return iServ->GetUniqueId(id).Id();
	}

// returns the entry's UID, a new one if it had to be made again
TUint32 CPmCalSync::UpdateFromEventL(TUint32 aAuid, const TDesC8& aLine)
	{
	CAgnEntry* e = FetchL(aAuid);
	TBool allday = Field(aLine, 4).Locate('A') >= 0;
	if (!e)
		return AddFromEventL(aLine);
	CleanupStack::PushL(e);
	TBool isAppt = e->Type() == CAgnEntry::EAppt;
	if ((e->Type() != CAgnEntry::EAppt && e->Type() != CAgnEntry::EEvent) || isAppt == allday || e->IsRepeating())
		{
		// a different kind of entry now: make it again
		CleanupStack::PopAndDestroy();
		iServ->DeleteEntry(TAgnUniqueId(aAuid));
		return AddFromEventL(aLine);
		}
	TTime start = ParseTime(Field(aLine, 5)), end = ParseTime(Field(aLine, 6));
	if (allday)
		e->CastToEvent()->SetStartAndEndDate(start, end);
	else
		e->CastToAppt()->SetStartAndEndDateTime(start, end);
	FillL(e, aLine, start);
	iModel->UpdateEntryL(e);
	CleanupStack::PopAndDestroy();
	return aAuid;
	}

void CPmCalSync::DeleteL(TUint32 aAuid)
	{
	iServ->DeleteEntry(TAgnUniqueId(aAuid));
	}

// ---------------------------------------------------------------- the steps

// what the engine did with the changes sent last time
void CPmCalSync::ReadPushedL()
	{
	HBufC8* text = ReadFileL(KPushedFile);
	if (!text)
		return;
	CleanupStack::PushL(text);
	TLines lines(*text);
	TPtrC8 line;
	while (lines.Next(line))
		{
		TPtrC8 key = Field(line, 0), what = Field(line, 1), href = Field(line, 2);
		if (key.Length() < 2 || key[0] != 'a') continue;
		TLex8 l(key.Mid(1));
		TUint auid = 0;
		l.Val(auid);
		for (TInt i = 0; i < iMap->Count(); i++)
			{
			TPmCalMap& m = (*iMap)[i];
			if (m.iAuid != auid) continue;
			TBool ok = what == _L8("ok");
			if (m.iFlags & KMapPending)
				{
				if (ok)
					{
					// now it has a place on the server
					HBufC8* k = HBufC8::NewL(href.Length() + 1);
					k->Des().Copy(href);
					k->Des().Append('|');
					delete m.iKey;
					m.iKey = k;
					m.iHash = 0;
					m.iFlags &= ~KMapPending;
					}
				else
					{
					RemoveMap(i);              // try again next time
					iFailed++;
					}
				}
			else if (m.iFlags & KMapDeleting)
				{
				if (ok || what == _L8("gone")) RemoveMap(i);
				else { m.iFlags = KMapForce; iFailed++; }
				}
			else if (!ok)
				{
				m.iFlags |= KMapForce;        // put the server's version back
				if (what != _L8("gone")) iFailed++;
				}
			break;
			}
		}
	CleanupStack::PopAndDestroy();
	iPushedRead = ETrue;                    // deleted once the links are saved
	}

// one batch of the entries we look after
TBool CPmCalSync::MappedStepL()
	{
	TBuf<60> prog;
	prog.Format(_L("Updating the Agenda (%d%%)..."), iMap->Count() ? iPos * 50 / iMap->Count() : 50);
	iObserver.CalProgress(prog);
	for (TInt n = 0; n < KStep && iPos < iMap->Count(); n++)
		{
		TPmCalMap& m = (*iMap)[iPos];
		if (m.iFlags & KMapPending)
			{
			iPos++;                            // sent, not confirmed yet (offline?)
			continue;
			}
		TInt ev = FindEvent(*m.iKey);
		if (ev >= 0 && (*iEvents)[ev].iMapped >= 0)
			ev = -1;                           // two entries for one event: keep the first
		if (ev >= 0) (*iEvents)[ev].iMapped = iPos;
		CAgnEntry* e = FetchL(m.iAuid);
		TBool gone = e == NULL;
		TTime start = Time::NullTTime();
		if (e)
			{
			start = e->Type() == CAgnEntry::EAppt ? e->CastToAppt()->StartDateTime() : e->InstanceStartDate();
			delete e;
			e = NULL;
			}
		TInt64 changed = gone ? TInt64(0) : Changed(m.iAuid);
		TBool local = !gone && changed != m.iChanged;
		if (m.iFlags & KMapDeleting)
			{
			if (ev < 0) { RemoveMap(iPos); continue; }
			iPos++;                            // waiting for the server
			continue;
			}
		if (ev < 0)
			{
			// not on the server (any more)
			if (gone || (start != Time::NullTTime() && (start < iWinStart || start >= iWinEnd)))
				{
				RemoveMap(iPos);                // gone, or just old: the Psion keeps it
				continue;
				}
			DeleteL(m.iAuid);
			iRemoved++;
			RemoveMap(iPos);
			continue;
			}
		const TDesC8& line = *(*iEvents)[ev].iLine;
		TUint32 hash = (*iEvents)[ev].iHash;
		if (ParseTime(Field(line, 5)) == Time::NullTTime() || ParseTime(Field(line, 6)) == Time::NullTTime())
			{
			iPos++;                            // a line we can't read: leave the entry be
			continue;
			}
		TBool writable = Writable(line);
		if (gone)
			{
			if (writable && !(m.iFlags & KMapForce))
				{
				PushEntryL('D', m.iAuid, NULL, line);
				m.iFlags = KMapDeleting;
				}
			else
				{
				m.iAuid = AddFromEventL(line);    // can't delete it there: back it comes
				m.iChanged = Changed(m.iAuid);
				m.iHash = hash;
				m.iFlags = 0;
				iAdded++;
				}
			iPos++;
			continue;
			}
		if (m.iHash == 0 && !(m.iFlags & KMapForce))
			m.iHash = hash;                     // made here: the server's copy is ours
		if (hash != m.iHash || (m.iFlags & KMapForce) || (local && !writable))
			{
			m.iAuid = UpdateFromEventL(m.iAuid, line);
			m.iChanged = Changed(m.iAuid);
			m.iHash = hash;
			m.iFlags = 0;
			iChangedN++;
			}
		else if (local)
			{
			CAgnEntry* le = FetchL(m.iAuid);
			if (le)
				{
				CleanupStack::PushL(le);
				PushEntryL('M', m.iAuid, le, line);
				CleanupStack::PopAndDestroy();
				}
			m.iChanged = changed;
			}
		iPos++;
		}
	return iPos < iMap->Count();
	}

// server events the Agenda doesn't have yet
TBool CPmCalSync::NewStepL()
	{
	TBuf<60> prog;
	prog.Format(_L("Updating the Agenda (%d%%)..."), 50 + (iEvents->Count() ? iPos * 40 / iEvents->Count() : 40));
	iObserver.CalProgress(prog);
	TBuf8<260> key;
	for (TInt n = 0; n < KStep && iPos < iEvents->Count(); iPos++)
		{
		TPmCalEvent& ev = (*iEvents)[iPos];
		if (ev.iMapped >= 0) continue;
		if (ParseTime(Field(*ev.iLine, 5)) == Time::NullTTime() || ParseTime(Field(*ev.iLine, 6)) == Time::NullTTime())
			continue;                          // a line we can't read
		EventKey(*ev.iLine, key);
		TUint32 auid = AddFromEventL(*ev.iLine);
		ev.iMapped = AddMapL(auid, key, ev.iHash, 0);
		iAdded++;
		n++;
		}
	return iPos < iEvents->Count();
	}

// Psion entries made or changed since sync was turned on
TBool CPmCalSync::LocalStepL()
	{
	CAgnSyncIter* it = (CAgnSyncIter*)iIter;
	if (!it)
		{
		if (iDefaultCal.Length() == 0)
			return EFalse;                       // nowhere to put them
		iObserver.CalProgress(_L("Looking for new Agenda entries..."));
		it = CAgnSyncIter::NewL(iServ);
		iIter = it;
		it->First();
		}
	for (TInt n = 0; n < 20 && it->Available(); n++, it->Next())
		{
		if (it->HasBeenDeleted()) continue;
		CAgnEntry::TType type = it->Type();
		if (type != CAgnEntry::EAppt && type != CAgnEntry::EEvent) continue;
		TUint32 auid = it->UniqueId().Id();
		TTime changed = it->LastChangedDate();
		if (changed.Int64() <= iSince) continue;
		TBool known = EFalse;
		for (TInt i = 0; i < iMap->Count() && !known; i++)
			if ((*iMap)[i].iAuid == auid) known = ETrue;
		if (known) continue;
		CAgnEntry* e = FetchL(auid);
		if (!e) continue;
		CleanupStack::PushL(e);
		TTime start = e->Type() == CAgnEntry::EAppt ? e->CastToAppt()->StartDateTime() : e->InstanceStartDate();
		// repeating Psion entries stay on the Psion (for now)
		if (!e->IsRepeating() && start >= iWinStart && start < iWinEnd)
			{
			PushEntryL('N', auid, e, KNullDesC8);
			AddMapL(auid, KNullDesC8, 0, KMapPending);
			}
		CleanupStack::PopAndDestroy();
		}
	return it->Available();
	}

void CPmCalSync::FinishL()
	{
	SaveMapL();
	SavePushL();
	if (iPushedRead)
		{
		TFileName path;
		Path(KPushedFile, path);
		iFs.Delete(path);
		}
	TBool pushed = iPush->Count() > 0;
	TBuf<120> msg;
	if (iAdded || iChangedN || iRemoved)
		{
		msg = _L("Agenda: ");
		TBool any = EFalse;
		if (iAdded) { msg.AppendFormat(_L("%d new"), iAdded); any = ETrue; }
		if (iChangedN) { if (any) msg.Append(_L(", ")); msg.AppendFormat(_L("%d changed"), iChangedN); any = ETrue; }
		if (iRemoved) { if (any) msg.Append(_L(", ")); msg.AppendFormat(_L("%d removed"), iRemoved); }
		}
	else
		msg = _L("Agenda up to date");
	if (iSent)
		msg.AppendFormat(_L("; sending %d change%s"), iSent, iSent == 1 ? _S("") : _S("s"));
	if (iFailed)
		msg.AppendFormat(_L("; %d not accepted by the server"), iFailed);
	Close();
	Reset();
	iPhase = EIdle;
	iObserver.CalSyncDone(KErrNone, msg, pushed && iSent > 0);
	}

// ---------------------------------------------------------------- a new entry

void CPmCalSync::AddToAgendaL(const TDesC& aFile, const TDesC& aTitle, const TDesC& aLocation,
	const TTime& aStart, const TTime& aEnd, TBool aAllDay, TInt aAlarm)
	{
	TEntry entry;
	if (CEikonEnv::Static()->FsSession().Entry(aFile, entry) != KErrNone)
		User::Leave(KErrNotFound);
	CParaFormatLayer* para = CParaFormatLayer::NewL();
	CleanupStack::PushL(para);
	CCharFormatLayer* chr = CCharFormatLayer::NewL();
	CleanupStack::PushL(chr);
	RAgendaServ* serv = RAgendaServ::NewL();
	CleanupStack::PushL(serv);
	User::LeaveIfError(serv->Connect());
	CleanupClosePushL(*serv);
	CAgnEntryModel* model = CAgnEntryModel::NewL();
	CleanupStack::PushL(model);
	model->SetServer(serv);
	model->OpenL(aFile, TTimeIntervalMinutes(9 * 60), TTimeIntervalMinutes(9 * 60), TTimeIntervalMinutes(9 * 60));
	serv->WaitUntilLoaded();
	CAgnEntry* e;
	if (aAllDay)
		{
		CAgnEvent* ev = CAgnEvent::NewL(para, chr);
		e = ev;
		CleanupStack::PushL(e);
		ev->SetStartAndEndDate(aStart, aEnd);
		}
	else
		{
		CAgnAppt* a = CAgnAppt::NewL(para, chr);
		e = a;
		CleanupStack::PushL(e);
		a->SetStartAndEndDateTime(aStart, aEnd);
		}
	e->RichTextL()->InsertL(0, aTitle);
	if (aLocation.Length())
		e->SetLocationL(aLocation);
	if (aAlarm >= 0)
		{
		TTime at = aStart - TTimeIntervalMinutes(aAlarm);
		TInt days = Midnight(aStart).DaysFrom(Midnight(at)).Int();
		TDateTime d = at.DateTime();
		e->SetAlarm(TTimeIntervalDays(days), TTimeIntervalMinutes(d.Hour() * 60 + d.Minute()));
		}
	model->AddEntryL(e);
	CleanupStack::PopAndDestroy();       // e
	CleanupStack::PopAndDestroy();       // model
	serv->CloseAgenda();
	CleanupStack::PopAndDestroy();       // serv->Close()
	CleanupStack::Pop();                 // serv
	delete serv;
	CleanupStack::PopAndDestroy(2);      // chr, para
	}
