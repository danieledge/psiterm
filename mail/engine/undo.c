/* undo.c - Edit > Undo: puts back a message that was deleted (moved to
 * the Trash), moved to a folder or archived
 *
 * A move takes the message out of the folder's local files at once (see
 * local_update in pmmain.c). For Undo, the moves remembered keep what the
 * message had here before it went:
 *
 *   <store>\A<n>\undo.txt        the last UNDO_MAX moves, oldest first:
 *       id TAB state TAB uid TAB destuid TAB folder TAB dest TAB arg TAB
 *       flags TAB date TAB size TAB from TAB subject TAB to TAB message-id TAB in-reply-to
 *   <store>\A<n>\undo\<id>.txt   its downloaded text (and .att, .htm, .pic,
 *                                <id>_<part>.pmi / .img: its pictures)
 *
 * state: UNDO_QUEUED - the move waits in pending.txt (offline), so undoing
 * it takes the line out again; UNDO_MOVED - the server has it in dest
 * (destuid from COPYUID, or 0 = found again by its Message-ID);
 * UNDO_FAILED - the server refused the move, so it is still in folder
 * there. arg is what the app asked for ("" = the Trash), as pending.txt has it.
 *
 * Undoing a move the server has made moves the message back (imap_unmove)
 * and the folder's index gets the uid it has there now, so the next check
 * does not take it for new mail. Offline, it goes back in the local files
 * at once and an UNMOVE line in pending.txt tells the server next time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm.h"

#define UNDO_MAX 5
enum { UNDO_QUEUED = 0, UNDO_MOVED = 1, UNDO_FAILED = 2 };

typedef struct
	{
	unsigned int id;
	int state;
	unsigned int uid, destuid;
	char folder[128];
	char dest[128];          /* the IMAP name it went to (the Trash's resolved) */
	char arg[128];           /* as the command had it */
	PmMsg m;                 /* its line in index.txt */
	} UndoRec;

static UndoRec g_rec[UNDO_MAX + 1];
static int g_nrec;
static int g_last = -1;      /* the record undo_note_move made for the move running now */

static void acct_file(int acct, const char *name, char *out, int max)
{
	char d[160];
	st_acct_dir(acct, d, sizeof(d));
	snprintf(out, max, "%s%s", d, name);
}

static void stash_dir(int acct, char *out, int max)
{
	char d[160];
	st_acct_dir(acct, d, sizeof(d));
	snprintf(out, max, "%sundo%s", d, PM_SEP);
}

static char *fld(char **p)
{
	char *s = *p, *t;
	if (!s) return "";
	t = strchr(s, '\t');
	if (t) { *t = 0; *p = t + 1; } else *p = 0;
	return s;
}

static void clean(char *s)
{
	for (; *s; s++) if (*s == '\t' || *s == '\r' || *s == '\n') *s = ' ';
}

static void load(int acct)
{
	static char line[1100];
	char path[190];
	FILE *f;
	g_nrec = 0;
	acct_file(acct, "undo.txt", path, sizeof(path));
	if (!(f = fopen(path, "r"))) return;
	while (g_nrec < UNDO_MAX && fgets(line, sizeof(line), f)) {
		UndoRec *u = &g_rec[g_nrec];
		char *p = line;
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0]) continue;
		memset(u, 0, sizeof(*u));
		u->id = (unsigned int)strtoul(fld(&p), 0, 10);
		u->state = atoi(fld(&p));
		u->uid = (unsigned int)strtoul(fld(&p), 0, 10);
		u->destuid = (unsigned int)strtoul(fld(&p), 0, 10);
		pm_copy(u->folder, fld(&p), sizeof(u->folder));
		pm_copy(u->dest, fld(&p), sizeof(u->dest));
		pm_copy(u->arg, fld(&p), sizeof(u->arg));
		pm_copy(u->m.flags, fld(&p), sizeof(u->m.flags));
		u->m.date = strtol(fld(&p), 0, 10);
		u->m.size = strtol(fld(&p), 0, 10);
		pm_copy(u->m.from, fld(&p), sizeof(u->m.from));
		pm_copy(u->m.subject, fld(&p), sizeof(u->m.subject));
		pm_copy(u->m.to, fld(&p), sizeof(u->m.to));
		pm_copy(u->m.msgid, fld(&p), sizeof(u->m.msgid));
		pm_copy(u->m.inreplyto, fld(&p), sizeof(u->m.inreplyto));
		u->m.uid = u->uid;
		u->m.attach = st_flag_has(u->m.flags, 'T');
		if (u->id && u->folder[0]) g_nrec++;
	}
	fclose(f);
}

static int save(int acct)
{
	char path[190], tmp[190];
	FILE *f;
	int i;
	acct_file(acct, "undo.txt", path, sizeof(path));
	acct_file(acct, "undo.new", tmp, sizeof(tmp));
	if (!g_nrec) { remove(path); return 0; }
	if (!(f = fopen(tmp, "w"))) return -1;
	for (i = 0; i < g_nrec; i++) {
		UndoRec *u = &g_rec[i];
		clean(u->m.from); clean(u->m.subject); clean(u->m.to); clean(u->m.msgid); clean(u->m.inreplyto);
		fprintf(f, "%u\t%d\t%u\t%u\t%s\t%s\t%s\t%s\t%ld\t%ld\t%s\t%s\t%s\t%s\t%s\n",
			u->id, u->state, u->uid, u->destuid, u->folder, u->dest, u->arg,
			u->m.flags, u->m.date, u->m.size, u->m.from, u->m.subject, u->m.to, u->m.msgid, u->m.inreplyto);
	}
	if (pm_fclose(f) != 0 || pm_replace(tmp, path) != 0) { remove(tmp); return -1; }
	return 0;
}

static int exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	fclose(f);
	return 1;
}

static void move_file(const char *from, const char *to)
{
	if (!exists(from)) return;
	remove(to);
	if (pm_replace(from, to) != 0) remove(from);
}

/* a message's picture files ("<uid>_<part>.pmi" / ".img") from one folder to another, renamed */
typedef struct { char from[160]; char to[160]; char oldp[24]; char newp[24]; } PicMove;

static void pic_cb(const char *name, void *ctx)
{
	PicMove *pm = (PicMove *)ctx;
	char a[220], b[220];
	int n = (int)strlen(pm->oldp);
	if (strncmp(name, pm->oldp, n)) return;
	snprintf(a, sizeof(a), "%s%s", pm->from, name);
	snprintf(b, sizeof(b), "%s%s%s", pm->to, pm->newp, name + n);
	move_file(a, b);
}

static void rm_cb(const char *name, void *ctx)
{
	PicMove *pm = (PicMove *)ctx;
	char a[220];
	if (strncmp(name, pm->oldp, strlen(pm->oldp))) return;
	snprintf(a, sizeof(a), "%s%s", pm->from, name);
	remove(a);
}

static void move_pictures(const char *fromdir, unsigned int fromid, const char *todir, unsigned int toid)
{
	static PicMove pm;
	pm_copy(pm.from, fromdir, sizeof(pm.from));
	pm_copy(pm.to, todir, sizeof(pm.to));
	snprintf(pm.oldp, sizeof(pm.oldp), "%u_", fromid);
	snprintf(pm.newp, sizeof(pm.newp), "%u_", toid);
	pm_list_dir(fromdir, ".pmi", pic_cb, &pm);
	pm_list_dir(fromdir, ".img", pic_cb, &pm);
}

static const char *k_ext[4] = { "txt", "att", "htm", "pic" };

/* the message's files: folder <-> the undo folder */
static void stash(int acct, const char *folder, unsigned int uid, unsigned int id, int back)
{
	char sd[160], fd[160], a[200], b[200];
	int i;
	stash_dir(acct, sd, sizeof(sd));
	st_folder_dir(acct, folder, fd, sizeof(fd));
	pm_mkdir(back ? fd : sd);
	for (i = 0; i < 4; i++) {
		st_msg_path(acct, folder, uid, k_ext[i], a, sizeof(a));
		snprintf(b, sizeof(b), "%s%u.%s", sd, id, k_ext[i]);
		if (back) move_file(b, a); else move_file(a, b);
	}
	if (back) move_pictures(sd, id, fd, uid);
	else move_pictures(fd, uid, sd, id);
}

static void forget(int acct, int i)
{
	char sd[160], p[200];
	int k;
	stash_dir(acct, sd, sizeof(sd));
	for (k = 0; k < 4; k++) {
		snprintf(p, sizeof(p), "%s%u.%s", sd, g_rec[i].id, k_ext[k]);
		remove(p);
	}
	{
		/* its pictures */
		static PicMove pm;
		pm_copy(pm.from, sd, sizeof(pm.from));
		snprintf(pm.oldp, sizeof(pm.oldp), "%u_", g_rec[i].id);
		pm_list_dir(sd, ".pmi", rm_cb, &pm);
		pm_list_dir(sd, ".img", rm_cb, &pm);
	}
	memmove(g_rec + i, g_rec + i + 1, (g_nrec - i - 1) * sizeof(UndoRec));
	g_nrec--;
}

/* ------------------------------------------------------------ the move */

void undo_note_move(int acct, const char *folder, unsigned int uid, const char *arg)
{
	static PmIndex ix;
	char dest[128];
	PmMsg *m;
	UndoRec *u;
	unsigned int id = 1;
	int i;
	g_last = -1;
	pm_copy(dest, arg, sizeof(dest));
	if (!dest[0]) imap_special_folder(acct, 'T', dest, sizeof(dest));
	if (!strcmp(dest, folder)) return;              /* deleted from the Trash: gone for good */
	if (st_index_load(acct, folder, "index.txt", &ix) != 0) return;
	m = st_index_find(&ix, uid);
	if (!m) { st_index_free(&ix); return; }
	load(acct);
	for (i = 0; i < g_nrec; i++) if (g_rec[i].id >= id) id = g_rec[i].id + 1;
	while (g_nrec >= UNDO_MAX) forget(acct, 0);
	u = &g_rec[g_nrec];
	memset(u, 0, sizeof(*u));
	u->id = id;
	u->state = UNDO_QUEUED;
	u->uid = uid;
	pm_copy(u->folder, folder, sizeof(u->folder));
	pm_copy(u->dest, dest, sizeof(u->dest));
	pm_copy(u->arg, arg, sizeof(u->arg));
	u->m = *m;
	st_index_free(&ix);
	stash(acct, folder, uid, id, 0);
	g_nrec++;
	if (save(acct) == 0) g_last = g_nrec - 1;
	else { g_nrec--; stash(acct, folder, uid, id, 1); }
}

void undo_note_result(int acct, int r, unsigned int destuid)
{
	if (g_last < 0 || g_last >= g_nrec) return;
	if (r == PM_RES_OK) { g_rec[g_last].state = UNDO_MOVED; g_rec[g_last].destuid = destuid; }
	else if (r == PM_RES_OFFLINE) g_rec[g_last].state = UNDO_QUEUED;
	else g_rec[g_last].state = UNDO_FAILED;
	save(acct);
	g_last = -1;
}

/* ------------------------------------------------------------ undoing */

/* takes the last "MOVE TAB folder TAB uid TAB arg" out of pending.txt: 1 if it was there */
static int unpend(int acct, const UndoRec *u)
{
	static char line[512];
	char path[190], tmp[190], want[400];
	FILE *f, *out;
	long n = 0, hit = -1, k = 0;
	acct_file(acct, "pending.txt", path, sizeof(path));
	acct_file(acct, "pending.und", tmp, sizeof(tmp));
	snprintf(want, sizeof(want), "MOVE\t%s\t%u\t%s", u->folder, u->uid, u->arg);
	if (!(f = fopen(path, "r"))) return 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strcmp(line, want)) hit = n;
		n++;
	}
	if (hit < 0) { fclose(f); return 0; }
	rewind(f);
	if (!(out = fopen(tmp, "w"))) { fclose(f); return 0; }
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (k++ != hit) fprintf(out, "%s\n", line);
	}
	fclose(f);
	if (pm_fclose(out) != 0 || pm_replace(tmp, path) != 0) { remove(tmp); return 0; }
	return 1;
}

/* the message back in folder's local files, as uid */
static void restore_local(int acct, const UndoRec *u, unsigned int uid)
{
	static PmIndex ix;
	PmMsg *m;
	st_index_load(acct, u->folder, "index.txt", &ix);
	if ((m = st_index_add(&ix, uid)) != 0) {
		*m = u->m;
		m->uid = uid;
		if (uid >= ix.uidnext) ix.uidnext = uid + 1;
		ix.exists++;
	}
	st_index_save(acct, u->folder, "index.txt", &ix);
	st_index_free(&ix);
	stash(acct, u->folder, uid, u->id, 1);
}

/* the copy in dest (if a check of dest has listed it) out of dest's files */
static void drop_dest(int acct, const char *dest, unsigned int uid)
{
	static PmIndex ix;
	char p[200];
	int i;
	if (!uid || st_index_load(acct, dest, "index.txt", &ix) != 0) return;
	if (st_index_find(&ix, uid)) {
		st_index_remove(&ix, uid);
		st_index_save(acct, dest, "index.txt", &ix);
		for (i = 0; i < 3; i++) { st_msg_path(acct, dest, uid, k_ext[i], p, sizeof(p)); remove(p); }
		pic_remove(acct, dest, uid);
	}
	st_index_free(&ix);
}

static int queued_res(int r)
{
	return r == PM_RES_OFFLINE || r == PM_RES_NEED_PASS || r == PM_RES_UNTRUSTED ||
	       r == PM_RES_LOGIN_FAILED || r == PM_RES_CANCELLED;
}

/* PM_CMD_UNDO: folder, uid = where the message was. The uid it has there
   now goes in last_file ("" if it could not be put back) */
int undo_run(int acct, const char *folder, unsigned int uid, char *why, int whymax)
{
	PmShared *s = pm_shared();
	UndoRec u;
	unsigned int newuid = 0;
	int i, r = PM_RES_OK;
	s->last_file[0] = 0;
	load(acct);
	for (i = g_nrec - 1; i >= 0; i--)
		if (g_rec[i].uid == uid && !strcmp(g_rec[i].folder, folder)) break;
	if (i < 0) { snprintf(why, whymax, "Nothing to undo"); return PM_RES_FAILED; }
	u = g_rec[i];
	if (u.state == UNDO_QUEUED && !unpend(acct, &u)) u.state = UNDO_MOVED;   /* replayed since */
	if (u.state == UNDO_QUEUED || u.state == UNDO_FAILED) {
		/* the server never moved it: it is still in folder as uid */
		newuid = uid;
	} else {
		char line[400];
		r = s->offline ? PM_RES_OFFLINE
		    : imap_unmove(acct, u.dest, u.destuid, u.m.msgid, u.folder, &newuid, why, whymax);
		if (r == PM_RES_OK) drop_dest(acct, u.dest, u.destuid);
		else if (queued_res(r)) {
			/* back here now; the server is told next time */
			snprintf(line, sizeof(line), "UNMOVE\t%s\t%u\t%s\x01%s\x01%u", u.dest, u.destuid, u.folder, u.m.msgid, uid);
			if (st_pending_add(acct, line) != 0) return r;
			drop_dest(acct, u.dest, u.destuid);
			newuid = uid;
			r = PM_RES_OFFLINE;
		} else {
			pm_log("undo: %s %u: %s", u.dest, u.destuid, why);
			load(acct);
			for (i = g_nrec - 1; i >= 0; i--) if (g_rec[i].id == u.id) { forget(acct, i); save(acct); break; }
			return r;
		}
		if (!newuid) newuid = uid;                     /* (the next check puts it right) */
	}
	restore_local(acct, &u, newuid);
	load(acct);
	for (i = g_nrec - 1; i >= 0; i--) if (g_rec[i].id == u.id) { forget(acct, i); save(acct); break; }
	snprintf(s->last_file, sizeof(s->last_file), "%u", newuid);
	if (r == PM_RES_OFFLINE) snprintf(why, whymax, "Put back here; the server will be told next time");
	else snprintf(why, whymax, "Undone");
	st_changed();
	return r;
}

/* pending.txt's UNMOVE: dest, destuid, arg = folder \1 message-id \1 uid here */
int undo_replay(int acct, const char *dest, unsigned int destuid, const char *arg, char *why, int whymax)
{
	char folder[128], msgid[100];
	const char *a = strchr(arg, '\x01'), *b = a ? strchr(a + 1, '\x01') : 0;
	unsigned int here, newuid = 0;
	int r;
	if (!a || !b) return PM_RES_FAILED;
	pm_copy(folder, arg, (int)(a - arg) + 1 < (int)sizeof(folder) ? (int)(a - arg) + 1 : (int)sizeof(folder));
	pm_copy(msgid, a + 1, (int)(b - a) < (int)sizeof(msgid) ? (int)(b - a) : (int)sizeof(msgid));
	here = (unsigned int)strtoul(b + 1, 0, 10);
	r = imap_unmove(acct, dest, destuid, msgid, folder, &newuid, why, whymax);
	if (r == PM_RES_OK && newuid && newuid != here) {
		/* the row put back offline gets the uid the server gave it */
		static PmIndex ix;
		PmMsg *m, keep;
		if (st_index_load(acct, folder, "index.txt", &ix) == 0 && (m = st_index_find(&ix, here)) != 0) {
			char fd[160];
			int i;
			keep = *m;
			st_index_remove(&ix, here);
			if ((m = st_index_add(&ix, newuid)) != 0) { *m = keep; m->uid = newuid; }
			if (newuid >= ix.uidnext) ix.uidnext = newuid + 1;
			st_index_save(acct, folder, "index.txt", &ix);
			st_folder_dir(acct, folder, fd, sizeof(fd));
			for (i = 0; i < 4; i++) {
				char x[200], y[200];
				st_msg_path(acct, folder, here, k_ext[i], x, sizeof(x));
				st_msg_path(acct, folder, newuid, k_ext[i], y, sizeof(y));
				move_file(x, y);
			}
			move_pictures(fd, here, fd, newuid);
		}
		st_index_free(&ix);
	}
	return r;
}
