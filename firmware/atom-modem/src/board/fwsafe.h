// fwsafe.h - the ESP-IDF side of the firmware trial (../fwtrial.h): the OTA
// calls, the two NVS records, and a small watchdog for the trial. Begin() is
// the first thing setup() does; Tick() runs in loop(). While a new firmware
// is on trial an esp_timer checks once a second that loop() is still
// running, and restarts the modem if it has not been seen for a minute (a
// start that hung: counted as failed). The trial never keeps the modem
// from working: it only watches, and restarts a start that is not healthy.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_FWSAFE_H
#define ATOM_FWSAFE_H

#include <Arduino.h>
#include "../fwtrial.h"
#include "../modem.h"

namespace am {

class FwSafe
	{
public:
	FwSafe() : iWeb(false), iTimer(0) {}
	void Begin(Hal& aHal);              // the first thing in setup(): may not return (aHal: its log)
	// from loop(): aUplink the uplink is up, aApOnly only the access point
	// (no network set). A page served counts too (WebServed)
	void Tick(bool aUplink, bool aApOnly);
	void WebServed() { iWeb = true; }   // (webui.cpp: a page that passed the password)
	int Show() const { return iTrial.Show(); }
	bool OnTrial() const { return iTrial.OnTrial(); }
	void Text(char* aOut, size_t aMax) const { iTrial.Text(aOut, aMax); }
	// the slots, for the System page: "slot 1 (running), slot 0: valid"
	void Slots(char* aOut, size_t aMax) const;

private:
	static void Watch(void* aSelf);
	FwTrial iTrial;
	volatile bool iWeb;
	void* iTimer;                       // esp_timer_handle_t, while on trial
	};

extern FwSafe gFwSafe;

} // namespace am

#endif
