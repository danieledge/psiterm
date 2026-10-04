/* pm.h - psimail.exe's internal interfaces */
#ifndef PM_H
#define PM_H

#include <stddef.h>
#include <stdio.h>
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
int  pm_replace(const char *tmp, const char *path); /* tmp takes path's place (path may exist), in one step; 0 ok */
long pm_free_kb(const char *path);         /* free space on the drive path is on; -1 unknown */
/* a whole file in one go (RFile::Write on the Psion: one file-server call
   rather than stdio's 512-byte pieces, which the emulator's card does not
   like); path is made afresh. 0 ok */
int  pm_write_whole(const char *path, const void *data, long n);
/* (0.75) work that needs no line and may take seconds - setting out a
   picture - runs below PsiMail.app (1), then back at the engine's working
   priority (0): the app stays responsive and its heartbeat goes on. On the
   Psion the engine otherwise runs above the app (to drain the serial port),
   and a long decode there starved the app of every tick. */
void pm_cpu_low(int low);
/* the engine has just been working (a command, a download ahead): the time
   since the app's last heartbeat starts again (see pmepoc.cpp Housekeeping) */
void pm_beat_reset(void);

/* ---- files (pmmain.c) */
int  pm_fclose(FILE *f);                   /* fclose that also checks the error flag: 0 = all written */
/* "Could not save <what>: no room left on D:" (or "...: is the card there?") */
void pm_write_why(char *why, int whymax, const char *what, const char *path);
void pm_ahead_label(const char *label);    /* "3 of 10": pm_progress says "Ahead (3 of 10): ..."; 0 = not ahead */
#ifdef __EPOC32__
#define PM_SEP "\\"
#else
#define PM_SEP "/"
#endif

/* engine-internal result: a download ahead stepped aside for a command
   from the app (never reaches the app; see pf_step) */
#define PM_RES_PAUSED 100

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
void pmn_close_why(int hangup, const char *why);   /* the same, saying why in the log */
int  pm_update(const char *src, const char *save, char *why, int whymax);
int  pm_version_newer(const char *remote, const char *local);
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
/* out must have room for n + 2 bytes: quoted-printable split across calls
   as "=", "Z", "Z" gives 3 bytes for the last 1 byte of input */
int  dec_feed(PmDecoder *d, const char *in, int n, char *out);

/* message structure from BODYSTRUCTURE */
#define PM_MAX_PARTS 24
typedef struct
	{
	char id[16];             /* "1", "1.2", ... ("TEXT" for a single-part message) */
	char type[24];           /* "text/plain" */
	char charset[24];
	char name[80];           /* attachment file name (cp1252) */
	char cid[80];            /* Content-ID without the <>: what "cid:" in the HTML points at */
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
int  imap_drops(void);                    /* connections lost mid-command so far */
int  imap_list_folders(int acct, char *why, int whymax);
int  imap_sync(int acct, const char *folder, int older, char *why, int whymax);
/* full: bit 0 = past max_body_kb, bit 1 = downloaded ahead (stays unread) */
int  imap_body(int acct, const char *folder, unsigned int uid, int full, char *why, int whymax);
int  imap_attach(int acct, const char *folder, unsigned int uid, const char *part, const char *dir, char *why, int whymax);
int  imap_flag(int acct, const char *folder, unsigned int uid, const char *op, char *why, int whymax);
int  imap_move(int acct, const char *folder, unsigned int uid, const char *dest, char *why, int whymax);
int  imap_search(int acct, const char *folder, const char *words, char *why, int whymax);
int  imap_expunge(int acct, const char *folder, char *why, int whymax);
int  imap_append(int acct, const char *folder, const char *path, const char *flags, char *why, int whymax);
int  imap_special_folder(int acct, char kind, char *out, int max); /* from folders.txt */
/* folders: name is the user's (cp1252); parent/folder are IMAP names. Each
   refreshes folders.txt and leaves the new IMAP name in last_file */
int  imap_create_folder(int acct, const char *parent, const char *name, char *why, int whymax);
int  imap_rename_folder(int acct, const char *folder, const char *name, char *why, int whymax);
int  imap_delete_folder(int acct, const char *folder, char *why, int whymax);
/* a part, decoded from its transfer encoding, into a file (pictures.c uses it) */
int  imap_part_to_file(int acct, const char *folder, unsigned int uid, const char *part, int enc, long size,
                       const char *path, const char *label, char *why, int whymax);

/* Edit > Undo (imap.c): where the last MOVE/COPY put a message (UIDPLUS's
   COPYUID; 0 = the server didn't say), and the move back */
void imap_copyuid_clear(void);
unsigned int imap_copyuid(void);
int  imap_unmove(int acct, const char *dest, unsigned int uid, const char *msgid, const char *folder,
                 unsigned int *newuid, char *why, int whymax);

/* ---- pictures in a message (pictures.c): <uid>.pic lists the image parts,
   <uid>_<part>.pmi is one decoded to 16 greys (img/pmimg.h) */
void pic_write_index(int acct, const char *folder, unsigned int uid, const PmStructure *st);
void pic_remove(int acct, const char *folder, unsigned int uid);   /* all of a message's picture files */
int  pic_fetch(int acct, const char *folder, unsigned int uid, const char *parts, char *why, int whymax);
/* one picture file to its .pmi, below the app (0 shown, 1 not: the .pmi says why, -1 could not write) */
int  pic_decode_file(const char *img_path, const char *pmi_path, const char *label, long budget_left, char *note, int notemax);
/* ---- pictures from the web, when asked for (webpics.c) */
int  web_fetch(int acct, const char *folder, unsigned int uid, char *why, int whymax);
unsigned long web_hash(const char *url);   /* <uid>_W<hash>.pmi: FNV-1a of the address */
int  web_is_spacer(const char *url, int w, int h);

/* ---- SMTP (smtp.c) */
/* sent_mark: a file written just before the final "." goes - the point past
   which a lost answer means the message may have been delivered */
int  smtp_send(int acct, const char *mime_path, const char *from, const char *rcpts, const char *sent_mark,
               char *why, int whymax);
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
int  st_pending_drop(int acct, const char *line);        /* takes one line out again (the server has it now) */
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

/* ---- Edit > Undo (undo.c): the last few moves, and putting one back */
void undo_note_move(int acct, const char *folder, unsigned int uid, const char *arg);  /* before the move */
void undo_note_result(int acct, int r, unsigned int destuid);                          /* after it */
int  undo_run(int acct, const char *folder, unsigned int uid, char *why, int whymax);   /* PM_CMD_UNDO */
int  undo_replay(int acct, const char *dest, unsigned int destuid, const char *arg, char *why, int whymax);

#endif
