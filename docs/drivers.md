# Drivers

## VGA Text Driver

**Source**: `drivers/vga.c`  
**Header**: `include/pureunix/vga.h`

### Responsibilities

- Manages the 80×25 VGA text mode framebuffer at `0xB8000`.
- Interprets a subset of ANSI/VT100 escape sequences for color and cursor control.
- Synchronizes all output to the serial port (every character written to VGA is also sent to COM1).
- Provides the hardware cursor position via CRTC registers.

### Initialization

`vga_init()` sets the initial color to `VGA_LIGHT_GREY` on `VGA_BLACK`, resets row and column to 0, resets the ANSI escape parser state, and clears the screen.

### Character Output

`vga_putc(c)` handles all output characters:

| Input | Action |
|---|---|
| `\033` | Begin ANSI escape sequence (state 1) |
| `\n` | Move to column 0 of next row; scroll if needed |
| `\r` | Move to column 0 of current row |
| `\t` | Advance to next 4-column tab stop |
| `\b` | Move cursor back one; erase character; wrap to end of previous row if at column 0 |
| printable | Write to VGA memory at current position; advance column; wrap if needed |

Every character is also forwarded to `serial_putc(c)`.

### ANSI Escape Sequences

The parser is a two-state machine: state 1 (saw `\033`), state 2 (saw `[`, accumulating parameters). The following CSI sequences are handled:

| Sequence | Action |
|---|---|
| `\033[...m` | SGR: set foreground/background color |
| `\033[2J` | Clear screen (`vga_clear`) |
| `\033[K` | Erase to end of line (`erase_current_line`) |
| `\033[H` | Move cursor to (0, 0) |

SGR color mapping:

| Range | Meaning |
|---|---|
| `0` or empty | Reset to light grey on black |
| `30–37` | Set foreground to VGA color 0–7 |
| `90–97` | Set foreground to VGA bright color 8–15 |
| `40–47` | Set background to VGA color 0–7 |

Multiple parameters separated by `;` are processed left to right.

### Scrolling

When `row >= VGA_HEIGHT` (25), `scroll()` copies rows 1–24 to rows 0–23, clears row 24 with the current color, and sets `row = 24`.

### Cursor Control Functions

| Function | Description |
|---|---|
| `vga_set_cursor(row, col)` | Move hardware cursor; update serial cursor |
| `vga_goto(row, col)` | Update internal position and serial cursor; **no** hardware cursor update |
| `vga_move_cursor_rel(drow, dcol)` | Relative move; clamp to screen bounds; update hardware cursor |
| `vga_erase_eol()` | Clear from current column to end of row; send `\033[K` to serial |
| `vga_status_bar(left, right)` | Draw inverted status bar on row 24 |

`vga_goto` is used by the editor for bulk screen redraws to avoid cursor flicker.

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

The VGA attribute byte is `fg | (bg << 4)`.

### Limitations

- Tab stop is hard-coded to 4 columns.
- No full ANSI cursor positioning sequence (`\033[row;colH` is handled only in the form `\033[H` for home).
- No double-width or double-height characters.
- No blinking cursor support.

---

## Serial Driver

**Source**: `drivers/serial.c`  
**Header**: `include/pureunix/serial.h`

### Responsibilities

- Initializes COM1 (I/O base `0x3F8`) at 38400 baud, 8N1, FIFO enabled.
- Mirrors all VGA output to the serial port for use with terminal emulators and QEMU `-serial stdio`.
- Provides ANSI cursor control sequences for terminal emulators.

### Initialization

`serial_init()` configures COM1:

```
DLAB=1: divisor = 3  → 1,843,200 / 16 / 3 = 38,400 baud
LCR = 0x03: 8 data bits, no parity, 1 stop bit
FCR = 0xC7: enable FIFO, clear TX/RX, 14-byte trigger
MCR = 0x0B: DTR, RTS, OUT2 (enables IRQ in loopback test; not in PureUnix)
```

### Character Output

`serial_putc(c)`:
- Converts `\n` to `\r\n` (CRLF for terminal emulators).
- Busy-waits up to 100,000 iterations for the transmitter holding register empty bit (LSR bit 5) before writing the character to the data register.

### ANSI Control

| Function | Sequence sent | Effect on terminal |
|---|---|---|
| `serial_clear()` | `\033[2J\033[H` | Clear screen and home cursor |
| `serial_erase_line()` | `\033[K` | Erase to end of current line |
| `serial_move_cursor(row, col)` | `\033[row+1;col+1H` | Absolute cursor position |

These are used by the VGA driver to keep the terminal emulator in sync with the VGA display.

---

## PS/2 Keyboard Driver

**Source**: `drivers/keyboard.c`  
**Header**: `include/pureunix/keyboard.h`

### Responsibilities

- Handles PS/2 keyboard IRQ1 (vector 33).
- Translates scan code set 1 bytes to key codes.
- Tracks modifier state: Shift, Ctrl, Alt, Caps Lock.
- Decodes extended (0xE0-prefixed) scancodes for arrow keys and navigation keys.
- Maintains a 128-entry circular ring buffer for key events.

### Initialization

`keyboard_init()`:
1. Flushes the PS/2 output buffer (reads and discards all pending bytes from port `0x60`).
2. Registers `keyboard_irq` as the handler for vector 33.
3. Enables IRQ1 via `irq_enable(1)`.

### IRQ Handler

`keyboard_irq(regs)`:
1. Reads the scancode from port `0x60`.
2. If the byte is `0xE0`, sets the `extended` flag and returns.
3. If the high bit is set (`sc & 0x80`), the key was released; updates modifier state (`shift_down`, `ctrl_down`, `alt_down`) and returns.
4. For Caps Lock (`0x3A`), toggles `caps_lock`.
5. Translates the scancode using either `normal_map` or `shift_map` (128-entry arrays indexed by scancode).
6. Applies Caps Lock to alphabetic keys.
7. If Ctrl is held: maps `s`→`KEY_CTRL_S`, `q`→`KEY_CTRL_Q`, `f`→`KEY_CTRL_F`, `c`→`KEY_CTRL_C`.
8. Pushes the key code to the ring buffer via `push_key`.

Extended scancodes map to:

| Scancode | Key constant |
|---|---|
| `0x48` | `KEY_UP` |
| `0x50` | `KEY_DOWN` |
| `0x4B` | `KEY_LEFT` |
| `0x4D` | `KEY_RIGHT` |
| `0x47` | `KEY_HOME` |
| `0x4F` | `KEY_END` |
| `0x49` | `KEY_PAGE_UP` |
| `0x51` | `KEY_PAGE_DOWN` |
| `0x53` | `KEY_DELETE` |

### Key Constants

```c
KEY_NONE      = 0
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

### API

```c
void keyboard_init(void);
int  keyboard_getkey(void);       // blocking: halts CPU until a key arrives
int  keyboard_try_getkey(void);   // non-blocking: returns KEY_NONE if buffer empty
bool keyboard_ctrl_down(void);    // returns current Ctrl state
```

`keyboard_getkey` loops calling `arch_halt()` until a key is available. This is the primary mechanism by which the kernel waits for user input.

### Limitations

- No support for numeric keypad separate keys.
- No key auto-repeat handling at the driver level (the PS/2 controller handles typematic repeat, but no rate is configured).
- IRQ-based only; no polling mode.

---

## ATA PIO Driver

**Source**: `drivers/ata.c`  
**Header**: `include/pureunix/disk.h`

### Responsibilities

- Probes the ATA primary bus master drive on initialization.
- Provides PIO (Programmed I/O) sector read and write for the primary master disk.
- Registers an IRQ14 handler to clear the interrupt (reads the status register to acknowledge).

### Register Map

| Macro | Port | Description |
|---|---|---|
| `ATA_PRIMARY_IO` | `0x1F0` | Data register (and base for others) |
| `ATA_REG_DATA` | `+0` | 16-bit data transfer |
| `ATA_REG_ERROR` | `+1` | Error register |
| `ATA_REG_SECCOUNT0` | `+2` | Sector count |
| `ATA_REG_LBA0–LBA2` | `+3,+4,+5` | LBA bits 0–23 |
| `ATA_REG_HDDEVSEL` | `+6` | Drive select + LBA bits 24–27 |
| `ATA_REG_COMMAND` | `+7` | Command register |
| `ATA_REG_STATUS` | `+7` | Status register (read) |
| `ATA_PRIMARY_CTRL` | `0x3F6` | Control register |

### Initialization

`ata_init()`:
1. Registers `ata_irq` for vector 46 (IRQ14).
2. Enables IRQ14 via `irq_enable(14)`.
3. Sets control register to 0 (enable IRQs).
4. Selects master drive (`0xA0`).
5. Issues `ATA_CMD_IDENTIFY` (`0xEC`).
6. Checks if status is 0 (no drive) or if `ata_wait_ready` times out.
7. Reads and discards the 256-word IDENTIFY response.
8. Sets `primary.present = true` on success.

### Sector Read

`ata_read_sector(lba, buffer)`:
1. Calls `ata_select_lba(lba, 1)`: waits for BSY=0, selects LBA28 address.
2. Issues `ATA_CMD_READ_PIO` (`0x20`).
3. Calls `ata_wait_ready`: polls until BSY=0 and DRQ=1.
4. Reads 256 words (512 bytes) from the data port with `inw`.
5. Calls `ata_delay` (reads status 4 times to satisfy ATA timing requirements).

### Sector Write

`ata_write_sector(lba, buffer)`:
1. Selects LBA address.
2. Issues `ATA_CMD_WRITE_PIO` (`0x30`).
3. Waits for DRQ.
4. Writes 256 words to the data port with `outw`.
5. Issues `ATA_CMD_CACHE_FLUSH` (`0xE7`).
6. Waits for BSY=0.

### disk_device_t Interface

```c
typedef struct disk_device {
    const char *name;
    uint32_t    sector_size;   // always 512
    bool        present;
    int (*read)(uint32_t lba, uint8_t *buffer);
    int (*write)(uint32_t lba, const uint8_t *buffer);
} disk_device_t;
```

`ata_primary_master()` returns a pointer to the static `disk_device_t` for the primary master. The FAT16 driver uses the `read` and `write` function pointers to perform all disk I/O.

### Limitations

- Primary master only; secondary channel and slave drives are not probed.
- PIO only; DMA is not implemented.
- LBA28 only; disks over 128 GiB are not addressable.
- No error recovery; a failed read or write returns `-1` immediately.
- IRQ14 handler only clears the interrupt; no DRQ-based IRQ-driven I/O (all transfers are polled).
- No disk cache.
