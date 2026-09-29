#ifndef PSI_SYS_UIO_H
#define PSI_SYS_UIO_H
#include <stddef.h>
struct iovec { void *iov_base; size_t iov_len; };
int psi_writev(int fd, const struct iovec *iov, int iovcnt);
#define writev psi_writev
#define IOV_MAX 64
#endif
