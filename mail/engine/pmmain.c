/* pmmain.c - psimail.exe: takes commands from PsiMail.app and does them
 *
 * One command at a time, in order. Each one connects if it needs to (and
 * the app is not working offline), talks to the server, updates the files
 * in the store and reports back in the shared chunk (psimail.h). Changes
 * to messages - read, flagged, moved, deleted - are made in the local
 * files at once and queued (pending.txt) if the server can't be reached.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "pm.h"

/* ---------------------------------------------------------------- helpers */

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

void pm_progress(const char *fmt, ...)
{
	PmShared *s = pm_shared();
	char b[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	pm_copy(s->progress, b, sizeof(s->progress));
}

int pm_cancelled(void)
{
	PmShared *s = pm_shared();
	return s->net.quit || s->quitting;
}

/* ------------------------------------------------------------ local edits */

/* applies a flag change or removal to index.txt and search.txt */
static void local_update(int acct, const char *folder, unsigned int uid, const char *op, int remove_it)
{
	static PmIndex ix;
	static const char *files[2] = { "index.txt", "search.txt" };
	int i;
	for (i = 0; i < 2; i++) {
		PmMsg *m;
		if (st_index_load(acct, folder, files[i], &ix) != 0) continue;
		if ((m = st_index_find(&ix, uid)) != 0) {
			if (remove_it) st_index_remove(&ix, uid);
			else st_flag_set(m->flags, op[1], op[0] == '+');
			st_index_save(acct, folder, files[i], &ix);
		}
		st_index_free(&ix);
	}
	if (remove_it) {
		char p[190];
		st_msg_path(acct, folder, uid, "txt", p, sizeof(p)); remove(p);
		st_msg_path(acct, folder, uid, "att", p, sizeof(p)); remove(p);
		st_msg_path(acct, folder, uid, "htm", p, sizeof(p)); remove(p);
	}
}

static int queued(int r)
{
	return r == PM_RES_OFFLINE || r == PM_RES_NEED_PASS || r == PM_RES_UNTRUSTED ||
	       r == PM_RES_LOGIN_FAILED || r == PM_RES_CANCELLED;
}

/* ----------------------------------------------------------------- outbox */

typedef struct { char names[40][24]; int n; } NameList;

static void add_name(const char *name, void *ctx)
{
	NameList *l = (NameList *)ctx;
	if (l->n < 40 && strlen(name) < sizeof(l->names[0])) strcpy(l->names[l->n++], name);
}

static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static int send_outbox(int acct, char *why, int whymax, int *nsent)
{
	static NameList l;
	PmAccount *a = &pm_shared()->acct[acct];
	char dir[160], obox[180], path[200], eml[200], err[200];
	static char rcpts[2000], from[100], line[300];
	int i, r = PM_RES_OK;

	*nsent = 0;
	st_acct_dir(acct, dir, sizeof(dir));
	snprintf(obox, sizeof(obox), "%soutbox%s", dir, PM_SEP);
	memset(&l, 0, sizeof(l));
	pm_list_dir(obox, ".txt", add_name, &l);
	qsort(l.names, l.n, sizeof(l.names[0]), cmp_names);
	for (i = 0; i < l.n; i++) {
		char rfolder[128];
		unsigned int ruid = 0;
		FILE *f;
		snprintf(path, sizeof(path), "%s%s", obox, l.names[i]);
		snprintf(eml, sizeof(eml), "%s%.*s.eml", obox, (int)strlen(l.names[i]) - 4, l.names[i]);
		snprintf(err, sizeof(err), "%s%.*s.err", obox, (int)strlen(l.names[i]) - 4, l.names[i]);
		/* reply bookkeeping from the header */
		rfolder[0] = 0;
		if ((f = fopen(path, "r")) != 0) {
			while (fgets(line, sizeof(line), f)) {
				line[strcspn(line, "\r\n")] = 0;
				if (!line[0]) break;
				if (!pm_strncasecmp(line, "Reply-Folder: ", 14)) pm_copy(rfolder, line + 14, sizeof(rfolder));
				if (!pm_strncasecmp(line, "Reply-Uid: ", 11)) ruid = (unsigned int)strtoul(line + 11, 0, 10);
				if (!pm_strncasecmp(line, "Draft: 1", 8)) { ruid = 0xffffffffu; break; }
			}
			fclose(f);
		}
		if (ruid == 0xffffffffu) continue;             /* a draft: not for sending */
		pm_progress("Preparing message %d of %d...", i + 1, l.n);
		r = compose_mime(acct, path, eml, from, sizeof(from), rcpts, sizeof(rcpts), why, whymax);
		if (r == PM_RES_OK) {
			pm_progress("Sending message %d of %d...", i + 1, l.n);
			r = smtp_send(acct, eml, from, rcpts, why, whymax);
		}
		if (r != PM_RES_OK) {
			if ((f = fopen(err, "w")) != 0) { fprintf(f, "%s\n", why); fclose(f); }
			remove(eml);
			st_changed();
			if (queued(r)) break;
			continue;                                   /* this one is bad; try the rest */
		}
		(*nsent)++;
		if (a->save_sent) {
			char sent[128], w2[100];
			imap_special_folder(acct, 'S', sent, sizeof(sent));
			if (imap_append(acct, sent, eml, "\\Seen", w2, sizeof(w2)) != PM_RES_OK)
				pm_log("could not save to %s: %s", sent, w2);
		}
		if (rfolder[0] && ruid) {
			char w2[100];
			local_update(acct, rfolder, ruid, "+A", 0);
			if (queued(imap_flag(acct, rfolder, ruid, "+A", w2, sizeof(w2)))) {
				snprintf(line, sizeof(line), "FLAG\t%s\t%u\t+A", rfolder, ruid);
				st_pending_add(acct, line);
			}
		}
		remove(path);
		remove(eml);
		remove(err);
		st_changed();
	}
	if (r == PM_RES_OK) {
		if (*nsent) snprintf(why, whymax, "%d sent", *nsent);
		else snprintf(why, whymax, "Nothing to send");
	}
	return r;
}

/* --------------------------------------------------------------- commands */

static int replay(int acct, char *why, int whymax)
{
	char w2[100];
	if (pm_shared()->offline) return 0;
	(void)why; (void)whymax;
	return st_pending_replay(acct, w2, sizeof(w2));
}

static int run(PmCmd *c, char *why, int whymax)
{
	PmShared *s = pm_shared();
	int r = PM_RES_OK, a = c->acct, n;
	char line[400];

	why[0] = 0;
	if (c->op == PM_CMD_HANGUP) { pmn_release_now(); snprintf(why, whymax, "Hung up"); return PM_RES_OK; }
	if (c->op == PM_CMD_TRUST) {
		if (!s->trust_fp[0]) { snprintf(why, whymax, "Nothing to trust"); return PM_RES_FAILED; }
		st_pin_save(c->arg, s->trust_fp);
		snprintf(why, whymax, "Trusted %s", c->arg);
		s->trust_fp[0] = 0;
		return PM_RES_OK;
	}
	if (a < 0 || a >= PM_MAX_ACCOUNTS || !s->acct[a].used) { snprintf(why, whymax, "No such account"); return PM_RES_FAILED; }
	st_check_account(a);
	switch (c->op) {
	case PM_CMD_FOLDERS:
		replay(a, why, whymax);
		r = imap_list_folders(a, why, whymax);
		break;
	case PM_CMD_SYNC:
		replay(a, why, whymax);
		r = imap_sync(a, c->folder, 0, why, whymax);
		break;
	case PM_CMD_OLDER:
		r = imap_sync(a, c->folder, 1, why, whymax);
		break;
	case PM_CMD_BODY:
	case PM_CMD_FULLBODY:
		r = imap_body(a, c->folder, c->uid, c->op == PM_CMD_FULLBODY, why, whymax);
		break;
	case PM_CMD_ATTACH:
		r = imap_attach(a, c->folder, c->uid, c->arg, why, whymax);
		break;
	case PM_CMD_FLAG:
		local_update(a, c->folder, c->uid, c->arg, 0);
		r = s->offline ? PM_RES_OFFLINE : imap_flag(a, c->folder, c->uid, c->arg, why, whymax);
		if (queued(r)) {
			snprintf(line, sizeof(line), "FLAG\t%s\t%u\t%s", c->folder, c->uid, c->arg);
			st_pending_add(a, line);
			snprintf(why, whymax, "Changed here; the server will be told next time");
			r = PM_RES_OFFLINE;
		}
		break;
	case PM_CMD_MOVE:
		local_update(a, c->folder, c->uid, "", 1);
		r = s->offline ? PM_RES_OFFLINE : imap_move(a, c->folder, c->uid, c->arg, why, whymax);
		if (queued(r)) {
			snprintf(line, sizeof(line), "MOVE\t%s\t%u\t%s", c->folder, c->uid, c->arg);
			st_pending_add(a, line);
			snprintf(why, whymax, "Moved here; the server will be told next time");
			r = PM_RES_OFFLINE;
		}
		break;
	case PM_CMD_EXPUNGE:
		r = imap_expunge(a, c->folder, why, whymax);
		break;
	case PM_CMD_SEARCH:
		r = imap_search(a, c->folder, c->arg, why, whymax);
		break;
	case PM_CMD_SEND:
		r = send_outbox(a, why, whymax, &n);
		break;
	case PM_CMD_SENDRECV:
	{
		char w2[160];
		int got = 0;
		r = send_outbox(a, why, whymax, &n);
		if (queued(r)) break;
		w2[0] = 0;
		replay(a, why, whymax);
		r = imap_list_folders(a, why, whymax);
		if (r == PM_RES_OK) r = imap_sync(a, "INBOX", 0, why, whymax);
		if (r == PM_RES_OK) got = s->new_mail;
		if (r == PM_RES_OK && c->folder[0] && strcmp(c->folder, "INBOX"))
			r = imap_sync(a, c->folder, 0, w2, sizeof(w2));
		if (r == PM_RES_OK) {
			if (n) snprintf(why, whymax, "%d sent, %d new", n, got);
			else if (got) snprintf(why, whymax, "%d new in the Inbox", got);
			else snprintf(why, whymax, "No new mail");
		} else if (w2[0]) pm_copy(why, w2, whymax);
		break;
	}
	default:
		snprintf(why, whymax, "Unknown command %d", c->op);
		r = PM_RES_FAILED;
	}
	return r;
}

void pm_do_command(PmCmd *c)
{
	PmShared *s = pm_shared();
	static char why[160];
	int r;
	s->cur_op = c->op;
	s->busy = 1;
	s->progress[0] = 0;
	r = run(c, why, sizeof(why));
	if (pm_cancelled() && r != PM_RES_OK) { r = PM_RES_CANCELLED; if (!why[0]) pm_copy(why, "Stopped", sizeof(why)); }
	pm_log("cmd %d acct %d %s uid %u -> %d %s", c->op, c->acct, c->folder, c->uid, r, why);
	s->last_op = c->op;
	s->last_acct = c->acct;
	s->last_uid = c->uid;
	pm_copy(s->last_folder, c->folder, sizeof(s->last_folder));
	pm_copy(s->last_msg, why, sizeof(s->last_msg));
	s->last_res = r;
	s->progress[0] = 0;
	s->net.quit = 0;
	s->busy = 0;
	s->done_seq++;
}

/* The engine's loop: returns when the app says quit (or has gone away). */
void pm_loop(int (*housekeeping)(void))
{
	PmShared *s = pm_shared();
	s->state = PM_STATE_READY;
	while (!s->quitting) {
		if (s->cmd_tail != s->cmd_head) {
			PmCmd c = s->cmd[s->cmd_tail % PM_CMDQ];
			s->cmd_tail++;
			if (c.op == PM_CMD_QUIT) break;
			pm_do_command(&c);
			continue;
		}
		if (housekeeping && housekeeping()) break;
		pm_idle(100);
	}
	pmn_release_now();
	s->state = PM_STATE_EXITED;
}
