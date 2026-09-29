/* pmnet.c - PsiMail's one network connection, on PsiTerm's psiglue
 *
 * As in PsiWeb: the Psion reaches the network through the serial port -
 * a WiFi modem that turns "ATDT host:port" into one TCP connection, or
 * EPOC's own dial-up (PPP) stack - so there is one connection at a time.
 * TLS is PsiTerm's TLS 1.3 client (ssh/tls13.c), here with the server's
 * certificate checked (certcheck.c).
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

#define IDLE_RELEASE_MS 90000

static unsigned long g_last_use;
static int  g_open, g_tls, g_dead;
static char g_tail[16];
static unsigned char g_buf[2048];
static int g_bpos, g_blen;

static int g_connid;

static void used(void) { g_last_use = pm_ms(); }

/* changes with every new connection, so IMAP can tell whether the open
   connection is still its own (SMTP uses the same line) */
int pmn_conn_id(void) { return g_open ? g_connid : -1; }

static int modem(void) { return pg_shared() && !pg_shared()->net_mode; }

int pmn_connect(const char *host, int port, int tls, char *why, int whymax)
{
	PsiShared *s = pg_shared();
	pmn_close(1);
	used();
	pm_copy(s->host, host, sizeof(s->host));
	s->port = port;
	pm_progress("Connecting to %s...", host);
	if (pg_dial(why, whymax) != 0)
		return -1;
	g_bpos = g_blen = 0;
	g_open = 1;
	g_connid++;
	g_dead = 0;
	g_tls = 0;
	memset(g_tail, 0, sizeof(g_tail));
	pm_shared()->online = 1;
	if (tls) {
		if (pmn_starttls(host, why, whymax) != 0) { pmn_close(1); return -1; }
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

static void note_tail(const unsigned char *b, int n)
{
	int keep = (int)sizeof(g_tail) - 1;
	if (n >= keep) memcpy(g_tail, b + n - keep, keep);
	else { memmove(g_tail, g_tail + n, keep - n); memcpy(g_tail + keep - n, b, n); }
	g_tail[keep] = 0;
	if (strstr(g_tail, "NO CARRIER")) g_dead = 1;
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
			if (pg_net_avail() == 0) return (m & 1) ? 0 : PMN_TIMEOUT;
		}
		n = tls_read(buf, max, 30000);
		if (n == -2) return PMN_CANCEL;
		if (n < 0 && modem() && pg_net_avail() == 0) return 0;
		return n;
	}
#endif
	if (pg_net_avail() == 0) {
		int m = pg_wait(timeout_ms, 1, 0);
		if (m & 8) return PMN_CANCEL;
		if (pg_net_avail() == 0) return (m & 1) ? 0 : PMN_TIMEOUT;
	}
	n = pg_net_read(buf, max);
	if (n > 0 && modem()) note_tail((const unsigned char *)buf, n);
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
		if (g_dead) return 0;
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

void pmn_close(int hangup)
{
	if (g_open && hangup) pg_hangup();
	g_open = 0;
	g_dead = 0;
	g_tls = 0;
	g_bpos = g_blen = 0;
	pm_shared()->online = 0;
}

/* Called about once a second: after a while with nothing to do, hang up and
   let go of the serial port so other programs (PsiTerm, PsiWeb) can have it. */
void pmn_idle_tick(void)
{
	if (!g_open && !pg_link_is_open()) return;
	if (pm_ms() - g_last_use < IDLE_RELEASE_MS) return;
	pmn_release_now();
}

void pmn_release_now(void)
{
	if (g_open) imap_logout();
	pmn_close(1);
	pg_link_close();
}
