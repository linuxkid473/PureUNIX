#ifndef PUREUNIX_PTY_H
#define PUREUNIX_PTY_H

/* Real PTY (pseudo-terminal) pairs — the general primitive a userspace
 * terminal emulator needs that kernel/vt.c's 6 physical VTs don't provide:
 * a *detached* tty whose "hardware" is another ordinary process (PUTerm,
 * docs/pude.md) rather than the VGA console. Deliberately modeled on
 * kernel/vt.c + drivers/tty.c, which already implement every piece of real
 * tty technology this needs (per-terminal termios, canonical-mode line
 * discipline with echo/VERASE/VKILL, ISIG signal interception at the point
 * data *arrives* rather than at read() time, controlling-terminal/
 * foreground-pgid job control) — this file reuses that design over a pair
 * of ring buffers instead of a VGA console + keyboard queue.
 *
 * Two ends, both real, refcounted open_file_t descriptions (include/
 * pureunix/task.h's FD_KIND_PTY):
 *   - master: PUTerm's end. Reads drain whatever the slave-side process
 *     wrote (never blocks — see pty_master_read()); writes are what the
 *     user typed, and are the ISIG interception point (mirrors
 *     kernel/vt.c's vt_input_push()).
 *   - slave: the shell's end (dup2()'d onto its fd 0/1/2, exactly like a
 *     real UNIX pty). Reads apply real canonical-mode line discipline
 *     (mirrors drivers/tty.c's tty_read()); writes are the shell's own
 *     stdout/stderr, queued for the master to render.
 */
#include <pureunix/task.h>
#include <pureunix/termios.h>
#include <pureunix/types.h>
#include <pureunix/wait.h>

typedef struct pty pty_t;

/* Allocates a new pty from a fixed system-wide pool (mirrors
 * MAX_OPEN_FILE_DESCRIPTIONS's style — see kernel/pty.c's MAX_PTYS).
 * Starts with master_ends == slave_ends == 0; the caller (SYS_PTY_CREATE)
 * must still call pty_master_ref()/pty_slave_ref() once each for the two
 * open_file_t's it installs, exactly like open_file_alloc() starting a
 * fresh description at refcount 1. Returns NULL if the pool is exhausted. */
pty_t *pty_alloc(void);

void pty_master_ref(pty_t *p);
void pty_slave_ref(pty_t *p);
/* Drops a reference; wakes the opposite end's blocked reader/writer (to
 * see EOF/EPIPE instead of hanging forever) the instant the last reference
 * on this end goes away. Frees the pty_t once *both* ends have hit zero
 * references — safe to call with p == NULL (no-op), matching
 * open_file_unref()'s own convention. */
void pty_master_unref(pty_t *p);
void pty_slave_unref(pty_t *p);

/* Master-side I/O (PUTerm's end).
 *
 * pty_master_read() never blocks: PUTerm's own event/render loop must stay
 * responsive to input and redraws regardless of whether the shell has
 * written anything, so (unlike a real UNIX master, which blocks unless
 * O_NONBLOCK is set) this always returns immediately — the number of bytes
 * copied (0 if the slave→master ring is currently empty, also 0 once the
 * slave end is gone and the ring has drained, matching real EOF; PUTerm
 * tells the two apart, if it cares, via waitpid()/PU_WNOHANG on the child
 * it forked, not via this return value).
 *
 * pty_master_write() is where Ctrl+C/Ctrl+Z/Ctrl+\ are intercepted (if the
 * pty's termios has ISIG set) — exactly like kernel/vt.c's vt_input_push():
 * a matching byte never reaches the slave→master... no, the master→slave
 * ring as data at all; it goes straight to signal_send_pgrp() against this
 * pty's own foreground process group, with a "^C"-style echo pushed onto
 * the slave→master ring so PUTerm still shows it, the same way a real
 * terminal does. This is why a foreground job that never calls read() at
 * all (a CPU-bound loop, `sleep`) is still interruptible. May block if the
 * ring is momentarily full (real keystroke arrival rates never fill a 4KiB
 * ring in practice) — wakes once the slave drains it, or returns -EPIPE if
 * the slave end is gone. */
int pty_master_read(pty_t *p, char *buf, size_t len);
int pty_master_write(pty_t *p, const char *buf, size_t len);

/* Slave-side I/O (the shell/child's end) — real canonical-mode line
 * discipline (ICANON/ECHO/ECHOE/ECHOK/VERASE/VKILL/VEOF), mirroring
 * drivers/tty.c's tty_read() exactly, just sourcing raw bytes already
 * written by the master instead of a KEY_* keyboard queue (there's no
 * separate escape-sequence encoding step here — the master already writes
 * real bytes, arrow-key escape sequences included, exactly like a real
 * terminal emulator's pty writes). Blocks via wait_queue_sleep() like a
 * pipe; returns 0 (EOF) once the ring is drained and the master end is
 * gone. pty_slave_write() is a plain ring-buffer write (blocks if full,
 * returns -EPIPE if the master end is gone) — no output post-processing
 * (OPOST/ONLCR are unused placeholders here for the same reason
 * include/pureunix/termios.h documents for the VGA console: the consumer
 * — PUTerm's own cell-grid renderer — already treats '\n' as the full
 * newline+carriage-return real terminals do, matching vga.c's newline()). */
int pty_slave_read(pty_t *p, char *buf, size_t len);
int pty_slave_write(pty_t *p, const char *buf, size_t len);

int pty_get_termios(pty_t *p, struct termios *out);
int pty_set_termios(pty_t *p, const struct termios *in);

/* Controlling-terminal/job-control state — same shape as kernel/vt.c's
 * vt_claim_session()/vt_get_fg_pgid()/vt_set_fg_pgid(), just per-pty
 * instead of per-VT. 0 until something sets it (0 is never a valid
 * pgid/sid — see kernel/vt.c). */
int pty_get_fg_pgid(pty_t *p);
void pty_set_fg_pgid(pty_t *p, int pgid);
int pty_get_sid(pty_t *p);
void pty_set_sid(pty_t *p, int sid);

/* Window size (TIOCGWINSZ/TIOCSWINSZ) — PUTerm sets this once at creation
 * from its own window's pixel size / font cell size; unlike the VGA
 * console's TIOCGWINSZ (a fixed hardware grid), a pty's size is whatever
 * the master says it is. */
void pty_get_winsize(pty_t *p, unsigned short *rows, unsigned short *cols);
void pty_set_winsize(pty_t *p, unsigned short rows, unsigned short cols);

#endif
