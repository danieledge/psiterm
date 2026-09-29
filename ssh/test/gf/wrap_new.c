#define dropbear_curve25519_scalarmult new_x25519
#define dropbear_ed25519_verify new_ed_verify
#define dropbear_ed25519_sign new_ed_sign
#define dropbear_ed25519_make_key new_ed_make_key
#define PSI_GF32 1
#include "../../db/src/curve25519.c"
