/* pwback.h - what the "epoc" libnsfb surface and the PsiWeb front end need
 * from the platform: pwepoc.cpp on the Psion, pwhost.c for tests on a PC. */
#ifndef PWBACK_H
#define PWBACK_H

enum { PWB_NONE = 0, PWB_KEYDOWN, PWB_KEYUP, PWB_MOVE, PWB_QUIT, PWB_WAKE };

/* NSFB key codes at or above this carry a Unicode character (code - base):
 * the Psion keyboard already applies Shift/Fn, so text arrives ready-made. */
#define PWB_UNICODE_BASE 0x10000

typedef struct { int type, code, x, y; } pwb_event;

#ifdef __cplusplus
extern "C" {
#endif

int  pwb_open(int *w, int *h);
void pwb_close(void);
void pwb_present(const unsigned short *fb, int fbw, int x0, int y0, int x1, int y1);
int  pwb_next_event(pwb_event *ev, int timeout_ms);   /* 1 = got one */

int  pwb_take_command(char *arg, int max);            /* PW_CMD_* or 0 */
void pwb_set_status(const char *utf8);
void pwb_set_title(const char *utf8);
void pwb_set_url(const char *url);
void pwb_set_busy(int busy);
void pwb_set_nav(int can_back, int can_forward);
void pwb_ready(void);
void pwb_fatal(const char *why);                       /* engine gives up: tell the app */
const char *pwb_home_url(void);
const char *pwb_first_url(void);                       /* Links: the first page (start_url or about:welcome) */
const char *pwb_res_dir(void);
int  pwb_load_images(void);
int  pwb_zoom(void);                                  /* percent */
void *pwb_shared(void);                               /* the PwShared block */
unsigned long pwb_ms(void);                           /* milliseconds, any origin */

/* 565 -> 16 greys, EGray16 layout (low nibble = left pixel, 'stride' bytes/row) */
void pw_grey_convert(const unsigned short *fb, int fbw, unsigned char *out, int stride,
                     int x0, int y0, int x1, int y1);

/* Diagnostic trace to PsiWeb.log (startup, per-frame). OFF by default; a
 * diagnostic build turns it on with PW_DIAG (web/links/epoc.mk:
 * PWEPOC_DEFS="-DPW_DIAG -DPW_LOG_ON_D" to also write the log to D:). */
void pw_log(const char *);
#ifdef PW_DIAG
#define PW_TRACE(msg)      pw_log(msg)
#define PW_TRACE_ONCE(msg) do { static int pw_once_ = 0; \
                                if (!pw_once_) { pw_once_ = 1; pw_log(msg); } } while (0)
#else
#define PW_TRACE(msg)      ((void)0)
#define PW_TRACE_ONCE(msg) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
#endif
