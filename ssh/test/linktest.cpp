// linktest.cpp - host test for Connection settings > Test (pglinktest.h):
// the reply classifier and the wording of the result. psiglue.cpp is built
// with only its pure link-test part (PG_LINK_TEST_HOST: no EPOC calls).
//   in ssh/: make -f test/Makefile.host linktest && ./linktest
#include <stdio.h>
#include <string.h>
#include "../psiglue.cpp"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int Cls(const char* s, int n = -1, char* first = 0)
	{
	char f[40];
	return pg_lt_classify((const unsigned char*)s, n < 0 ? (int)strlen(s) : n, first ? first : f, sizeof(f));
	}

static int Has(const PgLinkTest& t, const char* s)
	{
	for (int i = 0; i < t.nlines; i++)
		if (strstr(t.line[i], s))
			return 1;
	return 0;
	}

static void Dump(const char* what, const PgLinkTest& t)
	{
	printf("-- %s\n", what);
	for (int i = 0; i < t.nlines; i++)
		printf("   %s\n", t.line[i]);
	}

static void Blank(PgLinkTest& t)
	{
	memset(&t, 0, sizeof(t));
	t.baud_index = 3;
	t.found_baud = -1;
	t.dns = 1;
	t.net_after = -1;
	strcpy(t.host, "raw.githubusercontent.com");
	strcpy(t.ppp_start, "ATDT777");
	}

int main()
	{
	// --- what came back after AT ---
	CHECK(Cls("") == PG_AT_NOTHING);
	CHECK(Cls("\r\n\r\n") == PG_AT_NOTHING);
	CHECK(Cls("\r\nOK\r\n") == PG_AT_OK);                 // echo off (ATE0)
	CHECK(Cls("AT\r\r\nOK\r\n") == PG_AT_OK);             // WiRSa: echo, then OK
	CHECK(Cls("at\r\nok\r\n") == PG_AT_OK);
	CHECK(Cls("AT\r\r\nERROR\r\n") == PG_AT_ERROR);
	CHECK(Cls("AT\r") == PG_AT_ECHO);                     // TX and RX joined
	CHECK(Cls("AT\rAT\r") == PG_AT_ECHO);
	CHECK(Cls("\xf0\x00\xfe\x86\x1e\xe0", 6) == PG_AT_GARBAGE);   // wrong speed
	CHECK(Cls("\x80\x80\xff", 3) == PG_AT_GARBAGE);
	CHECK(Cls("\xff", 1) == PG_AT_GARBAGE);              // a framing error alone
	char first[40];
	CHECK(Cls("AT\r\r\nNO CARRIER\r\n", -1, first) == PG_AT_TEXT && strcmp(first, "NO CARRIER") == 0);
	CHECK(Cls("SSH-2.0-OpenSSH_9.6\r\n", -1, first) == PG_AT_TEXT && strncmp(first, "SSH-2.0", 7) == 0);
	CHECK(Cls("AT\r\x01\r\nOK\r\n") == PG_AT_OK);         // a stray byte does not hide OK
	CHECK(Cls("OKAY\r\n", -1, first) == PG_AT_TEXT);

	// --- the modem's name from ATI ---
	char name[48];
	const char* ati = "ATI\r\r\nWiRSa v3.12 (Psion)\r\nIP: 192.168.1.50\r\nOK\r\n";
	pg_lt_first_line((const unsigned char*)ati, strlen(ati), "ATI", name, sizeof(name));
	CHECK(strcmp(name, "WiRSa v3.12 (Psion)") == 0);
	const char* ati2 = "\r\nOK\r\n";
	pg_lt_first_line((const unsigned char*)ati2, strlen(ati2), "ATI", name, sizeof(name));
	CHECK(name[0] == 0);

	// --- the result, in sentences ---
	PgLinkTest t;
	Blank(t); t.at = PG_AT_OK; t.signals = 1; t.cts = 0; t.dcd = 0;
	strcpy(t.modem, "WiRSa fake modem 3.0");
	pg_lt_report(&t); Dump("modem OK at 57600", t);
	CHECK(strcmp(t.line[0], "The modem answered OK at 57600 baud") == 0);
	CHECK(Has(t, "CTS is low, DCD is off"));
	CHECK(Has(t, "Modem: WiRSa fake modem 3.0"));

	Blank(t); t.at = PG_AT_NOTHING; t.signals = 1;
	pg_lt_report(&t); Dump("nothing", t);
	CHECK(strcmp(t.line[0], "Nothing came back - check the cable, or try swapping TX and RX") == 0);

	Blank(t); t.at = PG_AT_GARBAGE; t.found_baud = 4; t.signals = 1;
	pg_lt_report(&t); Dump("garbage, answers at 115200", t);
	CHECK(Has(t, "Garbled reply at 57600 baud"));
	CHECK(Has(t, "The modem answers at 115200 baud - set Baud rate to 115200"));

	Blank(t); t.rtscts = 1; t.cts_blocked = 1; t.at_noflow = PG_AT_OK; t.signals = 1; t.cts = 0;
	pg_lt_report(&t); Dump("RTS/CTS on, CTS low", t);
	CHECK(Has(t, "with Flow control None"));
	CHECK(Has(t, "CTS is low - set Flow control to None"));
	CHECK(Has(t, "may not support flow control"));

	// CTS high with flow control off: suggest RTS/CTS; low or already on: not
	Blank(t); t.at = PG_AT_OK; t.signals = 1; t.cts = 1; t.dcd = 1;
	pg_lt_report(&t); Dump("CTS high, flow control off", t);
	CHECK(Has(t, "Flow control RTS/CTS is safer at speed"));
	Blank(t); t.at = PG_AT_OK; t.signals = 1; t.cts = 1; t.rtscts = 1;
	pg_lt_report(&t);
	CHECK(!Has(t, "Flow control RTS/CTS is safer"));

	Blank(t); t.at = PG_AT_OK; t.escaped = 1;
	pg_lt_report(&t);
	CHECK(Has(t, "+++ and ATH ended it"));

	Blank(t); t.at = PG_AT_ECHO;
	pg_lt_report(&t);
	CHECK(Has(t, "Only an echo came back"));

	Blank(t); t.port = PG_PORT_BUSY;
	pg_lt_report(&t); Dump("port busy", t);
	CHECK(strcmp(t.line[0], "The serial port is in use by another program or connection") == 0);
	Blank(t); t.port = PG_PORT_REMOTE;
	pg_lt_report(&t);
	CHECK(Has(t, "in use by another program") && Has(t, "Remote link off"));
	Blank(t); t.port = PG_PORT_FAIL; t.port_err = -18;
	pg_lt_report(&t);
	CHECK(strcmp(t.line[0], "Could not open the serial port (-18)") == 0);

	Blank(t); t.net_mode = 1; t.net_up = 1; t.dns = 0; t.addr = (185UL << 24) | (199 << 16) | (108 << 8) | 133;
	pg_lt_report(&t); Dump("Internet up, DNS ok", t);
	CHECK(strcmp(t.line[0], "The Psion's Internet connection is up") == 0);
	CHECK(Has(t, "Looked up raw.githubusercontent.com: 185.199.108.133 - DNS works"));

	Blank(t); t.net_mode = 1; t.net_up = 0; t.need_dial = 1;
	pg_lt_report(&t);
	CHECK(strcmp(t.line[0], "The Psion's Internet connection is not up") == 0);

	Blank(t); t.net_mode = 1; t.net_up = 0; t.dial = 1; t.ppp = 1; t.dns = -3006; t.net_after = 0;
	pg_lt_report(&t); Dump("dialled, failed", t);
	CHECK(Has(t, "Sent ATDT777 - the modem answered CONNECT"));
	CHECK(Has(t, "did not start"));
	CHECK(Has(t, "Could not look up raw.githubusercontent.com (-3006)"));

	// every line fits the result dialog's labels (reserve_length 80)
	Blank(t); t.port = PG_PORT_FAIL; t.port_err = -2147483647L;
	strcpy(t.host, "a-very-long-host-name-that-goes-on-and-on.example.com");
	t.net_mode = 1; t.dial = 1; t.dns = -2147483647L;
	strcpy(t.ppp_start, "AT&F&C1&D0E0V1Q0X4S0=0S7=60DT777777777777");
	t.ppp = 3;
	pg_lt_report(&t);
	for (int i = 0; i < t.nlines; i++)
		CHECK(strlen(t.line[i]) < PG_LT_LINE);
	CHECK(t.nlines <= PG_LT_LINES);

	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
	}
