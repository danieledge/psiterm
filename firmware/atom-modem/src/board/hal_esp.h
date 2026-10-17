// hal_esp.h - the Arduino/ESP32 side of the Atom modem: the UART to the
// Psion (through a transceiver; RTS/CTS when fitted), WiFi station and
// access point, TCP and TLS, NVS, the LED, the log, the USB host (ESP32-S3)
// and the web pages. The modem itself is the portable code in ../modem.cpp.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_HAL_ESP_H
#define ATOM_HAL_ESP_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "../modem.h"

namespace am { class WebUi; class UsbNet; }

class AtomHal : public am::Hal
	{
public:
	AtomHal();
	void BeginTls();
	void SetWeb(am::WebUi* aWeb) { iWeb = aWeb; }
	void SetUsb(am::UsbNet* aUsb) { iUsb = aUsb; }
	void Running() { iRunning = true; }          // setup() is over: USB mode changes wait for a restart
	bool UsbHostStarted() const { return iUsbStarted; }

	uint32_t Millis() override { return millis(); }
	uint32_t Micros() override { return micros(); }
	int SerialRead() override;
	size_t SerialWritable() override;
	size_t SerialWrite(const uint8_t* aData, size_t aLen) override;
	void SerialBaud(uint32_t aBaud) override;
	bool TcpConnect(const char* aHost, uint16_t aPort) override;
	bool UpConnect(const char* aHost, uint16_t aPort, bool aTls, char* aWhy, size_t aWhyMax) override;
	bool TcpOpen() override { return iCur->connected(); }
	size_t TcpAvailable() override { int n = iCur->available(); return n > 0 ? n : 0; }
	size_t TcpRead(uint8_t* aBuf, size_t aMax) override { int n = iCur->read(aBuf, aMax); return n > 0 ? n : 0; }
	size_t TcpWrite(const uint8_t* aData, size_t aLen) override { return iCur->write(aData, aLen); }
	void TcpClose() override { iCur->stop(); }
	void Idle() override { delay(1); }
	uint32_t UplinkDns() override { return (uint32_t)WiFi.dnsIP(); }   // (0.0.0.0 = none: PPP falls back)
	const char* BoardName() override;
	void ApPass(char* aOut, size_t aMax) override;
	void MemInfo(char* aOut, size_t aMax) override;
	void Log(const char* aLine) override;
	bool WifiUp() override { return WiFi.status() == WL_CONNECTED; }
	void WifiBegin(const char* aSsid, const char* aPass) override;
	void WifiHardReset() override;
	void RadioShared(bool aShared) override;
	void WifiEnd() override { WiFi.disconnect(true); }
	void WifiInfo(char* aOut, size_t aMax) override;
	int WifiScan(char aNames[][33], int aMax) override;
	int WifiScanFull(am::WifiNet* aOut, int aMax) override;
	bool WifiLink(char* aSsid, size_t aSsidMax, char* aIp, size_t aIpMax, int& aRssi) override;
	int WifiFail() override { return iWifiFail; }
	void ProbeInternet(const char* aHost, uint16_t aPort) override;
	int Internet() override { return WifiUp() ? iInternet : -1; }
	int DnsOk() override { return WifiUp() ? iDnsOk : -1; }
	bool UplinkAddr(uint32_t& aIp, uint32_t& aMask) override;
	void WgApply(const am::WgConfig& aConfig, bool aWant) override;
	void WgInfo(am::WgStatus& aOut) override;
	bool DnsRelayStart() override;
	void DnsRelayStop() override;
	void DnsRelayUpstream(uint32_t aServer) override;
	void PppUplink(bool aUp) override;
	bool LoadNets(am::NetList& aL, size_t& aLength) override;
	bool SaveNets(const am::NetList& aL) override;
	bool LoadWg(am::WgConfig& aC, size_t& aLength) override;
	bool SaveWg(const am::WgConfig& aC) override;
	int UsbState(char* aDetail, size_t aMax) override;
	bool LoadSettings(am::Settings& aS) override;
	bool SaveSettings(const am::Settings& aS) override;
	bool LoadSettings2(am::Settings2& aS, size_t& aLength) override;
	bool SaveSettings2(const am::Settings2& aS) override;
	void FactoryReset() override;
	void Restart() override;
	void Dcd(bool aOn) override;
	void Led(am::LedState aState) override;               // (also the status screen's bar: one source)
	void ApplyPins(const am::Settings& aS, const am::Settings2& aS2) override;
	void ApplyWeb(const am::Settings2& aS2) override;
	void ApplyUplink(const am::Settings2& aS2) override;
	void ApInfo(char* aOut, size_t aMax) override;
	void WebInfo(char* aOut, size_t aMax) override;
	int FirmwareInfo(char* aOut, size_t aMax) override;
	void Random(uint8_t* aOut, size_t aLen) override;
	void* BigAlloc(size_t aLen) override;
	bool BigRoom(size_t aLen) override;

	// the pins in use (after the settings, swaps and the board's defaults)
	int PinTx() const { return iTx; }
	int PinRx() const { return iRx; }
	int PinRts() const { return iRts; }
	int PinCts() const { return iCts; }
	bool Flow() const { return iFlow; }
	static void NvsClear();
	// from loop(): an extension module's light on the LED (ext.h: ExtLedPick),
	// or the modem's own state back. aBusy: a firmware update is being written
	void LedTick(const am::Modem& aModem, bool aBusy, uint32_t aNowMs);

private:
	void StartSerial();
	static void ProbeTask(void* aSelf);

	static void WifiEvent(arduino_event_id_t aEvent, arduino_event_info_t aInfo);
	volatile int iInternet;                   // the last Internet check: -1, 0, 1
	volatile int iDnsOk;                      // ... its DNS lookup: -1, 0, 1
	volatile int iWifiFail;                   // am::TWifiFail since the last WifiBegin
	bool iWifiEvents;                         // WifiEvent is registered
	bool iRadioShared = false;                // Bluetooth is on too (RadioShared): modem sleep stays on
	volatile bool iProbing;
	char iProbeHost[40];
	uint16_t iProbePort;
	WiFiClient iPlain;
	WiFiClientSecure iTls;
	WiFiClient* iCur;                         // the one in use
	am::WebUi* iWeb;
	am::UsbNet* iUsb;
	uint32_t iBaud;
	int iTx, iRx, iRts, iCts, iDcdPin;
	bool iFlow;
	bool iStarted;
	bool iRunning;
	bool iUsbStarted;
	bool iUsbWanted;
	int iLedState;                            // the modem's LED state, last set (-1 none yet)
	bool iExtLed;                             // a module's light is showing in its place
	uint32_t iExtLedMs;                       // when it was last written
	};

#endif
