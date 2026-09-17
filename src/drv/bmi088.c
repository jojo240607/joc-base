/*
 * BMI088 双片选六轴 IMU 驱动（SPI 主 + 两个 GPIO 片选）。
 *
 * 平台无关：只通过统一 `device *` 接口访问 SPI 总线（SPI_IOCTL_XFER）与两路
 * 片选 GPIO（device write 控制输出电平：1=拉高/0=拉低）。板级只提供设备名。
 *
 * 协议（SPI 全双工）：
 *   - 写寄存器：CS 低 → 发 (reg<<1)|0 → 发 1 数据字节 → CS 高
 *   - 读寄存器：CS 低 → 发 (reg<<1)|1 → 发 N dummy 收 N 字节（地址自动递增）→ CS 高
 *   - 首字节 MISO 回送无效（全双工），从第 1 个 dummy 起回寄存器值
 *
 * 数据换算（datasheet 灵敏度）：
 *   accel: raw/10920 g（±3g 量程）→ m/s² = raw/10920*9.81
 *   gyro : raw/16.4  dps（±2000dps）→ rad/s = raw/16.4*pi/180
 */
#include "bmi088.h"

#include "devmgr/device_manager.h"
#include "log/log.h"
#include "log/app_log.h"
#include "drv/spi.h"   /* SPI_IOCTL_XFER + spi_xfer_t */
#include <stdlib.h>
#include <string.h>

/* BMI088 SPI 寄存器地址（7bit，帧内 addr<<1|rw） */
#define BMI088_REG_WHO_AM_I    0x00
#define BMI088_REG_GYR_X_L     0x02
#define BMI088_REG_ACC_X_L     0x12
#define BMI088_WHO_ACCEL       0x1E
#define BMI088_WHO_GYRO        0x0F

/* 灵敏度（LSB/单位）——与模拟器 bmi088.rs 保持一致 */
#define BMI088_ACCEL_LSB_PER_G 10920.0f
#define BMI088_GYRO_LSB_PER_DPS 16.4f

struct _bmi088 {
    device parent;      /* unified interface — MUST be first member */
    device *spi;
    device *accel_cs;
    device *gyro_cs;
};

/* ---------- 内部：SPI 事务 ---------- */

static int spi_xfer_bytes(bmi088 *self, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    spi_xfer_t x = { .tx_buf = tx, .rx_buf = rx, .len = len };
    return self->spi->vtable->ioctl(self->spi, SPI_IOCTL_XFER, &x);
}

/* 片选（1=拉高不选中，0=拉低选中） */
static void cs_set(bmi088 *self, device *cs, int level)
{
    uint8_t v = level ? 1u : 0u;
    cs->vtable->write(cs, &v, 1);
}

/* 单寄存器写：CS 低 → (reg<<1)|0 → 数据 → CS 高 */
static void reg_write(bmi088 *self, device *cs, uint8_t reg, uint8_t value)
{
    uint8_t tx[2];
    tx[0] = (uint8_t)((reg << 1) | 0);
    tx[1] = value;
    cs_set(self, cs, 1);   /* 先确保高（帧完整性） */
    cs_set(self, cs, 0);
    spi_xfer_bytes(self, tx, NULL, 2);
    cs_set(self, cs, 1);
}

/* 寄存器块读：CS 低 → (reg<<1)|1 → len 个 dummy（收 len 字节，地址递增）→ CS 高 */
static int reg_read(bmi088 *self, device *cs, uint8_t reg, uint8_t *out, uint16_t len)
{
    uint8_t head = (uint8_t)((reg << 1) | 1);
    uint8_t tmp[8];
    if (len > sizeof(tmp)) len = sizeof(tmp);
    cs_set(self, cs, 1);
    cs_set(self, cs, 0);
    /* 首字节（读地址 + 首 MISO 无效） */
    uint8_t discard = 0;
    spi_xfer_bytes(self, &head, &discard, 1);
    /* 连续读：地址自动递增 */
    memset(tmp, 0xFF, sizeof(tmp));
    if (spi_xfer_bytes(self, tmp, out, len) != 0) {
        cs_set(self, cs, 1);
        return -1;
    }
    cs_set(self, cs, 1);
    return 0;
}

/* ---------- device 接口 ---------- */

static int bmi088_dev_open(device *self)
{
    bmi088 *b = (bmi088 *)self;
    /* 依赖在 create 时已解析并拉起；open 幂等返回 0 */
    if (!b->spi || !b->accel_cs || !b->gyro_cs) return -1;
    /* [SPI DMA] 总线切 STREAM_MODE_DMA：SPI 事务经 DMA 引擎搬运（SET_MODE 内部
     * 补 acquire——DMA 控制器晚于 spi 注册；流被占用/未配置则回退 POLL，不阻塞启动）。 */
    uint32_t dma_mode = STREAM_MODE_DMA;
    b->spi->vtable->ioctl(b->spi, STREAM_IOCTL_SET_MODE, &dma_mode);
    return 0;
}

static int bmi088_dev_close(device *self)
{
    (void)self;
    return 0;
}

static int bmi088_dev_read(device *self, void *buf, size_t len)
{
    bmi088 *b = (bmi088 *)self;
    if (!buf || len < 12) return -1;
    uint8_t *out = (uint8_t *)buf;
    /* accel 6B（0x12..） + gyro 6B（0x02..），16bit LE */
    if (reg_read(b, b->accel_cs, BMI088_REG_ACC_X_L, out, 6) != 0) return -1;
    if (reg_read(b, b->gyro_cs, BMI088_REG_GYR_X_L, out + 6, 6) != 0) return -1;
    return 12;
}

static int bmi088_dev_write(device *self, const void *buf, size_t len)
{
    (void)self; (void)buf; (void)len;
    return -1; /* IMU 只读 */
}

static int bmi088_dev_ioctl(device *self, int cmd, void *arg)
{
    bmi088 *b = (bmi088 *)self;
    if (!arg) return -1;
    switch (cmd) {
    case BMI088_IOCTL_GET_WHO: {
        bmi088_who_t *w = (bmi088_who_t *)arg;
        uint8_t acc = 0, gyr = 0;
        if (reg_read(b, b->accel_cs, BMI088_REG_WHO_AM_I, &acc, 1) != 0) return -1;
        if (reg_read(b, b->gyro_cs, BMI088_REG_WHO_AM_I, &gyr, 1) != 0) return -1;
        w->accel = acc;
        w->gyro  = gyr;
        return 0;
    }
    case BMI088_IOCTL_GET_RAW: {
        bmi088_raw_t *r = (bmi088_raw_t *)arg;
        uint8_t acc[6], gyr[6];
        if (reg_read(b, b->accel_cs, BMI088_REG_ACC_X_L, acc, 6) != 0) return -1;
        if (reg_read(b, b->gyro_cs, BMI088_REG_GYR_X_L, gyr, 6) != 0) return -1;
        for (int i = 0; i < 3; i++) {
            r->accel[i] = (int16_t)((uint16_t)acc[i * 2] | ((uint16_t)acc[i * 2 + 1] << 8));
            r->gyro[i]  = (int16_t)((uint16_t)gyr[i * 2] | ((uint16_t)gyr[i * 2 + 1] << 8));
        }
        return 0;
    }
    case BMI088_IOCTL_GET_SI: {
        bmi088_raw_t raw;
        if (bmi088_dev_ioctl(self, BMI088_IOCTL_GET_RAW, &raw) != 0) return -1;
        bmi088_si_t *s = (bmi088_si_t *)arg;
        for (int i = 0; i < 3; i++) {
            s->accel[i] = (float)raw.accel[i] / BMI088_ACCEL_LSB_PER_G * 9.81f;
            s->gyro[i]  = (float)raw.gyro[i]  / BMI088_GYRO_LSB_PER_DPS * 3.14159265f / 180.0f;
        }
        return 0;
    }
    default:
        return -1;
    }
}

static const struct deviceVtable bmi088_dev_vtable = {
    .open  = bmi088_dev_open,
    .close = bmi088_dev_close,
    .read  = bmi088_dev_read,
    .write = bmi088_dev_write,
    .ioctl = bmi088_dev_ioctl,
};

/* ---------- 工厂 ---------- */

device *bmi088_create(const void *config)
{
    const bmi088_config_t *c = (const bmi088_config_t *)config;
    bmi088 *self = (bmi088 *)malloc(sizeof(bmi088));
    if (!self) return NULL;
    memset(self, 0, sizeof(bmi088));
    self->parent.vtable = &bmi088_dev_vtable;
    self->parent.type   = DEVICE_TYPE_BMI088;
    self->parent.class  = DEVICE_CLASS_CONTROL;
    self->parent.name   = c->name;

    /* 依赖设备在 create 时解析（板级设备表顺序保证先注册 spi/gpio） */
    self->spi     = device_manager_get(c->spi);
    self->accel_cs = device_manager_get(c->accel_cs);
    self->gyro_cs  = device_manager_get(c->gyro_cs);
    if (!self->spi || !self->accel_cs || !self->gyro_cs) {
        log_printf(app_log(), LOG_ERROR, "bmi088", "[bmi088] %s: 依赖缺失 spi=%p accel_cs=%p gyro_cs=%p\n",
                   c->name, (void *)self->spi, (void *)self->accel_cs, (void *)self->gyro_cs);
        free(self);
        return NULL;
    }

    /* open 即拉起依赖并做 WHO_AM_I 校验 */
    if (self->spi->vtable->open(self->spi) != 0 ||
        self->accel_cs->vtable->open(self->accel_cs) != 0 ||
        self->gyro_cs->vtable->open(self->gyro_cs) != 0) {
        log_printf(app_log(), LOG_ERROR, "bmi088", "[bmi088] %s: 依赖 open 失败\n", c->name);
        free(self);
        return NULL;
    }

    bmi088_who_t who;
    if (bmi088_dev_ioctl((device *)self, BMI088_IOCTL_GET_WHO, &who) != 0 ||
        who.accel != BMI088_WHO_ACCEL || who.gyro != BMI088_WHO_GYRO) {
        log_printf(app_log(), LOG_ERROR, "bmi088", "[bmi088] %s: WHO 校验失败 who.accel=0x%02X who.gyro=0x%02X\n",
                   c->name, who.accel, who.gyro);
        free(self);
        return NULL;
    }

    return (device *)self;
}
