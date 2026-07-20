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
 *   - records each (id, cb, ctx) attachment in a single pool (g_mgr[]);
 *   - guarantees "enabled => has callback" (irq_manager_enable is a no-op when
 *     no handler is installed), eliminating the empty-slot lockup hazard;
 *   - reference-counts the NVIC enable PER LINE: an IRQ line is armed iff at
 *     least one attached handler on it is enabled, and masked only when the
 *     last enabled handler is removed/disabled. This is what lets two
 *     peripherals that share a line (TIM1_UP + TIM10 on IRQ 25) coexist — each
 *     is enabled/disabled independently yet the single NVIC line is managed
 *     correctly;
 *   - exposes irq_manager_dump() so the whole interrupt wiring can be verified
 *     from the console / BIST output.
 *
 * It is still fully platform-independent: it only calls irq_register /
 * irq_unregister / irq_enable / irq_disable and irq_hal_index. Drivers keep
 * using their HAL to learn the irq id (e.g. uart_hal_irq_id), so nothing
 * becomes chip-specific.
 *
 * USAGE (replaces the raw two-step pattern):
 *     irq_manager_attach(id, my_isr, self);        // install handler
 *     irq_manager_enable(id, my_isr, self);        // arm NVIC (safe: cb present)
 *     ...
 *     irq_manager_disable(id, my_isr, self);       // mask NVIC (cb stays)
 *     irq_manager_detach(id, my_isr, self);        // mask + uninstall
 * The (cb, ctx) arguments identify WHICH handler on a (possibly shared) line is
 * being enabled/disabled/detached, so multiple handlers on one IRQ line never
 * clobber each other.
 */

/* One entry per attached handler the manager knows about (a shared line has
 * several of these with the same id). */
typedef struct {
    irq_id_t       id;         /* the irq id (HAL-defined; on STM32: IRQn_Type) */
    irq_callback_t cb;         /* installed handler (NULL = slot empty) */
    void *         ctx;        /* per-registration context */
    int            registered; /* irq_register() was called (handler present) */
    int            enabled;    /* NVIC armed for this handler (line live) */
} irq_mgr_entry_t;

/* Register (ADD) a handler for an interrupt source. Records the attachment in
 * the manager pool but does NOT touch the NVIC — pair with irq_manager_enable()
 * to arm it (mirrors the original register-then-enable ordering so a source is
 * never enabled before its handler exists). Multiple handlers may share one id.
 * Returns 0 on success, <0 if the id is out of range, the line is full, or the
 * pool is exhausted. */
int irq_manager_attach(irq_id_t id, irq_callback_t cb, void *ctx);

/* Arm the NVIC for a SPECIFIC attached handler (identified by cb+ctx) and
 * update bookkeeping. The NVIC line is armed iff this is the first enabled
 * handler on it (reference counted). SAFE: if the handler is not attached the
 * call is a no-op, so an interrupt can never be enabled into an empty slot. */
void irq_manager_enable(irq_id_t id, irq_callback_t cb, void *ctx);

/* Mask the NVIC for a SPECIFIC handler; the callback stays installed. The NVIC
 * line is masked only when the last enabled handler on it is disabled. */
void irq_manager_disable(irq_id_t id, irq_callback_t cb, void *ctx);

/* Detach a SPECIFIC handler: mask the NVIC (if it was the last enabled on the
 * line) and uninstall the callback. */
int irq_manager_detach(irq_id_t id, irq_callback_t cb, void *ctx);

/* Print the manager's state table (for BIST / console debugging). */
void irq_manager_dump(void);

/* Lookup the first attached entry on a line (NULL if out of range or none). */
const irq_mgr_entry_t *irq_manager_get(irq_id_t id);

#endif /* IRQ_MANAGER_H */
