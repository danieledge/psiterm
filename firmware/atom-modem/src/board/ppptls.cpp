// ppptls.cpp - see ppptls.h. MIT licence (see LICENSE at the top of the repository).
#include "ppptls.h"
#include "../ppp.h"
#include "../cabundle.h"
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <new>

namespace am {

static const size_t kWritePerPass = 4096;   // most bytes moved per direction per Service() pass: one slot must not hog the loop
static const size_t kUpChunk = 1024;         // most bytes read from the Psion and pushed upstream per pass (bounds a TLS write)
// the upstream TLS session wants this much free *internal* heap before we open
// it (mbedTLS record buffers are not in PSRAM): refuse rather than drag WiFi's
// own allocations down and drop the link
static const size_t kUpMinHeap = 30 * 1024;

// is the socket writable right now, without waiting? (so a push upstream never
// blocks the loop that also drains PPP)
static bool Writable(int aFd)
	{
	if (aFd < 0)
		return false;
	fd_set wf;
	FD_ZERO(&wf);
	FD_SET(aFd, &wf);
	struct timeval tv = { 0, 0 };
	return select(aFd + 1, 0, &wf, 0, &tv) > 0 && FD_ISSET(aFd, &wf);
	}

// a buffer from PSRAM on the AtomS3R, internal heap elsewhere; free() releases either
static uint8_t* bigalloc(size_t aN)
	{
#if defined(BOARD_HAS_PSRAM)
	if (psramFound())
		{
		void* p = heap_caps_malloc(aN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (p) return (uint8_t*)p;
		}
#endif
	return (uint8_t*)malloc(aN);
	}

PppTls::PppTls()
	: iActive(false)
	{
	for (int i = 0; i < kPorts; i++) { iServer[i] = 0; iPort[i] = 0; }
	for (int i = 0; i < kSlots; i++)
		{
		Slot& s = iSlot[i];
		s.up = 0;
		s.buf = 0;
		s.len = s.ofs = 0;
		s.inUse = false;
		s.xc = 0;
		s.uw = s.ch = s.sh = 0;
		s.uwLen = s.uwOfs = s.chLen = s.chOfs = s.shLen = s.shOfs = 0;
		s.port = 0; s.host[0] = 0; s.openMs = 0; s.nUp = s.nDown = 0;
		}
	}

void PppTls::Open(Modem& aModem, Hal& aHal)
	{
	const Settings2& s2 = aModem.Config2();
	IPAddress ours;
	ours.fromString(kPppLocalIp);
	// listen on the PPP address only, so the ports are not visible on the WiFi
	// LAN at all (Accept() still checks the client's localIP(), defence in depth)
	static const uint16_t kList[kPorts] = { kMailImapPort, kMailSmtpPort, kMailPopPort };
	int n = 0;
	for (int i = 0; i < kPorts; i++)
		{
		iServer[i] = 0;
		iPort[i] = 0;
		if (!MailRelayHost(s2, kList[i]))
			continue;                        // this upstream is not configured: do not bind the port
		iServer[i] = new (std::nothrow) WiFiServer(ours, kList[i]);
		if (iServer[i])
			{
			iServer[i]->begin();
			iPort[i] = kList[i];
			n++;
			}
		}
	// a staging buffer per slot: from PSRAM when the board has it (the AtomS3R),
	// else internal memory
	for (int i = 0; i < kSlots; i++)
		{
		Slot& s = iSlot[i];
		s.up = 0;
		s.len = s.ofs = 0;
		s.inUse = false;
		uint8_t* b = 0;
#if defined(BOARD_HAS_PSRAM)
		if (psramFound())
			b = (uint8_t*)heap_caps_malloc(kBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
		if (!b)
			b = (uint8_t*)malloc(kBuf);
		s.buf = b;                           // 0 -> this slot is unusable (checked in Accept)
		}
	iActive = true;
	char m[96];
	snprintf(m, sizeof(m), "ppp tls relay: %d mail port(s) on %s (993/465/995 as set)", n, kPppLocalIp);
	aHal.Log(m);
	}

void PppTls::EndSlot(Slot& aS, Hal& aHal, const char* aReason)
	{
	// log every close with why + how long + how much moved each way, so a hangup
	// is never silent (this is the line to read when a session ends unexpectedly)
	if (aS.inUse)
		{
		char m[160];
		snprintf(m, sizeof(m), "ppp tls relay: %s:%u closed - %s after %lu ms (%lu B down / %lu B up)",
			aS.host[0] ? aS.host : "?", (unsigned)aS.port, aReason ? aReason : "?",
			(unsigned long)(millis() - aS.openMs), (unsigned long)aS.nDown, (unsigned long)aS.nUp);
		aHal.Log(m);
		}
	EndXcode(aS);
	if (aS.up)
		{
		aS.up->stop();
		delete aS.up;
		aS.up = 0;
		}
	aS.psion.stop();
	aS.len = aS.ofs = 0;
	aS.inUse = false;
	aS.nUp = aS.nDown = 0;
	}

// Layer 2: bring up the IMAP filter for this (993) session. On any failure the
// slot falls back to the plain relay (xc stays 0) - email is never worse off.
bool PppTls::StartXcode(Slot& aS, const Settings2& aS2, Hal& aHal)
	{
	iXl.h = &aHal;
	aS.xc = new (std::nothrow) ImapXcode();
	if (aS.xc && aS.xc->Begin(aS2.imgWidth, aS2.imgMaxKB, &iXl))
		{
		aS.uw = bigalloc(kXcHold);
		aS.ch = bigalloc(kXcHold);
		aS.sh = bigalloc(kXcHold);
		}
	if (!aS.xc || !aS.uw || !aS.ch || !aS.sh)
		{
		EndXcode(aS);
		aHal.Log("ppp tls relay: xcode off (no memory) - plain relay");
		return false;
		}
	aS.uwLen = aS.uwOfs = aS.chLen = aS.chOfs = aS.shLen = aS.shOfs = 0;
	aHal.Log("ppp tls relay: xcode on (993: embedded images slimmed)");
	return true;
	}

void PppTls::EndXcode(Slot& aS)
	{
	if (aS.xc) { delete aS.xc; aS.xc = 0; }      // ~ImapXcode -> End() frees its buffers
	free(aS.uw); free(aS.ch); free(aS.sh);
	aS.uw = aS.ch = aS.sh = 0;
	aS.uwLen = aS.uwOfs = aS.chLen = aS.chOfs = aS.shLen = aS.shOfs = 0;
	}

void PppTls::Close(Hal& aHal)
	{
	for (int i = 0; i < kSlots; i++)
		{
		EndSlot(iSlot[i], aHal, "PPP link down");
		free(iSlot[i].buf);                  // the staging buffers go back with the link
		iSlot[i].buf = 0;
		}
	for (int i = 0; i < kPorts; i++)
		if (iServer[i])
			{
			iServer[i]->end();
			delete iServer[i];
			iServer[i] = 0;
			iPort[i] = 0;
			}
	iActive = false;
	aHal.Log("ppp tls relay: closed");
	}

// Open the upstream TLS connection. Blocks during the handshake (as
// AtomHal::UpConnect does), but bounded to a few seconds so the loop recovers.
bool PppTls::StartUpstream(Slot& aS, const char* aHost, uint16_t aPort, bool aVerify, Hal& aHal)
	{
	// refuse only if the internal heap is genuinely too low for the TLS buffers
	// (total free, not largest block - internal RAM is fragmented, and the proven
	// web proxy opens TLS with no guard at all; kSlots=2 is the real bound). A
	// true exhaustion fails fast (closed socket -> PsiMail reports it) instead of
	// starving WiFi and dropping the link.
	size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (freeHeap < kUpMinHeap)
		{
		char m[96];
		snprintf(m, sizeof(m), "ppp tls relay: only %u KB internal heap free: refusing a new TLS session",
			(unsigned)(freeHeap / 1024));
		aHal.Log(m);
		return false;
		}
	WiFiClientSecure* up = new (std::nothrow) WiFiClientSecure();
	if (!up)
		{
		aHal.Log("ppp tls relay: no memory for a TLS client");
		return false;
		}
	if (aVerify)
		{
		// the Mozilla roots (cabundle.h), as UpConnect does
#if ESP_ARDUINO_VERSION_MAJOR >= 3
		up->setCACertBundle(kCaBundle, sizeof(kCaBundle));
#else
		up->setCACertBundle(kCaBundle);
#endif
		}
	else
		up->setInsecure();                   // AT$TLSV=0: no check at all (dangerous; own servers only)
	up->setHandshakeTimeout(5);              // bounded: a dead/slow host must not hold the loop (and the live PPP link) for long
	up->setTimeout(6);
	uint32_t t0 = millis();
	bool ok = up->connect(aHost, aPort);     // the hostname is the SNI name and is checked
	char m[160];
	if (!ok)
		{
		char e[80];
		e[0] = 0;
		up->lastError(e, sizeof(e));
		snprintf(m, sizeof(m), "ppp tls relay: %s:%u TLS failed after %lu ms: %s", aHost, (unsigned)aPort,
			(unsigned long)(millis() - t0), e[0] ? e : "no connection");
		aHal.Log(m);
		delete up;
		return false;
		}
	up->setNoDelay(true);
	aS.up = up;
	snprintf(m, sizeof(m), "ppp tls relay: %s:%u up in %lu ms%s", aHost, (unsigned)aPort,
		(unsigned long)(millis() - t0), aVerify ? "" : " (certificate NOT checked)");
	aHal.Log(m);
	return true;
	}

// Take new plaintext connections from the PPP side, one upstream each.
void PppTls::Accept(Modem& aModem, Hal& aHal)
	{
	const Settings2& s2 = aModem.Config2();
	IPAddress ours;
	ours.fromString(kPppLocalIp);
	for (int p = 0; p < kPorts; p++)
		{
		if (!iServer[p])
			continue;
		int slot = -1;
		for (int i = 0; i < kSlots; i++)
			if (!iSlot[i].inUse && iSlot[i].buf)
				{ slot = i; break; }
		if (slot < 0)
			return;                          // every slot busy: leave the clients queued
		WiFiClient c = iServer[p]->accept();
		if (!c)
			continue;
		// only a session that both arrived on our PPP address and came from the
		// Psion's PPP address: the WiFiServer actually binds every interface
		// must have arrived on our PPP gateway address (defence in depth: the core's
		// WiFiServer actually binds every interface, so this is what keeps the WiFi
		// LAN out). Same check the proven web proxy uses.
		if (c.localIP() != ours)
			{
			char d[96];
			snprintf(d, sizeof(d), "ppp tls relay: refused :%u from %s (not the PPP link)",
				(unsigned)iPort[p], c.remoteIP().toString().c_str());
			aHal.Log(d);
			c.stop(); continue;
			}
		const char* host = MailRelayHost(s2, iPort[p]);
		if (!host)                           // the upstream was cleared since Open()
			{ c.stop(); continue; }
		// log the source so the Psion's PPP address is visible (for tightening the
		// check later) and so a connect that reaches us is seen even if TLS stalls
		char dbg[80];
		snprintf(dbg, sizeof(dbg), "ppp tls relay: %s:%u <- Psion %s", host, (unsigned)iPort[p],
			c.remoteIP().toString().c_str());
		aHal.Log(dbg);
		Slot& s = iSlot[slot];
		s.psion = c;
		s.psion.setNoDelay(true);
		s.len = s.ofs = 0;
		// stamp the session for its close diagnostics
		s.port = iPort[p];
		snprintf(s.host, sizeof(s.host), "%s", host);
		s.openMs = millis();
		s.nUp = s.nDown = 0;
		if (!StartUpstream(s, host, iPort[p], s2.tlsVerify != 0, aHal))
			{ s.psion.stop(); continue; }    // TLS failed: drop (the Psion sees a closed socket -> error)
		s.inUse = true;
		// Layer 2: only IMAPS (993), only when AT$MAILXCODE is on. On failure the
		// session simply runs as the plain relay.
		if (iPort[p] == kMailImapPort && s2.mailXcode)
			StartXcode(s, s2, aHal);
		}
	}

// Relay one session both ways, non-blocking towards the Psion.
void PppTls::Service(Slot& aS, Hal& aHal)
	{
	if (aS.xc) { ServiceXcode(aS, aHal); return; }
	// 1. upstream -> Psion. Refill the stage only when it is empty, so the
	//    upstream's TCP/TLS holds back while the slow Psion link drains it.
	if (aS.ofs >= aS.len)
		{
		aS.ofs = aS.len = 0;
		int avail = aS.up->available();
		if (avail > 0)
			{
			int n = aS.up->read(aS.buf, avail < (int)kBuf ? avail : (int)kBuf);
			if (n > 0)
				aS.len = (size_t)n;
			}
		}
	// write what is staged, raw and non-blocking (NetworkClient::write() would
	// block up to ~10 s on a full window and deadlock the loop that drains PPP)
	size_t budget = kWritePerPass;
	while (aS.ofs < aS.len && budget)
		{
		size_t run = aS.len - aS.ofs;
		if (run > 1460) run = 1460;
		if (run > budget) run = budget;
		int w = ::send(aS.psion.fd(), aS.buf + aS.ofs, run, MSG_DONTWAIT);
		if (w < 0)
			{
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				{
				char r[40]; snprintf(r, sizeof(r), "Psion write error (errno %d)", errno);
				EndSlot(aS, aHal, r); return;   // a real error: the Psion went
				}
			break;                           // full: try again next loop
			}
		aS.ofs += (size_t)w;
		aS.nDown += (uint32_t)w;
		budget -= (size_t)w;
		if ((size_t)w < run)
			break;
		}
	// 2. Psion -> upstream. Only when the upstream socket is writable now (so the
	//    TLS write does not spin for seconds on a full window and stall the loop
	//    that drains PPP), and one small chunk per pass. If it is not writable we
	//    leave the bytes in the Psion's socket; its window closes and it holds off.
	if (aS.psion.available() > 0 && Writable(aS.up->fd()))
		{
		uint8_t tmp[kUpChunk];
		int avail = aS.psion.available();
		int n = aS.psion.read(tmp, avail < (int)sizeof(tmp) ? avail : (int)sizeof(tmp));
		if (n > 0)
			{
			if (!aS.up->connected())
				{ EndSlot(aS, aHal, "upstream dropped (write)"); return; }
			size_t off = 0;
			while (off < (size_t)n)
				{
				int w = aS.up->write(tmp + off, (size_t)n - off);
				if (w <= 0)
					{ EndSlot(aS, aHal, "upstream write failed"); return; } // the session is dead
				off += (size_t)w;
				aS.nUp += (uint32_t)w;
				}
			}
		}
	// 3. close when either side is finished
	if (!aS.psion.connected() && !aS.psion.available())
		{ EndSlot(aS, aHal, "Psion closed the connection"); return; } // the Psion went
	if (!aS.up->connected() && aS.up->available() <= 0 && aS.ofs >= aS.len)
		EndSlot(aS, aHal, "upstream finished (flushed)"); // upstream closed and all flushed to the Psion
	}

// Layer 2 pump: shuttle bytes between the two sockets THROUGH the IMAP filter.
// To the Psion: non-blocking ::send (as the plain relay). To upstream: only
// when writable. A socket is read only when the filter will consume it (Wants*)
// or a holdover is still being fed, so nothing is read then dropped. Bounded
// per call so PPP keeps draining.
void PppTls::ServiceXcode(Slot& aS, Hal& aHal)
	{
	ImapXcode& X = *aS.xc;
	for (int iter = 0; iter < 64; iter++)
		{
		bool progress = false;

		// A. filter -> Psion (staged in buf, drained non-blocking)
		if (aS.ofs >= aS.len) { aS.ofs = 0; aS.len = X.PullToPsion(aS.buf, kBuf); }
		while (aS.ofs < aS.len)
			{
			size_t run = aS.len - aS.ofs;
			if (run > 1460) run = 1460;
			int w = ::send(aS.psion.fd(), aS.buf + aS.ofs, run, MSG_DONTWAIT);
			if (w < 0)
				{
				if (errno != EAGAIN && errno != EWOULDBLOCK)
					{
					char r[40]; snprintf(r, sizeof(r), "Psion write error (errno %d)", errno);
					EndSlot(aS, aHal, r); return;
					}
				break;                           // the Psion's window is full: next pass
				}
			aS.ofs += (size_t)w; aS.nDown += (uint32_t)w; progress = true;
			if ((size_t)w < run) break;
			}

		// B. filter -> upstream (staged in uw, only when writable)
		if (aS.uwOfs >= aS.uwLen) { aS.uwOfs = 0; aS.uwLen = X.PullToServer(aS.uw, kXcHold); }
		if (aS.uwOfs < aS.uwLen && Writable(aS.up->fd()))
			{
			if (!aS.up->connected()) { EndSlot(aS, aHal, "upstream dropped (write)"); return; }
			int w = (int)aS.up->write(aS.uw + aS.uwOfs, aS.uwLen - aS.uwOfs);
			if (w <= 0) { EndSlot(aS, aHal, "upstream write failed"); return; }
			aS.uwOfs += (size_t)w; aS.nUp += (uint32_t)w; progress = true;
			}

		// C. Psion -> filter (client input via the ch holdover)
		if (aS.chOfs < aS.chLen)
			{
			size_t c = X.PushClient(aS.ch + aS.chOfs, aS.chLen - aS.chOfs);
			if (c) { aS.chOfs += c; progress = true; }
			}
		else if (X.WantsClient() && aS.psion.available() > 0)
			{
			int n = aS.psion.read(aS.ch, kXcHold);
			if (n > 0) { aS.chLen = (size_t)n; aS.chOfs = 0; progress = true; }
			}

		// D. upstream -> filter (server input via the sh holdover)
		if (aS.shOfs < aS.shLen)
			{
			size_t c = X.PushServer(aS.sh + aS.shOfs, aS.shLen - aS.shOfs);
			if (c) { aS.shOfs += c; progress = true; }
			}
		else if (X.WantsServer() && aS.up->available() > 0)
			{
			int n = aS.up->read(aS.sh, kXcHold);
			if (n > 0) { aS.shLen = (size_t)n; aS.shOfs = 0; progress = true; }
			}

		if (!progress) break;
		}

	// close when a side is gone and nothing is left to hand it
	bool toPsionIdle = aS.ofs >= aS.len && aS.chOfs >= aS.chLen;
	if (!aS.psion.connected() && aS.psion.available() <= 0 && toPsionIdle)
		{ EndSlot(aS, aHal, "Psion closed the connection"); return; }
	bool toUpIdle = aS.uwOfs >= aS.uwLen && aS.shOfs >= aS.shLen;
	if (!aS.up->connected() && aS.up->available() <= 0 && toUpIdle && aS.ofs >= aS.len)
		EndSlot(aS, aHal, "upstream finished (flushed)");
	}

void PppTls::Loop(Modem& aModem, Hal& aHal)
	{
	bool want = aModem.PppCall() && aModem.Ppp().Up() && MailRelayAny(aModem.Config2());
	if (!want)
		{
		if (iActive)
			Close(aHal);
		return;
		}
	if (!iActive)
		Open(aModem, aHal);
	// drain the live sessions first, then take new ones
	for (int i = 0; i < kSlots; i++)
		if (iSlot[i].inUse)
			Service(iSlot[i], aHal);
	Accept(aModem, aHal);
	}

} // namespace am
