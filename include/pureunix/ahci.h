#ifndef PUREUNIX_AHCI_H
#define PUREUNIX_AHCI_H

#include <pureunix/disk.h>
#include <pureunix/types.h>

/* AHCI (Advanced Host Controller Interface) driver -- native SATA support.
 *
 * Exists because drivers/ata.c only speaks the legacy IDE/PATA I/O-port
 * interface (0x1F0/0x3F6), which a real modern SATA-only board (no PATA
 * controller at all) simply does not expose -- ata_init()'s own IDENTIFY
 * probe on those ports finds nothing to probe in the first place, not a
 * device that fails to respond. Real hardware built after PATA was dropped
 * exposes its on-board SATA ports through one of two PCI-enumerable
 * controllers instead: AHCI (class 0x01/0x06/0x01, this driver) or NVMe
 * (class 0x01/0x08/0x02, out of scope -- see ahci_init()'s own diagnostic
 * for how that's detected and reported rather than silently missed).
 *
 * Register layout follows the Serial ATA AHCI 1.3.1 specification section
 * numbers referenced in each comment. Like drivers/xhci.c, this only
 * targets a 32-bit, non-PAE, sub-4GB physical address space -- every BAR
 * and every DMA structure allocated here is assumed to fit that.
 */

/* ---- PCI class code identifying an AHCI controller, formatted as
 * class/subclass/prog-if triples the same way xhci.h's XHCI_PCI_* already
 * does -- ------------------------------------------------------------------ */
#define AHCI_PCI_CLASS_STORAGE   0x01U
#define AHCI_PCI_SUBCLASS_SATA   0x06U
#define AHCI_PCI_PROGIF_AHCI     0x01U

/* NVMe's own class/subclass/prog-if -- this driver never talks to one, but
 * ahci_init() checks for it anyway so a board that turns out to use NVMe
 * instead of AHCI produces a loud, specific "NVMe controller found but not
 * supported" diagnostic rather than the same silent "no persistent root
 * disk" a genuinely absent controller would produce. */
#define NVME_PCI_CLASS_STORAGE   0x01U
#define NVME_PCI_SUBCLASS_NVM    0x08U
#define NVME_PCI_PROGIF_NVME     0x02U

/* ---- Generic Host Control registers, relative to the ABAR (BAR5) base
 * (AHCI sec 3.1) ------------------------------------------------------------ */
#define AHCI_REG_CAP      0x00U /* Host Capabilities */
#define AHCI_REG_GHC      0x04U /* Global HBA Control */
#define AHCI_REG_IS       0x08U /* Interrupt Status */
#define AHCI_REG_PI       0x0CU /* Ports Implemented (bitmap) */
#define AHCI_REG_VS       0x10U /* Version */

#define AHCI_CAP_NP(v)    (((uint32_t)(v) & 0x1FU) + 1U) /* Number of Ports (0-based) */
#define AHCI_CAP_NCS(v)   ((((uint32_t)(v) >> 8) & 0x1FU) + 1U) /* Number of Command Slots */
#define AHCI_CAP_S64A(v)  (((uint32_t)(v) >> 31) & 0x1U) /* 64-bit addressing supported */

#define AHCI_GHC_HR   (1U << 0)  /* HBA Reset */
#define AHCI_GHC_IE   (1U << 1)  /* Interrupt Enable */
#define AHCI_GHC_AE   (1U << 31) /* AHCI Enable -- must be set before touching port registers */

/* ---- Port registers: base = 0x100 + port_index * AHCI_PORT_STRIDE ------- */
#define AHCI_PORT_REGS_BASE 0x100U
#define AHCI_PORT_STRIDE    0x80U

#define AHCI_PXCLB   0x00U /* Command List Base Address (low) */
#define AHCI_PXCLBU  0x04U /* Command List Base Address (high) */
#define AHCI_PXFB    0x08U /* FIS Base Address (low) */
#define AHCI_PXFBU   0x0CU /* FIS Base Address (high) */
#define AHCI_PXIS    0x10U /* Interrupt Status */
#define AHCI_PXIE    0x14U /* Interrupt Enable */
#define AHCI_PXCMD   0x18U /* Command and Status */
#define AHCI_PXTFD   0x20U /* Task File Data */
#define AHCI_PXSIG   0x24U /* Signature */
#define AHCI_PXSSTS  0x28U /* SATA Status (SStatus) */
#define AHCI_PXSCTL  0x2CU /* SATA Control (SControl) */
#define AHCI_PXSERR  0x30U /* SATA Error (SError) */
#define AHCI_PXSACT  0x34U /* SATA Active */
#define AHCI_PXCI    0x38U /* Command Issue */

#define AHCI_PXCMD_ST   (1U << 0)  /* Start */
#define AHCI_PXCMD_SUD  (1U << 1)  /* Spin-Up Device */
#define AHCI_PXCMD_POD  (1U << 2)  /* Power On Device */
#define AHCI_PXCMD_FRE  (1U << 4)  /* FIS Receive Enable */
#define AHCI_PXCMD_CR   (1U << 15) /* Command List Running (RO) */
#define AHCI_PXCMD_FR   (1U << 14) /* FIS Receive Running (RO) */

#define AHCI_PXTFD_STS_ERR (1U << 0) /* Task File Status.ERR */
#define AHCI_PXTFD_STS_DRQ (1U << 3) /* Task File Status.DRQ */
#define AHCI_PXTFD_STS_BSY (1U << 7) /* Task File Status.BSY */

/* PxSSTS.DET (bits 3:0): device detection + Phy communication state. 3 means
 * a device is present *and* Phy communication has been established -- the
 * only value this driver treats as "there's a real disk here". */
#define AHCI_PXSSTS_DET(v)  ((uint32_t)(v) & 0xFU)
#define AHCI_PXSSTS_DET_PRESENT 3U

/* PxSIG values (AHCI sec 3.3.8) identifying what kind of device answered
 * the initial FIS after a COMRESET -- this driver only ever mounts the
 * plain ATA (SATA disk) signature; ATAPI/enclosure/port-multiplier
 * signatures are logged and skipped (see ahci_init()). */
#define AHCI_SIG_ATA            0x00000101U
#define AHCI_SIG_ATAPI          0xEB140101U
#define AHCI_SIG_ENCLOSURE      0xC33C0101U
#define AHCI_SIG_PORT_MULTIPLIER 0x96690101U

/* ---- FIS types this driver emits/reads (Serial ATA rev 3.x sec 10.3) --- */
#define AHCI_FIS_TYPE_REG_H2D 0x27U

/* ATA commands this driver issues -- LBA48 (*_EXT) throughout, since a real
 * SATA disk found here is never assumed to fit in LBA28's 128GiB limit, and
 * every disk that answers AHCI_SIG_ATA supports the 48-bit command set
 * (mandatory since ATA/ATAPI-6, decades before AHCI existed). */
#define AHCI_ATA_CMD_READ_DMA_EXT    0x25U
#define AHCI_ATA_CMD_WRITE_DMA_EXT   0x35U
#define AHCI_ATA_CMD_FLUSH_CACHE_EXT 0xEAU

/* Fixed number of AHCI disks this driver tracks -- matches
 * drivers/usb_msd.c's USB_MSD_MAX_DEVICES style (no dynamic disk registry
 * exists in this kernel). A real board has at most a handful of on-board
 * SATA ports; this comfortably covers every real target machine plus
 * headroom. */
#define AHCI_MAX_DISKS 8

/* Detects the AHCI controller via PCI class code, maps its ABAR, enables
 * AHCI mode, and brings up every implemented port whose PxSSTS/PxSIG show a
 * real, present SATA disk (not ATAPI/enclosure/port-multiplier) -- exactly
 * mirroring ata_init()'s "probe and record .present" contract, just over a
 * real memory-mapped register interface instead of legacy I/O ports. Safe
 * to call whether or not a controller is present; safe even if bring-up
 * fails partway through a given port (that port is simply left !present,
 * every other port is unaffected). Also detects (but does not drive) an
 * NVMe controller, logging it distinctly so "no persistent root disk
 * found" on an NVMe-only board is diagnosable as "wrong controller type",
 * not "no controller at all". */
void ahci_init(void);

/* Returns the disk_device_t for the index'th present AHCI disk (0-based, in
 * PxSSTS-implemented-port scan order), or NULL if no disk occupies that
 * slot -- same shape as usb_msd_disk(), for the same reason (kernel/main.c's
 * find_persistent_root_disk() is the intended consumer). */
disk_device_t *ahci_disk(int index);

#endif
