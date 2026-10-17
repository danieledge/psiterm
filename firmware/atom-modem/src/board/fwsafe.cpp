// fwsafe.cpp - see fwsafe.h. MIT licence (see LICENSE at the top of the repository).
#include "fwsafe.h"
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <esp_system.h>
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include <esp_app_desc.h>
#endif

// The core's start-up (initArduino) marks a "pending" firmware valid unless
// this says not yet: the trial decides instead (FwTrial::Boot, from setup())
extern "C" bool verifyRollbackLater() { return true; }

namespace am {

FwSafe gFwSafe;

// the loop's last pass, for the watchdog (esp_timer's task reads it)
static volatile uint32_t gLoopSeenMs = 0;

class EspOta : public FwOta
	{
public:
	const esp_partition_t* Slot(int aSlot)
		{
		return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
			(esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + aSlot), NULL);
		}
	static int SlotOf(const esp_partition_t* aP)
		{
		if (!aP || aP->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN || aP->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX)
			return -1;
		return (int)(aP->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
		}
	int RunningSlot() override
		{
		int s = SlotOf(esp_ota_get_running_partition());
		return s < 0 ? 0 : s;
		}
	int State(int aSlot) override
		{
		const esp_partition_t* p = Slot(aSlot);
		esp_ota_img_states_t st;
		if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK)
			return EFwStNone;
		switch (st)
			{
		case ESP_OTA_IMG_NEW: return EFwStNew;
		case ESP_OTA_IMG_PENDING_VERIFY: return EFwStPending;
		case ESP_OTA_IMG_VALID: return EFwStValid;
		case ESP_OTA_IMG_INVALID: return EFwStInvalid;
		case ESP_OTA_IMG_ABORTED: return EFwStAborted;
		default: return EFwStValid;          // (UNDEFINED: boots without limits)
			}
		}
	bool Sha(int aSlot, uint8_t aOut[kFwShaLen]) override
		{
		const esp_partition_t* p = Slot(aSlot);
		if (!p)
			return false;
		// the ELF SHA-256 in the image's description (the build puts it there:
		// elf2image --elf-sha256-offset); the image's own hash if it is blank
		esp_app_desc_t d;
		if (esp_ota_get_partition_description(p, &d) != ESP_OK)
			return false;
		bool blank = true;
		for (int i = 0; i < kFwShaLen; i++)
			if (d.app_elf_sha256[i])
				blank = false;
		if (!blank)
			{
			memcpy(aOut, d.app_elf_sha256, kFwShaLen);
			return true;
			}
		uint8_t h[32];
		if (esp_partition_get_sha256(p, h) != ESP_OK)
			return false;
		memcpy(aOut, h, kFwShaLen);
		return true;
		}
	void MarkValid() override { esp_ota_mark_app_valid_cancel_rollback(); }
	bool CanRollBack() override { return esp_ota_check_rollback_is_possible(); }
	void MarkInvalidAndReboot() override
		{
		Serial.flush();
		esp_ota_mark_app_invalid_rollback_and_reboot();   // (returns only on an error)
		}
	bool BootOther() override
		{
		const esp_partition_t* p = esp_ota_get_next_update_partition(NULL);
		return p && esp_ota_set_boot_partition(p) == ESP_OK;   // (it checks the image first)
		}
	void Restart() override { Serial.flush(); delay(100); esp_restart(); }
	int LastInvalidSlot() override { return SlotOf(esp_ota_get_last_invalid_partition()); }
	bool Load(const char* aKey, void* aRec, size_t aLen) override
		{
		Preferences p;
		if (!p.begin("fwtrial", true))
			return false;
		size_t n = p.getBytesLength(aKey) == aLen ? p.getBytes(aKey, aRec, aLen) : 0;
		p.end();
		return n == aLen;
		}
	void Save(const char* aKey, const void* aRec, size_t aLen) override
		{
		Preferences p;
		if (!p.begin("fwtrial", false))
			return;
		p.putBytes(aKey, aRec, aLen);
		p.end();
		}
	void Log(const char* aLine) override
		{
		if (iHal)
			iHal->Log(aLine);                 // (the log ring and the USB console)
		else
			Serial.println(aLine);
		}
	Hal* iHal = 0;
	};

static EspOta gOta;

void FwSafe::Begin(Hal& aHal)
	{
	gOta.iHal = &aHal;
	gLoopSeenMs = millis();
	esp_reset_reason_t why = esp_reset_reason();
	iTrial.Boot(gOta);
	if (iTrial.OnTrial())
		{
		static const char* const kWhy[] = { "unknown", "power-on", "external", "software", "panic", "interrupt watchdog",
			"task watchdog", "watchdog", "deep sleep", "brownout", "SDIO" };
		char m[80];
		snprintf(m, sizeof(m), "Firmware: the last restart was %s", (int)why < 11 ? kWhy[(int)why] : "other");
		gOta.Log(m);
		// the watchdog for a start that hangs (loop() never seen again)
		esp_timer_create_args_t a = {};
		a.callback = &FwSafe::Watch;
		a.arg = this;
		a.name = "fwtrial";
		esp_timer_handle_t t = 0;
		if (esp_timer_create(&a, &t) == ESP_OK && esp_timer_start_periodic(t, 1000000) == ESP_OK)
			iTimer = t;
		}
	}

void FwSafe::Watch(void* aSelf)
	{
	FwSafe* self = (FwSafe*)aSelf;
	if (!self->iTrial.OnTrial())
		return;
	uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
	if (now - gLoopSeenMs >= FwTrial::kStuckMs)
		esp_restart();                        // a start that hung: counted already (Boot)
	}

void FwSafe::Tick(bool aUplink, bool aApOnly)
	{
	uint32_t now = millis();
	gLoopSeenMs = now;
	if (!iTrial.OnTrial())
		return;
	if (iTrial.Tick(gOta, now, aUplink, iWeb, aApOnly))
		{
		if (iTimer)
			{
			esp_timer_stop((esp_timer_handle_t)iTimer);
			esp_timer_delete((esp_timer_handle_t)iTimer);
			iTimer = 0;
			}
		return;
		}
	if (iTrial.Overdue(now))
		{
		char m[96];
		snprintf(m, sizeof(m), "Firmware: not healthy after %u minutes: restarting (start %d of %d)",
			(unsigned)(FwTrial::kDeadlineMs / 60000), iTrial.Attempt(), FwTrial::kMaxAttempts);
		gOta.Log(m);
		gOta.Restart();
		}
	}

void FwSafe::Slots(char* aOut, size_t aMax) const
	{
	static const char* const kNames[] = { "no record", "new", "being tried", "valid", "invalid", "refused at its first start" };
	int run = gOta.RunningSlot();
	int other = run ? 0 : 1;
	int st = gOta.State(other);
	esp_app_desc_t d;
	const esp_partition_t* p = gOta.Slot(other);
	bool img = p && esp_ota_get_partition_description(p, &d) == ESP_OK;
	snprintf(aOut, aMax, "running from slot %d; slot %d: %s%s", run, other,
		img ? kNames[st >= 0 && st <= 5 ? st : 0] : "empty", "");
	}

} // namespace am
