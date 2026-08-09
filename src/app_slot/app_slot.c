#include "app_slot.h"
#include "rtos.h"            /* 系统内部实现：rtos_task_create / rtos_task_create_rt / IPC */
#include "device.h"          /* device vtable 调用 */
#include "devmgr/device_manager.h"
#include "irq/irq_manager.h"
#include "irq/irq.h"

/* ===========================================================================
 * g_app_slot：系统侧服务表实例。
 *  - 头部 + 函数指针在 app_slot_init() 填充；
 *  - irq_reg[] 由 App 在 app_start 入口填好，再经 irq_attach() 落真实路由。
 * 固定链接地址见 linker/STM32F407VGTX_FLASH.ld（APP_SLOT 段）。
 * ========================================================================= */
app_slot_t g_app_slot __attribute__((section(".app_slot")));

/* ---- 设备服务封装（镜像 rtos_abi.h 的 device_manager_get + vtable）---- */
static device *app_slot_dev_get(const char *name) {
    return device_manager_get(name);
}
static int app_slot_dev_open(device *self) {
    return self ? self->vtable->open(self) : -1;
}
static int app_slot_dev_read(device *self, void *buf, size_t len) {
    return self ? self->vtable->read(self, buf, len) : -1;
}
static int app_slot_dev_write(device *self, const void *buf, size_t len) {
    /* The Rust app emits its logs / telemetry through dev_write(uart0). Route
     * it to the UART vtable write (stream path). The UART console path owns the
     * serial wire, so output is serialized there; the DMA-TX contention that
     * once garbled interleaved blocks is fixed by the tx_idle lock in uart.c. */
    return self ? self->vtable->write(self, buf, len) : -1;
}
static int app_slot_dev_ioctl(device *self, int cmd, void *arg) {
    return self ? self->vtable->ioctl(self, cmd, arg) : -1;
}
static int app_slot_dev_close(device *self) {
    return self ? self->vtable->close(self) : -1;
}

/* ===========================================================================
 * app_slot_init：在 app_main 早期、调用 App 入口之前填充 g_app_slot。
 * ========================================================================= */
void app_slot_init(void) {
    g_app_slot.magic   = APP_SLOT_MAGIC;
    g_app_slot.version = APP_SLOT_VERSION;
    g_app_slot.reserved = 0;

    /* 内核服务 */
    g_app_slot.task_create    = rtos_task_create;
    g_app_slot.task_create_rt = rtos_task_create_rt;
    g_app_slot.msleep         = rtos_msleep;
    g_app_slot.tick_count     = rtos_tick_count;
    g_app_slot.cycle_now      = rtos_cycle_now;

    /* IPC 服务 */
    g_app_slot.sem_init   = rtos_sem_init;
    g_app_slot.sem_wait   = rtos_sem_wait;
    g_app_slot.sem_trywait = rtos_sem_trywait;
    g_app_slot.sem_give   = rtos_sem_give;

    /* 设备服务 */
    g_app_slot.dev_get   = app_slot_dev_get;
    g_app_slot.dev_open  = app_slot_dev_open;
    g_app_slot.dev_read  = app_slot_dev_read;
    g_app_slot.dev_write = app_slot_dev_write;
    g_app_slot.dev_ioctl = app_slot_dev_ioctl;
    g_app_slot.dev_close = app_slot_dev_close;

    /* 中断注册入口 */
    g_app_slot.irq_attach = app_slot_irq_attach;
    g_app_slot.irq_enable = app_slot_irq_enable;
    g_app_slot.irq_disable = app_slot_irq_disable;

    /* 生命周期：由 App 实现并通过 extern 接好（见 task_app_main.c） */
    g_app_slot.app_start = 0;
    g_app_slot.app_stop  = 0;
}

/* ===========================================================================
 * app_slot_irq_attach：把 App 填好的 irq_reg[] 项转成 irq_manager 真实路由。
 * 物理接线（NVIC 编程 / 共享线引用计数 / 优先级审计）全部由系统侧完成，
 * 与现有 att_rust 的 irq_manager_attach 用法完全同构。
 * ========================================================================= */
int app_slot_irq_attach(const app_irq_reg_t *reg) {
    if (!reg || !reg->used || !reg->isr_cb) return -1;

    irq_id_t     id  = (irq_id_t)reg->irq_id;
    irq_class_t  cls = (irq_class_t)reg->prio_class;
    uint8_t      prio;

    /* 硬实时回调若声明零延迟，则进零延迟带；否则按内核类给默认优先级。 */
    if (cls == IRQ_CLASS_ZERO_LATENCY) {
        prio = IRQ_PRIO_ZERO_LATENCY;
    } else if (cls == IRQ_CLASS_KERNEL) {
        prio = reg->rt_class ? IRQ_PRIO_KERNEL : IRQ_PRIO_DEFAULT;
    } else {
        prio = IRQ_PRIO_DEFAULT;
    }

    irq_manager_attach(id, reg->isr_cb, reg->ctx);
    irq_manager_set_priority(id, prio, cls);
    irq_manager_enable(id, reg->isr_cb, reg->ctx);
    return 0;
}

/* 按 irq_id 在 irq_reg[] 中找到 App 注册的 (cb,ctx)，做 NVIC 掩码。 */
static const app_irq_reg_t *app_slot_find_reg(uint8_t irq_id) {
    for (int i = 0; i < APP_IRQ_REG_MAX; i++) {
        if (g_app_slot.irq_reg[i].used && g_app_slot.irq_reg[i].irq_id == irq_id) {
            return &g_app_slot.irq_reg[i];
        }
    }
    return 0;
}

int app_slot_irq_enable(uint8_t irq_id) {
    const app_irq_reg_t *r = app_slot_find_reg(irq_id);
    if (!r) return -1;
    irq_manager_enable((irq_id_t)irq_id, r->isr_cb, r->ctx);
    return 0;
}

int app_slot_irq_disable(uint8_t irq_id) {
    const app_irq_reg_t *r = app_slot_find_reg(irq_id);
    if (!r) return -1;
    irq_manager_disable((irq_id_t)irq_id, r->isr_cb, r->ctx);
    return 0;
}
