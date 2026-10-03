/* x25519.c - PsiMail's TLS 1.3 arithmetic. X25519 (web/tls/fe25519.c: 13-bit limbs,
 * about twice Dropbear's speed on the ARM710), and Dropbear's curve25519.c
 * for the Ed25519 signature check of updates */
#include "includes.h"
#include "../../ssh/db/src/curve25519.c"
#include "../../web/tls/fe25519.c"
#include "../../web/tls/aead.c"   /* ChaCha20-Poly1305 for tls13.c (TLS_FAST_CRYPTO) */
#include "../../web/tls/sha256.c" /* SHA-256 for tls13.c (TLS_FAST_CRYPTO) */
