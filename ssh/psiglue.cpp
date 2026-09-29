// psiglue.cpp - native EPOC side of psissh.exe
//
// Owns the serial port (RComm) while an SSH session runs, and the shared
// chunk through which PsiTerm supplies keystrokes and receives output.
// Everything here is exposed as plain C functions (pg_*) for psishim.c.

#include <e32std.h>
#include <e32base.h>
#include <e32hal.h>
#include <c32comm.h>
#include <f32file.h>
#include <es_sock.h>
#include <in_sock.h>

extern "C" {
#include "psishared.h"
}

_LIT(KPddName, "EUART1");
_LIT(KLddName, "ECOMM");
_LIT(KCsyName, "ECUART");
_LIT(KPortName, "COMM::0");

// EXEs may have writable static data on EPOC R5 (unlike .app DLLs), but
// global objects with constructors are avoided: handles live on the heap.
static RChunk* gChunk = 0;
static PsiShared* gShared = 0;
static RCommServ* gServer = 0;
static RComm* gComm = 0;
static int gCommOpen = 0;
static TUint8 gRx[1024];
static int gRxLen = 0;
static int gRxPos = 0;
static int gNetClosed = 0;

// Psion TCP/IP mode (off by default): instead of the WiRSa's "ATDT host:port"
// pipe, connect a real socket through EPOC's own networking - normally a
// dial-up (PPP) connection to the WiRSa dialled with "ATDT PPP".
static int gNet = 0;
static RSocketServ* gSs = 0;
static int gSsOpen = 0;
static RSocket* gSock = 0;
static int gSockOpen = 0;
static RTimer* gTimer = 0;
static int gTimerOpen = 0;
static TRequestStatus gRecvStat;
static TSockXfrLength gRecvLen;
static TPtr8* gRecvDes = 0;
static int gRecvPending = 0;

static int LinkOpen()
	{
	return gNet ? gSockOpen : gCommOpen;
	}

static void SetMsg(char* aOut, int aMax, const char* aText)
	{
	if (!aOut || aMax <= 0)
		return;
	int k = 0;
	while (aText[k] && k < aMax - 1) { aOut[k] = aText[k]; k++; }
	aOut[k] = 0;
	}

static void SetMsgErr(char* aOut, int aMax, const char* aText, TInt aErr)
	{
	if (!aOut || aMax < 16)
		return;
	TPtr8 p((TUint8*)aOut, 0, aMax - 1);
	p.Copy(TPtrC8((const TUint8*)aText));
	p.AppendFormat(_L8(" (error %d)"), aErr);
	p.ZeroTerminate();
	}

static TInt64 NowMicro();
extern "C" int pg_quit_requested();

// Waits for aStat for at most aTimeoutUs (aTimeoutUs < 0: no limit), giving
// up early if PsiTerm asks us to quit and aQuitAware is set.
// Returns 1 if aStat completed, 0 if not (aStat is then still outstanding).
static int WaitFor(TRequestStatus& aStat, TInt aTimeoutUs, int aQuitAware)
	{
	if (!gTimerOpen)
		{
		User::WaitForRequest(aStat);
		return 1;
		}
	TInt64 start = NowMicro();
	for (;;)
		{
		TInt slice = 250000;
		if (aTimeoutUs >= 0)
			{
			TInt64 left = TInt64(aTimeoutUs) - (NowMicro() - start);
			if (left <= 0)
				return 0;
			if (left < slice)
				slice = left.Low();
			}
		TRequestStatus timerStat;
		gTimer->After(timerStat, slice);
		User::WaitForRequest(aStat, timerStat);
		if (aStat != KRequestPending)
			{
			gTimer->Cancel();
			User::WaitForRequest(timerStat);
			return 1;
			}
		if (aQuitAware && pg_quit_requested())
			return 0;
		}
	}

static void NetClose()
	{
	if (gSockOpen)
		{
		if (gRecvPending)
			{
			gSock->CancelRecv();
			User::WaitForRequest(gRecvStat);
			gRecvPending = 0;
			}
		gSock->Close();
		gSockOpen = 0;
		}
	if (gSsOpen)
		{
		gSs->Close();               // EPOC hangs the dial-up up after its idle time
		gSsOpen = 0;
		}
	}

static int NetConnect(char* aResult, int aMax)
	{
	if (!gSs) gSs = new RSocketServ;
	if (!gSock) gSock = new RSocket;
	if (!gTimer) gTimer = new RTimer;
	if (!gRecvDes) gRecvDes = new TPtr8(gRx, 0, sizeof(gRx));
	if (!gSs || !gSock || !gTimer || !gRecvDes)
		{
		SetMsg(aResult, aMax, "out of memory");
		return -1;
		}
	if (!gTimerOpen && gTimer->CreateLocal() == KErrNone)
		gTimerOpen = 1;
	TInt r = gSs->Connect();
	if (r != KErrNone)
		{
		SetMsgErr(aResult, aMax, "Psion networking is not available", r);
		return -1;
		}
	gSsOpen = 1;

	TBuf<128> host;
	host.Copy(TPtrC8((const TUint8*)gShared->host));
	TInetAddr addr;
	if (addr.Input(host) != KErrNone)
		{
		// a name: look it up (this is usually what starts the dial-up)
		RHostResolver resolver;
		r = resolver.Open(*gSs, KAfInet, KProtocolInetUdp);
		if (r != KErrNone)
			{
			SetMsgErr(aResult, aMax, "no name resolver - is TCP/IP installed?", r);
			return -1;
			}
		TNameEntry entry;
		TRequestStatus stat;
		resolver.GetByName(host, entry, stat);
		if (!WaitFor(stat, 120000000, 1))
			{
			resolver.Cancel();
			User::WaitForRequest(stat);
			resolver.Close();
			SetMsg(aResult, aMax, "gave up looking up the host name");
			return -1;
			}
		resolver.Close();
		if (stat.Int() != KErrNone)
			{
			SetMsgErr(aResult, aMax, "could not look up the host name", stat.Int());
			return -1;
			}
		addr = TInetAddr(entry().iAddr);
		}
	addr.SetPort(gShared->port > 0 ? gShared->port : 22);

	r = gSock->Open(*gSs, KAfInet, KSockStream, KProtocolInetTcp);
	if (r != KErrNone)
		{
		SetMsgErr(aResult, aMax, "could not open a TCP socket", r);
		return -1;
		}
	TRequestStatus stat;
	gSock->Connect(addr, stat);          // starts the dial-up if it isn't up
	if (!WaitFor(stat, 120000000, 1))
		{
		gSock->CancelConnect();
		User::WaitForRequest(stat);
		gSock->Close();
		SetMsg(aResult, aMax, "gave up connecting");
		return -1;
		}
	if (stat.Int() != KErrNone)
		{
		gSock->Close();
		SetMsgErr(aResult, aMax, "connection failed", stat.Int());
		return -1;
		}
	gSockOpen = 1;
	return 0;
	}

// Moves a completed receive into gRx. Keeps one receive outstanding at all
// times instead of cancelling, so no data can be lost to a cancel race.
static void NetRxFill(int aTimeoutUs)
	{
	if (gRxPos < gRxLen || !gSockOpen || gNetClosed)
		return;
	if (!gRecvPending)
		{
		gRecvDes->Set(gRx, 0, sizeof(gRx));
		gSock->RecvOneOrMore(*gRecvDes, 0, gRecvStat, gRecvLen);
		gRecvPending = 1;
		}
	if (gRecvStat == KRequestPending && !WaitFor(gRecvStat, aTimeoutUs, 0))
		return;                          // still waiting; try again later
	if (gRecvStat == KRequestPending)
		return;
	User::WaitForRequest(gRecvStat);     // consume its completion signal
	gRecvPending = 0;
	gRxPos = 0;
	if (gRecvStat.Int() == KErrNone)
		gRxLen = gRecvDes->Length();
	else
		{
		gRxLen = 0;
		gNetClosed = 1;                  // KErrEof or a link error
		}
	}

static int NetWrite(const void* aBuf, int aLen)
	{
	if (!gSockOpen)
		return -1;
	TPtrC8 data((const TUint8*)aBuf, aLen);
	TRequestStatus stat;
	gSock->Write(data, stat);
	if (!WaitFor(stat, 60000000, 0))
		{
		gSock->CancelWrite();
		User::WaitForRequest(stat);
		return -1;
		}
	return stat.Int() == KErrNone ? aLen : -1;
	}

static TBps BaudFromIndex(int aIndex)
	{
	switch (aIndex)
		{
	case 0: return EBps9600;
	case 1: return EBps19200;
	case 2: return EBps38400;
	case 3: return EBps57600;
	default: return EBps115200;
		}
	}

static TInt64 NowMicro()
	{
	TTime t;
	t.HomeTime();
	return t.Int64();
	}

extern "C" PsiShared* pg_shared()
	{
	return gShared;
	}

extern "C" int pg_init()
	{
	gChunk = new RChunk;
	if (!gChunk)
		return -1;
	TInt r = gChunk->OpenGlobal(_L(PSI_SHARED_NAME), EFalse);
	if (r != KErrNone)
		return -2;
	gShared = (PsiShared*)gChunk->Base();
	if (gShared->magic != PSI_SHARED_MAGIC)
		return -3;
	if (gShared->mode == 1)
		return 0;                   // speed test: no serial port needed
	if (gShared->net_mode)
		{
		gNet = 1;                   // Psion TCP/IP: the socket opens in pg_dial
		return 0;
		}

	r = User::LoadPhysicalDevice(KPddName);
	if (r != KErrNone && r != KErrAlreadyExists)
		return -4;
	r = User::LoadLogicalDevice(KLddName);
	if (r != KErrNone && r != KErrAlreadyExists)
		return -5;
	r = StartC32();
	if (r != KErrNone && r != KErrAlreadyExists)
		return -6;
	gServer = new RCommServ;
	gComm = new RComm;
	if (!gServer || !gComm)
		return -7;
	r = gServer->Connect();
	if (r != KErrNone)
		return -8;
	r = gServer->LoadCommModule(KCsyName);
	if (r != KErrNone && r != KErrAlreadyExists)
		return -9;
	r = gComm->Open(*gServer, KPortName, ECommExclusive);
	if (r != KErrNone)
		return -10;
	gCommOpen = 1;

	TCommConfig cfg;
	gComm->Config(cfg);
	cfg().iRate = BaudFromIndex(gShared->baud_index);
	cfg().iDataBits = EData8;
	cfg().iStopBits = EStop1;
	cfg().iParity = EParityNone;
	cfg().iFifo = EFifoEnable;
	cfg().iTerminatorCount = 0;
	cfg().iHandshake = gShared->rtscts ? (KConfigObeyCTS | KConfigFreeRTS) : 0;
	r = gComm->SetConfig(cfg);
	if (r != KErrNone)
		return -11;
	// Bigger than Dropbear's 8 KB SSH receive window plus packet overhead,
	// so the server can never send more than fits here: no overruns even
	// at 115200 without working hardware flow control.
	gComm->SetReceiveBufferLength(16384);
	gComm->SetSignals(KSignalDTR | KSignalRTS, 0);
	return 0;
	}

extern "C" void pg_close()
	{
	NetClose();
	if (gTimerOpen)
		{
		gTimer->Close();
		gTimerOpen = 0;
		}
	if (gCommOpen)
		{
		gComm->Close();
		gCommOpen = 0;
		}
	if (gServer)
		gServer->Close();
	}

extern "C" void pg_set_state(int aState)
	{
	if (gShared)
		gShared->state = aState;
	}

extern "C" void pg_set_exit(int aCode)
	{
	if (gShared)
		{
		gShared->exit_code = aCode;
		gShared->state = PSI_STATE_EXITED;
		}
	}

extern "C" int pg_quit_requested()
	{
	return gShared ? gShared->quit : 1;
	}

extern "C" void pg_msleep(int aMs)
	{
	User::After(aMs * 1000);
	}

// ----- serial ("network") ---------------------------------------------------

extern "C" int pg_serial_write(const void* aBuf, int aLen)
	{
	if (gNet)
		return aLen > 0 ? NetWrite(aBuf, aLen) : -1;
	if (!gCommOpen || aLen <= 0)
		return -1;
	TPtrC8 data((const TUint8*)aBuf, aLen);
	TRequestStatus stat;
	gComm->Write(stat, TTimeIntervalMicroSeconds32(10000000), data);
	User::WaitForRequest(stat);
	return (stat.Int() == KErrNone) ? aLen : -1;
	}

// Pull whatever arrives within aTimeoutUs into the receive buffer.
static void RxFill(int aTimeoutUs)
	{
	if (gNet)
		{
		NetRxFill(aTimeoutUs);
		return;
		}
	if (gRxPos < gRxLen || !gCommOpen)
		return;
	gRxPos = 0;
	gRxLen = 0;
	TPtr8 p(gRx, 0, sizeof(gRx));
	TRequestStatus stat;
	gComm->Read(stat, TTimeIntervalMicroSeconds32(aTimeoutUs), p);
	User::WaitForRequest(stat);
	if (stat.Int() == KErrNone || stat.Int() == KErrTimedOut)
		gRxLen = p.Length();
	else
		gRxLen = 0;      // line error: drop this burst, SSH will notice
	}

extern "C" int pg_net_avail()
	{
	return gRxLen - gRxPos;
	}

extern "C" int pg_net_read(void* aBuf, int aMax)
	{
	int n = gRxLen - gRxPos;
	if (n > aMax)
		n = aMax;
	if (n > 0)
		{
		Mem::Copy(aBuf, gRx + gRxPos, n);
		gRxPos += n;
		}
	return n;
	}

extern "C" void pg_net_set_closed()
	{
	gNetClosed = 1;
	}

// ----- keyboard / screen rings ----------------------------------------------

extern "C" int pg_kbd_avail()
	{
	if (!gShared)
		return 0;
	return (int)(gShared->kbd_head - gShared->kbd_tail);
	}

extern "C" int pg_kbd_read(void* aBuf, int aMax)
	{
	unsigned int tail = gShared->kbd_tail;
	int n = (int)(gShared->kbd_head - tail);
	if (n > aMax)
		n = aMax;
	if (n <= 0)
		return 0;
	unsigned int off = tail % PSI_KBD_SIZE;
	int run = PSI_KBD_SIZE - (int)off;
	if (run > n)
		run = n;
	Mem::Copy(aBuf, gShared->kbd + off, run);
	if (n > run)
		Mem::Copy((unsigned char*)aBuf + run, gShared->kbd, n - run);
	gShared->kbd_tail = tail + n;
	return n;
	}

extern "C" void pg_out_write(const void* aBuf, int aLen)
	{
	const unsigned char* in = (const unsigned char*)aBuf;
	while (aLen > 0)
		{
		// copy as much as fits in at most two runs (the ring may wrap),
		// then publish the new head: one reader (PsiTerm), one writer.
		unsigned int head = gShared->out_head;
		int space = PSI_OUT_SIZE - (int)(head - gShared->out_tail);
		if (space <= 0)
			{
			if (gShared->quit)
				return;
			User::After(10000);         // PsiTerm will drain it shortly
			continue;
			}
		int n = aLen < space ? aLen : space;
		unsigned int off = head % PSI_OUT_SIZE;
		int run = PSI_OUT_SIZE - (int)off;
		if (run > n)
			run = n;
		Mem::Copy(gShared->out + off, in, run);
		if (n > run)
			Mem::Copy(gShared->out, in + run, n - run);
		gShared->out_head = head + n;
		in += n;
		aLen -= n;
		}
	}

extern "C" void pg_winsize(int* aRows, int* aCols)
	{
	*aRows = gShared ? gShared->rows : 24;
	*aCols = gShared ? gShared->cols : 80;
	if (*aRows < 5) *aRows = 24;
	if (*aCols < 20) *aCols = 80;
	}

extern "C" int pg_take_resize()
	{
	if (gShared && gShared->resized)
		{
		gShared->resized = 0;
		return 1;
		}
	return 0;
	}

// ----- waiting ----------------------------------------------------------------
// Returns a bit mask: 1 = network data, 2 = keyboard data, 4 = resize, 8 = quit.
// aMs < 0 waits forever.
extern "C" int pg_wait(int aMs, int aWantNet, int aWantKbd)
	{
	TInt64 start = NowMicro();
	for (;;)
		{
		int mask = 0;
		if (aWantNet && (pg_net_avail() > 0 || gNetClosed))
			mask |= 1;
		if (aWantKbd && pg_kbd_avail() > 0)
			mask |= 2;
		if (gShared && gShared->resized)
			mask |= 4;
		if (pg_quit_requested())
			mask |= 8;
		if (mask)
			return mask;
		int slice = 15625;                  // look at the keyboard every tick (1/64 s)
		if (aMs >= 0)
			{
			TInt64 left = TInt64(aMs) * 1000 - (NowMicro() - start);
			if (left <= 0)
				return 0;
			if (left < slice)
				slice = left.Low();
			if (slice < 1000)
				slice = 1000;
			}
		if (aWantNet && LinkOpen() && !gNetClosed)
			RxFill(slice);
		else
			User::After(slice);
		}
	}

// ----- dialling the WiRSa -----------------------------------------------------

static int ReadLine(char* aLine, int aMax, int aTimeoutMs)
	{
	int n = 0;
	TInt64 start = NowMicro();
	for (;;)
		{
		if (pg_net_avail() == 0)
			{
			if ((NowMicro() - start) > TInt64(aTimeoutMs) * 1000)
				return -1;
			if (pg_quit_requested())
				return -1;
			RxFill(100000);
			continue;
			}
		char c;
		pg_net_read(&c, 1);
		if (c == '\r' || c == '\n')
			{
			if (n == 0)
				continue;
			aLine[n] = 0;
			return n;
			}
		if (n < aMax - 1)
			aLine[n++] = c;
		}
	}

static int StartsWith(const char* aS, const char* aP)
	{
	while (*aP)
		{
		if (*aS++ != *aP++)
			return 0;
		}
	return 1;
	}

// Dials host:port through the WiRSa. Returns 0 on CONNECT.
// Sends AT and waits briefly for OK: is the modem at its command prompt?
static int ModemAnswersAt()
	{
	char line[160];
	pg_serial_write("AT\r", 3);
	for (int i = 0; i < 4; i++)
		{
		int n = ReadLine(line, sizeof(line), 800);
		if (n < 0)
			return 0;
		if (StartsWith(line, "OK"))
			return 1;
		}
	return 0;
	}

extern "C" int pg_dial(char* aResult, int aResultMax)
	{
	char cmd[220];
	char line[160];
	int i;
	gNetClosed = 0;                      // a fresh connection (update does two)
	gRxPos = gRxLen = 0;
	if (gNet)
		return NetConnect(aResult, aResultMax);
	// quick wake-up so the modem is at a command prompt
	pg_serial_write("\r", 1);
	pg_msleep(300);
	gRxPos = gRxLen = 0;
	gComm->ResetBuffers();
	if (!ModemAnswersAt())
		{
		// still online from an old connection (e.g. the Psion was switched
		// off mid-session): escape to command mode and hang up first
		pg_msleep(1100);
		pg_serial_write("+++", 3);
		pg_msleep(1100);
		pg_serial_write("ATH\r", 4);
		pg_msleep(500);
		gRxPos = gRxLen = 0;
		gComm->ResetBuffers();
		ModemAnswersAt();
		}
	pg_msleep(100);
	gRxPos = gRxLen = 0;
	gComm->ResetBuffers();

	int len = 0;
	const char* p = gShared->dial_prefix[0] ? gShared->dial_prefix : "ATDT";
	while (*p && len < 20) cmd[len++] = *p++;
	cmd[len++] = ' ';
	p = gShared->host;
	while (*p && len < 190) cmd[len++] = *p++;
	cmd[len++] = ':';
	// port number
	char num[8];
	int port = gShared->port > 0 ? gShared->port : 22;
	int nd = 0;
	do { num[nd++] = (char)('0' + port % 10); port /= 10; } while (port && nd < 7);
	while (nd) cmd[len++] = num[--nd];
	cmd[len++] = '\r';
	pg_serial_write(cmd, len);

	for (i = 0; i < 10; i++)
		{
		int n = ReadLine(line, sizeof(line), 30000);
		if (n < 0)
			{
			if (aResult) { const char* m = "no answer from modem"; int k = 0; while (m[k] && k < aResultMax - 1) { aResult[k] = m[k]; k++; } aResult[k] = 0; }
			return -1;
			}
		if (StartsWith(line, "CONNECT"))
			return 0;
		if (StartsWith(line, "NO CARRIER") || StartsWith(line, "ERROR") ||
			StartsWith(line, "BUSY") || StartsWith(line, "NO ANSWER") ||
			StartsWith(line, "NO DIALTONE"))
			{
			if (aResult) { int k = 0; while (line[k] && k < aResultMax - 1) { aResult[k] = line[k]; k++; } aResult[k] = 0; }
			return -1;
			}
		// anything else (our own echoed command, status text) is ignored
		}
	if (aResult) { const char* m = "modem did not connect"; int k = 0; while (m[k] && k < aResultMax - 1) { aResult[k] = m[k]; k++; } aResult[k] = 0; }
	return -1;
	}

extern "C" void pg_hangup()
	{
	if (gNet)
		{
		NetClose();
		return;
		}
	if (!gCommOpen)
		return;
	User::After(1100000);
	pg_serial_write("+++", 3);
	User::After(1100000);
	pg_serial_write("ATH\r", 4);
	User::After(300000);
	}

// ----- entropy ----------------------------------------------------------------
// The Psion has no hardware random source, so collect timing noise:
// PsiTerm's keystroke timings, the system clock, and the jitter between the
// CPU and the tick timer. The result is hashed by Dropbear's own pool.
extern "C" int pg_entropy(unsigned char* aOut, int aMax)
	{
	int n = 0;
	#define PUT(v) do { TUint32 _v = (TUint32)(v); for (int _i = 0; _i < 4 && n < aMax; _i++) { aOut[n++] = (unsigned char)(_v & 0xff); _v >>= 8; } } while (0)
	if (gShared)
		{
		int e = gShared->entropy_len;
		if (e > PSI_ENTROPY_SIZE) e = PSI_ENTROPY_SIZE;
		for (int i = 0; i < e && n < aMax; i++)
			aOut[n++] = gShared->entropy[i];
		}
	TInt64 t = NowMicro();
	PUT(t.Low());
	PUT(t.High());
	PUT(User::TickCount());
	RThread thread;
	TThreadId tid = thread.Id();
	TUint32 raw = 0;
	Mem::Copy(&raw, &tid, sizeof(raw));
	PUT(raw);
	TMemoryInfoV1Buf mem;
	UserHal::MemoryInfo(mem);
	PUT(mem().iFreeRamInBytes);
	// timer/CPU jitter: count spins between tick edges
	for (int s = 0; s < 48 && n < aMax - 4; s++)
		{
		TUint tick = User::TickCount();
		TUint32 spins = 0;
		while (User::TickCount() == tick && spins < 2000000)
			spins++;
		PUT(spins ^ (NowMicro().Low() << 8));
		}
	#undef PUT
	return n;
	}

extern "C" const char* pg_home()
	{
	return (gShared && gShared->home[0]) ? gShared->home : "C:\\System\\Apps\\PsiTerm";
	}
