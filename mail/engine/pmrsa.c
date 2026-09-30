/* pmrsa.c - RSA public-key operation for certcheck.c, with libtommath
 * (kept apart: libtomcrypt's headers and libtommath's don't mix) */
#include <string.h>
#include "tommath.h"

int rsa_public(const unsigned char *n, int nlen, const unsigned char *e, int elen,
                      const unsigned char *sig, int siglen, unsigned char *out)
{
	mp_int mn, me, ms, mm;
	size_t wr;
	int ok = -1;
	if (nlen > 512 || siglen > nlen) return -1;
	if (mp_init_multi(&mn, &me, &ms, &mm, NULL) != MP_OKAY) return -1;
	if (mp_from_ubin(&mn, n, nlen) == MP_OKAY && mp_from_ubin(&me, e, elen) == MP_OKAY &&
	    mp_from_ubin(&ms, sig, siglen) == MP_OKAY && mp_cmp(&ms, &mn) == MP_LT &&
	    mp_exptmod(&ms, &me, &mn, &mm) == MP_OKAY) {
		size_t sz = mp_ubin_size(&mm);
		if ((int)sz <= nlen) {
			memset(out, 0, nlen);
			if (mp_to_ubin(&mm, out + nlen - sz, sz, &wr) == MP_OKAY) ok = 0;
		}
	}
	mp_clear_multi(&mn, &me, &ms, &mm, NULL);
	return ok;
}

