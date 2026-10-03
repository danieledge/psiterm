// kern.h - the kernel-side classes a minimal EPOC R5 logical device driver
// needs, reconstructed for the Psion Series 5mx ROM 1.05(260).
//
// The SDK has no kernel headers ("available in the Device Driver SDK"), so
// these declarations were rebuilt from the ROM's own Video.ldd (which derives
// from both classes), from the EKERN functions it imports, and from the
// mangled names in the SDK's ekern.lib. Every layout fact below was checked
// against a disassembly of the 1.05(260) ROM; see ../README.md, "How the
// contract was recovered". Keep drivers to what is declared here.
//
// Link with ../ekern_rom.def (ROM ordinals), never the SDK's ekern.lib: that
// one is for the Series 5 (Eiger) kernel and its ordinals are wrong here.
#ifndef PSI_KERN_H
#define PSI_KERN_H
#include <e32base.h>

class DLogicalChannel;

// The vtable: CObject's five slots (~, Open, Close, Name, FullName), then
// Remove, QueryVersionSupported, IsAvailable (EKERN 321, 302, 141), then the
// three pure virtuals the driver supplies (Video.ldd's vtable has 11 slots).
// Size 0x28 (Video.ldd allocates 0x28 for its device and adds nothing).
class DLogicalDevice : public CObject
	{
public:
	IMPORT_C DLogicalDevice();                         // EKERN 565
	IMPORT_C ~DLogicalDevice();                        // EKERN 557
	IMPORT_C virtual TInt Remove();                    // EKERN 321: returns 0 (name from the SDK list, not proven)
	IMPORT_C virtual TBool QueryVersionSupported(const TVersion& aVer) const;  // EKERN 302
	IMPORT_C virtual TBool IsAvailable(TInt aUnit,const TDesC8* aDriver,const TDesC8* aInfo) const; // EKERN 141: returns ETrue
	virtual TInt Install()=0;                          // SetName() here
	virtual void GetCaps(TDes8& aDes) const=0;
	virtual DLogicalChannel* CreateL()=0;
public:
	TVersion iVersion;      // 0x14 (set up by the constructor)
	TUint iParseMask;       // 0x18 (Video.ldd: 6. 0 = no unit, no PDD, no info)
	TUint iUnitsMask;       // 0x1c
	TUint iReserved;        // 0x20 (not touched by any code we looked at)
	TInt iOpenChannels;     // 0x24 (DLogicalChannel's constructor and destructor count it)
	};

// The vtable: CObject's five slots, Close overridden (EKERN 63), then DoCancel
// (slot 5, pure), DoRequest (slot 6, pure), DoCreateL (slot 7, EKERN 101 is
// an empty default) and DoControl (slot 8, EKERN 99 returns KErrNotSupported).
// The kernel calls DoCancel with a request number, DoRequest after it stores
// the TRequestStatus, and DoControl with the user's three arguments.
// Size 0x30 (Video.ldd's channel is 0x34 with one pointer of its own).
class DLogicalChannel : public CObject
	{
public:
	IMPORT_C DLogicalChannel(DLogicalDevice* aDevice); // EKERN 566
	IMPORT_C ~DLogicalChannel();                       // EKERN 558
	IMPORT_C virtual void Close();                     // EKERN 63
protected:
	virtual void DoCancel(TInt aReqNo)=0;
	virtual void DoRequest(TInt aReqNo,TAny* a1,TAny* a2)=0;
	IMPORT_C virtual void DoCreateL(TInt aUnit,CBase* aPdd,const TDesC8* aInfo,const TVersion& aVer); // EKERN 101
	IMPORT_C virtual TInt DoControl(TInt aFunction,TAny* a1,TAny* a2);  // EKERN 99
public:
	TAny* iThread;               // 0x14 the client thread (DThread*)
	TRequestStatus* iStatus[4];  // 0x18 one per asynchronous request
	TUint iBehaviour;            // 0x28 which requests are allowed (SetBehaviour)
	DLogicalDevice* iDevice;     // 0x2c
	};

// The screen, as the kernel reports it (Lcd.pdd calls this and writes the
// palette at iScreenAddress-0x20 itself). A TPckgBuf<TScreenInfoV01> is passed.
class Plat
	{
public:
	IMPORT_C static void ScreenInfo(TDes8& aInfo);     // EKERN 339
	};

#endif
