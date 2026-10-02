/* tmuxq.c - asks tmux, on the server, what its windows are.
 *
 * PsiTerm used to work out tmux's windows by reading its status line off the
 * screen, which breaks on themes, long or odd window names, a hidden bar or
 * a switch half drawn. This asks tmux itself instead: a command run on a
 * channel of its own ("exec", on the SSH connection that is already logged
 * in), whose output is a plain list. PsiTerm posts one request at a time in
 * the shared memory (psishared.h, tq_*); this file runs it and reports.
 *
 * The command is built here from fixed text; the only parts that come from
 * PsiTerm are a session id and a window index, both checked. It is run by
 * the user's own shell as sh -c "...", the one form every shell (bash, zsh,
 * fish, csh) reads the same way, with PATH widened for the places tmux
 * lives that a login-less shell lacks (Homebrew, MacPorts, /usr/local).
 *
 * Plumbing as for sftp.c: the channel's local end is the pretend file
 * descriptor PSI_FD_TQ; psishim.c hands what the server sends to
 * psi_tq_write(). The channel is opened for each request and closes when
 * the command has ended, so it never holds the session open.
 */

#include "includes.h"
#include "packet.h"
#include "buffer.h"
#include "session.h"
#include "dbutil.h"
#include "channel.h"
#include "ssh.h"
#include "psishared.h"

#define PSI_FD_TQ 54
#define TQ_SECS   25            /* no answer for this long: give up */

extern PsiShared* pg_shared(void);

enum { TQ_NONE, TQ_OPENING, TQ_EXEC, TQ_RUNNING, TQ_CLOSING };
static int ch_state = TQ_NONE;
static struct Channel *ch;
static int close_when_open;
static int running;                   /* logged in: requests can be served */
static int op;                        /* PSI_TQ_*, 0 = idle */
static unsigned int serving;
static time_t started;
static char out[PSI_TQ_OUT_SIZE];
static unsigned int outlen;
static int truncated;

static PsiShared *sh(void) { return pg_shared(); }

/* the shared text: "C" lines are the clients (when each was last active and
   its session), "W" lines the windows of every session (the name last, as
   it may hold anything), then tmux's status settings. A tab is a real tab. */
#define TQ_PATH  "PATH=\\\"\\$PATH:/usr/local/bin:/opt/homebrew/bin:/opt/local/bin\\\"; "
#define TQ_LIST \
	"tmux list-clients -F 'C\t#{client_activity}\t#{session_id}' 2>/dev/null; " \
	"tmux list-windows -a -F 'W\t#{session_id}\t#{window_index}\t#{window_active}\t#{window_name}' 2>/dev/null; " \
	"tmux show-options -g status 2>/dev/null; " \
	"tmux show-options -g status-position 2>/dev/null"

static int valid_sid(const char *p)
{
	int n = 0;
	if (*p != '$') return 0;
	for (p++; *p; p++, n++)
		if (*p < '0' || *p > '9') return 0;
	return n > 0 && n < 8;
}

/* the whole command line for the request, or 0 if its arguments are bad */
static int build_command(char *cmd, int max)
{
	PsiShared *s = sh();
	int n;
	if (op == PSI_TQ_SELECT) {
		char sid[16];
		memcpy(sid, s->tq_sid, sizeof(sid));
		sid[sizeof(sid) - 1] = 0;
		if (!valid_sid(sid) || s->tq_idx < 0 || s->tq_idx > 9999)
			return 0;
		/* ($ is escaped for the user's shell, which reads the double quotes) */
		n = snprintf(cmd, max, "sh -c \"" TQ_PATH "tmux select-window -t '\\$%s:%d' 2>/dev/null; " TQ_LIST "\"",
			sid + 1, s->tq_idx);
	} else if (op == PSI_TQ_LIST) {
		n = snprintf(cmd, max, "sh -c \"" TQ_PATH TQ_LIST "\"");
	} else {
		return 0;
	}
	return n > 0 && n < max;
}

/* ================================================================ finishing */

static void finish(int result)
{
	PsiShared *s = sh();
	if (!op) return;
	if (s) {
		if (result == PSI_TQ_OK) {
			/* (a list cut short ends at a whole line) */
			unsigned int n = outlen;
			if (truncated)
				while (n > 0 && out[n - 1] != '\n') n--;
			memcpy(s->tq_out, out, n);
			s->tq_len = (int)n;
		} else {
			s->tq_len = 0;
		}
		s->tq_result = result;
		s->tq_ack = serving;          /* last: PsiTerm now reads the result */
	}
	op = 0;
}

/* ================================================================ the channel */

static int tq_chan_init(struct Channel *c);
static void tq_chan_cleanup(const struct Channel *c);

static const struct ChanType clitq = {
	"session",
	tq_chan_init,         /* the server said yes to the channel */
	NULL,                 /* check_close */
	NULL,                 /* reqhandler (exit-status: the default answer) */
	NULL,                 /* closehandler */
	tq_chan_cleanup       /* the channel has gone */
};

static void open_channel(void)
{
	if (send_msg_channel_open_init(PSI_FD_TQ, &clitq) == DROPBEAR_FAILURE) {
		finish(PSI_TQ_FAILED);
		return;
	}
	encrypt_packet();
	ch_state = TQ_OPENING;
	ch = NULL;
	close_when_open = 0;
	outlen = 0;
	truncated = 0;
	started = time(NULL);
}

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
	ch->readfd = -1;
	ch->writefd = -1;
	ch_state = TQ_CLOSING;
}

static int tq_chan_init(struct Channel *c)
{
	char cmd[1100];
	ch = c;
	c->errfd = -1;                    /* stderr: not wanted */
	c->extrabuf = NULL;
	if (close_when_open || !op || !build_command(cmd, sizeof(cmd))) {
		close_channel();
		if (op) finish(PSI_TQ_FAILED);
		return 0;
	}
	start_send_channel_request(c, "exec");
	buf_putbyte(ses.writepayload, 1);                 /* with a reply */
	buf_putstring(ses.writepayload, cmd, strlen(cmd));
	encrypt_packet();
	ch_state = TQ_EXEC;
	started = time(NULL);
	return 0;
}

static void tq_chan_cleanup(const struct Channel *c)
{
	int was = ch_state;
	(void)c;
	ch = NULL;
	ch_state = TQ_NONE;
	if (op && was != TQ_CLOSING) {
		/* the server closed the channel: the command has ended. Before it
		   was running, it was refused. */
		if (was == TQ_RUNNING)
			finish(PSI_TQ_OK);
		else
			finish(PSI_TQ_NO_EXEC);
	}
}

/* SSH_MSG_CHANNEL_SUCCESS / FAILURE (cli-session.c): the answer to "exec" */
void psi_tq_chanreply(int success, unsigned int chan)
{
	if (!ch || ch->index != chan || ch_state != TQ_EXEC)
		return;
	if (!success) {
		close_channel();
		finish(PSI_TQ_NO_EXEC);
		return;
	}
	ch_state = TQ_RUNNING;
}

/* the shell's channel is closing: ours must not keep the session alive */
void psi_tq_shell_closed(void)
{
	if (op) finish(PSI_TQ_LINK);
	close_channel();
}

/* ================================================================ psishim hooks */

/* Dropbear writes what the command printed */
int psi_tq_write(const void *buf, int len)
{
	unsigned int room = sizeof(out) - outlen;
	unsigned int take = (unsigned int)len;
	if (take > room) { take = room; truncated = 1; }
	memcpy(out + outlen, buf, take);
	outlen += take;
	return len;
}

/* Dropbear asks for data to send: none (the command reads nothing) */
int psi_tq_read(void *buf, int len)
{
	(void)buf; (void)len;
	errno = EINTR;
	return -1;
}

int psi_tq_running(void) { return running; }

/* every turn of Dropbear's main loop, once logged in */
void psi_tq_loop(void)
{
	PsiShared *s = sh();
	running = 1;
	if (!s) return;
	if (!op) {
		if (s->tq_req == s->tq_ack || ch_state == TQ_CLOSING)
			return;                   /* nothing asked (or the last channel is still going) */
		op = s->tq_op;
		serving = s->tq_req;
		if (op != PSI_TQ_LIST && op != PSI_TQ_SELECT) {
			finish(PSI_TQ_FAILED);
			return;
		}
	}
	if (ch_state == TQ_NONE) {
		open_channel();
		return;
	}
	if (time(NULL) - started > TQ_SECS) {
		finish(PSI_TQ_TIMEOUT);
		close_channel();
	}
}

/* psissh is ending (the connection went) */
void psi_tq_session_ended(void)
{
	ch = NULL;
	ch_state = TQ_NONE;
	if (op) finish(PSI_TQ_LINK);
}
