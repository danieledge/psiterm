#ifndef PSI_GRP_H
#define PSI_GRP_H
#include <sys/types.h>
struct group { char *gr_name; char *gr_passwd; int gr_gid; char **gr_mem; };
#endif
