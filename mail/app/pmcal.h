// PMCAL.H - PsiMail's calendar sync with the Psion's Agenda
//
// The engine (../engine/caldav.c) keeps the server's events in
// <store>cal\events.txt. CPmCalSync puts them into the Agenda file - through
// the Agenda server, so the Agenda app can stay open - and writes what the
// user changed there to <store>cal\push.txt for the engine to send.
//
//   <store>cal\agenda.txt   "#PSIAGN1 TAB agenda file TAB since (hex) TAB copy existing"
//                           then per Agenda entry we look after:
//                           entry UID TAB last changed (hex) TAB server hash TAB flags TAB href|recurid
//
// An entry's UID (TAgnUniqueId) survives edits; its last-changed date tells
// us the user changed it. "since" is when sync was turned on: Psion entries
// made or changed after that are sent to the server.

#ifndef __PMCAL_H
#define __PMCAL_H

#include <e32base.h>
#include <f32file.h>
#include <badesca.h>

extern "C" {
#include <psimail.h>
}

class CAgnEntryModel;
class RAgendaServ;
class CAgnEntry;
class CParaFormatLayer;
class CCharFormatLayer;

struct TPmCalSettings
	{
	PmCalendar iCal;           // what the engine needs (host, zone, window ...)
	TBuf<128> iAgendaFile;     // C:\Documents\Agenda
	TInt iAlarms;              // alarms come from the server (and go back)
	TInt iCopyExisting;        // first sync: send the Psion's own entries too
	TInt iSpare[8];
	};

class MPmCalObserver
	{
public:
	virtual void CalProgress(const TDesC& aText) = 0;
	// done: aPushed = there are changes for the engine to send
	virtual void CalSyncDone(TInt aError, const TDesC& aSummary, TBool aPushed) = 0;
	};

struct TPmCalMap
	{
	TUint32 iAuid;             // TAgnUniqueId
	TInt64 iChanged;           // its last-changed date when we last looked
	TUint32 iHash;             // of the server's fields when we last looked
	TUint iFlags;              // KMap*
	HBufC8* iKey;              // href|recurid ("" = new, not yet on the server)
	TInt iEvent;               // index in events, -1
	};

struct TPmCalEvent
	{
	HBufC8* iLine;             // a line of events.txt
	TUint32 iKeyHash;
	TUint32 iHash;
	TInt iMapped;
	};

class CPmCalSync : public CActive
	{
public:
	static CPmCalSync* NewL(MPmCalObserver& aObserver);
	~CPmCalSync();
	void StartL(const TDesC& aStoreDir, const TPmCalSettings& aSettings);
	TBool Running() const { return iPhase != EIdle; }
	static void DefaultAgendaFile(TDes& aFile);
	// the calendars the engine found: names, ids, and which is the default
	static void CalendarsL(const TDesC& aStoreDir, CDesCArray& aNames, CDesC8Array& aIds, TInt& aDefault);
	static void SetDefaultCalendarL(const TDesC& aStoreDir, const TDesC8& aId);
	// forgets the links between Agenda entries and server events
	static void ForgetL(const TDesC& aStoreDir);
	// adds an entry to the Agenda (aAlarm: minutes before, -1 none)
	static void AddToAgendaL(const TDesC& aFile, const TDesC& aTitle, const TDesC& aLocation,
		const TTime& aStart, const TTime& aEnd, TBool aAllDay, TInt aAlarm);
private:
	enum TPhase { EIdle, EOpen, EPushed, EMapped, ENew, ELocal, EFinish };
	CPmCalSync(MPmCalObserver& aObserver);
	void RunL();
	TInt RunError(TInt aError);
	void DoCancel();
	void Next();
	void Close();
	void Reset();
	// steps
	void OpenL();
	void ReadPushedL();
	TBool MappedStepL();
	TBool NewStepL();
	TBool LocalStepL();
	void FinishL();
	// files
	void Path(const TDesC& aName, TDes& aPath) const;
	HBufC8* ReadFileL(const TDesC& aName);
	void LoadEventsL();
	void LoadMapL();
	void LoadCalendarsL();
	void LoadPushL();
	void SaveMapL();
	void SavePushL();
	// events
	TPtrC8 Field(const TDesC8& aLine, TInt aIndex) const;
	TInt FindEvent(const TDesC8& aKey) const;
	void EventKey(const TDesC8& aLine, TDes8& aKey) const;
	TBool Writable(const TDesC8& aLine) const;
	// Agenda entries
	CAgnEntry* FetchL(TUint32 aAuid);
	TInt64 Changed(TUint32 aAuid);
	TUint32 AddFromEventL(const TDesC8& aLine);
	TUint32 UpdateFromEventL(TUint32 aAuid, const TDesC8& aLine);
	void FillL(CAgnEntry* aEntry, const TDesC8& aLine, const TTime& aStart);
	void DeleteL(TUint32 aAuid);
	void PushL(const TDesC8& aLine);
	void PushEntryL(TChar aOp, TUint32 aAuid, CAgnEntry* aEntry, const TDesC8& aEventLine);
	TInt AddMapL(TUint32 aAuid, const TDesC8& aKey, TUint32 aHash, TUint aFlags);
	void RemoveMap(TInt aIndex);
private:
	MPmCalObserver& iObserver;
	TPhase iPhase;
	TInt iPos;
	TFileName iDir;
	TPmCalSettings iSettings;
	RFs iFs;
	TBool iFsOpen;
	// the Agenda
	RAgendaServ* iServ;
	CAgnEntryModel* iModel;
	CParaFormatLayer* iPara;
	CCharFormatLayer* iChar;
	TAny* iIter;               // CAgnSyncIter
	TBool iServConnected;
	TBool iAgendaOpen;
	TBool iPushedRead;
	// what we know
	CArrayFixFlat<TPmCalEvent>* iEvents;
	CArrayFixFlat<TPmCalMap>* iMap;
	CArrayPtrFlat<HBufC8>* iPush;
	CDesC8ArrayFlat* iWritable;
	TBuf8<12> iDefaultCal;
	TInt64 iSince;
	TTime iWinStart, iWinEnd;
	TBool iNewFile;
	// what happened
	TInt iAdded, iChangedN, iRemoved, iSent, iFailed;
	};

#endif
