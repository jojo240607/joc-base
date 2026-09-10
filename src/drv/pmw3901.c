/*
 * PMW3901 光流传感器驱动（SPI，PixArt 光学流）。
 *
 * 读寄存器协议：CS 拉低 → 发 (reg<<1)|1 → 后续每字节发 0xFF（dummy）读下一
 * 寄存器（地址递增）→ CS 拉高。寄存器 8 位，0x02 Motion bit7=数据就绪。
 * Delta_X/Y 为 16 位有符号 8.8 定点（1.0px = 0x0100）。
 */
#include "pmw3901.h"
#include "drv/spi.h"
#include "devmgr/device_manager.h"
#include "log/app_log.h"
#include <stdlib.h>
#include <string.h>

typedef struct _pmw3901 {
    device parent;
    pmw3901_config_t cfg;
    device *spi;
    device *cs;
} pmw3901;

static int spi_xfer(pmw3901 *self, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    spi_xfer_t x = { .tx_buf = tx, .rx_buf = rx, .len = len };
    return self->spi->vtable->ioctl(self->spi, SPI_IOCTL_XFER, &x);
}

/* 片选（1=拉高不选中，0=拉低选中） */
static void cs_set(pmw3901 *self, int level)
{
    uint8_t v = level ? 1u : 0u;
    self->cs->vtable->write(self->cs, &v, 1);
}

/* 读连续寄存器：reg 起 n 个，结果存 out[0..n)（首字节 out[0]=reg 值） */
static int read_regs(pmw3901 *self, uint8_t reg, uint8_t *out, uint16_t n)
{
    uint8_t tx[8];
    uint8_t rx[8];
    uint16_t len = 1 + n;
    if (len > sizeof(tx)) return -1;
    memset(tx, 0xFF, sizeof(tx));
    tx[0] = (uint8_t)((reg << 1) | 1); /* 读命令首字节 */
    /* 先确保 CS 高再拉低（同 bmi088）：即使 CS 复位后初始低电平（ODR=0），
     * 拉高→拉低两跳也保证 GpioLevel 事件产生，从机可靠进入选中帧状态 */
    cs_set(self, 1);
    cs_set(self, 0);
    int rc = spi_xfer(self, tx, rx, len);
    cs_set(self, 1);
    if (rc != 0) return rc;
    for (uint16_t i = 0; i < n; i++) out[i] = rx[1 + i];
    return 0;
}

static int pmw3901_dev_open(device *self)
{
    pmw3901 *p = (pmw3901 *)self;
    if (p->spi->vtable->open(p->spi) != 0 ||
        p->cs->vtable->open(p->cs) != 0) {
        log_printf(app_log(), LOG_ERROR, "pmw3901", "[pmw3901] %s: 依赖 open 失败\n", p->cfg.name);
        return -1;
    }
    uint8_t id = 0;
    if (read_regs(p, 0x00, &id, 1) != 0 || id != PMW3901_PRODUCT_ID) {
        log_printf(app_log(), LOG_ERROR, "pmw3901", "[pmw3901] %s: Product_ID 校验失败 id=0x%02X\n", p->cfg.name, id);
        return -1;
    }
    return 0;
}

static int pmw3901_dev_close(device *self)
{
    pmw3901 *p = (pmw3901 *)self;
    if (p->spi) p->spi->vtable->close(p->spi);
    if (p->cs)  p->cs->vtable->close(p->cs);
    return 0;
}

static int pmw3901_dev_ioctl(device *self, int cmd, void *arg)
{
    pmw3901 *p = (pmw3901 *)self;
    switch (cmd) {
    case PMW3901_IOCTL_GET_PRODUCT_ID: {
        if (!arg) return -1;
        return read_regs(p, 0x00, (uint8_t *)arg, 1);
    }
    case PMW3901_IOCTL_GET_MOTION: {
        if (!arg) return -1;
        pmw3901_motion_t *m = (pmw3901_motion_t *)arg;
        uint8_t r[6]; /* 0x02 Motion / 0x03-04 DX_LH / 0x05-06 DY_LH */
        if (read_regs(p, 0x02, r, 5) != 0) return -1;
        m->motion = r[0];
        m->dx = (int16_t)(((uint16_t)r[2] << 8) | r[1]);
        m->dy = (int16_t)(((uint16_t)r[4] << 8) | r[3]);
        if (read_regs(p, 0x07, &r[0], 1) != 0) return -1;
        m->squal = r[0];
        return 0;
    }
    default:
        return -1;
    }
}

static const struct deviceVtable pmw3901_dev_vtable = {
    .open   = pmw3901_dev_open,
    .close  = pmw3901_dev_close,
    .read   = NULL,
    .write  = NULL,
    .ioctl  = pmw3901_dev_ioctl,
    .irq_id = NULL,
};

device *pmw3901_create(const void *config)
{
    const pmw3901_config_t *c = (const pmw3901_config_t *)config;
    pmw3901 *self = (pmw3901 *)malloc(sizeof(pmw3901));
    if (!self) return NULL;
    memset(self, 0, sizeof(pmw3901));
    self->parent.vtable = &pmw3901_dev_vtable;
    self->parent.type   = DEVICE_TYPE_GPIO; /* 复用既有类型（光流观测，控制类） */
    self->parent.class  = DEVICE_CLASS_CONTROL;
    self->parent.name   = c->name;
    self->cfg = *c;

    /* 依赖设备在 create 时解析（板级设备表顺序保证 spi/gpio 先注册） */
    self->spi = device_manager_get(c->spi);
    self->cs  = device_manager_get(c->cs);
    if (!self->spi || !self->cs) {
        log_printf(app_log(), LOG_ERROR, "pmw3901", "[pmw3901] %s: 依赖缺失 spi=%p cs=%p\n",
                   c->name, (void *)self->spi, (void *)self->cs);
        free(self);
        return NULL;
    }
    return (device *)self;
}
