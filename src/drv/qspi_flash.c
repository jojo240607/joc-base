#include "qspi_flash.h"
#include "hal/stm32h7/qspi_hal.h"
#include <stdlib.h>
#include <string.h>

/*
 * External QUADSPI FLASH driver — BLOCK device (256-byte pages).
 *
 * One "block" is one flash page (block_size = 256). Write splits arbitrary
 * lengths into page-program calls (page-aligned start + page-boundary-safe
 * chunks); erase is sector-granular (4 KB), matching the hardware.
 *
 * Only built for STM32H750xx (see CMakeLists.txt); on Renode the HAL
 * indirect API returns -1, so every operation fails there — real-silicon
 * path only.
 */

static int  qspi_dev_open(device *self);
static int  qspi_dev_close(device *self);
static int  qspi_dev_read(device *self, void *buf, size_t len);
static int  qspi_dev_write(device *self, const void *buf, size_t len);
static int  qspi_dev_ioctl(device *self, int cmd, void *arg);
static int  qspi_blk_read(block_device *self, uint64_t lba, void *buf, uint32_t count);
static int  qspi_blk_write(block_device *self, uint64_t lba, const void *buf, uint32_t count);
static int  qspi_blk_erase(block_device *self, uint64_t lba, uint32_t count);
static int  qspi_blk_info(block_device *self, block_device_info_t *info);

static const struct block_deviceVtable qspi_block_vtable = {
    .read = qspi_blk_read, .write = qspi_blk_write,
    .erase = qspi_blk_erase, .get_info = qspi_blk_info,
};
static const struct deviceVtable qspi_dev_vtable = {
    .open = qspi_dev_open, .close = qspi_dev_close,
    .read = qspi_dev_read, .write = qspi_dev_write, .ioctl = qspi_dev_ioctl,
};

device *qspi_flash_create(const void *config)
{
    const qspi_flash_config_t *c = (const qspi_flash_config_t *)config;
    if (!c) return NULL;
    qspi_flash *p = (qspi_flash *)malloc(sizeof(qspi_flash));
    if (!p) return NULL;
    memset(p, 0, sizeof(qspi_flash));
    p->parent.parent.vtable = &qspi_dev_vtable;
    p->parent.vtable = &qspi_block_vtable;
    p->parent.parent.type  = DEVICE_TYPE_FLASH;
    p->parent.parent.class = DEVICE_CLASS_BLOCK;
    p->parent.parent.name  = c->name;
    return &p->parent.parent;
}

void qspi_flash_destroy(qspi_flash *self)
{
    if (!self) return;
    free(self);
}

static int qspi_dev_open(device *self)
{
    (void)self;
    /* Make sure the controller is up (idempotent); XIP window also usable. */
    return qspi_hal_init_mm();
}
static int qspi_dev_close(device *self) { (void)self; return 0; }

/* Base read/write are not meaningful for a page-addressed flash; block
 * transfers go through the block_device vtable. */
static int qspi_dev_read(device *s, void *b, size_t l)  { (void)s;(void)b;(void)l; return -1; }
static int qspi_dev_write(device *s, const void *b, size_t l) { (void)s;(void)b;(void)l; return -1; }

static int qspi_dev_ioctl(device *self, int cmd, void *arg)
{
    (void)self;
    switch (cmd) {
    case QSPI_FLASH_IOCTL_GET_ID:
        if (arg) return qspi_hal_read_id((uint8_t *)arg);
        return -1;
    default:
        return -1;
    }
}

/* lba is a page index (block_size = 256); bounds-check against flash size. */
static uint32_t qspi_page_addr(qspi_flash *p, uint64_t lba, uint32_t count)
{
    (void)p;
    uint64_t off = lba * QSPI_PAGE_SIZE;
    if (count == 0) return 0xFFFFFFFFu;
    if (off + (uint64_t)count * QSPI_PAGE_SIZE > QSPI_FLASH_SIZE)
        return 0xFFFFFFFFu;   /* out of bounds */
    return (uint32_t)off;
}

static int qspi_blk_read(block_device *self, uint64_t lba, void *buf, uint32_t count)
{
    qspi_flash *p = (qspi_flash *)self;
    if (!buf || count == 0) return -1;
    uint32_t addr = qspi_page_addr(p, lba, count);
    if (addr == 0xFFFFFFFFu) return -1;
    return qspi_hal_read(addr, (uint8_t *)buf, count * QSPI_PAGE_SIZE);
}

static int qspi_blk_write(block_device *self, uint64_t lba, const void *buf, uint32_t count)
{
    qspi_flash *p = (qspi_flash *)self;
    if (!buf || count == 0) return -1;
    uint32_t addr = qspi_page_addr(p, lba, count);
    if (addr == 0xFFFFFFFFu) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t remaining = count * QSPI_PAGE_SIZE;
    while (remaining) {
        /* chunk: up to the end of the current 256-byte page */
        uint32_t page_off = addr & (QSPI_PAGE_SIZE - 1u);
        uint32_t chunk = QSPI_PAGE_SIZE - page_off;
        if (chunk > remaining) chunk = remaining;
        if (qspi_hal_program(addr, src, chunk)) return -1;
        addr += chunk; src += chunk; remaining -= chunk;
    }
    return 0;
}

static int qspi_blk_erase(block_device *self, uint64_t lba, uint32_t count)
{
    qspi_flash *p = (qspi_flash *)self;
    (void)count;
    /* Erase is sector-granular (4 KB = 16 pages). Erase the sector
     * containing lba regardless of count (same convention as flash.c). */
    uint64_t sector_off = lba * QSPI_PAGE_SIZE;
    if (sector_off >= QSPI_FLASH_SIZE) return -1;
    uint32_t saddr = ((uint32_t)sector_off) & ~(QSPI_SECTOR_SIZE - 1u);
    return qspi_hal_erase_sector(saddr);
}

static int qspi_blk_info(block_device *self, block_device_info_t *info)
{
    (void)self;
    if (!info) return -1;
    info->block_size  = QSPI_PAGE_SIZE;
    info->block_count = QSPI_FLASH_SIZE / QSPI_PAGE_SIZE;
    info->total_bytes = QSPI_FLASH_SIZE;
    return 0;
}
