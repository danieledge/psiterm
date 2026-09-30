/* pwnet.c - PsiWeb's single network connection, on PsiTerm's psiglue */
#include <string.h>
#include <stdio.h>

#include "pwnet.h"
#include "../psiweb.h"
#ifndef PW_NO_TLS
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
extern unsigned long pwb_ms(void);

#define IDLE_RELEASE_MS 60000       /* give the serial port back after this */
static unsigned long g_last_use;
static void used(void) { g_last_use = pwb_ms(); }

static int  g_open;
static int  g_tls;
static int  g_port;
static char g_host[128];
static char g_tail[16];          /* modem: last bytes, to spot NO CARRIER */
static int  g_dead;

int pwn_modem(void)
{
	return pg_shared() && !pg_shared()->net_mode;
}

int pwn_connect(const char *host, int port, int tls, char *why, int whymax)
{
	PsiShared *s = pg_shared();

	pwn_close(1);
	used();
	snprintf(s->host, sizeof(s->host), "%s", host);
	s->port = port;
	if (pg_dial(why, whymax) != 0)
		return -1;
#ifndef PW_NO_TLS
	if (tls && tls_connect(host, why, whymax) != 0) {
		pg_hangup();
		return -1;
	}
#else
	if (tls) {
		snprintf(why, whymax, "HTTPS is not available in this build");
		pg_hangup();
		return -1;
	}
#endif
	g_open = 1;
	g_dead = 0;
	g_tls = tls;
	g_port = port;
	snprintf(g_host, sizeof(g_host), "%s", host);
	memset(g_tail, 0, sizeof(g_tail));
	return 0;
}

/* The modem reports a dropped connection in-band ("NO CARRIER"); anything
 * arriving while no request is outstanding means the old connection has
 * gone. */
int pwn_is_open(const char *host, int port, int tls)
{
	if (!g_open || g_dead || g_tls != tls || g_port != port || strcmp(g_host, host))
		return 0;
	/* EPOC's pg_net_avail only counts bytes already taken from the port:
	   look for anything the modem has said since the last reply (a
	   "NO CARRIER" when the server dropped the kept-alive connection) */
	if (pwn_modem() && pg_net_avail() == 0)
		pg_wait(2, 1, 0);
	if (pg_net_avail() > 0) {
		char junk[64];
		while (pg_net_avail() > 0)
			pg_net_read(junk, sizeof(junk));
		pwn_close(1);
		return 0;
	}
	return 1;
}

int pwn_write(const void *buf, int len)
{
	used();
	if (!g_open)
		return -1;
#ifndef PW_NO_TLS
	if (g_tls)
		return tls_write(buf, len) == 0 ? 0 : -1;
#endif
	return pg_serial_write(buf, len) == len ? 0 : -1;
}

/* byte search: bodies may contain NULs, so no strstr */
static int find_nc(const unsigned char *b, int n)
{
	int i;
	for (i = 0; i + 10 <= n; i++)
		if (b[i] == 'N' && memcmp(b + i, "NO CARRIER", 10) == 0)
			return i;
	return -1;
}

/* a "NO CARRIER" split across two reads is caught here */
static void note_tail(const unsigned char *b, int n)
{
	int keep = (int)sizeof(g_tail) - 1;
	if (n >= keep) {
		memcpy(g_tail, b + n - keep, keep);
	} else {
		memmove(g_tail, g_tail + n, keep - n);
		memcpy(g_tail + keep - n, b, n);
	}
	if (find_nc((const unsigned char *)g_tail, keep) >= 0)
		g_dead = 1;
}

/* Modem: the WiRSa reports the far end closing (or the link dropping)
 * in-band, with "NO CARRIER" - there is no other sign of it. Cut the
 * message out of the data and remember, so the next read says "closed".
 * Returns how many of the n bytes are real data. */
static int modem_data(unsigned char *b, int n)
{
	int i = find_nc(b, n);
	note_tail(b, n);
	if (i < 0)
		return n;
	g_dead = 1;
	while (i > 0 && (b[i - 1] == '\r' || b[i - 1] == '\n'))
		i--;
	return i;
}

int pwn_read(void *buf, int max, int timeout_ms)
{
	int n;
	used();
	if (!g_open)
		return -1;
	if (g_dead)
		return 0;                           /* the modem said NO CARRIER */
#ifndef PW_NO_TLS
	if (g_tls) {
		if (!tls_pending() && pg_net_avail() == 0) {
			int m = pg_wait(timeout_ms, 1, 0);
			if (m & 8) return PWN_CANCEL;
			if (pg_net_avail() == 0)
				return (m & 1) ? 0 : PWN_TIMEOUT;   /* closed : nothing yet */
		}
		/* a record is on its way: allow for a slow line */
		n = tls_read(buf, max, 20000);
		if (n == -2) return PWN_CANCEL;
		if (n < 0 && pwn_modem() && pg_net_avail() == 0) return 0;
		return n;
	}
#endif
	if (pg_net_avail() == 0) {
		int m = pg_wait(timeout_ms, 1, 0);
		if (m & 8) return PWN_CANCEL;
		if (pg_net_avail() == 0)
			return (m & 1) ? 0 : PWN_TIMEOUT;
	}
	n = pg_net_read(buf, max);
	if (n > 0 && pwn_modem()) {
		n = modem_data((unsigned char *)buf, n);
		if (n == 0)
			return 0;                       /* closed */
	}
	return n;
}

void pwn_close(int hangup)
{
	if (g_open && hangup)
		pg_hangup();
	g_open = 0;
	g_dead = 0;
}

/* Called about once a second from the engine's event loop: after a minute
 * with no fetch using the link, hang up and let go of the serial port so
 * other programs (PsiTerm) can have it. The next fetch dials again. */
void pwn_idle_tick(void)
{
	if (!pg_link_is_open())
		return;
	if (pwb_ms() - g_last_use < IDLE_RELEASE_MS)
		return;
	pwn_release_now();
}

void pwn_release_now(void)
{
	pwn_close(1);
	pg_link_close();
}
