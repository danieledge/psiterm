/* psishim.c - POSIX-flavoured shims that let unmodified Dropbear code run in
 * psissh.exe on EPOC R5. Built with psicompat.h, so the real estlib functions
 * we need are un-renamed below before use. */

#include "psishared.h"

#undef read
#undef write
#undef close
#undef fprintf
#undef vfprintf
#undef printf
#undef fputs
#undef fputc
#undef putc
#undef putchar
#undef fflush
#undef getc
#undef getchar
#undef fgets
#undef fopen
#undef fclose
#undef vsnprintf
#undef snprintf
#undef mkdir
#undef signal
#undef select

#include "../db/libtomcrypt/src/headers/tomcrypt.h"
#include "zlib.h"
#include <termios.h>
#include <sys/uio.h>
#ifdef PSI_HOST_TEST
#include <sys/ioctl.h>
int psi_tcgetattr(int, struct termios*);
int psi_tcsetattr(int, int, const struct termios*);
#endif

/* from psiglue.cpp */
extern PsiShared* pg_shared(void);
extern int pg_init(void);
extern void pg_close(void);
extern void pg_set_state(int);
extern void pg_set_exit(int);
extern int pg_quit_requested(void);
extern void pg_msleep(int);
extern int pg_serial_write(const void*, int);
extern int pg_net_avail(void);
extern int pg_net_read(void*, int);
extern void pg_net_set_closed(void);
extern int pg_kbd_avail(void);
extern int pg_kbd_read(void*, int);
extern void pg_out_write(const void*, int);
extern void pg_winsize(int*, int*);
extern int pg_take_resize(void);
extern int pg_wait(int, int, int);
extern int pg_dial(char*, int);
extern void pg_hangup(void);
extern int pg_entropy(unsigned char*, int);
extern const char* pg_home(void);

#define PSI_FD_SIGR 51
#define PSI_FD_SIGW 52

static FILE psi_tty_file;                    /* marker for "/dev/tty" */
#define PSI_TTY (&psi_tty_file)
static psi_sighandler_t psi_winch_handler = 0;
static int psi_sig_pending = 0;
static char psi_fmtbuf[4096];
static char psi_pass[256];
static struct passwd psi_pw;

static int is_term_stream(FILE* f)
{
	return f == stdout || f == stderr || f == PSI_TTY;
}

/* terminal output from Dropbear's own messages: '\n' -> "\r\n" */
static void term_out_text(const char* s, int len)
{
	int i, start = 0;
	for (i = 0; i < len; i++) {
		if (s[i] == '\n') {
			if (i > start)
				pg_out_write(s + start, i - start);
			pg_out_write("\r\n", 2);
			start = i + 1;
		}
	}
	if (len > start)
		pg_out_write(s + start, len - start);
}

/* ---------------------------------------------------------------- printf */

int psi_vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
	static char tmp[4096];
	int n = vsprintf(tmp, fmt, ap);
	if (size > 0) {
		size_t c = (n < 0) ? 0 : (size_t)n;
		if (c >= size) c = size - 1;
		memcpy(str, tmp, c);
		str[c] = 0;
	}
	return n;
}

int psi_snprintf(char *str, size_t size, const char *fmt, ...)
{
	va_list ap; int n;
	va_start(ap, fmt);
	n = psi_vsnprintf(str, size, fmt, ap);
	va_end(ap);
	return n;
}

int psi_vfprintf(FILE *f, const char *fmt, va_list ap)
{
	if (is_term_stream(f)) {
		int n = vsprintf(psi_fmtbuf, fmt, ap);
		if (n > 0)
			term_out_text(psi_fmtbuf, n);
		return n;
	}
	return vfprintf(f, fmt, ap);
}

int psi_fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap; int n;
	va_start(ap, fmt);
	n = psi_vfprintf(f, fmt, ap);
	va_end(ap);
	return n;
}

int psi_printf(const char *fmt, ...)
{
	va_list ap; int n;
	va_start(ap, fmt);
	n = psi_vfprintf(stdout, fmt, ap);
	va_end(ap);
	return n;
}

int psi_fputs(const char *s, FILE *f)
{
	if (is_term_stream(f)) {
		term_out_text(s, strlen(s));
		return 0;
	}
	return fputs(s, f);
}

int psi_fputc(int c, FILE *f)
{
	if (is_term_stream(f)) {
		char ch = (char)c;
		term_out_text(&ch, 1);
		return c;
	}
	return fputc(c, f);
}

int psi_fflush(FILE *f)
{
	if (f == NULL || is_term_stream(f))
		return 0;
	return fflush(f);
}

/* ---------------------------------------------------------------- keyboard */

static int key_wait(void)
{
	unsigned char c;
	for (;;) {
		if (pg_kbd_read(&c, 1) == 1)
			return c;
		if (pg_wait(-1, 0, 1) & 8)
			return -1;
	}
}

/* line input for prompts; echo=0 hides what is typed (passwords) */
static char* line_input(char* buf, int max, int echo)
{
	int n = 0;
	for (;;) {
		int c = key_wait();
		if (c < 0)
			return NULL;
		if (c == '\r' || c == '\n') {
			pg_out_write("\r\n", 2);
			break;
		}
		if (c == 8 || c == 127) {
			if (n > 0) {
				n--;
				if (echo)
					pg_out_write("\b \b", 3);
			}
			continue;
		}
		if (c == 3)                        /* Ctrl+C cancels */
			return NULL;
		if (c < 32)
			continue;
		if (n < max - 1) {
			buf[n++] = (char)c;
			if (echo)
				pg_out_write(&buf[n - 1], 1);
			else
				pg_out_write("*", 1);
		}
	}
	buf[n] = 0;
	return buf;
}

/* Saved password from PsiTerm's host list: offered once, at the first
   password prompt. If the server rejects it, the user is asked as usual. */
static char saved_pw[64];
static int saved_pw_used;

char *psi_getpass(const char *prompt)
{
	term_out_text(prompt, strlen(prompt));
	if (!saved_pw_used && saved_pw[0]) {
		const char *note = "(saved password)\n";
		saved_pw_used = 1;
		term_out_text(note, strlen(note));
		strcpy(psi_pass, saved_pw);
		memset(saved_pw, 0, sizeof(saved_pw));
		return psi_pass;
	}
	if (!line_input(psi_pass, sizeof(psi_pass), 0))
		return NULL;
	return psi_pass;
}

int psi_getc(FILE *f)
{
	if (f == stdin || f == PSI_TTY) {
		int c = key_wait();
		if (c < 0)
			return EOF;
		if (c == '\r')
			c = '\n';
		if (c >= 32) {
			char ch = (char)c;
			pg_out_write(&ch, 1);
		}
		pg_out_write("\r\n", 2);
		return c;
	}
	return getc(f);
}

char *psi_fgets(char *buf, int n, FILE *f)
{
	if (f == stdin || f == PSI_TTY) {
		if (!line_input(buf, n - 1, 1))
			return NULL;
		strcat(buf, "\n");
		return buf;
	}
	return fgets(buf, n, f);
}

/* ---------------------------------------------------------------- files */

static void to_epoc_path(const char* in, char* out, int max)
{
	int i;
	for (i = 0; in[i] && i < max - 1; i++) {
#ifdef PSI_HOST_TEST
		out[i] = in[i];
#else
		out[i] = (in[i] == '/') ? '\\' : in[i];
#endif
	}
	out[i] = 0;
}

FILE *psi_fopen(const char *path, const char *mode)
{
	char p[256];
	if (strcmp(path, "/dev/tty") == 0)
		return PSI_TTY;
	if (strncmp(path, "/dev/", 5) == 0)
		return NULL;
	to_epoc_path(path, p, sizeof(p));
	return fopen(p, mode);
}

int psi_fclose(FILE *f)
{
	if (f == PSI_TTY)
		return 0;
	return fclose(f);
}

int psi_mkdir(const char *path, mode_t mode)
{
	char p[256];
	to_epoc_path(path, p, sizeof(p));
	return mkdir(p, mode);
}

/* ---------------------------------------------------------------- fds */

int psi_read(int fd, void *buf, size_t len)
{
	if (fd == 0) {
		int n;
		while ((n = pg_kbd_read(buf, len)) == 0) {
			if (pg_wait(-1, 0, 1) & 8)
				return 0;
		}
		return n;
	}
	if (fd == PSI_FD_NET) {
		int n;
		while ((n = pg_net_read(buf, len)) == 0) {
			if (pg_quit_requested())
				return 0;
			pg_wait(-1, 1, 0);
		}
		return n;
	}
	if (fd == PSI_FD_SIGR) {
		if (psi_sig_pending) {
			psi_sig_pending = 0;
			((char*)buf)[0] = 0;
			return 1;
		}
		errno = EAGAIN;
		return -1;
	}
	return read(fd, buf, len);
}

int psi_write(int fd, const void *buf, size_t len)
{
	if (fd == 1 || fd == 2) {
		pg_out_write(buf, len);
		return len;
	}
	if (fd == PSI_FD_NET)
		return pg_serial_write(buf, len);
	if (fd == PSI_FD_SIGW) {
		psi_sig_pending = 1;
		return len;
	}
	return write(fd, buf, len);
}

int psi_writev(int fd, const struct iovec *iov, int iovcnt)
{
	int i, total = 0;
	for (i = 0; i < iovcnt; i++) {
		int r = psi_write(fd, iov[i].iov_base, iov[i].iov_len);
		if (r < 0)
			return total ? total : -1;
		total += r;
	}
	return total;
}

int psi_close(int fd)
{
	if (fd <= 2 || fd == PSI_FD_NET || fd == PSI_FD_SIGR || fd == PSI_FD_SIGW)
		return 0;
	return close(fd);
}

int psi_shutdown(int fd, int how)
{
	(void)fd; (void)how;
	return 0;
}

int psi_pipe(int fds[2])
{
	fds[0] = PSI_FD_SIGR;
	fds[1] = PSI_FD_SIGW;
	return 0;
}

int psi_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
	int wantNet = r && FD_ISSET(PSI_FD_NET, r);
	int wantKbd = r && FD_ISSET(0, r);
	int wantSig = r && FD_ISSET(PSI_FD_SIGR, r);
	int wantW = 0, i, count = 0, ms, mask;
	(void)n;
	if (w) {
		for (i = 0; i < FD_SETSIZE; i++)
			if (FD_ISSET(i, w)) { wantW = 1; break; }
	}
	ms = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
	if (wantW || (wantSig && psi_sig_pending))
		ms = 0;

	mask = pg_wait(ms, wantNet, wantKbd);

	if ((mask & 4) && pg_take_resize() && psi_winch_handler) {
		psi_winch_handler(SIGWINCH);   /* writes to the signal pipe */
	}
	if (mask & 8)
		pg_net_set_closed();           /* makes the net fd read EOF */

	if (r) {
		FD_ZERO(r);
		if (wantNet && (mask & (1 | 8))) { FD_SET(PSI_FD_NET, r); count++; }
		if (wantKbd && (mask & 2)) { FD_SET(0, r); count++; }
		if (wantSig && psi_sig_pending) { FD_SET(PSI_FD_SIGR, r); count++; }
	}
	if (w && wantW) {
		/* writes never block for long: report every requested fd writable */
		for (i = 0; i < FD_SETSIZE; i++)
			if (FD_ISSET(i, w)) count++;
	}
	if (e)
		FD_ZERO(e);
	return count;
}

/* socket options: nothing to set on a serial link; never reach estlib */
int psi_setsockopt(int fd, int level, int opt, const void *val, socklen_t len)
{
	(void)fd; (void)level; (void)opt; (void)val; (void)len;
	return 0;
}

int psi_getsockopt(int fd, int level, int opt, void *val, socklen_t *len)
{
	(void)fd; (void)level; (void)opt; (void)val; (void)len;
	errno = EINVAL;
	return -1;
}

int psi_getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
	(void)fd; (void)addr; (void)len;
	errno = EINVAL;
	return -1;
}

int psi_getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
	(void)fd; (void)addr; (void)len;
	errno = EINVAL;
	return -1;
}

int psi_fcntl(int fd, int cmd, ...)
{
	(void)fd; (void)cmd;
	return 0;
}

int psi_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	(void)fd;
	if (req == TIOCGWINSZ) {
		struct winsize *ws;
		int rows, cols;
		va_start(ap, req);
		ws = va_arg(ap, struct winsize*);
		va_end(ap);
		pg_winsize(&rows, &cols);
		ws->ws_row = rows;
		ws->ws_col = cols;
		ws->ws_xpixel = 640;
		ws->ws_ypixel = 240;
		return 0;
	}
	return 0;
}

int psi_isatty(int fd)
{
	return fd <= 2;
}

int psi_tcgetattr(int fd, struct termios *t)
{
	(void)fd;
	memset(t, 0, sizeof(*t));
	t->c_iflag = ICRNL | IXON;
	t->c_oflag = OPOST | ONLCR;
	t->c_cflag = CS8;
	t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
	t->c_cc[VINTR] = 3;
	t->c_cc[VQUIT] = 28;
	t->c_cc[VERASE] = 127;
	t->c_cc[VKILL] = 21;
	t->c_cc[VEOF] = 4;
	t->c_cc[VSUSP] = 26;
	t->c_ispeed = t->c_ospeed = 38400;
	return 0;
}

int psi_tcsetattr(int fd, int act, const struct termios *t)
{
	(void)fd; (void)act; (void)t;
	return 0;
}

psi_sighandler_t psi_signal(int sig, psi_sighandler_t h)
{
	if (sig == SIGWINCH)
		psi_winch_handler = h;
	return (psi_sighandler_t)0;
}

int psi_kill(int pid, int sig) { (void)pid; (void)sig; return 0; }
int psi_dup(int fd) { return fd; }
int psi_dup2(int a, int b) { (void)a; return b; }
uid_t psi_getuid(void) { return 0; }
/* EPOC has no user groups: report that, which is what Dropbear checks for */
int psi_getgroups(int n, gid_t *list) { (void)n; (void)list; errno = ENOSYS; return -1; }

struct passwd *psi_getpwuid(uid_t uid)
{
	PsiShared* s = pg_shared();
	(void)uid;
	psi_pw.pw_name = (s && s->user[0]) ? s->user : "psion";
	psi_pw.pw_passwd = "";
	psi_pw.pw_uid = 0;
	psi_pw.pw_gid = 0;
#ifndef PSI_HOST_TEST
	psi_pw.pw_comment = "";
#endif
	psi_pw.pw_gecos = "Psion 5mx";
	psi_pw.pw_dir = (char*)pg_home();
	psi_pw.pw_shell = "/bin/sh";
	return &psi_pw;
}

struct passwd *psi_getpwnam(const char *name)
{
	(void)name;
	return psi_getpwuid(0);
}

/* ---------------------------------------------------------------- entropy */

void psi_add_entropy(void *hsp)
{
	hash_state *hs = (hash_state*)hsp;
	unsigned char buf[1024];
	char path[160];
	FILE* f;
	int n = pg_entropy(buf, sizeof(buf));
	sha256_process(hs, buf, n);
	/* carry randomness across sessions */
	strcpy(path, pg_home());
#ifdef PSI_HOST_TEST
	strcat(path, "/ssh_seed.bin");
#else
	strcat(path, "\\ssh_seed.bin");
#endif
	f = fopen(path, "rb");
	if (f) {
		n = fread(buf, 1, sizeof(buf), f);
		if (n > 0)
			sha256_process(hs, buf, n);
		fclose(f);
	}
}

void psi_save_seed(const unsigned char *pool, int len)
{
	char path[160];
	FILE* f;
	strcpy(path, pg_home());
#ifdef PSI_HOST_TEST
	strcat(path, "/ssh_seed.bin");
#else
	strcat(path, "\\ssh_seed.bin");
#endif
	f = fopen(path, "wb");
	if (f) {
		fwrite(pool, 1, len, f);
		fclose(f);
	}
}

/* ---------------------------------------------------------------- main */

extern int psi_dropbear_main(int argc, char **argv);
extern int psi_bench_case(int which);

static double now_sec(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1000000.0;
}

/* On-device speed test: times the exact crypto an SSH login uses. */
static void run_speed_test(void)
{
	static const struct { int id; const char *name; int bulk; } T[] = {
		{ 3, "SHA-256 test vector", 0 },
		{ 4, "AES-128 test vector", 0 },
		{ 5, "ChaCha20 test vector", 0 },
		{ 6, "Poly1305 test vector", 0 },
		{ 1, "X25519 key agreement", 0 },
		{ 7, "RSA-2048 host key check", 0 },
		{ 2, "Ed25519 host key check", 0 },
		{ 8, "ChaCha20 speed", 1 },
		{ 9, "Poly1305 speed", 1 },
		{ 10, "AES-128-CTR speed", 1 },
		{ 11, "SHA-256 speed", 1 },
		{ 12, "Compression self-test", 0 },
		{ 13, "Decompression speed", 1 },
	};
	double x25519 = 0, rsa = 0, ed = 0;
	int i, fails = 0;
	char line[160];
	{ const char *h = "\r\nPsiTerm SSH speed test (takes about a minute)\r\n\r\n"; pg_out_write(h, strlen(h)); }
	for (i = 0; i < (int)(sizeof(T) / sizeof(T[0])); i++) {
		double t0, dt; int rc, ms;
		sprintf(line, "%-26s ", T[i].name);
		pg_out_write(line, strlen(line));
		/* The Psion clock ticks every 1/64 s, so a single 4 KB pass is too
		   quick to time: repeat bulk cases until at least half a second. */
		{
			int reps = 0;
			t0 = now_sec();
			do {
				rc = psi_bench_case(T[i].id);
				reps++;
				dt = now_sec() - t0;
			} while (T[i].bulk && rc == 0 && dt < 0.5 && reps < 1024);
			ms = (int)(dt * 1000.0 + 0.5);
			if (rc != 0) fails++;
			if (T[i].bulk)
				sprintf(line, "%s  %d KB/s\r\n", rc ? "FAIL" : "ok  ", ms > 0 ? (int)(4000.0 * reps / ms) : 9999);
		else
			sprintf(line, "%s  %d.%03d s\r\n", rc ? "FAIL" : "ok  ", ms / 1000, ms % 1000);
		pg_out_write(line, strlen(line));
		}
		if (T[i].id == 1) x25519 = dt;
		if (T[i].id == 7) rsa = dt;
		if (T[i].id == 2) ed = dt;
		if (pg_quit_requested()) return;
	}
	{
		int r10 = (int)((2 * x25519 + rsa) * 10 + 0.5), e10 = (int)((2 * x25519 + ed) * 10 + 0.5);
		sprintf(line, "\r\nEstimated SSH login maths: %d.%d s (RSA server key), %d.%d s (Ed25519 key)\r\n",
			r10 / 10, r10 % 10, e10 / 10, e10 % 10);
	}
	pg_out_write(line, strlen(line));
	sprintf(line, "%s\r\n", fails ? "SOME TESTS FAILED - please report this screen." : "All crypto self-tests passed.");
	pg_out_write(line, strlen(line));
}

/* ---------------------------------------------------------------- update */
/* Mode 2: download a newer PsiTerm.sis over the same link SSH uses (modem
   "ATDT host:port" or the Psion's own TCP/IP). Plain HTTP/1.0, one request
   per connection. Exit code 10 = new version saved, 0 = already current. */

static int net_read_byte(int timeout_ms)
{
	unsigned char c;
	if (pg_net_avail() == 0) {
		int m = pg_wait(timeout_ms, 1, 0);
		if (m & 8) return -2;                 /* user cancelled */
		if (pg_net_avail() == 0) return -1;   /* timeout or closed */
	}
	pg_net_read(&c, 1);
	return c;
}

static long http_response(const char *path, char *why, int whymax);
static int g_has_crc;                 /* last response had X-CRC32 */
static unsigned long g_crc;           /* ...and its value */
static long g_total = -1;             /* X-Total: whole file size */

/* Sends GET, parses the status and Content-Length. Returns the body length
   (or -1 if unknown) with the link positioned at the first body byte. */
static long http_request(const char *path, char *why, int whymax)
{
	PsiShared *s = pg_shared();
	char req[256];
	if (pg_dial(why, whymax) != 0)
		return -2;
	sprintf(req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PsiTerm\r\nConnection: close\r\n\r\n", path, s->host);
	pg_serial_write(req, strlen(req));
	return http_response(path, why, whymax);
}

/* Reads the status line and headers. Returns Content-Length (-1 if none),
   or -2 with 'why' set on error / non-200. */
static long http_response(const char *path, char *why, int whymax)
{
	char line[256];
	int n = 0, status = 0, c;
	long length = -1;
	(void)whymax;
	g_has_crc = 0;
	g_total = -1;
	for (;;) {                                /* header lines */
		c = net_read_byte(30000);
		if (c < 0) { sprintf(why, c == -2 ? "cancelled" : "no reply from server"); return -2; }
		if (c == '\r') continue;
		if (c != '\n') { if (n < (int)sizeof(line) - 1) line[n++] = (char)c; continue; }
		line[n] = 0;
		if (n == 0 && !status) continue;      /* leftovers of the modem's CONNECT line */
		if (n == 0) break;                    /* blank line: body follows */
		if (!status && !strncmp(line, "HTTP/", 5)) {
			char *sp = strchr(line, ' ');
			status = sp ? atoi(sp + 1) : 0;
		} else if (!strncasecmp(line, "Content-Length:", 15))
			length = atol(line + 15);
		else if (!strncasecmp(line, "X-CRC32:", 8)) {
			g_crc = strtoul(line + 8, NULL, 16);
			g_has_crc = 1;
		} else if (!strncasecmp(line, "X-Total:", 8))
			g_total = atol(line + 8);
		n = 0;
	}
	if (status != 200) {
		sprintf(why, "server said %d for %s", status, path);
		return -2;
	}
	return length;
}

#define CHUNK 16384

/* Reads exactly n body bytes (fewer on timeout/close); -2 if cancelled. */
static int read_body(unsigned char *buf, int n)
{
	int k = 0;
	while (k < n) {
		if (pg_net_avail() == 0) {
			int m = pg_wait(15000, 1, 0);
			if (m & 8) return -2;
			if (pg_net_avail() == 0) break;
		}
		k += pg_net_read(buf + k, n - k);
	}
	return k;
}

static int version_newer(const char *remote, const char *local)
{
	int rm = 0, rn = 0, lm = 0, ln = 0;
	sscanf(remote, "%d.%d", &rm, &rn);
	sscanf(local, "%d.%d", &lm, &ln);
	return rm > lm || (rm == lm && rn > ln);
}

static int run_update(void)
{
	PsiShared *s = pg_shared();
	char why[96], path[96], remote[24], msg[160];
	long len, got = 0;
	int c, n = 0, lastkb = -1;
	FILE *f;

	sprintf(msg, "Checking http://%s:%d%sversion.txt ...\r\n", s->host, s->port, s->path);
	pg_out_write(msg, strlen(msg));
	sprintf(path, "%sversion.txt", s->path);
	len = http_request(path, why, sizeof(why));
	if (len == -2) goto fail;
	while (n < (int)sizeof(remote) - 1 && (len < 0 || n < len)) {
		c = net_read_byte(10000);
		if (c < 0) break;
		if (c == '\r' || c == '\n' || c == ' ') { if (n) break; else continue; }
		remote[n++] = (char)c;
	}
	remote[n] = 0;
	pg_hangup();
	if (!n) { sprintf(why, "version.txt was empty"); goto fail; }
	if (!version_newer(remote, s->version)) {
		sprintf(msg, "PsiTerm %s is the latest version (server has %s).\r\n", s->version, remote);
		pg_out_write(msg, strlen(msg));
		return 0;
	}
	sprintf(msg, "Version %s is available (you have %s). Downloading...\r\n", remote, s->version);
	pg_out_write(msg, strlen(msg));

	/* Chunked download: each 16 KB piece carries a CRC-32 and is retried
	   until it arrives intact, so a dropped or garbled byte costs one chunk,
	   not the whole update. Falls back to one plain GET on old servers. */
	f = fopen(s->save_as, "wb");
	if (!f) { sprintf(why, "cannot write %s", s->save_as); goto fail; }
	{
		static unsigned char chunk[CHUNK];
		long total = -1;
		int tries = 0;
		for (;;) {
			long want, clen;
			int k;
			if (total >= 0 && got >= total) break;
			sprintf(path, "%sPsiTerm.sis?o=%ld&n=%d", s->path, got, CHUNK);
			clen = http_request(path, why, sizeof(why));
			if (clen == -2) {
				if (!strcmp(why, "cancelled") || ++tries > 6) { fclose(f); goto fail; }
				pg_hangup();
				continue;
			}
			if (!g_has_crc || g_total < 0) {  /* old server: this reply is the whole file */
				if (got == 0) { len = clen; goto stream; }
				pg_hangup();
				fclose(f); sprintf(why, "server stopped sending chunks"); goto fail;
			}
			total = g_total;
			want = total - got < CHUNK ? total - got : CHUNK;
			k = (clen == want) ? read_body(chunk, (int)want) : -1;
			pg_hangup();
			if (k == -2) { fclose(f); sprintf(why, "cancelled"); goto fail; }
			if (k != want || (crc32(0L, chunk, (unsigned)want) & 0xffffffffUL) != g_crc) {
				if (++tries > 6) {
					fclose(f);
					sprintf(why, "chunk at %ld kept failing (%d of %ld bytes)", got, k, want);
					goto fail;
				}
				sprintf(msg, "\r  %ld of %ld KB - retrying (%d) ", got / 1024, total / 1024, tries);
				pg_out_write(msg, strlen(msg));
				continue;
			}
			tries = 0;
			if ((long)fwrite(chunk, 1, (size_t)want, f) != want) { fclose(f); sprintf(why, "disk full?"); goto fail; }
			got += want;
			sprintf(msg, "\r  %ld of %ld KB            ", got / 1024, total / 1024);
			pg_out_write(msg, strlen(msg));
		}
		if (total >= 0) {
			fclose(f);
			goto done;
		}
	}
stream:
	for (;;) {
		unsigned char buf[512];
		int k = 0;
		if (len >= 0 && got >= len) break;
		while (k < (int)sizeof(buf) && (len < 0 || got + k < len)) {
			if (pg_net_avail() == 0) {
				int m = pg_wait(k ? 0 : 30000, 1, 0);
				if (m & 8) { fclose(f); pg_hangup(); sprintf(why, "cancelled"); goto fail; }
				if (pg_net_avail() == 0) break;
			}
			{
				int want = (int)sizeof(buf) - k;
				if (len >= 0 && want > len - got - k)
					want = (int)(len - got - k);
				k += pg_net_read(buf + k, want);
			}
		}
		if (k == 0) break;                    /* closed or timed out */
		if ((int)fwrite(buf, 1, k, f) != k) { fclose(f); pg_hangup(); sprintf(why, "disk full?"); goto fail; }
		got += k;
		if ((int)(got / 8192) != lastkb) {
			lastkb = (int)(got / 8192);
			if (len > 0) sprintf(msg, "\r  %ld of %ld KB ", got / 1024, len / 1024);
			else sprintf(msg, "\r  %ld KB ", got / 1024);
			pg_out_write(msg, strlen(msg));
		}
	}
	fclose(f);
	pg_hangup();
	if (len >= 0 && got != len) {
		sprintf(why, "download stopped at %ld of %ld bytes", got, len);
		goto fail;
	}
done:
	sprintf(msg, "\r\nSaved %ld bytes to %s\r\n", got, s->save_as);
	pg_out_write(msg, strlen(msg));
	return 10;

fail:
	sprintf(msg, "\r\nUpdate failed: %s\r\n", why);
	pg_out_write(msg, strlen(msg));
	return 2;
}

/* Mode 3: POST the file 'save_as' (screenshots bundled by PsiTerm) to
   http://host:port<path> in 8 KB chunks, each with a CRC-32 the server
   checks; a chunk that arrives short or damaged is sent again. Over the
   modem the bytes are paced (~5 KB/s): the modem drops data if the Psion
   sends at full 115200 for long. Exit code 11 = accepted by the server. */
#define UCHUNK 8192
static int send_chunk(const char *id, long off, long total, const unsigned char *d, int n, char *why, int whymax)
{
	PsiShared *s = pg_shared();
	char req[320];
	int i, st;
	if (pg_dial(why, whymax) != 0)
		return -1;
	sprintf(req, "POST %s?id=%s&o=%ld&t=%ld HTTP/1.0\r\nHost: %s\r\nUser-Agent: PsiTerm\r\n"
		"Content-Type: application/octet-stream\r\nContent-Length: %d\r\nX-CRC32: %08lx\r\n\r\n",
		s->path, id, off, total, s->host, n, crc32(0L, d, (unsigned)n) & 0xffffffffUL);
	pg_serial_write(req, strlen(req));
	for (i = 0; i < n; i += 256) {
		int k = n - i < 256 ? n - i : 256;
		if (pg_serial_write(d + i, k) != k) { pg_hangup(); sprintf(why, "link dropped"); return -1; }
		if (!s->net_mode && (pg_wait(25, 0, 0) & 8)) { pg_hangup(); sprintf(why, "cancelled"); return -2; }
	}
	st = (http_response(s->path, why, whymax) == -2) ? -1 : 0;
	pg_hangup();
	if (st && strstr(why, "cancelled")) return -2;
	return st;
}

static int run_upload(void)
{
	PsiShared *s = pg_shared();
	char why[96], msg[128], id[24];
	static unsigned char buf[UCHUNK];
	long size, sent = 0;
	unsigned long crc = 0;
	int tries = 0;
	FILE *f = fopen(s->save_as, "rb");
	if (!f) { sprintf(why, "cannot read %s", s->save_as); goto fail; }
	for (;;) {                                /* whole-file CRC names the upload */
		int k = (int)fread(buf, 1, sizeof(buf), f);
		if (k <= 0) break;
		crc = crc32(crc, buf, (unsigned)k);
	}
	size = ftell(f);
	sprintf(id, "%08lx%lx", crc & 0xffffffffUL, size);
	sprintf(msg, "Sending %ld KB to http://%s:%d%s ...\r\n", (size + 1023) / 1024, s->host, s->port, s->path);
	pg_out_write(msg, strlen(msg));
	while (sent < size) {
		int n, r;
		fseek(f, sent, SEEK_SET);
		n = (int)fread(buf, 1, UCHUNK, f);
		if (n <= 0) { fclose(f); sprintf(why, "read error"); goto fail; }
		r = send_chunk(id, sent, size, buf, n, why, sizeof(why));
		if (r == -2) { fclose(f); sprintf(why, "cancelled"); goto fail; }
		if (r != 0) {
			if (++tries > 6) { fclose(f); goto fail; }
			sprintf(msg, "\r  %ld of %ld KB - retrying (%d) ", sent / 1024, size / 1024, tries);
			pg_out_write(msg, strlen(msg));
			continue;
		}
		tries = 0;
		sent += n;
		sprintf(msg, "\r  %ld of %ld KB            ", sent / 1024, size / 1024);
		pg_out_write(msg, strlen(msg));
	}
	fclose(f);
	sprintf(msg, "\r\nSent %ld bytes.\r\n", sent);
	pg_out_write(msg, strlen(msg));
	return 11;

fail:
	sprintf(msg, "\r\nSending failed: %s\r\n", why);
	pg_out_write(msg, strlen(msg));
	return 2;
}

int main(int argc, char **argv)
{
	static char portstr[12];
	static char target[200];
	static char *dargv[12];
	char why[96];
	int r, dargc = 0;
	PsiShared* s;
	(void)argc; (void)argv;

	r = pg_init();
	s = pg_shared();
	if (s) {
		/* take the saved password (if any) and wipe it from shared memory */
		memcpy(saved_pw, s->password, sizeof(saved_pw) - 1);
		saved_pw[sizeof(saved_pw) - 1] = 0;
		memset(s->password, 0, sizeof(s->password));
	}
	if (r != 0) {
		if (s) {
			char msg[80];
			sprintf(msg, "\r\n[psissh: could not open the serial port (%d)]\r\n", r);
			pg_out_write(msg, strlen(msg));
			pg_set_exit(1);
		}
		pg_close();
		return 1;
	}

	if (s->mode == 2 || s->mode == 3) {
		r = (s->mode == 2) ? run_update() : run_upload();
		pg_set_exit(r);
		pg_close();
		return r;
	}

	if (s->mode == 1) {
		run_speed_test();
		pg_set_exit(0);
		pg_close();
		return 0;
	}

	/* estlib environment Dropbear expects */
	setenv("HOME", pg_home(), 1);
	setenv("TERM", "xterm-256color", 1);

	pg_set_state(PSI_STATE_DIALING);
	if (s->net_mode)
		sprintf(psi_fmtbuf, "Connecting to %s:%d over the Psion's Internet connection...\r\n", s->host, s->port);
	else
		sprintf(psi_fmtbuf, "Dialling %s:%d via the modem...\r\n", s->host, s->port);
	pg_out_write(psi_fmtbuf, strlen(psi_fmtbuf));
	if (pg_dial(why, sizeof(why)) != 0) {
		sprintf(psi_fmtbuf, "Could not connect: %s\r\n", why);
		pg_out_write(psi_fmtbuf, strlen(psi_fmtbuf));
		s->lost_link = 2;
		pg_set_exit(2);
		pg_close();
		return 2;
	}
	pg_set_state(PSI_STATE_KEYEX);
	{
		const char *m = "Connected. Setting up encryption - this takes about 5-15 seconds\r\n"
			"on a 5mx, please wait...\r\n";
		pg_out_write(m, strlen(m));
	}

	sprintf(portstr, "%d", s->port > 0 ? s->port : 22);
	sprintf(target, "%s@%s", s->user, s->host);
	dargv[dargc++] = "dbclient";
	dargv[dargc++] = "-p";
	dargv[dargc++] = portstr;
	dargv[dargc++] = "-K";                 /* keepalive: notice a dead link in ~30 s */
	dargv[dargc++] = "10";
	if (s->command[0])
		dargv[dargc++] = "-t";             /* a terminal even with a command */
	dargv[dargc++] = target;
	if (s->command[0]) {
		s->command[sizeof(s->command) - 1] = 0;
		dargv[dargc++] = s->command;
	}
	dargv[dargc] = NULL;

	r = psi_dropbear_main(dargc, dargv);   /* normally exits via dropbear_exit() */
	pg_set_exit(r);
	pg_close();
	return r;
}

/* Called from our patched cli_dropbear_exit before the process ends */
void psi_session_ended(int code, int lost)
{
	PsiShared *s = pg_shared();
	if (s && lost && !s->quit)
		s->lost_link = 1;      /* logged in, then the link failed: PsiTerm may reconnect */
	pg_hangup();
	pg_set_exit(code);
	pg_close();
}

#ifndef PSI_HOST_TEST
/* ---------------------------------------------------------------- stubs */
/* Things estlib lacks that Dropbear references on paths PsiTerm never uses
   (spawning local commands, daemonising, group lists). */
void bzero(void *p, size_t n) { memset(p, 0, n); }
int vfork(void) { errno = ENOSYS; return -1; }
int fork(void) { errno = ENOSYS; return -1; }
int execv(const char *path, char *const argv[]) { (void)path; (void)argv; errno = ENOSYS; return -1; }
int setsid(void) { return -1; }
struct servent *getservbyname(const char *name, const char *proto) { (void)name; (void)proto; return NULL; }
#endif
