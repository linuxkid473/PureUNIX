#include <pureunix/arch.h>
#include <pureunix/memory.h>
#include <pureunix/pci.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/hid.h>
#include <pureunix/usb.h>
#include <pureunix/vmm.h>
#include <pureunix/xhci.h>

static xhci_controller_t ctrl;

/* Set only while a control transfer submitted by control_transfer() is
 * outstanding -- mirrors xhci_pending_command_t below, but matched against
 * a Transfer Event's TRB pointer (the Status Stage TRB) instead of a
 * Command Completion Event's. Only one control transfer is ever
 * outstanding at a time (every caller blocks until completion), so a
 * single global slot is sufficient here too. */
typedef struct xhci_pending_transfer {
    phys_addr_t trb_phys;
    volatile bool done;
    volatile uint32_t completion_code;
} xhci_pending_transfer_t;

static xhci_pending_transfer_t pending_xfer;

/* One entry per (slot, DCI) with a repeating interrupt-IN transfer armed
 * via submit_interrupt_transfer() -- unlike pending_cmd/pending_xfer (each
 * a single global "one outstanding operation" slot, since command/control-
 * transfer submission always blocks the caller until completion), any
 * number of endpoints across any number of slots can have an interrupt
 * transfer armed simultaneously and indefinitely, so this needs one entry
 * per endpoint rather than one global slot. Indexed [slot_id][dci];
 * row/column 0 are unused (neither Slot ID nor DCI 0 is ever valid). */
typedef struct xhci_interrupt_listener {
    bool active;
    uint8_t endpoint_address;
    void *buf;
    uint16_t length;
    usb_interrupt_callback_t callback;
    void *ctx;
} xhci_interrupt_listener_t;

static xhci_interrupt_listener_t
    interrupt_listeners[XHCI_MAX_SLOTS_SUPPORTED + 1][XHCI_MAX_DCI];

/* Set only while a command submitted via submit_command() is outstanding --
 * process_event_ring() (called either by the pre-interrupt polling helper
 * in this milestone, or by xhci_irq() once interrupts are enabled in a
 * later milestone) matches a Command Completion Event's TRB pointer against
 * this to know which caller to wake. Only one command is ever outstanding
 * at a time in this driver -- every command path blocks until completion
 * before issuing the next one -- so a single global slot is sufficient. */
typedef struct xhci_pending_command {
    phys_addr_t trb_phys;
    volatile bool done;
    volatile uint32_t completion_code;
    volatile uint32_t slot_id;
} xhci_pending_command_t;

static xhci_pending_command_t pending_cmd;

static uint32_t reg_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static void reg_write32(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

/* Writes a 64-bit register pair as two 32-bit MMIO accesses, low dword
 * first. This driver never has a nonzero high dword (see pci_bar_address()'s
 * comment on this kernel's 32-bit/non-PAE physical addressing limit), but
 * the write order still matters for CRCR specifically: the xHCI spec (and
 * QEMU's qemu-xhci model, confirmed against its source) treats the *high*
 * dword write as the trigger that latches the internal command-ring
 * dequeue pointer from whatever is currently in the low dword -- writing
 * high before low latches a stale (pre-update) low value, silently pointing
 * the command ring at the wrong address with no error indication beyond a
 * command that mysteriously never completes. Low-first-high-last is safe
 * for every other 64-bit register pair this driver writes (DCBAAP, ERSTBA,
 * ERDP each just store their two dwords independently with no
 * write-triggered side effect tied to ordering), so this convention is used
 * uniformly rather than special-casing CRCR alone. */
static void reg_write64(volatile uint8_t *base, uint32_t offset, uint32_t lo, uint32_t hi)
{
    *(volatile uint32_t *)(base + offset) = lo;
    *(volatile uint32_t *)(base + offset + 4) = hi;
}

/* Compiler-only memory barrier -- same reasoning as e1000_barrier()
 * (drivers/e1000.c): TRB fields and DCBAA/ERST entries are plain structs,
 * so nothing stops the compiler from reordering their stores relative to a
 * later doorbell/register write in the same function. x86 doesn't reorder
 * normal stores at the hardware level, so a compiler barrier is sufficient
 * (no LOCK-prefixed fence instruction needed). */
static inline void xhci_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* Maps every page the BAR spans into the kernel's identity-mapped address
 * space (virt == phys), exactly like e1000_init()'s BAR0 mapping
 * (drivers/e1000.c) -- xHCI's BAR is typically a 64-bit BAR sitting far
 * above the low 128MiB identity map, but vmm_map_mmio_uc() lazily allocates
 * whatever page tables it needs for any address, so the same call works
 * here unchanged.
 *
 * vmm_map_mmio_uc(), not a bare vmm_map_page(): this BAR must be strictly
 * Uncacheable at the paging level, not merely "whatever the BIOS's MTRRs
 * happen to say" -- see the comment on vmm_map_framebuffer_wc() in
 * kernel/vmm.c for the real-hardware bug that came from this xHCI mapping
 * relying on that ambiguity (a neighboring framebuffer PD chunk had already
 * been blanket-marked Write-Combining, and this BAR happened to land in the
 * unused padding of that same chunk). doorbell/ERDP writes below
 * (xhci_barrier() and friends) assume true hardware UC ordering with only a
 * compiler barrier -- that assumption is only valid because this call now
 * forces it explicitly instead of hoping for it. */
static volatile uint8_t *map_bar(phys_addr_t base, uint32_t size)
{
    uint32_t pages = (size + PUREUNIX_PAGE_SIZE - 1) / PUREUNIX_PAGE_SIZE;
    for (uint32_t i = 0; i < pages; ++i) {
        phys_addr_t page = base + i * PUREUNIX_PAGE_SIZE;
        vmm_map_mmio_uc(page, page, PAGE_WRITE);
    }
    return (volatile uint8_t *)(uintptr_t)base;
}

/* Allocates one zeroed, page-aligned physical frame for a DMA structure.
 * Every xHCI structure this driver needs (DCBAA, scratchpad array/buffers,
 * command/transfer rings, event ring segment, ERST, Input/Device Contexts)
 * fits in a single 4096-byte page, and pmm_alloc_frame() frames come from
 * the identity-mapped low 128MiB (kernel/pmm.c), so virt == phys always and
 * no separate contiguous-DMA allocator is needed -- see docs/usb.md. */
static void *alloc_dma_page(void)
{
    phys_addr_t frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    void *ptr = (void *)(uintptr_t)frame;
    memset(ptr, 0, PUREUNIX_PAGE_SIZE);
    return ptr;
}

static uint32_t op_read32(uint32_t offset)
{
    return reg_read32(ctrl.op_base, offset);
}

static void op_write32(uint32_t offset, uint32_t value)
{
    reg_write32(ctrl.op_base, offset, value);
}

static void op_write64(uint32_t offset, uint32_t lo, uint32_t hi)
{
    reg_write64(ctrl.op_base, offset, lo, hi);
}

/* This driver only ever uses interrupter 0 (the "primary interrupter"),
 * matching the task's single-interrupter scope -- these helpers hardcode
 * that rather than taking an interrupter index nobody would ever vary. */
static uint32_t rt0_read32(uint32_t offset)
{
    return reg_read32(ctrl.rt_base, XHCI_RT_IR0_BASE + offset);
}

static void rt0_write32(uint32_t offset, uint32_t value)
{
    reg_write32(ctrl.rt_base, XHCI_RT_IR0_BASE + offset, value);
}

static void rt0_write64(uint32_t offset, uint32_t lo, uint32_t hi)
{
    reg_write64(ctrl.rt_base, XHCI_RT_IR0_BASE + offset, lo, hi);
}

static void doorbell_ring(uint8_t slot_id, uint8_t target)
{
    volatile uint32_t *db = (volatile uint32_t *)ctrl.db_base + slot_id;
    *db = target; /* Stream ID field (bits 31:16) left 0 -- see xhci.h */
}

static const char *xecp_name(uint32_t id)
{
    switch (id) {
    case XHCI_XECP_ID_USB_LEGACY_SUPPORT: return "USB Legacy Support";
    case XHCI_XECP_ID_SUPPORTED_PROTOCOL: return "Supported Protocol";
    default: return "unrecognized";
    }
}

/* Walks the extended capabilities list starting at
 * mmio + (HCCPARAMS1.xECP << 2). xECP is a dword offset from the MMIO base;
 * each capability's own Next Capability Pointer field is a dword offset
 * relative to *that capability's own address* -- both scaled by 4, and easy
 * to conflate if not kept distinct. A zero xECP means no extended
 * capabilities are present at all, a normal, expected outcome (QEMU's
 * qemu-xhci model, for instance, has no USB Legacy Support capability).
 *
 * If `log` is set, every capability found is printed regardless of `want_id`
 * (used by xhci_init()'s initial capability dump). Returns the offset of
 * the first capability matching `want_id`, or 0 if none was found -- 0 is
 * never a valid capability offset (it's always inside the capability
 * register block itself), so it doubles as a safe "not found" sentinel. */
static uint32_t find_extended_capability(uint32_t hccparams1, uint32_t want_id, bool log)
{
    uint32_t xecp_dwords = XHCI_HCCPARAMS1_XECP(hccparams1);
    if (xecp_dwords == 0) {
        if (log) {
            printf("xhci: no extended capabilities\n");
        }
        return 0;
    }

    uint32_t offset = xecp_dwords * 4U;
    uint32_t guard = 0;
    uint32_t found = 0;
    while (offset != 0 && guard < 64) {
        uint32_t dw = reg_read32(ctrl.mmio, offset);
        uint32_t id = XHCI_XECP_ID(dw);
        uint32_t next_dwords = XHCI_XECP_NEXT_DWORDS(dw);
        if (log) {
            printf("xhci: extended cap id=%u (%s) at offset=%x\n", id, xecp_name(id), offset);
        }
        if (id == want_id && found == 0) {
            found = offset;
        }
        if (next_dwords == 0) {
            break;
        }
        offset += next_dwords * 4U;
        ++guard;
    }
    return found;
}

/* USB Legacy Support BIOS handoff (sec 7.1). Absence of the capability is
 * the common/expected path (QEMU's qemu-xhci doesn't implement it) and is
 * not an error -- only real hardware behind a legacy BIOS/SMM USB keyboard
 * emulation layer needs this. The poll is bounded: a BIOS that never
 * responds gets a logged warning, not a boot hang, and bring-up proceeds
 * regardless (taking ownership is best-effort, not a hard prerequisite for
 * the controller to work). */
static void legacy_bios_handoff(uint32_t hccparams1)
{
    uint32_t cap_off = find_extended_capability(hccparams1, XHCI_XECP_ID_USB_LEGACY_SUPPORT, false);
    if (cap_off == 0) {
        printf("xhci: no USB Legacy Support capability (no BIOS handoff needed)\n");
        return;
    }

    uint32_t usblegsup = reg_read32(ctrl.mmio, cap_off);
    reg_write32(ctrl.mmio, cap_off, usblegsup | XHCI_USBLEGSUP_OS_OWNED);

    bool released = false;
    for (uint32_t i = 0; i < XHCI_POLL_ITERATIONS; ++i) {
        usblegsup = reg_read32(ctrl.mmio, cap_off);
        if (!(usblegsup & XHCI_USBLEGSUP_BIOS_OWNED)) {
            released = true;
            break;
        }
    }
    printf("xhci: BIOS handoff %s (USBLEGSUP=%x)\n",
           released ? "complete" : "timed out -- proceeding anyway", usblegsup);

    /* Regardless of handoff success, disable SMI generation and clear any
     * pending SMI status in USBLEGCTLSTS -- otherwise a buggy BIOS SMM
     * handler can keep firing SMIs after ownership has changed hands. */
    uint32_t legctlsts = reg_read32(ctrl.mmio, cap_off + 4);
    legctlsts &= ~XHCI_USBLEGCTLSTS_SMI_ENABLE_MASK;
    legctlsts |= XHCI_USBLEGCTLSTS_SMI_STATUS_MASK; /* RW1C: write 1s to clear pending */
    reg_write32(ctrl.mmio, cap_off + 4, legctlsts);
}

/* Host Controller Reset (sec 4.2, 5.4.1/5.4.2). Preferred order: stop the
 * controller first if it's running (e.g. left running by GRUB/firmware's
 * own USB keyboard probing -- observed in practice under QEMU) and wait
 * for it to actually halt, *then* assert HCRST, so the reset happens
 * against a cleanly quiesced controller rather than aborting whatever
 * DMA/transfer state firmware left in flight.
 *
 * The halt-wait is best-effort, not fatal: HCRST itself forcibly resets
 * the controller regardless of whether it's currently running (the spec
 * doesn't require a prior clean stop, just recommends it), so a timeout
 * here is logged and bring-up proceeds straight to HCRST anyway rather
 * than aborting -- this genuinely does time out occasionally in QEMU when
 * a device (e.g. -device usb-kbd) is attached and firmware was still mid-
 * operation against the controller, without indicating anything actually
 * wrong; HCRST's own bounded CNR wait below is the real correctness gate.
 *
 * HCRST's wait (both USBCMD.HCRST self-clearing and USBSTS.CNR clearing)
 * *is* fatal on timeout -- this runs before arch_enable_interrupts(), so a
 * stuck loop here would take down the entire boot, not just USB, and a
 * controller that never leaves CNR is genuinely unusable. */
static bool reset_controller(void)
{
    uint32_t usbcmd = op_read32(XHCI_OP_USBCMD);
    if (usbcmd & XHCI_USBCMD_RUN) {
        op_write32(XHCI_OP_USBCMD, usbcmd & ~XHCI_USBCMD_RUN);
        bool halted = false;
        for (uint32_t i = 0; i < XHCI_POLL_ITERATIONS; ++i) {
            if (op_read32(XHCI_OP_USBSTS) & XHCI_USBSTS_HCH) {
                halted = true;
                break;
            }
        }
        if (!halted) {
            printf("xhci: controller did not halt after Stop within the poll bound "
                   "(USBSTS.HCH not set) -- proceeding to HCRST anyway, which resets "
                   "the controller regardless of its running state\n");
        }
    }

    op_write32(XHCI_OP_USBCMD, XHCI_USBCMD_HCRST);
    bool ready = false;
    for (uint32_t i = 0; i < XHCI_POLL_ITERATIONS; ++i) {
        uint32_t cmd = op_read32(XHCI_OP_USBCMD);
        uint32_t sts = op_read32(XHCI_OP_USBSTS);
        if (!(cmd & XHCI_USBCMD_HCRST) && !(sts & XHCI_USBSTS_CNR)) {
            ready = true;
            break;
        }
    }
    if (!ready) {
        printf("xhci: controller did not become ready after HCRST (CNR never cleared); "
               "aborting bring-up\n");
        return false;
    }
    printf("xhci: controller reset complete\n");
    return true;
}

/* Allocates the Device Context Base Address Array (1 page of 64-bit
 * pointers, indexed by Slot ID; entry 0 is reserved for the Scratchpad
 * Buffer Array pointer rather than a device, since slot IDs start at 1) and,
 * if the controller requires them (HCSPARAMS2 Max Scratchpad Buffers > 0),
 * the scratchpad buffer array and the scratchpad buffers themselves.
 * Scratchpad buffers are spec-required to be exactly one page each, which
 * fits this driver's "one DMA structure per page" strategy naturally.
 *
 * QEMU's qemu-xhci model reports 0 scratchpad buffers, so this path is
 * untested by QEMU -- implemented to spec anyway (not deferred) because
 * real Intel PCH xHCI silicon commonly requires several, and skipping this
 * would boot fine in QEMU and then fail bring-up on the very first real-
 * hardware boot. */
static bool setup_dcbaa(void)
{
    ctrl.dcbaa = (uint64_t *)alloc_dma_page();
    if (!ctrl.dcbaa) {
        printf("xhci: failed to allocate DCBAA\n");
        return false;
    }
    ctrl.dcbaa_phys = (phys_addr_t)(uintptr_t)ctrl.dcbaa;

    if (ctrl.max_scratchpad_bufs > 0) {
        uint32_t max_entries = PUREUNIX_PAGE_SIZE / (uint32_t)sizeof(uint64_t);
        if (ctrl.max_scratchpad_bufs > max_entries) {
            printf("xhci: scratchpad buffer count %u exceeds this driver's single-page "
                   "array capacity (%u); aborting bring-up\n",
                   ctrl.max_scratchpad_bufs, max_entries);
            return false;
        }

        uint64_t *scratchpad_array = (uint64_t *)alloc_dma_page();
        if (!scratchpad_array) {
            printf("xhci: failed to allocate scratchpad buffer array\n");
            return false;
        }
        phys_addr_t scratchpad_array_phys = (phys_addr_t)(uintptr_t)scratchpad_array;

        for (uint16_t i = 0; i < ctrl.max_scratchpad_bufs; ++i) {
            void *buf = alloc_dma_page();
            if (!buf) {
                printf("xhci: failed to allocate scratchpad buffer %u/%u\n",
                       i, ctrl.max_scratchpad_bufs);
                return false;
            }
            scratchpad_array[i] = (uint64_t)(phys_addr_t)(uintptr_t)buf;
        }

        /* DCBAA[0] holds the Scratchpad Buffer Array's address -- slot IDs
         * start at 1, so this entry is never a device context pointer. */
        ctrl.dcbaa[0] = (uint64_t)scratchpad_array_phys;
        printf("xhci: allocated %u scratchpad buffer(s)\n", ctrl.max_scratchpad_bufs);
    }

    return true;
}

/* Allocates a ring (command or transfer ring): one DMA page of TRBs, PCS
 * initialized to 1 as required at ring creation (sec 4.9.2). The page is
 * already zeroed by alloc_dma_page(), so every unwritten TRB's Cycle bit is
 * 0 -- correctly "invalid" against the initial PCS=1 until this driver
 * actually enqueues something there. */
static bool ring_alloc(xhci_ring_t *ring)
{
    void *page = alloc_dma_page();
    if (!page) {
        return false;
    }
    ring->trbs = (xhci_trb_t *)page;
    ring->phys = (phys_addr_t)(uintptr_t)page;
    ring->enqueue_index = 0;
    ring->dequeue_index = 0;
    ring->cycle_state = true;
    return true;
}

/* Enqueues one TRB onto a producer ring (command or transfer), returning
 * the physical address of the slot it was written to -- callers that need
 * to correlate a later completion event with this specific TRB (e.g. the
 * command-completion wait helpers below) use this as the match key.
 *
 * TRB fields are written Parameter/Status first, Control (which carries the
 * Cycle bit that marks the TRB valid) last, with a compiler barrier between
 * them -- the Cycle bit is the hardware-visible "this TRB is ready" signal,
 * so every other field must already be in memory before it flips.
 *
 * When the enqueue pointer reaches the ring's last slot, that slot is
 * (re)written as a Link TRB (Toggle Cycle set, pointing back to slot 0)
 * with the *current* cycle bit, then PCS is toggled and the index wraps to
 * 0 -- every ring this driver allocates reserves its last slot for this,
 * i.e. only XHCI_TRBS_PER_PAGE-1 slots are ever usable for real TRBs. */
static phys_addr_t ring_enqueue(xhci_ring_t *ring, uint32_t param_lo, uint32_t param_hi,
                                 uint32_t status, uint32_t control_no_cycle)
{
    uint32_t idx = ring->enqueue_index;
    phys_addr_t trb_phys = ring->phys + idx * XHCI_TRB_SIZE;
    xhci_trb_t *trb = &ring->trbs[idx];

    trb->parameter_lo = param_lo;
    trb->parameter_hi = param_hi;
    trb->status = status;
    xhci_barrier();
    trb->control = control_no_cycle | (ring->cycle_state ? XHCI_TRB_CYCLE : 0);

    uint32_t next = idx + 1;
    if (next == XHCI_TRBS_PER_PAGE - 1) {
        xhci_trb_t *link = &ring->trbs[next];
        link->parameter_lo = ring->phys;
        link->parameter_hi = 0;
        link->status = 0;
        xhci_barrier();
        link->control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_LINK) | XHCI_TRB_CONTROL_TC
            | (ring->cycle_state ? XHCI_TRB_CYCLE : 0);
        ring->cycle_state = !ring->cycle_state;
        next = 0;
    }
    ring->enqueue_index = next;
    return trb_phys;
}

static void arm_interrupt_transfer(uint32_t slot_id, uint32_t dci);

/* Drains every event currently available on the event ring, dispatching
 * each by TRB type, then updates ERDP to acknowledge what was processed.
 *
 * This is the single, shared event-ring consumer for the entire driver's
 * lifetime -- called directly (bounded busy-poll) by the pre-interrupt
 * command self-test in this milestone, and later by xhci_irq() once
 * interrupts are enabled. The two call sites are never active
 * simultaneously (interrupts structurally cannot fire before
 * arch_enable_interrupts() runs, and after that point only xhci_irq() ever
 * calls this), so there is exactly one event-ring consumer at any given
 * time despite the two entry points -- see docs/usb.md for this invariant
 * and why violating it (e.g. issuing a blocking command wait from inside
 * xhci_irq() itself) would deadlock the same way the documented ip_send()
 * bug did (docs/networking.md). */
static void process_event_ring(void)
{
    xhci_ring_t *ring = &ctrl.event_ring;

    for (;;) {
        xhci_trb_t *trb = &ring->trbs[ring->dequeue_index];
        bool cycle = (trb->control & XHCI_TRB_CYCLE) != 0;
        if (cycle != ring->cycle_state) {
            break; /* caught up -- nothing new since the last drain */
        }

        uint32_t type = XHCI_TRB_GET_TYPE(trb->control);
        if (type == XHCI_TRB_TYPE_COMMAND_COMPLETION_EVENT) {
            phys_addr_t completed_trb = trb->parameter_lo;
            uint32_t completion_code = XHCI_TRB_GET_COMPLETION_CODE(trb->status);
            uint32_t slot_id = XHCI_TRB_GET_SLOT_ID(trb->control);
            usb_debugf("xhci: event: Command Completion trb=%p code=%u slot=%u%s\n",
                       (void *)(uintptr_t)completed_trb, completion_code, slot_id,
                       (pending_cmd.trb_phys != 0 && completed_trb == pending_cmd.trb_phys)
                           ? "" : " (unmatched -- no command currently waiting on this TRB)");
            if (pending_cmd.trb_phys != 0 && completed_trb == pending_cmd.trb_phys) {
                pending_cmd.completion_code = completion_code;
                pending_cmd.slot_id = slot_id;
                pending_cmd.done = true;
            }
        } else if (type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
            phys_addr_t completed_trb = trb->parameter_lo;
            uint32_t completion_code = XHCI_TRB_GET_COMPLETION_CODE(trb->status);
            uint32_t event_slot_id = XHCI_TRB_GET_SLOT_ID(trb->control);
            uint32_t event_dci = XHCI_TRB_GET_ENDPOINT_ID(trb->control);
            usb_debugf("xhci: event: Transfer Event trb=%p code=%u slot=%u dci=%u\n",
                       (void *)(uintptr_t)completed_trb, completion_code, event_slot_id, event_dci);

            /* Routed by (slot, DCI) rather than by TRB-pointer match --
             * unlike pending_cmd/pending_xfer's single outstanding
             * operation, an interrupt listener stays armed indefinitely
             * across many completions, so there's no single TRB pointer
             * to match against the way a one-shot wait has. */
            if (event_slot_id <= XHCI_MAX_SLOTS_SUPPORTED && event_dci < XHCI_MAX_DCI
                && interrupt_listeners[event_slot_id][event_dci].active) {
                xhci_interrupt_listener_t *listener = &interrupt_listeners[event_slot_id][event_dci];
                bool success = (completion_code == XHCI_COMPLETION_SUCCESS);
                usb_debugf("xhci: event: routed to interrupt listener slot=%u dci=%u ep=%02x "
                           "success=%u\n",
                           event_slot_id, event_dci, listener->endpoint_address, success);
                /* A Boot Protocol report is always exactly the endpoint's
                 * full max packet size -- this driver doesn't attempt to
                 * compute the exact transferred byte count from the
                 * event's residual-length field, unlike a general-purpose
                 * interrupt-transfer consumer would need to. */
                listener->callback(event_slot_id, listener->endpoint_address, listener->buf,
                                    listener->length, success, listener->ctx);
                arm_interrupt_transfer(event_slot_id, event_dci);
            } else if (pending_xfer.trb_phys != 0 && completed_trb == pending_xfer.trb_phys) {
                pending_xfer.completion_code = completion_code;
                pending_xfer.done = true;
            } else {
                usb_debugf("xhci: event: Transfer Event unmatched (no pending control transfer, "
                           "no active interrupt listener for slot=%u dci=%u) -- event dropped\n",
                           event_slot_id, event_dci);
            }
        } else if (type == XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT) {
            /* Diagnostic only -- xhci_enumerate() still does its own polled
             * PORTSC scan rather than consuming this event to drive
             * enumeration (see that function's comment), but logging it
             * unconditionally here proves whether the controller is telling
             * us about a connect at all, which the polled scan alone cannot
             * distinguish from "never saw one." Port number is bits 31:24
             * of the Port Status Change Event's Parameter field. */
            uint32_t port = (trb->parameter_lo >> 24) & 0xFFU;
            usb_debugf("xhci: event: Port Status Change port=%u\n", port);
        } else {
            usb_debugf("xhci: event: unrecognized TRB type=%u -- ignored\n", type);
        }

        uint32_t next = ring->dequeue_index + 1;
        if (next == XHCI_TRBS_PER_PAGE) {
            next = 0;
            ring->cycle_state = !ring->cycle_state;
        }
        ring->dequeue_index = next;
    }

    phys_addr_t dequeue_trb_phys = ring->phys + ring->dequeue_index * XHCI_TRB_SIZE;
    rt0_write64(XHCI_RT_ERDP, (dequeue_trb_phys & XHCI_ERDP_PTR_MASK) | XHCI_ERDP_EHB, 0);
}

/* Allocates the event ring segment (256 usable TRBs, no Link TRB -- its
 * wrap is defined entirely by the Event Ring Segment Table, not an in-band
 * TRB) and the ERST itself as a *separate* page-sized allocation (an array
 * of {base, size, reserved} entries -- conflating this with the event ring
 * segment it describes, e.g. pointing ERSTBA at the TRB array directly
 * instead of at a proper ERST entry, is a common first-implementation bug).
 * Programs interrupter 0: ERSTSZ=1 (single segment), ERSTBA, initial ERDP
 * at the ring's first TRB, no interrupt moderation delay (IMODI=0, lowest
 * latency), and IMAN.IE set so the controller is ready to actually deliver
 * an interrupt as soon as arch_enable_interrupts() makes that possible. */
static bool setup_event_ring(void)
{
    if (!ring_alloc(&ctrl.event_ring)) {
        printf("xhci: failed to allocate event ring\n");
        return false;
    }

    ctrl.erst = (xhci_erst_entry_t *)alloc_dma_page();
    if (!ctrl.erst) {
        printf("xhci: failed to allocate ERST\n");
        return false;
    }
    ctrl.erst_phys = (phys_addr_t)(uintptr_t)ctrl.erst;

    ctrl.erst[0].base_lo = ctrl.event_ring.phys;
    ctrl.erst[0].base_hi = 0;
    ctrl.erst[0].size = XHCI_TRBS_PER_PAGE; /* full page usable -- no Link TRB here */
    ctrl.erst[0].reserved = 0;
    xhci_barrier();

    rt0_write32(XHCI_RT_ERSTSZ, 1U); /* one segment */
    rt0_write64(XHCI_RT_ERDP, ctrl.event_ring.phys & XHCI_ERDP_PTR_MASK, 0);
    rt0_write64(XHCI_RT_ERSTBA, ctrl.erst_phys, 0); /* must be written after ERSTSZ/ERDP */
    rt0_write32(XHCI_RT_IMOD, 0U); /* no moderation delay */
    rt0_write32(XHCI_RT_IMAN, XHCI_IMAN_IE);

    return true;
}

/* Submits one command TRB and rings the command doorbell (doorbell 0,
 * target ignored/reserved for the command ring). Records the TRB's own
 * physical address as the "pending command" match key for
 * process_event_ring(); only one command is ever outstanding at a time. */
static void submit_command(uint32_t param_lo, uint32_t param_hi, uint32_t status,
                            uint32_t control_no_cycle)
{
    phys_addr_t trb_phys = ring_enqueue(&ctrl.cmd_ring, param_lo, param_hi, status, control_no_cycle);
    pending_cmd.trb_phys = trb_phys;
    pending_cmd.done = false;
    xhci_barrier();
    doorbell_ring(0, XHCI_DOORBELL_TARGET_COMMAND_RING);
}

/* Pre-interrupt-only wait: busy-polls event-ring memory directly via
 * process_event_ring(), bounded. Must never be called once
 * arch_enable_interrupts() has run -- see process_event_ring()'s comment on
 * the single-consumer invariant, and wait_command_irq() (added in a later
 * milestone) for the post-interrupt equivalent that instead blocks on
 * xhci_irq() via arch_halt(). */
static bool wait_command_poll(uint32_t *completion_code, uint32_t *slot_id)
{
    for (uint32_t i = 0; i < XHCI_POLL_ITERATIONS; ++i) {
        process_event_ring();
        if (pending_cmd.done) {
            if (completion_code) {
                *completion_code = pending_cmd.completion_code;
            }
            if (slot_id) {
                *slot_id = pending_cmd.slot_id;
            }
            pending_cmd.trb_phys = 0;
            return true;
        }
    }
    pending_cmd.trb_phys = 0;
    return false;
}

/* Post-interrupt wait: blocks via arch_halt() (identical pattern to
 * keyboard_getkey(), drivers/keyboard.c) until xhci_irq() sets
 * pending_cmd.done. Must only be called after arch_enable_interrupts() --
 * see process_event_ring()'s single-consumer invariant. Unbounded rather
 * than iteration-capped like wait_command_poll(): by this point the
 * command-ring/event-ring/interrupt mechanism has already been proven
 * working end-to-end by xhci_init()'s pre-interrupt self-test, so an
 * unbounded wait here matches every other blocking-on-a-real-interrupt
 * primitive in this kernel rather than treating a normal wait as a
 * suspected hang. */
static bool wait_command_irq(uint32_t *completion_code, uint32_t *slot_id)
{
    while (!pending_cmd.done) {
        arch_halt();
    }
    if (completion_code) {
        *completion_code = pending_cmd.completion_code;
    }
    if (slot_id) {
        *slot_id = pending_cmd.slot_id;
    }
    pending_cmd.trb_phys = 0;
    return true;
}

/* Post-interrupt wait for a control transfer's Status Stage TRB, same
 * shape and same "only after arch_enable_interrupts()" restriction as
 * wait_command_irq(). */
static bool wait_transfer_irq(uint32_t *completion_code)
{
    while (!pending_xfer.done) {
        arch_halt();
    }
    if (completion_code) {
        *completion_code = pending_xfer.completion_code;
    }
    pending_xfer.trb_phys = 0;
    return true;
}

/* Interrupt handler for the controller's legacy INTx line. Legacy PCI IRQ
 * lines are commonly shared between devices (this driver's own bring-up
 * routinely lands on the same vector as e1000 under QEMU's default PCI IRQ
 * routing, for instance, and interrupt_register_handler() -- see
 * arch/i386/idt.c -- invokes every handler registered on a shared vector
 * for every interrupt), so the very first thing this does is check
 * IMAN.IP (Interrupt Pending) and return immediately if it's clear: an
 * unconditional ack+drain here would silently swallow an unrelated
 * device's interrupt on every shared-IRQ firing, exactly the kind of bug
 * that boots fine with one PCI device and livelocks the instant a second
 * one shares the line.
 *
 * When it *is* this controller's interrupt: acknowledges the primary
 * interrupter (IMAN.IP is RW1C -- writing back the value just read clears
 * IP while preserving IE) and drains the event ring. Safe in interrupt
 * context: everything here is either a plain MMIO register access or
 * updating in-memory ring state, matching this kernel's documented
 * IRQ-context safety rules (no blocking, no pit_sleep()) -- see
 * docs/networking.md's case study on the deadlock that rule exists to
 * prevent. Registered but functionally inert until arch_enable_interrupts()
 * makes the CPU capable of actually taking the interrupt at all. */
static void xhci_irq(interrupt_regs_t *regs)
{
    (void)regs;
    uint32_t iman = rt0_read32(XHCI_RT_IMAN);
    if (!(iman & XHCI_IMAN_IP)) {
        return;
    }
    rt0_write32(XHCI_RT_IMAN, iman);
    process_event_ring();
}

/* ---- Device enumeration (M3+): everything from here down runs only after
 * arch_enable_interrupts() (see xhci_enumerate()), using the interrupt-
 * driven wait_command_irq()/wait_transfer_irq() helpers above rather than
 * the pre-interrupt polling helper bring_up_controller() uses. ---------- */

/* Locates the real 32-byte context struct for Device Context Index `dci`
 * within a Device Context page: dci=0 is the Slot Context, dci=1..31 are
 * Endpoint Contexts (dci=1 is always EP0). Strides by the runtime-captured
 * context_size (32 or 64 bytes, from HCCPARAMS1.CSZ), not sizeof(the
 * struct), so this is correct on CSZ==1 hardware too -- see xhci.h's
 * comment on xhci_slot_context_t. */
static void *device_context_entry(void *device_ctx_base, uint32_t dci)
{
    return (uint8_t *)device_ctx_base + dci * ctrl.context_size;
}

/* Same indexing as device_context_entry(), but within an Input Context
 * page, which prepends one extra context_size-sized slot (the Input
 * Control Context, at offset 0) ahead of the Slot/Endpoint contexts --
 * hence the "+1". */
static void *input_context_entry(void *input_ctx_base, uint32_t dci)
{
    return (uint8_t *)input_ctx_base + (dci + 1) * ctrl.context_size;
}

/* Port Status and Control read-modify-write helper: clears every RW1C
 * change bit (XHCI_PORTSC_CHANGE_BITS) while preserving the bits that
 * carry real state (PP, PLS) rather than a pending-change notification.
 * Used both to clear stale change bits before a reset and to acknowledge
 * the reset-complete change bit afterward. */
static void portsc_clear_changes(uint32_t offset)
{
    uint32_t portsc = op_read32(offset);
    op_write32(offset, (portsc & XHCI_PORTSC_PRESERVE_MASK) | XHCI_PORTSC_CHANGE_BITS);
}

/* Resets a connected port (sec 4.3.1): clears any stale change bits, sets
 * PORTSC.PR, and polls (bounded) for PORTSC.PRC (Port Reset Change) to
 * confirm the reset actually completed, then clears that change bit and
 * confirms PED (Port Enabled) came up. USB2-port procedure only -- see
 * xhci.h's Supported Protocol Capability comment on why USB3 ports (which
 * auto-enable via link training and should not have a software reset
 * forced on them) are out of this driver's scope entirely. */
static bool reset_port(uint32_t port)
{
    uint32_t offset = XHCI_OP_PORTSC_BASE + (port - 1) * XHCI_OP_PORTSC_STRIDE;
    portsc_clear_changes(offset);

    uint32_t portsc = op_read32(offset);
    op_write32(offset, (portsc & XHCI_PORTSC_PRESERVE_MASK) | XHCI_PORTSC_PR);

    bool reset_done = false;
    for (uint32_t i = 0; i < XHCI_POLL_ITERATIONS; ++i) {
        if (op_read32(offset) & XHCI_PORTSC_PRC) {
            reset_done = true;
            break;
        }
    }
    if (!reset_done) {
        printf("xhci: port %u reset timed out\n", port);
        return false;
    }
    portsc_clear_changes(offset);

    portsc = op_read32(offset);
    if (!(portsc & XHCI_PORTSC_PED)) {
        printf("xhci: port %u did not enable after reset (portsc=%x)\n", port, portsc);
        return false;
    }
    return true;
}

/* usb_hc_ops_t.enable_slot -- issues an Enable Slot Command and waits for
 * its Command Completion Event, which carries the newly-allocated Slot ID. */
static bool enable_slot(uint32_t slot_type, uint32_t *out_slot_id)
{
    submit_command(0, 0, 0,
                   XHCI_TRB_TYPE(XHCI_TRB_TYPE_ENABLE_SLOT_CMD)
                       | (slot_type << XHCI_ENABLE_SLOT_TYPE_SHIFT));
    uint32_t completion_code = 0;
    uint32_t slot_id = 0;
    if (!wait_command_irq(&completion_code, &slot_id)) {
        return false;
    }
    if (completion_code != XHCI_COMPLETION_SUCCESS) {
        printf("xhci: Enable Slot Command failed (completion code=%u)\n", completion_code);
        return false;
    }
    if (slot_id == 0 || slot_id > ctrl.slots_enabled) {
        printf("xhci: Enable Slot Command returned out-of-range slot id %u\n", slot_id);
        return false;
    }
    *out_slot_id = slot_id;
    return true;
}

/* usb_hc_ops_t.control_transfer -- builds and submits a Setup/[Data]/Status
 * Stage TRB sequence on the given slot's EP0 transfer ring and blocks (via
 * wait_transfer_irq()) until the Status Stage TRB's Transfer Event arrives.
 *
 * Setup Stage TRB uses IDT (Immediate Data): the 8 raw setup bytes are
 * embedded directly in the TRB's Parameter fields rather than pointed-to
 * via a data buffer -- an xHCI-specific quirk (unlike EHCI/UHCI queue-head
 * setup) that's easy to miss coming from generic USB-spec knowledge alone.
 * The Status Stage TRB always carries IOC (Interrupt On Completion) --
 * without it the transfer completes with no Transfer Event ever posted,
 * which manifests as this call hanging forever waiting for a completion
 * that was never going to arrive. */
static bool control_transfer(uint32_t slot_id, uint8_t bm_request_type, uint8_t b_request,
                              uint16_t w_value, uint16_t w_index, uint16_t w_length, void *data,
                              bool data_in)
{
    xhci_ring_t *ring = &ctrl.slots[slot_id].rings[XHCI_EP0_DCI];

    uint32_t setup_lo = (uint32_t)bm_request_type | ((uint32_t)b_request << 8)
        | ((uint32_t)w_value << 16);
    uint32_t setup_hi = (uint32_t)w_index | ((uint32_t)w_length << 16);
    uint32_t trt = (w_length == 0) ? XHCI_TRT_NO_DATA : (data_in ? XHCI_TRT_IN : XHCI_TRT_OUT);
    ring_enqueue(ring, setup_lo, setup_hi, 8U /* Setup Stage TRB transfer length is always 8 */,
                 XHCI_TRB_CONTROL_IDT | XHCI_TRB_TYPE(XHCI_TRB_TYPE_SETUP_STAGE)
                     | (trt << XHCI_TRT_SHIFT));

    if (w_length > 0) {
        phys_addr_t data_phys = (phys_addr_t)(uintptr_t)data;
        ring_enqueue(ring, data_phys, 0, w_length,
                     XHCI_TRB_TYPE(XHCI_TRB_TYPE_DATA_STAGE) | (data_in ? XHCI_TRB_DIR_IN : 0));
    }

    /* Status Stage direction is always opposite the Data Stage; a no-data
     * control request (w_length==0) always uses an IN status stage
     * regardless of bmRequestType's direction bit -- standard USB
     * convention, not an xHCI-specific rule. */
    bool status_dir_in = (w_length == 0) ? true : !data_in;
    phys_addr_t status_trb_phys = ring_enqueue(
        ring, 0, 0, 0,
        XHCI_TRB_TYPE(XHCI_TRB_TYPE_STATUS_STAGE) | XHCI_TRB_CONTROL_IOC
            | (status_dir_in ? XHCI_TRB_DIR_IN : 0));

    pending_xfer.trb_phys = status_trb_phys;
    pending_xfer.done = false;
    xhci_barrier();
    doorbell_ring((uint8_t)slot_id, XHCI_EP0_DCI);

    uint32_t completion_code = 0;
    if (!wait_transfer_irq(&completion_code)) {
        return false;
    }
    if (completion_code != XHCI_COMPLETION_SUCCESS) {
        printf("xhci: control transfer failed (slot %u, bRequest=%u, completion code=%u)\n",
               slot_id, b_request, completion_code);
        return false;
    }
    return true;
}

/* usb_hc_ops_t.address_device -- the two-stage BSR (Block Set Address
 * Request) dance recommended by the xHCI spec for robustness (not strictly
 * mandated, but this is what Linux's xhci-hcd does, and it eliminates an
 * entire edge-case class around guessing bMaxPacketSize0 wrong for a Full
 * Speed device):
 *
 *   1. Allocate Input/Device Context pages and an EP0 transfer ring, fill
 *      in the Input Control Context (Add Slot Context + Add EP0 Context)
 *      and the Slot/EP0 contexts themselves (EP0's Max Packet Size seeded
 *      with a conservative guess from port speed), then issue Address
 *      Device with BSR=1 -- this seeds the controller's EP0 state without
 *      it emitting a real SET_ADDRESS on the wire yet.
 *   2. Read 8 bytes of the device descriptor via a real control transfer
 *      on the now-seeded EP0 pipe, learning the device's actual
 *      bMaxPacketSize0. If it differs from the guess, patch the Input
 *      Context's EP0 Max Packet Size and issue Evaluate Context to correct
 *      the controller's live EP0 state.
 *   3. Issue Address Device again with BSR=0 -- only now does the
 *      controller perform the real SET_ADDRESS transaction and the slot
 *      transitions to the Addressed state. */
static bool address_device(uint32_t slot_id, uint32_t port, uint32_t speed)
{
    xhci_slot_state_t *st = &ctrl.slots[slot_id];

    st->input_ctx = alloc_dma_page();
    st->device_ctx = alloc_dma_page();
    if (!st->input_ctx || !st->device_ctx) {
        printf("xhci: slot %u: failed to allocate Input/Device Context pages\n", slot_id);
        return false;
    }
    st->input_ctx_phys = (phys_addr_t)(uintptr_t)st->input_ctx;
    st->device_ctx_phys = (phys_addr_t)(uintptr_t)st->device_ctx;
    st->port = port;
    st->speed = speed;

    ctrl.dcbaa[slot_id] = (uint64_t)st->device_ctx_phys;

    if (!ring_alloc(&st->rings[XHCI_EP0_DCI])) {
        printf("xhci: slot %u: failed to allocate EP0 transfer ring\n", slot_id);
        return false;
    }

    xhci_input_control_context_t *icc = (xhci_input_control_context_t *)st->input_ctx;
    icc->add_flags = XHCI_INPUT_CONTROL_ADD_SLOT | XHCI_INPUT_CONTROL_ADD_EP(XHCI_EP0_DCI);

    xhci_slot_context_t *slot_ctx = (xhci_slot_context_t *)input_context_entry(st->input_ctx, 0);
    slot_ctx->dword0 = (1U << XHCI_SLOT_CONTEXT_ENTRIES_SHIFT) /* Context Entries=1 (EP0 only) */
        | (speed << XHCI_SLOT_SPEED_SHIFT);
    slot_ctx->dword1 = port << XHCI_SLOT_ROOT_HUB_PORT_SHIFT;
    slot_ctx->dword2 = 0U << XHCI_SLOT_INTERRUPTER_TARGET_SHIFT; /* interrupter 0 */

    /* Conservative first guess at bMaxPacketSize0 by speed (sec 4.3's
     * table): Low Speed is always exactly 8, High Speed is always exactly
     * 64, Full Speed varies (8/16/32/64) so 8 is used as a safe minimum
     * every FS device accepts for an initial 8-byte read. */
    uint8_t max_packet_guess = (speed == XHCI_SPEED_HIGH) ? 64U : 8U;

    xhci_endpoint_context_t *ep0_ctx =
        (xhci_endpoint_context_t *)input_context_entry(st->input_ctx, XHCI_EP0_DCI);
    ep0_ctx->dword1 = (XHCI_EP_TYPE_CONTROL << XHCI_EP_TYPE_SHIFT) | (3U << XHCI_EP_CERR_SHIFT)
        | ((uint32_t)max_packet_guess << XHCI_EP_MAX_PACKET_SIZE_SHIFT);
    ep0_ctx->tr_dequeue_lo = (st->rings[XHCI_EP0_DCI].phys & ~0xFU) | XHCI_EP_DEQUEUE_CYCLE_STATE;
    ep0_ctx->tr_dequeue_hi = 0;
    ep0_ctx->dword4 = XHCI_EP_AVERAGE_TRB_LENGTH_CONTROL_DEFAULT;
    xhci_barrier();

    submit_command(st->input_ctx_phys, 0, 0,
                   XHCI_TRB_TYPE(XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD) | XHCI_ADDRESS_DEVICE_BSR
                       | (slot_id << 24));
    uint32_t completion_code = 0;
    if (!wait_command_irq(&completion_code, NULL) || completion_code != XHCI_COMPLETION_SUCCESS) {
        printf("xhci: slot %u: Address Device (BSR=1) failed (completion code=%u)\n", slot_id,
               completion_code);
        return false;
    }

    uint8_t desc8[8];
    if (!control_transfer(slot_id, USB_REQUEST_TYPE_DEVICE_TO_HOST | USB_REQUEST_TYPE_STANDARD
                               | USB_REQUEST_TYPE_RECIPIENT_DEVICE,
                           USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DESC_TYPE_DEVICE << 8), 0,
                           sizeof(desc8), desc8, true)) {
        printf("xhci: slot %u: 8-byte device descriptor probe failed\n", slot_id);
        return false;
    }

    uint8_t real_max_packet0 = desc8[7]; /* bMaxPacketSize0 is byte offset 7 */
    if (real_max_packet0 != 0 && real_max_packet0 != max_packet_guess) {
        ep0_ctx->dword1 = (ep0_ctx->dword1
                            & ~(XHCI_EP_MAX_PACKET_SIZE_MASK << XHCI_EP_MAX_PACKET_SIZE_SHIFT))
            | ((uint32_t)real_max_packet0 << XHCI_EP_MAX_PACKET_SIZE_SHIFT);
        icc->add_flags = XHCI_INPUT_CONTROL_ADD_EP(XHCI_EP0_DCI); /* Evaluate Context: EP0 only */
        xhci_barrier();
        submit_command(st->input_ctx_phys, 0, 0,
                       XHCI_TRB_TYPE(XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD) | (slot_id << 24));
        if (!wait_command_irq(&completion_code, NULL)
            || completion_code != XHCI_COMPLETION_SUCCESS) {
            printf("xhci: slot %u: Evaluate Context failed (completion code=%u)\n", slot_id,
                   completion_code);
            return false;
        }
    }

    icc->add_flags = XHCI_INPUT_CONTROL_ADD_SLOT | XHCI_INPUT_CONTROL_ADD_EP(XHCI_EP0_DCI);
    xhci_barrier();
    submit_command(st->input_ctx_phys, 0, 0,
                   XHCI_TRB_TYPE(XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD) | (slot_id << 24));
    if (!wait_command_irq(&completion_code, NULL) || completion_code != XHCI_COMPLETION_SUCCESS) {
        printf("xhci: slot %u: Address Device (BSR=0) failed (completion code=%u)\n", slot_id,
               completion_code);
        return false;
    }

    xhci_slot_context_t *dev_slot_ctx =
        (xhci_slot_context_t *)device_context_entry(st->device_ctx, 0);
    printf("xhci: slot %u: addressed (USB address %u, speed=%u, port=%u)\n", slot_id,
           XHCI_SLOT_GET_USB_ADDRESS(dev_slot_ctx->dword3), speed, port);
    st->in_use = true;
    return true;
}

/* Converts a USB endpoint descriptor's bInterval into the xHCI Endpoint
 * Context's Interval field (sec 6.2.3.6) -- the two encodings differ by
 * speed class:
 *   - LS/FS interrupt endpoints: bInterval is a frame count (1-255, 1ms
 *     units). xHCI's Interval is a power-of-2 exponent in 125us units, so
 *     converting requires both a unit change (1ms = 8 * 125us) and finding
 *     the nearest power of 2: Interval = floor(log2(bInterval)) + 3.
 *   - HS interrupt endpoints: bInterval already *is* that same exponent,
 *     just 1-based (actual interval = 2^(bInterval-1) microframes) where
 *     xHCI's Interval field is 0-based -- so Interval = bInterval - 1.
 * (SuperSpeed uses the same 0-based encoding as HS, but SS ports are out of
 * this driver's scope entirely -- see the Supported Protocol Capability
 * comment in xhci.h.) */
static uint32_t compute_interval(uint32_t speed, uint8_t b_interval)
{
    if (speed == XHCI_SPEED_LOW || speed == XHCI_SPEED_FULL) {
        uint32_t frames = b_interval ? b_interval : 1U;
        uint32_t exponent = 0;
        while ((1U << (exponent + 1)) <= frames) {
            ++exponent;
        }
        return exponent + 3U;
    }
    return (b_interval > 0) ? (uint32_t)(b_interval - 1) : 0U;
}

/* usb_hc_ops_t.configure_endpoint -- adds one Interrupt IN endpoint to an
 * already-addressed slot via the Configure Endpoint command. Only
 * Interrupt IN is supported (this driver has no bulk/isochronous device
 * support); the endpoint's DCI is derived from its address the same way
 * doorbell targeting is (xhci.h's DCI convention comment).
 *
 * Configure Endpoint's Input Control Context must add both the new
 * endpoint *and* the Slot Context (A0), since the Slot Context's Context
 * Entries field has to be updated to cover the new endpoint's DCI --
 * leaving A0 clear here is a common mistake that makes the command fail
 * with a Parameter Error completion code. */
static bool configure_endpoint(uint32_t slot_id, uint8_t endpoint_address,
                                uint16_t max_packet_size, uint8_t interval)
{
    if (slot_id == 0 || slot_id > ctrl.slots_enabled || !ctrl.slots[slot_id].in_use) {
        printf("xhci: configure_endpoint: slot %u not addressed\n", slot_id);
        return false;
    }
    if (!USB_ENDPOINT_ADDRESS_IS_IN(endpoint_address)) {
        printf("xhci: configure_endpoint: only Interrupt IN endpoints are supported "
               "(address=%02x)\n",
               endpoint_address);
        return false;
    }

    xhci_slot_state_t *st = &ctrl.slots[slot_id];
    uint32_t dci = USB_ENDPOINT_ADDRESS_NUMBER(endpoint_address) * 2U + 1U;
    if (dci >= XHCI_MAX_DCI) {
        printf("xhci: configure_endpoint: slot %u: endpoint address %02x maps to an "
               "out-of-range DCI %u\n",
               slot_id, endpoint_address, dci);
        return false;
    }

    if (!ring_alloc(&st->rings[dci])) {
        printf("xhci: configure_endpoint: slot %u: failed to allocate transfer ring for "
               "DCI %u\n",
               slot_id, dci);
        return false;
    }

    xhci_input_control_context_t *icc = (xhci_input_control_context_t *)st->input_ctx;
    icc->drop_flags = 0;
    icc->add_flags = XHCI_INPUT_CONTROL_ADD_SLOT | XHCI_INPUT_CONTROL_ADD_EP(dci);

    xhci_slot_context_t *slot_ctx = (xhci_slot_context_t *)input_context_entry(st->input_ctx, 0);
    slot_ctx->dword0 = (slot_ctx->dword0 & ~(0x1FU << XHCI_SLOT_CONTEXT_ENTRIES_SHIFT))
        | (dci << XHCI_SLOT_CONTEXT_ENTRIES_SHIFT); /* Context Entries = highest active DCI */

    xhci_endpoint_context_t *ep_ctx =
        (xhci_endpoint_context_t *)input_context_entry(st->input_ctx, dci);
    uint32_t xhci_interval = compute_interval(st->speed, interval);
    ep_ctx->dword0 = xhci_interval << XHCI_EP_INTERVAL_SHIFT;
    ep_ctx->dword1 = (XHCI_EP_TYPE_INTERRUPT_IN << XHCI_EP_TYPE_SHIFT) | (3U << XHCI_EP_CERR_SHIFT)
        | ((uint32_t)max_packet_size << XHCI_EP_MAX_PACKET_SIZE_SHIFT);
    ep_ctx->tr_dequeue_lo = (st->rings[dci].phys & ~0xFU) | XHCI_EP_DEQUEUE_CYCLE_STATE;
    ep_ctx->tr_dequeue_hi = 0;
    ep_ctx->dword4 = max_packet_size; /* Average TRB Length: a fixed-size interrupt report is
                                        * exactly max_packet_size bytes every time. */
    xhci_barrier();

    usb_debugf("xhci: slot %u: endpoint context built: dci=%u ep=%02x max_packet=%u "
               "b_interval=%u xhci_interval=%u ring_phys=%p\n",
               slot_id, dci, endpoint_address, max_packet_size, interval, xhci_interval,
               (void *)(uintptr_t)st->rings[dci].phys);

    submit_command(st->input_ctx_phys, 0, 0,
                   XHCI_TRB_TYPE(XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD) | (slot_id << 24));
    uint32_t completion_code = 0;
    if (!wait_command_irq(&completion_code, NULL) || completion_code != XHCI_COMPLETION_SUCCESS) {
        printf("xhci: slot %u: Configure Endpoint failed (completion code=%u)\n", slot_id,
               completion_code);
        return false;
    }
    return true;
}

/* (Re-)submits the listener's buffer as a single Normal TRB (sec 6.4.1.1)
 * on the target endpoint's transfer ring and rings its doorbell -- IOC set
 * so a Transfer Event is always posted, ISP set so a short packet (fewer
 * bytes than requested) also generates one rather than silently completing
 * with no event, matching this driver's requirement that every submitted
 * transfer eventually produces a Transfer Event. Called both for the
 * initial submission (submit_interrupt_transfer()) and, from IRQ context,
 * to re-arm after every completion (process_event_ring()) -- fire-and-
 * forget in both cases, never blocks. */
static void arm_interrupt_transfer(uint32_t slot_id, uint32_t dci)
{
    xhci_interrupt_listener_t *listener = &interrupt_listeners[slot_id][dci];
    xhci_ring_t *ring = &ctrl.slots[slot_id].rings[dci];
    phys_addr_t buf_phys = (phys_addr_t)(uintptr_t)listener->buf;

    phys_addr_t trb_phys = ring_enqueue(ring, buf_phys, 0, listener->length,
                 XHCI_TRB_TYPE(XHCI_TRB_TYPE_NORMAL) | XHCI_TRB_CONTROL_ISP
                     | XHCI_TRB_CONTROL_IOC);
    xhci_barrier();
    usb_debugf("xhci: interrupt transfer armed: slot=%u dci=%u ep=%02x trb=%p buf=%p len=%u "
               "-- ringing doorbell\n",
               slot_id, dci, listener->endpoint_address, (void *)(uintptr_t)trb_phys,
               listener->buf, listener->length);
    doorbell_ring((uint8_t)slot_id, (uint8_t)dci);
}

/* usb_hc_ops_t.submit_interrupt_transfer */
static bool submit_interrupt_transfer(uint32_t slot_id, uint8_t endpoint_address, void *buf,
                                       uint16_t length, usb_interrupt_callback_t callback,
                                       void *ctx)
{
    if (slot_id == 0 || slot_id > ctrl.slots_enabled || !ctrl.slots[slot_id].in_use) {
        printf("xhci: submit_interrupt_transfer: slot %u not addressed\n", slot_id);
        return false;
    }
    if (!USB_ENDPOINT_ADDRESS_IS_IN(endpoint_address)) {
        printf("xhci: submit_interrupt_transfer: only Interrupt IN endpoints are supported "
               "(address=%02x)\n",
               endpoint_address);
        return false;
    }
    uint32_t dci = USB_ENDPOINT_ADDRESS_NUMBER(endpoint_address) * 2U + 1U;
    if (dci >= XHCI_MAX_DCI || ctrl.slots[slot_id].rings[dci].trbs == NULL) {
        printf("xhci: submit_interrupt_transfer: slot %u: endpoint %02x has no configured "
               "ring (call configure_endpoint() first)\n",
               slot_id, endpoint_address);
        return false;
    }

    xhci_interrupt_listener_t *listener = &interrupt_listeners[slot_id][dci];
    listener->endpoint_address = endpoint_address;
    listener->buf = buf;
    listener->length = length;
    listener->callback = callback;
    listener->ctx = ctx;
    listener->active = true;

    arm_interrupt_transfer(slot_id, dci);
    return true;
}

static const usb_hc_ops_t xhci_hc_ops = {
    .enable_slot = enable_slot,
    .address_device = address_device,
    .control_transfer = control_transfer,
    .configure_endpoint = configure_endpoint,
    .submit_interrupt_transfer = submit_interrupt_transfer,
};

/* Walks the extended capabilities list for a Supported Protocol Capability
 * whose Major Revision is 2 (USB2 -- covers LS/FS/HS, i.e. every Boot
 * Protocol keyboard), returning its Compatible Port Offset/Count and
 * Protocol Slot Type. Any USB3-major capability found along the way is
 * logged and skipped -- see xhci.h's comment on this driver's USB2-only
 * enumeration scope. Returns false if no USB2 capability exists at all
 * (would mean a controller with no low/full/high-speed ports, unusual but
 * not itself an error worth failing bring-up over). */
static bool find_usb2_protocol(uint32_t *port_offset, uint32_t *port_count, uint32_t *slot_type)
{
    uint32_t hccparams1 = reg_read32(ctrl.mmio, XHCI_CAP_HCCPARAMS1);
    uint32_t xecp_dwords = XHCI_HCCPARAMS1_XECP(hccparams1);
    uint32_t offset = xecp_dwords * 4U;
    uint32_t guard = 0;

    while (offset != 0 && guard < 64) {
        uint32_t dw0 = reg_read32(ctrl.mmio, offset);
        uint32_t id = XHCI_XECP_ID(dw0);
        uint32_t next_dwords = XHCI_XECP_NEXT_DWORDS(dw0);

        if (id == XHCI_XECP_ID_SUPPORTED_PROTOCOL) {
            uint32_t major = XHCI_SUPPORTED_PROTOCOL_MAJOR(dw0);
            uint32_t dw2 = reg_read32(ctrl.mmio, offset + 8);
            uint32_t dw3 = reg_read32(ctrl.mmio, offset + 12);
            uint32_t p_off = XHCI_SUPPORTED_PROTOCOL_PORT_OFFSET(dw2);
            uint32_t p_cnt = XHCI_SUPPORTED_PROTOCOL_PORT_COUNT(dw2);

            if (major == 2) {
                *port_offset = p_off;
                *port_count = p_cnt;
                *slot_type = XHCI_SUPPORTED_PROTOCOL_SLOT_TYPE(dw3);
                printf("xhci: USB2 ports %u..%u (slot type %u)\n", p_off, p_off + p_cnt - 1,
                       *slot_type);
                return true;
            }
            printf("xhci: USB%u ports %u..%u -- out of scope, skipped (no SuperSpeed "
                   "enumeration; see docs/usb.md)\n",
                   major, p_off, p_off + p_cnt - 1);
        }

        if (next_dwords == 0) {
            break;
        }
        offset += next_dwords * 4U;
        ++guard;
    }
    return false;
}

void xhci_enumerate(void)
{
    if (!ctrl.present) {
        return;
    }

    /* Second No-Op self-test, this time proving the interrupt-driven path
     * end to end (INTx routing, USBCMD.INTE, IMAN.IE) rather than the
     * event-ring-memory-polling one xhci_init() already ran pre-interrupt
     * -- see wait_command_irq()'s comment. */
    submit_command(0, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_TYPE_NOOP_CMD));
    uint32_t completion_code = 0;
    wait_command_irq(&completion_code, NULL);
    if (completion_code != XHCI_COMPLETION_SUCCESS) {
        printf("xhci: interrupt-driven command self-test failed; device enumeration skipped\n");
        return;
    }
    printf("xhci: interrupt-driven command self-test OK\n");

    uint32_t port_offset = 0;
    uint32_t port_count = 0;
    uint32_t slot_type = 0;
    if (!find_usb2_protocol(&port_offset, &port_count, &slot_type)) {
        printf("xhci: no USB2 Supported Protocol Capability found; nothing to enumerate\n");
        return;
    }

    for (uint32_t port = port_offset; port < port_offset + port_count; ++port) {
        if (port == 0 || port > ctrl.max_ports) {
            continue;
        }
        uint32_t offset = XHCI_OP_PORTSC_BASE + (port - 1) * XHCI_OP_PORTSC_STRIDE;
        uint32_t portsc = op_read32(offset);
        usb_debugf("xhci: port %u: initial scan portsc=%x (ccs=%u ped=%u pls=%u speed=%u)\n", port,
                   portsc, (portsc & XHCI_PORTSC_CCS) != 0, (portsc & XHCI_PORTSC_PED) != 0,
                   (portsc >> XHCI_PORTSC_PLS_SHIFT) & XHCI_PORTSC_PLS_MASK,
                   XHCI_PORTSC_GET_SPEED(portsc));

        /* CCS reflects real analog device-presence detection, not something
         * this driver's own reset/bring-up sequence should need to wait on
         * -- but unlike QEMU's virtual device (CCS=1 from the very first
         * read after HCRST, with no signal-integrity/link-training delay to
         * model), real silicon's port state can take a short, variable time
         * to settle after this controller's own HCRST tears down and
         * retrains the downstream link, especially right after a BIOS/SMM
         * Legacy Support handoff was actively using the same port. A single
         * unconditional read here would misclassify that transient window
         * as "nothing plugged in" with no way to tell the two apart from the
         * boot log alone -- so retry (bounded) before giving up, logging
         * every raw portsc value seen along the way. */
        if (!(portsc & XHCI_PORTSC_CCS)) {
            bool became_connected = false;
            for (uint32_t i = 0; i < XHCI_POLL_ITERATIONS; ++i) {
                portsc = op_read32(offset);
                if (portsc & XHCI_PORTSC_CCS) {
                    became_connected = true;
                    break;
                }
            }
            if (!became_connected) {
                usb_debugf("xhci: port %u: no device connected after settle-wait (final "
                           "portsc=%x) -- skipping\n",
                           port, portsc);
                continue;
            }
            printf("xhci: port %u: device connected after settle-wait (portsc=%x)\n", port,
                   portsc);
        } else {
            printf("xhci: port %u: device connected (portsc=%x)\n", port, portsc);
        }

        if (!reset_port(port)) {
            continue;
        }
        printf("xhci: port %u: reset complete\n", port);

        uint32_t speed = XHCI_PORTSC_GET_SPEED(op_read32(offset));
        usb_device_t dev;
        if (!usb_enumerate_port(&xhci_hc_ops, port, speed, slot_type, &dev)) {
            continue;
        }
        /* Silently a no-op for any device that isn't a Boot Protocol
         * keyboard (checked internally via dev.interface_class/subclass/
         * protocol) -- future class drivers (mice, storage) would get
         * their own hid_try_attach()-shaped hook called here too. */
        hid_try_attach(&xhci_hc_ops, &dev);
    }
}

/* Full controller bring-up (sec 4.2), continuing from the capability
 * parsing done earlier in xhci_init(): BIOS handoff, reset, DCBAA/
 * scratchpad setup, command ring, event ring/ERST/interrupter, legacy
 * IRQ registration, USBCMD.RUN, and a pre-interrupt No-Op command
 * round-trip that proves the whole ring/doorbell/event-ring mechanism
 * actually works before anything above this layer relies on it.
 *
 * Every step here can fail (bounded polls timing out, allocation failure);
 * on any failure this logs a clear reason and returns false, leaving the
 * controller un-started and xhci_present() reporting false -- the rest of
 * kernel_main() must proceed unaffected either way, the same posture
 * e1000_init() takes toward a missing/misbehaving NIC. */
static bool bring_up_controller(uint32_t hccparams1)
{
    legacy_bios_handoff(hccparams1);

    if (!reset_controller()) {
        return false;
    }

    if (!setup_dcbaa()) {
        return false;
    }
    op_write64(XHCI_OP_DCBAAP, ctrl.dcbaa_phys, 0);

    ctrl.slots_enabled = ctrl.max_slots < XHCI_MAX_SLOTS_SUPPORTED
        ? ctrl.max_slots : XHCI_MAX_SLOTS_SUPPORTED;
    op_write32(XHCI_OP_CONFIG, ctrl.slots_enabled);

    /* CRCR must only be written while the command ring is not running
     * (CRR=0) -- true here since we're still before the first doorbell-0
     * ring and before USBCMD.RUN. RCS=1 matches the ring's initial PCS. */
    if (!ring_alloc(&ctrl.cmd_ring)) {
        printf("xhci: failed to allocate command ring\n");
        return false;
    }
    op_write64(XHCI_OP_CRCR, (ctrl.cmd_ring.phys & XHCI_CRCR_PTR_MASK) | XHCI_CRCR_RCS, 0);

    if (!setup_event_ring()) {
        return false;
    }

    pci_enable_legacy_interrupts(&ctrl.pci);
    interrupt_register_handler((uint8_t)(32 + ctrl.pci.interrupt_line), xhci_irq);
    irq_enable(ctrl.pci.interrupt_line);

    op_write32(XHCI_OP_USBCMD, XHCI_USBCMD_RUN | XHCI_USBCMD_INTE);
    bool running = false;
    for (uint32_t i = 0; i < XHCI_POLL_ITERATIONS; ++i) {
        if (!(op_read32(XHCI_OP_USBSTS) & XHCI_USBSTS_HCH)) {
            running = true;
            break;
        }
    }
    if (!running) {
        printf("xhci: controller did not start running (USBSTS.HCH never cleared)\n");
        return false;
    }
    printf("xhci: controller running\n");

    /* Pre-interrupt self-test: a No-Op command TRB should complete with
     * XHCI_COMPLETION_SUCCESS. This is checked by directly polling
     * event-ring memory (wait_command_poll()), not by waiting on the
     * interrupt -- interrupts structurally cannot fire yet here, since
     * arch_enable_interrupts() hasn't run (see kernel/main.c). A second,
     * independent No-Op test that actually exercises the interrupt path is
     * added in a later milestone, after interrupts are enabled. */
    submit_command(0, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_TYPE_NOOP_CMD));
    uint32_t completion_code = 0;
    if (!wait_command_poll(&completion_code, NULL)) {
        printf("xhci: command ring self-test timed out (no completion event seen)\n");
        return false;
    }
    if (completion_code != XHCI_COMPLETION_SUCCESS) {
        printf("xhci: command ring self-test failed (completion code=%u)\n", completion_code);
        return false;
    }
    printf("xhci: command ring self-test OK\n");
    return true;
}

/* Detects the xHCI controller via PCI class code, maps its MMIO BAR, parses
 * the capability register block, and brings the controller fully up
 * (reset, rings, interrupts, running, self-tested) -- everything that must
 * happen before arch_enable_interrupts() (see kernel/main.c), alongside
 * pci_scan()/e1000_init(). Safe to call whether or not a controller is
 * present, and safe even if bring-up fails partway through:
 * xhci_present() reports the real outcome either way, and the rest of the
 * boot sequence is never blocked on this succeeding. */
void xhci_init(void)
{
    if (ctrl.present) {
        return;
    }

    pci_device_t dev;
    if (!pci_find_by_class(XHCI_PCI_CLASS_SERIAL_BUS, XHCI_PCI_SUBCLASS_USB,
                            XHCI_PCI_PROGIF_XHCI, &dev)) {
        printf("xhci: no xHCI controller found\n");
        return;
    }
    printf("xhci: found %04x:%04x at %02x:%02x.%u (irq %u)\n",
           dev.vendor_id, dev.device_id, dev.bus, dev.slot, dev.func, dev.interrupt_line);

    pci_enable_bus_mastering(&dev);
    pci_enable_legacy_interrupts(&dev);

    phys_addr_t bar0 = pci_bar_address(&dev, 0);
    uint32_t bar_size = pci_bar_size(&dev, 0);
    if (!bar0 || !bar_size) {
        printf("xhci: BAR0 is not a usable memory-mapped BAR\n");
        return;
    }
    printf("xhci: BAR0 %s at phys=%p size=%u KiB\n",
           pci_bar_is_64bit(&dev, 0) ? "(64-bit)" : "(32-bit)",
           (void *)(uintptr_t)bar0, bar_size / 1024);

    ctrl.pci = dev;
    ctrl.mmio = map_bar(bar0, bar_size);

    /* CAPLENGTH (bits 7:0) and HCIVERSION (bits 31:16) share the first
     * capability dword. Read them together as one 32-bit-aligned access
     * rather than separate byte/word accesses -- some MMIO region
     * implementations (real silicon and emulators alike) only reliably
     * support dword-granular reads of this register block. */
    uint32_t cap_dword0 = reg_read32(ctrl.mmio, 0x00);
    ctrl.cap_length = (uint8_t)(cap_dword0 & 0xFFU);
    ctrl.hci_version = (uint16_t)(cap_dword0 >> 16);

    uint32_t hcsparams1 = reg_read32(ctrl.mmio, XHCI_CAP_HCSPARAMS1);
    uint32_t hcsparams2 = reg_read32(ctrl.mmio, XHCI_CAP_HCSPARAMS2);
    uint32_t hccparams1 = reg_read32(ctrl.mmio, XHCI_CAP_HCCPARAMS1);

    ctrl.max_slots = (uint8_t)XHCI_HCSPARAMS1_MAX_SLOTS(hcsparams1);
    ctrl.max_intrs = (uint16_t)XHCI_HCSPARAMS1_MAX_INTRS(hcsparams1);
    ctrl.max_ports = (uint8_t)XHCI_HCSPARAMS1_MAX_PORTS(hcsparams1);
    ctrl.max_scratchpad_bufs = (uint16_t)XHCI_HCSPARAMS2_MAX_SCRATCHPAD(hcsparams2);
    ctrl.context_size = XHCI_HCCPARAMS1_CSZ(hccparams1) ? XHCI_CONTEXT_SIZE_64 : XHCI_CONTEXT_SIZE_32;
    ctrl.dboff = reg_read32(ctrl.mmio, XHCI_CAP_DBOFF) & ~0x3U;
    ctrl.rtsoff = reg_read32(ctrl.mmio, XHCI_CAP_RTSOFF) & ~0x1FU;

    ctrl.op_base = ctrl.mmio + ctrl.cap_length;
    ctrl.db_base = ctrl.mmio + ctrl.dboff;
    ctrl.rt_base = ctrl.mmio + ctrl.rtsoff;

    /* PAGESIZE (operational register, sec 5.4.3): bit n set means the
     * controller supports/uses a (2^(n+12))-byte page size. Only used for
     * diagnostic logging -- every DMA structure this driver allocates is
     * sized to fit a single 4096-byte host page regardless of what the
     * controller reports it prefers (see alloc_dma_page()). */
    uint32_t pagesize_reg = op_read32(0x08);
    uint32_t page_shift = 12;
    for (uint32_t bit = 0; bit < 16; ++bit) {
        if (pagesize_reg & (1U << bit)) {
            page_shift = 12 + bit;
            break;
        }
    }
    ctrl.page_size = 1U << page_shift;

    printf("xhci: version=%x.%x ports=%u slots=%u intrs=%u\n",
           ctrl.hci_version >> 8, ctrl.hci_version & 0xFFU, ctrl.max_ports, ctrl.max_slots,
           ctrl.max_intrs);
    printf("xhci: page_size=%u context_size=%u scratchpad_bufs=%u ac64=%u\n",
           ctrl.page_size, ctrl.context_size, ctrl.max_scratchpad_bufs,
           XHCI_HCCPARAMS1_AC64(hccparams1));
    printf("xhci: cap_length=%u dboff=%x rtsoff=%x\n", ctrl.cap_length, ctrl.dboff, ctrl.rtsoff);

    find_extended_capability(hccparams1, 0, true); /* want_id=0 never matches: log-only pass */

    ctrl.present = bring_up_controller(hccparams1);
    if (!ctrl.present) {
        printf("xhci: bring-up failed; USB support disabled for this boot\n");
    }
}

bool xhci_present(void)
{
    return ctrl.present;
}
