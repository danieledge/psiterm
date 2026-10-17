// csepoc.cpp - psicast.exe on the Psion: the chunk, the clock, the log,
// reading the screen, injecting the viewer's input, power, and main (the
// protocol itself is csmain.c). The chunk is created by PsiCast.app and
// opened by psiglue.cpp's pg_attach(): it starts with the PsiShared the
// network code uses.
//
// Reading the screen. The fast way is the frame buffer itself, which
// UserSvr::ScreenInfo gives (TScreenInfoV01::iScreenAddress; on the 5mx
// virtual 58003020). The 32 bytes before it are the LCD palette: 16
// little-endian entries, entry 0's bits 12-13 giving the bits a pixel
// (0: 1, 1: 2, 2: 4). The depth follows the program in front: the System
// screen is 4 greys (2 bits), a 16-grey program 4 bits. Each frame is
// turned into PROTOCOL.md's model (4 bits, 0 black, the left pixel in the
// low nibble) through a table.
//
// Which way round the hardware keeps its pixels (nibble order, polarity) is
// not guessed: at start the frame buffer is compared with the window
// server's own copy of the screen (CWsScreenDevice::CopyScreenToBitmap into
// an EGray16 bitmap, which is the protocol's model by definition), and the
// layout that matches is used. The window server's copy is also the slow
// fallback if there is no frame buffer to read.

#include <e32std.h>
#include <e32base.h>
#include <e32hal.h>
#include <e32svr.h>
#include <f32file.h>
#include <w32std.h>
#include <fbs.h>

extern "C" {
#include "cs.h"
#include <stdio.h>
#include <stdarg.h>
#include <sys/reent.h>                   // CloseSTDLIB
int vsnprintf(char* str, size_t size, const char* fmt, va_list ap);
int snprintf(char* str, size_t size, const char* fmt, ...);
extern PsiShared* pg_shared();
extern int pg_attach();
extern void pg_set_link_log(void (*aFn)(const char*));
extern void pg_close();
}

static RFs* gFs = 0;
static CsShared* gCs = 0;

extern "C" CsShared* cs_shared()
	{
	return gCs;
	}

// milliseconds from the 64 Hz tick (the RTC counts whole seconds only):
// 15.625 ms a tick, worked out so it wraps only after 51 days
extern "C" unsigned long cs_ms()
	{
	TUint t = User::TickCount();
	return t * 15UL + (t >> 3) * 5UL + ((t & 7) * 5UL) / 8UL;
	}
extern "C" unsigned long pwb_ms() { return cs_ms(); }    // (pwnet.c's clock)

static RFs& Fs()
	{
	if (!gFs)
		{
		gFs = new RFs;
		if (gFs && gFs->Connect() != KErrNone)
			{
			delete gFs;
			gFs = 0;
			}
		}
	return *gFs;
	}

// ----- psicast.log in PsiCast's folder (the last run's: psicast.old) ------------

static RFile* gLog = 0;
static TUint gLogT0 = 0;

static void LogOpen()
	{
	if (gLog || !gFs)
		return;
	TParse parse;
	parse.Set(RProcess().FileName(), NULL, NULL);
	TFileName log(parse.DriveAndPath()), old;
	log.Append(_L("psicast.log"));
	old = log;
	old.SetLength(old.Length() - 3);
	old.Append(_L("old"));
	gLog = new RFile;
	if (!gLog)
		return;
	Fs().Delete(old);
	Fs().Rename(log, old);
	if (gLog->Replace(Fs(), log, EFileWrite | EFileShareAny) != KErrNone)
		{
		delete gLog;
		gLog = 0;
		return;
		}
	gLogT0 = User::TickCount();
	}

extern "C" void cs_log(const char* aFmt, ...)
	{
	if (!gLog)
		return;
	char text[160];
	va_list ap;
	va_start(ap, aFmt);
	vsnprintf(text, sizeof(text), aFmt, ap);
	va_end(ap);
	TUint t = (User::TickCount() - gLogT0) * 10 / 64;       // 1/10 s
	TBuf8<200> line;
	line.AppendFormat(_L8("%02u:%02u.%u "), (t / 600) % 100, (t / 10) % 60, t % 10);
	TPtrC8 p((const TUint8*)text);
	line.Append(p.Left(p.Length() < 180 ? p.Length() : 180));
	line.Append(_L8("\r\n"));
	gLog->Write(line);
	gLog->Flush();
	}

static void LinkLog(const char* aText) { cs_log("%s", aText); }

// ----- liveness -------------------------------------------------------------------

// PsiCast.app's process, by the id it left in the chunk. Gone = ended, or no
// such process (psiglue's heartbeat is not used: app_beat stays 0, because
// the engine may go on after the app has closed)
extern "C" int cs_app_gone()
	{
	TUint pid = gCs->app_pid;
	if (pid == 0)
		return 0;
	TProcessId id;
	Mem::Copy(&id, &pid, sizeof(pid));
	RProcess p;
	if (p.Open(id) != KErrNone)
		return 1;
	TBool gone = p.ExitType() != EExitPending;
	p.Close();
	return gone;
	}

// The tick stops while the Psion is off, the clock does not: a gap between
// them is a switch-off. psiglue then checks the link is still there (its
// SwitchOnCheck, on net.switch_on), as the apps ask on HandleSwitchOnEventL
// - here because the app may have closed.
static TInt64 gBeatClock;
static TUint gBeatTick = 0;

extern "C" void cs_beat()
	{
	gCs->eng_beat++;
	TTime now;
	now.UniversalTime();
	TUint tick = User::TickCount();
	if (gBeatTick != 0)
		{
		TInt64 real = (now.Int64() - gBeatClock) / TInt64(1000);       // ms
		TInt64 ticked = TInt64(TInt(tick - gBeatTick)) * TInt64(15625) / TInt64(1000);
		if (real - ticked > TInt64(5000))
			{
			cs_log("cast: switched on again (off for about %d s)", (real - ticked).Low() / 1000);
			gCs->net.switch_on++;
			}
		}
	gBeatClock = now.Int64();
	gBeatTick = tick;
	}

// ----- the screen -----------------------------------------------------------------

enum { KModeNone = 0, KModeDirect, KModeWs };
static int gMode = KModeNone;
static volatile const TUint8* gFb = 0;          // the pixels (the palette is 32 bytes before)
static int gFbBpp = 0;                          // as last seen
static int gSwap = 0, gInvert = 0;              // the layout that matched the window server's
static TUint8 gLut4[256];                       // a 4-bit byte -> the model's byte
static TUint16 gLut2[256];                      // a 2-bit byte (4 pixels) -> 2 model bytes
static TUint32 gLut1[256];                      // a 1-bit byte (8 pixels) -> 4 model bytes
static char gKind[40];

static RWsSession* gWs = 0;
static CWsScreenDevice* gScreen = 0;
// (LockHeap is protected on ER5: a bitmap that lets us hold the heap still
// while its pixels are copied)
class CCsBitmap : public CFbsBitmap
	{
public:
	void Lock() const { LockHeap(); }
	void Unlock() const { UnlockHeap(); }
	};
static CCsBitmap* gBmp = 0;
static int gFbsOpen = 0;

// grey g (0..max) of a pixel value v, with the polarity found
static TUint Grey(TUint aV, TUint aMax)
	{
	TUint v = gInvert ? aMax - aV : aV;
	return aMax == 15 ? v : aMax == 3 ? v * 5 : v * 15;
	}

static void BuildLuts()
	{
	for (TUint b = 0; b < 256; b++)
		{
		// 4 bits: two pixels, the left in the low nibble unless swapped
		TUint p0 = gSwap ? b >> 4 : b & 15, p1 = gSwap ? b & 15 : b >> 4;
		gLut4[b] = (TUint8)(Grey(p0, 15) | (Grey(p1, 15) << 4));
		// 2 bits: four pixels, the left in the low bits unless swapped
		TUint px[4];
		for (TInt i = 0; i < 4; i++)
			px[i] = Grey(gSwap ? (b >> (6 - 2 * i)) & 3 : (b >> (2 * i)) & 3, 3);
		TUint lo = px[0] | (px[1] << 4), hi = px[2] | (px[3] << 4);
		gLut2[b] = (TUint16)(lo | (hi << 8));               // (stored little-endian below)
		TUint32 w = 0;
		for (TInt i = 0; i < 8; i++)
			{
			TUint bit = gSwap ? (b >> (7 - i)) & 1 : (b >> i) & 1;
			w |= (TUint32)Grey(bit, 1) << (4 * i);
			}
		gLut1[b] = w;
		}
	}

// the depth the palette says, or 0 if those bytes do not look like one
static int PaletteBpp(volatile const TUint8* aFb)
	{
	volatile const TUint8* p = aFb - 32;
	TUint e0 = p[0] | (p[1] << 8);
	TUint code = (e0 >> 12) & 3;
	if (code == 3 || (e0 & 0xcff0))
		return 0;
	for (TInt i = 1; i < 16; i++)
		if (p[2 * i + 1] & 0xff || p[2 * i] & 0xf0)
			return 0;
	return code == 0 ? 1 : code == 1 ? 2 : 4;
	}

// the frame buffer into the model, through the tables. The frame buffer is
// read a word at a time (it may be slow, uncached memory): 4 bytes a load.
static void ConvertFb(volatile const TUint8* aFb, int aBpp, TUint8* aOut)
	{
	const TUint32* src = (const TUint32*)aFb;           // (word-aligned: checked at start)
	TUint32* out = (TUint32*)aOut;                       // (the caller's frame is word-aligned)
	TInt words = (CS_STRIDE * CS_H * aBpp / 4) / 4;      // of the frame buffer
	if (aBpp == 4)
		{
		if (!gSwap && !gInvert)
			for (TInt i = 0; i < words; i++)
				out[i] = src[i];
		else
			for (TInt i = 0; i < words; i++)
				{
				TUint32 v = src[i];
				out[i] = gLut4[v & 255] | (gLut4[(v >> 8) & 255] << 8)
					| (gLut4[(v >> 16) & 255] << 16) | ((TUint32)gLut4[v >> 24] << 24);
				}
		}
	else if (aBpp == 2)
		{
		for (TInt i = 0; i < words; i++)
			{
			TUint32 v = src[i];
			out[2 * i] = gLut2[v & 255] | ((TUint32)gLut2[(v >> 8) & 255] << 16);
			out[2 * i + 1] = gLut2[(v >> 16) & 255] | ((TUint32)gLut2[v >> 24] << 16);
			}
		}
	else
		{
		for (TInt i = 0; i < words; i++)
			{
			TUint32 v = src[i];
			out[4 * i] = gLut1[v & 255];
			out[4 * i + 1] = gLut1[(v >> 8) & 255];
			out[4 * i + 2] = gLut1[(v >> 16) & 255];
			out[4 * i + 3] = gLut1[v >> 24];
			}
		}
	}

// The frame buffer as last converted, to see cheaply whether anything has
// changed at all (a still screen costs one pass of word compares, no
// conversion and no encoder). Copied as it is compared.
static TUint32 gRaw[CS_STRIDE * CS_H / 4];
static int gRawBpp = 0;

// 1 if the frame buffer is as it was at the last call (and gRaw is it)
static int RawSame(volatile const TUint8* aFb, int aBpp)
	{
	const TUint32* src = (const TUint32*)aFb;
	TInt words = (CS_STRIDE * CS_H * aBpp / 4) / 4;
	TInt i = 0;
	if (aBpp == gRawBpp)
		{
		while (i < words && src[i] == gRaw[i])
			i++;
		if (i == words)
			return 1;
		}
	gRawBpp = aBpp;
	for (; i < words; i++)
		gRaw[i] = src[i];
	return 0;
	}

static int WsOpen()
	{
	if (gBmp)
		return 0;
	if (!gFbsOpen)
		{
		if (RFbsSession::Connect() != KErrNone)
			return -1;
		gFbsOpen = 1;
		}
	gWs = new RWsSession;
	if (!gWs || gWs->Connect() != KErrNone)
		{
		delete gWs;
		gWs = 0;
		return -2;
		}
	gScreen = new CWsScreenDevice(*gWs);
	if (!gScreen || gScreen->Construct() != KErrNone)
		{
		delete gScreen;
		gScreen = 0;
		return -3;
		}
	gBmp = new CCsBitmap;
	if (!gBmp || gBmp->Create(TSize(CS_W, CS_H), EGray16) != KErrNone)
		{
		delete gBmp;
		gBmp = 0;
		return -4;
		}
	return 0;
	}

static void WsClose()
	{
	delete gBmp;
	gBmp = 0;
	delete gScreen;
	gScreen = 0;
	if (gWs)
		{
		gWs->Close();
		delete gWs;
		gWs = 0;
		}
	if (gFbsOpen)
		RFbsSession::Disconnect();
	gFbsOpen = 0;
	}

// the window server's copy of the screen, into the model
static int WsCapture(TUint8* aOut)
	{
	if (!gBmp || gScreen->CopyScreenToBitmap(gBmp) != KErrNone)
		return -1;
	TInt stride = CFbsBitmap::ScanLineLength(CS_W, EGray16);
	gBmp->Lock();
	const TUint8* d = (const TUint8*)gBmp->DataAddress();
	for (TInt y = 0; y < CS_H; y++)
		Mem::Copy(aOut + y * CS_STRIDE, d + y * stride, CS_STRIDE);
	gBmp->Unlock();
	return 0;
	}

static TInt Differences(const TUint8* aA, const TUint8* aB)
	{
	TInt d = 0;
	for (TInt i = 0; i < CS_STRIDE * CS_H; i++)
		if (aA[i] != aB[i])
			d++;
	return d;
	}

static void SetKind()
	{
	if (gMode == KModeDirect)
		snprintf(gKind, sizeof(gKind), "frame buffer, %d greys", 1 << gFbBpp);
	else
		snprintf(gKind, sizeof(gKind), "window server");
	snprintf(gCs->capture, sizeof(gCs->capture), "%s", gKind);
	gCs->eng_seq++;
	}

extern "C" int cs_capture_slow()
	{
	return gMode == KModeWs;
	}

extern "C" const char* cs_capture_kind()
	{
	return gKind;
	}

extern "C" int cs_capture_init()
	{
	TPckgBuf<TScreenInfoV01> info;
	UserSvr::ScreenInfo(info);
	int ws = WsOpen();
	if (ws != 0)
		cs_log("cast: no window server copy of the screen (%d)", ws);
	TBool direct =
#ifdef CS_FORCE_WS
		// (a test build: make -f cast/engine/engine.mk CSEPOC_DEFS=-DCS_FORCE_WS)
		EFalse &&
#endif
		info().iScreenAddressValid && info().iScreenAddress
		&& info().iScreenSize.iWidth == CS_W && info().iScreenSize.iHeight == CS_H
		&& ((TUint)info().iScreenAddress & 3) == 0;
	cs_log("cast: screen at %08x, %dx%d, %s", (TUint)info().iScreenAddress,
		info().iScreenSize.iWidth, info().iScreenSize.iHeight,
		info().iScreenAddressValid ? "address valid" : "no address");
	if (direct)
		{
		gFb = (volatile const TUint8*)info().iScreenAddress;
		gFbBpp = PaletteBpp(gFb);
		if (gFbBpp == 0)
			{
			cs_log("cast: the bytes before the screen are not a palette: not reading it directly");
			direct = EFalse;
			}
		}
	if (direct && ws == 0)
		{
		// which layout matches the window server's copy? (the screen may
		// change between the two reads: three tries)
		static TUint32 refw[CS_STRIDE * CS_H / 4];      // (words: ConvertFb writes words)
		static TUint32 minew[CS_STRIDE * CS_H / 4];
		TUint8* ref = (TUint8*)refw;
		TUint8* mine = (TUint8*)minew;
		TInt best = -1, bestDiff = KMaxTInt;
		for (TInt attempt = 0; attempt < 3 && bestDiff != 0; attempt++)
			{
			if (WsCapture(ref) != 0)
				break;
			int bpp = PaletteBpp(gFb);
			for (TInt v = 0; v < 4; v++)
				{
				gSwap = v & 1;
				gInvert = (v >> 1) & 1;
				BuildLuts();
				ConvertFb(gFb, bpp, mine);
				TInt d = Differences(ref, mine);
				cs_log("cast: frame buffer (%d bits) %s%s: %d bytes differ", bpp,
					gSwap ? "pixels swapped" : "pixels in order", gInvert ? ", inverted" : "", d);
				if (d < bestDiff)
					{
					bestDiff = d;
					best = v;
					}
				}
			}
		if (best < 0 || bestDiff > CS_STRIDE * CS_H / 100)
			{
			cs_log("cast: the frame buffer does not match the screen (%d bytes): using the window server", bestDiff);
			direct = EFalse;
			}
		else
			{
			gSwap = best & 1;
			gInvert = (best >> 1) & 1;
			}
		}
	BuildLuts();
	if (direct)
		{
		gMode = KModeDirect;
		gFbBpp = PaletteBpp(gFb);
		}
	else if (ws == 0)
		gMode = KModeWs;
	else
		{
		cs_log("cast: no way to read the screen");
		return -1;
		}
	SetKind();
	return 0;
	}

extern "C" int cs_capture(unsigned char* aFrame)
	{
	if (gMode == KModeDirect)
		{
		int bpp = PaletteBpp(gFb);
		if (bpp == 0)
			return -1;                       // (being rewritten this moment: next time)
		if (bpp != gFbBpp)
			{
			gFbBpp = bpp;
			SetKind();
			cs_log("cast: the screen is now %d greys", 1 << bpp);
			}
		if (RawSame(gFb, bpp))
			return 1;                        // (aFrame still holds it)
		ConvertFb((volatile const TUint8*)gRaw, bpp, aFrame);
		return 0;
		}
	if (gMode == KModeWs)
		return WsCapture(aFrame);
	return -1;
	}

// ----- the viewer's input ---------------------------------------------------------
// Raw events, as the digitiser and keyboard drivers make them: the window
// server sends them to the program in front as if the pen or a key had been
// used. (csmain.c has checked them and only calls while control is allowed.)

extern "C" void cs_inject_pen(int aKind, int aX, int aY)
	{
	TRawEvent ev;
	ev.Set(aKind == 1 ? TRawEvent::EButton1Down : aKind == 3 ? TRawEvent::EButton1Up : TRawEvent::EPointerMove, aX, aY);
	UserSvr::AddEvent(ev);
	UserHal::ResetAutoSwitchOffTimer();
	}

extern "C" void cs_inject_key(int aDown, int aScanCode)
	{
	TRawEvent ev;
	ev.Set(aDown ? TRawEvent::EKeyDown : TRawEvent::EKeyUp, aScanCode);
	UserSvr::AddEvent(ev);
	UserHal::ResetAutoSwitchOffTimer();
	}

// ----- power and memory -----------------------------------------------------------

extern "C" void cs_power(int* aPct, int* aMains, int* aLow)
	{
	*aPct = 255;
	*aMains = 255;
	*aLow = 0;
	TSupplyInfoV1Buf b;
	if (UserHal::SupplyInfo(b) != KErrNone)
		return;
	const TSupplyInfoV1& s = b();
	*aMains = s.iExternalPowerPresent ? 1 : 0;
	*aLow = s.iMainBatteryStatus <= ELow;
	// two AA cells: about 3.2 V new, 2.0 V flat (the 5mx's own "low" is near 2.2 V)
	TInt mv = s.iMainBatteryMilliVolts;
	TInt top = s.iMainBatteryMaxMilliVolts > 2200 ? s.iMainBatteryMaxMilliVolts : 3200;
	if (mv > 0 && top > 2100)
		{
		TInt pct = (mv - 2000) * 100 / (top - 2000);
		*aPct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
		}
	else
		*aPct = s.iMainBatteryStatus == EGood ? 75 : s.iMainBatteryStatus == ELow ? 20
			: s.iMainBatteryStatus == EVeryLow ? 5 : 0;
	}

extern "C" unsigned long cs_free_ram()
	{
	TMemoryInfoV1Buf mi;
	return UserHal::MemoryInfo(mi) == KErrNone ? (unsigned long)mi().iFreeRamInBytes : 0;
	}

extern "C" void cs_digitiser(int* aW, int* aH)
	{
	*aW = CS_W;
	*aH = CS_H;
	TMachineInfoV1Buf mb;
	if (UserHal::MachineInfo(mb) == KErrNone && mb().iXYInputSizeInPixels.iWidth > 0
		&& mb().iXYInputSizeInPixels.iHeight > 0)
		{
		*aW = mb().iXYInputSizeInPixels.iWidth;
		*aH = mb().iXYInputSizeInPixels.iHeight;
		}
	}

static char gName[40];

extern "C" const char* cs_device_name()
	{
	if (!gName[0])
		{
		TMachineInfoV1Buf mb;
		TInt n = 0;
		if (UserHal::MachineInfo(mb) == KErrNone)
			{
			const TDesC& m = mb().iMachineName;
			for (TInt i = 0; i < m.Length() && n < (TInt)sizeof(gName) - 1; i++)
				if (m[i] >= ' ' && m[i] < 127)
					gName[n++] = (char)m[i];
			}
		if (n == 0)
			snprintf(gName, sizeof(gName), "Psion");
		else
			gName[n] = 0;
		}
	return gName;
	}

// ----- main -----------------------------------------------------------------------

int main(int, char**)
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	if (pg_attach() != 0)
		{
		delete cleanup;                      // not started by PsiCast.app
		return 1;
		}
	CsShared* s = (CsShared*)pg_shared();
	if (s->magic != CS_MAGIC || s->layout != CS_LAYOUT)
		{
		pg_close();
		delete cleanup;
		return 2;
		}
	gCs = s;
	{
	TProcessId id = RProcess().Id();
	TUint pid = 0;
	Mem::Copy(&pid, &id, sizeof(pid));
	s->eng_pid = pid;
	}
	// below every program, so the one in front never waits for a picture
	RProcess().SetPriority(EPriorityLow);
	Fs();
	LogOpen();
	pg_set_link_log(LinkLog);
	{
	TMemoryInfoV1Buf mi;
	TInt freeRam = UserHal::MemoryInfo(mi) == KErrNone ? mi().iFreeRamInBytes : 0;
	TMachineInfoV1Buf mb;
	TBool mOk = UserHal::MachineInfo(mb) == KErrNone;
	cs_log("psicast.exe %s started on %s (RAM free %d KB; digitiser %dx%d)", s->net.version, cs_device_name(),
		freeRam / 1024, mOk ? mb().iXYInputSizeInPixels.iWidth : 0, mOk ? mb().iXYInputSizeInPixels.iHeight : 0);
	}
	cs_loop();
	cs_log("psicast.exe ends");
	WsClose();
	pg_close();
	s->state = PSI_STATE_EXITED;
	if (gLog)
		{
		gLog->Close();
		delete gLog;
		}
	if (gFs)
		{
		gFs->Close();
		delete gFs;
		}
	CloseSTDLIB();
	delete cleanup;
	return 0;
	}
