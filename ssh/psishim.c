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
#include "tls13.h"
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

/* All update/upload I/O goes through io_read/io_write, which use TLS when
   the source is HTTPS (GitHub) and the raw link otherwise. */
static int g_tls;

static int io_read(unsigned char *buf, int max, int timeout_ms)
{
	if (g_tls)
		return tls_read(buf, max, timeout_ms);   /* >0, 0 closed, -1 error, -2 cancelled */
	if (pg_net_avail() == 0) {
		int m = pg_wait(timeout_ms, 1, 0);
		if (m & 8) return -2;
		if (pg_net_avail() == 0) return -1;
	}
	return pg_net_read(buf, max);
}

static int io_write(const void *buf, int len)
{
	if (g_tls)
		return tls_write(buf, len) == 0 ? len : -1;
	return pg_serial_write(buf, len);
}

static int net_read_byte(int timeout_ms)
{
	unsigned char c;
	int r = io_read(&c, 1, timeout_ms);
	if (r == 1) return c;
	return r == -2 ? -2 : -1;
}

static long http_response(const char *path, char *why, int whymax);
static int g_has_crc;                 /* last response had X-CRC32 (local server) */
static unsigned long g_crc;           /* ...and its value */
static long g_total = -1;             /* whole file size: X-Total or Content-Range */

/* Dials (plus a TLS handshake for HTTPS), sends GET, parses the headers.
   For HTTPS a byte range can be asked for (rlen > 0). Returns the body
   length (-1 if unknown) with the link at the first body byte, or -2. */
static long http_request(const char *path, long rfrom, long rlen, char *why, int whymax)
{
	PsiShared *s = pg_shared();
	char req[384];
	if (pg_dial(why, whymax) != 0)
		return -2;
	if (g_tls && tls_connect(s->host, why, whymax) != 0) {
		pg_hangup();
		return -2;
	}
	sprintf(req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PsiTerm/%s\r\nConnection: close\r\n",
		path, s->host, s->version);
	if (rlen > 0)
		sprintf(req + strlen(req), "Range: bytes=%ld-%ld\r\n", rfrom, rfrom + rlen - 1);
	strcat(req, "\r\n");
	if (io_write(req, strlen(req)) < 0) {
		sprintf(why, "could not send the request");
		pg_hangup();
		return -2;
	}
	{
		/* on any failure - Stop included - hang up here: a modem left
		   online keeps streaming the download into PsiTerm's terminal */
		long r = http_response(path, why, whymax);
		if (r == -2)
			pg_hangup();
		return r;
	}
}

/* Reads the status line and headers. Returns Content-Length (-1 if none),
   or -2 with 'why' set on error / other status than 200 or 206. */
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
		if (c < 0) {
			if (c == -2) sprintf(why, "cancelled");
			else if (g_tls && *tls_error()) sprintf(why, "%.80s", tls_error());
			else sprintf(why, "no reply from server");
			return -2;
		}
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
		else if (!strncasecmp(line, "Content-Range:", 14)) {
			char *sl = strchr(line, '/');     /* bytes a-b/total */
			if (sl) g_total = atol(sl + 1);
		} else if (!strncasecmp(line, "X-CRC32:", 8)) {
			g_crc = strtoul(line + 8, NULL, 16);
			g_has_crc = 1;
		} else if (!strncasecmp(line, "X-Total:", 8))
			g_total = atol(line + 8);
		n = 0;
	}
	if (status != 200 && status != 206) {
		sprintf(why, "server said %d for %.60s", status, path);
		return -2;
	}
	return length;
}

/* Reads exactly n body bytes (fewer on timeout/close); -2 if cancelled. */
static int read_body(unsigned char *buf, int n)
{
	int k = 0;
	while (k < n) {
		int r = io_read(buf + k, n - k, 15000);
		if (r == -2) return -2;
		if (r <= 0) break;
		k += r;
	}
	return k;
}

/* GETs a small text file into buf (NUL-terminated). Returns length or -1. */
static int fetch_small(const char *name, char *buf, int max, char *why, int whymax)
{
	PsiShared *s = pg_shared();
	char path[128];
	long len;
	int k;
	sprintf(path, "%s%s", s->path, name);
	len = http_request(path, 0, 0, why, whymax);
	if (len == -2) return -1;
	if (len < 0 || len > max - 1) len = max - 1;
	k = read_body((unsigned char *)buf, (int)len);
	pg_hangup();
	if (k < 0) { sprintf(why, "cancelled"); return -1; }
	buf[k] = 0;
	return k;
}

static int version_newer(const char *remote, const char *local)
{
	int rm = 0, rn = 0, lm = 0, ln = 0;
	sscanf(remote, "%d.%d", &rm, &rn);
	sscanf(local, "%d.%d", &lm, &ln);
	return rm > lm || (rm == lm && rn > ln);
}

/* ---- release signature
   PsiTerm.sis.sig = "<version>\n<Ed25519 signature, hex>\n" over
   "PsiTerm update\n" <version> "\n" SHA-256(PsiTerm.sis), made with the
   release key (tools/release/sign.py). Nothing is installed unless it
   verifies against this public key. */
#ifdef PSI_HOST_TEST
/* host test build only: test/update-test.key signs the test releases */
static const unsigned char KUpdateKey[32] = { 0xf8,0x33,0x12,0xd1,0xee,0x3f,0x58,0x23,0x7c,0xfb,0x99,0x34,0x29,0x9e,0x1c,0x12,0x9c,0x98,0x16,0xc2,0xfd,0x54,0x0a,0x1b,0x24,0x43,0xeb,0xd0,0xae,0xdd,0xa0,0xd7 };
#else
static const unsigned char KUpdateKey[32] = {
	0x37,0x74,0xc9,0x3a,0xb2,0xf8,0x4b,0x0e,0x07,0x68,0x41,0x34,0x11,0x6c,0xc0,0x3e,
	0x81,0x63,0x90,0xd8,0x00,0x67,0x8f,0x33,0x54,0x38,0x6b,0xf9,0xfd,0x5e,0x9e,0x36 };
#endif

extern int dropbear_ed25519_verify(const unsigned char *m, unsigned long mlen,
	const unsigned char *s, unsigned long slen, const unsigned char *pk);
extern void seedrandom(void);

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Parses the .sig file: returns 0 with version and 64-byte signature. */
static int parse_sig(const char *txt, char *ver, int vermax, unsigned char sig[64])
{
	int i = 0, n = 0;
	while (txt[i] && txt[i] != '\n' && txt[i] != '\r' && n < vermax - 1) ver[n++] = txt[i++];
	ver[n] = 0;
	while (txt[i] == '\r' || txt[i] == '\n') i++;
	for (n = 0; n < 64; n++) {
		int h = hexval(txt[i]), l = h >= 0 ? hexval(txt[i + 1]) : -1;
		if (h < 0 || l < 0) return -1;
		sig[n] = (unsigned char)(h * 16 + l);
		i += 2;
	}
	return ver[0] ? 0 : -1;
}

static int verify_file(const char *file, const char *ver, const unsigned char sig[64])
{
	unsigned char msg[15 + 16 + 1 + 32], buf[1024];
	hash_state h;
	int n = 0, vl = (int)strlen(ver), k;
	FILE *f = fopen(file, "rb");
	if (!f || vl > 16) { if (f) fclose(f); return -1; }
	sha256_init(&h);
	while ((k = (int)fread(buf, 1, sizeof(buf), f)) > 0)
		sha256_process(&h, buf, k);
	fclose(f);
	memcpy(msg, "PsiTerm update\n", 15); n = 15;
	memcpy(msg + n, ver, vl); n += vl;
	msg[n++] = '\n';
	sha256_done(&h, msg + n); n += 32;
	return dropbear_ed25519_verify(msg, n, sig, 64, KUpdateKey);
}

#define CHUNK_LOCAL 16384            /* local server: pieces with a CRC-32 */
#define CHUNK_TLS   49152            /* HTTPS: TLS already checks every record */

static int run_update(void)
{
	PsiShared *s = pg_shared();
	char why[96], path[128], remote[24], msg[160], sigtxt[300], sigver[24];
	unsigned char sig[64];
	long got = 0, total = -1;
	int n, tries = 0, chunk;
	const char *scheme;
	FILE *f;

	g_tls = s->tls;
	scheme = g_tls ? "https" : "http";
	chunk = g_tls ? CHUNK_TLS : CHUNK_LOCAL;
	if (g_tls)
		seedrandom();                         /* TLS needs fresh random numbers */

	sprintf(msg, "Checking %s://%s%s ...\r\n", scheme, s->host, s->path);
	pg_out_write(msg, strlen(msg));
	/* GitHub's CDN caches each file for up to 5 minutes, independently, so
	   right after a release version.txt, the .sig and the .sis can disagree.
	   A query string makes it fetch a fresh copy: a changing one for
	   version.txt, the version for the rest. (Not for a local server.) */
	if (g_tls) sprintf(path, "version.txt?t=%lu", (unsigned long)time(NULL));
	else strcpy(path, "version.txt");
	if (fetch_small(path, remote, sizeof(remote), why, sizeof(why)) < 0) goto fail;
	for (n = 0; remote[n] && remote[n] != '\r' && remote[n] != '\n' && remote[n] != ' '; n++) ;
	remote[n] = 0;
	if (!n) { sprintf(why, "version.txt was empty"); goto fail; }
	if (!version_newer(remote, s->version)) {
		sprintf(msg, "PsiTerm %s is the latest version (server has %s).\r\n", s->version, remote);
		pg_out_write(msg, strlen(msg));
		return 0;
	}
	sprintf(msg, "Version %s is available (you have %s).\r\n", remote, s->version);
	pg_out_write(msg, strlen(msg));

	/* The release files are fetched from the release's git tag
	   (.../v0.30/dist/ instead of .../main/dist/): a tag never changes, so
	   no cache anywhere can hand out a mix of old and new files. */
	if (g_tls) {
		char *m = strstr(s->path, "/main/");
		if (m && strlen(s->path) + strlen(remote) < sizeof(s->path) - 2) {
			char rest[64];
			strcpy(rest, m + 6);
			sprintf(m, "/v%s/%s", remote, rest);
		}
	}
	strcpy(path, "PsiTerm.sis.sig");
	if (fetch_small(path, sigtxt, sizeof(sigtxt), why, sizeof(why)) < 0) {
		sprintf(why, "no release signature on the server (PsiTerm.sis.sig)");
		goto fail;
	}
	if (parse_sig(sigtxt, sigver, sizeof(sigver), sig) != 0) { sprintf(why, "the release signature file is damaged"); goto fail; }
	if (strcmp(sigver, remote) != 0) {
		sprintf(why, "the server's copies are still updating (%.10s vs %.10s) - try again in a few minutes", sigver, remote);
		goto fail;
	}
	pg_out_write("Downloading...\r\n", 16);

	/* In pieces, each held in memory until complete and then written, and
	   retried if it arrives short or damaged: so a dropped byte costs one
	   piece, not the whole download. */
	f = fopen(s->save_as, "wb");
	if (!f) { sprintf(why, "cannot write %s", s->save_as); goto fail; }
	{
		static unsigned char buf[CHUNK_TLS];
		for (;;) {
			long want, clen;
			int k;
			if (total >= 0 && got >= total) break;
			if (g_tls) {
				sprintf(path, "%sPsiTerm.sis", s->path);
				clen = http_request(path, got, chunk, why, sizeof(why));
			} else {
				sprintf(path, "%sPsiTerm.sis?o=%ld&n=%d", s->path, got, chunk);
				clen = http_request(path, 0, 0, why, sizeof(why));
			}
			if (clen == -2) {
				if (!strcmp(why, "cancelled") || ++tries > 6) { fclose(f); goto fail; }
				sprintf(msg, "\r  %ld KB - %.40s, retrying (%d) ", got / 1024, why, tries);
				pg_out_write(msg, strlen(msg));
				continue;
			}
			if (g_total < 0) {
				pg_hangup();
				fclose(f);
				sprintf(why, "server does not support partial downloads");
				goto fail;
			}
			total = g_total;
			want = total - got < chunk ? total - got : chunk;
			k = (clen == want) ? read_body(buf, (int)want) : -1;
			pg_hangup();
			if (k == -2) { fclose(f); sprintf(why, "cancelled"); goto fail; }
			if (k != want || (g_has_crc && (crc32(0L, buf, (unsigned)want) & 0xffffffffUL) != g_crc)) {
				if (++tries > 6) {
					fclose(f);
					sprintf(why, "piece at %ld kept failing (%d of %ld bytes)", got, k, want);
					goto fail;
				}
				sprintf(msg, "\r  %ld of %ld KB - retrying (%d) ", got / 1024, total / 1024, tries);
				pg_out_write(msg, strlen(msg));
				continue;
			}
			tries = 0;
			if ((long)fwrite(buf, 1, (size_t)want, f) != want) { fclose(f); sprintf(why, "disk full?"); goto fail; }
			got += want;
			sprintf(msg, "\r  %ld of %ld KB                    ", got / 1024, total / 1024);
			pg_out_write(msg, strlen(msg));
		}
	}
	fclose(f);
	pg_out_write("\r\nChecking the release signature...\r\n", 37);
	if (verify_file(s->save_as, remote, sig) != 0) {
		remove(s->save_as);
		sprintf(why, "SIGNATURE CHECK FAILED - the download was deleted, nothing installed");
		goto fail;
	}
	sprintf(msg, "Signature OK. Saved %ld bytes to %s\r\n", got, s->save_as);
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

extern int psi_keygen(const char *base, const char *name, char *pub, int pubmax, char *why, int whymax);
extern int psi_keyimport(const char *src, const char *base, const char *name,
	char *pub, int pubmax, char *why, int whymax);
extern int psi_have_key(const char *base, char *path, int max);

/* Mode 4: make a new SSH key (Ed25519). Mode 5: import one from a file. */
static int run_keytool(int import)
{
	PsiShared *s = pg_shared();
	char pub[900], why[120];
	int r;
	const char *m = import ? "Importing the key...\r\n" : "Making a new SSH key (Ed25519)...\r\n";
	pg_out_write(m, strlen(m));
	s->keyfile[sizeof(s->keyfile) - 1] = 0;
	s->keysrc[sizeof(s->keysrc) - 1] = 0;
	s->keyname[sizeof(s->keyname) - 1] = 0;
	r = import ? psi_keyimport(s->keysrc, s->keyfile, s->keyname, pub, sizeof(pub), why, sizeof(why))
		: psi_keygen(s->keyfile, s->keyname, pub, sizeof(pub), why, sizeof(why));
	if (r < 0) {
		sprintf(psi_fmtbuf, "%s failed: %s\r\n", import ? "Import" : "Making the key", why);
		pg_out_write(psi_fmtbuf, strlen(psi_fmtbuf));
		return 1;
	}
	pg_out_write("Done.\r\n", 7);
	return 0;
}

int main(int argc, char **argv)
{
	static char portstr[12];
	static char target[200];
	static char *dargv[14];
	static char keyfile[200];
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
	if (s && (s->mode == 4 || s->mode == 5)) {   /* no serial port needed */
		r = run_keytool(s->mode == 5);
		pg_set_exit(r);
		pg_close();
		return r;
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
	s->keyfile[sizeof(s->keyfile) - 1] = 0;
	if (s->keyfile[0] && psi_have_key(s->keyfile, keyfile, sizeof(keyfile))) {
		dargv[dargc++] = "-i";             /* the login key: tried before the password */
		dargv[dargc++] = keyfile;
	}
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
