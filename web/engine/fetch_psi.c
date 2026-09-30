/* fetch_psi.c - NetSurf's http: and https: fetcher for PsiWeb
 *
 * Replaces the curl fetcher. The Psion has one serial line, so fetches run
 * one after another over a single connection (pwnet.c), which is kept open
 * between requests where the server allows - with a proxy such as WebOne
 * that means one dial for a whole browsing session.
 *
 *   direct:  http://  -> host:80 in the clear
 *            https:// -> host:443 through PsiTerm's TLS 1.3 client
 *   proxy:   everything -> proxy_host:proxy_port as plain HTTP; https://
 *            URLs are asked for as http:// (WebOne fetches them over
 *            HTTPS itself), so the Psion never pays for a TLS handshake
 *
 * HTTP/1.1 with keep-alive, chunked bodies and gzip/deflate (zlib) are
 * supported, as are cookies, redirects, 304s and POSTed forms.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>

#include <zlib.h>

#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "utils/ring.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/http.h"
#include "netsurf/misc.h"
#include "desktop/gui_internal.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"
#include "utils/useragent.h"

#include "pwnet.h"
#include "pwback.h"
#include "psiweb.h"

#define LINE_MAX_  4096
#define IO_CHUNK   2048
#define POLL_MS    60          /* longest one poll may take once data flows */
#define REUSE_WAIT_MS 15000   /* no reply on a kept-alive link: redial once */
#define IDLE_TIMEOUT_MS 60000 /* nothing at all for this long: give up */

enum fstate {
	F_QUEUED, F_CONNECT, F_HEADERS, F_BODY, F_DONE
};

struct pf {
	struct pf *r_next, *r_prev;           /* RING_* */
	struct fetch *fh;
	nsurl *url;
	bool only_2xx;
	bool aborted;
	bool locked;
	bool started;
	enum fstate st;

	char *req;                             /* whole request, built at setup */
	size_t reqlen;

	/* response */
	int code;
	char *location;
	long clen;                             /* -1 unknown */
	long got;                              /* body bytes received (wire) */
	bool chunked;
	bool conn_close;
	bool http10;
	bool gzip;
	bool zinit;
	z_stream z;
	int chunk_state;                       /* 0 size line, 1 data, 2 CRLF, 3 trailers */
	long chunk_left;
	bool retried;
	bool reused;
	char line[LINE_MAX_];
	int linelen;
	char host[128];
	int port;
	int tls;
	long last_progress;
	unsigned long last_rx;                 /* pwb_ms() of the last byte (or the request) */
};

static struct pf *ring = NULL;             /* all fetches, in order */
static struct pf *current = NULL;          /* the one on the wire */
static long g_drain;                       /* body bytes of a finished reply
                                              still to be skipped on the link */

static void pf_send(struct pf *f, fetch_msg *msg)
{
	f->locked = true;
	fetch_send_callback(msg, f->fh);
	f->locked = false;
}

static void pf_progress(struct pf *f, const char *fmt, ...)
{
	char buf[128];
	fetch_msg msg;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	msg.type = FETCH_PROGRESS;
	msg.data.progress = buf;
	pf_send(f, &msg);
	pwb_set_status(buf);
}

static void pf_error(struct pf *f, const char *why)
{
	fetch_msg msg;
	msg.type = FETCH_ERROR;
	msg.data.error = why;
	pf_send(f, &msg);
}

/* ------------------------------------------------------------------ setup */

static bool pf_initialise(lwc_string *scheme) { (void)scheme; return true; }
static void pf_finalise(lwc_string *scheme)
{
	(void)scheme;
	pwn_close(1);
}
static bool pf_acceptable(const nsurl *url) { (void)url; return true; }

static int use_proxy(void)
{
	PwShared *s = (PwShared *)pwb_shared();
	return s && s->use_proxy && s->proxy_host[0];
}

/* appends to a growing buffer */
struct sbuf { char *p; size_t len, cap; bool bad; };
static void sb_add(struct sbuf *b, const char *s, size_t n)
{
	if (b->bad) return;
	if (b->len + n + 1 > b->cap) {
		size_t nc = (b->cap ? b->cap * 2 : 512);
		char *np;
		while (nc < b->len + n + 1) nc *= 2;
		np = realloc(b->p, nc);
		if (!np) { b->bad = true; return; }
		b->p = np; b->cap = nc;
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
}
static void sb_str(struct sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_multipart(struct sbuf *b, const struct fetch_multipart_data *mp,
		const char *boundary)
{
	for (; mp; mp = mp->next) {
		sb_str(b, "--"); sb_str(b, boundary); sb_str(b, "\r\n");
		sb_str(b, "Content-Disposition: form-data; name=\"");
		sb_str(b, mp->name); sb_str(b, "\"");
		if (mp->file) {
			/* file uploads are not supported: send an empty file */
			sb_str(b, "; filename=\"\"\r\nContent-Type: application/octet-stream");
			sb_str(b, "\r\n\r\n\r\n");
		} else {
			sb_str(b, "\r\n\r\n");
			sb_str(b, mp->value);
			sb_str(b, "\r\n");
		}
	}
	sb_str(b, "--"); sb_str(b, boundary); sb_str(b, "--\r\n");
}

static void *pf_setup(struct fetch *parent, nsurl *url, bool only_2xx,
		bool downgrade_tls, const char *post_urlenc,
		const struct fetch_multipart_data *post_multipart,
		const char **headers)
{
	struct pf *f = calloc(1, sizeof(*f));
	struct sbuf b = { 0 }, body = { 0 };
	lwc_string *scheme, *host, *port;
	char *path = NULL, *cookie;
	size_t pathlen;
	bool https;
	int i;
	char tmp[160];
	const char *boundary = "----PsiWebFormBoundary7d91";
	(void)downgrade_tls;

	if (!f)
		return NULL;
	f->fh = parent;
	f->url = nsurl_ref(url);
	f->only_2xx = only_2xx;
	f->clen = -1;

	scheme = nsurl_get_component(url, NSURL_SCHEME);
	host = nsurl_get_component(url, NSURL_HOST);
	port = nsurl_get_component(url, NSURL_PORT);
	https = scheme && lwc_string_length(scheme) == 5;

	if (use_proxy()) {
		PwShared *s = (PwShared *)pwb_shared();
		snprintf(f->host, sizeof(f->host), "%s", s->proxy_host);
		f->port = s->proxy_port > 0 ? s->proxy_port : 8080;
		f->tls = 0;
		/* absolute URL, with https asked for as http (the proxy upgrades) */
		nsurl_get(url, NSURL_COMPLETE & ~NSURL_FRAGMENT, &path, &pathlen);
		if (path && https && strncmp(path, "https://", 8) == 0) {
			memmove(path + 4, path + 5, pathlen - 4);
			pathlen--;
		}
	} else {
		snprintf(f->host, sizeof(f->host), "%s", host ? lwc_string_data(host) : "");
		f->port = port ? atoi(lwc_string_data(port)) : (https ? 443 : 80);
		f->tls = https;
		nsurl_get(url, NSURL_PATH | NSURL_QUERY, &path, &pathlen);
	}

	/* request line and headers */
	sb_str(&b, (post_urlenc || post_multipart) ? "POST " : "GET ");
	sb_str(&b, path ? path : "/");
	sb_str(&b, " HTTP/1.1\r\nHost: ");
	sb_str(&b, host ? lwc_string_data(host) : "");
	if (port) { sb_str(&b, ":"); sb_str(&b, lwc_string_data(port)); }
	sb_str(&b, "\r\nUser-Agent: ");
	sb_str(&b, user_agent_string());
	sb_str(&b, "\r\nAccept: text/html,application/xhtml+xml,text/css,image/gif,image/bmp,*/*;q=0.5"
	           "\r\nAccept-Encoding: gzip, deflate"
	           "\r\nConnection: keep-alive\r\n");
	if (use_proxy())
		sb_str(&b, "Proxy-Connection: keep-alive\r\n");
	for (i = 0; headers && headers[i]; i++) {
		sb_str(&b, headers[i]);
		sb_str(&b, "\r\n");
	}
	cookie = urldb_get_cookie(url, true);
	if (cookie) {
		sb_str(&b, "Cookie: ");
		sb_str(&b, cookie);
		sb_str(&b, "\r\n");
		free(cookie);
	}
	if (post_urlenc) {
		sb_str(&body, post_urlenc);
		sb_str(&b, "Content-Type: application/x-www-form-urlencoded\r\n");
	} else if (post_multipart) {
		sb_multipart(&body, post_multipart, boundary);
		snprintf(tmp, sizeof(tmp), "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
		sb_str(&b, tmp);
	}
	if (post_urlenc || post_multipart) {
		snprintf(tmp, sizeof(tmp), "Content-Length: %u\r\n", (unsigned)body.len);
		sb_str(&b, tmp);
	}
	sb_str(&b, "\r\n");
	if (body.len)
		sb_add(&b, body.p, body.len);
	free(body.p);
	free(path);
	if (scheme) lwc_string_unref(scheme);
	if (host) lwc_string_unref(host);
	if (port) lwc_string_unref(port);

	if (b.bad || !b.p) {
		free(b.p);
		nsurl_unref(f->url);
		free(f);
		return NULL;
	}
	f->req = b.p;
	f->reqlen = b.len;
	RING_INSERT(ring, f);
	return f;
}

static bool pf_start(void *vf)
{
	((struct pf *)vf)->started = true;
	return true;
}

static void pf_abort(void *vf)
{
	struct pf *f = vf;
	f->aborted = true;          /* the poll loop tidies up */
}

static void pf_free(void *vf)
{
	struct pf *f = vf;
	if (f->zinit)
		inflateEnd(&f->z);
	nsurl_unref(f->url);
	free(f->req);
	free(f->location);
	free(f);
}

/* ------------------------------------------------------------- response */

static void pf_finish(struct pf *f, bool keep_connection)
{
	if (current == f) {
		current = NULL;
		if (!keep_connection)
			pwn_close(1);
	}
	RING_REMOVE(ring, f);
	fetch_remove_from_queues(f->fh);
	fetch_free(f->fh);
}

/* body bytes after de-chunking: inflate if needed and pass on */
static bool pf_deliver(struct pf *f, const unsigned char *d, size_t n)
{
	fetch_msg msg;
	if (!n)
		return true;
	if (f->gzip) {
		unsigned char out[4096];
		if (!f->zinit) {
			memset(&f->z, 0, sizeof(f->z));
			if (inflateInit2(&f->z, 15 + 32) != Z_OK)   /* gzip or zlib */
				return false;
			f->zinit = true;
		}
		f->z.next_in = (Bytef *)d;
		f->z.avail_in = (uInt)n;
		while (f->z.avail_in > 0) {
			int r;
			f->z.next_out = out;
			f->z.avail_out = sizeof(out);
			r = inflate(&f->z, Z_NO_FLUSH);
			if (r == Z_DATA_ERROR && f->z.total_out == 0) {
				/* "deflate" sent raw, without the zlib header */
				inflateEnd(&f->z);
				memset(&f->z, 0, sizeof(f->z));
				if (inflateInit2(&f->z, -15) != Z_OK)
					return false;
				f->z.next_in = (Bytef *)d;
				f->z.avail_in = (uInt)n;
				continue;
			}
			if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR)
				return false;
			if (sizeof(out) - f->z.avail_out) {
				msg.type = FETCH_DATA;
				msg.data.header_or_data.buf = out;
				msg.data.header_or_data.len = sizeof(out) - f->z.avail_out;
				pf_send(f, &msg);
				if (f->aborted)
					return true;
			}
			if (r == Z_STREAM_END || r == Z_BUF_ERROR)
				break;
		}
		return true;
	}
	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = d;
	msg.data.header_or_data.len = n;
	pf_send(f, &msg);
	return true;
}

/* one header line (without CRLF). Returns false when headers are done and
 * the fetch has been handled (redirect, 304, error...). */
static void pf_header(struct pf *f, char *l)
{
	fetch_msg msg;
	char *v;

	if (f->code == 0) {
		/* status line */
		if (strncmp(l, "HTTP/", 5) == 0) {
			char *sp = strchr(l, ' ');
			f->code = sp ? atoi(sp + 1) : 500;
			f->http10 = (strncmp(l, "HTTP/1.0", 8) == 0);
			f->conn_close = f->http10;
		}
		return;
	}
	v = strchr(l, ':');
	if (v) {
		v++;
		while (*v == ' ' || *v == '\t') v++;
	}
	if (!strncasecmp(l, "Location:", 9) && v) {
		free(f->location);
		f->location = strdup(v);
	} else if (!strncasecmp(l, "Content-Length:", 15) && v) {
		f->clen = atol(v);
	} else if (!strncasecmp(l, "Transfer-Encoding:", 18) && v) {
		if (strstr(v, "chunked") || strstr(v, "Chunked"))
			f->chunked = true;
	} else if (!strncasecmp(l, "Content-Encoding:", 17) && v) {
		if (!strncasecmp(v, "gzip", 4) || !strncasecmp(v, "x-gzip", 6) ||
		    !strncasecmp(v, "deflate", 7))
			f->gzip = true;
		/* the body we pass on is decoded: don't claim otherwise */
		return;
	} else if ((!strncasecmp(l, "Connection:", 11) ||
		    !strncasecmp(l, "Proxy-Connection:", 17)) && v) {
		if (!strncasecmp(v, "close", 5)) f->conn_close = true;
		else if (!strncasecmp(v, "keep-alive", 10)) f->conn_close = false;
	} else if (!strncasecmp(l, "Set-Cookie:", 11) && v) {
		fetch_set_cookie(f->fh, v);
	}
	if (!strncasecmp(l, "Content-Length:", 15) && f->gzip)
		return;
	msg.type = FETCH_HEADER;
	msg.data.header_or_data.buf = (const uint8_t *)l;
	msg.data.header_or_data.len = strlen(l);
	pf_send(f, &msg);
}

/* headers finished: decide what to do. Returns true if the body should be
 * read and passed on, false if the fetch is complete (callbacks sent). */
static bool pf_headers_done(struct pf *f)
{
	fetch_msg msg;

	fetch_set_http_code(f->fh, f->code);
	if (f->code == 304) {
		msg.type = FETCH_NOTMODIFIED;
		pf_send(f, &msg);
		return false;
	}
	if (f->code >= 300 && f->code < 400 && f->location) {
		msg.type = FETCH_REDIRECT;
		msg.data.redirect = f->location;
		pf_send(f, &msg);
		return false;
	}
	if (f->code == 401) {
		msg.type = FETCH_AUTH;
		msg.data.auth.realm = "";
		pf_send(f, &msg);
		return false;
	}
	if (f->only_2xx && (f->code < 200 || f->code > 299)) {
		pf_error(f, messages_get("Not2xx"));
		return false;
	}
	return true;
}

/* the body has no bytes when... */
static bool pf_no_body(struct pf *f)
{
	return f->code == 204 || f->code == 304 || (f->code >= 100 && f->code < 200) ||
	       (!f->chunked && f->clen == 0);
}

/* consumes wire bytes for the body. Returns 1 when the body is complete,
 * 0 if more is needed, -1 on a broken stream. */
static int pf_body(struct pf *f, const unsigned char *d, int n)
{
	if (!f->chunked) {
		int take = n;
		if (f->clen >= 0 && f->got + take > f->clen)
			take = (int)(f->clen - f->got);
		f->got += take;
		if (!pf_deliver(f, d, take))
			return -1;
		return (f->clen >= 0 && f->got >= f->clen) ? 1 : 0;
	}
	while (n > 0 && !f->aborted) {
		if (f->chunk_state == 1) {               /* chunk data */
			int take = n < f->chunk_left ? n : (int)f->chunk_left;
			if (!pf_deliver(f, d, take))
				return -1;
			d += take; n -= take; f->chunk_left -= take; f->got += take;
			if (f->chunk_left == 0)
				f->chunk_state = 2;
			continue;
		}
		/* a line: size, the CRLF after data, or trailers */
		{
			unsigned char c = *d++;
			n--;
			if (c == '\r') continue;
			if (c != '\n') {
				if (f->linelen < LINE_MAX_ - 1) f->line[f->linelen++] = (char)c;
				continue;
			}
			f->line[f->linelen] = 0;
			if (f->chunk_state == 2) {
				f->chunk_state = 0;
			} else if (f->chunk_state == 0) {
				if (f->linelen == 0) { continue; }
				f->chunk_left = strtol(f->line, NULL, 16);
				f->chunk_state = f->chunk_left ? 1 : 3;
			} else if (f->chunk_state == 3) {
				if (f->linelen == 0) { f->linelen = 0; return 1; }
			}
			f->linelen = 0;
		}
	}
	return 0;
}

/* the connection has gone (closed, dropped, or the modem is back at its
 * command prompt). Returns after handling it one way or another. */
static void pf_closed(struct pf *f)
{
	fetch_msg msg;
	if (f->st == F_HEADERS && f->code == 0 && f->reused && !f->retried) {
		f->retried = true;                 /* the kept-alive link had gone */
		pwn_close(1);
		f->st = F_QUEUED;
		f->linelen = 0;
		return;
	}
	if (f->st == F_BODY && !f->chunked && f->clen < 0) {
		/* body delimited by the close */
		msg.type = FETCH_FINISHED;
		pf_send(f, &msg);
		pf_finish(f, false);
		return;
	}
	pf_error(f, f->st == F_HEADERS ? "The server closed the connection"
	                               : "The page was cut short");
	pf_finish(f, false);
}

/* ------------------------------------------------------------------ poll */

static void pf_run(struct pf *f)
{
	unsigned char buf[IO_CHUNK];
	char why[160];
	unsigned long start = pwb_ms();
	int r;
	fetch_msg msg;

	if (f->st == F_QUEUED) {
		f->st = F_CONNECT;
		why[0] = 0;
		if (g_drain > 0 && pwn_is_open(f->host, f->port, f->tls)) {
			/* skip the unread body of the last reply (a redirect, say) */
			while (g_drain > 0) {
				int n = pwn_read(buf, g_drain < (long)sizeof(buf) ? (int)g_drain : (int)sizeof(buf), 5000);
				if (n <= 0) { pwn_close(1); break; }
				g_drain -= n;
			}
		}
		g_drain = 0;
		if (pwn_is_open(f->host, f->port, f->tls)) {
			f->reused = true;
		} else {
			pf_progress(f, "Connecting to %s...", f->host);
			if (f->aborted) return;
			if (pwn_connect(f->host, f->port, f->tls, why, sizeof(why)) != 0) {
				char msgbuf[220];
				snprintf(msgbuf, sizeof(msgbuf), "Could not connect to %s: %s",
						f->host, why[0] ? why : "no connection");
				pf_error(f, msgbuf);
				pf_finish(f, false);
				return;
			}
			f->reused = false;
		}
		if (pwn_write(f->req, (int)f->reqlen) != 0) {
			if (f->reused && !f->retried) {
				f->retried = true;           /* the kept-alive link had gone */
				pwn_close(1);
				f->st = F_QUEUED;
				return;
			}
			pf_error(f, "The connection failed while sending the request");
			pf_finish(f, false);
			return;
		}
		f->st = F_HEADERS;
		f->last_progress = 0;
		f->last_rx = pwb_ms();
		pf_progress(f, "Waiting for %s...", f->host);
		if (f->aborted) return;
	}

	for (;;) {
		int i;
		/* the first reply can take a while; later reads return quickly so
		 * the page can be redrawn between pieces */
		r = pwn_read(buf, sizeof(buf), f->st == F_HEADERS && f->code == 0 ? 50 : 10);
		if (r == PWN_TIMEOUT) {
			/* nothing yet. Without these limits any lost reply meant
			   "Loading" for ever */
			unsigned long idle = pwb_ms() - f->last_rx;
			if (f->st == F_HEADERS && f->code == 0 && f->reused && !f->retried &&
			    idle > REUSE_WAIT_MS) {
				f->retried = true;
				pwn_close(1);
				f->st = F_QUEUED;
				f->linelen = 0;
				return;
			}
			if (idle > IDLE_TIMEOUT_MS) {
				char m[160];
				snprintf(m, sizeof(m), f->st == F_HEADERS
					? "No reply from %s (timed out)"
					: "%s stopped sending (timed out)", f->host);
				pf_error(f, m);
				pf_finish(f, false);
			}
			return;
		}
		if (r == PWN_CANCEL) {
			pf_error(f, "Stopped");
			pf_finish(f, false);
			return;
		}
		if (r <= 0) {
			pf_closed(f);
			return;
		}
		f->last_rx = pwb_ms();
		i = 0;
		if (f->st == F_HEADERS) {
			while (i < r && f->st == F_HEADERS) {
				char c = (char)buf[i++];
				if (c == '\r') continue;
				if (c != '\n') {
					if (f->linelen < LINE_MAX_ - 1) f->line[f->linelen++] = c;
					continue;
				}
				f->line[f->linelen] = 0;
				if (f->linelen == 0) {
					bool more;
					if (f->code == 0) continue;           /* modem CONNECT leftovers */
					if (f->code >= 100 && f->code < 200) { /* 100 Continue */
						f->code = 0; continue;
					}
					more = pf_headers_done(f);
					if (f->aborted && more) return;
					if (!more || pf_no_body(f)) {
						bool keep = !f->conn_close;
						if (more && !f->aborted) {
							msg.type = FETCH_FINISHED;   /* empty body */
							pf_send(f, &msg);
						}
						if (!more && !pf_no_body(f)) {
							/* a reply we don't want the body of */
							if (keep && !f->chunked && f->clen > 0)
								g_drain = f->clen - (r - i);
							else
								keep = false;
							if (g_drain < 0) keep = false, g_drain = 0;
						}
						pf_finish(f, keep);
						return;
					}
					f->st = F_BODY;
					break;
				}
				if (f->code == 0 && strncmp(f->line, "HTTP/", 5) != 0) {
					/* a modem result instead of a reply: the modem is at
					   its command prompt, so the connection had gone */
					if (pwn_modem() && (!strcmp(f->line, "ERROR") ||
					    !strcmp(f->line, "OK") ||
					    !strncmp(f->line, "NO CARRIER", 10))) {
						pwn_close(1);
						pf_closed(f);
						return;
					}
					f->linelen = 0;               /* noise before the status line */
					continue;
				}
				pf_header(f, f->line);
				f->linelen = 0;
				if (f->aborted) return;
			}
		}
		if (f->st == F_BODY && i < r) {
			int done = pf_body(f, buf + i, r - i);
			if (f->aborted) return;
			if (done < 0) {
				pf_error(f, "The page could not be decoded");
				pf_finish(f, false);
				return;
			}
			if (done > 0) {
				msg.type = FETCH_FINISHED;
				pf_send(f, &msg);
				pf_finish(f, !f->conn_close);
				return;
			}
			if (f->got - f->last_progress >= 4096) {
				f->last_progress = f->got;
				if (f->clen > 0)
					pf_progress(f, "%s: %ld of %ld KB", f->host, f->got / 1024, f->clen / 1024);
				else
					pf_progress(f, "%s: %ld KB", f->host, f->got / 1024);
				if (f->aborted) return;
			}
		}
		if (pwb_ms() - start > POLL_MS)
			return;
	}
}

static void pf_poll(lwc_string *scheme)
{
	struct pf *f;
	(void)scheme;

	/* tidy aborted fetches that are not on the wire */
	for (;;) {
		struct pf *g = NULL;
		f = ring;
		if (f) do {
			if (f->aborted && !f->locked) { g = f; break; }
			f = f->r_next;
		} while (f != ring);
		if (!g) break;
		pf_finish(g, current != g ? true : false);
	}

	if (!current) {
		f = ring;
		if (!f) return;
		do {
			if (f->started && !f->aborted) { current = f; break; }
			f = f->r_next;
		} while (f != ring);
	}
	if (current && !current->locked)
		pf_run(current);
}

nserror fetch_psi_register(void)
{
	const struct fetcher_operation_table ops = {
		.initialise = pf_initialise,
		.acceptable = pf_acceptable,
		.setup = pf_setup,
		.start = pf_start,
		.abort = pf_abort,
		.free = pf_free,
		.poll = pf_poll,
		.finalise = pf_finalise
	};
	nserror r = fetcher_add(lwc_string_ref(corestring_lwc_http), &ops);
	if (r != NSERROR_OK)
		return r;
	return fetcher_add(lwc_string_ref(corestring_lwc_https), &ops);
}
