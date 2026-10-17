/* cs.h - psicast.exe: the engine's own parts (csmain.c, cskeys.c), and what
 * the platform gives them (csepoc.cpp on the Psion). */
#ifndef CS_H
#define CS_H

#include "psicast.h"

#define CS_W        640
#define CS_H        240
#define CS_STRIDE   (CS_W / 2)          /* the protocol's model: 4 bits a pixel */

/* ---- the platform (csepoc.cpp) ---- */
CsShared *cs_shared(void);
unsigned long cs_ms(void);             /* milliseconds, from the tick (stops while off) */
void cs_log(const char *fmt, ...);
int  cs_app_gone(void);                /* 1: PsiCast's process has ended */
void cs_beat(void);

/* the screen, into aFrame (CS_STRIDE x CS_H, PROTOCOL.md's model, word-
   aligned). 0, 1 if it has not changed at all since the last call (aFrame
   is left as it was), or <0 if it could not be read this time. */
int  cs_capture_init(void);            /* 0, or <0: no way to read the screen */
int  cs_capture(unsigned char *aFrame);
const char *cs_capture_kind(void);
/* a cheap look: have rows aPart, aPart + aParts, ... of the frame buffer
   changed since the last cs_capture? 1 changed (or the depth has, or it
   cannot be read just now: capture to see), 0 not, -1 no frame buffer to
   look at (the window server's copy) */
int  cs_capture_peek(int aPart, int aParts);
int  cs_capture_slow(void);            /* 1: the window server's copy (much dearer) */

/* the viewer's input, already checked (csmain.c) */
void cs_inject_pen(int aKind, int aX, int aY);   /* 1 down, 2 move, 3 up */
void cs_inject_key(int aDown, int aScanCode);

/* power and memory */
void cs_power(int *aPct, int *aMains, int *aLow);   /* 255 / 255 if not known */
unsigned long cs_free_ram(void);
const char *cs_device_name(void);
void cs_digitiser(int *aW, int *aH);   /* the digitiser in pixels (the LCD's size if not known) */

/* ---- cskeys.c: a Latin-1 character as keys on the 5mx (UK) keyboard ---- */
/* Fills aMods (0, or a modifier's scan code) and aScan; 0 if the character
   has no key (it is then dropped). */
int  cs_char_keys(int aChar, int *aMods, int *aScan);

/* ---- csmain.c ---- */
void cs_loop(void);

#endif
