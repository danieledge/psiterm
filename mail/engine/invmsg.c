/* invmsg.c - the engine's side of invitations and contact cards (invite.h):
 * after a message's text is downloaded, its first calendar part and first
 * contact card are fetched (when small) and summed up for PsiMail.app.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm.h"
#include "cal.h"
#include "invite.h"

#define INV_PART_MAX (128L * 1024L)     /* bigger is not an invitation or a card */
#define VCF_MAX_CARDS 8

static int ends_with(const char *s, const char *tail)
{
	int n = (int)strlen(s), t = (int)strlen(tail);
	return n >= t && !pm_strcasecmp(s + n - t, tail);
}

/* 1 a calendar, 2 a contact card, 0 neither */
static int part_kind(const PmPart *p)
{
	if (!strcmp(p->type, "text/calendar") || !strcmp(p->type, "application/ics") ||
	    ends_with(p->name, ".ics") || ends_with(p->name, ".vcs")) return 1;
	if (!strcmp(p->type, "text/vcard") || !strcmp(p->type, "text/x-vcard") || !strcmp(p->type, "text/directory") ||
	    ends_with(p->name, ".vcf")) return 2;
	return 0;
}

static char *read_all(const char *path, long max, int *len)
{
	FILE *f = fopen(path, "rb");
	char *b;
	long n;
	*len = 0;
	if (!f) return 0;
	b = (char *)malloc(max + 1);
	if (!b) { fclose(f); return 0; }
	n = (long)fread(b, 1, max, f);
	fclose(f);
	if (n <= 0) { free(b); return 0; }
	b[n] = 0;
	*len = (int)n;
	return b;
}

static void write_summary(int acct, const char *folder, unsigned int uid, const char *ext, int kind,
                          const char *text, int len)
{
	static PmInvite iv;
	static PmCard cards[VCF_MAX_CARDS];
	PmShared *s = pm_shared();
	char path[200], tmp[210];
	FILE *f;
	int n = 0;
	if (kind == 1 && inv_parse(text, len, s->cal.zone, s->acct[acct].email, &iv) != 0) return;
	if (kind == 2 && (n = vcf_parse(text, len, cards, VCF_MAX_CARDS)) <= 0) return;
	st_msg_path(acct, folder, uid, ext, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (!(f = fopen(tmp, "w"))) return;
	if (kind == 1) inv_write(f, &iv); else vcf_write(f, cards, n);
	if (pm_fclose(f) != 0 || pm_replace(tmp, path) != 0) { remove(tmp); pm_log("invite: could not write %s", path); }
}

void inv_fetch(int acct, const char *folder, unsigned int uid, const PmStructure *st)
{
	int i, done[3] = { 0, 0, 0 };
	for (i = 0; i < st->n; i++) {
		const PmPart *p = &st->part[i];
		int kind = part_kind(p), len, r;
		char path[200], why[120];
		const char *ext;
		char *text;
		if (!kind || done[kind] || i == st->text) continue;
		done[kind] = 1;
		if (p->size > INV_PART_MAX * 4 / 3 + 1024) { pm_log("invite: part %s too big (%ld)", p->id, p->size); continue; }
		ext = kind == 1 ? "ics" : "vcf";
		st_msg_path(acct, folder, uid, ext, path, sizeof(path));
		r = imap_part_to_file(acct, folder, uid, p->id, p->enc, p->size, path,
			kind == 1 ? "the invitation" : "the contact card", why, sizeof(why));
		if (r != PM_RES_OK) { pm_log("invite: %s: %s", p->id, why); continue; }
		if (!(text = read_all(path, INV_PART_MAX, &len))) continue;
		write_summary(acct, folder, uid, kind == 1 ? "inv" : "vcd", kind, text, len);
		free(text);
	}
}

void inv_remove(int acct, const char *folder, unsigned int uid)
{
	static const char *ext[] = { "ics", "vcf", "inv", "vcd", "inr", 0 };
	char p[200];
	int i;
	for (i = 0; ext[i]; i++) { st_msg_path(acct, folder, uid, ext[i], p, sizeof(p)); remove(p); }
}
