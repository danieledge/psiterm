/* emu_back.c - pwback.h and the psiglue pg_* calls for running psiweb's ARM
 * code in the emulator (run_psiweb.py): the screen is converted with the
 * real pw_grey_convert into a 16-grey buffer the harness saves as
 * screenshots; events, commands and the network are hypercalls. */
#include "psiweb.h"
#include "fb/pwback.h"

char *strcpy(char *d, const char *s);

int emu_hc(int op, int a, int b, int c, int d);
void emu_mem_report(void);
enum {
	HB = 200, HB_OPEN = HB, HB_PRESENT, HB_EVENT, HB_CMD, HB_STATUS, HB_TITLE,
	HB_URL, HB_BUSY, HB_READY, HB_MS, HB_CONFIG, HB_UPDCFG,
	HP = 300, HP_DIAL = HP, HP_HANGUP, HP_AVAIL, HP_READ, HP_WRITE, HP_WAIT, HP_ENTROPY
};

static PwShared g_sh;
static unsigned char g_fb4[PW_MAX_H * PW_STRIDE];

void *pwb_shared(void) { return &g_sh; }
PsiShared *pg_shared(void) { return &g_sh.net; }

int pwb_open(int *w, int *h)
{
	*w = 640; *h = 240;
	/* the harness fills in proxy settings etc. */
	emu_hc(HB_CONFIG, (int)&g_sh.use_proxy, (int)g_sh.proxy_host, (int)&g_sh.proxy_port, (int)&g_sh.net.net_mode);
	emu_hc(HB_UPDCFG, (int)&g_sh.upd_source, (int)g_sh.upd_host, (int)&g_sh.upd_port, (int)g_sh.net.version);
	strcpy(g_sh.net.save_as, "C:\\PsiWeb-update.sis");
	return emu_hc(HB_OPEN, (int)g_fb4, PW_STRIDE, 0, 0);
}
void pwb_close(void) { emu_mem_report(); }
void pwb_present(const unsigned short *fb, int fbw, int x0, int y0, int x1, int y1)
{
	pw_grey_convert(fb, fbw, g_fb4, PW_STRIDE, x0, y0, x1, y1);
	emu_hc(HB_PRESENT, y0, y1, 0, 0);
}
int pwb_next_event(pwb_event *ev, int timeout_ms) { return emu_hc(HB_EVENT, (int)ev, timeout_ms, 0, 0); }
int pwb_take_command(char *arg, int max) { return emu_hc(HB_CMD, (int)arg, max, 0, 0); }
void pwb_set_status(const char *s) { emu_hc(HB_STATUS, (int)s, 0, 0, 0); }
void pwb_set_title(const char *s) { emu_hc(HB_TITLE, (int)s, 0, 0, 0); }
void pwb_set_url(const char *s) { emu_hc(HB_URL, (int)s, 0, 0, 0); }
void pwb_set_busy(int b) { emu_hc(HB_BUSY, b, 0, 0, 0); if (!b) emu_mem_report(); }
void pwb_set_nav(int b, int f) { (void)b; (void)f; }
void pwb_fatal(const char *s) { emu_hc(HB_STATUS, (int)s, 0, 0, 0); }
void pwb_ready(void) { emu_hc(HB_READY, 0, 0, 0, 0); emu_mem_report(); }
const char *pwb_home_url(void) { return "about:welcome"; }
const char *pwb_res_dir(void) { return "/tmp/psiwebemu/"; }
int pwb_load_images(void) { return 1; }
int pwb_zoom(void) { return 100; }
unsigned long pwb_ms(void) { return (unsigned long)emu_hc(HB_MS, 0, 0, 0, 0); }

int pg_dial(char *why, int max) { return emu_hc(HP_DIAL, (int)g_sh.net.host, g_sh.net.port, (int)why, max); }
void pg_hangup(void) { emu_hc(HP_HANGUP, 0, 0, 0, 0); }
void pg_link_close(void) { emu_hc(HP_HANGUP, 0, 0, 0, 0); }
void pw_log(const char *t) { (void)t; }
int pg_link_is_open(void) { return 0; }
int pg_net_closed(void) { return 0; }
int pg_net_avail(void) { return emu_hc(HP_AVAIL, 0, 0, 0, 0); }
int pg_net_read(void *b, int m) { return emu_hc(HP_READ, (int)b, m, 0, 0); }
int pg_serial_write(const void *b, int n) { return emu_hc(HP_WRITE, (int)b, n, 0, 0); }
int pg_wait(int ms, int n, int k) { return emu_hc(HP_WAIT, ms, n, k, 0); }
int pg_entropy(unsigned char *o, int m) { return emu_hc(HP_ENTROPY, (int)o, m, 0, 0); }
