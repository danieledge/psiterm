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
//  - EInvert, EPalSet: the only writes. They change grey levels in the LCD
//    palette (the 32 bytes before the frame buffer, which the LCD controller
//    reads every frame), and put the saved bytes back on ERestore (or
//    EInvert 0) and when the channel closes. They write RAM only, never a
//    register, and only after EPalCheck's test has passed on those bytes, so
//    a wrong guess about where the palette is writes nothing.
//  - EPalRead, EPalCheck: read the palette and say whether it looks like one.
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
	{ERegU2Fcr,4},{ERegU2Ubrcr,4},{ERegU2Con,1},{ERegU2Flg,1},{ERegU2IntM,1},
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
	static TBool LooksLikePalette(const TUint8* aP);
	TInt Snapshot();
	TInt Invert(TBool aOn);
	TInt SetEntry(TInt aEntry,TInt aLevel);
	void Restore();
private:
	TUint32 iLatch;                    // the last ERead32
	TUint8* iPalette;                  // non-NULL while we have written it
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
	case EPalRead:
		{
		if (off<0 || off>15)
			return KErrArgument;
		TUint8* p=Palette();
		if (!p)
			return KErrNotSupported;
		return p[off*2] | (p[off*2+1]<<8);
		}
	case EPalCheck:
		{
		if (iPalette)
			return KErrNone;            // (ours now: it was checked before the first write)
		TUint8* p=Palette();
		if (!p)
			return KErrNotSupported;
		return LooksLikePalette(p) ? KErrNone : KErrCorrupt;
		}
	case EPalSet:
		return SetEntry((off>>8)&0xff,off&15);
	case ERestore:
		Restore();
		return KErrNone;
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

// The test before any write: 16 little-endian entries, each a grey level in
// bits 0-3 and nothing else set, but for entry 0's bits-per-pixel code in
// bits 12-13 (0: 2 entries in use, 1: 4, 2: 16). The levels of the entries
// in use run one way (never back), from one end to a different one. In the
// emulator, the System screen (4 greys) has 100f 000a 0005 0000 and zeros,
// and a 16-grey app 200e 000d .. 0008 0007 0007 0006 .. 0000.
// Anything else and these are not the palette: no write.
TBool DPsiKernChannel::LooksLikePalette(const TUint8* aP)
	{
	TUint e0=aP[0] | (aP[1]<<8);
	TUint code=(e0>>12)&3;
	if (code==3)
		return EFalse;
	TInt used=code==0 ? 2 : code==1 ? 4 : 16;
	TInt up=0, down=0;
	TInt prev=-1;
	for (TInt i=0; i<16; i++)
		{
		TUint e=aP[i*2] | (aP[i*2+1]<<8);
		TUint extra=e & ~(i==0 ? 0x300fu : 0x000fu);
		if (extra)
			return EFalse;
		TInt level=e&15;
		if (i<used && prev>=0)
			{
			if (level>prev) up++;
			if (level<prev) down++;
			}
		if (i<used)
			prev=level;
		}
	return (up==0) != (down==0);       // one way only, and not all the same
	}

// Before the first write: check the bytes and keep them.
TInt DPsiKernChannel::Snapshot()
	{
	if (iPalette)
		return KErrNone;
	TUint8* p=Palette();
	if (!p)
		return KErrNotSupported;
	if (!LooksLikePalette(p))
		return KErrCorrupt;
	for (TInt i=0; i<KPaletteBytes; i++)
		iWritten[i]=iSaved[i]=p[i];
	iPalette=p;
	return KErrNone;
	}

TInt DPsiKernChannel::SetEntry(TInt aEntry,TInt aLevel)
	{
	if (aEntry<0 || aEntry>15 || aLevel<0 || aLevel>15)
		return KErrArgument;
	TInt r=Snapshot();
	if (r!=KErrNone)
		return r;
	TInt i=aEntry*2;
	iWritten[i]=(TUint8)((iSaved[i]&0xf0)|aLevel);
	iWritten[i+1]=iSaved[i+1];
	iPalette[i]=iWritten[i];
	return KErrNone;
	}

TInt DPsiKernChannel::Invert(TBool aOn)
	{
	if (!aOn)
		{
		Restore();
		return KErrNone;
		}
	TInt r=Snapshot();
	if (r!=KErrNone)
		return r;
	for (TInt e=0; e<16; e++)
		{
		r=SetEntry(e,15-(iSaved[e*2]&0x0f));
		if (r!=KErrNone)
			return r;
		}
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
