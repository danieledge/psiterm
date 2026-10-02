// pwepoc.cpp - PsiWeb backend on the Psion (see fb/pwback.h)
//
// psiweb.exe draws into its own RGB565 buffer; here the changed rows are
// turned into 16 greys straight into the chunk shared with PsiWeb.app, and
// input events and menu commands are taken from it. The chunk is opened by
// psiglue.cpp's pg_init() (it starts with the PsiShared the network code
// uses), so this file only adds the browser's part.

#include <e32std.h>
#include <e32base.h>
#include <e32hal.h>
#include <f32file.h>
#include <e32keys.h>

extern "C" {
#include "psiweb.h"
#include "fb/pwback.h"
extern PsiShared* pg_shared();
extern int pg_attach();
extern void pg_set_link_log(void (*aFn)(const char*));
extern void pwn_idle_tick(void);
extern void pg_msleep(int);
}

// NSFB key codes (libnsfb_event.h) used below
enum
	{
	NK_BACKSPACE = 8, NK_TAB = 9, NK_RETURN = 13, NK_ESCAPE = 27, NK_DELETE = 127,
	NK_UP = 273, NK_DOWN, NK_RIGHT, NK_LEFT, NK_INSERT, NK_HOME, NK_END,
	NK_PAGEUP, NK_PAGEDOWN,
	NK_RSHIFT = 303, NK_LSHIFT, NK_RCTRL, NK_LCTRL,
	NK_MOUSE_1 = 401
	};

// The Psion's clock (gettimeofday, TTime) counts whole seconds: its RTC has
// no finer step. NetSurf's scheduler compares times with it, so every
// callback (a fetch poll due in 10 ms, a redraw) waited for the next
// second, and many in a row made a page take minutes or never appear. This
// gettimeofday (NetSurf and libnsutils are built to call it instead) adds
// the 64 Hz tick count to the clock's seconds.
extern "C" int gettimeofday(void*, void*);
extern "C" int pwb_gettimeofday(void* aTv, void*)
	{
	static TUint sBaseTick = 0;
	static long sBaseSec = 0, sBaseUs = 0;
	static TBool sInit = EFalse;
	long* tv = (long*)aTv;
	if (!sInit)
		{
		long t[2] = {0, 0};
		gettimeofday(t, 0);
		sBaseSec = t[0];
		sBaseUs = 0;                     // (the sub-second part isn't real)
		sBaseTick = User::TickCount();
		sInit = ETrue;
		}
	TUint d = User::TickCount() - sBaseTick;          // 1/64 s
	long sec = sBaseSec + (long)(d >> 6);
	long us = sBaseUs + (long)(d & 63) * 15625;
	tv[0] = sec;
	tv[1] = us;
	return 0;
	}


// ----- C:\System\Data\PsiWeb.log -------------------------------------------
// One line per event, "mm:ss.t text", for finding out where a page stops:
// status text, busy changes, link messages, fetch errors, frames drawn.
// The last run's log is kept as PsiWeb.old.
// (a test build can put it on the emulator's card, where it can be read
// back: web/links/epoc.mk PWEPOC_DEFS=-DPW_LOG_ON_D)
#ifdef PW_LOG_ON_D
#define PW_LOG_DIR "D:\\"
#else
#define PW_LOG_DIR "C:\\System\\Data\\"
#endif
_LIT(KLogFile, PW_LOG_DIR "PsiWeb.log");
_LIT(KLogOld, PW_LOG_DIR "PsiWeb.old");
static RFs gLogFs;
static RFile gLog;
static TInt gLogOpen = 0;       // 0 not tried, 1 open, -1 failed
static TUint gLogT0 = 0;
static char gLogLast[96];

static void LogOpen()
	{
	gLogOpen = -1;
	if (gLogFs.Connect() != KErrNone)
		return;
	gLogFs.Delete(KLogOld);
	gLogFs.Rename(KLogFile, KLogOld);
	if (gLog.Replace(gLogFs, KLogFile, EFileWrite | EFileShareAny) != KErrNone)
		{
		gLogFs.Close();
		return;
		}
	gLogT0 = User::TickCount();
	gLogOpen = 1;
	}

extern "C" void pw_log(const char* aText)
	{
	if (gLogOpen == 0)
		LogOpen();
	if (gLogOpen != 1 || !aText)
		return;
	TUint t = (User::TickCount() - gLogT0) * 10 / 64;       // 1/10 s
	TBuf8<160> line;
	line.AppendFormat(_L8("%02u:%02u.%u "), (t / 600) % 100, (t / 10) % 60, t % 10);
	TInt n = 0;
	while (aText[n] && n < 120) n++;
	line.Append((const TUint8*)aText, n);
	line.Append(_L8("\r\n"));
	gLog.Write(line);
	gLog.Flush();
	}

static void LogLink(const char* aText) { pw_log(aText); }

static PwShared* gPw = 0;
static int gInitDone = 0;
static int gInitResult = -1;
static PwShared* gDummy = 0;       // stands in if PsiWeb.app's chunk is missing
static pwb_event gQ[8];            // events expanded from one shared event
static int gQn = 0;
static int gCtrlDown = 0;

// NetSurf asks for settings (resource folder, home page, zoom...) long
// before it opens the screen, so the chunk is opened on first use - never
// returns NULL.
static PwShared* Pw()
	{
	if (gPw)
		return gPw;
	if (!gInitDone)
		{
		gInitDone = 1;
		pw_log("engine start");
		pg_set_link_log(LogLink);
		gInitResult = pg_attach();     // the chunk only: the serial port is
		                               // opened when a page is fetched
		PwShared* s = (PwShared*)pg_shared();
		if (s && s->magic == PW_MAGIC)
			gPw = s;
		}
	if (gPw)
		return gPw;
	if (!gDummy)
		{
		gDummy = (PwShared*)User::Alloc(sizeof(PwShared));
		if (gDummy)
			Mem::FillZ(gDummy, sizeof(PwShared));
		}
	return gDummy;                     // (only NULL if even that failed)
	}

static void Push(int aType, int aCode, int aX, int aY)
	{
	if (gQn < 8)
		{
		gQ[gQn].type = aType; gQ[gQn].code = aCode;
		gQ[gQn].x = aX; gQ[gQn].y = aY;
		gQn++;
		}
	}

static void SetText(char* aDst, int aMax, const char* aSrc)
	{
	int i = 0;
	if (aSrc)
		while (aSrc[i] && i < aMax - 1) { aDst[i] = aSrc[i]; i++; }
	aDst[i] = 0;
	}

// ----------------------------------------------------------------------------

extern "C" int pwb_open(int* aW, int* aH)
	{
	Pw();
	if (!gPw)
		return -1;                     // not started by PsiWeb.app
	*aW = Pw()->width > 0 && Pw()->width <= PW_MAX_W ? Pw()->width : PW_MAX_W;
	*aH = Pw()->height > 0 && Pw()->height <= PW_MAX_H ? Pw()->height : PW_MAX_H;
	Pw()->dirty_y0 = *aH;
	Pw()->dirty_y1 = 0;
	return 0;
	}

extern "C" void pwb_close()
	{
	if (Pw())
		Pw()->state = PW_STATE_EXITED;
	}

extern "C" void pwb_present(const unsigned short* aFb, int aFbW, int aX0, int aY0, int aX1, int aY1)
	{
	PwShared* s = Pw();
	pw_grey_convert(aFb, aFbW, s->fb, PW_STRIDE, aX0, aY0, aX1, aY1);
	// widen the dirty band; the app resets it after copying
	if (aY0 < s->dirty_y0) s->dirty_y0 = aY0;
	if (aY1 > s->dirty_y1) s->dirty_y1 = aY1;
	s->frame_seq++;
	if (s->frame_seq <= 5 || s->frame_seq % 25 == 0)
		{
		char b[48];
		TBuf8<48> t;
		t.Format(_L8("frame %u rows %d-%d"), s->frame_seq, aY0, aY1);
		Mem::Copy(b, t.Ptr(), t.Length());
		b[t.Length()] = 0;
		pw_log(b);
		}
	}

// EPOC key code -> NSFB key code (or a ready character)
static int MapKey(int aCode, int aMods)
	{
	switch (aCode)
		{
		case EKeyBackspace: return NK_BACKSPACE;
		case EKeyTab: return NK_TAB;
		case EKeyEnter: return NK_RETURN;
		case EKeyEscape: return NK_ESCAPE;
		case EKeyDelete: return NK_DELETE;
		case EKeyUpArrow: return NK_UP;
		case EKeyDownArrow: return NK_DOWN;
		case EKeyLeftArrow: return NK_LEFT;
		case EKeyRightArrow: return NK_RIGHT;
		case EKeyPageUp: return NK_PAGEUP;
		case EKeyPageDown: return NK_PAGEDOWN;
		case EKeyHome: return NK_HOME;
		case EKeyEnd: return NK_END;
		}
	if (aMods & EModifierCtrl)
		{
		// Ctrl+letter arrives as a control code: give NetSurf the letter
		// with Ctrl held (copy, paste, select all...)
		if (aCode >= 1 && aCode <= 26)
			return 'a' + aCode - 1;
		return aCode;
		}
	if (aCode >= 32 && aCode < 0xf700)
		return PWB_UNICODE_BASE + aCode;   // EPOC's 8-bit set is Windows-1252
	return 0;
	}

// Windows-1252 0x80..0x9f -> Unicode
static const unsigned short KCp1252[32] =
	{
	0x20ac, 0x81, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
	0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8d, 0x017d, 0x8f,
	0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
	0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x9d, 0x017e, 0x0178
	};

static void Expand(const PwEvent& aEv)
	{
	switch (aEv.type)
		{
		case PW_EV_KEYDOWN:
			{
			int k = MapKey(aEv.code, aEv.x);
			if (k >= PWB_UNICODE_BASE + 0x80 && k < PWB_UNICODE_BASE + 0xa0)
				k = PWB_UNICODE_BASE + KCp1252[k - PWB_UNICODE_BASE - 0x80];
			if (!k)
				break;
			int ctrl = (aEv.x & EModifierCtrl) && k < 256;
			if (ctrl && !gCtrlDown) { Push(PWB_KEYDOWN, NK_LCTRL, 0, 0); gCtrlDown = 1; }
			Push(PWB_KEYDOWN, k, 0, 0);
			Push(PWB_KEYUP, k, 0, 0);
			if (gCtrlDown) { Push(PWB_KEYUP, NK_LCTRL, 0, 0); gCtrlDown = 0; }
			break;
			}
		case PW_EV_PENDOWN:
			Push(PWB_MOVE, 0, aEv.x, aEv.y);
			Push(PWB_KEYDOWN, NK_MOUSE_1, aEv.x, aEv.y);
			break;
		case PW_EV_PENMOVE:
			Push(PWB_MOVE, 0, aEv.x, aEv.y);
			break;
		case PW_EV_PENUP:
			Push(PWB_MOVE, 0, aEv.x, aEv.y);
			Push(PWB_KEYUP, NK_MOUSE_1, aEv.x, aEv.y);
			break;
		}
	}

static int TakeShared()
	{
	PwShared* s = Pw();
	while (s->ev_tail != s->ev_head && gQn == 0)
		{
		PwEvent ev = s->ev[s->ev_tail % PW_EVQ];
		s->ev_tail++;
		Expand(ev);
		}
	return gQn;
	}

// Housekeeping every ~second: give the serial port back when idle
// (pwnet.c), and quit if PsiWeb.app has gone - its heartbeat, app_beat,
// has stopped - so the engine can never be left holding the port.
static TUint gLastCheck = 0;
static TUint gLastBeat = 0;
static TUint gBeatSeen = 0;
static int Housekeeping(PwShared* s)
	{
	TUint now = User::TickCount();       // 1/64 s
	if (now - gLastCheck < 64)
		return 0;
	gLastCheck = now;
	pwn_idle_tick();
	if (s->app_beat != gLastBeat || gBeatSeen == 0)
		{
		gLastBeat = s->app_beat;
		gBeatSeen = now;
		}
	else if (now - gBeatSeen > 64 * 60)  // a minute without a sign of the app
		{
		SetText(s->exit_msg, sizeof(s->exit_msg),
			"PsiWeb.app stopped responding, so the browser engine closed");
		return 1;
		}
	return 0;
	}

extern "C" int pwb_next_event(pwb_event* aEv, int aTimeoutMs)
	{
	PwShared* s = Pw();
	TInt waited = 0;
	for (;;)
		{
		if (gPw && Housekeeping(s))
			{
			aEv->type = PWB_QUIT;     // (not 'quitting': the app should say why)
			return 1;
			}
		if (gQn || TakeShared())
			{
			*aEv = gQ[0];
			for (int i = 1; i < gQn; i++) gQ[i - 1] = gQ[i];
			gQn--;
			return 1;
			}
		if (s->quitting)
			{
			aEv->type = PWB_QUIT;
			return 1;
			}
		if (s->cmd != PW_CMD_NONE)
			{
			aEv->type = PWB_WAKE;
			return 1;
			}
		if (aTimeoutMs >= 0 && waited >= aTimeoutMs)
			return 0;
		// 1/64 s: the Psion's tick. Keeps the pen and keys responsive while
		// letting the CPU sleep between events.
		User::After(15625);
		waited += 16;
		}
	}

extern "C" int pwb_take_command(char* aArg, int aMax)
	{
	PwShared* s = Pw();
	int c = s->cmd;
	if (c != PW_CMD_NONE)
		{
		SetText(aArg, aMax, s->cmd_arg);
		s->cmd = PW_CMD_NONE;
		// Stop sets net.quit to interrupt a dial or download; it has done
		// that by now. Clear it on any command, not just Stop: if another
		// command replaced Stop before we saw it, a leftover quit would
		// cancel every fetch from then on.
		if (!s->quitting)
			s->net.quit = 0;
		}
	return c;
	}

extern "C" void pwb_set_status(const char* aText)
	{
	SetText(Pw()->status, sizeof(Pw()->status), aText);
	// (progress lines repeat every few KB: only log a change of wording)
	if (aText && Mem::Compare((const TUint8*)aText, 12, (const TUint8*)gLogLast, 12) != 0)
		{
		SetText(gLogLast, sizeof(gLogLast), aText);
		char b[120];
		b[0] = 's'; b[1] = 't'; b[2] = ':'; b[3] = ' ';
		SetText(b + 4, sizeof(b) - 4, aText);
		pw_log(b);
		}
	}
extern "C" void pwb_set_title(const char* aText) { SetText(Pw()->title, sizeof(Pw()->title), aText); }
extern "C" void pwb_set_url(const char* aText) { SetText(Pw()->url, sizeof(Pw()->url), aText); }
extern "C" void pwb_set_busy(int aBusy)
	{
	Pw()->busy = aBusy;
	pw_log(aBusy ? "busy 1" : "busy 0");
	}
extern "C" void pwb_set_nav(int aBack, int aFwd) { Pw()->can_back = aBack; Pw()->can_forward = aFwd; }

extern "C" void pwb_fatal(const char* aWhy)
	{
	if (gPw && !gPw->exit_msg[0])
		SetText(Pw()->exit_msg, sizeof(Pw()->exit_msg), aWhy);
	}

extern "C" void pwb_ready()
	{
	pw_log("ready");
	TMemoryInfoV1Buf mem;
	UserHal::MemoryInfo(mem);
	Pw()->free_ram = mem().iFreeRamInBytes;
	Pw()->state = PW_STATE_READY;
	}

/* the first call (NetSurf's first page) takes start_url if there is one;
   Home always goes to the home page */
extern "C" const char* pwb_home_url()
	{
	PwShared* s = Pw();
	if (s->start_url[0] && !s->start_taken)
		{
		s->start_taken = 1;
		return s->start_url;
		}
	s->start_taken = 1;
	return s->home_url[0] ? s->home_url : "about:welcome";
	}
/* Links (web/links): the first page is the one another program (PsiMail)
   asked for, else PsiWeb's built-in welcome page - never the home page, so
   starting PsiWeb needs no network. Home then goes to the home page. */
extern "C" const char* pwb_first_url()
	{
	PwShared* s = Pw();
	if (s->start_url[0] && !s->start_taken)
		{
		s->start_taken = 1;
		return s->start_url;
		}
	s->start_taken = 1;
	return "about:welcome";
	}
extern "C" const char* pwb_res_dir() { return Pw()->res_dir; }
extern "C" int pwb_load_images() { return Pw()->load_images; }
extern "C" int pwb_zoom() { return Pw()->zoom; }
extern "C" void* pwb_shared() { return Pw(); }

// (the fetcher's poll slices and time-outs: tick resolution, not seconds)
extern "C" unsigned long pwb_ms()
	{
	long tv[2];
	pwb_gettimeofday(tv, 0);
	return (unsigned long)tv[0] * 1000UL + (unsigned long)(tv[1] / 1000);
	}
