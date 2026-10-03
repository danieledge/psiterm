// psikern.h - the user side of PsiKern.ldd, the experimental kernel driver.
// Shared by the driver (ldd/psikern.cpp) and the test program (test/).
// EXPERIMENTAL: never ship this in a .sis or a release. See README.md.
#ifndef PSIKERN_H
#define PSIKERN_H
#include <e32std.h>

_LIT(KPsiKernName,"PsiKern");          // the device's name (Install)
_LIT(KPsiKernFile,"PSIKERN.LDD");      // \System\Libs\ on any drive
const TInt KPsiKernMajor=0;
const TInt KPsiKernMinor=2;
const TInt KPsiKernBuild=1;
const TInt KPsiKernMagic=0x504B;       // "PK", the top half of EVersion's answer

// DoControl functions. No function takes a user pointer: every answer is the
// return value, so the driver never touches user memory.
enum TPsiKernControl
	{
	EVersion=0,     // -> KPsiKernMagic<<16 | major<<8 | minor
	ECpuMode=1,     // -> CPSR & 0xff as the driver runs (0x13 is SVC, 0x10 USR)
	ERead32=2,      // a1=offset from the register base -> low 16 bits (whole value latched)
	ERead8=3,       // a1=offset -> the byte (0..255)
	ELatchHigh=4,   // -> top 16 bits of the last ERead32
	EInvert=5,      // a1=1 inverts the LCD palette, 0 puts it back -> 0 or an error
	// (0.2)
	EPalRead=6,     // a1=entry 0..15 -> the 16-bit palette entry (read only)
	EPalCheck=7,    // -> 0 if the 32 bytes look like a 16-grey palette, else KErrCorrupt
	EPalSet=8,      // a1=entry<<8 | grey level 0..15: sets that entry's level -> 0 or an error
	ERestore=9,     // puts the palette back as it was before the first write -> 0
	};

// Register offsets from the Windermere's register base (virtual 0x58000000
// in the 1.05(260) ROM). The driver only reads the ones in its own list.
enum TPsiKernReg
	{
	ERegMemCfg1=0x000, ERegMemCfg2=0x004, ERegDramCfg=0x100,
	ERegLcdCtl=0x200, ERegLcdSt=0x204, ERegLcdDbar1=0x210,
	ERegLcdT0=0x220, ERegLcdT1=0x224, ERegLcdT2=0x228,
	ERegPwrSr=0x400, ERegPwrCnt=0x404,
	ERegIntSr=0x500, ERegIntRsr=0x504, ERegIntEns=0x508,
	ERegU2Fcr=0x704, ERegU2Ubrcr=0x708, ERegU2Con=0x70c, ERegU2Flg=0x710, ERegU2IntM=0x718,
	ERegTc1Load=0xc00, ERegTc1Val=0xc04, ERegTc1Ctrl=0xc08,
	ERegTc2Load=0xc20, ERegTc2Val=0xc24, ERegTc2Ctrl=0xc28,
	ERegRtcL=0xd00, ERegRtcU=0xd04,
	ERegPbdr=0xe04, ERegPcdr=0xe08, ERegPddr=0xe0c, ERegPedr=0xe20,
	};

class RPsiKern : public RBusLogicalChannel
	{
public:
	inline TInt Open()
		{ return DoCreate(KPsiKernName,TVersion(KPsiKernMajor,KPsiKernMinor,KPsiKernBuild),NULL,KNullUnit,NULL,NULL); }
	inline TInt Version() { return DoControl(EVersion); }
	inline TInt CpuMode() { return DoControl(ECpuMode); }
	// a 32-bit register: two calls, but one read of the register
	inline TInt Read32(TInt aOffset,TUint32& aValue)
		{
		TInt lo=DoControl(ERead32,(TAny*)aOffset);
		if (lo<0) return lo;
		TInt hi=DoControl(ELatchHigh);
		if (hi<0) return hi;
		aValue=((TUint32)hi<<16)|(TUint32)lo;
		return KErrNone;
		}
	inline TInt Read8(TInt aOffset) { return DoControl(ERead8,(TAny*)aOffset); }
	inline TInt Invert(TBool aOn) { return DoControl(EInvert,(TAny*)(aOn?1:0)); }
	inline TInt PalRead(TInt aEntry) { return DoControl(EPalRead,(TAny*)aEntry); }
	inline TInt PalCheck() { return DoControl(EPalCheck); }
	inline TInt PalSet(TInt aEntry,TInt aLevel) { return DoControl(EPalSet,(TAny*)((aEntry<<8)|(aLevel&15))); }
	inline TInt Restore() { return DoControl(ERestore); }
	};

#endif
