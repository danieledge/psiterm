// PSITERM.CPP - PsiTerm: libvterm-based serial terminal for the Psion Series 5mx
//
// Structure:
//   CSerialPort   - RComm wrapper; battery-friendly reads (block for 1 byte,
//                   then short timed reads while data keeps flowing)
//   CTermView     - owns the libvterm instance, draws its screen into a
//                   backed-up 16-grey window, maps Psion keys to vterm keys
//   CPsiTermAppUi - menus, commands, settings file

#include <basched.h>        // KLeaveExit
#include <e32keys.h>
#include <e32svr.h>
#include <estlib.h>
#include <e32hal.h>
#include <eikchlst.h>
#include <eikseced.h>
#include <baclipb.h>
#include <txtetext.h>
#include <eikdll.h>
#include <apgcli.h>
#include <eikedwin.h>
#include <eikcmbut.h>
#include <eiksbfrm.h>
#include <eikbtpan.h>
#include "psiterm.h"

#ifndef TRAP_IGNORE
#define TRAP_IGNORE(s) { TInt _ignored; TRAP(_ignored, s); }
#endif

_LIT(KPddName, "EUART1");
_LIT(KLddName, "ECOMM");
_LIT(KCsyName, "ECUART");
_LIT(KPortName, "COMM::0");
_LIT(KFontCourier, "Courier");
_LIT(KFontTerminus, "Terminus");         // from PsiTerm.gdr, next to the app
_LIT(KFontFile, "PsiTerm.gdr");

// Zoom levels, smallest first: typeface (0 = Terminus, 1 = the ROM's Courier)
// and pixel height. The sidebar zoom buttons step through these. The one
// Courier size is the only one with 24+ rows, for apps like btop.
const TInt KZoomLevels = 5;
const TInt KZoomFace[KZoomLevels] = { 1, 0, 0, 0, 0 };
const TInt KZoomPixels[KZoomLevels] = { 8, 12, 14, 16, 18 };
const TInt KDefaultZoom = 1;             // Terminus 6x12: 106 x 20
_LIT(KIniFile, "C:\\System\\Apps\\PsiTerm\\PsiTerm.ini");
_LIT(KHostsFile, "C:\\System\\Apps\\PsiTerm\\Hosts.dat");
_LIT(KSnippetsFile, "C:\\System\\Apps\\PsiTerm\\Snippets.dat");
_LIT8(KHangupEscape, "+++");
_LIT8(KHangupCommand, "ATH\r");

const TInt KMoreDataTimeout = 25000;   // microseconds to wait for more bytes
_LIT(KSshExeName, "psissh.exe");
_LIT(KSeedFile, "C:\\System\\Apps\\PsiTerm\\ssh_seed.bin");
_LIT(KKeysFile, "C:\\System\\Apps\\PsiTerm\\Keys.dat");
_LIT(KKeysDir, "C:\\System\\Apps\\PsiTerm\\Keys\\");
_LIT(KOldKey, "C:\\System\\Apps\\PsiTerm\\id_ed25519");       // 0.44-0.49
_LIT(KSshHome, "C:\\System\\Apps\\PsiTerm");
const TInt KEntropyKeysNeeded = 40;
const TInt KScrollbackLines = 300;       // ~150 KB of history
const TInt KScrollbackCols = 128;
const TInt KClipMax = 16384;            // most text copied/pasted at once
// releases: dist/ in github.com/danieledge/psiterm, fetched over HTTPS
_LIT8(KGitHubHost, "raw.githubusercontent.com");
_LIT8(KGitHubPath, "/danieledge/psiterm/main/dist/");
_LIT(KPsiTermVersion, "0.53");           // also in psiterm.pkg; version.txt must match

static TBps BaudFromIndex(TInt aIndex)
	{
	switch (aIndex)
		{
	case 0: return EBps9600;
	case 1: return EBps19200;
	case 2: return EBps38400;
	case 3: return EBps57600;
	default: return EBps115200;
		}
	}

static TPtrC LeftSafe(const TDesC& aText, TInt aMax)
	{
	return aText.Left(aText.Length() < aMax ? aText.Length() : aMax);
	}

_LIT(KPppSuffix, " (Psion Internet)");

static TInt BaudValue(TInt aIndex)
	{
	switch (aIndex)
		{
	case 0: return 9600;
	case 1: return 19200;
	case 2: return 38400;
	case 3: return 57600;
	default: return 115200;
		}
	}

// ===========================================================================
// CSerialPort
// ===========================================================================

CSerialPort::CSerialPort(MSerialObserver& aObserver)
	: CActive(EPriorityStandard), iObserver(aObserver), iState(EIdle)
	{
	}

CSerialPort* CSerialPort::NewL(MSerialObserver& aObserver)
	{
	CSerialPort* self = new(ELeave) CSerialPort(aObserver);
	CActiveScheduler::Add(self);
	return self;
	}

CSerialPort::~CSerialPort()
	{
	Close();
	if (iServerOpen)
		iServer.Close();
	}

TInt CSerialPort::Open(TInt aBaudIndex, TBool aRtsCts)
	{
	Close();
	TInt r = User::LoadPhysicalDevice(KPddName);
	if (r != KErrNone && r != KErrAlreadyExists)
		return r;
	r = User::LoadLogicalDevice(KLddName);
	if (r != KErrNone && r != KErrAlreadyExists)
		return r;
	if (!iServerOpen)
		{
		r = StartC32();
		if (r != KErrNone && r != KErrAlreadyExists)
			return r;
		r = iServer.Connect();
		if (r != KErrNone)
			return r;
		iServerOpen = ETrue;
		r = iServer.LoadCommModule(KCsyName);
		if (r != KErrNone && r != KErrAlreadyExists)
			return r;
		}
	r = iComm.Open(iServer, KPortName, ECommExclusive);
	if (r != KErrNone)
		return r;

	TCommConfig cfg;
	iComm.Config(cfg);
	cfg().iRate = BaudFromIndex(aBaudIndex);
	cfg().iDataBits = EData8;
	cfg().iStopBits = EStop1;
	cfg().iParity = EParityNone;
	cfg().iFifo = EFifoEnable;
	cfg().iTerminatorCount = 0;
	// No KConfigFailDSR: the WiRSa's DSR line must never matter.
	cfg().iHandshake = aRtsCts ? (KConfigObeyCTS | KConfigFreeRTS) : 0;
	r = iComm.SetConfig(cfg);
	if (r != KErrNone)
		{
		iComm.Close();
		return r;
		}
	iComm.SetReceiveBufferLength(16384);
	iComm.SetSignals(KSignalDTR | KSignalRTS, 0);
	iComm.ResetBuffers();
	iOpen = ETrue;
	iErrorCount = 0;
	ReadFirst();
	return KErrNone;
	}

// Reports what the serial hardware says it can do, and whether it accepts
// rates above 115200 (EPOC R5 only lists up to 115200; faster needs the
// driver to accept a "special" rate). Also looks for a second port (IrDA).
void CSerialPort::Probe(TDes8& aOut)
	{
	if (!iOpen)
		{
		aOut.Append(_L8("Serial port is not open.\r\n"));
		return;
		}
	Cancel();
	iState = EIdle;
	TCommCaps caps;
	iComm.Caps(caps);
	aOut.AppendFormat(_L8("COMM::0 rate caps 0x%x, handshake caps 0x%x, IR caps 0x%x\r\n"),
		caps().iRate, caps().iHandshake, caps().iSIR);
	aOut.Append((caps().iRate & KCapsBpsSpecial) ? _L8("  Driver allows special rates.\r\n")
		: _L8("  No special rates: 115200 is the ceiling.\r\n"));
	TCommConfig old;
	iComm.Config(old);
	static const TInt KRates[] = { 230400, 460800 };
	for (TInt i = 0; i < 2; i++)
		{
		TCommConfig c = old;
		c().iRate = EBpsSpecial;
		c().iSpecialRate = KRates[i];
		TInt r = iComm.SetConfig(c);
		if (r == KErrNone)
			aOut.AppendFormat(_L8("  %d baud: accepted by the driver\r\n"), KRates[i]);
		else
			aOut.AppendFormat(_L8("  %d baud: refused (%d)\r\n"), KRates[i], r);
		}
	iComm.SetConfig(old);
	RComm second;
	TInt r = second.Open(iServer, _L("COMM::1"), ECommShared);
	if (r == KErrNone)
		{
		second.Caps(caps);
		aOut.AppendFormat(_L8("COMM::1 rate caps 0x%x, IR caps 0x%x\r\n"), caps().iRate, caps().iSIR);
		second.Close();
		}
	else
		aOut.AppendFormat(_L8("COMM::1 not available (%d)\r\n"), r);
	ReadFirst();
	}

void CSerialPort::Close()
	{
	Cancel();
	if (iOpen)
		{
		iComm.Close();
		iOpen = EFalse;
		}
	iState = EIdle;
	}

TInt CSerialPort::Write(const TDesC8& aData)
	{
	if (!iOpen || aData.Length() == 0)
		return KErrNotReady;
	TRequestStatus stat;
	iComm.Write(stat, TTimeIntervalMicroSeconds32(3000000), aData);
	User::WaitForRequest(stat);
	return stat.Int();
	}

void CSerialPort::ReadFirst()
	{
	// Block (no timer) until one byte arrives - costs no battery while idle.
	iFirst.Zero();
	iComm.Read(iStatus, iFirst);
	iState = EWaitFirst;
	SetActive();
	}

void CSerialPort::ReadMore()
	{
	// Data is flowing: collect a burst, returning early after a short gap.
	iBuf.Zero();
	iComm.Read(iStatus, TTimeIntervalMicroSeconds32(KMoreDataTimeout), iBuf);
	iState = EWaitMore;
	SetActive();
	}

void CSerialPort::RunL()
	{
	TInt status = iStatus.Int();
	if (iState == EWaitFirst)
		{
		if (status == KErrNone)
			{
			iErrorCount = 0;
			iObserver.SerialDataL(iFirst);
			ReadMore();
			return;
			}
		}
	else if (iState == EWaitMore)
		{
		if (status == KErrNone || status == KErrTimedOut)
			{
			iErrorCount = 0;
			if (iBuf.Length() > 0)
				{
				iObserver.SerialDataL(iBuf);
				ReadMore();
				}
			else
				ReadFirst();
			return;
			}
		}
	// Line errors (overrun, framing, parity): report the first few, keep going.
	if (status == KErrCancel)
		return;
	if (++iErrorCount <= 3)
		iObserver.SerialError(status);
	if (iErrorCount > 50)
		{
		iState = EIdle;      // give up rather than spin forever
		return;
		}
	ReadFirst();
	}

void CSerialPort::DoCancel()
	{
	iComm.ReadCancel();
	}

// ===========================================================================
// CTermView
// ===========================================================================

CTermView::CTermView()
	{
	}

CTermView::~CTermView()
	{
	if (iSshActive && iShared)
		{
		iShared->quit = 1;
		for (TInt i = 0; i < 30 && iSshProcess.ExitType() == EExitPending; i++)
			User::After(100000);
		if (iSshProcess.ExitType() == EExitPending)
			iSshProcess.Kill(0);
		}
	delete iPump;
	delete iWatcher;
	delete iShotTimer;
	delete iPendingIdle;
	delete iTick;
	delete iReconnectTimer;
	delete iDebugText;
	if (iSshActive)
		iSshProcess.Close();
	if (iChunkOpen)
		iChunk.Close();
	delete iSerial;
	if (iVt)
		vterm_free(iVt);
	User::Free(iSbCells);
	User::Free(iSbCols);
	ReleaseFont();
	if (iFontFileLoaded)
		iCoeEnv->ScreenDevice()->RemoveFile(iFontFileId);
	CloseSTDLIB();
	}

void CTermView::ReleaseFont()
	{
	if (iFont)
		{
		iCoeEnv->ReleaseScreenFont(iFont);
		iFont = NULL;
		}
	}

void CTermView::ConstructL(const TRect& aRect, const TPsiSettings& aSettings)
	{
	iSettings = aSettings;
	iTabRow = -1;
	CreateBackedUpWindowL(iCoeEnv->RootWin(), EGray16);
	SetRectL(aRect);
	EnableDragEvents();                      // pen drag selects text

	// Terminal engine; real size is set by SetFontL
	iVt = vterm_new(24, 80);
	User::LeaveIfNull(iVt);
	vterm_set_utf8(iVt, 1);
	vterm_output_set_callback(iVt, &CTermView::CbOutput, this);
	iScreen = vterm_obtain_screen(iVt);
	Mem::FillZ(&iCallbacks, sizeof(iCallbacks));
	iCallbacks.damage = &CTermView::CbDamage;
	iCallbacks.moverect = &CTermView::CbMoveRect;
	iCallbacks.movecursor = &CTermView::CbMoveCursor;
	iCallbacks.settermprop = &CTermView::CbSetTermProp;
	iCallbacks.bell = &CTermView::CbBell;
	iCallbacks.sb_pushline = &CTermView::CbPushLine;
	iCallbacks.sb_clear = &CTermView::CbClearScrollback;
	// scrollback memory; without it PsiTerm still works, just with no history
	iSbCells = (TSbCell*)User::Alloc(KScrollbackLines * KScrollbackCols * sizeof(TSbCell));
	iSbCols = (short*)User::Alloc(KScrollbackLines * sizeof(short));
	if (!iSbCells || !iSbCols)
		{
		User::Free(iSbCells);
		User::Free(iSbCols);
		iSbCells = NULL;
		iSbCols = NULL;
		}
	SbInit(&iSb, iSbCells, iSbCols, KScrollbackLines, KScrollbackCols);
	vterm_screen_set_callbacks(iScreen, &iCallbacks, this);
	vterm_screen_enable_altscreen(iScreen, 1);
	// Let libvterm batch scrolling into window blits (CopyRect) instead of
	// asking us to repaint every scrolled line.
	vterm_screen_set_damage_merge(iScreen, VTERM_DAMAGE_SCROLL);
	VTermColor fg, bg;
	vterm_color_rgb(&fg, 0, 0, 0);
	vterm_color_rgb(&bg, 255, 255, 255);
	vterm_state_set_default_colors(vterm_obtain_state(iVt), &fg, &bg);
	vterm_screen_reset(iScreen, 1);
	iCurVisible = ETrue;

	// our bundled bitmap font; if it can't be loaded the Courier levels remain
	{
	TParse parse;
	parse.Set(CEikonEnv::Static()->EikAppUi()->Application()->AppFullName(), NULL, NULL);
	TFileName fontFile(parse.DriveAndPath());
	fontFile.Append(KFontFile);
	if (iCoeEnv->ScreenDevice()->AddFile(fontFile, iFontFileId) == KErrNone)
		iFontFileLoaded = ETrue;
	}
	SetFontL(iSettings.iZoom);
	ActivateL();
	DrawNow();

	iSerial = CSerialPort::NewL(*this);
	ShowWelcome();
	ApplySerialSettings();
	StartTick();
	}

void CTermView::ApplySerialSettings()
	{
	if (!iSerial)
		return;
	TInt r = iSerial->Open(iSettings.iBaudIndex, iSettings.iRtsCts);
	if (r != KErrNone)
		{
		TBuf8<160> msg;
		if (iSettings.iNetMode && r == KErrInUse)
			msg.Format(_L8("\r\n[PsiTerm: serial port in use - probably by the Psion's dial-up\r\n"
				" connection. It is freed when the dial-up hangs up.]\r\n"));
		else
			msg.Format(_L8("\r\n[PsiTerm: could not open serial port, error %d.\r\n"
				" Is Remote link switched off?]\r\n"), r);
		LocalMessage(msg);
		}
	}

void CTermView::ZoomBy(TInt aStep)
	{
	TInt z = iSettings.iZoom + aStep;
	if (z < 0) z = 0;
	if (z >= KZoomLevels) z = KZoomLevels - 1;
	if (z != iSettings.iZoom)
		SetFontL(z);
	}

void CTermView::SetFontL(TInt aZoom)
	{
	if (aZoom < 0 || aZoom >= KZoomLevels)
		aZoom = KDefaultZoom;
	iSettings.iZoom = aZoom;
	ReleaseFont();
	TInt pixels = KZoomPixels[aZoom];
	TInt twips = iCoeEnv->ScreenDevice()->VerticalPixelsToTwips(pixels);
	if (KZoomFace[aZoom] == 0)
		{
		TFontSpec spec(KFontTerminus, twips);
		iFont = iCoeEnv->CreateScreenFontL(spec);
		if (iFont->FontSpecInTwips().iTypeface.iName.CompareF(KFontTerminus) != 0)
			ReleaseFont();                  // not installed: use Courier instead
		}
	if (!iFont)
		{
		TFontSpec spec(KFontCourier, twips);
		iFont = iCoeEnv->CreateScreenFontL(spec);
		}

	// Cell = widest common glyph; every character is placed at its cell
	// position so even a proportional font lines up.
	iCellW = 1;
	const TText probe[] = { 'M', 'W', 'm', 'w', '@', '#', '0' };
	for (TUint i = 0; i < sizeof(probe) / sizeof(probe[0]); i++)
		{
		TInt w = iFont->CharWidthInPixels(probe[i]);
		if (w > iCellW)
			iCellW = w;
		}
	iCellH = iFont->HeightInPixels();
	if (iCellH < 6)
		iCellH = 6;
	iAscent = iFont->AscentInPixels();
	// Monospaced font (Courier is)? Then a run of cells can be drawn with a
	// single DrawText instead of one call per character.
	iMono = ETrue;
	for (TUint g = 32; g < 256 && iMono; g++)
		{
		if (g >= 127 && g < 160)
			continue;
		if (iFont->CharWidthInPixels((TChar)g) != iCellW)
			iMono = EFalse;
		}

	TSize size = Rect().Size();
	// the status line takes a slim strip at the bottom
	iStatusH = 0;
	if (iSettings.iStatus)
		{
		iStatusH = iEikonEnv->AnnotationFont()->HeightInPixels() + 4;
		size.iHeight -= iStatusH;
		}
	iCols = size.iWidth / iCellW;
	iRows = size.iHeight / iCellH;
	if (iCols > 160) iCols = 160;
	if (iRows > 60) iRows = 60;
	if (iCols < 20) iCols = 20;
	if (iRows < 5) iRows = 5;
	iOriginX = (size.iWidth - iCols * iCellW) / 2;
	iOriginY = (size.iHeight - iRows * iCellH) / 2;

	iScrollOffset = 0;                       // back to the live screen
	iSelActive = EFalse;
	iCacheValid = EFalse;
	vterm_set_size(iVt, iRows, iCols);
	vterm_screen_flush_damage(iScreen);
	if (iSshActive && iShared)
		{
		iShared->rows = iRows;
		iShared->cols = iCols;
		iShared->resized = 1;
		}
	iDamaged = EFalse;
	if (IsActivated())
		DrawNow();
	}

// ----- serial data in ------------------------------------------------------

static TInt LastFind(const TDesC8& aIn, const TDesC8& aWord)
	{
	TInt last = -1, from = 0;
	for (;;)
		{
		TInt i = aIn.Mid(from).Find(aWord);
		if (i < 0)
			return last;
		last = from + i;
		from = last + 1;
		}
	}

void CTermView::SerialDataL(const TDesC8& aData)
	{
	// follow the modem's own messages, so Hang up modem is only offered
	// (and the status line only says "Modem connected") when it is online
	iLastRx = User::TickCount();
	TBuf8<64> look(iRxTail);
	TInt room = look.MaxLength() - look.Length();
	look.Append(aData.Right(aData.Length() < room ? aData.Length() : room));
	TInt on = LastFind(look, _L8("CONNECT"));
	TInt off = LastFind(look, _L8("NO CARRIER"));
	if (on >= 0 || off >= 0)
		iModemOnline = (on > off);
	iRxTail = look.Right(look.Length() < 12 ? look.Length() : 12);
	FeedTerminal(aData.Ptr(), aData.Length());
	}

// Online as far as we can tell: CONNECT seen, or data still arriving
// (e.g. a connection opened some other way) within the last 10 seconds
TBool CTermView::ModemOnline() const
	{
	if (iSshActive)
		return EFalse;
	return iModemOnline || (iLastRx != 0 && User::TickCount() - iLastRx < 640);
	}

void CTermView::SerialError(TInt aError)
	{
	TBuf8<64> msg;
	msg.Format(_L8("\r\n[PsiTerm: serial error %d]\r\n"), aError);
	LocalMessage(msg);
	}

void CTermView::FeedTerminal(const TUint8* aBytes, TInt aLen)
	{
	TBool outer = (iPaintGc == NULL);
	if (outer)
		BeginPaint();
	vterm_input_write(iVt, (const char*)aBytes, aLen);
	if (outer)
		EndPaint();
	}

void CTermView::LocalMessage(const TDesC8& aText)
	{
	if (iCapture)
		AppendDebug(aText);
	else
		FeedTerminal(aText.Ptr(), aText.Length());
	}

// ----- Debug screen ----------------------------------------------------------

void CTermView::BeginDebugL(const TDesC& aTitle)
	{
	delete iDebugText;
	iDebugText = NULL;
	iDebugText = HBufC8::NewL(8000);
	iDebugTitle = aTitle.Left(aTitle.Length() < iDebugTitle.MaxLength() ? aTitle.Length() : iDebugTitle.MaxLength());
	iCapture = ETrue;
	}

void CTermView::AppendDebug(const TDesC8& aText)
	{
	if (!iDebugText)
		return;
	TPtr8 p = iDebugText->Des();
	TInt room = p.MaxLength() - p.Length();
	p.Append(aText.Left(aText.Length() < room ? aText.Length() : room));
	if (iToolDlg)
		TRAP_IGNORE(iToolDlg->RefreshL());
	}

// Terminal output -> editor text: CR goes back to the start of the line
// (progress counters), LF starts a paragraph, escape sequences are dropped.
HBufC* CTermView::DebugTextLC() const
	{
	TInt len = iDebugText ? iDebugText->Length() : 0;
	HBufC* text = HBufC::NewLC(len + 1);
	if (!iDebugText)
		return text;
	TPtr t = text->Des();
	const TDesC8& in = *iDebugText;
	TInt lineStart = 0;
	for (TInt i = 0; i < in.Length(); i++)
		{
		TUint c = in[i];
		if (c == '\r')
			{
			if (i + 1 < in.Length() && in[i + 1] == '\n')
				continue;
			if (i + 1 < in.Length())       // a lone CR at the very end: keep the line
				t.SetLength(lineStart);
			}
		else if (c == '\n')
			{
			if (t.Length() > 0)
				{
				t.Append(CEditableText::EParagraphDelimiter);
				lineStart = t.Length();
				}
			}
		else if (c == 0x1b)
			{
			if (i + 1 < in.Length() && in[i + 1] == '[')
				{
				i += 2;
				while (i < in.Length() && (in[i] < 0x40 || in[i] > 0x7e))
					i++;
				}
			}
		else if (c >= 0x20)
			t.Append((TText)c);
		}
	while (t.Length() > 0 && t[t.Length() - 1] == CEditableText::EParagraphDelimiter)
		t.SetLength(t.Length() - 1);
	return text;
	}

// Shows the window for the current job: live while psissh runs (Stop), or
// straight away with the result if it has already finished (Close).
void CTermView::RunToolDialogL()
	{
	if (!iCapture)
		return;
	CToolDialog* dlg = new(ELeave) CToolDialog(*this, iDebugTitle, !iSshActive);
	iToolDlg = dlg;
	TRAPD(err, dlg->ExecuteLD(R_PT_TOOL_DIALOG));
	iToolDlg = NULL;
	iCapture = EFalse;
	delete iDebugText;
	iDebugText = NULL;
	User::LeaveIfError(err);
	if (iInstallPending)
		{
		iInstallPending = EFalse;
		StartInstallerL();
		}
	}

void CTermView::StopTool()
	{
	if (iSshActive && iShared)
		{
		iShared->quit = 1;
		AppendDebug(_L8("\r\nStopping...\r\n"));
		}
	}

// A newer PsiTerm.sis was downloaded and its signature checked: open it with
// the system installer (InstApp, UID 0x10000419 - a .sis carries PsiTerm's
// own UID in its header, so a plain "open document" would start PsiTerm on
// it) and close, so the installer can replace this app's files.
void CTermView::StartInstallerL()
	{
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

void CTermView::RunAfterDisconnectL(TInt aCommand)
	{
	iPendingCmd = aCommand;
	DisconnectSsh();
	}

TInt CTermView::PendingCallback(TAny* aSelf)
	{
	CTermView* self = (CTermView*)aSelf;
	TInt cmd = self->iPendingCmd;
	self->iPendingCmd = 0;
	if (cmd)
		{
		TRAPD(err, CEikonEnv::Static()->EikAppUi()->HandleCommandL(cmd));
		// PsiTerm closing (e.g. to let the installer run after an update
		// that first had to end the SSH session) leaves with KLeaveExit:
		// that must reach EIKON, not be swallowed here
		if (err == KLeaveExit)
			User::Leave(err);
		}
	return 0;
	}

// ----- libvterm callbacks --------------------------------------------------

int CTermView::CbDamage(VTermRect aRect, void* aUser)
	{
	CTermView* self = (CTermView*)aUser;
	if (self->iScrollOffset > 0)
		{
		self->iNeedFull = ETrue;          // view shows history: redraw at the end
		return 1;
		}
	// libvterm's screen matches this rect right now, so paint it now if we
	// can: a later scroll blit must move up-to-date pixels.
	if (self->iPaintGc)
		self->DrawCells(*self->iPaintGc, aRect.start_row, aRect.start_col, aRect.end_row, aRect.end_col);
	else
		self->AddDamage(aRect.start_row, aRect.start_col, aRect.end_row, aRect.end_col);
	return 1;
	}

// Part of the screen moved (scrolling). Blit the pixels already on screen;
// libvterm then reports only the newly exposed lines as damage.
int CTermView::CbMoveRect(VTermRect aDest, VTermRect aSrc, void* aUser)
	{
	CTermView* self = (CTermView*)aUser;
	if (self->iScrollOffset > 0)
		{
		self->iNeedFull = ETrue;
		return 1;
		}
	if (!self->iPaintGc)
		return 0;                     // not painting: libvterm marks it damaged
	TInt w = self->iCellW, h = self->iCellH;
	TRect src(TPoint(self->iOriginX + aSrc.start_col * w, self->iOriginY + aSrc.start_row * h),
		TPoint(self->iOriginX + aSrc.end_col * w, self->iOriginY + aSrc.end_row * h));
	TPoint offset((aDest.start_col - aSrc.start_col) * w, (aDest.start_row - aSrc.start_row) * h);
	self->iPaintGc->CopyRect(offset, src);
	return 1;
	}

// A line scrolled off the top of the screen: keep it in the history.
int CTermView::CbPushLine(int aCols, const VTermScreenCell* aCells, void* aUser)
	{
	CTermView* self = (CTermView*)aUser;
	TSbCell line[KScrollbackCols];
	if (aCols > KScrollbackCols)
		aCols = KScrollbackCols;
	for (TInt i = 0; i < aCols; i++)
		{
		const VTermScreenCell& c = aCells[i];
		TInt fg, bg;
		self->CellColours(c, fg, bg);
		TUint ch = c.chars[0];
		line[i].iCh = (ch == (TUint)-1 || ch > 0xFFFF) ? 0xFFFF : (unsigned short)ch;
		line[i].iGrey = (unsigned char)((fg << 4) | bg);
		line[i].iFlags = (unsigned char)((c.attrs.bold ? KSbBold : 0) | (c.attrs.underline ? KSbUnderline : 0)
			| (c.attrs.strike ? KSbStrike : 0) | (c.width > 1 ? KSbWide : 0));
		}
	SbPush(&self->iSb, line, aCols);
	self->iLinesPushed++;
	if (self->iScrollOffset > 0)
		{
		// keep the history the user is reading still on the screen
		if (self->iScrollOffset < self->iSb.iCount)
			self->iScrollOffset++;
		self->iNeedFull = ETrue;
		}
	return 1;
	}

int CTermView::CbClearScrollback(void* aUser)
	{
	CTermView* self = (CTermView*)aUser;
	SbClear(&self->iSb);
	if (self->iScrollOffset)
		{
		self->iScrollOffset = 0;
		self->iNeedFull = ETrue;
		}
	return 1;
	}

int CTermView::CbMoveCursor(VTermPos aPos, VTermPos /*aOldPos*/, int aVisible, void* aUser)
	{
	CTermView* self = (CTermView*)aUser;
	self->iCurRow = aPos.row;
	self->iCurCol = aPos.col;
	self->iCurVisible = aVisible;
	return 1;
	}

int CTermView::CbSetTermProp(VTermProp aProp, VTermValue* aVal, void* aUser)
	{
	CTermView* self = (CTermView*)aUser;
	if (aProp == VTERM_PROP_CURSORVISIBLE)
		self->iCurVisible = aVal->boolean;
	else if (aProp == VTERM_PROP_TITLE)
		{
		// tmux can send its window list as the title (tmux > Set up tabs):
		// "PSITABS 0:bash- 1:vim* "
		const VTermStringFragment& f = aVal->string;
		if (f.initial)
			self->iTitle.Zero();
		TInt room = self->iTitle.MaxLength() - self->iTitle.Length();
		self->iTitle.Append(TPtrC8((const TUint8*)f.str, (TInt)f.len < room ? (TInt)f.len : room));
		if (f.final)
			{
			self->iTitleTabCount = 0;
			if (self->iTitle.Length() >= 8 && self->iTitle.Left(8) == _L8("PSITABS "))
				{
				TBuf<240> t;
				t.Copy(self->iTitle.Mid(8));
				TInt cur = 0;
				self->iTitleTabCount = ParseTabList(t, self->iTitleTabs, cur);
				}
			}
		}
	else if (aProp == VTERM_PROP_MOUSE)
		self->iMouseMode = aVal->number;    // e.g. tmux with "set -g mouse on"
	return 1;
	}

int CTermView::CbBell(void* aUser)
	{
	CTermView* self = (CTermView*)aUser;
	// only for a live SSH session (not stray modem data), only if wanted,
	// and at most a few a second so a burst of bells is one beep
	if (!self->iSshActive || self->iSettings.iBell)
		return 1;
	TUint now = User::TickCount();
	if (now - self->iLastBell < 16)          // ticks are 1/64 s
		return 1;
	self->iLastBell = now;
	CEikonEnv::Beep();
	return 1;
	}

void CTermView::CbOutput(const char* aBytes, size_t aLen, void* aUser)
	{
	CTermView* self = (CTermView*)aUser;
	self->WriteToHost(TPtrC8((const TUint8*)aBytes, aLen));
	}

// Bytes typed by the user go to the SSH process when a session is running,
// otherwise straight down the serial line.
void CTermView::WriteToHost(const TDesC8& aBytes)
	{
	if (iSshActive && iShared)
		{
		for (TInt i = 0; i < aBytes.Length(); i++)
			{
			TInt guard = 0;
			while (iShared->kbd_head - iShared->kbd_tail >= PSI_KBD_SIZE && guard++ < 200)
				User::After(5000);
			iShared->kbd[iShared->kbd_head % PSI_KBD_SIZE] = aBytes[i];
			iShared->kbd_head++;
			}
		return;
		}
	if (iSerial)
		iSerial->Write(aBytes);
	}

// ----- drawing -------------------------------------------------------------

void CTermView::AddDamage(TInt aRow0, TInt aCol0, TInt aRow1, TInt aCol1)
	{
	if (!iDamaged)
		{
		iDmgRow0 = aRow0; iDmgCol0 = aCol0; iDmgRow1 = aRow1; iDmgCol1 = aCol1;
		iDamaged = ETrue;
		return;
		}
	if (aRow0 < iDmgRow0) iDmgRow0 = aRow0;
	if (aCol0 < iDmgCol0) iDmgCol0 = aCol0;
	if (aRow1 > iDmgRow1) iDmgRow1 = aRow1;
	if (aCol1 > iDmgCol1) iDmgCol1 = aCol1;
	}

// All screen updates from terminal output happen between BeginPaint and
// EndPaint, with the old cursor removed so scroll blits never move it.
void CTermView::BeginPaint()
	{
	ActivateGc();
	iPaintGc = &SystemGc();
	iPaintGc->UseFont(iFont);
	if (iDrawnCurVisible && iDrawnCurRow < iRows && iDrawnCurCol < iCols)
		DrawOneCell(*iPaintGc, iDrawnCurRow, iDrawnCurCol);
	iDrawnCurVisible = EFalse;
	if (iDamaged)
		{
		iDamaged = EFalse;
		DrawCells(*iPaintGc, iDmgRow0, iDmgCol0, iDmgRow1, iDmgCol1);
		}
	}

void CTermView::EndPaint()
	{
	vterm_screen_flush_damage(iScreen);    // pending scroll blit + damage
	if (iNeedFull)
		{
		iNeedFull = EFalse;
		DrawAll(*iPaintGc);
		}
	DrawCursor(*iPaintGc);
	iPaintGc->DiscardFont();
	iPaintGc = NULL;
	DeactivateGc();
	}

void CTermView::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	gc.UseFont(iFont);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(Grey(15));
	gc.DrawRect(Rect());
	CONST_CAST(CTermView*, this)->iDamaged = EFalse;
	DrawAll(gc);
	DrawCursor(gc);
	DrawStatus(gc);
	gc.UseFont(iFont);
	}

// ----- status line --------------------------------------------------------------
// Connection state on the left, the time on the right. aSplit = where the
// right-hand part starts in aText.
void CTermView::StatusText(TDes& aText, TInt& aSplit) const
	{
	aText.Zero();
	TBuf<40> host;
	host.Copy(LeftSafe(iSettings.iSshHost, 30));
	if (iReconnectWait)
		{
		TTime now;
		now.HomeTime();
		TTimeIntervalSeconds left(0);
		if (iReconnectAt.SecondsFrom(now, left) != KErrNone || left.Int() < 0)
			left = 0;
		aText.Format(_L("Connection lost - reconnecting in %ds   Enter: now  Esc: stop"), left.Int());
		}
	else if (iSshActive)
		{
		TInt state = iShared ? iShared->state : PSI_STATE_STARTING;
		if (iLaunchMode == 1) aText.Append(_L("Running the speed test..."));
		else if (iLaunchMode == 2) aText.Append(_L("Updating PsiTerm..."));
		else if (iLaunchMode == 3) aText.Append(_L("Sending screenshots..."));
		else if (iLaunchMode == 4) aText.Append(_L("Making an SSH key..."));
		else if (iLaunchMode == 5) aText.Append(_L("Importing an SSH key..."));
		else if (state == PSI_STATE_DIALING || state == PSI_STATE_KEYEX || state == PSI_STATE_AUTH)
			{
			// something that moves, so a slow 36 MHz handshake never looks stuck
			static const TText KSpin[] = { '|', '/', '-', '\\' };
			TInt secs = (TInt)((User::TickCount() - iStateSince) / 64);
			if (state == PSI_STATE_DIALING)
				aText.Format(_L("Dialling %S"), &host);
			else if (state == PSI_STATE_AUTH)
				{
				TBuf<24> user;
				user.Copy(LeftSafe(iSettings.iSshUser, 20));
				aText.Format(_L("Logging in as %S"), &user);
				}
			else
				{
				static const TText* const KSteps[] = {
					_S("Saying hello to the server"), _S("Agreeing on a cipher"),
					_S("Doing the X25519 maths"), _S("Checking the server's identity"),
					_S("Working out the session keys"), _S("Nearly there") };
				TInt step = secs / 3;
				if (step > 5) step = 5;
				aText.Append(TPtrC(KSteps[step]));
				}
			aText.AppendFormat(_L("...  %c  %ds   "), KSpin[iTickCount & 3], secs);
			}
		else if (state == PSI_STATE_CONNECTED)
			{
			TBuf<24> user;
			user.Copy(LeftSafe(iSettings.iSshUser, 20));
			aText.Format(_L("\x95 %S@%S   SSH"), &user, &host);
			}
		else aText.Append(_L("Starting SSH..."));
		}
	else if (ModemOnline())
		aText.Format(_L("Modem connected   %d baud   Shift+Ctrl+H: hang up"), BaudValue(iSettings.iBaudIndex));
	else
		aText.Format(_L("Not connected   %d baud%S"), BaudValue(iSettings.iBaudIndex),
			iSettings.iNetMode ? &KPppSuffix : &KNullDesC);
	aSplit = aText.Length();
	TTime now;
	now.HomeTime();
	TDateTime t = now.DateTime();
	aText.AppendFormat(_L("%02d:%02d"), t.Hour(), t.Minute());
	}

void CTermView::DrawStatus(CWindowGc& aGc) const
	{
	if (!iStatusH)
		return;
	TBuf<120> text;
	TInt split;
	StatusText(text, split);
	CONST_CAST(CTermView*, this)->iStatusDrawn = text;
	const CFont* font = iEikonEnv->AnnotationFont();
	TRect bar(Rect().iTl.iX, Rect().iBr.iY - iStatusH, Rect().iBr.iX, Rect().iBr.iY);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(Grey(4));
	aGc.DrawRect(bar);
	aGc.UseFont(font);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(Grey(15));
	TInt y = bar.iTl.iY + 2 + font->AscentInPixels();
	TPtrC left(text.Left(split));
	TPtrC right(text.Mid(split));
	aGc.DrawText(left, TPoint(bar.iTl.iX + 6, y));
	aGc.DrawText(right, TPoint(bar.iBr.iX - 6 - font->TextWidthInPixels(right), y));
	aGc.DiscardFont();
	}

// Every 0.5 s: blink the cursor, and refresh the status line if it changed
TInt CTermView::TickCallback(TAny* aSelf)
	{
	((CTermView*)aSelf)->Tick();
	return 1;
	}

void CTermView::Tick()
	{
	if (!IsActivated() || iPaintGc || iCapture)
		return;
	if (iSettings.iBlink && iCurVisible && iScrollOffset == 0)
		{
		iBlinkHidden = !iBlinkHidden;
		BeginPaint();                  // erases the old cursor cell, EndPaint redraws it
		EndPaint();
		}
	else if (iBlinkHidden)
		{
		iBlinkHidden = EFalse;
		BeginPaint();
		EndPaint();
		}
	iTickCount++;
	TInt state = (iSshActive && iShared) ? iShared->state : -1;
	if (state != iLastState)
		{
		iLastState = state;
		iStateSince = User::TickCount();
		}
	if (SshLoggedIn())
		iEverLoggedIn = ETrue;
	ParseTmuxTabs();
	if (iStatusH)
		{
		TBuf<120> text;
		TInt split;
		StatusText(text, split);
		if (text != iStatusDrawn)
			{
			ActivateGc();
			DrawStatus(SystemGc());
			DeactivateGc();
			}
		}
	}

void CTermView::StartTick()
	{
	if (!iTick)
		iTick = CPeriodic::New(CActive::EPriorityLow);
	if (!iTick)
		return;
	iTick->Cancel();
	iBlinkHidden = EFalse;
	// always: the status line, the cursor blink and the tmux tabs
	iTick->Start(500000, 500000, TCallBack(TickCallback, this));
	}

void CTermView::ApplyAppearanceL()
	{
	iBlinkHidden = EFalse;
	iCacheValid = EFalse;
	SetFontL(iSettings.iZoom);      // lays out again (status line on/off) and redraws
	StartTick();
	// start screen switched on or off while not connected: show / drop it now
	if (!iSshActive && !iReconnectWait)
		{
		if (iSettings.iStartScreen && !iWelcome)
			ShowWelcome();
		else if (!iSettings.iStartScreen)
			iWelcome = EFalse;
		}
	}

// ----- tmux windows as tabs ------------------------------------------------------
// tmux's default status line reads "[session] 0:bash* 1:vim- 2:top  "host" 12:00".
// When PsiTerm sees one (bottom row, or top) during an SSH session it draws the
// window list as tabs over it: tap a tab to go to that window, Ctrl+Tab and
// Shift+Ctrl+Tab for the next / previous one.

static TBool IsTmuxFlag(TUint aCh)
	{
	return aCh == '*' || aCh == '-' || aCh == '#' || aCh == '!' || aCh == '~';
	}

// Reads "N:name" window entries out of tmux's status line (or a PSITABS
// title), whatever decorates them: "[psion] 0:bash- 1:vim*" (the default),
// "SESSION:main < 3:VirtualTeam > host  22:41" (themes). Times, dates and
// other text are skipped. Returns the count; aCurrent = entries marked *.
TInt CTermView::ParseTabList(const TDesC& aText, TTmuxTab* aTabs, TInt& aCurrent)
	{
	TBuf<240> line(LeftSafe(aText, 240));
	// "(B" is left behind by a stray tput sgr0 in some status formats
	for (TInt k = line.Find(_L("(B")); k >= 0; k = line.Find(_L("(B")))
		line.Replace(k, 2, _L("  "));
	TInt n = 0, i = 0;
	aCurrent = 0;
	while (i < line.Length() && n < KMaxTabs)
		{
		while (i < line.Length() && line[i] == ' ')
			i++;
		TInt s = i;
		while (i < line.Length() && line[i] != ' ')
			i++;
		TInt e = i;
		// decorations around an entry
		while (s < e && (line[s] == '<' || line[s] == '>' || line[s] == '[' || line[s] == '(' || line[s] == '|'))
			s++;
		while (e > s && (line[e - 1] == '<' || line[e - 1] == '>' || line[e - 1] == ']' || line[e - 1] == ')' || line[e - 1] == '|'))
			e--;
		TInt d = s, idx = 0;
		while (d < e && line[d] >= '0' && line[d] <= '9')
			idx = idx * 10 + (line[d++] - '0');
		if (d == s || d - s > 3 || d >= e || line[d] != ':')
			continue;                         // not "N:..."
		TInt nameStart = d + 1, end = e;
		if (nameStart >= end || (line[nameStart] >= '0' && line[nameStart] <= '9'))
			continue;                         // a time such as 22:41
		TBool cur = EFalse;
		while (end > nameStart && (IsTmuxFlag(line[end - 1])
			|| ((line[end - 1] == 'Z' || line[end - 1] == 'M') && end - 2 >= nameStart
				&& (IsTmuxFlag(line[end - 2]) || line[end - 2] == 'M'))))
			{
			if (line[end - 1] == '*')
				cur = ETrue;
			end--;
			}
		if (end <= nameStart || line.Mid(nameStart, end - nameStart).Locate(':') >= 0)
			continue;
		aTabs[n].iIndex = idx;
		aTabs[n].iName.Copy(line.Mid(nameStart, end - nameStart > 20 ? 20 : end - nameStart));
		aTabs[n].iCurrent = cur;
		aTabs[n].iX0 = aTabs[n].iX1 = 0;
		if (cur)
			aCurrent++;
		n++;
		}
	return n;
	}

// Is screen row aRow coloured like a status bar (most cells not on the
// default background, or reversed)?
TBool CTermView::RowIsBar(TInt aRow) const
	{
	TInt coloured = 0;
	for (TInt c = 0; c < iCols; c++)
		{
		VTermPos pos;
		pos.row = aRow;
		pos.col = c;
		VTermScreenCell cell;
		if (vterm_screen_get_cell(iScreen, pos, &cell)
			&& (!VTERM_COLOR_IS_DEFAULT_BG(&cell.bg) || cell.attrs.reverse))
			coloured++;
		}
	return coloured * 10 >= iCols * 6;
	}

void CTermView::ParseTmuxTabs()
	{
	TTmuxTab tabs[KMaxTabs];
	TInt count = 0;
	TInt row = -1;
	if (SshLoggedIn() && iScreen)
		{
		for (TInt pass = 0; pass < 2 && row < 0; pass++)
			{
			TInt r = pass == 0 ? iRows - 1 : 0;
			TBool bar = RowIsBar(r);
			if (iTitleTabCount > 0)
				{
				// tmux sends the list itself: just find its status bar
				if (bar || (pass == 1 && row < 0))
					{
					row = bar ? r : iRows - 1;
					count = iTitleTabCount;
					for (TInt t = 0; t < count; t++)
						tabs[t] = iTitleTabs[t];
					}
				continue;
				}
			if (!bar)
				continue;
			TBuf<200> line;
			for (TInt c = 0; c < iCols && c < line.MaxLength(); c++)
				{
				VTermPos pos;
				pos.row = r;
				pos.col = c;
				VTermScreenCell cell;
				TUint ch = vterm_screen_get_cell(iScreen, pos, &cell) ? cell.chars[0] : 0;
				line.Append((TText)(ch >= 0x20 && ch < 0x7f ? ch : (ch == 0 ? ' ' : '?')));
				}
			TInt current = 0;
			TInt n = ParseTabList(line, tabs, current);
			if (n >= 1 && (current == 1 || (current == 0 && n == 1)))
				{
				if (current == 0)
					tabs[0].iCurrent = ETrue;     // a theme showing only this window
				row = r;
				count = n;
				}
			}
		}
	TBuf<200> sig;
	sig.AppendNum(row);
	for (TInt t = 0; t < count && sig.Length() < 170; t++)
		{
		sig.Append(tabs[t].iCurrent ? '*' : ' ');
		sig.AppendNum(tabs[t].iIndex);
		sig.Append(LeftSafe(tabs[t].iName, 8));
		}
	TBool drawn = iSettings.iTmuxTabs && row >= 0;
	if (sig == iTabSig && drawn == iTabsDrawn)
		return;
	TInt oldRow = iTabsDrawn ? iTabRow : -1;
	iTabSig = sig;
	iTabRow = row;
	iTabCount = count;
	for (TInt t = 0; t < count; t++)
		iTabs[t] = tabs[t];
	iTabsDrawn = drawn;
	if (iPaintGc || iScrollOffset > 0)
		return;
	if (oldRow >= 0 && oldRow != row)
		RepaintRows(oldRow, oldRow);
	if (drawn)
		RepaintRows(row, row);
	}

void CTermView::DrawTabs(CWindowGc& aGc) const
	{
	TInt y0 = iOriginY + iTabRow * iCellH;
	TRect bar(Rect().iTl.iX, y0, Rect().iBr.iX, y0 + iCellH);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(Grey(11));
	aGc.DrawRect(bar);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	TInt x = bar.iTl.iX + 2;
	CTermView* self = CONST_CAST(CTermView*, this);
	for (TInt i = 0; i < iTabCount; i++)
		{
		TBuf<28> label;
		label.AppendNum(iTabs[i].iIndex);
		label.Append(' ');
		label.Append(iTabs[i].iName);
		TInt w = iFont->TextWidthInPixels(label) + 12;
		if (x + w > bar.iBr.iX - 2)
			w = bar.iBr.iX - 2 - x;
		if (w < 16)
			break;
		TRect t(x, y0 + 1, x + w, y0 + iCellH);
		if (iTabs[i].iCurrent)
			{
			aGc.SetBrushColor(Grey(0));
			aGc.SetPenColor(Grey(15));
			}
		else
			{
			aGc.SetBrushColor(Grey(14));
			aGc.SetPenColor(Grey(0));
			}
		aGc.DrawText(label, t, iAscent - 1, CGraphicsContext::ELeft, 6);
		self->iTabs[i].iX0 = x;
		self->iTabs[i].iX1 = x + w;
		x += w + 3;
		}
	for (TInt j = 0; j < iTabCount; j++)       // tabs that did not fit: not tappable
		if (iTabs[j].iX1 > bar.iBr.iX || iTabs[j].iX0 >= x)
			self->iTabs[j].iX0 = self->iTabs[j].iX1 = 0;
	}

void CTermView::CheckTabsL()
	{
	BeginDebugL(_L("tmux tabs"));
	TBuf8<260> m;
	m.Format(_L8("Logged in: %s   Tabs setting: %s   Terminal %dx%d\r\n"),
		SshLoggedIn() ? "yes" : "no", iSettings.iTmuxTabs ? "on" : "off", iCols, iRows);
	LocalMessage(m);
	for (TInt pass = 0; pass < 2; pass++)
		{
		TInt r = pass == 0 ? iRows - 1 : 0;
		m.Format(_L8("\r\nRow %d:\r\n"), r + 1);
		for (TInt c = 0; c < iCols && m.Length() < 250; c++)
			{
			VTermPos pos;
			pos.row = r;
			pos.col = c;
			VTermScreenCell cell;
			TUint ch = vterm_screen_get_cell(iScreen, pos, &cell) ? cell.chars[0] : 0;
			m.Append((TUint8)(ch >= 0x20 && ch < 0x7f ? ch : (ch == 0 ? ' ' : '?')));
			}
		m.Append(_L8("\r\n"));
		LocalMessage(m);
		}
	m.Format(_L8("\r\nFound: row %d, %d window(s):"), iTabRow + 1, iTabCount);
	for (TInt t = 0; t < iTabCount && m.Length() < 220; t++)
		{
		m.Append(' ');
		m.AppendNum(iTabs[t].iIndex);
		m.Append(':');
		TBuf8<20> n;
		n.Copy(iTabs[t].iName);
		m.Append(n);
		if (iTabs[t].iCurrent)
			m.Append('*');
		}
	m.Append(_L8("\r\n"));
	LocalMessage(m);
	ShowDebugL();
	}

void CTermView::SelectTmuxWindow(TInt aIndex)
	{
	SendCtrl(iSettings.iTmuxPrefix ? 'A' : 'B');
	if (aIndex >= 0 && aIndex <= 9)
		SendChar('0' + aIndex);
	else
		{
		TBuf8<32> cmd;
		cmd.Format(_L8(":select-window -t :%d\r"), aIndex);
		SendString(cmd);
		}
	}

void CTermView::TmuxNextWindow(TBool aBack)
	{
	SendCtrl(iSettings.iTmuxPrefix ? 'A' : 'B');
	SendChar(aBack ? 'p' : 'n');
	}

// ----- welcome screen -------------------------------------------------------------
// Written into the terminal as text, so it scrolls away like anything else.
// While it is showing, 1-9 connect to the saved hosts.
static void AppendUtf8(TDes8& aOut, TUint aCh)
	{
	if (aCh < 0x80)
		aOut.Append((TUint8)aCh);
	else if (aCh < 0x800)
		{
		aOut.Append((TUint8)(0xC0 | (aCh >> 6)));
		aOut.Append((TUint8)(0x80 | (aCh & 0x3F)));
		}
	else
		{
		aOut.Append((TUint8)(0xE0 | (aCh >> 12)));
		aOut.Append((TUint8)(0x80 | ((aCh >> 6) & 0x3F)));
		aOut.Append((TUint8)(0x80 | (aCh & 0x3F)));
		}
	}

static void AppendText(TDes8& aOut, const TDesC& aText, TInt aWidth)
	{
	TInt n = 0;
	for (TInt i = 0; i < aText.Length() && n < aWidth; i++, n++)
		AppendUtf8(aOut, PsiCodePageToUnicode(aText[i]));
	for (; n < aWidth; n++)
		aOut.Append(' ');
	}

void CTermView::ShowWelcome()
	{
	iWelcome = EFalse;
	if (!iSettings.iStartScreen)
		return;
	TInt width = iCols - 4;
	if (width > 56) width = 56;
	HBufC8* buf = HBufC8::New(2600);
	if (!buf)
		return;
	TPtr8 w = buf->Des();
	// a clean screen: scroll what was there up into the scrollback (so a
	// session's last messages stay a Shift+PgUp away), then go home
	w.Append(_L8("\x1b[999;1H"));
	for (TInt r = 0; r < iRows && r < 60; r++)
		w.Append(_L8("\r\n"));
	w.Append(_L8("\x1b[H\x1b[J\r\n  \x1b[7m PsiTerm "));
	TBuf<8> ver(KPsiTermVersion);
	AppendText(w, ver, ver.Length());
	w.Append(_L8(" \x1b[0m  SSH for the Psion Series 5mx\r\n  "));
	for (TInt i = 0; i < width; i++)
		AppendUtf8(w, 0x2500);
	w.Append(_L8("\r\n"));
	TInt count = iHosts ? iHosts->Count() : 0;
	if (count == 0)
		w.Append(_L8("  No saved servers yet: Shift+Ctrl+S to add one.\r\n"));
	else
		{
		for (TInt i = 0; i < count && i < 9; i++)
			{
			const THostEntry& e = iHosts->At(i);
			w.Append(_L8("   \x1b[1m"));
			w.Append((TUint8)('1' + i));
			w.Append(_L8("\x1b[0m  "));
			AppendText(w, e.iName, 14);
			w.Append(_L8(" \x1b[90m"));
			TBuf<80> where(e.iUser);
			where.Append('@');
			where.Append(LeftSafe(e.iHost, 60));
			AppendText(w, where, width - 20 > 10 ? width - 20 : 10);
			w.Append(_L8("\x1b[0m\r\n"));
			}
		}
	w.Append(_L8("  "));
	for (TInt i = 0; i < width; i++)
		AppendUtf8(w, 0x2500);
	w.Append(_L8("\r\n  \x1b[90m"));
	if (count)
		w.Append(_L8("1-9 connect   "));
	w.Append(_L8("Shift+Ctrl+S servers   Menu: everything else\x1b[0m\r\n\r\n"));
	LocalMessage(w);
	delete buf;
	iWelcome = (count > 0);
	}

void CTermView::DrawAll(CWindowGc& aGc) const
	{
	DrawCells(aGc, 0, 0, iRows, iCols);
	DrawScrollIndicator(aGc);
	}

// While looking at history, show where we are in the top-right corner.
void CTermView::DrawScrollIndicator(CWindowGc& aGc) const
	{
	if (iScrollOffset <= 0)
		return;
	TBuf<40> text;
	text.Format(_L(" history -%d/%d "), iScrollOffset, iSb.iCount);
	TInt w = iFont->TextWidthInPixels(text);
	TInt x = Rect().iBr.iX - w - 2;
	TRect box(TPoint(x, iOriginY), TSize(w, iCellH));
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(Grey(0));
	aGc.DrawRect(box);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(Grey(15));
	aGc.DrawText(text, TPoint(x, iOriginY + iAscent));
	}

// Draws a rectangle of cells. Runs of ordinary text that share colours and
// attributes are drawn with one background fill and one DrawText, which is
// many times faster on the Psion than a fill + text per character.
void CTermView::DrawCells(CWindowGc& aGc, TInt aRow0, TInt aCol0, TInt aRow1, TInt aCol1) const
	{
	if (aRow0 < 0) aRow0 = 0;
	if (aCol0 < 0) aCol0 = 0;
	if (aRow1 > iRows) aRow1 = iRows;
	if (aCol1 > iCols) aCol1 = iCols;
	TBuf<160> run;
	const TUint KRunFlags = KSbBold | KSbUnderline | KSbStrike;
	for (TInt r = aRow0; r < aRow1; r++)
		{
		if (iTabsDrawn && r == iTabRow && iScrollOffset == 0)
			{
			DrawTabs(aGc);                // tmux's status line, drawn as tabs
			continue;
			}
		TInt c = aCol0;
		TLook look;
		TInt byte = -1;
		TBool have = EFalse;          // look/byte already fetched for column c
		while (c < aCol1)
			{
			if (!have)
				{
				if (!GetLook(r, c, look))
					{
					c++;                  // outside the screen: nothing to draw
					continue;
					}
				byte = TextByte(look);
				}
			have = EFalse;
			if (byte < 0)
				{
				DrawOneCell(aGc, r, c, look);  // graphics, wide chars, etc.
				c++;
				continue;
				}
			TInt fg = look.iFg, bg = look.iBg;
			TUint flags = look.iFlags & KRunFlags;
			TBool ink = (byte != ' ');
			TInt start = c;
			run.Zero();
			run.Append((TText)byte);
			for (c++; c < aCol1; c++)
				{
				TLook next;
				if (!GetLook(r, c, next))
					break;
				TInt b = TextByte(next);
				if (b < 0 || next.iFg != fg || next.iBg != bg || (next.iFlags & KRunFlags) != flags)
					{
					look = next;              // becomes the next run's first cell
					byte = b;
					have = ETrue;
					break;
					}
				run.Append((TText)b);
				if (b != ' ')
					ink = ETrue;
				}
			TBool bold = (flags & KSbBold) != 0 || iSettings.iBold;
			TBool ul = (flags & KSbUnderline) != 0;
			TBool st = (flags & KSbStrike) != 0;
			TInt x0 = iOriginX + start * iCellW;
			TInt y0 = iOriginY + r * iCellH;
			TInt x1 = x0 + run.Length() * iCellW;
			aGc.SetPenStyle(CGraphicsContext::ENullPen);
			aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
			TInt tf, tb;
			ThemePair(fg, bg, tf, tb);
			aGc.SetBrushColor(TRgb::Gray16(tb));
			aGc.DrawRect(TRect(x0, y0, x1, y0 + iCellH));
			if (!ink && !ul && !st)
				continue;
			aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
			aGc.SetPenStyle(CGraphicsContext::ESolidPen);
			aGc.SetPenColor(TRgb::Gray16(tf));
			if (ink)
				{
				TPoint base(x0, y0 + iAscent);
				if (iMono)
					{
					aGc.DrawText(run, base);
					if (bold)
						aGc.DrawText(run, base + TPoint(1, 0));
					}
				else
					{
					for (TInt i = 0; i < run.Length(); i++)
						{
						if (run[i] == ' ')
							continue;
						TPtrC one(run.Mid(i, 1));
						TPoint at(x0 + i * iCellW, base.iY);
						aGc.DrawText(one, at);
						if (bold)
							aGc.DrawText(one, at + TPoint(1, 0));
						}
					}
				}
			if (ul)
				aGc.DrawLine(TPoint(x0, y0 + iCellH - 1), TPoint(x1, y0 + iCellH - 1));
			if (st)
				aGc.DrawLine(TPoint(x0, y0 + iCellH / 2), TPoint(x1, y0 + iCellH / 2));
			}
		}
	}

// One cell of the view: the live screen, or a scrollback line when the user
// has scrolled back. Selected cells come back with fg/bg swapped.
TBool CTermView::GetLook(TInt aRow, TInt aCol, TLook& aLook) const
	{
	TInt line = ViewLine(aRow);
	if (line >= iLinesPushed)
		{
		VTermPos pos;
		pos.row = line - iLinesPushed;
		pos.col = aCol;
		VTermScreenCell cell;
		if (!vterm_screen_get_cell(iScreen, pos, &cell))
			return EFalse;
		aLook.iCh = cell.chars[0];
		aLook.iWidth = cell.width;
		aLook.iFlags = (cell.attrs.bold ? KSbBold : 0) | (cell.attrs.underline ? KSbUnderline : 0)
			| (cell.attrs.strike ? KSbStrike : 0);
		// colours repeat along a line: reuse the last conversion when possible
		CTermView* self = CONST_CAST(CTermView*, this);
		if (!iCacheValid || (TInt)cell.attrs.reverse != iCacheRev
			|| !vterm_color_is_equal(&cell.fg, &iCacheFg) || !vterm_color_is_equal(&cell.bg, &iCacheBg))
			{
			CellColours(cell, self->iCacheF, self->iCacheB);
			self->iCacheFg = cell.fg;
			self->iCacheBg = cell.bg;
			self->iCacheRev = cell.attrs.reverse;
			self->iCacheValid = ETrue;
			}
		aLook.iFg = iCacheF;
		aLook.iBg = iCacheB;
		}
	else
		{
		int cols;
		const TSbCell* sb = SbLine(&iSb, iLinesPushed - line, &cols);
		if (sb && aCol < cols)
			{
			const TSbCell& c = sb[aCol];
			aLook.iCh = (c.iCh == 0xFFFF) ? (TUint)-1 : c.iCh;
			aLook.iWidth = (c.iFlags & KSbWide) ? 2 : 1;
			aLook.iFg = c.iGrey >> 4;
			aLook.iBg = c.iGrey & 0x0f;
			aLook.iFlags = c.iFlags & (KSbBold | KSbUnderline | KSbStrike);
			}
		else
			{
			aLook.iCh = 0;
			aLook.iWidth = 1;
			aLook.iFg = 0;
			aLook.iBg = 15;
			aLook.iFlags = 0;
			}
		}
	if (iSelActive)
		{
		TInt l0 = iSelLine0, c0 = iSelCol0, l1 = iSelLine1, c1 = iSelCol1;
		SbOrder(&l0, &c0, &l1, &c1);
		if (SbInSelection(line, aCol, l0, c0, l1, c1))
			{
			TInt t = aLook.iFg;
			aLook.iFg = aLook.iBg;
			aLook.iBg = t;
			TInt d = aLook.iFg - aLook.iBg;
			if (d < 0) d = -d;
			if (d < 6)
				{
				aLook.iBg = 0;
				aLook.iFg = 15;
				}
			}
		}
	return ETrue;
	}

// Code-page byte for a cell that is plain text (spaces included), or -1 if
// the cell needs DrawOneCell (box drawing, blocks, braille, wide glyphs).
// U+00A0 and the other Unicode spaces are drawn as plain blanks: spaces
// are only ever cleared, never drawn from the font, and a font's 0xA0 glyph
// can come out as garbage on the Psion (Claude Code uses U+00A0).
static TBool IsBlankChar(TUint aCh)
	{
	return aCh == 0 || aCh == ' ' || aCh == (TUint)-1 || aCh == 0xA0 ||
		(aCh >= 0x2000 && aCh <= 0x200A) || aCh == 0x202F || aCh == 0x205F || aCh == 0x3000;
	}

TInt CTermView::TextByte(const TLook& aLook) const
	{
	TUint ch = aLook.iCh;
	if (IsBlankChar(ch))
		return ' ';
	if (aLook.iWidth != 1)
		return -1;
	if (PsiBoxSegments(ch) > 0)
		return -1;
	if ((ch >= 0x2580 && ch <= 0x259F) || (ch >= 0x2800 && ch <= 0x28FF))
		return -1;
	TInt byte = PsiMapToCodePage(ch);
	if (byte == 0)
		return ' ';                   // zero-width
	if (byte < 0)
		return '?';
	return byte;
	}

// Colour -> one of 16 greys. The Psion screen is dark-on-light, so text
// colours are kept dark enough to read and backgrounds are kept light
// unless they are genuinely dark.
static TInt GreyOf(const VTermScreen* aScreen, VTermColor aCol)
	{
	vterm_screen_convert_color_to_rgb(aScreen, &aCol);
	// ((r*30 + g*59 + b*11) / 100) / 17 without two software divisions:
	// x*9869 >> 24 == x/1700 for every x <= 25500
	TUint x = aCol.rgb.red * 30 + aCol.rgb.green * 59 + aCol.rgb.blue * 11;
	return (TInt)((x * 9869u) >> 24);    // 0..15
	}

void CTermView::CellColours(const VTermScreenCell& aCell, TInt& aFg, TInt& aBg) const
	{
	TInt fg = 0;
	TInt bg = 15;
	TBool fgDefault = VTERM_COLOR_IS_DEFAULT_FG(&aCell.fg);
	TBool bgDefault = VTERM_COLOR_IS_DEFAULT_BG(&aCell.bg);
	if (!fgDefault)
		{
		fg = GreyOf(iScreen, aCell.fg);
		// light text colours were chosen for black backgrounds: darken them
		if (bgDefault)
			fg = (fg > 9) ? 9 - (fg - 9) / 2 : fg * 2 / 3;
		}
	if (!bgDefault)
		bg = GreyOf(iScreen, aCell.bg);
	if (aCell.attrs.reverse)
		{
		TInt t = fg; fg = bg; bg = t;
		}
	// keep enough contrast to read
	TInt diff = fg - bg;
	if (diff < 0) diff = -diff;
	if (diff < 6)
		fg = (bg >= 8) ? 0 : 15;
	aFg = fg;
	aBg = bg;
	}

void CTermView::DrawOneCell(CWindowGc& aGc, TInt aRow, TInt aCol) const
	{
	TLook look;
	if (GetLook(aRow, aCol, look))
		DrawOneCell(aGc, aRow, aCol, look);
	}

void CTermView::DrawOneCell(CWindowGc& aGc, TInt aRow, TInt aCol, const TLook& aLook) const
	{
	TInt fg = aLook.iFg, bg = aLook.iBg;

	TRect box(TPoint(iOriginX + aCol * iCellW, iOriginY + aRow * iCellH),
		TSize(iCellW, iCellH));
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	TInt tf, tb;
	ThemePair(fg, bg, tf, tb);
	aGc.SetBrushColor(TRgb::Gray16(tb));
	aGc.DrawRect(box);

	TUint ch = aLook.iCh;
	if (IsBlankChar(ch))
		return;

	TRgb fgRgb = TRgb::Gray16(tf);

	// Box drawing: real lines
	TInt seg = PsiBoxSegments(ch);
	if (seg > 0)
		{
		aGc.SetPenStyle(CGraphicsContext::ESolidPen);
		aGc.SetPenColor(fgRgb);
		aGc.SetPenSize(TSize((seg & KBoxHeavy) ? 2 : 1, (seg & KBoxHeavy) ? 2 : 1));
		TInt cx = box.iTl.iX + iCellW / 2;
		TInt cy = box.iTl.iY + iCellH / 2;
		TInt x0 = box.iTl.iX, x1 = box.iBr.iX;
		TInt y0 = box.iTl.iY, y1 = box.iBr.iY;
		if (seg & KBoxDouble)
			{
			if (seg & (KBoxLeft | KBoxRight))
				{
				TInt a = (seg & KBoxLeft) ? x0 : cx;
				TInt b = (seg & KBoxRight) ? x1 : cx + 1;
				aGc.DrawLine(TPoint(a, cy - 1), TPoint(b, cy - 1));
				aGc.DrawLine(TPoint(a, cy + 1), TPoint(b, cy + 1));
				}
			if (seg & (KBoxUp | KBoxDown))
				{
				TInt a = (seg & KBoxUp) ? y0 : cy;
				TInt b = (seg & KBoxDown) ? y1 : cy + 1;
				aGc.DrawLine(TPoint(cx - 1, a), TPoint(cx - 1, b));
				aGc.DrawLine(TPoint(cx + 1, a), TPoint(cx + 1, b));
				}
			}
		else
			{
			if (seg & KBoxLeft)  aGc.DrawLine(TPoint(x0, cy), TPoint(cx + 1, cy));
			if (seg & KBoxRight) aGc.DrawLine(TPoint(cx, cy), TPoint(x1, cy));
			if (seg & KBoxUp)    aGc.DrawLine(TPoint(cx, y0), TPoint(cx, cy + 1));
			if (seg & KBoxDown)  aGc.DrawLine(TPoint(cx, cy), TPoint(cx, y1));
			}
		aGc.SetPenSize(TSize(1, 1));
		return;
		}

	// Block elements U+2580..U+259F: filled rectangles / grey shades
	if (ch >= 0x2580 && ch <= 0x259F)
		{
		TRect r = box;
		TInt level = fg;
		switch (ch)
			{
		case 0x2580: r.iBr.iY = box.iTl.iY + iCellH / 2; break;           // upper half
		case 0x2584: r.iTl.iY = box.iTl.iY + iCellH / 2; break;           // lower half
		case 0x258C: r.iBr.iX = box.iTl.iX + iCellW / 2; break;           // left half
		case 0x2590: r.iTl.iX = box.iTl.iX + iCellW / 2; break;           // right half
		case 0x2591: level = (fg * 1 + bg * 3) / 4; break;                // light shade
		case 0x2592: level = (fg + bg) / 2; break;                        // medium shade
		case 0x2593: level = (fg * 3 + bg * 1) / 4; break;                // dark shade
		case 0x2594: r.iBr.iY = box.iTl.iY + (iCellH + 7) / 8; break;   // upper eighth
		case 0x2595: r.iTl.iX = box.iBr.iX - (iCellW + 7) / 8; break;   // right eighth
		default:
			if (ch >= 0x2596)                                             // quadrants
				{
				// bits: 1 upper-left, 2 upper-right, 4 lower-left, 8 lower-right
				static const TUint8 KQuad[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
				TInt q = KQuad[ch - 0x2596];
				TInt mx = box.iTl.iX + iCellW / 2, my = box.iTl.iY + iCellH / 2;
				aGc.SetBrushColor(Grey(level));
				if (q & 1) aGc.DrawRect(TRect(box.iTl.iX, box.iTl.iY, mx, my));
				if (q & 2) aGc.DrawRect(TRect(mx, box.iTl.iY, box.iBr.iX, my));
				if (q & 4) aGc.DrawRect(TRect(box.iTl.iX, my, mx, box.iBr.iY));
				if (q & 8) aGc.DrawRect(TRect(mx, my, box.iBr.iX, box.iBr.iY));
				return;
				}
			if (ch >= 0x2581 && ch <= 0x2587)                             // lower eighths
				r.iTl.iY = box.iBr.iY - (iCellH * (TInt)(ch - 0x2580)) / 8;
			else if (ch >= 0x2589 && ch <= 0x258F)                        // left eighths
				r.iBr.iX = box.iTl.iX + (iCellW * (TInt)(0x2590 - ch)) / 8;
			break;                                                        // 0x2588 = full block
			}
		aGc.SetBrushColor(Grey(level));
		aGc.DrawRect(r);
		return;
		}

	// Braille U+2800..U+28FF (spinners, sparklines): draw the dots
	if (ch >= 0x2800 && ch <= 0x28FF)
		{
		TUint bits = ch - 0x2800;
		// dot order: 1,2,3 left column top-down; 4,5,6 right; 7 left bottom; 8 right bottom
		static const TInt8 dx[8] = { 0, 0, 0, 1, 1, 1, 0, 1 };
		static const TInt8 dy[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };
		aGc.SetBrushColor(fgRgb);
		TInt stepX = iCellW / 2;
		TInt stepY = iCellH / 4;
		if (stepY < 1) stepY = 1;
		for (TInt i = 0; i < 8; i++)
			{
			if (bits & (1 << i))
				{
				TInt px = box.iTl.iX + dx[i] * stepX + stepX / 2 - 1;
				TInt py = box.iTl.iY + dy[i] * stepY + stepY / 2;
				aGc.DrawRect(TRect(TPoint(px, py), TSize(2, 2)));
				}
			}
		return;
		}

	// Ordinary text via the code page
	TInt byte = PsiMapToCodePage(ch);
	if (byte == 0)
		return;                       // zero-width
	if (byte < 0)
		byte = '?';
	TBuf<1> text;
	text.Append((TText)byte);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(fgRgb);
	TPoint base(box.iTl.iX, box.iTl.iY + iAscent);
	aGc.DrawText(text, base);
	if ((aLook.iFlags & KSbBold) || iSettings.iBold)
		aGc.DrawText(text, base + TPoint(1, 0));        // fake bold
	if (aLook.iFlags & KSbUnderline)
		aGc.DrawLine(TPoint(box.iTl.iX, box.iBr.iY - 1), TPoint(box.iBr.iX, box.iBr.iY - 1));
	if (aLook.iFlags & KSbStrike)
		aGc.DrawLine(TPoint(box.iTl.iX, box.iTl.iY + iCellH / 2),
			TPoint(box.iBr.iX, box.iTl.iY + iCellH / 2));
	}

// ----- themes ----------------------------------------------------------------
// Greys are 0 (black) .. 15 (white). Themes are applied only when drawing, so
// switching theme also re-colours the scrollback.
TInt CTermView::Theme(TInt aGrey) const
	{
	if (aGrey < 0) aGrey = 0;
	if (aGrey > 15) aGrey = 15;
	switch (iSettings.iTheme)
		{
	case 1: return 15 - aGrey;                        // inverted: light on dark
	case 2: return aGrey < 8 ? 0 : 15;                // high contrast
	case 3: return 2 + (aGrey * 11 + 7) / 15;         // soft: 2..13
	default: return aGrey;                            // classic
		}
	}

// Text and background together: in high contrast the darker of the two
// becomes black and the other white, so text can never vanish.
void CTermView::ThemePair(TInt aFg, TInt aBg, TInt& aThemedFg, TInt& aThemedBg) const
	{
	if (iSettings.iTheme == 2 && aFg != aBg)
		{
		aThemedFg = aFg < aBg ? 0 : 15;
		aThemedBg = aFg < aBg ? 15 : 0;
		return;
		}
	aThemedFg = Theme(aFg);
	aThemedBg = Theme(aBg);
	}

void CTermView::DrawCursor(CWindowGc& aGc) const
	{
	CTermView* self = CONST_CAST(CTermView*, this);
	self->iDrawnCurRow = iCurRow;
	self->iDrawnCurCol = iCurCol;
	self->iDrawnCurVisible = iCurVisible && !iBlinkHidden;
	if (iScrollOffset > 0)
		self->iDrawnCurVisible = EFalse;
	if (!iCurVisible || iBlinkHidden || iScrollOffset > 0 || iCurRow >= iRows || iCurCol >= iCols)
		return;
	TRect box(TPoint(iOriginX + iCurCol * iCellW, iOriginY + iCurRow * iCellH),
		TSize(iCellW, iCellH));
	if (iSettings.iCursor == 1)                        // underline
		box.iTl.iY = box.iBr.iY - (iCellH >= 12 ? 2 : 1) - iCellH / 12;
	else if (iSettings.iCursor == 2)                   // bar
		box.iBr.iX = box.iTl.iX + 2;
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetDrawMode(CGraphicsContext::EDrawModeNOTSCREEN);
	aGc.DrawRect(box);
	aGc.SetDrawMode(CGraphicsContext::EDrawModePEN);
	}

// ----- keyboard ------------------------------------------------------------

void CTermView::SendKey(VTermKey aKey, TInt aMod)
	{
	vterm_keyboard_key(iVt, aKey, (VTermModifier)aMod);
	}

void CTermView::SendChar(TUint aChar)
	{
	vterm_keyboard_unichar(iVt, aChar, VTERM_MOD_NONE);
	}

void CTermView::SendCtrl(TUint aLetter)
	{
	TBuf8<1> b;
	b.Append((TUint8)(aLetter & 0x1f));
	WriteToHost(b);
	}

void CTermView::SendString(const TDesC8& aText)
	{
	WriteToHost(aText);
	}

// Sends a snippet's text: printable characters as typed (so accented letters
// go out as UTF-8), plus the escapes \n Enter, \e Esc, \t Tab, ^X Ctrl+X,
// \\ backslash and \^ caret. A ^ not followed by a letter or @[\]_ is sent
// as it is (HEAD^ works).
void CTermView::SendSnippetText(const TDesC& aText, TBool aEnter)
	{
	if (iSelActive)
		ClearSelection();
	if (iScrollOffset > 0)
		ScrollTo(0);
	TInt n = aText.Length();
	for (TInt i = 0; i < n; i++)
		{
		TUint c = aText[i];
		TUint next = (i + 1 < n) ? aText[i + 1] : 0;
		if (c == '\\' && next)
			{
			i++;
			switch (next)
				{
			case 'n': case 'r': SendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE); break;
			case 'e': SendKey(VTERM_KEY_ESCAPE, VTERM_MOD_NONE); break;
			case 't': SendKey(VTERM_KEY_TAB, VTERM_MOD_NONE); break;
			case '\\': case '^': SendChar(next); break;
			default:                                   // not an escape: send both
				SendChar('\\');
				SendChar(PsiCodePageToUnicode(next));
				break;
				}
			continue;
			}
		if (c == '^' && ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
			next == '@' || next == '[' || next == '\\' || next == ']' || next == '_'))
			{
			SendCtrl(next);
			i++;
			continue;
			}
		SendChar(PsiCodePageToUnicode(c));
		}
	if (aEnter)
		SendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
	}

void CTermView::SerialInfo()
	{
	if (iSshActive)
		return;
	TBuf8<512> info;
	if (iSerial)
		iSerial->Probe(info);
	TRAPD(err, BeginDebugL(_L("Serial port info")));
	if (err != KErrNone)
		return;
	LocalMessage(info);
	TRAP_IGNORE(ShowDebugL());
	}

void CTermView::SendScreenSize()
	{
	TBuf8<48> cmd;
	cmd.Format(_L8("stty cols %d rows %d\r"), iCols, iRows);
	SendString(cmd);
	}

void CTermView::HangUp()
	{
	iModemOnline = EFalse;
	iLastRx = 0;
	iRxTail.Zero();
	// Hayes escape needs a quiet guard time either side of "+++"
	User::After(1100000);
	SendString(KHangupEscape);
	User::After(1100000);
	SendString(KHangupCommand);
	}

void CTermView::ResetTerminal()
	{
	SbClear(&iSb);
	iScrollOffset = 0;
	iSelActive = EFalse;
	vterm_screen_reset(iScreen, 1);
	vterm_screen_flush_damage(iScreen);
	DrawNow();
	}

TKeyResponse CTermView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKey)
		return EKeyWasNotConsumed;
	TUint code = aKeyEvent.iCode;
	TUint mods = aKeyEvent.iModifiers;
	if (code != EKeyMenu)
		AddKeyEntropy(code);
	if (iReconnectWait)
		{
		if (code == EKeyEscape)
			{
			CancelReconnect(_L8("\r\n[Reconnect cancelled]\r\n"));
			return EKeyWasConsumed;
			}
		if (code == EKeyEnter)
			{
			ReconnectNowL();
			return EKeyWasConsumed;
			}
		}
	if (iGatheringEntropy)
		{
		if (code == EKeyEscape)
			{
			iGatheringEntropy = EFalse;
			LocalMessage(_L8("\r\n[SSH cancelled]\r\n"));
			return EKeyWasConsumed;
			}
		if (code == EKeyMenu)
			return EKeyWasNotConsumed;
		if (iKeyCount >= KEntropyKeysNeeded)
			{
			iGatheringEntropy = EFalse;
			LocalMessage(_L8(" done.\r\n"));
			if (iEntropyMode == 4)
				KeyToolL(iPendKeyMode, iPendKeyBase, iPendKeyName, iPendKeySrc);
			else
				LaunchSshL();
			}
		else
			LocalMessage(_L8("."));
		return EKeyWasConsumed;
		}
	// Shift+PgUp/PgDn/Home/End move through the history (Fn+arrows on the Psion)
	if ((mods & EModifierShift) && !(mods & EModifierCtrl))
		{
		switch (code)
			{
		case EKeyPageUp:   ScrollBy(iRows - 1); return EKeyWasConsumed;
		case EKeyPageDown: ScrollBy(-(iRows - 1)); return EKeyWasConsumed;
		case EKeyHome:     ScrollTo(iSb.iCount); return EKeyWasConsumed;
		case EKeyEnd:      ScrollTo(0); return EKeyWasConsumed;
		default: break;
			}
		}
	// welcome screen: 1-9 connect to that saved host; any other key goes on
	// as normal (e.g. AT commands to the modem) and ends the welcome
	if (iWelcome && code != EKeyMenu)
		{
		iWelcome = EFalse;
		if (!iSshActive && !(mods & (EModifierCtrl | EModifierShift)) && code >= '1' && code <= '9'
			&& iHosts && (TInt)(code - '1') < iHosts->Count())
			{
			CEikonEnv::Static()->EikAppUi()->HandleCommandL(EPtCmdHost0 + (code - '1'));
			return EKeyWasConsumed;
			}
		}
	// Shift+Ctrl + a key: a snippet on that hotkey (menu shortcuts such as
	// Shift+Ctrl+S never get here - EIKON handles those first)
	if ((mods & EModifierCtrl) && (mods & EModifierShift) && iSnippets)
		{
		TInt key = aKeyEvent.iScanCode;
		if (key >= 'a' && key <= 'z')
			key -= 'a' - 'A';
		TInt idx = iSnippets->FindKey(key);
		if (idx >= 0)
			{
			SendSnippetText(iSnippets->At(idx).iText, iSnippets->At(idx).iEnter);
			return EKeyWasConsumed;
			}
		}
	if (code != EKeyMenu)
		{
		// typing returns to the live screen and drops the selection
		if (iSelActive)
			ClearSelection();
		if (iScrollOffset > 0)
			ScrollTo(0);
		}
	// Ctrl+Tab / Shift+Ctrl+Tab: next / previous tmux window
	if (code == EKeyTab && (mods & EModifierCtrl) && SshLoggedIn() && InTmux())
		{
		TmuxNextWindow(mods & EModifierShift);
		return EKeyWasConsumed;
		}
	TInt vm = VTERM_MOD_NONE;
	if (mods & EModifierShift) vm |= VTERM_MOD_SHIFT;
	if (mods & EModifierCtrl) vm |= VTERM_MOD_CTRL;

	switch (code)
		{
	case EKeyMenu:
		return EKeyWasNotConsumed;               // let EIKON open the menu
	case EKeyUpArrow:    SendKey(VTERM_KEY_UP, vm); break;
	case EKeyDownArrow:  SendKey(VTERM_KEY_DOWN, vm); break;
	case EKeyLeftArrow:  SendKey(VTERM_KEY_LEFT, vm); break;
	case EKeyRightArrow: SendKey(VTERM_KEY_RIGHT, vm); break;
	case EKeyPageUp:     SendKey(VTERM_KEY_PAGEUP, vm); break;
	case EKeyPageDown:   SendKey(VTERM_KEY_PAGEDOWN, vm); break;
	case EKeyHome:       SendKey(VTERM_KEY_HOME, vm); break;
	case EKeyEnd:        SendKey(VTERM_KEY_END, vm); break;
	case EKeyEnter:      SendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE); break;
	case EKeyBackspace:  SendKey(VTERM_KEY_BACKSPACE, VTERM_MOD_NONE); break;
	case EKeyDelete:     SendKey(VTERM_KEY_DEL, VTERM_MOD_NONE); break;
	case EKeyEscape:     SendKey(VTERM_KEY_ESCAPE, VTERM_MOD_NONE); break;
	case EKeyTab:        SendKey(VTERM_KEY_TAB, vm & VTERM_MOD_SHIFT); break;
	default:
		if (code >= EKeyF1 && code <= EKeyF12)
			{
			SendKey((VTermKey)VTERM_KEY_FUNCTION(code - EKeyF1 + 1), VTERM_MOD_NONE);
			break;
			}
		if (code < 0x20)
			{
			SendCtrl(code | 0x40);               // already a control character
			break;
			}
		if (code < 0x100)
			{
			if ((mods & EModifierCtrl) && ((code >= 'a' && code <= 'z') ||
				(code >= 'A' && code <= 'Z') || code == '\\' || code == ']' ||
				code == '[' || code == '^' || code == '_' || code == '@'))
				{
				SendCtrl(code);
				break;
				}
			SendChar(PsiCodePageToUnicode(code));
			break;
			}
		return EKeyWasNotConsumed;
		}
	return EKeyWasConsumed;
	}

// ----- scrollback, pen selection, clipboard -----------------------------------

void CTermView::ScrollBy(TInt aLines)
	{
	ScrollTo(iScrollOffset + aLines);
	}

void CTermView::ScrollTo(TInt aOffset)
	{
	if (aOffset > iSb.iCount)
		aOffset = iSb.iCount;
	if (aOffset < 0)
		aOffset = 0;
	if (aOffset == iScrollOffset)
		return;
	iScrollOffset = aOffset;
	iCacheValid = EFalse;
	DrawNow();
	}

// Repaints whole view rows [aRow0, aRow1] (e.g. after the selection moved).
void CTermView::RepaintRows(TInt aRow0, TInt aRow1)
	{
	if (aRow0 > aRow1)
		{
		TInt t = aRow0; aRow0 = aRow1; aRow1 = t;
		}
	if (aRow0 < 0) aRow0 = 0;
	if (aRow1 >= iRows) aRow1 = iRows - 1;
	if (aRow0 > aRow1 || !IsActivated())
		return;
	BeginPaint();
	DrawCells(*iPaintGc, aRow0, 0, aRow1 + 1, iCols);
	DrawScrollIndicator(*iPaintGc);
	EndPaint();
	}

void CTermView::ClearSelection()
	{
	if (!iSelActive)
		return;
	iSelActive = EFalse;
	TInt l0 = iSelLine0, c0 = iSelCol0, l1 = iSelLine1, c1 = iSelCol1;
	SbOrder(&l0, &c0, &l1, &c1);
	RepaintRows(l0 - ViewLine(0), l1 - ViewLine(0));
	}

// Diagnostic: the Unicode code points and attributes of the first few
// selected cells, e.g. to find out what an odd-looking character really is.
void CTermView::ShowCharInfoL()
	{
	if (!iSelActive)
		{
		iEikonEnv->InfoMsg(_L("Drag the pen over the character first"));
		return;
		}
	TInt l0 = iSelLine0, c0 = iSelCol0, l1 = iSelLine1, c1 = iSelCol1;
	SbOrder(&l0, &c0, &l1, &c1);
	TBuf<200> info;
	TInt shown = 0;
	for (TInt line = l0; line <= l1 && shown < 6; line++)
		{
		TInt row = line - ViewLine(0);
		TInt from = (line == l0) ? c0 : 0;
		TInt to = (line == l1) ? c1 : iCols - 1;
		for (TInt c = from; c <= to && shown < 6; c++, shown++)
			{
			TLook look;
			TUint ch = 0;
			TInt w = 1;
			TUint attr = 0;
			if (row >= 0 && row < iRows && GetLook(row, c, look))
				{
				ch = look.iCh;
				w = look.iWidth;
				attr = look.iFlags;
				}
			if (line >= iLinesPushed)
				{
				VTermPos pos;
				pos.row = line - iLinesPushed;
				pos.col = c;
				VTermScreenCell cell;
				if (vterm_screen_get_cell(iScreen, pos, &cell) && cell.attrs.reverse)
					attr |= 0x100;
				}
			if (shown)
				info.Append(' ');
			info.AppendFormat(_L("%04x"), ch == (TUint)-1 ? 0xFFFF : ch);
			if (w > 1) info.Append('w');
			if (attr & KSbBold) info.Append('b');
			if (attr & 0x100) info.Append('r');
			TInt b = PsiMapToCodePage(ch);
			if (b > 0 && ch != (TUint)-1)
				info.AppendFormat(_L("=%02x"), b);
			}
		}
	iEikonEnv->InfoMsg(info);
	}

void CTermView::SelectScreen()
	{
	ClearSelection();
	iSelLine0 = ViewLine(0);
	iSelCol0 = 0;
	iSelLine1 = ViewLine(iRows - 1);
	iSelCol1 = iCols - 1;
	iSelActive = ETrue;
	DrawNow();
	}

// Pen: drag across text to select it. The right-most column works as a
// scroll bar: drag there to move through the history.
void CTermView::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	TPoint p = aEvent.iPosition;
	TInt col = (p.iX - iOriginX) / iCellW;
	TInt row = (p.iY - iOriginY) / iCellH;
	if (p.iY < iOriginY) row = -1;
	if (col < 0) col = 0;
	if (col >= iCols) col = iCols - 1;
	TInt rowC = row < 0 ? 0 : (row >= iRows ? iRows - 1 : row);

	// a tap on a tab switches to that tmux window
	if (iTabsDrawn && row == iTabRow && iScrollOffset == 0 && aEvent.iType == TPointerEvent::EButton1Down)
		{
		iPenOnTabs = ETrue;
		for (TInt i = 0; i < iTabCount; i++)
			if (p.iX >= iTabs[i].iX0 && p.iX < iTabs[i].iX1)
				{
				SelectTmuxWindow(iTabs[i].iIndex);
				break;
				}
		return;
		}
	if (iPenOnTabs)
		{
		if (aEvent.iType == TPointerEvent::EButton1Up)
			iPenOnTabs = EFalse;
		return;
		}
	// The program asked for the mouse (tmux "mouse on", vim, htop...): a tap
	// is a click, a drag up or down is the scroll wheel. Shift+pen still
	// selects text here, as in xterm.
	TBool mouse = (iMouseMode != VTERM_PROP_MOUSE_NONE) && SshLoggedIn() && iScrollOffset == 0
		&& !(aEvent.iModifiers & EModifierShift);
	if (aEvent.iType == TPointerEvent::EButton1Down && mouse && row >= 0 && row < iRows)
		{
		ClearSelection();
		iPenMode = EPenMouse;
		iPenY0 = p.iY;
		iPenRow0 = row;
		iPenCol0 = col;
		iPenWheeled = EFalse;
		return;
		}
	if (iPenMode == EPenMouse)
		{
		if (aEvent.iType == TPointerEvent::EDrag)
			{
			// a row's height of movement = one wheel step (4 up, 5 down)
			TInt steps = (p.iY - iPenY0) / iCellH;
			if (steps != 0)
				{
				vterm_mouse_move(iVt, iPenRow0, iPenCol0, VTERM_MOD_NONE);
				for (TInt i = 0; i < (steps < 0 ? -steps : steps) && i < 20; i++)
					{
					// pen moves down = see older text = wheel up
					vterm_mouse_button(iVt, steps > 0 ? 4 : 5, ETrue, VTERM_MOD_NONE);
					vterm_mouse_button(iVt, steps > 0 ? 4 : 5, EFalse, VTERM_MOD_NONE);
					}
				iPenY0 += steps * iCellH;
				iPenWheeled = ETrue;
				}
			}
		else if (aEvent.iType == TPointerEvent::EButton1Up)
			{
			if (!iPenWheeled)
				{
				vterm_mouse_move(iVt, iPenRow0, iPenCol0, VTERM_MOD_NONE);
				vterm_mouse_button(iVt, 1, ETrue, VTERM_MOD_NONE);
				vterm_mouse_button(iVt, 1, EFalse, VTERM_MOD_NONE);
				}
			iPenMode = EPenNone;
			}
		return;
		}

	switch (aEvent.iType)
		{
	case TPointerEvent::EButton1Down:
		ClearSelection();
		if (col >= iCols - 1 && iSb.iCount > 0)
			{
			iPenMode = EPenScroll;
			iPenY0 = p.iY;
			iPenOffset0 = iScrollOffset;
			}
		else
			{
			iPenMode = EPenSelect;
			iSelLine0 = iSelLine1 = ViewLine(rowC);
			iSelCol0 = iSelCol1 = col;
			}
		break;
	case TPointerEvent::EDrag:
		if (iPenMode == EPenScroll)
			{
			// drag down pulls older text into view
			ScrollTo(iPenOffset0 + (p.iY - iPenY0) / iCellH);
			}
		else if (iPenMode == EPenSelect)
			{
			// dragging past the top or bottom edge scrolls while selecting
			if (row < 0)
				ScrollBy(1);
			else if (row >= iRows)
				ScrollBy(-1);
			TInt line = ViewLine(rowC);
			if (line == iSelLine1 && col == iSelCol1 && iSelActive)
				break;
			TInt oldRow = iSelLine1 - ViewLine(0);
			iSelLine1 = line;
			iSelCol1 = col;
			TBool wasActive = iSelActive;
			iSelActive = (iSelLine1 != iSelLine0 || iSelCol1 != iSelCol0) || wasActive;
			if (iSelActive)
				RepaintRows(wasActive ? oldRow : iSelLine0 - ViewLine(0), rowC);
			}
		break;
	case TPointerEvent::EButton1Up:
		if (iPenMode == EPenSelect && iSelActive)
			iEikonEnv->InfoMsg(_L("Shift+Ctrl+C copies the selection"));
		iPenMode = EPenNone;
		break;
	default:
		break;
		}
	}

// Characters of one absolute line, for turning a selection into text.
int CTermView::SelLineFn(void* aCtx, int aLine, unsigned int* aChars, int aMaxCols)
	{
	CTermView* self = (CTermView*)aCtx;
	if (aLine >= self->iLinesPushed)
		{
		TInt r = aLine - self->iLinesPushed;
		if (r >= self->iRows)
			return 0;
		TInt n = self->iCols < aMaxCols ? self->iCols : aMaxCols;
		for (TInt c = 0; c < n; c++)
			{
			VTermPos pos;
			pos.row = r;
			pos.col = c;
			VTermScreenCell cell;
			aChars[c] = vterm_screen_get_cell(self->iScreen, pos, &cell) ? cell.chars[0] : 0;
			}
		return n;
		}
	int cols;
	const TSbCell* sb = SbLine(&self->iSb, self->iLinesPushed - aLine, &cols);
	if (!sb)
		return 0;
	if (cols > aMaxCols)
		cols = aMaxCols;
	for (TInt c = 0; c < cols; c++)
		aChars[c] = (sb[c].iCh == 0xFFFF) ? (unsigned int)-1 : sb[c].iCh;
	return cols;
	}

// Unicode -> the Psion's code page, with ASCII stand-ins for line drawing.
int CTermView::SelMapFn(unsigned int aCh)
	{
	TInt seg = PsiBoxSegments(aCh);
	if (seg > 0)
		{
		TBool h = (seg & (KBoxLeft | KBoxRight)) != 0;
		TBool v = (seg & (KBoxUp | KBoxDown)) != 0;
		return (h && v) ? '+' : (h ? '-' : '|');
		}
	if (aCh >= 0x2580 && aCh <= 0x259F)
		return '#';
	if (aCh >= 0x2800 && aCh <= 0x28FF)
		return '.';
	TInt b = PsiMapToCodePage(aCh);
	return b < 0 ? '?' : b;
	}

void CTermView::CopySelectionL()
	{
	if (!iSelActive)
		{
		iEikonEnv->InfoMsg(_L("Drag the pen over some text first"));
		return;
		}
	HBufC8* buf = HBufC8::NewLC(KClipMax);
	TPtr8 p(buf->Des());
	TInt n = SbSelectionText(&CTermView::SelLineFn, this, &CTermView::SelMapFn,
		iSelLine0, iSelCol0, iSelLine1, iSelCol1,
		(unsigned char*)p.Ptr(), KClipMax, CEditableText::EParagraphDelimiter);
	p.SetLength(n);
	CClipboard* cb = CClipboard::NewForWritingLC(iCoeEnv->FsSession());
	CPlainText* text = CPlainText::NewL();
	CleanupStack::PushL(text);
	text->InsertL(0, p);
	text->CopyToStoreL(cb->Store(), cb->StreamDictionary(), 0, text->DocumentLength());
	cb->CommitL();
	CleanupStack::PopAndDestroy(3);         // text, cb, buf
	TBuf<40> msg;
	msg.Format(_L("Copied %d characters"), n);
	iEikonEnv->InfoMsg(msg);
	}

// Types the clipboard to the host (as a bracketed paste when the remote
// program asked for that, so editors don't auto-indent it).
void CTermView::PasteL()
	{
	CClipboard* cb = NULL;
	TRAPD(err, cb = CClipboard::NewForReadingL(iCoeEnv->FsSession()));
	if (err != KErrNone || !cb)
		{
		iEikonEnv->InfoMsg(_L("Nothing to paste"));
		return;
		}
	CleanupStack::PushL(cb);
	CPlainText* text = CPlainText::NewL();
	CleanupStack::PushL(text);
	text->PasteFromStoreL(cb->Store(), cb->StreamDictionary(), 0);
	TInt len = text->DocumentLength();
	if (len > KClipMax)
		len = KClipMax;
	HBufC* buf = HBufC::NewLC(len);
	TPtr p(buf->Des());
	text->Extract(p, 0, len);
	if (iScrollOffset > 0)
		ScrollTo(0);
	vterm_keyboard_start_paste(iVt);
	for (TInt i = 0; i < p.Length(); i++)
		{
		TUint c = p[i];
		if (c == CEditableText::EParagraphDelimiter || c == CEditableText::ELineBreak)
			SendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
		else if (c == CEditableText::ETabCharacter)
			SendKey(VTERM_KEY_TAB, VTERM_MOD_NONE);
		else if (c == CEditableText::ENonBreakingSpace)
			SendChar(' ');
		else if (c >= 0x20)
			SendChar(PsiCodePageToUnicode(c));
		}
	vterm_keyboard_end_paste(iVt);
	CleanupStack::PopAndDestroy(3);         // buf, text, cb
	}

// ----- SSH session -----------------------------------------------------------

// Keystroke timing is the Psion's best source of unpredictability for the
// SSH key exchange: keep a rolling buffer of tick counts and clock bits.
void CTermView::AddKeyEntropy(TUint aCode)
	{
	TTime now;
	now.HomeTime();
	TUint32 t = now.Int64().Low();
	TUint32 tick = User::TickCount();
	TUint8 sample[4];
	sample[0] = (TUint8)(t ^ aCode);
	sample[1] = (TUint8)(t >> 8);
	sample[2] = (TUint8)(tick ^ (t >> 16));
	sample[3] = (TUint8)(tick >> 8);
	for (TInt i = 0; i < 4; i++)
		{
		iEntropy[iEntropyPos] ^= sample[i];
		iEntropyPos = (iEntropyPos + 1) % PSI_ENTROPY_SIZE;
		if (iEntropyFill < PSI_ENTROPY_SIZE)
			iEntropyFill++;
		}
	iKeyCount++;
	}

TBool CTermView::SeedFileExists()
	{
	TEntry entry;
	return iCoeEnv->FsSession().Entry(KSeedFile, entry) == KErrNone;
	}

void CTermView::StartSshL()
	{
	if (iSshActive || iGatheringEntropy)
		return;
	if (iSettings.iSshHost.Length() == 0 || iSettings.iSshUser.Length() == 0)
		return;
	if (!SeedFileExists() && iKeyCount < KEntropyKeysNeeded)
		{
		// first ever SSH: gather randomness from the user's typing
		LocalMessage(_L8("\r\nSSH needs some randomness for its keys the first time.\r\n"
			"Please type random keys until it says done (Esc cancels): "));
		iGatheringEntropy = ETrue;
		iEntropyMode = 0;
		return;
		}
	LaunchSshL();
	}

// ----- SSH login key ---------------------------------------------------------------
// psissh (mode 4) makes an Ed25519 key once: C:\System\Apps\PsiTerm\id_ed25519,
// public half in id_ed25519.pub. It is offered to every server before the
// password; "Install login key on server" adds it to a server's
// ~/.ssh/authorized_keys from inside a logged-in session.

TBool CTermView::SshLoggedIn() const
	{
	return iSshActive && iLaunchMode == 0 && iShared && iShared->state == PSI_STATE_CONNECTED;
	}

// Reads <base>.pub (the OpenSSH public key line)
static TInt ReadPubKey(RFs& aFs, const TDesC& aBase, TDes8& aKey)
	{
	TFileName f(aBase);
	f.Append(_L(".pub"));
	RFile file;
	TInt r = file.Open(aFs, f, EFileRead);
	if (r != KErrNone)
		return r;
	r = file.Read(aKey);
	file.Close();
	while (aKey.Length() && (aKey[aKey.Length() - 1] == '\n' || aKey[aKey.Length() - 1] == '\r'))
		aKey.SetLength(aKey.Length() - 1);
	if (r == KErrNone && (aKey.Length() < 20 || aKey.Left(4) != _L8("ssh-")))
		r = KErrCorrupt;
	return r;
	}

// The key's name, fingerprint, public line and how to use it
static void AppendKeyHelp(RFs& aFs, TDes8& aOut, const TDesC& aBase, const TDesC& aName)
	{
	HBufC8* key = HBufC8::New(900);
	if (!key)
		return;
	TPtr8 k = key->Des();
	if (ReadPubKey(aFs, aBase, k) != KErrNone)
		{
		delete key;
		aOut.Append(_L8("\r\n(The key's files are missing.)\r\n"));
		return;
		}
	aOut.Append(_L8("\r\nKey: "));
	TBuf8<24> name;
	name.Copy(aName);
	aOut.Append(name);
	TFileName fp(aBase);
	fp.Append(_L(".fp"));
	RFile f;
	if (f.Open(aFs, fp, EFileRead) == KErrNone)
		{
		TBuf8<64> b;
		f.Read(b);
		f.Close();
		aOut.Append(_L8("\r\nFingerprint: "));
		aOut.Append(b);
		}
	aOut.Append(_L8("\r\n\r\nPublic key (for a server's ~/.ssh/authorized_keys):\r\n\r\n"));
	aOut.Append(k);
	delete key;
	aOut.Append(_L8("\r\n\r\nTo use it: connect with your password, choose Terminal > "
		"Install login key on server (at a shell prompt), then SSH to... > Edit > "
		"Log in with. If this Psion is lost, remove the line from the server's "
		"~/.ssh/authorized_keys.\r\n"));
	}

void CTermView::ShowKeyL(const TDesC& aBase, const TDesC& aName)
	{
	HBufC8* buf = HBufC8::NewLC(2000);
	TPtr8 t = buf->Des();
	AppendKeyHelp(iCoeEnv->FsSession(), t, aBase, aName);
	BeginDebugL(_L("SSH key"));
	LocalMessage(t);
	CleanupStack::PopAndDestroy();
	ShowDebugL();
	}

// Make (4) or import (5) a key with psissh, in the tool window. EFalse if it
// has to wait for randomness first (typed in the terminal; it then runs).
TBool CTermView::KeyToolL(TInt aMode, const TDesC& aBase, const TDesC& aName, const TDesC& aSrc)
	{
	if (iSshActive || iGatheringEntropy)
		return ETrue;
	iPendKeyMode = aMode;
	iPendKeyBase = aBase;
	iPendKeyName = LeftSafe(aName, iPendKeyName.MaxLength());
	iPendKeySrc = LeftSafe(aSrc, iPendKeySrc.MaxLength());
	if (aMode == 4 && !SeedFileExists() && iKeyCount < KEntropyKeysNeeded)
		{
		LocalMessage(_L8("\r\nMaking a key needs some randomness.\r\n"
			"Please type random keys until it says done (Esc cancels): "));
		iGatheringEntropy = ETrue;
		iEntropyMode = 4;
		return EFalse;
		}
	iCoeEnv->FsSession().MkDirAll(KKeysDir);
	BeginDebugL(aMode == 4 ? _L("New SSH key") : _L("Import SSH key"));
	LaunchSshL(aMode);
	RunToolDialogL();
	return ETrue;
	}

void CTermView::InstallLoginKeyL(const TDesC& aBase)
	{
	HBufC8* keyBuf = HBufC8::NewLC(900);
	TPtr8 key = keyBuf->Des();
	if (!SshLoggedIn() || ReadPubKey(iCoeEnv->FsSession(), aBase, key) != KErrNone)
		{
		CleanupStack::PopAndDestroy();
		return;
		}
	// the key's middle (the base64 blob) is what grep looks for
	TInt sp1 = key.Locate(' ');
	TPtrC8 blob(key.Mid(sp1 + 1));
	TInt sp = blob.Locate(' ');
	if (sp > 0)
		blob.Set(blob.Left(sp));
	HBufC8* buf = HBufC8::NewLC(2200);
	TPtr8 c = buf->Des();
	// in a subshell, so the umask does not stick to the user's shell
	c.Append(_L8(" (umask 077; mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys && "
		"{ grep -qF '"));
	c.Append(blob);
	c.Append(_L8("' ~/.ssh/authorized_keys || echo '"));
	c.Append(key);
	c.Append(_L8("' >> ~/.ssh/authorized_keys; }) && echo 'PsiTerm: login key installed'\r"));
	SendString(c);
	CleanupStack::PopAndDestroy(2);
	}

void CTermView::StartSpeedTestL()
	{
	if (iSshActive || iGatheringEntropy)
		return;
	LaunchSshL(1);
	}

void CTermView::StartUpdateL()
	{
	if (iSshActive || iGatheringEntropy)
		return;
	if (iSettings.iUpdSource == 1 && iSettings.iUpdHost.Length() == 0)
		return;
	LaunchSshL(2);
	}

// ----- screenshots -------------------------------------------------------------
// A screenshot (.psi) holds the screen pixels (16 greys) plus every cell's
// character and colours, so problems can be seen and decoded exactly.
// Layout: "PSISHOT1", width, height, rows, cols, zoom (6 x uint32 LE),
// width*height 4-bit pixels (row by row), then rows*cols cells of
// { uint32 code point, uint8 fg, uint8 bg, uint8 flags, uint8 width }.

void CTermView::ShotDir(TDes& aDir)
	{
	TVolumeInfo vol;
	aDir.Copy(iCoeEnv->FsSession().Volume(vol, EDriveD) == KErrNone
		? _L("D:\\PsiTerm\\") : _L("C:\\PsiTerm\\"));
	}

void CTermView::ScreenshotL()
	{
	if (!iShotTimer)
		iShotTimer = CPeriodic::NewL(CActive::EPriorityStandard);
	iShotTimer->Cancel();
	iShotTimer->Start(600000, 600000, TCallBack(&CTermView::ShotCallback, this));
	}

TInt CTermView::ShotCallback(TAny* aSelf)
	{
	CTermView* self = (CTermView*)aSelf;
	self->iShotTimer->Cancel();
	TRAPD(err, self->TakeScreenshotL());
	if (err != KErrNone)
		{
		TBuf<48> m;
		m.Format(_L("Screenshot failed (%d)"), err);
		CEikonEnv::Static()->InfoMsg(m);
		}
	return 0;
	}

static void PutU32(TDes8& aBuf, TUint aValue)
	{
	for (TInt i = 0; i < 4; i++)
		aBuf.Append((TUint8)(aValue >> (8 * i)));
	}

void CTermView::TakeScreenshotL()
	{
	RFs& fs = iCoeEnv->FsSession();
	TFileName name;
	ShotDir(name);
	fs.MkDirAll(name);
	TInt dirLen = name.Length();
	TInt n;
	for (n = 1; n < 1000; n++)
		{
		name.SetLength(dirLen);
		name.AppendFormat(_L("shot%03d.psi"), n);
		TEntry e;
		if (fs.Entry(name, e) != KErrNone)
			break;
		}
	CWsScreenDevice* screen = iCoeEnv->ScreenDevice();
	TSize size = screen->SizeInPixels();
	CFbsBitmap* bmp = new(ELeave) CFbsBitmap;
	CleanupStack::PushL(bmp);
	User::LeaveIfError(bmp->Create(size, EGray16));
	User::LeaveIfError(screen->CopyScreenToBitmap(bmp));
	RFile file;
	User::LeaveIfError(file.Replace(fs, name, EFileWrite));
	CleanupClosePushL(file);
	TBuf8<32> hdr;
	hdr.Append(_L8("PSISHOT1"));
	PutU32(hdr, size.iWidth);
	PutU32(hdr, size.iHeight);
	PutU32(hdr, iRows);
	PutU32(hdr, iCols);
	PutU32(hdr, iSettings.iZoom);
	User::LeaveIfError(file.Write(hdr));
	HBufC8* line = HBufC8::NewLC(size.iWidth / 2 + 8);
	TPtr8 lp(line->Des());
	for (TInt y = 0; y < size.iHeight; y++)
		{
		bmp->GetScanLine(lp, TPoint(0, y), size.iWidth, EGray16);
		lp.SetLength((size.iWidth + 1) / 2);
		User::LeaveIfError(file.Write(lp));
		}
	CleanupStack::PopAndDestroy();          // line
	TBuf8<8 * 16> cells;
	for (TInt r = 0; r < iRows; r++)
		for (TInt c = 0; c < iCols; c++)
			{
			TLook look;
			if (!GetLook(r, c, look))
				{
				look.iCh = 0; look.iFg = 0; look.iBg = 15; look.iFlags = 0; look.iWidth = 1;
				}
			PutU32(cells, look.iCh);
			cells.Append((TUint8)look.iFg);
			cells.Append((TUint8)look.iBg);
			cells.Append((TUint8)look.iFlags);
			cells.Append((TUint8)look.iWidth);
			if (cells.Length() + 8 > cells.MaxLength())
				{
				User::LeaveIfError(file.Write(cells));
				cells.Zero();
				}
			}
	User::LeaveIfError(file.Write(cells));
	CleanupStack::PopAndDestroy(2);         // file, bmp
	TBuf<64> m;
	m.Format(_L("Screenshot %d saved - Terminal > Send screenshots"), n);
	iEikonEnv->InfoMsg(m);
	}

// Deletes the sent .psi files; returns how many.
TInt CTermView::DeleteShots()
	{
	RFs& fs = iCoeEnv->FsSession();
	TFileName dir, spec;
	ShotDir(dir);
	spec.Copy(dir);
	spec.Append(_L("*.psi"));
	CDir* list = NULL;
	if (fs.GetDir(spec, KEntryAttNormal, ESortByName, list) != KErrNone || !list)
		return 0;
	TInt n = list->Count();
	for (TInt i = 0; i < n; i++)
		{
		TFileName f(dir);
		f.Append((*list)[i].iName);
		fs.Delete(f);
		}
	delete list;
	return n;
	}

// Bundles every .psi into one file and POSTs it to the update server.
void CTermView::SendScreenshotsL()
	{
	if (iSshActive || iGatheringEntropy)
		return;
	RFs& fs = iCoeEnv->FsSession();
	TFileName dir, spec;
	ShotDir(dir);
	spec.Copy(dir);
	spec.Append(_L("*.psi"));
	CDir* list = NULL;
	if (fs.GetDir(spec, KEntryAttNormal, ESortByName, list) != KErrNone || !list || list->Count() == 0)
		{
		delete list;
		iEikonEnv->InfoMsg(_L("No screenshots to send (Shift+Ctrl+P takes one)"));
		return;
		}
	CleanupStack::PushL(list);
	iUpdateFile.Copy(dir);
	iUpdateFile.Append(_L("send.tmp"));
	RFile out;
	User::LeaveIfError(out.Replace(fs, iUpdateFile, EFileWrite));
	CleanupClosePushL(out);
	HBufC8* buf = HBufC8::NewLC(4096);
	TPtr8 bp(buf->Des());
	for (TInt i = 0; i < list->Count(); i++)
		{
		const TEntry& e = (*list)[i];
		TBuf8<64> hdr;
		hdr.Append(_L8("PSIFILE1"));
		PutU32(hdr, e.iName.Length());
		hdr.Append(e.iName);
		PutU32(hdr, e.iSize);
		User::LeaveIfError(out.Write(hdr));
		TFileName f(dir);
		f.Append(e.iName);
		RFile in;
		User::LeaveIfError(in.Open(fs, f, EFileRead));
		for (;;)
			{
			in.Read(bp);
			if (bp.Length() == 0)
				break;
			out.Write(bp);
			}
		in.Close();
		}
	CleanupStack::PopAndDestroy(3);         // buf, out, list
	LaunchSshL(3);
	}

void CTermView::LaunchSshL(TInt aMode)
	{
	// hand the serial port over to psissh.exe
	if (iSerial)
		iSerial->Close();

	TInt r = iChunk.CreateGlobal(_L(PSI_SHARED_NAME), sizeof(PsiShared), sizeof(PsiShared));
	if (r == KErrAlreadyExists)
		r = iChunk.OpenGlobal(_L(PSI_SHARED_NAME), EFalse);
	if (r != KErrNone)
		{
		TBuf8<64> msg;
		msg.Format(_L8("\r\n[SSH: shared memory error %d]\r\n"), r);
		LocalMessage(msg);
		ApplySerialSettings();
		return;
		}
	iChunkOpen = ETrue;
	iShared = (PsiShared*)iChunk.Base();
	Mem::FillZ(iShared, sizeof(PsiShared));
	iShared->magic = PSI_SHARED_MAGIC;
	iShared->rows = iRows;
	iShared->cols = iCols;
	iShared->baud_index = iSettings.iBaudIndex;
	iShared->rtscts = iSettings.iRtsCts;
	iShared->port = iSettings.iSshPort > 0 ? iSettings.iSshPort : 22;
	TPtr8 host((TUint8*)iShared->host, sizeof(iShared->host) - 1);
	host.Copy(iSettings.iSshHost);
	host.ZeroTerminate();
	TPtr8 user((TUint8*)iShared->user, sizeof(iShared->user) - 1);
	user.Copy(iSettings.iSshUser);
	user.ZeroTerminate();
	TPtr8 prefix((TUint8*)iShared->dial_prefix, sizeof(iShared->dial_prefix) - 1);
	prefix.Copy(_L8("ATDT"));
	prefix.ZeroTerminate();
	TPtr8 home((TUint8*)iShared->home, sizeof(iShared->home) - 1);
	home.Copy(KSshHome);
	home.ZeroTerminate();
	iCoeEnv->FsSession().MkDirAll(_L("C:\\System\\Apps\\PsiTerm\\"));
	Mem::Copy(iShared->entropy, iEntropy, PSI_ENTROPY_SIZE);
	iShared->entropy_len = PSI_ENTROPY_SIZE;
	iShared->mode = aMode;
	iEverLoggedIn = EFalse;
	iLastState = -1;
	{
	// the key to offer (SSH), or where a new / imported key goes (4, 5)
	TPtr8 kf((TUint8*)iShared->keyfile, sizeof(iShared->keyfile) - 1);
	kf.Zero();
	if (aMode == 0)
		kf.Copy(iKeyBase);
	else if (aMode >= 4)
		kf.Copy(iPendKeyBase);
	kf.ZeroTerminate();
	TPtr8 ks((TUint8*)iShared->keysrc, sizeof(iShared->keysrc) - 1);
	ks.Zero();
	if (aMode == 5)
		ks.Copy(iPendKeySrc);
	ks.ZeroTerminate();
	TPtr8 kn((TUint8*)iShared->keyname, sizeof(iShared->keyname) - 1);
	kn.Zero();
	if (aMode >= 4)
		kn.Copy(iPendKeyName);
	kn.ZeroTerminate();
	}
	iLaunchMode = aMode;
	iShared->net_mode = (aMode != 1 && aMode < 4 && iSettings.iNetMode) ? 1 : 0;
	if (aMode == 3)
		{
		// send screenshots: POST the bundle to the update server
		iShared->port = iSettings.iUpdPort > 0 ? iSettings.iUpdPort : 80;
		host.Copy(iSettings.iUpdHost);
		host.ZeroTerminate();
		TPtr8 path((TUint8*)iShared->path, sizeof(iShared->path) - 1);
		path.Copy(_L8("/upload"));
		path.ZeroTerminate();
		TPtr8 save((TUint8*)iShared->save_as, sizeof(iShared->save_as) - 1);
		save.Copy(iUpdateFile);
		save.ZeroTerminate();
		}
	if (aMode == 2)
		{
		// update: same link as SSH, to GitHub (HTTPS) or the local server
		TPtr8 path((TUint8*)iShared->path, sizeof(iShared->path) - 1);
		if (iSettings.iUpdSource == 0)
			{
			iShared->tls = 1;
			iShared->port = 443;
			host.Copy(KGitHubHost);
			path.Copy(KGitHubPath);
			}
		else
			{
			iShared->port = iSettings.iUpdPort > 0 ? iSettings.iUpdPort : 80;
			host.Copy(iSettings.iUpdHost);
			path.Copy(_L8("/"));
			}
		host.ZeroTerminate();
		path.ZeroTerminate();
		TPtr8 ver((TUint8*)iShared->version, sizeof(iShared->version) - 1);
		ver.Copy(KPsiTermVersion);
		ver.ZeroTerminate();
		// save to the CF card if there is one (D:), else internal memory
		TVolumeInfo vol;
		iUpdateFile.Copy(iCoeEnv->FsSession().Volume(vol, EDriveD) == KErrNone
			? _L("D:\\PsiTerm-update.sis") : _L("C:\\PsiTerm-update.sis"));
		TPtr8 save((TUint8*)iShared->save_as, sizeof(iShared->save_as) - 1);
		save.Copy(iUpdateFile);
		save.ZeroTerminate();
		}
	if (aMode == 0)
		{
		iUserQuit = EFalse;
		TPtr8 cmd((TUint8*)iShared->command, sizeof(iShared->command) - 1);
		cmd.Copy(iLoginCmd);
		cmd.ZeroTerminate();
		if (iSshPassword.Length() > 0)
			iReconnectPw = iSshPassword;
		}
	if (aMode == 0 && iSshPassword.Length() > 0)
		{
		TPtr8 pw((TUint8*)iShared->password, sizeof(iShared->password) - 1);
		pw.Copy(iSshPassword);
		pw.ZeroTerminate();
		}
	iSshPassword.FillZ();
	iSshPassword.Zero();

	// psissh.exe lives next to PsiTerm.app
	TParse parse;
	parse.Set(CEikonEnv::Static()->EikAppUi()->Application()->AppFullName(), NULL, NULL);
	TFileName exe(parse.DriveAndPath());
	exe.Append(KSshExeName);
	r = iSshProcess.Create(exe, KNullDesC);
	// not there (a half-finished install?): try the same folder on C: and D:
	for (TInt d = 0; r == KErrNotFound && d < 2; d++)
		{
		exe[0] = (TText)(d == 0 ? 'C' : 'D');
		r = iSshProcess.Create(exe, KNullDesC);
		}
	if (r != KErrNone)
		{
		TBuf8<200> msg;
		if (r == KErrNotFound)
			msg.Format(_L8("\r\n[PsiTerm's SSH program (psissh.exe) is missing or cannot load.\r\n"
				" Please reinstall PsiTerm from its .sis file.]\r\n"));
		else
			msg.Format(_L8("\r\n[SSH: could not start psissh.exe, error %d]\r\n"), r);
		LocalMessage(msg);
		iChunk.Close();
		iChunkOpen = EFalse;
		iShared = NULL;
		ApplySerialSettings();
		return;
		}
	iSshActive = ETrue;
	if (!iWatcher)
		iWatcher = new(ELeave) CSshWatcher(*this);
	iWatcher->Watch(iSshProcess);
	if (!iPump)
		iPump = CPeriodic::NewL(CActive::EPriorityStandard);
	// every system tick (1/64 s): the old 40 ms poll added up to 3 ticks
	// to every key echo. An idle poll is a couple of compares.
	iPump->Start(15625, 15625, TCallBack(PumpCallback, this));
	iSshProcess.Resume();
	}

TInt CTermView::PumpCallback(TAny* aSelf)
	{
	((CTermView*)aSelf)->PumpSsh();
	return 1;
	}

// Move psissh.exe's terminal output into libvterm.
void CTermView::PumpSsh()
	{
	if (!iShared)
		return;
	if (iShared->out_tail == iShared->out_head)
		return;
	// Feed everything waiting in one paint pass, so libvterm can merge a
	// burst of scrolling into a single blit. Not while a job's output goes
	// to the tool window: that window draws itself, and the terminal's gc
	// must not be active then (WSERV 10).
	TUint8 buf[1024];
	TBool paint = !iCapture;
	if (paint)
		BeginPaint();
	for (TInt rounds = 0; rounds < 8; rounds++)
		{
		// copy out up to sizeof(buf) in one or two runs (the ring may wrap)
		TUint tail = iShared->out_tail;
		TInt n = (TInt)(iShared->out_head - tail);
		if (n <= 0)
			break;
		if (n > (TInt)sizeof(buf))
			n = sizeof(buf);
		TUint off = tail % PSI_OUT_SIZE;
		TInt run = PSI_OUT_SIZE - (TInt)off;
		if (run > n)
			run = n;
		Mem::Copy(buf, iShared->out + off, run);
		if (n > run)
			Mem::Copy(buf + run, iShared->out, n - run);
		iShared->out_tail = tail + n;
		if (iCapture)
			AppendDebug(TPtrC8(buf, n));
		else
			FeedTerminal(buf, n);
		}
	if (paint)
		EndPaint();
	}

void CTermView::DisconnectSsh()
	{
	iUserQuit = ETrue;
	if (iReconnectWait)
		CancelReconnect(_L8("\r\n[Reconnect cancelled]\r\n"));
	if (iSshActive && iShared)
		iShared->quit = 1;
	}

// ----- auto-reconnect ----------------------------------------------------------
// If a logged-in session drops (Psion switched off, WiFi gone, modem hung up)
// PsiTerm redials the same host after a short wait, backing off to 1 minute.
// A start command such as "tmux new -A -s psion" puts you back where you were.

void CTermView::ScheduleReconnect()
	{
	iReconnectTries++;
	if (iReconnectTries > 10)
		{
		CancelReconnect(_L8("\r\n[Could not reconnect - use SSH to... to try again]\r\n"));
		return;
		}
	TInt secs = 5;
	for (TInt i = 1; i < iReconnectTries && secs < 60; i++)
		secs *= 2;
	if (secs > 60)
		secs = 60;
	TBuf8<120> m;
	m.Format(_L8("\r\n[Connection lost - reconnecting in %d s (try %d). Enter: now, Esc: stop]\r\n"),
		secs, iReconnectTries);
	LocalMessage(m);
	if (!iReconnectTimer)
		iReconnectTimer = CPeriodic::New(CActive::EPriorityStandard);
	if (!iReconnectTimer)
		return;
	iReconnectTimer->Cancel();
	iReconnectTimer->Start(secs * 1000000, secs * 1000000, TCallBack(ReconnectCallback, this));
	iReconnectAt.HomeTime();
	iReconnectAt += TTimeIntervalSeconds(secs);
	iReconnectWait = ETrue;
	}

void CTermView::CancelReconnect(const TDesC8& aWhy)
	{
	if (iReconnectTimer)
		iReconnectTimer->Cancel();
	iReconnectWait = EFalse;
	iReconnecting = EFalse;
	iReconnectTries = 0;
	iReconnectPw.FillZ();
	iReconnectPw.Zero();
	LocalMessage(aWhy);
	}

TInt CTermView::ReconnectCallback(TAny* aSelf)
	{
	TRAP_IGNORE(((CTermView*)aSelf)->ReconnectNowL());
	return 0;
	}

void CTermView::ReconnectNowL()
	{
	if (iReconnectTimer)
		iReconnectTimer->Cancel();
	if (!iReconnectWait || iSshActive)
		return;
	iReconnectWait = EFalse;
	iReconnecting = ETrue;
	iSshPassword = iReconnectPw;
	LaunchSshL();
	}

void CTermView::SshProcessEnded()
	{
	PumpSsh();
	if (iPump)
		iPump->Cancel();
	TInt reason = iSshProcess.ExitReason();
	TExitType type = iSshProcess.ExitType();
	TExitCategoryName category(iSshProcess.ExitCategory());
	TInt stage = iShared ? iShared->state : -1;
	TInt exitCode = iShared ? iShared->exit_code : -1;
	TInt lost = iShared ? iShared->lost_link : 0;
	iSshProcess.Close();
	iShared = NULL;
	if (iChunkOpen)
		{
		iChunk.Close();
		iChunkOpen = EFalse;
		}
	iSshActive = EFalse;
	iModemOnline = EFalse;              // psissh hangs up as it ends
	iMouseMode = VTERM_PROP_MOUSE_NONE;  // whatever asked for the mouse has gone
	ParseTmuxTabs();                     // no session: no tabs
	iLastRx = 0;
	iRxTail.Zero();
	TBuf8<160> msg;
	if (type == EExitPanic)
		{
		static const char* const KStage[] = { "starting", "dialling", "setting up encryption", "connected", "finished" };
		TBuf8<16> cat;
		cat.Copy(category);
		const char* st = (stage >= 0 && stage <= 4) ? KStage[stage] : "unknown";
		msg.Format(_L8("\r\n[SSH program crashed: %S %d, while %s]\r\n"), &cat, reason, st);
		}
	else if (iLaunchMode != 0)
		msg.Zero();                      // the tool has said how it went
	else
		msg.Format(_L8("\r\n[SSH program finished]\r\n"));
	LocalMessage(msg);
	ApplySerialSettings();
	if (iLaunchMode == 3)
		{
		iCoeEnv->FsSession().Delete(iUpdateFile);        // the bundle
		if (type != EExitPanic && exitCode == 11)
			{
			TBuf8<64> m;
			m.Format(_L8("[%d screenshot(s) sent]\r\n"), DeleteShots());
			LocalMessage(m);
			}
		}
	if (iLaunchMode == 2 && type != EExitPanic && exitCode == 10)
		{
		LocalMessage(_L8("\r\nStarting the installer - PsiTerm will close.\r\n"));
		iInstallPending = ETrue;
		if (iToolDlg)
			{
			// the installer starts when the window has closed. Closing it
			// deletes it, so nothing below may touch it (FinishL on the
			// deleted window was a KERN-EXEC 3 at the end of every update)
			CToolDialog* dlg = iToolDlg;
			iToolDlg = NULL;
			TRAP_IGNORE(dlg->CloseL());
			}
		}
	if ((iLaunchMode == 4 || iLaunchMode == 5) && type != EExitPanic && exitCode == 0)
		{
		HBufC8* buf = HBufC8::New(2000);
		if (buf)
			{
			TPtr8 t = buf->Des();
			AppendKeyHelp(iCoeEnv->FsSession(), t, iPendKeyBase, iPendKeyName);
			LocalMessage(t);
			delete buf;
			}
		}
	if (iToolDlg)
		TRAP_IGNORE(iToolDlg->FinishL());
	if (iLaunchMode == 0)
		{
		if (!iUserQuit && iSettings.iAutoReconnect && type != EExitPanic && !iPendingCmd
			&& (lost == 1 || (iReconnecting && lost == 2)))
			{
			if (lost == 1)
				iReconnectTries = 0;          // it was working: start the back-off again
			ScheduleReconnect();
			}
		else
			{
			iReconnecting = EFalse;
			iReconnectTries = 0;
			iReconnectPw.FillZ();
			iReconnectPw.Zero();
			if (iPendingCmd)
				;
			else if (iEverLoggedIn || iUserQuit)
				ShowWelcome();                   // back to the start screen
			else
				{
				// never got in: keep the messages that say why on screen
				LocalMessage(_L8("\r\n[Not connected - the lines above say why. "
					"1-9 or Shift+Ctrl+S to try again.]\r\n"));
				iWelcome = iHosts && iHosts->Count() > 0;
				}
			}
		}
	if (iPendingCmd)
		{
		// a Debug tool was waiting for the SSH session to end
		if (!iPendingIdle)
			iPendingIdle = CIdle::New(CActive::EPriorityStandard);
		if (iPendingIdle)
			{
			iPendingIdle->Cancel();
			iPendingIdle->Start(TCallBack(PendingCallback, this));
			}
		}
	}

CSshWatcher::CSshWatcher(CTermView& aView)
	: CActive(EPriorityStandard), iView(aView)
	{
	CActiveScheduler::Add(this);
	}

CSshWatcher::~CSshWatcher()
	{
	Cancel();
	}

void CSshWatcher::Watch(RProcess& aProcess)
	{
	iProcess = &aProcess;
	aProcess.Logon(iStatus);
	SetActive();
	}

void CSshWatcher::RunL()
	{
	iView.SshProcessEnded();
	}

void CSshWatcher::DoCancel()
	{
	if (iProcess)
		iProcess->LogonCancel(iStatus);
	}

// EPOC R5's TDesC::Left(n) panics (USER 22) when n > Length(), unlike later
// Symbian versions which clamp. Always go through this.

// ===========================================================================
// Saved hosts
// ===========================================================================

CHostList* CHostList::NewL(RFs& aFs)
	{
	CHostList* self = new(ELeave) CHostList(aFs);
	CleanupStack::PushL(self);
	self->iEntries = new(ELeave) CArrayFixFlat<THostEntry>(4);
	CleanupStack::Pop();
	return self;
	}

CHostList::~CHostList()
	{
	if (iEntries)
		{
		for (TInt i = 0; i < iEntries->Count(); i++)
			(*iEntries)[i].iPassword.FillZ();
		delete iEntries;
		}
	}

void CHostList::AddL(const THostEntry& aEntry)
	{
	iEntries->AppendL(aEntry);
	}

void CHostList::Delete(TInt aIndex)
	{
	(*iEntries)[aIndex].iPassword.FillZ();
	iEntries->Delete(aIndex);
	if (iLast >= iEntries->Count())
		iLast = iEntries->Count() - 1;
	if (iLast < 0)
		iLast = 0;
	}

// Scrambling key: this Psion's unique ID mixed with a per-file salt.
TUint32 CHostList::KeyFor(TUint32 aSalt) const
	{
	TMachineInfoV1Buf info;
	TUint32 id = 0x5053494fu;
	if (UserHal::MachineInfo(info) == KErrNone)
		id ^= info().iMachineUniqueId.Low() ^ (info().iMachineUniqueId.High() * 2654435761u);
	TUint32 k = id ^ (aSalt * 2246822519u);
	return k ? k : 0x9e3779b9u;
	}

static void Scramble(TDes8& aData, TUint32 aKey)
	{
	TUint32 x = aKey;
	for (TInt i = 0; i < aData.Length(); i++)
		{
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;      // xorshift32
		aData[i] = (TUint8)(aData[i] ^ (x >> 24));
		}
	}

static void PutStr(TDes8& aOut, const TDesC& aText)
	{
	TBuf8<128> tmp;
	tmp.Copy(LeftSafe(aText, 127));
	aOut.Append((TUint8)tmp.Length());
	aOut.Append(tmp);
	}

static TBool GetStr(const TDesC8& aIn, TInt& aPos, TDes& aText)
	{
	if (aPos >= aIn.Length())
		return EFalse;
	TInt len = aIn[aPos++];
	if (aPos + len > aIn.Length() || len > aText.MaxLength())
		return EFalse;
	aText.Copy(aIn.Mid(aPos, len));
	aPos += len;
	return ETrue;
	}

// Hosts.dat: "PH" 1 count last salt[4], then per host:
//   name host user (length-prefixed), port (2 bytes), password (length-prefixed, scrambled)
void CHostList::Load()
	{
	iEntries->Reset();
	iLast = 0;
	RFile file;
	if (file.Open(iFs, KHostsFile, EFileRead) != KErrNone)
		return;
	HBufC8* buf = HBufC8::New(KMaxHosts * 400 + 16);
	if (!buf)
		{
		file.Close();
		return;
		}
	TPtr8 data(buf->Des());
	TInt r = file.Read(data);
	file.Close();
	if (r != KErrNone || data.Length() < 9 || data[0] != 'P' || data[1] != 'H' || (data[2] < 1 || data[2] > 4))
		{
		delete buf;
		return;
		}
	TInt fileVer = data[2];
	TInt count = data[3];
	iLast = data[4];
	TUint32 salt = data[5] | (data[6] << 8) | (data[7] << 16) | (data[8] << 24);
	TUint32 key = KeyFor(salt);
	TInt pos = 9;
	for (TInt i = 0; i < count && i < KMaxHosts; i++)
		{
		THostEntry e;
		if (!GetStr(data, pos, e.iName) || !GetStr(data, pos, e.iHost) || !GetStr(data, pos, e.iUser))
			break;
		if (pos + 3 > data.Length())
			break;
		e.iPort = data[pos] | (data[pos + 1] << 8);
		pos += 2;
		TInt len = data[pos++];
		if (pos + len > data.Length() || len > e.iPassword.MaxLength())
			break;
		TBuf8<64> pw(data.Mid(pos, len));
		pos += len;
		Scramble(pw, key + (TUint32)i * 0x10001u);
		e.iPassword.Copy(pw);
		pw.FillZ();
		if (fileVer >= 2 && !GetStr(data, pos, e.iCommand))
			break;
		// before v3 a saved password meant "use it"; otherwise the key was
		// tried first, then the password asked for
		e.iAuth = e.iPassword.Length() ? 2 : 0;
		if (fileVer >= 3 && pos < data.Length())
			e.iAuth = data[pos++] <= 3 ? data[pos - 1] : 0;
		e.iKeyId = 0;
		if (fileVer >= 4 && pos < data.Length())
			e.iKeyId = data[pos++];
		TRAPD(err, iEntries->AppendL(e));
		e.iPassword.FillZ();
		if (err != KErrNone)
			break;
		}
	data.FillZ();
	delete buf;
	if (iLast >= iEntries->Count())
		iLast = 0;
	}

TInt CHostList::Save()
	{
	HBufC8* buf = HBufC8::New(KMaxHosts * 400 + 16);
	if (!buf)
		return KErrNoMemory;
	TPtr8 data(buf->Des());
	TUint32 salt = (TUint32)User::TickCount() * 2654435761u ^ (TUint32)(TInt)this;
	TTime now;
	now.HomeTime();
	salt ^= now.Int64().Low();
	TUint32 key = KeyFor(salt);
	data.Append('P');
	data.Append('H');
	data.Append(4);
	data.Append((TUint8)iEntries->Count());
	data.Append((TUint8)iLast);
	for (TInt b = 0; b < 4; b++)
		data.Append((TUint8)(salt >> (8 * b)));
	for (TInt i = 0; i < iEntries->Count(); i++)
		{
		const THostEntry& e = (*iEntries)[i];
		PutStr(data, e.iName);
		PutStr(data, e.iHost);
		PutStr(data, e.iUser);
		data.Append((TUint8)(e.iPort & 0xff));
		data.Append((TUint8)(e.iPort >> 8));
		TBuf8<64> pw;
		pw.Copy(e.iPassword);
		Scramble(pw, key + (TUint32)i * 0x10001u);
		data.Append((TUint8)pw.Length());
		data.Append(pw);
		pw.FillZ();
		PutStr(data, e.iCommand);
		data.Append((TUint8)e.iAuth);
		data.Append((TUint8)e.iKeyId);
		}
	iFs.MkDirAll(KHostsFile);
	RFile file;
	TInt r = file.Replace(iFs, KHostsFile, EFileWrite);
	if (r == KErrNone)
		{
		r = file.Write(data);
		file.Close();
		}
	data.FillZ();
	delete buf;
	return r;
	}

// ----- SSH keys -------------------------------------------------------------
// Keys.dat: "PK" 1 count last, then per key: id, name. The key files are
// Keys\k<id>.key (private, Dropbear format), .pub (OpenSSH line) and .fp
// (SHA256 fingerprint), all written by psissh.

CKeyList* CKeyList::NewL(RFs& aFs)
	{
	CKeyList* self = new(ELeave) CKeyList(aFs);
	CleanupStack::PushL(self);
	self->iEntries = new(ELeave) CArrayFixFlat<TSshKey>(4);
	CleanupStack::Pop();
	return self;
	}

CKeyList::~CKeyList()
	{
	delete iEntries;
	}

void CKeyList::Base(TInt aId, TDes& aBase)
	{
	aBase.Copy(KKeysDir);
	aBase.Append('k');
	aBase.AppendNum(aId);
	}

TInt CKeyList::Find(TInt aId) const
	{
	for (TInt i = 0; i < iEntries->Count(); i++)
		if ((*iEntries)[i].iId == aId)
			return i;
	return -1;
	}

TInt CKeyList::AddL(const TDesC& aName)
	{
	TInt id = 1;
	for (TInt i = 0; i < iEntries->Count(); i++)
		if ((*iEntries)[i].iId >= id)
			id = (*iEntries)[i].iId + 1;
	TSshKey k;
	k.iId = id;
	k.iName = LeftSafe(aName, k.iName.MaxLength());
	iEntries->AppendL(k);
	return id;
	}

void CKeyList::Delete(TInt aIndex)
	{
	TFileName f;
	Base((*iEntries)[aIndex].iId, f);
	TInt n = f.Length();
	f.Append(_L(".key")); iFs.Delete(f); f.SetLength(n);
	f.Append(_L(".pub")); iFs.Delete(f); f.SetLength(n);
	f.Append(_L(".fp"));  iFs.Delete(f);
	iEntries->Delete(aIndex);
	if (iLast >= iEntries->Count())
		iLast = 0;
	}

TBool CKeyList::HasFile(TInt aIndex)
	{
	TFileName f;
	Base((*iEntries)[aIndex].iId, f);
	f.Append(_L(".key"));
	TEntry e;
	return iFs.Entry(f, e) == KErrNone;
	}

void CKeyList::Fingerprint(TInt aIndex, TDes& aFp)
	{
	aFp.Zero();
	TFileName f;
	Base((*iEntries)[aIndex].iId, f);
	f.Append(_L(".fp"));
	RFile file;
	if (file.Open(iFs, f, EFileRead) != KErrNone)
		return;
	TBuf8<64> b;
	file.Read(b);
	file.Close();
	aFp.Copy(b.Left(aFp.MaxLength() < b.Length() ? aFp.MaxLength() : b.Length()));
	}

void CKeyList::Load()
	{
	iEntries->Reset();
	iLast = 0;
	RFile file;
	if (file.Open(iFs, KKeysFile, EFileRead) != KErrNone)
		return;
	TBuf8<512> data;
	TInt r = file.Read(data);
	file.Close();
	if (r != KErrNone || data.Length() < 5 || data[0] != 'P' || data[1] != 'K' || data[2] != 1)
		return;
	TInt count = data[3];
	iLast = data[4];
	TInt pos = 5;
	for (TInt i = 0; i < count && i < KMaxKeys && pos < data.Length(); i++)
		{
		TSshKey k;
		k.iId = data[pos++];
		if (!GetStr(data, pos, k.iName))
			break;
		TRAPD(err, iEntries->AppendL(k));
		if (err != KErrNone)
			break;
		}
	if (iLast >= iEntries->Count())
		iLast = 0;
	}

TInt CKeyList::Save()
	{
	TBuf8<512> data;
	data.Append('P');
	data.Append('K');
	data.Append(1);
	data.Append((TUint8)iEntries->Count());
	data.Append((TUint8)iLast);
	for (TInt i = 0; i < iEntries->Count(); i++)
		{
		data.Append((TUint8)(*iEntries)[i].iId);
		PutStr(data, (*iEntries)[i].iName);
		}
	iFs.MkDirAll(KKeysFile);
	RFile file;
	TInt r = file.Replace(iFs, KKeysFile, EFileWrite);
	if (r == KErrNone)
		{
		r = file.Write(data);
		file.Close();
		}
	return r;
	}

// 0.44-0.49 kept one key as id_ed25519(.pub): it becomes key 1, "Psion key"
void CKeyList::MigrateL()
	{
	TFileName old(KOldKey);
	TEntry e;
	if (iEntries->Count() > 0 || iFs.Entry(old, e) != KErrNone)
		return;
	iFs.MkDirAll(KKeysDir);
	TInt id = AddL(_L("Psion key"));
	TFileName to;
	Base(id, to);
	TInt n = to.Length();
	to.Append(_L(".key"));
	iFs.Rename(old, to);
	to.SetLength(n);
	to.Append(_L(".pub"));
	old.Append(_L(".pub"));
	iFs.Rename(old, to);
	Save();
	}

// ----- snippets -------------------------------------------------------------

CSnippetList* CSnippetList::NewL(RFs& aFs)
	{
	CSnippetList* self = new(ELeave) CSnippetList(aFs);
	CleanupStack::PushL(self);
	self->iEntries = new(ELeave) CArrayFixFlat<TSnippet>(4);
	CleanupStack::Pop();
	return self;
	}

CSnippetList::~CSnippetList()
	{
	delete iEntries;
	}

TInt CSnippetList::FindKey(TInt aKey) const
	{
	if (aKey == 0)
		return -1;
	for (TInt i = 0; i < iEntries->Count(); i++)
		if ((*iEntries)[i].iKey == aKey)
			return i;
	return -1;
	}

static void AddSnippetL(CArrayFixFlat<TSnippet>& aList, const TDesC& aName,
	const TDesC& aText, TInt aEnter, TInt aKey)
	{
	TSnippet s;
	s.iName = aName;
	s.iText = aText;
	s.iEnter = aEnter;
	s.iKey = aKey;
	aList.AppendL(s);
	}

// A few useful ones to start with; all can be edited or deleted
void CSnippetList::AddDefaultsL()
	{
	AddSnippetL(*iEntries, _L("Claude Code"), _L("claude"), 1, '1');
	AddSnippetL(*iEntries, _L("Claude: continue"), _L("claude --continue"), 1, '2');
	AddSnippetL(*iEntries, _L("tmux: attach"), _L("tmux new -A -s psion"), 1, '3');
	AddSnippetL(*iEntries, _L("Git status"), _L("git status"), 1, 0);
	AddSnippetL(*iEntries, _L("Disk space"), _L("df -h"), 1, 0);
	}

// Snippets.dat: "PN" 1 count last, then per snippet:
//   name text (length-prefixed), enter (1 byte), key (1 byte)
void CSnippetList::Load()
	{
	iEntries->Reset();
	iLast = 0;
	RFile file;
	if (file.Open(iFs, KSnippetsFile, EFileRead) != KErrNone)
		{
		TRAP_IGNORE(AddDefaultsL());
		return;
		}
	HBufC8* buf = HBufC8::New(KMaxSnippets * 160 + 16);
	if (!buf)
		{
		file.Close();
		return;
		}
	TPtr8 data(buf->Des());
	TInt r = file.Read(data);
	file.Close();
	if (r == KErrNone && data.Length() >= 5 && data[0] == 'P' && data[1] == 'N' && data[2] == 1)
		{
		TInt count = data[3];
		iLast = data[4];
		TInt pos = 5;
		for (TInt i = 0; i < count && i < KMaxSnippets; i++)
			{
			TSnippet s;
			if (!GetStr(data, pos, s.iName) || !GetStr(data, pos, s.iText) || pos + 2 > data.Length())
				break;
			s.iEnter = data[pos++] ? 1 : 0;
			s.iKey = data[pos++];
			if (SnippetKeyIndex(s.iKey) < 0)
				s.iKey = 0;
			TRAPD(err, iEntries->AppendL(s));
			if (err != KErrNone)
				break;
			}
		}
	delete buf;
	if (iLast >= iEntries->Count())
		iLast = 0;
	}

TInt CSnippetList::Save()
	{
	HBufC8* buf = HBufC8::New(KMaxSnippets * 160 + 16);
	if (!buf)
		return KErrNoMemory;
	TPtr8 data(buf->Des());
	data.Append('P');
	data.Append('N');
	data.Append(1);
	data.Append((TUint8)iEntries->Count());
	data.Append((TUint8)iLast);
	for (TInt i = 0; i < iEntries->Count(); i++)
		{
		const TSnippet& s = (*iEntries)[i];
		PutStr(data, s.iName);
		PutStr(data, s.iText);
		data.Append((TUint8)(s.iEnter ? 1 : 0));
		data.Append((TUint8)s.iKey);
		}
	iFs.MkDirAll(KSnippetsFile);
	RFile file;
	TInt r = file.Replace(iFs, KSnippetsFile, EFileWrite);
	if (r == KErrNone)
		{
		r = file.Write(data);
		file.Close();
		}
	delete buf;
	return r;
	}

// Hotkeys: Shift+Ctrl + 1..9, 0, then the letters the menus leave free
static const char KSnippetKeys[] = "1234567890ABDFGIJKLMNOQRUWXYZ";

TInt SnippetKeyCount()
	{
	return (TInt)sizeof(KSnippetKeys) - 1 + 1;     // + "none"
	}

TInt SnippetKeyAt(TInt aIndex)
	{
	if (aIndex <= 0 || aIndex >= SnippetKeyCount())
		return 0;
	return KSnippetKeys[aIndex - 1];
	}

TInt SnippetKeyIndex(TInt aKey)
	{
	if (aKey == 0)
		return 0;
	for (TInt i = 1; i < SnippetKeyCount(); i++)
		if (KSnippetKeys[i - 1] == aKey)
			return i;
	return -1;
	}

void SnippetKeyName(TInt aKey, TDes& aText)
	{
	aText.Zero();
	if (aKey == 0)
		{
		aText.Append(_L("None"));
		return;
		}
	aText.Append(_L("Shift+Ctrl+"));
	aText.Append((TChar)aKey);
	}

// keeps a dialog on the 640x240 screen (EIKON centres whatever size it asks for)
static TSize ClampToScreen(const TSize& aSize)
	{
	TSize screen = CEikonEnv::Static()->ScreenDevice()->SizeInPixels();
	return TSize(aSize.iWidth < screen.iWidth - 8 ? aSize.iWidth : screen.iWidth - 8,
		aSize.iHeight < screen.iHeight - 8 ? aSize.iHeight : screen.iHeight - 8);
	}

void CSnippetListDialog::SetSizeAndPositionL(const TSize& aSize) { SetCornerAndSizeL(EHCenterVCenter, ClampToScreen(aSize)); }

void CSnippetListDialog::PreLayoutDynInitL()
	{
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(4);
	CleanupStack::PushL(names);
	for (TInt i = 0; i < iList.Count(); i++)
		{
		const TSnippet& s = iList.At(i);
		TBuf<40> line;
		if (s.iKey)
			{
			line.Append((TChar)s.iKey);
			line.Append(_L("  "));
			}
		else
			line.Append(_L("    "));
		line.Append(s.iName);
		names->AppendL(line);
		}
	CEikChoiceList* list = (CEikChoiceList*)Control(EPtDlgSnipList);
	list->SetArrayL(names);
	list->SetArrayExternalOwnership(EFalse);
	CleanupStack::Pop();                      // names: now owned by the list
	TInt current = iIndex;
	if (current < 0 || current >= iList.Count())
		current = 0;
	list->SetCurrentItem(current);
	}

TBool CSnippetListDialog::OkToExitL(TInt aButtonId)
	{
	iIndex = ChoiceListCurrentItem(EPtDlgSnipList);
	iAction = (aButtonId == EEikBidCancel) ? 0 : aButtonId;
	return ETrue;
	}

void CSnippetEditDialog::SetSizeAndPositionL(const TSize& aSize) { SetCornerAndSizeL(EHCenterVCenter, ClampToScreen(aSize)); }

void CSnippetEditDialog::PreLayoutDynInitL()
	{
	SetEdwinTextL(EPtDlgSnipName, &iEntry.iName);
	SetEdwinTextL(EPtDlgSnipText, &iEntry.iText);
	((CEikChoiceList*)Control(EPtDlgSnipEnter))->SetCurrentItem(iEntry.iEnter ? 1 : 0);
	CDesCArrayFlat* keys = new(ELeave) CDesCArrayFlat(8);
	CleanupStack::PushL(keys);
	for (TInt i = 0; i < SnippetKeyCount(); i++)
		{
		TBuf<20> name;
		SnippetKeyName(SnippetKeyAt(i), name);
		keys->AppendL(name);
		}
	CEikChoiceList* list = (CEikChoiceList*)Control(EPtDlgSnipKey);
	list->SetArrayL(keys);
	list->SetArrayExternalOwnership(EFalse);
	CleanupStack::Pop();                      // keys: now owned by the list
	TInt k = SnippetKeyIndex(iEntry.iKey);
	list->SetCurrentItem(k > 0 ? k : 0);
	}

TBool CSnippetEditDialog::OkToExitL(TInt /*aButtonId*/)
	{
	TBuf<24> name;
	TBuf<120> text;
	GetEdwinText(name, EPtDlgSnipName);
	GetEdwinText(text, EPtDlgSnipText);
	name.Trim();
	if (text.Length() == 0)
		{
		CEikonEnv::Static()->InfoMsg(_L("Enter the text to send"));
		TryChangeFocusToL(EPtDlgSnipText);
		return EFalse;
		}
	TInt key = SnippetKeyAt(((CEikChoiceList*)Control(EPtDlgSnipKey))->CurrentItem());
	TInt other = iList.FindKey(key);
	if (key && other >= 0 && other != iSelf)
		{
		TBuf<60> m(_L("That key is used by "));
		m.Append(iList.At(other).iName);
		CEikonEnv::Static()->InfoMsg(m);
		TryChangeFocusToL(EPtDlgSnipKey);
		return EFalse;
		}
	if (name.Length())
		iEntry.iName = name;
	else
		iEntry.iName = LeftSafe(text, iEntry.iName.MaxLength());
	iEntry.iText = text;
	iEntry.iEnter = ((CEikChoiceList*)Control(EPtDlgSnipEnter))->CurrentItem() == 1;
	iEntry.iKey = key;
	return ETrue;
	}

// ----- "SSH to" host list ---------------------------------------------------

CHostListDialog::CHostListDialog(CHostList& aHosts, TInt& aIndex, TInt& aAction)
	: iHosts(aHosts), iIndex(aIndex), iAction(aAction)
	{
	iAction = 0;
	}

void CHostListDialog::SetSizeAndPositionL(const TSize& aSize) { SetCornerAndSizeL(EHCenterVCenter, ClampToScreen(aSize)); }
void CHostEditDialog::SetSizeAndPositionL(const TSize& aSize) { SetCornerAndSizeL(EHCenterVCenter, ClampToScreen(aSize)); }
void CConnDialog::SetSizeAndPositionL(const TSize& aSize) { SetCornerAndSizeL(EHCenterVCenter, ClampToScreen(aSize)); }
void CUpdateDialog::SetSizeAndPositionL(const TSize& aSize) { SetCornerAndSizeL(EHCenterVCenter, ClampToScreen(aSize)); }

void CHostListDialog::PreLayoutDynInitL()
	{
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(4);
	CleanupStack::PushL(names);
	for (TInt i = 0; i < iHosts.Count(); i++)
		{
		const THostEntry& e = iHosts.At(i);
		TBuf<48> line;
		if (e.iName.Length())
			line = e.iName;
		else
			line = LeftSafe(e.iHost, 24);
		if (e.iPassword.Length())
			line.Append(_L(" *"));          // has a saved password
		// "name - user@host", as much as fits
		TBuf<80> where(e.iUser);
		where.Append('@');
		where.Append(LeftSafe(e.iHost, 60));
		if (e.iName.CompareF(e.iHost) == 0 && e.iUser.Length())
			{
			// named after the host: just "user@host"
			line = LeftSafe(where, line.MaxLength() - 2);
			if (e.iPassword.Length())
				line.Append(_L(" *"));
			}
		else if (e.iName.CompareF(e.iHost) != 0 && line.Length() + 3 < line.MaxLength())
			{
			line.Append(_L(" - "));
			line.Append(LeftSafe(where, line.MaxLength() - line.Length()));
			}
		names->AppendL(line);
		}
	CEikChoiceList* list = (CEikChoiceList*)Control(EPtDlgHostList);
	list->SetArrayL(names);
	list->SetArrayExternalOwnership(EFalse);
	CleanupStack::Pop();                      // names: now owned by the list
	TInt current = iIndex;
	if (current < 0 || current >= iHosts.Count())
		current = 0;
	list->SetCurrentItem(current);
	}

TBool CHostListDialog::OkToExitL(TInt aButtonId)
	{
	iIndex = ChoiceListCurrentItem(EPtDlgHostList);
	iAction = (aButtonId == EEikBidCancel) ? 0 : aButtonId;
	return ETrue;
	}

// ----- add / edit one host ---------------------------------------------------

CHostEditDialog::CHostEditDialog(THostEntry& aEntry, CKeyList& aKeys)
	: iEntry(aEntry), iKeys(aKeys)
	{
	}

void CHostEditDialog::PreLayoutDynInitL()
	{
	SetEdwinTextL(EPtDlgName, &iEntry.iName);
	// the port rides along as host:port, which saves a line on the screen
	TBuf<108> host(iEntry.iHost);
	if (iEntry.iPort > 0 && iEntry.iPort != 22)
		host.AppendFormat(_L(":%d"), iEntry.iPort);
	SetEdwinTextL(EPtDlgHost, &host);
	SetEdwinTextL(EPtDlgUser, &iEntry.iUser);
	SetEdwinTextL(EPtDlgStartCmd, &iEntry.iCommand);
	// (the secret editor holds at most CEikSecretEditor::EMaxSecEdLength = 32
	//  characters; its limit is set in the resource - more panics EIKON 12)
	// "Log in with": each key (alone, or then the saved password), then the
	// two password choices
	CDesCArrayFlat* opts = new(ELeave) CDesCArrayFlat(8);
	CleanupStack::PushL(opts);
	iOptCount = 0;
	TInt current = -1;
	for (TInt i = 0; i < iKeys.Count() && i < KMaxKeys; i++)
		{
		TBool mine = (iEntry.iKeyId == iKeys.At(i).iId) || (iEntry.iKeyId == 0 && i == 0);
		for (TInt pass = 0; pass < 2; pass++)
			{
			TBuf<48> line(_L("Key: "));
			line.Append(iKeys.At(i).iName);
			if (pass)
				line.Append(_L(" + saved password"));
			opts->AppendL(line);
			iOptAuth[iOptCount] = pass ? 3 : 0;
			iOptKey[iOptCount] = iKeys.At(i).iId;
			if (mine && iEntry.iAuth == iOptAuth[iOptCount] && current < 0)
				current = iOptCount;
			iOptCount++;
			}
		}
	opts->AppendL(_L("Password (ask)"));
	iOptAuth[iOptCount] = 1;
	iOptKey[iOptCount++] = 0;
	opts->AppendL(_L("Saved password"));
	iOptAuth[iOptCount] = 2;
	iOptKey[iOptCount++] = 0;
	if (current < 0)
		current = (iEntry.iAuth == 2 || iEntry.iAuth == 3) ? iOptCount - 1 : iOptCount - 2;
	CEikChoiceList* list = (CEikChoiceList*)Control(EPtDlgAuth);
	list->SetArrayL(opts);
	list->SetArrayExternalOwnership(EFalse);
	CleanupStack::Pop();
	list->SetCurrentItem(current);
	}

TBool CHostEditDialog::OkToExitL(TInt /*aButtonId*/)
	{
	TBuf<108> host;
	TBuf<60> user;
	GetEdwinText(host, EPtDlgHost);
	GetEdwinText(user, EPtDlgUser);
	host.Trim();
	user.Trim();
	TInt port = 22;
	TInt colon = host.LocateReverse(':');
	if (colon > 0 && colon < host.Length() - 1)
		{
		TLex lex(host.Mid(colon + 1));
		TInt p;
		if (lex.Val(p) == KErrNone && lex.Eos() && p > 0 && p < 65536)
			{
			port = p;
			host.SetLength(colon);
			}
		}
	if (host.Length() > 100)
		host.SetLength(100);
	if (host.Length() == 0)
		{
		CEikonEnv::Static()->InfoMsg(_L("Enter a host name or IP address"));
		TryChangeFocusToL(EPtDlgHost);
		return EFalse;
		}
	if (user.Length() == 0)
		{
		CEikonEnv::Static()->InfoMsg(_L("Enter a user name"));
		TryChangeFocusToL(EPtDlgUser);
		return EFalse;
		}
	iEntry.iHost = host;
	iEntry.iUser = user;
	GetEdwinText(iEntry.iName, EPtDlgName);
	iEntry.iName.Trim();
	if (iEntry.iName.Length() == 0)
		iEntry.iName = LeftSafe(host, iEntry.iName.MaxLength());
	iEntry.iPort = port;
	GetEdwinText(iEntry.iCommand, EPtDlgStartCmd);
	iEntry.iCommand.Trim();
	TBuf<63> typed;
	GetSecretEditorText(typed, EPtDlgPassword);
	TInt opt = ((CEikChoiceList*)Control(EPtDlgAuth))->CurrentItem();
	if (opt < 0 || opt >= iOptCount)
		opt = iOptCount - 2;
	TInt auth = iOptAuth[opt];
	iEntry.iKeyId = iOptKey[opt];
	if (auth == 2 || auth == 3)          // the password is remembered
		{
		if (typed.Length())                 // blank keeps the saved password
			iEntry.iPassword = typed;
		if (iEntry.iPassword.Length() == 0)
			{
			typed.FillZ();
			CEikonEnv::Static()->InfoMsg(_L("Type the password to save"));
			TryChangeFocusToL(EPtDlgPassword);
			return EFalse;
			}
		}
	else
		{
		iEntry.iPassword.FillZ();          // not remembered: the key, or ask each time
		iEntry.iPassword.Zero();
		}
	iEntry.iAuth = auth;
	typed.FillZ();
	return ETrue;
	}

// ----- About box ---------------------------------------------------------------

CAboutDialog::CAboutDialog(const TDesC& aStatus)
	: iStatus(aStatus)
	{
	}

void CAboutDialog::SetSizeAndPositionL(const TSize& aSize)
	{
	TSize screen = iEikonEnv->ScreenDevice()->SizeInPixels();
	TSize size(aSize.iWidth < screen.iWidth - 8 ? aSize.iWidth : screen.iWidth - 8,
		aSize.iHeight < screen.iHeight - 8 ? aSize.iHeight : screen.iHeight - 8);
	SetCornerAndSizeL(EHCenterVCenter, size);
	}

void CAboutDialog::PreLayoutDynInitL()
	{
	TBuf<32> title(_L("PsiTerm "));
	title.Append(KPsiTermVersion);
	SetLabelL(EPtDlgAbout1, title);
	SetLabelL(EPtDlgAboutStatus, iStatus);
	}

// ----- update server dialog -----------------------------------------------------

void CConnDialog::PreLayoutDynInitL()
	{
	((CEikChoiceList*)Control(EPtDlgBaud))->SetCurrentItem(iSettings.iBaudIndex);
	((CEikChoiceList*)Control(EPtDlgFlow))->SetCurrentItem(iSettings.iRtsCts ? 1 : 0);
	((CEikChoiceList*)Control(EPtDlgLink))->SetCurrentItem(iSettings.iNetMode ? 1 : 0);
	((CEikChoiceList*)Control(EPtDlgReconnect))->SetCurrentItem(iSettings.iAutoReconnect ? 1 : 0);
	}

TBool CConnDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iSettings.iBaudIndex = ((CEikChoiceList*)Control(EPtDlgBaud))->CurrentItem();
	iSettings.iRtsCts = ((CEikChoiceList*)Control(EPtDlgFlow))->CurrentItem() == 1;
	iSettings.iNetMode = ((CEikChoiceList*)Control(EPtDlgLink))->CurrentItem() == 1;
	iSettings.iAutoReconnect = ((CEikChoiceList*)Control(EPtDlgReconnect))->CurrentItem() == 1;
	return ETrue;
	}

// EIKON sizes a dialog to its contents and centres it; if that is bigger
// than the 640x240 screen the title and buttons end up off-screen and the
// (modal) dialog looks like a frozen, shifted terminal. Keep it on screen.
void CAppearanceDialog::SetSizeAndPositionL(const TSize& aSize) { SetCornerAndSizeL(EHCenterVCenter, ClampToScreen(aSize)); }

void CAppearanceDialog::PreLayoutDynInitL()
	{
	((CEikChoiceList*)Control(EPtDlgTheme))->SetCurrentItem(iSettings.iTheme);
	((CEikChoiceList*)Control(EPtDlgCursor))->SetCurrentItem(iSettings.iCursor);
	((CEikChoiceList*)Control(EPtDlgBlink))->SetCurrentItem(iSettings.iBlink ? 1 : 0);
	((CEikChoiceList*)Control(EPtDlgStatus))->SetCurrentItem(iSettings.iStatus ? 1 : 0);
	((CEikChoiceList*)Control(EPtDlgBell))->SetCurrentItem(iSettings.iBell ? 0 : 1);
	((CEikChoiceList*)Control(EPtDlgStartScreen))->SetCurrentItem(iSettings.iStartScreen ? 1 : 0);
	}

TBool CAppearanceDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iSettings.iTheme = ((CEikChoiceList*)Control(EPtDlgTheme))->CurrentItem();
	iSettings.iCursor = ((CEikChoiceList*)Control(EPtDlgCursor))->CurrentItem();
	iSettings.iBlink = ((CEikChoiceList*)Control(EPtDlgBlink))->CurrentItem() == 1;
	iSettings.iStatus = ((CEikChoiceList*)Control(EPtDlgStatus))->CurrentItem() == 1;
	iSettings.iBell = ((CEikChoiceList*)Control(EPtDlgBell))->CurrentItem() == 1 ? 0 : 1;
	iSettings.iStartScreen = ((CEikChoiceList*)Control(EPtDlgStartScreen))->CurrentItem() == 1;
	return ETrue;
	}

void CToolDialog::SetSizeAndPositionL(const TSize& aSize)
	{
	TSize screen = iEikonEnv->ScreenDevice()->SizeInPixels();
	TSize size(aSize.iWidth < screen.iWidth - 8 ? aSize.iWidth : screen.iWidth - 8,
		aSize.iHeight < screen.iHeight - 8 ? aSize.iHeight : screen.iHeight - 8);
	SetCornerAndSizeL(EHCenterVCenter, size);   // what CEikDialog does, clamped
	}

// One button: "Stop" while the job runs, "Close" afterwards (a hidden
// second button would still take its space and push this one off centre)
void CToolDialog::SetButtonTextL(const TDesC& aText)
	{
	CEikCommandButtonBase* b = ButtonPanel()->ButtonById(EEikBidOk);
	if (b)
		{
		((CEikCommandButton*)b)->SetTextL(aText);
		b->DrawNow();
		}
	}

void CToolDialog::PreLayoutDynInitL()
	{
	SetTitleL(iTitle);
	if (iFinished)
		SetButtonTextL(_L("Close"));
	RefreshL();
	}

// No scroll bar: adding one to this editor crashed PsiTerm as the window
// opened (KERN-EXEC 3 in 0.29-0.31). The text scrolls with the arrow keys.
void CToolDialog::PostLayoutDynInitL()
	{
	iLaidOut = ETrue;
	}

// Scrolls the tool window so its last few paragraphs show. (Putting the
// cursor at the end, as before, left only the last line showing, at the top.)
static void ShowTail(CEikEdwin* aEd, const TDesC& aText)
	{
	TInt pos = aText.Length();
	TInt paras = 0;
	while (pos > 0)
		{
		if (aText[pos - 1] == CEditableText::EParagraphDelimiter && ++paras >= 6)
			break;
		pos--;
		}
	aEd->SetCursorPosL(aText.Length(), EFalse);
	aEd->SetCursorPosL(pos, EFalse);
	}

void CToolDialog::RefreshL()
	{
	HBufC* text = iView.DebugTextLC();
	CEikEdwin* ed = (CEikEdwin*)Control(EPtDlgDebugText);
	ed->SetTextL(text);
	ShowTail(ed, *text);
	ed->DrawNow();
	CleanupStack::PopAndDestroy();       // text
	}

void CToolDialog::FinishL()
	{
	if (iFinished)
		return;
	iFinished = ETrue;
	RefreshL();
	SetButtonTextL(_L("Close"));
	}

TBool CToolDialog::OkToExitL(TInt /*aButtonId*/)
	{
	if (iFinished || iView.InstallPending())
		return ETrue;
	iView.StopTool();                    // Stop (or Esc): ask the job to end, stay open
	return EFalse;
	}

void CUpdateDialog::PreLayoutDynInitL()
	{
	((CEikChoiceList*)Control(EPtDlgSource))->SetCurrentItem(iSource ? 1 : 0);
	SetEdwinTextL(EPtDlgHost, &iHost);
	SetNumberEditorValue(EPtDlgPort, iPort);
	}

TBool CUpdateDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iSource = ((CEikChoiceList*)Control(EPtDlgSource))->CurrentItem() == 1 ? 1 : 0;
	GetEdwinText(iHost, EPtDlgHost);
	iHost.Trim();
	iPort = NumberEditorValue(EPtDlgPort);
	if (iHost.Length() == 0 && (iSource == 1 || iNeedHost))
		{
		CEikonEnv::Static()->InfoMsg(_L("Enter the local server's name or IP address"));
		return EFalse;
		}
	return ETrue;
	}

// ===========================================================================
// App UI
// ===========================================================================

void CPsiTermAppUi::ConstructL()
	{
	BaseConstructL();
	TPsiSettings settings;
	LoadSettings(settings);
	iHosts = CHostList::NewL(iCoeEnv->FsSession());
	iHosts->Load();
	if (iHosts->Count() == 0 && settings.iSshUser.Length() > 0)
		{
		// carry over the host from PsiTerm 0.3's single "SSH to" setting
		THostEntry e;
		e.iAuth = 1;
		e.iKeyId = 0;
		e.iName = LeftSafe(settings.iSshHost, 24);
		e.iHost = settings.iSshHost;
		e.iUser = settings.iSshUser;
		e.iPort = settings.iSshPort;
		iHosts->AddL(e);
		iHosts->Save();
		}
	if (settings.iStartCmd.Length() > 0)
		{
		// 0.33-0.36 had one login command for every host: give it to each
		// saved host that has none, then retire the global setting
		for (TInt i = 0; i < iHosts->Count(); i++)
			if (iHosts->At(i).iCommand.Length() == 0)
				iHosts->At(i).iCommand = settings.iStartCmd;
		iHosts->Save();
		settings.iStartCmd.Zero();
		SaveSettings(settings);
		}
	iKeys = CKeyList::NewL(iCoeEnv->FsSession());
	iKeys->Load();
	iKeys->MigrateL();
	iSnippets = CSnippetList::NewL(iCoeEnv->FsSession());
	iSnippets->Load();
	iView = new(ELeave) CTermView;
	iView->SetSnippets(iSnippets);
	iView->SetHosts(iHosts);
	iView->ConstructL(ClientRect(), settings);
	AddToStackL(iView);
	}

CPsiTermAppUi::~CPsiTermAppUi()
	{
	if (iView)
		{
		RemoveFromStack(iView);
		delete iView;
		}
	delete iHosts;
	delete iSnippets;
	delete iKeys;
	}

// Add / edit dialog. Returns ETrue if the user pressed OK.
TBool CPsiTermAppUi::EditHostL(THostEntry& aEntry)
	{
	CHostEditDialog* dlg = new(ELeave) CHostEditDialog(aEntry, *iKeys);
	return dlg->ExecuteLD(R_PT_HOST_EDIT_DIALOG) != 0;
	}

// "SSH to...": the saved host list. Loops until the user connects or cancels.
void CPsiTermAppUi::SshToL()
	{
	if (iView->SshActive())
		{
		iEikonEnv->InfoMsg(_L("Already connected - Disconnect SSH first"));
		return;
		}
	for (;;)
		{
		if (iHosts->Count() == 0)
			{
			THostEntry e;
			e.iPort = 22;
			e.iAuth = iKeys->Count() ? 0 : 1;
			e.iKeyId = 0;
			if (!EditHostL(e))
				return;
			iHosts->AddL(e);
			iHosts->iLast = iHosts->Count() - 1;
			iHosts->Save();
			e.iPassword.FillZ();
			continue;
			}
		TInt index = iHosts->iLast;
		TInt action = 0;
		CHostListDialog* dlg = new(ELeave) CHostListDialog(*iHosts, index, action);
		TInt ok = dlg->ExecuteLD(R_PT_HOSTS_DIALOG);
		if (!ok && action == 0)
			return;
		if (index < 0 || index >= iHosts->Count())
			index = 0;
		switch (action)
			{
		case EPtBidNew:
			{
			if (iHosts->Count() >= KMaxHosts)
				{
				iEikonEnv->InfoMsg(_L("Host list is full - delete one first"));
				break;
				}
			THostEntry e;
			e.iPort = 22;
			e.iAuth = iKeys->Count() ? 0 : 1;
			e.iKeyId = 0;
			e.iUser = iHosts->At(index).iUser;     // most people reuse a user name
			if (EditHostL(e))
				{
				iHosts->AddL(e);
				iHosts->iLast = iHosts->Count() - 1;
				iHosts->Save();
				}
			e.iPassword.FillZ();
			break;
			}
		case EPtBidEdit:
			{
			THostEntry e = iHosts->At(index);
			if (EditHostL(e))
				{
				iHosts->At(index) = e;
				iHosts->iLast = index;
				iHosts->Save();
				}
			e.iPassword.FillZ();
			break;
			}
		case EPtBidDelete:
			{
			TBuf<60> what(iHosts->At(index).iName);
			if (CEikonEnv::QueryWinL(_L("Delete this saved host?"), what))
				{
				iHosts->Delete(index);
				iHosts->Save();
				}
			break;
			}
		default:                                 // Connect
			ConnectHostL(index);
			return;
			}
		}
	}

void CPsiTermAppUi::LoadSettings(TPsiSettings& aSettings)
	{
	aSettings.iBaudIndex = 0;     // 9600: what most modems start at
	aSettings.iRtsCts = 0;
	aSettings.iZoom = KDefaultZoom;
	aSettings.iSshHost.Zero();
	aSettings.iSshUser.Zero();
	aSettings.iSshPort = 22;
	aSettings.iNetMode = 0;
	aSettings.iUpdHost.Zero();
	aSettings.iUpdPort = 8686;
	aSettings.iBold = 0;
	aSettings.iAutoReconnect = 1;
	aSettings.iStartCmd.Zero();
	aSettings.iUpdSource = 0;
	aSettings.iTheme = 0;
	aSettings.iCursor = 0;
	aSettings.iBlink = 0;
	aSettings.iStatus = 1;
	aSettings.iTmuxPrefix = 0;
	aSettings.iBell = 0;
	aSettings.iStartScreen = 1;
	aSettings.iTmuxTabs = 1;
	RFs& fs = iCoeEnv->FsSession();
	RFile file;
	if (file.Open(fs, KIniFile, EFileRead) != KErrNone)
		return;
	TBuf8<512> data;
	if (file.Read(data) == KErrNone && data.Length() >= 3)
		{
		if (data[0] <= 4) aSettings.iBaudIndex = data[0];
		aSettings.iRtsCts = (data[1] != 0);
		// zoom: 32+level since 0.12 (Terminus); 0.11 stored 16+level of its
		// Spleen/Courier list; earlier versions 0=small 1=large
		static const TUint8 KFrom011[5] = { 0, 0, 1, 1, 3 };
		if (data[2] >= 32 && data[2] < 32 + KZoomLevels)
			aSettings.iZoom = data[2] - 32;
		else if (data[2] >= 16 && data[2] < 21)
			aSettings.iZoom = KFrom011[data[2] - 16];
		else if (data[2] <= 1)
			aSettings.iZoom = data[2] ? 1 : 0;
		// v2: host, user as length-prefixed strings, then port (2 bytes)
		TInt pos = 3;
		if (pos < data.Length())
			{
			TInt len = data[pos++];
			if (pos + len <= data.Length() && len <= 100)
				{
				aSettings.iSshHost.Copy(data.Mid(pos, len));
				pos += len;
				if (pos < data.Length())
					{
					len = data[pos++];
					if (pos + len <= data.Length() && len <= 60)
						{
						aSettings.iSshUser.Copy(data.Mid(pos, len));
						pos += len;
						if (pos + 2 <= data.Length())
							aSettings.iSshPort = data[pos] | (data[pos + 1] << 8);
						pos += 2;
						if (pos < data.Length())                 // v3: link type
							aSettings.iNetMode = (data[pos] == 1);
						pos++;
						if (pos < data.Length())                 // v4: update server
							{
							TInt ulen = data[pos++];
							if (pos + ulen + 2 <= data.Length() && ulen <= 100)
								{
								aSettings.iUpdHost.Copy(data.Mid(pos, ulen));
								pos += ulen;
								aSettings.iUpdPort = data[pos] | (data[pos + 1] << 8);
								pos += 2;
								if (pos < data.Length())     // v5: bold text
									aSettings.iBold = (data[pos] == 1);
								pos++;
								if (pos + 1 < data.Length())  // v6: reconnect, start command
									{
									aSettings.iAutoReconnect = (data[pos++] != 0);
									TInt clen = data[pos++];
									if (pos + clen <= data.Length() && clen <= 100)
										aSettings.iStartCmd.Copy(data.Mid(pos, clen));
									pos += clen;
									if (pos < data.Length())      // v7: update source
										aSettings.iUpdSource = (data[pos] == 1) ? 1 : 0;
									pos++;
									if (pos + 4 < data.Length())  // v8: appearance, tmux
										{
										aSettings.iTheme = data[pos] <= 3 ? data[pos] : 0;
										aSettings.iCursor = data[pos + 1] <= 2 ? data[pos + 1] : 0;
										aSettings.iBlink = data[pos + 2] ? 1 : 0;
										aSettings.iStatus = data[pos + 3] ? 1 : 0;
										aSettings.iTmuxPrefix = data[pos + 4] ? 1 : 0;
										if (pos + 5 < data.Length())   // v9: bell
											aSettings.iBell = data[pos + 5] ? 1 : 0;
										if (pos + 6 < data.Length())   // v10: start screen
											aSettings.iStartScreen = data[pos + 6] ? 1 : 0;
										if (pos + 7 < data.Length())   // v11: tmux tabs
											aSettings.iTmuxTabs = data[pos + 7] ? 1 : 0;
										}
									}
								}
							}
						}
					}
				}
			}
		}
	file.Close();
	}

void CPsiTermAppUi::SaveSettings(const TPsiSettings& aSettings)
	{
	RFs& fs = iCoeEnv->FsSession();
	fs.MkDirAll(KIniFile);
	RFile file;
	if (file.Replace(fs, KIniFile, EFileWrite) != KErrNone)
		return;
	TBuf8<512> data;
	data.Append((TUint8)aSettings.iBaudIndex);
	data.Append((TUint8)aSettings.iRtsCts);
	data.Append((TUint8)(32 + aSettings.iZoom));
	TBuf8<100> tmp;
	tmp.Copy(aSettings.iSshHost);
	data.Append((TUint8)tmp.Length());
	data.Append(tmp);
	tmp.Copy(aSettings.iSshUser);
	data.Append((TUint8)tmp.Length());
	data.Append(tmp);
	data.Append((TUint8)(aSettings.iSshPort & 0xff));
	data.Append((TUint8)(aSettings.iSshPort >> 8));
	data.Append((TUint8)(aSettings.iNetMode ? 1 : 0));
	tmp.Copy(aSettings.iUpdHost);
	data.Append((TUint8)tmp.Length());
	data.Append(tmp);
	data.Append((TUint8)(aSettings.iUpdPort & 0xff));
	data.Append((TUint8)(aSettings.iUpdPort >> 8));
	data.Append((TUint8)(aSettings.iBold ? 1 : 0));
	data.Append((TUint8)(aSettings.iAutoReconnect ? 1 : 0));
	tmp.Copy(aSettings.iStartCmd);
	data.Append((TUint8)tmp.Length());
	data.Append(tmp);
	data.Append((TUint8)(aSettings.iUpdSource ? 1 : 0));
	data.Append((TUint8)aSettings.iTheme);
	data.Append((TUint8)aSettings.iCursor);
	data.Append((TUint8)(aSettings.iBlink ? 1 : 0));
	data.Append((TUint8)(aSettings.iStatus ? 1 : 0));
	data.Append((TUint8)(aSettings.iTmuxPrefix ? 1 : 0));
	data.Append((TUint8)(aSettings.iBell ? 1 : 0));
	data.Append((TUint8)(aSettings.iStartScreen ? 1 : 0));
	data.Append((TUint8)(aSettings.iTmuxTabs ? 1 : 0));
	file.Write(data);
	file.Close();
	}

// The Debug tools need the serial port: offer to end the SSH session first.
// Returns ETrue if the tool can run now; if the user agrees to disconnect,
// the command is run again by itself once the session has ended.
TBool CPsiTermAppUi::ConfirmDisconnectL(TInt aCommand)
	{
	if (!iView->SshActive())
		return ETrue;
	if (iEikonEnv->QueryWinL(_L("SSH is connected"), _L("Disconnect, then continue?")))
		iView->RunAfterDisconnectL(aCommand);
	return EFalse;
	}

// Connects to saved host aIndex (SSH to... list, or 1-9 on the welcome screen)
void CPsiTermAppUi::ConnectHostL(TInt aIndex)
	{
	if (iView->SshActive() || aIndex < 0 || aIndex >= iHosts->Count())
		return;
	const THostEntry& e = iHosts->At(aIndex);
	iHosts->iLast = aIndex;
	iHosts->Save();
	TPsiSettings& s = iView->Settings();
	s.iSshHost = e.iHost;
	s.iSshUser = e.iUser;
	s.iSshPort = e.iPort;
	SaveSettings(s);
	iView->SetSshPassword(e.iPassword);
	iView->SetLoginCommand(e.iCommand);
	TFileName base;
	if (e.iAuth == 0 || e.iAuth == 3)
		{
		TInt k = e.iKeyId ? iKeys->Find(e.iKeyId) : (iKeys->Count() ? 0 : -1);
		if (k >= 0)
			CKeyList::Base(iKeys->At(k).iId, base);
		}
	iView->SetKeyBase(base);
	iView->StartSshL();
	}

// tmux commands: the prefix (Ctrl+B or Ctrl+A), then the command key
void CPsiTermAppUi::SendTmux(TUint aKey)
	{
	iView->SendCtrl(iView->Settings().iTmuxPrefix ? 'A' : 'B');
	iView->SendChar(aKey);
	}

// Manage snippets: the list (Send / New / Edit / Delete), like SSH to...
void CPsiTermAppUi::ManageSnippetsL()
	{
	for (;;)
		{
		if (iSnippets->Count() == 0)
			{
			TSnippet e;
			e.iEnter = 1;
			e.iKey = 0;
			if (!EditSnippetL(e, -1))
				return;
			iSnippets->AddL(e);
			iSnippets->iLast = iSnippets->Count() - 1;
			iSnippets->Save();
			continue;
			}
		TInt index = iSnippets->iLast;
		TInt action = 0;
		CSnippetListDialog* dlg = new(ELeave) CSnippetListDialog(*iSnippets, index, action);
		TInt ok = dlg->ExecuteLD(R_PT_SNIPPETS_DIALOG);
		if (!ok && action == 0)
			return;
		if (index < 0 || index >= iSnippets->Count())
			index = 0;
		switch (action)
			{
		case EPtBidNew:
			{
			if (iSnippets->Count() >= KMaxSnippets)
				{
				iEikonEnv->InfoMsg(_L("Snippet list is full - delete one first"));
				break;
				}
			TSnippet e;
			e.iEnter = 1;
			e.iKey = 0;
			if (EditSnippetL(e, -1))
				{
				iSnippets->AddL(e);
				iSnippets->iLast = iSnippets->Count() - 1;
				iSnippets->Save();
				}
			break;
			}
		case EPtBidEdit:
			{
			TSnippet e = iSnippets->At(index);
			if (EditSnippetL(e, index))
				{
				iSnippets->At(index) = e;
				iSnippets->iLast = index;
				iSnippets->Save();
				}
			break;
			}
		case EPtBidDelete:
			{
			TBuf<30> what(iSnippets->At(index).iName);
			if (CEikonEnv::QueryWinL(_L("Delete this snippet?"), what))
				{
				iSnippets->Delete(index);
				iSnippets->Save();
				}
			break;
			}
		default:                                 // Send
			{
			iSnippets->iLast = index;
			iSnippets->Save();
			const TSnippet& e = iSnippets->At(index);
			iView->SendSnippetText(e.iText, e.iEnter);
			return;
			}
			}
		}
	}

TBool CPsiTermAppUi::EditSnippetL(TSnippet& aEntry, TInt aSelf)
	{
	CSnippetEditDialog* dlg = new(ELeave) CSnippetEditDialog(aEntry, *iSnippets, aSelf);
	return dlg->ExecuteLD(R_PT_SNIPPET_EDIT_DIALOG);
	}

void CPsiTermAppUi::DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	if (aMenuId == R_PT_TERM_MENU)
		{
		TBool ssh = iView->SshActive();
		aMenuPane->SetItemDimmed(EPtCmdSsh, ssh);
		aMenuPane->SetItemDimmed(EPtCmdSshDisconnect, !ssh);
		aMenuPane->SetItemDimmed(EPtCmdHangup, ssh || !iView->ModemOnline());
		aMenuPane->SetItemDimmed(EPtCmdInstallKey, !iView->SshLoggedIn() || iKeys->Count() == 0);
		return;
		}
	if (aMenuId == R_PT_SNIPPETS_MENU)
		{
		// your snippets, each with its hotkey shown on the right
		for (TInt i = 0; i < iSnippets->Count() && i < KMaxSnippets; i++)
			{
			const TSnippet& sn = iSnippets->At(i);
			CEikMenuPane::TItem::SData item;
			item.iCommandId = EPtCmdSnippet0 + i;
			item.iCascadeId = 0;
			item.iFlags = 0;
			item.iText = sn.iName;
			item.iExtraText.Zero();
			if (sn.iKey)
				{
				item.iExtraText.Append(_L("Shift+Ctrl+"));
				item.iExtraText.Append((TChar)sn.iKey);
				}
			aMenuPane->AddMenuItemL(item);
			}
		return;
		}
	// tmux and Claude Code keys only mean something in an SSH session
	if (aMenuId == R_PT_TMUX_MENU || aMenuId == R_PT_TMUX_WIN_MENU
		|| aMenuId == R_PT_TMUX_PANE_MENU || aMenuId == R_PT_CLAUDE_MENU)
		{
		if (aMenuId == R_PT_TMUX_MENU)
			aMenuPane->SetItemButtonState(EPtCmdTmuxTabs,
				iView->Settings().iTmuxTabs ? EEikMenuItemSymbolOn : 0);
		if (!iView->SshLoggedIn())
			{
			static const TInt KTmux[] = { EPtCmdTmuxCopy, EPtCmdTmuxDetach, EPtCmdTmuxMouse, EPtCmdTabsSetup };
			static const TInt KWin[] = { EPtCmdTmuxNew, EPtCmdTmuxNext, EPtCmdTmuxPrev,
				EPtCmdTmuxChoose, EPtCmdTmuxRename };
			static const TInt KPane[] = { EPtCmdTmuxSplitH, EPtCmdTmuxSplitV, EPtCmdTmuxPane,
				EPtCmdTmuxZoom };
			static const TInt KClaude[] = { EPtCmdClaudeEsc, EPtCmdClaudeEscEsc, EPtCmdClaudeMode,
				EPtCmdClaudeClear, EPtCmdClaudeCompact, EPtCmdClaudeResume, EPtCmdClaudeHelp };
			const TInt* ids = KTmux;
			TInt n = 4;
			if (aMenuId == R_PT_TMUX_WIN_MENU) { ids = KWin; n = 5; }
			else if (aMenuId == R_PT_TMUX_PANE_MENU) { ids = KPane; n = 4; }
			else if (aMenuId == R_PT_CLAUDE_MENU) { ids = KClaude; n = 7; }
			for (TInt i = 0; i < n; i++)
				aMenuPane->SetItemDimmed(ids[i], ETrue);
			}
		return;
		}
	if (aMenuId == R_PT_TMUX_PREFIX_MENU)
		{
		aMenuPane->SetItemButtonState(iView->Settings().iTmuxPrefix ? EPtCmdTmuxPrefixA : EPtCmdTmuxPrefixB,
			EEikMenuItemSymbolOn);
		return;
		}
	TPsiSettings& s = iView->Settings();
	if (aMenuId == R_PT_FONT_MENU)
		aMenuPane->SetItemButtonState(EPtCmdZoom0 + s.iZoom, EEikMenuItemSymbolOn);
	else if (aMenuId == R_PT_SETTINGS_MENU)
		aMenuPane->SetItemButtonState(EPtCmdBold, s.iBold ? EEikMenuItemSymbolOn : 0);
	}

void CPsiTermAppUi::HandleCommandL(TInt aCommand)
	{
	TPsiSettings& s = iView->Settings();
	if (aCommand >= EPtCmdHost0 && aCommand < EPtCmdHost0 + KMaxHosts)
		{
		ConnectHostL(aCommand - EPtCmdHost0);
		return;
		}
	if (aCommand >= EPtCmdSnippet0 && aCommand < EPtCmdSnippet0 + KMaxSnippets)
		{
		TInt i = aCommand - EPtCmdSnippet0;
		if (i < iSnippets->Count())
			iView->SendSnippetText(iSnippets->At(i).iText, iSnippets->At(i).iEnter);
		return;
		}
	if (((aCommand >= EPtCmdTmuxNew && aCommand <= EPtCmdTmuxDetach) || aCommand == EPtCmdTmuxMouse
		|| aCommand == EPtCmdTabsSetup
		|| (aCommand >= EPtCmdClaudeEsc && aCommand <= EPtCmdClaudeHelp))
		&& !iView->SshLoggedIn())
		{
		iEikonEnv->InfoMsg(_L("Not connected - connect with SSH first"));
		return;
		}
	switch (aCommand)
		{
	case EEikCmdExit:
		Exit();
		return;
	case EPtCmdSsh:     SshToL(); break;
	case EPtCmdSshDisconnect: iView->DisconnectSsh(); break;
	case EPtCmdSpeedTest:
		if (!ConfirmDisconnectL(aCommand))
			break;
		iView->BeginDebugL(_L("SSH speed test"));
		iView->StartSpeedTestL();
		iView->RunToolDialogL();
		break;
	case EPtCmdHangup:   iView->HangUp(); break;
	case EPtCmdSendSize: iView->SendScreenSize(); break;
	case EPtCmdLoginKey:
		ManageKeysL();
		break;
	case EPtCmdInstallKey:
		InstallKeyCmdL();
		iEikonEnv->InfoMsg(_L("To use it: SSH to... > Edit > Log in with"));
		break;
	case EPtCmdSerialInfo:
		if (ConfirmDisconnectL(aCommand))
			iView->SerialInfo();
		break;
	case EPtCmdUpdate:
	case EPtCmdSendShots:
	case EPtCmdUpdateServer:
		{
		if (aCommand != EPtCmdUpdateServer && !ConfirmDisconnectL(aCommand))
			break;
		// Updates come from GitHub unless a local server is chosen. Sending
		// screenshots (a developer feature) always needs the local server.
		TBool needHost = (aCommand == EPtCmdSendShots) ||
			(aCommand == EPtCmdUpdate && s.iUpdSource == 1);
		if (aCommand == EPtCmdUpdateServer || (needHost && s.iUpdHost.Length() == 0))
			{
			if (aCommand == EPtCmdSendShots)
				iEikonEnv->InfoWinL(_L("Screenshots go to a local server"),
					_L("Run server/psion-update.sh from the PsiTerm source on a computer, then enter its address."));
			TInt source = s.iUpdSource;
			TBuf<100> host(s.iUpdHost);
			TInt port = s.iUpdPort > 0 ? s.iUpdPort : 8686;
			CUpdateDialog* dlg = new(ELeave) CUpdateDialog(source, host, port, aCommand == EPtCmdSendShots);
			if (!dlg->ExecuteLD(R_PT_UPDATE_DIALOG))
				break;
			s.iUpdSource = source;
			s.iUpdHost = host;
			s.iUpdPort = port;
			SaveSettings(s);
			if (aCommand == EPtCmdUpdateServer)
				break;
			}
		iView->BeginDebugL(aCommand == EPtCmdUpdate ? _L("Update PsiTerm") : _L("Send screenshots"));
		if (aCommand == EPtCmdUpdate)
			iView->StartUpdateL();
		else
			iView->SendScreenshotsL();
		iView->RunToolDialogL();
		break;
		}
	case EPtCmdScreenshot: iView->ScreenshotL(); break;
	case EPtCmdCopy:     iView->CopySelectionL(); break;
	case EPtCmdPaste:    iView->PasteL(); break;
	case EPtCmdSelectScreen: iView->SelectScreen(); break;
	case EPtCmdCharInfo: iView->ShowCharInfoL(); break;
	case EPtCmdScrollUp:   iView->ScrollBy(iView->Rows() - 1); break;
	case EPtCmdScrollDown: iView->ScrollBy(-(iView->Rows() - 1)); break;
	case EPtCmdScrollEnd:  iView->ScrollTo(0); break;
	case EPtCmdReset:    iView->ResetTerminal(); break;
	case EPtCmdAbout:
		{
		TPtrC link(s.iNetMode ? _L("Psion Internet") : _L("modem"));
		TBuf<80> status;
		status.Format(_L("Screen %dx%d, %d baud, SSH via %S"),
			iView->Cols(), iView->Rows(), BaudValue(s.iBaudIndex), &link);
		CAboutDialog* dlg = new(ELeave) CAboutDialog(status);
		dlg->ExecuteLD(R_PT_ABOUT_DIALOG);
		break;
		}
	case EPtCmdSnippets:    ManageSnippetsL(); break;
	case EPtCmdAppearance:
		{
		CAppearanceDialog* dlg = new(ELeave) CAppearanceDialog(s);
		if (dlg->ExecuteLD(R_PT_APPEARANCE_DIALOG))
			{
			SaveSettings(s);
			iView->ApplyAppearanceL();
			}
		break;
		}
	// Claude Code
	case EPtCmdClaudeEsc:   iView->SendKey(VTERM_KEY_ESCAPE, VTERM_MOD_NONE); break;
	case EPtCmdClaudeEscEsc:
		iView->SendKey(VTERM_KEY_ESCAPE, VTERM_MOD_NONE);
		iView->SendKey(VTERM_KEY_ESCAPE, VTERM_MOD_NONE);
		break;
	case EPtCmdClaudeMode:  iView->SendKey(VTERM_KEY_TAB, VTERM_MOD_SHIFT); break;
	case EPtCmdClaudeClear: iView->SendSnippetText(_L("/clear"), ETrue); break;
	case EPtCmdClaudeCompact: iView->SendSnippetText(_L("/compact"), ETrue); break;
	case EPtCmdClaudeResume: iView->SendSnippetText(_L("/resume"), ETrue); break;
	case EPtCmdClaudeHelp:  iView->SendSnippetText(_L("/help"), ETrue); break;
	// tmux: the prefix, then the command key
	case EPtCmdTmuxNew:     SendTmux('c'); break;
	case EPtCmdTmuxNext:    SendTmux('n'); break;
	case EPtCmdTmuxPrev:    SendTmux('p'); break;
	case EPtCmdTmuxChoose:  SendTmux('w'); break;
	case EPtCmdTmuxSplitH:  SendTmux('"'); break;
	case EPtCmdTmuxSplitV:  SendTmux('%'); break;
	case EPtCmdTmuxPane:    SendTmux('o'); break;
	case EPtCmdTmuxZoom:    SendTmux('z'); break;
	case EPtCmdTmuxCopy:    SendTmux('['); break;
	case EPtCmdTmuxRename:  SendTmux(','); break;
	case EPtCmdTmuxDetach:  SendTmux('d'); break;
	case EPtCmdTabsSetup:
		// at a shell prompt inside tmux: tmux sends its window list as the
		// terminal title, and ~/.tmux.conf keeps that for new tmux servers
		iView->SendString(_L8(" tmux set -g set-titles on \\; set -g set-titles-string "
			"'PSITABS #{W:#I:#W#F }'; grep -q PSITABS ~/.tmux.conf 2>/dev/null || "
			"printf '%s\\n' 'set -g set-titles on' \"set -g set-titles-string 'PSITABS "
			"#{W:#I:#W#F }'\" >> ~/.tmux.conf; echo 'PsiTerm: tabs set up'\r"));
		break;
	case EPtCmdCheckTabs:
		iView->CheckTabsL();
		break;
	case EPtCmdTmuxTabs:
		s.iTmuxTabs = !s.iTmuxTabs;
		SaveSettings(s);
		iView->ParseTmuxTabs();
		iView->DrawNow();
		break;
	case EPtCmdTmuxMouse:
		// tmux's command prompt: "set -g mouse" with no value flips it
		SendTmux(':');
		iView->SendString(_L8("set -g mouse\r"));
		break;
	case EPtCmdTmuxPrefixB:
	case EPtCmdTmuxPrefixA:
		s.iTmuxPrefix = (aCommand == EPtCmdTmuxPrefixA);
		SaveSettings(s);
		break;
	case EPtCmdKeyEsc:      iView->SendKey(VTERM_KEY_ESCAPE, VTERM_MOD_NONE); break;
	case EPtCmdKeyTab:      iView->SendKey(VTERM_KEY_TAB, VTERM_MOD_NONE); break;
	case EPtCmdKeyShiftTab: iView->SendKey(VTERM_KEY_TAB, VTERM_MOD_SHIFT); break;
	case EPtCmdKeyInsert:   iView->SendKey(VTERM_KEY_INS, VTERM_MOD_NONE); break;
	case EPtCmdKeyCtrlC:    iView->SendCtrl('C'); break;
	case EPtCmdKeyCtrlD:    iView->SendCtrl('D'); break;
	case EPtCmdKeyCtrlZ:    iView->SendCtrl('Z'); break;
	case EPtCmdKeyCtrlL:    iView->SendCtrl('L'); break;
	case EPtCmdKeyCtrlR:    iView->SendCtrl('R'); break;
	case EPtCmdKeyCtrlA:    iView->SendCtrl('A'); break;
	case EPtCmdKeyCtrlBackslash: iView->SendCtrl('\\'); break;
	case EPtCmdBaud9600:
	case EPtCmdBaud19200:
	case EPtCmdBaud38400:
	case EPtCmdBaud57600:
	case EPtCmdBaud115200:
		s.iBaudIndex = aCommand - EPtCmdBaud9600;
		if (!iView->SshActive())
			iView->ApplySerialSettings();
		SaveSettings(s);
		break;
	case EPtCmdFlowNone:
	case EPtCmdFlowRtsCts:
		s.iRtsCts = (aCommand == EPtCmdFlowRtsCts);
		if (!iView->SshActive())
			iView->ApplySerialSettings();
		SaveSettings(s);
		break;
	case EPtCmdLinkModem:
	case EPtCmdLinkPpp:
		s.iNetMode = (aCommand == EPtCmdLinkPpp);
		SaveSettings(s);
		iView->LocalMessage(s.iNetMode
			? _L8("\r\n[SSH will use the Psion's own Internet connection (dial-up/PPP).\r\n"
				" Set it up in Control panel > Dial: number PPP, no login script.]\r\n")
			: _L8("\r\n[SSH will dial through the modem (ATDT host:port).]\r\n"));
		break;
	case EPtCmdZoom0:
	case EPtCmdZoom1:
	case EPtCmdZoom2:
	case EPtCmdZoom3:
	case EPtCmdZoom4:
		iView->SetFontL(aCommand - EPtCmdZoom0);
		SaveSettings(s);
		break;
	case EPtCmdConnSettings:
		{
		TPsiSettings old = s;
		CConnDialog* dlg = new(ELeave) CConnDialog(s);
		if (!dlg->ExecuteLD(R_PT_CONN_DIALOG))
			break;
		SaveSettings(s);
		if (s.iBaudIndex != old.iBaudIndex || s.iRtsCts != old.iRtsCts)
			{
			if (iView->SshActive())
				iEikonEnv->InfoMsg(_L("New speed applies when SSH disconnects"));
			else
				iView->ApplySerialSettings();
			}
		if (s.iNetMode != old.iNetMode && s.iNetMode)
			iEikonEnv->InfoMsg(_L("Set up the dial-up in Control panel > Dial (number PPP)"));
		break;
		}
	case EPtCmdBold:
		s.iBold = !s.iBold;
		SaveSettings(s);
		iView->DrawNow();
		break;
	case EEikCmdZoomIn:                       // sidebar zoom buttons
		iView->ZoomBy(1);
		SaveSettings(s);
		break;
	case EEikCmdZoomOut:
		iView->ZoomBy(-1);
		SaveSettings(s);
		break;
	case EEikCmdEditCopy:                     // sidebar clipboard menu
	case EEikCmdEditCut:
		iView->CopySelectionL();
		break;
	case EEikCmdEditPaste:
		iView->PasteL();
		break;
	default:
		if (aCommand >= EPtCmdF1 && aCommand <= EPtCmdF12)
			iView->SendKey((VTermKey)VTERM_KEY_FUNCTION(aCommand - EPtCmdF1 + 1), VTERM_MOD_NONE);
		break;
		}
	}

// ===========================================================================
// Document / Application / entry points
// ===========================================================================

CPsiTermDocument::CPsiTermDocument(CEikApplication& aApp)
	: CEikDocument(aApp)
	{
	}

CEikAppUi* CPsiTermDocument::CreateAppUiL()
	{
	return new(ELeave) CPsiTermAppUi;
	}

TUid CPsiTermApplication::AppDllUid() const
	{
	return KUidPsiTerm;
	}

CApaDocument* CPsiTermApplication::CreateDocumentL()
	{
	return new(ELeave) CPsiTermDocument(*this);
	}

EXPORT_C CApaApplication* NewApplication()
	{
	return new CPsiTermApplication;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}

// ----- SSH keys: list and dialogs ---------------------------------------------

void CKeyListDialog::SetSizeAndPositionL(const TSize& aSize) { SetCornerAndSizeL(EHCenterVCenter, ClampToScreen(aSize)); }

void CKeyListDialog::PreLayoutDynInitL()
	{
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(4);
	CleanupStack::PushL(names);
	for (TInt i = 0; i < iKeys.Count(); i++)
		{
		TBuf<64> line(iKeys.At(i).iName);
		TBuf<60> fp;
		iKeys.Fingerprint(i, fp);
		if (fp.Length() > 7)
			{
			line.Append(_L("  "));
			line.Append(fp.Mid(7, fp.Length() - 7 < 10 ? fp.Length() - 7 : 10));   // after "SHA256:"
			line.Append(_L("..."));
			}
		names->AppendL(line);
		}
	if (iKeys.Count() == 0)
		names->AppendL(_L("(no keys yet - New or Import)"));
	CEikChoiceList* list = (CEikChoiceList*)Control(EPtDlgKeyList);
	list->SetArrayL(names);
	list->SetArrayExternalOwnership(EFalse);
	CleanupStack::Pop();
	list->SetCurrentItem(iIndex >= 0 && iIndex < iKeys.Count() ? iIndex : 0);
	}

TBool CKeyListDialog::OkToExitL(TInt aButtonId)
	{
	iIndex = ChoiceListCurrentItem(EPtDlgKeyList);
	iAction = (aButtonId == EEikBidCancel) ? 0 : aButtonId;
	return ETrue;
	}

void CKeyEditDialog::SetSizeAndPositionL(const TSize& aSize) { SetCornerAndSizeL(EHCenterVCenter, ClampToScreen(aSize)); }

void CKeyEditDialog::PreLayoutDynInitL()
	{
	SetEdwinTextL(EPtDlgKeyName, &iName);
	if (iFile)
		SetEdwinTextL(EPtDlgKeyFile, iFile);
	if (iRegen)
		((CEikChoiceList*)Control(EPtDlgKeyRegen))->SetCurrentItem(0);
	}

TBool CKeyEditDialog::OkToExitL(TInt /*aButtonId*/)
	{
	GetEdwinText(iName, EPtDlgKeyName);
	iName.Trim();
	if (iName.Length() == 0)
		{
		CEikonEnv::Static()->InfoMsg(_L("Give the key a name"));
		TryChangeFocusToL(EPtDlgKeyName);
		return EFalse;
		}
	if (iFile)
		{
		GetEdwinText(*iFile, EPtDlgKeyFile);
		iFile->Trim();
		TEntry e;
		if (iFile->Length() == 0 || CEikonEnv::Static()->FsSession().Entry(*iFile, e) != KErrNone)
			{
			CEikonEnv::Static()->InfoMsg(_L("No such file"));
			TryChangeFocusToL(EPtDlgKeyFile);
			return EFalse;
			}
		}
	if (iRegen)
		*iRegen = ((CEikChoiceList*)Control(EPtDlgKeyRegen))->CurrentItem() == 1;
	return ETrue;
	}

void CKeyPickDialog::SetSizeAndPositionL(const TSize& aSize) { SetCornerAndSizeL(EHCenterVCenter, ClampToScreen(aSize)); }

void CKeyPickDialog::PreLayoutDynInitL()
	{
	CDesCArrayFlat* names = new(ELeave) CDesCArrayFlat(4);
	CleanupStack::PushL(names);
	for (TInt i = 0; i < iKeys.Count(); i++)
		names->AppendL(iKeys.At(i).iName);
	CEikChoiceList* list = (CEikChoiceList*)Control(EPtDlgKeyPick);
	list->SetArrayL(names);
	list->SetArrayExternalOwnership(EFalse);
	CleanupStack::Pop();
	list->SetCurrentItem(iIndex >= 0 && iIndex < iKeys.Count() ? iIndex : 0);
	}

TBool CKeyPickDialog::OkToExitL(TInt /*aButtonId*/)
	{
	iIndex = ChoiceListCurrentItem(EPtDlgKeyPick);
	return ETrue;
	}

// Settings > SSH keys: the key list (New / Import / Show / Edit / Delete)
void CPsiTermAppUi::ManageKeysL()
	{
	for (;;)
		{
		// forget keys whose making or import did not finish
		for (TInt i = iKeys->Count() - 1; i >= 0; i--)
			if (!iKeys->HasFile(i))
				iKeys->Delete(i);
		iKeys->Save();

		TInt index = iKeys->iLast;
		TInt action = 0;
		CKeyListDialog* dlg = new(ELeave) CKeyListDialog(*iKeys, index, action);
		dlg->ExecuteLD(R_PT_KEYS_DIALOG);
		if (action == 0)
			return;
		TBool have = (index >= 0 && index < iKeys->Count());
		if ((action == EPtBidNew || action == EPtBidImport) && iView->SshActive())
			{
			iEikonEnv->InfoMsg(_L("Disconnect SSH first"));
			continue;
			}
		if ((action == EPtBidNew || action == EPtBidImport) && iKeys->Count() >= KMaxKeys)
			{
			iEikonEnv->InfoMsg(_L("Key list is full - delete one first"));
			continue;
			}
		if (!have && action != EPtBidNew && action != EPtBidImport)
			continue;
		TFileName base;
		switch (action)
			{
		case EPtBidNew:
		case EPtBidImport:
			{
			TBuf<24> name;
			name.Format(_L("Key %d"), iKeys->Count() + 1);
			TFileName file(_L("D:\\id_ed25519"));
			CKeyEditDialog* ed = new(ELeave) CKeyEditDialog(name, action == EPtBidImport ? &file : NULL, NULL);
			if (!ed->ExecuteLD(action == EPtBidImport ? R_PT_KEY_IMPORT_DIALOG : R_PT_KEY_NEW_DIALOG))
				break;
			TInt id = iKeys->AddL(name);
			iKeys->iLast = iKeys->Count() - 1;
			iKeys->Save();
			CKeyList::Base(id, base);
			if (!iView->KeyToolL(action == EPtBidNew ? 4 : 5, base, name, file))
				return;                  // gathering randomness first
			break;
			}
		case EPtBidShow:
			CKeyList::Base(iKeys->At(index).iId, base);
			iView->ShowKeyL(base, iKeys->At(index).iName);
			break;
		case EPtBidEdit:
			{
			TBuf<24> name(iKeys->At(index).iName);
			TInt regen = 0;
			CKeyEditDialog* ed = new(ELeave) CKeyEditDialog(name, NULL, &regen);
			if (!ed->ExecuteLD(R_PT_KEY_EDIT_DIALOG))
				break;
			iKeys->At(index).iName = name;
			iKeys->iLast = index;
			iKeys->Save();
			if (regen)
				{
				if (iView->SshActive())
					{
					iEikonEnv->InfoMsg(_L("Disconnect SSH first"));
					break;
					}
				if (!iEikonEnv->QueryWinL(_L("Replace this key?"),
					_L("Servers set up with the old key will stop accepting it")))
					break;
				CKeyList::Base(iKeys->At(index).iId, base);
				if (!iView->KeyToolL(4, base, name, KNullDesC))
					return;
				}
			break;
			}
		case EPtBidDelete:
			{
			TBuf<30> what(iKeys->At(index).iName);
			if (iEikonEnv->QueryWinL(_L("Delete this key?"), what))
				{
				iKeys->Delete(index);
				iKeys->Save();
				}
			break;
			}
		default:
			break;
			}
		}
	}

// Terminal > Install login key on server: the host's own key, or the only
// one, or ask which
void CPsiTermAppUi::InstallKeyCmdL()
	{
	if (!iView->SshLoggedIn())
		return;
	if (iKeys->Count() == 0)
		{
		iEikonEnv->InfoMsg(_L("Make a key first: Settings > SSH keys"));
		return;
		}
	TFileName base;
	TInt pick = -1;
	for (TInt i = 0; i < iKeys->Count() && pick < 0; i++)
		{
		CKeyList::Base(iKeys->At(i).iId, base);
		if (base == iView->KeyBase())
			pick = i;
		}
	if (pick < 0 && iKeys->Count() == 1)
		pick = 0;
	if (pick < 0)
		{
		TInt index = iKeys->iLast;
		CKeyPickDialog* dlg = new(ELeave) CKeyPickDialog(*iKeys, index);
		if (!dlg->ExecuteLD(R_PT_KEY_PICK_DIALOG))
			return;
		pick = index;
		}
	CKeyList::Base(iKeys->At(pick).iId, base);
	iView->InstallLoginKeyL(base);
	iEikonEnv->InfoMsg(_L("To use it: SSH to... > Edit > Log in with"));
	}
