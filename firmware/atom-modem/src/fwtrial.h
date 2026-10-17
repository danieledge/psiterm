// fwtrial.h - a new firmware on trial, and the way back to the old one.
// A firmware written over the web (/ota) goes into the spare app slot; the
// modem restarts on it. If it cannot start properly - it crashes, hangs, or
// never gets the network or the pages up - it is given three starts, then
// the modem goes back to the firmware it ran before, on its own, so a bad
// build sent from a phone never needs a USB cable to recover.
//
// Two layers do this:
//   - the bootloader (Arduino-ESP32's prebuilt one has
//     CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): a slot just written is booted
//     once as "pending"; if it restarts before the app has marked it valid
//     (a crash before setup(), in the core's start-up, a watchdog), the
//     bootloader marks it aborted and boots the other slot;
//   - this trial, from the start of setup(): the new firmware marks itself
//     valid at once (so the bootloader allows more than one start) and
//     counts its own starts in NVS. A start that reaches "healthy" (running
//     for 30 s with the uplink up, or a web page served) ends the trial. A
//     start that does not (a crash, a watchdog, a brownout, the power
//     pulled, 3 minutes without the network, the main loop stuck for a
//     minute) is counted; at the fourth start the firmware marks itself
//     invalid and the bootloader boots the previous one.
// The previous firmware then shows "Running the previous firmware: the
// update failed to start 3 times" (the web System page, ATI, AT$STAT?'s
// FW=PREVIOUS) until another firmware is written over the failed one.
//
// The cases: the first USB flash (no otadata entry, nothing on trial, and
// nothing to go back to); a USB flash over a slot that was on trial (its
// image differs: the record is for another image and is dropped); an older
// build sent on purpose (it is "new" to the bootloader and goes through the
// same trial; one without this code marks itself valid in the core's
// start-up, as every build did before); no previous firmware to go back to
// (the trial is given up, the firmware keeps running, and says so).
//
// This file is the part with no ESP-IDF calls (the host tests run it with a
// model of the bootloader); board/fwsafe.cpp is the glue.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_FWTRIAL_H
#define ATOM_FWTRIAL_H

#include <stdint.h>
#include <stddef.h>

namespace am {

// the otadata state of a slot, as esp_ota_get_state_partition gives it
enum TFwSlotState { EFwStNone = 0, EFwStNew, EFwStPending, EFwStValid, EFwStInvalid, EFwStAborted };

// what the pages show
enum TFwShow { EFwShowOk = 0, EFwShowTrial, EFwShowPrevious };

static const int kFwShaLen = 8;            // the first bytes of the image's ELF SHA-256 name it

// The board's side: the OTA calls and two small NVS records
class FwOta
	{
public:
	virtual ~FwOta() {}
	virtual int RunningSlot() = 0;                     // 0 or 1
	virtual int State(int aSlot) = 0;                  // TFwSlotState
	virtual bool Sha(int aSlot, uint8_t aOut[kFwShaLen]) = 0;   // false: no image there
	virtual void MarkValid() = 0;                      // the running slot
	virtual bool CanRollBack() = 0;                    // esp_ota_check_rollback_is_possible
	virtual void MarkInvalidAndReboot() = 0;           // returns only if it could not
	virtual bool BootOther() = 0;                      // esp_ota_set_boot_partition(other): false if not an image
	virtual void Restart() = 0;
	virtual int LastInvalidSlot() = 0;                 // -1 none (esp_ota_get_last_invalid_partition)
	// the records (false: none saved)
	virtual bool Load(const char* aKey, void* aRec, size_t aLen) = 0;
	virtual void Save(const char* aKey, const void* aRec, size_t aLen) = 0;
	virtual void Log(const char* aLine) = 0;
	};

// the trial, kept in NVS ("fwtrial"): which image, and how many starts so far
struct FwTrialRec
	{
	uint32_t magic;            // kFwTrialMagic
	uint8_t slot;
	uint8_t attempts;          // starts of this image not yet healthy (0: no trial)
	uint8_t sha[kFwShaLen];
	uint8_t spare[2];
	};
// the outcome of a trial that failed ("fwnote"): the image given up on
struct FwNoteRec
	{
	uint32_t magic;            // kFwNoteMagic
	uint8_t slot;              // where it is (the slot not running now)
	uint8_t attempts;          // how many starts it had
	uint8_t sha[kFwShaLen];
	uint8_t spare[2];
	};
static const uint32_t kFwTrialMagic = 0x46575431;   // 'FWT1'
static const uint32_t kFwNoteMagic = 0x46574e31;    // 'FWN1'

class FwTrial
	{
public:
	static const int kMaxAttempts = 3;
	static const uint32_t kHealthyAfterMs = 30 * 1000;     // running this long ...
	static const uint32_t kDeadlineMs = 3 * 60 * 1000;     // ... and healthy by then, or the start failed
	static const uint32_t kStuckMs = 60 * 1000;            // the main loop not seen this long: the start failed

	FwTrial() : iTrial(false), iAttempt(0), iShow(EFwShowOk), iNoteAttempts(0), iGaveUp(false) {}
	// at the start of setup(). May not return (the way back to the old firmware)
	void Boot(FwOta& aOta);
	// once a pass of loop(): aUplink the network is up, aWeb a page was
	// served, aApOnly the access point is up and no network is set (then the
	// access point is the network). True once, when the trial ends well
	bool Tick(FwOta& aOta, uint32_t aNowMs, bool aUplink, bool aWeb, bool aApOnly);
	// the trial's last word: true when a start has failed by the clock
	// (the board then restarts: the start is counted already)
	bool Overdue(uint32_t aNowMs) const { return iTrial && aNowMs >= kDeadlineMs; }
	bool OnTrial() const { return iTrial; }
	int Attempt() const { return iAttempt; }
	int Show() const { return iShow; }                 // TFwShow
	int NoteAttempts() const { return iNoteAttempts; }
	bool GaveUp() const { return iGaveUp; }            // failed, with nothing to go back to
	// a line for the pages and ATI ("" when all is well)
	void Text(char* aOut, size_t aMax) const;

private:
	void Note(FwOta& aOta, int aRunning);
	bool iTrial;
	int iAttempt;
	int iShow;
	int iNoteAttempts;
	bool iGaveUp;
	};

} // namespace am

#endif
