/* psi_os.c - what Links 2 needs from the system, on EPOC R5 for PsiWeb
 *
 * - main(): Links' main (renamed links_main) with PsiWeb's built-in
 *   settings, as there are no configuration files on the Psion.
 * - psi_select(): Links' select loop (select.c) waits here. The only
 *   descriptor that ever becomes readable is the network connection; input
 *   from PsiWeb.app is polled by the psi driver's timer (psi_drv.c), which
 *   keeps the wait short.
 * - Pseudo-descriptors: a socket is PsiWeb's single network connection,
 *   pwn_* in web/engine/pwnet.c, over psiglue (modem or Psion Internet).
 *   psiglue also resolves the name, and pwn does TLS (ssh/tls13.c), so
 *   Links' DNS, connect() and OpenSSL are not used at all. Only one socket
 *   is live: making a new one ends the old (Links runs with
 *   max-connections 1). A pipe (Links makes one at start) is a dummy.
 *   read/write/close/fcntl/pipe/select are routed here by psicompat.h.
 */
#include "links.h"

#include "pwback.h"
#include "pwnet.h"

#undef read
#undef write
#undef close
#undef fcntl
#undef pipe
#undef select

extern int pg_net_avail(void);
extern int pg_wait(int ms, int want_net, int want_kbd);
extern int tls_pending(void);

/* ------------------------------------------------------------ main */

int links_main(int argc, char *argv[]);

/* Links' settings for a 640x240 screen in 16 greys, a 10 MB heap and one
 * network connection at a time (see web/links/PORTING.md) */
static char *psi_args[] = {
	"psiweb",
	"-async-dns", "0",
	"-max-connections", "1",
	"-max-connections-to-host", "1",
	"-retries", "1",
	"-html-user-font-size", "14",
	"-menu-font-size", "12",
	"-memory-cache-size", "262144",
	"-image-cache-size", "262144",
	"-font-cache-size", "262144",
	"-format-cache-size", "1",
	"-html-g-background-color", "0xffffff",
	"-menu-background-color", "0xffffff",
	"-menu-foreground-color", "0x000000",
	"-dither-images", "0",
	"-dither-letters", "0",
	/* 8-bit gamma tables: the 16-bit ones cost 120 million instructions
	   of soft float (8 seconds) at every start */
	"-gamma-correction", "0",
	"-html-display-images", "0",	/* pictures off: "Show pictures" turns them on */
	NULL
};

int main(int argc, char **argv)
{
	int n = 0;
	(void)argc; (void)argv;
	while (psi_args[n]) n++;
	return links_main(n, psi_args);
}

/* ------------------------------------------------------------ pseudo-descriptors */

#define PSI_FD0	40		/* above any descriptor ESTLIB hands out */
#define PSI_NFD	16
enum { PF_FREE = 0, PF_SOCK, PF_PIPE };

static struct {
	int kind;
	int live;		/* PF_SOCK: still the pwn connection */
	int port, tls;
	char host[128];
} pfd[PSI_NFD];
static int live_fd = -1;
static int net_eof;		/* pg_wait said "net" with nothing to read: closed */
unsigned long psi_net_bytes_in, psi_net_bytes_out, psi_net_connects;

static int pidx(int fd)
{
	fd -= PSI_FD0;
	if (fd < 0 || fd >= PSI_NFD || pfd[fd].kind == PF_FREE) return -1;
	return fd;
}

static int palloc(int kind)
{
	int i;
	for (i = 0; i < PSI_NFD; i++) if (pfd[i].kind == PF_FREE) {
		memset(&pfd[i], 0, sizeof(pfd[i]));
		pfd[i].kind = kind;
		return PSI_FD0 + i;
	}
	errno = EMFILE;
	return -1;
}

static void end_live(void)
{
	if (live_fd >= 0) {
		int i = pidx(live_fd);
		if (i >= 0) pfd[i].live = 0;
		live_fd = -1;
	}
}

int psi_sock_connect(unsigned char *host, int port, int tls, unsigned char **why)
{
	char msg[160];
	int fd, i;

	end_live();
	fd = palloc(PF_SOCK);
	if (fd < 0) return -1;
	snprintf(msg, sizeof(msg), "Connecting to %s", (char *)host);
	pwb_set_status(msg);
	msg[0] = 0;
	if (pwn_connect((const char *)host, port, tls, msg, sizeof(msg)) != 0) {
		pfd[fd - PSI_FD0].kind = PF_FREE;
		if (!msg[0]) snprintf(msg, sizeof(msg), "Could not connect to %s", (char *)host);
		pwb_set_status(msg);
		*why = stracpy(cast_uchar msg);
		errno = ECONNREFUSED;
		return -1;
	}
	i = fd - PSI_FD0;
	pfd[i].live = 1;
	pfd[i].port = port;
	pfd[i].tls = tls;
	snprintf(pfd[i].host, sizeof(pfd[i].host), "%s", (char *)host);
	live_fd = fd;
	net_eof = 0;
	psi_net_connects++;
	return fd;
}

static int sock_readable(int i)
{
	if (!pfd[i].live) return 1;		/* the read says it is over */
	if (net_eof) return 1;
	if (pg_net_avail() > 0) return 1;
	if (pfd[i].tls && tls_pending()) return 1;
	return 0;
}

int psi_read(int fd, void *buf, size_t n)
{
	int i = pidx(fd), r;
	if (i < 0) return read(fd, buf, n);
	if (pfd[i].kind == PF_PIPE) { errno = EAGAIN; return -1; }
	if (!pfd[i].live) { errno = ECONNRESET; return -1; }
	r = pwn_read(buf, (int)n, 30000);
	if (r > 0) { psi_net_bytes_in += r; return r; }
	if (r == 0) { net_eof = 1; return 0; }
	errno = r == PWN_TIMEOUT ? ETIMEDOUT : EINTR;
	return -1;
}

int psi_write(int fd, const void *buf, size_t n)
{
	int i = pidx(fd);
	if (i < 0) return write(fd, buf, n);
	if (pfd[i].kind == PF_PIPE) return (int)n;
	if (!pfd[i].live) { errno = ECONNRESET; return -1; }
	/* a kept-alive connection the server has since closed: Links then
	   retries on a new one */
	if (!pwn_is_open(pfd[i].host, pfd[i].port, pfd[i].tls)) {
		end_live();
		errno = ECONNRESET;
		return -1;
	}
	if (pwn_write(buf, (int)n) != 0) {
		pwn_close(1);
		end_live();
		errno = ECONNRESET;
		return -1;
	}
	psi_net_bytes_out += n;
	return (int)n;
}

int psi_close(int fd)
{
	int i = pidx(fd);
	if (i < 0) return close(fd);
	if (pfd[i].kind == PF_SOCK && pfd[i].live) {
		pwn_close(1);
		live_fd = -1;
	}
	pfd[i].kind = PF_FREE;
	return 0;
}

int psi_fcntl(int fd, int cmd, ...)
{
	(void)fd; (void)cmd;
	return 0;		/* non-blocking: pseudo-descriptors never block long */
}

int psi_pipe(int fd[2])
{
	fd[0] = palloc(PF_PIPE);
	fd[1] = palloc(PF_PIPE);
	if (fd[0] < 0 || fd[1] < 0) {
		if (fd[0] >= 0) psi_close(fd[0]);
		if (fd[1] >= 0) psi_close(fd[1]);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------ the wait */

static int scan(int n, fd_set *r, fd_set *w, fd_set *ro, fd_set *wo)
{
	int fd, k = 0;
	for (fd = 0; fd < n; fd++) {
		int i = pidx(fd);
		if (r && FD_ISSET(fd, r)) {
			if (i < 0 || (pfd[i].kind == PF_SOCK && sock_readable(i))) { FD_SET(fd, ro); k++; }
		}
		if (w && FD_ISSET(fd, w)) {
			/* writes go out as they are made (pwn_write waits for the link) */
			FD_SET(fd, wo); k++;
		}
	}
	return k;
}

static int live_wanted(int n, fd_set *r)
{
	return r && live_fd >= 0 && live_fd < n && FD_ISSET(live_fd, r);
}

int psi_select(int n, void *rv, void *wv, void *ev, void *tvv)
{
	fd_set *r = rv, *w = wv, ro, wo;
	struct timeval *tv = tvv;
	int ms = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
	unsigned long t0 = pwb_ms();
	int k;

	(void)ev;
	for (;;) {
		int left, step;
		FD_ZERO(&ro);
		FD_ZERO(&wo);
		if ((k = scan(n, r, w, &ro, &wo)) > 0) break;
		left = ms < 0 ? 1000 : ms - (int)(pwb_ms() - t0);
		if (left <= 0) break;
		step = left;
		if (live_wanted(n, r)) {
			if (pg_wait(step, 1, 0) & 1 && pg_net_avail() == 0 && !tls_pending())
				net_eof = 1;
		} else
			pg_wait(step, 0, 0);
		if (ms < 0) continue;
	}
	if (r) memcpy(r, &ro, sizeof(fd_set));
	if (w) memcpy(w, &wo, sizeof(fd_set));
	if (ev) FD_ZERO((fd_set *)ev);
	return k;
}

/* ------------------------------------------------------------ the rest */

/* no other programs, signals or terminals on the Psion */
pid_t fork(void) { errno = ENOSYS; return -1; }
int psi_sigaction_stub;
