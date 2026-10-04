/* psi_stubs.c - POSIX calls Links (and the updater) link against that
 * ESTLIB lacks. None is used on PsiWeb's paths: no signals, no other
 * programs, no symbolic links, no downloads to a file. Device build only:
 * the emulator harness has its own (emu/links_rt.c). */
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#ifndef ENOSYS
#define ENOSYS 88
#endif

#undef remove
int remove(const char *path) { return unlink(path); }
int lstat(const char *path, struct stat *st) { return stat(path, st); }
int readlink(const char *path, char *buf, size_t n) { (void)path; (void)buf; (void)n; errno = EINVAL; return -1; }
int kill(int pid, int sig) { (void)pid; (void)sig; errno = ENOSYS; return -1; }
typedef void (*psi_sighandler)(int);
psi_sighandler signal(int sig, psi_sighandler h) { (void)sig; (void)h; return (psi_sighandler)0; }
int ftruncate(int fd, off_t len) { (void)fd; (void)len; errno = ENOSYS; return -1; }
int execvp(const char *f, char *const argv[]) { (void)f; (void)argv; errno = ENOSYS; return -1; }
