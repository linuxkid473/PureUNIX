# USB (xHCI Host Controller + HID Boot Keyboard)

Native USB 3.x host controller support: PCI detection, xHCI bring-up, a host-controller-agnostic USB core, and a Boot Protocol HID keyboard driver, feeding the same shared input-event queue the PS/2 driver uses. Four new files (`drivers/xhci.c`, `drivers/usb.c`, `drivers/hid.c`, `drivers/input.c`) plus small, additive extensions to `drivers/pci.c` and `arch/i386/idt.c`.

Layer boundaries, bottom to top:

```
drivers/xhci.c   -- xHCI register/TRB/ring mechanics; PCI detection; port/slot/address bring-up
drivers/usb.c    -- host-controller-agnostic descriptors + generic enumeration, via usb_hc_ops_t
drivers/hid.c    -- Boot Protocol keyboard: SET_PROTOCOL, report decode
drivers/input.c  -- shared key-event queue (also fed by drivers/keyboard.c's PS/2 driver)
```

Only xHCI is implemented; `usb_hc_ops_t` (`include/pureunix/usb.h`) is the seam a future host controller (or `drivers/hid.c` a future mouse) would plug into, matching this codebase's existing convention (`pci_find`/`pci_walk` in `drivers/pci.c`) of "one real implementation behind a small interface" rather than a speculative multi-backend framework.

**Real-hardware status**: implemented to the xHCI 1.2 / USB 2.0 / USB HID 1.11 specifications and validated exhaustively under QEMU (`-device qemu-xhci -device usb-kbd`). Not yet tested on real hardware (the HP Pavilion 500-201 this was written for) — see Known Limitations for the specific things QEMU's `qemu-xhci` model doesn't exercise (Context Size, scratchpad buffers) that real Intel PCH silicon may.

---

## PCI: detecting the controller

**Source**: `drivers/pci.c` / **Header**: `include/pureunix/pci.h`

`pci_find()` (pre-existing) only matches by vendor/device ID, which doesn't work for "the xHCI controller" (varies by chipset and QEMU machine type). Added:

- `pci_find_by_class(class_code, subclass, prog_if, out)` — same `pci_walk()` traversal as `pci_find()`, matching class/subclass/prog-if (config offset 0x08) instead. xHCI is class `0x0C`, subclass `0x03`, prog-if `0x30`.
- 64-bit BAR support in `pci_bar_address()`/`pci_bar_size()`/`pci_bar_is_64bit()`: xHCI conventionally exposes a 64-bit BAR0 (memory-type bits 2:1 of the BAR dword == `0b10`), consuming two consecutive 32-bit BAR slots. Since this kernel is 32-bit with no PAE (`phys_addr_t`/`virt_addr_t` are `uint32_t`, `include/pureunix/types.h`), a 64-bit BAR whose high dword is nonzero (a region genuinely above 4GB) is treated as unusable — logged and rejected rather than silently truncated.
- `pci_enable_legacy_interrupts()` — clears the PCI command register's Interrupt Disable bit (bit 10), which `pci_enable_bus_mastering()` didn't touch and legacy INTx delivery depends on.

No MSI/MSI-X support exists anywhere in this kernel, so xHCI uses legacy INTx exclusively, exactly like `drivers/e1000.c`.

---

## xHCI controller

**Source**: `drivers/xhci.c` / **Header**: `include/pureunix/xhci.h`

### Capability parsing and BAR mapping

`xhci_init()` finds the controller via `pci_find_by_class()`, maps its BAR the same way `e1000_init()` does (`vmm_map_page()` per page, virt==phys — `vmm_map_page()` lazily allocates whatever page tables it needs, so this works unchanged for a BAR that, unlike the low-128MiB identity map, typically sits far above it), then parses the capability register block:

| Field | Source | Notes |
|---|---|---|
| `cap_length`, `hci_version` | dword at offset 0x00 | Read together as one dword — some MMIO implementations only reliably support dword-granular access to this register |
| `max_slots`, `max_ports`, `max_intrs` | HCSPARAMS1 | Clamped against `XHCI_MAX_SLOTS_SUPPORTED` (32) for this driver's own bookkeeping arrays |
| `max_scratchpad_bufs` | HCSPARAMS2 | Split 5+5-bit field (bits 31:27 low, bits 25:21 high) |
| `context_size` | HCCPARAMS1.CSZ | 32 or 64 bytes — see "Context Size" below |
| `page_size` | operational PAGESIZE register | Diagnostic only; every DMA structure here is sized to one 4096-byte page regardless |
| `dboff`, `rtsoff` | DBOFF / RTSOFF | Doorbell array / runtime register offsets from the MMIO base |

Also walks the extended capabilities list (`HCCPARAMS1.xECP << 2` from the MMIO base; each capability's own Next Pointer field is a dword offset relative to *that capability's own address*, not the base — an easy off-by-shift bug), logging every capability found.

### Bring-up sequence

Everything below runs in `xhci_init()`, before `arch_enable_interrupts()` (same pre-interrupt window `pci_scan()`/`e1000_init()` run in, `kernel/main.c`). Every polling loop is bounded (`XHCI_POLL_ITERATIONS`); on any failure, bring-up is aborted with a logged reason and `xhci_present()` reports `false` — the rest of boot proceeds unaffected, matching `e1000_init()`'s posture toward a missing/misbehaving NIC.

1. **USB Legacy Support (BIOS handoff)**, if the extended capability exists (it doesn't on QEMU's `qemu-xhci` — absence is the common path, not an error): sets the OS-owned bit, polls for the BIOS-owned bit to clear, then clears SMI-enable bits and acknowledges pending SMI status in USBLEGCTLSTS so a BIOS SMM handler doesn't keep firing SMIs after ownership changes hands.
2. **Reset**: if the controller is already running (observed in practice — GRUB's own USB keyboard probing leaves it running under QEMU), clears `USBCMD.RUN` and *best-effort* waits for `USBSTS.HCH`. This wait is **not fatal on timeout** — asserting `USBCMD.HCRST` next resets the controller regardless of its running state (the spec recommends a clean stop first but doesn't require it), and in practice this wait does time out fairly often under QEMU with a real USB device attached and actively being polled by firmware, without indicating anything actually wrong. The **following** HCRST wait (both `USBCMD.HCRST` self-clearing and `USBSTS.CNR` clearing) *is* fatal on timeout — that's the real correctness gate, and this runs before interrupts are enabled, so a stuck loop here would take down the whole boot.
3. **Context Size (CSZ)**: captured once from HCCPARAMS1 and used for every subsequent Slot/Endpoint/Input-Control context offset calculation (`device_context_entry()`/`input_context_entry()`, which stride by `context_size` — 32 or 64 bytes — never `sizeof()` a context struct). QEMU's `qemu-xhci` always reports CSZ=0; getting this wrong is invisible there and only surfaces as context corruption on real 64-byte-context hardware.
4. **DCBAA + scratchpad buffers**: the Device Context Base Address Array is one page of 64-bit pointers indexed by Slot ID (entries 1..MaxSlotsEn; entry 0 is reserved). If `HCSPARAMS2`'s scratchpad count is nonzero, a Scratchpad Buffer Array (one page of pointers) plus that many one-page scratchpad buffers are allocated, with the array's address stored in `DCBAA[0]`. Implemented unconditionally, not deferred — like CSZ, this is invisible on QEMU (which reports 0 scratchpad buffers) and required on real Intel PCH xHCI silicon.
5. **CONFIG.MaxSlotsEn**, then the **command ring** (one page, 255 usable TRBs + a mandatory Link TRB at slot 255 — every command/transfer ring reserves its last slot this way; only the event ring, whose wrap is defined by the ERST instead, uses all 256), and **CRCR** (Command Ring Control Register).
6. **Event ring + ERST + interrupter 0**: the event ring segment (256 usable TRBs, no Link TRB) and the Event Ring Segment Table (a *separate* one-page allocation — an array of `{base, size, reserved}` entries, not the same memory as the event ring segment it describes) are distinct allocations. Interrupter 0 is programmed with ERSTSZ=1, ERSTBA, an initial ERDP, IMODI=0 (no interrupt moderation delay), and IMAN.IE set.
7. **Legacy INTx registration** and **USBCMD.RUN**.
8. **Pre-interrupt self-test**: a No-Op command TRB, its completion checked by directly polling event-ring memory (interrupts structurally cannot fire yet — `arch_enable_interrupts()` hasn't run).

### 64-bit register writes: order matters

Every 64-bit xHCI register this driver writes (CRCR, DCBAAP, ERSTBA, ERDP) is split across two 32-bit MMIO accesses. `reg_write64()` writes **low dword first, then high**. This matters concretely for CRCR: confirmed against QEMU's own `hw/usb/hcd-xhci.c` source, the command-ring dequeue pointer is only re-latched from the *currently stored* low dword at the moment the **high** dword is written — writing high-then-low (the initially-chosen, plausible-sounding "commit-on-low-write" convention) latches a stale, pre-update low value, silently pointing the command ring at address 0 with no error indication beyond "the No-Op self-test times out." Low-first-high-last is confirmed safe for DCBAAP/ERSTBA/ERDP too (each just stores its two dwords independently, no write-order-dependent side effect), so it's used uniformly.

### Rings, TRBs, and the Cycle bit

A ring (`xhci_ring_t`) is one 4096-byte page of 16-byte TRBs (`XHCI_TRBS_PER_PAGE` = 256) plus software producer/consumer state. `ring_enqueue()` writes a TRB's Parameter/Status fields first, then its Control field (which carries the Cycle bit marking the TRB valid) last, with a compiler barrier between — the same reordering hazard `e1000_barrier()` guards against in `drivers/e1000.c`, just for TRB fields instead of descriptor fields. When the enqueue pointer reaches a ring's last slot, that slot is (re)written as a Link TRB (Toggle Cycle set, pointing back to slot 0) and the producer cycle state (PCS) toggles.

`process_event_ring()` is the **single shared event-ring consumer** for the driver's entire lifetime: called directly (bounded busy-poll) by `xhci_init()`'s pre-interrupt self-test, and by `xhci_irq()` once interrupts are enabled. The two call sites are never active simultaneously — interrupts structurally cannot fire before `arch_enable_interrupts()` runs, and after that point only `xhci_irq()` ever calls it — so there is exactly one consumer at any given time despite two entry points. **Invariant**: nothing invoked from inside `xhci_irq()` may call a blocking command/transfer-wait helper (`wait_command_irq()`/`wait_transfer_irq()`) — re-arming a transfer ring (enqueue + doorbell) is fire-and-forget and safe, but issuing a Configure/Evaluate Context command and blocking on its completion from IRQ context would deadlock exactly like the documented `ip_send()` bug (`docs/networking.md`).

### Shared legacy IRQ lines (a real bug found and fixed along the way)

`arch/i386/idt.c`'s `interrupt_register_handler()` used to be a flat "one handler per vector" array — registering xHCI's IRQ handler silently *replaced* whichever handler (e.g. `e1000_irq()`) was already registered on the same vector whenever two PCI devices shared a legacy IRQ line (which they routinely do — under QEMU's default PCI IRQ routing, xHCI and e1000 both land on IRQ 11). The real-world symptom was a livelock: once a shared-IRQ interrupt fired for the *other* device, only the newly-registered handler ran, the original device's actual interrupt cause was never acknowledged, and the level-triggered INTx line stayed asserted forever.

Fixed generally, not with a USB-specific workaround: `interrupt_register_handler()` now *adds* to a small fixed-capacity list per vector (`MAX_HANDLERS_PER_VECTOR = 4`), and `isr_dispatch()` invokes every handler registered on a vector for each interrupt. Every handler on a possibly-shared vector must therefore check its own device's pending-interrupt status first and do nothing if it isn't the source — `xhci_irq()` checks `IMAN.IP` before doing anything else; `e1000_irq()` was already effectively polite this way (reading `ICR` when nothing is pending just returns 0). See `docs/interrupts.md` for the updated `interrupt_register_handler()` contract.

---

## Device enumeration

**Source**: `drivers/xhci.c` (`xhci_enumerate()`, port/slot/address mechanics) + `drivers/usb.c` (generic descriptor/enumeration logic)

Runs after `arch_enable_interrupts()` (`kernel/main.c`), unlike `xhci_init()`'s pre-interrupt bring-up — port status, slot commands, and control transfers all rely on the real interrupt-driven completion path (`xhci_irq()`), not direct event-ring polling.

1. A **second, independent No-Op self-test**, this time through `xhci_irq()` + `wait_command_irq()` (blocks via `arch_halt()`, identical pattern to `keyboard_getkey()`) — proves INTx routing, `USBCMD.INTE`, and `IMAN.IE` are all correctly wired end-to-end, distinct from the memory-polled test `xhci_init()` already ran.
2. **Supported Protocol Capability walk** (`find_usb2_protocol()`): maps PORTSC index ranges to USB major revision. **Only the USB2-labeled port range (LS/FS/HS) is enumerated** — see Known Limitations for why USB3 is out of scope entirely.
3. For each USB2 port with `PORTSC.CCS` set (device connected): **port reset** (`PORTSC.PR`, polled `PORTSC.PRC`, `PED` confirmed), then `usb_enumerate_port()`:
   - **Enable Slot** (Slot Type from the Supported Protocol Capability).
   - **Address Device**, two-stage BSR (Block Set Address Request) dance: BSR=1 with a port-speed-based `bMaxPacketSize0` guess (Low Speed always 8, High Speed always 64, Full Speed conservatively guessed at 8) → an 8-byte control-transfer read of the real device descriptor → Evaluate Context if the guess was wrong → BSR=0, which is when the controller actually issues `SET_ADDRESS` on the wire.
   - **GET_DESCRIPTOR(Device)**, full 18 bytes: logs VID/PID/class/subclass/protocol/config count.
   - **GET_DESCRIPTOR(Configuration)**: a 9-byte header read for `wTotalLength`, then a full read (capped at `USB_MAX_CONFIG_DESC_SIZE` = 256 bytes), walked generically by `bLength`/`bDescriptorType` to log every interface and endpoint and record the first Interrupt IN endpoint found (the only endpoint type this driver's HID support needs).
   - **String descriptors** for manufacturer/product, if present (UTF-16LE decoded lossily to ASCII — see `usb.c`'s `decode_string_ascii()`).
   - **SET_CONFIGURATION**.
   - **Configure Endpoint** for the interrupt endpoint found above, if any: builds an Input Context entry for the endpoint's DCI (`ep_num*2+1`), converts its `bInterval` to xHCI's Interval encoding (`compute_interval()` — the LS/FS-vs-HS conversion differs: LS/FS is a 1ms-frame count needing both a unit change and a power-of-2 search, HS is already a 0-based microframe exponent needing only a -1).
4. `hid_try_attach()` (`drivers/hid.c`) is offered every enumerated device; it's a silent no-op for anything that isn't a Boot Protocol keyboard interface.

Control transfers (`control_transfer()`, `drivers/xhci.c`) build a Setup/[Data]/Status Stage TRB sequence on the target slot's EP0 ring. The Setup Stage TRB uses **IDT** (Immediate Data) — the 8 raw setup bytes are embedded directly in the TRB's Parameter fields, not pointed to via a buffer, an xHCI-specific quirk unlike EHCI/UHCI queue-head setup. The Status Stage TRB always carries **IOC** — omitting it means the transfer completes with no Transfer Event ever posted, silently hanging the waiting caller forever.

### Doorbell / DCI convention

Doorbell register 0 targets the command ring. Doorbell register N (N = Slot ID) targets a specific endpoint via **Device Context Index (DCI)**: DCI=1 is always EP0 (bidirectional control); for endpoint number *e* direction *d*, `DCI = e*2 + (d==IN ? 1 : 0)`.

---

## USB core

**Source**: `drivers/usb.c` / **Header**: `include/pureunix/usb.h`

Host-controller-agnostic: standard descriptor structs (device/config/interface/endpoint/string, USB 2.0 spec chapter 9, all `__attribute__((packed))` matching the real wire layout) and the generic enumeration sequence above, driven entirely through `usb_hc_ops_t` — a small vtable (`enable_slot`, `address_device`, `control_transfer`, `configure_endpoint`, `submit_interrupt_transfer`) `drivers/xhci.c` implements. `usb_device_t` carries everything learned about one enumerated device (identity, strings, and the first Interrupt IN endpoint found) out to a class driver like `drivers/hid.c`.

`submit_interrupt_transfer()` is the odd one out in the vtable: every other operation blocks the caller until completion (this kernel is cooperatively scheduled with no async I/O model, so a synchronous vtable matches every other driver interface), but an interrupt endpoint needs to stay armed and repeating indefinitely. It submits once, then the host controller automatically re-arms the same buffer after every completion, calling a callback each time — from IRQ context (see `drivers/xhci.c`'s IRQ-context invariant above), so the callback must be fast and non-blocking.

---

## HID Boot Protocol keyboard

**Source**: `drivers/hid.c` / **Header**: `include/pureunix/hid.h`

Deliberately **not** a general HID report-descriptor parser — Boot Protocol exists specifically so a BIOS/bootloader (and, here, this kernel) can talk to a HID keyboard without implementing the full HID Report Descriptor language. Every Boot Protocol keyboard is contractually guaranteed to accept `SET_PROTOCOL(Boot)` (a HID class request, `bRequest=0x0B`, `wValue=0`) and then produce exactly the fixed 8-byte report format below, regardless of what its real (non-Boot) report descriptor says.

`hid_try_attach()` checks `usb_device_t.interface_class/subclass/protocol` for HID (3) / Boot (1) / Keyboard (1); if matched, issues `SET_PROTOCOL(Boot)` and arms a repeating interrupt transfer via `submit_interrupt_transfer()`.

### Report decoding

Byte 0 is a modifier bitmap (Ctrl/Shift/Alt/GUI × Left/Right); byte 1 is reserved; bytes 2-7 are up to six simultaneously-held key usage IDs (0 = empty slot, 1 = rollover error, both ignored). `decode_boot_report()` diffs the just-received report against the previous one and pushes a key event only for usage IDs that are **newly** present — matching `drivers/keyboard.c`'s PS/2 driver's press-only event model exactly (a still-held key repeated in every ~8-10ms report doesn't flood the queue; key repeat is a documented future addition for both keyboard types, not yet implemented for either). `translate_usage()` maps USB HID Keyboard/Keypad usage IDs (0x04-0x38, the standard US-layout alphanumeric/punctuation block, plus 0x4A-0x52 for the arrow/navigation cluster) to the same `KEY_*` constants and ASCII the PS/2 driver produces, including identical Shift/Caps Lock/Ctrl-combo handling.

---

## Generic input layer

**Source**: `drivers/input.c` / **Header**: `include/pureunix/input.h`

The circular key-event queue `drivers/keyboard.c` used to own directly, moved here so a second producer could be added without duplicating it: `input_push_key()`, `input_try_getkey()`, `input_getkey()` (blocking via `arch_halt()`, identical to the old `keyboard_getkey()`). `drivers/keyboard.c`'s PS/2 driver and `drivers/hid.c`'s USB HID driver both call `input_push_key()`; `keyboard_getkey()`/`keyboard_try_getkey()` are kept as thin forwarding wrappers so **every existing caller** (`drivers/tty.c`, the built-in shell) needed zero changes.

---

## Diagnostic logging

Every enumeration stage the task's own spec calls out is logged, prefixed by subsystem:

```
xhci: found 1b36:000d at 00:04.0 (irq 11)                    <- PCI: vendor/device/BAR/IRQ
xhci: BAR0 (64-bit) at phys=0xfebf0000 size=16 KiB
xhci: version=1.0 ports=8 slots=64 intrs=16                  <- xHCI: version/ports/slots
xhci: page_size=4096 context_size=32 scratchpad_bufs=0 ac64=1 <- xHCI: capabilities
xhci: controller running
xhci: command ring self-test OK
xhci: interrupt-driven command self-test OK
xhci: USB2 ports 5..8 (slot type 0)
xhci: port 5: device connected (portsc=...)                  <- enumeration: connected
xhci: port 5: reset complete                                 <- enumeration: reset
usb: port 5: slot 1 assigned                                 <- enumeration: slot assigned
usb: slot 1: address assigned                                <- enumeration: address assigned
usb: slot 1: VID=0627 PID=0001 class=... configs=1            <- enumeration: VID/PID
usb: interface 0: class=03 subclass=01 protocol=01
usb: endpoint 81: attributes=03 max_packet=8 interval=7
usb: slot 1: manufacturer="QEMU"                              <- enumeration: manufacturer
usb: slot 1: product="QEMU USB Keyboard"                      <- enumeration: product
usb: slot 1: configuration 1 selected                         <- enumeration: configuration selected
usb: slot 1: interrupt endpoint 81 configured (interface 0, max_packet=8)
hid: slot 1: Boot Protocol keyboard attached (interface 0, endpoint 81)
```

No raw-boot-report or translated-key-event debug logging is left compiled in (the task's "debug mode" logging was used extensively during bring-up — see the commit history's temporary `printf("... DEBUG ...")` additions — and removed once each stage was confirmed working, per "no debug hacks left behind").

---

## Known Limitations

- **32-bit / non-PAE physical addressing only.** A 64-bit BAR or DMA address whose high dword is nonzero is rejected, not truncated. Every physical address this kernel can produce is under the 128MiB identity-mapped region anyway, so this is a hard architectural ceiling, not a soft one.
- **Legacy INTx only — no MSI/MSI-X.** This kernel has no MSI infrastructure at all; adding it was out of scope (the task's own instructions permit falling back to legacy interrupts).
- **Boot Protocol HID only — no general report-descriptor parser.** Non-Boot-Protocol HID devices (most mice, many specialty keyboards) aren't supported. A general parser is a substantially larger undertaking Boot Protocol exists specifically to avoid needing.
- **USB2 ports only — no SuperSpeed (USB3) enumeration, no hub support.** The Supported Protocol Capability walk only handles Major Revision 2 port ranges; USB3-only ranges are logged and skipped. No hub (multi-tier topology) support exists — only devices plugged directly into a root port are enumerated.
- **No bulk/isochronous endpoint support.** Only Control (EP0) and Interrupt IN are implemented — sufficient for a Boot Protocol keyboard, not for mass storage (bulk) or audio/video (isochronous) devices.
- **No key repeat.** Both the PS/2 and USB HID keyboard drivers are press-event-only; holding a key produces one event, not a repeating stream. Documented as a future addition in the original task spec.
- **No hot-unplug handling.** Enumeration is boot-time only (`xhci_enumerate()` runs once, after `arch_enable_interrupts()`, before the login prompt); a device connected afterward, or disconnected mid-session, isn't handled. Port Status Change Events are read (during the boot-time scan) but not consumed via an ongoing interrupt-driven path.
- **QEMU-validated only.** Every milestone was built, booted, and exhaustively tested under QEMU (`-device qemu-xhci -device usb-kbd`), including scenarios specifically chosen because they're invisible under QEMU's `qemu-xhci` model (Context Size=64, nonzero scratchpad buffers) — implemented to spec anyway, but not yet confirmed against real hardware. Not claimed to work on the HP Pavilion 500-201 until tested there.

---

## Testing

QEMU invocation used throughout bring-up (adds an xHCI controller with a USB HID keyboard to the existing IDE/e1000 setup from `make run`):

```
qemu-system-i386 -m 128M -cdrom build/pureunix.iso -boot d \
  -drive file=build/pureunix.img,format=raw,if=ide,index=0 \
  -drive file=build/ext2.img,format=raw,if=ide,index=1 \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -device qemu-xhci,id=xhci0 -device usb-kbd,bus=xhci0.0 \
  -serial stdio
```

Regression posture verified repeatedly (5+ consecutive boots per configuration, no observed flakiness after the fixes documented above): boots identically with no xHCI controller present, with a controller present but no USB device attached, and with a USB keyboard attached; PS/2 keyboard input is unaffected in every configuration; e1000/ARP/ICMP self-tests and FAT16/EXT2 mounting are unaffected by xHCI's PCI IRQ sharing with e1000 (previously a real livelock — see the IRQ-sharing fix above).
