/* drivers/usb_msd.c -- USB Mass Storage (Bulk-Only Transport + SCSI
 * transparent command set), the near-universal protocol real USB flash
 * drives implement. Mirrors drivers/hid.c's shape: usb_msd_try_attach()
 * called from xhci_enumerate() (drivers/xhci.c) once per enumerated
 * device, a no-op for anything that isn't Mass Storage. See
 * include/pureunix/usb_msd.h's own comment for scope: single LUN only, no
 * dynamic disk registry -- kernel/main.c's persistent-root search is the
 * only consumer.
 */
#include <pureunix/arch.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/usb_msd.h>
#include <pureunix/wait.h>

#define MSD_CBW_SIGNATURE 0x43425355U /* 'USBC', little-endian on the wire */
#define MSD_CSW_SIGNATURE 0x53425355U /* 'USBS' */
#define MSD_SECTOR_SIZE   512U
#define MSD_MAX_RETRIES   3
#define MSD_LUN           0U /* single-LUN only -- see usb_msd.h */

/* Bulk-Only Transport Command Block Wrapper / Command Status Wrapper (USB
 * Mass Storage Class Bulk-Only Transport spec, secs 5.1/5.2) -- packed,
 * fixed 31/13-byte wire layouts. */
typedef struct __attribute__((packed)) msd_cbw {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags; /* bit7: 1=data-in (device->host), 0=data-out */
    uint8_t  bCBWLUN;    /* bits3:0 */
    uint8_t  bCBWCBLength; /* bits4:0, 1..16 */
    uint8_t  CBWCB[16];
} msd_cbw_t; /* 31 bytes */

typedef struct __attribute__((packed)) msd_csw {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus; /* 0=passed, 1=failed, 2=phase error */
} msd_csw_t; /* 13 bytes */

#define MSD_CBW_FLAG_DATA_IN 0x80U

#define MSD_REQ_BULK_ONLY_RESET 0xFFU /* class-specific control request */

#define MSD_CSW_STATUS_PASSED      0U
#define MSD_CSW_STATUS_FAILED      1U
#define MSD_CSW_STATUS_PHASE_ERROR 2U

/* Guards msd_transact()'s entire CBW->[data]->CSW sequence (including its
 * own bounded retries) so a full logical SCSI command is atomic per device.
 * Bulk-Only Transport is only sequential (spec sec 5.3: exactly one CBW
 * outstanding at a time, its CSW always arrives before the next CBW may be
 * sent) if nothing ever interleaves two commands on the wire -- true for a
 * single task, but this driver's disk_device_t is shared by every VT's
 * login-shell session (kernel/main.c), which can all be reading/exec'ing
 * off the same USB-MSD-backed root filesystem concurrently right after
 * boot. Without this lock, one task's msd_transact() can send its CBW
 * while another task's is still mid-command, desyncing the transport (the
 * device attributes a CSW to the wrong CBW) -- indistinguishable from
 * silent data corruption on a real drive, and invisible under QEMU's
 * usb-storage backend, which completes each transfer synchronously enough
 * that this driver's own pit_sleep()-based bulk_transfer() wait (see
 * drivers/xhci.c) never actually yields into a second task's command. */
typedef struct msd_lock {
    bool busy;
    wait_queue_t wq;
} msd_lock_t;

static bool msd_lock_is_free(void *ctx)
{
    return !((msd_lock_t *)ctx)->busy;
}

static void msd_lock_acquire(msd_lock_t *lock)
{
    for (;;) {
        uint32_t flags = arch_save_and_disable_interrupts();
        if (!lock->busy) {
            lock->busy = true;
            arch_restore_interrupts(flags);
            return;
        }
        arch_restore_interrupts(flags);
        /* Same reasoning as drivers/xhci.c's identical guard on
         * bulk_xfer_lock -- see that comment for the full explanation.
         * `int $0x80` enters with interrupts masked; blocking here without
         * re-enabling them first means the wait's own `hlt` can never be
         * woken by any interrupt at all, freezing the whole single-core
         * machine, not just this task. Guaranteed here rather than relying
         * on every syscall handler that transitively reaches msd_transact()
         * (SYS_OPEN, SYS_READDIR, SYS_EXEC, ...) to remember it themselves.
         * Harmless no-op if interrupts were already enabled. */
        arch_enable_interrupts();
        wait_queue_sleep(&lock->wq, msd_lock_is_free, lock);
    }
}

static void msd_lock_release(msd_lock_t *lock)
{
    uint32_t flags = arch_save_and_disable_interrupts();
    lock->busy = false;
    arch_restore_interrupts(flags);
    wait_queue_wake_one(&lock->wq);
}

typedef struct msd_device {
    bool present;
    const usb_hc_ops_t *hc;
    uint32_t slot_id;
    uint8_t interface_number;
    uint8_t bulk_in_addr;
    uint8_t bulk_out_addr;
    uint32_t tag;
    msd_lock_t lock;
    disk_device_t disk;
} msd_device_t;

static msd_device_t devices[USB_MSD_MAX_DEVICES];

/* disk_device_t's read/write take no device-context argument (see
 * drivers/ata.c's identical master/slave split, drivers/ramdisk.c's
 * per-slot thunks) -- one pair of thunks per fixed slot. */
static int msd_disk_read(int index, uint32_t lba, uint8_t *buffer);
static int msd_disk_write(int index, uint32_t lba, const uint8_t *buffer);
#define MSD_THUNK_PAIR(n) \
    static int msd_read_##n(uint32_t lba, uint8_t *buf) { return msd_disk_read(n, lba, buf); } \
    static int msd_write_##n(uint32_t lba, const uint8_t *buf) { return msd_disk_write(n, lba, buf); }
MSD_THUNK_PAIR(0)
MSD_THUNK_PAIR(1)
MSD_THUNK_PAIR(2)
MSD_THUNK_PAIR(3)
static int (*const msd_reads[USB_MSD_MAX_DEVICES])(uint32_t, uint8_t *) = {
    msd_read_0, msd_read_1, msd_read_2, msd_read_3,
};
static int (*const msd_writes[USB_MSD_MAX_DEVICES])(uint32_t, const uint8_t *) = {
    msd_write_0, msd_write_1, msd_write_2, msd_write_3,
};
static const char *const msd_names[USB_MSD_MAX_DEVICES] = { "usb0", "usb1", "usb2", "usb3" };

/* Clears a STALLed bulk endpoint at both the USB level (CLEAR_FEATURE, a
 * real control request the device itself needs) and the host-controller
 * level (reset_endpoint(), which un-wedges the xHCI ring state) -- see
 * usb_hc_ops_t.reset_endpoint's own comment on why both are needed
 * together. Safe to call even when the endpoint merely timed out rather
 * than genuinely STALLed: reset_endpoint() reads the endpoint's real live
 * state and does the right thing either way. */
static void msd_clear_halt(msd_device_t *d, uint8_t endpoint_address)
{
    d->hc->control_transfer(d->slot_id,
                             USB_REQUEST_TYPE_HOST_TO_DEVICE | USB_REQUEST_TYPE_STANDARD
                                 | USB_REQUEST_TYPE_RECIPIENT_ENDPOINT,
                             USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, endpoint_address, 0,
                             NULL, false);
    d->hc->reset_endpoint(d->slot_id, endpoint_address);
}

/* Full Bulk-Only Mass Storage Reset (class request, BOT spec sec 3.1) plus
 * clearing both endpoints' halt state -- used when the transport itself
 * desynced (bad/mismatched CSW, phase error) rather than a plain STALL on
 * one pipe, discarding whatever command was in flight rather than
 * retrying it blind. */
static void msd_reset_recovery(msd_device_t *d)
{
    d->hc->control_transfer(d->slot_id,
                             USB_REQUEST_TYPE_HOST_TO_DEVICE | USB_REQUEST_TYPE_CLASS
                                 | USB_REQUEST_TYPE_RECIPIENT_INTERFACE,
                             MSD_REQ_BULK_ONLY_RESET, 0, d->interface_number, 0, NULL, false);
    msd_clear_halt(d, d->bulk_in_addr);
    msd_clear_halt(d, d->bulk_out_addr);
}

#define MSD_SENSE_KEY_NOT_READY     0x02U
#define MSD_SENSE_KEY_UNIT_ATTENTION 0x06U

/* REQUEST SENSE (opcode 0x03), fixed-format sense data (18 bytes) -- a raw,
 * single-shot CBW->data->CSW transaction (no retry, no recursion into
 * msd_transact_locked()) issued right after a CHECK CONDITION (CSW status
 * "Failed") to learn *why*. Real SCSI targets -- including real USB flash
 * controllers, unlike QEMU's usb-storage backend -- commonly answer the
 * very first command after a reset/power-up with CHECK CONDITION and sense
 * key UNIT ATTENTION (bus reset occurred), which the initiator is required
 * to clear by issuing REQUEST SENSE before anything else will succeed; some
 * also transiently report NOT READY (still spinning up/settling) the same
 * way. Without ever issuing this, a real drive's UNIT ATTENTION can persist
 * across every subsequent command, so usb_msd_try_attach()'s own TEST UNIT
 * READY retry loop would fail all 5 attempts and give up -- invisible under
 * QEMU, which never asserts CHECK CONDITION for either condition. Returns
 * the sense key (byte 2, low nibble); ASC/ASCQ (bytes 12/13) are logged but
 * not otherwise interpreted -- this driver only needs to tell "transient,
 * retry" apart from "a real error." */
static uint8_t msd_request_sense_raw(msd_device_t *d)
{
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    memset(sense, 0, sizeof(sense));

    msd_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = MSD_CBW_SIGNATURE;
    cbw.dCBWTag = ++d->tag;
    cbw.dCBWDataTransferLength = sizeof(sense);
    cbw.bmCBWFlags = MSD_CBW_FLAG_DATA_IN;
    cbw.bCBWLUN = MSD_LUN;
    cbw.bCBWCBLength = sizeof(cdb);
    memcpy(cbw.CBWCB, cdb, sizeof(cdb));

    if (!d->hc->bulk_transfer(d->slot_id, d->bulk_out_addr, &cbw, sizeof(cbw), NULL)) {
        return 0xFFU; /* transport failure -- caller treats as "unknown, don't retry" */
    }
    d->hc->bulk_transfer(d->slot_id, d->bulk_in_addr, sense, sizeof(sense), NULL);
    msd_csw_t csw;
    d->hc->bulk_transfer(d->slot_id, d->bulk_in_addr, &csw, sizeof(csw), NULL);

    uint8_t sense_key = sense[2] & 0x0FU;
    printf("usb_msd: slot %u: REQUEST SENSE key=%x asc=%02x ascq=%02x\n", d->slot_id, sense_key,
           sense[12], sense[13]);
    return sense_key;
}

/* One full CBW -> [data stage] -> CSW transaction, bounded-retried on any
 * transport-level failure or a transient CHECK CONDITION (UNIT ATTENTION /
 * NOT READY -- see msd_request_sense_raw()'s comment; a genuine SCSI
 * command failure otherwise is returned as false without further retrying,
 * that's a real answer from the device, not a desync). Runs under d->lock
 * (see msd_transact() below) so the whole thing, retries included, is
 * atomic with respect to any other task's commands to the same device. */
static bool msd_transact_locked(msd_device_t *d, const uint8_t *cdb, uint8_t cdb_len, void *data,
                                 uint32_t data_len, bool data_in)
{
    for (int attempt = 0; attempt < MSD_MAX_RETRIES; ++attempt) {
        msd_cbw_t cbw;
        memset(&cbw, 0, sizeof(cbw));
        cbw.dCBWSignature = MSD_CBW_SIGNATURE;
        cbw.dCBWTag = ++d->tag;
        cbw.dCBWDataTransferLength = data_len;
        cbw.bmCBWFlags = data_in ? MSD_CBW_FLAG_DATA_IN : 0U;
        cbw.bCBWLUN = MSD_LUN;
        cbw.bCBWCBLength = cdb_len;
        memcpy(cbw.CBWCB, cdb, cdb_len);

        if (!d->hc->bulk_transfer(d->slot_id, d->bulk_out_addr, &cbw, sizeof(cbw), NULL)) {
            msd_reset_recovery(d);
            continue;
        }

        if (data_len > 0) {
            bool ok = data_in ? d->hc->bulk_transfer(d->slot_id, d->bulk_in_addr, data,
                                                      (uint16_t)data_len, NULL)
                               : d->hc->bulk_transfer(d->slot_id, d->bulk_out_addr, data,
                                                       (uint16_t)data_len, NULL);
            if (!ok) {
                msd_clear_halt(d, data_in ? d->bulk_in_addr : d->bulk_out_addr);
                /* Fall through to read the CSW anyway -- BOT spec: after
                 * clearing a STALLed data-stage endpoint, the device still
                 * has a CSW queued up on the IN pipe. */
            }
        }

        msd_csw_t csw;
        if (!d->hc->bulk_transfer(d->slot_id, d->bulk_in_addr, &csw, sizeof(csw), NULL)) {
            msd_reset_recovery(d);
            continue;
        }
        if (csw.dCSWSignature != MSD_CSW_SIGNATURE || csw.dCSWTag != cbw.dCBWTag) {
            printf("usb_msd: slot %u: transport desync (bad CSW signature/tag) -- resetting\n",
                   d->slot_id);
            msd_reset_recovery(d);
            continue;
        }
        if (csw.bCSWStatus == MSD_CSW_STATUS_PASSED) {
            return true;
        }
        if (csw.bCSWStatus == MSD_CSW_STATUS_FAILED) {
            uint8_t sense_key = msd_request_sense_raw(d);
            if (sense_key == MSD_SENSE_KEY_UNIT_ATTENTION || sense_key == MSD_SENSE_KEY_NOT_READY) {
                /* Transient -- REQUEST SENSE above already cleared the
                 * condition (UNIT ATTENTION) or the device just needs a
                 * moment longer (NOT READY); retry the same command. */
                continue;
            }
            return false; /* a real SCSI-level answer, not a desync -- don't retry */
        }
        printf("usb_msd: slot %u: transport desync (csw_status=%u) -- resetting\n", d->slot_id,
               csw.bCSWStatus);
        msd_reset_recovery(d);
    }
    printf("usb_msd: slot %u: command failed after %d attempts, giving up\n", d->slot_id,
           MSD_MAX_RETRIES);
    return false;
}

static bool msd_transact(msd_device_t *d, const uint8_t *cdb, uint8_t cdb_len, void *data,
                          uint32_t data_len, bool data_in)
{
    msd_lock_acquire(&d->lock);
    bool ok = msd_transact_locked(d, cdb, cdb_len, data, data_len, data_in);
    msd_lock_release(&d->lock);
    return ok;
}

static bool msd_test_unit_ready(msd_device_t *d)
{
    uint8_t cdb[6] = { 0x00, 0, 0, 0, 0, 0 }; /* TEST UNIT READY */
    return msd_transact(d, cdb, sizeof(cdb), NULL, 0, false);
}

static bool msd_read_capacity10(msd_device_t *d, uint32_t *out_last_lba, uint32_t *out_block_size)
{
    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x25; /* READ CAPACITY (10) */
    uint8_t buf[8];
    if (!msd_transact(d, cdb, sizeof(cdb), buf, sizeof(buf), true)) {
        return false;
    }
    *out_last_lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8)
        | buf[3];
    *out_block_size = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8)
        | buf[7];
    return true;
}

static bool msd_read10(msd_device_t *d, uint32_t lba, uint8_t *buf)
{
    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x28; /* READ (10) */
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[8] = 1; /* transfer length: 1 block */
    return msd_transact(d, cdb, sizeof(cdb), buf, MSD_SECTOR_SIZE, true);
}

static bool msd_write10(msd_device_t *d, uint32_t lba, const uint8_t *buf)
{
    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x2A; /* WRITE (10) */
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[8] = 1;
    return msd_transact(d, cdb, sizeof(cdb), (void *)(uintptr_t)buf, MSD_SECTOR_SIZE, false);
}

static int msd_disk_read(int index, uint32_t lba, uint8_t *buffer)
{
    msd_device_t *d = &devices[index];
    if (!d->present) {
        return -1;
    }
    return msd_read10(d, lba, buffer) ? 0 : -1;
}

static int msd_disk_write(int index, uint32_t lba, const uint8_t *buffer)
{
    msd_device_t *d = &devices[index];
    if (!d->present) {
        return -1;
    }
    return msd_write10(d, lba, buffer) ? 0 : -1;
}

disk_device_t *usb_msd_disk(int index)
{
    if (index < 0 || index >= USB_MSD_MAX_DEVICES || !devices[index].present) {
        return NULL;
    }
    return &devices[index].disk;
}

bool usb_msd_try_attach(const usb_hc_ops_t *hc, const usb_device_t *dev)
{
    if (!dev->has_bulk_endpoints) {
        return false;
    }

    int slot = -1;
    for (int i = 0; i < USB_MSD_MAX_DEVICES; ++i) {
        if (!devices[i].present) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        printf("usb_msd: slot %u: too many Mass Storage devices already attached (max %u)\n",
               dev->slot_id, USB_MSD_MAX_DEVICES);
        return false;
    }

    msd_device_t *d = &devices[slot];
    memset(d, 0, sizeof(*d));
    d->hc = hc;
    d->slot_id = dev->slot_id;
    d->interface_number = dev->msd_interface_number;
    d->bulk_in_addr = dev->bulk_in_addr;
    d->bulk_out_addr = dev->bulk_out_addr;

    /* Bounded retry: some drives report Not Ready transiently right after
     * reset/power-up (e.g. still spinning up media, or a brief post-
     * enumeration settle time on real flash controllers). */
    bool ready = false;
    for (int i = 0; i < 5 && !ready; ++i) {
        ready = msd_test_unit_ready(d);
        if (!ready) {
            pit_sleep(200);
        }
    }
    if (!ready) {
        printf("usb_msd: slot %u: device not ready after retries, giving up\n", dev->slot_id);
        return false;
    }

    uint32_t last_lba = 0, block_size = 0;
    if (!msd_read_capacity10(d, &last_lba, &block_size)) {
        printf("usb_msd: slot %u: READ CAPACITY (10) failed\n", dev->slot_id);
        return false;
    }
    if (block_size != MSD_SECTOR_SIZE) {
        printf("usb_msd: slot %u: unsupported block size %u (only %u supported)\n", dev->slot_id,
               block_size, MSD_SECTOR_SIZE);
        return false;
    }

    d->disk.name = msd_names[slot];
    d->disk.sector_size = MSD_SECTOR_SIZE;
    d->disk.present = true;
    d->disk.read = msd_reads[slot];
    d->disk.write = msd_writes[slot];
    d->present = true;

    uint64_t total_mb = ((uint64_t)(last_lba + 1) * MSD_SECTOR_SIZE) / (1024ULL * 1024ULL);
    printf("usb_msd: slot %u: attached as %s (%u sectors, ~%u MiB)\n", dev->slot_id,
           d->disk.name, (unsigned)(last_lba + 1), (unsigned)total_mb);
    return true;
}
