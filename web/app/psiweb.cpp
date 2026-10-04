// PSIWEB.CPP - PsiWeb.app: screen, keyboard, pen, menus and settings for the
// browser engine, psiweb.exe (Links 2 from 0.62: web/links; NetSurf before).
// See psiweb.h.

#include <e32keys.h>
#include <eikkeys.h>
#include <eikscrlb.h>
#include <e32hal.h>
#include <eikchlst.h>
#include <eikedwin.h>
#include <eiklabel.h>
#include <eikmfne.h>
#include <eikcmbut.h>
#include <eikbtpan.h>
#include <txtetext.h>
#include <eiktbar.h>
#include <eikimage.h>
#include <apgcli.h>
#include <eikcfdlg.h>
#include <eikseced.h>
#include <eikon.rsg>
#include "pwapp.h"
#include "pwicons.h"
#include "psilink.h"
#include "pglinktest.h"
#include "psigreyui.h"

// The link settings are shared with PsiTerm and PsiMail (psilink.h)
static void UseSharedLink(RFs& aFs, TPwSettings& aSettings)
	{
	TPsiLink link;
	if (!link.Load(aFs))
		return;
	aSettings.iBaudIndex = link.iBaudIndex;
	aSettings.iRtsCts = link.iRtsCts;
	aSettings.iNetMode = link.iNetMode;
	aSettings.iPppStart = link.iPppStart;
	}

static void SaveSharedLink(RFs& aFs, const TPwSettings& aSettings)
	{
	TPsiLink link, old;
	link.iBaudIndex = aSettings.iBaudIndex;
	link.iRtsCts = aSettings.iRtsCts ? 1 : 0;
	link.iNetMode = aSettings.iNetMode ? 1 : 0;
	link.iPppStart = aSettings.iPppStart;
	if (old.Load(aFs) && old.iBaudIndex == link.iBaudIndex && old.iRtsCts == link.iRtsCts
		&& old.iNetMode == link.iNetMode && old.iPppStart == link.iPppStart)
		return;
	link.Save(aFs);
	}

_LIT(KEngineExe, "psiweb.exe");
_LIT(KIniFile, "C:\\System\\Apps\\PsiWeb\\PsiWeb.ini");
_LIT(KVersion, "0.67");           // also web/pkg/psiweb.pkg and dist/PsiWeb-version.txt (two digits from 0.54: see the .pkg)
_LIT(KDefaultHome, "http://68k.news/");
const TInt KZoomSteps[] = { 50, 60, 70, 80, 90, 100, 110, 125, 150, 175, 200 };
const TInt KZoomCount = 11;
const TInt KTick = 62500;            // look for new frames 16 times a second
const TInt KTickQuiet = 2000000;     // (0.81) ...or every 2 s when nothing is going on: the doorbell brings news
const TInt KCalmTicks = 16;          // quick ticks with nothing going on before the slow pace (1 s)

static void CopyToC(char* aDst, TInt aMax, const TDesC& aSrc)
	{
	TPtr8 p((TUint8*)aDst, aMax - 1);
	p.Copy(aSrc.Left(aSrc.Length() < aMax - 1 ? aSrc.Length() : aMax - 1));
	p.ZeroTerminate();
	}

// UTF-8 from the engine -> text for the Psion's 8-bit (Windows-1252) UI
static void FromUtf8(TDes& aDst, const char* aSrc)
	{
	aDst.Zero();
	const TUint8* s = (const TUint8*)aSrc;
	while (*s && aDst.Length() < aDst.MaxLength())
		{
		TUint c = *s++;
		if (c >= 0x80)
			{
			TInt extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
			c &= 0x3f >> extra;
			while (extra-- && (*s & 0xc0) == 0x80)
				c = (c << 6) | (*s++ & 0x3f);
			if (c == 0x2018 || c == 0x2019) c = '\'';
			else if (c == 0x201c || c == 0x201d) c = '"';
			else if (c == 0x2013 || c == 0x2014) c = '-';
			else if (c > 0xff) c = '?';
			}
		aDst.Append(TChar(c));
		}
	}

// text from the Psion's 8-bit UI (Latin-1 here) -> UTF-8 for the engine
static void ToUtf8(char* aDst, TInt aMax, const TDesC& aSrc)
	{
	TInt n = 0;
	for (TInt i = 0; i < aSrc.Length(); i++)
		{
		TUint c = aSrc[i];
		if (c < 0x80)
			{
			if (n + 1 >= aMax) break;
			aDst[n++] = (char)c;
			}
		else
			{
			if (n + 2 >= aMax) break;
			aDst[n++] = (char)(0xc0 | (c >> 6));
			aDst[n++] = (char)(0x80 | (c & 0x3f));
			}
		}
	aDst[n] = 0;
	}

// ===========================================================================
// Engine watcher
// ===========================================================================

CPwWatcher::CPwWatcher(CPwView& aView)
	: CActive(EPriorityStandard), iView(aView)
	{
	CActiveScheduler::Add(this);
	}

CPwWatcher::~CPwWatcher()
	{
	Cancel();
	}

void CPwWatcher::Watch(RProcess& aProcess)
	{
	iProcess = &aProcess;
	aProcess.Logon(iStatus);
	SetActive();
	}

void CPwWatcher::RunL()
	{
	iView.EngineEnded();
	}

void CPwWatcher::DoCancel()
	{
	if (iProcess)
		iProcess->LogonCancel(iStatus);
	}

// ===========================================================================
// View
// ===========================================================================

CPwView::~CPwView()
	{
	StartBusyCancel();
	StopEngine();
	delete iSBFrame;
	delete iAsker;
	delete iStarter;
	delete iTimer;
	delete iBellWaiter;                  // (before the chunk it points into goes)
	iEngRinger.Close();
	delete iWatcher;
	delete iBitmap;
	if (iChunkOpen)
		iChunk.Close();
	}

void CPwView::ConstructL(const TRect& aRect, const TPwSettings& aSettings)
	{
	iSettings = aSettings;
	CreateBackedUpWindowL(iCoeEnv->RootWin(), EGray16);
	SetRectL(aRect);
	EnableDragEvents();
	iPics = -1;
	// the page's scroll bar, as the built-in programs have it: always there
	// (dimmed when the page fits), beside the page
	iSBFrame = new(ELeave) CEikScrollBarFrame(this, this);
	iSBFrame->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff, CEikScrollBarFrame::EOn);
	LayoutL();

	TSize size = iPageArea.Size();
	if (size.iWidth > PW_MAX_W) size.iWidth = PW_MAX_W;
	if (size.iHeight > PW_MAX_H) size.iHeight = PW_MAX_H;
	iBitmap = new(ELeave) CFbsBitmap;
	User::LeaveIfError(iBitmap->Create(TSize(PW_MAX_W, PW_MAX_H), EGray16));

	TInt r = iChunk.CreateGlobal(_L(PSI_SHARED_NAME), sizeof(PwShared), sizeof(PwShared));
	if (r == KErrAlreadyExists)
		r = iChunk.OpenGlobal(_L(PSI_SHARED_NAME), EFalse);
	User::LeaveIfError(r);
	iChunkOpen = ETrue;
	iShared = (PwShared*)iChunk.Base();
	Mem::FillZ(iShared, sizeof(PwShared));
	iShared->width = size.iWidth;
	iShared->height = size.iHeight;

	iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
	iTimer->Start(KTick, KTick, TCallBack(TickCallback, this));
	ActivateL();
	// started once the app is up, so a URL on the command line (from
	// PsiMail) can be the first page: see CPwAppUi::ProcessCommandParametersL
	iStarter = CIdle::NewL(CActive::EPriorityStandard);
	iStarter->Start(TCallBack(StartCallback, this));
	iAsker = CIdle::NewL(CActive::EPriorityStandard);
	}

TInt CPwView::StartCallback(TAny* aSelf)
	{
	CPwView* v = (CPwView*)aSelf;
	TRAPD(r, v->StartEngineL());
	(void)r;
	return 0;
	}

// a page to open: now if the engine is running, else as its first page
void CPwView::OpenUrlL(const TDesC& aUrl)
	{
	if (iRunning)
		Command(PW_CMD_OPEN, aUrl);
	else
		{
		iStartUrl = Clip(aUrl, iStartUrl.MaxLength());
		if (iStarter && !iStarter->IsActive())
			StartEngineL();
		}
	}

void CPwView::StartEngineL()
	{
	if (iRunning)
		return;
	PwShared* s = iShared;
	TInt w = s->width, h = s->height;
	if (iBellWaiter)
		iBellWaiter->Cancel();               // (before the bell it waits on is cleared)
	iEngRinger.Close();                      // (a new engine: a new thread)
	TickFast();                              // (and the tick follows it from the start)
	Mem::FillZ(s, sizeof(PwShared) - sizeof(s->fb));
	s->width = w;
	s->height = h;
	s->magic = PW_MAGIC;
	s->net.magic = PSI_SHARED_MAGIC;
	s->net.bell_magic = PSI_BELL_MAGIC;      // (0.81) we ring eng_bell with every key, tap, command and quit
	UseSharedLink(iCoeEnv->FsSession(), iSettings);   // may have changed in another app
	s->net.baud_index = iSettings.iBaudIndex;
	s->net.rtscts = iSettings.iRtsCts;
	s->net.net_mode = iSettings.iNetMode;
	CopyToC(s->net.ppp_start, sizeof(s->net.ppp_start), iSettings.iPppStart);
	s->net.port = 80;
	CopyToC(s->net.dial_prefix, sizeof(s->net.dial_prefix), _L("ATDT"));
	CopyToC(s->net.home, sizeof(s->net.home), _L("C:\\System\\Apps\\PsiWeb"));
	s->use_proxy = iSettings.iUseProxy;
	CopyToC(s->proxy_host, sizeof(s->proxy_host), iSettings.iProxyHost);
	s->proxy_port = iSettings.iProxyPort;
	s->load_images = iSettings.iImages;
	s->zoom = iSettings.iZoom;
	s->display = (iSettings.iDisplay & KPwDisplayScaledText) ? PW_DISPLAY_SCALED_TEXT : 0;
	CopyToC(s->net.version, sizeof(s->net.version), KVersion);
	// updates are saved to the CF card if there is one (D:), else C:
	TVolumeInfo vol;
	iUpdateFile.Copy(iCoeEnv->FsSession().Volume(vol, EDriveD) == KErrNone
		? _L("D:\\PsiWeb-update.sis") : _L("C:\\PsiWeb-update.sis"));
	CopyToC(s->net.save_as, sizeof(s->net.save_as), iUpdateFile);
	CopyToC(s->home_url, sizeof(s->home_url), iSettings.iHome);
	CopyToC(s->start_url, sizeof(s->start_url), iStartUrl);

	// the engine and its resources (Messages, CSS) live next to the app
	TParse parse;
	parse.Set(CEikonEnv::Static()->EikAppUi()->Application()->AppFullName(), NULL, NULL);
	TFileName dir(parse.DriveAndPath());
	CopyToC(s->res_dir, sizeof(s->res_dir), dir);
	TFileName exe(dir);
	exe.Append(KEngineExe);

	iLastFrame = 0;
	iMsg1.Zero();                        // a blank page until the engine draws
	iMsg2.Zero();
	iShowMsg = ETrue;
	DrawNow();
	// Loading the engine (1.9 MB) and its start-up take some seconds, during
	// which this thread is held in Create(): say so, bottom left, and flush
	// the window server's buffer so the message and the blank page show now
	// rather than when Create() returns. Tick takes it down when the first
	// page starts loading.
	if (!iStartBusy)
		StartBusy(_L("Starting the browser engine..."));
	iCoeEnv->WsSession().Flush();

	TInt r = iProcess.Create(exe, KNullDesC);
	if (r != KErrNone)
		{
		StartBusyCancel();
		TBuf<80> e;
		if (r == KErrNotFound || r == KErrPathNotFound)
			e = _L("psiweb.exe is not next to PsiWeb - reinstall PsiWeb from its .sis file");
		else
			e.Format(_L("Could not start psiweb.exe (%d) - reinstall PsiWeb from its .sis file"), r);
		ShowMessage(_L("The browser engine did not start"), e);
		return;
		}
	iRunning = ETrue;
	iStartUrl.Zero();                  // (kept if the engine could not start)
	if (!iWatcher)
		iWatcher = new(ELeave) CPwWatcher(*this);
	iWatcher->Watch(iProcess);
	iProcess.Resume();
	}

void CPwView::StartBusy(const TDesC& aText)
	{
	if (iStartBusy)
		iEikonEnv->BusyMsgCancel();
	iStartBusy = EFalse;
	TRAPD(err, iEikonEnv->BusyMsgL(aText, EHLeftVBottom, TTimeIntervalMicroSeconds32(0)));
	iStartBusy = err == KErrNone;
	iStartBusyAt = User::TickCount();
	}

void CPwView::StartBusyCancel()
	{
	if (iStartBusy)
		iEikonEnv->BusyMsgCancel();
	iStartBusy = EFalse;
	}

void CPwView::StopEngine()
	{
	if (!iRunning)
		return;
	iShared->quitting = 1;
	iShared->net.quit = 1;
	iShared->cmd = PW_CMD_QUIT;
	RingEngine();
	// give the engine a few seconds to hang up and close
	for (TInt i = 0; i < 40 && iShared->state != PW_STATE_EXITED; i++)
		User::After(100000);
	if (iWatcher)
		iWatcher->Cancel();
	if (iShared->state != PW_STATE_EXITED)
		{
		iProcess.Kill(0);
		PsiLinkTimersBack();             // it never got to give NIFMAN its timers back
		}
	iProcess.Close();
	iRunning = EFalse;
	}

// Stops the engine and starts it again, on the page it was showing if asked
// (settings that the engine reads at start, and the toolbar's room)
void CPwView::RestartL(TBool aSamePage)
	{
	TBuf<PW_URL_MAX> url;
	if (aSamePage && iRunning)
		{
		FromUtf8(url, iShared->url);
		if (url.Compare(_L("about:blank")) == 0)
			url.Zero();
		}
	StartBusy(_L("Restarting the browser engine..."));
	StopEngine();
	if (url.Length())
		iStartUrl = url;
	StartEngineL();
	}

// The page area changed (the toolbar was shown or hidden): the engine draws
// at the size it was started with, so it starts again, on the same page
void CPwView::SetPageRectL(const TRect& aRect)
	{
	TBool running = iRunning;
	TBuf<PW_URL_MAX> url;
	if (running)
		{
		FromUtf8(url, iShared->url);
		if (url.Compare(_L("about:blank")) == 0)
			url.Zero();
		StartBusy(_L("Restarting the browser engine..."));
		StopEngine();
		}
	SetRectL(aRect);
	LayoutL();
	TSize size = iPageArea.Size();
	if (size.iWidth > PW_MAX_W) size.iWidth = PW_MAX_W;
	if (size.iHeight > PW_MAX_H) size.iHeight = PW_MAX_H;
	iShared->width = size.iWidth;
	iShared->height = size.iHeight;
	if (running)
		{
		if (url.Length())
			iStartUrl = url;
		StartEngineL();
		}
	else
		DrawNow();
	}

// ----- the page's scroll bar --------------------------------------------------------

// The bar takes its breadth from the right of Rect(); the engine draws in
// what is left (iPageArea, at the window's origin)
void CPwView::LayoutL()
	{
	TEikScrollBarModel hModel(0, 0, 0);
	TEikScrollBarModel vModel(iSbH, iSbH > 0 ? iSbVh : 0, iSbH > 0 ? iSbY : 0);
	TRect inclusive(Rect());
	TRect client(Rect());
	TEikScrollBarFrameLayout layout;
	layout.iTilingMode = TEikScrollBarFrameLayout::EInclusiveRectConstant;
	iSBFrame->TileL(&hModel, &vModel, client, inclusive, layout);
	iPageArea = client;
	}

TInt CPwView::CountComponentControls() const
	{
	return iSBFrame ? iSBFrame->CountComponentControls() : 0;
	}

CCoeControl* CPwView::ComponentControl(TInt aIndex) const
	{
	return iSBFrame->ComponentControl(aIndex);
	}

// The engine has said where the page is (psiweb.h page_*): the thumb goes
// there, unless the pen is dragging it. Only a change from the engine moves
// it, so a tap on the bar is not undone while the engine catches up.
void CPwView::UpdateScrollBarL()
	{
	PwShared* s = iShared;
	TInt h = 0, y = 0, vh = 0;
	if (s && iRunning && !iShowMsg && s->page_h > 0 && s->page_vh > 0)
		{
		h = s->page_h;
		y = s->page_y;
		vh = s->page_vh;
		}
	if (iSbDragging || (h == iEngH && y == iEngY && vh == iEngVh))
		return;
	iEngH = h;
	iEngY = y;
	iEngVh = vh;
	if (h == iSbH && y == iSbY && vh == iSbVh)
		return;
	iSbH = h;
	iSbY = y;
	iSbVh = vh;
	CEikScrollBar* sb = iSBFrame->GetScrollBarHandle(CEikScrollBar::EVertical);
	if (!sb)
		return;
	TEikScrollBarModel model(h, h > 0 ? vh : 0, h > 0 ? y : 0);
	sb->SetModelL(&model);
	sb->DrawNow();
	}

// The pen on the bar: the arrows move a few lines, the shaft a screen (as
// Fn+Up and Fn+Down), the thumb where it is put
void CPwView::HandleScrollEventL(CEikScrollBar* aScrollBar, TEikScrollEvent aEventType)
	{
	if (!iRunning || iSbH <= 0 || iUpdState == PW_UPD_RUNNING)
		return;
	TInt line = iSbVh / 8 < 16 ? 16 : iSbVh / 8;
	TInt y = iSbY;
	switch (aEventType)
		{
	case EEikScrollUp:
		y -= line;
		break;
	case EEikScrollDown:
		y += line;
		break;
	case EEikScrollPageUp:
		y -= iSbVh - line;
		break;
	case EEikScrollPageDown:
		y += iSbVh - line;
		break;
	case EEikScrollTop:
	case EEikScrollHome:
		y = 0;
		break;
	case EEikScrollBottom:
	case EEikScrollEnd:
		y = iSbH;
		break;
	case EEikScrollThumbDragVert:
		iSbDragging = ETrue;
		y = aScrollBar->ThumbPosition();
		break;
	case EEikScrollThumbReleaseVert:
		iSbDragging = EFalse;
		y = aScrollBar->ThumbPosition();
		break;
	default:
		return;
		}
	ScrollTo(y);
	}

void CPwView::ScrollTo(TInt aY)
	{
	if (aY > iSbH - iSbVh) aY = iSbH - iSbVh;
	if (aY < 0) aY = 0;
	if (aY == iSbY && !iSbDragging)
		return;
	iSbY = aY;
	CEikScrollBar* sb = iSBFrame->GetScrollBarHandle(CEikScrollBar::EVertical);
	if (sb && !iSbDragging)
		{
		sb->SetModelThumbPosition(aY);
		sb->DrawNow();
		}
	TBuf<12> arg;
	arg.Num(aY);
	Command(PW_CMD_SCROLL, arg);
	}

// the page's pictures are showing: on every page (Preferences), or asked
// for this one (View > Show pictures, the Pictures button)
TBool CPwView::PicturesShown() const
	{
	return iSettings.iImages || (iRunning && iShared && iShared->page_pics);
	}

void CPwView::EngineEnded()
	{
	TExitType type = iProcess.ExitType();
	TInt reason = iProcess.ExitReason();
	TExitCategoryName cat = iProcess.ExitCategory();
	iProcess.Close();
	iRunning = EFalse;
	StartBusyCancel();
	if (iUpdState == PW_UPD_RUNNING || iUpdState == -1)
		{
		// the engine went while updating: the progress window must not wait for it
		iUpdState = PW_UPD_FAILED;
		iUpdMsg = _L("The browser engine stopped");
		UpdateLine(iUpdMsg);
		if (iUpdDlg)
			{
			TRAPD(fe, iUpdDlg->FinishL(iUpdLog));
			(void)fe;
			}
		}
	if (iShared->quitting)
		return;
	TBuf<120> why;
	if (type == EExitPanic)
		PsiLinkTimersBack();             // (0.54) the crash skipped the engine's own clean-up
	if (type == EExitPanic)
		why.Format(_L("%S %d - Tools > Restart browser engine starts it again"), &cat, reason);
	else if (iShared->exit_msg[0])
		{
		FromUtf8(why, iShared->exit_msg);
		}
	else
		why.Format(_L("It closed (%d) - Tools > Restart browser engine starts it again"), reason);
	ShowMessage(_L("The browser engine has stopped"), why);
	}

void CPwView::ShowMessage(const TDesC& aLine1, const TDesC& aLine2)
	{
	iMsg1 = aLine1.Left(aLine1.Length() < 80 ? aLine1.Length() : 80);
	iMsg2 = aLine2.Left(aLine2.Length() < 120 ? aLine2.Length() : 120);
	iShowMsg = ETrue;
	DrawNow();
	}

void CPwView::Command(TInt aCmd, const TDesC& aArg)
	{
	if (!iRunning)
		{
		iEikonEnv->InfoMsg(_L("Not available - the browser engine has stopped"));
		return;
		}
	CopyToC(iShared->cmd_arg, sizeof(iShared->cmd_arg), aArg);
	if (aCmd == PW_CMD_STOP)
		iShared->net.quit = 1;          // also interrupts a dial or download
	iShared->cmd = aCmd;
	iShared->net.resized = 1;            // wakes the engine's wait (see psi_os.c)
	RingEngine();
	TickFast();
	}

// (0.81) The engine waits on a doorbell when nothing is happening
// (psibell.h): every key, tap, command, answer and quit rings it.
void CPwView::RingEngine()
	{
	if (iRunning && iShared)
		iEngRinger.Ring(&iShared->net.eng_bell);
	}

// Is anything going on that the tick must follow closely? A page loading,
// frames coming (within the last second), input not yet taken, a message
// about the link, a question, an update, the start-up message.
TBool CPwView::TickWanted() const
	{
	PwShared* s = iShared;
	if (!s || iStartBusy || iLinkBusy || iAsking || iUpdState == PW_UPD_RUNNING)
		return ETrue;
	if (!iRunning)
		return EFalse;                       // (the engine has stopped: nothing to follow)
	if (s->busy || s->state != PW_STATE_READY || s->ev_tail != s->ev_head || s->cmd != PW_CMD_NONE)
		return ETrue;
	if (s->frame_seq != iLastFrame || s->net.link_seq != iLinkSeq || s->update_state != iUpdState)
		return ETrue;
	if (s->auth_state == PW_ASK_ASKING || s->save_state == PW_ASK_ASKING)
		return ETrue;
	if (User::TickCount() - iFrameAt < 64)
		return ETrue;
	return EFalse;
	}

void CPwView::TickFast()
	{
	iCalmTicks = 0;
	if (!iTickQuiet || !iTimer)
		return;
	iTickQuiet = EFalse;
	iTimer->Cancel();
	iTimer->Start(KTick, KTick, TCallBack(TickCallback, this));
	}

void CPwView::TickQuiet()
	{
	iCalmTicks = 0;
	if (iTickQuiet || !iTimer || !iShared)
		return;
	if (!iBellWaiter)
		iBellWaiter = new CPsiBellWaiter(TCallBack(BellCallback, this));   // (no memory: stays quick)
	if (!iBellWaiter || iBellWaiter->Arm(&iShared->net.app_bell) || !iBellWaiter->IsActive())
		return;                              // (rung just now, or no doorbell: stays quick)
	iTickQuiet = ETrue;
	iTimer->Cancel();
	iTimer->Start(KTickQuiet, KTickQuiet, TCallBack(TickCallback, this));
	}

TInt CPwView::BellCallback(TAny* aSelf)
	{
	CPwView* self = (CPwView*)aSelf;
	self->TickFast();
	self->Tick();
	return 0;
	}

TInt CPwView::TickCallback(TAny* aSelf)
	{
	((CPwView*)aSelf)->Tick();
	return 1;
	}

// copies the rows the engine has redrawn into our bitmap
void CPwView::Tick()
	{
	PwShared* s = iShared;
	if (s)
		s->app_beat++;                   // "still here": see pwepoc.cpp
	// (0.81) the pace: quick while anything is going on, else slow with the
	// doorbell armed (the heartbeat above still goes every 2 s)
	if (TickWanted())
		TickFast();
	else if (!iTickQuiet && ++iCalmTicks >= KCalmTicks)
		TickQuiet();
	// "Starting the browser engine...": down once the engine starts loading
	// its first page (its window is up by then; its first frame is only the
	// blank background, seconds before that), or after a minute regardless
	if (s && iStartBusy && iRunning && (s->busy || User::TickCount() - iStartBusyAt > 64 * 60))
		StartBusyCancel();
	if (s && s->net.link_seq != iLinkSeq)
		{
		// what the connection is doing (dialling, looking up, connecting):
		// the engine is busy then and can't draw its own status line. A
		// note that ends in "..." is a busy message, bottom left, until the
		// page has loaded; anything else is an infoprint, top right
		iLinkSeq = s->net.link_seq;
		TBuf<80> m;
		FromUtf8(m, s->net.link_msg);
		m.Trim();
		// (Right(3) of a shorter text panics USER 22: a one- or two-letter
		// note, e.g. after NO CARRIER, closed PsiWeb up to 0.56)
		TBool dots = m.Length() >= 3 && m.Right(3).Compare(_L("...")) == 0;
		if (m.Length() && m[m.Length() - 1] == '.' && !dots)
			m.SetLength(m.Length() - 1);     // no full stop at the end of a message
		if (iUpdDlg)
			{
			// updating: the progress window shows each step of getting online
			if (m.Length())
				{
				UpdateLine(m);
				TRAPD(ue, iUpdDlg->ShowL(iUpdLog));
				(void)ue;
				}
			}
		else if (dots)
			{
			TRAPD(err, iEikonEnv->BusyMsgL(m, EHLeftVBottom, TTimeIntervalMicroSeconds32(0)));
			iLinkBusy = err == KErrNone;
			}
		else if (m.Length())
			{
			// a plain note after a busy one ("Checking the modem..." then
			// "CONNECT 115200"): that step is over
			if (iLinkBusy)
				{
				iEikonEnv->BusyMsgCancel();
				iLinkBusy = EFalse;
				}
			iEikonEnv->InfoMsg(m);
			}
		}
	if (s && iLinkBusy && !s->busy)
		{
		iEikonEnv->BusyMsgCancel();
		iLinkBusy = EFalse;
		}
	// the engine asks something (a password, where to save a file): the
	// dialog is shown from an idle callback, not inside this tick
	if (s && !iAsking && iAsker && !iAsker->IsActive()
		&& (s->auth_state == PW_ASK_ASKING || s->save_state == PW_ASK_ASKING))
		iAsker->Start(TCallBack(AskCallback, this));
	if (s && (iUpdState == PW_UPD_RUNNING || s->update_state != iUpdState))
		{
		TRAPD(err, UpdateTickL());
		(void)err;
		}
	if (s && iRunning && s->page_pics != iPics)
		{
		iPics = s->page_pics;
		((CPwAppUi*)iEikonEnv->EikAppUi())->ShowPicturesState(PicturesShown());
		}
	if (!s || s->frame_seq == iLastFrame || iUpdState == PW_UPD_RUNNING)
		{
		TRAPD(se, UpdateScrollBarL());
		(void)se;
		return;
		}
	iLastFrame = s->frame_seq;
	iFrameAt = User::TickCount();
	TInt y0 = s->dirty_y0, y1 = s->dirty_y1;
	s->dirty_y0 = s->height;
	s->dirty_y1 = 0;
	if (y0 < 0) y0 = 0;
	if (y1 > s->height) y1 = s->height;
	if (y0 >= y1)
		{
		// the engine widened the band just as we reset it: copy it all
		y0 = 0;
		y1 = s->height;
		}
	// the shared buffer has exactly an EGray16 scan line's layout
	for (TInt y = y0; y < y1; y++)
		{
		TPtr8 row(s->fb + y * PW_STRIDE, PW_STRIDE, PW_STRIDE);
		iBitmap->SetScanLine(row, y);
		}
	iShowMsg = EFalse;
	// a backed-up window: draw straight in, no redraw cycle
	TRect r(0, y0, s->width, y1);
	ActivateGc();
	SystemGc().BitBlt(r.iTl, iBitmap, r);
	DeactivateGc();
	TRAPD(se, UpdateScrollBarL());
	(void)se;
	}

// ----- the engine's questions (Links phase 5) --------------------------------------

TInt CPwView::AskCallback(TAny* aSelf)
	{
	CPwView* v = (CPwView*)aSelf;
	PwShared* s = v->iShared;
	if (!s || v->iAsking)
		return 0;
	v->iAsking = ETrue;
	if (v->iLinkBusy)
		{
		CEikonEnv::Static()->BusyMsgCancel();
		v->iLinkBusy = EFalse;
		}
	if (s->auth_state == PW_ASK_ASKING)
		{
		TRAPD(err, v->AskAuthL());
		if (err != KErrNone && s->auth_state == PW_ASK_ASKING)
			s->auth_state = PW_ASK_CANCEL;
		}
	if (s->save_state == PW_ASK_ASKING)
		{
		TRAPD(err, v->AskSaveL());
		if (err != KErrNone && s->save_state == PW_ASK_ASKING)
			s->save_state = PW_ASK_CANCEL;
		}
	s->net.resized = 1;                  // wakes the engine's wait (psi_os.c)
	v->RingEngine();
	v->iAsking = EFalse;
	return 0;
	}

// User name and password: who asks, and the server's name for it
void CPwView::AskAuthL()
	{
	PwShared* s = iShared;
	TBuf<64> host;
	TBuf<96> realm;
	FromUtf8(host, s->auth_host);
	FromUtf8(realm, s->auth_realm);
	TBuf<100> who;
	TPtrC h = Clip(host, 60);
	if (s->auth_proxy)
		who.Format(_L("The proxy %S needs a password"), &h);
	else
		who.Format(_L("%S needs a password"), &h);
	TBuf<100> what;
	if (realm.Length() && realm.Compare(host))
		{
		TPtrC r = Clip(realm, 90);
		what.Format(_L("\"%S\""), &r);
		}
	TBuf<60> user;
	TBuf<CEikSecretEditor::EMaxSecEdLength> pass;
	CPwAuthDialog* dlg = new(ELeave) CPwAuthDialog(who, what, user, pass);
	if (dlg->ExecuteLD(R_PW_AUTH_DIALOG))
		{
		ToUtf8(s->auth_user, sizeof(s->auth_user), user);
		ToUtf8(s->auth_pass, sizeof(s->auth_pass), pass);
		s->auth_state = PW_ASK_OK;
		}
	else
		s->auth_state = PW_ASK_CANCEL;
	pass.FillZ();
	}

void CPwAuthDialog::PreLayoutDynInitL()
	{
	((CEikLabel*)Control(EPwDlgAuthWho))->SetTextL(Clip(iWho, 60));
	if (iRealm.Length())
		((CEikLabel*)Control(EPwDlgAuthRealm))->SetTextL(Clip(iRealm, 70));
	else
		MakeLineVisible(EPwDlgAuthRealm, EFalse);
	}

TBool CPwAuthDialog::OkToExitL(TInt /*aButtonId*/)
	{
	((CEikEdwin*)Control(EPwDlgAuthUser))->GetText(iUser);
	iUser.Trim();
	if (iUser.Length() == 0)
		{
		TryChangeFocusToL(EPwDlgAuthUser);
		iEikonEnv->InfoMsg(_L("No user name entered"));
		return EFalse;
		}
	((CEikSecretEditor*)Control(EPwDlgAuthPass))->GetText(iPass);
	return ETrue;
	}

// A file PsiWeb cannot show: say what it is, then the standard Save as
// dialog (on the Memory disk if there is one). The engine saves it, with
// its progress as the busy message and "Saved" at the end.
void CPwView::AskSaveL()
	{
	PwShared* s = iShared;
	TBuf<64> name, type;
	FromUtf8(name, s->save_name);
	FromUtf8(type, s->save_type);
	for (TInt i = 0; i < name.Length(); i++)
		{
		TChar c = name[i];
		if (c < ' ' || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
			name[i] = '_';
		}
	name.Trim();
	if (name.Length() == 0)
		name = _L("Download");
	TBuf<120> what;
	TPtrC n = Clip(name, 36), t = Clip(type, 30);
	if (s->save_size >= 0)
		what.Format(_L("PsiWeb cannot show \"%S\" (%S, %d KB)"), &n, &t, (s->save_size + 1023) / 1024);
	else
		what.Format(_L("PsiWeb cannot show \"%S\" (%S)"), &n, &t);
	if (!iEikonEnv->QueryWinL(what, _L("Save it to a file?")))
		{
		s->save_state = PW_ASK_CANCEL;
		return;
		}
	RFs& fs = iCoeEnv->FsSession();
	TDriveInfo di;
	TBool memDisk = fs.Drive(di, EDriveD) == KErrNone && di.iType != EMediaNotPresent;
	TFileName path;
	path = memDisk ? _L("D:\\Documents\\") : _L("C:\\Documents\\");
	fs.MkDirAll(path);
	path.Append(name);
	TBuf<40> title(_L("Save to file"));
	CEikFileSaveAsDialog* dlg = new(ELeave) CEikFileSaveAsDialog(&path, &title, NULL, EFalse);
	if (!dlg->ExecuteLD(R_EIK_DIALOG_FILE_SAVEAS))
		{
		s->save_state = PW_ASK_CANCEL;
		return;
		}
	// room for it? (if the size is not known, the engine finds out)
	TInt drive;
	TVolumeInfo vol;
	if (s->save_size >= 0 && RFs::CharToDrive(path[0], drive) == KErrNone && fs.Volume(vol, drive) == KErrNone
		&& vol.iFree < TInt64(s->save_size) + TInt64(16384))
		{
		TBuf<80> m;
		m.Format(_L("Not enough room on the disk - the file is %d KB"), (s->save_size + 1023) / 1024);
		iEikonEnv->InfoMsg(m);
		s->save_state = PW_ASK_CANCEL;
		return;
		}
	fs.MkDirAll(TParsePtrC(path).DriveAndPath());
	ToUtf8(s->save_path, sizeof(s->save_path), path);
	s->save_state = PW_ASK_OK;
	}

// ----- Update PsiWeb ------------------------------------------------------------

void CPwView::StartUpdateL()
	{
	if (!iRunning)
		{
		iEikonEnv->InfoMsg(_L("Not available - the browser engine has stopped"));
		return;
		}
	if (iUpdState == PW_UPD_RUNNING)
		{
		iEikonEnv->InfoMsg(_L("Not available while updating"));
		return;
		}
	iShared->update_state = PW_UPD_RUNNING;
	iUpdState = -1;                      // (the first tick shows the engine's first words)
	iUpdLog.Zero();
	UpdateLine(_L("Starting the update..."));
	Command(PW_CMD_UPDATE, KNullDesC);
	UpdateDialogL();
	}

// A line for the progress window. A download count ("12 of 2700 KB")
// replaces the last line if that was one too, so the window shows steps and
// not a hundred counts.
void CPwView::UpdateLine(const TDesC& aLine)
	{
	TBool count = aLine.Find(_L(" KB")) >= 0;
	TInt last = iUpdLog.LocateReverse(CEditableText::EParagraphDelimiter);
	if (count && iUpdLog.Length() > 0 && iUpdLog.Mid(last + 1).Find(_L(" KB")) >= 0)
		iUpdLog.SetLength(last >= 0 ? last : 0);
	if (iUpdLog.Length() > 0)
		iUpdLog.Append(CEditableText::EParagraphDelimiter);
	while (iUpdLog.Length() + aLine.Length() + 1 > iUpdLog.MaxLength())
		{
		TInt p = iUpdLog.Locate(CEditableText::EParagraphDelimiter);
		if (p < 0)
			{
			iUpdLog.Zero();
			break;
			}
		iUpdLog.Delete(0, p + 1);
		}
	iUpdLog.Append(aLine.Left(aLine.Length() < 120 ? aLine.Length() : 120));
	}

// The progress window stays up while the engine works (Stop asks it to give
// up); the tick adds its words and, at the end, turns Stop into Close. What
// the update came to is then said: install it, saved, current, failed.
void CPwView::UpdateDialogL()
	{
	CPwUpdateProgress* dlg = new(ELeave) CPwUpdateProgress(*this);
	iUpdDlg = dlg;
	TRAPD(err, dlg->ExecuteLD(R_PW_UPDATE_PROGRESS));
	iUpdDlg = NULL;
	User::LeaveIfError(err);
	if (iUpdState == PW_UPD_READY)
		{
		TBuf<64> q;
		TBuf<16> v;
		FromUtf8(v, iShared->update_version);
		q.Format(_L("Install PsiWeb %S now?"), &v);
		if (iEikonEnv->QueryWinL(_L("PsiWeb will close while it installs"), q))
			StartInstallerL();
		else
			iEikonEnv->InfoWinL(_L("Update saved"), iUpdateFile);
		}
	}

// progress while the engine downloads; the outcome when it has finished
void CPwView::UpdateTickL()
	{
	PwShared* s = iShared;
	TInt st = s->update_state;
	TBuf<128> msg;
	FromUtf8(msg, s->update_msg);
	if (st == PW_UPD_RUNNING)
		{
		if (iUpdState != PW_UPD_RUNNING || msg != iUpdMsg)
			{
			iUpdState = PW_UPD_RUNNING;
			iUpdMsg = msg;
			UpdateLine(msg);
			if (iUpdDlg)
				iUpdDlg->ShowL(iUpdLog);
			}
		return;
		}
	TInt was = iUpdState;
	iUpdState = st;
	if (was != PW_UPD_RUNNING && was != -1)
		return;
	// it has ended: the last words, and Stop becomes Close. What it came
	// to is said when the window closes (UpdateDialogL).
	iUpdMsg = msg;
	UpdateLine(msg);
	if (iUpdDlg)
		iUpdDlg->FinishL(iUpdLog);
	}

void CPwUpdateProgress::SetSizeAndPositionL(const TSize& aSize)
	{
	TSize screen = iEikonEnv->ScreenDevice()->SizeInPixels();
	TSize size(aSize.iWidth < screen.iWidth - 8 ? aSize.iWidth : screen.iWidth - 8,
		aSize.iHeight < screen.iHeight - 8 ? aSize.iHeight : screen.iHeight - 8);
	SetCornerAndSizeL(EHCenterVCenter, size);   // what CEikDialog does, clamped
	}

void CPwUpdateProgress::PreLayoutDynInitL()
	{
	ShowL(iView.UpdateLog());
	}

// The last few lines showing (the text scrolls with the arrow keys)
void CPwUpdateProgress::ShowL(const TDesC& aText)
	{
	CEikEdwin* ed = (CEikEdwin*)Control(EPwDlgUpdText);
	ed->SetTextL(&aText);
	TInt pos = aText.Length(), paras = 0;
	while (pos > 0)
		{
		if (aText[pos - 1] == CEditableText::EParagraphDelimiter && ++paras >= 6)
			break;
		pos--;
		}
	ed->SetCursorPosL(aText.Length(), EFalse);
	ed->SetCursorPosL(pos, EFalse);
	ed->DrawNow();
	}

// One button: "Stop" while the job runs, "Close" afterwards
void CPwUpdateProgress::SetButtonTextL(const TDesC& aText)
	{
	CEikCommandButtonBase* b = ButtonPanel()->ButtonById(EEikBidOk);
	if (b)
		{
		((CEikCommandButton*)b)->SetTextL(aText);
		b->DrawNow();
		}
	}

void CPwUpdateProgress::FinishL(const TDesC& aText)
	{
	if (iFinished)
		return;
	iFinished = ETrue;
	ShowL(aText);
	SetButtonTextL(_L("Close"));
	}

TBool CPwUpdateProgress::OkToExitL(TInt /*aButtonId*/)
	{
	if (iFinished)
		return ETrue;
	iView.StopUpdate();                  // Stop (or Esc): ask the updater to give up, stay open
	return EFalse;
	}

void CPwView::StartInstallerL()
	{
	StartBusyCancel();
	StopEngine();                        // psiweb.exe is one of the files replaced
	TInt err;
	RApaLsSession ls;
	err = ls.Connect();
	if (err == KErrNone)
		{
		TThreadId tid;
		err = ls.StartDocument(iUpdateFile, TUid::Uid(0x10000419), tid);
		ls.Close();
		}
	if (err == KErrNone)
		{
		iEikonEnv->EikAppUi()->HandleCommandL(EEikCmdExit);
		return;
		}
	TBuf<160> m;
	m.Format(_L("Could not start the installer (%d) - open %S from the System screen"), err, &iUpdateFile);
	iEikonEnv->InfoWinL(_L("Update downloaded"), m);
	}

void CPwView::Draw(const TRect& aRect) const
	{
	CWindowGc& gc = SystemGc();
	if (iShowMsg)
		{
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		gc.SetBrushColor(KRgbWhite);
		gc.SetPenStyle(CGraphicsContext::ENullPen);
		gc.DrawRect(iPageArea);
		gc.SetPenStyle(CGraphicsContext::ESolidPen);
		gc.SetPenColor(KRgbBlack);
		const CFont* font = iEikonEnv->TitleFont();
		gc.UseFont(font);
		gc.DrawText(iMsg1, TPoint(20, 90));
		gc.DiscardFont();
		font = iEikonEnv->NormalFont();
		gc.UseFont(font);
		gc.DrawText(iMsg2, TPoint(20, 120));
		gc.DiscardFont();
		return;
		}
	TRect r(aRect);
	r.Intersection(iPageArea);           // (the scroll bar draws itself)
	if (!r.IsEmpty())
		gc.BitBlt(r.iTl, iBitmap, r);
	}

void CPwView::PushEvent(TInt aType, TInt aCode, TInt aX, TInt aY)
	{
	PwShared* s = iShared;
	if (!iRunning || s->ev_head - s->ev_tail >= PW_EVQ)
		return;
	PwEvent& e = s->ev[s->ev_head % PW_EVQ];
	e.type = aType;
	e.code = aCode;
	e.x = aX;
	e.y = aY;
	s->ev_head++;
	s->net.resized = 1;                  // wakes the engine's wait (see psi_os.c)
	RingEngine();                        // (0.81) it may be waiting on its doorbell
	TickFast();                          // ...and the frame it draws is wanted soon
	}

// Key and pen timings: the randomness behind TLS keys (see psiglue pg_entropy)
void CPwView::AddEntropy(TUint aValue)
	{
	TUint32 t = User::TickCount() ^ (aValue << 16);
	TTime now;
	now.UniversalTime();
	t ^= now.Int64().Low();
	iShared->net.entropy[iEntropyPos % PSI_ENTROPY_SIZE] ^= (TUint8)t;
	iShared->net.entropy[(iEntropyPos + 1) % PSI_ENTROPY_SIZE] ^= (TUint8)(t >> 8);
	iEntropyPos += 2;
	if (iShared->net.entropy_len < PSI_ENTROPY_SIZE)
		iShared->net.entropy_len = iEntropyPos < PSI_ENTROPY_SIZE ? iEntropyPos : PSI_ENTROPY_SIZE;
	}

void CPwView::Key(TUint aCode, TUint aMods)
	{
	PushEvent(PW_EV_KEYDOWN, aCode, aMods, 0);
	}

TKeyResponse CPwView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKey)
		return EKeyWasNotConsumed;
	TUint code = aKeyEvent.iCode;
	TUint mods = aKeyEvent.iModifiers;
	if (code == EKeyMenu)
		return EKeyWasNotConsumed;
	// the Zoom in and Zoom out icons beside the screen: as View > Zoom in
	// and Zoom out (Ctrl+M, Shift+Ctrl+M)
	if (code == (TUint)EEikSidebarZoomInKey || code == (TUint)EEikSidebarZoomOutKey)
		{
		iEikonEnv->EikAppUi()->HandleCommandL(code == (TUint)EEikSidebarZoomInKey ? EPwCmdZoomIn : EPwCmdZoomOut);
		return EKeyWasConsumed;
		}
	AddEntropy(code);
	if (mods & EModifierCtrl)
		{
		// leave the menu's shortcuts to the menu (see psiweb.rss);
		// Ctrl+letter arrives as 1..26
		TUint letter = (code >= 1 && code <= 26) ? 'a' + code - 1 : (code | 0x20);
		if (mods & EModifierShift)
			{
			// Shift+Ctrl+M zoom out, Q page information, H help, A about,
			// R Reading mode
			switch (letter)
				{
				case 'm': case 'q': case 'h': case 'a': case 'r':
					return EKeyWasNotConsumed;
				}
			}
		else
			{
			switch (letter)
				{
				case 'e': case 'l': case 'o': case 'h': case 'r': case 'b':
				case 'f': case 'm': case 'i': case 't': case 'k': case 'u':
					return EKeyWasNotConsumed;
				}
			}
		}
	if (code == EKeyEscape && iUpdState == PW_UPD_RUNNING)
		{
		StopUpdate();                    // (the progress window has it too)
		return EKeyWasConsumed;
		}
	if (iUpdState == PW_UPD_RUNNING)
		return EKeyWasConsumed;
	if (code == EKeyEscape && iShared->busy)
		{
		Command(PW_CMD_STOP, KNullDesC);
		return EKeyWasConsumed;
		}
	Key(code, mods);
	return EKeyWasConsumed;
	}

void CPwView::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	TPoint p = aEvent.iPosition;
	AddEntropy(p.iX * 1000 + p.iY);
	// the pen went down on the scroll bar: it has the pen until it comes up
	if (aEvent.iType == TPointerEvent::EButton1Down)
		iSbPen = !iPageArea.Contains(p);
	if (iSbPen)
		{
		CCoeControl::HandlePointerEventL(aEvent);
		if (aEvent.iType == TPointerEvent::EButton1Up)
			iSbPen = EFalse;
		return;
		}
	switch (aEvent.iType)
		{
	case TPointerEvent::EButton1Down:
		PushEvent(PW_EV_PENDOWN, 0, p.iX, p.iY);
		break;
	case TPointerEvent::EDrag:
		PushEvent(PW_EV_PENMOVE, 0, p.iX, p.iY);
		break;
	case TPointerEvent::EButton1Up:
		PushEvent(PW_EV_PENUP, 0, p.iX, p.iY);
		break;
	default:
		break;
		}
	}

// ===========================================================================
// Dialogs
// ===========================================================================

void CPwInfoDialog::PreLayoutDynInitL()
	{
	for (TInt i = 0; i < 5; i++)
		{
		if (i < iCount)
			SetLabelL(EPwDlgInfo1 + i, iLines[i]);
		else
			MakeLineVisible(EPwDlgInfo1 + i, EFalse);
		}
	}

void CPwOpenDialog::PreLayoutDynInitL()
	{
	CEikEdwin* ed = (CEikEdwin*)Control(EPwDlgUrl);
	ed->SetTextL(&iUrl);
	ed->SelectAllL();
	}

TBool CPwOpenDialog::OkToExitL(TInt /*aButtonId*/)
	{
	((CEikEdwin*)Control(EPwDlgUrl))->GetText(iUrl);
	iUrl.Trim();
	if (iUrl.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No address entered"));
		TryChangeFocusToL(EPwDlgUrl);
		return EFalse;
		}
	return ETrue;
	}

void CPwConnDialog::PreLayoutDynInitL()
	{
	((CEikChoiceList*)Control(EPwDlgLink))->SetCurrentItem(iSettings.iNetMode ? 1 : 0);
	((CEikChoiceList*)Control(EPwDlgBaud))->SetCurrentItem(iSettings.iBaudIndex);
	((CEikChoiceList*)Control(EPwDlgFlow))->SetCurrentItem(iSettings.iRtsCts ? 1 : 0);
	SetEdwinTextL(EPwDlgPppStart, &iSettings.iPppStart);
	}

// Test: tries the values shown (not yet saved). If the browser engine has
// the port (mid-connection), the test says the port is in use.
void CPwConnDialog::TestL()
	{
	TBuf<40> ppp;
	GetEdwinText(ppp, EPwDlgPppStart);
	ppp.TrimAll();
	PgLinkTestL(((CEikChoiceList*)Control(EPwDlgBaud))->CurrentItem(),
		((CEikChoiceList*)Control(EPwDlgFlow))->CurrentItem() == 1,
		((CEikChoiceList*)Control(EPwDlgLink))->CurrentItem() == 1,
		ppp, R_PW_TEST_DIALOG, EPwDlgTest1);
	}

TBool CPwConnDialog::OkToExitL(TInt aButtonId)
	{
	if (aButtonId == EPwBidTest)
		{
		TestL();
		return EFalse;
		}
	iSettings.iNetMode = ((CEikChoiceList*)Control(EPwDlgLink))->CurrentItem() == 1;
	iSettings.iBaudIndex = ((CEikChoiceList*)Control(EPwDlgBaud))->CurrentItem();
	iSettings.iRtsCts = ((CEikChoiceList*)Control(EPwDlgFlow))->CurrentItem() == 1;
	TBuf<40> ppp;
	GetEdwinText(ppp, EPwDlgPppStart);
	ppp.TrimAll();
	iSettings.iPppStart = ppp;
	return ETrue;
	}

void CPwAboutDialog::PreLayoutDynInitL()
	{
	TBuf<32> title(_L("PsiWeb "));
	title.Append(KVersion);
	// the name and version are the heading: the dialog title font (the
	// legend font is smaller than the bold line under it)
	((CEikLabel*)Control(EPwDlgAbout1))->SetFont(iEikonEnv->TitleFont());
	SetLabelL(EPwDlgAbout1, title);
	SetLabelL(EPwDlgAboutStatus, iStatus);
	}

void CPwUpdateDialog::PreLayoutDynInitL()
	{
	((CEikChoiceList*)Control(EPwDlgUpdSource))->SetCurrentItem(iSource >= 0 && iSource <= 2 ? iSource : 0);
	SetEdwinTextL(EPwDlgUpdHost, &iHost);
	SetNumberEditorValue(EPwDlgUpdPort, iPort > 0 ? iPort : 8686);
	}

TBool CPwUpdateDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iSource = ((CEikChoiceList*)Control(EPwDlgUpdSource))->CurrentItem();
	GetEdwinText(iHost, EPwDlgUpdHost);
	iHost.Trim();
	iPort = NumberEditorValue(EPwDlgUpdPort);
	if (iSource == 2 && iHost.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No local server entered"));
		TryChangeFocusToL(EPwDlgUpdHost);
		return EFalse;
		}
	return ETrue;
	}

void CPwPrefsDialog::PreLayoutDynInitL()
	{
	((CEikChoiceList*)Control(EPwDlgProxy))->SetCurrentItem(iSettings.iUseProxy ? 1 : 0);
	SetEdwinTextL(EPwDlgProxyHost, &iSettings.iProxyHost);
	SetNumberEditorValue(EPwDlgProxyPort, iSettings.iProxyPort);
	SetEdwinTextL(EPwDlgHome, &iSettings.iHome);
	((CEikChoiceList*)Control(EPwDlgPictures))->SetCurrentItem(iSettings.iImages ? 1 : 0);
	((CEikChoiceList*)Control(EPwDlgText))->SetCurrentItem((iSettings.iDisplay & KPwDisplayScaledText) ? 1 : 0);
	}

TBool CPwPrefsDialog::OkToExitL(TInt aButtonId)
	{
	if (aButtonId == EPwBidGreys)
		{
		// the greys screen shared with PsiTerm and PsiMail, on top of this
		// dialog; the engine reads PsiGrey.ini at each page load
		PsiGrey grey;
		TInt r = PsiGreyScreenL(grey);
		if (r > 0)
			iEikonEnv->InfoMsg(_L("Greys saved - used from the next page"));
		else if (r < 0)
			iEikonEnv->InfoMsg(_L("Not saved - the internal disk is full or in use"));
		return EFalse;                        // (the dialog stays open)
		}
	TInt proxy = ((CEikChoiceList*)Control(EPwDlgProxy))->CurrentItem() == 1;
	TBuf<60> host;
	GetEdwinText(host, EPwDlgProxyHost);
	host.Trim();
	if (proxy && host.Length() == 0)
		{
		iEikonEnv->InfoMsg(_L("No proxy host entered"));
		TryChangeFocusToL(EPwDlgProxyHost);
		return EFalse;
		}
	iSettings.iUseProxy = proxy;
	iSettings.iProxyHost = host;
	iSettings.iProxyPort = NumberEditorValue(EPwDlgProxyPort);
	GetEdwinText(iSettings.iHome, EPwDlgHome);
	iSettings.iHome.Trim();
	iSettings.iImages = ((CEikChoiceList*)Control(EPwDlgPictures))->CurrentItem() == 1;
	if (((CEikChoiceList*)Control(EPwDlgText))->CurrentItem() == 1)
		iSettings.iDisplay |= KPwDisplayScaledText;
	else
		iSettings.iDisplay &= ~KPwDisplayScaledText;
	return ETrue;
	}

// ===========================================================================
// App UI
// ===========================================================================

void CPwAppUi::ConstructL()
	{
	BaseConstructL();
	TPwSettings settings;
	LoadSettings(settings);
	TRAPD(pics, ToolbarPicturesL());
	(void)pics;                              // (no PsiWeb.mbm: words only)
	// the Pictures button stays pressed in while the page's pictures show
	CEikButtonBase* picsButton = iToolBar ? (CEikButtonBase*)iToolBar->ControlById(EPwCmdImages) : NULL;
	if (picsButton)
		picsButton->SetBehavior(EEikButtonLatches);
	ShowPicturesState(settings.iImages);
	if (iToolBar && !settings.iToolbar)
		iToolBar->MakeVisible(EFalse);       // remembered from last time
	iView = new(ELeave) CPwView;
	iView->ConstructL(PageRect(settings.iToolbar), settings);
	AddToStackL(iView);
	iCoeEnv->RootWin().EnableOnEvents(EEventControlAlways);   // (0.54) switch-on events even when in the background
	// the user's contrast and backlight back if a program stopped in
	// Reading mode without doing it (docs/display.md)
	TPsiReading::Recover(iCoeEnv->FsSession(), iCoeEnv->WsSession(), KUidPsiWeb);
	iForeground = ETrue;
	UpdateReading();
	}

// ----- Reading mode (docs/display.md) ----------------------------------------------

void CPwAppUi::UpdateReading(TBool aEnterOnly)
	{
	if (!iView)
		return;
	if (iForeground && (iView->Settings().iDisplay & KPwDisplayReading))
		iReading.Enter(iCoeEnv->FsSession(), KUidPsiWeb);
	else if (!aEnterOnly)
		iReading.Leave(iCoeEnv->FsSession(), KUidPsiWeb);
	}

// Reading mode follows PsiWeb in and out of the foreground (CCoeAppUi's
// HandleForegroundEventL is private on ER5: the focus events are seen here);
// after a switch-on turned it off, the next key turns it on again
void CPwAppUi::HandleWsEventL(const TWsEvent& aEvent, CCoeControl* aDestination)
	{
	if (aEvent.Type() == EEventFocusGained || aEvent.Type() == EEventFocusLost)
		{
		iForeground = aEvent.Type() == EEventFocusGained;
		UpdateReading();
		}
	else if (aEvent.Type() == EEventKey && iForeground && !iReading.Active())
		UpdateReading(ETrue);
	CEikAppUi::HandleWsEventL(aEvent, aDestination);
	}

// The Psion was switched back on (0.54): the engine re-checks the link
// (PPP up? modem carrier?) rather than wait on a dead connection.
void CPwAppUi::HandleSwitchOnEventL(CCoeControl* aDestination)
	{
	(void)aDestination;                  // (the CONE default does nothing, and is private)
	if (iView && iView->Shared())
		{
		iView->Shared()->net.switch_on++;
		iView->RingEngine();             // (0.81) it may be waiting on its doorbell
		}
	iReading.Leave(iCoeEnv->FsSession(), KUidPsiWeb);   // (back on with the next key)
	}

// a toolbar button's picture, from PsiWeb.mbm (made by web/tools/mkicons.py):
// 24x20, in the middle of its side, the words beside it as the built-in
// programs' buttons have them
void CPwAppUi::ButtonPictureL(TInt aId, TInt aIcon)
	{
	CEikCommandButton* b = iToolBar ? (CEikCommandButton*)iToolBar->ControlById(aId) : NULL;
	if (!b)
		return;
	TFileName mbm = Application()->BitmapStoreName();
	CFbsBitmap* bmp = iEikonEnv->CreateBitmapL(mbm, aIcon);
	CleanupStack::PushL(bmp);
	CFbsBitmap* mask = iEikonEnv->CreateBitmapL(mbm, aIcon + 1);
	CleanupStack::PushL(mask);
	b->SetPictureL(bmp, mask);                // (the button owns them now)
	CleanupStack::Pop(2);
	if (b->Picture())
		b->Picture()->SetAlignment(EHCenterVCenter);
	if (b->Label())
		b->Label()->SetAlignment(EHLeftVCenter);
	b->LayoutComponentsL();
	}

// the Pictures button pressed in (the page's pictures show) or not
void CPwAppUi::ShowPicturesState(TBool aOn)
	{
	CEikButtonBase* b = iToolBar ? (CEikButtonBase*)iToolBar->ControlById(EPwCmdImages) : NULL;
	if (!b)
		return;
	CEikButtonBase::TState st = aOn ? CEikButtonBase::ESet : CEikButtonBase::EClear;
	if (b->State() == st)
		return;
	b->SetState(st);
	if (iToolBar->IsVisible())
		b->DrawNow();
	}

void CPwAppUi::ToolbarPicturesL()
	{
	if (!iToolBar)
		return;
	ButtonPictureL(EPwCmdOpen, EMbmToolOpen);
	ButtonPictureL(EPwCmdBack, EMbmToolBack);
	ButtonPictureL(EPwCmdHome, EMbmToolHome);
	ButtonPictureL(EPwCmdImages, EMbmToolPictures);
	}

// the page's room: ClientRect() keeps the toolbar's width back even when
// the toolbar is hidden, so give it to the page here
TRect CPwAppUi::PageRect(TBool aToolbar) const
	{
	TRect r = ClientRect();
	if (!aToolbar || !iToolBar)
		r.iBr.iX = iEikonEnv->ScreenDevice()->SizeInPixels().iWidth;
	return r;
	}

// View > Show toolbar (Ctrl+T): the page takes its room, or gives it back.
// The engine draws at the width it started with, so it starts again on the
// same page (a few seconds: say so, bottom left).
void CPwAppUi::ShowToolBarL(TBool aShow)
	{
	if (!iToolBar)
		return;
	iToolBar->MakeVisible(aShow);
	if (aShow)
		{
		iToolBar->DrawNow();             // whole, before the engine's stop holds the screen up
		iCoeEnv->WsSession().Flush();
		}
	iView->SetPageRectL(PageRect(aShow));
	}

CPwAppUi::~CPwAppUi()
	{
	iReading.Leave(iCoeEnv->FsSession(), KUidPsiWeb);   // the user's contrast and backlight back
	if (iView)
		{
		RemoveFromStack(iView);
		delete iView;
		}
	}

void CPwAppUi::LoadSettings(TPwSettings& aSettings)
	{
	aSettings.iBaudIndex = 4;          // 115200, as PsiTerm recommends
	aSettings.iRtsCts = 0;
	aSettings.iNetMode = 0;
	aSettings.iPppStart.Copy(_L("ATDT777"));
	aSettings.iUseProxy = 0;
	aSettings.iProxyHost.Zero();
	aSettings.iProxyPort = 8080;
	aSettings.iHome = KDefaultHome;
	aSettings.iImages = 0;            // pictures only when asked (View > Show pictures)
	aSettings.iZoom = 100;
	aSettings.iToolbar = 1;
	aSettings.iDisplay = 0;           // sharp text, Reading mode off (docs/display.md)
	RFile file;
	if (file.Open(iCoeEnv->FsSession(), KIniFile, EFileRead) != KErrNone)
		{
		UseSharedLink(iCoeEnv->FsSession(), aSettings);
		return;
		}
	TBuf8<400> d;
	if (file.Read(d) == KErrNone && d.Length() >= 10 && d[0] == 1)
		{
		TInt pos = 1;
		aSettings.iBaudIndex = d[pos++] % 5;
		aSettings.iRtsCts = d[pos++] != 0;
		aSettings.iNetMode = d[pos++] != 0;
		aSettings.iUseProxy = d[pos++] != 0;
		aSettings.iProxyPort = d[pos] | (d[pos + 1] << 8); pos += 2;
		pos++;               // (NetSurf's pictures setting, up to 0.61: see below)
		aSettings.iZoom = d[pos++];
		if (aSettings.iZoom < 30) aSettings.iZoom = 100;
		TInt len = d[pos++];
		if (pos + len <= d.Length() && len <= 60)
			{
			aSettings.iProxyHost.Copy(d.Mid(pos, len));
			pos += len;
			if (pos < d.Length())
				{
				len = d[pos++];
				if (pos + len <= d.Length() && len <= 200)
					{
					aSettings.iHome.Copy(d.Mid(pos, len));
					pos += len;
					if (pos < d.Length())            // (0.55) after the home page
						aSettings.iToolbar = d[pos++] != 0;
					// (0.62) pictures on every page: a new byte, so that
					// NetSurf's setting (on by default) does not carry over
					if (pos < d.Length())
						aSettings.iImages = d[pos++] != 0;
					if (pos < d.Length())            // (0.81) display: Reading mode, text
						aSettings.iDisplay = d[pos++] & (KPwDisplayReading | KPwDisplayScaledText);
					}
				}
			}
		}
	file.Close();
	UseSharedLink(iCoeEnv->FsSession(), aSettings);
	}

void CPwAppUi::SaveSettings(const TPwSettings& aSettings)
	{
	RFs& fs = iCoeEnv->FsSession();
	SaveSharedLink(fs, aSettings);
	fs.MkDirAll(KIniFile);
	// (0.81) to a temporary file, then renamed over the old one, so a full
	// disk or a switch-off part way never leaves half a settings file
	TFileName tmpName(KIniFile);
	tmpName.Append('~');
	RFile file;
	if (file.Replace(fs, tmpName, EFileWrite) != KErrNone)
		return;
	TBuf8<400> d;
	d.Append(1);                         // version
	d.Append((TUint8)aSettings.iBaudIndex);
	d.Append((TUint8)aSettings.iRtsCts);
	d.Append((TUint8)aSettings.iNetMode);
	d.Append((TUint8)aSettings.iUseProxy);
	d.Append((TUint8)(aSettings.iProxyPort & 0xff));
	d.Append((TUint8)(aSettings.iProxyPort >> 8));
	d.Append((TUint8)aSettings.iImages);
	d.Append((TUint8)aSettings.iZoom);
	TBuf8<200> tmp;
	tmp.Copy(aSettings.iProxyHost);
	d.Append((TUint8)tmp.Length());
	d.Append(tmp);
	tmp.Copy(aSettings.iHome);
	d.Append((TUint8)tmp.Length());
	d.Append(tmp);
	d.Append((TUint8)aSettings.iToolbar);
	d.Append((TUint8)aSettings.iImages);    // (0.62)
	d.Append((TUint8)aSettings.iDisplay);   // (0.81)
	TInt r = file.Write(d);
	if (r == KErrNone)
		r = file.Flush();
	file.Close();
	if (r == KErrNone)
		r = fs.Replace(tmpName, KIniFile);
	if (r != KErrNone)
		fs.Delete(tmpName);
	}

void CPwAppUi::DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	PwShared* s = iView->Shared();
	if (aMenuId == R_PW_GO_MENU)
		{
		aMenuPane->SetItemDimmed(EPwCmdBack, !s->can_back);
		aMenuPane->SetItemDimmed(EPwCmdForward, !s->can_forward);
		}
	else if (aMenuId == R_PW_FILE_MENU)
		{
		aMenuPane->SetItemDimmed(EPwCmdStop, !s->busy);
		aMenuPane->SetItemDimmed(EPwCmdConnect, s->busy);   // (a page is using the link)
		}
	else if (aMenuId == R_PW_VIEW_MENU)
		{
		aMenuPane->SetItemButtonState(EPwCmdToggleToolbar,
			iView->Settings().iToolbar ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemButtonState(EPwCmdReading,
			(iView->Settings().iDisplay & KPwDisplayReading) ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemButtonState(EPwCmdImages, iView->PicturesShown() ? EEikMenuItemSymbolOn : 0);
		}
	}

// cuts aText to aWidth pixels of aFont, ending in "..." (one character on
// the Psion), as the style guide has long names shown
static void Ellipsis(TDes& aText, const CFont& aFont, TInt aWidth)
	{
	if (aFont.TextWidthInPixels(aText) <= aWidth)
		return;
	const TText KEllipsis = 0x85;
	TInt n = aText.Length();
	while (n > 0)
		{
		n--;
		if (aFont.TextWidthInPixels(aText.Left(n)) + aFont.CharWidthInPixels(KEllipsis) <= aWidth)
			break;
		}
	aText.SetLength(n);
	aText.Append(KEllipsis);
	}

// View > Page information: the title, the address, what the engine says,
// memory, the route. Each line is cut to fit beside its prompt.
void CPwAppUi::PageInfoL()
	{
	PwShared* s = iView->Shared();
	TBuf<160> lines[5];
	FromUtf8(lines[0], s->title);
	lines[0].Trim();
	if (lines[0].Length() == 0)
		lines[0] = _L("(no title)");
	FromUtf8(lines[1], s->url);
	if (lines[1].Compare(_L("about:blank")) == 0)
		lines[1] = _L("(no page open)");
	FromUtf8(lines[2], s->status);
	lines[2].Trim();
	if (lines[2].Length() == 0)
		lines[2] = _L("-");
	TMemoryInfoV1Buf mem;
	UserHal::MemoryInfo(mem);
	lines[3].Format(_L("%d KB"), mem().iFreeRamInBytes / 1024);
	if (iView->Settings().iUseProxy)
		{
		lines[4] = _L("Via proxy ");
		lines[4].Append(Clip(iView->Settings().iProxyHost, 60));
		lines[4].AppendFormat(_L(":%d"), iView->Settings().iProxyPort);
		}
	else
		lines[4] = _L("Direct");
	// the prompts take about 120 px of the 640, the frame and margins more:
	// the dialog's labels are bold, which the normal font's widths allow for
	const CFont* font = iEikonEnv->NormalFont();
	TPtrC ptrs[5];
	for (TInt i = 0; i < 5; i++)
		{
		Ellipsis(lines[i], *font, 420);
		ptrs[i].Set(lines[i]);
		}
	CPwInfoDialog* dlg = new(ELeave) CPwInfoDialog(ptrs, 5);
	dlg->ExecuteLD(R_PW_INFO_DIALOG);
	}

void CPwAppUi::AboutL()
	{
	const TPwSettings& s = iView->Settings();
	static const TInt KBaud[] = { 9600, 19200, 38400, 57600, 115200 };
	TBuf<120> status;
	if (s.iNetMode)
		status.Copy(_L("Connects via Psion Internet (PPP)"));
	else
		status.Format(_L("Connects via modem at %d baud"), KBaud[s.iBaudIndex >= 0 && s.iBaudIndex < 5 ? s.iBaudIndex : 4]);
	if (s.iUseProxy)
		{
		status.Append(_L(" - proxy "));
		status.Append(Clip(s.iProxyHost, 40));
		status.AppendFormat(_L(":%d"), s.iProxyPort);
		}
	else
		status.Append(_L(" - no proxy"));
	CPwAboutDialog* dlg = new(ELeave) CPwAboutDialog(status);
	dlg->ExecuteLD(R_PW_ABOUT_DIALOG);
	}

_LIT(KUpdIniFile, "C:\\System\\Apps\\PsiWeb\\Update.ini");

// Tools > Update PsiWeb: where from (Update.ini, in PsiMail's words:
// "github", "github-dev" or "host:port" - the engine reads it), then the
// engine fetches and checks the new version
void CPwAppUi::UpdateL()
	{
	if (iView->Updating())
		{
		iEikonEnv->InfoMsg(_L("Not available while updating"));
		return;
		}
	RFs& fs = iCoeEnv->FsSession();
	TBuf<60> src;
	RFile f;
	if (f.Open(fs, KUpdIniFile, EFileRead) == KErrNone)
		{
		TBuf8<60> b;
		f.Read(b);
		f.Close();
		src.Copy(b);
		src.Trim();
		}
	TInt source = 0, port = 8686;
	TBuf<50> host;
	if (src.CompareF(_L("github-dev")) == 0)
		source = 1;
	else if (src.Length() && src.CompareF(_L("github")) != 0)
		{
		source = 2;
		TInt c = src.Locate(':');
		host.Copy(src.Left(c >= 0 ? (c < 50 ? c : 50) : (src.Length() < 50 ? src.Length() : 50)));
		if (c >= 0)
			{
			TLex lex(src.Mid(c + 1));
			if (lex.Val(port) != KErrNone || port <= 0) port = 8686;
			}
		}
	CPwUpdateDialog* dlg = new(ELeave) CPwUpdateDialog(source, host, port);
	if (!dlg->ExecuteLD(R_PW_UPDATE_DIALOG))
		return;
	if (source == 0) src = _L("github");
	else if (source == 1) src = _L("github-dev");
	else
		{
		src = host;
		src.AppendFormat(_L(":%d"), port);
		}
	fs.MkDirAll(KUpdIniFile);
	if (f.Replace(fs, KUpdIniFile, EFileWrite) == KErrNone)
		{
		TBuf8<60> b;
		b.Copy(src);
		f.Write(b);
		f.Close();
		}
	iView->StartUpdateL();
	}

void CPwAppUi::HandleCommandL(TInt aCommand)
	{
	TPwSettings& st = iView->Settings();
	PwShared* sh = iView->Shared();
	// the Zoom in and Zoom out icons beside the screen come as EIKON's own
	// zoom commands (or as their keys: CPwView::OfferKeyEventL)
	if (aCommand == EEikCmdZoomIn) aCommand = EPwCmdZoomIn;
	else if (aCommand == EEikCmdZoomOut) aCommand = EPwCmdZoomOut;
	else if (aCommand == EEikCmdZoomNormal) aCommand = EPwCmdZoomNormal;
	switch (aCommand)
		{
	case EEikCmdExit:
		iView->StopEngine();
		Exit();
		break;
	case EPwCmdOpen:
		{
		TBuf<500> url;
		FromUtf8(url, sh->url);
		if (url.Compare(_L("about:blank")) == 0)
			url.Zero();
		CPwOpenDialog* dlg = new(ELeave) CPwOpenDialog(url);
		if (dlg->ExecuteLD(R_PW_OPEN_DIALOG) && url.Length() > 0)
			iView->Command(PW_CMD_OPEN, url);
		break;
		}
	case EPwCmdHome:
		iView->Command(PW_CMD_HOME, KNullDesC);
		break;
	case EPwCmdReload:
		iView->Command(PW_CMD_RELOAD, KNullDesC);
		break;
	case EPwCmdStop:
		// dimmed on the menu when nothing is loading, but the key still arrives
		if (iView->EngineRunning() && !sh->busy)
			iEikonEnv->InfoMsg(_L("Nothing to stop"));
		else
			iView->Command(PW_CMD_STOP, KNullDesC);
		break;
	case EPwCmdBack:
		// the toolbar button and the key arrive whatever the history holds
		if (iView->EngineRunning() && !sh->can_back)
			iEikonEnv->InfoMsg(_L("Nothing to go back to"));
		else
			iView->Command(PW_CMD_BACK, KNullDesC);
		break;
	case EPwCmdForward:
		if (iView->EngineRunning() && !sh->can_forward)
			iEikonEnv->InfoMsg(_L("Nothing to go forward to"));
		else
			iView->Command(PW_CMD_FORWARD, KNullDesC);
		break;
	case EPwCmdTop:
		iView->Command(PW_CMD_TOP, KNullDesC);
		break;
	case EPwCmdBottom:
		iView->Command(PW_CMD_BOTTOM, KNullDesC);
		break;
	case EPwCmdZoomIn:
	case EPwCmdZoomOut:
	case EPwCmdZoomNormal:
		{
		// Zoom in and out cycle round the sizes (style guide 7.3.7): past the
		// largest comes the smallest, and the other way
		TInt i = 0;
		while (i < KZoomCount - 1 && KZoomSteps[i] < st.iZoom)
			i++;
		if (aCommand == EPwCmdZoomIn) i = (i + 1) % KZoomCount;
		if (aCommand == EPwCmdZoomOut) i = (i + KZoomCount - 1) % KZoomCount;
		st.iZoom = aCommand == EPwCmdZoomNormal ? 100 : KZoomSteps[i];
		TBuf<8> z;
		z.Num(st.iZoom);
		iView->Command(PW_CMD_ZOOM, z);
		TBuf<32> msg;
		msg.Format(_L("Zoom %d%%"), st.iZoom);
		iEikonEnv->InfoMsg(msg);
		SaveSettings(st);
		break;
		}
	case EPwCmdImages:
		{
		// View > Show pictures (a tick box) and the Pictures button: the
		// pictures of the page showing, on or off (pictures on every page is
		// in Preferences). The button shows what the page has come to.
		TBool on = iView->PicturesShown();
		if (!iView->EngineRunning())
			iView->Command(PW_CMD_IMAGES, _L("1"));   // (says why not)
		else if (st.iImages)
			iEikonEnv->InfoMsg(_L("Pictures are already shown on every page"));
		else if (sh->page_pics)
			{
			iView->Command(PW_CMD_IMAGES, _L("0"));
			on = EFalse;
			}
		else
			{
			TBuf<16> u;
			FromUtf8(u, sh->url);
			if (u.Length() == 0 || u.Left(6).Compare(_L("about:")) == 0)
				iEikonEnv->InfoMsg(_L("No pictures to show"));
			else
				{
				iView->Command(PW_CMD_IMAGES, _L("1"));
				on = ETrue;
				}
			}
		ShowPicturesState(on);
		break;
		}
	case EPwCmdToggleToolbar:
		st.iToolbar = !st.iToolbar;
		SaveSettings(st);
		ShowToolBarL(st.iToolbar);
		break;
	case EPwCmdReading:                      // View > Reading mode (docs/display.md)
		st.iDisplay ^= KPwDisplayReading;
		SaveSettings(st);
		UpdateReading();
		iEikonEnv->InfoMsg((st.iDisplay & KPwDisplayReading) ? _L("Reading mode on") : _L("Reading mode off"));
		break;
	case EPwCmdPageInfo:
		PageInfoL();
		break;
	case EPwCmdConnSettings:
		{
		CPwConnDialog* dlg = new(ELeave) CPwConnDialog(st);
		if (dlg->ExecuteLD(R_PW_CONN_DIALOG))
			{
			SaveSettings(st);
			RestartEngineL();
			}
		break;
		}
	case EPwCmdPrefs:
		{
		CPwPrefsDialog* dlg = new(ELeave) CPwPrefsDialog(st);
		if (dlg->ExecuteLD(R_PW_PREFS_DIALOG))
			{
			SaveSettings(st);
			ShowPicturesState(st.iImages);   // (pictures on every page, or not)
			RestartEngineL();
			}
		break;
		}
	case EPwCmdConnect:
		{
		// as PsiTerm's File > Connect: brings the link up now, before a page
		// needs it - the Psion's Internet dialled, or the modem checked - with
		// the shared connection settings, and says what it found
		TPsiLink link;
		link.SetDefaults();
		link.Load(iCoeEnv->FsSession());
		PgLinkConnectL(link.iBaudIndex, link.iRtsCts, link.iNetMode, link.iPppStart,
			R_PW_TEST_DIALOG, EPwDlgTest1, _L("Connect"));
		break;
		}
	case EPwCmdHangup:
		iView->Command(PW_CMD_HANGUP, KNullDesC);
		if (iView->EngineRunning())
			iEikonEnv->InfoMsg(_L("Disconnecting - the serial port will be free"));
		break;
	case EPwCmdUpdate:
		UpdateL();
		break;
	case EPwCmdRestart:
		RestartEngineL();
		break;
	case EPwCmdAbout:
		AboutL();
		break;
	case EPwCmdHelp:
		HelpL();
		break;
	default:
		break;
		}
	}

// Tools > Restart browser engine, and after settings the engine reads when
// it starts: it takes a few seconds, so say so, bottom left
void CPwAppUi::RestartEngineL()
	{
	iView->RestartL(ETrue);              // (it shows "Restarting..." until the engine draws)
	}

// PsiMail starts PsiWeb with a URL as the command line's tail ...
TBool CPwAppUi::ProcessCommandParametersL(TApaCommand /*aCommand*/, TFileName& aDocumentName, const TDesC8& aTail)
	{
	if (aTail.Length() > 0)
		{
		TBuf<PW_URL_MAX> url;
		url.Copy(Clip(aTail, url.MaxLength()));
		url.Trim();
		if (url.Length() > 0 && iView)
			iView->OpenUrlL(url);
		}
	aDocumentName.Zero();          // PsiWeb has no document file
	return EFalse;
	}

// ... or, when PsiWeb is already running, sends it the URL as a message
void CPwAppUi::ProcessMessageL(TUid aUid, const TDesC8& aParams)
	{
	if (aUid != KUidPsiWeb)
		{
		CEikAppUi::ProcessMessageL(aUid, aParams);
		return;
		}
	TBuf<PW_URL_MAX> url;
	url.Copy(Clip(aParams, url.MaxLength()));
	url.Trim();
	if (url.Length() > 0 && iView)
		iView->OpenUrlL(url);
	}

// ===========================================================================
// Document / Application / entry points
// ===========================================================================

CPwDocument::CPwDocument(CEikApplication& aApp)
	: CEikDocument(aApp)
	{
	}

CEikAppUi* CPwDocument::CreateAppUiL()
	{
	return new(ELeave) CPwAppUi;
	}

TUid CPwApplication::AppDllUid() const
	{
	return KUidPsiWeb;
	}

CApaDocument* CPwApplication::CreateDocumentL()
	{
	return new(ELeave) CPwDocument(*this);
	}

EXPORT_C CApaApplication* NewApplication()
	{
	return new CPwApplication;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
