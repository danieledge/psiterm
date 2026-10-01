/* psiglue_host.c - Linux stand-in for psiglue.cpp so the exact same shim and
 * patched Dropbear can be tested on a PC.
 *   serial  -> TCP connection to fakemodem.py (a Hayes-style modem emulator)
 *   keyboard-> this process's stdin (raw)
 *   screen  -> this process's stdout
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "../psishared.h"
#include <signal.h>

static PsiShared g;
static int gSer = -1;
static unsigned char gRx[1024];
static int gRxLen = 0, gRxPos = 0, gNetClosed = 0;
static struct termios gOld;
static int gRaw = 0;

static long long now_us(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000000LL + tv.tv_usec; }

PsiShared* pg_shared(void) { return &g; }

static void restore_tty(void) { if (gRaw) tcsetattr(0, TCSANOW, &gOld); }
/* test hook: SIGUSR1 simulates PsiTerm switching to the large font */
static void on_usr1(int s) { (void)s; g.rows = 21; g.cols = 91; g.resized = 1; }

int pg_init(void)
{
	struct sockaddr_in a;
	const char* h = getenv("PSI_HOST"); const char* u = getenv("PSI_USER"); const char* p = getenv("PSI_PORT");
	memset(&g, 0, sizeof(g));
	g.magic = PSI_SHARED_MAGIC; g.rows = 30; g.cols = 106;
	strcpy(g.host, h ? h : "127.0.0.1"); strcpy(g.user, u ? u : "psitest"); g.port = p ? atoi(p) : 22;
	strcpy(g.dial_prefix, "ATDT");
	if (getenv("PSI_PASS")) strncpy(g.password, getenv("PSI_PASS"), sizeof(g.password) - 1);
	strcpy(g.home, getenv("PSI_HOME") ? getenv("PSI_HOME") : "/tmp/psihome");
	signal(SIGUSR1, on_usr1);
	g.net_mode = getenv("PSI_NET") ? 1 : 0;
	if (getenv("PSI_CMD")) strncpy(g.command, getenv("PSI_CMD"), sizeof(g.command) - 1);
	g.tls = getenv("PSI_TLS") ? 1 : 0;
	if (getenv("PSI_KEY")) strncpy(g.keyfile, getenv("PSI_KEY"), sizeof(g.keyfile) - 1);
	if (getenv("PSI_KEYSRC")) strncpy(g.keysrc, getenv("PSI_KEYSRC"), sizeof(g.keysrc) - 1);
	if (getenv("PSI_KEYNAME")) strncpy(g.keyname, getenv("PSI_KEYNAME"), sizeof(g.keyname) - 1);
	if (getenv("PSI_KEYGEN")) g.mode = 4;
	else if (getenv("PSI_KEYSRC")) g.mode = 5;
	else if (getenv("PSI_UPLOAD")) {
		g.mode = 3;
		strcpy(g.path, "/upload");
		strcpy(g.save_as, getenv("PSI_UPLOAD"));
	} else if (getenv("PSI_UPDATE")) {
		g.mode = 2;
		strcpy(g.path, getenv("PSI_UPDATE"));
		strcpy(g.version, getenv("PSI_VERSION") ? getenv("PSI_VERSION") : "0.1");
		strcpy(g.save_as, getenv("PSI_SAVE") ? getenv("PSI_SAVE") : "/tmp/psiupd.sis");
	}
	if (isatty(0)) { struct termios t; tcgetattr(0, &gOld); t = gOld; cfmakeraw(&t); tcsetattr(0, TCSANOW, &t); gRaw = 1; atexit(restore_tty); }
	if (g.net_mode) return 0;          /* "Psion TCP/IP": socket opens in pg_dial */
	gSer = socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons(7777); a.sin_addr.s_addr = htonl(0x7f000001);
	if (connect(gSer, (struct sockaddr*)&a, sizeof(a)) < 0) return -10;
	return 0;
}
void pg_close(void) { if (gSer >= 0) close(gSer); gSer = -1; restore_tty(); }
void pg_set_state(int s) { g.state = s; }
void pg_dial_verbose(int on) { (void)on; }
void pg_set_exit(int c) { g.exit_code = c; g.state = PSI_STATE_EXITED; }
int pg_quit_requested(void) { return g.quit; }
void pg_msleep(int ms) { usleep(ms * 1000); }
int pg_serial_write(const void* b, int n) { return (int)send(gSer, b, n, 0); }

static void rx_fill(int timeout_us)
{
	struct pollfd pf = { gSer, POLLIN, 0 };
	if (gRxPos < gRxLen) return;
	gRxPos = gRxLen = 0;
	if (poll(&pf, 1, timeout_us / 1000) > 0) {
		int n = recv(gSer, gRx, sizeof(gRx), 0);
		if (n > 0) gRxLen = n; else gNetClosed = 1;
	}
}
int pg_net_avail(void) { return gRxLen - gRxPos; }
int pg_net_read(void* b, int m) { int n = gRxLen - gRxPos; if (n > m) n = m; memcpy(b, gRx + gRxPos, n); gRxPos += n; return n; }
void pg_net_set_closed(void) { gNetClosed = 1; }
int pg_net_closed(void) { return gNetClosed; }

/* keyboard: stdin is pumped into the shared ring, as PsiTerm would do */
static void pump_stdin(int timeout_ms)
{
	struct pollfd pf = { 0, POLLIN, 0 };
	if (poll(&pf, 1, timeout_ms) > 0) {
		unsigned char buf[256]; int n = read(0, buf, sizeof(buf)), i;
		if (n <= 0) { g.quit = 1; return; }
		for (i = 0; i < n; i++) { g.kbd[g.kbd_head % PSI_KBD_SIZE] = buf[i]; g.kbd_head++; }
	}
}
/* file transfer test hook (0.74): PSI_XFER names a script, run once logged
   in, one request at a time, as PsiTerm would post them. Results go to
   PSI_XFER_OUT (default /tmp/psixfer.out). Lines:
     put <psion file> <server file>     get <server file> <psion file>
     list <server folder|.>             stat <server file>
     cancel <bytes>   (Stop the next request once this much has moved)
     quit             (as Disconnect) */
static FILE* gXferIn;
static FILE* gXferOut;
static int gXferBusy, gXferCancelAt = -1, gXferEnd;
static char gXferCmd[600];
static void xfer_tick(void)
{
	char line[1200], a[600], b[600];
	if (!gXferIn) {
		static int tried;
		if (tried || !getenv("PSI_XFER")) return;
		tried = 1;
		gXferIn = fopen(getenv("PSI_XFER"), "r");
		gXferOut = fopen(getenv("PSI_XFER_OUT") ? getenv("PSI_XFER_OUT") : "/tmp/psixfer.out", "w");
		if (!gXferIn || !gXferOut) return;
	}
	if (g.state != PSI_STATE_CONNECTED || gXferEnd) return;
	if (gXferBusy) {
		if (gXferCancelAt >= 0 && g.xfer_done >= (unsigned)gXferCancelAt) { g.xfer_cancel = 1; gXferCancelAt = -1; }
		if (g.xfer_ack != g.xfer_req) return;
		fprintf(gXferOut, "RESULT %s rc=%d done=%u total=%u exists=%d msg=%s path=%s more=%d\n", gXferCmd,
			g.xfer_result, g.xfer_done, g.xfer_total, g.xfer_exists, g.xfer_msg, g.xfer_path, g.xfer_list_more);
		if (g.xfer_op == PSI_XOP_LIST && g.xfer_result == 0) fwrite(g.xfer_list, 1, g.xfer_list_len, gXferOut);
		fflush(gXferOut);
		gXferBusy = 0;
	}
	for (;;) {
		if (!fgets(line, sizeof(line), gXferIn)) { fprintf(gXferOut, "END\n"); fflush(gXferOut); gXferEnd = 1; return; }
		line[strcspn(line, "\r\n")] = 0;
		a[0] = b[0] = 0;
		if (sscanf(line, "cancel %d", &gXferCancelAt) == 1) continue;
		if (!strcmp(line, "quit")) { g.quit = 1; return; }
		if (sscanf(line, "put %599s %599s", a, b) == 2) { g.xfer_op = PSI_XOP_PUT; strcpy(g.xfer_local, a); strcpy(g.xfer_remote, b); }
		else if (sscanf(line, "get %599s %599s", a, b) == 2) { g.xfer_op = PSI_XOP_GET; strcpy(g.xfer_remote, a); strcpy(g.xfer_local, b); }
		else if (sscanf(line, "list %599s", a) == 1) { g.xfer_op = PSI_XOP_LIST; strcpy(g.xfer_remote, strcmp(a, ".") ? a : ""); }
		else if (sscanf(line, "stat %599s", a) == 1) { g.xfer_op = PSI_XOP_STAT; strcpy(g.xfer_remote, a); }
		else continue;
		snprintf(gXferCmd, sizeof(gXferCmd), "%s", line);
		g.xfer_cancel = 0;
		g.xfer_done = 0;
		g.xfer_req++;
		gXferBusy = 1;
		return;
	}
}
int pg_kbd_avail(void) { xfer_tick(); pump_stdin(0); return (int)(g.kbd_head - g.kbd_tail); }
int pg_kbd_read(void* b, int m) { int n = 0; unsigned char* o = b; pump_stdin(0); while (n < m && g.kbd_tail != g.kbd_head) { o[n++] = g.kbd[g.kbd_tail % PSI_KBD_SIZE]; g.kbd_tail++; } return n; }
void pg_out_write(const void* b, int n) { fwrite(b, 1, n, stdout); fflush(stdout); }
void pg_winsize(int* r, int* c) { *r = g.rows; *c = g.cols; }
int pg_take_resize(void) { if (g.resized) { g.resized = 0; return 1; } return 0; }

int pg_wait(int ms, int wantNet, int wantKbd)
{
	long long start = now_us();
	for (;;) {
		int mask = 0;
		if (wantNet && (pg_net_avail() > 0 || gNetClosed)) mask |= 1;
		if (wantKbd && pg_kbd_avail() > 0) mask |= 2;
		if (g.resized) mask |= 4;
		if (g.quit) mask |= 8;
		if (mask) return mask;
		xfer_tick();
		{
			int slice = 30000;
			if (ms >= 0) { long long left = ms * 1000LL - (now_us() - start); if (left <= 0) return 0; if (left < slice) slice = (int)left; if (slice < 1000) slice = 1000; }
			if (wantNet) rx_fill(slice); else pump_stdin(slice / 1000);
		}
	}
}

static int read_line(char* line, int max, int timeout_ms)
{
	int n = 0; long long start = now_us();
	for (;;) {
		char c;
		if (pg_net_avail() == 0) { if (now_us() - start > timeout_ms * 1000LL || gNetClosed) return -1; rx_fill(100000); continue; }
		pg_net_read(&c, 1);
		if (c == '\r' || c == '\n') { if (!n) continue; line[n] = 0; return n; }
		if (n < max - 1) line[n++] = c;
	}
}

int pg_dial(char* why, int max)
{
	char cmd[220], line[160]; int i;
	gNetClosed = 0; gRxPos = gRxLen = 0;
	if (g.net_mode) {
		struct sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons(g.port);
		if (inet_pton(AF_INET, getenv("PSI_ADDR") ? getenv("PSI_ADDR") : g.host, &a.sin_addr) != 1) { snprintf(why, max, "could not look up the host name (error -5120)"); return -1; }
		gSer = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(gSer, (struct sockaddr*)&a, sizeof(a)) < 0) { snprintf(why, max, "connection failed (error -33)"); return -1; }
		return 0;
	}
	/* like psiglue.cpp: wake the modem, then throw away anything pending
	   (e.g. the NO CARRIER from a previous connection) */
	pg_serial_write("\r", 1);
	usleep(300000);
	{ char junk[512]; struct pollfd pf = { gSer, POLLIN, 0 }; while (poll(&pf, 1, 0) > 0 && recv(gSer, junk, sizeof(junk), 0) > 0) ; }
	gRxPos = gRxLen = 0;
	snprintf(cmd, sizeof(cmd), "%s %s:%d\r", g.dial_prefix, g.host, g.port);
	pg_serial_write(cmd, strlen(cmd));
	for (i = 0; i < 10; i++) {
		if (read_line(line, sizeof(line), 30000) < 0) { snprintf(why, max, "no answer from modem"); return -1; }
		if (!strncmp(line, "CONNECT", 7)) return 0;
		if (!strncmp(line, "NO CARRIER", 10) || !strncmp(line, "ERROR", 5) || !strncmp(line, "BUSY", 4)) { snprintf(why, max, "%s", line); return -1; }
	}
	snprintf(why, max, "modem did not connect"); return -1;
}
void pg_hangup(void)
{
	if (g.net_mode) { if (gSer >= 0) { close(gSer); gSer = -1; } return; }
	if (gSer < 0) return;
	usleep(1100000); pg_serial_write("+++", 3);   /* like the device: escape, then ATH */
	usleep(1100000); pg_serial_write("ATH\r", 4);
	usleep(300000);
}
int pg_entropy(unsigned char* o, int m) { FILE* f = fopen("/dev/urandom", "rb"); int n = 0; if (f) { n = fread(o, 1, m > 64 ? 64 : m, f); fclose(f); } return n; }
const char* pg_home(void) { return g.home; }
