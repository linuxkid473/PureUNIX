#include <pureunix/arch.h>
#include <pureunix/io.h>
#include <pureunix/mouse.h>
#include <pureunix/stdio.h>
#include <pureunix/vt.h>

#define I8042_DATA   0x60
#define I8042_STATUS 0x64
#define I8042_CMD    0x64

#define I8042_STATUS_OUT_FULL 0x01
#define I8042_STATUS_IN_FULL  0x02

/* Standard 3-byte PS/2 mouse packet (no scroll wheel -- IntelliMouse's 4-byte
 * extension is never negotiated below, keeping this the same "plain PS/2
 * mouse" baseline every real controller and QEMU's virtual one both support
 * unconditionally). Bit layout, byte 0:
 *   bit0 left button, bit1 right button, bit2 middle button, bit3 always 1
 *   (used below to resynchronize if a byte gets lost), bit4 X sign, bit5 Y
 *   sign, bit6/7 X/Y overflow (dropped -- see push below). */
#define PACKET_BTN_LEFT    0x01
#define PACKET_BTN_RIGHT   0x02
#define PACKET_BTN_MIDDLE  0x04
#define PACKET_ALWAYS_1    0x08
#define PACKET_X_SIGN      0x10
#define PACKET_Y_SIGN      0x20
#define PACKET_X_OVERFLOW  0x40
#define PACKET_Y_OVERFLOW  0x80

static uint8_t packet[3];
static uint32_t packet_pos;
static uint8_t prev_buttons;
static bool mouse_present;

static void wait_write(void)
{
    for (int i = 0; i < 100000 && (inb(I8042_STATUS) & I8042_STATUS_IN_FULL); ++i) {
        /* spin -- i8042 controller round trips are microseconds, not worth
         * a real wait queue (same busy-wait style keyboard_init()'s own
         * output-buffer flush uses). */
    }
}

static void wait_read(void)
{
    for (int i = 0; i < 100000 && !(inb(I8042_STATUS) & I8042_STATUS_OUT_FULL); ++i) {
    }
}

static void ctrl_write(uint8_t cmd)
{
    wait_write();
    outb(I8042_CMD, cmd);
}

static void data_write(uint8_t val)
{
    wait_write();
    outb(I8042_DATA, val);
}

static uint8_t data_read(void)
{
    wait_read();
    return inb(I8042_DATA);
}

/* Sends a command byte to the mouse itself (as opposed to the i8042
 * controller) via the 0xD4 "next data byte goes to the aux port" prefix,
 * and waits for its 0xFA (ACK) response. Returns false if no ACK arrived
 * within a bounded number of tries -- the one signal this driver has that
 * no PS/2 mouse is actually attached (real hardware with a USB-only
 * trackpad, or a QEMU machine with no `-device'd PS/2 mouse). */
static bool mouse_command(uint8_t cmd)
{
    ctrl_write(0xD4);
    data_write(cmd);
    for (int tries = 0; tries < 3; ++tries) {
        if (!(inb(I8042_STATUS) & I8042_STATUS_OUT_FULL)) {
            /* Give the device a moment to respond before giving up on this
             * try -- same bounded spin as wait_read() above. */
            for (int i = 0; i < 100000 && !(inb(I8042_STATUS) & I8042_STATUS_OUT_FULL); ++i) {
            }
        }
        if (!(inb(I8042_STATUS) & I8042_STATUS_OUT_FULL)) {
            continue;
        }
        if (inb(I8042_DATA) == 0xFA) {
            return true;
        }
    }
    return false;
}

static void mouse_irq(interrupt_regs_t *regs)
{
    (void)regs;
    uint8_t byte = inb(I8042_DATA);

    if (packet_pos == 0 && !(byte & PACKET_ALWAYS_1)) {
        /* Out of sync with the packet stream (a byte was lost, or this is
         * the very first byte after enabling reporting) -- drop it and
         * wait for a byte that actually looks like a packet start, exactly
         * how every real PS/2 mouse driver resynchronizes. */
        return;
    }
    packet[packet_pos++] = byte;
    if (packet_pos < 3) {
        return;
    }
    packet_pos = 0;

    if (!mouse_present) {
        return;
    }

    uint8_t buttons = packet[0];
    int dx = packet[1];
    int dy = packet[2];
    if (buttons & PACKET_X_SIGN) {
        dx -= 256;
    }
    if (buttons & PACKET_Y_SIGN) {
        dy -= 256;
    }
    if (buttons & (PACKET_X_OVERFLOW | PACKET_Y_OVERFLOW)) {
        /* Overflow means the reported delta is garbage -- drop the whole
         * packet's motion (but still process button state below) rather
         * than feed a corrupt jump to the pointer. */
        dx = 0;
        dy = 0;
    }
    if (dx != 0 || dy != 0) {
        /* PS/2's Y axis is "up is positive", the opposite of this kernel's
         * framebuffer coordinate space (include/pureunix/framebuffer.h,
         * y grows downward) -- flip it so motion matches what actually
         * happens on screen, unlike the USB HID boot mouse (drivers/hid.c),
         * whose report already matches the framebuffer's sense directly. */
        vt_raw_input_push_mouse_motion(dx, -dy);
    }
    if ((prev_buttons & PACKET_BTN_LEFT) != (buttons & PACKET_BTN_LEFT)) {
        vt_raw_input_push_mouse_button(PU_MOUSE_BTN_LEFT, (buttons & PACKET_BTN_LEFT) != 0);
    }
    if ((prev_buttons & PACKET_BTN_RIGHT) != (buttons & PACKET_BTN_RIGHT)) {
        vt_raw_input_push_mouse_button(PU_MOUSE_BTN_RIGHT, (buttons & PACKET_BTN_RIGHT) != 0);
    }
    if ((prev_buttons & PACKET_BTN_MIDDLE) != (buttons & PACKET_BTN_MIDDLE)) {
        vt_raw_input_push_mouse_button(PU_MOUSE_BTN_MIDDLE, (buttons & PACKET_BTN_MIDDLE) != 0);
    }
    prev_buttons = buttons;
}

void mouse_init(void)
{
    /* Disable both ports while reconfiguring (standard i8042 init dance --
     * see keyboard_init()'s own output-buffer flush for the same "don't
     * trust the BIOS's idea of controller state" caution). */
    ctrl_write(0xAD); /* disable keyboard port */
    ctrl_write(0xA7); /* disable aux (mouse) port */
    while (inb(I8042_STATUS) & I8042_STATUS_OUT_FULL) {
        (void)inb(I8042_DATA);
    }

    ctrl_write(0xA8); /* enable aux port */

    /* Read-modify-write the controller command byte: set bit1 (enable
     * IRQ12 on aux-port activity) and clear bit5 (aux port clock
     * disabled), leave everything else (including bit0, the keyboard's
     * own IRQ1 enable) exactly as the BIOS/keyboard_init() left it. */
    ctrl_write(0x20);
    uint8_t status = data_read();
    status |= 0x02;
    status &= (uint8_t)~0x20;
    ctrl_write(0x60);
    data_write(status);

    ctrl_write(0xAE); /* re-enable keyboard port */

    if (!mouse_command(0xF6)) { /* set defaults */
        printf("mouse: no PS/2 mouse detected (no ACK to 0xF6)\n");
        return; /* no mouse attached -- leave IRQ12 unrouted */
    }
    if (!mouse_command(0xF4)) { /* enable data reporting */
        printf("mouse: PS/2 mouse detected but failed to enable reporting (no ACK to 0xF4)\n");
        return;
    }

    mouse_present = true;
    packet_pos = 0;
    interrupt_register_handler(44, mouse_irq); /* IRQ12 -> vector 32+12 */
    irq_enable(12);
    printf("mouse: PS/2 mouse attached (IRQ12)\n");
}
