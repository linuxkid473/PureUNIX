/*
 * user/tty.c — report or switch the calling shell's virtual terminal.
 *
 * With no arguments, prints the VT this process's stdin/stdout/stderr are
 * attached to (POSIX ttyname() semantics) via ioctl(VT_GETACTIVE).
 * With a numeric argument 1..PU_NUM_VTS, switches the *foreground* VT via
 * ioctl(VT_ACTIVATE) — arch/i386/syscall.c's SYS_IOCTL handler calls the
 * exact same kernel/vt.c vt_switch() that Alt+F<n> does (drivers/keyboard.c,
 * drivers/hid.c), so this is not a separate implementation of switching,
 * just a second caller of the same one.
 */
#include "libpure.h"

static void print_current(void)
{
    int n;
    if (pu_ioctl(0, VT_GETACTIVE, &n) != 0) {
        pu_puts("not a tty\n");
        return;
    }
    pu_puts("/dev/tty");
    pu_puti(n);
    pu_puts("\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_current();
        return 0;
    }

    int n = pu_atoi(argv[1]);
    if (n < 1 || n > PU_NUM_VTS) {
        pu_puts("usage: tty [1-");
        pu_puti(PU_NUM_VTS);
        pu_puts("]\n  with no argument, prints the current virtual terminal\n");
        return 1;
    }

    if (pu_ioctl(0, VT_ACTIVATE, &n) != 0) {
        pu_puts("tty: could not switch virtual terminal\n");
        return 1;
    }
    return 0;
}
