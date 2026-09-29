// PSIWEB.CPP - PsiWeb.app: screen, keyboard, pen, menus and settings for the
// NetSurf engine (psiweb.exe). See psiweb.h.

#include <e32keys.h>
#include <e32hal.h>
#include <eikchlst.h>
#include <eikedwin.h>
#include <eiklabel.h>
#include <eikmfne.h>
#include <apgcli.h>
#include "pwapp.h"

_LIT(KEngineExe, "psiweb.exe");
_LIT(KIniFile, "C:\\System\\Apps\\PsiWeb\\PsiWeb.ini");
_LIT(KVersion, "0.3");          // also web/pkg/psiweb.pkg and dist/PsiWeb-version.txt
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
	StopEngine();
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
	StartEngineL();
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
	s->net.baud_index = iSettings.iBaudIndex;
	s->net.rtscts = iSettings.iRtsCts;
	s->net.net_mode = iSettings.iNetMode;
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

	// the engine and its resources (Messages, CSS) live next to the app
	TParse parse;
	parse.Set(CEikonEnv::Static()->EikAppUi()->Application()->AppFullName(), NULL, NULL);
	TFileName dir(parse.DriveAndPath());
	CopyToC(s->res_dir, sizeof(s->res_dir), dir);
	TFileName exe(dir);
	exe.Append(KEngineExe);

	iLastFrame = 0;
	iMsg1 = _L("Starting NetSurf...");
	iMsg2.Zero();
	iShowMsg = ETrue;
	DrawNow();

	TInt r = iProcess.Create(exe, KNullDesC);
	if (r != KErrNone)
		{
		TBuf<80> e;
		e.Format(_L("Could not start psiweb.exe (error %d)"), r);
		ShowMessage(_L("PsiWeb"), e);
		return;
		}
	iRunning = ETrue;
	if (!iWatcher)
		iWatcher = new(ELeave) CPwWatcher(*this);
	iWatcher->Watch(iProcess);
	iProcess.Resume();
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
		iProcess.Kill(0);
	iProcess.Close();
	iRunning = EFalse;
	}

void CPwView::EngineEnded()
	{
	TExitType type = iProcess.ExitType();
	TInt reason = iProcess.ExitReason();
	TExitCategoryName cat = iProcess.ExitCategory();
	iProcess.Close();
	iRunning = EFalse;
	if (iShared->quitting)
		return;
	TBuf<120> why;
	if (type == EExitPanic)
		why.Format(_L("It stopped: %S %d. Tools > Restart to try again."), &cat, reason);
	else if (iShared->exit_msg[0])
		{
		FromUtf8(why, iShared->exit_msg);
		}
	else
		why.Format(_L("It closed (%d). Tools > Restart to start it again."), reason);
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
		iEikonEnv->InfoMsg(_L("The browser engine is not running"));
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
		iEikonEnv->InfoMsg(_L("The browser engine is not running"));
		return;
		}
	if (iUpdState == PW_UPD_RUNNING)
		return;
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
		if (iEikonEnv->QueryWinL(q, _L("PsiWeb will close while it installs")))
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
	m.Format(_L("Could not start the installer (%d). Open %S from the System screen."), err, &iUpdateFile);
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
		switch (letter)
			{
			case 'e': case 'l': case 'o': case 'h': case 'r': case 'b':
			case 'f': case 'm': case 'i': case 'g':
				return EKeyWasNotConsumed;
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
	SetTitleL(iTitle);
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
	return ETrue;
	}

void CPwConnDialog::PreLayoutDynInitL()
	{
	((CEikChoiceList*)Control(EPwDlgLink))->SetCurrentItem(iSettings.iNetMode ? 1 : 0);
	((CEikChoiceList*)Control(EPwDlgBaud))->SetCurrentItem(iSettings.iBaudIndex);
	((CEikChoiceList*)Control(EPwDlgFlow))->SetCurrentItem(iSettings.iRtsCts ? 1 : 0);
	}

TBool CPwConnDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iSettings.iNetMode = ((CEikChoiceList*)Control(EPwDlgLink))->CurrentItem() == 1;
	iSettings.iBaudIndex = ((CEikChoiceList*)Control(EPwDlgBaud))->CurrentItem();
	iSettings.iRtsCts = ((CEikChoiceList*)Control(EPwDlgFlow))->CurrentItem() == 1;
	return ETrue;
	}

void CPwBrowserDialog::PreLayoutDynInitL()
	{
	((CEikChoiceList*)Control(EPwDlgProxy))->SetCurrentItem(iSettings.iUseProxy ? 1 : 0);
	SetEdwinTextL(EPwDlgProxyHost, &iSettings.iProxyHost);
	SetNumberEditorValue(EPwDlgProxyPort, iSettings.iProxyPort);
	SetEdwinTextL(EPwDlgHome, &iSettings.iHome);
	}

TBool CPwBrowserDialog::OkToExitL(TInt /*aButtonId*/)
	{
	TInt proxy = ((CEikChoiceList*)Control(EPwDlgProxy))->CurrentItem() == 1;
	TBuf<60> host;
	GetEdwinText(host, EPwDlgProxyHost);
	host.Trim();
	if (proxy && host.Length() == 0)
		{
		CEikonEnv::Static()->InfoMsg(_L("Enter the proxy's name or IP address"));
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
	iView = new(ELeave) CPwView;
	iView->ConstructL(ClientRect(), settings);
	AddToStackL(iView);
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
	aSettings.iUseProxy = 0;
	aSettings.iProxyHost.Zero();
	aSettings.iProxyPort = 8080;
	aSettings.iHome = KDefaultHome;
	aSettings.iImages = 1;
	aSettings.iZoom = 100;
	RFile file;
	if (file.Open(iCoeEnv->FsSession(), KIniFile, EFileRead) != KErrNone)
		return;
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
					aSettings.iHome.Copy(d.Mid(pos, len));
				}
			}
		}
	file.Close();
	}

void CPwAppUi::SaveSettings(const TPwSettings& aSettings)
	{
	RFs& fs = iCoeEnv->FsSession();
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
		}
	}

void CPwAppUi::PageInfoL()
	{
	PwShared* s = iView->Shared();
	TBuf<120> lines[5];
	FromUtf8(lines[0], s->title);
	if (lines[0].Length() == 0)
		lines[0] = _L("(no title)");
	FromUtf8(lines[1], s->url);
	FromUtf8(lines[2], s->status);
	TMemoryInfoV1Buf mem;
	UserHal::MemoryInfo(mem);
	lines[3].Format(_L("Free memory: %d KB"), mem().iFreeRamInBytes / 1024);
	if (iView->Settings().iUseProxy)
		{
		lines[4] = _L("Via proxy ");
		lines[4].Append(iView->Settings().iProxyHost.Left(60));
		}
	else
		lines[4] = _L("Direct connection");
	CPwInfoDialog* dlg = new(ELeave) CPwInfoDialog(_L("Page info"), lines, 5);
	dlg->ExecuteLD(R_PW_INFO_DIALOG);
	}

void CPwAppUi::HandleCommandL(TInt aCommand)
	{
	TPwSettings& st = iView->Settings();
	switch (aCommand)
		{
	case EEikCmdExit:
		iView->StopEngine();
		Exit();
		break;
	case EPwCmdOpen:
		{
		TBuf<500> url;
		FromUtf8(url, iView->Shared()->url);
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
		iView->Command(PW_CMD_STOP, KNullDesC);
		break;
	case EPwCmdBack:
		iView->Command(PW_CMD_BACK, KNullDesC);
		break;
	case EPwCmdForward:
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
		TInt i = 0;
		while (i < KZoomCount - 1 && KZoomSteps[i] < st.iZoom)
			i++;
		if (aCommand == EPwCmdZoomIn && i < KZoomCount - 1) i++;
		if (aCommand == EPwCmdZoomOut && i > 0) i--;
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
		iEikonEnv->InfoMsg(st.iImages ? _L("Images on (from the next page)") : _L("Images off"));
		SaveSettings(st);
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
			iEikonEnv->InfoMsg(_L("Restarting the browser engine..."));
			iView->StopEngine();
			iView->StartEngineL();
			}
		break;
		}
	case EPwCmdBrowserSettings:
		{
		CPwBrowserDialog* dlg = new(ELeave) CPwBrowserDialog(st);
		if (dlg->ExecuteLD(R_PW_BROWSER_DIALOG))
			{
			SaveSettings(st);
			iView->StopEngine();
			iView->StartEngineL();
			}
		break;
		}
	case EPwCmdHangup:
		iView->Command(PW_CMD_HANGUP, KNullDesC);
		iEikonEnv->InfoMsg(_L("Hanging up - the serial port will be free"));
		break;
	case EPwCmdUpdate:
		iView->StartUpdateL();
		break;
	case EPwCmdRestart:
		iView->StopEngine();
		iView->StartEngineL();
		break;
	case EPwCmdAbout:
		{
		TBuf<120> lines[4];
		lines[0] = _L("PsiWeb ");
		lines[0].Append(KVersion);
		lines[0].Append(_L(" - NetSurf for the Psion 5mx"));
		lines[1] = _L("NetSurf: HTML, CSS 2.1, no JavaScript. GPL v2.");
		lines[2] = _L("Networking and TLS 1.3 from PsiTerm (MIT).");
		lines[3] = _L("Esc stops loading. Ctrl+L opens an address.");
		CPwInfoDialog* dlg = new(ELeave) CPwInfoDialog(_L("About PsiWeb"), lines, 4);
		dlg->ExecuteLD(R_PW_INFO_DIALOG);
		break;
		}
	default:
		break;
		}
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
