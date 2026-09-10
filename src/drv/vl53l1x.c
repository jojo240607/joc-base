/*
 * VL53L1X ToF 激光测距传感器驱动（I2C 0x29，16 位寄存器地址）。
 *
 * 访问模式（固件侧用 MASTER_WRITE/READ 两个独立事务）：
 *   - 写 16 位寄存器：WRITE[addr=0x29, reg_hi, reg_lo, data...]（一次事务）
 *   - 读 16 位寄存器：WRITE[addr=0x29, reg_hi, reg_lo] 设指针 →
 *                    READ[addr=0x29, len] 连续读
 * 测距流程：写 RANGE_START → 轮询 INTERRUPT_STATUS bit3（模拟器立即置位）→
 * 读 RANGE_MM → 清中断。
 */
#include "vl53l1x.h"
#include "drv/i2c.h"
#include "devmgr/device_manager.h"
#include "log/app_log.h"
#include <stdlib.h>
#include <string.h>

#define VL53L1X_ADDR 0x29u

typedef struct _vl53l1x {
    device parent;
    vl53l1x_config_t cfg;
    device *i2c;
} vl53l1x;

/* 写 16 位寄存器（1 次事务：地址 + 数据） */
static int reg_write16(vl53l1x *self, uint16_t reg, uint16_t val)
{
    uint8_t buf[4] = { (uint8_t)(reg >> 8), (uint8_t)reg,
                       (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    i2c_xfer_t x = { .addr = VL53L1X_ADDR, .buf = buf, .len = 4, .result = -1 };
    int rc = self->i2c->vtable->ioctl(self->i2c, I2C_IOCTL_MASTER_WRITE, &x);
    return (rc == 0 && x.result == 0) ? 0 : -1;
}

/* 读 16 位寄存器：设指针 + 读 2 字节（小端：低字节在前） */
static int reg_read16(vl53l1x *self, uint16_t reg, uint16_t *val)
{
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    i2c_xfer_t xw = { .addr = VL53L1X_ADDR, .buf = a, .len = 2, .result = -1 };
    if (self->i2c->vtable->ioctl(self->i2c, I2C_IOCTL_MASTER_WRITE, &xw) != 0 || xw.result != 0)
        return -1;
    uint8_t d[2] = { 0, 0 };
    i2c_xfer_t xr = { .addr = VL53L1X_ADDR, .buf = d, .len = 2, .result = -1 };
    if (self->i2c->vtable->ioctl(self->i2c, I2C_IOCTL_MASTER_READ, &xr) != 0 || xr.result != 0)
        return -1;
    *val = (uint16_t)(((uint16_t)d[1] << 8) | d[0]);
    return 0;
}

/* 读单字节寄存器 */
static int reg_read8(vl53l1x *self, uint16_t reg, uint8_t *val)
{
    uint16_t v;
    if (reg_read16(self, reg, &v) != 0) return -1;
    *val = (uint8_t)(v & 0xFF);
    return 0;
}

static int vl53l1x_dev_open(device *self)
{
    vl53l1x *p = (vl53l1x *)self;
    if (p->i2c->vtable->open(p->i2c) != 0) {
        log_printf(app_log(), LOG_ERROR, "vl53l1x", "[vl53l1x] %s: i2c 依赖 open 失败\n", p->cfg.name);
        return -1;
    }
    /* WHO_AM_I 校验（0x010F = 0xEA） */
    uint8_t who = 0;
    if (reg_read8(p, 0x010F, &who) != 0 || who != VL53L1X_WHO_AM_I) {
        log_printf(app_log(), LOG_ERROR, "vl53l1x", "[vl53l1x] %s: WHO 校验失败 who=0x%02X\n", p->cfg.name, who);
        return -1;
    }
    /* 软复位 + 等固件就绪（FIRMWARE_SYSTEM_STATUS bit3） */
    reg_write16(p, 0x0000, 0x00);
    uint8_t st = 0;
    for (int i = 0; i < 5; i++) {
        if (reg_read8(p, 0x000F, &st) == 0 && (st & 0x08) != 0) break;
    }
    return 0;
}

static int vl53l1x_dev_close(device *self)
{
    vl53l1x *p = (vl53l1x *)self;
    if (p->i2c) p->i2c->vtable->close(p->i2c);
    return 0;
}

static int vl53l1x_dev_ioctl(device *self, int cmd, void *arg)
{
    vl53l1x *p = (vl53l1x *)self;
    switch (cmd) {
    case VL53L1X_IOCTL_GET_WHO: {
        if (!arg) return -1;
        return reg_read8(p, 0x010F, (uint8_t *)arg);
    }
    case VL53L1X_IOCTL_START_RANGE:
        return reg_write16(p, 0x0040, 0x40);
    case VL53L1X_IOCTL_GET_DISTANCE: {
        if (!arg) return -1;
        return reg_read16(p, 0x0096, (uint16_t *)arg);
    }
    case VL53L1X_IOCTL_CLEAR_INT:
        return reg_write16(p, 0x0013, 0x01);
    default:
        return -1;
    }
}

static const struct deviceVtable vl53l1x_dev_vtable = {
    .open   = vl53l1x_dev_open,
    .close  = vl53l1x_dev_close,
    .read   = NULL,
    .write  = NULL,
    .ioctl  = vl53l1x_dev_ioctl,
    .irq_id = NULL,
};

device *vl53l1x_create(const void *config)
{
    const vl53l1x_config_t *c = (const vl53l1x_config_t *)config;
    vl53l1x *self = (vl53l1x *)malloc(sizeof(vl53l1x));
    if (!self) return NULL;
    memset(self, 0, sizeof(vl53l1x));
    self->parent.vtable = &vl53l1x_dev_vtable;
    self->parent.type   = DEVICE_TYPE_GPIO; /* 复用既有类型（测距观测，控制类） */
    self->parent.class  = DEVICE_CLASS_CONTROL;
    self->parent.name   = c->name;
    self->cfg = *c;

    self->i2c = device_manager_get(c->i2c);
    if (!self->i2c) {
        log_printf(app_log(), LOG_ERROR, "vl53l1x", "[vl53l1x] %s: 依赖 i2c %s 缺失\n", c->name, c->i2c);
        free(self);
        return NULL;
    }
    return (device *)self;
}
