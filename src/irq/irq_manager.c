#include "irq_manager.h"
#include "irq_hal.h"   /* irq_hal_index() + IRQ_HAL_TABLE_SIZE (HAL index mapping) */
#include <stdio.h>

/*
 * Centralized interrupt registry.
 *
 * One slot per exception the chip can raise, indexed by the platform's LINEAR
 * interrupt index (irq_hal_index maps irq_id_t -> this index). The HAL owns the
 * index mapping and table size, so this file stays chip-free — exactly like
 * irq.c. The manager merely ADDS bookkeeping + a dump on top of the raw
 * framework, and enforces "enabled => has callback".
 */
static irq_mgr_entry_t g_mgr[IRQ_HAL_TABLE_SIZE];

int irq_manager_attach(irq_id_t id, irq_callback_t cb, void *ctx)
{
    int idx = irq_hal_index(id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return -1;

    /* install the handler FIRST so the source can never fire into a gap */
    irq_register(id, cb, ctx);

    g_mgr[idx].id         = id;
    g_mgr[idx].cb         = cb;
    g_mgr[idx].ctx        = ctx;
    g_mgr[idx].registered = 1;
    /* attach alone does not arm the NVIC; enabled tracks the real HW state */
    g_mgr[idx].enabled    = 0;
    return 0;
}

void irq_manager_enable(irq_id_t id)
{
    int idx = irq_hal_index(id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return;
    /* SAFE: never enable a slot that has no handler (empty-slot lockup guard) */
    if (!g_mgr[idx].registered || !g_mgr[idx].cb)
        return;
    irq_enable(id);
    g_mgr[idx].enabled = 1;
}

void irq_manager_disable(irq_id_t id)
{
    int idx = irq_hal_index(id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return;
    irq_disable(id);
    g_mgr[idx].enabled = 0;
}

int irq_manager_detach(irq_id_t id)
{
    int idx = irq_hal_index(id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return -1;
    irq_disable(id);                 /* mask first, then uninstall */
    irq_register(id, NULL, NULL);
    g_mgr[idx].id         = id;
    g_mgr[idx].cb         = NULL;
    g_mgr[idx].ctx        = NULL;
    g_mgr[idx].registered = 0;
    g_mgr[idx].enabled    = 0;
    return 0;
}

const irq_mgr_entry_t *irq_manager_get(irq_id_t id)
{
    int idx = irq_hal_index(id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return NULL;
    if (!g_mgr[idx].registered)
        return NULL;
    return &g_mgr[idx];
}

void irq_manager_dump(void)
{
    printf("--- IRQ manager table ---\r\n");
    int n = 0;
    for (int idx = 0; idx < (int)IRQ_HAL_TABLE_SIZE; idx++) {
        if (!g_mgr[idx].registered)
            continue;
        n++;
        printf("  irq %3d : reg=%d en=%d cb=%s\r\n",
               (int)g_mgr[idx].id,
               g_mgr[idx].registered,
               g_mgr[idx].enabled,
               g_mgr[idx].cb ? "yes" : "no");
    }
    printf("  (%d source(s) attached)\r\n", n);
    printf("--- end IRQ manager table ---\r\n");
}
