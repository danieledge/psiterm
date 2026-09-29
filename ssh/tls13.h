/* tls13.h - minimal TLS 1.3 client (see tls13.c) */
#ifndef TLS13_H
#define TLS13_H
int tls_connect(const char *host, char *why, int whymax);   /* after pg_dial */
int tls_write(const void *buf, int len);
int tls_read(void *buf, int max, int timeout_ms);           /* >0 bytes, 0 closed, <0 error */
const char *tls_error(void);
int tls_pending(void);                                     /* decrypted bytes waiting */
#endif
