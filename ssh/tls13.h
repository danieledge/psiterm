/* tls13.h - minimal TLS 1.3 client (see tls13.c) */
#ifndef TLS13_H
#define TLS13_H
int tls_connect(const char *host, char *why, int whymax);   /* after pg_dial */
int tls_write(const void *buf, int len);
int tls_read(void *buf, int max, int timeout_ms);           /* >0 bytes, 0 closed, <0 error */
const char *tls_error(void);
int tls_pending(void);                                     /* decrypted bytes waiting */
void tls_close(void);                                      /* after the connection: wipes the keys and buffers */
/* PsiWeb and PsiMail only (TLS_FAST_X25519, TLS_RESUME in their tls/includes.h) */
int tls_background(void);                                  /* some of the next key's work; 1 = more to do */
int tls_resumed(void);                                     /* the last connection was resumed */
void tls_forget(void);                                     /* forget session tickets and the next key */

/* SHA-256 (web/tls/sha256.c, TLS_FAST_CRYPTO) */
typedef struct {
	unsigned int h[8];
	unsigned int len_lo, len_hi;   /* bytes so far */
	int n;                         /* bytes waiting in buf */
	unsigned char buf[64];
} tls_sha256;
void tls_sha256_init(tls_sha256 *s);
void tls_sha256_process(tls_sha256 *s, const unsigned char *m, unsigned long len);
void tls_sha256_done(tls_sha256 *s, unsigned char out[32]);
#endif
