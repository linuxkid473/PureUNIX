#include <pureunix/arch.h>
#include <pureunix/disk.h>
#include <pureunix/io.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6

#define ATA_REG_DATA     0
#define ATA_REG_ERROR    1
#define ATA_REG_SECCOUNT0 2
#define ATA_REG_LBA0     3
#define ATA_REG_LBA1     4
#define ATA_REG_LBA2     5
#define ATA_REG_HDDEVSEL 6
#define ATA_REG_COMMAND  7
#define ATA_REG_STATUS   7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_CACHE_FLUSH 0xE7

/* HDDEVSEL bit 4: 0 = master, 1 = slave */
#define ATA_DEV_MASTER 0x00
#define ATA_DEV_SLAVE  0x10

static int ata_slave_read(uint32_t lba, uint8_t *buffer);
static int ata_slave_write(uint32_t lba, const uint8_t *buffer);

static disk_device_t primary = {
    .name = "ata0",  .sector_size = 512, .present = false,
    .read = ata_read_sector, .write = ata_write_sector,
};
static disk_device_t slave_dev = {
    .name = "ata1",  .sector_size = 512, .present = false,
    .read = ata_slave_read,  .write = ata_slave_write,
};

static void ata_irq(interrupt_regs_t *regs)
{
    (void)inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
}

static void ata_delay(void)
{
    for (int i = 0; i < 4; ++i)
        (void)inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
}

static int ata_bsy_wait(void)
{
    for (uint32_t i = 0; i < 100000; ++i) {
        uint8_t st = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY)) return 0;
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return -1;
    }
    return -1;
}

static int ata_wait_ready(void)
{
    for (uint32_t i = 0; i < 100000; ++i) {
        uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return -1;
    }
    return -1;
}

/* Probe one ATA device.  dev_sel = ATA_DEV_MASTER or ATA_DEV_SLAVE. */
static bool ata_probe(uint8_t dev_sel)
{
    outb(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL, 0xA0 | dev_sel);
    ata_delay();
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay();
    if (inb(ATA_PRIMARY_IO + ATA_REG_STATUS) == 0) return false;
    if (ata_wait_ready() != 0) return false;
    for (int i = 0; i < 256; ++i)
        (void)inw(ATA_PRIMARY_IO + ATA_REG_DATA);
    return true;
}

void ata_init(void)
{
    interrupt_register_handler(46, ata_irq);
    irq_enable(14);
    outb(ATA_PRIMARY_CTRL, 0);

    primary.present   = ata_probe(ATA_DEV_MASTER);
    slave_dev.present = ata_probe(ATA_DEV_SLAVE);

    if (primary.present)   printf("ATA: master present\n");
    if (slave_dev.present) printf("ATA: slave present\n");
}

disk_device_t *ata_primary_master(void) { return &primary; }
disk_device_t *ata_primary_slave(void)  { return &slave_dev; }

/* Generic LBA28 setup — dev 0=master, 1=slave. */
static int ata_select_lba28(int dev, uint32_t lba, uint8_t count)
{
    if (ata_bsy_wait() != 0) return -1;
    /* E0 = LBA | master; F0 = LBA | slave (bit 4 distinguishes) */
    outb(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL,
         (dev ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    ata_delay();
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, count);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0,  lba        & 0xFF);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, (lba >>  8) & 0xFF);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, (lba >> 16) & 0xFF);
    return 0;
}

/* ---- master r/w (existing public interface, unchanged) -------------- */

int ata_read_sector(uint32_t lba, uint8_t *buffer)
{
    if (!primary.present || ata_select_lba28(0, lba, 1) != 0) return -1;
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    if (ata_wait_ready() != 0) return -1;
    uint16_t *w = (uint16_t *)buffer;
    for (int i = 0; i < 256; ++i) w[i] = inw(ATA_PRIMARY_IO + ATA_REG_DATA);
    ata_delay();
    return 0;
}

int ata_write_sector(uint32_t lba, const uint8_t *buffer)
{
    if (!primary.present || ata_select_lba28(0, lba, 1) != 0) return -1;
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    if (ata_wait_ready() != 0) return -1;
    const uint16_t *w = (const uint16_t *)buffer;
    for (int i = 0; i < 256; ++i) outw(ATA_PRIMARY_IO + ATA_REG_DATA, w[i]);
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (ata_bsy_wait() != 0) return -1;
    ata_delay();
    return 0;
}

/* ---- slave r/w (Stage 4: EXT2 is mounted on the slave disk and is now
 * writable, so the slave needs a real write path too — mirrors
 * ata_write_sector() above, just selecting the slave device). ----------- */

static int ata_slave_read(uint32_t lba, uint8_t *buffer)
{
    if (!slave_dev.present || ata_select_lba28(1, lba, 1) != 0) return -1;
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    if (ata_wait_ready() != 0) return -1;
    uint16_t *w = (uint16_t *)buffer;
    for (int i = 0; i < 256; ++i) w[i] = inw(ATA_PRIMARY_IO + ATA_REG_DATA);
    ata_delay();
    return 0;
}

static int ata_slave_write(uint32_t lba, const uint8_t *buffer)
{
    if (!slave_dev.present || ata_select_lba28(1, lba, 1) != 0) return -1;
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    if (ata_wait_ready() != 0) return -1;
    const uint16_t *w = (const uint16_t *)buffer;
    for (int i = 0; i < 256; ++i) outw(ATA_PRIMARY_IO + ATA_REG_DATA, w[i]);
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (ata_bsy_wait() != 0) return -1;
    ata_delay();
    return 0;
}
