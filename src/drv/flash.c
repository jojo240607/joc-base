#include "flash.h"
#include "hal/stm32/flash_hal.h"
#if defined(STM32F407xx)
#include "stm32f4xx.h"          /* FLASH peripheral base — driver layer only */
#elif defined(STM32F103xx)
#include "stm32f103xx.h"
#else
#error "flash.c: unsupported MCU target"
#endif
#include <stdlib.h>
#include <string.h>

/*
 * Internal FLASH driver — BLOCK device. Wraps flash_hal (the only place that
 * touches FLASH registers) and presents the sector as a sequence of 32-bit
 * words through the unified block_device interface. No register access here.
 */

static int  flash_dev_open(device *self);
static int  flash_dev_close(device *self);
static int  flash_dev_read(device *self, void *buf, size_t len);
static int  flash_dev_write(device *self, const void *buf, size_t len);
static int  flash_dev_ioctl(device *self, int cmd, void *arg);
static int  flash_blk_read(block_device *self, uint64_t lba, void *buf, uint32_t count);
static int  flash_blk_write(block_device *self, uint64_t lba, const void *buf, uint32_t count);
static int  flash_blk_erase(block_device *self, uint64_t lba, uint32_t count);
static int  flash_blk_info(block_device *self, block_device_info_t *info);

static const struct block_deviceVtable flash_block_vtable = {
    .read = flash_blk_read, .write = flash_blk_write,
    .erase = flash_blk_erase, .get_info = flash_blk_info,
};
static const struct deviceVtable flash_dev_vtable = {
    .open = flash_dev_open, .close = flash_dev_close,
    .read = flash_dev_read, .write = flash_dev_write, .ioctl = flash_dev_ioctl,
};

device *flash_create(const void *config)
{
    const flash_config_t *c = (const flash_config_t *)config;
    if (!c) return NULL;
    flash *p = (flash *)malloc(sizeof(flash));
    if (!p) return NULL;
    memset(p, 0, sizeof(flash));
    p->parent.parent.vtable = &flash_dev_vtable;
    p->parent.vtable = &flash_block_vtable;
    p->parent.parent.type  = DEVICE_TYPE_FLASH;
    p->parent.parent.class = DEVICE_CLASS_BLOCK;
    p->parent.parent.name  = c->name;
    p->sector = c->sector;
    p->hal = flash_hal_create((void *)FLASH);
    if (!p->hal) { free(p); return NULL; }
    p->base = flash_hal_sector_base(p->sector);
    p->sector_size = flash_hal_sector_size(p->sector);
    return &p->parent.parent;
}

void flash_destroy(flash *self)
{
    if (!self) return;
    flash_hal_destroy(self->hal);
    free(self);
}

static int flash_dev_open(device *self)  { (void)self; return 0; }
static int flash_dev_close(device *self) { (void)self; return 0; }
/* Block transfers go through the block_device vtable; the base read/write are
 * not meaningful for a word-addressed flash, so they are rejected. */
static int flash_dev_read(device *s, void *b, size_t l)  { (void)s;(void)b;(void)l; return -1; }
static int flash_dev_write(device *s, const void *b, size_t l) { (void)s;(void)b;(void)l; return -1; }

static int flash_dev_ioctl(device *self, int cmd, void *arg)
{
    flash *p = (flash *)self;
    switch (cmd) {
    case FLASH_IOCTL_GET_SECTOR: if (arg) *(uint32_t *)arg = p->sector;     return 0;
    case FLASH_IOCTL_GET_BASE:   if (arg) *(uint32_t *)arg = p->base;       return 0;
    case FLASH_IOCTL_GET_STATUS: if (arg) *(uint32_t *)arg = flash_hal_get_status(p->hal); return 0;
    default: return -1;
    }
}

/* lba is a word index (block_size = 4); bounds-checked against the sector. */
static uint32_t flash_word_addr(flash *p, uint64_t lba, uint32_t count)
{
    uint32_t off = (uint32_t)(lba * 4U);
    if (!p->sector_size || count == 0) return 0U;
    if (off + count * 4U > p->sector_size) return 0U;   /* out of bounds */
    return p->base + off;
}

static int flash_blk_read(block_device *self, uint64_t lba, void *buf, uint32_t count)
{
    flash *p = (flash *)self;
    if (!buf || count == 0) return -1;
    uint32_t addr = flash_word_addr(p, lba, count);
    if (addr == 0U) return -1;
    for (uint32_t i = 0; i < count; i++)
        ((uint32_t *)buf)[i] = flash_hal_read_u32(p->hal, addr + i * 4U);
    return 0;
}

static int flash_blk_write(block_device *self, uint64_t lba, const void *buf, uint32_t count)
{
    flash *p = (flash *)self;
    if (!buf || count == 0) return -1;
    uint32_t addr = flash_word_addr(p, lba, count);
    if (addr == 0U) return -1;
    for (uint32_t i = 0; i < count; i++)
        flash_hal_program_u32(p->hal, addr + i * 4U, ((const uint32_t *)buf)[i]);
    return 0;
}

static int flash_blk_erase(block_device *self, uint64_t lba, uint32_t count)
{
    (void)lba; (void)count;
    flash *p = (flash *)self;
    if (!p->sector_size) return -1;
    /* Internal flash erase is always sector-granular: erase the managed sector. */
    flash_hal_erase_sector(p->hal, p->sector);
    return 0;
}

static int flash_blk_info(block_device *self, block_device_info_t *info)
{
    flash *p = (flash *)self;
    if (!info || !p->sector_size) return -1;
    info->block_size  = 4;                       /* one 32-bit word per block */
    info->block_count = p->sector_size / 4U;
    info->total_bytes = (uint64_t)p->sector_size;
    return 0;
}
