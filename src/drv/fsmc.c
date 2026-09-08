#include "fsmc.h"
#include <stdlib.h>
#include <string.h>

/*
 * FSMC driver — STM32F407 flexible static memory controller.
 *
 * Register-free driver: all register/window work is delegated to the HAL
 * (fsmc_hal.c). open() gates the AHB3 clock only; BCR/BTR configuration and
 * the Bank1 chip-select enable are done through ioctl(), and the raw
 * read/write surface is the Bank1 window (0x60000000). The window returns
 * -2 while the bank is not enabled (hardware/bus semantics), so a case cannot
 * accidentally write through an unselected chip-select.
 */

static int fsmc_dev_open(device *self);
static int fsmc_dev_close(device *self);
static int fsmc_dev_read(device *self, void *buf, size_t len);
static int fsmc_dev_write(device *self, const void *buf, size_t len);
static int fsmc_dev_ioctl(device *self, int cmd, void *arg);

static const struct deviceVtable fsmc_dev_vtable = {
    .open  = fsmc_dev_open,
    .close = fsmc_dev_close,
    .read  = fsmc_dev_read,
    .write = fsmc_dev_write,
    .ioctl = fsmc_dev_ioctl,
    .irq_id = NULL,   /* FSMC has an IRQ line (48) but we poll nothing & enable nothing */
};

device *fsmc_create(const void *config)
{
    const fsmc_config_t *c = (const fsmc_config_t *)config;
    if (!c || !c->name || !c->periph) return NULL;

    fsmc *p = (fsmc *)calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->parent.vtable = &fsmc_dev_vtable;
    p->parent.type   = DEVICE_TYPE_FSMC;
    p->parent.class  = DEVICE_CLASS_CONTROL;   /* config surface + memory window */
    p->parent.name   = c->name;

    p->hal = fsmc_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }

    return &p->parent;
}

void fsmc_destroy(fsmc *self)
{
    if (!self) return;
    fsmc_hal_destroy(self->hal);
    free(self);
}

static int fsmc_dev_open(device *self)
{
    fsmc *p = (fsmc *)self;
    fsmc_hal_enable_clock(p->hal);
    return 0;
}

static int fsmc_dev_close(device *self)
{
    (void)self;
    return 0;
}

/* Bank1 窗口读：len 字节（对齐 4 的倍数由调用方保证；驱动按 32 位字搬运）。 */
static int fsmc_dev_read(device *self, void *buf, size_t len)
{
    fsmc *p = (fsmc *)self;
    if (!buf || (len & 3U) != 0) return -1;
    uint32_t *dst = (uint32_t *)buf;
    size_t n = len / 4;
    for (size_t i = 0; i < n; i++) {
        if (fsmc_hal_bank1_read32(p->hal, (uint32_t)(i * 4), &dst[i]) != 0)
            return -2;   /* bank not enabled */
    }
    return (int)len;
}

static int fsmc_dev_write(device *self, const void *buf, size_t len)
{
    fsmc *p = (fsmc *)self;
    if (!buf || (len & 3U) != 0) return -1;
    const uint32_t *src = (const uint32_t *)buf;
    size_t n = len / 4;
    for (size_t i = 0; i < n; i++) {
        if (fsmc_hal_bank1_write32(p->hal, (uint32_t)(i * 4), src[i]) != 0)
            return -2;   /* bank not enabled */
    }
    return (int)len;
}

static int fsmc_dev_ioctl(device *self, int cmd, void *arg)
{
    fsmc *p = (fsmc *)self;
    switch (cmd) {
    case FSMC_IOCTL_SET_BCR:
        if (!arg) return -1;
        fsmc_hal_set_bcr(p->hal, 1, *(const uint32_t *)arg);
        return 0;
    case FSMC_IOCTL_GET_BCR:
        if (!arg) return -1;
        *(uint32_t *)arg = fsmc_hal_get_bcr(p->hal, 1);
        return 0;
    case FSMC_IOCTL_SET_BTR:
        if (!arg) return -1;
        fsmc_hal_set_btr(p->hal, 1, *(const uint32_t *)arg);
        return 0;
    case FSMC_IOCTL_GET_BTR:
        if (!arg) return -1;
        *(uint32_t *)arg = fsmc_hal_get_btr(p->hal, 1);
        return 0;
    case FSMC_IOCTL_GET_BWTR:
        if (!arg) return -1;
        *(uint32_t *)arg = fsmc_hal_get_bwtr(p->hal, 1);
        return 0;
    case FSMC_IOCTL_BANK1_ENABLE:
        fsmc_hal_bank1_enable(p->hal);
        return 0;
    default:
        return -1;
    }
}
