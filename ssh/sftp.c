/* sftp.c - PsiTerm's file transfer: a small SFTP (version 3) client.
 *
 * It runs inside psissh on a second "session" channel of the SSH connection
 * that is already logged in, so no second login is needed. The channel asks
 * for the "sftp" subsystem, which every modern OpenSSH server offers (scp's
 * old protocol is being retired). PsiTerm posts one request at a time in
 * the shared memory (psishared.h, xfer_*); this file does it and reports.
 *
 * How it plugs into Dropbear: the channel's local end is the pretend file
 * descriptor PSI_FD_SFTP. psishim.c sends Dropbear's read()/write()/select()
 * on it here: what the server sends arrives in psi_sftp_write(), and what
 * psi_sftp_read() hands back goes to the server. Everything received is used
 * at once (a piece of a download goes straight into the Psion file), so the
 * channel's buffer never fills.
 *
 * Slow links (a 57600 baud modem): data moves in 4-8 KB pieces with a few in
 * flight, so the line stays busy without long waits; Stop takes effect at
 * once (late replies are recognised and ignored), and if the server stops
 * answering for a minute the transfer fails instead of hanging.
 */

#include "includes.h"
#include "packet.h"
#include "buffer.h"
#include "session.h"
#include "dbutil.h"
#include "channel.h"
#include "ssh.h"
#include "psishared.h"

#define PSI_FD_SFTP 53

extern PsiShared* pg_shared(void);

/* ---- SFTP v3 (draft-ietf-secsh-filexfer-02) */
#define FXP_INIT      1
#define FXP_VERSION   2
#define FXP_OPEN      3
#define FXP_CLOSE     4
#define FXP_READ      5
#define FXP_WRITE     6
#define FXP_OPENDIR   11
#define FXP_READDIR   12
#define FXP_REMOVE    13
#define FXP_REALPATH  16
#define FXP_STAT      17
#define FXP_STATUS    101
#define FXP_HANDLE    102
#define FXP_DATA      103
#define FXP_NAME      104
#define FXP_ATTRS     105

#define FXF_READ   0x01
#define FXF_WRITE  0x02
#define FXF_CREAT  0x08
#define FXF_TRUNC  0x10

#define FILEXFER_ATTR_SIZE        0x01
#define FILEXFER_ATTR_UIDGID      0x02
#define FILEXFER_ATTR_PERMISSIONS 0x04
#define FILEXFER_ATTR_ACMODTIME   0x08
#define FILEXFER_ATTR_EXTENDED    0x80000000u

#define FX_OK                0
#define FX_EOF               1
#define FX_NO_SUCH_FILE      2
#define FX_PERMISSION_DENIED 3

/* ---- sizes */
#define WRITE_LEN   4096        /* upload piece */
#define MAX_WRITES  4           /* ...in flight */
#define READ_LEN    8192        /* download piece */
#define MAX_READS   3
#define MAX_REQS    8
#define IN_MAX      (65536 + 64)  /* largest reply we take (a READDIR batch) */
#define OUT_MAX     20480
#define REPLY_SECS  60          /* no reply for this long: give up */

/* ---- the channel */
enum { CH_NONE, CH_OPENING, CH_SUBSYS, CH_INIT, CH_READY, CH_CLOSING };
static int ch_state = CH_NONE;
static struct Channel *ch;
static int close_when_open;           /* the shell went while ours was opening */
static int running;                   /* logged in: requests can be served */

static unsigned char inbuf[IN_MAX];
static unsigned int inlen;
static unsigned char outbuf[OUT_MAX];
static unsigned int outlen, outpos;

/* ---- the request being served */
enum { K_FREE, K_STAT, K_OPEN, K_WRITE, K_READ, K_CLOSE, K_REMOVE, K_REALPATH, K_OPENDIR, K_READDIR };
struct sreq { unsigned int id; int kind; unsigned long off; unsigned int len; };
static struct sreq reqs[MAX_REQS];
static unsigned int next_id = 1;

static int op;                        /* PSI_XOP_*, 0 = idle */
static unsigned int serving;          /* the xfer_req being served */
static int failed;                    /* PSI_XFER_* once something went wrong */
static char failmsg[96];
static FILE *lf;                      /* the Psion file */
static unsigned long lf_pos;          /* GET: where the next fwrite goes */
static unsigned char handle[256];
static unsigned int hlen;
static int have_handle;
static unsigned long off_next;        /* next offset to send / ask for */
static unsigned long total;           /* the file's size (GET: 0 if not known) */
static unsigned long done_bytes;
static int eof_seen;
static unsigned long gap_off[MAX_REQS];   /* GET: rest of a short read, to ask again */
static unsigned int gap_len[MAX_REQS];
static int gaps;
static time_t last_heard;             /* the server's last reply (or our last request) */

static void start_op(void);
static void finish(int result);

#ifdef PSI_SFTP_TRACE
/* (debugging only: what happens when, on the terminal) */
extern void pg_out_write(const void*, int);
static void trace(const char *what, int a)
{
	char line[80];
	struct timeval tv;
	gettimeofday(&tv, NULL);
	sprintf(line, "[sftp %ld.%03ld %s %d]\r\n", (long)tv.tv_sec % 1000, (long)tv.tv_usec / 1000, what, a);
	pg_out_write(line, strlen(line));
}
#else
#define trace(w, a)
#endif

/* ================================================================ helpers */

static unsigned int get32(const unsigned char *p)
{
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

static void put32at(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

/* reading a reply: a cursor that never runs past the end */
struct rd { const unsigned char *p; unsigned int left; int bad; };

static unsigned int rd32(struct rd *r)
{
	unsigned int v;
	if (r->left < 4) { r->bad = 1; r->left = 0; return 0; }
	v = get32(r->p);
	r->p += 4; r->left -= 4;
	return v;
}

static unsigned long rd64(struct rd *r)
{
	unsigned int hi = rd32(r), lo = rd32(r);
	if (hi) return 0xffffffffUL;      /* > 4 GB: more than a Psion holds anyway */
	return lo;
}

static const unsigned char *rdstr(struct rd *r, unsigned int *len)
{
	unsigned int n = rd32(r);
	const unsigned char *s = r->p;
	if (r->bad || n > r->left) { r->bad = 1; r->left = 0; *len = 0; return (const unsigned char*)""; }
	r->p += n; r->left -= n;
	*len = n;
	return s;
}

/* attributes: we want the size and whether it is a folder */
static void rdattrs(struct rd *r, int *has_size, unsigned long *size, unsigned int *perm, int *has_perm)
{
	unsigned int flags = rd32(r), i, n, l;
	*has_size = 0; *has_perm = 0; *size = 0; *perm = 0;
	if (flags & FILEXFER_ATTR_SIZE) { *size = rd64(r); *has_size = 1; }
	if (flags & FILEXFER_ATTR_UIDGID) { rd32(r); rd32(r); }
	if (flags & FILEXFER_ATTR_PERMISSIONS) { *perm = rd32(r); *has_perm = 1; }
	if (flags & FILEXFER_ATTR_ACMODTIME) { rd32(r); rd32(r); }
	if (flags & FILEXFER_ATTR_EXTENDED) {
		n = rd32(r);
		for (i = 0; i < n && !r->bad; i++) { rdstr(r, &l); rdstr(r, &l); }
	}
}

#define S_TYPE(p) ((p) & 0170000)
#define T_DIR 0040000
#define T_REG 0100000

/* ---- building requests in outbuf */
static unsigned int pkt_start;

/* room for a request of about 'need' bytes? (moves what is unsent to the front) */
static int out_room(unsigned int need)
{
	if (outpos == outlen) outpos = outlen = 0;
	if (outlen + need > OUT_MAX && outpos > 0) {
		memmove(outbuf, outbuf + outpos, outlen - outpos);
		outlen -= outpos;
		outpos = 0;
	}
	return outlen + need <= OUT_MAX;
}

static void put8(unsigned int v) { outbuf[outlen++] = (unsigned char)v; }
static void put32(unsigned int v) { put32at(outbuf + outlen, v); outlen += 4; }
static void put64(unsigned long v) { put32(0); put32((unsigned int)v); }
static void putstr(const void *s, unsigned int n) { put32(n); memcpy(outbuf + outlen, s, n); outlen += n; }

static void pkt_begin(int type)
{
	pkt_start = outlen;
	put32(0);
	put8(type);
}

static void pkt_end(void)
{
	put32at(outbuf + pkt_start, outlen - pkt_start - 4);
}

static struct sreq *new_req(int kind, unsigned long off, unsigned int len)
{
	int i;
	for (i = 0; i < MAX_REQS; i++)
		if (reqs[i].kind == K_FREE) {
			reqs[i].kind = kind;
			reqs[i].id = next_id++;
			reqs[i].off = off;
			reqs[i].len = len;
			return &reqs[i];
		}
	return NULL;
}

static int count_reqs(int kind)
{
	int i, n = 0;
	for (i = 0; i < MAX_REQS; i++)
		if (reqs[i].kind == kind) n++;
	return n;
}

static int busy_reqs(void)
{
	int i, n = 0;
	for (i = 0; i < MAX_REQS; i++)
		if (reqs[i].kind != K_FREE) n++;
	return n;
}

/* a request that names a path (STAT, OPENDIR, REALPATH, REMOVE) */
static int send_path(int type, int kind, const char *path)
{
	unsigned int n = strlen(path);
	struct sreq *q;
	if (!out_room(n + 32)) return -1;
	q = new_req(kind, 0, 0);
	if (!q) return -1;
	pkt_begin(type);
	put32(q->id);
	putstr(path, n);
	pkt_end();
	last_heard = time(NULL);
	return 0;
}

/* a request on the open handle (CLOSE, READDIR). kind K_FREE: nobody waits for the answer */
static int send_handle(int type, int kind)
{
	struct sreq *q = NULL;
	unsigned int id = next_id++;
	if (!out_room(hlen + 32)) return -1;
	if (kind != K_FREE) {
		q = new_req(kind, 0, 0);
		if (!q) return -1;
		id = q->id;
	}
	pkt_begin(type);
	put32(id);
	putstr(handle, hlen);
	pkt_end();
	last_heard = time(NULL);
	return 0;
}

static int send_open(const char *path, unsigned int flags)
{
	unsigned int n = strlen(path);
	struct sreq *q;
	if (!out_room(n + 40)) return -1;
	q = new_req(K_OPEN, 0, 0);
	if (!q) return -1;
	pkt_begin(FXP_OPEN);
	put32(q->id);
	putstr(path, n);
	put32(flags);
	put32(0);                         /* no attributes: the server's usual permissions */
	pkt_end();
	last_heard = time(NULL);
	return 0;
}

static PsiShared *sh(void) { return pg_shared(); }

static void set_fail(int result, const char *msg)
{
	if (failed) return;               /* the first reason is the one to report */
	failed = result;
	if (msg) {
		strncpy(failmsg, msg, sizeof(failmsg) - 1);
		failmsg[sizeof(failmsg) - 1] = 0;
	}
}

/* a STATUS reply that is not OK: what it means for the user */
static void status_fail(unsigned int code, const unsigned char *msg, unsigned int mlen)
{
	char m[96];
	if (mlen > sizeof(m) - 1) mlen = sizeof(m) - 1;
	memcpy(m, msg, mlen);
	m[mlen] = 0;
	if (code == FX_NO_SUCH_FILE) set_fail(PSI_XFER_NOT_FOUND, m);
	else if (code == FX_PERMISSION_DENIED) set_fail(PSI_XFER_DENIED, m);
	else set_fail(PSI_XFER_FAILED, m[0] ? m : "the server refused");
}

/* ================================================================ the channel */

static int sftp_chan_init(struct Channel *c);
static void sftp_chan_cleanup(const struct Channel *c);

static const struct ChanType clisftp = {
	"session",
	sftp_chan_init,       /* inithandler: the server said yes to the channel */
	NULL,                 /* check_close */
	NULL,                 /* reqhandler (exit-status etc: the default answer) */
	NULL,                 /* closehandler */
	sftp_chan_cleanup     /* cleanup: the channel has gone */
};

static void open_channel(void)
{
	if (send_msg_channel_open_init(PSI_FD_SFTP, &clisftp) == DROPBEAR_FAILURE) {
		finish(PSI_XFER_NO_SFTP);
		return;
	}
	encrypt_packet();
	ch_state = CH_OPENING;
	ch = NULL;
	close_when_open = 0;
	inlen = 0;
	outlen = outpos = 0;
	last_heard = time(NULL);
}

/* our end of the channel: send CLOSE and stop moving data on it. The
   channel goes (sftp_chan_cleanup) when the server closes its end too. */
static void close_channel(void)
{
	if (!ch) return;
	if (ch->await_open) { close_when_open = 1; return; }
	if (ch->sent_close) return;
	CHECKCLEARTOWRITE();
	buf_putbyte(ses.writepayload, SSH_MSG_CHANNEL_CLOSE);
	buf_putint(ses.writepayload, ch->remotechan);
	encrypt_packet();
	ch->sent_eof = 1;
	ch->sent_close = 1;
	ch->readfd = -1;                  /* FD_CLOSED: Dropbear moves no more data here */
	ch->writefd = -1;
	ch_state = CH_CLOSING;
}

static int sftp_chan_init(struct Channel *c)
{
	ch = c;
	c->errfd = -1;                    /* the server's stderr: not wanted */
	c->extrabuf = NULL;
	if (close_when_open) {
		close_channel();
		return 0;
	}
	/* "subsystem sftp", with a reply: a server without SFTP says no */
	start_send_channel_request(c, "subsystem");
	buf_putbyte(ses.writepayload, 1);
	buf_putstring(ses.writepayload, "sftp", 4);
	encrypt_packet();
	ch_state = CH_SUBSYS;
	last_heard = time(NULL);
	return 0;
}

static void sftp_chan_cleanup(const struct Channel *c)
{
	int was = ch_state;
	(void)c;
	ch = NULL;
	ch_state = CH_NONE;
	inlen = 0;
	outlen = outpos = 0;
	/* (a channel we closed ourselves: a request waiting for it gets a new one) */
	if (op && was != CH_CLOSING) {
		/* refused (channel open failure or straight after asking) or lost */
		if (was == CH_OPENING || was == CH_SUBSYS || was == CH_INIT)
			finish(PSI_XFER_NO_SFTP);
		else
			finish(PSI_XFER_LINK);
	}
}

/* SSH_MSG_CHANNEL_SUCCESS / FAILURE (cli-session.c sends them here): the
   answer to our subsystem request. Others (keepalives) are ignored. */
void psi_sftp_chanreply(int success, unsigned int chan)
{
	if (!ch || ch->index != chan || ch_state != CH_SUBSYS)
		return;
	if (!success) {
		close_channel();
		if (op) finish(PSI_XFER_NO_SFTP);
		return;
	}
	/* INIT: the SFTP conversation starts */
	if (out_room(16)) {
		pkt_begin(FXP_INIT);
		put32(3);
		pkt_end();
	}
	ch_state = CH_INIT;
	last_heard = time(NULL);
}

/* the shell's channel is closing: ours must not keep the session alive */
void psi_sftp_shell_closed(void)
{
	if (op) finish(PSI_XFER_LINK);
	close_channel();
}

/* ================================================================ finishing */

static void close_local(void)
{
	if (lf) { fclose(lf); lf = NULL; }
}

static void finish(int result)
{
	PsiShared *s = sh();
	int i;
	if (!op) return;
	if (op == PSI_XOP_GET && lf) {
		/* a finished download is only good if every byte reached the card */
		if (fflush(lf) != 0 || ferror(lf)) {
			if (result == PSI_XFER_OK) result = PSI_XFER_LOCAL_WRITE;
		}
		if (fclose(lf) != 0 && result == PSI_XFER_OK)
			result = PSI_XFER_LOCAL_WRITE;
		lf = NULL;
		if (result != PSI_XFER_OK)
			remove(s->xfer_local);    /* no half files left behind */
	}
	close_local();
	/* a handle still open on the server: close it (nobody waits for the
	   answer); a half upload is removed too */
	if (have_handle && ch && ch_state == CH_READY) {
		send_handle(FXP_CLOSE, K_FREE);
		if (op == PSI_XOP_PUT && result != PSI_XFER_OK && out_room(strlen(s->xfer_remote) + 32)) {
			pkt_begin(FXP_REMOVE);
			put32(next_id++);
			putstr(s->xfer_remote, strlen(s->xfer_remote));
			pkt_end();
		}
	}
	have_handle = 0;
	for (i = 0; i < MAX_REQS; i++) reqs[i].kind = K_FREE;   /* late replies are now strays */
	gaps = 0;
	s->xfer_done = done_bytes;
	s->xfer_msg[0] = 0;
	if (result != PSI_XFER_OK && result != PSI_XFER_CANCELLED && failmsg[0])
		memcpy(s->xfer_msg, failmsg, sizeof(s->xfer_msg));
	s->xfer_result = result;
	op = 0;
	s->xfer_ack = serving;            /* last: PsiTerm now reads the result */
}

/* ================================================================ the requests */

static void start_op(void)
{
	PsiShared *s = sh();
	op = s->xfer_op;
	serving = s->xfer_req;
	trace("start", op);
	failed = 0;
	failmsg[0] = 0;
	have_handle = 0;
	off_next = total = done_bytes = 0;
	eof_seen = 0;
	gaps = 0;
	s->xfer_local[sizeof(s->xfer_local) - 1] = 0;
	s->xfer_remote[sizeof(s->xfer_remote) - 1] = 0;
	s->xfer_done = 0;
	s->xfer_total = 0;
	s->xfer_exists = 0;
	s->xfer_list_len = 0;
	s->xfer_list_more = 0;
	s->xfer_msg[0] = 0;
	if (op < PSI_XOP_PUT || op > PSI_XOP_STAT) {
		op = PSI_XOP_STAT;            /* (so finish() reports) */
		finish(PSI_XFER_FAILED);
		return;
	}
	if (op == PSI_XOP_PUT) {
		long size;
		lf = fopen(s->xfer_local, "rb");
		if (!lf) { finish(PSI_XFER_LOCAL_READ); return; }
		if (fseek(lf, 0, SEEK_END) != 0 || (size = ftell(lf)) < 0 || fseek(lf, 0, SEEK_SET) != 0) {
			finish(PSI_XFER_LOCAL_READ);
			return;
		}
		total = (unsigned long)size;
		s->xfer_total = total;
	}
	if (op != PSI_XOP_LIST && !s->xfer_remote[0]) {
		set_fail(PSI_XFER_NOT_FOUND, "no file name");
		finish(PSI_XFER_NOT_FOUND);
		return;
	}
	if (ch_state == CH_NONE)
		open_channel();
	/* the first request goes once the channel is ready (step_ready) */
}

/* the channel has just become ready, or was already: send the first request */
static void first_request(void)
{
	PsiShared *s = sh();
	switch (op) {
	case PSI_XOP_PUT:
		if (send_open(s->xfer_remote, FXF_WRITE | FXF_CREAT | FXF_TRUNC) < 0) finish(PSI_XFER_FAILED);
		break;
	case PSI_XOP_GET:
	case PSI_XOP_STAT:
		if (send_path(FXP_STAT, K_STAT, s->xfer_remote) < 0) finish(PSI_XFER_FAILED);
		break;
	case PSI_XOP_LIST:
		if (send_path(FXP_REALPATH, K_REALPATH, s->xfer_remote[0] ? s->xfer_remote : ".") < 0)
			finish(PSI_XFER_FAILED);
		break;
	}
}

/* keep the pipeline full: more WRITEs (PUT) or READs (GET) */
static void pump(void)
{
	PsiShared *s = sh();
	if (!op || ch_state != CH_READY || !have_handle)
		return;
	if (s->xfer_cancel) {
		set_fail(PSI_XFER_CANCELLED, NULL);
		finish(PSI_XFER_CANCELLED);
		return;
	}
	if (op == PSI_XOP_PUT) {
		while (!failed && off_next < total && count_reqs(K_WRITE) < MAX_WRITES) {
			unsigned int want = total - off_next < WRITE_LEN ? (unsigned int)(total - off_next) : WRITE_LEN;
			struct sreq *q;
			size_t got;
			if (!out_room(want + hlen + 40)) break;
			q = new_req(K_WRITE, off_next, want);
			if (!q) break;
			pkt_begin(FXP_WRITE);
			put32(q->id);
			putstr(handle, hlen);
			put64(off_next);
			put32(want);
			got = fread(outbuf + outlen, 1, want, lf);
			if (got != want) {
				/* the Psion file could not be read (card taken out?) */
				outlen = pkt_start;
				q->kind = K_FREE;
				set_fail(PSI_XFER_LOCAL_READ, NULL);
				break;
			}
			outlen += want;
			pkt_end();
			off_next += want;
			last_heard = time(NULL);
		}
		if (count_reqs(K_WRITE) == 0 && count_reqs(K_CLOSE) == 0 && (failed || off_next >= total)) {
			if (failed) { finish(failed); return; }
			/* all written: CLOSE, and the server's answer says it is safe */
			if (send_handle(FXP_CLOSE, K_CLOSE) == 0)
				have_handle = 0;
		}
		return;
	}
	if (op == PSI_XOP_GET) {
		for (;;) {
			unsigned long off;
			unsigned int len;
			struct sreq *q;
			if (failed || count_reqs(K_READ) >= MAX_READS) break;
			if (gaps > 0) {
				off = gap_off[0]; len = gap_len[0];
			} else {
				if (eof_seen) break;
				if (total && off_next >= total) break;   /* asked for it all */
				off = off_next; len = READ_LEN;
				if (total && total - off_next < len)
					len = (unsigned int)(total - off_next);   /* no asking past the end */
			}
			if (!out_room(hlen + 40)) break;
			q = new_req(K_READ, off, len);
			if (!q) break;
			pkt_begin(FXP_READ);
			put32(q->id);
			putstr(handle, hlen);
			put64(off);
			put32(len);
			pkt_end();
			last_heard = time(NULL);
			if (gaps > 0) {
				int i;
				for (i = 1; i < gaps; i++) { gap_off[i - 1] = gap_off[i]; gap_len[i - 1] = gap_len[i]; }
				gaps--;
			} else
				off_next += len;
		}
		if (count_reqs(K_READ) == 0 && count_reqs(K_CLOSE) == 0 && gaps == 0
			&& (failed || eof_seen || (total && off_next >= total))) {
			if (failed) { finish(failed); return; }
			if (send_handle(FXP_CLOSE, K_CLOSE) == 0)
				have_handle = 0;
		}
	}
}

/* LIST: add one entry to the shared list */
static void list_add(const unsigned char *name, unsigned int nlen, int has_size, unsigned long size,
	unsigned int perm, int has_perm)
{
	PsiShared *s = sh();
	char line[40];
	int t, ll, len = s->xfer_list_len;
	if ((nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.'))
		return;
	if (nlen == 0 || memchr(name, '\n', nlen) || memchr(name, '\t', nlen))
		return;                       /* names we could not show safely */
	t = !has_perm ? 'f' : S_TYPE(perm) == T_DIR ? 'd' : S_TYPE(perm) == T_REG ? 'f' : 'l';
	ll = sprintf(line, "%c%lu\t", t, has_size ? size : 0UL);
	if (len + ll + (int)nlen + 1 > PSI_XFER_LIST_SIZE) {
		s->xfer_list_more = 1;
		return;
	}
	memcpy(s->xfer_list + len, line, ll);
	memcpy(s->xfer_list + len + ll, name, nlen);
	s->xfer_list[len + ll + nlen] = '\n';
	s->xfer_list_len = len + ll + nlen + 1;
}

/* one whole reply from the server */
static void handle_reply(const unsigned char *p, unsigned int n)
{
	PsiShared *s = sh();
	struct rd r;
	int type, i;
	unsigned int id;
	struct sreq *q = NULL;
	struct sreq qc;

	if (n < 1) return;
	type = p[0];
	trace("reply", type);
	r.p = p + 1; r.left = n - 1; r.bad = 0;
	last_heard = time(NULL);

	if (type == FXP_VERSION) {
		if (ch_state == CH_INIT) {
			ch_state = CH_READY;
			if (op) first_request();
		}
		return;
	}
	id = rd32(&r);
	if (r.bad) return;
	for (i = 0; i < MAX_REQS; i++)
		if (reqs[i].kind != K_FREE && reqs[i].id == id) { q = &reqs[i]; break; }
	if (!q || !op)
		return;                       /* a late reply to something given up */
	qc = *q;
	q->kind = K_FREE;

	if (type == FXP_STATUS) {
		unsigned int code = rd32(&r), mlen;
		const unsigned char *msg = rdstr(&r, &mlen);
		if (r.bad) { msg = (const unsigned char*)""; mlen = 0; }
		switch (qc.kind) {
		case K_STAT:
			if (op == PSI_XOP_STAT && code == FX_NO_SUCH_FILE) {
				s->xfer_exists = 0;
				finish(PSI_XFER_OK);
				return;
			}
			status_fail(code, msg, mlen);
			finish(failed);
			return;
		case K_OPEN:
		case K_REALPATH:
		case K_OPENDIR:
			status_fail(code, msg, mlen);
			finish(failed);
			return;
		case K_WRITE:
			if (code != FX_OK) status_fail(code, msg, mlen);
			else {
				done_bytes += qc.len;
				s->xfer_done = done_bytes;
			}
			break;
		case K_READ:
			if (code == FX_EOF) eof_seen = 1;
			else status_fail(code, msg, mlen);
			break;
		case K_READDIR:
			if (code != FX_EOF) status_fail(code, msg, mlen);
			if (send_handle(FXP_CLOSE, K_CLOSE) == 0) have_handle = 0;
			return;
		case K_CLOSE:
			/* for an upload, the last chance for the server to say "disk full" */
			if (code != FX_OK && op == PSI_XOP_PUT) status_fail(code, msg, mlen);
			finish(failed ? failed : PSI_XFER_OK);
			return;
		}
		pump();
		return;
	}

	if (type == FXP_HANDLE && (qc.kind == K_OPEN || qc.kind == K_OPENDIR)) {
		unsigned int len;
		const unsigned char *h = rdstr(&r, &len);
		if (r.bad || len == 0 || len > sizeof(handle)) { set_fail(PSI_XFER_FAILED, "bad reply from the server"); finish(failed); return; }
		memcpy(handle, h, len);
		hlen = len;
		have_handle = 1;
		if (qc.kind == K_OPENDIR) {
			if (send_handle(FXP_READDIR, K_READDIR) < 0) finish(PSI_XFER_FAILED);
			return;
		}
		if (op == PSI_XOP_GET) {
			lf = fopen(s->xfer_local, "wb");
			if (!lf) { set_fail(PSI_XFER_LOCAL_WRITE, NULL); finish(failed); return; }
			lf_pos = 0;
		}
		pump();
		return;
	}

	if (type == FXP_ATTRS && qc.kind == K_STAT) {
		int has_size, has_perm;
		unsigned long size;
		unsigned int perm;
		rdattrs(&r, &has_size, &size, &perm, &has_perm);
		if (op == PSI_XOP_STAT) {
			s->xfer_exists = (has_perm && S_TYPE(perm) == T_DIR) ? 2 : 1;
			s->xfer_total = size;
			finish(PSI_XFER_OK);
			return;
		}
		/* GET: a folder can't be fetched; otherwise open it */
		if (has_perm && S_TYPE(perm) == T_DIR) {
			set_fail(PSI_XFER_FAILED, "that is a folder");
			finish(failed);
			return;
		}
		total = has_size ? size : 0;
		s->xfer_total = total;
		if (send_open(s->xfer_remote, FXF_READ) < 0) finish(PSI_XFER_FAILED);
		return;
	}

	if (type == FXP_DATA && qc.kind == K_READ) {
		unsigned int len;
		const unsigned char *d = rdstr(&r, &len);
		if (r.bad || len > qc.len) { set_fail(PSI_XFER_FAILED, "bad reply from the server"); pump(); return; }
		if (!failed && lf) {
			/* (no ftell/fflush per piece: on the Psion each one costs a
			   file server round trip, and slowed a download to a crawl;
			   a write that fails shows here or at the fflush in finish) */
			if (lf_pos != qc.off && fseek(lf, (long)qc.off, SEEK_SET) != 0)
				set_fail(PSI_XFER_LOCAL_WRITE, NULL);
			else if (len && fwrite(d, 1, len, lf) != len)
				set_fail(PSI_XFER_LOCAL_WRITE, NULL);   /* disk full, or the card taken out */
			else {
				lf_pos = qc.off + len;
				done_bytes += len;
				s->xfer_done = done_bytes;
			}
		}
		if (len < qc.len && !failed && len > 0 && gaps < MAX_REQS && !(total && qc.off + len >= total)) {
			/* a short piece: ask again for the rest of it */
			gap_off[gaps] = qc.off + len;
			gap_len[gaps] = qc.len - len;
			gaps++;
		} else if (len == 0)
			eof_seen = 1;
		pump();
		return;
	}

	if (type == FXP_NAME && qc.kind == K_REALPATH) {
		unsigned int cnt = rd32(&r), len, ll;
		const unsigned char *name = rdstr(&r, &len);
		if (r.bad || cnt < 1 || len == 0 || len >= sizeof(s->xfer_path)) { set_fail(PSI_XFER_FAILED, "bad reply from the server"); finish(failed); return; }
		rdstr(&r, &ll);
		memcpy(s->xfer_path, name, len);
		s->xfer_path[len] = 0;
		if (send_path(FXP_OPENDIR, K_OPENDIR, s->xfer_path) < 0) finish(PSI_XFER_FAILED);
		return;
	}

	if (type == FXP_NAME && qc.kind == K_READDIR) {
		unsigned int cnt = rd32(&r), k;
		for (k = 0; k < cnt && !r.bad; k++) {
			unsigned int nlen, llen;
			int has_size, has_perm;
			unsigned long size;
			unsigned int perm;
			const unsigned char *name = rdstr(&r, &nlen);
			rdstr(&r, &llen);                 /* the "ls -l" line: not used */
			rdattrs(&r, &has_size, &size, &perm, &has_perm);
			if (!r.bad) list_add(name, nlen, has_size, size, perm, has_perm);
		}
		if (s->xfer_cancel) { finish(PSI_XFER_CANCELLED); return; }
		if (send_handle(FXP_READDIR, K_READDIR) < 0) finish(PSI_XFER_FAILED);
		return;
	}

	set_fail(PSI_XFER_FAILED, "unexpected reply from the server");
	finish(failed);
}

/* ================================================================ psishim hooks */

/* Dropbear writes what the server sent on our channel */
int psi_sftp_write(const void *buf, int len)
{
	const unsigned char *b = (const unsigned char*)buf;
	int left = len;
	while (left > 0) {
		unsigned int take, plen;
		if (inlen < 4) {
			take = 4 - inlen;
		} else {
			plen = get32(inbuf);
			if (plen > IN_MAX - 4 || plen == 0) {
				/* more than we can take in: give up on this channel */
				inlen = 0;
				if (op) { set_fail(PSI_XFER_FAILED, "reply too large"); finish(failed); }
				close_channel();
				return len;
			}
			take = plen + 4 - inlen;
		}
		if (take > (unsigned int)left) take = left;
		memcpy(inbuf + inlen, b, take);
		inlen += take;
		b += take;
		left -= take;
		if (inlen >= 4) {
			plen = get32(inbuf);
			if (plen <= IN_MAX - 4 && inlen == plen + 4) {
				inlen = 0;
				handle_reply(inbuf + 4, plen);
			}
		}
	}
	return len;
}

/* Dropbear asks for data to send on our channel */
int psi_sftp_read(void *buf, int len)
{
	int n = (int)(outlen - outpos);
	if (n <= 0) {
		errno = EINTR;                /* nothing now (Dropbear tries again later) */
		return -1;
	}
	if (n > len) n = len;
	trace("send", n);
	memcpy(buf, outbuf + outpos, n);
	outpos += n;
	if (outpos == outlen) outpos = outlen = 0;
	return n;
}

/* is there something to send? (select()) */
int psi_sftp_pending(void)
{
	if (!ch || ch_state == CH_CLOSING) return 0;
	pump();
	return outlen > outpos;
}

/* logged in: the select() wait is kept short so a new request is seen soon */
int psi_sftp_running(void) { return running; }

/* every turn of Dropbear's main loop, once logged in */
void psi_sftp_loop(void)
{
	PsiShared *s = sh();
	running = 1;
	if (!s) return;
	if (!op && s->xfer_req != s->xfer_ack) {
		start_op();
		if (op && ch_state == CH_READY)
			first_request();
	}
	if (!op) return;
	if (s->xfer_cancel && !(op == PSI_XOP_PUT || op == PSI_XOP_GET) ) {
		finish(PSI_XFER_CANCELLED);
		return;
	}
	if (s->xfer_cancel && (!have_handle || ch_state != CH_READY)
		&& !(op == PSI_XOP_PUT && ch_state == CH_READY && count_reqs(K_OPEN))) {
		/* (an upload's OPEN still on its way has made the file: wait for
		   the handle, then pump() closes and removes it) */
		finish(PSI_XFER_CANCELLED);
		return;
	}
	if (ch_state == CH_NONE) {
		open_channel();
		return;
	}
	if ((busy_reqs() > 0 || ch_state != CH_READY) && time(NULL) - last_heard > REPLY_SECS) {
		/* no answer for a minute: the link or the server is stuck */
		finish(ch_state == CH_READY ? PSI_XFER_TIMEOUT : PSI_XFER_NO_SFTP);
		close_channel();
		return;
	}
	pump();
}

/* psissh is ending (the connection went): leave no half-written file */
void psi_sftp_session_ended(void)
{
	ch = NULL;                        /* nothing more can be sent */
	ch_state = CH_NONE;
	if (op) finish(PSI_XFER_LINK);
}
