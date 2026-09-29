#define dropbear_curve25519_scalarmult ref_x25519
#define dropbear_ed25519_verify ref_ed_verify
#define dropbear_ed25519_sign ref_ed_sign
#define dropbear_ed25519_make_key ref_ed_make_key
#define PSI_GF32 0
#include "../../db/src/curve25519.c"
