/* pwupdate.c - "Update PsiWeb": download and check a newer PsiWeb.sis
 *
 * Like PsiTerm's updater (ssh/psishim.c run_update), and the same release
 * key: dist/PsiWeb-version.txt says the newest version, dist/PsiWeb.sis.sig
 * holds an Ed25519 signature over
 *     "PsiWeb update\n" <version> "\n" SHA-256(PsiWeb.sis)
 * (tools/release/sign.py --product PsiWeb), and nothing is handed to the
 * installer unless it verifies against the public key built in here.
 *
 * Sources (the app sets them in PwShared, or - 0.54 - Update.ini next to
 * the app says, in PsiMail's words: "github", "github-dev" or "host:port"):
 *   GitHub  raw.githubusercontent.com/danieledge/psiterm/main/dist/ (the
 *           branch's dist/ folder, not a release tag: whatever is committed
 *           there is what gets offered), in 64 KB pieces with HTTP Range on
 *           one kept-alive connection: through the proxy if one is set
 *           (WebOne does the HTTPS), else directly over PsiTerm's TLS 1.3
 *           client. "github-dev" is the same from the dev branch.
 *   local   PsiTerm's update server (server/psion-update.sh): plain HTTP,
 *           ?o=&n= pieces each with a CRC-32
 * Each piece is kept in memory until complete, and retried if it arrives
 * short or damaged, so a dropped connection costs one piece.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "includes.h"          /* web/tls: libtomcrypt (SHA-256) */
#include "pwnet.h"
#include "../psiweb.h"
#include "../fb/pwback.h"

extern PsiShared *pg_shared(void);
extern int dropbear_ed25519_verify(const unsigned char *m, unsigned long mlen,
	const unsigned char *s, unsigned long slen, const unsigned char *pk);
extern unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len);

#ifdef PW_TEST_KEY
/* emulator/host tests: ssh/test/update-test.key signs the test releases */
static const unsigned char KUpdateKey[32] = {
	0xf8,0x33,0x12,0xd1,0xee,0x3f,0x58,0x23,0x7c,0xfb,0x99,0x34,0x29,0x9e,0x1c,0x12,
	0x9c,0x98,0x16,0xc2,0xfd,0x54,0x0a,0x1b,0x24,0x43,0xeb,0xd0,0xae,0xdd,0xa0,0xd7 };
#else
/* PsiTerm's release key (ssh/psishim.c) */
static const unsigned char KUpdateKey[32] = {
	0x37,0x74,0xc9,0x3a,0xb2,0xf8,0x4b,0x0e,0x07,0x68,0x41,0x34,0x11,0x6c,0xc0,0x3e,
	0x81,0x63,0x90,0xd8,0x00,0x67,0x8f,0x33,0x54,0x38,0x6b,0xf9,0xfd,0x5e,0x9e,0x36 };
#endif

#define GH_HOST  "raw.githubusercontent.com"
#define GH_PATH  "/danieledge/psiterm/main/dist/"
#define GH_DEV_PATH "/danieledge/psiterm/dev/dist/"
#define PIECE    65536

static PwShared *S(void) { return (PwShared *)pwb_shared(); }

static void say(int state, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(S()->update_msg, sizeof(S()->update_msg), fmt, ap);
	va_end(ap);
	S()->update_state = state;
	pwb_set_status(S()->update_msg);
}

/* where requests go */
static struct {
	char host[64];          /* connect to */
	int port;
	int tls;
	char vhost[64];         /* Host: header */
	char prefix[128];       /* before the file name in the request line */
	int local;              /* PsiTerm's local server protocol */
} R;

/* (0.54) The same choice PsiMail keeps in its Update.ini, read from
   <home>\Update.ini: "github" (default), "github-dev" or "host[:port]" for
   PsiTerm's local update server. Without the file nothing changes. */
static void read_update_ini(char *src, int max)
{
	char path[128];
	FILE *f;
	int n = 0, c;
	src[0] = 0;
	snprintf(path, sizeof(path), "%s\\Update.ini", pg_shared()->home[0] ? pg_shared()->home : "C:\\System\\Apps\\PsiWeb");
	f = fopen(path, "rb");
	if (!f) return;
	while ((c = fgetc(f)) != EOF && c != '\r' && c != '\n' && n < max - 1)
		src[n++] = (char)c;
	fclose(f);
	while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t')) n--;
	src[n] = 0;
	while (src[0] == ' ' || src[0] == '\t') memmove(src, src + 1, strlen(src));
}

static void setup_route(void)
{
	PwShared *s = S();
	char ini[64];
	const char *gh_path = GH_PATH;
	memset(&R, 0, sizeof(R));
	read_update_ini(ini, sizeof(ini));
	if (!strncasecmp(ini, "github", 6)) {
		if (!strcasecmp(ini, "github-dev")) gh_path = GH_DEV_PATH;
		ini[0] = 0;
	} else if (ini[0]) {
		/* a local server, PsiMail's way: host or host:port */
		char *c = strchr(ini, ':');
		if (c) { *c = 0; s->upd_port = atoi(c + 1); }
		else s->upd_port = 0;
		snprintf(s->upd_host, sizeof(s->upd_host), "%s", ini);
		s->upd_source = 1;
	}
	if (s->upd_source == 1 && s->upd_host[0]) {
		snprintf(R.host, sizeof(R.host), "%s", s->upd_host);
		R.port = s->upd_port > 0 ? s->upd_port : 8686;
		snprintf(R.vhost, sizeof(R.vhost), "%s", s->upd_host);
		snprintf(R.prefix, sizeof(R.prefix), "/");
		R.local = 1;
	} else if (s->use_proxy && s->proxy_host[0]) {
		snprintf(R.host, sizeof(R.host), "%s", s->proxy_host);
		R.port = s->proxy_port > 0 ? s->proxy_port : 8080;
		snprintf(R.vhost, sizeof(R.vhost), GH_HOST);
		snprintf(R.prefix, sizeof(R.prefix), "http://%s%s", GH_HOST, gh_path);
	} else {
		snprintf(R.host, sizeof(R.host), GH_HOST);
		R.port = 443;
		R.tls = 1;
		snprintf(R.vhost, sizeof(R.vhost), GH_HOST);
		snprintf(R.prefix, sizeof(R.prefix), "%s", gh_path);
	}
}

/* response of the last request */
static struct { int status; long clen; long total; int has_crc; unsigned long crc; int close; } H;

static int rd_byte(int timeout_ms)
{
	unsigned char c;
	int r = pwn_read(&c, 1, timeout_ms);
	if (r == 1) return c;
	if (r == PWN_TIMEOUT) {
		/* pwn_read's own timeout is per call: keep waiting to 'timeout_ms' */
		unsigned long t0 = pwb_ms();
		while (pwb_ms() - t0 < (unsigned long)timeout_ms) {
			r = pwn_read(&c, 1, 500);
			if (r == 1) return c;
			if (r != PWN_TIMEOUT) break;
		}
	}
	return r == PWN_CANCEL ? -2 : -1;
}

/* sends a GET and reads the headers; body follows on the link.
   0 = ok (status 200/206), -1 = failed (why set), -2 = cancelled */
static int request(const char *name, const char *query, long rfrom, long rlen,
		char *why, int whymax)
{
	char req[512], line[256];
	int n = 0, c, attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		int reused = pwn_is_open(R.host, R.port, R.tls);
		if (!reused) {
			if (pwn_connect(R.host, R.port, R.tls, why, whymax) != 0)
				return pg_shared()->quit ? -2 : -1;
		}
		snprintf(req, sizeof(req),
			"GET %s%s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PsiWeb/%s\r\n"
			"Connection: %s\r\n",
			R.prefix, name, query ? query : "", R.vhost, pg_shared()->version,
			R.local ? "close" : "keep-alive");
		if (rlen > 0 && !R.local)
			snprintf(req + strlen(req), sizeof(req) - strlen(req),
				"Range: bytes=%ld-%ld\r\n", rfrom, rfrom + rlen - 1);
		strcat(req, "\r\n");
		if (pwn_write(req, (int)strlen(req)) != 0) {
			pwn_close(1);
			if (reused) continue;           /* the kept-alive link had gone */
			snprintf(why, whymax, "could not send the request");
			return -1;
		}
		memset(&H, 0, sizeof(H));
		H.clen = -1;
		H.total = -1;
		n = 0;
		for (;;) {
			c = rd_byte(30000);
			if (c < 0) {
				if (c == -2) return -2;
				if (reused && !H.status) { pwn_close(1); break; }   /* retry fresh */
				snprintf(why, whymax, "no reply from %s", R.host);
				return -1;
			}
			if (c == '\r') continue;
			if (c != '\n') { if (n < (int)sizeof(line) - 1) line[n++] = (char)c; continue; }
			line[n] = 0;
			if (n == 0 && !H.status) continue;       /* modem CONNECT leftovers */
			if (n == 0) {
				if (H.status == 200 || H.status == 206)
					return 0;
				snprintf(why, whymax, "the server said %d for %.40s", H.status, name);
				pwn_close(1);
				return -1;
			}
			if (!H.status && !strncmp(line, "HTTP/", 5)) {
				char *sp = strchr(line, ' ');
				H.status = sp ? atoi(sp + 1) : 0;
				H.close = !strncmp(line, "HTTP/1.0", 8);
			} else if (!strncasecmp(line, "Content-Length:", 15))
				H.clen = atol(line + 15);
			else if (!strncasecmp(line, "Content-Range:", 14)) {
				char *sl = strchr(line, '/');
				if (sl) H.total = atol(sl + 1);
			} else if (!strncasecmp(line, "X-Total:", 8))
				H.total = atol(line + 8);
			else if (!strncasecmp(line, "X-CRC32:", 8)) {
				H.crc = strtoul(line + 8, NULL, 16);
				H.has_crc = 1;
			} else if (!strncasecmp(line, "Connection:", 11) && strstr(line, "close"))
				H.close = 1;
			else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strstr(line, "chunked")) {
				snprintf(why, whymax, "the server sent a chunked reply");
				pwn_close(1);
				return -1;
			}
			n = 0;
		}
	}
	snprintf(why, whymax, "the connection to %s kept dropping", R.host);
	return -1;
}

/* reads n body bytes; returns how many arrived, -2 if cancelled */
static long body(unsigned char *buf, long n)
{
	long k = 0;
	while (k < n) {
		int r = pwn_read(buf + k, n - k > 4096 ? 4096 : (int)(n - k), 15000);
		if (r == PWN_CANCEL) return -2;
		if (r == PWN_TIMEOUT) {
			unsigned long t0 = pwb_ms();
			while (r == PWN_TIMEOUT && pwb_ms() - t0 < 15000)
				r = pwn_read(buf + k, n - k > 4096 ? 4096 : (int)(n - k), 500);
			if (r == PWN_CANCEL) return -2;
		}
		if (r <= 0) break;
		k += r;
	}
	return k;
}

static void after(void)
{
	if (H.close || R.local)
		pwn_close(1);
}

static int fetch_small(const char *name, char *buf, int max, char *why, int whymax)
{
	long k = 0, want;
	int r, attempt;
	/* a reply cut short (a slow or dropping link) is asked for again, up to
	   3 times; it used to be taken as the whole file, and a half signature
	   was then "damaged" */
	for (attempt = 0; attempt < 3; attempt++) {
		r = request(name, NULL, 0, 0, why, whymax);
		if (r) return r;
		want = H.clen >= 0 && H.clen < max ? H.clen : max - 1;
		k = body((unsigned char *)buf, want);
		if (k == -2) return -2;
		if (H.clen < 0 || H.clen >= max) pwn_close(1);   /* unread rest */
		else after();
		buf[k < 0 ? 0 : k] = 0;
		if (H.clen < 0 || H.clen >= max || k >= want)
			return 0;
		pwn_close(1);                                    /* cut short: again on a new connection */
	}
	snprintf(why, whymax, "the reply from the server was cut short (%ld of %ld bytes)", k < 0 ? 0 : k, H.clen);
	return -1;
}

/* major.minor only, as the EPOC installer compares them. Versions were
   three-part up to 0.5.3 and two-digit from 0.54: "0.5.3" parses as 0.5,
   "0.54" as 0.54, so 0.54 is newer than 0.5.3 (54 > 5) and a device on
   0.5.3 is offered it; later 0.55 > 0.54 as usual. */
static int version_newer(const char *remote, const char *local)
{
	int rm = 0, rn = 0, lm = 0, ln = 0;
	sscanf(remote, "%d.%d", &rm, &rn);
	sscanf(local, "%d.%d", &lm, &ln);
	return rm > lm || (rm == lm && rn > ln);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int parse_sig(const char *txt, char *ver, int vermax, unsigned char sig[64])
{
	int i = 0, n = 0;
	while (txt[i] && txt[i] != '\n' && txt[i] != '\r' && n < vermax - 1) ver[n++] = txt[i++];
	ver[n] = 0;
	while (txt[i] == '\r' || txt[i] == '\n') i++;
	for (n = 0; n < 64; n++) {
		int h = hexval(txt[i]), l = h >= 0 ? hexval(txt[i + 1]) : -1;
		if (h < 0 || l < 0) return -1;
		sig[n] = (unsigned char)(h * 16 + l);
		i += 2;
	}
	return ver[0] ? 0 : -1;
}

static int verify_file(const char *file, const char *ver, const unsigned char sig[64])
{
	unsigned char msg[14 + 16 + 1 + 32], buf[1024];
	hash_state h;
	int n, vl = (int)strlen(ver), k;
	FILE *f = fopen(file, "rb");
	if (!f || vl > 16) { if (f) fclose(f); return -1; }
	sha256_init(&h);
	while ((k = (int)fread(buf, 1, sizeof(buf), f)) > 0)
		sha256_process(&h, buf, k);
	fclose(f);
	memcpy(msg, "PsiWeb update\n", 14); n = 14;
	memcpy(msg + n, ver, vl); n += vl;
	msg[n++] = '\n';
	sha256_done(&h, msg + n); n += 32;
	return dropbear_ed25519_verify(msg, n, sig, 64, KUpdateKey);
}

/* returns PW_UPD_* (also left in update_state) */
int pw_update_run(void)
{
	PsiShared *net = pg_shared();
	PwShared *s = S();
	char why[128], remote[24], sigtxt[300], sigver[24], q[48];
	unsigned char sig[64];
	unsigned char *buf = NULL;
	long got = 0, total = -1;
	int n, r, tries = 0;
	FILE *f = NULL;

	setup_route();
	why[0] = 0;
	say(PW_UPD_RUNNING, "Checking for a new PsiWeb (%s%s)...",
		R.local ? "local server " : (R.tls ? "GitHub" : "GitHub via the proxy"),
		R.local ? R.host : "");

	r = fetch_small("PsiWeb-version.txt", remote, sizeof(remote), why, sizeof(why));
	if (r) goto fail;
	for (n = 0; remote[n] && remote[n] != '\r' && remote[n] != '\n' && remote[n] != ' '; n++) ;
	remote[n] = 0;
	if (!n) { snprintf(why, sizeof(why), "PsiWeb-version.txt was empty"); r = -1; goto fail; }
	if (!version_newer(remote, net->version)) {
		pwn_close(1);
		say(PW_UPD_CURRENT, "PsiWeb %s is the latest version (the server has %s)",
			net->version, remote);
		return PW_UPD_CURRENT;
	}
	snprintf(s->update_version, sizeof(s->update_version), "%s", remote);
	r = fetch_small("PsiWeb.sis.sig", sigtxt, sizeof(sigtxt), why, sizeof(why));
	if (r) goto fail;
	if (parse_sig(sigtxt, sigver, sizeof(sigver), sig) != 0) {
		snprintf(why, sizeof(why), "the release signature (PsiWeb.sis.sig) is damaged (%.30s)", sigtxt);
		r = -1; goto fail;
	}
	if (strcmp(sigver, remote) != 0) {
		snprintf(why, sizeof(why), "the signature is for %.10s, not %.10s", sigver, remote);
		r = -1; goto fail;
	}

	buf = malloc(PIECE);
	f = fopen(net->save_as, "wb");
	if (!buf || !f) {
		snprintf(why, sizeof(why), buf ? "cannot write %.60s" : "out of memory", net->save_as);
		r = -1; goto fail;
	}
	say(PW_UPD_RUNNING, "Downloading PsiWeb %s...", remote);
	for (;;) {
		long want, k;
		if (total >= 0 && got >= total) break;
		if (R.local) {
			snprintf(q, sizeof(q), "?o=%ld&n=%d", got, PIECE);
			r = request("PsiWeb.sis", q, 0, 0, why, sizeof(why));
		} else
			r = request("PsiWeb.sis", NULL, got, PIECE, why, sizeof(why));
		if (r == -2) goto fail;
		if (r) {
			if (++tries > 6) goto fail;
			say(PW_UPD_RUNNING, "%ld KB - %.50s, retrying (%d)", got / 1024, why, tries);
			continue;
		}
		if (H.status == 200 && !R.local) {
			/* no Range support (e.g. a proxy): the whole file, in one go */
			if (got) { snprintf(why, sizeof(why), "the server cannot resume downloads"); r = -1; goto fail; }
			total = H.clen;
		} else if (H.total >= 0)
			total = H.total;
		if (total < 0 || total > 8 * 1024 * 1024) {
			snprintf(why, sizeof(why), "the server did not say how big the file is");
			r = -1; goto fail;
		}
		if (H.status == 200 && !R.local) {
			/* stream it straight to the file */
			while (got < total) {
				want = total - got < PIECE ? total - got : PIECE;
				k = body(buf, want);
				if (k == -2) { r = -2; goto fail; }
				if (k != want) { snprintf(why, sizeof(why), "the download broke off at %ld KB", got / 1024); r = -1; goto fail; }
				if ((long)fwrite(buf, 1, (size_t)want, f) != want) { snprintf(why, sizeof(why), "the disk is full"); r = -1; goto fail; }
				got += want;
				say(PW_UPD_RUNNING, "Downloading PsiWeb %s: %ld of %ld KB", remote, got / 1024, total / 1024);
			}
			after();
			break;
		}
		want = total - got < PIECE ? total - got : PIECE;
		k = H.clen == want ? body(buf, want) : -1;
		if (k == -2) { r = -2; goto fail; }
		if (k != want || (H.has_crc && (crc32(0L, buf, (unsigned)want) & 0xffffffffUL) != H.crc)) {
			pwn_close(1);
			if (++tries > 6) {
				snprintf(why, sizeof(why), "the piece at %ld KB kept failing", got / 1024);
				r = -1; goto fail;
			}
			say(PW_UPD_RUNNING, "%ld of %ld KB - retrying (%d)", got / 1024, total / 1024, tries);
			continue;
		}
		after();
		tries = 0;
		if ((long)fwrite(buf, 1, (size_t)want, f) != want) { snprintf(why, sizeof(why), "the disk is full"); r = -1; goto fail; }
		got += want;
		say(PW_UPD_RUNNING, "Downloading PsiWeb %s: %ld of %ld KB", remote, got / 1024, total / 1024);
	}
	fclose(f);
	f = NULL;
	free(buf);
	buf = NULL;
	pwn_close(1);
	say(PW_UPD_RUNNING, "Checking the release signature...");
	if (verify_file(net->save_as, remote, sig) != 0) {
		remove(net->save_as);
		snprintf(why, sizeof(why), "SIGNATURE CHECK FAILED - the download was deleted");
		r = -1; goto fail;
	}
	say(PW_UPD_READY, "PsiWeb %s is downloaded and its signature checks out", remote);
	return PW_UPD_READY;

fail:
	if (f) { fclose(f); remove(net->save_as); }
	free(buf);
	pwn_close(1);
	if (r == -2) {
		net->quit = 0;
		say(PW_UPD_FAILED, "Update cancelled");
	} else
		say(PW_UPD_FAILED, "Update failed: %s", why);
	return PW_UPD_FAILED;
}
