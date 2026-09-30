#ifndef PSIWEB_ENDIAN_H
#define PSIWEB_ENDIAN_H
/* ARM710T on the Psion runs little-endian */
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __ORDER_BIG_ENDIAN__ 4321
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#endif
