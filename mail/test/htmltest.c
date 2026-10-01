/* htmltest.c - the HTML converter (engine/html.c) on a PC, for htmltest.py
 *
 *   htmltest FILE [SPLIT]   FILE converted as the engine does, to stdout;
 *                           fed SPLIT bytes at a time (default 977), as
 *                           the message arrives in pieces
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../engine/pm.h"

void pm_copy(char *dst, const char *src, int max)
{
	int i = 0;
	if (max <= 0) return;
	if (src) while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
	dst[i] = 0;
}
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int pm_strcasecmp(const char *a, const char *b)
{
	while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; }
	return lc((unsigned char)*a) - lc((unsigned char)*b);
}
int pm_strncasecmp(const char *a, const char *b, int n)
{
	while (n > 0 && *a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; n--; }
	if (n == 0) return 0;
	return lc((unsigned char)*a) - lc((unsigned char)*b);
}
const char *pm_stristr(const char *hay, const char *needle)
{
	int n = (int)strlen(needle);
	if (!n) return hay;
	for (; *hay; hay++) if (!pm_strncasecmp(hay, needle, n)) return hay;
	return 0;
}
long pm_time(void) { return (long)time(0); }
void pm_log(const char *fmt, ...) { (void)fmt; }

static void sink(const char *s, int n, void *ctx) { fwrite(s, 1, n, (FILE *)ctx); }

int main(int argc, char **argv)
{
	FILE *f;
	static char buf[1 << 20];
	long n, pos;
	int split = argc > 2 ? atoi(argv[2]) : 977;
	HtmlConv *h;
	if (argc < 2 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: htmltest FILE [SPLIT]\n"); return 2; }
	n = (long)fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (split <= 0) split = 977;
	h = html_new();
	for (pos = 0; pos < n; pos += split)
		html_feed(h, buf + pos, (int)(n - pos < split ? n - pos : split), sink, stdout);
	html_end(h, sink, stdout);
	html_free(h);
	return 0;
}
