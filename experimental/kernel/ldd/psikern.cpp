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
//  - (0.3) Two register writes, each to one register, with a short list of
//    values, saved first and put back by ERestore and on close:
//     - EUbrcrSet: UART2's baud divider (UBRCR), 1 or 3, while the UART is on;
//     - ERomProbe, ERomKeep: the ROM's wait states (the CS0 and CS1 bytes of
//       MEMCFG1). ERomProbe runs with interrupts off and calls nothing, so
//       no ROM code runs while the setting is on: only this driver (in RAM)
//       reads the ROM as data, and compares it with a read at the normal
//       setting. ERomKeep then sets a value that passed, for real.
// It never touches user memory: every answer is DoControl's return value.
#include "kern.h"
#include <e32svr.h>
#include <e32rom.h>
#include "../psikern.h"

const TUint32 KRegBase=0x58000000;      // the kernel's own "mov rX,#0x58000000"
const TInt KPaletteBytes=32;            // 16 entries of 16 bits
const TUint8 KRomTry[]={0x54,0x58,0x5c,0x70,0x74,0x78,0x7c};
const TInt KUartEnable=0x01;            // UART2 CON: UARTEN

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
	TInt UbrcrSet(TInt aValue);
	TInt RomProbe(TInt aByte);
	TInt RomKeep(TInt aByte);
	static TInt RomTryIndex(TInt aByte);
private:
	TUint32 iLatch;                    // the last ERead32
	TUint8* iPalette;                  // non-NULL while we have written it
	TUint8 iSaved[KPaletteBytes];      // the palette as we found it
	TUint8 iWritten[KPaletteBytes];    // what we put there
	TBool iUbrcrSet;                   // UBRCR written: iUbrcrSaved to put back
	TUint32 iUbrcrSaved, iUbrcrWritten;
	TBool iMemSet;                     // MEMCFG1 written: iMemSaved to put back
	TUint32 iMemSaved, iMemWritten;
	TUint iRomPassed;                  // bit i: KRomTry[i] passed ERomProbe
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
		TUint8* p=Palette();
		if (!p)
			return KErrNotSupported;
		if (iPalette==p)
			{
			TInt i;
			for (i=0; i<KPaletteBytes && p[i]==iWritten[i]; i++)
				;
			if (i==KPaletteBytes)
				return KErrNone;            // ours: checked before the first write
			}
		return LooksLikePalette(p) ? KErrNone : KErrCorrupt;
		}
	case EPalSet:
		return SetEntry((off>>8)&0xff,off&15);
	case ERestore:
		Restore();
		return KErrNone;
	case EUbrcrSet:
		return UbrcrSet(off);
	case ERomProbe:
		return RomProbe(off);
	case ERomKeep:
		return RomKeep(off);
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

// Before the first write: check the bytes and keep them. If EPOC has
// written the palette since our last write (a 4- or 16-grey screen, a
// contrast change, switch-on), its new one is taken as the palette to put
// back, and checked again; ours is gone anyway.
TInt DPsiKernChannel::Snapshot()
	{
	TUint8* p=Palette();
	if (!p)
		return KErrNotSupported;
	if (iPalette==p)
		{
		TInt i;
		for (i=0; i<KPaletteBytes && p[i]==iWritten[i]; i++)
			;
		if (i==KPaletteBytes)
			return KErrNone;                 // still ours
		}
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
	if (iMemSet)
		{
		volatile TUint32* mem=(volatile TUint32*)(KRegBase+ERegMemCfg1);
		if (*mem==iMemWritten)
			*mem=iMemSaved;
		iMemSet=EFalse;
		}
	if (iUbrcrSet)
		{
		volatile TUint32* ubrcr=(volatile TUint32*)(KRegBase+ERegU2Ubrcr);
		if (*ubrcr==iUbrcrWritten)
			*ubrcr=iUbrcrSaved;
		iUbrcrSet=EFalse;
		}
	if (!iPalette)
		return;
	for (TInt i=0; i<KPaletteBytes; i++)
		if (iPalette[i]==iWritten[i])
			iPalette[i]=iSaved[i];
	iPalette=NULL;
	}

// --- the UART's baud divider

// UBRCR = 7372800 / (16 x rate) - 1: 3 is 115200, 1 is 230400. Only while the
// UART is on (a port is open), and only these two values.
TInt DPsiKernChannel::UbrcrSet(TInt aValue)
	{
	if (aValue!=1 && aValue!=3)
		return KErrArgument;
	if (!(*(volatile TUint8*)(KRegBase+ERegU2Con) & KUartEnable))
		return KErrNotReady;
	volatile TUint32* ubrcr=(volatile TUint32*)(KRegBase+ERegU2Ubrcr);
	if (!iUbrcrSet)
		{
		iUbrcrSaved=*ubrcr;
		iUbrcrSet=ETrue;
		}
	iUbrcrWritten=(TUint32)aValue;
	*ubrcr=iUbrcrWritten;
	return KErrNone;
	}

// --- the ROM's wait states

TInt DPsiKernChannel::RomTryIndex(TInt aByte)
	{
	for (TUint i=0; i<sizeof(KRomTry); i++)
		if (KRomTry[i]==aByte)
			return i;
	return KErrNotFound;
	}

static inline TUint IntsOff()
	{
	TUint old,tmp;
	asm volatile("mrs %0, cpsr\n\torr %1, %0, #0xc0\n\tmsr cpsr_c, %1" : "=r"(old), "=r"(tmp) : : "memory");
	return old;
	}

static inline void IntsBack(TUint aOld)
	{
	asm volatile("msr cpsr_c, %0" : : "r"(aOld) : "memory");
	}

// A check sum of aBytes of ROM from aAddr, by words. (Written out where it is
// used: nothing may be called while the ROM's setting is changed.)
#define PSI_ROM_SUM(aSum,aAddr,aBytes) \
	{ \
	const volatile TUint32* _p=(const volatile TUint32*)(aAddr); \
	const volatile TUint32* _e=_p+(aBytes)/4; \
	TUint32 _s=0; \
	while (_p<_e) { _s=((_s<<1)|(_s>>31))^*_p++; } \
	(aSum)=_s; \
	}

// Sets aByte for CS0 and CS1 with interrupts off, reads two 64 KB stretches
// of ROM four times, and puts the normal setting back before anything else
// runs. The ROM is read at the normal setting first, and a third stretch
// is read in between so that none of it is in the 8 KB cache.
TInt DPsiKernChannel::RomProbe(TInt aByte)
	{
	TInt i=RomTryIndex(aByte);
	if (i<0)
		return KErrArgument;
	if (iMemSet)
		return KErrInUse;                // (ERestore first)
	volatile TUint32* mem=(volatile TUint32*)(KRegBase+ERegMemCfg1);
	TUint32 normal=*mem;
	if ((normal&0xffff)!=((KRomNormal<<8)|KRomNormal))
		return KErrNotSupported;         // not the setting this was made for
	// the ROM's extent from its own header
	const TRomHeader* rom=(const TRomHeader*)KRomHeaderLinAddr;
	TUint32 base=rom->iRomBase;
	TUint32 size=rom->iRomSize;
	if (base!=KRomHeaderLinAddr || size<0x400000 || size>0x2000000 || (size&0xffff))
		return KErrCorrupt;
	TUint32 a=base+size/4, b=base+(size/4)*3, c=base+size/2;
	TUint32 fast=(normal&0xffff0000)|((TUint32)aByte<<8)|(TUint32)aByte;
	TUint32 refA,refB,flush,sumA,sumB;
	TInt bad=0;
	TUint ints=IntsOff();
	PSI_ROM_SUM(refA,a,0x10000);
	PSI_ROM_SUM(refB,b,0x10000);
	PSI_ROM_SUM(flush,c,0x4000);
	*mem=fast;
	for (TInt pass=0; pass<4; pass++)
		{
		PSI_ROM_SUM(sumA,a,0x10000);
		PSI_ROM_SUM(sumB,b,0x10000);
		if (sumA!=refA || sumB!=refB)
			bad=1;
		}
	*mem=normal;
	PSI_ROM_SUM(sumA,a,0x10000);         // and the same again at the normal setting
	IntsBack(ints);
	(void)flush;
	if (sumA!=refA)
		return KErrGeneral;              // (the ROM reads differently even now: odd)
	if (!bad)
		iRomPassed|=1u<<i;
	return bad;
	}

TInt DPsiKernChannel::RomKeep(TInt aByte)
	{
	TInt i=RomTryIndex(aByte);
	if (i<0)
		return KErrArgument;
	if (!(iRomPassed&(1u<<i)))
		return KErrNotReady;             // ERomProbe first
	volatile TUint32* mem=(volatile TUint32*)(KRegBase+ERegMemCfg1);
	TUint32 now=*mem;
	if (!iMemSet)
		{
		iMemSaved=now;
		iMemSet=ETrue;
		}
	iMemWritten=(iMemSaved&0xffff0000)|((TUint32)aByte<<8)|(TUint32)aByte;
	*mem=iMemWritten;
	return KErrNone;
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
