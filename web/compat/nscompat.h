/* nscompat.h - force-included into NetSurf sources for EPOC R5 (estlib, gcc 3.0) */
#ifndef NSCOMPAT_H
#define NSCOMPAT_H
#define restrict __restrict__
#define WITHOUT_ICONV_FILTER 1
#include <_ansi.h>
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif
ssize_t nsc_pread(int fd, void *buf, size_t n, off_t off);
ssize_t nsc_pwrite(int fd, const void *buf, size_t n, off_t off);
#define pread nsc_pread
#define pwrite nsc_pwrite
#ifndef NULL
#define NULL ((void*)0)
#endif
#ifndef PATH_MAX
#define PATH_MAX 256
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#endif
#ifndef EILSEQ
#define EILSEQ 84
#endif
#define SCNx32 "x"
#define SCNu32 "u"
#define SCNd32 "d"
#include <stdarg.h>
#ifndef va_copy
#define va_copy(d, s) ((d) = (s))   /* APCS: va_list is a plain pointer */
#endif
float ceilf(float);
float strtof(const char *, char **);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
#ifndef E2BIG
#define E2BIG 7
#endif

/* estlib would open a text console over the app for anything written to
 * stdout/stderr: send it to psiweb.log (next to the app) instead. */
#include <stdio.h>
int nsc_fprintf(FILE *f, const char *fmt, ...);
int nsc_vfprintf(FILE *f, const char *fmt, va_list ap);
int nsc_printf(const char *fmt, ...);
int nsc_fputs(const char *s, FILE *f);
int nsc_fputc(int c, FILE *f);
int nsc_puts(const char *s);
size_t nsc_fwrite(const void *p, size_t sz, size_t n, FILE *f);
void nsc_perror(const char *s);
int nsc_fflush(FILE *f);
int nsc_setvbuf(FILE *f, char *buf, int mode, size_t size);
#ifndef NSCOMPAT_IMPL
#define fprintf nsc_fprintf
#define vfprintf nsc_vfprintf
#define printf nsc_printf
#define fputs nsc_fputs
#undef fputc
#define fputc nsc_fputc
#undef putc
#define putc nsc_fputc
#undef putchar
#define putchar(c) nsc_fputc((c), stdout)
#define puts nsc_puts
#define fwrite nsc_fwrite
#define perror nsc_perror
#define fflush nsc_fflush
#define setbuf(f, b) nsc_setvbuf((f), (b), 0, 0)
#define setvbuf nsc_setvbuf
#endif
#endif
