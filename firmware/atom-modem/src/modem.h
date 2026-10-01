// modem.h - the Psion-tuned WiFi modem: Hayes commands, +++ escape, and
// paced output towards the Psion. Plain C++ with no Arduino calls, so the
// same code runs on the M5Stack Atom (main.cpp) and in the host tests
// (hosttest/). MIT licence (see LICENSE at the top of the repository).
//
// The board (Atom + Atomic RS232 Base) has only TX, RX and GND: no RTS/CTS,
// so the Psion cannot stop us when its receive buffer fills. Instead:
//   - everything from the server goes into a big ring buffer here (64-128 KB);
//   - the ring is emptied towards the Psion at a paced rate below the line
//     rate, in small bursts with gaps (a token bucket);
//   - when the ring is full we stop reading the TCP socket, so TCP's own
//     flow control holds the server back.
// That gives end-to-end flow control without a single handshake wire.
#ifndef ATOM_MODEM_H
#define ATOM_MODEM_H

#include <stdint.h>
#include <stddef.h>

namespace am {

static const char* const kVersion = "1.0";

// ----- settings (saved whole in NVS by AT&W) -------------------------------
struct Settings
	{
	uint32_t magic;            // kMagic: a valid record
	uint32_t baud;             // the serial line to the Psion
	char ssid[33];
	char pass[65];
	uint8_t echo;              // E1: echo commands back (as the WiRSa does)
	uint8_t verbose;           // V1: words ("OK"), V0: digits ("0")
	uint8_t quiet;             // Q1: no result codes at all
	uint8_t dcdMode;           // &C: 0 DCD always on, 1 DCD follows the connection
	uint8_t s2;                // escape character, '+'
	uint8_t s12;               // escape guard time, 1/50 s (40 = 0.8 s)
	uint8_t paceAuto;          // 1: pacing chosen from the baud rate
	uint8_t swapPins;          // 1: RX and TX swapped (a different cable or base)
	uint32_t paceRate;         // bytes/s towards the Psion; 0 = no pacing
	uint16_t paceBurst;        // bytes per burst (the token bucket's depth)
	uint16_t paceGap;          // extra quiet time after each burst, ms
	int8_t dcdPin;             // GPIO driving an emulated DCD, -1 = none
	uint8_t spare[15];
	};

static const uint32_t kMagic = 0x41544d31;   // 'ATM1'

void FactoryDefaults(Settings& aS);
// The pacing that suits a Psion 5mx at this line rate (see README)
void AutoPacing(uint32_t aBaud, uint32_t& aRate, uint16_t& aBurst, uint16_t& aGap);

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

// ----- the platform: serial line, TCP, WiFi, NVS, LED ----------------------
enum LedState { ELedNoWifi, ELedWifi, ELedConnected, ELedData, ELedConnecting };

class Hal
	{
public:
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
	// WiFi
	virtual bool WifiUp() = 0;
	virtual void WifiBegin(const char* aSsid, const char* aPass) = 0;
	virtual void WifiEnd() = 0;
	virtual void WifiInfo(char* aOut, size_t aMax) = 0; // "SSID x, IP a.b.c.d, RSSI -60"
	virtual int WifiScan(char aNames[][33], int aMax) { (void)aNames; (void)aMax; return 0; }
	// settings
	virtual bool LoadSettings(Settings& aS) = 0;
	virtual bool SaveSettings(const Settings& aS) = 0;
	// optional extras
	virtual void Dcd(bool aOn) { (void)aOn; }
	virtual void Led(LedState aState) { (void)aState; }
	virtual void ApplyPins(const Settings& aS) { (void)aS; }
	};

// ----- the modem ----------------------------------------------------------
class Modem
	{
public:
	Modem(Hal& aHal, uint8_t* aRing, size_t aRingSize);
	void Begin();                       // load settings, start WiFi
	void Loop();                        // call as often as possible
	// for the tests and the status LED
	bool Online() const { return iOnline; }
	bool Connected() const { return iConnected; }
	const Settings& Config() const { return iS; }
	size_t Buffered() const { return iRing.Count(); }
	uint32_t ToPsion() const { return iToPsion; }
	uint32_t ToServer() const { return iToServer; }

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
	TResult SetCommand(const char* aCmd, const char*& aP, bool& aHandled);
	void Info();
	void Result(TResult aR);
	void Say(const char* aText);
	void SayLine(const char* aText);
	void Send(const uint8_t* aData, size_t aLen);       // to the Psion, unpaced (command mode)
	void Hangup(bool aSayNoCarrier);
	void FlushUp();
	void PumpServer(uint32_t aNow);
	void PumpPsion(uint32_t aNow);
	void ApplyPacing();
	void UpdateDcd();
	void UpdateLed(uint32_t aNow);
	uint32_t GuardMs() const { return (uint32_t)iS.s12 * 20; }

	Hal& iHal;
	Ring iRing;
	Settings iS;
	Pacer iPacer;
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
	int iLed;                           // the LED state last shown
	uint8_t iTcpBuf[1460];
	uint8_t iUpBuf[256];                // Psion -> server, batched
	size_t iUpLen;
	};

} // namespace am

#endif
