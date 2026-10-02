/* pwhost.c - PsiWeb backend for testing on a PC
 *
 * Runs the same NetSurf build as the Psion against a 640x240 16-grey buffer
 * and a script instead of a person:
 *
 *   PW_SCRIPT=test.txt ./psiweb-host
 *
 * Script lines:
 *   open <url>        menu File > Open (PW_CMD_OPEN)
 *   cmd <n> [arg]     any other PW_CMD_*
 *   idle [ms]         wait until nothing is loading and the screen has been
 *                     still for ms (default 1500)
 *   wait <ms>
 *   key <nsfb code>   press and release a key (e.g. 281 = Page Down)
 *   type <text>       type characters
 *   pen <x> <y>       tap the screen
 *   shot <file.pgm>   save the screen as the Psion would show it
 *   quit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "pwback.h"
#include "../psiweb.h"

static PwShared g_sh;
void *pwb_shared(void) { return &g_sh; }
unsigned long pwb_ms(void);

#define W 640
#define H 240
#define STRIDE (W / 2)

static unsigned char g_fb[H * STRIDE];
static FILE *g_script;
static int g_busy;
static long long g_last_draw;
static int g_cmd;
static char g_cmd_arg[512];
static pwb_event g_q[16];
static int g_qn;
static long long g_wait_until;
static int g_idle_ms = -1;
static int g_w = W, g_h = H;

static long long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void push(int type, int code, int x, int y)
{
	if (g_qn < 16) {
		g_q[g_qn].type = type; g_q[g_qn].code = code;
		g_q[g_qn].x = x; g_q[g_qn].y = y;
		g_qn++;
	}
}

static void shot(const char *name)
{
	FILE *f = fopen(name, "wb");
	int x, y;
	if (!f) { perror(name); return; }
	fprintf(f, "P5\n%d %d\n255\n", g_w, g_h);
	for (y = 0; y < g_h; y++)
		for (x = 0; x < g_w; x++) {
			int v = g_fb[y * STRIDE + x / 2];
			v = (x & 1) ? v >> 4 : v & 15;
			fputc(v * 17, f);
		}
	fclose(f);
	fprintf(stderr, "[host] saved %s\n", name);
}

int pwb_open(int *w, int *h)
{
	const char *s = getenv("PW_SCRIPT");
	if (getenv("PW_H")) g_h = atoi(getenv("PW_H"));
	*w = g_w; *h = g_h;
	g_sh.magic = PW_MAGIC;
	g_sh.net.magic = PSI_SHARED_MAGIC;
	g_sh.net.net_mode = getenv("PW_MODEM") ? 0 : 1;
	if (getenv("PW_PROXY")) {
		const char *c = strchr(getenv("PW_PROXY"), ':');
		g_sh.use_proxy = 1;
		snprintf(g_sh.proxy_host, sizeof(g_sh.proxy_host), "%.*s",
			c ? (int)(c - getenv("PW_PROXY")) : 60, getenv("PW_PROXY"));
		g_sh.proxy_port = c ? atoi(c + 1) : 8080;
	}
	memset(g_fb, 0xff, sizeof(g_fb));
	g_script = s ? fopen(s, "r") : NULL;
	g_last_draw = now_ms();
	return 0;
}

void pwb_close(void)
{
	if (g_script) fclose(g_script);
}

void pwb_present(const unsigned short *fb, int fbw, int x0, int y0, int x1, int y1)
{
	pw_grey_convert(fb, fbw, g_fb, STRIDE, x0, y0, x1, y1);
	g_last_draw = now_ms();
}

/* runs script lines until one produces an event or has to wait */
static void script_step(void)
{
	char line[600];
	while (g_script && g_qn == 0 && g_cmd == 0) {
		long long t = now_ms();
		if (g_wait_until && t < g_wait_until)
			return;
		g_wait_until = 0;
		if (g_idle_ms >= 0) {
			if (g_busy || t - g_last_draw < g_idle_ms)
				return;
			g_idle_ms = -1;
		}
		if (!fgets(line, sizeof(line), g_script)) {
			fclose(g_script); g_script = NULL;
			push(PWB_QUIT, 0, 0, 0);
			return;
		}
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#') continue;
		fprintf(stderr, "[host] > %s\n", line);
		if (!strncmp(line, "open ", 5)) {
			g_cmd = PW_CMD_OPEN;
			snprintf(g_cmd_arg, sizeof(g_cmd_arg), "%s", line + 5);
			g_busy = 1;         /* until the core says otherwise */
			g_last_draw = t;
			push(PWB_WAKE, 0, 0, 0);
		} else if (!strncmp(line, "cmd ", 4)) {
			char *arg = strchr(line + 4, ' ');
			g_cmd = atoi(line + 4);
			snprintf(g_cmd_arg, sizeof(g_cmd_arg), "%s", arg ? arg + 1 : "");
			g_last_draw = t;
			push(PWB_WAKE, 0, 0, 0);
		} else if (!strncmp(line, "idle", 4)) {
			g_idle_ms = line[4] ? atoi(line + 5) : 1500;
		} else if (!strncmp(line, "wait ", 5)) {
			g_wait_until = t + atoi(line + 5);
		} else if (!strncmp(line, "key ", 4)) {
			int k = atoi(line + 4);
			push(PWB_KEYDOWN, k, 0, 0);
			push(PWB_KEYUP, k, 0, 0);
		} else if (!strncmp(line, "type ", 5)) {
			const char *p;
			for (p = line + 5; *p; p++) {
				push(PWB_KEYDOWN, PWB_UNICODE_BASE + (unsigned char)*p, 0, 0);
				push(PWB_KEYUP, PWB_UNICODE_BASE + (unsigned char)*p, 0, 0);
			}
		} else if (!strncmp(line, "pen ", 4)) {
			int x = 0, y = 0;
			sscanf(line + 4, "%d %d", &x, &y);
			push(PWB_MOVE, 0, x, y);
			push(PWB_KEYDOWN, 401, x, y);      /* NSFB_KEY_MOUSE_1 */
			push(PWB_KEYUP, 401, x, y);
		} else if (!strncmp(line, "shot ", 5)) {
			shot(line + 5);
		} else if (!strcmp(line, "quit")) {
			push(PWB_QUIT, 0, 0, 0);
		}
	}
}

int pwb_next_event(pwb_event *ev, int timeout_ms)
{
	long long end = now_ms() + (timeout_ms < 0 ? 100 : timeout_ms);
	for (;;) {
		script_step();
		if (g_qn) {
			*ev = g_q[0];
			memmove(g_q, g_q + 1, --g_qn * sizeof(g_q[0]));
			return 1;
		}
		if (now_ms() >= end)
			return 0;
		usleep(5000);
	}
}

int pwb_take_command(char *arg, int max)
{
	int c = g_cmd;
	if (c) {
		snprintf(arg, max, "%s", g_cmd_arg);
		g_cmd = 0;
	}
	return c;
}

void pwb_set_status(const char *s) { if (s && *s) fprintf(stderr, "[status] %s\n", s); }
void pwb_set_title(const char *s) { fprintf(stderr, "[title] %s\n", s ? s : ""); }
void pwb_set_url(const char *s) { fprintf(stderr, "[url] %s\n", s ? s : ""); }
void pwb_set_busy(int b)
{
	if (b != g_busy) fprintf(stderr, "[busy] %d\n", b);
	g_busy = b;
	g_last_draw = now_ms();
}
void pwb_set_nav(int b, int f) { (void)b; (void)f; }
void pwb_fatal(const char *s) { fprintf(stderr, "[fatal] %s\n", s); }
int pw_update_run(void) { fprintf(stderr, "[host] no updater in the PC build\n"); return 4; }
void pwb_ready(void) { fprintf(stderr, "[host] ready\n"); }
const char *pwb_home_url(void) { return getenv("PW_HOME") ? getenv("PW_HOME") : "about:blank"; }
const char *pwb_res_dir(void) { return getenv("PW_RES") ? getenv("PW_RES") : "res/"; }
int pwb_load_images(void) { return 1; }
int pwb_zoom(void) { return getenv("PW_ZOOM") ? atoi(getenv("PW_ZOOM")) : 100; }

unsigned long pwb_ms(void) { return (unsigned long)now_ms(); }

/* NetSurf and libnsutils call this instead of gettimeofday (see pwepoc.cpp) */
void pw_log(const char *t) { (void)t; }
int pwb_gettimeofday(struct timeval *tv, void *tz) { (void)tz; return gettimeofday(tv, NULL); }
