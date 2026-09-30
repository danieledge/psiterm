/* includes.h - stands in for Dropbear's includes.h when PsiWeb builds
 * PsiTerm's TLS client (ssh/tls13.c) and Dropbear's X25519
 * (ssh/db/src/curve25519.c) without the rest of Dropbear. The include
 * guards match Dropbear's, so its own includes.h and dbrandom.h are skipped. */
#ifndef DROPBEAR_INCLUDES_H_
#define DROPBEAR_INCLUDES_H_
#define DROPBEAR_RANDOM_H_
#include "options.h"
#include <string.h>
#include <stdlib.h>
#include "tomcrypt.h"
void genrandom(unsigned char *buf, unsigned int len);
#endif
