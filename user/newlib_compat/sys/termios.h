/* Override for <sys/termios.h>, which newlib's own <termios.h>
 * (third_party/newlib/i686-elf/include/termios.h) includes but which was
 * never provided for this bare i686-elf target (see
 * user/newlib_compat/byteswap.h's header comment for the general pattern).
 *
 * Field layout and bit values match real Linux/glibc i386 termios exactly
 * (not PureUNIX's own struct termios — include/pureunix/termios.h — which
 * has a much smaller, different, kernel-internal layout); tcgetattr()/
 * tcsetattr() (user/newlib_syscalls.c) translate between the two, the same
 * way this directory's stat()/dirent() shims translate PureUNIX's raw
 * structs into newlib-shaped ones. Matching Linux's real values (rather
 * than inventing new ones) avoids repeated one-off gaps as more
 * termios-heavy BusyBox code gets built later.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_TERMIOS_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_TERMIOS_H

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32

#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

/* c_iflag */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IUCLC   0001000
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

/* c_oflag */
#define OPOST  0000001
#define OLCUC  0000002
#define ONLCR  0000004
#define OCRNL  0000010
#define ONOCR  0000020
#define ONLRET 0000040
#define OFILL  0000100
#define OFDEL  0000200
#define NLDLY  0000400
#define CRDLY  0003000
#define TABDLY 0014000
#define BSDLY  0020000
#define VTDLY  0040000
#define FFDLY  0100000

/* c_cflag */
#define CBAUD   0010017
#define CBAUDEX 0010000
#define CSIZE   0000060
#define CS5     0000000
#define CS6     0000020
#define CS7     0000040
#define CS8     0000060
#define CSTOPB  0000100
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000
#define CIBAUD  002003600000
#define CRTSCTS 020000000000

/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define XCASE   0000004
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOPRT 0002000
#define ECHOKE  0004000
#define FLUSHO  0010000
#define PENDIN  0040000
#define IEXTEN  0100000

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

#define TCOOFF 0
#define TCOON  1
#define TCIOFF 2
#define TCION  3

/* Standard Linux/glibc termios baud-rate constants (octal values, matching
 * CBAUD's bit layout above) — PureUNIX's console has no real serial line
 * speed to configure, so these only matter for BusyBox code that
 * stores/prints them symbolically (stty-style tools); none of the
 * applets currently enabled in .config call cfsetispeed() et al. for
 * real. */
#define B0      0000000
#define B50     0000001
#define B75     0000002
#define B110    0000003
#define B134    0000004
#define B150    0000005
#define B200    0000006
#define B300    0000007
#define B600    0000010
#define B1200   0000011
#define B1800   0000012
#define B2400   0000013
#define B4800   0000014
#define B9600   0000015
#define B19200  0000016
#define B38400  0000017
#define B57600   0010001
#define B115200  0010002
#define B230400  0010003
#define EXTA B19200
#define EXTB B38400

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int actions, const struct termios *termios_p);
speed_t cfgetispeed(const struct termios *termios_p);
speed_t cfgetospeed(const struct termios *termios_p);
int cfsetispeed(struct termios *termios_p, speed_t speed);
int cfsetospeed(struct termios *termios_p, speed_t speed);
void cfmakeraw(struct termios *termios_p);
int tcflush(int fd, int queue_selector);
int tcdrain(int fd);
int tcflow(int fd, int action);
int tcsendbreak(int fd, int duration);

#endif
