// psikt.cpp - PsiKernTest, the test program for PsiKern.ldd.
// EXPERIMENTAL, emulator only: never ship this in a .sis or a release.
//
// A bare EIKON app. At start it loads the driver, opens a channel, asks its
// version and CPU mode, and reads a list of Windermere registers from kernel
// side. Each step is shown as a line, and the screen is drawn after every
// step, so if a step crashes the machine the last line says how far it got.
// The one write (inverting the LCD palette for 3 s, then putting it back)
// only runs if a file \PSIKERN.WR exists on C: or D:.
#include <eikenv.h>
#include <eikapp.h>
#include <eikdoc.h>
#include <eikappui.h>
#include <coecntrl.h>
#include <f32file.h>
#include <eikcmds.hrh>
#include "../psikern.h"

const TUid KUidPsiKt={0x0100FA11};
const TInt KMaxLines=20;

struct TRegShow { TInt iOffset; TInt iWidth; const char* iName; };
static const TRegShow KShow[]=
	{
	{ERegPwrCnt,4,"PWRCNT"},{ERegPwrSr,4,"PWRSR"},
	{ERegLcdCtl,4,"LCDCTL"},{ERegLcdSt,4,"LCDST"},{ERegLcdDbar1,4,"LCDDBAR1"},
	{ERegLcdT0,4,"LCDT0"},{ERegLcdT1,4,"LCDT1"},{ERegLcdT2,4,"LCDT2"},
	{ERegMemCfg1,4,"MEMCFG1"},{ERegDramCfg,4,"DRAMCFG"},
	{ERegU2Lcr,4,"UART2 LCR"},{ERegU2Con,1,"UART2 CON"},{ERegU2Flg,1,"UART2 FLG"},
	{ERegTc1Val,4,"TC1 VALUE"},{ERegRtcL,4,"RTC LOW"},
	};

class CPsiKtView : public CCoeControl
	{
public:
	void ConstructL(const TRect& aRect);
	void AddLine(const TDesC& aText);
	void RunTestsL();
private:
	void Draw(const TRect& aRect) const;
	TBuf<64> iLines[KMaxLines];
	TInt iCount;
	};

void CPsiKtView::ConstructL(const TRect& aRect)
	{
	CreateWindowL();
	SetRectL(aRect);
	ActivateL();
	}

// a line, then the screen drawn and flushed at once (so a crash after it
// still leaves it on screen)
void CPsiKtView::AddLine(const TDesC& aText)
	{
	// scroll once the screen is full, so the latest line is always shown
	TInt rows=Min(KMaxLines,Rect().Height()/(iCoeEnv->NormalFont()->HeightInPixels()+1));
	if (iCount >= rows)
		{
		for (TInt i=1; i<iCount; i++) iLines[i-1]=iLines[i];
		iCount--;
		}
	iLines[iCount++]=aText.Left(Min(aText.Length(),64));
	DrawNow();
	iCoeEnv->WsSession().Flush();
	User::After(200000);
	}

void CPsiKtView::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc=SystemGc();
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.Clear(Rect());
	const CFont* f=iCoeEnv->NormalFont();
	gc.UseFont(f);
	TInt h=f->HeightInPixels()+1;
	for (TInt i=0; i<iCount; i++)
		gc.DrawText(iLines[i],TPoint(4,h*(i+1)));
	gc.DiscardFont();
	}

static TBool WriteAllowed(RFs& aFs)
	{
	TEntry e;
	return aFs.Entry(_L("C:\\PSIKERN.WR"),e)==KErrNone || aFs.Entry(_L("D:\\PSIKERN.WR"),e)==KErrNone;
	}

void CPsiKtView::RunTestsL()
	{
	TBuf<64> l;
	AddLine(_L("PsiKernTest: loading PSIKERN.LDD..."));
	TInt r=User::LoadLogicalDevice(KPsiKernFile);
	l.Format(_L("LoadLogicalDevice: %d"),r);
	AddLine(l);
	if (r!=KErrNone && r!=KErrAlreadyExists)
		return;
	RPsiKern k;
	r=k.Open();
	l.Format(_L("Open channel: %d"),r);
	AddLine(l);
	if (r!=KErrNone)
		return;
	TInt v=k.Version();
	l.Format(_L("Version: %x (magic %x, %d.%d)"),v,(TUint)v>>16,(v>>8)&0xff,v&0xff);
	AddLine(l);
	TInt m=k.CpuMode();
	// (user mode is 0x10; anything else is a privileged mode: 0x13 SVC,
	// 0x1b UND, which is what the emulator shows for the driver)
	TPtrC mode(m==0x10 ? _L("user, not privileged") : m==0x13 ? _L("SVC, privileged") : m==0x1b ? _L("UND, privileged") : _L("privileged"));
	l.Format(_L("CPU mode in driver: %x (%S)"),m,&mode);
	AddLine(l);
	for (TUint i=0; i<sizeof(KShow)/sizeof(KShow[0]); i++)
		{
		TBuf<16> name;
		name.Copy(TPtrC8((const TUint8*)KShow[i].iName));
		if (KShow[i].iWidth==4)
			{
			TUint32 val=0;
			r=k.Read32(KShow[i].iOffset,val);
			if (r==KErrNone) l.Format(_L("%S = %08x"),&name,val);
			else l.Format(_L("%S: %d"),&name,r);
			}
		else
			{
			r=k.Read8(KShow[i].iOffset);
			if (r>=0) l.Format(_L("%S = %02x"),&name,r);
			else l.Format(_L("%S: %d"),&name,r);
			}
		AddLine(l);
		}
	if (WriteAllowed(iCoeEnv->FsSession()))
		{
		r=k.Invert(ETrue);
		l.Format(_L("Palette inverted: %d (3 s)"),r);
		AddLine(l);
		User::After(3000000);
		r=k.Invert(EFalse);
		l.Format(_L("Palette restored: %d"),r);
		AddLine(l);
		}
	else
		AddLine(_L("Write test off (no \\PSIKERN.WR)"));
	k.Close();
	AddLine(_L("Channel closed. Done."));
	}

class CPsiKtAppUi : public CEikAppUi
	{
public:
	void ConstructL();
	~CPsiKtAppUi();
private:
	void HandleCommandL(TInt aCommand);
	static TInt StartTests(TAny* aSelf);
	CPsiKtView* iView;
	CIdle* iIdle;
	};

void CPsiKtAppUi::ConstructL()
	{
	BaseConstructL();
	iView=new(ELeave) CPsiKtView;
	iView->ConstructL(ClientRect());
	AddToStackL(iView);
	iIdle=CIdle::NewL(CActive::EPriorityIdle);
	iIdle->Start(TCallBack(StartTests,this));      // once the window is up
	}

CPsiKtAppUi::~CPsiKtAppUi()
	{
	delete iIdle;
	if (iView)
		{
		RemoveFromStack(iView);
		delete iView;
		}
	}

TInt CPsiKtAppUi::StartTests(TAny* aSelf)
	{
	CPsiKtAppUi* self=(CPsiKtAppUi*)aSelf;
	TRAPD(err,self->iView->RunTestsL());
	if (err!=KErrNone)
		{
		TBuf<32> l;
		l.Format(_L("Left with %d"),err);
		self->iView->AddLine(l);
		}
	return 0;
	}

void CPsiKtAppUi::HandleCommandL(TInt aCommand)
	{
	if (aCommand==EEikCmdExit)
		Exit();
	}

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
