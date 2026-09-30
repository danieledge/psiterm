/* pmupdate.c - Tools > Update PsiMail: download and check a newer PsiMail.sis
 *
 * PsiWeb's updater (web/engine/pwupdate.c) on PsiMail's network layer, with
 * the same release key: PsiMail-version.txt names the newest version and
 * PsiMail.sis.sig holds an Ed25519 signature over
 *     "PsiMail update\n" <version> "\n" SHA-256(PsiMail.sis)
 * (tools/release/sign.py --product PsiMail). Nothing is offered to the
 * installer unless it verifies against the public key built in here.
 *
 * Sources (PmCmd.arg):
 *   "host:port"  PsiTerm's update server (server/psion-update.sh) on the
 *                local network: plain HTTP, ?o=&n= pieces each with a CRC-32
 *   "github"     raw.githubusercontent.com/danieledge/psiterm/main/dist/
 *                over TLS 1.3, pieces with HTTP Range on one connection
 * A piece is kept in memory until it is whole (and its CRC matches), so a
 * dropped line costs one piece.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "includes.h"          /* libtomcrypt: SHA-256 */
#include "pm.h"

extern int dropbear_ed25519_verify(const unsigned char *m, unsigned long mlen,
	const unsigned char *s, unsigned long slen, const unsigned char *pk);

#ifdef PM_TEST_KEY
/* tests: ssh/test/update-test.key signs the test releases */
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
#define PIECE    65536

static struct {
	char host[64];
	int port;
	int tls;
	char prefix[64];
	int local;
} R;

/* the last reply's headers */
static struct { int status; long clen; long total; int has_crc; unsigned long crc; int close; } H;

static unsigned long crc32_of(const unsigned char *p, long n)
{
	unsigned long c = 0xffffffffUL;
	long i;
	int k;
	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320UL & (0UL - (c & 1)));
	}
	return c ^ 0xffffffffUL;
}

static int route(const char *src, char *why, int whymax)
{
	memset(&R, 0, sizeof(R));
	if (!src[0] || !pm_strncasecmp(src, "github", 6)) {
		pm_copy(R.host, GH_HOST, sizeof(R.host));
		R.port = 443;
		R.tls = 1;
		pm_copy(R.prefix, GH_PATH, sizeof(R.prefix));
		return 0;
	}
	{
		const char *c = strchr(src, ':');
		int n = c ? (int)(c - src) : (int)strlen(src);
		if (n <= 0 || n >= (int)sizeof(R.host)) { snprintf(why, whymax, "No update server set"); return -1; }
		memcpy(R.host, src, n);
		R.host[n] = 0;
		R.port = c ? atoi(c + 1) : 8686;
		if (R.port <= 0) R.port = 8686;
		pm_copy(R.prefix, "/", sizeof(R.prefix));
		R.local = 1;
	}
	return 0;
}

static int g_conn = -1;        /* pmn_conn_id() of a kept-alive link */

/* sends a GET and reads the headers; the body follows.
   0 ok (200/206), -1 failed (why), -2 cancelled */
static int request(const char *name, const char *query, long rfrom, long rlen, char *why, int whymax)
{
	char req[400], line[200];
	int attempt;
	for (attempt = 0; attempt < 2; attempt++) {
		int reused = g_conn >= 0 && pmn_is_open() && pmn_conn_id() == g_conn;
		int n = 0;
		if (!reused) {
			if (pmn_connect(R.host, R.port, R.tls, why, whymax) != 0)
				return pm_cancelled() ? -2 : -1;
			g_conn = pmn_conn_id();
		}
		snprintf(req, sizeof(req),
			"GET %s%s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PsiMail/%s\r\nConnection: %s\r\n",
			R.prefix, name, query ? query : "", R.host, pm_shared()->net.version,
			R.local ? "close" : "keep-alive");
		if (rlen > 0 && !R.local)
			snprintf(req + strlen(req), sizeof(req) - strlen(req),
				"Range: bytes=%ld-%ld\r\n", rfrom, rfrom + rlen - 1);
		strcat(req, "\r\n");
		if (pmn_write(req, (int)strlen(req)) != 0) {
			pmn_close(1);
			g_conn = -1;
			if (reused) continue;
			snprintf(why, whymax, "Could not send the request");
			return -1;
		}
		memset(&H, 0, sizeof(H));
		H.clen = -1;
		H.total = -1;
		for (;;) {
			int c = pmn_getc(30000);
			if (c < 0) {
				if (c == PMN_CANCEL) return -2;
				pmn_close(1);
				g_conn = -1;
				if (reused && !H.status) break;      /* the kept link had gone: again */
				snprintf(why, whymax, "No reply from %s", R.host);
				return -1;
			}
			if (c == '\r') continue;
			if (c != '\n') { if (n < (int)sizeof(line) - 1) line[n++] = (char)c; continue; }
			line[n] = 0;
			if (n == 0 && !H.status) continue;      /* modem CONNECT leftovers */
			if (n == 0) {
				if (H.status == 200 || H.status == 206) return 0;
				snprintf(why, whymax, "The server said %d for %.40s", H.status, name);
				pmn_close(1);
				g_conn = -1;
				return -1;
			}
			if (!H.status && !strncmp(line, "HTTP/", 5)) {
				char *sp = strchr(line, ' ');
				H.status = sp ? atoi(sp + 1) : 0;
				H.close = !strncmp(line, "HTTP/1.0", 8);
			} else if (!pm_strncasecmp(line, "Content-Length:", 15)) H.clen = atol(line + 15);
			else if (!pm_strncasecmp(line, "Content-Range:", 14)) {
				char *sl = strchr(line, '/');
				if (sl) H.total = atol(sl + 1);
			} else if (!pm_strncasecmp(line, "X-Total:", 8)) H.total = atol(line + 8);
			else if (!pm_strncasecmp(line, "X-CRC32:", 8)) { H.crc = strtoul(line + 8, 0, 16); H.has_crc = 1; }
			else if (!pm_strncasecmp(line, "Connection:", 11) && pm_stristr(line, "close")) H.close = 1;
			else if (!pm_strncasecmp(line, "Transfer-Encoding:", 18) && pm_stristr(line, "chunked")) {
				snprintf(why, whymax, "The server sent a chunked reply");
				pmn_close(1);
				g_conn = -1;
				return -1;
			}
			n = 0;
		}
	}
	snprintf(why, whymax, "The connection to %s kept dropping", R.host);
	return -1;
}

/* n body bytes: how many came, or -2 if cancelled */
static long body(unsigned char *buf, long n)
{
	long k = 0;
	while (k < n) {
		int r = pmn_read(buf + k, n - k > 4096 ? 4096 : (int)(n - k), 20000);
		if (r == PMN_CANCEL) return -2;
		if (r <= 0) break;
		k += r;
	}
	return k;
}

static void after(void)
{
	if (H.close || R.local) { pmn_close(1); g_conn = -1; }
}

static int fetch_small(const char *name, char *buf, int max, char *why, int whymax)
{
	long k, want;
	int r = request(name, 0, 0, 0, why, whymax);
	if (r) return r;
	want = H.clen >= 0 && H.clen < max ? H.clen : max - 1;
	k = body((unsigned char *)buf, want);
	if (k == -2) return -2;
	if (H.clen < 0 || H.clen >= max) { pmn_close(1); g_conn = -1; }
	else after();
	buf[k < 0 ? 0 : k] = 0;
	return 0;
}

/* "0.3" < "0.3.1" < "0.4" */
int pm_version_newer(const char *remote, const char *local)
{
	int r[3] = { 0, 0, 0 }, l[3] = { 0, 0, 0 }, i;
	sscanf(remote, "%d.%d.%d", &r[0], &r[1], &r[2]);
	sscanf(local, "%d.%d.%d", &l[0], &l[1], &l[2]);
	for (i = 0; i < 3; i++)
		if (r[i] != l[i]) return r[i] > l[i];
	return 0;
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
	unsigned char msg[15 + 16 + 1 + 32], buf[1024];
	hash_state h;
	int n, vl = (int)strlen(ver), k;
	FILE *f = fopen(file, "rb");
	if (!f || vl > 16) { if (f) fclose(f); return -1; }
	sha256_init(&h);
	while ((k = (int)fread(buf, 1, sizeof(buf), f)) > 0)
		sha256_process(&h, buf, k);
	fclose(f);
	memcpy(msg, "PsiMail update\n", 15); n = 15;
	memcpy(msg + n, ver, vl); n += vl;
	msg[n++] = '\n';
	sha256_done(&h, msg + n); n += 32;
	return dropbear_ed25519_verify(msg, n, sig, 64, KUpdateKey);
}

/* src: "host:port" or "github"; save: where PsiMail.sis goes */
int pm_update(const char *src, const char *save, char *why, int whymax)
{
	PmShared *s = pm_shared();
	char remote[24], sigtxt[300], sigver[24], q[48];
	unsigned char sig[64];
	unsigned char *buf = 0;
	long got = 0, total = -1;
	int n, r, tries = 0;
	FILE *f = 0;

	s->update_ready = 0;
	s->update_version[0] = 0;
	why[0] = 0;
	g_conn = -1;
	if (route(src, why, whymax)) return PM_RES_FAILED;
	pm_progress("Looking for a new PsiMail (%s)...", R.local ? R.host : "GitHub");

	r = fetch_small("PsiMail-version.txt", remote, sizeof(remote), why, whymax);
	if (r) goto fail;
	for (n = 0; remote[n] && remote[n] != '\r' && remote[n] != '\n' && remote[n] != ' '; n++) ;
	remote[n] = 0;
	if (!n) { snprintf(why, whymax, "PsiMail-version.txt was empty"); r = -1; goto fail; }
	pm_copy(s->update_version, remote, sizeof(s->update_version));
	if (!pm_version_newer(remote, s->net.version)) {
		pmn_close(1);
		snprintf(why, whymax, "PsiMail %s is the newest (the server has %s)", s->net.version, remote);
		return PM_RES_OK;
	}
	r = fetch_small("PsiMail.sis.sig", sigtxt, sizeof(sigtxt), why, whymax);
	if (r) goto fail;
	if (parse_sig(sigtxt, sigver, sizeof(sigver), sig) != 0) {
		snprintf(why, whymax, "The release signature (PsiMail.sis.sig) is missing or damaged");
		r = -1; goto fail;
	}
	if (strcmp(sigver, remote) != 0) {
		snprintf(why, whymax, "The signature is for %.10s, not %.10s", sigver, remote);
		r = -1; goto fail;
	}
	buf = (unsigned char *)malloc(PIECE);
	f = fopen(save, "wb");
	if (!buf || !f) {
		snprintf(why, whymax, buf ? "Cannot write %.60s" : "Out of memory", save);
		r = -1; goto fail;
	}
	pm_progress("Downloading PsiMail %s...", remote);
	for (;;) {
		long want, k;
		if (total >= 0 && got >= total) break;
		if (R.local) {
			snprintf(q, sizeof(q), "?o=%ld&n=%d", got, PIECE);
			r = request("PsiMail.sis", q, 0, 0, why, whymax);
		} else
			r = request("PsiMail.sis", 0, got, PIECE, why, whymax);
		if (r == -2) goto fail;
		if (r) {
			if (++tries > 6) goto fail;
			pm_progress("%ld KB - %.50s, trying again (%d)", got / 1024, why, tries);
			continue;
		}
		if (H.status == 200 && !R.local) {
			/* no Range: the whole file in one go (only from the start) */
			if (got) { snprintf(why, whymax, "The server cannot resume downloads"); r = -1; goto fail; }
			total = H.clen;
		} else if (H.total >= 0)
			total = H.total;
		if (total < 0 || total > 8 * 1024 * 1024) {
			snprintf(why, whymax, "The server did not say how big the file is");
			r = -1; goto fail;
		}
		if (H.status == 200 && !R.local) {
			while (got < total) {
				want = total - got < PIECE ? total - got : PIECE;
				k = body(buf, want);
				if (k == -2) { r = -2; goto fail; }
				if (k != want) { snprintf(why, whymax, "The download broke off at %ld KB", got / 1024); r = -1; goto fail; }
				if ((long)fwrite(buf, 1, (size_t)want, f) != want) { snprintf(why, whymax, "The disk is full"); r = -1; goto fail; }
				got += want;
				pm_progress("Downloading PsiMail %s: %ld of %ld KB", remote, got / 1024, total / 1024);
			}
			after();
			break;
		}
		want = total - got < PIECE ? total - got : PIECE;
		k = H.clen == want ? body(buf, want) : -1;
		if (k == -2) { r = -2; goto fail; }
		if (k != want || (H.has_crc && crc32_of(buf, want) != H.crc)) {
			pmn_close(1);
			g_conn = -1;
			if (++tries > 6) {
				snprintf(why, whymax, "The piece at %ld KB kept failing", got / 1024);
				r = -1; goto fail;
			}
			pm_log("update: piece at %ld: %ld of %ld bytes, crc %s", got, k, want,
				k == want ? "bad" : "-");
			pm_progress("%ld of %ld KB - trying again (%d)", got / 1024, total / 1024, tries);
			continue;
		}
		after();
		tries = 0;
		if ((long)fwrite(buf, 1, (size_t)want, f) != want) { snprintf(why, whymax, "The disk is full"); r = -1; goto fail; }
		got += want;
		pm_progress("Downloading PsiMail %s: %ld of %ld KB", remote, got / 1024, total / 1024);
	}
	if (fclose(f) != 0) { f = 0; remove(save); snprintf(why, whymax, "The disk is full"); r = -1; goto fail; }
	f = 0;
	free(buf);
	buf = 0;
	pmn_close(1);
	g_conn = -1;
	pm_progress("Checking the release signature...");
	if (verify_file(save, remote, sig) != 0) {
		remove(save);
		snprintf(why, whymax, "SIGNATURE CHECK FAILED - the download was deleted");
		return PM_RES_FAILED;
	}
	s->update_ready = 1;
	pm_copy(s->last_file, save, sizeof(s->last_file));
	snprintf(why, whymax, "PsiMail %s is downloaded and its signature checks out", remote);
	return PM_RES_OK;

fail:
	if (f) { fclose(f); remove(save); }
	free(buf);
	pmn_close(1);
	g_conn = -1;
	if (r == -2) { pm_copy(why, "Update stopped", whymax); return PM_RES_CANCELLED; }
	pm_log("update failed: %s", why);
	return PM_RES_FAILED;
}
