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
#include "psiweb.h"

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

extern void pwn_release_now(void);

int main(int argc, char **argv)
{
	static char *args[sizeof(psi_args) / sizeof(psi_args[0]) + 4];
	static char proxy[80];
	PwShared *s = (PwShared *)pwb_shared();
	int n = 0, r;
	(void)argc; (void)argv;
	while (psi_args[n]) args[n] = psi_args[n], n++;
	/* Preferences > Use a proxy (WebOne): every request goes to it as plain
	   HTTP, https:// addresses too (the proxy does the TLS: see
	   get_proxy_string in sched.c) */
	if (s && s->use_proxy && s->proxy_host[0]) {
		snprintf(proxy, sizeof(proxy), "%s:%d", s->proxy_host, s->proxy_port > 0 ? s->proxy_port : 8080);
		args[n++] = "-http-proxy";
		args[n++] = proxy;
	}
	args[n] = NULL;
	r = links_main(n, args);
	pwn_release_now();		/* hang up and give the serial port back */
	return r;
}

/* ------------------------------------------------------------ about: pages */

/* about:welcome, PsiWeb's start page (web/links/welcome.html, built in so
 * the first page needs neither the network nor a file), and about:blank */
static const char psi_welcome[] =
#include "welcome.inc"
;

void psi_about_func(struct connection *c)
{
	struct cache_entry *e;
	const char *page;
	int r, len;
	unsigned char *u = c->url;
	if (!casecmp(u, cast_uchar "about:welcome", 13)) page = psi_welcome;
	else if (!casecmp(u, cast_uchar "about:blank", 11)) page = "<html><body></body></html>";
	else {
		setcstate(c, S_BAD_URL);
		abort_connection(c);
		return;
	}
	if (!c->cache) {
		if (get_connection_cache_entry(c)) {
			setcstate(c, S_OUT_OF_MEM);
			abort_connection(c);
			return;
		}
		c->cache->refcount--;
	}
	e = c->cache;
	if (e->head) mem_free(e->head);
	e->head = stracpy(cast_uchar "\r\nContent-type: text/html\r\n");
	len = (int)strlen(page);
	r = add_fragment(e, 0, (const unsigned char *)page, len);
	if (r < 0) {
		setcstate(c, r);
		abort_connection(c);
		return;
	}
	truncate_entry(e, len, 1);
	c->cache->incomplete = 0;
	setcstate(c, S__OK);
	abort_connection(c);
}

/* ------------------------------------------------------------ stdout, stderr */

#undef fprintf
#undef vfprintf
#undef printf
#undef fflush
#undef perror

extern void pw_log(const char *text);

static char con_line[160];
static int con_len;

/* a whole line: to PsiWeb.log; a fatal one also becomes what the app says
   when the engine stops (pwb_fatal keeps only the first) */
static void con_flush_line(void)
{
	char *p = con_line;
	if (!con_len) return;
	con_line[con_len] = 0;
	con_len = 0;
	while (*p == ' ' || *p == '\007') p++;
	if (!*p) return;
	pw_log(p);
	if (strstr(p, "out of memory"))
		pwb_fatal("Not enough memory for this page - Tools > Restart browser engine starts it again");
	else if (!strncmp(p, "ERROR", 5) || !strncmp(p, "INTERNAL ERROR", 14) || !strncmp(p, "Internal error", 14))
		pwb_fatal(p);
}

static void con_out(const char *s, int n)
{
	for (; n > 0; n--, s++) {
		if (*s == '\n' || *s == '\r') { con_flush_line(); continue; }
		if (*s == '\033') continue;		/* (ANSI bold) */
		if (con_len < (int)sizeof(con_line) - 1) con_line[con_len++] = *s;
	}
}

static int is_con(FILE *f) { return f == stdout || f == stderr; }

int psi_vfprintf(FILE *f, const char *fmt, va_list ap)
{
	char buf[256];
	int n;
	if (!is_con(f)) return vfprintf(f, fmt, ap);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	con_out(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
	return n;
}

int psi_fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = psi_vfprintf(f, fmt, ap);
	va_end(ap);
	return n;
}

int psi_printf(const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = psi_vfprintf(stdout, fmt, ap);
	va_end(ap);
	return n;
}

int psi_fflush(FILE *f)
{
	if (f && !is_con(f)) return fflush(f);
	con_flush_line();
	return 0;
}

void psi_perror(const char *s)
{
	psi_fprintf(stderr, "%s: %d\n", s ? s : "", errno);
}

/* ------------------------------------------------------------ pseudo-descriptors */

#define PSI_FD0	40		/* above any descriptor ESTLIB hands out */
#define PSI_NFD	16
enum { PF_FREE = 0, PF_SOCK, PF_PIPE };

static struct {
	int kind;
	int live;		/* PF_SOCK: still the pwn connection */
	int answered;		/* PF_SOCK: a reply has come on it (so a write
				   now is a kept-alive connection's next request) */
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
		{ void psi_conn_failed(const char *why); psi_conn_failed(msg); }
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
	if (r > 0) { psi_net_bytes_in += r; pfd[i].answered = 1; return r; }
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
	   retries on a new one. Only for a connection that has already
	   answered: on a new one, bytes waiting before the first request are
	   the TLS 1.3 server's NewSessionTicket records (tls_read skips them),
	   and pwn_is_open took them for a closed connection. That threw away
	   nearly every new connection: BBC with pictures made 100 TLS
	   connections (each about 13 million instructions of handshake, and
	   a dial over the modem) where 3 do. */
	if (pfd[i].answered && !pwn_is_open(pfd[i].host, pfd[i].port, pfd[i].tls)) {
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

#define PSI_NET_BATCH_MS	30	/* see psi_select */
#define PSI_SLEEP_MS		20	/* longest sleep with no connection waiting */

extern void pg_msleep(int ms);
extern PsiShared *pg_shared(void);

/* PsiWeb.app has handed over input or a command (it sets net.resized) */
static int psi_woken(void)
{
	PsiShared *ps = pg_shared();
	if (!ps || !ps->resized) return 0;
	ps->resized = 0;
	return 1;
}
#define PSI_NET_BATCH_BYTES	2048

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
	int k, batched = 0;

	(void)ev;
	for (;;) {
		int left, step;
		FD_ZERO(&ro);
		FD_ZERO(&wo);
		if ((k = scan(n, r, w, &ro, &wo)) > 0) {
			/* Over a modem the bytes come a few at a time, and each
			   return from here costs a turn of Links' main loop (timers,
			   this scan, a read call and the protocol's bookkeeping): at
			   10 KB/s that was about 700 instructions a byte, more than
			   formatting the page. If all there is to do is a trickle on
			   the connection, let up to PSI_NET_BATCH_MS of it collect in
			   the link's buffer first (once a call). */
			if (!batched && ms != 0 && k == 1 && live_fd >= 0 && FD_ISSET(live_fd, &ro) &&
			    pfd[live_fd - PSI_FD0].live && !net_eof && !tls_pending() &&
			    pg_net_avail() < PSI_NET_BATCH_BYTES) {
				int rest = ms < 0 ? PSI_NET_BATCH_MS : ms - (int)(pwb_ms() - t0);
				if (rest > PSI_NET_BATCH_MS) rest = PSI_NET_BATCH_MS;
				batched = 1;
				if (rest > 0) {
					pg_wait(rest, 0, 0);
					continue;
				}
			}
			break;
		}
		left = ms < 0 ? 1000 : ms - (int)(pwb_ms() - t0);
		if (left <= 0) break;
		step = left;
		if (live_wanted(n, r)) {
			/* psiglue's waits time out by the Psion's clock (TTime), which
			   counts whole seconds: with nothing arriving, this can take up
			   to a second longer than asked. PsiWeb.app sets net.resized
			   when it hands over a key, a tap or a command, which ends the
			   wait at once (on the modem route; Psion Internet still waits
			   for the second). */
			int m = pg_wait(step, 1, 0);
			if (m & 4) {
				pg_shared()->resized = 0;
				break;
			}
			if (m & 1 && pg_net_avail() == 0 && !tls_pending())
				net_eof = 1;
		} else {
			/* nothing to wait for but time: sleep exactly (pg_wait would
			   wait for the clock's next second) */
			pg_msleep(step < PSI_SLEEP_MS ? step : PSI_SLEEP_MS);
			if (psi_woken()) break;
		}
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
