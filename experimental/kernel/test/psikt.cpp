// psikt.cpp - PsiKernTest, the test harness for PsiKern.ldd, for the 5mx.
// EXPERIMENTAL: never ship this in a release. See ../README.md.
//
// Every test is a menu command, so nothing runs by itself:
//  - Read: the machine and the driver, the registers, the palette, and the
//    CPU and memory speed. These change nothing.
//  - Write: invert the screen, and grey curves. They change only the LCD
//    palette (RAM), put it back after, and ask first.
//
// The log, PsiKern.log, goes on D: (the CF card) if there is one, else C:.
// A line "> step" is written and flushed before each step, and "< step" after
// it. If a step takes the machine down, the next start finds a "> " with no
// "< " and says which step it was. The previous log is kept as PsiKern.old.
//
// The driver is loaded only on the ROM it was made for (1.05(260)): its
// kernel imports are by ordinal, and another ROM's are different.
#include <eikenv.h>
#include <eikapp.h>
#include <eikdoc.h>
#include <eikappui.h>
#include <coecntrl.h>
#include <f32file.h>
#include <e32hal.h>
#include <e32svr.h>
#include <c32comm.h>
#include <eikcmds.hrh>
#include "psikt.hrh"
#include "../psikern.h"

const TUid KUidPsiKt={0x0100FA11};
const TInt KMaxLines=40;
const TInt KLineLen=80;
_LIT(KLogName,"PsiKern.log");
_LIT(KOldName,"PsiKern.old");
_LIT(KPsiKtVersion,"0.5");

// the ROM the driver's ordinals come from (ekern_rom.def)
const TInt KRomMajor=1, KRomMinor=5, KRomBuild=260;

struct TRegShow { TInt iOffset; TInt iWidth; const char* iName; };
static const TRegShow KShow[]=
	{
	{ERegMemCfg1,4,"MEMCFG1"},{ERegMemCfg2,4,"MEMCFG2"},{ERegDramCfg,4,"DRAMCFG"},
	{ERegLcdCtl,4,"LCDCTL"},{ERegLcdSt,4,"LCDST"},{ERegLcdDbar1,4,"LCDDBAR1"},
	{ERegLcdT0,4,"LCDT0"},{ERegLcdT1,4,"LCDT1"},{ERegLcdT2,4,"LCDT2"},
	{ERegPwrSr,4,"PWRSR"},{ERegPwrCnt,4,"PWRCNT"},
	{ERegIntSr,4,"INTSR"},{ERegIntRsr,4,"INTRSR"},{ERegIntEns,4,"INTENS"},
	{ERegU2Fcr,4,"UART2 FCR"},{ERegU2Ubrcr,4,"UART2 UBRCR"},{ERegU2Con,1,"UART2 CON"},
	{ERegU2Flg,1,"UART2 FLG"},{ERegU2IntM,1,"UART2 INTM"},
	{ERegTc1Load,4,"TC1 LOAD"},{ERegTc1Val,4,"TC1 VALUE"},{ERegTc1Ctrl,1,"TC1 CTRL"},
	{ERegTc2Load,4,"TC2 LOAD"},{ERegTc2Val,4,"TC2 VALUE"},{ERegTc2Ctrl,1,"TC2 CTRL"},
	{ERegRtcL,4,"RTC LOW"},{ERegRtcU,4,"RTC HIGH"},
	{ERegPbdr,1,"PORT B"},{ERegPcdr,1,"PORT C"},{ERegPddr,1,"PORT D"},{ERegPedr,1,"PORT E"},
	};
const TInt KShowCount=sizeof(KShow)/sizeof(KShow[0]);

// grey curves: how light each grey is shown, 0 black .. 15 white, for
// greys 0 (black) .. 15 (white). The palette's levels may run either way
// (the 16-grey one runs 14 .. 0, black first), so CurvesL turns them round.
struct TCurve { const char* iName; TUint8 iLevel[16]; };
static const TCurve KCurves[]=
	{
	{"All 16 levels (EPOC uses 15, with 7 twice)",{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}},
	{"Lighter middle greys (gamma 0.6)",{0,3,4,6,7,8,9,9,10,11,12,12,13,14,14,15}},
	{"Darker middle greys (gamma 1.6)",{0,0,1,1,2,3,3,4,5,7,8,9,10,12,13,15}},
	{"More contrast (S-curve)",{0,0,1,2,3,4,5,7,8,10,11,12,13,14,15,15}},
	{"Less contrast",{3,4,4,5,5,6,7,7,8,8,9,10,10,11,11,12}},
	{"Inverted",{15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0}},
	};
const TInt KCurveCount=sizeof(KCurves)/sizeof(KCurves[0]);

// ----- the log -------------------------------------------------------------

class TPsiKtLog
	{
public:
	TPsiKtLog() : iOpen(EFalse) {}
	void OpenL(RFs& aFs, TDes& aCrashedStep, TDes& aWhere);
	void Close() { if (iOpen) iFile.Close(); iOpen=EFalse; }
	void Line(const TDesC& aText);
private:
	RFile iFile;
	TBool iOpen;
	};

// Finds what the last run was doing if it stopped, keeps its log as
// PsiKern.old, and starts a new one.
void TPsiKtLog::OpenL(RFs& aFs, TDes& aCrashedStep, TDes& aWhere)
	{
	aCrashedStep.Zero();
	TDriveInfo d;
	_LIT(KDirD,"D:\\");
	_LIT(KDirC,"C:\\");
	if (aFs.Drive(d,EDriveD)==KErrNone && d.iType!=EMediaNotPresent)
		aWhere=KDirD;
	else
		aWhere=KDirC;
	TFileName name(aWhere), old(aWhere);
	name.Append(KLogName);
	old.Append(KOldName);
	RFile f;
	if (f.Open(aFs,name,EFileRead|EFileShareAny)==KErrNone)
		{
		HBufC8* buf=HBufC8::New(16384);
		if (buf)
			{
			TInt size=0;
			f.Size(size);
			TInt pos=Max(0,size-buf->Des().MaxLength());
			f.Seek(ESeekStart,pos);
			TPtr8 p=buf->Des();
			f.Read(p);
			// the last "> step" that no "< " followed
			TPtrC8 rest(p);
			TPtrC8 pending;
			TBool isPending=EFalse;
			while (rest.Length())
				{
				TInt nl=rest.Locate('\n');
				TPtrC8 line(nl<0 ? rest : rest.Left(nl));
				if (nl<0)
					rest.Set(KNullDesC8);
				else
					rest.Set(rest.Mid(nl+1));
				if (line.Length() && line[line.Length()-1]=='\r')
					line.Set(line.Left(line.Length()-1));
				// (after the time stamp "hh:mm:ss ")
				if (line.Length()>9)
					line.Set(line.Mid(9));
				if (line.Length()>2 && line[0]=='>' && line[1]==' ')
					{
					pending.Set(line.Mid(2));
					isPending=ETrue;
					}
				else if (line.Length()>1 && line[0]=='<')
					isPending=EFalse;
				}
			if (isPending)
				aCrashedStep.Copy(pending.Left(Min(pending.Length(),aCrashedStep.MaxLength())));
			delete buf;
			}
		f.Close();
		aFs.Delete(old);
		aFs.Rename(name,old);
		}
	User::LeaveIfError(iFile.Replace(aFs,name,EFileWrite|EFileShareAny|EFileStreamText));
	iOpen=ETrue;
	}

// a line, flushed to the disk at once
void TPsiKtLog::Line(const TDesC& aText)
	{
	if (!iOpen)
		return;
	TBuf8<KLineLen+12> b;
	TTime now;
	now.HomeTime();
	TDateTime dt=now.DateTime();
	b.Format(_L8("%02d:%02d:%02d "),dt.Hour(),dt.Minute(),dt.Second());
	TBuf8<KLineLen> t;
	t.Copy(aText.Left(Min(aText.Length(),KLineLen)));
	b.Append(t);
	b.Append(_L8("\r\n"));
	iFile.Write(b);
	iFile.Flush();
	}

// ----- the serial port -----------------------------------------------------

// COMM::0 (UART2), opened as the apps open it (psiglue.cpp OpenSerial)
class TPsiKtSerial
	{
public:
	TPsiKtSerial() : iServerOpen(EFalse), iOpen(EFalse) {}
	TInt Open(TBps aRate);
	TInt SetRate(TBps aRate);
	TInt Talk(const TDesC8& aCommand, TDes8& aReply, TInt aTimeoutMs);
	void Close();
private:
	RCommServ iServer;
	RComm iComm;
	TBool iServerOpen, iOpen;
	};

TInt TPsiKtSerial::Open(TBps aRate)
	{
	TInt r=User::LoadPhysicalDevice(_L("EUART1"));
	if (r==KErrNone || r==KErrAlreadyExists)
		r=User::LoadLogicalDevice(_L("ECOMM"));
	if (r==KErrNone || r==KErrAlreadyExists)
		r=StartC32();
	if (r==KErrNone || r==KErrAlreadyExists)
		{
		r=iServer.Connect();
		if (r==KErrNone)
			{
			iServerOpen=ETrue;
			r=iServer.LoadCommModule(_L("ECUART"));
			}
		}
	if (r==KErrNone || r==KErrAlreadyExists)
		r=iComm.Open(iServer,_L("COMM::0"),ECommExclusive);
	if (r!=KErrNone)
		{
		Close();
		return r;
		}
	iOpen=ETrue;
	iComm.SetReceiveBufferLength(4096);
	r=SetRate(aRate);
	if (r!=KErrNone)
		return r;
	// (DTR and the UART only come on with the first read or write)
	iComm.SetSignals(KSignalDTR|KSignalRTS,0);
	TRequestStatus st;
	TBuf8<4> none;
	iComm.Read(st,none,0);
	User::WaitForRequest(st);
	User::After(100000);
	return KErrNone;
	}

TInt TPsiKtSerial::SetRate(TBps aRate)
	{
	TCommConfig cfg;
	iComm.Config(cfg);
	cfg().iRate=aRate;
	cfg().iDataBits=EData8;
	cfg().iStopBits=EStop1;
	cfg().iParity=EParityNone;
	cfg().iFifo=EFifoEnable;
	cfg().iTerminatorCount=0;
	cfg().iHandshake=0;                  // the Atom has no RTS/CTS
	iComm.Cancel();
	return iComm.SetConfig(cfg);
	}

// Sends aCommand and collects the answer until OK or ERROR, or aTimeoutMs.
// KErrNone for OK, KErrGeneral for ERROR, KErrTimedOut for nothing.
TInt TPsiKtSerial::Talk(const TDesC8& aCommand, TDes8& aReply, TInt aTimeoutMs)
	{
	aReply.Zero();
	if (!iOpen)
		return KErrNotReady;
	iComm.ResetBuffers();
	TRequestStatus ws;
	iComm.Write(ws,aCommand);
	User::WaitForRequest(ws);
	if (ws.Int()!=KErrNone)
		return ws.Int();
	RTimer timer;
	TInt r=timer.CreateLocal();
	if (r!=KErrNone)
		return r;
	TRequestStatus ts;
	timer.After(ts,aTimeoutMs*1000);
	r=KErrTimedOut;
	TBuf8<64> piece;
	for (;;)
		{
		TRequestStatus rs;
		iComm.ReadOneOrMore(rs,piece);
		User::WaitForRequest(rs,ts);
		if (rs==KRequestPending)
			{
			iComm.ReadCancel();
			User::WaitForRequest(rs);
			break;                           // the timer: out of time
			}
		if (rs.Int()!=KErrNone)
			{
			r=rs.Int();
			break;
			}
		if (aReply.Length()+piece.Length()<=aReply.MaxLength())
			aReply.Append(piece);
		if (aReply.Find(_L8("OK\r\n"))>=0)
			{
			r=KErrNone;
			break;
			}
		if (aReply.Find(_L8("ERROR"))>=0)
			{
			r=KErrGeneral;
			break;
			}
		}
	if (ts==KRequestPending)
		{
		timer.Cancel();
		User::WaitForRequest(ts);
		}
	timer.Close();
	return r;
	}

void TPsiKtSerial::Close()
	{
	if (iOpen)
		{
		iComm.Cancel();
		iComm.Close();
		}
	if (iServerOpen)
		iServer.Close();
	iOpen=iServerOpen=EFalse;
	}

// ----- the view ------------------------------------------------------------

class CPsiKtView : public CCoeControl
	{
public:
	void ConstructL(const TRect& aRect);
	void AddLine(const TDesC& aText);
	void ShowRamp(TBool aOn, const TDesC& aLabel);
private:
	void Draw(const TRect& aRect) const;
	TBuf<KLineLen> iLines[KMaxLines];
	TInt iCount;
	TBool iRamp;
	TBuf<KLineLen> iRampLabel;
	};

void CPsiKtView::ConstructL(const TRect& aRect)
	{
	// 16 greys, as PsiTerm and PsiMail ask for (the System screen runs in
	// 4, and the palette then has only 4 entries in use)
	CreateBackedUpWindowL(iCoeEnv->RootWin(),EGray16);
	SetRectL(aRect);
	ActivateL();
	}

// a line, then the screen drawn and flushed at once
void CPsiKtView::AddLine(const TDesC& aText)
	{
	TInt h=iCoeEnv->NormalFont()->HeightInPixels()+1;
	TInt top=iRamp ? Rect().Height()/2 : 0;
	TInt rows=Min(KMaxLines,(Rect().Height()-top)/h);
	if (rows<1)
		rows=1;
	while (iCount>=rows)
		{
		for (TInt i=1; i<iCount; i++)
			iLines[i-1]=iLines[i];
		iCount--;
		}
	iLines[iCount++]=aText.Left(Min(aText.Length(),KLineLen));
	DrawNow();
	iCoeEnv->WsSession().Flush();
	}

void CPsiKtView::ShowRamp(TBool aOn, const TDesC& aLabel)
	{
	iRamp=aOn;
	iRampLabel=aLabel.Left(Min(aLabel.Length(),KLineLen));
	iCount=0;
	DrawNow();
	iCoeEnv->WsSession().Flush();
	}

void CPsiKtView::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc=SystemGc();
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.Clear(Rect());
	const CFont* f=iCoeEnv->NormalFont();
	gc.UseFont(f);
	TInt h=f->HeightInPixels()+1;
	TInt top=0;
	if (iRamp)
		{
		// the 16 greys as bars, with their numbers, then the label
		TInt w=Rect().Width()/16;
		TInt barH=Rect().Height()/2-2*h-4;
		for (TInt i=0; i<16; i++)
			{
			gc.SetBrushColor(TRgb::Gray16(i));
			gc.DrawRect(TRect(i*w,0,(i+1)*w,barH));
			}
		gc.SetBrushStyle(CGraphicsContext::ENullBrush);
		gc.SetPenStyle(CGraphicsContext::ESolidPen);
		gc.SetPenColor(KRgbBlack);
		for (TInt j=0; j<16; j++)
			{
			TBuf<4> n;
			n.Num(j);
			gc.DrawText(n,TPoint(j*w+w/2-f->TextWidthInPixels(n)/2,barH+h));
			}
		gc.DrawText(iRampLabel,TPoint(4,barH+2*h));
		top=Rect().Height()/2;
		}
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbBlack);
	for (TInt i=0; i<iCount; i++)
		gc.DrawText(iLines[i],TPoint(4,top+h*(i+1)));
	gc.DiscardFont();
	}

// ----- the app UI ----------------------------------------------------------

class CPsiKtAppUi : public CEikAppUi
	{
public:
	void ConstructL();
	~CPsiKtAppUi();
private:
	void HandleCommandL(TInt aCommand);
	void Say(const TDesC& aText);
	TBool BeginStepL(const TDesC& aName);
	void EndStep(const TDesC& aName, TInt aResult);
	TInt OpenDriver();
	void CheckL();
	void RegistersL();
	void PaletteL();
	void SpeedL();
	void InvertL();
	void CurvesL();
	void SerialL();
	void SerialFastL();
	void RomL();
	void KeepCurveL();
	void HandleKeyEventL(const TKeyEvent& aKeyEvent,TEventCode aType);
	TBool RomOk();
	TBool Ask(const TDesC& aFirst, const TDesC& aSecond);
	void EverythingL();
	TBool PaletteReady();
	void ReadPalette();
	TInt ApplyCurve(TInt aCurve);
	void RunSpeed(TInt& aLoopMs, TInt& aCopyMs, TInt& aRomMs);
	static TInt KeepTick(TAny* aSelf);
	void Keep();
	CPsiKtView* iView;
	TPsiKtLog iLog;
	TBuf<40> iCrashed;              // the step the last run stopped in, if any
	RPsiKern iKern;
	TBool iKernOpen;
	// the palette as EPOC set it, and a curve kept on it
	TInt iOrig[16];
	TInt iUsed;
	TBool iBlackHigh;
	TInt iApplied[16];                // the levels we wrote, or -1
	TInt iKeepCurve;                  // -1: none kept
	TBool iPicking;                   // keys 0-6 choose a curve
	TInt iReapplied;                  // how often EPOC set its own again
	TBool iAll;                       // Test everything: asked once, at the start
	CPeriodic* iKeeper;
	};

void CPsiKtAppUi::ConstructL()
	{
	BaseConstructL();
	iKeepCurve=-1;
	for (TInt k=0; k<16; k++)
		iApplied[k]=-1;
	iView=new(ELeave) CPsiKtView;
	iView->ConstructL(ClientRect());
	AddToStackL(iView);
	TBuf<4> where;
	TRAPD(err,iLog.OpenL(iCoeEnv->FsSession(),iCrashed,where));
	TBuf<KLineLen> l;
	l.Format(_L("PsiKernTest %S. Choose a test from the Read or Write menus."),&KPsiKtVersion);
	Say(l);
	if (err==KErrNone)
		{
		TPtrC log(KLogName), old(KOldName);
		l.Format(_L("Log: %S%S (the last one: %S)"),&where,&log,&old);
		Say(l);
		}
	else
		{
		l.Format(_L("No log: %d"),err);
		Say(l);
		}
	if (iCrashed.Length())
		{
		l.Format(_L("The last run stopped during \"%S\"."),&iCrashed);
		Say(l);
		}
	}

CPsiKtAppUi::~CPsiKtAppUi()
	{
	delete iKeeper;
	if (iKernOpen)
		{
		iKern.Restore();
		iKern.Close();
		}
	iLog.Line(_L("< closed"));
	iLog.Close();
	if (iView)
		{
		RemoveFromStack(iView);
		delete iView;
		}
	}

void CPsiKtAppUi::Say(const TDesC& aText)
	{
	iView->AddLine(aText);
	iLog.Line(aText);
	}

// The breadcrumb, and a question if this step stopped the last run.
TBool CPsiKtAppUi::BeginStepL(const TDesC& aName)
	{
	if (iCrashed.Length() && iCrashed==aName)
		{
		TBuf<KLineLen> q;
		q.Format(_L("The last run stopped during \"%S\""),&aName);
		if (!iEikonEnv->QueryWinL(q,_L("Run it again?")))
			return EFalse;
		iCrashed.Zero();
		}
	TBuf<KLineLen> l;
	l.Format(_L("> %S"),&aName);
	iLog.Line(l);
	return ETrue;
	}

void CPsiKtAppUi::EndStep(const TDesC& aName, TInt aResult)
	{
	TBuf<KLineLen> l;
	l.Format(_L("< %S: %d"),&aName,aResult);
	iLog.Line(l);
	}

TBool CPsiKtAppUi::RomOk()
	{
	TMachineInfoV1Buf mi;
	if (UserHal::MachineInfo(mi)!=KErrNone)
		return EFalse;
	const TVersion& v=mi().iRomVersion;
	return v.iMajor==KRomMajor && v.iMinor==KRomMinor && v.iBuild==KRomBuild &&
		mi().iDisplaySizeInPixels==TSize(640,240);
	}

// Loads the driver and opens a channel, once. Its own step, so a crash in
// the load is named as such.
TInt CPsiKtAppUi::OpenDriver()
	{
	if (iKernOpen)
		return KErrNone;
	if (!RomOk())
		{
		Say(_L("Not a 5mx with ROM 1.05(260): the driver is not loaded"));
		return KErrNotSupported;
		}
	_LIT(KStep,"Load the driver");
	TBool go=EFalse;
	TRAPD(err,go=BeginStepL(KStep));
	if (err!=KErrNone || !go)
		return KErrCancel;
	TInt r=User::LoadLogicalDevice(KPsiKernFile);
	TInt load=r;
	TBuf<KLineLen> l;
	l.Format(_L("Load driver: %d"),r);
	Say(l);
	if (r==KErrNone || r==KErrAlreadyExists)
		{
		r=iKern.Open();
		l.Format(_L("Open channel: %d"),r);
		Say(l);
		if (r==KErrNotSupported || (r==KErrNotFound && load==KErrAlreadyExists))
			Say(_L("Another version of the driver is still loaded: restart the Psion"));
		}
	if (r==KErrNone)
		{
		iKernOpen=ETrue;
		TInt v=iKern.Version();
		if (((TUint)v>>16)!=(TUint)KPsiKernMagic)
			{
			l.Format(_L("Wrong driver version %x: closing it"),v);
			Say(l);
			iKern.Close();
			iKernOpen=EFalse;
			r=KErrNotSupported;
			}
		}
	EndStep(KStep,r);
	return r;
	}

void CPsiKtAppUi::CheckL()
	{
	_LIT(KStep,"Machine and driver");
	if (!BeginStepL(KStep))
		return;
	TBuf<KLineLen> l;
	TMachineInfoV1Buf mi;
	TInt r=UserHal::MachineInfo(mi);
	if (r==KErrNone)
		{
		const TMachineInfoV1& m=mi();
		l.Format(_L("Machine: %S, ROM %d.%02d(%d)"),&m.iMachineName,m.iRomVersion.iMajor,
			m.iRomVersion.iMinor,m.iRomVersion.iBuild);
		Say(l);
		l.Format(_L("Processor: %S, %d kHz, speed factor %d"),&m.iProcessorName,
			m.iProcessorClockInKHz,m.iSpeedFactor);
		Say(l);
		l.Format(_L("Display %dx%d, %d colours"),m.iDisplaySizeInPixels.iWidth,
			m.iDisplaySizeInPixels.iHeight,m.iMaximumDisplayColors);
		Say(l);
		}
	else
		{
		l.Format(_L("Machine info: %d"),r);
		Say(l);
		}
	TMemoryInfoV1Buf mem;
	if (UserHal::MemoryInfo(mem)==KErrNone)
		{
		l.Format(_L("RAM %d KB, free %d KB; ROM %d KB"),mem().iTotalRamInBytes/1024,
			mem().iFreeRamInBytes/1024,mem().iTotalRomInBytes/1024);
		Say(l);
		}
	EndStep(KStep,r);
	r=OpenDriver();
	if (r==KErrNone)
		{
		TInt v=iKern.Version();
		l.Format(_L("Driver version %d.%d"),(v>>8)&0xff,v&0xff);
		Say(l);
		TInt m=iKern.CpuMode();
		TPtrC mode(m==0x10 ? _L("user") : m==0x13 ? _L("SVC") : m==0x1b ? _L("UND") :
			m==0x17 ? _L("ABT") : m==0x12 ? _L("IRQ") : m==0x1f ? _L("SYS") : _L("?"));
		l.Format(_L("CPU mode in the driver: %x (%S)"),m,&mode);
		Say(l);
		}
	}

void CPsiKtAppUi::RegistersL()
	{
	_LIT(KStep,"Registers");
	TInt r=OpenDriver();
	if (r!=KErrNone || !BeginStepL(KStep))
		return;
	TBuf<KLineLen> l;
	TUint32 first[KShowCount];
	TUint t0=0;
	for (TInt pass=0; pass<2; pass++)
		{
		if (pass==1)
			{
			User::After(1000000);
			Say(_L("A second later, what changed:"));
			}
		TUint t=User::TickCount();
		if (pass==0)
			t0=t;
		for (TInt i=0; i<KShowCount; i++)
			{
			TBuf<16> name;
			name.Copy(TPtrC8((const TUint8*)KShow[i].iName));
			TUint32 val=0;
			if (KShow[i].iWidth==4)
				r=iKern.Read32(KShow[i].iOffset,val);
			else
				{
				r=iKern.Read8(KShow[i].iOffset);
				if (r>=0) { val=r; r=KErrNone; }
				}
			if (r!=KErrNone)
				{
				if (pass==0)
					{
					l.Format(_L("%S: %d"),&name,r);
					Say(l);
					}
				first[i]=0xdeadbeef;
				continue;
				}
			if (pass==0)
				{
				first[i]=val;
				l.Format(_L("%S = %08x"),&name,val);
				Say(l);
				}
			else if (val!=first[i])
				{
				l.Format(_L("%S = %08x (was %08x)"),&name,val,first[i]);
				Say(l);
				}
			}
		if (pass==1)
			{
			TTimeIntervalMicroSeconds32 period;
			UserHal::TickPeriod(period);
			l.Format(_L("(%d ticks of %d us between the reads)"),t-t0,period.Int());
			Say(l);
			}
		}
	EndStep(KStep,KErrNone);
	}

void CPsiKtAppUi::PaletteL()
	{
	_LIT(KStep,"Palette");
	TInt r=OpenDriver();
	if (r!=KErrNone || !BeginStepL(KStep))
		return;
	TPckgBuf<TScreenInfoV01> info;
	UserSvr::ScreenInfo(info);
	TBuf<KLineLen> l;
	l.Format(_L("Screen at %08x, %dx%d; palette 32 bytes before it"),
		(TUint)info().iScreenAddress,info().iScreenSize.iWidth,info().iScreenSize.iHeight);
	Say(l);
	l.Copy(_L("Palette:"));
	for (TInt i=0; i<16; i++)
		{
		TInt e=iKern.PalRead(i);
		if (e<0)
			l.AppendFormat(_L(" [%d]"),e);
		else
			l.AppendFormat(_L(" %04x"),e);
		if (i==7)
			{
			Say(l);
			l.Copy(_L("        "));
			}
		}
	Say(l);
	r=iKern.PalCheck();
	if (r==KErrNone)
		Say(_L("It looks like the palette: the Write tests can run"));
	else
		Say(_L("It does not look like the palette: the Write tests will not write"));
	EndStep(KStep,r);
	}

// ticks to ms, with the tick period from the HAL
static TInt TicksToMs(TUint aTicks)
	{
	TTimeIntervalMicroSeconds32 period;
	if (UserHal::TickPeriod(period)!=KErrNone || period.Int()<=0)
		period=15625;
	return (TInt)((TInt64((TInt)aTicks)*TInt64(period.Int())/TInt64(1000)).Low());
	}

// The CPU, the memory and ROM code, timed by the tick. No driver.
//  - aLoopMs: 2,000,000 turns of a loop of a few instructions, in RAM;
//  - aCopyMs: 8 MB copied in RAM (Mem::Copy runs from the ROM, but its time
//    is the memory's);
//  - aRomMs: 200,000 calls of Mem::Compare on 16 bytes: code that runs in
//    place from the ROM, so the ROM's wait states show here.
void CPsiKtAppUi::RunSpeed(TInt& aLoopMs, TInt& aCopyMs, TInt& aRomMs)
	{
	const TInt KLoops=2000000;
	TUint t0=User::TickCount();
	volatile TInt sink=0;
	TInt acc=0;
	for (TInt i=KLoops; i>0; i--)
		acc+=i;
	sink=acc;
	aLoopMs=TicksToMs(User::TickCount()-t0);
	const TInt KSize=65536;
	HBufC8* a=HBufC8::New(KSize);
	HBufC8* b=HBufC8::New(KSize);
	aCopyMs=0;
	if (a && b)
		{
		a->Des().SetLength(KSize);
		b->Des().SetLength(KSize);
		TUint8* pa=(TUint8*)a->Ptr();
		TUint8* pb=(TUint8*)b->Ptr();
		t0=User::TickCount();
		for (TInt k=0; k<128; k++)
			Mem::Copy(k&1 ? pa : pb,k&1 ? pb : pa,KSize);
		aCopyMs=TicksToMs(User::TickCount()-t0);
		}
	delete a;
	delete b;
	TUint8 x[16], y[16];
	Mem::Fill(x,16,'x');
	Mem::Fill(y,16,'x');
	t0=User::TickCount();
	for (TInt n=0; n<200000; n++)
		acc+=Mem::Compare(x,16,y,16);
	sink=acc;
	aRomMs=TicksToMs(User::TickCount()-t0);
	(void)sink;
	}

void CPsiKtAppUi::SpeedL()
	{
	_LIT(KStep,"Speed");
	if (!BeginStepL(KStep))
		return;
	TInt loopMs, copyMs, romMs;
	RunSpeed(loopMs,copyMs,romMs);
	TBuf<KLineLen> l;
	l.Format(_L("Loop: 2000000 turns in %d ms"),loopMs);
	Say(l);
	l.Format(_L("Memory copy: 8 MB in %d ms (%d KB a second)"),copyMs,copyMs ? 8192*1000/copyMs : 0);
	Say(l);
	l.Format(_L("ROM code: 200000 calls in %d ms"),romMs);
	Say(l);
	EndStep(KStep,KErrNone);
	}

void CPsiKtAppUi::InvertL()
	{
	_LIT(KStep,"Invert");
	TInt r=OpenDriver();
	if (r!=KErrNone)
		return;
	if (!PaletteReady())
		return;
	if (!Ask(_L("The screen will be inverted for 3 seconds"),_L("Go ahead?")))
		return;
	if (!BeginStepL(KStep))
		return;
	r=iKern.Invert(ETrue);
	TBuf<KLineLen> l;
	l.Format(_L("Inverted: %d"),r);
	Say(l);
	User::After(3000000);
	iKern.Restore();
	Say(_L("Put back"));
	EndStep(KStep,r);
	}

// Says why not, if the palette can't be written
TBool CPsiKtAppUi::PaletteReady()
	{
	if (iKern.PalCheck()==KErrNone)
		return ETrue;
	Say(_L("Not available: that is not the palette (Read > Palette)"));
	return EFalse;
	}

// The palette as EPOC set it: how many entries are in use, and which way
// its levels run (on the 5mx black is the high level)
void CPsiKtAppUi::ReadPalette()
	{
	for (TInt e=0; e<16; e++)
		{
		TInt v=iKern.PalRead(e);
		iOrig[e]=v<0 ? 0 : v&15;
		}
	TInt code=(iKern.PalRead(0)>>12)&3;
	iUsed=code==0 ? 2 : code==1 ? 4 : 16;
	iBlackHigh=iOrig[0]>iOrig[iUsed-1];
	}

// Puts curve aCurve on the palette ReadPalette found (-1: EPOC's own)
TInt CPsiKtAppUi::ApplyCurve(TInt aCurve)
	{
	TInt r=KErrNone;
	for (TInt e=0; e<16 && r==KErrNone; e++)
		{
		TInt level;
		if (aCurve<0)
			level=iOrig[e];
		else
			{
			TInt grey=e<iUsed ? e*15/(iUsed-1) : 15;     // the grey this entry is for
			TInt light=KCurves[aCurve].iLevel[grey];
			level=iBlackHigh ? 15-light : light;
			}
		r=iKern.PalSet(e,level);
		iApplied[e]=r==KErrNone ? level : -1;
		}
	return r;
	}

void CPsiKtAppUi::CurvesL()
	{
	_LIT(KStep,"Curves");
	TInt r=OpenDriver();
	if (r!=KErrNone || !PaletteReady())
		return;
	if (iKeepCurve>=0)
		{
		Say(_L("Not available while a curve is kept (Write > Keep a grey curve, 0)"));
		return;
		}
	if (!Ask(_L("The greys will change for 35 seconds, then go back"),_L("Go ahead?")))
		return;
	if (!BeginStepL(KStep))
		return;
	ReadPalette();
	TBuf<KLineLen> l;
	l.Format(_L("%d greys in use; black is level %d"),iUsed,iOrig[0]);
	Say(l);
	iView->ShowRamp(ETrue,_L("As EPOC set it"));
	User::After(5000000);
	for (TInt c=0; c<KCurveCount && r==KErrNone; c++)
		{
		TBuf<KLineLen> name;
		name.Copy(TPtrC8((const TUint8*)KCurves[c].iName));
		r=ApplyCurve(c);
		iView->ShowRamp(ETrue,name);
		iLog.Line(name);
		User::After(5000000);
		}
	iKern.Restore();
	iView->ShowRamp(EFalse,KNullDesC);
	l.Format(_L("Grey curves done, put back: %d"),r);
	Say(l);
	EndStep(KStep,r);
	}

// ----- keeping a curve on ---------------------------------------------------
// Keys 1-6 put a curve on, 0 puts EPOC's own back, Esc or Enter ends the
// choosing and leaves the curve on. While PsiKernTest runs (in front or
// not), a timer looks every 2 s: if EPOC has set its own palette again (a 4-
// or 16-grey screen in front, contrast, switch-on), the curve goes back on,
// and the log says so. Closing PsiKernTest puts EPOC's palette back.

void CPsiKtAppUi::KeepCurveL()
	{
	TInt r=OpenDriver();
	if (r!=KErrNone || !PaletteReady())
		return;
	if (!iKeeper)
		{
		iKeeper=CPeriodic::NewL(CActive::EPriorityLow);
		iKeeper->Start(2000000,2000000,TCallBack(KeepTick,this));
		}
	if (iKeepCurve<0)
		ReadPalette();
	iPicking=ETrue;
	iView->ShowRamp(ETrue,_L("Keys 1-6: a curve.  0: EPOC's own.  Enter: keep it on"));
	for (TInt c=0; c<KCurveCount; c++)
		{
		TBuf<KLineLen> l, name;
		name.Copy(TPtrC8((const TUint8*)KCurves[c].iName));
		l.Format(_L("%d  %S"),c+1,&name);
		iView->AddLine(l);
		}
	}

void CPsiKtAppUi::HandleKeyEventL(const TKeyEvent& aKeyEvent,TEventCode aType)
	{
	if (!iPicking || aType!=EEventKey)
		return;
	TInt key=aKeyEvent.iCode;
	TBuf<KLineLen> l;
	if (key>='0' && key<='0'+KCurveCount)
		{
		TInt c=key-'1';                      // ('0' gives -1: EPOC's own)
		if (iKeepCurve>=0 && c<0)
			{
			ReadPalette();                   // (what EPOC has now)
			iKern.Restore();
			}
		else
			{
			if (iKeepCurve<0)
				ReadPalette();
			ApplyCurve(c);
			}
		iKeepCurve=c;
		if (c<0)
			l.Copy(_L("Kept: EPOC's own"));
		else
			{
			TBuf<KLineLen> name;
			name.Copy(TPtrC8((const TUint8*)KCurves[c].iName));
			l.Format(_L("Kept: %S"),&name);
			}
		iView->ShowRamp(ETrue,l);
		iLog.Line(l);
		}
	else if (key==EKeyEscape || key==EKeyEnter)
		{
		iPicking=EFalse;
		iView->ShowRamp(EFalse,KNullDesC);
		if (iKeepCurve>=0)
			Say(_L("The curve stays on while PsiKernTest is open (in front or not)"));
		}
	}

TInt CPsiKtAppUi::KeepTick(TAny* aSelf)
	{
	((CPsiKtAppUi*)aSelf)->Keep();
	return 0;
	}

void CPsiKtAppUi::Keep()
	{
	if (iKeepCurve<0 || !iKernOpen)
		return;
	TBool ours=ETrue;
	for (TInt e=0; e<16 && ours; e++)
		{
		TInt v=iKern.PalRead(e);
		if (v<0 || (v&15)!=iApplied[e])
			ours=EFalse;
		}
	if (ours)
		return;
	// EPOC has set its own palette again: take it, and put the curve back
	if (iKern.PalCheck()!=KErrNone)
		return;
	ReadPalette();
	TInt r=ApplyCurve(iKeepCurve);
	iReapplied++;
	TBuf<KLineLen> l;
	l.Format(_L("EPOC set its palette (%d greys): curve back on (%d times): %d"),iUsed,iReapplied,r);
	iLog.Line(l);
	}

// ----- the serial port ------------------------------------------------------

// Opens COMM::0 at three speeds and reads UART2's divider each time:
// UBRCR = 7372800 / (16 x rate) - 1, so 47, 7 and 3 are expected.
void CPsiKtAppUi::SerialL()
	{
	_LIT(KStep,"Serial port");
	TInt r=OpenDriver();
	if (r!=KErrNone || !BeginStepL(KStep))
		return;
	TBuf<KLineLen> l;
	TPsiKtSerial port;
	r=port.Open(EBps9600);
	if (r!=KErrNone)
		{
		l.Format(_L("Could not open the serial port: %d (the Remote link may have it)"),r);
		Say(l);
		EndStep(KStep,r);
		return;
		}
	static const TBps KRates[]={EBps9600,EBps57600,EBps115200};
	static const TInt KBauds[]={9600,57600,115200};
	for (TInt i=0; i<3; i++)
		{
		if (i)
			r=port.SetRate(KRates[i]);
		TUint32 ubrcr=0;
		TInt con=iKern.Read8(ERegU2Con);
		TInt rr=iKern.Read32(ERegU2Ubrcr,ubrcr);
		l.Format(_L("%d: UBRCR %d (expected %d), CON %x  [%d %d]"),KBauds[i],ubrcr,
			7372800/(16*KBauds[i])-1,con,r,rr);
		Say(l);
		}
	port.Close();
	EndStep(KStep,KErrNone);
	}

// A reply in one line for the log: control characters as dots
static void ReplyLine(const TDesC8& aReply, TDes& aOut)
	{
	aOut.Zero();
	for (TInt i=0; i<aReply.Length() && aOut.Length()<aOut.MaxLength(); i++)
		{
		TUint c=aReply[i];
		aOut.Append(c>=32 && c<127 ? (TChar)c : (TChar)'.');
		}
	}

// Printable, CR and LF only: a garbled reply has other bytes
static TBool Clean(const TDesC8& aReply)
	{
	for (TInt i=0; i<aReply.Length(); i++)
		{
		TUint c=aReply[i];
		if (!(c>=32 && c<127) && c!='\r' && c!='\n')
			return EFalse;
		}
	return ETrue;
	}

// 230400 with the Atom modem: AT$SB=230400 at 115200, then UART2's divider
// to 1, then ATI ten times. Back to 115200 at the end, or if anything fails
// (the Atom goes back by itself 15 s after a speed change that nothing at
// the new speed confirms).
void CPsiKtAppUi::SerialFastL()
	{
	_LIT(KStep,"Serial 230400");
	TInt r=OpenDriver();
	if (r!=KErrNone)
		return;
	if (!Ask(_L("The Atom modem must be connected and idle"),_L("Try 230400 with it?")))
		return;
	if (!BeginStepL(KStep))
		return;
	TBuf<KLineLen> l;
	TBuf<KLineLen> shown;
	TBuf8<512> reply;
	TPsiKtSerial port;
	r=port.Open(EBps115200);
	if (r!=KErrNone)
		{
		l.Format(_L("Could not open the serial port: %d"),r);
		Say(l);
		EndStep(KStep,r);
		return;
		}
	r=port.Talk(_L8("AT\r"),reply,2000);
	if (r!=KErrNone)
		r=port.Talk(_L8("AT\r"),reply,2000);
	if (r!=KErrNone)
		{
		l.Format(_L("No modem answers at 115200: %d"),r);
		Say(l);
		port.Close();
		EndStep(KStep,r);
		return;
		}
	Say(_L("115200: the modem answers"));
	TUint t0=User::TickCount();
	r=port.Talk(_L8("ATI\r"),reply,3000);
	TInt slowMs=TicksToMs(User::TickCount()-t0);
	TInt slowLen=reply.Length();
	l.Format(_L("115200: ATI, %d bytes in %d ms: %d"),slowLen,slowMs,r);
	Say(l);
	r=port.Talk(_L8("AT$SB=230400\r"),reply,2000);
	if (r!=KErrNone)
		{
		ReplyLine(reply,shown);
		l.Format(_L("The modem refused AT$SB=230400: %d %S"),r,&shown);
		Say(l);
		port.Close();
		EndStep(KStep,r);
		return;
		}
	User::After(300000);                     // (the Atom switches after its OK)
	iLog.Line(_L("> UBRCR 1"));
	r=iKern.UbrcrSet(1);
	l.Format(_L("UART2 at 230400: %d"),r);
	Say(l);
	TInt ok=0, clean=0, fastMs=0, fastLen=0;
	if (r==KErrNone)
		{
		r=port.Talk(_L8("AT\r"),reply,2000);
		if (r!=KErrNone)
			r=port.Talk(_L8("AT\r"),reply,2000);
		ReplyLine(reply,shown);
		l.Format(_L("230400: AT: %d %S"),r,&shown);
		Say(l);
		if (r==KErrNone)
			{
			for (TInt n=0; n<10; n++)
				{
				t0=User::TickCount();
				TInt rr=port.Talk(_L8("ATI\r"),reply,3000);
				fastMs+=TicksToMs(User::TickCount()-t0);
				fastLen+=reply.Length();
				if (rr==KErrNone) ok++;
				if (Clean(reply)) clean++;
				}
			l.Format(_L("230400: ATI ten times: %d answered, %d clean, %d bytes in %d ms"),
				ok,clean,fastLen,fastMs);
			Say(l);
			// back to 115200 at both ends
			r=port.Talk(_L8("AT$SB=115200\r"),reply,2000);
			User::After(300000);
			}
		}
	iKern.UbrcrSet(3);
	iLog.Line(_L("< UBRCR 3"));
	r=port.Talk(_L8("AT\r"),reply,2000);
	if (r!=KErrNone)
		{
		Say(_L("Waiting 16 s for the modem to go back to 115200 by itself..."));
		User::After(16000000);
		r=port.Talk(_L8("AT\r"),reply,2000);
		}
	l.Format(_L("Back at 115200: the modem %S"),r==KErrNone ? &_L("answers") : &_L("does not answer"));
	Say(l);
	port.Close();
	if (ok==10 && clean==10)
		Say(_L("230400 works with the Atom"));
	EndStep(KStep,ok==10 && clean==10 ? KErrNone : KErrGeneral);
	}

// ----- the ROM's wait states --------------------------------------------------

// Each value in turn with ERomProbe (interrupts off, the ROM read as data
// only). Then, if one passes and Dan agrees, the fastest that passed is
// kept for one speed test, and put back.
void CPsiKtAppUi::RomL()
	{
	_LIT(KStep,"ROM timing");
	TInt r=OpenDriver();
	if (r!=KErrNone)
		return;
	if (!Ask(_L("The ROM will be read at faster settings, a moment each"),_L("Go ahead?")))
		return;
	if (!BeginStepL(KStep))
		return;
	struct TTry { TUint8 iByte; const char* iName; };
	// fastest first
	static const TTry KTries[]=
		{
		{0x7c,"50 ns, 20 ns sequential"},{0x5c,"50 ns"},
		{0x78,"75 ns, 20 ns sequential"},{0x58,"75 ns"},
		{0x74,"100 ns, 20 ns sequential"},{0x54,"100 ns"},
		{0x70,"125 ns, 20 ns sequential"},
		};
	const TInt KTryCount=sizeof(KTries)/sizeof(KTries[0]);
	TBuf<KLineLen> l, name;
	TInt best=-1;
	// slowest first, so the first trouble is the mildest
	for (TInt i=KTryCount-1; i>=0; i--)
		{
		name.Copy(TPtrC8((const TUint8*)KTries[i].iName));
		l.Format(_L("> ROM probe %x"),KTries[i].iByte);
		iLog.Line(l);
		r=iKern.RomProbe(KTries[i].iByte);
		l.Format(_L("< ROM probe %x: %d"),KTries[i].iByte,r);
		iLog.Line(l);
		l.Format(_L("ROM at %S: %S"),&name,r==0 ? &_L("reads the same") : r==1 ? &_L("reads differently") : &_L("not tried"));
		if (r<0)
			l.AppendFormat(_L(" (%d)"),r);
		Say(l);
		if (r<0)
			break;
		if (r==0)
			best=i;
		}
	if (best<0 && r==KErrNotSupported)
		{
		Say(_L("Not available: the ROM is not set as on the 5mx this was made for (MEMCFG1)"));
		EndStep(KStep,r);
		return;
		}
	if (best<0)
		{
		Say(_L("No faster setting reads the ROM correctly: nothing to keep"));
		EndStep(KStep,r);
		return;
		}
	name.Copy(TPtrC8((const TUint8*)KTries[best].iName));
	TBuf<KLineLen> q;
	q.Format(_L("The ROM reads correctly at %S"),&name);
	if (!Ask(q,_L("Run on it for a speed test (a few seconds)?")))
		{
		EndStep(KStep,KErrNone);
		return;
		}
	TInt loop0, copy0, rom0, loop1=0, copy1=0, rom1=0;
	RunSpeed(loop0,copy0,rom0);
	l.Format(_L("> ROM keep %x"),KTries[best].iByte);
	iLog.Line(l);
	r=iKern.RomKeep(KTries[best].iByte);
	if (r==KErrNone)
		RunSpeed(loop1,copy1,rom1);
	iKern.Restore();
	l.Format(_L("< ROM keep %x: %d"),KTries[best].iByte,r);
	iLog.Line(l);
	l.Format(_L("Normal: ROM code %d ms, copy %d ms, loop %d ms"),rom0,copy0,loop0);
	Say(l);
	l.Format(_L("%S: ROM code %d ms, copy %d ms, loop %d ms"),&name,rom1,copy1,loop1);
	Say(l);
	if (r==KErrNone && rom0>0)
		{
		l.Format(_L("ROM code %d%% faster; put back to normal"),(rom0-rom1)*100/rom0);
		Say(l);
		}
	EndStep(KStep,r);
	}

// A question before a write test, unless Test everything has asked already
TBool CPsiKtAppUi::Ask(const TDesC& aFirst, const TDesC& aSecond)
	{
	if (iAll)
		{
		iLog.Line(aFirst);
		return ETrue;
		}
	return iEikonEnv->QueryWinL(aFirst,aSecond);
	}

// Every test in turn, the safest first, with one question at the start.
// Each still writes its "> step" line first, so a test that stops the
// machine is named on the next start, and asked about before it runs again.
// The 230400 test needs the Atom: without it, it says so and goes on.
void CPsiKtAppUi::EverythingL()
	{
	if (!iEikonEnv->QueryWinL(_L("All tests: screen, 230400 with the Atom, ROM timing"),
		_L("Back up C: first. Run them all?")))
		return;
	if (iKeepCurve>=0)
		{
		Say(_L("Not available while a curve is kept (Write > Keep a grey curve, 0)"));
		return;
		}
	iAll=ETrue;
	CheckL();
	if (!iKernOpen)
		{
		iAll=EFalse;
		Say(_L("Stopped: the driver is not available, so only the speed test can run"));
		Say(_L("If another version is loaded, restart the Psion and try again"));
		return;
		}
	TRAPD(err,
		SpeedL();
		RegistersL();
		PaletteL();
		SerialL();
		InvertL();
		CurvesL();
		SerialFastL();
		RomL();
		);
	iAll=EFalse;
	TBuf<KLineLen> l;
	l.Format(_L("All tests done: %d (the log has every result)"),err);
	Say(l);
	User::LeaveIfError(err);
	}

void CPsiKtAppUi::HandleCommandL(TInt aCommand)
	{
	switch (aCommand)
		{
	case EEikCmdExit:
		Exit();
		break;
	case EPsiKtCmdCheck:
		CheckL();
		break;
	case EPsiKtCmdRegisters:
		RegistersL();
		break;
	case EPsiKtCmdPalette:
		PaletteL();
		break;
	case EPsiKtCmdSpeed:
		SpeedL();
		break;
	case EPsiKtCmdSerial:
		SerialL();
		break;
	case EPsiKtCmdReadAll:
		CheckL();
		SpeedL();
		RegistersL();
		PaletteL();
		SerialL();
		Say(_L("Read tests done"));
		break;
	case EPsiKtCmdInvert:
		InvertL();
		break;
	case EPsiKtCmdCurves:
		CurvesL();
		break;
	case EPsiKtCmdKeepCurve:
		KeepCurveL();
		break;
	case EPsiKtCmdSerialFast:
		SerialFastL();
		break;
	case EPsiKtCmdRom:
		RomL();
		break;
	case EPsiKtCmdEverything:
		EverythingL();
		break;
		}
	}

// ----- the application -----------------------------------------------------

class CPsiKtDocument : public CEikDocument
	{
public:
	CPsiKtDocument(CEikApplication& aApp) : CEikDocument(aApp) {}
private:
	CEikAppUi* CreateAppUiL() { return new(ELeave) CPsiKtAppUi; }
	};

class CPsiKtApplication : public CEikApplication
	{
private:
	CApaDocument* CreateDocumentL() { return new(ELeave) CPsiKtDocument(*this); }
	TUid AppDllUid() const { return KUidPsiKt; }
	};

EXPORT_C CApaApplication* NewApplication()
	{
	return new CPsiKtApplication;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
