//! SPI NOR Flash（W25Q128 类）驱动 —— 保存/存储用非易失外设。
//!
//! 依赖 SPI 主 + 一路 GPIO 片选（与 BMI088 同构，但帧协议为命令流而非寄存器流）。
//! 页编程/擦除前必须发 WREN（模拟器 WEL 锁存；CS 上升沿提交并清 WEL）。
//! open 时做 JEDEC ID 校验（0xEF 40 18），失败即拒绝挂载。

#include "drv/spi_flash.h"

#include "drv/spi.h"
#include "devmgr/device_manager.h"
#include "log/log.h"
#include "log/app_log.h"
#include <stdlib.h>
#include <string.h>

/* ---------- 命令码（W25Q128 常用子集） ---------- */
#define FLASH_CMD_WREN          0x06
#define FLASH_CMD_READ          0x03
#define FLASH_CMD_PAGE_PROGRAM  0x02
#define FLASH_CMD_SECTOR_ERASE  0x20
#define FLASH_CMD_JEDEC_ID      0x9F
#define FLASH_PAGE_SIZE         256u

struct _spi_flash {
    device parent;      /* unified interface — MUST be first member */
    device *spi;
    device *cs;
};

static int spi_xfer_bytes(spi_flash *self, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    spi_xfer_t x = { .tx_buf = tx, .rx_buf = rx, .len = len };
    return self->spi->vtable->ioctl(self->spi, SPI_IOCTL_XFER, &x);
}

/* 片选（1=拉高不选中，0=拉低选中） */
static void cs_set(spi_flash *self, int level)
{
    uint8_t v = level ? 1u : 0u;
    self->cs->vtable->write(self->cs, &v, 1);
}

/* 写使能（WREN） */
static void write_enable(spi_flash *self)
{
    uint8_t tx = FLASH_CMD_WREN;
    cs_set(self, 1);
    cs_set(self, 0);
    spi_xfer_bytes(self, &tx, NULL, 1);
    cs_set(self, 1);
}

/* JEDEC ID：CS 低 → 0x9F → 3 dummy → 3B ID → CS 高 */
static int read_jedec(spi_flash *self, uint8_t out[3])
{
    uint8_t tx[4] = { FLASH_CMD_JEDEC_ID, 0xFF, 0xFF, 0xFF };
    uint8_t rx[4] = { 0, 0, 0, 0 };
    cs_set(self, 1);
    cs_set(self, 0);
    if (spi_xfer_bytes(self, tx, rx, 4) != 0) {
        cs_set(self, 1);
        return -1;
    }
    cs_set(self, 1);
    out[0] = rx[1];
    out[1] = rx[2];
    out[2] = rx[3];
    return 0;
}

/* 读：CS 低 → 0x03 + 3B 地址 → len 字节（每字节 dummy 写收 1B） → CS 高 */
static int flash_read(spi_flash *self, uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint8_t hdr[4] = { FLASH_CMD_READ, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    uint8_t hdr_rx[4];
    cs_set(self, 1);
    cs_set(self, 0);
    if (spi_xfer_bytes(self, hdr, hdr_rx, 4) != 0) {
        cs_set(self, 1);
        return -1;
    }
    /* 数据段：tx 全 dummy */
    uint8_t dummy = 0xFF;
    for (uint16_t i = 0; i < len; i++) {
        if (spi_xfer_bytes(self, &dummy, &buf[i], 1) != 0) {
            cs_set(self, 1);
            return -1;
        }
    }
    cs_set(self, 1);
    return 0;
}

/* 页编程：WREN → CS 低 → 0x02 + 3B 地址 → len 字节 → CS 高 */
static int flash_program(spi_flash *self, uint32_t addr, const uint8_t *buf, uint16_t len)
{
    if (len == 0 || len > FLASH_PAGE_SIZE) return -1;
    write_enable(self);
    uint8_t hdr[4] = { FLASH_CMD_PAGE_PROGRAM, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    cs_set(self, 1);
    cs_set(self, 0);
    if (spi_xfer_bytes(self, hdr, NULL, 4) != 0) {
        cs_set(self, 1);
        return -1;
    }
    if (spi_xfer_bytes(self, buf, NULL, len) != 0) {
        cs_set(self, 1);
        return -1;
    }
    cs_set(self, 1);
    return 0;
}

/* 扇区擦除：WREN → CS 低 → 0x20 + 3B 地址 → CS 高 */
static int flash_erase_sector(spi_flash *self, uint32_t addr)
{
    write_enable(self);
    uint8_t tx[4] = { FLASH_CMD_SECTOR_ERASE, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    cs_set(self, 1);
    cs_set(self, 0);
    int r = spi_xfer_bytes(self, tx, NULL, 4);
    cs_set(self, 1);
    return r;
}

/* ---------- device 接口 ---------- */

static int spi_flash_dev_open(device *self)
{
    spi_flash *f = (spi_flash *)self;
    if (!f->spi || !f->cs) return -1;
    return 0; /* 依赖在 create 时已拉起并校验 */
}

static int spi_flash_dev_close(device *self)
{
    (void)self;
    return 0;
}

static int spi_flash_dev_read(device *self, void *buf, size_t len)
{
    /* device read = 从地址 0 顺序读（简化接口，实际按地址访问走 ioctl） */
    return flash_read((spi_flash *)self, 0, (uint8_t *)buf, (uint16_t)len);
}

static int spi_flash_dev_write(device *self, const void *buf, size_t len)
{
    return flash_program((spi_flash *)self, 0, (const uint8_t *)buf, (uint16_t)len);
}

static int spi_flash_dev_ioctl(device *self, int cmd, void *arg)
{
    spi_flash *f = (spi_flash *)self;
    if (!arg) return -1;
    switch (cmd) {
    case FLASH_IOCTL_GET_JEDEC: {
        uint32_t *id = (uint32_t *)arg;
        uint8_t b[3];
        if (read_jedec(f, b) != 0) return -1;
        *id = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
        return 0;
    }
    case FLASH_IOCTL_READ: {
        flash_io_t *io = (flash_io_t *)arg;
        if (!io->buf || io->len == 0) return -1;
        return flash_read(f, io->addr, io->buf, io->len);
    }
    case FLASH_IOCTL_WRITE: {
        flash_io_t *io = (flash_io_t *)arg;
        if (!io->buf || io->len == 0) return -1;
        return flash_program(f, io->addr, io->buf, io->len);
    }
    case FLASH_IOCTL_ERASE_SECTOR: {
        uint32_t *addr = (uint32_t *)arg;
        return flash_erase_sector(f, *addr);
    }
    default:
        return -1;
    }
}

static const struct deviceVtable spi_flash_dev_vtable = {
    .open  = spi_flash_dev_open,
    .close = spi_flash_dev_close,
    .read  = spi_flash_dev_read,
    .write = spi_flash_dev_write,
    .ioctl = spi_flash_dev_ioctl,
};

/* ---------- 工厂 ---------- */

device *spi_flash_create(const void *config)
{
    const spi_flash_config_t *c = (const spi_flash_config_t *)config;
    spi_flash *self = (spi_flash *)malloc(sizeof(spi_flash));
    if (!self) return NULL;
    memset(self, 0, sizeof(spi_flash));
    self->parent.vtable = &spi_flash_dev_vtable;
    self->parent.type   = DEVICE_TYPE_FLASH;
    self->parent.class  = DEVICE_CLASS_BLOCK;
    self->parent.name   = c->name;

    self->spi = device_manager_get(c->spi);
    self->cs  = device_manager_get(c->cs);
    if (!self->spi || !self->cs) {
        log_printf(app_log(), LOG_ERROR, "spi_flash", "[spi_flash] %s: 依赖缺失 spi=%p cs=%p\n",
                   c->name, (void *)self->spi, (void *)self->cs);
        free(self);
        return NULL;
    }

    /* 拉起依赖 + JEDEC 校验 */
    if (self->spi->vtable->open(self->spi) != 0 || self->cs->vtable->open(self->cs) != 0) {
        log_printf(app_log(), LOG_ERROR, "spi_flash", "[spi_flash] %s: 依赖 open 失败\n", c->name);
        free(self);
        return NULL;
    }

    uint32_t id = 0;
    if (spi_flash_dev_ioctl((device *)self, FLASH_IOCTL_GET_JEDEC, &id) != 0 || id != 0xEF4018u) {
        log_printf(app_log(), LOG_ERROR, "spi_flash", "[spi_flash] %s: JEDEC 校验失败 id=0x%06X\n", c->name, id);
        free(self);
        return NULL;
    }

    return (device *)self;
}
