/* pwnet.h - one network connection at a time for PsiWeb's fetcher
 *
 * The Psion reaches the network through the serial port: either a WiFi
 * modem that turns "ATDT host:port" into a single TCP connection, or EPOC's
 * own dial-up (PPP) stack. Either way PsiWeb keeps just one connection, and
 * reuses it for the next request when the server (usually the proxy) keeps
 * it alive. HTTPS uses PsiTerm's TLS 1.3 client (ssh/tls13.c).
 */
#ifndef PWNET_H
#define PWNET_H

#define PWN_TIMEOUT  (-3)     /* pwn_read: nothing arrived in time */
#define PWN_CANCEL   (-2)     /* pwn_read: the app asked us to stop */

int  pwn_connect(const char *host, int port, int tls, char *why, int whymax);
int  pwn_is_open(const char *host, int port, int tls);   /* reusable? */
int  pwn_write(const void *buf, int len);                  /* 0 ok */
int  pwn_read(void *buf, int max, int timeout_ms);         /* >0, 0 closed, <0 */
void pwn_close(int hangup);
int  pwn_modem(void);          /* 1 = WiRSa-style modem (in-band NO CARRIER) */
void pwn_idle_tick(void);      /* hang up and free the port when idle */
void pwn_release_now(void);    /* hang up and free the port now */

#endif
