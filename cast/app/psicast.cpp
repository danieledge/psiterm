// PSICAST.CPP - PsiCast.app: share the Psion's screen with a web browser.
//
// One view (CCsView) says how the sharing is going: the state, the viewers,
// where to look on the phone, whether the viewer may tap and type, and how
// fast it goes. Reading, encoding and sending the screen is psicast.exe's
// work, through the chunk in ../engine/psicast.h. See csapp.h.

#include <e32keys.h>
#include <e32hal.h>
#include <eiklabel.h>
#include <eikchlst.h>
#include <eikmfne.h>
#include <eikcmbut.h>
#include <eiktbar.h>
#include <eikimage.h>
#include <eikon.rsg>
#include "csapp.h"
#include "csicons.h"
#include "psilink.h"
#include "pglinktest.h"

_LIT(KVersion, "0.10");            // also cast/pkg/psicast.pkg
_LIT(KEngineExe, "psicast.exe");
_LIT(KIniFile, "C:\\System\\Apps\\PsiCast\\PsiCast.ini");
_LIT(KAtomHost, "192.168.7.1");    // the Atom's own address (PROTOCOL.md link A)
const TInt KAtomPort = 5961;
const TInt KRates[4] = { 2, 5, 10, 20 };
const TInt KTickFast = 250000;     // while connecting: the state and the busy message
const TInt KTickIdle = 1000000;
const TUint KPauseKeyCode = 'w' - 'a' + 1;     // Shift+Ctrl+W: the key code Ctrl makes of W
const TInt KBigTwips[3] = { 200, 250, 300 };
const TInt KTextTwips[3] = { 150, 200, 250 };
const TInt KSmallTwips[3] = { 120, 150, 200 };

static void CopyToC(char* aDst, TInt aMax, const TDesC& aSrc)
	{
	TInt n = aSrc.Length() < aMax - 1 ? aSrc.Length() : aMax - 1;
	for (TInt i = 0; i < n; i++)
		aDst[i] = (char)aSrc[i];
	aDst[n] = 0;
	}

// ===========================================================================
// The view
// ===========================================================================

CCsView::~CCsView()
	{
	CWsScreenDevice* dev = iCoeEnv->ScreenDevice();
	if (iBig) dev->ReleaseFont(iBig);
	if (iLabel) dev->ReleaseFont(iLabel);
	if (iText) dev->ReleaseFont(iText);
	if (iSmall) dev->ReleaseFont(iSmall);
	}

void CCsView::ConstructL(const TRect& aRect)
	{
	CreateWindowL();
	SetRectL(aRect);
	ApplyZoomL();
	ActivateL();
	}

CFont* CCsView::FontL(TInt aTwips, TBool aBold, CFont*& aOld)
	{
	TFontSpec spec(_L("Arial"), aTwips);
	if (aBold)
		spec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	CFont* font = NULL;
	CWsScreenDevice* dev = iCoeEnv->ScreenDevice();
	User::LeaveIfError(dev->GetNearestFontInTwips(font, spec));
	if (aOld)
		dev->ReleaseFont(aOld);
	aOld = font;
	return font;
	}

void CCsView::ApplyZoomL()
	{
	TInt z = iUi.Settings().iZoom;
	FontL(KBigTwips[z], ETrue, iBig);
	FontL(KTextTwips[z], ETrue, iLabel);
	FontL(KTextTwips[z], EFalse, iText);
	FontL(KSmallTwips[z], EFalse, iSmall);
	}

void CCsView::SetViewRectL(const TRect& aRect)
	{
	SetRectL(aRect);
	DrawNow();
	}

static void DrawClipped(CWindowGc& aGc, const CFont* aFont, const TDesC& aText, TPoint aAt, TInt aWidth)
	{
	if (aWidth <= 0)
		return;
	if (aFont->TextWidthInPixels(aText) <= aWidth)
		{
		aGc.DrawText(aText, aAt);
		return;
		}
	_LIT(KDots, "...");
	TInt n = aFont->TextCount(aText, aWidth - aFont->TextWidthInPixels(KDots));
	TBuf<130> t(aText.Left(n < 120 ? n : 120));
	t.Append(KDots);
	aGc.DrawText(t, aAt);
	}

// a labelled line: the label in bold, what it says beside it
void CCsView::DrawLine(CWindowGc& aGc, TInt aY, const TDesC& aLabel, const TDesC& aText) const
	{
	TRect r = Rect();
	TInt labelW = iLabel->TextWidthInPixels(_L("On the phone")) + 16;
	aGc.UseFont(iLabel);
	aGc.SetPenColor(KRgbBlack);
	DrawClipped(aGc, iLabel, aLabel, TPoint(r.iTl.iX + 14, aY), labelW - 4);
	aGc.UseFont(iText);
	DrawClipped(aGc, iText, aText, TPoint(r.iTl.iX + 14 + labelW, aY), r.Width() - labelW - 24);
	}

void CCsView::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	TRect r = Rect();
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.DrawRect(r);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);

	const CsShared* s = iUi.Shared();
	TBuf<100> t;
	TInt y = r.iTl.iY + 10 + iBig->AscentInPixels();
	iUi.StateText(t);
	gc.UseFont(iBig);
	gc.SetPenColor(KRgbBlack);
	DrawClipped(gc, iBig, t, TPoint(r.iTl.iX + 14, y), r.Width() - 28);
	y += iBig->DescentInPixels() + 4;
	gc.SetPenColor(KRgbDarkGray);
	gc.DrawLine(TPoint(r.iTl.iX + 14, y), TPoint(r.iBr.iX - 14, y));
	TInt pitch = iText->HeightInPixels() + 7;
	y += 6 + iText->AscentInPixels();

	// Viewers
	if (!s || s->viewers < 0)
		t = _L("-");
	else if (s->viewers == 0)
		t = _L("None yet");
	else
		{
		t.Format(s->viewers == 1 ? _L("%d viewer") : _L("%d viewers"), s->viewers);
		if (s->viewers_control > 0)
			t.AppendFormat(_L(", %d may tap and type"), s->viewers_control);
		}
	DrawLine(gc, y, _L("Viewers"), t);
	y += pitch;

	// where to look
	TBuf8<64> url;
	if (s)
		url = FromC(s->viewer_url, sizeof(s->viewer_url) - 1);
	if (url.Length())
		{
		t = _L("Open ");
		TBuf<64> u;
		u.Copy(url);
		t.Append(u);
		}
	else
		t = _L("http://(the Atom's address)/screen");
	DrawLine(gc, y, _L("On the phone"), t);
	y += pitch;

	// control
	t = iUi.Settings().iControl ? _L("The viewer may tap and type") : _L("The viewer can only watch");
	DrawLine(gc, y, _L("Control"), t);
	y += pitch;

	// how fast
	if (s && s->phase == CS_PHASE_SHARING)
		{
		TUint b = s->bytes_sec, u = s->ups_x10;
		if (b >= 1024)
			t.Format(_L("%u.%u KB a second"), b / 1024, (b % 1024) * 10 / 1024);
		else
			t.Format(_L("%u bytes a second"), b);
		t.AppendFormat(_L(", %u.%u updates a second"), u / 10, u % 10);
		}
	else
		t = _L("-");
	DrawLine(gc, y, _L("Sending"), t);
	y += pitch;

	if (s && s->capture[0])
		{
		TBuf8<40> c = FromC(s->capture, sizeof(s->capture) - 1);
		t.Copy(c);
		t[0] = (TText)(t[0] >= 'a' && t[0] <= 'z' ? t[0] - 32 : t[0]);
		DrawLine(gc, y, _L("Screen"), t);
		}

	// what the link is doing, in grey at the bottom (secondary)
	if (s && s->phase != CS_PHASE_SHARING && s->msg[0])
		{
		TBuf8<96> m = FromC(s->msg, sizeof(s->msg) - 1);
		t.Copy(m);
		gc.UseFont(iSmall);
		gc.SetPenColor(KRgbDarkGray);
		DrawClipped(gc, iSmall, t, TPoint(r.iTl.iX + 14, r.iBr.iY - 6 - iSmall->DescentInPixels()), r.Width() - 28);
		}
	gc.DiscardFont();
	}

// ===========================================================================
// Dialogs
// ===========================================================================

void CCsPrefsDialog::PreLayoutDynInitL()
	{
	SetChoiceListCurrentItem(ECsDlgRate, iSettings.iRate);
	SetChoiceListCurrentItem(ECsDlgControl, iSettings.iControl ? 1 : 0);
	SetChoiceListCurrentItem(ECsDlgBattery, iSettings.iBatteryStop ? 1 : 0);
	SetChoiceListCurrentItem(ECsDlgOnClose, iSettings.iKeepOnClose ? 1 : 0);
	}

TBool CCsPrefsDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iSettings.iRate = ChoiceListCurrentItem(ECsDlgRate);
	iSettings.iControl = ChoiceListCurrentItem(ECsDlgControl) == 1;
	iSettings.iBatteryStop = ChoiceListCurrentItem(ECsDlgBattery) == 1;
	iSettings.iKeepOnClose = ChoiceListCurrentItem(ECsDlgOnClose) == 1;
	return ETrue;
	}

void CCsConnDialog::PreLayoutDynInitL()
	{
	TPsiLink link;
	link.SetDefaults();
	link.Load(iCoeEnv->FsSession());
	SetChoiceListCurrentItem(ECsDlgLink, link.iNetMode ? 1 : 0);
	SetChoiceListCurrentItem(ECsDlgBaud, link.iBaudIndex);
	SetChoiceListCurrentItem(ECsDlgFlow, link.iRtsCts ? 1 : 0);
	SetEdwinTextL(ECsDlgPppStart, &link.iPppStart);
	SetChoiceListCurrentItem(ECsDlgProxy, link.iUseProxy ? 1 : 0);
	SetEdwinTextL(ECsDlgProxyHost, &link.iProxyHost);
	SetNumberEditorValue(ECsDlgProxyPort, link.iProxyPort);
	SetChoiceListCurrentItem(ECsDlgKeepUp, link.iKeepUp ? 0 : 1);
	DimKeepUpLine();
	}

// staying connected after use is the Psion Internet route's only: the
// modem route always hangs up (it holds the serial port)
void CCsConnDialog::DimKeepUpLine()
	{
	SetLineDimmedNow(ECsDlgKeepUp, ChoiceListCurrentItem(ECsDlgLink) != 1);
	}

void CCsConnDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId == ECsDlgLink)
		DimKeepUpLine();
	}

TBool CCsConnDialog::OkToExitL(TInt aButtonId)
	{
	TBuf<40> ppp;
	GetEdwinText(ppp, ECsDlgPppStart);
	ppp.TrimAll();
	if (aButtonId == ECsBidTest)
		{
		TBuf8<40> ppp8;
		ppp8.Copy(ppp);
		PgLinkTestL(ChoiceListCurrentItem(ECsDlgBaud), ChoiceListCurrentItem(ECsDlgFlow) == 1,
			ChoiceListCurrentItem(ECsDlgLink) == 1, ppp8, R_CS_TEST_DIALOG, ECsDlgTest1);
		return EFalse;
		}
	TBuf<60> host;
	GetEdwinText(host, ECsDlgProxyHost);
	host.Trim();
	RFs& fs = iCoeEnv->FsSession();
	TPsiLink link;
	link.SetDefaults();
	link.Load(fs);
	link.iNetMode = ChoiceListCurrentItem(ECsDlgLink) == 1;
	link.iBaudIndex = ChoiceListCurrentItem(ECsDlgBaud);
	link.iRtsCts = ChoiceListCurrentItem(ECsDlgFlow) == 1;
	link.iPppStart = ppp;
	link.iUseProxy = ChoiceListCurrentItem(ECsDlgProxy) == 1;
	link.iProxyHost = host;
	TInt port = NumberEditorValue(ECsDlgProxyPort);
	if (port > 0 && port < 65536)
		link.iProxyPort = port;
	link.iKeepUp = ChoiceListCurrentItem(ECsDlgKeepUp) == 0;
	link.Save(fs);
	// a link already up and now to stay up: hold it (psihold.exe ends by
	// itself when the setting goes the other way)
	if (link.iKeepUp && link.iNetMode)
		{
		TFileName near(iEikonEnv->EikAppUi()->Application()->AppFullName());
		PsiHoldStart(fs, near);
		}
	return ETrue;
	}

void CCsAboutDialog::PreLayoutDynInitL()
	{
	TBuf<32> title(_L("PsiCast "));
	title.Append(KVersion);
	((CEikLabel*)Control(ECsDlgAbout1))->SetFont(iEikonEnv->TitleFont());
	SetLabelL(ECsDlgAbout1, title);
	SetLabelL(ECsDlgAboutStatus, iStatus);
	}

// ===========================================================================
// The engine's process
// ===========================================================================

CCsWatcher::CCsWatcher(CCsAppUi& aUi)
	: CActive(EPriorityStandard), iUi(aUi)
	{
	CActiveScheduler::Add(this);
	}

CCsWatcher::~CCsWatcher()
	{
	Cancel();
	}

void CCsWatcher::Watch(RProcess& aProcess)
	{
	Cancel();
	iProcess = &aProcess;
	aProcess.Logon(iStatus);
	SetActive();
	}

void CCsWatcher::RunL()
	{
	iUi.EngineEnded();
	}

void CCsWatcher::DoCancel()
	{
	if (iProcess)
		iProcess->LogonCancel(iStatus);
	}

// ===========================================================================
// The app
// ===========================================================================

void CCsAppUi::ConstructL()
	{
	BaseConstructL();
	iLastPhase = -1;
	LoadSettings();
	TRAPD(pics, ToolbarPicturesL());
	(void)pics;                              // (no PsiCast.mbm: words only)
	if (iToolBar)
		{
		const TInt latch[3] = { ECsCmdShareButton, ECsCmdPause, ECsCmdControl };
		for (TInt k = 0; k < 3; k++)
			{
			CEikButtonBase* b = (CEikButtonBase*)iToolBar->ControlById(latch[k]);
			if (b)
				b->SetBehavior(EEikButtonLatches);
			}
		if (!iSettings.iToolbar)
			iToolBar->MakeVisible(EFalse);
		}
	iView = new(ELeave) CCsView(*this);
	iView->ConstructL(ViewRect(iSettings.iToolbar));
	AddToStackL(iView);
	iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
	iCoeEnv->RootWin().EnableOnEvents(EEventControlAlways);   // switch-on events in the background too
	iForeground = ETrue;
	// still sharing from before PsiCast closed? take it up again
	TRAPD(err, ReattachL());
	(void)err;
	SetTickRate(EFalse);
	ButtonStates();
	}

CCsAppUi::~CCsAppUi()
	{
	delete iTimer;
	iTimer = NULL;
	CapturePauseKey(EFalse);
	if (iView)
		{
		RemoveFromStack(iView);
		delete iView;
		iView = NULL;
		}
	iClosing = ETrue;                        // (nothing more is drawn)
	if (iEngineOn)
		{
		// closed from the System screen or the list of open programs: the
		// same as Close
		if (iSettings.iKeepOnClose)
			Detach();
		else
			Stop(EFalse);
		}
	delete iWatcher;
	iRinger.Close();
	if (iChunkOpen)
		iChunk.Close();
	}

// ----- settings -----------------------------------------------------------------

void CCsAppUi::LoadSettings()
	{
	iSettings.iToolbar = 1;
	iSettings.iZoom = 1;
	iSettings.iRate = 2;                     // 10 a second
	iSettings.iControl = 0;                  // watch only, as standard
	iSettings.iBatteryStop = 1;
	iSettings.iKeepOnClose = 0;
	RFile f;
	if (f.Open(iCoeEnv->FsSession(), KIniFile, EFileRead) != KErrNone)
		return;
	TBuf8<8 + 4 * 32> d;
	TInt r = f.Read(d);
	f.Close();
	if (r != KErrNone || d.Length() < 8)
		return;
	const TUint8* p = d.Ptr();
	TInt version = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
	TInt count = p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24);
	if (version < 1 || count < 0)
		return;
	TInt* field = &iSettings.iToolbar;
	for (TInt i = 0; i < count && i < KCsSettingsFields && 8 + 4 * i + 4 <= d.Length(); i++)
		{
		const TUint8* q = p + 8 + 4 * i;
		field[i] = q[0] | (q[1] << 8) | (q[2] << 16) | (q[3] << 24);
		}
	if (iSettings.iZoom < 0 || iSettings.iZoom > 2)
		iSettings.iZoom = 1;
	if (iSettings.iRate < 0 || iSettings.iRate > 3)
		iSettings.iRate = 2;
	}

void CCsAppUi::SaveSettings()
	{
	RFs& fs = iCoeEnv->FsSession();
	fs.MkDirAll(KIniFile);
	TBuf8<8 + 4 * KCsSettingsFields> d;
	const TInt head[2] = { KCsSettingsVersion, KCsSettingsFields };
	const TInt* field = &iSettings.iToolbar;
	for (TInt i = 0; i < 2 + KCsSettingsFields; i++)
		{
		TInt v = i < 2 ? head[i] : field[i - 2];
		d.Append((TUint8)v);
		d.Append((TUint8)(v >> 8));
		d.Append((TUint8)(v >> 16));
		d.Append((TUint8)(v >> 24));
		}
	RFile f;
	if (f.Replace(fs, KIniFile, EFileWrite) != KErrNone)
		return;
	f.Write(d);
	f.Close();
	}

// ----- the engine ---------------------------------------------------------------

static TUint ProcessIdOf(RProcess& aProcess)
	{
	TProcessId id = aProcess.Id();
	TUint v = 0;
	Mem::Copy(&v, &id, sizeof(v));
	return v;
	}

static TBool OpenProcess(RProcess& aProcess, TUint aPid)
	{
	if (aPid == 0)
		return EFalse;
	TProcessId id;
	Mem::Copy(&id, &aPid, sizeof(aPid));
	if (aProcess.Open(id) != KErrNone)
		return EFalse;
	if (aProcess.ExitType() != EExitPending)
		{
		aProcess.Close();
		return EFalse;
		}
	return ETrue;
	}

// An engine still sharing after PsiCast closed (Keep sharing): PsiCast
// watches it again, and it watches PsiCast again
TBool CCsAppUi::ReattachL()
	{
	RChunk c;
	if (c.OpenGlobal(_L(PSI_SHARED_NAME), EFalse) != KErrNone)
		return EFalse;
	CsShared* s = (CsShared*)c.Base();
	if (s->magic != CS_MAGIC || s->layout != CS_LAYOUT || s->state == PSI_STATE_EXITED
		|| s->phase == CS_PHASE_STOPPED || !OpenProcess(iProcess, s->eng_pid))
		{
		c.Close();
		return EFalse;
		}
	iChunk = c;
	iChunkOpen = ETrue;
	iShared = s;
	RProcess me;
	s->app_pid = ProcessIdOf(me);
	s->detached = 0;
	iPaused = s->paused;
	iEngineOn = ETrue;
	iSaidSharing = s->phase == CS_PHASE_SHARING;
	if (!iWatcher)
		iWatcher = new(ELeave) CCsWatcher(*this);
	iWatcher->Watch(iProcess);
	PassSettings();
	CapturePauseKey(ETrue);
	return ETrue;
	}

// a byte in the engine's key ring, and its doorbell: its wait ends at once
void CCsAppUi::Poke()
	{
	if (!iShared)
		return;
	PsiShared& n = iShared->net;
	if (n.kbd_head - n.kbd_tail < PSI_KBD_SIZE)
		{
		n.kbd[n.kbd_head % PSI_KBD_SIZE] = 0;
		n.kbd_head++;
		}
	iRinger.Ring(&n.eng_bell);
	}

void CCsAppUi::PassSettings()
	{
	if (!iShared)
		return;
	iShared->max_rate = KRates[iSettings.iRate];
	iShared->allow_control = iSettings.iControl;
	iShared->paused = iPaused;
	iShared->battery_stop = iSettings.iBatteryStop;
	iShared->app_seq++;
	Poke();
	}

void CCsAppUi::StartL()
	{
	if (iEngineOn)
		return;
	if (!iChunkOpen)
		{
		TInt r = iChunk.CreateGlobal(_L(PSI_SHARED_NAME), sizeof(CsShared), sizeof(CsShared));
		if (r == KErrAlreadyExists)
			{
			r = iChunk.OpenGlobal(_L(PSI_SHARED_NAME), EFalse);
			// (an engine from another version may still hold it: stop that one)
			if (r == KErrNone)
				{
				CsShared* old = (CsShared*)iChunk.Base();
				RProcess p;
				if (old->magic == CS_MAGIC && OpenProcess(p, old->eng_pid))
					{
					p.Kill(0);
					p.Close();
					PsiLinkTimersBack();
					}
				}
			}
		User::LeaveIfError(r);
		iChunkOpen = ETrue;
		iShared = (CsShared*)iChunk.Base();
		}
	CsShared* s = iShared;
	Mem::FillZ(s, sizeof(CsShared));
	s->net.magic = PSI_SHARED_MAGIC;
	TPsiLink link;
	link.SetDefaults();
	link.Load(iCoeEnv->FsSession());
	s->net.baud_index = link.iBaudIndex;
	s->net.rtscts = link.iRtsCts;
	s->net.net_mode = link.iNetMode;
	s->net.modem_tls = 0;
	CopyToC(s->net.ppp_start, sizeof(s->net.ppp_start), link.iPppStart);
	CopyToC(s->net.dial_prefix, sizeof(s->net.dial_prefix), _L("ATDT"));
	CopyToC(s->net.host, sizeof(s->net.host), KAtomHost);
	s->net.port = KAtomPort;
	CopyToC(s->net.home, sizeof(s->net.home), _L("C:\\System\\Apps\\PsiCast"));
	CopyToC(s->net.version, sizeof(s->net.version), KVersion);
	s->net.bell_magic = PSI_BELL_MAGIC;      // we ring eng_bell for every change, quit and switch-on
	s->net.app_beat = 0;                     // (not watched by psiglue: the engine watches our process)
	s->magic = CS_MAGIC;
	s->layout = CS_LAYOUT;
	RProcess me;
	s->app_pid = ProcessIdOf(me);
	s->viewers = -1;
	iPaused = EFalse;
	PassSettings();

	TParse parse;
	parse.Set(Application()->AppFullName(), NULL, NULL);
	TFileName exe(parse.DriveAndPath());
	exe.Append(KEngineExe);
	TInt r = iProcess.Create(exe, KNullDesC);
	if (r != KErrNone)
		{
		iEikonEnv->InfoMsg(_L("psicast.exe not found - install PsiCast again"));
		User::Leave(r);
		}
	iEngineOn = ETrue;
	iSaidSharing = EFalse;
	iLastPhase = -1;
	if (!iWatcher)
		iWatcher = new(ELeave) CCsWatcher(*this);
	iWatcher->Watch(iProcess);
	iProcess.Resume();
	CapturePauseKey(ETrue);
	SetTickRate(ETrue);
	ButtonStates();
	iView->DrawNow();
	}

void CCsAppUi::Stop(TBool aSay)
	{
	if (!iEngineOn)
		return;
	if (iWatcher)
		iWatcher->Cancel();
	iShared->quitting = 1;
	iShared->net.quit = 1;
	iShared->app_seq++;
	Poke();
	// BYE, then the modem's hang-up (+++, ATH: about 3 s)
	for (TInt i = 0; i < 60 && iShared->state != PSI_STATE_EXITED; i++)
		User::After(100000);
	if (iShared->state != PSI_STATE_EXITED)
		{
		iProcess.Kill(0);
		PsiLinkTimersBack();
		}
	iProcess.Close();
	iEngineOn = EFalse;
	iPaused = EFalse;
	CapturePauseKey(EFalse);
	if (iBusyShown)
		{
		iEikonEnv->BusyMsgCancel();
		iBusyShown = EFalse;
		}
	SetTickRate(EFalse);
	if (aSay)
		Say(_L("Sharing stopped"));
	ButtonStates();
	if (iView)
		iView->DrawNow();
	}

// PsiCast closes, the sharing goes on (Preferences > When PsiCast closes)
void CCsAppUi::Detach()
	{
	if (!iEngineOn)
		return;
	if (iWatcher)
		iWatcher->Cancel();
	iShared->detached = 1;
	iShared->app_pid = 0;
	iShared->app_seq++;
	Poke();
	iProcess.Close();
	iEngineOn = EFalse;
	}

// it ended by itself: the battery, or something it could not get past
void CCsAppUi::EngineEnded()
	{
	TInt why = iShared ? iShared->stop_reason : CS_STOP_NONE;
	TExitType type = iProcess.ExitType();
	iProcess.Close();
	iEngineOn = EFalse;
	iPaused = EFalse;
	CapturePauseKey(EFalse);
	if (iBusyShown)
		{
		iEikonEnv->BusyMsgCancel();
		iBusyShown = EFalse;
		}
	SetTickRate(EFalse);
	if (type != EExitKill && type != EExitPending)
		PsiLinkTimersBack();                 // (a panic: the link's timers back on)
	switch (why)
		{
	case CS_STOP_BATTERY:
		Say(_L("Sharing stopped - the battery is low"));
		break;
	case CS_STOP_CAPTURE:
		Say(_L("Sharing stopped - the screen could not be read"));
		break;
	case CS_STOP_MEMORY:
		Say(_L("Sharing stopped - not enough memory"));
		break;
	default:
		Say(_L("Sharing stopped"));
		break;
		}
	ButtonStates();
	iView->DrawNow();
	}

void CCsAppUi::StateText(TDes& aText) const
	{
	if (!iEngineOn || !iShared)
		{
		aText = _L("Not sharing");
		return;
		}
	switch (iShared->phase)
		{
	case CS_PHASE_STARTING:
	case CS_PHASE_CONNECTING:
		aText = _L("Connecting to the Atom...");
		return;
	case CS_PHASE_WAITING:
		aText = _L("Waiting for the Atom...");
		return;
	case CS_PHASE_STOPPED:
		aText = _L("Not sharing");
		return;
		}
	if (iPaused)
		aText = _L("Paused - viewers see the last picture");
	else if (iShared->viewers < 0)
		aText = _L("Sharing");
	else if (iShared->viewers == 0)
		aText = _L("Sharing - no viewers yet");
	else if (iShared->viewers == 1)
		aText = _L("Sharing with 1 viewer");
	else
		aText.Format(_L("Sharing with %d viewers"), iShared->viewers);
	}

// ----- the timer ---------------------------------------------------------------------

void CCsAppUi::SetTickRate(TBool aFast)
	{
	if (!iTimer)
		return;
	if (iTimer->IsActive() && aFast == iFast)
		return;
	iTimer->Cancel();
	iFast = aFast;
	TInt t = aFast ? KTickFast : KTickIdle;
	iTimer->Start(t, t, TCallBack(TickCallback, this));
	}

TInt CCsAppUi::TickCallback(TAny* aSelf)
	{
	((CCsAppUi*)aSelf)->Tick();
	return 0;
	}

void CCsAppUi::Tick()
	{
	if (!iEngineOn || !iShared)
		return;
	TInt phase = iShared->phase;
	TBool connecting = phase == CS_PHASE_STARTING || phase == CS_PHASE_CONNECTING;
	SetTickRate(connecting);
	// the busy message bottom left while it connects (in front only)
	if (connecting && iForeground && !iBusyShown)
		{
		iEikonEnv->BusyMsgL(_L("Connecting to the Atom..."), EHLeftVBottom, TTimeIntervalMicroSeconds32(300000));
		iBusyShown = ETrue;
		}
	else if (!connecting && iBusyShown)
		{
		iEikonEnv->BusyMsgCancel();
		iBusyShown = EFalse;
		}
	if (phase != iLastPhase)
		{
		if (phase == CS_PHASE_SHARING && !iSaidSharing)
			{
			Say(_L("Sharing the screen"));
			iSaidSharing = ETrue;
			}
		else if (phase == CS_PHASE_WAITING && iLastPhase == CS_PHASE_SHARING)
			Say(_L("Lost the Atom - trying again"));
		iLastPhase = phase;
		}
	if (iShared->eng_seq != iSeenSeq)
		{
		iSeenSeq = iShared->eng_seq;
		iView->DrawNow();
		}
	}

// ----- commands ------------------------------------------------------------------------

void CCsAppUi::Say(const TDesC& aText)
	{
	if (iForeground)
		iEikonEnv->InfoMsg(aText);
	else
		User::InfoPrint(aText);              // (over the program in front)
	}

void CCsAppUi::PauseL(TBool aPause)
	{
	if (!iEngineOn)
		{
		Say(_L("Not available - not sharing"));
		ButtonStates();
		return;
		}
	if (aPause == iPaused)
		return;
	iPaused = aPause;
	PassSettings();
	Say(iPaused ? _L("Sharing paused") : _L("Sharing again"));
	ButtonStates();
	iView->DrawNow();
	}

void CCsAppUi::ControlL(TBool aAllow)
	{
	if (aAllow && !iSettings.iControl
		&& !iEikonEnv->QueryWinL(_L("Viewers will be able to tap and type on this Psion"), _L("Allow control from the viewer?")))
		{
		ButtonStates();
		return;
		}
	iSettings.iControl = aAllow;
	SaveSettings();
	PassSettings();
	Say(aAllow ? _L("The viewer may tap and type") : _L("The viewer can only watch"));
	ButtonStates();
	iView->DrawNow();
	}

// PsiCast to the back: the program used before it comes to the front, to
// be shown
void CCsAppUi::PreviousProgram()
	{
	iCoeEnv->RootWin().SetOrdinalPosition(-1);
	}

void CCsAppUi::PrefsL()
	{
	TCsSettings s = iSettings;
	CCsPrefsDialog* dlg = new(ELeave) CCsPrefsDialog(s);
	if (!dlg->ExecuteLD(R_CS_PREFS_DIALOG))
		return;
	iSettings = s;
	SaveSettings();
	PassSettings();
	ButtonStates();
	iView->DrawNow();
	}

void CCsAppUi::ConnSettingsL()
	{
	CCsConnDialog* dlg = new(ELeave) CCsConnDialog;
	if (!dlg->ExecuteLD(R_CS_CONN_DIALOG))
		return;
	// the engine reads the settings as it starts: start it again
	if (iEngineOn)
		{
		Stop(EFalse);
		StartL();
		}
	}

void CCsAppUi::AboutL()
	{
	TBuf<100> status;
	StateText(status);
	if (iEngineOn && iShared && iShared->capture[0])
		{
		TBuf<40> c;
		c.Copy(FromC(iShared->capture, sizeof(iShared->capture) - 1));
		status.Append(_L(" - "));
		status.Append(c);
		}
	CCsAboutDialog* dlg = new(ELeave) CCsAboutDialog(status);
	dlg->ExecuteLD(R_CS_ABOUT_DIALOG);
	}

void CCsAppUi::ZoomL(TInt aStep)
	{
	iSettings.iZoom = (iSettings.iZoom + aStep + 3) % 3;     // (round, as the built-in programs)
	iView->ApplyZoomL();
	iView->DrawNow();
	SaveSettings();
	}

TRect CCsAppUi::ViewRect(TBool aToolbar) const
	{
	TRect r = ClientRect();
	if (!aToolbar || !iToolBar)
		r.iBr.iX = iEikonEnv->ScreenDevice()->SizeInPixels().iWidth;
	return r;
	}

void CCsAppUi::ShowToolBarL(TBool aShow)
	{
	if (!iToolBar)
		return;
	iToolBar->MakeVisible(aShow);
	if (aShow)
		iToolBar->DrawNow();
	iView->SetViewRectL(ViewRect(aShow));
	iSettings.iToolbar = aShow;
	SaveSettings();
	}

// the latching buttons say what is so
void CCsAppUi::ButtonStates()
	{
	if (!iToolBar || iClosing)
		return;
	const TInt ids[3] = { ECsCmdShareButton, ECsCmdPause, ECsCmdControl };
	const TBool on[3] = { iEngineOn, iEngineOn && iPaused, iSettings.iControl };
	for (TInt k = 0; k < 3; k++)
		{
		CEikButtonBase* b = (CEikButtonBase*)iToolBar->ControlById(ids[k]);
		if (!b)
			continue;
		CEikButtonBase::TState want = on[k] ? CEikButtonBase::ESet : CEikButtonBase::EClear;
		if (b->State() != want)
			{
			b->SetState(want);
			if (iToolBar->IsVisible())
				b->DrawNow();
			}
		}
	}

// Shift+Ctrl+W pauses from any program while the screen is shared (it is
// taken from the others meanwhile): no need to come back to PsiCast first
void CCsAppUi::CapturePauseKey(TBool aOn)
	{
	if (aOn && !iPauseCapture)
		{
		TInt32 h = iCoeEnv->RootWin().CaptureKey(KPauseKeyCode, EModifierCtrl | EModifierShift, EModifierCtrl | EModifierShift);
		iPauseCapture = h > 0 ? h : 0;
		}
	else if (!aOn && iPauseCapture)
		{
		iCoeEnv->RootWin().CancelCaptureKey(iPauseCapture);
		iPauseCapture = 0;
		}
	}

void CCsAppUi::HandleCommandL(TInt aCommand)
	{
	switch (aCommand)
		{
	case ECsCmdStart:
		if (iEngineOn)
			Say(_L("Already sharing"));
		else
			StartL();
		break;
	case ECsCmdStop:
		if (!iEngineOn)
			Say(_L("Not available - not sharing"));
		else
			Stop(ETrue);
		break;
	case ECsCmdShareButton:
		if (iEngineOn)
			Stop(ETrue);
		else
			StartL();
		break;
	case ECsCmdPause:
		PauseL(!iPaused);
		break;
	case ECsCmdControl:
		ControlL(!iSettings.iControl);
		break;
	case ECsCmdPrevious:
		PreviousProgram();
		break;
	case ECsCmdZoomIn:
		ZoomL(1);
		break;
	case ECsCmdZoomOut:
		ZoomL(-1);
		break;
	case ECsCmdToolbar:
		ShowToolBarL(!iToolBar || !iToolBar->IsVisible());
		break;
	case ECsCmdPrefs:
		PrefsL();
		break;
	case ECsCmdConnSettings:
		ConnSettingsL();
		break;
	case ECsCmdDisconnect:
		{
		// the Psion Internet connection down, even when "Stay connected"
		// holds it (ssh/pglinktest.cpp; ends psihold.exe). Sharing stops
		// first, or it would only dial again.
		TPsiLink link;
		link.SetDefaults();
		link.Load(iCoeEnv->FsSession());
		if (!link.iNetMode)
			Say(_L("Not available - the modem route hangs up after use"));
		else
			{
			Stop(EFalse);
			PgLinkDisconnectL();
			}
		break;
		}
	case ECsCmdHelp:
		{
		CCsHelpDialog* dlg = new(ELeave) CCsHelpDialog;
		dlg->ExecuteLD(R_CS_HELP_DIALOG);
		break;
		}
	case ECsCmdAbout:
		AboutL();
		break;
	case EEikCmdExit:
		if (iEngineOn)
			{
			if (iSettings.iKeepOnClose)
				Detach();
			else
				Stop(EFalse);
			}
		Exit();
		break;
		}
	}

// Only items in this pane are dimmed or ticked (EIKON 8 otherwise)
void CCsAppUi::DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	switch (aMenuId)
		{
	case R_CS_SHARE_MENU:
		aMenuPane->SetItemDimmed(ECsCmdStart, iEngineOn);
		aMenuPane->SetItemDimmed(ECsCmdStop, !iEngineOn);
		aMenuPane->SetItemDimmed(ECsCmdPause, !iEngineOn);
		aMenuPane->SetItemButtonState(ECsCmdPause,
			iEngineOn && iPaused ? EEikMenuItemSymbolOn : EEikMenuItemSymbolIndeterminate);
		aMenuPane->SetItemButtonState(ECsCmdControl,
			iSettings.iControl ? EEikMenuItemSymbolOn : EEikMenuItemSymbolIndeterminate);
		break;
	case R_CS_VIEW_MENU:
		aMenuPane->SetItemButtonState(ECsCmdToolbar,
			iToolBar && iToolBar->IsVisible() ? EEikMenuItemSymbolOn : EEikMenuItemSymbolIndeterminate);
		break;
	case R_CS_TOOLS_MENU:
		{
		TPsiLink link;
		link.SetDefaults();
		link.Load(iCoeEnv->FsSession());
		aMenuPane->SetItemDimmed(ECsCmdDisconnect, !link.iNetMode || !PgLinkUp());
		break;
		}
		}
	}

// switched back on: the engine checks its connection is still there
void CCsAppUi::HandleSwitchOnEventL(CCoeControl* /*aDestination*/)
	{
	if (iEngineOn && iShared)
		{
		iShared->net.switch_on++;
		Poke();
		}
	}

void CCsAppUi::HandleWsEventL(const TWsEvent& aEvent, CCoeControl* aDestination)
	{
	if (aEvent.Type() == EEventKey && iPauseCapture)
		{
		const TKeyEvent* k = aEvent.Key();
		if (k->iCode == KPauseKeyCode && (k->iModifiers & EModifierShift) && (k->iModifiers & EModifierCtrl))
			{
			if (IsDisplayingMenuOrDialog() && iForeground)
				return;                      // (a dialog of ours has the keys)
			PauseL(!iPaused);                // (taken: it comes here in the background too)
			return;
			}
		}
	if (aEvent.Type() == EEventFocusGained)
		iForeground = ETrue;
	else if (aEvent.Type() == EEventFocusLost)
		{
		iForeground = EFalse;
		if (iBusyShown)
			{
			iEikonEnv->BusyMsgCancel();
			iBusyShown = EFalse;
			}
		}
	CEikAppUi::HandleWsEventL(aEvent, aDestination);
	}

// a toolbar button's picture, from PsiCast.mbm (cast/pkg/mktbar.py)
void CCsAppUi::ButtonPictureL(TInt aId, TInt aIcon)
	{
	CEikCommandButton* b = iToolBar ? (CEikCommandButton*)iToolBar->ControlById(aId) : NULL;
	if (!b)
		return;
	TFileName mbm = Application()->BitmapStoreName();
	CFbsBitmap* bmp = iEikonEnv->CreateBitmapL(mbm, aIcon);
	CleanupStack::PushL(bmp);
	CFbsBitmap* mask = iEikonEnv->CreateBitmapL(mbm, aIcon + 1);
	CleanupStack::PushL(mask);
	b->SetPictureL(bmp, mask);
	CleanupStack::Pop(2);
	if (b->Picture())
		b->Picture()->SetAlignment(EHCenterVCenter);
	if (b->Label())
		b->Label()->SetAlignment(EHLeftVCenter);
	b->LayoutComponentsL();
	}

void CCsAppUi::ToolbarPicturesL()
	{
	ButtonPictureL(ECsCmdShareButton, EMbmToolShare);
	ButtonPictureL(ECsCmdPause, EMbmToolPause);
	ButtonPictureL(ECsCmdControl, EMbmToolControl);
	ButtonPictureL(ECsCmdPrevious, EMbmToolPrevious);
	}

// ===========================================================================
// Document, application, DLL
// ===========================================================================

CEikAppUi* CCsDocument::CreateAppUiL()
	{
	return new(ELeave) CCsAppUi;
	}

TUid CCsApplication::AppDllUid() const
	{
	return KUidPsiCast;
	}

CApaDocument* CCsApplication::CreateDocumentL()
	{
	return new(ELeave) CCsDocument(*this);
	}

EXPORT_C CApaApplication* NewApplication()
	{
	return new CCsApplication;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
