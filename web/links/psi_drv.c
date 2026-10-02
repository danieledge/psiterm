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

/* NSFB key codes (libnsfb_event.h) that PsiWeb.app sends */
enum {
	NK_BS = 8, NK_TAB = 9, NK_LF = 10, NK_RETURN = 13, NK_ESC = 27, NK_DEL = 127,
	NK_KP_ENTER = 271, NK_UP = 273, NK_DOWN, NK_RIGHT, NK_LEFT, NK_INS, NK_HOME,
	NK_END, NK_PGUP, NK_PGDN, NK_F1 = 282, NK_F12 = 293,
	NK_RSHIFT = 303, NK_LSHIFT, NK_RCTRL, NK_LCTRL, NK_RALT, NK_LALT,
	NK_MOUSE1 = 401, NK_MOUSE2, NK_MOUSE3, NK_MOUSE4, NK_MOUSE5
};

#define POLL_MS		20	/* input poll while active */
#define STATUS_MS	250	/* title/url/status/busy report */

static unsigned short *psi_fb;
static int psi_w, psi_h;
static int dirty, dx0, dy0, dx1, dy1;
static struct timer *poll_timer;
static uttime last_status;
static int mods;			/* KBD_SHIFT | KBD_CTRL | KBD_ALT */
static int mouse_x, mouse_y, mouse_down;
static int home_done;
static int last_busy = -1, last_back = -1, last_fwd = -1;
static unsigned char last_url[512], last_status_txt[256];
static unsigned char *psi_param;

extern struct graphics_driver psi_driver;

/* memory accounting (psi_mem.c on the PC; RHeap figures on the Psion) */
void psi_mem_page_start(void);
void psi_mem_report(const char *what);

/* ---------- drawing ---------- */

static inline void mark(int x0, int y0, int x1, int y1);

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

static int psi_get_empty_bitmap(struct bitmap *dest)
{
	dest->data = NULL;
	if (dest->x && (size_t)dest->x * (size_t)dest->y / (size_t)dest->x != (size_t)dest->y)
		return -1;
	if ((size_t)dest->x * (size_t)dest->y > MAX_SIZE_T / 4)
		return -1;
	dest->data = mem_alloc_mayfail((size_t)dest->x * (size_t)dest->y * 2);
	if (!dest->data)
		return -1;
	dest->skip = (ssize_t)dest->x * 2;
	dest->flags = 0;
	return 0;
}

static void psi_register_bitmap(struct bitmap *bmp) { (void)bmp; }

static void psi_unregister_bitmap(struct bitmap *bmp)
{
	if (bmp->data) mem_free(bmp->data);
}

static void *psi_prepare_strip(struct bitmap *bmp, int top, int lines)
{
	(void)lines;
	if (!bmp->data) return NULL;
	return (unsigned char *)bmp->data + bmp->skip * top;
}

static void psi_commit_strip(struct bitmap *bmp, int top, int lines)
{
	(void)bmp; (void)top; (void)lines;
}

static void psi_draw_bitmap(struct graphics_device *dev, struct bitmap *bmp, int x, int y)
{
	int xs, ys;
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
		data += 2 * (dev->clip.x1 - x);
		x = dev->clip.x1;
	}
	if (dev->clip.y1 - y > 0) {
		ys -= dev->clip.y1 - y;
		data += bmp->skip * (dev->clip.y1 - y);
		y = dev->clip.y1;
	}
	if (xs <= 0 || ys <= 0) return;
	mark(x, y, x + xs, y + ys);
	d = psi_fb + y * psi_w + x;
	for (; ys; ys--) {
		copy16(d, (const unsigned short *)data, xs);
		data += bmp->skip;
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
	int busy = !list_empty(queue);
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
	case NK_ESC:
#ifdef PSI_NO_BARS
		return 0;	/* Esc and F9/F10 would open Links' own menus */
#else
		return KBD_ESC;
#endif
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
	send_key(k, mods & (KBD_CTRL | KBD_ALT | (k < 0 ? KBD_SHIFT : 0)));
}

/* a load is starting: say busy now, as it may finish before the next report */
static void load_start(void)
{
	psi_mem_page_start();
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
	case PW_CMD_HOME:	psi_goto(ses, pwb_home_url()); break;
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
		html_interpret_recursive(ses->screen);
		draw_formatted(ses);
		break;
	}
	case PW_CMD_IMAGES:
		ses->ds.display_images = arg[0] == '1';
		html_interpret_recursive(ses->screen);
		draw_formatted(ses);
		break;
	case PW_CMD_QUIT:
		terminate_loop = 1;
		break;
	case PW_CMD_HANGUP:
		abort_all_connections();
		break;
	}
}

static void poll_fn(void *p)
{
	pwb_event ev;
	char arg[512];
	int n, cmd;
	uttime now;

	(void)p;
	poll_timer = install_timer(POLL_MS, poll_fn, NULL);

	if (!home_done) {
		struct session *ses = psi_ses();
		if (ses) {
			const char *h = pwb_home_url();
			home_done = 1;
			/* (about:welcome is NetSurf's start page: Links has none) */
			if (list_empty(ses->history) && !ses->rq && h && *h && strncmp(h, "about:", 6))
				psi_goto(ses, h);
			report_state(1);
			pwb_ready();
		}
	}

	for (n = 0; n < 32 && pwb_next_event(&ev, 0); n++) {
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
			return;
		case PWB_WAKE:
			break;
		}
	}
	while ((cmd = pwb_take_command(arg, sizeof(arg))) != 0)
		run_command(cmd, arg);

	now = get_time();
	if (now - last_status >= STATUS_MS) {
		last_status = now;
		report_state(0);
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
