/* pccodec.h - the PsiCast screen codec (see cast/docs/PROTOCOL.md, CODEC.md).
 *
 * Plain C (C89-friendly: built by gcc 3.0 for EPOC, by the ESP32 toolchain
 * for the Atom, and by the host compiler for tests). No malloc in the hot
 * path, no 64-bit arithmetic, no floating point.
 *
 * The encoder runs on the Psion: give it each new captured frame and it
 * works out what changed since the last frame it encoded (tiles, scrolling)
 * and writes one UPDATE body (the deflated op list).
 *
 * The decoder runs on the Atom (and in host tests): it applies UPDATE
 * bodies to its copy of the screen and reports the area that changed.
 *
 * Build: pccodec.c holds both halves. Define PC_NO_ENCODER (the Atom) or
 * PC_NO_DECODER (the Psion) to leave one out, so only zlib's inflate or
 * deflate side is needed. The encoder reads frames a word at a time:
 * compile it with -fno-strict-aliasing (the EPOC builds already do).
 */
#ifndef PCCODEC_H
#define PCCODEC_H

#ifdef __cplusplus
extern "C" {
#endif

#define PC_MAX_W 640
#define PC_MAX_H 480

/* results */
#define PC_OK          0
#define PC_ERR_ARG    -1   /* bad size or argument */
#define PC_ERR_NOMEM  -2
#define PC_ERR_DATA   -3   /* corrupt or truncated update: ask for a keyframe */
#define PC_ERR_SPACE  -4   /* output buffer too small */

/* The smallest aOutMax pc_enc_frame accepts. With at least this much room
 * the encoder never fails for lack of space: it sends what fits and keeps
 * the rest for the next update (see pc_enc_pending). */
#define PC_ENC_MIN_OUT 1024

/* A changed area reported to the caller, in pixels (x and w even). */
typedef struct {
	unsigned short x, y, w, h;
} pc_rect;

/* A vertical move the decoder applied: rows [y0, y1) moved by dy.
 * x and w (pixels, even) are the columns that moved. Unless the decoder
 * was told PC_DEC_PARTIAL_SCROLL, only full-width moves (x = 0, w = the
 * screen width) are reported as moves; others come as a pc_rect of the
 * destination area instead. */
typedef struct {
	unsigned short y0, y1;
	short dy;
	unsigned short x, w;
} pc_scroll;

/* What the encoder did in its last pc_enc_frame (for STATUS and tests). */
typedef struct {
	int moves, fills;               /* ops written */
	int raws, rles;                 /* pixel ops, plain and RLE-coded */
	int in_bytes;                   /* the op list before deflate */
	int out_bytes;                  /* the UPDATE body */
	int pending;                    /* changes left for the next update */
	int pix1, pix2, pix4;           /* pixel ops by bits a pixel */
} pc_stats;

/* ---- encoder (Psion) ---- */

typedef struct pc_enc pc_enc;

/* Allocates everything the encoder needs (previous frame, deflate state,
 * scratch). aLevel is the deflate level (1..6). aWidth must be a multiple
 * of 8, up to PC_MAX_W; aHeight up to PC_MAX_H. */
pc_enc* pc_enc_new(int aWidth, int aHeight, int aLevel);
void    pc_enc_free(pc_enc* aEnc);

/* Encode the change from the decoder's screen (as the encoder last left
 * it) to aFrame (packed 4bpp, aStride bytes a row, the model in
 * PROTOCOL.md; aFrame and aStride must be multiples of 4).
 * aKeyframe != 0 resets the deflate stream and the screen (to white, 15)
 * on both sides, then paints aFrame.
 * Writes the UPDATE body (without seq) to aOut; *aOutLen gets its length.
 * Returns PC_OK, or 1 if nothing changed (no update needed, *aOutLen = 0;
 * a keyframe is always written), or an error:
 * - PC_ERR_SPACE if aOutMax < PC_ENC_MIN_OUT (nothing is consumed);
 * - PC_ERR_ARG if the stream needs a keyframe (after an internal deflate
 *   failure, which should never happen) and aKeyframe is 0.
 * If the change does not fit in aOutMax, the update carries part of it
 * and pc_enc_pending() is non-zero: encode again soon (the same frame is
 * fine) to send the rest. */
int pc_enc_frame(pc_enc* aEnc, const unsigned char* aFrame, int aStride,
                 int aKeyframe, unsigned char* aOut, int aOutMax, int* aOutLen);

/* Quick check, cheaper than pc_enc_frame: does aFrame differ from the last
 * encoded frame at all? (Used to decide whether to encode now or wait.)
 * Also true while part of a change is still pending. */
int pc_enc_changed(pc_enc* aEnc, const unsigned char* aFrame, int aStride);

/* Non-zero if the last update could not carry the whole change. */
int pc_enc_pending(const pc_enc* aEnc);

/* What the last pc_enc_frame did. */
void pc_enc_stats(const pc_enc* aEnc, pc_stats* aStats);

/* ---- decoder (Atom, tests) ---- */

typedef struct pc_dec pc_dec;

/* aWidth even, up to PC_MAX_W x PC_MAX_H. The screen starts white. */
pc_dec* pc_dec_new(int aWidth, int aHeight);
void    pc_dec_free(pc_dec* aDec);

/* Flags for pc_dec_set_flags. */
#define PC_DEC_PARTIAL_SCROLL 1   /* report moves of part of the width too */

void pc_dec_set_flags(pc_dec* aDec, int aFlags);

/* Apply one UPDATE body. aKeyframe as in the UPDATE's flags.
 * aOnScroll / aOnRect (may be NULL) are called, in order, as each move and
 * each changed rectangle is applied, so a caller can mark per-viewer dirty
 * state. A keyframe reports one rectangle, the whole screen, at the end.
 * On PC_ERR_DATA the screen may be partly updated (and some callbacks may
 * already have been made): the caller must request a keyframe. Until that
 * keyframe arrives, every other update fails with PC_ERR_DATA too. */
int pc_dec_update(pc_dec* aDec, const unsigned char* aBody, int aLen,
                  int aKeyframe,
                  void (*aOnScroll)(void* aCtx, const pc_scroll* aScroll),
                  void (*aOnRect)(void* aCtx, const pc_rect* aRect),
                  void* aCtx);

/* The decoder's current screen (packed 4bpp, PROTOCOL.md model). */
const unsigned char* pc_dec_screen(const pc_dec* aDec);
int pc_dec_stride(const pc_dec* aDec);

#ifdef __cplusplus
}
#endif

#endif
