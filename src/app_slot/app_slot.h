#ifndef APP_SLOT_H
#define APP_SLOT_H

/* ===========================================================================
 * 方案 Y（轻量版）App 服务表：RTOS 系统暴露给独立 App 层（joc-app-rust）的
 * 「函数指针表 + 中断回调注册位」契约。
 *
 * 设计要点：
 *  - App 不碰裸寄存器 / NVIC / VTOR，所有系统能力经本表函数指针拿到。
 *  - App 想注册中断回调时，只填 irq_reg[]「注册位」并调 irq_attach()，
 *    真实中断路由由系统侧 irq_manager 完成（与 att_rust 现有机制同构）。
 *  - g_app_slot 由系统在固定链接地址定义（APP_SLOT 段），App 经 extern 引用。
 *  - 字段变更必须 +RTOS_ABI_VERSION（见 tools/abi/rtos_abi.h）。
 *
 * 本文件被系统侧 app_slot.c 填充，也被 joc-app-rust 镜像成 abi/app_slot.rs。
 * ========================================================================= */

#include <stdint.h>
#include <stddef.h>
#include "rtos.h"            /* RTOS_ABI_VERSION, rtos_task_entry_t, rtos_sem_t, rtos_task_attr_t */
#include "device.h"         /* device, deviceVtable */
#include "irq/irq_manager.h" /* irq_id_t, irq_class_t, irq_manager_* 签名 */

/* App 提交给系统的中断回调注册请求（att_isr_give 这类） */
typedef struct app_irq_reg {
    uint8_t  used;                 /* 1 = App 填了本槽 */
    uint8_t  irq_id;               /* TIMx_IRQn 等（系统 irq_id_t 镜像，App 只给逻辑 id） */
    uint8_t  prio_class;           /* IRQ_CLASS_* 镜像：0=NORMAL,1=KERNEL,2=ZERO_LATENCY */
    uint8_t  rt_class;             /* 0=普通, 1=硬实时（与 rtos_task_attr_t.rt_class 一致） */
    void   (*isr_cb)(void *ctx);   /* App 提供的回调，如 att_isr_give（ISR 安全） */
    void    *ctx;                  /* 通常指向 App 私有 sem */
} app_irq_reg_t;

#define APP_SLOT_MAGIC    0x41505053u   /* "APPS" */
#define APP_IRQ_REG_MAX   8             /* 轻量版：App 最多注册 8 个 ISR 回调 */
#define APP_SLOT_VERSION  1   /* MUST match tools/abi/rtos_abi.h RTOS_ABI_VERSION */

typedef void (*rtos_task_entry_t)(void *);   /* 镜像 rtos_abi.h；rtos.h 用字面量 */

typedef struct app_slot {
    /* ---- 头部：链接期 ABI 校验 ---- */
    uint32_t magic;        /* APP_SLOT_MAGIC */
    uint32_t version;      /* 必须等于 RTOS_ABI_VERSION */
    uint32_t reserved;

    /* ---- RTOS 内核服务指针（镜像 rtos_abi.h，字段顺序严格一致）---- */
    void (*task_create)(const char *name, rtos_task_entry_t entry, void *arg,
                        uint8_t prio, void *stack, size_t stack_size);
    void (*task_create_rt)(const char *name, rtos_task_entry_t entry, void *arg,
                           uint8_t prio, void *stack, size_t stack_size,
                           uint8_t priv, const rtos_task_attr_t *attr);
    void (*msleep)(uint32_t ms);
    uint32_t (*tick_count)(void);
    uint32_t (*cycle_now)(void);

    /* ---- IPC 服务指针 ---- */
    void (*sem_init)(rtos_sem_t *s, uint32_t initial, uint32_t limit);
    int  (*sem_wait)(rtos_sem_t *s);
    int  (*sem_trywait)(rtos_sem_t *s);
    void (*sem_give)(rtos_sem_t *s);

    /* ---- 设备服务指针（统一 device vtable 镜像）---- */
    device *(*dev_get)(const char *name);
    int (*dev_open)(device *self);
    int (*dev_read)(device *self, void *buf, size_t len);
    int (*dev_write)(device *self, const void *buf, size_t len);
    int (*dev_ioctl)(device *self, int cmd, void *arg);
    int (*dev_close)(device *self);

    /* ---- 中断回调注册位（方案 Y 轻量版关键）---- */
    app_irq_reg_t irq_reg[APP_IRQ_REG_MAX];

    /* ---- 系统提供的注册入口（App 调它，不直接碰 irq_manager）---- */
    int (*irq_attach)(const app_irq_reg_t *reg);   /* 内部转 irq_manager_attach */
    int (*irq_enable)(uint8_t irq_id);
    int (*irq_disable)(uint8_t irq_id);

    /* ---- 生命周期 ---- */
    int  (*app_start)(void);    /* 系统调用：App 入口，返回 0=OK */
    void (*app_stop)(void);     /* 系统调用：App 卸载钩子 */
} app_slot_t;

/* 系统在固定链接地址定义实例；App 经 extern 引用，不可自行定义。 */
extern app_slot_t g_app_slot;

/* 系统侧：在 app_main 早期填充 g_app_slot 的函数指针 + 头部。
 * 必须在调用 rust_app_start()（或任何 App 入口）之前完成。 */
void app_slot_init(void);

/* 系统侧：把 App 填好的 irq_reg[] 某一项转成 irq_manager 真实路由。 */
int app_slot_irq_attach(const app_irq_reg_t *reg);
int app_slot_irq_enable(uint8_t irq_id);   /* 按 irq_id 查 irq_reg[] 做 NVIC 掩码 */
int app_slot_irq_disable(uint8_t irq_id);

#endif /* APP_SLOT_H */
