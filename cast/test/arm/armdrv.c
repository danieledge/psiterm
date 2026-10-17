/* armdrv.c - the functions armbench.py calls in the ARM build. */
#include "pccodec.h"
#include "pctune.h"

unsigned int drv_fb[640 * 480 / 8];	/* the frame, written by the harness */
unsigned char drv_out[65536];
int drv_len, drv_pending;
pc_stats drv_stats;
static pc_enc* enc;

int drv_init(int w, int h, int level, int moves)
{
	enc = pc_enc_new(w, h, level);
	if (!enc)
		return -1;
	pc_enc_tune(enc, PC_TUNE_SCROLL, moves);
	return 0;
}

void drv_tune(int knob, int value)
{
	pc_enc_tune(enc, knob, value);
}

int drv_frame(int key, int stride)
{
	int r = pc_enc_frame(enc, (const unsigned char*)drv_fb, stride, key, drv_out, 65532, &drv_len);
	drv_pending = pc_enc_pending(enc);
	pc_enc_stats(enc, &drv_stats);
	return r;
}

int drv_changed(int stride)
{
	return pc_enc_changed(enc, (const unsigned char*)drv_fb, stride);
}
