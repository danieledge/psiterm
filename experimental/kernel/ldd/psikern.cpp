// psikern.cpp - PsiKern.ldd, an experimental logical device driver for the
// Psion Series 5mx (EPOC R5, ROM 1.05(260)).
//
// EXPERIMENTAL. Kernel code: a bug here takes the whole machine down, and C:
// is a RAM disk. Develop in the emulator; on a real 5mx back up C: first.
// Never put this in a .sis or a release. See ../README.md.
//
// What it does, all from DoControl:
//  - EVersion, ECpuMode: proves the driver loaded and runs in a privileged mode;
//  - ERead32/ERead8/ELatchHigh: reads a fixed list of Windermere registers
//    that have no read side effects (no data, FIFO or end-of-interrupt ones);
//  - EInvert: the one write. It inverts the 16 grey levels in the LCD
//    palette (the 32 bytes before the frame buffer, which the LCD controller
//    reads every frame), and puts the saved bytes back on EInvert 0 and when
//    the channel closes. It writes RAM only, never a register.
// It never touches user memory: every answer is DoControl's return value.
#include "kern.h"
#include <e32svr.h>
#include "../psikern.h"

const TUint32 KRegBase=0x58000000;      // the kernel's own "mov rX,#0x58000000"
const TInt KPaletteBytes=32;            // 16 entries of 16 bits

// readable registers: offset and access width (4 or 1)
struct TRegOk { TUint16 iOffset; TUint16 iWidth; };
static const TRegOk KReadable[]=
	{
	{ERegMemCfg1,4},{ERegMemCfg2,4},{ERegDramCfg,4},
	{ERegLcdCtl,4},{ERegLcdSt,4},{ERegLcdDbar1,4},{ERegLcdT0,4},{ERegLcdT1,4},{ERegLcdT2,4},
	{ERegPwrSr,4},{ERegPwrCnt,4},
	{ERegIntSr,4},{ERegIntRsr,4},{ERegIntEns,4},
	{ERegU2Fcr,4},{ERegU2Lcr,4},{ERegU2Con,1},{ERegU2Flg,1},{ERegU2IntM,1},
	{ERegTc1Load,4},{ERegTc1Val,4},{ERegTc1Ctrl,1},
	{ERegTc2Load,4},{ERegTc2Val,4},{ERegTc2Ctrl,1},
	{ERegRtcL,4},{ERegRtcU,4},
	{ERegPbdr,1},{ERegPcdr,1},{ERegPddr,1},{ERegPedr,1},
	};

static TBool Readable(TInt aOffset,TInt aWidth)
	{
	for (TUint i=0; i<sizeof(KReadable)/sizeof(KReadable[0]); i++)
		if (KReadable[i].iOffset==aOffset && KReadable[i].iWidth==aWidth)
			return ETrue;
	return EFalse;
	}

static TUint CpuMode()
	{
	TUint cpsr;
	asm volatile("mrs %0, cpsr" : "=r"(cpsr));
	return cpsr&0xff;
	}

class DPsiKernDevice : public DLogicalDevice
	{
public:
	DPsiKernDevice();
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual DLogicalChannel* CreateL();
	};

class DPsiKernChannel : public DLogicalChannel
	{
public:
	DPsiKernChannel(DLogicalDevice* aDevice);
	~DPsiKernChannel();
protected:
	virtual void DoCancel(TInt aReqNo);
	virtual void DoRequest(TInt aReqNo,TAny* a1,TAny* a2);
	virtual void DoCreateL(TInt aUnit,CBase* aPdd,const TDesC8* aInfo,const TVersion& aVer);
	virtual TInt DoControl(TInt aFunction,TAny* a1,TAny* a2);
private:
	TUint8* Palette();
	TInt Invert(TBool aOn);
	void Restore();
private:
	TUint32 iLatch;                    // the last ERead32
	TUint8* iPalette;                  // non-NULL while inverted
	TUint8 iSaved[KPaletteBytes];      // the palette as we found it
	TUint8 iWritten[KPaletteBytes];    // what we put there
	};

// --- the device

DPsiKernDevice::DPsiKernDevice()
	{
	iVersion=TVersion(KPsiKernMajor,KPsiKernMinor,KPsiKernBuild);
	iParseMask=0;       // no unit, no physical device, no info
	iUnitsMask=0;
	}

TInt DPsiKernDevice::Install()
	{
	return SetName(&KPsiKernName);
	}

void DPsiKernDevice::GetCaps(TDes8& aDes) const
	{
	TVersion v(KPsiKernMajor,KPsiKernMinor,KPsiKernBuild);
	aDes.FillZ(aDes.MaxLength());
	aDes.Copy((const TUint8*)&v,Min(aDes.MaxLength(),(TInt)sizeof(v)));
	}

DLogicalChannel* DPsiKernDevice::CreateL()
	{
	return new(ELeave) DPsiKernChannel(this);
	}

// --- the channel

DPsiKernChannel::DPsiKernChannel(DLogicalDevice* aDevice)
	: DLogicalChannel(aDevice)
	{
	}

DPsiKernChannel::~DPsiKernChannel()
	{
	Restore();      // never leave the screen inverted
	}

void DPsiKernChannel::DoCreateL(TInt /*aUnit*/,CBase* /*aPdd*/,const TDesC8* /*aInfo*/,const TVersion& aVer)
	{
	if (!User::QueryVersionSupported(TVersion(KPsiKernMajor,KPsiKernMinor,KPsiKernBuild),aVer))
		User::Leave(KErrNotSupported);
	}

void DPsiKernChannel::DoCancel(TInt /*aReqNo*/)
	{
	}

void DPsiKernChannel::DoRequest(TInt /*aReqNo*/,TAny* /*a1*/,TAny* /*a2*/)
	{
	// no asynchronous requests (iBehaviour stays 0, so the kernel refuses them)
	}

TInt DPsiKernChannel::DoControl(TInt aFunction,TAny* a1,TAny* /*a2*/)
	{
	TInt off=(TInt)a1;
	switch (aFunction)
		{
	case EVersion:
		return (KPsiKernMagic<<16)|(KPsiKernMajor<<8)|KPsiKernMinor;
	case ECpuMode:
		return CpuMode();
	case ERead32:
		if (!Readable(off,4))
			return KErrArgument;
		iLatch=*(volatile TUint32*)(KRegBase+off);
		return iLatch&0xffff;
	case ERead8:
		if (!Readable(off,1))
			return KErrArgument;
		return *(volatile TUint8*)(KRegBase+off);
	case ELatchHigh:
		return iLatch>>16;
	case EInvert:
		return Invert(off!=0);
	default:
		return KErrNotSupported;
		}
	}

// The palette is the 32 bytes before the pixels: entry i is a grey level in
// bits 0-3 (entry 0 also holds the bits-per-pixel code in bits 12-13).
TUint8* DPsiKernChannel::Palette()
	{
	TPckgBuf<TScreenInfoV01> info;
	Plat::ScreenInfo(info);
	if (!info().iScreenAddressValid || info().iScreenAddress==NULL)
		return NULL;
	return (TUint8*)info().iScreenAddress-KPaletteBytes;
	}

TInt DPsiKernChannel::Invert(TBool aOn)
	{
	if (!aOn)
		{
		Restore();
		return KErrNone;
		}
	if (iPalette)
		return KErrInUse;
	TUint8* p=Palette();
	if (!p)
		return KErrNotSupported;
	TInt i;
	for (i=0; i<KPaletteBytes; i++)
		iSaved[i]=p[i];
	for (i=0; i<KPaletteBytes; i+=2)
		{
		iWritten[i]=(TUint8)((iSaved[i]&0xf0)|(15-(iSaved[i]&0x0f)));
		iWritten[i+1]=iSaved[i+1];
		}
	iPalette=p;
	for (i=0; i<KPaletteBytes; i++)
		p[i]=iWritten[i];
	return KErrNone;
	}

// Puts the saved palette back, but only bytes still as we wrote them: if
// EPOC has set a new palette since (contrast, switch-on), that one stays.
void DPsiKernChannel::Restore()
	{
	if (!iPalette)
		return;
	for (TInt i=0; i<KPaletteBytes; i++)
		if (iPalette[i]==iWritten[i])
			iPalette[i]=iSaved[i];
	iPalette=NULL;
	}

// --- the DLL

EXPORT_C DLogicalDevice* CreateLogicalDevice()
	{
	return new DPsiKernDevice;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
