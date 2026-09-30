/* psimail.h - memory shared between PsiMail.app and psimail.exe
 *
 * PsiMail.app (EIKON: screen, menus, dialogs) creates a global chunk with
 * this layout and starts psimail.exe, the mail engine. The engine does all
 * the network work - IMAP, SMTP, TLS 1.3 - and keeps the local mail store on
 * disk (see engine/store.c for the files). The app reads those files to show
 * folders, message lists and messages, and asks the engine for everything
 * else through a small command queue.
 *
 * 'net' comes first so PsiTerm's network glue (ssh/psiglue.cpp, which
 * expects a PsiShared at the start of the chunk) works unchanged.
 *
 * Every field has one writer, so no locking is needed on the single-CPU
 * Psion: the app writes the settings, the commands and cmd_head; the engine
 * writes everything marked (engine).
 */
#ifndef PSIMAIL_H
#define PSIMAIL_H

#ifndef PSI_SHARED_NAME
#define PSI_SHARED_NAME "PsiMailShared"
#endif
#include "psishared.h"     /* ssh/ - PsiTerm's shared-memory layout */

#define PM_MAGIC         0x4c49414du    /* 'MAIL' */
#define PM_MAX_ACCOUNTS  4
#define PM_CMDQ          16
#define PM_ARG_MAX       256

/* tls: how to talk to a server */
enum { PM_TLS_NONE = 0, PM_TLS_ON = 1, PM_TLS_STARTTLS = 2 };

typedef struct
	{
	int used;
	char name[32];               /* shown in the app, e.g. "Fastmail" */
	char fullname[64];           /* "Dan Edge" */
	char email[96];              /* From: address */
	char imap_host[64];
	int imap_port;               /* 993 */
	int imap_tls;
	char smtp_host[64];
	int smtp_port;               /* 465 */
	int smtp_tls;
	char user[96];               /* usually the full address */
	char pass[64];               /* app password; empty = ask (need_pass) */
	int sync_count;              /* newest N messages kept per folder (50) */
	int max_body_kb;             /* text downloaded per message (64) */
	int save_sent;               /* APPEND a copy to Sent after sending */
	char signature[160];         /* added to new messages ("\n" = new line) */
	} PmAccount;

/* the calendar: CalDAV (Fastmail: caldav.fastmail.com), kept in step with
   the Psion's Agenda by the app (engine/caldav.c has the files) */
typedef struct
	{
	int enabled;
	int acct;                    /* whose user name and password, unless below */
	char host[64];               /* "caldav.fastmail.com" */
	int port;                    /* 443 */
	int plain;                   /* tests only: no TLS */
	char path[128];              /* where to look for calendars: "" = discover */
	char user[96];               /* "" = the account's */
	char pass[64];               /* "" = the account's */
	int zone;                    /* the Psion's time zone (engine/caltz.c) */
	int days_back;               /* events from this many days ago (30) */
	int days_ahead;              /* to this many days ahead (180) */
	} PmCalendar;

/* commands: the app fills in a PmCmd at cmd[cmd_head % PM_CMDQ], then
   increments cmd_head. The engine takes them in order. */
enum
	{
	PM_CMD_NONE = 0,
	PM_CMD_FOLDERS,          /* refresh the folder list (and unread counts) */
	PM_CMD_SYNC,             /* folder: new mail, flag changes, deletions */
	PM_CMD_OLDER,            /* folder: fetch older messages than we have */
	PM_CMD_BODY,             /* folder, uid: download the message text */
	PM_CMD_FULLBODY,         /* folder, uid: the whole text, past max_body_kb */
	PM_CMD_ATTACH,           /* folder, uid, arg = part id: save to attach_dir ("part\tdir": to dir) */
	PM_CMD_FLAG,             /* folder, uid, arg = "+S" "-S" "+F" "-F" (seen / flagged) */
	PM_CMD_MOVE,             /* folder, uid, arg = destination folder ("" = Trash) */
	PM_CMD_SEARCH,           /* folder, arg = words: results in search.idx */
	PM_CMD_SEND,             /* send everything in the outbox */
	PM_CMD_SENDRECV,         /* send the outbox, then sync INBOX and folder */
	PM_CMD_HANGUP,           /* close connections and free the serial port */
	PM_CMD_TRUST,            /* arg = "host:port": accept its certificate (pin) */
	PM_CMD_EXPUNGE,          /* folder: remove messages marked deleted */
	PM_CMD_CALSYNC,          /* calendar: send push.txt, fetch changes (arg "list": calendars only) */
	PM_CMD_UPDATE,           /* arg = "host:port" or "github", folder = where to save PsiMail.sis */
	PM_CMD_QUIT
	};

typedef struct
	{
	int op;
	int acct;
	unsigned int uid;
	char folder[128];        /* IMAP name (modified UTF-7), as in folders.txt */
	char arg[PM_ARG_MAX];
	} PmCmd;

/* engine state */
enum { PM_STATE_STARTING = 0, PM_STATE_READY, PM_STATE_EXITED };

/* result of the last command */
enum
	{
	PM_RES_OK = 0,
	PM_RES_FAILED,           /* last_msg says why */
	PM_RES_OFFLINE,          /* queued: will happen at the next connection */
	PM_RES_CANCELLED,
	PM_RES_UNTRUSTED,        /* certificate not trusted: see trust_* */
	PM_RES_NEED_PASS,        /* the account has no password: ask for one */
	PM_RES_LOGIN_FAILED
	};

typedef struct
	{
	PsiShared net;                  /* MUST stay first - see above */

	unsigned int magic;
	volatile int state;             /* PM_STATE_* (engine) */
	volatile int quitting;          /* app: exit now */
	volatile unsigned int app_beat; /* app: +1 every tick; engine quits if it stops */
	char exit_msg[128];             /* (engine) why it stopped */

	/* settings (app) */
	char store_dir[96];             /* e.g. "D:\\PsiMail\\" (ends with \) */
	char attach_dir[96];            /* where attachments are saved */
	int offline;                    /* 1 = never dial: queue changes */
	int prefetch;                   /* after a sync, download the text of the newest N (0 = off) */
	PmAccount acct[PM_MAX_ACCOUNTS];
	volatile unsigned int acct_seq; /* app bumps after changing acct[] */
	PmCalendar cal;

	/* command queue */
	volatile unsigned int cmd_head; /* app */
	volatile unsigned int cmd_tail; /* engine */
	PmCmd cmd[PM_CMDQ];

	/* progress (engine) */
	volatile int busy;              /* a command is running */
	volatile int online;            /* a connection is open */
	volatile int cur_op;
	char progress[128];             /* "Fetching 12 of 50..." */

	/* results (engine) */
	volatile unsigned int done_seq; /* +1 after each command */
	volatile int last_op;
	volatile int last_acct;
	volatile unsigned int last_uid;
	volatile int last_res;          /* PM_RES_* */
	char last_msg[160];
	char last_folder[128];
	char last_file[128];            /* e.g. the attachment saved */
	volatile unsigned int changed_seq; /* +1 whenever store files change */
	volatile int new_mail;          /* INBOX messages that arrived in the last sync */

	/* PM_RES_UNTRUSTED: what to show the user (engine) */
	char trust_host[80];            /* "imap.example.com:993" */
	char trust_why[96];
	char trust_fp[100];             /* SHA-256 of the key, hex with colons */

	volatile unsigned int heap_used;

	/* Tools > Update PsiMail (the version running is in net.version) */
	volatile int update_ready;      /* 1: last_file is a checked, newer PsiMail.sis */
	char update_version[16];        /* what the server has */
	} PmShared;

#endif
