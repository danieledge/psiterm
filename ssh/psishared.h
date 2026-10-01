/* psishared.h - memory shared between PsiTerm.app and psissh.exe
 *
 * PsiTerm creates a global chunk with this layout, fills in the connection
 * details and launches psissh.exe. Each ring buffer has exactly one writer
 * and one reader, so no locking is needed on the single-CPU Psion.
 *
 *   kbd ring : PsiTerm writes key bytes,          psissh reads
 *   out ring : psissh writes terminal output,     PsiTerm reads (-> libvterm)
 */
#ifndef PSISHARED_H
#define PSISHARED_H

#ifndef PSI_SHARED_NAME
#define PSI_SHARED_NAME   "PsiTermSSH"
#endif
#define PSI_SHARED_MAGIC  0x48535350u      /* 'PSSH' */
#define PSI_KBD_SIZE      2048
#define PSI_OUT_SIZE      16384
#define PSI_ENTROPY_SIZE  512
#define PSI_XFER_LIST_SIZE 16384

enum
	{
	PSI_STATE_STARTING = 0,
	PSI_STATE_DIALING,
	PSI_STATE_KEYEX,
	PSI_STATE_CONNECTED,
	PSI_STATE_EXITED,
	PSI_STATE_AUTH                 /* logging in (0.52) */
	};

typedef struct
	{
	unsigned int magic;
	volatile int rows;
	volatile int cols;
	volatile int resized;          /* PsiTerm sets, psissh clears */
	volatile int state;            /* PSI_STATE_* */
	volatile int exit_code;
	volatile int quit;             /* PsiTerm asks psissh to stop */
	int baud_index;                /* 0=9600 .. 4=115200 */
	int rtscts;
	int port;
	char host[128];
	char user[64];
	char dial_prefix[16];          /* normally "ATDT" */
	char home[96];                 /* folder for known_hosts / seed */

	volatile unsigned int kbd_head;   /* written by PsiTerm */
	volatile unsigned int kbd_tail;   /* written by psissh */
	unsigned char kbd[PSI_KBD_SIZE];

	volatile unsigned int out_head;   /* written by psissh */
	volatile unsigned int out_tail;   /* written by PsiTerm */
	unsigned char out[PSI_OUT_SIZE];

	volatile int entropy_len;
	unsigned char entropy[PSI_ENTROPY_SIZE];   /* key timing samples etc. */
	int mode;                                  /* 0 = SSH session, 1 = speed test */
	char password[64];                         /* saved password, or empty; psissh wipes it */
	int net_mode;                              /* 0 = dial through the WiRSa (raw TCP pipe),
	                                              1 = Psion TCP/IP (dial-up PPP, e.g. WiRSa ATDT PPP) */
	/* mode 2 (update): fetch http://host:port/<path>version.txt, and if it is
	   newer than 'version' download <path>PsiTerm.sis to 'save_as' */
	char path[64];
	char version[16];
	char save_as[64];
	/* auto-reconnect (0.22) */
	volatile int lost_link;        /* psissh sets: 1 = was logged in and the link
	                                  dropped, 2 = could not dial/connect */
	char command[128];             /* optional remote command, e.g. tmux new -A -s psion */
	int tls;                       /* update over HTTPS (TLS 1.3), e.g. from GitHub (0.23) */
	int use_key;                   /* offer the SSH login key to this host (0.48) */
	/* SSH keys (0.50): mode 0 offers <keyfile>.key if set; mode 4 makes a
	   new key and mode 5 imports <keysrc>, both saving <keyfile>.key/.pub/.fp */
	char keyfile[96];
	char keysrc[96];
	char keyname[32];              /* the comment on the .pub line */
	/* Psion Internet (0.61): sent to the modem on the serial port before the
	   TCP/IP connection starts, e.g. ATDT777 to put a WiRSa into PPP.
	   Empty = send nothing (the Psion's own dial-up does the dialling). */
	char ppp_start[48];
	/* (0.65) what the link is doing right now - "Sending ATDT777",
	   "Looking up 68k.news"... - for apps that show it (PsiWeb, PsiMail) */
	char link_msg[80];
	volatile unsigned int link_seq;    /* +1 each time link_msg changes */
	/* (0.68) the app adds 1 when the Psion is switched back on
	   (HandleSwitchOnEventL): the engine then checks the link is still
	   there (PPP up? modem carrier?) instead of trusting a dead socket */
	volatile unsigned int switch_on;
	/* (0.69) PsiTerm adds 1 every pump tick (1/64 s) while psissh runs. If
	   it stops for 45 s the app has gone (crashed, or killed from the task
	   list) and psissh quits instead of holding the serial port and the
	   chunk for ever. 0 = never beat: PsiMail and PsiWeb have their own. */
	volatile unsigned int app_beat;
	/* (0.74) file transfer (SFTP) on a second channel of the SSH session
	   (sftp.c). PsiTerm fills in the request, then adds 1 to xfer_req;
	   psissh does it and sets xfer_ack = xfer_req when it has finished,
	   with xfer_result (PSI_XFER_*) and, for a server refusal, its words
	   in xfer_msg. Only one request at a time. */
	volatile unsigned int xfer_req;    /* written by PsiTerm */
	volatile unsigned int xfer_ack;    /* written by psissh */
	int xfer_op;                       /* PSI_XOP_* */
	char xfer_local[256];              /* the Psion file (EPOC path) */
	char xfer_remote[512];             /* the server's file or folder; "" = home */
	volatile int xfer_cancel;          /* PsiTerm sets 1: Stop */
	volatile int xfer_result;          /* PSI_XFER_* */
	volatile unsigned int xfer_done;   /* bytes so far */
	volatile unsigned int xfer_total;  /* bytes in all (0 = not known yet) */
	volatile int xfer_exists;          /* STAT: 1 file, 2 folder, 0 not there */
	char xfer_msg[96];
	char xfer_path[512];               /* LIST: the folder's full name */
	volatile int xfer_list_len;        /* LIST: bytes in xfer_list */
	volatile int xfer_list_more;       /* LIST: 1 = it did not all fit */
	/* LIST: one line per entry: 'd' (folder), 'f' (file) or 'l' (other),
	   the size in decimal, a tab, the name, '\n' */
	char xfer_list[PSI_XFER_LIST_SIZE];
	} PsiShared;

/* file transfer: what to do (xfer_op) */
enum
	{
	PSI_XOP_PUT = 1,        /* send xfer_local to xfer_remote (replaces it) */
	PSI_XOP_GET,            /* fetch xfer_remote into xfer_local */
	PSI_XOP_LIST,           /* list the folder xfer_remote */
	PSI_XOP_STAT            /* is xfer_remote there? (xfer_exists, xfer_total) */
	};

/* ...and how it went (xfer_result) */
enum
	{
	PSI_XFER_OK = 0,
	PSI_XFER_CANCELLED,     /* Stop */
	PSI_XFER_NO_SFTP,       /* the server does not offer the sftp subsystem */
	PSI_XFER_DENIED,        /* permission denied on the server */
	PSI_XFER_NOT_FOUND,     /* no such file or folder on the server */
	PSI_XFER_LOCAL_WRITE,   /* writing the Psion file failed (disk full, card out) */
	PSI_XFER_LOCAL_READ,    /* the Psion file could not be opened or read */
	PSI_XFER_LINK,          /* not logged in, or the connection went */
	PSI_XFER_FAILED,        /* the server refused (xfer_msg says why) */
	PSI_XFER_TIMEOUT        /* the server stopped answering */
	};

#endif
