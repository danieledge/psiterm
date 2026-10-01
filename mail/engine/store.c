/* store.c - PsiMail's files on the Psion (normally on the CF card)
 *
 *   <store> is <disk>:\System\Data\PsiMail\ (app/pmstore.cpp; \PsiMail\ before 0.74)
 *   <store>\pins.txt                 trusted certificates: host:port TAB sha256
 *   <store>\A<n>\account.txt         whose files these are (user@host)
 *   <store>\A<n>\folders.txt         folder list (see imap.c)
 *   <store>\A<n>\pending.txt         changes made offline, replayed later
 *   <store>\A<n>\outbox\<id>.txt     messages written in the app, to send
 *   <store>\A<n>\F<hash>\index.txt   the folder's messages, one per line
 *   <store>\A<n>\F<hash>\search.txt  results of the last search there
 *   <store>\A<n>\F<hash>\<uid>.txt   a downloaded message (text)
 *   <store>\A<n>\F<hash>\<uid>.att   its attachments: part TAB size TAB name TAB type
 *
 * index.txt:  "#PSIMAIL1 TAB uidvalidity TAB uidnext TAB exists", then
 *   uid TAB flags TAB date TAB size TAB from TAB subject TAB to TAB message-id TAB in-reply-to
 * sorted by uid. flags are letters: S seen, F flagged, A answered, D deleted,
 * T has attachments, B text downloaded. date is seconds since 1970 (UTC).
 * All text is Windows-1252, the Psion's character set.
 *
 * Only the engine writes these files (the app writes outbox messages), and
 * always to a temporary file first, renamed into place when complete.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm.h"

static unsigned long fnv(const char *s)
{
	unsigned long h = 2166136261UL;
	while (*s) { h ^= (unsigned char)*s++; h = (h * 16777619UL) & 0xffffffffUL; }
	return h;
}

void st_acct_dir(int acct, char *out, int max)
{
	snprintf(out, max, "%sA%d%s", pm_shared()->store_dir, acct, PM_SEP);
}

void st_folder_dir(int acct, const char *folder, char *out, int max)
{
	snprintf(out, max, "%sA%d%sF%08lX%s", pm_shared()->store_dir, acct, PM_SEP, fnv(folder), PM_SEP);
}

void st_msg_path(int acct, const char *folder, unsigned int uid, const char *ext, char *out, int max)
{
	char d[160];
	st_folder_dir(acct, folder, d, sizeof(d));
	snprintf(out, max, "%s%u.%s", d, uid, ext);
}

void st_changed(void)
{
	pm_shared()->changed_seq++;
}

int st_check_account(int acct)
{
	char dir[160], path[180], want[180], have[180];
	PmAccount *a = &pm_shared()->acct[acct];
	FILE *f;
	st_acct_dir(acct, dir, sizeof(dir));
	snprintf(want, sizeof(want), "%s@%s", a->user, a->imap_host);
	snprintf(path, sizeof(path), "%saccount.txt", dir);
	have[0] = 0;
	if ((f = fopen(path, "r")) != 0) {
		if (!fgets(have, sizeof(have), f)) have[0] = 0;
		fclose(f);
		have[strcspn(have, "\r\n")] = 0;
	}
	if (strcmp(have, want)) {
		if (have[0]) pm_rmtree(dir);           /* another account's mail: start afresh */
		pm_mkdir(pm_shared()->store_dir);
		pm_mkdir(dir);
		if ((f = fopen(path, "w")) != 0) { fprintf(f, "%s\n", want); fclose(f); }
		st_changed();
	}
	snprintf(path, sizeof(path), "%soutbox", dir);
	pm_mkdir(path);
	return 0;
}

/* ----------------------------------------------------------------- index */

void st_index_free(PmIndex *ix)
{
	free(ix->m);
	memset(ix, 0, sizeof(*ix));
}

static char *field(char **p)
{
	char *s = *p, *t;
	if (!s) return "";
	t = strchr(s, '\t');
	if (t) { *t = 0; *p = t + 1; } else *p = 0;
	return s;
}

PmMsg *st_index_find(PmIndex *ix, unsigned int uid)
{
	int lo = 0, hi = ix->n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (ix->m[mid].uid == uid) return &ix->m[mid];
		if (ix->m[mid].uid < uid) lo = mid + 1; else hi = mid - 1;
	}
	return 0;
}

PmMsg *st_index_add(PmIndex *ix, unsigned int uid)
{
	int i;
	PmMsg *m = st_index_find(ix, uid);
	if (m) return m;
	if (ix->n >= ix->cap) {
		int nc = ix->cap ? ix->cap * 2 : 64;
		PmMsg *nm = (PmMsg *)realloc(ix->m, nc * sizeof(PmMsg));
		if (!nm) return 0;
		ix->m = nm; ix->cap = nc;
	}
	for (i = ix->n; i > 0 && ix->m[i - 1].uid > uid; i--)
		ix->m[i] = ix->m[i - 1];
	memset(&ix->m[i], 0, sizeof(PmMsg));
	ix->m[i].uid = uid;
	ix->n++;
	return &ix->m[i];
}

void st_index_remove(PmIndex *ix, unsigned int uid)
{
	PmMsg *m = st_index_find(ix, uid);
	int i;
	if (!m) return;
	i = (int)(m - ix->m);
	memmove(ix->m + i, ix->m + i + 1, (ix->n - i - 1) * sizeof(PmMsg));
	ix->n--;
}

int st_index_load(int acct, const char *folder, const char *file, PmIndex *ix)
{
	char dir[160], path[190];
	static char line[1024];
	FILE *f;
	memset(ix, 0, sizeof(*ix));
	st_folder_dir(acct, folder, dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s%s", dir, file);
	if (!(f = fopen(path, "r"))) return -1;
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *uid;
		PmMsg *m;
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#') {
			field(&p);
			ix->uidvalidity = strtoul(field(&p), 0, 10);
			ix->uidnext = strtoul(field(&p), 0, 10);
			ix->exists = strtol(field(&p), 0, 10);
			continue;
		}
		uid = field(&p);
		if (!*uid) continue;
		m = st_index_add(ix, (unsigned int)strtoul(uid, 0, 10));
		if (!m) break;
		pm_copy(m->flags, field(&p), sizeof(m->flags));
		m->date = strtol(field(&p), 0, 10);
		m->size = strtol(field(&p), 0, 10);
		pm_copy(m->from, field(&p), sizeof(m->from));
		pm_copy(m->subject, field(&p), sizeof(m->subject));
		pm_copy(m->to, field(&p), sizeof(m->to));
		pm_copy(m->msgid, field(&p), sizeof(m->msgid));
		pm_copy(m->inreplyto, field(&p), sizeof(m->inreplyto));
		m->attach = st_flag_has(m->flags, 'T');
	}
	fclose(f);
	return 0;
}

static void clean(char *s)
{
	for (; *s; s++) if (*s == '\t' || *s == '\r' || *s == '\n') *s = ' ';
}

int st_index_save(int acct, const char *folder, const char *file, PmIndex *ix)
{
	char dir[160], path[190], tmp[190];
	FILE *f;
	int i;
	st_folder_dir(acct, folder, dir, sizeof(dir));
	pm_mkdir(dir);
	snprintf(path, sizeof(path), "%s%s", dir, file);
	snprintf(tmp, sizeof(tmp), "%s%s.new", dir, file);
	if (!(f = fopen(tmp, "w"))) return -1;
	fprintf(f, "#PSIMAIL1\t%lu\t%lu\t%ld\n", ix->uidvalidity, ix->uidnext, ix->exists);
	for (i = 0; i < ix->n; i++) {
		PmMsg *m = &ix->m[i];
		st_flag_set(m->flags, 'T', m->attach);
		clean(m->from); clean(m->subject); clean(m->to); clean(m->msgid); clean(m->inreplyto);
		fprintf(f, "%u\t%s\t%ld\t%ld\t%s\t%s\t%s\t%s\t%s\n", m->uid, m->flags, m->date, m->size,
			m->from, m->subject, m->to, m->msgid, m->inreplyto);
	}
	if (pm_fclose(f) != 0 || pm_replace(tmp, path) != 0) {
		pm_log("store: could not save %s", path);
		remove(tmp);                          /* the old list is still there */
		return -1;
	}
	st_changed();
	return 0;
}

void st_flag_set(char *flags, char f, int on)
{
	char *p = strchr(flags, f);
	if (on && !p) {
		int n = (int)strlen(flags);
		if (n < 10) { flags[n] = f; flags[n + 1] = 0; }
	} else if (!on && p) {
		memmove(p, p + 1, strlen(p));
	}
}

int st_flag_has(const char *flags, char f)
{
	return strchr(flags, f) != 0;
}

/* --------------------------------------------------------------- pending */

int st_pending_add(int acct, const char *line)
{
	char dir[160], path[190];
	FILE *f;
	st_acct_dir(acct, dir, sizeof(dir));
	snprintf(path, sizeof(path), "%spending.txt", dir);
	if (!(f = fopen(path, "a"))) { pm_log("store: could not note '%s' in pending.txt", line); return -1; }
	fprintf(f, "%s\n", line);
	if (pm_fclose(f) != 0) { pm_log("store: could not note '%s' in pending.txt (disk full?)", line); return -1; }
	return 0;
}

/* Replays changes made while offline, in order. Lines the server refuses are
   dropped (the message may have gone); a lost connection keeps the rest. */
int st_pending_replay(int acct, char *why, int whymax)
{
	char dir[160], path[190], tmp[190];
	static char line[512];
	FILE *f, *out = 0;
	int lost = 0, n = 0;
	st_acct_dir(acct, dir, sizeof(dir));
	snprintf(path, sizeof(path), "%spending.txt", dir);
	snprintf(tmp, sizeof(tmp), "%spending.new", dir);
	if (!(f = fopen(path, "r"))) return 0;
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *op, *folder, *uid, *arg;
		int r = 0;
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0]) continue;
		if (lost) {
			if (!out) out = fopen(tmp, "w");
			if (out) fprintf(out, "%s\n", line);
			continue;
		}
		op = field(&p); folder = field(&p); uid = field(&p); arg = field(&p);
		pm_progress("Sending changes made offline (%d)...", ++n);
		if (!strcmp(op, "FLAG")) r = imap_flag(acct, folder, (unsigned int)strtoul(uid, 0, 10), arg, why, whymax);
		else if (!strcmp(op, "MOVE")) r = imap_move(acct, folder, (unsigned int)strtoul(uid, 0, 10), arg, why, whymax);
		else if (!strcmp(op, "EXPUNGE")) r = imap_expunge(acct, folder, why, whymax);
		if (r == PM_RES_OFFLINE || r == PM_RES_CANCELLED || !pmn_is_open()) {
			/* keep it (and the rest) for next time */
			lost = 1;
			if (!out) out = fopen(tmp, "w");
			if (out) { fprintf(out, "%s\t%s\t%s\t%s\n", op, folder, uid, arg); }
		}
	}
	fclose(f);
	if (out) {
		/* the ones still to do take the file's place; if that fails the
		   whole file stays (doing a change twice is harmless, losing it is not) */
		if (pm_fclose(out) != 0 || pm_replace(tmp, path) != 0) { remove(tmp); return -1; }
	} else if (lost) {
		return -1;                            /* could not write the rest down: keep them all */
	} else remove(path);
	return lost ? -1 : 0;
}

/* ------------------------------------------------------------------ pins */

int st_pin_check(const char *hostport, const char *fp)
{
	char path[160];
	static char line[200];
	FILE *f;
	int r = 0;
	snprintf(path, sizeof(path), "%spins.txt", pm_shared()->store_dir);
	if (!(f = fopen(path, "r"))) return 0;
	while (fgets(line, sizeof(line), f)) {
		char *t = strchr(line, '\t');
		line[strcspn(line, "\r\n")] = 0;
		if (!t) continue;
		*t = 0;
		if (!strcmp(line, hostport)) r = strcmp(t + 1, fp) ? -1 : 1;
		if (r == 1) break;
	}
	fclose(f);
	return r;
}

int st_pin_save(const char *hostport, const char *fp)
{
	char path[160];
	FILE *f;
	pm_mkdir(pm_shared()->store_dir);
	snprintf(path, sizeof(path), "%spins.txt", pm_shared()->store_dir);
	if (!(f = fopen(path, "a"))) return -1;
	fprintf(f, "%s\t%s\n", hostport, fp);
	fclose(f);
	return 0;
}
