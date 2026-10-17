/* pctune.h - encoder tuning knobs, for the host benchmark only.
 *
 * The defaults in pccodec.c are the measured best (cast/docs/CODEC.md);
 * the Psion never calls this.
 */
#ifndef PCTUNE_H
#define PCTUNE_H

#include "pccodec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PC_TUNE_SCROLL  1   /* 0 = no move detection, else the most moves an update */
#define PC_TUNE_RLE     2   /* 0 = never RLE, 1 = RLE when smaller than raw */
#define PC_TUNE_GAP     3   /* clean tiles allowed inside a rect's run */
#define PC_TUNE_TH      4   /* tile height in rows (1..32) */
#define PC_TUNE_STRATEGY 5  /* zlib strategy (Z_DEFAULT_STRATEGY, Z_RLE...) */
#define PC_TUNE_MEMLEVEL 6  /* zlib memLevel (the decoder does not care) */
/* (STRATEGY and MEMLEVEL restart deflate: the next update must be a keyframe) */

void pc_enc_tune(pc_enc* aEnc, int aKnob, int aValue);

#ifdef __cplusplus
}
#endif

#endif
