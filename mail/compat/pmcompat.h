/* pmcompat.h - force-included into the engine's C files on the Psion:
 * what EPOC R5's estlib (gcc 3.0) lacks or keeps elsewhere */
#ifndef PMCOMPAT_H
#define PMCOMPAT_H
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>        /* rename, unlink, and remove() as a macro */
/* web/compat/nsprintf.c: estlib has neither */
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
#endif
