# Drivers

## VGA Text Driver

**Source**: `drivers/vga.c`  
**Header**: `include/pureunix/vga.h`

### Responsibilities

- Manages the 80×25 text grid, backed by either the legacy VGA text mode framebuffer at `0xB8000`, or — when GRUB/multiboot2 granted a usable linear framebuffer (`drivers/framebuffer.c`) at least as big as the grid — a software text renderer that draws each cell as a glyph bitmap onto that framebuffer instead (`use_fb`; see `vga_init()`). `cell_char[][]`/`cell_attr[][]` hold the grid's true contents either way, so both backends can be driven by the exact same escape-sequence parser below.
- Interprets a subset of ANSI/VT100 escape sequences for color and cursor control.
- Synchronizes all output to the serial port (every character written to VGA is also sent to COM1) — since `serial_putc()` is called unconditionally before any escape-sequence interpretation, the serial mirror reflects the raw byte stream regardless of whether this driver's own parsing of it is complete or correct.
- Provides the hardware cursor position via CRTC registers in text mode, or a software-drawn inverted-cell overlay in framebuffer mode.

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

The parser is a two-state machine: state 1 (saw `\033`), state 2 (saw `[`, accumulating parameters). `ansi_params()` parses up to 4 `;`-separated decimal parameters out of the accumulated buffer (an empty/omitted parameter defaults to 0) before `ansi_execute()` dispatches on the final byte:

| Sequence | Action |
|---|---|
| `\033[...m` | SGR: set foreground/background color |
| `\033[2J` | Clear screen (`vga_clear`) |
| `\033[K` | Erase from the cursor to end of line (`erase_current_line`) |
| `\033[row;colH` / `\033[...f` | Move cursor to `(row-1, col-1)`, 1-indexed like real ANSI CUP; a bare `\033[H` homes to `(0,0)` |
| `\033[colG` | Move cursor to column `col-1` on the current row (CHA) |
| `\033[top;botr` | DECSTBM: confine scrolling to `[top-1, bot-1]` (0-indexed, inclusive); a bare `\033[r` resets it to the whole screen. Homes the cursor, matching real terminals. |
| `\033[nL` / `\033[nM` | VT100 IL/DL: insert/delete `n` blank lines at the cursor row, shifting the rest of the scroll region down/up (`insert_lines()`/`delete_lines()`) |

This is the actual set Neatvi (`user/vi/term.c`, vendored under `user/vi/` — see `docs/userland.md`) needs for full-screen redraws: it positions with `H`/`G`, confines scrolling to everything above its status line with `r`, and uses `K` to blank leftover trailing characters after writing each line's real content.

SGR color mapping:

| Range | Meaning |
|---|---|
| `0` or empty | Reset to light grey on black |
| `30–37` | Set foreground to VGA color 0–7 |
| `90–97` | Set foreground to VGA bright color 8–15 |
| `40–47` | Set background to VGA color 0–7 |

The 8 basic ANSI color numbers (black/red/green/yellow/blue/magenta/cyan/white) count in a different order than the VGA/CGA palette this driver indexes (black/blue/green/cyan/red/magenta/brown/white) — `ansi_to_vga[]` remaps one to the other before indexing `vga_rgb_palette[]`, so e.g. ANSI "34" (blue) actually renders as VGA color 1 (blue), not VGA color 4 (red).

Multiple parameters separated by `;` are processed left to right.

### Scrolling

`scroll_top`/`scroll_bottom` (0-indexed, inclusive; reset to the whole screen by `vga_init()`/`vga_clear()`, and set by DECSTBM above) bound where `newline()`/`scroll()` are allowed to shift content: when the cursor is at `scroll_bottom` and a `\n` (or column-wrap) occurs, `scroll()` shifts `[scroll_top, scroll_bottom]` up by one row and blanks the newly exposed bottom row of *that region only* — rows outside it (e.g. a fixed status line below `scroll_bottom`) are left untouched. With the default full-screen region this reproduces the old unconditional "shift rows 1–24 into 0–23" behavior exactly.

Without this, a program that reserves the last line as a status bar (again, Neatvi) would have that line scroll away with the rest of the buffer on every line feed.

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
- No 256-color (`\033[38;5;Nm`/`\033[48;5;Nm`) or truecolor SGR support — only the 16 basic/bright ANSI color numbers.
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

---

## PCI Bus

**Source**: `drivers/pci.c`
**Header**: `include/pureunix/pci.h`

### Responsibilities

- Reads and writes PCI configuration space via the legacy I/O-port mechanism (`0xCF8`/`0xCFC`), the same mechanism every x86 PC (including QEMU's `i440fx`/`q35` machines) supports without needing ACPI/MCFG.
- Enumerates every bus (0-255), slot (0-31), and function (0-7 for multi-function devices, detected via header-type bit 7), printing vendor/device/class for each device found.
- Locates a specific device by vendor/device ID (`pci_find()`), used by `e1000_init()` to find the NIC without hardcoding a bus/slot/function.
- Reads a BAR's physical base address and size (via the standard "write all-ones, read back, restore" probe), and sets the Bus Master Enable + Memory Space Enable bits in the command register.

### `pci_device_t`

```c
typedef struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  header_type;
    uint8_t  interrupt_line;
} pci_device_t;
```

### API

```c
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value);

void pci_scan(void);   // prints every device found, called from kernel_main()
bool pci_find(uint16_t vendor_id, const uint16_t *device_ids, int count, pci_device_t *out);
void pci_enable_bus_mastering(const pci_device_t *dev);

phys_addr_t pci_bar_address(const pci_device_t *dev, int index);  // 0 for I/O-space BARs
uint32_t    pci_bar_size(const pci_device_t *dev, int index);
```

### Limitations

- No PCI Express extended configuration space (MCFG/ECAM) support; legacy port I/O only covers the first 256 bytes of each device's config space, which is all any device this kernel drives needs.
- No support for PCI bridges beyond simply enumerating past them (bus 0-255 are scanned unconditionally rather than following bridge topology).

---

## Intel e1000 NIC Driver

**Source**: `drivers/e1000.c`
**Header**: `include/pureunix/e1000.h`

### Responsibilities

- Locates an Intel e1000-family Gigabit Ethernet controller on the PCI bus (`pci_find()` against a table of classic e1000 device IDs — 82540/82541/82543-82547; deliberately excludes 82574L/e1000e-family IDs, which use a different register layout).
- Enables PCI bus mastering and memory space, then maps the device's MMIO BAR0 (typically `0xFEBC0000` on QEMU, 128 KiB) into the identity-mapped address space via `vmm_map_page()` — BAR0 sits far above the 128 MiB range `vmm_init()` maps at boot, so it's mapped lazily here instead.
- Resets the controller (`CTRL.RST`), reads the MAC address (via the EEPROM if present, otherwise falling back to the pre-populated `RAL0`/`RAH0` registers — QEMU always populates these even without an emulated EEPROM), and programs the primary receive-address filter with it.
- Allocates fixed-size RX/TX descriptor rings and packet buffers as static (kernel-image, BSS) arrays — the same trick `kernel/vmm.c`'s `page_directory`/`identity_tables` use — so each ring is one guaranteed-contiguous, naturally-aligned physical block, and (being inside the identity-mapped low 128 MiB) each buffer's own address doubles as the physical address the NIC's DMA engine needs.
- Enables transmit and receive (`TCTL`/`RCTL`), registers an IRQ handler on the PCI-assigned interrupt line that acknowledges the interrupt cause register (mirroring `ata_irq()`'s role for the ATA driver), and enables the RX-timer and link-status-change interrupt sources.
- Runs a boot-time self-test: sends a broadcast ARP probe for QEMU's SLIRP gateway address (`10.0.2.2`) and polls briefly for a reply, exercising the real TX and RX datapath. A missing reply is logged, not treated as an error — it depends on the network backend being reachable, whereas the TX side is already confirmed independently (`e1000_send()` only returns success once the NIC's own descriptor head has advanced past the frame).

### Register Map (byte offsets into MMIO BAR0)

| Register | Offset | Purpose |
|---|---|---|
| `CTRL` | `0x0000` | Device control (reset, link-up, speed/duplex) |
| `STATUS` | `0x0008` | Link status, speed, duplex |
| `EEPROM` (EERD) | `0x0014` | EEPROM read register |
| `ICR` / `IMS` / `IMC` | `0x00C0`/`0xD0`/`0xD8` | Interrupt cause / mask set / mask clear |
| `RCTL` | `0x0100` | Receive control |
| `TCTL` / `TIPG` | `0x0400`/`0x0410` | Transmit control / inter-packet gap |
| `RDBAL`/`RDBAH`/`RDLEN`/`RDH`/`RDT` | `0x2800`-`0x2818` | RX ring base/length/head/tail |
| `TDBAL`/`TDBAH`/`TDLEN`/`TDH`/`TDT` | `0x3800`-`0x3818` | TX ring base/length/head/tail |
| `MTA` | `0x5200` (128 entries) | Multicast table array |
| `RAL0`/`RAH0` | `0x5400`/`0x5404` | Primary receive address (our MAC) |

### Descriptor Rings

32 RX descriptors and 8 TX descriptors, each with a dedicated 2048-byte packet buffer — plenty for a standard 1518-byte Ethernet frame. Both rings use the classic 16-byte legacy descriptor format (`e1000_rx_desc_t`/`e1000_tx_desc_t`), matching the register layout above.

### API

```c
void e1000_init(void);                              // called from kernel_main(), before vfs_init()
bool e1000_present(void);
void e1000_get_mac(uint8_t mac[6]);
int  e1000_send(const void *data, uint16_t len);     // 0 on success, -1 on failure/timeout
int  e1000_receive(void *buf, uint16_t buf_len);     // frame length, 0 if none ready, -1 if buf too small
void e1000_set_rx_handler(void (*handler)(void));    // called from the RX interrupt; see net/eth.c
void e1000_selftest(void);                           // called from kernel_main(), after arch_enable_interrupts()
void e1000_dump_stats(void);                         // diagnostic: counters + live register snapshot
```

`e1000_send()` busy-polls the chosen TX descriptor's own completion (`DD`) bit before reusing it, so back-to-back sends never overwrite a frame still in flight. `e1000_receive()` is non-blocking: it checks the next RX descriptor's `DD` bit, and if set, copies the frame out and immediately recycles the descriptor back to the NIC by advancing `RDT`.

`e1000_init()` also programs the PCI Cache Line Size register (config offset `0x0C`) to `0x10` (16 dwords = 64 bytes, the standard x86 cache line size) — real hardware uses this to pick efficient DMA burst lengths for descriptor writeback; QEMU's e1000 model doesn't appear to condition behavior on it, but it's correct to set regardless of emulation.

`e1000_dump_stats()` prints running counters (`tx_ok`, `tx_timeout`, `rx_ok`, `rx_dropped_size`, `irq_count`, `irq_rxt0_count` — maintained by `e1000_send()`/`e1000_receive()`/`e1000_irq()`) plus a live snapshot of `RDH`/`RDT`/`TDH`/`TDT`/`ICR`/`IMS`/`STATUS`; `e1000_selftest()` calls it automatically at the end of its run. Reading `ICR` here clears its pending cause bits, same side effect as `e1000_irq()` itself — diagnostic-only.

### Limitations

- Single device only (first Intel e1000-family NIC found); no multi-NIC support.
- Classic e1000 register layout only (82540/82541/82543-82547); e1000e-family devices (82574L, 0x10D3, and newer) are not matched, since they use a different register set.
- No jumbo frame support (`RCTL.LPE` unset; frames are capped by the 2048-byte buffer size).
- No checksum offload; `CTRL.RXCSUM`/TX checksum context descriptors are not used.
- No link-layer (ARP/IP) stack above the raw `e1000_send()`/`e1000_receive()` frame interface — callers must build their own Ethernet headers.
