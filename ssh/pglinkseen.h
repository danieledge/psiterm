// pglinkseen.h - the Psion Internet link's last sign of life, shared by every
// engine (psiglue.cpp). Plain C types only, so the rules can be tested on a PC
// (ssh/test/linkseentest.cpp).
//
// Why: ER5's PPP has no LCP echo, so NIFMAN's "up" is only a claim, and an
// engine that finds the link up probes it (a DNS query across PPP) and hangs
// it up (RNif::Stop) if the probe gets no answer. But the link is shared by
// every program: a second program's probe can fail on a link that is alive
// and busy - another program streaming over it, the CPU taken by its TLS -
// and its Stop then took the link from under the program using it (PsiTerm
// "hanging up before it can connect", one program failing while another
// was online). A probe in one process cannot see another's traffic,
// so each engine stamps, in one small global chunk, when data last came in
// over the link and when a failure last made it suspect. Data that arrived
// after the last suspicion, and recently, proves the link carries data: no
// engine then hangs it up, and a second program uses it without a probe.
// Only data that actually came in counts (a dead link delivers none), so a
// busy program cannot fake "dead", and nothing can fake "alive".
//
// The times are seconds of universal time (they run on while the Psion is
// off, unlike the tick count), stored as unsigned 32-bit values.
#ifndef PGLINKSEEN_H
#define PGLINKSEEN_H

#define PG_SEEN_CHUNK  "PsiLinkSeen"
#define PG_SEEN_MAGIC  0x5053454eu       // "PSEN"
#define PG_SEEN_FRESH_S 30u               // data this recent proves the link alive

typedef struct
	{
	unsigned int iMagic;
	unsigned int iSeen;                  // data last came in over the link (0 = never, or hung up since)
	unsigned int iSuspect;               // a failure last made the link suspect (0 = never)
	unsigned int iSpare;
	} TPgLinkSeen;

// Did data come in over the link recently, and after the last suspicion?
// A time ahead of aNow (the clock was set back) proves nothing.
static inline int PgSeenFresh(const TPgLinkSeen* aS, unsigned int aNow, unsigned int aWindow)
	{
	if (!aS || aS->iMagic != PG_SEEN_MAGIC || aS->iSeen == 0)
		return 0;
	if (aS->iSeen > aNow || aNow - aS->iSeen > aWindow)
		return 0;
	if (aS->iSuspect != 0 && aS->iSuspect <= aNow && aS->iSeen <= aS->iSuspect)
		return 0;                        // nothing has come in since the link was last in doubt
	return 1;
	}

// How long ago data came in (seconds), or -1 if never / not to be trusted
static inline int PgSeenAgo(const TPgLinkSeen* aS, unsigned int aNow)
	{
	if (!aS || aS->iMagic != PG_SEEN_MAGIC || aS->iSeen == 0 || aS->iSeen > aNow)
		return -1;
	unsigned int d = aNow - aS->iSeen;
	return d > 0x7fffffffu ? -1 : (int)d;
	}

// The DNS probe's outcome. 1: an answer from a DNS server across the link
// (an address, or any error of the DNS family -5120..-5199: "no such name" is
// an answer). 0: completed with another error - not proof either way (the ER5
// resolver's code for a name that does not exist is not documented), so the
// real lookup or connect goes on and is judged instead; never a reason to
// hang the link up. (A timeout is the caller's: only that suggests a dead link.)
static inline int PgProbeAnswered(int aErr)
	{
	return aErr == 0 || (aErr <= -5120 && aErr > -5200);
	}

#endif
