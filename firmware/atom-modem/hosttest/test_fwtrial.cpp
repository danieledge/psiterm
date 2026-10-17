// test_fwtrial.cpp - the firmware trial (src/fwtrial.h) against a model of
// ESP-IDF's two-slot OTA scheme as the prebuilt bootloader runs it (with
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): otadata's two entries with their
// sequence numbers and states, NEW -> PENDING_VERIFY at the first boot,
// PENDING_VERIFY -> ABORTED at the next, the highest valid sequence wins,
// ota_0 when there is no entry at all. Then whole histories: a USB flash, a
// good update, one that crashes every start, one that crashes before
// setup(), one with a brownout, the first update after a USB flash, a USB
// flash over a slot on trial, an older build, nothing to go back to.
// MIT licence.
#include "../src/fwtrial.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <map>
#include <vector>

static int gChecks = 0, gFails = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

enum TState { SNew, SPending, SValid, SInvalid, SAborted, SUndefined };

struct Entry { uint32_t seq; TState state; bool crc; };

enum TBehaviour { BHealthy, BCrashAfterSetup, BCrashBeforeSetup, BBrownoutOnce };

struct Image
	{
	bool present = false;
	uint8_t sha[am::kFwShaLen] = {};
	bool trialCode = true;                     // built with fwtrial (else the core marks it valid at once)
	TBehaviour how = BHealthy;
	int starts = 0;
	};

// the chip: two app slots, otadata, NVS
struct Chip : public am::FwOta
	{
	Entry ota[2] = { { 0xffffffff, SUndefined, false }, { 0xffffffff, SUndefined, false } };
	Image slot[2];
	std::map<std::string, std::vector<uint8_t>> nvs;
	int running = 0;
	bool rebooted = false;                     // the app asked for a restart
	std::vector<std::string> log;

	static bool Usable(const Entry& e) { return e.crc && e.seq != 0xffffffff && e.state != SInvalid && e.state != SAborted; }
	int Active() const
		{
		bool a = Usable(ota[0]), b = Usable(ota[1]);
		if (a && b) return ota[0].seq >= ota[1].seq ? 0 : 1;
		return a ? 0 : b ? 1 : -1;
		}
	static int SlotOfSeq(uint32_t aSeq) { return (int)((aSeq - 1) % 2); }
	// the bootloader: which slot runs
	int Boot()
		{
		int a = Active();
		if (a >= 0 && ota[a].state == SPending)
			{
			ota[a].state = SAborted;              // it never confirmed itself
			a = Active();
			}
		if (a >= 0 && ota[a].state == SNew)
			ota[a].state = SPending;
		int s = a >= 0 ? SlotOfSeq(ota[a].seq) : 0;   // (no entry: ota_0)
		if (!slot[s].present)
			s = 1 - s;
		return s;
		}
	// esp_ota_set_boot_partition
	bool SetBoot(int aSlot)
		{
		if (!slot[aSlot].present)
			return false;
		int a = Active();
		uint32_t seq;
		int into;
		if (a < 0)
			{
			seq = (uint32_t)aSlot + 1;
			into = 0;
			}
		else
			{
			seq = ota[a].seq + 1;
			while (SlotOfSeq(seq) != aSlot) seq++;
			into = 1 - a;
			}
		ota[into] = { seq, SNew, true };
		return true;
		}
	void UsbFlash(int aSlot, const Image& aImg)
		{
		slot[aSlot] = aImg;
		slot[aSlot].present = true;
		ota[0] = ota[1] = { 0xffffffff, SUndefined, false };   // (boot_app0.bin: otadata blank)
		}
	// the web page's /ota: the slot not running, then the boot partition
	void Ota(const Image& aImg)
		{
		int s = 1 - running;
		slot[s] = aImg;
		slot[s].present = true;
		SetBoot(s);
		}
	// am::FwOta
	int RunningSlot() override { return running; }
	int State(int aSlot) override
		{
		int best = -1;
		for (int i = 0; i < 2; i++)
			if (ota[i].crc && ota[i].seq != 0xffffffff && SlotOfSeq(ota[i].seq) == aSlot && (best < 0 || ota[i].seq > ota[best].seq))
				best = i;
		if (best < 0) return am::EFwStNone;
		switch (ota[best].state)
			{
		case SNew: return am::EFwStNew;
		case SPending: return am::EFwStPending;
		case SInvalid: return am::EFwStInvalid;
		case SAborted: return am::EFwStAborted;
		default: return am::EFwStValid;
			}
		}
	bool Sha(int aSlot, uint8_t aOut[am::kFwShaLen]) override
		{
		if (!slot[aSlot].present) return false;
		memcpy(aOut, slot[aSlot].sha, am::kFwShaLen);
		return true;
		}
	void MarkValid() override { int a = Active(); if (a >= 0) ota[a].state = SValid; }
	bool CanRollBack() override
		{
		int a = Active();
		if (a < 0) return false;
		int o = 1 - a;
		return Usable(ota[o]) && slot[SlotOfSeq(ota[o].seq)].present;
		}
	void MarkInvalidAndReboot() override
		{
		if (!CanRollBack()) return;
		ota[Active()].state = SInvalid;
		rebooted = true;
		}
	bool BootOther() override { return SetBoot(1 - running); }
	void Restart() override { rebooted = true; }
	int LastInvalidSlot() override
		{
		for (int i = 0; i < 2; i++)
			if (ota[i].crc && ota[i].seq != 0xffffffff && (ota[i].state == SInvalid || ota[i].state == SAborted))
				return SlotOfSeq(ota[i].seq);
		return -1;
		}
	bool Load(const char* aKey, void* aRec, size_t aLen) override
		{
		auto it = nvs.find(aKey);
		if (it == nvs.end() || it->second.size() != aLen) return false;
		memcpy(aRec, it->second.data(), aLen);
		return true;
		}
	void Save(const char* aKey, const void* aRec, size_t aLen) override
		{
		nvs[aKey].assign((const uint8_t*)aRec, (const uint8_t*)aRec + aLen);
		}
	void Log(const char* aLine) override { log.push_back(aLine); }

	// one power-on or restart: what happened (the trial as it ended this start)
	am::FwTrial Start()
		{
		running = Boot();
		rebooted = false;
		Image& img = slot[running];
		img.starts++;
		am::FwTrial t;
		if (img.how == BCrashBeforeSetup)
			return t;                             // (a crash in the core's start-up: the app never ran)
		if (!img.trialCode)
			{
			// an older build: initArduino() marks a pending image valid at once
			if (State(running) == am::EFwStPending) MarkValid();
			return t;
			}
		t.Boot(*this);
		if (rebooted)
			return t;
		if (img.how == BCrashAfterSetup)
			return t;                             // (a crash in loop(), a hang, no network: the start failed)
		if (img.how == BBrownoutOnce && img.starts == 1)
			return t;
		// healthy: the uplink up after 30 s
		t.Tick(*this, 1000, true, false, false);  // (too soon: not yet)
		t.Tick(*this, am::FwTrial::kHealthyAfterMs, true, false, false);
		return t;
		}
	};

static Image Img(uint8_t aTag, TBehaviour aHow = BHealthy, bool aTrialCode = true)
	{
	Image i;
	i.present = true;
	memset(i.sha, aTag, sizeof(i.sha));
	i.how = aHow;
	i.trialCode = aTrialCode;
	return i;
	}

static void TestGoodUpdate()
	{
	Chip c;
	c.UsbFlash(0, Img(0xA0));
	am::FwTrial t = c.Start();
	CHECK(c.running == 0 && !t.OnTrial() && t.Show() == am::EFwShowOk);   // a USB flash: nothing on trial
	c.Ota(Img(0xB0));
	t = c.Start();
	CHECK(c.running == 1);
	CHECK(!t.OnTrial() && t.Show() == am::EFwShowOk);   // on trial, then healthy: kept
	CHECK(c.State(1) == am::EFwStValid);
	t = c.Start();
	CHECK(c.running == 1 && !t.OnTrial() && t.Attempt() == 0);   // and not on trial again
	char m[160];
	t.Text(m, sizeof(m));
	CHECK(m[0] == 0);
	}

static void TestCrashingUpdate()
	{
	Chip c;
	c.UsbFlash(0, Img(0xA0));
	c.Start();
	c.Ota(Img(0xB0));
	c.Start();                                    // B is good, in slot 1
	c.Ota(Img(0xC0, BCrashAfterSetup));           // C goes to slot 0
	am::FwTrial t;
	t = c.Start(); CHECK(c.running == 0 && t.OnTrial() && t.Attempt() == 1 && t.Show() == am::EFwShowTrial);
	char m[160];
	t.Text(m, sizeof(m));
	CHECK(strstr(m, "start 1 of 3") != 0);
	t = c.Start(); CHECK(c.running == 0 && t.Attempt() == 2);
	t = c.Start(); CHECK(c.running == 0 && t.Attempt() == 3);
	t = c.Start(); CHECK(c.rebooted);             // the fourth start: given up, back to B
	CHECK(c.State(0) == am::EFwStInvalid);
	t = c.Start(); CHECK(c.running == 1);
	CHECK(t.Show() == am::EFwShowPrevious && t.NoteAttempts() == 3);
	t.Text(m, sizeof(m));
	CHECK(strcmp(m, "Running the previous firmware: the update failed to start 3 times") == 0);
	CHECK(c.slot[0].starts == 4);
	t = c.Start(); CHECK(c.running == 1 && t.Show() == am::EFwShowPrevious);   // (still said after a restart)
	// a good firmware over the failed one: the note goes
	c.Ota(Img(0xD0));
	t = c.Start(); CHECK(c.running == 0 && t.Show() == am::EFwShowOk && !t.OnTrial());
	t = c.Start(); CHECK(c.running == 0 && t.Show() == am::EFwShowOk);
	}

static void TestCrashBeforeSetup()
	{
	Chip c;
	c.UsbFlash(0, Img(0xA0));
	c.Start();
	c.Ota(Img(0xB0));
	c.Start();
	c.Ota(Img(0xC0, BCrashBeforeSetup));
	am::FwTrial t = c.Start();
	CHECK(c.running == 0);                        // C ran (and crashed in the core's start-up)
	t = c.Start();
	CHECK(c.running == 1);                        // the bootloader refused it at once
	CHECK(t.Show() == am::EFwShowPrevious && t.NoteAttempts() == 1);
	char m[160];
	t.Text(m, sizeof(m));
	CHECK(strcmp(m, "Running the previous firmware: the update failed to start") == 0);
	CHECK(c.slot[0].starts == 1);
	}

static void TestBrownout()
	{
	Chip c;
	c.UsbFlash(0, Img(0xA0));
	c.Start();
	c.Ota(Img(0xB0, BBrownoutOnce));
	am::FwTrial t = c.Start();
	CHECK(c.running == 1 && t.OnTrial());         // the first start lost its power
	t = c.Start();
	CHECK(c.running == 1 && !t.OnTrial() && t.Show() == am::EFwShowOk);   // the second was fine: kept
	t = c.Start();
	CHECK(c.running == 1 && !t.OnTrial());
	}

static void TestFirstUpdateAfterUsb()
	{
	// the USB flash leaves otadata blank: the only entry is the update's, so
	// esp_ota_check_rollback_is_possible() says no; the way back is to point
	// the bootloader at the USB image's slot
	Chip c;
	c.UsbFlash(0, Img(0xA0));
	c.Start();
	c.Ota(Img(0xC0, BCrashAfterSetup));
	am::FwTrial t;
	for (int i = 0; i < 3; i++)
		t = c.Start();
	CHECK(c.running == 1 && t.Attempt() == 3);
	t = c.Start();
	CHECK(c.rebooted);
	t = c.Start();
	CHECK(c.running == 0);                        // A again
	CHECK(t.Show() == am::EFwShowPrevious && t.NoteAttempts() == 3);
	t = c.Start();
	CHECK(c.running == 0 && t.Show() == am::EFwShowPrevious && !t.OnTrial());
	}

static void TestUsbOverTrial()
	{
	Chip c;
	c.UsbFlash(0, Img(0xA0));
	c.Start();
	c.Ota(Img(0xC0, BCrashAfterSetup));
	c.Start();
	c.Start();                                    // C on trial, start 2
	// a USB flash (esptool writes ota_0 and a blank otadata): a crashing build
	// flashed by hand is never on trial, and an old trial record cannot apply to it
	c.UsbFlash(0, Img(0xE0, BCrashAfterSetup));
	am::FwTrial t;
	for (int i = 0; i < 6; i++)
		{
		t = c.Start();
		CHECK(c.running == 0 && !c.rebooted && !t.OnTrial());
		}
	}

static void TestOlderBuild()
	{
	Chip c;
	c.UsbFlash(0, Img(0xA0));
	c.Start();
	c.Ota(Img(0xB0, BHealthy, false));            // a build from before the trial
	am::FwTrial t = c.Start();
	CHECK(c.running == 1 && c.State(1) == am::EFwStValid);   // marked valid by the core, as before
	c.Ota(Img(0xC0));                             // and back to a new one: a trial as usual
	t = c.Start();
	CHECK(c.running == 0 && !t.OnTrial() && c.State(0) == am::EFwStValid);
	}

static void TestNothingToGoBackTo()
	{
	Chip c;
	c.UsbFlash(0, Img(0xA0));
	c.Start();
	c.Ota(Img(0xC0, BCrashAfterSetup));
	c.slot[0].present = false;                    // (the other slot erased)
	am::FwTrial t;
	for (int i = 0; i < 4; i++)
		t = c.Start();
	CHECK(c.running == 1 && !c.rebooted && t.GaveUp() && !t.OnTrial());
	char m[160];
	t.Text(m, sizeof(m));
	CHECK(strstr(m, "no previous firmware") != 0);
	t = c.Start();
	CHECK(c.running == 1 && !t.OnTrial());        // and it keeps running, no longer on trial
	}

static void TestHealthRules()
	{
	Chip c;
	c.UsbFlash(0, Img(0xA0));
	c.Start();
	c.slot[1] = Img(0xB0);
	c.SetBoot(1);
	c.running = c.Boot();
	am::FwTrial t;
	t.Boot(c);
	CHECK(t.OnTrial());
	CHECK(!t.Tick(c, 10000, true, false, false));         // the uplink, but too soon
	CHECK(!t.Tick(c, 40000, false, false, false));        // long enough, but no network
	CHECK(!t.Overdue(am::FwTrial::kDeadlineMs - 1));
	CHECK(t.Overdue(am::FwTrial::kDeadlineMs));           // the board restarts then
	CHECK(t.Tick(c, 5000, false, true, false));           // a page served: at once
	CHECK(!t.OnTrial() && !t.Overdue(am::FwTrial::kDeadlineMs));
	// the access point only (no network set) counts after 30 s
	c.Ota(Img(0xC0));
	c.running = c.Boot();
	am::FwTrial u;
	u.Boot(c);
	CHECK(u.OnTrial() && !u.Tick(c, 1000, false, false, true) && u.Tick(c, 30000, false, false, true));
	}

int main()
	{
	TestGoodUpdate();
	TestCrashingUpdate();
	TestCrashBeforeSetup();
	TestBrownout();
	TestFirstUpdateAfterUsb();
	TestUsbOverTrial();
	TestOlderBuild();
	TestNothingToGoBackTo();
	TestHealthRules();
	printf("test_fwtrial: %d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
	}
