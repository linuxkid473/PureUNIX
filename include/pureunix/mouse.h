#ifndef PUREUNIX_MOUSE_H
#define PUREUNIX_MOUSE_H

/* PS/2 mouse (i8042 "auxiliary" port, IRQ12) -- the legacy counterpart to
 * drivers/hid.c's USB Boot Protocol mouse, same relationship drivers/
 * keyboard.c's PS/2 keyboard already has to drivers/hid.c's USB keyboard.
 * Both feed the same raw input queue (include/pureunix/vt.h), so userspace
 * (SDL2's event pump, docs/sdl-port.md) sees identical events regardless of
 * which one is actually present. Harmless to call if no PS/2 mouse exists
 * (real hardware with no i8042 aux port, or a QEMU machine that never
 * attaches one): the detection handshake below just fails cleanly and
 * mouse_init() leaves IRQ12 unrouted. */
void mouse_init(void);

#endif
