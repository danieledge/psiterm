/* pgnet_host.c - PC stand-in for the parts of ssh/psiglue.cpp PsiMail uses:
 * pg_dial opens a TCP connection to pg_shared()->host:port (like the Psion's
 * own TCP/IP mode). PM_MODEM=1 adds a fake "NO CARRIER" on close, as the
 * WiRSa does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "../psimail.h"

PsiShared *pg_shared(void);



static int g_fd = -1;
static unsigned char g_rx[4096];
static int g_len, g_pos, g_closed;


void pg_msleep(int ms) { usleep(ms * 1000); }
int pg_quit_requested(void) { return 0; }
void pg_link_close(void) { if (g_fd >= 0) { close(g_fd); g_fd = -1; } fprintf(stderr, "[net] port released\n"); }
int pg_link_is_open(void) { return g_fd >= 0; }

int pg_dial(char *why, int max)
{
	struct addrinfo hints, *ai = NULL;
	char port[16];
	PsiShared *s = pg_shared();
	if (g_fd >= 0) close(g_fd);
	g_fd = -1; g_len = g_pos = 0; g_closed = 0;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_INET;
	snprintf(port, sizeof(port), "%d", s->port);
	fprintf(stderr, "[net] dial %s:%d\n", s->host, s->port);
	if (getaddrinfo(s->host, port, &hints, &ai) != 0 || !ai) {
		snprintf(why, max, "could not look up the host name");
		return -1;
	}
	g_fd = socket(ai->ai_family, SOCK_STREAM, 0);
	if (g_fd < 0 || connect(g_fd, ai->ai_addr, ai->ai_addrlen) != 0) {
		snprintf(why, max, "connection refused");
		freeaddrinfo(ai);
		if (g_fd >= 0) close(g_fd);
		g_fd = -1;
		return -1;
	}
	freeaddrinfo(ai);
	return 0;
}

void pg_hangup(void)
{
	fprintf(stderr, "[net] hang up\n");
	if (g_fd >= 0) close(g_fd);
	g_fd = -1; g_closed = 1;
}

static void fill(int ms)
{
	struct pollfd p;
	int n;
	if (g_fd < 0 || g_closed || g_pos < g_len) return;
	p.fd = g_fd; p.events = POLLIN;
	if (poll(&p, 1, ms) <= 0) return;
	n = recv(g_fd, g_rx, sizeof(g_rx), 0);
	if (n <= 0) {
		g_closed = 1;
		if (getenv("PM_MODEM")) {
			memcpy(g_rx, "\r\nNO CARRIER\r\n", 14);
			g_pos = 0; g_len = 14;
		}
		return;
	}
	g_pos = 0; g_len = n;
}

int pg_net_avail(void) { fill(0); return g_len - g_pos; }
int pg_net_read(void *b, int m)
{
	int n = g_len - g_pos;
	if (n > m) n = m;
	memcpy(b, g_rx + g_pos, n);
	g_pos += n;
	return n;
}
int pg_serial_write(const void *b, int n)
{
	return g_fd >= 0 ? (int)send(g_fd, b, n, 0) : -1;
}
int pg_wait(int ms, int want_net, int want_kbd)
{
	(void)want_kbd;
	if (!want_net) { usleep(ms * 1000); return 0; }
	if (g_pos < g_len || g_closed) return 1;
	fill(ms < 0 ? 60000 : ms);
	return (g_pos < g_len || g_closed) ? 1 : 0;
}

int pg_entropy(unsigned char *o, int m)
{
	FILE *f = fopen("/dev/urandom", "rb");
	int n = f ? (int)fread(o, 1, m, f) : 0;
	if (f) fclose(f);
	return n;
}
