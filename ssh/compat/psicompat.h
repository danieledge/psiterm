/* psicompat.h - force-included into every Dropbear/libtom source file when
 * building psissh.exe for the Psion 5mx (EPOC R5, estlib).
 *
 * Dropbear talks to three things through POSIX calls:
 *   fd 0/1/2  - the user's terminal  -> PsiTerm's screen and keyboard
 *   PSI_FD_NET - the network socket   -> the serial link to the WiRSa modem
 *   real files - known_hosts etc.     -> estlib (passed straight through)
 * The system headers are included first so their own prototypes are not
 * renamed by the macros below. */

#ifndef PSICOMPAT_H
#define PSICOMPAT_H

#ifndef PSI_HOST_TEST
#include <_ansi.h>
#endif
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <pwd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSI_FD_NET 50

/* BSD network typedefs used by <netinet/ip.h> (no in_systm.h on EPOC) */
#ifndef PSI_HOST_TEST
typedef unsigned short n_short;
typedef unsigned int n_long;
typedef unsigned int n_time;
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif
typedef long suseconds_t;
struct timespec { long tv_sec; long tv_nsec; };
typedef int socklen_t;
#else
#include <sys/uio.h>
int psi_writev(int fd, const struct iovec *iov, int iovcnt);
#define writev psi_writev
#endif
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
#ifndef AF_MAX
#define AF_MAX 32
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 119
#endif
#ifndef SHUT_RD
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#if !defined(RLIMIT_CORE) && !defined(PSI_HOST_TEST)
#define RLIMIT_CORE 4
struct rlimit { unsigned long rlim_cur, rlim_max; };
#define getrlimit(a,b) (0)
#define setrlimit(a,b) (0)
#endif

typedef void (*psi_sighandler_t)(int);

int psi_read(int fd, void *buf, size_t len);
int psi_write(int fd, const void *buf, size_t len);
int psi_close(int fd);
int psi_shutdown(int fd, int how);
int psi_pipe(int fds[2]);
int psi_mkdir(const char *path, mode_t mode);
int psi_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv);
int psi_fcntl(int fd, int cmd, ...);
int psi_setsockopt(int fd, int level, int opt, const void *val, socklen_t len);
int psi_getsockopt(int fd, int level, int opt, void *val, socklen_t *len);
int psi_getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int psi_getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int psi_ioctl(int fd, unsigned long req, ...);
int psi_isatty(int fd);
char *psi_getpass(const char *prompt);
psi_sighandler_t psi_signal(int sig, psi_sighandler_t h);
int psi_kill(int pid, int sig);
int psi_dup(int fd);
int psi_dup2(int a, int b);
uid_t psi_getuid(void);
int psi_getgroups(int n, gid_t *list);
struct passwd *psi_getpwuid(uid_t uid);
struct passwd *psi_getpwnam(const char *name);

int psi_fprintf(FILE *f, const char *fmt, ...);
int psi_vfprintf(FILE *f, const char *fmt, va_list ap);
int psi_printf(const char *fmt, ...);
int psi_fputs(const char *s, FILE *f);
int psi_fputc(int c, FILE *f);
int psi_fflush(FILE *f);
int psi_getc(FILE *f);
char *psi_fgets(char *buf, int n, FILE *f);
FILE *psi_fopen(const char *path, const char *mode);
int psi_fclose(FILE *f);
int psi_vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int psi_snprintf(char *str, size_t size, const char *fmt, ...);

void psi_add_entropy(void *hs_sha256);   /* hashes PsiTerm-supplied entropy */
void psi_save_seed(const unsigned char *pool, int len);

#ifdef __cplusplus
}
#endif

#define read psi_read
#define write psi_write
#define close psi_close
#define shutdown psi_shutdown
#define pipe psi_pipe
#define mkdir psi_mkdir
#define select psi_select
#define fcntl psi_fcntl
/* Dropbear sets TCP_NODELAY / IP_TOS on its connection as soon as the
   session starts. Our "socket" is fd 50 (the serial link, or the native
   socket in psiglue.cpp) which estlib knows nothing about - passing it to
   estlib's socket calls panicked (USER 22) on the real Psion. */
#define setsockopt psi_setsockopt
#define getsockopt psi_getsockopt
#define getpeername psi_getpeername
#define getsockname psi_getsockname
#define ioctl psi_ioctl
#define isatty psi_isatty
#define getpass psi_getpass
#define signal psi_signal
#define kill psi_kill
#define dup psi_dup
#define dup2 psi_dup2
#define getuid psi_getuid
#define getgroups psi_getgroups
#define geteuid psi_getuid
#define getgid psi_getuid
#define getegid psi_getuid
#define getpwuid psi_getpwuid
#define getpwnam psi_getpwnam

#undef getchar
#undef getc
#undef putchar
#define fprintf psi_fprintf
#define vfprintf psi_vfprintf
#define printf psi_printf
#define fputs psi_fputs
#define fputc psi_fputc
#define putc psi_fputc
#define putchar(c) psi_fputc((c), stdout)
#define fflush psi_fflush
#define getc psi_getc
#define getchar() psi_getc(stdin)
#define fgets psi_fgets
#define fopen psi_fopen
#define fclose psi_fclose
#define vsnprintf psi_vsnprintf
#define snprintf psi_snprintf

#define _PATH_TTY "/dev/tty"
#define _PATH_DEVNULL "/dev/null"

#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGCHLD
#define SIGCHLD 18
#endif
#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif
#ifndef ENOTSUP
#define ENOTSUP 134
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifndef EINTR
#define EINTR 4
#endif

/* libtomcrypt on an ARM710: no unaligned word tricks, no asm */
#ifndef PSI_HOST_TEST
#define LTC_NO_FAST
#define LTC_NO_ASM

/* The ARM710 has no long multiply (UMULL), so every 64-bit product of two
 * 32-bit values goes through libgcc's generic __muldi3 (~90 cycles plus the
 * call). PSI_MULACC(lo, hi, a, b) adds a*b into a 64-bit accumulator held
 * as two 32-bit words using four 32-bit MULs on 16-bit halves. It requires
 * a, b < 2^31 so that the two cross products (each < 2^31) sum without a
 * carry; every caller satisfies this (28-bit bignum digits, 27x29-bit
 * Poly1305 limbs). The (x < y) carries compile to CMP/ADC. Used by
 * libtommath's comba routines and libtomcrypt's Poly1305. */
#define PSI_MULACC(lo, hi, a, b) do { \
   unsigned int a_ = (a), b_ = (b); \
   unsigned int a1_ = a_ >> 16, a0_ = a_ - (a1_ << 16); \
   unsigned int b1_ = b_ >> 16, b0_ = b_ - (b1_ << 16); \
   unsigned int ll_ = a0_ * b0_, mid_ = a1_ * b0_ + a0_ * b1_; \
   unsigned int l_ = ll_ + (mid_ << 16); \
   unsigned int h_ = a1_ * b1_ + (mid_ >> 16) + (l_ < ll_); \
   (lo) += l_; (hi) += h_ + ((lo) < l_); } while (0)
#endif
#define LTC_NO_PROTOTYPES_STRCASECMP

#endif
