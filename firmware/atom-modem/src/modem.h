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
#include "ppp.h"
#include "wgconf.h"
#include "netlist.h"
#include "dnsrelay.h"

namespace am {

class Cast;

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
	// the aFromEnd'th line from the end (0 = the last), up to aMax-1 bytes,
	// without its newline; false if there are not that many lines
	bool Line(size_t aFromEnd, char* aOut, size_t aMax) const;
	static const size_t kSize = 4096;
private:
	char iBuf[kSize];
	size_t iHead, iCount;
	};

// ----- a WiFi network seen in a scan (AT$SCAN) ------------------------------
struct WifiNet
	{
	char ssid[33];             // as broadcast (bytes, not always ASCII); "" = hidden
	int8_t rssi;               // dBm; 0 = not known
	uint8_t secure;            // 1: needs a password, 0: open
	uint8_t channel;           // 0 = not known
	};

// why the last join did not work (Hal::WifiFail, AT$STAT? FAIL=)
enum TWifiFail { EWifiFailNone = 0, EWifiFailNoNetwork, EWifiFailPassword, EWifiFailOther };

// ----- the WireGuard tunnel, as the board reports it (AT$STAT?, the pages) ----
enum TWgState
	{
	EWgStOff = 0,          // not running (off, no config, at home, no uplink: see TWgWant)
	EWgStTime,             // waiting for the clock (NTP): the handshake needs the time
	EWgStResolve,          // looking up the endpoint
	EWgStConnecting,       // handshake sent, no answer yet
	EWgStUp,               // a handshake within the last 3 minutes
	EWgStRetry             // it failed; trying again shortly
	};

struct WgStatus
	{
	int state;             // TWgState
	int32_t handshakeAge;  // seconds since the last handshake, -1 none yet
	uint32_t rx, tx;       // bytes through the tunnel (inside it), since it started
	uint32_t endpointIp;   // the endpoint's address once looked up (ip4_addr), 0 not yet
	char why[48];          // a few words when it is not up ("endpoint not found")
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
	// the uplink's DNS server as an lwIP ip4_addr (bytes a,b,c,d in memory
	// order: see Ip4()), 0 = not known. PPP hands it to the Psion
	virtual uint32_t UplinkDns() { return 0; }
	// the uplink's own address and netmask (ip4_addr order); false = none
	virtual bool UplinkAddr(uint32_t& aIp, uint32_t& aMask) { aIp = aMask = 0; return false; }
	virtual const char* BoardName() { return "host"; }
	virtual void ApPass(char* aOut, size_t aMax) { if (aMax) aOut[0] = 0; }   // the access point's password, while it is up
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
	virtual void WifiHardReset() {}            // a real radio down/up for the rejoin retry (no-op off-device)
	virtual void WifiEnd() = 0;
	virtual void WifiInfo(char* aOut, size_t aMax) = 0; // "SSID x, IP a.b.c.d, RSSI -60"
	virtual int WifiScan(char aNames[][33], int aMax) { (void)aNames; (void)aMax; return 0; }
	// the networks in range with their signal and security, up to aMax (the
	// default asks WifiScan for the names only: signal and channel unknown)
	virtual int WifiScanFull(WifiNet* aOut, int aMax);
	// the network joined now: its name, address and signal; false = none
	virtual bool WifiLink(char* aSsid, size_t aSsidMax, char* aIp, size_t aIpMax, int& aRssi)
		{ (void)aSsid; (void)aSsidMax; (void)aIp; (void)aIpMax; aRssi = 0; return false; }
	// TWifiFail: why joining has not worked since the last WifiBegin
	virtual int WifiFail() { return EWifiFailNone; }
	// the Internet check: a TCP connect to aHost:aPort, run by the board in
	// the background (never in the modem's loop); Internet() is the last
	// result, -1 unknown, 0 failed, 1 passed
	virtual void ProbeInternet(const char* aHost, uint16_t aPort) { (void)aHost; (void)aPort; }
	virtual int Internet() { return -1; }
	// the check's DNS lookup (2.2): -1 unknown, 0 failed, 1 a name resolved.
	// Internet() is 1 only when this passed too
	virtual int DnsOk() { return -1; }
	// the WireGuard tunnel (board/wgtunnel.cpp): brought up with aConfig when
	// aWant, taken down when not. Called when either changes (and harmless
	// to call again). WgInfo: how it is doing
	virtual void WgApply(const WgConfig& aConfig, bool aWant) { (void)aConfig; (void)aWant; }
	virtual void WgInfo(WgStatus& aOut) { aOut.state = EWgStOff; aOut.handshakeAge = -1; aOut.rx = aOut.tx = 0; aOut.endpointIp = 0; aOut.why[0] = 0; }
	// the DNS relay on 192.168.7.1:53 (board/pppdns.cpp), for a PPP call:
	// started with the link (false: not available, the Psion is then offered
	// the uplink's DNS), stopped with it; its upstream set by the modem
	virtual bool DnsRelayStart() { return false; }
	virtual void DnsRelayStop() {}
	virtual void DnsRelayUpstream(uint32_t aServer) { (void)aServer; }
	// the USB host (ESP32-S3): a TUsbState, and a few words (the device, its address)
	virtual int UsbState(char* aDetail, size_t aMax) { if (aMax) aDetail[0] = 0; return EUsbNone; }
	// settings
	virtual bool LoadSettings(Settings& aS) = 0;
	virtual bool SaveSettings(const Settings& aS) = 0;
	// the 2.0 record: aLength is how many bytes were stored (an older record is shorter)
	virtual bool LoadSettings2(Settings2& aS, size_t& aLength) { (void)aS; aLength = 0; return false; }
	virtual bool SaveSettings2(const Settings2& aS) { (void)aS; return true; }
	// 2.2: the saved networks and the WireGuard config, each its own record
	virtual bool LoadNets(NetList& aL, size_t& aLength) { (void)aL; aLength = 0; return false; }
	virtual bool SaveNets(const NetList& aL) { (void)aL; return true; }
	virtual bool LoadWg(WgConfig& aC, size_t& aLength) { (void)aC; aLength = 0; return false; }
	virtual bool SaveWg(const WgConfig& aC) { (void)aC; return true; }
	virtual void FactoryReset() {}                      // clears NVS and restarts (the board)
	virtual void Restart() {}
	// optional extras
	virtual void Dcd(bool aOn) { (void)aOn; }
	virtual void Led(LedState aState) { (void)aState; }
	// the UART's pins and flow control, the DCD pin
	virtual void ApplyPins(const Settings& aS, const Settings2& aS2) { (void)aS; (void)aS2; }
	virtual void ApplyWeb(const Settings2& aS2) { (void)aS2; }   // the pages and the access point
	virtual void ApplyUplink(const Settings2& aS2) { (void)aS2; } // (the USB host at the next restart)
	virtual void ApInfo(char* aOut, size_t aMax) { if (aMax) aOut[0] = 0; }   // "PsiConnect-1A2B up, 192.168.4.1"
	virtual void WebInfo(char* aOut, size_t aMax) { if (aMax) aOut[0] = 0; }  // "http://192.168.1.50/"
	// the firmware trial (fwtrial.h): a TFwShow (0 all well, 1 a new firmware
	// on trial, 2 running the previous one after an update failed), and a line
	// for ATI ("" when all is well)
	virtual int FirmwareInfo(char* aOut, size_t aMax) { if (aMax) aOut[0] = 0; return 0; }
	// PsiCast (cast/cast.h): random bytes for its tokens, and its big buffers
	// (link A's 64 KB receive buffer; PSRAM where there is some). BigRoom:
	// could aLen more be taken now (the screen copy, which the codec mallocs)
	// and still leave the heap enough for WiFi and TLS
	virtual void Random(uint8_t* aOut, size_t aLen);
	virtual void* BigAlloc(size_t aLen);
	virtual void BigFree(void* aP);
	virtual bool BigRoom(size_t aLen) { (void)aLen; return true; }
private:
	LogRing iLog;
	bool iTlsVerify;
	};

// the command line as the log shows it, with every secret left out
void LogSafe(const char* aCmd, char* aOut, size_t aMax);

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
	bool PppCall() const { return iPppCall; }
	// PsiCast (cast/cast.h): the service, given by main.cpp (or a test); a
	// dial to ATDT192.168.7.1:5961 is then a call to it, here in the modem
	void SetCast(Cast* aCast) { iCast = aCast; }
	Cast* CastService() const { return iCast; }
	bool CastCall() const { return iCastCall; }
	const PppLink& Ppp() const { return iPpp; }
	bool Closing() const { return iClosing; }
	int LedNow() const { return iLed; }             // the LED state last shown (the status screen's bar too)
	const char* LastDial() const { return iLastDial; }          // the last number or host dialled, "" = none yet
	const char* LastDialResult() const { return iLastDialResult; }   // "CONNECT", "NO CARRIER" or "ERROR"
	const Proxy& WebProxy() const { return iProxy; }
	const Uplink& Link() const { return iUplink; }
	size_t RingSize() const { return iRing.Size(); }
	// the web pages: a setting by schema entry (applied as AT$ would), save, factory settings
	bool WebSet(const SchemaEntry& aE, const char* aValue);
	void WebGet(const SchemaEntry& aE, char* aOut, size_t aMax) const;
	bool Save();
	void Factory(bool aKeepWifi = false);
	// first boot with no network saved: a network preset at build time
	// (main.cpp's AM_DEFAULT_SSID/PASS) is taken, saved and joined. Does
	// nothing once a network is saved, or when aSsid is empty
	void SeedNetwork(const char* aSsid, const char* aPass);
	// "config mode": the access point for a while (the button)
	void ConfigMode(bool aOn);
	bool InConfigMode() const { return iConfigMode; }
	void LogLine(const char* aLine) { iHal.Log(aLine); }

	// ----- 2.2: the saved networks -----
	const NetList& Nets() const { return iNets; }
	// switch to saved entry aIndex now (pinned while it is in range); false: no such entry
	bool SwitchNet(int aIndex);
	// the next saved network in range (the button); false: none other
	bool NextNet();
	// add or update (the web pages: saved at once with aSave)
	bool AddNet(const char* aSsid, const char* aPass, bool aSave);
	bool ForgetNet(int aIndex, bool aSave);
	const char* Pinned() const { return iPinned; }

	// ----- 2.2: WireGuard -----
	const WgConfig& Wg() const { return iWg; }
	int WgMode() const { return iS2.wgMode; }
	// AT$WG / the pages / the button; aSave: saved at once (not waiting for AT&W)
	void SetWgMode(int aMode, bool aSave);
	// the next mode Off -> On -> Auto -> Off (the button), saved
	int CycleWgMode();
	// a pasted config (the web page, AT$WGL): parsed, kept and saved at once.
	// False: not taken, aWhy says why (never quoting a key)
	bool SetWgConfig(const char* aText, char* aWhy, size_t aWhyMax);
	void ClearWgConfig();
	TWgWant WgWant() const { return iWgWant; }
	// a one-line state for the LCD, the pages and AT$WG? ("up, 12 s ago")
	void WgStateText(char* aOut, size_t aMax) const;
	// the board's view of the tunnel (cached each tick)
	const WgStatus& WgNow() const { return iWgStatus; }

	// ----- 2.2: AT commands over PPP (atremote.h) -----
	// one command line from PsiConnect over TCP: run as a serial one would be
	// (AtRemoteAllowed refusing what would cut the PPP link or the serial
	// line), its answer captured into aOut (always verbose; at most aMax-1
	// bytes, NUL-terminated); returns its length. Never hangs up
	size_t RemoteLine(const char* aLine, char* aOut, size_t aMax);

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
	TResult DialTo(const char* aArgs);
	TResult DialExec(const char* aCommand);
	TResult DialCast();
	bool ExecHandshake(const char* aCommand);
	TResult SetCommand(const char* aCmd, const char*& aP, bool& aHandled);
	void Apply(TSchemaApply aWhat, uint32_t aOldBaud, bool aFromPsion = false);
	void Info();
	void ShowSettings();
	void Help();
	void ShowLog();
	void Scan();                        // AT$SCAN
	TResult Join(const char* aArgs);    // AT$JOIN=ssid,password
	void Status();                      // AT$STAT?
	void ListNets();                    // AT$NETS?
	void ShowSchema();                  // AT$SCHEMA?
	int NetArg(const char* aArg) const; // AT$NET=, AT$FORGET=: an index (1-based) or a quoted name
	void ChooseNet(uint32_t aNow, bool aStart);   // scan and join the best saved network
	void SaveNetsNow();
	void WgTick(uint32_t aNow);
	void UpsertCurrent();
	void StartLineTrial();
	void LineTrialRevert();
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
	char iLastDial[48];                 // what was dialled last (for the status screen)
	char iLastDialResult[12];
	Proxy iProxy;                       // the web proxy (a psiproxy call)
	bool iProxyCall;
	PppLink iPpp;                       // PPP-over-serial (a numeric dial, AT$PPP=1)
	bool iPppCall;                      // the current call is a PPP link
	bool iExecCall;                     // a psiexec call (a text channel to the helper)
	Cast* iCast;                        // PsiCast, if this firmware has it
	bool iCastCall;                     // the call is PsiCast's link A (ATDT192.168.7.1:5961)
	uint32_t iCastId;
	bool iExecOneShot;                  // ... from AT$EXEC=: ends with OK, not NO CARRIER
	bool iTlsCall;                      // the call is TLS-terminated here
	bool iConfigMode;
	uint32_t iConfigUntilMs;
	uint32_t iLastTickMs;
	// the WiFi: joining again when it stays down, and the Internet check
	static const uint32_t kWifiRetryAfterMs = 8000;    // down this long: reset the radio and join again
	static const uint32_t kWifiRetryEveryMs = 15000;   // ... and every so often after that
	static const uint32_t kProbeSoonMs = 15000;        // the check, while not confirmed
	static const uint32_t kProbeAgainMs = 120000;      // ... and once it has passed
	uint32_t iJoinMs;                   // when AT$JOIN asked (0: not since the start)
	int iJoinFail;                      // TWifiFail seen since then, kept until joined (a
	                                    // rejoin's WifiBegin clears the board's own)
	uint32_t iWifiDownMs;               // 0: not down
	uint32_t iWifiRetryMs;
	uint32_t iWifiRetries;              // rejoins attempted this outage (a hard radio reset only from the 2nd)
	uint32_t iProbeDueMs;
	int iLastWifiState;
	int iLastActive;
	void NetTick(uint32_t aNow);
	// 2.2
	NetList iNets;
	char iPinned[33];                   // a network picked by hand (AT$NET), kept while in range
	uint32_t iNetSkip;                  // saved entries that failed this time round (bit i)
	int iNetTrying;                     // the entry being joined by ChooseNet, -1 none
	WgConfig iWg;
	TWgWant iWgWant;
	bool iWgApplied;                    // WgApply has been called with the current want
	uint32_t iWgGen, iWgAppliedGen;     // bumped when the config or mode changes
	uint32_t iWgIp;                     // the uplink address the decision was made on
	WgStatus iWgStatus;
	int iWgLastState;
	uint32_t iWgTickMs;
	int iLastDnsOk;                     // the check's DNS lookup, as last logged
	uint32_t iDnsUp;                    // the DNS relay's upstream last given to the board
	char iWgText[kWgTextMax + 1];       // AT$WGL lines gathered
	size_t iWgTextLen;
	bool iWgTextBad;                    // a line did not fit: END refuses
	// the line trial (2.2): AT$FC, $FCSWAP, $SWAP, $PINS, $DCD changed from the
	// Psion are applied after the OK and undone if no command is heard within
	// kBaudTrialMs (as the speed is), so a wrong setting cannot cut the Psion off
	bool iPendingPins;                  // apply the pins after the OK
	bool iLineTrial;                    // a line change is on trial
	bool iLineArmed;
	uint32_t iLineStartMs;
	uint8_t iLineOldFlow, iLineOldFlowSwap, iLineOldSwap;
	int8_t iLineOldPins[kPinCount];
	int8_t iLineOldDcd;
	uint8_t iTcpBuf[1460];
	uint8_t iUpBuf[256];                // Psion -> server, batched
	size_t iUpLen;
	// RemoteLine: the answer goes here rather than to the serial line
	void RemoteReload();                // ATZ over PPP: the saved settings, the live link kept
	char* iCap;                         // 0: not capturing
	size_t iCapLen, iCapMax;
	bool iCapCut;                       // something did not fit
	bool iInResult;                     // Result() may use the reserved room
	bool iRemote;                       // a RemoteLine is running
	};

} // namespace am

#endif
