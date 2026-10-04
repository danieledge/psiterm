/* includes.h - stands in for Dropbear's includes.h when PsiMail builds
 * PsiTerm's TLS client (ssh/tls13.c) and Dropbear's X25519
 * (ssh/db/src/curve25519.c) without the rest of Dropbear. The include
 * guards match Dropbear's, so its own includes.h and dbrandom.h are skipped. */
#ifndef DROPBEAR_INCLUDES_H_
#define DROPBEAR_INCLUDES_H_
#define DROPBEAR_RANDOM_H_
#include "options.h"
/* ssh/tls13.c: our X25519 (web/tls/fe25519.c) with keys worked out ahead,
   session resumption, and our ChaCha20-Poly1305 (web/tls/aead.c).
   PsiTerm's psissh builds tls13.c without these. */
#define TLS_FAST_X25519 1
#define TLS_RESUME 1
#define TLS_FAST_CRYPTO 1     /* web/tls/aead.c */
#include <string.h>
#include <stdlib.h>
#include "tomcrypt.h"
void genrandom(unsigned char *buf, unsigned int len);
#endif
