/* pglinktest.h - Connection settings > Test, shared by PsiTerm, PsiMail
 * and PsiWeb.
 *
 * The test itself (pg_link_test) is in psiglue.cpp, next to the code that
 * dials for real. Each app compiles it through pglinktest.cpp, which also
 * holds the small EIKON part (busy message, "?" query, result dialog), so
 * the app side is only "read the dialog, call PgLinkTestL".
 *
 * Modem route: open COMM::0 with the settings shown in the dialog, send AT,
 * say what came back, read CTS and DCD, ask ATI for the modem's name, close
 * the port. Psion Internet: ask NIFMAN whether the link is up and look up a
 * name; it never dials unless the caller sets 'dial' (after asking).
 *
 * Plain C data so the report wording can be unit-tested on a PC
 * (ssh/test/linktest.cpp).
 */
#ifndef PGLINKTEST_H
#define PGLINKTEST_H

#define PG_LT_LINES   5
#define PG_LT_LINE    80      /* = the labels' reserve_length in the .rss (a longer text panics) */

enum
	{
	PG_PORT_OK = 0,
	PG_PORT_BUSY,          /* another program or connection has it */
	PG_PORT_REMOTE,        /* KErrAccessDenied: the Remote link, or another program */
	PG_PORT_FAIL           /* any other failure: port_err says which */
	};

enum
	{
	PG_AT_NOTHING = 0,     /* not a byte */
	PG_AT_OK,              /* "OK" (with or without the modem's echo) */
	PG_AT_ECHO,            /* only our own "AT" came back */
	PG_AT_GARBAGE,         /* bytes that are not text: wrong speed, or noise */
	PG_AT_TEXT,            /* readable text, but not OK (reply[] has it) */
	PG_AT_ERROR,           /* "ERROR": a modem is there but refused AT */
	PG_AT_NOTSENT          /* the write itself stalled (CTS low with RTS/CTS on) */
	};

typedef struct
	{
	/* ---- in ---- */
	int baud_index;        /* 0=9600 .. 4=115200, as PsiLink.ini */
	int rtscts;
	int net_mode;          /* 0 modem, 1 Psion Internet */
	int dial;              /* Psion Internet: may bring the link up */
	char ppp_start[44];    /* Psion Internet: sent to the modem first (dial only) */
	char host[64];         /* a name to look up (Psion Internet) */
	void (*progress)(void* aCtx, const char* aText);   /* busy message, or 0 */
	void* ctx;

	/* ---- out: modem ---- */
	int port;              /* PG_PORT_* */
	int port_err;          /* the EPOC error when port == PG_PORT_FAIL */
	int at;                /* PG_AT_* at the chosen speed and flow control */
	int escaped;           /* the modem answered only after +++ ATH */
	int cts_blocked;       /* RTS/CTS on and the write stalled: retried with none */
	int at_noflow;         /* PG_AT_* of that retry */
	int found_baud;        /* -1, or the speed the modem did answer at */
	int signals;           /* 1 = cts and dcd below are valid */
	int cts, dcd;
	char reply[40];        /* the first line of an odd reply (PG_AT_TEXT) */
	char modem[48];        /* the first line of the ATI reply */

	/* ---- out: Psion Internet ---- */
	int net_up;            /* 1 up, 0 down, -1 the Psion would not say */
	int need_dial;         /* down, and 'dial' was not set: ask, then call again */
	int ppp;               /* 0 not sent, 1 CONNECT, 2 no CONNECT, 3 port busy, 4 no modem */
	int dns;               /* 1 not tried, 0 looked up, else the EPOC error */
	unsigned long addr;    /* the address looked up */
	int net_after;         /* after dialling: 1 up, 0 not, -1 unknown */

	/* ---- out: the result, in plain sentences ---- */
	int nlines;
	char line[PG_LT_LINES][PG_LT_LINE];
	} PgLinkTest;

#ifdef __cplusplus
extern "C" {
#endif
/* Runs the test (blocking, a few seconds; up to 90 s when it dials) and
 * fills in the result lines. Never leaves the port, a socket or a request
 * open. Returns 0, or -1 if it could not start (no memory). */
int pg_link_test(PgLinkTest* aTest);

/* The pure parts (no EPOC calls), for the host test. */
int pg_lt_classify(const unsigned char* aData, int aLen, char* aFirst, int aFirstMax);
void pg_lt_first_line(const unsigned char* aData, int aLen, const char* aSkip, char* aOut, int aMax);
void pg_lt_report(PgLinkTest* aTest);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* pglinktest.cpp (EIKON, compiled into each .app): runs the test with the
 * values shown in the Connection settings dialog, with a busy message, the
 * "Connect now to test it?" query when the Psion's Internet connection is
 * down, and the result in aResultDialog (a dialog of PG_LT_LINES labels,
 * ids aFirstLineId onwards). */
class TDesC8;
void PgLinkTestL(int aBaudIndex, int aRtsCts, int aNetMode, const TDesC8& aPppStart,
	int aResultDialog, int aFirstLineId);
/* The same, as a command: a Connect menu item. It brings the Psion's Internet
 * connection up (dialling, with no "Connect now?" question), or checks the
 * modem, and says what it found in the result dialog, titled aTitle. */
void PgLinkConnectL(int aBaudIndex, int aRtsCts, int aNetMode, const TDesC8& aPppStart,
	int aResultDialog, int aFirstLineId, const TDesC8& aTitle);
#endif

#endif
