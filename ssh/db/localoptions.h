/* PsiTerm build of the Dropbear client: modern, minimal, fast enough for a 36MHz ARM */
#define DROPBEAR_SMALL_CODE 1
#define DROPBEAR_X11FWD 0
#define DROPBEAR_CLI_LOCALTCPFWD 0
#define DROPBEAR_CLI_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_CLI_PROXYCMD 0
#define DROPBEAR_CLI_NETCAT 0
#define DROPBEAR_USER_ALGO_LIST 0
#define DROPBEAR_AES128 1
#define DROPBEAR_AES256 0
#define DROPBEAR_CHACHA20POLY1305 1
#define DROPBEAR_ENABLE_CTR_MODE 1
#define DROPBEAR_ENABLE_GCM_MODE 0
#define DROPBEAR_SHA2_256_HMAC 1
#define DROPBEAR_SHA2_512_HMAC 0
#define DROPBEAR_RSA 1
#define DROPBEAR_ECDSA 0
#define DROPBEAR_ED25519 1
#define DROPBEAR_SK_KEYS 0
#define DROPBEAR_DH_GROUP14_SHA256 0
#define DROPBEAR_CURVE25519 1
#define DROPBEAR_SNTRUP761 0
#define DROPBEAR_MLKEM768 0
#define DROPBEAR_ECDH 0
#define DROPBEAR_DH_GROUP1_CLIENTONLY 0
#define DROPBEAR_CLI_PASSWORD_AUTH 1
#define DROPBEAR_CLI_PUBKEY_AUTH 1
/* PsiTerm passes its own key with -i when there is one (0.44) */
#define DROPBEAR_DEFAULT_CLI_AUTHKEY ""
#define DROPBEAR_USE_PASSWORD_ENV 0
#define DROPBEAR_SFTPSERVER 0
/* Psion paths and behaviour */
#define DROPBEAR_URANDOM_DEV "/dev/urandom"
#define DROPBEAR_CLI_IMMEDIATE_AUTH 0
#define DEFAULT_RECV_WINDOW 8192
#define RECV_MAX_PAYLOAD_LEN 32768
/* client-only build: turn off server features that need Unix accounts */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_PUBKEY_OPTIONS 0
#define DROPBEAR_SVR_MULTIUSER 0
#define DROPBEAR_SVR_DROP_PRIVS 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_REEXEC 0
#define DO_MOTD 0
/* SSH compression (zlib@openssh.com, after login). Server->Psion text
   shrinks 3-5x, which is what matters on a serial line. Small deflate
   window/memory for our (tiny) outgoing keystroke stream. */
#define DROPBEAR_CLI_COMPRESSION 1
#define DROPBEAR_ZLIB_WINDOW_BITS 10
