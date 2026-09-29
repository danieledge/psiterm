/* Runs a program in a pty with libvterm as its terminal (answers queries like
   PsiTerm does), types scripted keys, then dumps the screen for render.py. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pty.h>
#include <poll.h>
#include <sys/time.h>
#include "vterm.h"
static int master;
static FILE *rawlog;
static void out_cb(const char *s, size_t len, void *u) { (void)u; write(master, s, len); fprintf(stderr, "[reply %zu bytes: ", len); for (size_t i=0;i<len;i++) fprintf(stderr, (s[i]>=32&&s[i]<127)?"%c":"\\x%02x", (unsigned char)s[i]); fprintf(stderr, "]\n"); }
static long ms(void){ struct timeval t; gettimeofday(&t,0); return t.tv_sec*1000L+t.tv_usec/1000; }
int main(int argc, char **argv) {
	int R = 30, C = 106;
	struct winsize ws = { R, C, 0, 0 };
	pid_t pid = forkpty(&master, NULL, NULL, &ws);
	if (pid == 0) { setenv("TERM", "xterm-256color", 1); execl("/bin/bash", "bash", "-c", argv[1], NULL); _exit(127); }
	VTerm *vt = vterm_new(R, C); vterm_set_utf8(vt, 1);
	vterm_output_set_callback(vt, out_cb, NULL);
	VTermScreen *s = vterm_obtain_screen(vt); vterm_screen_enable_altscreen(s, 1); vterm_screen_reset(s, 1);
	rawlog = fopen("host.raw", "wb");
	/* argv[2..]: "wait_ms:keys" steps; keys support \r \x02 via bash $'' quoting by caller */
	int step = 2; long next = ms() + 1500;
	for (;;) {
		struct pollfd p = { master, POLLIN, 0 };
		if (poll(&p, 1, 50) > 0) { char b[65536]; int n = read(master, b, sizeof b); if (n <= 0) break; fwrite(b,1,n,rawlog); vterm_input_write(vt, b, n); }
		if (ms() >= next) {
			if (step >= argc) break;
			char *k = strchr(argv[step], ':'); int wait = atoi(argv[step]);
			write(master, k + 1, strlen(k + 1)); step++; next = ms() + wait;
		}
	}
	fclose(rawlog);
	vterm_screen_flush_damage(s);
	FILE *o = fopen("host.screen", "w");
	for (int r = 0; r < R; r++) { for (int c = 0; c < C; c++) { VTermScreenCell cell; VTermPos pp = { r, c }; vterm_screen_get_cell(s, pp, &cell); unsigned ch = cell.chars[0]; if (!ch || ch == (unsigned)-1) ch = ' '; char u[8]; int n = 0; if (ch < 0x80) u[n++] = ch; else if (ch < 0x800) { u[n++] = 0xC0|(ch>>6); u[n++] = 0x80|(ch&63);} else { u[n++] = 0xE0|(ch>>12); u[n++] = 0x80|((ch>>6)&63); u[n++] = 0x80|(ch&63);} fwrite(u,1,n,o);} fputc('\n', o); }
	fclose(o);
	return 0;
}
