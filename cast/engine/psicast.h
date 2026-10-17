/* psicast.h - memory shared between PsiCast.app and psicast.exe
 *
 * PsiCast.app shows what is happening and owns the settings; psicast.exe,
 * the engine, captures the screen, encodes it (cast/codec), sends it to the
 * Atom (link A, cast/docs/PROTOCOL.md) and injects the viewer's taps and
 * keys. The engine runs only while the screen is shared, and can go on
 * after the app has closed (Preferences > When PsiCast closes).
 *
 * The app creates a global chunk with this layout and starts the engine.
 * 'net' comes first so PsiTerm's network glue (ssh/psiglue.cpp, which
 * expects a PsiShared at the start of the chunk) works unchanged. The
 * engine keeps its own handle on the chunk, so the chunk outlives the app:
 * a PsiCast opened again finds the engine still sharing and takes it up.
 *
 * Every field has one writer, marked "app" or "engine". On the single-CPU
 * Psion no locking is needed. The app also writes a byte into net.kbd (and
 * rings net.eng_bell) whenever it changes anything here, so the engine's
 * wait (pg_wait, which returns on keys) ends at once: a pause takes effect
 * before the next picture goes.
 */
#ifndef PSICAST_H
#define PSICAST_H

#ifndef PSI_SHARED_NAME
#define PSI_SHARED_NAME "PsiCastShared"
#endif
#include "psishared.h"     /* ssh/ - PsiTerm's shared-memory layout */

#define CS_MAGIC        0x54534143u     /* 'CAST' */
#define CS_LAYOUT       1               /* + 1 whenever this struct changes */

/* where the engine is up to (phase, engine) */
enum
	{
	CS_PHASE_STARTING = 0,
	CS_PHASE_CONNECTING,        /* dialling or connecting to the Atom */
	CS_PHASE_WAITING,           /* not connected: trying again soon (msg says why) */
	CS_PHASE_SHARING,           /* connected: the Atom has the screen */
	CS_PHASE_STOPPED            /* the engine has finished (stop_reason) */
	};

/* why the engine stopped by itself (stop_reason, engine) */
enum
	{
	CS_STOP_NONE = 0,           /* asked to (Stop sharing, or PsiCast closed) */
	CS_STOP_BATTERY,            /* the battery is low (Preferences) */
	CS_STOP_APP_GONE,           /* PsiCast ended without asking (not detached) */
	CS_STOP_CAPTURE,            /* the screen could not be read at all */
	CS_STOP_MEMORY              /* not enough memory for the encoder */
	};

typedef struct
	{
	PsiShared net;                     /* MUST stay first - see above */

	unsigned int magic;                /* app: CS_MAGIC */
	int layout;                        /* app: CS_LAYOUT (a new app finds an old engine) */

	/* ---- app ---- */
	volatile int quitting;             /* the engine should say BYE and finish now */
	volatile int detached;             /* 1: PsiCast has closed, keep sharing */
	volatile unsigned int app_pid;     /* PsiCast's process id (watched unless detached) */
	volatile int max_rate;             /* highest updates a second: 1..20 */
	volatile int allow_control;        /* 1: inject the viewer's taps and keys */
	volatile int paused;               /* 1: send nothing new (viewers keep the last picture) */
	volatile int battery_stop;         /* 1: stop sharing when the battery is low */
	volatile unsigned int app_seq;     /* + 1 when anything above changes */

	/* ---- engine ---- */
	volatile int state;                /* PSI_STATE_EXITED when it has ended */
	volatile unsigned int eng_pid;     /* psicast.exe's process id */
	volatile int phase;                /* CS_PHASE_* */
	volatile int stop_reason;          /* CS_STOP_*, with phase CS_PHASE_STOPPED */
	volatile int viewers;              /* -1 not known yet, else the Atom's count */
	volatile int viewers_control;      /* ...of which may tap and type */
	volatile unsigned int bytes_sec;   /* data sent, bytes a second (the last 5 s) */
	volatile unsigned int ups_x10;     /* updates a second x 10 */
	volatile unsigned int cpu_x10;     /* the engine's share of the CPU, % x 10 */
	volatile unsigned int updates;     /* sent this connection */
	volatile unsigned int inputs;      /* viewer input events injected */
	volatile unsigned int eng_beat;    /* + 1 each time round the loop (at least every 5 s) */
	char viewer_url[64];               /* the address to open on the phone, if the Atom says */
	char msg[96];                      /* what is happening, or why not connected */
	char capture[40];                  /* how the screen is read, e.g. "frame buffer, 16 greys" */
	volatile unsigned int eng_seq;     /* + 1 when anything above (but beat) changes */
	} CsShared;

#endif
