/* pmnet.c - PsiMail's one network connection, on PsiTerm's psiglue
 *
 * As in PsiWeb: the Psion reaches the network through the serial port -
 * a WiFi modem that turns "ATDT host:port" into one TCP connection, or
 * EPOC's own dial-up (PPP) stack - so there is one connection at a time.
 * TLS is PsiTerm's TLS 1.3 client (ssh/tls13.c), here with the server's
 * certificate checked (certcheck.c).
 *
 * Every dial, close and hang-up is logged with its reason (psimail.log):
 * on a modem line each one costs seconds, so the log must say why.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "pm.h"
#ifndef PM_NO_TLS
#include "../../ssh/tls13.h"
extern int tls_pending(void);
#endif

extern PsiShared *pg_shared(void);
extern int  pg_dial(char *why, int max);
extern void pg_hangup(void);
extern int  pg_net_avail(void);
extern int  pg_net_read(void *buf, int max);
extern int  pg_serial_write(const void *buf, int len);
extern int  pg_wait(int ms, int want_net, int want_kbd);
extern void pg_link_close(void);
extern int  pg_link_is_open(void);
extern int  pg_rx_errors(int *last);      /* psiglue: serial line errors so far (host: 0) */
extern void pg_set_link_log(void (*fn)(const char *));   /* psiglue's link messages, for the log */

/* With nothing to do for this long, hang up and let go of the line.
 *
 * Modem (WiRSa pipe): 90 s. The serial port is ours while a connection is
 * open, PsiTerm and PsiWeb need it, and dialling again costs ten seconds.
 *
 * Psion Internet (PPP): 20 minutes, with the IMAP connection kept open.
 * Here the port belongs to the Psion's dial-up, not to us, and while a
 * socket is open the dial-up stays up. Letting go after 90 s made EPOC
 * hang the dial-up up, and the next thing the user did - opening a
 * message, a sync - had to start PPP again from the modem's AT prompt:
 * half a minute of "Looking up ...", the Psion's connection dialogs, and
 * a WiRSa still in PPP mode from last time answering nothing. IMAP servers
 * keep an idle connection at least 30 minutes (RFC 3501), so 20 minutes
 * is safe; a server or router that drops it sooner is caught by the NOOP
 * imap_open sends before reusing an idle connection. */
#define IDLE_RELEASE_MS      90000
#define IDLE_RELEASE_NET_MS  (20 * 60 * 1000L)

/* After the modem's "NO CARRIER" nothing more ever arrives. Text that only
   looks like it (an email about modems, on plain IMAP) is followed by more
   data, so the line is only given up when it goes quiet for this long. */
#define NO_CARRIER_QUIET_MS 3000

static unsigned long g_last_use;
static int  g_open, g_tls, g_dead, g_suspect;
static char g_tail[16];
static unsigned char g_buf[2048];
static int g_bpos, g_blen;

static int g_connid;
static int g_dials;                       /* this session, for the log */

static char g_err[48];

const char *pmn_error(void) { return g_err; }

static void used(void) { g_last_use = pm_ms(); }

/* changes with every new connection, so IMAP can tell whether the open
   connection is still its own (SMTP uses the same line) */
int pmn_conn_id(void) { return g_open ? g_connid : -1; }

static int modem(void) { return pg_shared() && !pg_shared()->net_mode; }

/* psiglue's one-line link messages ("Looking up...", "Modem: CONNECT",
   "The connection dropped (error -36)") in psimail.log too: on the Psion
   Internet route they are the only account of what the dial-up did */
static void link_log(const char *msg) { pm_log("link: %s", msg); }

int pmn_connect(const char *host, int port, int tls, char *why, int whymax)
{
	PsiShared *s = pg_shared();
	unsigned long t0;
	int errs, last;
	static int hooked;
	if (!hooked) { hooked = 1; pg_set_link_log(link_log); }
	if (g_open) pmn_close_why(1, "making way for a new connection");
	used();
	pm_copy(s->host, host, sizeof(s->host));
	s->port = port;
	pm_progress("Connecting to %s...", host);
	g_dials++;
	pm_log("net: dial %d: %s:%d%s (%s)", g_dials, host, port, tls ? " tls" : "", modem() ? "modem" : "psion tcp/ip");
	t0 = pm_ms();
	if (pg_dial(why, whymax) != 0) {
		pm_log("net: dial failed after %lu ms: %s%s", pm_ms() - t0, why, pm_cancelled() ? " (stopped)" : "");
		return -1;
	}
	g_bpos = g_blen = 0;
	g_err[0] = 0;
	g_open = 1;
	g_connid++;
	g_dead = 0;
	g_suspect = 0;
	g_tls = 0;
	memset(g_tail, 0, sizeof(g_tail));
	pm_shared()->online = 1;
	errs = pg_rx_errors(&last);
	pm_log("net: connected in %lu ms (conn %d%s)", pm_ms() - t0, g_connid,
		errs ? ", serial line errors so far" : "");
	if (tls) {
		t0 = pm_ms();
		if (pmn_starttls(host, why, whymax) != 0) { pmn_close_why(1, why); return -1; }
		pm_log("net: tls up in %lu ms", pm_ms() - t0);
	}
	return 0;
}

int pmn_starttls(const char *host, char *why, int whymax)
{
#ifndef PM_NO_TLS
	pm_progress("Securing the connection to %s...", host);
	tlsv_set_host(host, pg_shared()->port);
	if (tls_connect(host, why, whymax) != 0)
		return -1;
	g_tls = 1;
	g_bpos = g_blen = 0;
	return 0;
#else
	snprintf(why, whymax, "TLS is not available in this build");
	return -1;
#endif
}

int pmn_is_open(void)
{
	return g_open && !g_dead;
}

int pmn_write(const void *buf, int len)
{
	used();
	if (!g_open || g_dead) return -1;
#ifndef PM_NO_TLS
	if (g_tls) return tls_write(buf, len) == 0 ? 0 : -1;
#endif
	return pg_serial_write(buf, len) == len ? 0 : -1;
}

int pmn_printf(const char *fmt, ...)
{
	static char b[1200];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n < 0 || n >= (int)sizeof(b)) return -1;
	return pmn_write(b, n);
}

/* plain (non-TLS) modem line: has the modem's "NO CARRIER" gone by? It is
   only a suspicion until the line falls silent (see NO_CARRIER_QUIET_MS) */
static void note_tail(const unsigned char *b, int n)
{
	int keep = (int)sizeof(g_tail) - 1;
	if (n >= keep) memcpy(g_tail, b + n - keep, keep);
	else { memmove(g_tail, g_tail + n, keep - n); memcpy(g_tail + keep - n, b, n); }
	g_tail[keep] = 0;
	if (strstr(g_tail, "NO CARRIER")) g_suspect = 1;
}

static int raw_read(void *buf, int max, int timeout_ms)
{
	int n;
	if (!g_open) return -1;
#ifndef PM_NO_TLS
	if (g_tls) {
		if (!tls_pending() && pg_net_avail() == 0) {
			int m = pg_wait(timeout_ms, 1, 0);
			if (m & 8) return PMN_CANCEL;
			if (pg_net_avail() == 0) {
				pm_copy(g_err, (m & 1) ? "line closed" : "timeout", sizeof(g_err));
				return (m & 1) ? 0 : PMN_TIMEOUT;
			}
		}
		n = tls_read(buf, max, 10000);   /* mid-record: silence means bytes were lost */
		if (n == -2) return PMN_CANCEL;
		if (n <= 0) snprintf(g_err, sizeof(g_err), "tls: %s", n == 0 ? "closed" : tls_error());
		if (n < 0 && modem() && pg_net_avail() == 0) {
			/* on a modem line what is not TLS is the modem talking: after
			   the handshake that is its NO CARRIER (the TCP side closed) */
			if (strstr(tls_error(), "not TLS")) { pm_copy(g_err, "NO CARRIER (modem)", sizeof(g_err)); g_dead = 1; }
			return 0;
		}
		return n;
	}
#endif
	if (pg_net_avail() == 0) {
		int t = timeout_ms;
		int m;
		if (g_suspect && (t < 0 || t > NO_CARRIER_QUIET_MS)) t = NO_CARRIER_QUIET_MS;
		m = pg_wait(t, 1, 0);
		if (m & 8) return PMN_CANCEL;
		if (pg_net_avail() == 0) {
			if (g_suspect) {
				pm_copy(g_err, "NO CARRIER (modem)", sizeof(g_err));
				g_dead = 1;
				return 0;
			}
			if (!(m & 1)) pm_copy(g_err, "timeout", sizeof(g_err));
			return (m & 1) ? 0 : PMN_TIMEOUT;
		}
	}
	n = pg_net_read(buf, max);
	if (n > 0 && modem()) {
		if (g_suspect) g_suspect = 0;      /* more came: it was only text */
		note_tail((const unsigned char *)buf, n);
	}
	return n;
}

int pmn_read(void *buf, int max, int timeout_ms)
{
	used();
	if (g_bpos < g_blen) {
		int k = g_blen - g_bpos;
		if (k > max) k = max;
		memcpy(buf, g_buf + g_bpos, k);
		g_bpos += k;
		return k;
	}
	if (g_dead) return 0;
	return raw_read(buf, max, timeout_ms);
}

int pmn_getc(int timeout_ms)
{
	if (g_bpos >= g_blen) {
		int n;
		if (g_dead) return -1;           /* (0 is a byte: it made pmn_readline spin) */
		n = raw_read(g_buf, sizeof(g_buf), timeout_ms);
		if (n <= 0) return n == 0 ? -1 : n;
		g_bpos = 0;
		g_blen = n;
	}
	used();
	return g_buf[g_bpos++];
}

/* A line without its CRLF; long lines are cut (the rest is dropped). */
int pmn_readline(char *buf, int max, int timeout_ms)
{
	int k = 0, c;
	for (;;) {
		c = pmn_getc(timeout_ms);
		if (c < 0) { buf[k] = 0; return c == PMN_CANCEL ? PMN_CANCEL : c == PMN_TIMEOUT ? PMN_TIMEOUT : -1; }
		if (c == '\n') break;
		if (k < max - 1) buf[k++] = (char)c;
	}
	if (k > 0 && buf[k - 1] == '\r') k--;
	buf[k] = 0;
	return k;
}

void pmn_close_why(int hangup, const char *why)
{
	if (g_open) {
		int last = 0, errs = pg_rx_errors(&last);
		pm_log("net: close conn %d, hangup=%d: %s%s%s", g_connid, hangup && !g_dead, why ? why : "",
			g_err[0] ? "; last error: " : "", g_err[0] ? g_err : "");
		if (errs) pm_log("net: %d serial line error(s) on this connection, last %d", errs, last);
		/* Psion Internet: "hang up" only closes the TCP socket (the dial-up
		   stays), and that must always happen - a socket left open, as
		   hangup=0 used to after SMTP's QUIT or an HTTP "Connection: close",
		   was reopened by the next connection: a leaked ESOCK handle whose
		   stale receive made the new connection look dropped, which then
		   had PPP taken down and dialled again. */
		if (!modem()) pg_hangup();
		/* modem: after its own NO CARRIER it is at its prompt already, and
		   hangup=0 says the server is closing the call: +++ ATH would only
		   cost 2.5 s */
		else if (hangup && !g_dead) pg_hangup();
	}
	g_open = 0;
	g_dead = 0;
	g_suspect = 0;
	g_tls = 0;
	g_bpos = g_blen = 0;
	pm_shared()->online = 0;
}

void pmn_close(int hangup)
{
	pmn_close_why(hangup, "closed");
}

/* Called about once a second: after a while with nothing to do, hang up and
   let go of the serial port so other programs (PsiTerm, PsiWeb) can have it. */
void pmn_idle_tick(void)
{
	long idle = modem() ? IDLE_RELEASE_MS : IDLE_RELEASE_NET_MS;
	if (!g_open && !pg_link_is_open()) return;
	if (pm_ms() - g_last_use < (unsigned long)idle) return;
	pm_log("net: nothing to do for %ld s: releasing the line", idle / 1000);
	pmn_release_now();
}

void pmn_release_now(void)
{
	if (g_open) imap_logout();
	pmn_close_why(1, "releasing the line");
	pg_link_close();
}
