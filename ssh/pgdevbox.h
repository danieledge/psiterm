/* pgdevbox.h - where PsiProxy's own services are (Send log, and a dev tool's
   builds). That is the shared proxy (PsiLink.ini), unless the proxy is none
   or the Atom modem's own web proxy (192.168.7.1 on its PPP link, or
   "psiproxy" through the modem): that one is right for PsiWeb's pages and
   PsiMail's web pictures but has none of PsiProxy's services, so they go to
   the address in the optional C:\System\Data\PsiDevBox.ini ("<host>\r\n
   <port>\r\n", written by an optional dev tool's Dev box... dialog), and are not
   available when there is none. No address is built in. Plain C, for the
   engines; TPsiLink::DevBox (psilink.h) is the same rule for the apps. */
#ifndef PGDEVBOX_H
#define PGDEVBOX_H

#include <stdio.h>

#define PG_DEVBOX_FILE "C:\\System\\Data\\PsiDevBox.ini"
#define PG_DEVBOX_PORT 8081            /* PsiProxy's standard port */

/* 1 if host is empty or the Atom's own proxy */
static int pg_is_atom_proxy(const char *host)
{
	static const char *const atom[2] = { "192.168.7.1", "psiproxy" };
	int i, k;
	if (!host || !host[0])
		return 1;
	for (i = 0; i < 2; i++) {
		for (k = 0; atom[i][k]; k++) {
			char c = host[k];
			if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
			if (c != atom[i][k]) break;
		}
		if (!atom[i][k] && !host[k])
			return 1;
	}
	return 0;
}

/* PsiDevBox.ini: 1 and the address if it is there and sound */
static int pg_read_devbox(char *host, int hostmax, int *port)
{
	char d[100];
	int n = 0, k, p, v = 0, digits = 0;
	FILE *f = fopen(PG_DEVBOX_FILE, "rb");
	if (!f)
		return 0;
	k = (int)fread(d, 1, sizeof(d) - 1, f);
	fclose(f);
	if (k <= 0)
		return 0;
	d[k] = 0;
	while (n < k && d[n] != '\r' && d[n] != '\n')
		n++;
	if (n == 0 || n >= hostmax || n > 60)
		return 0;
	for (p = 0; p < n; p++)                  /* (a name or an address: no spaces or controls) */
		if ((unsigned char)d[p] <= ' ' || (unsigned char)d[p] > '~')
			return 0;
	p = n;
	while (p < k && (d[p] == '\r' || d[p] == '\n'))
		p++;
	while (p < k && d[p] >= '0' && d[p] <= '9' && digits < 6) {
		v = v * 10 + (d[p++] - '0');
		digits++;
	}
	if (v <= 0 || v >= 65536)
		return 0;
	for (p = 0; p < n; p++)
		host[p] = d[p];
	host[n] = 0;
	*port = v;
	return 1;
}

/* Where PsiProxy's services are, from the shared proxy: 1 and host and port
   set, or 0 if there is nowhere ("Not available - set a proxy ...") */
static int pg_devbox(const char *proxy_host, int proxy_port, const char **host, int *port)
{
	static char devbox[64];
	if (!pg_is_atom_proxy(proxy_host)) {
		*host = proxy_host;
		*port = proxy_port > 0 && proxy_port < 65536 ? proxy_port : PG_DEVBOX_PORT;
		return 1;
	}
	if (!pg_read_devbox(devbox, (int)sizeof(devbox), port))
		return 0;
	*host = devbox;
	return 1;
}

#endif
