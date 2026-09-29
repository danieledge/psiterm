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

#define PSI_SHARED_NAME   "PsiTermSSH"
#define PSI_SHARED_MAGIC  0x48535350u      /* 'PSSH' */
#define PSI_KBD_SIZE      2048
#define PSI_OUT_SIZE      16384
#define PSI_ENTROPY_SIZE  512

enum
	{
	PSI_STATE_STARTING = 0,
	PSI_STATE_DIALING,
	PSI_STATE_KEYEX,
	PSI_STATE_CONNECTED,
	PSI_STATE_EXITED
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
	} PsiShared;

#endif
