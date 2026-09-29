/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */

/* The implementation is based on:
 * chacha-ref.c version 20080118
 * Public domain from D. J. Bernstein
 */

#include "tomcrypt.h"

#ifdef LTC_CHACHA

#define QUARTERROUND(a,b,c,d) \
  x[a] += x[b]; x[d] = ROL(x[d] ^ x[a], 16); \
  x[c] += x[d]; x[b] = ROL(x[b] ^ x[c], 12); \
  x[a] += x[b]; x[d] = ROL(x[d] ^ x[a],  8); \
  x[c] += x[d]; x[b] = ROL(x[b] ^ x[c],  7);

#ifdef PSI_MULACC
/* Psion 5mx: the state lives in 16 scalars (GCC 3.0 keeps most of them in
 * registers, an array always goes back to memory) and the keystream is
 * produced as 16 little-endian words instead of 64 bytes. */
#define QR(a,b,c,d) \
  a += b; d = ROL(d ^ a, 16); \
  c += d; b = ROL(b ^ c, 12); \
  a += b; d = ROL(d ^ a,  8); \
  c += d; b = ROL(b ^ c,  7);
static void _chacha_block(ulong32 *output, const ulong32 *input, int rounds)
{
   ulong32 x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15;
   int i;
   x0 = input[0]; x1 = input[1]; x2 = input[2]; x3 = input[3];
   x4 = input[4]; x5 = input[5]; x6 = input[6]; x7 = input[7];
   x8 = input[8]; x9 = input[9]; x10 = input[10]; x11 = input[11];
   x12 = input[12]; x13 = input[13]; x14 = input[14]; x15 = input[15];
   for (i = rounds; i > 0; i -= 2) {
      QR(x0, x4, x8, x12)
      QR(x1, x5, x9, x13)
      QR(x2, x6, x10, x14)
      QR(x3, x7, x11, x15)
      QR(x0, x5, x10, x15)
      QR(x1, x6, x11, x12)
      QR(x2, x7, x8, x13)
      QR(x3, x4, x9, x14)
   }
   output[0] = x0 + input[0]; output[1] = x1 + input[1];
   output[2] = x2 + input[2]; output[3] = x3 + input[3];
   output[4] = x4 + input[4]; output[5] = x5 + input[5];
   output[6] = x6 + input[6]; output[7] = x7 + input[7];
   output[8] = x8 + input[8]; output[9] = x9 + input[9];
   output[10] = x10 + input[10]; output[11] = x11 + input[11];
   output[12] = x12 + input[12]; output[13] = x13 + input[13];
   output[14] = x14 + input[14]; output[15] = x15 + input[15];
}
#else
static void _chacha_block(unsigned char *output, const ulong32 *input, int rounds)
{
   ulong32 x[16];
   int i;
   XMEMCPY(x, input, sizeof(x));
   for (i = rounds; i > 0; i -= 2) {
      QUARTERROUND(0, 4, 8,12)
      QUARTERROUND(1, 5, 9,13)
      QUARTERROUND(2, 6,10,14)
      QUARTERROUND(3, 7,11,15)
      QUARTERROUND(0, 5,10,15)
      QUARTERROUND(1, 6,11,12)
      QUARTERROUND(2, 7, 8,13)
      QUARTERROUND(3, 4, 9,14)
   }
   for (i = 0; i < 16; ++i) {
     x[i] += input[i];
     STORE32L(x[i], output + 4 * i);
   }
}
#endif

/**
   Encrypt (or decrypt) bytes of ciphertext (or plaintext) with ChaCha
   @param st      The ChaCha state
   @param in      The plaintext (or ciphertext)
   @param inlen   The length of the input (octets)
   @param out     [out] The ciphertext (or plaintext), length inlen
   @return CRYPT_OK if successful
*/
int chacha_crypt(chacha_state *st, const unsigned char *in, unsigned long inlen, unsigned char *out)
{
#ifdef PSI_MULACC
   ulong32 wbuf[16];
   unsigned char *buf = (unsigned char *)wbuf;   /* little-endian ARM */
#else
   unsigned char buf[64];
#endif
   unsigned long i, j;

   if (inlen == 0) return CRYPT_OK; /* nothing to do */

   LTC_ARGCHK(st        != NULL);
   LTC_ARGCHK(in        != NULL);
   LTC_ARGCHK(out       != NULL);
   LTC_ARGCHK(st->ivlen != 0);

   if (st->ksleft > 0) {
      j = MIN(st->ksleft, inlen);
      for (i = 0; i < j; ++i, st->ksleft--) out[i] = in[i] ^ st->kstream[64 - st->ksleft];
      inlen -= j;
      if (inlen == 0) return CRYPT_OK;
      out += j;
      in  += j;
   }
   for (;;) {
#ifdef PSI_MULACC
     _chacha_block(wbuf, st->input, st->rounds);
#else
     _chacha_block(buf, st->input, st->rounds);
#endif
     if (st->ivlen == 8) {
       /* IV-64bit, increment 64bit counter */
       if (0 == ++st->input[12] && 0 == ++st->input[13]) return CRYPT_OVERFLOW;
     }
     else {
       /* IV-96bit, increment 32bit counter */
       if (0 == ++st->input[12]) return CRYPT_OVERFLOW;
     }
     if (inlen <= 64) {
       for (i = 0; i < inlen; ++i) out[i] = in[i] ^ buf[i];
       st->ksleft = 64 - inlen;
       for (i = inlen; i < 64; ++i) st->kstream[i] = buf[i];
       return CRYPT_OK;
     }
#ifdef PSI_MULACC
     if ((((unsigned long)in | (unsigned long)out) & 3) == 0) {
       /* word-aligned (the usual case for packet buffers): 16 word XORs
        * instead of 64 byte loads/stores. Needs -fno-strict-aliasing. */
       const ulong32 *wi = (const ulong32 *)in;
       ulong32 *wo = (ulong32 *)out;
       for (i = 0; i < 16; ++i) wo[i] = wi[i] ^ wbuf[i];
     } else
#endif
     for (i = 0; i < 64; ++i) out[i] = in[i] ^ buf[i];
     inlen -= 64;
     out += 64;
     in  += 64;
   }
}

#endif

/* ref:         $Format:%D$ */
/* git commit:  $Format:%H$ */
/* commit time: $Format:%ai$ */
