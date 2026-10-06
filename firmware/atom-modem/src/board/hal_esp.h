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
	void WifiEnd() override { WiFi.disconnect(true); }
	void WifiInfo(char* aOut, size_t aMax) override;
	int WifiScan(char aNames[][33], int aMax) override;
	void ProbeInternet(const char* aHost, uint16_t aPort) override;
	int Internet() override { return WifiUp() ? iInternet : -1; }
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

	// the pins in use (after the settings, swaps and the board's defaults)
	int PinTx() const { return iTx; }
	int PinRx() const { return iRx; }
	int PinRts() const { return iRts; }
	int PinCts() const { return iCts; }
	bool Flow() const { return iFlow; }
	static void NvsClear();

private:
	void StartSerial();
	static void ProbeTask(void* aSelf);

	volatile int iInternet;                   // the last Internet check: -1, 0, 1
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
	};

#endif
