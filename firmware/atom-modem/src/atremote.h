// atremote.h - AT commands over PPP (2.2): while the Psion is online through
// the modem's PPP link, PsiConnect (the Psion's set-up program) can talk to
// the modem over TCP, 192.168.7.1:2323, instead of needing the serial port
// that PPP is using. The same command language and replies as the serial
// command mode, one line at a time, with the commands that would cut the
// PPP link or change the serial line refused ("+NOTVIAPPP: why", ERROR).
// This file is the part with no sockets: the filter and one client's
// session (line assembly, the answer queue, the idle timeout). The socket
// glue is board/pppat.cpp on the Atom, and hostmodem in the host tests.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_ATREMOTE_H
#define ATOM_ATREMOTE_H

#include <stdint.h>
#include <stddef.h>

namespace am {

class Modem;

static const uint16_t kAtRemotePort = 2323;

// aCmd: the command line after "AT" (as Modem::RunCommands takes it).
// False if any command on it may not be run over PPP; aWhy then says why in
// a few words ("it would end the PPP link", "it changes the serial line")
bool AtRemoteAllowed(const char* aCmd, char* aWhy, size_t aWhyMax);

// One client's session. The glue reads from the client only while
// CanTake(), passes the bytes to Input (which says how many it took: the
// rest are offered again once the answer has gone), and sends Pending()
class AtRemote
	{
public:
	static const size_t kLineMax = 256;          // a longer line is refused (+TOOLONG)
	static const size_t kOutMax = 8192;          // one answer, at most (+TRUNCATED beyond)
	static const uint32_t kIdleMs = 120000;      // no input this long: the glue closes it
	AtRemote() : iLineLen(0), iTooLong(false), iLastMs(0), iOutLen(0), iOutPos(0) {}
	void Begin(uint32_t aNow);
	// takes bytes up to and including the first CR whose line gave an
	// answer (then CanTake() is false until it has been sent); returns how many
	size_t Input(Modem& aModem, const uint8_t* aData, size_t aLen, uint32_t aNow);
	bool CanTake() const { return iOutPos >= iOutLen; }
	size_t Pending(const uint8_t*& aPtr) const;
	void Sent(size_t aLen);
	bool Expired(uint32_t aNow) const { return aNow - iLastMs >= kIdleMs; }
private:
	char iLine[kLineMax];
	size_t iLineLen;
	bool iTooLong;
	uint32_t iLastMs;
	char iOut[kOutMax];
	size_t iOutLen, iOutPos;
	};

} // namespace am

#endif
