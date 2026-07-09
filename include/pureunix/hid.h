#ifndef PUREUNIX_HID_H
#define PUREUNIX_HID_H

#include <pureunix/usb.h>

/* USB HID Boot Protocol keyboard support -- deliberately *not* a general
 * HID report-descriptor parser. Boot Protocol exists specifically so a
 * BIOS/bootloader (and, here, this kernel) can talk to a HID keyboard/mouse
 * without implementing the full HID Report Descriptor language: every Boot
 * Protocol keyboard is contractually guaranteed to accept
 * SET_PROTOCOL(Boot) and then produce exactly the fixed 8-byte report
 * format decoded below, regardless of what its real (non-Boot) HID report
 * descriptor actually says. A general report-descriptor parser would be a
 * substantially larger undertaking and add support for devices (arbitrary
 * HID gadgets, some mice) this driver has no need to support -- see
 * docs/usb.md's known limitations. */

/* HID class (interface descriptor bInterfaceClass/SubClass/Protocol, USB
 * HID spec sec 4.2/4.3) values identifying a Boot Protocol keyboard. */
#define HID_CLASS               3U
#define HID_SUBCLASS_BOOT       1U
#define HID_PROTOCOL_KEYBOARD   1U

/* HID class-specific request (bRequest) and protocol values (USB HID spec
 * sec 7.2.6). */
#define HID_REQ_SET_PROTOCOL 0x0BU
#define HID_PROTOCOL_BOOT    0U

/* Boot keyboard report modifier byte bits (USB HID spec Appendix B.1). */
#define HID_MOD_LEFT_CTRL   (1U << 0)
#define HID_MOD_LEFT_SHIFT  (1U << 1)
#define HID_MOD_LEFT_ALT    (1U << 2)
#define HID_MOD_LEFT_GUI    (1U << 3)
#define HID_MOD_RIGHT_CTRL  (1U << 4)
#define HID_MOD_RIGHT_SHIFT (1U << 5)
#define HID_MOD_RIGHT_ALT   (1U << 6)
#define HID_MOD_RIGHT_GUI   (1U << 7)

/* Upper bound on simultaneously attached Boot Protocol keyboards this
 * driver tracks -- generous headroom over the realistic "0 or 1 USB
 * keyboard" case. */
#define HID_MAX_KEYBOARDS 4U

/* Examines `dev` (as populated by usb_enumerate_port(), include/pureunix/
 * usb.h) and, if it's a Boot Protocol keyboard interface (HID class, Boot
 * subclass, Keyboard protocol) with an Interrupt IN endpoint already
 * configured, attaches to it: issues SET_PROTOCOL(Boot) and arms a
 * repeating interrupt transfer that decodes each 8-byte boot report and
 * feeds translated key events into the active VT's input queue
 * (include/pureunix/vt.h's vt_input_push()) -- the same call
 * drivers/keyboard.c's PS/2 driver makes, so this keyboard and any PS/2
 * keyboard produce identical events to everything above them. Returns
 * false (silently, not an error)
 * if `dev` isn't a Boot Protocol keyboard at all; logs and returns false if
 * it is one but attachment fails. */
bool hid_try_attach(const usb_hc_ops_t *hc, const usb_device_t *dev);

#endif
