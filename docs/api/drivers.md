# API Reference: Drivers

---

## VGA

**Header**: `<pureunix/vga.h>`

### Colors

```c
enum vga_color {
    VGA_BLACK=0, VGA_BLUE=1, VGA_GREEN=2, VGA_CYAN=3,
    VGA_RED=4, VGA_MAGENTA=5, VGA_BROWN=6, VGA_LIGHT_GREY=7,
    VGA_DARK_GREY=8, VGA_LIGHT_BLUE=9, VGA_LIGHT_GREEN=10,
    VGA_LIGHT_CYAN=11, VGA_LIGHT_RED=12, VGA_LIGHT_MAGENTA=13,
    VGA_LIGHT_BROWN=14, VGA_WHITE=15,
};
```

### Functions

```c
void vga_init(void);
```
Sets default color (light grey on black), clears screen, resets ANSI parser state.

```c
void vga_clear(void);
```
Fills all 80×25 cells with spaces in the current color.

```c
void vga_putc(char c);
```
Writes one character, handling `\n`, `\r`, `\t`, `\b`, ANSI escape sequences, and scrolling. Forwards the character to `serial_putc`.

```c
void vga_write(const char *str);
```
Calls `vga_putc` for each character until `\0`.

```c
void vga_write_len(const char *str, size_t len);
```
Calls `vga_putc` for exactly `len` characters.

```c
void vga_set_color(uint8_t fg, uint8_t bg);
```
Sets the active color. `fg` and `bg` are `vga_color` values. Subsequent characters use this color.

```c
uint8_t vga_color(void);
```
Returns the current attribute byte (`fg | (bg << 4)`).

```c
void vga_get_cursor(size_t *row, size_t *col);
```
Writes the current internal cursor position to `*row` and `*col`.

```c
void vga_goto(size_t row, size_t col);
```
Moves the internal cursor position and sends a serial cursor-position sequence. Does **not** update the VGA hardware cursor register.

```c
void vga_set_cursor(size_t row, size_t col);
```
Moves the internal cursor and updates the VGA CRTC hardware cursor (registers `0x0E`/`0x0F` via ports `0x3D4`/`0x3D5`). Also sends a serial cursor-position sequence.

```c
void vga_move_cursor_rel(int drow, int dcol);
```
Moves the hardware cursor by `(drow, dcol)` relative to the current position, clamped to screen bounds.

```c
void vga_erase_eol(void);
```
Clears from the current column to the end of the current row. Sends `\033[K` to serial.

```c
void vga_status_bar(const char *left, const char *right);
```
Draws a status bar on row 24 with inverted colors (`fg=black, bg=white`). `left` is left-aligned; `right` is right-aligned.

---

## Serial

**Header**: `<pureunix/serial.h>`

```c
void serial_init(void);
```
Configures COM1 (base `0x3F8`) at 38400 baud, 8N1, FIFO enabled.

```c
void serial_putc(char c);
```
Transmits one character. Converts `\n` to `\r\n`. Busy-waits for the transmitter holding register to be empty.

```c
void serial_write(const char *s);
```
Calls `serial_putc` for each character until `\0`.

```c
void serial_clear(void);
```
Sends `\033[2J\033[H` to clear the terminal and home the cursor.

```c
void serial_erase_line(void);
```
Sends `\033[K` to erase to end of line.

```c
void serial_move_cursor(unsigned row, unsigned col);
```
Sends `\033[row+1;col+1H` to position the cursor (1-based in ANSI).

---

## PS/2 Keyboard

**Header**: `<pureunix/keyboard.h>`

### Key Constants

```c
KEY_NONE      = 0      // no key available
KEY_BACKSPACE = 8
KEY_TAB       = 9
KEY_ENTER     = 10
KEY_ESCAPE    = 27
KEY_UP        = 0x101
KEY_DOWN      = 0x102
KEY_LEFT      = 0x103
KEY_RIGHT     = 0x104
KEY_HOME      = 0x105
KEY_END       = 0x106
KEY_PAGE_UP   = 0x107
KEY_PAGE_DOWN = 0x108
KEY_DELETE    = 0x109
KEY_CTRL_S    = 0x10A
KEY_CTRL_Q    = 0x10B
KEY_CTRL_F    = 0x10C
KEY_CTRL_C    = 0x10D
```

Printable ASCII characters are returned as their ASCII values.

### Functions

```c
void keyboard_init(void);
```
Flushes the PS/2 buffer, registers the IRQ1 handler, enables IRQ1.

```c
int keyboard_getkey(void);
```
Blocking. Loops on `arch_halt()` until a key is available in the ring buffer, then removes and returns it.

```c
int keyboard_try_getkey(void);
```
Non-blocking. Returns `KEY_NONE` (0) if the ring buffer is empty, otherwise removes and returns the next key.

```c
bool keyboard_ctrl_down(void);
```
Returns `true` if the Ctrl key is currently held down.

---

## Console TTY / termios

**Headers**: `<pureunix/termios.h>`, `<pureunix/tty.h>`

One `struct termios` for the whole machine (`drivers/tty.c`'s static `console_termios`) — there's exactly one terminal (the VGA console / PS2 keyboard), so unlike a real kernel this isn't per-open-file-description state. Backs `SYS_TCGETATTR`/`SYS_TCSETATTR` (see `docs/syscalls.md`) and `SYS_READ`'s fd == 0 case.

```c
struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];   // NCCS == 8
};
```

`c_cc[]` indices: `VINTR`, `VQUIT`, `VERASE`, `VKILL`, `VEOF`, `VMIN`, `VTIME`, `VSUSP`. `c_lflag` bits: `ISIG`, `ICANON`, `ECHO`, `ECHOE`, `ECHOK`, `ECHONL`. `c_iflag`/`c_oflag` (`ICRNL`/`INLCR`, `OPOST`/`ONLCR`) are defined for API completeness but currently have no effect — `vga_putc` already renders `\n`/`\r` correctly without translation.

Default (cooked) mode: `ISIG|ICANON|ECHO|ECHOE|ECHOK`, `VINTR`=^C (3), `VQUIT`=^\\ (28), `VERASE`=DEL (127), `VKILL`=^U (21), `VEOF`=^D (4), `VMIN`=1, `VTIME`=0, `VSUSP`=^Z (26).

```c
void tty_init(void);
```
Resets `console_termios` to the cooked defaults above. Called once from `kernel_main`, after `keyboard_init()`.

```c
int tty_get_termios(struct termios *out);
int tty_set_termios(const struct termios *in);
```
Copy the current termios out to `*out`, or replace it wholesale from `*in`. Both return `-EINVAL` on a null pointer; there is no partial/masked update.

```c
int tty_read(char *buf, size_t len);
```
The fd-0 half of `SYS_READ`. In canonical mode (`ICANON` set): line-buffered, with `ECHO`/`ECHOE`/`ECHOK` controlling echo and `VERASE`/`VKILL`/`VEOF` editing; returns the line including its trailing `\n`, or 0 on `VEOF` with an empty line. In raw mode: blocks for one byte, then drains whatever else is already queued without blocking again (approximates `VMIN`=1, `VTIME`=0; arbitrary `VMIN`/`VTIME` combinations are not implemented — there's no timer-driven read yet). `VINTR` aborts either mode and returns `-EINTR` when `ISIG` is set. Returns `-EINVAL` on a null buffer.

Re-enables interrupts (`arch_enable_interrupts()`) before it may block — `SYS_READ` arrives via `int $0x80`, which runs with interrupts masked from entry (see `arch/i386/interrupt_stubs.S`'s `isr128`) until its own `iret`; without this, `keyboard_getkey()`'s `hlt`-based wait could never be woken by the keyboard IRQ.

---

## ATA PIO

**Header**: `<pureunix/disk.h>`

### `disk_device_t`

```c
typedef struct disk_device {
    const char *name;           // "ata0"
    uint32_t    sector_size;    // always 512
    bool        present;        // true if drive responded to IDENTIFY
    int (*read)(uint32_t lba, uint8_t *buffer);   // read one sector
    int (*write)(uint32_t lba, const uint8_t *buffer); // write one sector
} disk_device_t;
```

### Functions

```c
void ata_init(void);
```
Probes the ATA primary master. Registers IRQ14 handler. Sets `disk->present` on successful IDENTIFY.

```c
disk_device_t *ata_primary_master(void);
```
Returns a pointer to the static `disk_device_t` for the primary master drive. Check `present` before using `read`/`write`.

```c
int ata_read_sector(uint32_t lba, uint8_t *buffer);
```
Reads one 512-byte sector at LBA `lba` into `buffer`. Uses PIO polling. Returns `0` on success, `-1` on timeout or error.

```c
int ata_write_sector(uint32_t lba, const uint8_t *buffer);
```
Writes one 512-byte sector from `buffer` to LBA `lba`. Issues a cache flush after writing. Returns `0` on success, `-1` on error.

---

## Architecture / Interrupts

**Header**: `<pureunix/arch.h>`

```c
void arch_init(void);
```
Calls `gdt_init()` and `idt_init()`. Must be called before any interrupt registration.

```c
void interrupt_register_handler(uint8_t vector, interrupt_handler_t handler);
```
Registers a C function to handle a given interrupt vector. `handler` is called from `isr_dispatch` with the saved register state. For IRQ handlers (vectors 32–47), the handler must not call `pic_send_eoi` directly; EOI is sent by `isr_dispatch` after returning.

```c
void irq_enable(uint8_t irq);
```
Clears the corresponding bit in the PIC IMR to unmask the IRQ line.

```c
void irq_disable(uint8_t irq);
```
Sets the corresponding bit in the PIC IMR to mask the IRQ line.

```c
void pit_init(uint32_t hz);
```
Programs PIT channel 0 in square-wave mode. Divisor = 1193182 / `hz`. Registers an IRQ0 handler that increments `ticks`.

```c
uint64_t pit_ticks(void);
```
Returns the current tick count (incremented by the IRQ0 handler).

```c
void pit_sleep(uint32_t ms);
```
Busy-waits for approximately `ms` milliseconds by polling `pit_ticks()`.

### Inline Functions

```c
static inline void arch_halt(void);           // HLT instruction
static inline void arch_enable_interrupts(void);  // STI
static inline void arch_disable_interrupts(void); // CLI
```

---

## Port I/O

**Header**: `<pureunix/io.h>`

All functions are inline; no library call overhead.

```c
void    outb(uint16_t port, uint8_t value);
void    outw(uint16_t port, uint16_t value);
void    outl(uint16_t port, uint32_t value);
uint8_t  inb(uint16_t port);
uint16_t inw(uint16_t port);
uint32_t inl(uint16_t port);
void    io_wait(void);   // write to port 0x80 for ~1µs delay
```

---

## PCI

**Header**: `<pureunix/pci.h>`

```c
typedef struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  header_type;
    uint8_t  interrupt_line;
} pci_device_t;

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value);

void pci_scan(void);
bool pci_find(uint16_t vendor_id, const uint16_t *device_ids, int count, pci_device_t *out);
void pci_enable_bus_mastering(const pci_device_t *dev);

phys_addr_t pci_bar_address(const pci_device_t *dev, int index);  // 0 for I/O-space BARs
uint32_t    pci_bar_size(const pci_device_t *dev, int index);
```

See `docs/drivers.md`'s "PCI Bus" section for the full design writeup.

---

## Intel e1000 NIC

**Header**: `<pureunix/e1000.h>`

```c
void e1000_init(void);
bool e1000_present(void);
void e1000_get_mac(uint8_t mac[6]);
int  e1000_send(const void *data, uint16_t len);      // 0 on success, -1 on failure
int  e1000_receive(void *buf, uint16_t buf_len);      // frame length, 0 if none ready, -1 if buf too small
void e1000_set_rx_handler(void (*handler)(void));     // called from the RX interrupt; see net/eth.c
void e1000_selftest(void);
void e1000_dump_stats(void);                          // diagnostic: counters + live register snapshot
```

See `docs/drivers.md`'s "Intel e1000 NIC Driver" section for the register map, descriptor layout, and design writeup.
