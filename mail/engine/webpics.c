/* webpics.c - pictures from the web in a message, only when asked for
 *
 * PsiMail fetches nothing from the web unasked: a newsletter's pictures
 * would tell the sender when and where it was read. When the user asks
 * (the reader's "Show them" line, Message > Show web pictures, or the
 * "Web pictures: Always" preference), PM_CMD_WEBPICS fetches the pictures
 * the message's HTML points at, over the engine's one line (pmnet.c, TLS
 * for https), and sets them out as pictures.c does its parts:
 *
 *   <folder>\<uid>_W<hash>.img   one picture as it came, while it waits
 *   <folder>\<uid>_W<hash>.pmi   the same in 16 greys (pictures.c), or why not
 *
 * <hash> is FNV-1a (32 bits, 8 hex digits) of the address as the message's
 * text has it (html.c: "\x01i" alt "\x02" src ["\x02" w "x" h]); the app
 * (app/pmwebpic.cpp) works out the same name to find it.
 *
 * Limits: WEB_MAX pictures a message, each at most WEB_MAX_KB, WEB_TOTAL_KB
 * in all, WEB_TIME_MS for the lot; anything that says it is 1x1 (or less
 * than 4 pixels either way) or looks like a spacer or a tracking pixel by
 * its address is not fetched; one that turns out that small is marked
 * "spacer" (the app shows nothing for it). No cookies, no Referer, no
 * password: a plain GET. A server whose certificate can't be checked is
 * skipped (no question asked: these are not the user's servers).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm.h"
#include "../../ssh/pgdevbox.h"

#define WEB_MAX       16
#define WEB_MAX_KB    300
#define WEB_TOTAL_KB  1536
#define WEB_TIME_MS   (240 * 1000L)
#define WEB_URL_MAX   400

/* the HTTP proxy does the upstream TLS and the 16-grey GIF transcoding for
   us.  Since 0.87 it is the one shared by PsiTerm, PsiMail and PsiWeb
   (Connection settings, PsiLink.ini): the app copies it into the shared
   chunk (use_proxy, proxy_host, proxy_port). */
#define PROXY_ON(s)   ((s)->use_proxy && (s)->proxy_host[0] && (s)->proxy_port > 0)

typedef struct { char url[WEB_URL_MAX]; int w, h; } WebPic;

unsigned long web_hash(const char *s)
{
	unsigned long h = 2166136261UL;
	while (*s) {
		h ^= (unsigned char)*s++;
		h = (h * 16777619UL) & 0xffffffffUL;
	}
	return h;
}

static void web_path(int acct, const char *folder, unsigned int uid, const char *url, const char *ext, char *out, int max)
{
	char d[160];
	st_folder_dir(acct, folder, d, sizeof(d));
	snprintf(out, max, "%s%u_W%08lX.%s", d, uid, web_hash(url), ext);
}

static long fsize(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return n;
}

/* a spacer or a tracking pixel, by its address or its size */
int web_is_spacer(const char *url, int w, int h)
{
	static const char *k_names[] = { "spacer", "pixel.gif", "pixel.png", "/blank.gif", "clear.gif", "transparent.gif",
		"/open.aspx", "/open?", "/o.gif", "/track/open", "/wf/open", "beacon", "/e2t/o/", 0 };
	int i;
	if ((w > 0 && w < 4) || (h > 0 && h < 4)) return 1;
	for (i = 0; k_names[i]; i++)
		if (pm_stristr(url, k_names[i])) return 1;
	return 0;
}

/* the message's web pictures, from its text (in order, each once) */
int web_list(int acct, const char *folder, unsigned int uid, WebPic *out, int max)
{
	char path[190];
	static char line[700];
	FILE *f;
	int n = 0, i;
	st_msg_path(acct, folder, uid, "txt", path, sizeof(path));
	if (!(f = fopen(path, "rb"))) return 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		char *src, *size, *e;
		int w = 0, h = 0, dup = 0;
		if (line[0] != 0x01 || line[1] != 'i') continue;
		line[strcspn(line, "\r\n")] = 0;
		if (!(src = strchr(line + 2, 0x02))) continue;
		src++;
		if ((size = strchr(src, 0x02)) != 0) {
			*size++ = 0;
			w = atoi(size);
			if ((e = strchr(size, 'x')) != 0) h = atoi(e + 1);
		}
		if (pm_strncasecmp(src, "http://", 7) && pm_strncasecmp(src, "https://", 8)) continue;
		if ((int)strlen(src) >= WEB_URL_MAX) continue;
		if (web_is_spacer(src, w, h)) continue;
		for (i = 0; i < n; i++) if (!strcmp(out[i].url, src)) dup = 1;
		if (dup) continue;
		pm_copy(out[n].url, src, sizeof(out[n].url));
		out[n].w = w;
		out[n].h = h;
		n++;
	}
	fclose(f);
	return n;
}

/* ------------------------------------------------------------ HTTP GET */

typedef struct { char host[80]; int port, tls; char path[WEB_URL_MAX]; } Url;

static int parse_url(const char *u, Url *o)
{
	const char *p, *slash, *colon;
	int n;
	memset(o, 0, sizeof(*o));
	if (!pm_strncasecmp(u, "https://", 8)) { o->tls = 1; o->port = 443; p = u + 8; }
	else if (!pm_strncasecmp(u, "http://", 7)) { o->port = 80; p = u + 7; }
	else return -1;
	slash = strchr(p, '/');
	n = slash ? (int)(slash - p) : (int)strlen(p);
	colon = memchr(p, ':', n);
	if (colon) { o->port = atoi(colon + 1); n = (int)(colon - p); }
	if (n <= 0 || n >= (int)sizeof(o->host) || o->port <= 0) return -1;
	memcpy(o->host, p, n);
	o->host[n] = 0;
	pm_copy(o->path, slash ? slash : "/", sizeof(o->path));
	{
		/* (no fragment, no spaces on the request line) */
		char *h = strchr(o->path, '#');
		int i;
		if (h) *h = 0;
		for (i = 0; o->path[i]; i++) if (o->path[i] == ' ') o->path[i] = '+';
	}
	return 0;
}

static char g_host[80];
static int g_port = -1, g_tls, g_conn = -1;
static int g_proxy_down;                     /* this run: the proxy did not answer, so go direct */

/* GETs url into path: PM_RES_OK, or FAILED with why (this picture only),
   or CANCELLED / OFFLINE (the line: stop) */
static int get(const char *url0, const char *path, long max, long *got, char *why, int whymax)
{
	static char line[600], req[1024];
	static unsigned char buf[2048];
	char url[WEB_URL_MAX];
	int hops;
	int proxy = (pm_shared()->web_pic_proxy && PROXY_ON(pm_shared()) && !g_proxy_down) ? 1 : 0;   /* the preference, and a proxy to use */
	*got = 0;
	pm_copy(url, url0, sizeof(url));
	for (hops = 0; hops < 4; hops++) {
		Url u;
		int status = 0, chunked = 0, close_after = 0, attempt, image = -1, direct = 0;
		long clen = -1;
		char loc[WEB_URL_MAX];
		FILE *f;
		loc[0] = 0;
		if (parse_url(url, &u) != 0) { snprintf(why, whymax, "not a web address"); return PM_RES_FAILED; }
		/* where we actually connect: the proxy, or the origin.  With the proxy
		   on, the hop is always plain HTTP (the proxy does the upstream TLS). */
		{
		const char *chost = proxy ? pm_shared()->proxy_host : u.host;
		int cport = proxy ? pm_shared()->proxy_port : u.port;
		int ctls  = proxy ? 0 : u.tls;
		for (attempt = 0; attempt < 2; attempt++) {
			int reused = g_conn >= 0 && pmn_is_open() && pmn_conn_id() == g_conn &&
				g_port == cport && g_tls == ctls && !strcmp(g_host, chost);
			if (!reused) {
				if (pm_shared()->offline) { snprintf(why, whymax, "Working offline"); return PM_RES_OFFLINE; }
				pm_progress("Connecting to %s...", u.host);
				if (pmn_connect(chost, cport, ctls, why, whymax) != 0) {
					g_conn = -1;
					if (pm_cancelled()) return PM_RES_CANCELLED;
					if (proxy) {
						/* the proxy is down or gone: straight to the web instead,
						   for the rest of this run too (a failure is not cached) */
						pm_log("web picture: the proxy %s:%d did not answer (%s) - fetching directly", chost, cport, why);
						proxy = 0;
						g_proxy_down = 1;
						direct = 1;
						break;
					}
					if (tlsv_problem()[0]) snprintf(why, whymax, "%s: %s", u.host, tlsv_problem());
					pm_log("web picture: could not connect to %s:%d: %s", chost, cport, why);
					return PM_RES_FAILED;
				}
				g_conn = pmn_conn_id();
				pm_copy(g_host, chost, sizeof(g_host));
				g_port = cport;
				g_tls = ctls;
			}
			if (proxy) {
				/* an absolute-URL request, always http:// so the proxy does the
				   upstream TLS; the origin stays in the Host header.  The port
				   goes in the URL only when it is not the origin scheme's default
				   (80 for http, 443 for https). */
				int def = u.tls ? 443 : 80;
				if (u.port == def)
					snprintf(req, sizeof(req), "GET http://%s%s HTTP/1.1\r\nHost: %s\r\n"
						"User-Agent: PsiMail (Psion Series 5mx)\r\n"
						"Accept: image/jpeg, image/png, image/gif\r\n"
						"Proxy-Connection: keep-alive\r\nConnection: keep-alive\r\n\r\n",
						u.host, u.path, u.host);
				else
					snprintf(req, sizeof(req), "GET http://%s:%d%s HTTP/1.1\r\nHost: %s\r\n"
						"User-Agent: PsiMail (Psion Series 5mx)\r\n"
						"Accept: image/jpeg, image/png, image/gif\r\n"
						"Proxy-Connection: keep-alive\r\nConnection: keep-alive\r\n\r\n",
						u.host, u.port, u.path, u.host);
			} else
			snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PsiMail (Psion Series 5mx)\r\n"
				"Accept: image/jpeg, image/png, image/gif\r\nConnection: keep-alive\r\n\r\n", u.path, u.host);
			if (pmn_write(req, (int)strlen(req)) != 0) {
				pmn_close(1);
				g_conn = -1;
				if (reused) continue;
				snprintf(why, whymax, "could not send the request");
				return PM_RES_FAILED;
			}
			status = 0;
			for (;;) {
				int n = pmn_readline(line, sizeof(line), 30000);
				if (n < 0) {
					pmn_close(1);
					g_conn = -1;
					if (n == PMN_CANCEL || pm_cancelled()) return PM_RES_CANCELLED;
					if (reused && !status) break;          /* the kept connection had gone: again */
					snprintf(why, whymax, "no reply from %s", u.host);
					return PM_RES_FAILED;
				}
				if (!status) {
					if (n == 0) continue;                  /* (modem CONNECT leftovers) */
					if (strncmp(line, "HTTP/", 5)) { pmn_close(1); g_conn = -1; snprintf(why, whymax, "not a web server"); return PM_RES_FAILED; }
					status = atoi(strchr(line, ' ') ? strchr(line, ' ') + 1 : line + 9);
					if (!strncmp(line, "HTTP/1.0", 8)) close_after = 1;
					continue;
				}
				if (n == 0) break;
				if (!pm_strncasecmp(line, "Content-Length:", 15)) clen = atol(line + 15);
				else if (!pm_strncasecmp(line, "Transfer-Encoding:", 18) && pm_stristr(line, "chunked")) chunked = 1;
				else if (!pm_strncasecmp(line, "Connection:", 11) && pm_stristr(line, "close")) close_after = 1;
				else if (!pm_strncasecmp(line, "Connection:", 11) && pm_stristr(line, "keep-alive")) close_after = 0;
				else if (!pm_strncasecmp(line, "Content-Type:", 13)) image = pm_stristr(line, "image/") != 0;
				else if (!pm_strncasecmp(line, "Location:", 9)) {
					const char *v = line + 9;
					while (*v == ' ') v++;
					pm_copy(loc, v, sizeof(loc));
				}
			}
			if (status) break;
		}
		}
		if (direct) { hops--; continue; }       /* the same address again, not via the proxy */
		if (!status) { snprintf(why, whymax, "the connection to %s kept dropping", u.host); return PM_RES_FAILED; }
		/* a redirect: no body worth reading (a short one is skipped below) */
		if (status >= 300 && status < 400 && loc[0]) {
			if (loc[0] == '/') snprintf(url, sizeof(url), "%s://%s:%d%s", u.tls ? "https" : "http", u.host, u.port, loc);
			else pm_copy(url, loc, sizeof(url));
		}
		if (status != 200 || image == 0 || (clen > max)) {
			/* not worth the body: skip it if it is short, else let the line go */
			if (!chunked && clen >= 0 && clen <= 4096 && !close_after) {
				long left = clen;
				while (left > 0) {
					int k = pmn_read(buf, left > (long)sizeof(buf) ? (int)sizeof(buf) : (int)left, 20000);
					if (k <= 0) { pmn_close(1); g_conn = -1; break; }
					left -= k;
				}
			} else { pmn_close(1); g_conn = -1; }
			if (status >= 300 && status < 400 && loc[0]) continue;
			if (status != 200) snprintf(why, whymax, "the server said %d", status);
			else if (image == 0) snprintf(why, whymax, "not a picture");
			else snprintf(why, whymax, "%ld KB: too big", (clen + 1023) / 1024);
			return PM_RES_FAILED;
		}
		/* the picture */
		if (!(f = fopen(path, "wb"))) { pmn_close(1); g_conn = -1; snprintf(why, whymax, "could not save it"); return PM_RES_FAILED; }
		{
			int bad = 0, r = PM_RES_OK;
			long left = clen;
			for (;;) {
				long chunk;
				if (chunked) {
					int n = pmn_readline(line, sizeof(line), 30000);
					if (n == 0) n = pmn_readline(line, sizeof(line), 30000);
					if (n < 0) { bad = n; break; }
					chunk = strtol(line, 0, 16);
					if (chunk <= 0) { while (pmn_readline(line, sizeof(line), 10000) > 0) {} break; }
				} else chunk = left;          /* (-1: to the end) */
				while (chunk != 0) {
					int want = chunk < 0 || chunk > (long)sizeof(buf) ? (int)sizeof(buf) : (int)chunk;
					int k = pmn_read(buf, want, 30000);
					if (k == 0 && chunk < 0) { close_after = 1; break; }
					if (k <= 0) { bad = k ? k : -1; break; }
					if (*got + k > max) { bad = -100; break; }
					if (fwrite(buf, 1, k, f) != (size_t)k) { bad = -101; break; }
					*got += k;
					if (chunk > 0) chunk -= k;
				}
				if (bad || !chunked) break;
			}
			if (pm_fclose(f) != 0 && !bad) bad = -101;
			if (bad) {
				pmn_close(1);
				g_conn = -1;
				remove(path);
				if (bad == PMN_CANCEL || pm_cancelled()) return PM_RES_CANCELLED;
				if (bad == -100) snprintf(why, whymax, "over %d KB: too big", (int)(max / 1024));
				else if (bad == -101) snprintf(why, whymax, "could not save it");
				else snprintf(why, whymax, "the connection to %s dropped", u.host);
				return PM_RES_FAILED;
			}
			if (close_after) { pmn_close(0); g_conn = -1; }
			return r;
		}
	}
	snprintf(why, whymax, "too many redirections");
	return PM_RES_FAILED;
}

/* writes a .pmi saying why there is no picture (pictures.c's format) */
static void note_pmi(const char *pmi, const char *reason)
{
	unsigned char b[16 + 80];
	int n = (int)strlen(reason);
	if (n > 79) n = 79;
	memset(b, 0, sizeof(b));
	memcpy(b, "PMI1", 4);
	b[12] = 2;
	memcpy(b + 16, reason, n);
	pm_write_whole(pmi, b, 16 + n + 1);
}

int web_fetch(int acct, const char *folder, unsigned int uid, char *why, int whymax)
{
	static WebPic pics[WEB_MAX];
	PmShared *s = pm_shared();
	int n, i, shown = 0, failed = 0, r = PM_RES_OK, hosts = 0;
	long total = 0, budget = PM_PIC_BUDGET_KB * 1024L;
	unsigned long t0 = pm_ms();
	char note[80], label[40], last_host[80];

	n = web_list(acct, folder, uid, pics, WEB_MAX);
	if (!n) { snprintf(why, whymax, "No pictures from the web in this message"); return PM_RES_OK; }
	if (s->offline) { snprintf(why, whymax, "Not downloaded - you are working offline"); return PM_RES_OFFLINE; }
	pm_log("web pictures: %d in message %u (%s)", n, uid, s->net.net_mode ? "Psion Internet" : "modem");
	g_proxy_down = 0;
	last_host[0] = 0;
	for (i = 0; i < n; i++) {
		char img[200], pmi[200], err[100];
		long got = 0;
		Url u;
		int d;
		if (pm_cancelled()) { r = PM_RES_CANCELLED; break; }
		web_path(acct, folder, uid, pics[i].url, "pmi", pmi, sizeof(pmi));
		web_path(acct, folder, uid, pics[i].url, "img", img, sizeof(img));
		if (fsize(pmi) >= 16) { shown++; continue; }          /* the cache */
		if ((long)(pm_ms() - t0) > WEB_TIME_MS) { pm_log("web pictures: out of time after %d", i); break; }
		if (total > WEB_TOTAL_KB * 1024L) { pm_log("web pictures: %ld KB so far - enough", total / 1024); break; }
		if (parse_url(pics[i].url, &u) == 0 && strcmp(u.host, last_host)) { hosts++; pm_copy(last_host, u.host, sizeof(last_host)); }
		snprintf(label, sizeof(label), "web picture %d of %d", i + 1, n);
		pm_progress("Getting the web pictures (%d of %d)...", i + 1, n);
		err[0] = 0;
		d = get(pics[i].url, img, WEB_MAX_KB * 1024L, &got, err, sizeof(err));
		pm_log("web picture %d: %s -> %d %s (%ld bytes)", i + 1, pics[i].url, d, err, got);
		if (d == PM_RES_CANCELLED || d == PM_RES_OFFLINE) { r = d; pm_copy(why, err, whymax); break; }
		if (d != PM_RES_OK) {
			note_pmi(pmi, err[0] ? err : "could not be downloaded");
			failed++;
			st_changed();
			continue;
		}
		total += got;
		pm_progress("Setting out the web pictures (%d of %d)...", i + 1, n);
		d = pic_decode_file(img, pmi, label, budget, note, sizeof(note));
		if (d == 0) {
			unsigned char h[16];
			FILE *f = fopen(pmi, "rb");
			int ok = f && fread(h, 1, 16, f) == 16;
			if (f) fclose(f);
			if (ok) {
				int sw = h[8] | (h[9] << 8), sh = h[10] | (h[11] << 8), w = h[4] | (h[5] << 8), hh = h[6] | (h[7] << 8);
				if (sw < 4 || sh < 4) { note_pmi(pmi, "spacer"); pm_log("web picture %d: %dx%d - a spacer", i + 1, sw, sh); }
				else { budget -= ((w + 7) / 8) * 4L * hh; shown++; }
			}
		} else failed++;
		st_changed();
	}
	(void)hosts;
	if (r == PM_RES_OK) {
		if (shown == 1 && !failed) snprintf(why, whymax, "1 web picture");
		else if (shown && !failed) snprintf(why, whymax, "%d web pictures", shown);
		else if (shown) snprintf(why, whymax, "%d web pictures, %d not shown", shown, failed);
		else if (failed) snprintf(why, whymax, "The web pictures could not be shown");
		else snprintf(why, whymax, "No pictures from the web to show");
	}
	pm_log("web pictures: %d shown, %d not, %ld KB, %lu ms", shown, failed, total / 1024, pm_ms() - t0);
	return r;
}

/* ---- Tools > Send log to server (PM_CMD_PUTLOG) ------------------------
 * POSTs psimail.old and psimail.log to the same proxy the web pictures use
 * (the shared proxy host:port) as text/plain:
 *
 *   POST /_putlog?app=PsiMail&ver=<version> HTTP/1.0
 *   Host: <proxy>   Content-Type: text/plain   Content-Length: n   Connection: close
 *
 * The body is streamed from the files in 1 KB pieces (nothing big on the
 * 64 KB stack). If it comes to more than LOG_MAX the front is cut, so the
 * newest lines are the ones kept. The reply is read for its status line only. */
#define LOG_MAX (1000L * 1024)

static int log_send_file(const char *path, long skip, long *left)
{
	static char b[1024];
	FILE *f = fopen(path, "rb");
	int ok = 1;
	if (!f) return 1;
	if (skip > 0) fseek(f, skip, SEEK_SET);
	for (;;) {
		int k = (int)fread(b, 1, sizeof(b), f);
		if (k <= 0) break;
		if (k > *left) k = (int)*left;       /* (the file grew meanwhile) */
		if (k > 0 && pmn_write(b, k) != 0) { ok = 0; break; }
		*left -= k;
		if (*left <= 0 || pm_cancelled()) break;
	}
	fclose(f);
	return ok;
}

int pm_send_log(char *why, int whymax)
{
	static char hdr_old[] = "==== psimail.old ====\n", hdr_cur[] = "==== psimail.log ====\n";
	static char req[400], line[300], pold[160], pcur[160];
	PmShared *s = pm_shared();
	long sold, scur, total, cut, left, ho = 0, hc = 0, skip_old = 0, skip_cur = 0;
	int k, status = 0, ok = 1, lport;
	const char *lhost;

	if (!PROXY_ON(s)) { snprintf(why, whymax, "Not available - set a proxy in Connection settings"); return PM_RES_FAILED; }
	if (s->offline) { snprintf(why, whymax, "Working offline"); return PM_RES_OFFLINE; }
	/* (not the Atom's own proxy: it has no /_putlog; then PsiDevBox.ini, if set) */
	if (!pg_devbox(s->proxy_host, s->proxy_port, &lhost, &lport)) {
		snprintf(why, whymax, "Not available - set a proxy in Connection settings");
		return PM_RES_FAILED;
	}
	pm_log_release();                        /* (the log is open for writing: shut it to read it) */
	pm_log_path(pold, sizeof(pold), 1);
	pm_log_path(pcur, sizeof(pcur), 0);
	sold = fsize(pold); scur = fsize(pcur);
	if (sold < 0) sold = 0;
	if (scur < 0) scur = 0;
	if (!sold && !scur) { snprintf(why, whymax, "No log to send"); return PM_RES_FAILED; }
	/* the pieces, oldest first; cut from the front if over LOG_MAX */
	if (sold) ho = (long)strlen(hdr_old);
	hc = (long)strlen(hdr_cur);
	total = ho + sold + hc + scur;
	cut = total > LOG_MAX ? total - LOG_MAX : 0;
	if (cut > 0) {
		/* the front goes first: the old log (and its heading), then the front of the current one */
		if (cut >= sold) { skip_cur = cut - sold; sold = 0; ho = 0; }
		else { skip_old = cut; sold -= cut; ho = 0; }
		scur -= skip_cur;
		total = ho + sold + hc + scur;
	}

	g_conn = -1;
	pmn_close(0);                            /* a fresh line: not one the web pictures kept */
	pm_progress("Sending log...");
	if (pmn_connect(lhost, lport, 0, why, whymax) != 0) {
		pm_log("send log: could not connect to %s:%d: %s", lhost, lport, why);
		return pm_cancelled() ? PM_RES_CANCELLED : PM_RES_FAILED;
	}
	snprintf(req, sizeof(req),
		"POST /_putlog?app=PsiMail&ver=%s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: PsiMail (Psion Series 5mx)\r\n"
		"Content-Type: text/plain\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
		s->net.version, lhost, lport, total);
	left = total;
	if (pmn_write(req, (int)strlen(req)) != 0) ok = 0;
	if (ok && ho > 0) { if (pmn_write(hdr_old, (int)strlen(hdr_old)) != 0) ok = 0; else left -= ho; }
	if (ok && sold > 0) ok = log_send_file(pold, skip_old, &left);
	if (ok && left > 0) { if (pmn_write(hdr_cur, (int)hc) != 0) ok = 0; else left -= hc; }
	if (ok && scur > 0) ok = log_send_file(pcur, skip_cur, &left);
	if (ok && left > 0) {                    /* a file that shrank: pad so the length holds */
		static char sp[64];
		memset(sp, ' ', sizeof(sp));
		while (ok && left > 0) {
			k = left > (long)sizeof(sp) ? (int)sizeof(sp) : (int)left;
			if (pmn_write(sp, k) != 0) ok = 0;
			left -= k;
		}
	}
	if (!ok) {
		pmn_close(1);
		if (pm_cancelled()) return PM_RES_CANCELLED;
		snprintf(why, whymax, "the connection was lost");
		return PM_RES_FAILED;
	}
	/* the status line (anything else is ignored; garbage is "not a web server") */
	for (k = 0; k < 20 && !status; k++) {
		int n = pmn_readline(line, sizeof(line), 30000);
		if (n < 0) { pmn_close(1); snprintf(why, whymax, pm_cancelled() ? "stopped" : "no reply from the proxy"); return pm_cancelled() ? PM_RES_CANCELLED : PM_RES_FAILED; }
		if (n == 0) continue;
		if (strncmp(line, "HTTP/", 5) || !strchr(line, ' ')) { pmn_close(1); snprintf(why, whymax, "not a web proxy"); return PM_RES_FAILED; }
		status = atoi(strchr(line, ' ') + 1);
	}
	pmn_close(0);
	pm_log("send log: %ld bytes, the proxy said %d", total, status);
	if (status == 200) { snprintf(why, whymax, "Log sent"); return PM_RES_OK; }
	if (status == 413) snprintf(why, whymax, "the log is too big");
	else snprintf(why, whymax, "the proxy said %d", status);
	return PM_RES_FAILED;
}
