#ifndef IRQ_MANAGER_H
#define IRQ_MANAGER_H

#include "irq.h"

/*
 * Centralized interrupt MANAGER — a thin layer ON TOP of the platform-neutral
 * irq framework (irq_register / irq_enable / ...).
 *
 * WHY: the raw framework lets any driver register a callback and arm the NVIC
 * independently, so the two can drift apart (e.g. an interrupt enabled with no
 * handler -> the core vectors into an empty slot and silently locks up). The
 * manager forces every attachment through ONE funnel that:
 *   - records the callback + enable state in a single table (g_mgr[]);
 *   - guarantees "enabled => has callback" (irq_manager_enable is a no-op when
 *     no handler is installed), eliminating the empty-slot lockup hazard;
 *   - exposes irq_manager_dump() so the whole interrupt wiring can be verified
 *     from the console / BIST output.
 *
 * It is still fully platform-independent: it only calls irq_register / irq_enable
 * / irq_disable and irq_hal_index (the HAL index mapping). Drivers keep using
 * their HAL to learn the irq id (e.g. uart_hal_irq_id), so nothing becomes
 * chip-specific.
 *
 * USAGE (replaces the raw two-step pattern):
 *     irq_manager_attach(id, my_isr, self);   // install handler
 *     irq_manager_enable(id);                 // arm NVIC (safe: cb present)
 *     ...
 *     irq_manager_disable(id);                // mask NVIC (cb stays)
 *     irq_manager_detach(id);                 // mask + uninstall
 */

/* One entry per interrupt source the manager knows about. */
typedef struct {
    irq_id_t       id;         /* the irq id (HAL-defined; on STM32: IRQn_Type) */
    irq_callback_t cb;         /* installed handler (NULL = slot empty) */
    void *         ctx;        /* per-registration context */
    int            registered; /* irq_register() was called (handler present) */
    int            enabled;    /* NVIC armed (irq_enable called) */
} irq_mgr_entry_t;

/* Register (or replace) the callback for an interrupt source. Records the
 * attachment in the manager table but does NOT touch the NVIC — pair with
 * irq_manager_enable() to arm it (mirrors the original register-then-enable
 * ordering so a source is never enabled before its handler exists). */
int irq_manager_attach(irq_id_t id, irq_callback_t cb, void *ctx);

/* Arm the NVIC for an already-attached source and update bookkeeping.
 * SAFE: if no callback is installed the call is a no-op, so an interrupt can
 * never be enabled into an empty slot. */
void irq_manager_enable(irq_id_t id);

/* Mask the NVIC for a source; the callback stays installed. */
void irq_manager_disable(irq_id_t id);

/* Detach: mask the NVIC and uninstall the callback. */
int irq_manager_detach(irq_id_t id);

/* Print the manager's state table (for BIST / console debugging). */
void irq_manager_dump(void);

/* Lookup an attached entry (NULL if out of range or not attached). */
const irq_mgr_entry_t *irq_manager_get(irq_id_t id);

#endif /* IRQ_MANAGER_H */
