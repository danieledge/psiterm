/* psicompat.h - force-included into every Links source for EPOC R5
 * (ESTLIB, gcc 3.0). Fills the gaps between ESTLIB's headers and what
 * Links expects of a POSIX system. The functions declared here are in
 * psi_os.c (or web/compat/nsprintf.c for snprintf).
 */
#ifndef PSICOMPAT_H
#define PSICOMPAT_H

/* Links' cfg.h includes "config.h" from its own folder, where the PC
 * build's configure output may be: so ours comes in from here instead */
#include "config.h"

#include <_ansi.h>
#include <sys/types.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif
typedef int socklen_t;

int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
#ifndef va_copy
#define va_copy(d, s) ((d) = (s))	/* APCS: va_list is a plain pointer */
#endif

#ifndef EINTR
#define EINTR 4
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 115
#endif
#ifndef EALREADY
#define EALREADY 114
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef ECONNRESET
#define ECONNRESET 104
#endif
#ifndef ENOTSUP
#define ENOTSUP 95
#endif
#ifndef EILSEQ
#define EILSEQ 84
#endif

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

/* psi_os.c: sockets and pipes are pseudo-descriptors there (one network
 * connection, over web/engine/pwnet.c); files go on to ESTLIB */
#include <unistd.h>
#include <fcntl.h>
int psi_select(int n, void *r, void *w, void *e, void *tv);
int psi_read(int fd, void *buf, size_t n);
int psi_write(int fd, const void *buf, size_t n);
int psi_close(int fd);
int psi_fcntl(int fd, int cmd, ...);
int psi_pipe(int fd[2]);
#define select psi_select
#define read(f, b, n) psi_read(f, b, n)
#define write(f, b, n) psi_write(f, b, n)
#define close(f) psi_close(f)
#define fcntl psi_fcntl
#define pipe(f) psi_pipe(f)
#define NO_SIGNAL_HANDLERS
#endif
