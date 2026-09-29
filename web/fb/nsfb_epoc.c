/* nsfb_epoc.c - the libnsfb "epoc" surface for PsiWeb
 *
 * NetSurf's framebuffer front end draws into an ordinary RGB565 buffer in
 * psiweb's own heap. On each update the changed rectangle is turned into 16
 * greys (ordered dither) by a backend (pwback.h): on the Psion that is the
 * chunk shared with PsiWeb.app; on a PC it is a test harness that writes
 * screenshots.
 *
 * libnsfb normally registers surfaces with __attribute__((constructor)),
 * which EPOC's EXE start-up never runs, so psiweb calls nsfb_epoc_register()
 * from main() instead.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"

#include "pwback.h"

static int epoc_defaults(nsfb_t *nsfb)
{
	nsfb->width = 640;
	nsfb->height = 240;
	nsfb->format = NSFB_FMT_RGB565;
	select_plotters(nsfb);
	return 0;
}

static int epoc_initialise(nsfb_t *nsfb)
{
	int w = nsfb->width, h = nsfb->height;
	size_t size;

	if (pwb_open(&w, &h) != 0)
		return -1;
	nsfb->width = w;
	nsfb->height = h;
	nsfb->format = NSFB_FMT_RGB565;
	select_plotters(nsfb);

	size = (size_t)nsfb->width * nsfb->height * 2;
	nsfb->ptr = malloc(size);
	if (nsfb->ptr == NULL)
		return -1;
	memset(nsfb->ptr, 0xff, size);
	nsfb->linelen = nsfb->width * 2;
	return 0;
}

static int epoc_geometry(nsfb_t *nsfb, int width, int height,
		enum nsfb_format_e format)
{
	/* the Psion screen does not change size: keep what the backend gave */
	(void)format;
	if (nsfb->ptr == NULL) {
		if (width > 0) nsfb->width = width;
		if (height > 0) nsfb->height = height;
	}
	nsfb->format = NSFB_FMT_RGB565;
	select_plotters(nsfb);
	nsfb->linelen = nsfb->width * 2;
	return 0;
}

static int epoc_finalise(nsfb_t *nsfb)
{
	pwb_close();
	free(nsfb->ptr);
	nsfb->ptr = NULL;
	return 0;
}

static int epoc_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
	int x0 = box->x0, y0 = box->y0, x1 = box->x1, y1 = box->y1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > nsfb->width) x1 = nsfb->width;
	if (y1 > nsfb->height) y1 = nsfb->height;
	if (x0 >= x1 || y0 >= y1)
		return 0;
	pwb_present((const unsigned short *)nsfb->ptr, nsfb->width,
			x0, y0, x1, y1);
	return 0;
}

static bool epoc_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
	pwb_event ev;
	(void)nsfb;

	if (!pwb_next_event(&ev, timeout))
		return false;

	switch (ev.type) {
	case PWB_KEYDOWN:
		event->type = NSFB_EVENT_KEY_DOWN;
		event->value.keycode = ev.code;
		return true;
	case PWB_KEYUP:
		event->type = NSFB_EVENT_KEY_UP;
		event->value.keycode = ev.code;
		return true;
	case PWB_MOVE:
		event->type = NSFB_EVENT_MOVE_ABSOLUTE;
		event->value.vector.x = ev.x;
		event->value.vector.y = ev.y;
		event->value.vector.z = 0;
		return true;
	case PWB_QUIT:
		event->type = NSFB_EVENT_CONTROL;
		event->value.controlcode = NSFB_CONTROL_QUIT;
		return true;
	case PWB_WAKE:
		/* a menu command is waiting: return so the main loop sees it */
		event->type = NSFB_EVENT_CONTROL;
		event->value.controlcode = NSFB_CONTROL_TIMEOUT;
		return true;
	}
	return false;
}

static int epoc_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
	(void)nsfb; (void)box;
	return 0;
}

static int epoc_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
	/* the Psion has a pen, not a mouse: no pointer is drawn */
	(void)nsfb; (void)cursor;
	return 0;
}

static const nsfb_surface_rtns_t epoc_rtns = {
	epoc_defaults,
	epoc_initialise,
	epoc_finalise,
	epoc_geometry,
	NULL,                   /* parameters */
	epoc_input,
	epoc_claim,
	epoc_update,
	epoc_cursor
};

void nsfb_epoc_register(void)
{
	_nsfb_register_surface(NSFB_SURFACE_EPOC, &epoc_rtns, "epoc");
}
