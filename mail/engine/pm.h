/* pm.h - psimail.exe's internal interfaces */
#ifndef PM_H
#define PM_H

#include <stddef.h>
#include "../psimail.h"

/* ---- platform (pmepoc.cpp on the Psion, host/pmhost.c on a PC) */
PmShared *pm_shared(void);
unsigned long pm_ms(void);                 /* milliseconds, wraps */
long pm_time(void);                        /* seconds since 1970, UTC */
int pm_mkdir(const char *path);            /* 0 ok or exists */
int pm_list_dir(const char *dir, const char *suffix,
                void (*cb)(const char *name, void *ctx), void *ctx);
void pm_log(const char *fmt, ...);
void pm_idle(int ms);                      /* sleep, keeping an eye on quit */
void pm_rmtree(const char *dir);           /* delete a folder and all in it */
#ifdef __EPOC32__
#define PM_SEP "\\"
#else
#define PM_SEP "/"
#endif

/* ---- network (pmnet.c): one connection at a time */
#define PMN_TIMEOUT (-3)
#define PMN_CANCEL  (-2)
int  pmn_connect(const char *host, int port, int tls, char *why, int whymax);
int  pmn_starttls(const char *host, char *why, int whymax);
int  pmn_is_open(void);
int  pmn_conn_id(void);
int  pmn_write(const void *buf, int len);          /* 0 ok */
int  pmn_printf(const char *fmt, ...);             /* 0 ok */
int  pmn_read(void *buf, int max, int timeout_ms); /* >0, 0 closed, <0 */
int  pmn_getc(int timeout_ms);                     /* byte, or <0 */
int  pmn_readline(char *buf, int max, int timeout_ms); /* length without CRLF, <0 error */
void pmn_close(int hangup);
const char *pmn_error(void);      /* why the last read failed, if known */
void pmn_idle_tick(void);
void pmn_release_now(void);

/* ---- text (charset.c) - the Psion's own character set is Windows-1252 */
int  cs_is_utf8(const char *charset);
/* converts n bytes in 'charset' to cp1252, appending to out (max incl. NUL);
   returns bytes written */
int  cs_to_cp1252(const char *charset, const char *in, int n, char *out, int max);
int  cs_utf8_to_cp1252(const char *in, int n, char *out, int max);
int  cs_cp1252_to_utf8(const char *in, int n, char *out, int max);
unsigned int cs_cp1252_to_ucs(unsigned char c);
int  cs_ucs_to_cp1252(unsigned int u);             /* -1 if none */
/* RFC 2047 encoded-words in a header -> cp1252 */
void cs_decode_header(const char *in, char *out, int max);
/* header text (cp1252) -> RFC 2047 if needed, for sending */
void cs_encode_header(const char *in, char *out, int max);
/* IMAP modified UTF-7 folder name -> cp1252 for display */
void cs_mutf7_decode(const char *in, char *out, int max);
/* cp1252 -> modified UTF-7 */
void cs_mutf7_encode(const char *in, char *out, int max);

/* ---- imap parser (imapparse.c) */
enum { IT_ATOM = 1, IT_STRING, IT_NIL, IT_LIST };
typedef struct ImapNode
	{
	int type;
	int len;
	const char *s;           /* ATOM / STRING (not NUL-terminated) */
	struct ImapNode *child;  /* LIST */
	struct ImapNode *next;
	} ImapNode;
/* parses buf[0..len) (one response, literals inline as {n}CRLF<bytes>) */
ImapNode *ip_parse(const char *buf, int len);
int  ip_eq(const ImapNode *n, const char *s);           /* case-insensitive atom/string compare */
void ip_str(const ImapNode *n, char *out, int max);     /* copy (NIL -> "") */
long ip_num(const ImapNode *n);
ImapNode *ip_nth(ImapNode *list, int i);                 /* i-th child */
ImapNode *ip_get(ImapNode *list, const char *key);       /* value after key in a flat list */

/* ---- streaming decoders (mime.c) */
typedef struct
	{
	int enc;                 /* 0 none, 1 base64, 2 quoted-printable */
	unsigned int acc;
	int bits;
	char pend[4];            /* QP: a split "=XX" */
	int npend;
	} PmDecoder;
enum { ENC_7BIT = 0, ENC_BASE64 = 1, ENC_QP = 2 };
int  mime_enc_from_name(const char *name);
void dec_init(PmDecoder *d, int enc);
int  dec_feed(PmDecoder *d, const char *in, int n, char *out); /* out >= n */

/* message structure from BODYSTRUCTURE */
#define PM_MAX_PARTS 24
typedef struct
	{
	char id[16];             /* "1", "1.2", ... ("TEXT" for a single-part message) */
	char type[24];           /* "text/plain" */
	char charset[24];
	char name[80];           /* attachment file name (cp1252) */
	int enc;
	long size;
	int attachment;          /* 1 = offer as an attachment */
	int flowed, delsp;       /* text/plain; format=flowed (RFC 3676) */
	} PmPart;
typedef struct
	{
	int n;
	PmPart part[PM_MAX_PARTS];
	int text;                /* index of the part to show, or -1 */
	int html;                /* 1 if that part is text/html */
	int nattach;
	} PmStructure;

void mime_structure(ImapNode *body, PmStructure *st);

/* ---- html.c: HTML -> plain text, streamed */
typedef struct HtmlConv HtmlConv;
HtmlConv *html_new(void);
/* feeds cp1252 HTML; writes plain text via the callback */
void html_feed(HtmlConv *h, const char *in, int n, void (*out)(const char *s, int n, void *ctx), void *ctx);
void html_end(HtmlConv *h, void (*out)(const char *s, int n, void *ctx), void *ctx);
void html_free(HtmlConv *h);

/* ---- IMAP client (imap.c) */
typedef struct
	{
	unsigned int uid;
	char flags[12];
	long date;
	long size;
	char from[80];
	char to[80];
	char subject[160];
	char msgid[100];
	char inreplyto[100];
	int attach;
	} PmMsg;

int  imap_open(int acct, char *why, int whymax);        /* connect + login (reuses) */
void imap_logout(void);
int  imap_list_folders(int acct, char *why, int whymax);
int  imap_sync(int acct, const char *folder, int older, char *why, int whymax);
int  imap_body(int acct, const char *folder, unsigned int uid, int full, char *why, int whymax);
int  imap_attach(int acct, const char *folder, unsigned int uid, const char *part, char *why, int whymax);
int  imap_flag(int acct, const char *folder, unsigned int uid, const char *op, char *why, int whymax);
int  imap_move(int acct, const char *folder, unsigned int uid, const char *dest, char *why, int whymax);
int  imap_search(int acct, const char *folder, const char *words, char *why, int whymax);
int  imap_expunge(int acct, const char *folder, char *why, int whymax);
int  imap_append(int acct, const char *folder, const char *path, const char *flags, char *why, int whymax);
int  imap_special_folder(int acct, char kind, char *out, int max); /* from folders.txt */

/* ---- SMTP (smtp.c) */
int  smtp_send(int acct, const char *mime_path, const char *from, const char *rcpts, char *why, int whymax);
void genrandom(unsigned char *buf, unsigned int len);

/* ---- composing (compose.c): outbox file -> MIME file */
int  compose_mime(int acct, const char *outbox_path, const char *mime_path,
                  char *from_addr, int famax, char *rcpts, int rmax, char *why, int whymax);

/* ---- local store (store.c) */
void st_acct_dir(int acct, char *out, int max);
void st_folder_dir(int acct, const char *folder, char *out, int max);
int  st_check_account(int acct);                         /* wipes a stale account's files */
typedef struct
	{
	unsigned long uidvalidity;
	unsigned long uidnext;
	long exists;
	int n;
	int cap;
	PmMsg *m;                /* sorted by uid */
	} PmIndex;
int  st_index_load(int acct, const char *folder, const char *file, PmIndex *ix);
int  st_index_save(int acct, const char *folder, const char *file, PmIndex *ix);
void st_index_free(PmIndex *ix);
PmMsg *st_index_find(PmIndex *ix, unsigned int uid);
PmMsg *st_index_add(PmIndex *ix, unsigned int uid);      /* keeps order */
void st_index_remove(PmIndex *ix, unsigned int uid);
void st_flag_set(char *flags, char f, int on);
int  st_flag_has(const char *flags, char f);
int  st_pending_add(int acct, const char *line);
int  st_pending_replay(int acct, char *why, int whymax);
void st_msg_path(int acct, const char *folder, unsigned int uid, const char *ext, char *out, int max);
void st_changed(void);
int  st_pin_check(const char *hostport, const char *fp); /* 1 pinned, 0 none, -1 different */
int  st_pin_save(const char *hostport, const char *fp);

/* ---- helpers (pmmain.c) */
void pm_progress(const char *fmt, ...);
int  pm_cancelled(void);
void pm_copy(char *dst, const char *src, int max);
int  pm_strcasecmp(const char *a, const char *b);
int  pm_strncasecmp(const char *a, const char *b, int n);
const char *pm_stristr(const char *hay, const char *needle);

/* ---- certificate checks (certcheck.c) */
void tlsv_set_host(const char *host, int port);
const char *tlsv_fingerprint(void);
const char *tlsv_problem(void);            /* why the certificate was not trusted */

#endif
