/* http.c - HTTP/1.1 requests for CalDAV (see cal.h)
 *
 * One keep-alive connection on the engine's single network line (pmnet.c):
 * after the first request the TLS handshake - the slow part on a Psion - is
 * not repeated. If IMAP or SMTP used the line in between, or the server
 * closed it, the next request connects again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm.h"
#include "cal.h"

static char g_host[80], g_auth[300];
static int  g_port, g_tls;
static int  g_conn = -1;              /* pmn_conn_id() of our connection */

static void b64(const unsigned char *in, int n, char *out)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i, k = 0;
	for (i = 0; i < n; i += 3) {
		unsigned long v = (unsigned long)in[i] << 16;
		if (i + 1 < n) v |= (unsigned long)in[i + 1] << 8;
		if (i + 2 < n) v |= in[i + 2];
		out[k++] = t[(v >> 18) & 63];
		out[k++] = t[(v >> 12) & 63];
		out[k++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
		out[k++] = i + 2 < n ? t[v & 63] : '=';
	}
	out[k] = 0;
}

void http_target(const char *host, int port, int tls, const char *user, const char *pass)
{
	char raw[200];
	if (strcmp(host, g_host) || port != g_port) g_conn = -1;
	pm_copy(g_host, host, sizeof(g_host));
	g_port = port;
	g_tls = tls;
	snprintf(raw, sizeof(raw), "%s:%s", user, pass);
	b64((const unsigned char *)raw, (int)strlen(raw), g_auth);
	memset(raw, 0, sizeof(raw));
}

void http_close(void)
{
	if (g_conn >= 0 && pmn_conn_id() == g_conn) pmn_close(0);
	g_conn = -1;
}

static int connect_now(char *why, int whymax)
{
	PmShared *s = pm_shared();
	if (g_conn >= 0 && pmn_conn_id() == g_conn && pmn_is_open()) return PM_RES_OK;
	if (s->offline) { snprintf(why, whymax, "Working offline"); return PM_RES_OFFLINE; }
	if (pmn_connect(g_host, g_port, g_tls, why, whymax) != 0) {
		if (tlsv_problem()[0]) {
			snprintf(s->trust_host, sizeof(s->trust_host), "%s:%d", g_host, g_port);
			pm_copy(s->trust_why, tlsv_problem(), sizeof(s->trust_why));
			pm_copy(s->trust_fp, tlsv_fingerprint(), sizeof(s->trust_fp));
			return PM_RES_UNTRUSTED;
		}
		return pm_cancelled() ? PM_RES_CANCELLED : PM_RES_OFFLINE;
	}
	g_conn = pmn_conn_id();
	return PM_RES_OK;
}

static int lost(int r, char *why, int whymax)
{
	g_conn = -1;
	if (r == PMN_CANCEL || pm_cancelled()) { snprintf(why, whymax, "Stopped"); return PM_RES_CANCELLED; }
	snprintf(why, whymax, r == PMN_TIMEOUT ? "The calendar server stopped answering" : "Lost the connection to the calendar server");
	pmn_close(1);
	return PM_RES_OFFLINE;
}

/* reads exactly n body bytes (n < 0: until the connection closes) */
static int body_bytes(long n, HttpResp *r, HttpSink sink, void *ctx)
{
	static char b[1024];
	while (n != 0) {
		int want = n < 0 || n > (long)sizeof(b) ? (int)sizeof(b) : (int)n;
		int k = pmn_read(b, want, 60000);
		if (k == 0 && n < 0) return 0;
		if (k <= 0) return k == 0 ? -1 : k;
		if (sink) sink(b, k, ctx);
		r->length += k;
		if (n > 0) n -= k;
	}
	return 0;
}

int http_request(const char *method, const char *path, const char *headers,
                 const char *body, int blen, HttpResp *r, HttpSink sink, void *ctx,
                 char *why, int whymax)
{
	static char line[600], req[1400];
	int attempt, res, k;
	memset(r, 0, sizeof(*r));
	for (attempt = 0; attempt < 2; attempt++) {
		int chunked = 0, close_after = 0, got_status = 0;
		long clen = -1;
		res = connect_now(why, whymax);
		if (res != PM_RES_OK) return res;
		k = snprintf(req, sizeof(req),
			"%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PsiMail (Psion Series 5mx)\r\n"
			"Authorization: Basic %s\r\nContent-Length: %d\r\n%s\r\n",
			method, path, g_host, g_auth, blen, headers ? headers : "");
		if (k >= (int)sizeof(req)) { snprintf(why, whymax, "Request too long"); return PM_RES_FAILED; }
		if (pmn_write(req, k) != 0 || (blen > 0 && pmn_write(body, blen) != 0)) {
			g_conn = -1;
			if (attempt == 0) continue;       /* the server had closed the connection */
			return lost(-1, why, whymax);
		}
		/* status line and headers */
		for (;;) {
			int n = pmn_readline(line, sizeof(line), 60000);
			if (n < 0) {
				if (!got_status && attempt == 0 && n != PMN_CANCEL) { g_conn = -1; pmn_close(0); break; }
				return lost(n, why, whymax);
			}
			if (!got_status) {
				if (n == 0) continue;
				if (strncmp(line, "HTTP/1.", 7)) { snprintf(why, whymax, "Not a web server: %.40s", line); return PM_RES_FAILED; }
				r->status = atoi(line + 9);
				got_status = 1;
				continue;
			}
			if (n == 0) break;
			if (!pm_strncasecmp(line, "Content-Length:", 15)) clen = atol(line + 15);
			else if (!pm_strncasecmp(line, "Transfer-Encoding:", 18) && pm_stristr(line, "chunked")) chunked = 1;
			else if (!pm_strncasecmp(line, "Connection:", 11) && pm_stristr(line, "close")) close_after = 1;
			else if (!pm_strncasecmp(line, "ETag:", 5)) {
				const char *v = line + 5;
				while (*v == ' ') v++;
				pm_copy(r->etag, v, sizeof(r->etag));
			} else if (!pm_strncasecmp(line, "Location:", 9)) {
				const char *v = line + 9;
				while (*v == ' ') v++;
				pm_copy(r->location, v, sizeof(r->location));
			}
		}
		if (!got_status) continue;             /* retry on a new connection */
		/* the body */
		if (r->status == 204 || r->status == 304 || !strcmp(method, "HEAD")) clen = 0;
		if (chunked) {
			for (;;) {
				long size;
				int n = pmn_readline(line, sizeof(line), 60000);
				if (n < 0) return lost(n, why, whymax);
				if (n == 0) continue;
				size = strtol(line, 0, 16);
				if (size <= 0) {
					while ((n = pmn_readline(line, sizeof(line), 60000)) > 0) {}   /* trailers */
					break;
				}
				if ((n = body_bytes(size, r, sink, ctx)) != 0) return lost(n, why, whymax);
			}
		} else if (clen >= 0) {
			if ((k = body_bytes(clen, r, sink, ctx)) != 0) return lost(k, why, whymax);
		} else {
			body_bytes(-1, r, sink, ctx);
			close_after = 1;
		}
		if (close_after) { pmn_close(0); g_conn = -1; }
		if (r->status == 401) {
			snprintf(why, whymax, "The calendar server did not accept the password");
			return PM_RES_LOGIN_FAILED;
		}
		return PM_RES_OK;
	}
	return lost(-1, why, whymax);
}
