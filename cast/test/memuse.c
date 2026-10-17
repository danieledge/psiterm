/* memuse.c - heap the encoder and decoder take on this host (64-bit
 * pointers: the 32-bit Psion and ESP32 figures are a little smaller; the
 * ARM figure for the encoder comes from arm/armbench.py). */
#include <malloc.h>
#include "tutil.h"

static size_t used(void)
{
	struct mallinfo2 m = mallinfo2();
	return m.uordblks;
}

int main(void)
{
	static unsigned char f[320 * 240], out[65536];
	size_t u0 = used(), u1, u2;
	int len;
	pc_enc* e = pc_enc_new(640, 240, 1);
	pc_dec* d;
	memset(f, 0x5a, sizeof(f));
	pc_enc_frame(e, f, 320, 1, out, sizeof(out), &len);
	u1 = used();
	d = pc_dec_new(640, 240);
	pc_dec_update(d, out, len, 1, 0, 0, 0);
	u2 = used();
	printf("encoder 640x240: %lu bytes\ndecoder 640x240: %lu bytes (after the first update)\n",
	       (unsigned long)(u1 - u0), (unsigned long)(u2 - u1));
	pc_dec_free(d);
	pc_enc_free(e);
	return 0;
}
