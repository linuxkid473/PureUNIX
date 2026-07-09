#ifndef PUREUNIX_TTY_H
#define PUREUNIX_TTY_H

#include <pureunix/termios.h>
#include <pureunix/types.h>

/* Console tty driver: the keyboard-input half of termios. Sits between
 * SYS_READ (fd 0) and drivers/keyboard.c's keyboard_getkey(), applying
 * whatever struct termios is currently in effect. */

void tty_init(void);

/* fd must be 0, 1, or 2 (all three name the same console); anything else is
 * the caller's responsibility to reject before calling in (see SYS_TCGETATTR
 * / SYS_TCSETATTR in arch/i386/syscall.c). */
int tty_get_termios(struct termios *out);
int tty_set_termios(const struct termios *in);

/* Same contract as read(fd, buf, len) for fd 0's default console binding:
 * returns the number of bytes stored, 0 on end-of-input (VEOF pressed on an
 * empty line), or a negative errno (-EINTR when ISIG's VINTR fires, -EINVAL
 * on a null buffer). Blocks/echoes against VT vt_id specifically, not
 * necessarily the calling task's own -- fd 0 (SYS_READ, arch/i386/
 * syscall.c) always passes the caller's own vt_id; a /dev/ttyN descriptor
 * opened for a *different* VT (see SYS_OPEN's /dev/tty interception) passes
 * that VT's id instead, so reading someone else's tty device node blocks
 * until that VT is the active one, same as a real Linux tty device. */
int tty_read(int vt_id, char *buf, size_t len);

#endif
