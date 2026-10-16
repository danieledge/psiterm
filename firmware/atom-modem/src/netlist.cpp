// netlist.cpp - see netlist.h. MIT licence (see LICENSE at the top of the repository).
#include "netlist.h"
#include "modem.h"               // WifiNet
#include <string.h>

namespace am {

static void Wipe(void* aP, size_t aN)
	{
	volatile uint8_t* p = (volatile uint8_t*)aP;
	for (size_t i = 0; i < aN; i++)
		p[i] = 0;
	}

void NetListInit(NetList& aL)
	{
	Wipe(&aL, sizeof(aL));
	aL.magic = kNetMagic;
	aL.version = kNetVersion;
	aL.length = (uint16_t)sizeof(NetList);
	}

bool NetListAccept(NetList& aL, size_t aLength)
	{
	if (aLength < 16 || aL.magic != kNetMagic || aL.version == 0)
		return false;
	if (aLength < sizeof(NetList))
		{
		// a shorter (future-older) record: what is missing is empty
		NetList f;
		NetListInit(f);
		memcpy(&f, &aL, aLength);
		aL = f;
		}
	aL.version = kNetVersion;
	aL.length = (uint16_t)sizeof(NetList);
	if (aL.count > kNetMax)
		aL.count = kNetMax;
	int out = 0;
	for (int i = 0; i < aL.count; i++)
		{
		SavedNet& n = aL.net[i];
		n.ssid[sizeof(n.ssid) - 1] = 0;
		n.pass[sizeof(n.pass) - 1] = 0;
		if (!n.ssid[0])
			continue;                        // (an empty slot: closed up)
		if (out != i)
			aL.net[out] = n;
		out++;
		}
	for (int i = out; i < kNetMax; i++)
		Wipe(&aL.net[i], sizeof(aL.net[i]));
	aL.count = (uint8_t)out;
	return true;
	}

int NetFind(const NetList& aL, const char* aSsid)
	{
	if (!aSsid || !aSsid[0])
		return -1;
	for (int i = 0; i < aL.count && i < kNetMax; i++)
		if (strcmp(aL.net[i].ssid, aSsid) == 0)
			return i;
	return -1;
	}

int NetUpsert(NetList& aL, const char* aSsid, const char* aPass, int aKeep, bool* aEvicted)
	{
	if (aEvicted)
		*aEvicted = false;
	if (!aSsid || !aSsid[0] || strlen(aSsid) > 32)
		return -1;
	if (!aPass)
		aPass = "";
	if (strlen(aPass) > 64)
		return -1;
	int i = NetFind(aL, aSsid);
	if (i < 0)
		{
		if (aL.count >= kNetMax)
			{
			// full: the last entry that is not the one to keep makes room
			int victim = kNetMax - 1;
			if (victim == aKeep)
				victim--;
			NetForget(aL, victim);
			if (aEvicted)
				*aEvicted = true;
			}
		i = aL.count++;
		Wipe(&aL.net[i], sizeof(aL.net[i]));
		strncpy(aL.net[i].ssid, aSsid, sizeof(aL.net[i].ssid) - 1);
		}
	Wipe(aL.net[i].pass, sizeof(aL.net[i].pass));
	strncpy(aL.net[i].pass, aPass, sizeof(aL.net[i].pass) - 1);
	return i;
	}

bool NetForget(NetList& aL, int aIndex)
	{
	if (aIndex < 0 || aIndex >= aL.count)
		return false;
	for (int i = aIndex; i + 1 < aL.count; i++)
		aL.net[i] = aL.net[i + 1];
	aL.count--;
	Wipe(&aL.net[aL.count], sizeof(aL.net[aL.count]));
	return true;
	}

bool NetPrefer(NetList& aL, int aIndex)
	{
	if (aIndex < 0 || aIndex >= aL.count)
		return false;
	SavedNet keep = aL.net[aIndex];
	for (int i = aIndex; i > 0; i--)
		aL.net[i] = aL.net[i - 1];
	aL.net[0] = keep;
	Wipe(&keep, sizeof(keep));
	return true;
	}

// the strongest signal the scan saw for a name (a name can be seen more than
// once: several access points), or 0 if not seen
static int Seen(const WifiNet* aScan, int aCount, const char* aSsid)
	{
	int best = 0;
	for (int k = 0; k < aCount; k++)
		if (aScan[k].ssid[0] && strcmp(aScan[k].ssid, aSsid) == 0)
			{
			int r = aScan[k].rssi ? aScan[k].rssi : -100;
			if (!best || r > best)
				best = r;
			}
	return best;
	}

int NetChoose(const NetList& aL, const WifiNet* aScan, int aCount, const char* aPinned, uint32_t aSkip, int* aRssi)
	{
	if (aPinned && aPinned[0])
		{
		int p = NetFind(aL, aPinned);
		if (p >= 0 && !(aSkip & (1u << p)))
			{
			int r = Seen(aScan, aCount, aPinned);
			if (r)
				{
				if (aRssi) *aRssi = r;
				return p;
				}
			}
		}
	int best = -1, bestRssi = 0;
	for (int i = 0; i < aL.count && i < kNetMax; i++)
		{
		if (aSkip & (1u << i))
			continue;
		int r = Seen(aScan, aCount, aL.net[i].ssid);
		if (r && (best < 0 || r > bestRssi))
			{
			best = i;
			bestRssi = r;
			}
		}
	if (best >= 0 && aRssi)
		*aRssi = bestRssi;
	return best;
	}

int NetNext(const NetList& aL, int aCurrent, const WifiNet* aScan, int aCount)
	{
	if (aL.count == 0)
		return -1;
	int start = aCurrent >= 0 && aCurrent < aL.count ? aCurrent : -1;
	bool anySeen = false;
	for (int i = 0; i < aL.count; i++)
		if (Seen(aScan, aCount, aL.net[i].ssid))
			anySeen = true;
	for (int k = 1; k <= aL.count; k++)
		{
		int i = (start + k + aL.count) % aL.count;
		if (i == start)
			break;
		if (!anySeen || Seen(aScan, aCount, aL.net[i].ssid))
			return i;
		}
	return -1;
	}

} // namespace am
