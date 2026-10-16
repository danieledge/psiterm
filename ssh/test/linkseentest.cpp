// linkseentest.cpp - host test for the shared link's last sign of life
// (ssh/pglinkseen.h): when one program's failed probe may hang up the Psion
// Internet link that every program shares, and when a second program may use
// it without a probe.
//   in ssh/: make -f test/Makefile.host linkseentest && ./linkseentest
#include <stdio.h>
#include "../pglinkseen.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static TPgLinkSeen Make(unsigned seen, unsigned suspect)
	{
	TPgLinkSeen s;
	s.iMagic = PG_SEEN_MAGIC;
	s.iSeen = seen;
	s.iSuspect = suspect;
	s.iSpare = 0;
	return s;
	}

int main()
	{
	const unsigned now = 1000000;
	const unsigned W = PG_SEEN_FRESH_S;

	// no chunk, an unset chunk, never any data: nothing vouches for the link
	CHECK(!PgSeenFresh(0, now, W));
	TPgLinkSeen z = Make(0, 0);
	z.iMagic = 0;
	z.iSeen = now;
	CHECK(!PgSeenFresh(&z, now, W));
	TPgLinkSeen never = Make(0, 0);
	CHECK(!PgSeenFresh(&never, now, W));
	CHECK(PgSeenAgo(&never, now) == -1);

	// the device's case: one program (PsiMail, say) streaming over the link, a
	// second program's probe times out - data a second ago: alive, keep it
	TPgLinkSeen busy = Make(now - 1, 0);
	CHECK(PgSeenFresh(&busy, now, W));
	CHECK(PgSeenAgo(&busy, now) == 1);
	TPgLinkSeen edge = Make(now - W, 0);
	CHECK(PgSeenFresh(&edge, now, W));

	// the original dead-link case (PPP dropped while idle): the last data is
	// minutes old, so the probe's timeout is believed and the link hung up
	TPgLinkSeen idle = Make(now - 300, 0);
	CHECK(!PgSeenFresh(&idle, now, W));
	TPgLinkSeen justOld = Make(now - W - 1, 0);
	CHECK(!PgSeenFresh(&justOld, now, W));

	// a failure since the last data (a receive error, an idle NOOP that went
	// unanswered, a switch-on): the earlier data no longer vouches
	TPgLinkSeen doubted = Make(now - 5, now - 2);
	CHECK(!PgSeenFresh(&doubted, now, W));
	TPgLinkSeen sameSecond = Make(now - 2, now - 2);
	CHECK(!PgSeenFresh(&sameSecond, now, W));
	// ...until data comes in again (the busy program's next receive)
	TPgLinkSeen again = Make(now, now - 2);
	CHECK(PgSeenFresh(&again, now, W));
	TPgLinkSeen oldDoubt = Make(now - 3, now - 400);
	CHECK(PgSeenFresh(&oldDoubt, now, W));

	// the clock set back: a time ahead of now proves nothing
	TPgLinkSeen ahead = Make(now + 60, 0);
	CHECK(!PgSeenFresh(&ahead, now, W));
	CHECK(PgSeenAgo(&ahead, now) == -1);
	// a suspicion "in the future" (clock set back after it) does not block data
	TPgLinkSeen aheadDoubt = Make(now - 1, now + 60);
	CHECK(PgSeenFresh(&aheadDoubt, now, W));

	// the probe: only an answer from a DNS server is proof; nothing else is
	// a verdict (in particular not -1, which ER5 may use for "no such name")
	CHECK(PgProbeAnswered(0));
	CHECK(PgProbeAnswered(-5120));
	CHECK(PgProbeAnswered(-5121));
	CHECK(PgProbeAnswered(-5199));
	CHECK(!PgProbeAnswered(-5200));
	CHECK(!PgProbeAnswered(-5119));
	CHECK(!PgProbeAnswered(-1));
	CHECK(!PgProbeAnswered(-33));
	CHECK(!PgProbeAnswered(-190));

	printf("linkseentest: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
	}
