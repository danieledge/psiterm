/* psi_drv.c - Links 2 graphics driver for PsiWeb on the Psion Series 5mx
 *
 * Links draws into an ordinary RGB565 frame (640x240) in the engine's own
 * heap. Changed areas are collected into one dirty rectangle and handed to
 * the platform backend (web/fb/pwback.h) from a bottom half, after Links has
 * finished the drawing for the current event:
 *   - on the Psion, pwepoc.cpp turns them into 16 greys in the chunk shared
 *     with PsiWeb.app;
 *   - on a PC, web/fb/pwhost.c keeps them for screenshots.
 *
 * Input comes from pwb_next_event(), polled from a Links timer (the select
 * loop stays the only place the engine waits). Keys use the NSFB codes the
 * NetSurf engine already gets from PsiWeb.app; menu commands (psiweb_cmds.h)
 * become Links actions. Title, address, status, busy and back/forward are
 * reported back with pwb_set_*.
 *
 * Built into links-2.30 by web/links/patches/links-2.30-psion.diff, which
 * adds GRDRV_PSI to drivers.c and makes graphics mode the default.
 */
#include "cfg.h"

#ifdef GRDRV_PSI

#include "links.h"

#include "pwback.h"
#include "psiweb_cmds.h"
#ifdef PSI_EPOC
#include "psiweb.h"
#endif

/* NSFB key codes (libnsfb_event.h) that PsiWeb.app sends */
enum {
	NK_BS = 8, NK_TAB = 9, NK_LF = 10, NK_RETURN = 13, NK_ESC = 27, NK_DEL = 127,
	NK_KP_ENTER = 271, NK_UP = 273, NK_DOWN, NK_RIGHT, NK_LEFT, NK_INS, NK_HOME,
	NK_END, NK_PGUP, NK_PGDN, NK_F1 = 282, NK_F12 = 293,
	NK_RSHIFT = 303, NK_LSHIFT, NK_RCTRL, NK_LCTRL, NK_RALT, NK_LALT,
	NK_MOUSE1 = 401, NK_MOUSE2, NK_MOUSE3, NK_MOUSE4, NK_MOUSE5
};

#define POLL_MS		20	/* input poll while active */
#define POLL_QUIET_MS	2000	/* (0.81) ...and with nothing happening: PsiWeb.app's ring wakes it */
#define QUIET_AFTER_MS	1000	/* nothing for this long: quiet */
#define STATUS_MS	250	/* title/url/status/busy report */

static unsigned short *psi_fb;
static int psi_w, psi_h;
static int dirty, dx0, dy0, dx1, dy1;
static struct timer *poll_timer;
static uttime last_active;		/* input, a command or an answer last taken */
static int in_poll;
#ifdef PSI_EPOC
extern int psi_quiet_ok(void);		/* psi_os.c: the app rings the doorbell */
#endif
static uttime last_status;
static int mods;			/* KBD_SHIFT | KBD_CTRL | KBD_ALT */
static int mouse_x, mouse_y, mouse_down;
static int home_done;
static int last_busy = -1, last_back = -1, last_fwd = -1;
static unsigned char last_url[512], last_status_txt[256];
static unsigned char *psi_param;
static unsigned char *psi_pics_url;	/* View > Show pictures was asked for this page */
static char psi_conn_why[80];		/* why the last connection failed (psi_os.c) */

extern struct graphics_driver psi_driver;

/* memory accounting (psi_mem.c on the PC; RHeap figures on the Psion) */
void psi_mem_page_start(void);
void psi_mem_report(const char *what);

/* ---------- drawing ---------- */

static inline void mark(int x0, int y0, int x1, int y1);
static void check_heap(int busy);
static void save_progress(void);
static void load_start(void);

static void present_bh(void *p)
{
	(void)p;
	if (!dirty) return;
	dirty = 0;
	pwb_present(psi_fb, psi_w, dx0, dy0, dx1, dy1);
}

static inline void mark(int x0, int y0, int x1, int y1)
{
	if (!dirty) {
		dirty = 1;
		dx0 = x0; dy0 = y0; dx1 = x1; dy1 = y1;
		register_bottom_half(present_bh, NULL);
		return;
	}
	if (x0 < dx0) dx0 = x0;
	if (y0 < dy0) dy0 = y0;
	if (x1 > dx1) dx1 = x1;
	if (y1 > dy1) dy1 = y1;
}

/* The ARM710 (ARMv3) has no halfword loads or stores: a 16-bit pixel is
 * two byte accesses. Fills and copies therefore go a word (two pixels) at
 * a time. A source and destination that are 2 bytes apart in alignment
 * are joined from two words, as glyph rows usually are. (Reads stay
 * inside the aligned words holding the row, so inside the heap cell.) */
static void fill16(unsigned short *d, unsigned short c, int n)
{
	unsigned int w = c | ((unsigned int)c << 16), *dw;
	if (n <= 0) return;
	if ((unsigned long)d & 2) *d++ = c, n--;
	for (dw = (unsigned int *)d; n >= 8; n -= 8, dw += 4)
		dw[0] = w, dw[1] = w, dw[2] = w, dw[3] = w;
	for (; n >= 2; n -= 2) *dw++ = w;
	if (n) *(unsigned short *)dw = c;
}

static void copy16(unsigned short *d, const unsigned short *s, int n)
{
	unsigned int *dw;
	if (n <= 0) return;
	if ((unsigned long)d & 2) *d++ = *s++, n--;
	dw = (unsigned int *)d;
	if (!((unsigned long)s & 2)) {
		const unsigned int *sw = (const unsigned int *)s;
		for (; n >= 2; n -= 2) *dw++ = *sw++;
		s = (const unsigned short *)sw;
	} else {
		/* s[0] is the high half of the word at s - 1 (little-endian) */
		const unsigned int *sw = (const unsigned int *)(s - 1);
		unsigned int prev = *sw++, cur;
		for (; n >= 2; n -= 2, s += 2) {
			cur = *sw++;
			*dw++ = (prev >> 16) | (cur << 16);
			prev = cur;
		}
	}
	if (n) *(unsigned short *)dw = *s;
}

#define TEST_INACTIVITY if (dev != current_virtual_device) return;
#define TEST_INACTIVITY_0 if (dev != current_virtual_device) return 0;

/* ---------- bitmaps ----------
 * Glyphs stay RGB565 (they are small, and drawn most often). Pictures
 * (img.c sets psi_pic_bitmap around its get_empty_bitmap calls) are kept
 * as one byte a pixel: the grey level v that psi_grey.c would work out
 * from the 565 pixel. Drawing turns v back into a 565 pixel with that
 * same v (g2p[]), so the screen is the same to the bit, for half the
 * memory.
 *
 * To Links, a picture bitmap still looks like 565 (skip = 2x): it writes
 * 565 rows between get_empty_bitmap and register_bitmap (the buffer is
 * then 2 bytes a pixel; register_bitmap packs it to 1 in place and gives
 * the rest back), and into the scratch rows prepare_strip hands out, which
 * commit_strip packs. */
#define PSI_BMP_GREY	((void *)1)	/* bitmap->flags: packed grey */

int psi_pic_bitmap;			/* img.c: the next bitmap is a picture's */
unsigned long psi_pic_bytes;		/* bytes in picture bitmaps now */

static unsigned int g2p[256];		/* grey v -> a 565 pixel with that v */
static int g2p_ready;
static unsigned char *strip_buf;	/* prepare_strip's scratch rows */
static size_t strip_size;

/* v of a 565 pixel, exactly as psi_grey.c works it out (its table) */
const unsigned char *pw_v565_table(void);
static const unsigned char *vtab;
#define v565(p) (vtab[(p) & 0xffff])

static void make_g2p(void)
{
	unsigned p, n = 0;
	int v;
	vtab = pw_v565_table();
	/* grey 565 values first (r = g = b), then any 565 value for the v
	   still missing: every v from 0 to 255 has one */
	for (v = 0; v < 256; v++) g2p[v] = 0x10000;
	for (p = 0; p < 32; p++) {
		unsigned q = (p << 11) | ((p << 1) << 5) | p;
		if (g2p[v565(q)] == 0x10000) g2p[v565(q)] = q, n++;
		q = (p << 11) | (((p << 1) | 1) << 5) | p;
		if (g2p[v565(q)] == 0x10000) g2p[v565(q)] = q, n++;
	}
	for (p = 0; p < 65536 && n < 256; p++)
		if (g2p[v565(p)] == 0x10000) g2p[v565(p)] = p, n++;
	for (v = 0; v < 256; v++)		/* (none left: belt and braces) */
		if (g2p[v] == 0x10000) g2p[v] = v ? g2p[v - 1] : 0;
	g2p[0] = 0;
	g2p[255] = 0xffff;
	g2p_ready = 1;
}

/* 565 row -> grey row (may be the same memory: d never passes s) */
static void pack_row(unsigned char *d, const unsigned short *s, int n)
{
	const unsigned char *t = vtab;
	for (; n > 0; n--) *d++ = t[*s++];
}

/* Pictures by error diffusion (docs/display.md): once packed to v, each
 * row is Floyd-Steinberg dithered (serpentine) to the calibrated levels
 * and stored as lv[level], a v that psi_grey.c's frame conversion turns
 * back into that same level with no further dithering. Done once per
 * picture, not at every redraw. Rows come all at once (register_bitmap)
 * or in strips (commit_strip), top to bottom; the error is carried from
 * one strip to the next when it follows on, and starts again otherwise
 * (a picture redrawn from the top, another picture). The errors are
 * words (the ARM710 has no halfword loads), two rows; if they cannot be
 * had, the picture is only rounded. */
int pw_grey_diffuse(const unsigned char **levels, const unsigned char **nearest);
static int *fs_err;			/* two rows of (width + 2) */
static int fs_w = -1, fs_next = -1, fs_serp;
static const struct bitmap *fs_bmp;

static void fs_forget(void)
{
	fs_bmp = NULL;
	fs_next = -1;
}

static void fs_rows(const struct bitmap *bmp, unsigned char *row, int top, int lines, size_t stride)
{
	const unsigned char *lvt, *cal;
	int *ec, *en, *t;
	int w = bmp->x, y, x;
	if (!pw_grey_diffuse(&lvt, &cal) || w <= 0) return;
	if (bmp != fs_bmp || top != fs_next || w != fs_w || !fs_err) {
		if (w != fs_w || !fs_err) {
			if (fs_err) mem_free(fs_err);
			fs_err = mem_alloc_mayfail(sizeof(int) * 2 * (size_t)(w + 2));
			fs_w = fs_err ? w : -1;
		}
		if (!fs_err) {			/* no memory: the nearest level only */
			for (y = 0; y < lines; y++, row += stride)
				for (x = 0; x < w; x++) row[x] = lvt[cal[row[x]]];
			fs_forget();
			return;
		}
		memset(fs_err, 0, sizeof(int) * 2 * (size_t)(w + 2));
		fs_serp = 0;
		fs_bmp = bmp;
	}
	ec = fs_err;
	en = fs_err + w + 2;
	for (y = 0; y < lines; y++, row += stride) {
		/* of each pixel's error 7/16 goes on to the next (fwd), 3/16 back
		   and down, 5/16 down, 1/16 on and down: the row below is built
		   up in dn1 and dn2 and written once per pixel (as mail/engine/
		   img/pmimg.c). Two loops with pointers, one each way, so gcc 3.0
		   keeps everything in registers (about 25 instructions a pixel) */
		int fwd = 0, dn1 = 0, dn2 = 0, v, l, e, d7, e3, e5;
#define FS_PIXEL(p, ei, eo) \
		v = *(p) + *(ei) + fwd; \
		if (v < 0) v = 0; else if (v > 255) v = 255; \
		l = lvt[cal[v]]; \
		*(p) = (unsigned char)l; \
		e = v - l; \
		d7 = (e * 7) >> 4; e3 = (e * 3) >> 4; e5 = (e * 5) >> 4; \
		fwd = d7; \
		*(eo) = dn1 + e3; \
		dn1 = dn2 + e5; \
		dn2 = e - d7 - e3 - e5;
		if (!fs_serp) {
			unsigned char *p = row, *pe = row + w;
			int *ei = ec + 1, *eo = en;		/* ec[x + 1], en[x] */
			for (; p < pe; p++, ei++, eo++) { FS_PIXEL(p, ei, eo) }
			*eo = dn1;				/* en[w] */
		} else {
			unsigned char *p = row + w - 1;
			int *ei = ec + w, *eo = en + w + 1;	/* ec[x + 1], en[x + 2] */
			for (; p >= row; p--, ei--, eo--) { FS_PIXEL(p, ei, eo) }
			*eo = dn1;				/* en[1] */
		}
#undef FS_PIXEL
		t = ec; ec = en; en = t;
		memset(en, 0, sizeof(int) * (size_t)(w + 2));
		fs_serp = !fs_serp;
	}
	if (ec != fs_err) {			/* keep the incoming row first */
		memcpy(fs_err, ec, sizeof(int) * (size_t)(w + 2));
		memset(fs_err + w + 2, 0, sizeof(int) * (size_t)(w + 2));
	}
	fs_next = top + lines;
}

static int psi_get_empty_bitmap(struct bitmap *dest)
{
	size_t n;
	dest->data = NULL;
	dest->flags = psi_pic_bitmap ? PSI_BMP_GREY : NULL;
	if (dest->x && (size_t)dest->x * (size_t)dest->y / (size_t)dest->x != (size_t)dest->y)
		return -1;
	if ((size_t)dest->x * (size_t)dest->y > MAX_SIZE_T / 4)
		return -1;
	n = (size_t)dest->x * (size_t)dest->y * 2;
	dest->data = mem_alloc_mayfail(n ? n : 2);
	if (!dest->data)
		return -1;
	dest->skip = (ssize_t)dest->x * 2;
	if (dest->flags) psi_pic_bytes += n;
	return 0;
}

static void psi_register_bitmap(struct bitmap *bmp)
{
	size_t n;
	unsigned char *d;
	const unsigned char *s;
	int y;
	if (bmp->flags != PSI_BMP_GREY || !bmp->data) return;
	n = (size_t)bmp->x * (size_t)bmp->y;
	if (!n) return;
	if (!g2p_ready) make_g2p();
	d = bmp->data;
	s = bmp->data;
	for (y = 0; y < bmp->y; y++, d += bmp->x, s += bmp->skip)
		pack_row(d, (const unsigned short *)s, bmp->x);
	fs_forget();
	fs_rows(bmp, bmp->data, 0, bmp->y, (size_t)bmp->x);
	fs_forget();
	psi_pic_bytes -= n;			/* (2n before, n now) */
	bmp->data = mem_realloc(bmp->data, n);	/* (shrinking: in place) */
	bmp->flags = (void *)2;			/* packed */
}
#define PACKED(b) ((b)->flags == (void *)2)

static void psi_unregister_bitmap(struct bitmap *bmp)
{
	if (bmp == fs_bmp) fs_forget();
	if (bmp->flags && bmp->data)
		psi_pic_bytes -= (size_t)bmp->x * (size_t)bmp->y * (PACKED(bmp) ? 1 : 2);
	if (bmp->data) mem_free(bmp->data);
	bmp->data = NULL;
}

static void *psi_prepare_strip(struct bitmap *bmp, int top, int lines)
{
	size_t n;
	if (!bmp->data) return NULL;
	if (!PACKED(bmp)) return (unsigned char *)bmp->data + bmp->skip * top;
	n = (size_t)bmp->skip * (size_t)lines;
	if (n > strip_size) {
		unsigned char *b = mem_alloc_mayfail(n);
		if (!b) return NULL;
		if (strip_buf) mem_free(strip_buf);
		strip_buf = b;
		strip_size = n;
	}
	return strip_buf;
}

static void psi_commit_strip(struct bitmap *bmp, int top, int lines)
{
	int y;
	if (!PACKED(bmp) || !bmp->data || !strip_buf) return;
	if (top < 0 || top + lines > bmp->y) return;
	for (y = 0; y < lines; y++)
		pack_row((unsigned char *)bmp->data + (size_t)(top + y) * bmp->x,
			(const unsigned short *)(strip_buf + (size_t)y * bmp->skip), bmp->x);
	fs_rows(bmp, (unsigned char *)bmp->data + (size_t)top * bmp->x, top, lines, (size_t)bmp->x);
	if (strip_size > 65536) {		/* (a wide picture's: give it back) */
		mem_free(strip_buf);
		strip_buf = NULL;
		strip_size = 0;
	}
}

/* a grey row onto the 565 frame */
static void unpack16(unsigned short *d, const unsigned char *s, int n)
{
	unsigned int *dw;
	if (n <= 0) return;
	if ((unsigned long)d & 2) *d++ = (unsigned short)g2p[*s++], n--;
	dw = (unsigned int *)d;
	for (; n >= 4; n -= 4, s += 4, dw += 2) {
		dw[0] = g2p[s[0]] | (g2p[s[1]] << 16);
		dw[1] = g2p[s[2]] | (g2p[s[3]] << 16);
	}
	for (; n >= 2; n -= 2, s += 2) *dw++ = g2p[s[0]] | (g2p[s[1]] << 16);
	if (n) *(unsigned short *)dw = (unsigned short)g2p[*s];
}

static void psi_draw_bitmap(struct graphics_device *dev, struct bitmap *bmp, int x, int y)
{
	int xs, ys, packed = PACKED(bmp);
	ssize_t skip = packed ? bmp->x : bmp->skip;
	unsigned char *data = bmp->data;
	unsigned short *d;

	if (!data) return;
	TEST_INACTIVITY
	CLIP_DRAW_BITMAP
	xs = bmp->x;
	ys = bmp->y;
	if (x + xs > dev->clip.x2) xs = dev->clip.x2 - x;
	if (y + ys > dev->clip.y2) ys = dev->clip.y2 - y;
	if (dev->clip.x1 - x > 0) {
		xs -= dev->clip.x1 - x;
		data += (packed ? 1 : 2) * (dev->clip.x1 - x);
		x = dev->clip.x1;
	}
	if (dev->clip.y1 - y > 0) {
		ys -= dev->clip.y1 - y;
		data += skip * (dev->clip.y1 - y);
		y = dev->clip.y1;
	}
	if (xs <= 0 || ys <= 0) return;
	mark(x, y, x + xs, y + ys);
	d = psi_fb + y * psi_w + x;
	for (; ys; ys--) {
		if (packed) unpack16(d, data, xs);
		else copy16(d, (const unsigned short *)data, xs);
		data += skip;
		d += psi_w;
	}
}

static void psi_fill_area(struct graphics_device *dev, int x1, int y1, int x2, int y2, long color)
{
	unsigned short c = (unsigned short)color;
	int y, n;

	TEST_INACTIVITY
	CLIP_FILL_AREA
	mark(x1, y1, x2, y2);
	n = x2 - x1;
	for (y = y1; y < y2; y++)
		fill16(psi_fb + y * psi_w + x1, c, n);
}

static void psi_draw_hline(struct graphics_device *dev, int x1, int y, int x2, long color)
{
	unsigned short c = (unsigned short)color;

	TEST_INACTIVITY
	CLIP_DRAW_HLINE
	mark(x1, y, x2, y + 1);
	fill16(psi_fb + y * psi_w + x1, c, x2 - x1);
}

static void psi_draw_vline(struct graphics_device *dev, int x, int y1, int y2, long color)
{
	unsigned short c = (unsigned short)color, *d;

	TEST_INACTIVITY
	CLIP_DRAW_VLINE
	mark(x, y1, x + 1, y2);
	d = psi_fb + y1 * psi_w + x;
	for (; y1 < y2; y1++, d += psi_w) *d = c;
}

/* moves the clip area by (scx, scy); Links redraws what is uncovered */
static int psi_scroll(struct graphics_device *dev, struct rect_set **ignore, int scx, int scy)
{
	int cx1 = dev->clip.x1, cx2 = dev->clip.x2, cy1 = dev->clip.y1, cy2 = dev->clip.y2;
	int w = cx2 - cx1, h = cy2 - cy1, y, len, so, doff;
	unsigned short *dst, *src;

	(void)ignore;
	TEST_INACTIVITY_0
	if (w <= 0 || h <= 0) return 1;
	if (scx >= w || -scx >= w || scy >= h || -scy >= h) {
		mark(cx1, cy1, cx2, cy2);
		return 1;
	}
	if (scx >= 0) { len = w - scx; doff = scx; so = 0; }
	else { len = w + scx; doff = 0; so = -scx; }
	if (scy > 0) {
		for (y = cy2 - 1; y >= cy1 + scy; y--) {
			dst = psi_fb + y * psi_w + cx1;
			src = psi_fb + (y - scy) * psi_w + cx1;
			memmove(dst + doff, src + so, len * 2);
		}
	} else {
		for (y = cy1; y < cy2 + scy; y++) {
			dst = psi_fb + y * psi_w + cx1;
			src = psi_fb + (y - scy) * psi_w + cx1;
			memmove(dst + doff, src + so, len * 2);
		}
	}
	mark(cx1, cy1, cx2, cy2);
	return 1;
}

/* ---------- state reported to PsiWeb.app ---------- */

static struct session *psi_ses(void)
{
	if (list_empty(sessions)) return NULL;
	return list_struct(sessions.next, struct session);
}

static void psi_set_title(struct graphics_device *dev, unsigned char *title)
{
	(void)dev;
	/* Links passes "Links - <page title>": PsiWeb shows just the page */
	if (title && !strncmp(cast_const_char title, "Links - ", 8)) title += 8;
	else if (title && !strcmp(cast_const_char title, "Links")) title = cast_uchar "";
	pwb_set_title(cast_const_char title);
}

static void report_state(int force)
{
	struct session *ses = psi_ses();
	int busy = !list_empty(queue) || !list_empty(downloads);	/* (Esc stops a file being saved too) */
	int back = 0, fwd = 0;
	unsigned char *u = cast_uchar "", *st = cast_uchar "";

	if (ses) {
		if (!list_empty(ses->history)) {
			u = cur_loc(ses)->url;
			back = ses->history.next->next != &ses->history;
		}
		fwd = !list_empty(ses->forward_history);
		if (ses->st) st = ses->st;
	}
	check_heap(busy);
	save_progress();
	if (force || busy != last_busy) {
		if (last_busy == 1 && !busy)
			psi_mem_report(cast_const_char u);
		last_busy = busy;
		pwb_set_busy(busy);
	}
	if (force || back != last_back || fwd != last_fwd) {
		last_back = back; last_fwd = fwd;
		pwb_set_nav(back, fwd);
	}
	if (force || strncmp(cast_const_char u, cast_const_char last_url, sizeof(last_url) - 1)) {
		safe_strncpy(last_url, u, sizeof(last_url));
		pwb_set_url(cast_const_char u);
	}
	if (force || strncmp(cast_const_char st, cast_const_char last_status_txt, sizeof(last_status_txt) - 1)) {
		safe_strncpy(last_status_txt, st, sizeof(last_status_txt));
		pwb_set_status(cast_const_char st);
	}
}

/* ---------- messages for PsiWeb.app ---------- */

/* An infoprint in the app (top right): the line goes where psiglue puts
 * what the link is doing (net.link_msg), which the app already shows. No
 * full stop at the end, as the style guide has it. */
static void psi_infoprint(const char *text)
{
#ifdef PSI_EPOC
	PwShared *s = (PwShared *)pwb_shared();
	char *d;
	int n = 0, max;
	extern void pw_log(const char *);
	if (!s || !text) return;
	while (*text == ' ' || *text == '\n') text++;
	d = s->net.link_msg;
	max = (int)sizeof(s->net.link_msg) - 1;
	for (; text[n] && n < max; n++) d[n] = text[n] == '\n' || text[n] == '\r' ? ' ' : text[n];
	while (n > 0 && (d[n - 1] == ' ' || (d[n - 1] == '.' && !(n >= 3 && d[n - 2] == '.')))) n--;
	d[n] = 0;
	s->net.link_seq++;
	pw_log(d);
#else
	fprintf(stderr, "[infoprint] %s\n", text);
#endif
}

/* a Links message box (bfu.c msg_box, under PSI_NO_BARS): its words, joined
   into one line */
void psi_msg_box(unsigned char *title, unsigned char *text)
{
	char line[160];
	int i, j = 0, sp = 0;
	(void)title;
	for (i = 0; text && text[i] && j < (int)sizeof(line) - 1; i++) {
		unsigned char c = text[i];
		if (c == '\n' || c == '\r' || c == ' ' || c == '\t') { sp = j > 0; continue; }
		if (sp && j < (int)sizeof(line) - 2) line[j++] = ' ';
		sp = 0;
		line[j++] = c;
	}
	line[j] = 0;
	psi_infoprint(line);
}

/* psi_os.c: a connection could not be made, and pwn said why */
void psi_conn_failed(const char *why)
{
	snprintf(psi_conn_why, sizeof(psi_conn_why), "%s", why ? why : "");
}

/* session.c print_error_dialog: the page did not load. Links' words for
   the commonest causes are put plainly (no "error", no "socket") */
void psi_load_failed(unsigned char *why)
{
	static const char *const map[][2] = {
		{ "Error writing to socket", "Page not loaded - the connection broke" },
		{ "Error reading from socket", "Page not loaded - the connection broke" },
		{ "Connection refused", "Page not loaded - the server refused the connection" },
		{ "Receive timeout", "Page not loaded - the server did not answer in time" },
		{ "SSL error", "Page not loaded - the secure connection failed" },
		{ "Host not found", "Page not loaded - server not found" },
		{ "Bad HTTP response", "Page not loaded - the server's answer made no sense" },
		{ "Server returned empty response", "Page not loaded - the server sent nothing" },
		{ "File not found", "File not found" },
		{ "Interrupted", "Stopped" },
	};
	char line[120];
	unsigned i;
	if (psi_conn_why[0]) {
		psi_infoprint(psi_conn_why);
		psi_conn_why[0] = 0;
		return;
	}
	if (!why) why = cast_uchar "";
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!casestrcmp(why, cast_uchar map[i][0])) {
			psi_infoprint(map[i][1]);
			return;
		}
	if (!casecmp(why, cast_uchar "Error ", 6)) why += 6;
	snprintf(line, sizeof(line), "Page not loaded - %s", (char *)why);
	psi_infoprint(line);
}

/* ---------- memory: the soft ceiling ----------
 * psiweb.exe's heap stops at 10 MB (less if the Psion's RAM runs out
 * first), and Links ends the engine when an allocation it cannot do
 * without fails. So:
 *  - big allocations that may fail (picture buffers and bitmaps) stop
 *    PSI_HEAP_SOFT short of the limit, after Links has dropped what it can
 *    (pictures away from the screen, its caches): the picture is shown as
 *    a box, and the page says it is too big;
 *  - a page still loading when the room left falls below PSI_HEAP_STOP
 *    is stopped where it is: what has come stays on the screen. */
long psi_heap_room(void);	/* emu/links_rt.c, epoc/psi_heap.cpp, psi_mem.c */
static int too_big_said;	/* 1: pictures, 2: the page; per page */

static void psi_too_big(int stop)
{
	int what = stop ? 2 : 1;
	if (too_big_said >= what) return;
	too_big_said = what;
	psi_infoprint(stop ? "Page too big - only part of it is shown"
		: "Page too big - not all the pictures are shown");
}

int psi_heap_tight(size_t size)
{
	if (psi_heap_room() - (long)size >= PSI_HEAP_SOFT) return 0;
	shrink_memory(SH_FREE_SOMETHING, 0);	/* (psi_evict_pictures(1) among others) */
	if (psi_heap_room() - (long)size >= PSI_HEAP_SOFT) return 0;
#ifdef PSI_DEBUG_MEM
	fprintf(stderr, "[mem] refused %lu bytes, room %ld KB\n", (unsigned long)size, psi_heap_room() >> 10);
	if (global_cimg) {
		fprintf(stderr, "[mem]   %d x %d", (int)global_cimg->width, (int)global_cimg->height);
		fprintf(stderr, " -> %d x %d", (int)global_cimg->xww, (int)global_cimg->yww);
		fprintf(stderr, " state %d bpp %d\n", (int)global_cimg->state, (int)global_cimg->buffer_bytes_per_pixel);
	}
#endif
	psi_too_big(0);
	return 1;
}

static void check_heap(int busy)
{
#ifdef PSI_DEBUG_MEM
	{
		static long lr; static unsigned long lb;
		long r = psi_heap_room();
		if (r / 65536 != lr / 65536 || psi_pic_bytes / 65536 != lb / 65536)
			fprintf(stderr, "[mem] room %ld KB, picture bitmaps %lu KB, imgcache %lu KB, cache %lu KB\n",
				r >> 10, psi_pic_bytes >> 10, (unsigned long)imgcache_info(CI_BYTES) >> 10, (unsigned long)cache_info(CI_BYTES) >> 10);
		lr = r; lb = psi_pic_bytes;
	}
#endif
	psi_evict_pictures(0);			/* (only if over the budget) */
	if (!busy || psi_heap_room() >= PSI_HEAP_STOP) return;
	shrink_memory(SH_FREE_SOMETHING, 0);
	if (psi_heap_room() >= PSI_HEAP_STOP) return;
	abort_all_connections();
	psi_too_big(1);
}

/* ---------- questions for the user (PsiWeb.app's dialogs) ----------
 * Links asks with its own dialogs; PsiWeb asks through the shared chunk
 * (psiweb.h auth_* and save_*), PsiWeb.app shows an EIKON dialog, and the
 * answer is taken here from the input poll. One question of each kind at
 * a time. On the PC build (no app) the questions are cancelled. */
static tcount ask_rq;			/* the request waiting for a password */
static unsigned char *ask_realm;
static int ask_proxy;
static struct session *save_ses;	/* the session whose ses->tq waits */
static int save_said;			/* last progress line (KB) */
static uttime save_said_at;

static void copy_utf8(char *d, int max, const unsigned char *s)
{
	int n = 0;
	if (s) for (; s[n] && n < max - 1; n++) d[n] = s[n];
	d[n] = 0;
}

/* objreq.c (auth_window): 0 if the app will ask, -1 if it cannot now */
int psi_auth_ask(tcount count, unsigned char *realm, unsigned char *host, int proxy)
{
#ifdef PSI_EPOC
	PwShared *s = (PwShared *)pwb_shared();
	if (!s || s->auth_state != PW_ASK_NONE || ask_realm) return -1;
	ask_rq = count;
	ask_realm = stracpy(realm ? realm : cast_uchar "");
	ask_proxy = proxy;
	s->auth_proxy = proxy;
	copy_utf8(s->auth_host, sizeof(s->auth_host), host);
	copy_utf8(s->auth_realm, sizeof(s->auth_realm), realm);
	s->auth_user[0] = 0;
	s->auth_pass[0] = 0;
	s->auth_state = PW_ASK_ASKING;
	return 0;
#else
	(void)count; (void)realm; (void)host; (void)proxy;
	return -1;
#endif
}

/* session.c (type_query): a response PsiWeb cannot show. 0 if the app
   will ask where to save it, -1 if not (then it is said not available) */
int psi_save_ask(struct session *ses, unsigned char *name, unsigned char *ct, long size)
{
#ifdef PSI_EPOC
	PwShared *s = (PwShared *)pwb_shared();
	if (!s || s->save_state != PW_ASK_NONE || save_ses) return -1;
	save_ses = ses;
	copy_utf8(s->save_name, sizeof(s->save_name), name && *name ? name : cast_uchar "Download");
	copy_utf8(s->save_type, sizeof(s->save_type), ct);
	s->save_size = (int)size;
	s->save_max = PSI_SAVE_MAX;
	s->save_path[0] = 0;
	s->save_state = PW_ASK_ASKING;
	return 0;
#else
	(void)ses; (void)name; (void)ct; (void)size;
	return -1;
#endif
}

/* the session is going: forget the question about it */
void psi_save_forget(struct session *ses)
{
	if (save_ses == ses) save_ses = NULL;
}

static void take_answers(void)
{
#ifdef PSI_EPOC
	PwShared *s = (PwShared *)pwb_shared();
	extern void psi_auth_reply(tcount count, unsigned char *realm, int proxy, unsigned char *user, unsigned char *pass);
	extern void psi_save_reply(struct session *ses, unsigned char *path);
	int st;
	if (!s) return;
	st = s->auth_state;
	if (ask_realm && (st == PW_ASK_OK || st == PW_ASK_CANCEL)) {
		unsigned char *realm = ask_realm;
		ask_realm = NULL;
		s->auth_user[sizeof(s->auth_user) - 1] = 0;
		s->auth_pass[sizeof(s->auth_pass) - 1] = 0;
		psi_auth_reply(ask_rq, realm, ask_proxy, st == PW_ASK_OK ? cast_uchar s->auth_user : NULL, cast_uchar s->auth_pass);
		memset(s->auth_pass, 0, sizeof(s->auth_pass));
		mem_free(realm);
		s->auth_state = PW_ASK_NONE;
		load_start();
	}
	st = s->save_state;
	if (st == PW_ASK_OK || st == PW_ASK_CANCEL) {
		struct session *ses = save_ses;
		save_ses = NULL;
		s->save_path[sizeof(s->save_path) - 1] = 0;
		s->save_state = PW_ASK_NONE;
		if (ses) {
			char *c;
			for (c = s->save_path; *c; c++) if (*c == '\\') *c = '/';	/* (D:/x, as file:// has it) */
			save_said = -1;
			psi_save_reply(ses, st == PW_ASK_OK && s->save_path[0] ? cast_uchar s->save_path : NULL);
		}
	}
#endif
}

/* "Saving x.pdf: 120 of 300 KB..." as the busy message (a line ending in
   "..." is one), at most once a second */
static void save_progress(void)
{
	struct download *down;
	struct list_head *ld;
	char line[120];
	const char *name;
	long kb, of = -1;
	uttime now = get_time();
	if (list_empty(downloads) || now - save_said_at < 1000) return;
	down = list_struct(downloads.next, struct download);
	kb = (long)(down->last_pos >> 10);
	if (kb == save_said) return;
	save_said = kb;
	save_said_at = now;
	if (down->stat.prg && down->stat.prg->size > 0) of = (long)(down->stat.prg->size >> 10);
	name = (const char *)down->file;
	{
		const char *p = name;
		for (; *p; p++) if (*p == '/' || *p == '\\' || *p == ':') name = p + 1;
	}
	if (of >= 0) snprintf(line, sizeof(line), "Saving %.40s: %ld of %ld KB...", name, kb, of);
	else snprintf(line, sizeof(line), "Saving %.40s: %ld KB...", name, kb);
	psi_infoprint(line);
	(void)ld;
}

/* session.c: the file has been saved, or not */
void psi_save_done(unsigned char *file, int ok, unsigned char *why)
{
	char line[120];
	const char *name = (const char *)file, *p;
	for (p = name; p && *p; p++) if (*p == '/' || *p == '\\' || *p == ':') name = p + 1;
	if (ok) snprintf(line, sizeof(line), "Saved %.60s", name ? name : "");
	else snprintf(line, sizeof(line), "File not saved - %.80s", why ? (char *)why : "");
	psi_infoprint(line);
	save_said = -1;
}

/* session.c cached_format_html: pictures for this page? */
int psi_show_pictures(struct session *ses)
{
	if (dds.display_images) return 1;	/* the app's "pictures on every page" */
	if (!psi_pics_url || !ses || list_empty(ses->history)) return 0;
	return !strcmp(cast_const_char cur_loc(ses)->url, cast_const_char psi_pics_url);
}

/* ---------- input ---------- */

static void send_key(int key, int flags)
{
	struct graphics_device *dev = current_virtual_device;
	if (getenv("PSI_DEBUG")) fprintf(stderr, "[psi] key %d flags %d dev %p h %p\n", key, flags, (void *)dev, dev ? (void *)dev->keyboard_handler : NULL);
	if (dev && dev->keyboard_handler) dev->keyboard_handler(dev, key, flags);
}

static void send_mouse(int b)
{
	struct graphics_device *dev = current_virtual_device;
	if (dev && dev->mouse_handler) dev->mouse_handler(dev, mouse_x, mouse_y, b);
}

static int nsfb_to_links(int c)
{
	switch (c) {
	case NK_BS: return KBD_BS;
	case NK_TAB: return KBD_TAB;
	case NK_LF: case NK_RETURN: case NK_KP_ENTER: return KBD_ENTER;
	case NK_ESC: return KBD_ESC;	/* (filtered in key_event) */
	case NK_DEL: return KBD_DEL;
	case NK_UP: return KBD_UP;
	case NK_DOWN: return KBD_DOWN;
	case NK_RIGHT: return KBD_RIGHT;
	case NK_LEFT: return KBD_LEFT;
	case NK_INS: return KBD_INS;
	case NK_HOME: return KBD_HOME;
	case NK_END: return KBD_END;
	case NK_PGUP: return KBD_PAGE_UP;
	case NK_PGDN: return KBD_PAGE_DOWN;
	}
#ifndef PSI_NO_BARS
	if (c >= NK_F1 && c <= NK_F12) return KBD_F1 - (c - NK_F1);
#else
	if (c >= NK_F1 && c <= NK_F12) return 0;
#endif
	if (c >= PWB_UNICODE_BASE) return c - PWB_UNICODE_BASE;
	if (c >= 32 && c < 127) return c;
	return 0;
}

static int mod_bit(int c)
{
	switch (c) {
	case NK_RSHIFT: case NK_LSHIFT: return KBD_SHIFT;
	case NK_RCTRL: case NK_LCTRL: return KBD_CTRL;
	case NK_RALT: case NK_LALT: return KBD_ALT;
	}
	return 0;
}

#ifdef PSI_NO_BARS
/* ---------- no Links interface: keys ----------
 * PsiWeb.app's menus do what Links' letter shortcuts do, so outside a form
 * field only the keys for moving round the page reach Links: g, q, /, s,
 * d, Esc and the rest would open Links' own dialogs and menus. */

static int psi_dialog_open(void)
{
	struct session *ses = psi_ses();
	/* a Links window above the page (a <select> list, or a dialog that
	   got through): it has the keys, Esc included, so it can be closed */
	return ses && ses->term && ses->term->windows.next != &ses->win->list_entry;
}

static int psi_in_text_field(void)
{
	struct session *ses = psi_ses();
	struct f_data_c *fd;
	int n;
	if (!ses || !(fd = current_frame(ses)) || !fd->vs || !fd->f_data) return 0;
	n = fd->vs->current_link;
	if (n < 0 || n >= fd->f_data->nlinks) return 0;
	return fd->f_data->links[n].type == L_FIELD || fd->f_data->links[n].type == L_AREA;
}

/* the key to give Links, or 0 to drop it */
static int psi_filter_key(int k, int m)
{
	if (psi_dialog_open()) return k;
	switch (k) {
	case KBD_UP: case KBD_DOWN: case KBD_PAGE_UP: case KBD_PAGE_DOWN:
	case KBD_HOME: case KBD_END: case KBD_ENTER: case KBD_TAB:
		return k;
	case KBD_ESC:
		return 0;		/* (the app's Stop) */
	}
	if (psi_in_text_field()) {
		/* typing and editing in a field */
		if (k == KBD_LEFT || k == KBD_RIGHT || k == KBD_BS || k == KBD_DEL) return k;
		if (k > 0 && !(m & (KBD_CTRL | KBD_ALT))) return k;
		if (k > 0 && (m & KBD_CTRL) && strchr("AEUKD", k)) return k;	/* line start/end, delete */
		return 0;
	}
	switch (k) {
	case KBD_LEFT: return '[';	/* scroll a wide page sideways (Links: back) */
	case KBD_RIGHT: return ']';	/* (Links: follow the link) */
	case KBD_BS: return k;		/* back, as browsers have it */
	case ' ': return (m & (KBD_CTRL | KBD_ALT)) ? 0 : k;	/* page down */
	}
	return 0;
}
#endif

static void key_event(int down, int c)
{
	int k, m = mod_bit(c);

	if (m) {
		if (down) mods |= m; else mods &= ~m;
		return;
	}
	if (c >= NK_MOUSE1 && c <= NK_MOUSE5) {
		if (c == NK_MOUSE1) {
			mouse_down = down;
			send_mouse(B_LEFT | (down ? B_DOWN : B_UP));
		} else if (down && c == NK_MOUSE4) send_mouse(B_WHEELUP | B_MOVE);
		else if (down && c == NK_MOUSE5) send_mouse(B_WHEELDOWN | B_MOVE);
		return;
	}
	if (!down) return;
	k = nsfb_to_links(c);
	if (!k) return;
	if (k > 0 && (mods & KBD_CTRL) && k < 128) {
		/* Links' Ctrl shortcuts are on upper-case letters */
		if (k >= 'a' && k <= 'z') k -= 'a' - 'A';
	}
#ifdef PSI_NO_BARS
	if (!(k = psi_filter_key(k, mods))) return;
	if ((k == '[' || k == ']') && !psi_in_text_field() && !psi_dialog_open()) {
		send_key(k, 0);
		return;
	}
#endif
	send_key(k, mods & (KBD_CTRL | KBD_ALT | (k < 0 ? KBD_SHIFT : 0)));
}

/* a load is starting: say busy now, as it may finish before the next report */
int pw_grey_check(void);

static void load_start(void)
{
	pw_grey_check();		/* PsiGrey.ini changed (Display calibration)? */
	psi_mem_page_start();
	too_big_said = 0;
	last_busy = 1;
	pwb_set_busy(1);
}

static void psi_goto(struct session *ses, const char *url)
{
	if (!ses || !url || !*url) return;
	load_start();
	goto_url_utf8(ses, cast_uchar url);
}

static void run_command(int cmd, char *arg)
{
	struct session *ses = psi_ses();
	if (!ses) return;
	switch (cmd) {
	case PW_CMD_OPEN:	psi_goto(ses, arg); break;
	case PW_CMD_HOME: {
		const char *h = pwb_home_url();
		/* (no home page set: NetSurf's about:welcome is ours too) */
		psi_goto(ses, h && *h ? h : "about:welcome");
		break;
	}
	case PW_CMD_BACK:	load_start(); go_back(ses, 1); break;
	case PW_CMD_FORWARD:	load_start(); go_back(ses, -1); break;
	case PW_CMD_RELOAD:	load_start(); reload(ses, -1); break;
	case PW_CMD_STOP:
		abort_all_connections();
		change_screen_status(ses);
		print_screen_status(ses);
		break;
	case PW_CMD_PAGEUP:	send_key(KBD_PAGE_UP, 0); break;
	case PW_CMD_PAGEDOWN:	send_key(KBD_PAGE_DOWN, 0); break;
	case PW_CMD_TOP:	send_key(KBD_HOME, 0); break;
	case PW_CMD_BOTTOM:	send_key(KBD_END, 0); break;
	case PW_CMD_ZOOM: {
		int pct = atoi(arg), fs;
		if (pct < 50) pct = 50;
		if (pct > 300) pct = 300;
		fs = (dds.font_size * pct + 50) / 100;
		if (fs < 6) fs = 6;
		ses->ds.font_size = fs;
#ifdef PSI_STRIKES
		psi_set_font_base(fs);		/* at 100% the pre-drawn fonts, else all scaled */
#endif
		html_interpret_recursive(ses->screen);
		draw_formatted(ses);
		break;
	}
	case PW_CMD_IMAGES:
		/* View > Show pictures: for the page showing (psi_show_pictures) */
		if (psi_pics_url) mem_free(psi_pics_url), psi_pics_url = NULL;
		if (arg[0] == '1' && !list_empty(ses->history)) {
			psi_pics_url = stracpy(cur_loc(ses)->url);
			load_start();
		}
		html_interpret_recursive(ses->screen);
		draw_formatted(ses);
		break;
	case PW_CMD_QUIT:
		terminate_loop = 1;
		break;
	case PW_CMD_HANGUP:
		abort_all_connections();
#ifdef PSI_EPOC
		{
			extern void pwn_release_now(void);
			pwn_release_now();
		}
		pwb_set_status("Hung up - the serial port is free");
#endif
		break;
	case PW_CMD_UPDATE:
		/* Tools > Update PsiWeb (web/engine/pwupdate.c): it needs the
		   connection, and runs to the end here (it keeps the app told
		   through update_state) */
		abort_all_connections();
		{
			extern int pw_update_run(void);
			pw_update_run();
		}
		break;
	}
}

/* (0.81) The input poll's pace: every 20 ms while anything happens, and
   every 2 s once nothing has for a second - the engine's wait (psi_os.c)
   then lets the CPU halt, and PsiWeb.app's doorbell ends it at once when
   there is input (psi_poll_soon). Loading or saving is "happening". */
static int poll_ms(void)
{
#ifdef PSI_EPOC
	if (psi_quiet_ok() && last_busy == 0 && list_empty(queue) && list_empty(downloads) &&
	    get_time() - last_active >= QUIET_AFTER_MS)
		return POLL_QUIET_MS;
#endif
	return POLL_MS;
}

static void poll_fn(void *p);

/* PsiWeb.app has rung: take its input now, not at the next slow poll */
void psi_poll_soon(void)
{
	if (in_poll || !poll_timer)
		return;			/* (in the poll: it looks again anyway) */
	kill_timer(poll_timer);
	poll_timer = install_timer(0, poll_fn, NULL);
}

static void poll_fn(void *p)
{
	pwb_event ev;
	char arg[512];
	int n, cmd, quiet;
	uttime now;

	(void)p;
	quiet = poll_ms() != POLL_MS;
	poll_timer = install_timer(quiet ? POLL_QUIET_MS : POLL_MS, poll_fn, NULL);
	in_poll = 1;

	if (!home_done) {
		struct session *ses = psi_ses();
		if (ses) {
#ifdef PSI_EPOC
			/* what PsiMail asked for, else the built-in welcome page:
			   starting needs no network */
			const char *h = pwb_first_url();
#else
			const char *h = pwb_home_url();
#endif
			home_done = 1;
			if (list_empty(ses->history) && !ses->rq && h && *h && strcmp(h, "about:blank"))
				psi_goto(ses, h);
			report_state(1);
			pwb_ready();
		}
	}

	for (n = 0; n < 32 && pwb_next_event(&ev, 0); n++) {
#ifdef PSI_DEBUG_EVENTS
		if (ev.type != PWB_WAKE) {
			char b[64];
			snprintf(b, sizeof(b), "ev %d code %d at %d,%d dlg %d", ev.type, ev.code, ev.x, ev.y, psi_dialog_open());
			psi_infoprint(b);
		}
#endif
		switch (ev.type) {
		case PWB_KEYDOWN:
		case PWB_KEYUP:
			key_event(ev.type == PWB_KEYDOWN, ev.code);
			break;
		case PWB_MOVE:
			mouse_x = ev.x; mouse_y = ev.y;
			send_mouse(mouse_down ? (B_LEFT | B_DRAG) : (B_LEFT | B_MOVE));
			break;
		case PWB_QUIT:
			terminate_loop = 1;
			in_poll = 0;
			return;
		case PWB_WAKE:
			break;
		}
	}
	while ((cmd = pwb_take_command(arg, sizeof(arg))) != 0) {
		run_command(cmd, arg);
		n++;
	}
	take_answers();

	now = get_time();
	if (n > 0)
		last_active = now;
	if (now - last_status >= STATUS_MS) {
		last_status = now;
		report_state(0);
	}
	in_poll = 0;
	if (quiet && poll_ms() == POLL_MS && poll_timer) {
		/* input after a quiet spell: the quick pace from now */
		kill_timer(poll_timer);
		poll_timer = install_timer(POLL_MS, poll_fn, NULL);
	}
}

/* ---------- driver ---------- */

static unsigned char *psi_init_driver(unsigned char *param, unsigned char *display)
{
	int w = 640, h = 240;
	(void)display;

	psi_param = stracpy(param ? param : cast_uchar "");
	if (pwb_open(&w, &h) != 0)
		return stracpy(cast_uchar "psi: no screen\n");
	psi_w = w; psi_h = h;
	psi_fb = malloc((size_t)w * h * 2);
	if (!psi_fb) {
		pwb_close();
		return stracpy(cast_uchar "psi: no memory for the screen\n");
	}
	memset(psi_fb, 0xff, (size_t)w * h * 2);

	psi_driver.x = w;
	psi_driver.y = h;
	psi_driver.depth = 2 | (16 << 3);	/* RGB565, little-endian */
	psi_driver.get_color = get_color_fn(psi_driver.depth);
	if (!psi_driver.get_color) {
		free(psi_fb);
		pwb_close();
		return stracpy(cast_uchar "psi: no colour function\n");
	}
	init_virtual_devices(&psi_driver, 1);
#ifdef PSI_EPOC
	/* PsiWeb.app's setting; off unless the user has asked for pictures */
	dds.display_images = pwb_load_images() ? 1 : 0;
#endif
	last_status = get_time();
	last_active = last_status;		/* (quick while starting) */
	poll_timer = install_timer(POLL_MS, poll_fn, NULL);
	return NULL;
}

static void psi_shutdown_driver(void)
{
	if (poll_timer) kill_timer(poll_timer), poll_timer = NULL;
	unregister_bottom_half(present_bh, NULL);
	shutdown_virtual_devices();
	psi_mem_report("exit");
	pwb_close();
	free(psi_fb);
	psi_fb = NULL;
	if (psi_param) mem_free(psi_param), psi_param = NULL;
	if (psi_pics_url) mem_free(psi_pics_url), psi_pics_url = NULL;
}

static void psi_emergency_shutdown(void) { }

static unsigned char *psi_get_driver_param(void) { return psi_param; }

static void psi_get_margin(int *l, int *r, int *t, int *b) { *l = *r = *t = *b = 0; }

static int psi_set_margin(int l, int r, int t, int b) { (void)l; (void)r; (void)t; (void)b; return -1; }

static int psi_block(struct graphics_device *dev) { (void)dev; return 0; }

static int psi_unblock(struct graphics_device *dev)
{
	if (dev && dev->redraw_handler) dev->redraw_handler(dev, &dev->size);
	return 0;
}

struct graphics_driver psi_driver = {
	cast_uchar "psi",
	psi_init_driver,
	init_virtual_device,
	shutdown_virtual_device,
	psi_shutdown_driver,
	psi_emergency_shutdown,
	NULL,				/* after_fork */
	psi_get_driver_param,
	NULL,				/* get_af_unix_name */
	psi_get_margin,
	psi_set_margin,
	psi_get_empty_bitmap,
	psi_register_bitmap,
	psi_prepare_strip,
	psi_commit_strip,
	psi_unregister_bitmap,
	psi_draw_bitmap,
	NULL,				/* get_color (set in init) */
	psi_fill_area,
	psi_draw_hline,
	psi_draw_vline,
	psi_scroll,
	NULL,				/* set_clip_area */
	NULL,				/* flush: presented from a bottom half */
	psi_block,
	psi_unblock,
	NULL,				/* set_palette */
	NULL,				/* get_real_colors */
	psi_set_title,
	NULL,				/* exec: no external programs on the Psion */
	NULL,				/* set_clipboard_text */
	NULL,				/* get_clipboard_text */
	0,				/* depth (set in init) */
	0, 0,				/* size (set in init) */
	GD_UNICODE_KEYS | GD_ONLY_1_WINDOW | GD_NO_OS_SHELL | GD_NO_LIBEVENT,
	NULL,				/* param */
};

#endif /* GRDRV_PSI */
