/*
 * DShot 电调数字协议发送驱动（GPIO bit-bang）。
 *
 * 协议（DShot300 语义，模拟器虚拟外设按比例解码）：
 *   - 帧 = 16 位，MSB 先发：(油门 11 位 << 1 | 遥测 1 位) << 4 | CRC4；
 *   - 行空闲 HIGH；每个位单元 = 一个 LOW 脉冲 + 剩余 HIGH；
 *     1 位 = 短 LOW（约 1/4 位周期），0 位 = 长 LOW（约 3/4 位周期）；
 *   - CRC = 对 12 位有效载荷按多项式 0x05 移位求余后取反（XNOR），共 4 位。
 *
 * 位时序为「比例制」：位周期/位脉宽用延迟迭代次数表示（DSHOT_CELL / DSHOT_LOW1
 * / DSHOT_LOW0），解码端按「LOW 脉宽 < 位周期一半 ⇔ 1 位」分类，叠加的 GPIO 写
 * 开销在比例中抵消，故与模拟器虚拟时钟的绝对速率无关。
 */
#include "dshot.h"
#include "devmgr/device_manager.h"
#include "log/app_log.h"
#include <stdlib.h>
#include <string.h>

#define DSHOT_CELL 24u  /* 位周期延迟迭代数（比例基准） */
#define DSHOT_LOW1  6u  /* 1 位 LOW 脉宽迭代数（1/4 周期） */
#define DSHOT_LOW0 18u  /* 0 位 LOW 脉宽迭代数（3/4 周期） */

typedef struct _dshot {
    device parent;
    dshot_config_t cfg;
    device *pin;   /* GPIO 输出引脚（行空闲 HIGH） */
} dshot;

/* 帧内 12 位有效载荷的 DShot CRC（与模拟器 ESC 解码端一致） */
static uint8_t dshot_crc(uint16_t payload)
{
    uint8_t crc = 0;
    for (uint8_t i = 0; i < 12; i++) {
        crc <<= 1;
        if ((payload & (0x800u >> i)) != 0) {
            crc |= 1;
        }
        if ((crc & 0x10u) != 0) {
            crc ^= 0x05u;
        }
    }
    return (uint8_t)((~crc) & 0x0Fu);
}

static void dshot_delay(volatile uint32_t n)
{
    while (n--) {}
}

static void pin_write(dshot *self, uint8_t v)
{
    self->pin->vtable->write(self->pin, &v, 1);
}

/* 发送一帧 16 位（MSB 先发）：每位置 LOW 脉宽 + 高位补齐位周期，帧后保持空闲 */
static int dshot_send_frame(dshot *self, uint16_t frame)
{
    pin_write(self, 1); /* 保证行空闲 HIGH 起始 */
    for (int i = 15; i >= 0; i--) {
        int one = (frame >> i) & 1;
        pin_write(self, 0);
        dshot_delay(one ? DSHOT_LOW1 : DSHOT_LOW0);
        pin_write(self, 1);
        dshot_delay(DSHOT_CELL - (one ? DSHOT_LOW1 : DSHOT_LOW0));
    }
    dshot_delay(DSHOT_CELL); /* 帧间空闲 */
    return 0;
}

static int dshot_dev_open(device *self)
{
    dshot *d = (dshot *)self;
    if (!d->pin) return -1;
    return d->pin->vtable->open(d->pin);
}

static int dshot_dev_close(device *self)
{
    dshot *d = (dshot *)self;
    if (d->pin) d->pin->vtable->close(d->pin);
    return 0;
}

static int dshot_dev_ioctl(device *self, int cmd, void *arg)
{
    dshot *d = (dshot *)self;
    switch (cmd) {
    case DSHOT_IOCTL_SEND: {
        if (!arg) return -1;
        uint16_t throttle = *(const uint16_t *)arg;
        if (throttle > 1999u) throttle = 1999u;
        uint16_t payload = (uint16_t)((throttle << 1) | 0u); /* 遥测关闭 */
        uint16_t frame = (uint16_t)((payload << 4) | dshot_crc(payload));
        return dshot_send_frame(d, frame);
    }
    default:
        return -1;
    }
}

static const struct deviceVtable dshot_dev_vtable = {
    .open   = dshot_dev_open,
    .close  = dshot_dev_close,
    .read   = NULL,
    .write  = NULL,
    .ioctl  = dshot_dev_ioctl,
    .irq_id = NULL,
};

device *dshot_create(const void *config)
{
    const dshot_config_t *c = (const dshot_config_t *)config;
    dshot *self = (dshot *)malloc(sizeof(dshot));
    if (!self) return NULL;
    memset(self, 0, sizeof(dshot));
    self->parent.vtable = &dshot_dev_vtable;
    self->parent.type   = DEVICE_TYPE_GPIO; /* 复用既有类型，不再引入新枚举 */
    self->parent.class  = DEVICE_CLASS_CONTROL;
    self->parent.name   = c->name;
    self->cfg = *c;

    /* 依赖 GPIO 引脚在 create 时解析（板级设备表顺序保证 gpio 先注册） */
    self->pin = device_manager_get(c->gpio);
    if (!self->pin) {
        log_printf(app_log(), LOG_ERROR, "dshot", "[dshot] %s: 依赖 GPIO %s 缺失\n", c->name, c->gpio);
        free(self);
        return NULL;
    }
    return (device *)self;
}
