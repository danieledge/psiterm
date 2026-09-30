/* pmhost.c - runs PsiMail's engine on a PC, for testing: the same code as
 * psimail.exe, with this file standing in for the Psion (pmepoc.cpp) and
 * pgnet_host.c for the serial line / TCP/IP (real TCP connections).
 *
 *   psimail-host [-s STOREDIR] COMMAND [ARGS] [, COMMAND [ARGS]] ...
 *
 * commands: folders | sync F | older F | body F UID | full F UID |
 *   attach F UID PART | flag F UID +S | move F UID [DEST] | search F WORDS |
 *   send | sendrecv [F] | trust HOST:PORT | trustlast | hangup
 * The account comes from the environment: PM_HOST PM_PORT PM_TLS (0 none,
 * 1 TLS, 2 STARTTLS) PM_SMTP PM_SMTP_PORT PM_SMTP_TLS PM_USER PM_PASS
 * PM_EMAIL PM_NAME PM_SAVE_SENT PM_COUNT PM_BODY_KB. PM_OFFLINE=1 works offline.
 */
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include "../engine/pm.h"

void pm_do_command(PmCmd *c);

static PmShared g_sh;
PmShared *pm_shared(void) { return &g_sh; }
PsiShared *pg_shared(void) { return &g_sh.net; }

unsigned long pm_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000);
}

long pm_time(void)
{
	const char *t = getenv("PM_NOW");       /* tests: pretend it's another time */
	return t ? atol(t) : (long)time(0);
}

int pm_mkdir(const char *path)
{
	char p[300];
	pm_copy(p, path, sizeof(p));
	if (p[0] && p[strlen(p) - 1] == '/') p[strlen(p) - 1] = 0;
	return mkdir(p, 0755) == 0 ? 0 : 0;
}

int pm_list_dir(const char *dir, const char *suffix, void (*cb)(const char *name, void *ctx), void *ctx)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0, sl = (int)strlen(suffix);
	if (!d) return -1;
	while ((e = readdir(d)) != 0) {
		int l = (int)strlen(e->d_name);
		if (e->d_name[0] == '.') continue;
		if (l >= sl && !strcmp(e->d_name + l - sl, suffix)) { cb(e->d_name, ctx); n++; }
	}
	closedir(d);
	return n;
}

void pm_rmtree(const char *dir)
{
	char cmd[400];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd)) {}
}

int pm_replace(const char *tmp, const char *path)
{
	if (getenv("PM_FAIL_REPLACE")) return -1;      /* tests */
	return rename(tmp, path) == 0 ? 0 : -1;
}

long pm_free_kb(const char *path)
{
	struct statvfs v;
	const char *fake = getenv("PM_FREE_KB");       /* tests: pretend */
	char p[300];
	if (fake) return atol(fake);
	pm_copy(p, path, sizeof(p));
	if (statvfs(p, &v) != 0) {
		char *sl = strrchr(p, '/');
		if (!sl) return -1;
		*sl = 0;
		if (statvfs(p[0] ? p : "/", &v) != 0) return -1;
	}
	return (long)((unsigned long long)v.f_bavail * v.f_frsize / 1024);
}

void pm_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[log] ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

void pm_idle(int ms) { usleep(ms * 1000); }

static const char *env(const char *k, const char *d) { const char *v = getenv(k); return v ? v : d; }

static const char *k_res[] = { "OK", "FAILED", "OFFLINE", "CANCELLED", "UNTRUSTED", "NEED_PASS", "LOGIN_FAILED" };

int main(int argc, char **argv)
{
	PmAccount *a = &g_sh.acct[0];
	int i = 1, rc = 0;
	const char *store = "/tmp/psimail-store/";
	if (i + 1 < argc && !strcmp(argv[i], "-s")) { store = argv[i + 1]; i += 2; }
	signal(SIGPIPE, SIG_IGN);
	g_sh.magic = PM_MAGIC;
	g_sh.net.magic = PSI_SHARED_MAGIC;
	g_sh.net.net_mode = 1;
	snprintf(g_sh.store_dir, sizeof(g_sh.store_dir), "%s%s", store, store[strlen(store) - 1] == '/' ? "" : "/");
	snprintf(g_sh.attach_dir, sizeof(g_sh.attach_dir), "%sattachments/", g_sh.store_dir);
	pm_mkdir(g_sh.store_dir);
	g_sh.offline = atoi(env("PM_OFFLINE", "0"));
	pm_copy(g_sh.net.version, env("PM_VERSION", "0.3"), sizeof(g_sh.net.version));
	a->used = 1;
	pm_copy(a->name, "Test", sizeof(a->name));
	pm_copy(a->imap_host, env("PM_HOST", "127.0.0.1"), sizeof(a->imap_host));
	a->imap_port = atoi(env("PM_PORT", "993"));
	a->imap_tls = atoi(env("PM_TLS", "1"));
	pm_copy(a->smtp_host, env("PM_SMTP", a->imap_host), sizeof(a->smtp_host));
	a->smtp_port = atoi(env("PM_SMTP_PORT", "465"));
	a->smtp_tls = atoi(env("PM_SMTP_TLS", "1"));
	pm_copy(a->user, env("PM_USER", "test@example.com"), sizeof(a->user));
	pm_copy(a->pass, env("PM_PASS", ""), sizeof(a->pass));
	pm_copy(a->email, env("PM_EMAIL", a->user), sizeof(a->email));
	pm_copy(a->fullname, env("PM_NAME", "Test User"), sizeof(a->fullname));
	a->sync_count = atoi(env("PM_COUNT", "50"));
	a->max_body_kb = atoi(env("PM_BODY_KB", "64"));
	a->save_sent = atoi(env("PM_SAVE_SENT", "1"));
	g_sh.cal.enabled = 1;
	pm_copy(g_sh.cal.host, env("PM_CAL_HOST", "caldav.fastmail.com"), sizeof(g_sh.cal.host));
	g_sh.cal.port = atoi(env("PM_CAL_PORT", "443"));
	g_sh.cal.plain = atoi(env("PM_CAL_PLAIN", "0"));
	pm_copy(g_sh.cal.path, env("PM_CAL_PATH", ""), sizeof(g_sh.cal.path));
	g_sh.cal.zone = atoi(env("PM_CAL_ZONE", "1"));
	g_sh.cal.days_back = atoi(env("PM_CAL_BACK", "30"));
	g_sh.cal.days_ahead = atoi(env("PM_CAL_AHEAD", "180"));

	while (i < argc) {
		PmCmd c;
		const char *op = argv[i++];
		char *args[4] = { "", "", "", "" };
		int n = 0;
		while (i < argc && strcmp(argv[i], ",")) { if (n < 4) args[n++] = argv[i]; i++; }
		if (i < argc) i++;
		memset(&c, 0, sizeof(c));
		pm_copy(c.folder, args[0], sizeof(c.folder));
		c.uid = (unsigned int)strtoul(args[1], 0, 10);
		if (!strcmp(op, "folders")) c.op = PM_CMD_FOLDERS;
		else if (!strcmp(op, "sync")) c.op = PM_CMD_SYNC;
		else if (!strcmp(op, "older")) c.op = PM_CMD_OLDER;
		else if (!strcmp(op, "body")) c.op = PM_CMD_BODY;
		else if (!strcmp(op, "full")) c.op = PM_CMD_FULLBODY;
		else if (!strcmp(op, "attach")) { c.op = PM_CMD_ATTACH; pm_copy(c.arg, args[2], sizeof(c.arg)); }
		else if (!strcmp(op, "flag")) { c.op = PM_CMD_FLAG; pm_copy(c.arg, args[2], sizeof(c.arg)); }
		else if (!strcmp(op, "move")) { c.op = PM_CMD_MOVE; pm_copy(c.arg, args[2], sizeof(c.arg)); }
		else if (!strcmp(op, "search")) { c.op = PM_CMD_SEARCH; pm_copy(c.arg, args[1], sizeof(c.arg)); c.uid = 0; }
		else if (!strcmp(op, "send")) c.op = PM_CMD_SEND;
		else if (!strcmp(op, "sendrecv")) c.op = PM_CMD_SENDRECV;
		else if (!strcmp(op, "expunge")) c.op = PM_CMD_EXPUNGE;
		else if (!strcmp(op, "cal")) { c.op = PM_CMD_CALSYNC; c.folder[0] = 0; }
		else if (!strcmp(op, "calendars")) { c.op = PM_CMD_CALSYNC; pm_copy(c.arg, "list", sizeof(c.arg)); c.folder[0] = 0; }
		else if (!strcmp(op, "update")) { c.op = PM_CMD_UPDATE; pm_copy(c.arg, args[0], sizeof(c.arg)); pm_copy(c.folder, args[1], sizeof(c.folder)); c.uid = 0; }
		else if (!strcmp(op, "hangup")) c.op = PM_CMD_HANGUP;
		else if (!strcmp(op, "trust")) { c.op = PM_CMD_TRUST; pm_copy(c.arg, args[0], sizeof(c.arg)); c.folder[0] = 0; }
		else if (!strcmp(op, "trustlast")) { c.op = PM_CMD_TRUST; pm_copy(c.arg, g_sh.trust_host, sizeof(c.arg)); c.folder[0] = 0; }
		else { fprintf(stderr, "unknown command %s\n", op); return 2; }
		pm_do_command(&c);
		printf("%s: %s %s\n", op, k_res[g_sh.last_res], g_sh.last_msg);
		if (c.op == PM_CMD_UPDATE && g_sh.update_ready) printf("  ready: %s\n", g_sh.last_file);
		if (g_sh.last_res == PM_RES_UNTRUSTED)
			printf("  untrusted %s: %s\n  key %s\n", g_sh.trust_host, g_sh.trust_why, g_sh.trust_fp);
		if (g_sh.last_res != PM_RES_OK) rc = 1;
		fflush(stdout);
	}
	pmn_release_now();
	return rc;
}
