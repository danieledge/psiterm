#ifndef PSI_TERMIOS_H
#define PSI_TERMIOS_H
/* Minimal termios for Dropbear on EPOC: PsiTerm is always a raw 8-bit
   terminal, so these only need to exist; the shim reports a raw tty. */
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;
#define NCCS 20
struct termios {
	tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
	cc_t c_cc[NCCS];
	speed_t c_ispeed, c_ospeed;
};
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VEOL 5
#define VEOL2 6
#define VSTART 7
#define VSTOP 8
#define VSUSP 9
#define VREPRINT 10
#define VWERASE 11
#define VLNEXT 12
#define VDISCARD 13
#define VMIN 14
#define VTIME 15
#define IGNPAR 0x0004
#define PARMRK 0x0008
#define INPCK 0x0010
#define ISTRIP 0x0020
#define INLCR 0x0040
#define IGNCR 0x0080
#define ICRNL 0x0100
#define IXON 0x0400
#define IXANY 0x0800
#define IXOFF 0x1000
#define IMAXBEL 0x2000
#define ISIG 0x0001
#define ICANON 0x0002
#define ECHO 0x0008
#define ECHOE 0x0010
#define ECHOK 0x0020
#define ECHONL 0x0040
#define NOFLSH 0x0080
#define TOSTOP 0x0100
#define IEXTEN 0x8000
#define ECHOCTL 0x0200
#define ECHOKE 0x0800
#define PENDIN 0x4000
#define OPOST 0x0001
#define ONLCR 0x0004
#define OCRNL 0x0008
#define ONOCR 0x0010
#define ONLRET 0x0020
#define CS7 0x0020
#define CS8 0x0030
#define CSIZE 0x0030
#define PARENB 0x0100
#define PARODD 0x0200
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define B9600 9600
#define B38400 38400
int psi_tcgetattr(int fd, struct termios *t);
int psi_tcsetattr(int fd, int act, const struct termios *t);
#define tcgetattr psi_tcgetattr
#define tcsetattr psi_tcsetattr
#define cfgetospeed(t) ((t)->c_ospeed)
#define cfgetispeed(t) ((t)->c_ispeed)
#define TIOCGWINSZ 0x5413
#endif
