#ifndef PUREUNIX_INPUT_H
#define PUREUNIX_INPUT_H

#include <pureunix/types.h>

/* Generic keyboard input-event queue, shared by every keyboard producer
 * (PS/2: drivers/keyboard.c, USB HID boot-protocol: drivers/hid.c) so the
 * tty layer (drivers/tty.c) and everything above it sees identical KEY_*
 * events regardless of which physical keyboard produced them. This is the
 * same circular-buffer/arch_halt() blocking design drivers/keyboard.c used
 * to own directly; it moved here so a second producer could be added
 * without duplicating it. */

/* Pushes a translated key event (an ASCII byte, or one of keyboard.h's
 * KEY_* constants) onto the shared queue. Drops the event silently if the
 * queue is full (matches the previous drivers/keyboard.c behavior) --
 * losing a keystroke under a full queue is preferable to blocking a
 * producer that may be running in interrupt context. */
void input_push_key(int key);

/* Non-blocking: returns the next queued key, or KEY_NONE if the queue is
 * empty. */
int input_try_getkey(void);

/* Blocking: halts (via arch_halt(), woken by the next IRQ of any kind) until
 * a key is available. */
int input_getkey(void);

#endif
