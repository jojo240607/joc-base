#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

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

/* Register (or replace) the handler for an interrupt source.
 *   cb  != NULL  -> install/overwrite the handler (called with `ctx`).
 *   cb  == NULL  -> uninstall the handler (the slot dispatches to nothing).
 * Returns 0 on success, <0 if the id is outside the platform's range. */
int  irq_register(irq_id_t id, irq_callback_t cb, void *ctx);

/* Enable / disable the interrupt at the NVIC (HAL-backed). */
void irq_enable(irq_id_t id);
void irq_disable(irq_id_t id);

/* Set the preemption priority (platform-defined; on STM32 0 = highest). */
void irq_set_priority(irq_id_t id, uint32_t prio);

/* Clear a pending interrupt (write the NVIC ICPR). */
void irq_clear_pending(irq_id_t id);

/* Bridge called by the HAL's shared ISR with the ACTIVE exception id.
 * Looks up the registered callback and invokes it. Defined in irq.c. */
void irq_dispatch(irq_id_t active_id);

#endif /* IRQ_H */
