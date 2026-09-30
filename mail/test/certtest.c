/* certtest.c - checks certcheck.c's chain verification on a saved chain:
 *   certtest CHAIN.der-list HOST [NOW]
 * (the chain as a TLS 1.3 Certificate message body, made by certtest.py) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/pm.h"
const char *tlsv_certificate(const unsigned char *msg, int len);
void tlsv_start(void);
static PmShared g;
PmShared *pm_shared(void) { return &g; }
#include <sys/stat.h>
PsiShared *pg_shared(void) { return &g.net; }
unsigned long pm_ms(void) { return 0; }
int pm_mkdir(const char *p) { mkdir(p, 0755); return 0; }
int pm_list_dir(const char *d, const char *s, void (*cb)(const char *, void *), void *c) { return 0; }
void pm_log(const char *f, ...) {}
void pm_idle(int ms) {}
void pm_rmtree(const char *d) {}
long pm_time(void) { return getenv("PM_NOW") ? atol(getenv("PM_NOW")) : 1790700000L; }
int main(int argc, char **argv)
{
	static unsigned char buf[20000];
	FILE *f = fopen(argv[1], "rb");
	int n = (int)fread(buf, 1, sizeof(buf), f), i;
	const char *e;
	fclose(f);
	strcpy(g.store_dir, argc > 3 ? argv[3] : "/tmp/certtest/");
	for (i = 0; i < 2; i++) {
		tlsv_set_host(argv[2], 993);
		tlsv_start();
		e = tlsv_certificate(buf, n);
		printf("%s: %s %s\n", argv[2], e ? e : "trusted", tlsv_problem());
	}
	return e ? 1 : 0;
}
