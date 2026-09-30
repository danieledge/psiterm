/* emu_mail.c - runs psimail.exe's engine (the same ARM objects, built by the
 * Psion's GCC) in an ARM emulator on a PC, driven by run_psimail.py:
 * pm_* (pmepoc.cpp's jobs) and pg_* (psiglue's) are hypercalls, and main()
 * takes commands from the harness instead of from PsiMail.app. */
#include <stdarg.h>
#include "psimail.h"

int emu_hc(int op, int a, int b, int c, int d);
void emu_mem_report(void);
void pm_do_command(PmCmd *c);
int vsnprintf(char *str, unsigned int size, const char *fmt, va_list ap);
void *memset(void *d, int c, unsigned int n);

enum {
	HM = 400, HM_CONFIG = HM, HM_NEXT, HM_RESULT, HM_MS, HM_TIME, HM_LISTDIR, HM_RMTREE, HM_LOG, HM_MKDIR,
	HP = 300, HP_DIAL = HP, HP_HANGUP, HP_AVAIL, HP_READ, HP_WRITE, HP_WAIT, HP_ENTROPY
};

static PmShared g_sh;

PmShared *pm_shared(void) { return &g_sh; }
PsiShared *pg_shared(void) { return &g_sh.net; }
unsigned long pm_ms(void) { return (unsigned long)emu_hc(HM_MS, 0, 0, 0, 0); }
long pm_time(void) { return emu_hc(HM_TIME, 0, 0, 0, 0); }
int pm_mkdir(const char *p) { return emu_hc(HM_MKDIR, (int)p, 0, 0, 0); }
void pm_rmtree(const char *p) { emu_hc(HM_RMTREE, (int)p, 0, 0, 0); }
void pm_idle(int ms) { (void)ms; }
int unlink(const char *p);
int pm_replace(const char *tmp, const char *path) { unlink(path); return rename(tmp, path); }
long pm_free_kb(const char *path) { (void)path; return -1; }
int pm_write_whole(const char *path, const void *data, long n)
{
	FILE *f = fopen(path, "wb");
	int ok;
	if (!f) return -1;
	ok = (long)fwrite(data, 1, n, f) == n;
	if (fclose(f) != 0) ok = 0;
	if (!ok) unlink(path);
	return ok ? 0 : -1;
}
int pg_rx_errors(int *last) { if (last) *last = 0; return 0; }
void pg_set_link_log(void (*fn)(const char *)) { (void)fn; }

/* the harness writes the names, NUL-separated, into buf */
int pm_list_dir(const char *dir, const char *suffix, void (*cb)(const char *, void *), void *ctx)
{
	static char buf[4096];
	int n = emu_hc(HM_LISTDIR, (int)dir, (int)suffix, (int)buf, sizeof(buf)), i, k = 0;
	for (i = 0; i < n; i++) {
		cb(buf + k, ctx);
		while (buf[k]) k++;
		k++;
	}
	return n;
}

void pm_log(const char *fmt, ...)
{
	char b[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	emu_hc(HM_LOG, (int)b, 0, 0, 0);
}

int pg_dial(char *why, int max) { return emu_hc(HP_DIAL, (int)g_sh.net.host, g_sh.net.port, (int)why, max); }
void pg_hangup(void) { emu_hc(HP_HANGUP, 0, 0, 0, 0); }
void pg_link_close(void) { emu_hc(HP_HANGUP, 0, 0, 0, 0); }
int pg_link_is_open(void) { return 0; }
int pg_net_closed(void) { return 0; }
int pg_net_avail(void) { return emu_hc(HP_AVAIL, 0, 0, 0, 0); }
int pg_net_read(void *b, int m) { return emu_hc(HP_READ, (int)b, m, 0, 0); }
int pg_serial_write(const void *b, int n) { return emu_hc(HP_WRITE, (int)b, n, 0, 0); }
int pg_wait(int ms, int n, int k) { return emu_hc(HP_WAIT, ms, n, k, 0); }
int pg_entropy(unsigned char *o, int m) { return emu_hc(HP_ENTROPY, (int)o, m, 0, 0); }

static int geti(const char *k, int d) { char b[16]; if (!emu_hc(HM_CONFIG, (int)k, (int)b, sizeof(b), 0)) return d; { int v = 0, i = 0, neg = 0; if (b[0] == '-') { neg = 1; i = 1; } for (; b[i]; i++) v = v * 10 + b[i] - '0'; return neg ? -v : v; } }
static void gets_(const char *k, char *out, int max) { emu_hc(HM_CONFIG, (int)k, (int)out, max, 0); }

int main(int argc, char **argv)
{
	static PmCmd c;
	PmAccount *a = &g_sh.acct[0];
	(void)argc; (void)argv;
	memset(&g_sh, 0, sizeof(g_sh));
	g_sh.magic = PM_MAGIC;
	g_sh.net.magic = PSI_SHARED_MAGIC;
	g_sh.net.net_mode = 1;
	gets_("store_dir", g_sh.store_dir, sizeof(g_sh.store_dir));
	gets_("attach_dir", g_sh.attach_dir, sizeof(g_sh.attach_dir));
	g_sh.offline = geti("offline", 0);
	strcpy(g_sh.net.version, "0.3");
	a->used = 1;
	gets_("name", a->name, sizeof(a->name));
	gets_("fullname", a->fullname, sizeof(a->fullname));
	gets_("email", a->email, sizeof(a->email));
	gets_("imap_host", a->imap_host, sizeof(a->imap_host));
	a->imap_port = geti("imap_port", 993);
	a->imap_tls = geti("imap_tls", 1);
	gets_("smtp_host", a->smtp_host, sizeof(a->smtp_host));
	a->smtp_port = geti("smtp_port", 465);
	a->smtp_tls = geti("smtp_tls", 1);
	gets_("user", a->user, sizeof(a->user));
	gets_("pass", a->pass, sizeof(a->pass));
	a->sync_count = geti("sync_count", 50);
	a->max_body_kb = geti("max_body_kb", 64);
	a->save_sent = geti("save_sent", 1);
	g_sh.cal.enabled = 1;
	gets_("cal_host", g_sh.cal.host, sizeof(g_sh.cal.host));
	g_sh.cal.port = geti("cal_port", 443);
	g_sh.cal.plain = geti("cal_plain", 0);
	gets_("cal_path", g_sh.cal.path, sizeof(g_sh.cal.path));
	g_sh.cal.zone = geti("cal_zone", 1);
	g_sh.cal.days_back = geti("cal_back", 30);
	g_sh.cal.days_ahead = geti("cal_ahead", 180);
	for (;;) {
		memset(&c, 0, sizeof(c));
		c.op = emu_hc(HM_NEXT, (int)&c.uid, (int)c.folder, (int)c.arg, 0);
		if (c.op <= 0) break;
		if (c.op == PM_CMD_TRUST && !c.arg[0]) {
			int i;
			for (i = 0; g_sh.trust_host[i]; i++) c.arg[i] = g_sh.trust_host[i];
		}
		pm_do_command(&c);
		emu_hc(HM_RESULT, g_sh.last_res, (int)g_sh.last_msg, (int)g_sh.trust_host, (int)g_sh.trust_fp);
	}
	emu_mem_report();
	return 0;
}

