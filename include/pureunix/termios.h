#ifndef PUREUNIX_TERMIOS_H
#define PUREUNIX_TERMIOS_H

/* PureUNIX's termios: enough of POSIX's terminal-control API to switch the
 * console between cooked (line-buffered, echoing) and raw (byte-at-a-time)
 * input and to configure which control characters do what. There is only
 * one physical terminal (the VGA console / PS2 keyboard), so unlike a real
 * kernel this isn't per-tty-device state — see drivers/tty.c, which keeps a
 * single global struct termios that fds 0/1/2 all share.
 *
 * Field layout is this kernel's own; it does not need to match Linux's or
 * any other UNIX's on-the-wire struct termios. */

#define NCCS 8

/* c_cc[] indices */
#define VINTR  0  /* SIGINT-like line abort (default: Ctrl-C, 0x03) */
#define VQUIT  1  /* default: Ctrl-\, 0x1C — recognized, not acted on (no signals) */
#define VERASE 2  /* erase previous character (default: DEL, 0x7F) */
#define VKILL  3  /* erase entire input line (default: Ctrl-U, 0x15) */
#define VEOF   4  /* end-of-file in canonical mode (default: Ctrl-D, 0x04) */
#define VMIN   5  /* raw mode: minimum bytes before read() returns */
#define VTIME  6  /* raw mode: read() timeout in tenths of a second (unused: no timer-driven read yet) */
#define VSUSP  7  /* default: Ctrl-Z, 0x1A — recognized, not acted on (no job control) */

/* c_iflag bits */
#define ICRNL  0x0001 /* map CR to NL on input (unused placeholder: PureUNIX's keyboard driver has no CR key) */
#define INLCR  0x0002 /* map NL to CR on input (unused placeholder) */

/* c_oflag bits */
#define OPOST  0x0001 /* post-process output (unused placeholder: vga_putc already renders '\n' correctly) */
#define ONLCR  0x0002 /* map NL to CR-NL on output (unused placeholder, same reason) */

/* c_lflag bits */
#define ISIG   0x0001 /* VINTR aborts the current read() with -EINTR */
#define ICANON 0x0002 /* line-buffered input with VERASE/VKILL/VEOF editing */
#define ECHO   0x0004 /* echo input characters as they're typed */
#define ECHOE  0x0008 /* VERASE visually erases the last character (backspace-space-backspace) */
#define ECHOK  0x0010 /* VKILL is followed by a newline echo */
#define ECHONL 0x0020 /* echo newline even when ECHO is off (canonical mode only) */

/* tcsetattr() "actions" argument. PureUNIX's console has no pending output
 * queue or unread-input buffer to drain/flush, so all three apply the new
 * settings immediately — they exist only so tcsetattr()'s signature and
 * argument validation match POSIX. */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
};

#endif
