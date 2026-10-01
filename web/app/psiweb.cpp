// PSIWEB.CPP - PsiWeb.app: screen, keyboard, pen, menus and settings for the
// NetSurf engine (psiweb.exe). See psiweb.h.

#include <e32keys.h>
#include <e32hal.h>
#include <eikchlst.h>
#include <eikedwin.h>
#include <eiklabel.h>
#include <eikmfne.h>
#include <eikcmbut.h>
#include <eiktbar.h>
#include <eikimage.h>
#include <apgcli.h>
#include "pwapp.h"
#include "pwicons.h"
#include "psilink.h"

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
_LIT(KVersion, "0.55");           // also web/pkg/psiweb.pkg and dist/PsiWeb-version.txt (two digits from 0.54: see the .pkg)
_LIT(KDefaultHome, "http://68k.news/");
const TInt KZoomSteps[] = { 50, 60, 70, 80, 90, 100, 110, 125, 150, 175, 200 };
const TInt KZoomCount = 11;
const TInt KTick = 62500;            // look for new frames 16 times a second

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
	delete iStarter;
	delete iTimer;
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

	TSize size = aRect.Size();
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
	}

TInt CPwView::StartCallback(TAny* aSelf)
	{
	CPwView* v = (CPwView*)aSelf;
	TRAPD(r, v->StartEngineL());
	(void)r;
	return 0;
	}

// a page to open: now if NetSurf is running, else as its first page
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
	Mem::FillZ(s, sizeof(PwShared) - sizeof(s->fb));
	s->width = w;
	s->height = h;
	s->magic = PW_MAGIC;
	s->net.magic = PSI_SHARED_MAGIC;
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
	// Loading the engine (2.6 MB) and its start-up take many seconds, during
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
	// give NetSurf a few seconds to hang up and save cookies
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
	TSize size = aRect.Size();
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

void CPwView::EngineEnded()
	{
	TExitType type = iProcess.ExitType();
	TInt reason = iProcess.ExitReason();
	TExitCategoryName cat = iProcess.ExitCategory();
	iProcess.Close();
	iRunning = EFalse;
	StartBusyCancel();
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
		if (m.Length() && m[m.Length() - 1] == '.' && m.Right(3).Compare(_L("...")) != 0)
			m.SetLength(m.Length() - 1);     // no full stop at the end of a message
		if (m.Length() && m.Right(3).Compare(_L("...")) == 0)
			{
			TRAPD(err, iEikonEnv->BusyMsgL(m, EHLeftVBottom, TTimeIntervalMicroSeconds32(0)));
			iLinkBusy = err == KErrNone;
			}
		else if (m.Length())
			iEikonEnv->InfoMsg(m);
		}
	if (s && iLinkBusy && !s->busy)
		{
		iEikonEnv->BusyMsgCancel();
		iLinkBusy = EFalse;
		}
	if (s && (iUpdState == PW_UPD_RUNNING || s->update_state != iUpdState))
		{
		TRAPD(err, UpdateTickL());
		(void)err;
		}
	if (!s || s->frame_seq == iLastFrame || iUpdState == PW_UPD_RUNNING)
		return;
	iLastFrame = s->frame_seq;
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
	iUpdState = -1;                      // show the progress screen at once
	Command(PW_CMD_UPDATE, KNullDesC);
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
			ShowMessage(_L("Updating PsiWeb  (Esc to stop)"), msg);
			}
		return;
		}
	TInt was = iUpdState;
	iUpdState = st;
	if (was != PW_UPD_RUNNING && was != -1)
		return;
	iShowMsg = EFalse;
	DrawNow();                           // the page again
	if (st == PW_UPD_READY)
		{
		TBuf<64> q;
		TBuf<16> v;
		FromUtf8(v, s->update_version);
		q.Format(_L("Install PsiWeb %S now?"), &v);
		if (iEikonEnv->QueryWinL(_L("PsiWeb will close while it installs"), q))
			StartInstallerL();
		else
			iEikonEnv->InfoWinL(_L("Update saved"), iUpdateFile);
		}
	else if (st == PW_UPD_CURRENT)
		iEikonEnv->InfoMsg(msg);
	else if (st == PW_UPD_FAILED)
		iEikonEnv->InfoWinL(_L("Update PsiWeb"), msg);
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
		gc.DrawRect(Rect());
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
	gc.BitBlt(aRect.iTl, iBitmap, aRect);
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
	AddEntropy(code);
	if (mods & EModifierCtrl)
		{
		// leave the menu's shortcuts to the menu (see psiweb.rss);
		// Ctrl+letter arrives as 1..26
		TUint letter = (code >= 1 && code <= 26) ? 'a' + code - 1 : (code | 0x20);
		if (mods & EModifierShift)
			{
			// Shift+Ctrl+M zoom out, Q page information, H help, A about
			switch (letter)
				{
				case 'm': case 'q': case 'h': case 'a':
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
		iShared->net.quit = 1;           // the updater gives up and says so
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

TBool CPwConnDialog::OkToExitL(TInt /*aButtonId*/)
	{
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
	}

TBool CPwPrefsDialog::OkToExitL(TInt /*aButtonId*/)
	{
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
	if (iToolBar && !settings.iToolbar)
		iToolBar->MakeVisible(EFalse);       // remembered from last time
	iView = new(ELeave) CPwView;
	iView->ConstructL(PageRect(settings.iToolbar), settings);
	AddToStackL(iView);
	iCoeEnv->RootWin().EnableOnEvents(EEventControlAlways);   // (0.54) switch-on events even when in the background
	}

// The Psion was switched back on (0.54): the engine re-checks the link
// (PPP up? modem carrier?) rather than wait on a dead connection.
void CPwAppUi::HandleSwitchOnEventL(CCoeControl* aDestination)
	{
	(void)aDestination;                  // (the CONE default does nothing, and is private)
	if (iView && iView->Shared())
		iView->Shared()->net.switch_on++;
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

void CPwAppUi::ToolbarPicturesL()
	{
	if (!iToolBar)
		return;
	ButtonPictureL(EPwCmdOpen, EMbmToolOpen);
	ButtonPictureL(EPwCmdBack, EMbmToolBack);
	ButtonPictureL(EPwCmdHome, EMbmToolHome);
	ButtonPictureL(EPwCmdZoomIn, EMbmToolZoom);
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
	aSettings.iImages = 1;
	aSettings.iZoom = 100;
	aSettings.iToolbar = 1;
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
		aSettings.iImages = d[pos++] != 0;
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
	RFile file;
	if (file.Replace(fs, KIniFile, EFileWrite) != KErrNone)
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
	file.Write(d);
	file.Close();
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
		}
	else if (aMenuId == R_PW_VIEW_MENU)
		{
		aMenuPane->SetItemButtonState(EPwCmdImages,
			iView->Settings().iImages ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemButtonState(EPwCmdToggleToolbar,
			iView->Settings().iToolbar ? EEikMenuItemSymbolOn : 0);
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
		st.iImages = !st.iImages;
		iView->Command(PW_CMD_IMAGES, st.iImages ? _L("1") : _L("0"));
		iEikonEnv->InfoMsg(st.iImages ? _L("Pictures shown from the next page on") : _L("Pictures not shown from the next page on"));
		SaveSettings(st);
		break;
	case EPwCmdToggleToolbar:
		st.iToolbar = !st.iToolbar;
		SaveSettings(st);
		ShowToolBarL(st.iToolbar);
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
			RestartEngineL();
			}
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
