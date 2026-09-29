/* dbmalloc.h - libtomcrypt's Dropbear config wants m_malloc and friends */
#ifndef PSIMAIL_DBMALLOC_H
#define PSIMAIL_DBMALLOC_H
#include <stdlib.h>
#include <string.h>
#define m_malloc malloc
#define m_calloc calloc
#define m_realloc realloc
#define m_free free
static void m_burn(void *p, unsigned int n) { volatile unsigned char *q = p; while (n--) *q++ = 0; }
/* libtommath's allocator hooks */
static void m_free_ltm(void *p, size_t size) { (void)size; free(p); }
static void *m_realloc_ltm(void *p, size_t old, size_t size) { (void)old; return realloc(p, size); }
#endif
