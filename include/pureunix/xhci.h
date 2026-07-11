#ifndef PUREUNIX_XHCI_H
#define PUREUNIX_XHCI_H

#include <pureunix/config.h>
#include <pureunix/pci.h>
#include <pureunix/types.h>

/* xHCI (Extensible Host Controller Interface) driver -- native USB 3.x host
 * controller support. See docs/usb.md for the full architecture writeup.
 *
 * Register layout below follows the xHCI 1.2 specification section
 * numbers referenced in each comment. All MMIO registers are little-endian,
 * matching this (i686) CPU's own endianness, so no byteswapping is needed.
 */

/* ---- PCI class code identifying an xHCI controller (xHCI 1.2 sec 4.21) -- */
#define XHCI_PCI_CLASS_SERIAL_BUS   0x0CU
#define XHCI_PCI_SUBCLASS_USB       0x03U
#define XHCI_PCI_PROGIF_XHCI        0x30U

/* ---- Capability register offsets, relative to the MMIO BAR base
 * (xHCI 1.2 sec 5.3) ------------------------------------------------------ */
#define XHCI_CAP_CAPLENGTH   0x00U /* 1 byte: offset to operational regs */
#define XHCI_CAP_HCIVERSION  0x02U /* 2 bytes: BCD interface version */
#define XHCI_CAP_HCSPARAMS1  0x04U
#define XHCI_CAP_HCSPARAMS2  0x08U
#define XHCI_CAP_HCSPARAMS3  0x0CU
#define XHCI_CAP_HCCPARAMS1  0x10U
#define XHCI_CAP_DBOFF       0x14U /* doorbell array offset from base */
#define XHCI_CAP_RTSOFF      0x18U /* runtime register offset from base */
#define XHCI_CAP_HCCPARAMS2  0x1CU /* only valid if HCIVERSION >= 0x0110 */

/* HCSPARAMS1 fields (sec 5.3.3) */
#define XHCI_HCSPARAMS1_MAX_SLOTS(v)  ((uint32_t)(v) & 0xFFU)
#define XHCI_HCSPARAMS1_MAX_INTRS(v)  (((uint32_t)(v) >> 8) & 0x7FFU)
#define XHCI_HCSPARAMS1_MAX_PORTS(v)  (((uint32_t)(v) >> 24) & 0xFFU)

/* HCSPARAMS2 fields (sec 5.3.4) -- Max Scratchpad Buffers is a 10-bit value
 * split across a low 5-bit field and a high 5-bit field. */
#define XHCI_HCSPARAMS2_IST(v)              ((uint32_t)(v) & 0xFU)
#define XHCI_HCSPARAMS2_ERST_MAX(v)         (((uint32_t)(v) >> 4) & 0xFU)
#define XHCI_HCSPARAMS2_MAX_SCRATCHPAD(v) \
    ((((uint32_t)(v) >> 27) & 0x1FU) | ((((uint32_t)(v) >> 21) & 0x1FU) << 5))
#define XHCI_HCSPARAMS2_SPR(v)              (((uint32_t)(v) >> 26) & 0x1U)

/* HCCPARAMS1 fields (sec 5.3.6) */
#define XHCI_HCCPARAMS1_AC64(v)    ((uint32_t)(v) & 0x1U)
#define XHCI_HCCPARAMS1_CSZ(v)     (((uint32_t)(v) >> 2) & 0x1U)
#define XHCI_HCCPARAMS1_PORT_POWER(v) (((uint32_t)(v) >> 3) & 0x1U)
#define XHCI_HCCPARAMS1_MAX_PSA_SIZE(v) (((uint32_t)(v) >> 12) & 0xFU)
#define XHCI_HCCPARAMS1_XECP(v)    (((uint32_t)(v) >> 16) & 0xFFFFU)

/* Context size in bytes, driven by the HCCPARAMS1.CSZ capability bit -- 32
 * bytes if clear, 64 if set. Every Slot/Endpoint/Input-Control context
 * offset calculation in this driver must go through the runtime value
 * captured from this at init time, never a hardcoded sizeof(struct). QEMU's
 * qemu-xhci model always reports CSZ=0, so getting this wrong is invisible
 * in QEMU and only surfaces on real 64-byte-context hardware. */
#define XHCI_CONTEXT_SIZE_32 32U
#define XHCI_CONTEXT_SIZE_64 64U

/* ---- Extended capability list (sec 7): singly-linked list of capabilities
 * starting at MMIO base + (HCCPARAMS1.xECP << 2). Each capability's own
 * "Next Capability Pointer" field is a dword offset relative to *that
 * capability's own address*, not the MMIO base -- easy off-by-shift bug if
 * conflated with xECP's base-relative offset. Pointer of 0 means "no more
 * capabilities". */
#define XHCI_XECP_ID(dw)          ((uint32_t)(dw) & 0xFFU)
#define XHCI_XECP_NEXT_DWORDS(dw) (((uint32_t)(dw) >> 8) & 0xFFU)

#define XHCI_XECP_ID_USB_LEGACY_SUPPORT   1U
#define XHCI_XECP_ID_SUPPORTED_PROTOCOL   2U

/* USB Legacy Support Capability (sec 7.1): word 0 holds the BIOS/OS
 * ownership semaphores on top of the standard cap-ID/next-pointer dword;
 * word 1 (offset +4) is USBLEGCTLSTS, holding SMI enable bits and RW1C SMI
 * status bits that a buggy BIOS SMM handler can keep asserting unless
 * explicitly cleared after taking ownership. */
#define XHCI_USBLEGSUP_BIOS_OWNED  (1U << 16)
#define XHCI_USBLEGSUP_OS_OWNED    (1U << 24)
/* USBLEGCTLSTS SMI enable bits (writable, must be cleared on handoff) and
 * RW1C SMI status bits (must be written back to clear pending SMIs). */
#define XHCI_USBLEGCTLSTS_SMI_ENABLE_MASK 0x0000E01FU
#define XHCI_USBLEGCTLSTS_SMI_STATUS_MASK 0xE01F0000U

/* Bounded-iteration cap shared by every busy-wait polling loop in this
 * driver (BIOS handoff, HCRST/CNR, command-ring self-test, ...) -- mirrors
 * e1000_reset()'s pattern (drivers/e1000.c) of never hanging kernel_main()
 * indefinitely on hardware that fails to respond; every wait degrades to a
 * logged failure with xHCI left disabled rather than a boot hang. */
#define XHCI_POLL_ITERATIONS 1000000U

/* Bound on usb_hc_ops_t.bulk_transfer()'s wait for a single bulk transfer's
 * completion (pit_sleep()-based, unlike the pre-interrupt polling loops
 * above -- see bulk_transfer()'s own comment in xhci.c). Unlike EP0 control
 * transfers (an already-proven, effectively-never-removable-mid-transfer
 * endpoint, hence wait_transfer_irq()'s unbounded wait), a real USB storage
 * device can be yanked or simply stop responding mid-transfer, and this
 * kernel is cooperatively scheduled with no preemption -- an unbounded wait
 * here would hang the entire kernel forever on a lost completion. */
#define XHCI_BULK_TRANSFER_TIMEOUT_MS 5000U

/* ---- Operational register offsets, relative to (MMIO base + CAPLENGTH)
 * (xHCI 1.2 sec 5.4) -------------------------------------------------------- */
#define XHCI_OP_USBCMD        0x00U
#define XHCI_OP_USBSTS        0x04U
#define XHCI_OP_PAGESIZE      0x08U
#define XHCI_OP_DNCTRL        0x14U
#define XHCI_OP_CRCR          0x18U /* 8 bytes */
#define XHCI_OP_DCBAAP        0x30U /* 8 bytes */
#define XHCI_OP_CONFIG        0x38U
#define XHCI_OP_PORTSC_BASE   0x400U
#define XHCI_OP_PORTSC_STRIDE 0x10U

/* USBCMD fields (sec 5.4.1) */
#define XHCI_USBCMD_RUN    (1U << 0) /* R/S: Run(1)/Stop(0) */
#define XHCI_USBCMD_HCRST  (1U << 1) /* Host Controller Reset */
#define XHCI_USBCMD_INTE   (1U << 2) /* Interrupter Enable */

/* USBSTS fields (sec 5.4.2) */
#define XHCI_USBSTS_HCH   (1U << 0)  /* HCHalted */
#define XHCI_USBSTS_HSE   (1U << 2)  /* Host System Error */
#define XHCI_USBSTS_EINT  (1U << 3)  /* Event Interrupt */
#define XHCI_USBSTS_PCD   (1U << 4)  /* Port Change Detect */
#define XHCI_USBSTS_CNR   (1U << 11) /* Controller Not Ready */

/* CRCR fields (sec 5.4.5) -- only RCS/CS/CA are writable; CRR is read-only.
 * Must only be written while CRR==0 (i.e. before the first doorbell-0 ring /
 * before USBCMD.RUN). */
#define XHCI_CRCR_RCS       (1U << 0) /* Ring Cycle State: matches initial PCS */
#define XHCI_CRCR_CS        (1U << 1) /* Command Stop */
#define XHCI_CRCR_CA        (1U << 2) /* Command Abort */
#define XHCI_CRCR_CRR       (1U << 3) /* Command Ring Running (RO) */
#define XHCI_CRCR_PTR_MASK  0xFFFFFFC0U /* low dword pointer bits, 64-byte aligned */

/* ---- Runtime register set: MMIO base + RTSOFF (sec 5.5). Interrupter
 * register set N starts at (runtime base + 0x20 + N*0x20); this driver only
 * ever uses interrupter 0 (the "primary interrupter"), matching the task's
 * single-interrupter scope. -------------------------------------------- */
#define XHCI_RT_IR0_BASE   0x20U
#define XHCI_RT_IMAN       0x00U
#define XHCI_RT_IMOD       0x04U
#define XHCI_RT_ERSTSZ     0x08U
#define XHCI_RT_ERSTBA     0x10U /* 8 bytes */
#define XHCI_RT_ERDP       0x18U /* 8 bytes */

#define XHCI_IMAN_IP  (1U << 0) /* Interrupt Pending (RW1C) */
#define XHCI_IMAN_IE  (1U << 1) /* Interrupt Enable */

#define XHCI_ERDP_EHB       (1U << 3) /* Event Handler Busy (RW1C) */
#define XHCI_ERDP_PTR_MASK  0xFFFFFFF0U /* low dword pointer bits, 16-byte aligned */

/* Event Ring Segment Table entry (sec 6.5) -- a separate allocation from the
 * event ring segment(s) it describes; ERSTBA points at an array of these,
 * ERSTSZ (interrupter register) is the array's element count. */
typedef struct __attribute__((packed)) xhci_erst_entry {
    uint32_t base_lo;
    uint32_t base_hi;
    uint32_t size;     /* bits 15:0 = segment size in TRBs; rest reserved */
    uint32_t reserved;
} xhci_erst_entry_t;

/* ---- Doorbell array: MMIO base + DBOFF (sec 5.6), one 32-bit register per
 * slot ID (0 = command ring, 1..MaxSlotsEn = device slots). Target (DCI for
 * device slots) in bits 7:0; Stream ID in bits 31:16 (always 0 here --
 * streams are bulk-endpoint-only in xHCI 1.x and irrelevant to control/
 * interrupt endpoints). DCI convention: DCI=1 is always EP0 (control); for
 * endpoint number e direction d, DCI = e*2 + (d==IN ? 1 : 0). */
#define XHCI_DOORBELL_TARGET_COMMAND_RING 0U
#define XHCI_EP0_DCI 1U

/* ---- TRB (Transfer Request Block): the fundamental 16-byte unit of every
 * ring (command, event, transfer) in xHCI (sec 4.11). ------------------- */
#define XHCI_TRB_SIZE 16U

typedef struct __attribute__((packed)) xhci_trb {
    uint32_t parameter_lo;
    uint32_t parameter_hi;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

/* TRB Control field bits common across most TRB types (sec 6.4). */
#define XHCI_TRB_CYCLE       (1U << 0)  /* C: Cycle bit */
#define XHCI_TRB_CONTROL_TC  (1U << 1)  /* Toggle Cycle -- Link TRB only */
#define XHCI_TRB_CONTROL_ENT (1U << 1)  /* Evaluate Next TRB -- non-Link TRBs */
#define XHCI_TRB_CONTROL_ISP (1U << 2)  /* Interrupt on Short Packet -- transfer rings only */
#define XHCI_TRB_CONTROL_CH  (1U << 4)  /* Chain bit -- transfer rings only */
#define XHCI_TRB_CONTROL_IOC (1U << 5)  /* Interrupt On Completion */
#define XHCI_TRB_CONTROL_IDT (1U << 6)  /* Immediate Data -- Setup Stage only */

#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRB_TYPE_MASK  0x3FU
#define XHCI_TRB_TYPE(t)    (((uint32_t)(t) & XHCI_TRB_TYPE_MASK) << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_GET_TYPE(control) (((uint32_t)(control) >> XHCI_TRB_TYPE_SHIFT) & XHCI_TRB_TYPE_MASK)

/* TRB Type field values actually used by this driver (sec 6.4.6). Types not
 * listed (Isoch, No-Op transfer, etc.) aren't used by a Boot Protocol HID
 * keyboard and are out of scope. */
#define XHCI_TRB_TYPE_NORMAL                     1U
#define XHCI_TRB_TYPE_SETUP_STAGE                2U
#define XHCI_TRB_TYPE_DATA_STAGE                 3U
#define XHCI_TRB_TYPE_STATUS_STAGE               4U
#define XHCI_TRB_TYPE_LINK                       6U
#define XHCI_TRB_TYPE_ENABLE_SLOT_CMD            9U
#define XHCI_TRB_TYPE_DISABLE_SLOT_CMD           10U
#define XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD         11U
#define XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD     12U
#define XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD       13U
/* Used by bulk-transfer error recovery (drivers/usb_msd.c via
 * usb_hc_ops_t.reset_endpoint()) -- Reset Endpoint is only legal from the
 * Halted state (a real STALL), Stop Endpoint is required first from Running
 * (e.g. a software timeout, where the controller never reported any error
 * so the endpoint is still Running), and Set TR Dequeue Pointer is what
 * actually un-wedges the ring afterward in both cases. See
 * XHCI_EP_STATE_* below and reset_endpoint()'s own comment. */
#define XHCI_TRB_TYPE_RESET_ENDPOINT_CMD         14U
#define XHCI_TRB_TYPE_STOP_ENDPOINT_CMD          15U
#define XHCI_TRB_TYPE_SET_TR_DEQUEUE_POINTER_CMD 16U
#define XHCI_TRB_TYPE_NOOP_CMD                   23U
#define XHCI_TRB_TYPE_TRANSFER_EVENT             32U
#define XHCI_TRB_TYPE_COMMAND_COMPLETION_EVENT   33U
#define XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT   34U

/* Command TRBs that target a specific endpoint (Reset Endpoint, Stop
 * Endpoint, Set TR Dequeue Pointer -- unlike Configure Endpoint, which
 * targets whichever DCIs the Input Context's add/drop flags name) carry
 * Slot ID in Control bits 31:24 (same position Command Completion Events
 * report it at, XHCI_TRB_GET_SLOT_ID) and Endpoint ID (DCI) in bits 20:16. */
#define XHCI_TRB_SLOT_ID_SHIFT     24U
#define XHCI_TRB_ENDPOINT_ID_SHIFT 16U

/* Enable Slot Command TRB's Control dword carries the Slot Type (from the
 * matching Supported Protocol Capability's own Protocol Slot Type field --
 * see XHCI_SUPPORTED_PROTOCOL_SLOT_TYPE below) in bits 20:16. */
#define XHCI_ENABLE_SLOT_TYPE_SHIFT 16U

/* Address Device Command TRB Control dword's BSR (Block Set Address
 * Request) bit. Set (1) for the first of the two-stage Address Device
 * dance this driver uses (xhci.c's address_device()): seeds the EP0
 * context/ring and lets a control transfer read the real bMaxPacketSize0
 * without the controller emitting a real SET_ADDRESS yet. Clear (0) for
 * the second stage, where the controller performs the real SET_ADDRESS and
 * the slot transitions to the Addressed state. */
#define XHCI_ADDRESS_DEVICE_BSR (1U << 9)

/* Setup Stage TRB's Transfer Type (TRT) field, Control dword bits 17:16
 * (sec 6.4.1.2.1) -- which data/status stages follow. */
#define XHCI_TRT_SHIFT    16U
#define XHCI_TRT_NO_DATA  0U
#define XHCI_TRT_OUT      2U
#define XHCI_TRT_IN       3U

/* Data/Status Stage TRB direction bit, Control dword bit 16 (1 = IN). */
#define XHCI_TRB_DIR_IN (1U << 16)

/* Completion Code field (Status dword, bits 31:24) values this driver checks
 * for explicitly; every other nonzero value is treated as "some failure",
 * logged with its raw numeric code rather than named individually. */
#define XHCI_COMPLETION_SUCCESS       1U
#define XHCI_COMPLETION_STALL_ERROR   6U  /* real device STALL */
#define XHCI_COMPLETION_SHORT_PACKET  13U /* fewer bytes than requested -- not itself an error (ISP) */
#define XHCI_COMPLETION_CONTEXT_STATE_ERROR 19U /* wrong EP State for the command issued */
#define XHCI_TRB_GET_COMPLETION_CODE(status) (((uint32_t)(status) >> 24) & 0xFFU)
/* Command Completion Event's Control dword carries the Slot ID (for
 * commands that allocate/target one, e.g. Enable Slot) in bits 31:24. A
 * Transfer Event's Control dword also carries a Slot ID at the same bit
 * position, plus an Endpoint ID (numerically the same as DCI) at bits
 * 20:16 identifying which endpoint's ring the completed TRB belongs to --
 * this is how process_event_ring() (xhci.c) routes a Transfer Event to the
 * right interrupt-transfer listener without needing to search by TRB
 * pointer the way command completions and one-shot control transfers do. */
#define XHCI_TRB_GET_SLOT_ID(control) (((uint32_t)(control) >> 24) & 0xFFU)
#define XHCI_TRB_GET_ENDPOINT_ID(control) (((uint32_t)(control) >> 16) & 0x1FU)

/* One page (4096 bytes / 16-byte TRBs = 256 slots) backs every ring this
 * driver allocates -- see docs/usb.md for why this is sufficient without a
 * general contiguous-DMA allocator. Command/transfer rings reserve their
 * last slot for a mandatory Link TRB (255 usable); the event ring has no
 * Link TRB (its wrap is defined by the ERST instead), so all 256 slots are
 * usable there. */
#define XHCI_TRBS_PER_PAGE (PUREUNIX_PAGE_SIZE / XHCI_TRB_SIZE)

/* Compile-time cap on device slots this driver tracks -- clamped against
 * whatever HCSPARAMS1.MaxSlots actually reports at init time. 32 is far
 * more than a single boot keyboard (plus headroom for a future hub/storage
 * device) ever needs. */
#define XHCI_MAX_SLOTS_SUPPORTED 32U

/* ---- PORTSC (Port Status and Control), one per port, at (op_base +
 * XHCI_OP_PORTSC_BASE + (port-1)*XHCI_OP_PORTSC_STRIDE) for port = 1..
 * MaxPorts (sec 5.4.8). Several bits are RW1C (write 1 to clear) -- a
 * naive read-modify-write of this register would unintentionally clear
 * every pending change bit at once, so callers must always mask to the
 * specific bits they intend to preserve vs. clear (see
 * XHCI_PORTSC_PRESERVE_MASK / XHCI_PORTSC_CHANGE_BITS below). ------------ */
#define XHCI_PORTSC_CCS    (1U << 0)  /* Current Connect Status (RO) */
#define XHCI_PORTSC_PED    (1U << 1)  /* Port Enabled/Disabled */
#define XHCI_PORTSC_PR     (1U << 4)  /* Port Reset (RW) */
#define XHCI_PORTSC_PLS_SHIFT 5U
#define XHCI_PORTSC_PLS_MASK  0xFU
#define XHCI_PORTSC_PP     (1U << 9)  /* Port Power */
#define XHCI_PORTSC_SPEED_SHIFT 10U
#define XHCI_PORTSC_SPEED_MASK  0xFU
#define XHCI_PORTSC_CSC    (1U << 17) /* Connect Status Change (RW1C) */
#define XHCI_PORTSC_PEC    (1U << 18) /* Port Enabled/Disabled Change (RW1C) */
#define XHCI_PORTSC_WRC    (1U << 19) /* Warm Port Reset Change (RW1C) */
#define XHCI_PORTSC_OCC    (1U << 20) /* Over-current Change (RW1C) */
#define XHCI_PORTSC_PRC    (1U << 21) /* Port Reset Change (RW1C) */
#define XHCI_PORTSC_PLC    (1U << 22) /* Port Link State Change (RW1C) */
#define XHCI_PORTSC_CEC    (1U << 23) /* Config Error Change (RW1C) */

#define XHCI_PORTSC_CHANGE_BITS \
    (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | \
     XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)
/* Bits a read-modify-write must preserve verbatim (not RW1C, and not safe
 * to just re-write the value most recently read back, since PP/PLS carry
 * real hardware state this driver never intends to change as a side effect
 * of clearing an unrelated change bit). */
#define XHCI_PORTSC_PRESERVE_MASK (XHCI_PORTSC_PP | (XHCI_PORTSC_PLS_MASK << XHCI_PORTSC_PLS_SHIFT))

#define XHCI_PORTSC_GET_SPEED(portsc) (((uint32_t)(portsc) >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK)

/* Default USB Speed ID values (sec 7.2.1, Table 7-13) -- the mapping every
 * port uses unless the Supported Protocol Capability defines custom
 * Protocol Speed ID entries, a USB3-only feature this driver's USB2-only
 * scope never needs to parse (see find_usb2_protocol() in xhci.c). */
#define XHCI_SPEED_FULL  1U /* 12 Mbps */
#define XHCI_SPEED_LOW   2U /* 1.5 Mbps */
#define XHCI_SPEED_HIGH  3U /* 480 Mbps */
#define XHCI_SPEED_SUPER 4U /* 5 Gbps -- USB3, out of this driver's scope */

/* ---- Supported Protocol Capability (sec 7.2): maps a contiguous PORTSC
 * index range to a USB major revision (2 vs 3) and a Slot Type value
 * required by the Enable Slot Command. This driver only enumerates devices
 * on USB2-labeled port ranges (LS/FS/HS -- covers every Boot Protocol
 * keyboard); USB3-only ranges are logged and skipped (see docs/usb.md's
 * "no SuperSpeed enumeration" limitation). Capability layout: dword0 =
 * standard cap-ID/next-pointer header plus Minor/Major Revision; dword1 =
 * "USB " name string (unused here); dword2 = Compatible Port Offset/Count;
 * dword3 = Protocol Slot Type. */
#define XHCI_SUPPORTED_PROTOCOL_MAJOR(dw0)        (((uint32_t)(dw0) >> 24) & 0xFFU)
#define XHCI_SUPPORTED_PROTOCOL_PORT_OFFSET(dw2)  ((uint32_t)(dw2) & 0xFFU)
#define XHCI_SUPPORTED_PROTOCOL_PORT_COUNT(dw2)   (((uint32_t)(dw2) >> 8) & 0xFFU)
#define XHCI_SUPPORTED_PROTOCOL_SLOT_TYPE(dw3)    ((uint32_t)(dw3) & 0x1FU)

/* ---- Slot/Endpoint/Input-Control Context structures (sec 6.2). Each is
 * spec-defined as exactly 32 bytes when HCCPARAMS1.CSZ==0, or 32 real
 * bytes followed by 32 reserved bytes (64 total) when CSZ==1 -- these
 * structs model only the real 32 bytes. Callers locate a given context's
 * struct within the surrounding Device/Input Context page using
 * device_context_entry()/input_context_entry() (xhci.c), which stride by
 * the runtime-captured context_size (32 or 64), never sizeof(these
 * structs), so the trailing 32 reserved bytes on CSZ==1 hardware are
 * correctly skipped over rather than overlaid by the next context --
 * getting this wrong is invisible on QEMU (CSZ always 0) and only surfaces
 * on real 64-byte-context hardware. ------------------------------------- */
typedef struct __attribute__((packed)) xhci_slot_context {
    uint32_t dword0; /* Route String[19:0], Speed[23:20], MTT[25], Hub[26], Context Entries[31:27] */
    uint32_t dword1; /* Max Exit Latency[15:0], Root Hub Port Number[23:16], Number of Ports[31:24] */
    uint32_t dword2; /* Parent Hub Slot ID[7:0], Parent Port Number[15:8], Interrupter Target[31:22] */
    uint32_t dword3; /* USB Device Address[7:0], Slot State[31:27] */
    uint32_t reserved[4];
} xhci_slot_context_t;

#define XHCI_SLOT_SPEED_SHIFT               20U
#define XHCI_SLOT_CONTEXT_ENTRIES_SHIFT     27U
#define XHCI_SLOT_ROOT_HUB_PORT_SHIFT       16U
#define XHCI_SLOT_INTERRUPTER_TARGET_SHIFT  22U
#define XHCI_SLOT_GET_USB_ADDRESS(dword3)   ((uint32_t)(dword3) & 0xFFU)

typedef struct __attribute__((packed)) xhci_endpoint_context {
    uint32_t dword0;        /* EP State[2:0], Interval[23:16] */
    uint32_t dword1;        /* CErr[2:1], EP Type[5:3], Max Burst Size[15:8], Max Packet Size[31:16] */
    uint32_t tr_dequeue_lo; /* bit0 = DCS (Dequeue Cycle State), bits 31:4 = pointer */
    uint32_t tr_dequeue_hi;
    uint32_t dword4;        /* Average TRB Length[15:0] */
    uint32_t reserved[3];
} xhci_endpoint_context_t;

/* EP Type field values (Table 6-9) this driver uses -- Control (bidirectional)
 * for EP0, Interrupt IN for a Boot Protocol HID keyboard's report endpoint,
 * Bulk IN/OUT for a Mass Storage device's data pipes (drivers/usb_msd.c).
 * Isoch types remain unused. */
#define XHCI_EP_TYPE_BULK_OUT             2U
#define XHCI_EP_TYPE_CONTROL              4U
#define XHCI_EP_TYPE_BULK_IN              6U
#define XHCI_EP_TYPE_INTERRUPT_IN         7U
#define XHCI_EP_TYPE_SHIFT                3U
#define XHCI_EP_CERR_SHIFT                1U
#define XHCI_EP_INTERVAL_SHIFT            16U
#define XHCI_EP_MAX_PACKET_SIZE_SHIFT     16U
#define XHCI_EP_MAX_PACKET_SIZE_MASK      0xFFFFU
#define XHCI_EP_DEQUEUE_CYCLE_STATE       (1U << 0)

/* Endpoint Context dword0 bits 2:0 (sec 6.2.3) -- live endpoint state, read
 * from the *Device* Context (not the Input Context, which is only ever a
 * staging area for commands) to decide how reset_endpoint() must recover a
 * wedged bulk endpoint: see XHCI_TRB_TYPE_RESET_ENDPOINT_CMD's comment. */
#define XHCI_EP_STATE_MASK     0x7U
#define XHCI_EP_STATE_DISABLED 0U
#define XHCI_EP_STATE_RUNNING  1U
#define XHCI_EP_STATE_HALTED   2U
#define XHCI_EP_STATE_STOPPED  3U
#define XHCI_EP_STATE_ERROR    4U
#define XHCI_EP_GET_STATE(dword0) ((uint32_t)(dword0) & XHCI_EP_STATE_MASK)
/* Average TRB Length has no "correct" value known in advance; the spec
 * explicitly recommends 8 for Control endpoints (sec 4.14.1). For an
 * Interrupt endpoint carrying small fixed-size reports (an 8-byte HID boot
 * report), the report size itself is a reasonable estimate. */
#define XHCI_EP_AVERAGE_TRB_LENGTH_CONTROL_DEFAULT 8U

typedef struct __attribute__((packed)) xhci_input_control_context {
    uint32_t drop_flags; /* D2..D31 -- which DCIs to remove; D0/D1 reserved=0 */
    uint32_t add_flags;  /* A0..A31 -- which DCIs to add/modify; A0=Slot Context */
    uint32_t reserved[6];
} xhci_input_control_context_t;

#define XHCI_INPUT_CONTROL_ADD_SLOT     (1U << 0) /* A0 */
#define XHCI_INPUT_CONTROL_ADD_EP(dci)  (1U << (dci)) /* A[dci], dci=1 is EP0 */

/* A command/transfer/event ring: one DMA page of TRBs plus the software
 * producer/consumer state needed to walk it. Command and transfer rings use
 * enqueue_index/cycle_state as the *producer* (PCS); the event ring uses
 * dequeue_index/cycle_state as the *consumer* (CCS) -- the two roles never
 * overlap for a given ring, so one struct covers both. */
typedef struct xhci_ring {
    xhci_trb_t *trbs;     /* virt == phys, see docs/usb.md */
    phys_addr_t phys;
    uint32_t enqueue_index; /* producer rings only */
    uint32_t dequeue_index; /* consumer (event) ring only */
    bool cycle_state;       /* PCS for producer rings, CCS for the event ring */
} xhci_ring_t;

/* Per-slot software state: the Input/Device Context pages and EP0 transfer
 * ring a device occupies once addressed. Indexed by Slot ID (1..
 * slots_enabled); index 0 is unused (Slot ID 0 is never valid). */

/* One more than the highest possible DCI (31, endpoint 15 IN) -- index 0 is
 * unused (DCI 0 is never valid; the Slot Context lives there conceptually
 * but has no transfer ring), index XHCI_EP0_DCI (1) is always EP0 once a
 * device is addressed, higher indices are populated on demand by
 * configure_endpoint() (xhci.c) for whichever additional endpoints a
 * device's class driver actually needs (today: at most one Interrupt IN
 * endpoint, for a Boot Protocol HID keyboard -- see drivers/hid.c). */
#define XHCI_MAX_DCI 32U

typedef struct xhci_slot_state {
    bool in_use;

    void *input_ctx;  /* 1 page */
    phys_addr_t input_ctx_phys;
    void *device_ctx; /* 1 page, address stored in dcbaa[slot_id] */
    phys_addr_t device_ctx_phys;

    /* Indexed by DCI -- see XHCI_MAX_DCI's comment. */
    xhci_ring_t rings[XHCI_MAX_DCI];

    uint32_t port;  /* 1-based PORTSC index this device is attached to */
    uint32_t speed; /* XHCI_SPEED_* */
} xhci_slot_state_t;

/* Runtime-captured controller state (register layout mirrors what's parsed,
 * not a raw MMIO overlay -- registers are read/written explicitly via the
 * offsets above, since operational/runtime/doorbell regions all live at
 * variable offsets from the capability registers that must be parsed
 * first). One global instance -- this kernel has no multi-controller
 * support today, matching e1000's single-NIC assumption. */
typedef struct xhci_controller {
    bool present;

    pci_device_t pci;
    volatile uint8_t *mmio; /* BAR base, byte-addressed for offset math */

    uint8_t  cap_length;
    uint16_t hci_version;
    uint8_t  max_slots;
    uint16_t max_intrs;
    uint8_t  max_ports;
    uint16_t max_scratchpad_bufs;
    uint32_t page_size; /* actual byte page size the controller reports */
    uint32_t context_size; /* XHCI_CONTEXT_SIZE_32 or _64, from CSZ */
    uint32_t dboff;
    uint32_t rtsoff;

    volatile uint8_t *op_base;
    volatile uint8_t *db_base;
    volatile uint8_t *rt_base;

    uint32_t slots_enabled; /* CONFIG.MaxSlotsEn -- min(max_slots, XHCI_MAX_SLOTS_SUPPORTED) */

    uint64_t *dcbaa;       /* Device Context Base Address Array, 1 page */
    phys_addr_t dcbaa_phys;

    xhci_ring_t cmd_ring;
    xhci_ring_t event_ring;
    xhci_erst_entry_t *erst; /* 1 page, ERSTSZ=1 entry describing event_ring */
    phys_addr_t erst_phys;

    /* Index 1..slots_enabled; index 0 unused (Slot ID 0 is never valid). */
    xhci_slot_state_t slots[XHCI_MAX_SLOTS_SUPPORTED + 1];
} xhci_controller_t;

/* Detects the xHCI controller via PCI class code, maps its MMIO BAR, and
 * parses + logs the capability register block (version, ports, slots, page
 * size, context size, scratchpad count, extended capabilities). Safe to
 * call whether or not a controller is present -- xhci_present() reports the
 * outcome. Must be called before arch_enable_interrupts() (see
 * kernel/main.c), alongside pci_scan()/e1000_init(). */
void xhci_init(void);

bool xhci_present(void);

/* Scans every USB2-labeled port (see the Supported Protocol Capability
 * comment above) for a connected device, and for each one: resets the
 * port, enables a slot, addresses the device, and reads its device
 * descriptor -- logging every stage per docs/usb.md's diagnostic
 * requirements. Must be called after arch_enable_interrupts() (see
 * kernel/main.c): unlike xhci_init()'s pre-interrupt bring-up, this relies
 * on the real interrupt-driven command/transfer completion path
 * (xhci_irq()), not direct event-ring polling. A no-op if xhci_present()
 * is false. */
void xhci_enumerate(void);

#endif
