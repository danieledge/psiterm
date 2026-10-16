// PTXFER.H - PsiTerm's file transfer (File > Send file... / Get file...) and
// session log (File > Log to file...). The transfers run in psissh.exe
// (ssh/sftp.c) over SFTP on the SSH connection that is already logged in;
// this side picks the files, posts the request in the shared memory and
// shows the progress.

#ifndef __PTXFER_H
#define __PTXFER_H

#include <e32base.h>
#include <f32file.h>
#include <eikdialg.h>
#include <eiklbo.h>
#include <coecntrl.h>

class CTermView;
class CPeriodic;

// ---------------------------------------------------------------------------
// The session log: everything received, as plain text (escape sequences
// taken out, UTF-8 turned into the Psion's characters, lines ending CR LF)
// or raw (exactly as received)
// ---------------------------------------------------------------------------
class CPtLog : public CBase
	{
public:
	static CPtLog* NewL(RFs& aFs);
	~CPtLog();
	TInt Start(const TDesC& aFile, TBool aRaw, TBool aAppend);
	TBool Stop();                      // flushes and closes; EFalse (said why) if that failed
	void Write(const TUint8* aData, TInt aLen);
	void Flush();                      // (not while the terminal paints: may show an infoprint)
	TBool Active() const { return iOpen; }
	const TDesC& FileName() const { return iName; }
	TInt Bytes() const { return iBytes; }
private:
	CPtLog(RFs& aFs) : iFs(aFs) {}
	void Put(TUint8 aByte);
	void PutChar(TUint aCh);
	void DoFlush();
	void Fail(TInt aErr);
	TInt iFailErr;                     // why the log stopped, not yet said
	RFs& iFs;
	RFile iFile;
	TBool iOpen;
	TBool iRaw;
	TFileName iName;
	TBuf8<2048> iBuf;
	TInt iBytes;                       // written to the file so far
	// plain text: where in an escape sequence / UTF-8 character we are
	TInt iState;
	TUint iUtf;
	TInt iUtfLeft;
	};

// What the transfers used last (this run of PsiTerm): the server folder,
// per server, and the Psion folders for sending and saving. (An APP may
// not have writable static data, so the view keeps this.)
class CPtXferMemory : public CBase
	{
public:
	TBuf8<512> iRemoteDir;
	TBuf<100> iRemoteHost;
	TFileName iSendFrom;
	TFileName iSaveTo;
	// Scratch for one transfer at a time, here rather than on the stack: an
	// EIKON app thread's stack is small (the SDK's default is 8 KB) and
	// Send file -> BrowseL -> the folder dialog held about 4.7 KB of these
	// at once. (0.91: the file view keeps its own folders, on the heap.)
	TBuf8<512> iPick;                  // the chosen server file (Get) or folder (Send)
	TBuf8<512> iRemote;                // Send: the file's full name on the server
	TBuf<512> iText;                   // a server name as Psion text
	// 0.91: what the file view chose, for after it has closed
	TInt iBrowseResult;                // EPtBrowse*
	TUint iPickSize;                   // Get: the file's size
	};

enum { EPtBrowseNone, EPtBrowseGet, EPtBrowseSendHere };

// ---------------------------------------------------------------------------
// 0.91: Get file / Send file's file view. While browsing, the program's
// window is the file view (not a dialog over the terminal): the app UI puts
// it over the terminal (which goes on running under it), gives it the menus,
// shortcuts and toolbar of its own, and passes it their commands. The
// folders are listed in place: the view stays on screen while psissh lists
// the next one.
// ---------------------------------------------------------------------------
class CEikMenuPane;

class MPtBrowseHost
	{
public:
	// the view has finished (its choice in CPtXferMemory): close it soon -
	// not from inside the view's own code
	virtual void BrowseCloseSoon() = 0;
	// the toolbar's Favourite button: down while the folder shown is one
	virtual void BrowseFavouriteShown(TBool aOn) = 0;
	};

class CPtBrowseView : public CCoeControl
	{
public:
	virtual void CommandL(TInt aCommand) = 0;
	virtual void InitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane) = 0;
	virtual void SetViewRectL(const TRect& aRect) = 0;
	virtual void StartL() = 0;              // the first listing (once it is on screen)
	virtual TBool Sending() const = 0;
	};

CPtBrowseView* PtNewBrowseViewL(CTermView& aView, TBool aSend, MPtBrowseHost& aHost, const TRect& aRect);
// after the view has gone: the Get or Send it chose, if any
void PtBrowseFinishedL(CTermView& aView);

// File menu commands
void PtSendFileL(CTermView& aView);
void PtGetFileL(CTermView& aView);
void PtLogCommandL(CTermView& aView);
TBool PtLogging(const CTermView& aView);

#endif
