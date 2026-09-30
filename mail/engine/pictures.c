/* pictures.c - the pictures in a message: fetched and decoded here, in the
 * engine, so the app never waits on them
 *
 *   <folder>\<uid>.pic            the picture parts of a message, one a line:
 *                                 part TAB size TAB type TAB enc TAB cid TAB name
 *                                 (written with the text, from BODYSTRUCTURE)
 *   <folder>\<uid>_<part>.img     one part as it came (decoded from base64), while
 *                                 it waits to be turned into greys; '.' in the
 *                                 part id becomes '_' ("1.2" -> "123_1_2.img")
 *   <folder>\<uid>_<part>.pmi     the same in 16 greys, shrunk to fit the reader
 *                                 (see PmiHeader below), or a note of why not
 *
 * The app reads the .pic list when it shows a message, and asks for the
 * parts it wants (PM_CMD_PICTURES, the ids in arg: "1.2 1.3", a '!' before
 * one that is to be fetched however big). Each part is downloaded, decoded
 * (img/pmimg.h) and written as a .pmi; the app is told (st_changed) after
 * each one, so pictures appear as they are ready. A .pmi that is there is
 * never made again: it is the cache. A part that could not be decoded gets
 * a .pmi saying why, so it isn't tried at every reading.
 *
 * Limits (psimail.h): a part bigger than PM_PIC_MAX_KB is never fetched;
 * the decoded pictures of one message stop at PM_PIC_BUDGET_KB; a decode
 * that takes over PIC_TIME_MS is given up (a slow ARM on a huge picture).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm.h"
#include "img/pmimg.h"

#define PIC_TIME_MS 90000L
#define PIC_MAX_PARTS 24

/* the .pmi file: this header, then (status 0 or 1) h rows of stride bytes,
   two pixels a byte as EPOC's EGray16 (the left pixel in the low nibble),
   or (status 2) a NUL-terminated reason. Little-endian, as ARM and x86 are. */
typedef struct
	{
	char magic[4];           /* "PMI1" */
	unsigned short w, h;     /* the picture as stored */
	unsigned short src_w, src_h;   /* its own size */
	unsigned char status;    /* 0 whole, 1 cut short (the rest grey), 2 not shown: a reason follows */
	unsigned char reserved[3];
	} PmiHeader;

typedef struct
	{
	char part[16];
	long size;
	char type[24];
	int enc;
	char cid[80];
	char name[80];
	} PicEntry;

static int is_picture_type(const char *type)
{
	return !pm_strcasecmp(type, "image/jpeg") || !pm_strcasecmp(type, "image/pjpeg") ||
	       !pm_strcasecmp(type, "image/png") || !pm_strcasecmp(type, "image/x-png") ||
	       !pm_strcasecmp(type, "image/gif");
}

static void pic_path(int acct, const char *folder, unsigned int uid, const char *part, const char *ext, char *out, int max)
{
	char d[160], id[20];
	int i;
	st_folder_dir(acct, folder, d, sizeof(d));
	pm_copy(id, part, sizeof(id));
	for (i = 0; id[i]; i++) if (id[i] == '.') id[i] = '_';
	snprintf(out, max, "%s%u_%s.%s", d, uid, id, ext);
}

void pic_write_index(int acct, const char *folder, unsigned int uid, const PmStructure *st)
{
	char path[190], tmp[196];
	FILE *f;
	int i, n = 0;
	st_msg_path(acct, folder, uid, "pic", path, sizeof(path));
	for (i = 0; i < st->n; i++)
		if (i != st->text && is_picture_type(st->part[i].type)) n++;
	if (!n) { remove(path); return; }
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (!(f = fopen(tmp, "w"))) return;
	fputs("#PSIMAIL1\n", f);
	for (i = 0; i < st->n; i++) {
		const PmPart *p = &st->part[i];
		if (i == st->text || !is_picture_type(p->type)) continue;
		fprintf(f, "%s\t%ld\t%s\t%d\t%s\t%s\n", p->id, p->size, p->type, p->enc, p->cid, p->name);
	}
	if (pm_fclose(f) != 0 || pm_replace(tmp, path) != 0) { remove(tmp); remove(path); }
}

static int read_index(int acct, const char *folder, unsigned int uid, PicEntry *e, int max)
{
	char path[190], line[400];
	FILE *f;
	int n = 0;
	st_msg_path(acct, folder, uid, "pic", path, sizeof(path));
	if (!(f = fopen(path, "r"))) return 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		char *fld[6];
		int k = 0;
		char *p = line;
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || !line[0]) continue;
		fld[k++] = p;
		while (k < 6 && (p = strchr(p, '\t')) != 0) { *p++ = 0; fld[k++] = p; }
		if (k < 4) continue;
		memset(&e[n], 0, sizeof(e[n]));
		pm_copy(e[n].part, fld[0], sizeof(e[n].part));
		e[n].size = atol(fld[1]);
		pm_copy(e[n].type, fld[2], sizeof(e[n].type));
		e[n].enc = atoi(fld[3]);
		if (k > 4) pm_copy(e[n].cid, fld[4], sizeof(e[n].cid));
		if (k > 5) pm_copy(e[n].name, fld[5], sizeof(e[n].name));
		n++;
	}
	fclose(f);
	return n;
}

typedef struct { char dir[160]; char prefix[24]; } RmCtx;

static void rm_cb(const char *name, void *ctx)
{
	RmCtx *c = (RmCtx *)ctx;
	char p[220];
	if (strncmp(name, c->prefix, strlen(c->prefix))) return;
	snprintf(p, sizeof(p), "%s%s", c->dir, name);
	remove(p);
}

void pic_remove(int acct, const char *folder, unsigned int uid)
{
	static RmCtx c;
	char path[190];
	st_msg_path(acct, folder, uid, "pic", path, sizeof(path));
	remove(path);
	st_folder_dir(acct, folder, c.dir, sizeof(c.dir));
	snprintf(c.prefix, sizeof(c.prefix), "%u_", uid);
	pm_list_dir(c.dir, ".pmi", rm_cb, &c);
	pm_list_dir(c.dir, ".img", rm_cb, &c);
}

/* ---------------------------------------------------------- decoding */

static long file_size(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return n;
}

typedef struct { unsigned long start; const char *label; int shown; } AbortCtx;

/* every few rows: the busy message follows, and a decode that is stopped
   or has gone on too long ends */
static int pic_abort(void *ctx, int percent)
{
	AbortCtx *a = (AbortCtx *)ctx;
	if (pm_cancelled()) return 1;
	if (percent >= a->shown + 10) {
		a->shown = percent - percent % 10;
		pm_progress("Setting out %s... %d%%", a->label, a->shown);
	}
	/* (signed: the clock has been seen to step back a little in the emulator) */
	return (long)(pm_ms() - a->start) > PIC_TIME_MS;
}

/* writes a .pmi: the picture, or the reason there is none. 0 ok */
static int write_pmi(const char *path, const PmImage *img, const char *reason)
{
	char tmp[210];
	PmiHeader h;
	unsigned char *buf;
	long n, body;
	int ok;
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, "PMI1", 4);
	if (img) {
		h.w = (unsigned short)img->w;
		h.h = (unsigned short)img->h;
		h.src_w = (unsigned short)(img->src_w > 65535 ? 65535 : img->src_w);
		h.src_h = (unsigned short)(img->src_h > 65535 ? 65535 : img->src_h);
		h.status = img->partial ? 1 : 0;
		body = (long)img->stride * img->h;
	} else {
		h.status = 2;
		body = (long)strlen(reason) + 1;
	}
	/* the whole file in one write (see pm_write_whole) */
	n = (long)sizeof(h) + body;
	if (!(buf = (unsigned char *)malloc(n))) return -1;
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), img ? (const void *)img->bits : (const void *)reason, body);
	ok = pm_write_whole(tmp, buf, n) == 0 && pm_replace(tmp, path) == 0;
	free(buf);
	if (!ok) remove(tmp);
	return ok ? 0 : -1;
}

static int pmi_read_header(const char *path, PmiHeader *h)
{
	FILE *f = fopen(path, "rb");
	int ok;
	if (!f) return -1;
	ok = fread(h, 1, sizeof(*h), f) == sizeof(*h) && !memcmp(h->magic, "PMI1", 4);
	fclose(f);
	return ok ? 0 : -1;
}

/* decodes <uid>_<part>.img to .pmi; the .img goes either way. 0 ok, 1 not
   shown (a .pmi says why), -1 could not write */
static int decode_part(const char *img_path, const char *pmi_path, const char *label, long budget_left, char *note, int notemax)
{
	PmImgOpts o;
	PmImage img;
	AbortCtx a;
	char err[80];
	int r, w;
	memset(&o, 0, sizeof(o));
	o.max_w = PM_PIC_MAX_W;
	o.max_h = PM_PIC_MAX_H;
	o.max_full_bytes = 640L * 1024L;
	o.abort = pic_abort;
	o.abort_ctx = &a;
	a.start = pm_ms();
	a.label = label;
	a.shown = 0;
	pm_progress("Setting out %s...", label);
	{
		/* over the message's budget before it is even decoded? (the size is
		   in the first bytes; the JPEG header may be too long to say) */
		static unsigned char head[4096];
		FILE *f = fopen(img_path, "rb");
		int n = f ? (int)fread(head, 1, sizeof(head), f) : 0, sw, sh;
		if (f) fclose(f);
		if (n > 0 && pmimg_size(head, n, &sw, &sh) == 0 && sw > 0 && sh > 0) {
			int fw = (sw + o.max_w - 1) / o.max_w, fh = (sh + o.max_h - 1) / o.max_h, fs = fw > fh ? fw : fh;
			long ow = (sw + fs - 1) / fs, oh = (sh + fs - 1) / fs;
			if (((ow + 7) / 8) * 4L * oh > budget_left) {
				remove(img_path);
				pm_log("picture %s: %dx%d would be over the message's budget", label, sw, sh);
				snprintf(note, notemax, "too many pictures in this message");
				return write_pmi(pmi_path, 0, "too many pictures in this message") ? -1 : 1;
			}
		}
	}
	r = pmimg_decode_file(img_path, &o, &img, err, sizeof(err));
	remove(img_path);
	if (r != PMIMG_OK) {
		const char *why = r == PMIMG_E_ABORTED ? (pm_cancelled() ? "stopped" : "it takes too long to decode") :
			r == PMIMG_E_MEMORY ? "not enough memory" : err;
		pm_log("picture %s: %s (%d)", label, why, r);
		snprintf(note, notemax, "%s", why);
		if (r == PMIMG_E_ABORTED && pm_cancelled()) return 1;    /* (no note: try again next time) */
		w = write_pmi(pmi_path, 0, why);
		return w ? -1 : 1;
	}
	if ((long)img.stride * img.h > budget_left) {
		pm_log("picture %s: over the message's budget (%ld bytes)", label, (long)img.stride * img.h);
		pmimg_free(&img);
		snprintf(note, notemax, "too many pictures in this message");
		w = write_pmi(pmi_path, 0, "too many pictures in this message");
		return w ? -1 : 1;
	}
	w = write_pmi(pmi_path, &img, 0);
	pm_log("picture %s: %dx%d -> %dx%d%s in %lu ms", label, img.src_w, img.src_h, img.w, img.h,
		img.partial ? " (cut short)" : "", pm_ms() - a.start);
	pmimg_free(&img);
	return w ? -1 : 0;
}

int pic_fetch(int acct, const char *folder, unsigned int uid, const char *parts, char *why, int whymax)
{
	static PicEntry e[PIC_MAX_PARTS];
	PmShared *s = pm_shared();
	int n, i, r = PM_RES_OK, done = 0, shown = 0, notshown = 0, wanted = 0, k;
	long budget = PM_PIC_BUDGET_KB * 1024L;
	const char *p = parts;
	char want[PIC_MAX_PARTS][16];
	int force[PIC_MAX_PARTS], nwant = 0;
	char note[80], label[40];

	n = read_index(acct, folder, uid, e, PIC_MAX_PARTS);
	if (!n) { snprintf(why, whymax, "No pictures in the message"); return PM_RES_FAILED; }
	/* what is asked for */
	while (*p && nwant < PIC_MAX_PARTS) {
		int f = 0, l = 0;
		while (*p == ' ') p++;
		if (*p == '!') { f = 1; p++; }
		while (*p && *p != ' ' && l < 15) want[nwant][l++] = *p++;
		want[nwant][l] = 0;
		while (*p && *p != ' ') p++;
		if (l) { force[nwant] = f; nwant++; }
	}
	/* what is there already counts against the budget */
	for (i = 0; i < n; i++) {
		char pmi[200];
		PmiHeader h;
		pic_path(acct, folder, uid, e[i].part, "pmi", pmi, sizeof(pmi));
		if (pmi_read_header(pmi, &h) == 0 && h.status != 2) budget -= ((h.w + 7) / 8) * 4L * h.h;
	}
	note[0] = 0;
	for (k = 0; k < nwant && !pm_cancelled(); k++) {
		char img[200], pmi[200];
		PicEntry *pe = 0;
		int d;
		for (i = 0; i < n; i++) if (!strcmp(e[i].part, want[k])) pe = &e[i];
		if (!pe) continue;
		wanted++;
		pic_path(acct, folder, uid, pe->part, "pmi", pmi, sizeof(pmi));
		pic_path(acct, folder, uid, pe->part, "img", img, sizeof(img));
		if (file_size(pmi) >= (long)sizeof(PmiHeader)) { done++; continue; }     /* the cache */
		snprintf(label, sizeof(label), "picture %d of %d", k + 1, nwant);
		if (file_size(img) <= 0) {
			/* not here yet: fetch it */
			long kb = (pe->size + 1023) / 1024;
			if (pe->size > PM_PIC_MAX_KB * 1024L) {
				snprintf(note, sizeof(note), "%ld KB: too big to download", kb);
				write_pmi(pmi, 0, note);
				notshown++;
				continue;
			}
			if (!force[k] && pe->size > PM_PIC_AUTO_KB * 1024L) continue;    /* (waits for a tap) */
			if (s->offline) continue;             /* (the app's frames say so; a .img here is still decoded) */
			{
				long need = pe->size / 1024 + 8, freekb;
				char dir[160];
				st_folder_dir(acct, folder, dir, sizeof(dir));
				if ((freekb = pm_free_kb(dir)) >= 0 && freekb < need) {
					snprintf(why, whymax, "No room left for the pictures (%ld KB needed, %ld KB free)", need, freekb);
					return PM_RES_FAILED;
				}
			}
			pm_progress("Downloading %s...", label);
			d = imap_part_to_file(acct, folder, uid, pe->part, pe->enc, pe->size, img, label, why, whymax);
			if (d != PM_RES_OK) { r = d; break; }
		}
		d = decode_part(img, pmi, label, budget, note, sizeof(note));
		if (d < 0) { pm_write_why(why, whymax, "the picture", pmi); r = PM_RES_FAILED; break; }
		if (d == 0) {
			PmiHeader h;
			if (pmi_read_header(pmi, &h) == 0) budget -= ((h.w + 7) / 8) * 4L * h.h;
			shown++;
		} else notshown++;
		done++;
		st_changed();
	}
	if (r == PM_RES_OK && pm_cancelled()) r = PM_RES_CANCELLED;
	if (r == PM_RES_OK) {
		if (notshown && !shown) snprintf(why, whymax, "%s", note[0] ? note : "The picture could not be shown");
		else if (shown == 1) snprintf(why, whymax, "1 picture");
		else if (shown) snprintf(why, whymax, "%d pictures", shown);
		else snprintf(why, whymax, done ? "Pictures" : "No pictures to get");
	}
	if (r == PM_RES_OK && !shown && !notshown) why[0] = 0;       /* nothing to say */
	(void)wanted;
	return r;
}
