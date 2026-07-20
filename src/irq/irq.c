#include "irq.h"
#include "irq_hal.h"   /* platform backing: IRQ_HAL_TABLE_SIZE + irq_hal_index() */

/*
 * Platform-independent interrupt registry.
 *
 * One LINE per exception the chip can raise, indexed by the platform's LINEAR
 * interrupt index (irq_hal_index() maps irq_id_t -> this index). Each line can
 * host up to IRQ_MAX_HANDLERS_PER_LINE handlers, so peripherals that share an
 * IRQ line (e.g. TIM1_UP + TIM10 on STM32F4 IRQ 25) can BOTH be wired through
 * the framework at once. The platform HAL owns the index mapping and the table
 * size, so this file stays chip-free.
 *
 * When a line fires, irq_dispatch() invokes EVERY handler registered on it. A
 * handler that shares a line MUST therefore check its own peripheral's status
 * flag before acting — a sibling on the same line may have been the real
 * trigger (see timer_isr()'s UIF guard for the canonical example).
 */
typedef struct {
    irq_callback_t cb;
    void *ctx;
} irq_handler_t;

static irq_handler_t g_irq[IRQ_HAL_TABLE_SIZE][IRQ_MAX_HANDLERS_PER_LINE];

int irq_register(irq_id_t id, irq_callback_t cb, void *ctx)
{
    int idx = irq_hal_index(id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return -1;
    if (!cb)
        return -1;

    /* idempotent: same (cb, ctx) already present -> nothing to do */
    for (int i = 0; i < (int)IRQ_MAX_HANDLERS_PER_LINE; i++) {
        if (g_irq[idx][i].cb == cb && g_irq[idx][i].ctx == ctx)
            return 0;
    }
    /* append into the first free slot */
    for (int i = 0; i < (int)IRQ_MAX_HANDLERS_PER_LINE; i++) {
        if (g_irq[idx][i].cb == NULL) {
            g_irq[idx][i].cb  = cb;
            g_irq[idx][i].ctx = ctx;
            return 0;
        }
    }
    return -1;   /* line full */
}

int irq_unregister(irq_id_t id, irq_callback_t cb, void *ctx)
{
    int idx = irq_hal_index(id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return -1;
    for (int i = 0; i < (int)IRQ_MAX_HANDLERS_PER_LINE; i++) {
        if (g_irq[idx][i].cb == cb && g_irq[idx][i].ctx == ctx) {
            g_irq[idx][i].cb  = NULL;
            g_irq[idx][i].ctx = NULL;
            return 0;
        }
    }
    return -1;   /* not found */
}

void irq_enable(irq_id_t id)        { irq_hal_enable(id); }
void irq_disable(irq_id_t id)       { irq_hal_disable(id); }
void irq_set_priority(irq_id_t id, uint32_t prio) { irq_hal_set_priority(id, prio); }
void irq_clear_pending(irq_id_t id) { irq_hal_clear_pending(id); }

void irq_dispatch(irq_id_t active_id)
{
    int idx = irq_hal_index(active_id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return;                     /* id not in the dispatchable range */
    /* Invoke EVERY handler on this line. On a shared line a sibling peripheral
     * may have been the actual trigger, so each handler must self-guard. */
    for (int i = 0; i < (int)IRQ_MAX_HANDLERS_PER_LINE; i++) {
        if (g_irq[idx][i].cb)
            g_irq[idx][i].cb(g_irq[idx][i].ctx);
    }
}
