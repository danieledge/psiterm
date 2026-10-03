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
#include <eikcmds.hrh>
#include "psikt.hrh"
#include "../psikern.h"

const TUid KUidPsiKt={0x0100FA11};
const TInt KMaxLines=40;
const TInt KLineLen=80;
_LIT(KLogName,"PsiKern.log");
_LIT(KOldName,"PsiKern.old");
_LIT(KPsiKtVersion,"0.2");

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
	TBool RomOk();
	CPsiKtView* iView;
	TPsiKtLog iLog;
	TBuf<40> iCrashed;              // the step the last run stopped in, if any
	RPsiKern iKern;
	TBool iKernOpen;
	};

void CPsiKtAppUi::ConstructL()
	{
	BaseConstructL();
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
	TBuf<KLineLen> l;
	l.Format(_L("Load driver: %d"),r);
	Say(l);
	if (r==KErrNone || r==KErrAlreadyExists)
		{
		r=iKern.Open();
		l.Format(_L("Open channel: %d"),r);
		Say(l);
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

// The CPU and the memory, timed by the tick. No driver.
void CPsiKtAppUi::SpeedL()
	{
	_LIT(KStep,"Speed");
	if (!BeginStepL(KStep))
		return;
	TBuf<KLineLen> l;
	// a loop of 3 instructions (add, subs, bne)
	const TInt KLoops=2000000;
	TUint t0=User::TickCount();
	volatile TInt sink=0;
	TInt acc=0;
	for (TInt i=KLoops; i>0; i--)
		acc+=i;
	sink=acc;
	TUint t1=User::TickCount();
	TInt ms=TicksToMs(t1-t0);
	l.Format(_L("Loop: %d turns in %d ms (%d k turns a second)"),KLoops,ms,ms ? KLoops/ms : 0);
	Say(l);
	// memory: copy 64 KB 32 times
	const TInt KSize=65536;
	HBufC8* a=HBufC8::NewLC(KSize);
	HBufC8* b=HBufC8::NewLC(KSize);
	a->Des().SetLength(KSize);
	b->Des().SetLength(KSize);
	TUint8* pa=(TUint8*)a->Ptr();
	TUint8* pb=(TUint8*)b->Ptr();
	t0=User::TickCount();
	for (TInt k=0; k<32; k++)
		Mem::Copy(k&1 ? pa : pb,k&1 ? pb : pa,KSize);
	t1=User::TickCount();
	ms=TicksToMs(t1-t0);
	l.Format(_L("Memory copy: 2 MB in %d ms (%d KB a second)"),ms,ms ? 2048*1000/ms : 0);
	Say(l);
	CleanupStack::PopAndDestroy(2);
	(void)sink;
	EndStep(KStep,KErrNone);
	}

void CPsiKtAppUi::InvertL()
	{
	_LIT(KStep,"Invert");
	TInt r=OpenDriver();
	if (r!=KErrNone)
		return;
	if (iKern.PalCheck()!=KErrNone)
		{
		Say(_L("Not available: that is not the palette (Read > Palette)"));
		return;
		}
	if (!iEikonEnv->QueryWinL(_L("The screen will be inverted for 3 seconds"),_L("Go ahead?")))
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

void CPsiKtAppUi::CurvesL()
	{
	_LIT(KStep,"Curves");
	TInt r=OpenDriver();
	if (r!=KErrNone)
		return;
	if (iKern.PalCheck()!=KErrNone)
		{
		Say(_L("Not available: that is not the palette (Read > Palette)"));
		return;
		}
	if (!iEikonEnv->QueryWinL(_L("The greys will change for 35 seconds, then go back"),_L("Go ahead?")))
		return;
	if (!BeginStepL(KStep))
		return;
	// the palette as it was: how many entries are in use, and which way
	// its levels run (black first is the high level on the 5mx)
	TInt e0=iKern.PalRead(0);
	TInt code=(e0>>12)&3;
	TInt used=code==0 ? 2 : code==1 ? 4 : 16;
	TBool blackHigh=(e0&15)>(iKern.PalRead(used-1)&15);
	TBuf<KLineLen> l;
	l.Format(_L("%d greys in use; black is level %d"),used,e0&15);
	Say(l);
	iView->ShowRamp(ETrue,_L("As EPOC set it"));
	User::After(5000000);
	for (TInt c=0; c<KCurveCount && r==KErrNone; c++)
		{
		TBuf<KLineLen> name;
		name.Copy(TPtrC8((const TUint8*)KCurves[c].iName));
		for (TInt e=0; e<16 && r==KErrNone; e++)
			{
			TInt grey=e<used ? e*15/(used-1) : 15;     // the grey this entry is for
			TInt light=KCurves[c].iLevel[grey];
			r=iKern.PalSet(e,blackHigh ? 15-light : light);
			}
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
	case EPsiKtCmdReadAll:
		CheckL();
		SpeedL();
		RegistersL();
		PaletteL();
		Say(_L("Read tests done"));
		break;
	case EPsiKtCmdInvert:
		InvertL();
		break;
	case EPsiKtCmdCurves:
		CurvesL();
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
