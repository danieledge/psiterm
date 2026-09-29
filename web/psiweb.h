/* psiweb.h - memory shared between PsiWeb.app and psiweb.exe
 *
 * PsiWeb.app (the EIKON front: menus, dialogs, the screen) creates a global
 * chunk with this layout and launches psiweb.exe (NetSurf). psiweb draws each
 * page into 'fb' - 4 bits per pixel, 16 greys, the same layout as an EGray16
 * CFbsBitmap - and the app copies the changed rows to the screen.
 *
 * 'net' comes first so the PsiTerm network glue (ssh/psiglue.cpp, which
 * expects a PsiShared at the start of the chunk) works unchanged: the app
 * fills in the serial/dial-up settings; the fetcher sets host/port per
 * connection.
 *
 * Every ring and field has one writer, so no locking is needed on the
 * single-CPU Psion.
 */
#ifndef PSIWEB_H
#define PSIWEB_H

#ifndef PSI_SHARED_NAME
#define PSI_SHARED_NAME "PsiWebShared"
#endif
#include "psishared.h"     /* ssh/ - PsiTerm's shared-memory layout */

#define PW_MAGIC        0x42455750u     /* 'PWEB' */
#define PW_MAX_W        640
#define PW_MAX_H        240
#define PW_STRIDE       (PW_MAX_W / 2)  /* bytes per 4bpp row */
#define PW_EVQ          64
#define PW_URL_MAX      512

/* input events: app writes, psiweb reads */
enum
	{
	PW_EV_KEYDOWN = 1,              /* code = NSFB key code (see pwkeys) */
	PW_EV_KEYUP,
	PW_EV_PENDOWN,                  /* x, y in page-area pixels */
	PW_EV_PENUP,
	PW_EV_PENMOVE
	};

typedef struct
	{
	int type;
	int code;
	int x;
	int y;
	} PwEvent;

#include "psiweb_cmds.h"

/* psiweb state */
enum
	{
	PW_STATE_STARTING = 0,
	PW_STATE_READY,
	PW_STATE_EXITED
	};

typedef struct
	{
	PsiShared net;                  /* MUST stay first - see above */

	unsigned int magic;
	int width;                      /* page area size in pixels (app sets) */
	int height;
	volatile int state;             /* PW_STATE_* (psiweb) */
	volatile int quitting;          /* app: psiweb should exit now. (net.quit
	                                   only interrupts a dial or a download:
	                                   the app sets it for Stop too) */
	volatile int exit_code;
	char exit_msg[128];

	/* connection (app sets) */
	int use_proxy;                  /* 1 = send every request to an HTTP proxy
	                                   such as WebOne, which also does HTTPS */
	char proxy_host[64];
	int proxy_port;
	int load_images;
	int zoom;                       /* percent */
	char home_url[PW_URL_MAX];
	char res_dir[96];               /* e.g. C:\System\Apps\PsiWeb\ */

	/* input */
	volatile unsigned int ev_head;  /* app */
	volatile unsigned int ev_tail;  /* psiweb */
	PwEvent ev[PW_EVQ];

	volatile int cmd;               /* app sets, psiweb clears when taken */
	char cmd_arg[PW_URL_MAX];

	/* output: psiweb writes, app reads */
	volatile unsigned int frame_seq;   /* bumped after each update */
	volatile int dirty_y0;             /* rows changed since the app last looked */
	volatile int dirty_y1;             /* (app resets to y0=height, y1=0) */
	volatile int busy;                 /* a page is loading */
	char status[128];                  /* status bar text (UTF-8) */
	char title[128];                   /* page title (UTF-8) */
	char url[PW_URL_MAX];              /* current URL */
	volatile int can_back;
	volatile int can_forward;
	volatile unsigned int free_ram;    /* bytes, for the status line */
	volatile unsigned int heap_used;

	unsigned char fb[PW_MAX_H * PW_STRIDE];
	} PwShared;

#endif
