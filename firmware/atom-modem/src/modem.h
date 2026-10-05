// modem.h - the Psion-tuned WiFi/USB modem: Hayes commands, +++ escape, and
// paced output towards the Psion. Plain C++ with no Arduino calls, so the
// same code runs on the Atom (board/, main.cpp) and in the host tests
// (hosttest/). MIT licence (see LICENSE at the top of the repository).
//
// A three-wire board (the Atomic RS232 Base) has only TX, RX and GND: no
// RTS/CTS, so the Psion cannot stop us when its receive buffer fills.
// Instead:
//   - everything from the server goes into a big ring buffer here (32-128 KB);
//   - the ring is emptied towards the Psion at a paced rate below the line
//     rate, in small bursts with gaps (a token bucket);
//   - when the ring is full we stop reading the TCP socket, so TCP's own
//     flow control holds the server back.
// With a four-wire transceiver (AT$FC=1) the UART's own RTS/CTS does the
// stopping and the pacing is relaxed.
//
// Dialling "psiproxy" (ATDT psiproxy:8080) opens no TCP connection: the
// modem is then a web proxy for PsiWeb itself (proxy.h), doing the TLS and
// simplifying pages. "psiexec" is a text channel to a helper on the LAN
// (AT$XE). "tls:host:port", or a port in AT$TLSP with AT$TLS=1, makes the
// modem do the TLS and give the Psion the plain protocol.
#ifndef ATOM_MODEM_H
#define ATOM_MODEM_H

#include <stdint.h>
#include <stddef.h>
#include "settings.h"
#include "schema.h"
#include "uplink.h"
#include "proxy.h"

namespace am {

// ----- the ring buffer (server -> Psion) -----------------------------------
class Ring
	{
public:
	Ring() : iBuf(0), iSize(0), iHead(0), iTail(0), iCount(0) {}
	void Init(uint8_t* aBuf, size_t aSize) { iBuf = aBuf; iSize = aSize; Clear(); }
	void Clear() { iHead = iTail = iCount = 0; }
	size_t Count() const { return iCount; }
	size_t Free() const { return iSize - iCount; }
	size_t Size() const { return iSize; }
	size_t Put(const uint8_t* aData, size_t aLen);
	size_t Get(uint8_t* aOut, size_t aMax);
	// the contiguous bytes at the tail (for writing without a copy)
	size_t Peek(const uint8_t*& aPtr) const;
	void Drop(size_t aLen);
private:
	uint8_t* iBuf;
	size_t iSize, iHead, iTail, iCount;
	};

// ----- the pacer (a token bucket, plus a gap after each burst) ------------
class Pacer
	{
public:
	Pacer() : iRate(0), iBurst(0), iGapUs(0), iTokens(0), iLastUs(0), iQuietUntil(0), iInBurst(0), iStarted(0) {}
	void Configure(uint32_t aRate, uint16_t aBurst, uint16_t aGapMs);
	// how many bytes may go now (0 = wait); aWant = bytes waiting
	size_t Allow(uint32_t aNowUs, size_t aWant);
	// aSent bytes actually went
	void Spent(uint32_t aNowUs, size_t aSent);
	uint32_t Rate() const { return iRate; }
private:
	uint32_t iRate;            // bytes/s, 0 = no pacing
	uint32_t iBurst;
	uint32_t iGapUs;
	uint64_t iTokens;          // in millionths of a byte (rate * microseconds)
	uint32_t iLastUs;
	uint32_t iQuietUntil;
	uint32_t iInBurst;         // bytes sent in the burst so far
	int iStarted;
	};

// ----- the status log, kept in RAM ------------------------------------------
// The USB console is not there in USB host mode (the port is the phone's),
// so the last few KB of status lines are kept here: AT$LOG? and the web
// page /log show them
class LogRing
	{
public:
	LogRing() : iHead(0), iCount(0) { iBuf[0] = 0; }
	void Add(const char* aLine);
	// the oldest aMax-1 bytes from aFrom (0 = the start); returns how many
	size_t Read(size_t aFrom, char* aOut, size_t aMax) const;
	size_t Count() const { return iCount; }
	void Clear() { iHead = iCount = 0; }
	static const size_t kSize = 4096;
private:
	char iBuf[kSize];
	size_t iHead, iCount;
	};

// ----- the platform: serial line, TCP, WiFi, NVS, LED ----------------------
enum LedState { ELedNoWifi, ELedWifi, ELedConnected, ELedData, ELedConnecting, ELedConfig };

class Hal
	{
public:
	Hal() : iTlsVerify(true) {}
	virtual ~Hal() {}
	virtual uint32_t Millis() = 0;
	virtual uint32_t Micros() = 0;
	// serial towards the Psion
	virtual int SerialRead() = 0;                       // -1 = nothing
	virtual size_t SerialWritable() = 0;                // room in the UART's queue
	virtual size_t SerialWrite(const uint8_t* aData, size_t aLen) = 0;
	virtual void SerialBaud(uint32_t aBaud) = 0;        // after the pending output
	// TCP
	virtual bool TcpConnect(const char* aHost, uint16_t aPort) = 0;   // blocks
	virtual bool TcpOpen() = 0;                         // connected, or data still to read
	virtual size_t TcpAvailable() = 0;
	virtual size_t TcpRead(uint8_t* aBuf, size_t aMax) = 0;
	virtual size_t TcpWrite(const uint8_t* aData, size_t aLen) = 0;
	virtual void TcpClose() = 0;
	// a connection to a server, over TLS if aTls (with the certificate
	// checked unless TlsVerify(false)). It is then used through
	// TcpOpen..TcpClose above. Used by the web proxy and by TLS-terminated
	// dials. aWhy: a few words on why it failed. The default does plain
	// TCP only.
	virtual bool UpConnect(const char* aHost, uint16_t aPort, bool aTls, char* aWhy, size_t aWhyMax);
	void TlsVerify(bool aOn) { iTlsVerify = aOn; }
	bool TlsVerify() const { return iTlsVerify; }
	virtual void Idle() {}                              // a moment's wait in a busy loop
	virtual void MemInfo(char* aOut, size_t aMax);      // "heap free 120 KB, ..." for ATI
	// a status line: kept in the log ring, and shown on the USB console
	// when there is one. Platforms that override it call Record() too
	virtual void Log(const char* aLine) { Record(aLine); }
	void Record(const char* aLine) { iLog.Add(aLine); }
	const LogRing& LogLines() const { return iLog; }
	// WiFi
	virtual bool WifiUp() = 0;
	virtual void WifiBegin(const char* aSsid, const char* aPass) = 0;
	virtual void WifiEnd() = 0;
	virtual void WifiInfo(char* aOut, size_t aMax) = 0; // "SSID x, IP a.b.c.d, RSSI -60"
	virtual int WifiScan(char aNames[][33], int aMax) { (void)aNames; (void)aMax; return 0; }
	// the Internet check: a TCP connect to aHost:aPort, run by the board in
	// the background (never in the modem's loop); Internet() is the last
	// result, -1 unknown, 0 failed, 1 passed
	virtual void ProbeInternet(const char* aHost, uint16_t aPort) { (void)aHost; (void)aPort; }
	virtual int Internet() { return -1; }
	// the USB host (ESP32-S3): a TUsbState, and a few words (the device, its address)
	virtual int UsbState(char* aDetail, size_t aMax) { if (aMax) aDetail[0] = 0; return EUsbNone; }
	// settings
	virtual bool LoadSettings(Settings& aS) = 0;
	virtual bool SaveSettings(const Settings& aS) = 0;
	// the 2.0 record: aLength is how many bytes were stored (an older record is shorter)
	virtual bool LoadSettings2(Settings2& aS, size_t& aLength) { (void)aS; aLength = 0; return false; }
	virtual bool SaveSettings2(const Settings2& aS) { (void)aS; return true; }
	virtual void FactoryReset() {}                      // clears NVS and restarts (the board)
	virtual void Restart() {}
	// optional extras
	virtual void Dcd(bool aOn) { (void)aOn; }
	virtual void Led(LedState aState) { (void)aState; }
	// the UART's pins and flow control, the DCD pin
	virtual void ApplyPins(const Settings& aS, const Settings2& aS2) { (void)aS; (void)aS2; }
	virtual void ApplyWeb(const Settings2& aS2) { (void)aS2; }   // the pages and the access point
	virtual void ApplyUplink(const Settings2& aS2) { (void)aS2; } // (the USB host at the next restart)
	virtual void ApInfo(char* aOut, size_t aMax) { if (aMax) aOut[0] = 0; }   // "AtomModem-1a2b up, 192.168.4.1"
	virtual void WebInfo(char* aOut, size_t aMax) { if (aMax) aOut[0] = 0; }  // "http://192.168.1.50/"
private:
	LogRing iLog;
	bool iTlsVerify;
	};

// ----- the modem ----------------------------------------------------------
class Modem
	{
public:
	Modem(Hal& aHal, uint8_t* aRing, size_t aRingSize);
	void Begin();                       // load settings, start WiFi
	void Loop();                        // call as often as possible
	// for the tests, the status LED and the web pages
	bool Online() const { return iOnline; }
	bool Connected() const { return iConnected; }
	const Settings& Config() const { return iS; }
	const Settings2& Config2() const { return iS2; }
	size_t Buffered() const { return iRing.Count(); }
	uint32_t ToPsion() const { return iToPsion; }
	uint32_t ToServer() const { return iToServer; }
	bool ProxyCall() const { return iProxyCall; }
	bool ExecCall() const { return iExecCall; }
	const Proxy& WebProxy() const { return iProxy; }
	const Uplink& Link() const { return iUplink; }
	size_t RingSize() const { return iRing.Size(); }
	// the web pages: a setting by schema entry (applied as AT$ would), save, factory settings
	bool WebSet(const SchemaEntry& aE, const char* aValue);
	void WebGet(const SchemaEntry& aE, char* aOut, size_t aMax) const;
	bool Save();
	void Factory(bool aKeepWifi);
	// first boot with no network saved: a network preset at build time
	// (main.cpp's AM_DEFAULT_SSID/PASS) is taken, saved and joined. Does
	// nothing once a network is saved, or when aSsid is empty
	void SeedNetwork(const char* aSsid, const char* aPass);
	// "config mode": the access point for a while (the button)
	void ConfigMode(bool aOn);
	bool InConfigMode() const { return iConfigMode; }
	void LogLine(const char* aLine) { iHal.Log(aLine); }

private:
	enum TResult { ROk = 0, RConnect = 1, RRing = 2, RNoCarrier = 3, RError = 4, RNone = -1 };
	void SerialIn(uint8_t aC);
	void CommandChar(uint8_t aC);
	void OnlineChar(uint8_t aC);
	void EscapeTick(uint32_t aNow);
	void FlushHeldPluses();
	void RunCommandLine();
	TResult RunCommands(const char* aCmd);
	TResult Dial(const char* aArgs);
	TResult DialExec(const char* aCommand);
	bool ExecHandshake(const char* aCommand);
	TResult SetCommand(const char* aCmd, const char*& aP, bool& aHandled);
	void Apply(TSchemaApply aWhat, uint32_t aOldBaud);
	void Info();
	void ShowSettings();
	void Help();
	void ShowLog();
	void Result(TResult aR);
	void Say(const char* aText);
	void SayLine(const char* aText);
	void Send(const uint8_t* aData, size_t aLen);       // to the Psion, unpaced (command mode)
	void Hangup(bool aSayNoCarrier);
	void EndCall();
	void FlushUp();
	void PumpServer(uint32_t aNow);
	void PumpPsion(uint32_t aNow);
	void ApplyPacing();
	void StartBaudTrial(uint32_t aOld);
	void ConfirmBaud(uint32_t aGen, TResult aR);
	void BaudTrialTick(uint32_t aNow);
	void UpdateDcd();
	void UpdateLed(uint32_t aNow);
	void LoadAll();
	uint32_t GuardMs() const { return (uint32_t)iS.s12 * 20; }

	Hal& iHal;
	Ring iRing;
	Settings iS;
	Settings2 iS2;
	Pacer iPacer;
	Uplink iUplink;
	char iLine[256];
	size_t iLineLen;
	char iLast[256];                    // for A/
	bool iOnline;                       // data mode (else command mode)
	bool iConnected;                    // a TCP connection exists
	bool iClosing;                      // it has closed: NO CARRIER once the data is out
	bool iWifiWasUp;
	uint32_t iLastSerialMs;             // last byte from the Psion
	int iPluses;                        // escape characters held back
	uint32_t iLastPlusMs;
	uint32_t iToPsion, iToServer;
	uint32_t iLastDataMs;
	uint32_t iPendingBaud;              // AT$SB: switch after the OK has gone
	// AT$SB/ATB on trial: back to iTrialFrom if no valid command line
	// arrives at the new speed within kBaudTrialMs (and no AT&W saves it)
	static const uint32_t kBaudTrialMs = 15000;
	uint32_t iTrialFrom;                // the speed to go back to; 0 = none
	uint32_t iTrialStartMs;             // when the new speed took effect
	bool iTrialArmed;                   // it has (the clock is running)
	uint32_t iTrialGen;                 // counts speed changes (see ConfirmBaud)
	int iLed;                           // the LED state last shown
	Proxy iProxy;                       // the web proxy (a psiproxy call)
	bool iProxyCall;
	bool iExecCall;                     // a psiexec call (a text channel to the helper)
	bool iExecOneShot;                  // ... from AT$EXEC=: ends with OK, not NO CARRIER
	bool iTlsCall;                      // the call is TLS-terminated here
	bool iConfigMode;
	uint32_t iConfigUntilMs;
	uint32_t iLastTickMs;
	// the WiFi: joining again when it stays down, and the Internet check
	static const uint32_t kWifiRetryAfterMs = 20000;   // down this long: join again
	static const uint32_t kWifiRetryEveryMs = 30000;   // ... and every so often after that
	static const uint32_t kProbeSoonMs = 15000;        // the check, while not confirmed
	static const uint32_t kProbeAgainMs = 120000;      // ... and once it has passed
	uint32_t iWifiDownMs;               // 0: not down
	uint32_t iWifiRetryMs;
	uint32_t iProbeDueMs;
	int iLastWifiState;
	int iLastActive;
	void NetTick(uint32_t aNow);
	uint8_t iTcpBuf[1460];
	uint8_t iUpBuf[256];                // Psion -> server, batched
	size_t iUpLen;
	};

} // namespace am

#endif
