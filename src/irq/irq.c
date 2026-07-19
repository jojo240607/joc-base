#include "irq.h"
#include "irq_hal.h"   /* platform backing: IRQ_HAL_TABLE_SIZE + irq_hal_index() */

/*
 * Platform-independent interrupt registry.
 *
 * One slot per exception the chip can raise, indexed by the platform's LINEAR
 * interrupt index (irq_hal_index() maps irq_id_t -> this index). The platform
 * HAL owns the index mapping and the table size, so this file stays chip-free.
 */
static struct {
    irq_callback_t cb;
    void *ctx;
} g_irq[IRQ_HAL_TABLE_SIZE];

int irq_register(irq_id_t id, irq_callback_t cb, void *ctx)
{
    int idx = irq_hal_index(id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return -1;
    g_irq[idx].cb  = cb;
    g_irq[idx].ctx = ctx;
    return 0;
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
    if (g_irq[idx].cb)
        g_irq[idx].cb(g_irq[idx].ctx);
}
