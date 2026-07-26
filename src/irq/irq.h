#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>
#include <stddef.h>   /* NULL */

/*
 * Unified, PLATFORM-INDEPENDENT interrupt framework.
 *
 * LAYERING (mirrors the project's drv/ + hal/<chip>/ split):
 *   - This file (src/irq/) is platform-neutral: it defines the generic
 *     interrupt API (register / enable / disable / set-priority / dispatch)
 *     and owns the callback registry. It knows NOTHING about the NVIC, the
 *     chip's IRQ numbering, or the vector table.
 *   - The chip-specific part lives in hal/<chip>/irq_hal.{h,c}: it programs the
 *     NVIC and supplies the SINGLE shared ISR that the vector table points at.
 *     That ISR reads the active exception and calls irq_dispatch(), which looks
 *     up the registered callback here. So the vector table, the NVIC, and the
 *     chip's IRQ names are ALL hidden behind this generic API.
 *
 * An "interrupt id" (irq_id_t) is an opaque integer whose meaning is defined by
 * the HAL. On STM32 it is the CMSIS IRQn_Type (core exceptions negative, device
 * IRQs >= 0). Drivers obtain the id from their HAL (e.g. uart_hal_irq_id()) so
 * they NEVER name a chip-specific interrupt directly — keeping them portable.
 *
 * USAGE (driver side, platform-neutral):
 *     irq_register(uart_hal_irq_id(hal), my_isr, self);  // install callback
 *     irq_set_priority(id, 0);
 *     irq_enable(id);
 * and  void my_isr(void *ctx)  runs in interrupt context when the IRQ fires.
 * The callback MUST clear the interrupt source (read the peripheral DR / flag),
 * otherwise the interrupt re-enters.
 */

/* Opaque interrupt source id. The HAL gives it meaning (on STM32: IRQn_Type). */
typedef int irq_id_t;

/* Callback invoked from interrupt context; ctx is the per-registration context. */
typedef void (*irq_callback_t)(void *ctx);

/* Max handlers that may share a single physical IRQ line. On STM32F4 several
 * peripherals route their interrupt to the SAME line (e.g. TIM1_UP and TIM10
 * both land on IRQ 25; TIM8_UP and TIM13 both land on IRQ 44). The dispatch
 * walks this many slots per line and invokes every registered callback, so a
 * driver that shares a line MUST guard on its OWN peripheral status flag. */
#ifndef IRQ_MAX_HANDLERS_PER_LINE
#define IRQ_MAX_HANDLERS_PER_LINE 4
#endif

/* Register (ADD) a handler for an interrupt source. Multiple handlers may be
 * registered on the same id (a shared IRQ line); each is invoked in turn when
 * the line fires. Registering the same (cb, ctx) pair twice is idempotent.
 * Returns 0 on success, <0 if the id is out of range or the line is full. */
int  irq_register(irq_id_t id, irq_callback_t cb, void *ctx);

/* Remove a previously-registered handler, identified by its (cb, ctx) pair.
 * Returns 0 if found and removed, <0 otherwise. */
int  irq_unregister(irq_id_t id, irq_callback_t cb, void *ctx);

/* Enable / disable the interrupt at the NVIC (HAL-backed). */
void irq_enable(irq_id_t id);
void irq_disable(irq_id_t id);

/* Set the preemption priority (platform-defined; on STM32 0 = highest). */
void irq_set_priority(irq_id_t id, uint32_t prio);

/* ---------------------------------------------------------------------------
 * 集中式优先级治理（取代各驱动里散落的魔法数字；详见 irq_manager.h）
 *
 * ISR 类别：供启动期契约校验（FreeRTOS 式 BASEPRI 屏蔽）判断一个 ISR 能否被内核
 * 临界区安全屏蔽。
 *   IRQ_CLASS_KERNEL       调用内核 API（rtos_* 等内核 API / IPC / 工作队列 / BH 触发等），临界区
 *                          必须能屏蔽它 => 其 NVIC 优先级数必须 >= 阈值；
 *   IRQ_CLASS_ZERO_LATENCY 不调用内核 API、且对抖动极敏感，需永不被内核临界区屏蔽
 *                          => 其优先级数必须 < 阈值（落在阈值之下的零延迟带）；
 *   IRQ_CLASS_NORMAL       其它（不调内核 API、非抖动敏感）。
 *
 * 语义化优先级带（数值越小优先级越高，0 = 最高）：
 *   IRQ_PRIO_ZERO_LATENCY  零延迟带（默认 2）
 *   IRQ_PRIO_KERNEL        内核类 ISR 带（默认 5）
 *   IRQ_PRIO_DEFAULT       普通带（默认 8）
 * 真正启用 BASEPRI 选择性屏蔽（RTOS_MAX_ZERO_LATENCY_IRQS > 0）时，必须保证
 *   IRQ_PRIO_KERNEL >= 阈值 且 IRQ_PRIO_ZERO_LATENCY < 阈值，否则 rtos_start 的
 *   启动期审计（irq_manager_audit_priorities）会报违例。默认关闭时这些带仅作组织
 *   用途、不改变任何行为。
 * ------------------------------------------------------------------------- */
typedef enum irq_class {
    IRQ_CLASS_NORMAL = 0,
    IRQ_CLASS_KERNEL,
    IRQ_CLASS_ZERO_LATENCY
} irq_class_t;

#ifndef IRQ_PRIO_ZERO_LATENCY
  #define IRQ_PRIO_ZERO_LATENCY 2
#endif
#ifndef IRQ_PRIO_KERNEL
  #define IRQ_PRIO_KERNEL 5
#endif
#ifndef IRQ_PRIO_DEFAULT
  #define IRQ_PRIO_DEFAULT 8
#endif

/* Clear a pending interrupt (write the NVIC ICPR). */
void irq_clear_pending(irq_id_t id);

/* Bridge called by the HAL's shared ISR with the ACTIVE exception id.
 * Looks up the registered callback and invokes it. Defined in irq.c. */
void irq_dispatch(irq_id_t active_id);

#endif /* IRQ_H */
