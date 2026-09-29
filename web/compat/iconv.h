/* Minimal iconv for NetSurf on EPOC: UTF-8, UTF-16LE/BE, UCS-4, ASCII,
 * ISO-8859-1 and Windows-1252. */
#ifndef PSIWEB_ICONV_H
#define PSIWEB_ICONV_H
#include <stddef.h>
typedef void *iconv_t;
iconv_t iconv_open(const char *to, const char *from);
size_t iconv(iconv_t cd, char **in, size_t *inleft, char **out, size_t *outleft);
int iconv_close(iconv_t cd);
#endif
