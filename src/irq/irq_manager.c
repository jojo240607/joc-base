#include "irq_manager.h"
#include "irq.h"
#include "irq_hal.h"   /* irq_hal_index() + IRQ_HAL_TABLE_SIZE (HAL index mapping) */
#include <stdio.h>
#include "log/log.h"
#include "log/app_log.h"

/*
 * Centralized interrupt registry — a thin layer ON TOP of the platform-neutral
 * irq framework (irq_register / irq_unregister / irq_enable / irq_disable).
 *
 * The manager adds bookkeeping + a dump on top of the raw framework, and
 * enforces "enabled => has callback". Crucially, it reference-counts the NVIC
 * enable PER IRQ LINE: because several handlers can share one line (TIM1_UP +
 * TIM10 on STM32F4 IRQ 25), the line must stay armed while ANY of its handlers
 * is enabled, and be masked only when the last enabled handler goes away. Each
 * handler is identified by its (id, cb, ctx) triple so enable/disable/detach on
 * a shared line never disturbs a sibling.
 *
 * It is still fully platform-independent: it only calls irq_register /
 * irq_unregister / irq_enable / irq_disable and irq_hal_index. Drivers keep
 * using their HAL to learn the irq id (e.g. uart_hal_irq_id), so nothing
 * becomes chip-specific.
 */

/* Flat pool of attached handlers, bounded by the realistic number of
 * concurrently-attached interrupt sources (well under IRQ_HAL_TABLE_SIZE even
 * with shared lines). IRQ_MGR_POOL is defined in irq_manager.h. */
static irq_mgr_entry_t g_mgr[IRQ_MGR_POOL];

/* Count the enabled handlers on a given irq line. */
static int mgr_enabled_on_line(irq_id_t id)
{
    int n = 0;
    for (int i = 0; i < IRQ_MGR_POOL; i++)
        if (g_mgr[i].registered && g_mgr[i].enabled && g_mgr[i].id == id)
            n++;
    return n;
}

/* Find the pool entry for a specific handler (id, cb, ctx). */
static irq_mgr_entry_t *mgr_find(irq_id_t id, irq_callback_t cb, void *ctx)
{
    for (int i = 0; i < IRQ_MGR_POOL; i++)
        if (g_mgr[i].registered && g_mgr[i].id == id &&
            g_mgr[i].cb == cb && g_mgr[i].ctx == ctx)
            return &g_mgr[i];
    return NULL;
}

int irq_manager_attach(irq_id_t id, irq_callback_t cb, void *ctx)
{
    int idx = irq_hal_index(id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return -1;
    if (!cb)
        return -1;

    /* install the handler FIRST so the source can never fire into a gap */
    if (irq_register(id, cb, ctx) != 0)
        return -1;               /* line full or id out of range */

    irq_mgr_entry_t *e = mgr_find(id, cb, ctx);   /* already in the pool? */
    if (!e) {
        for (int i = 0; i < IRQ_MGR_POOL; i++) {  /* else grab a free slot */
            if (!g_mgr[i].registered) { e = &g_mgr[i]; break; }
        }
    }
    if (!e) {                    /* pool exhausted: roll back the irq register */
        irq_unregister(id, cb, ctx);
        return -1;
    }
    e->id         = id;
    e->cb         = cb;
    e->ctx        = ctx;
    e->registered = 1;
    e->enabled    = 0;            /* attach alone does not arm the NVIC */
    e->prio       = 0xFF;         /* priority not yet set via the manager */
    e->cls        = IRQ_CLASS_NORMAL;
    return 0;
}

void irq_manager_enable(irq_id_t id, irq_callback_t cb, void *ctx)
{
    irq_mgr_entry_t *e = mgr_find(id, cb, ctx);
    if (!e)
        return;                   /* not attached -> nothing to arm */
    if (e->enabled)
        return;                   /* idempotent */
    int was_enabled = mgr_enabled_on_line(id);
    e->enabled = 1;
    if (was_enabled == 0)        /* first enabled handler on this line -> arm NVIC */
        irq_enable(id);
}

void irq_manager_disable(irq_id_t id, irq_callback_t cb, void *ctx)
{
    irq_mgr_entry_t *e = mgr_find(id, cb, ctx);
    if (!e || !e->enabled)
        return;
    e->enabled = 0;
    if (mgr_enabled_on_line(id) == 0)   /* last enabled handler gone -> mask NVIC */
        irq_disable(id);
}

int irq_manager_detach(irq_id_t id, irq_callback_t cb, void *ctx)
{
    irq_mgr_entry_t *e = mgr_find(id, cb, ctx);
    if (!e)
        return -1;
    irq_unregister(id, cb, ctx);
    e->id         = id;
    e->cb         = NULL;
    e->ctx        = NULL;
    e->registered = 0;
    e->enabled    = 0;
    /* mask the NVIC line only if no enabled handler remains on it */
    if (mgr_enabled_on_line(id) == 0)
        irq_disable(id);
    return 0;
}

void irq_manager_set_priority(irq_id_t id, uint8_t prio, irq_class_t cls)
{
    /* program the NVIC (framework -> chip HAL) */
    irq_set_priority(id, (uint32_t)prio);
    /* record prio + class for every handler registered on this line, so the
     * boot-time contract check and irq_manager_dump() can see the assignment. */
    for (int i = 0; i < IRQ_MGR_POOL; i++) {
        if (g_mgr[i].registered && g_mgr[i].id == id) {
            g_mgr[i].prio = prio;
            g_mgr[i].cls  = cls;
        }
    }
}

int irq_manager_audit_priorities(uint8_t zero_latency_threshold)
{
    int viol = 0;
    for (int i = 0; i < IRQ_MGR_POOL; i++) {
        irq_mgr_entry_t *e = &g_mgr[i];
        if (!e->registered || !e->enabled)
            continue;             /* only live, kernel-reachable ISRs matter */
        if (e->cls == IRQ_CLASS_KERNEL && e->prio < zero_latency_threshold) {
            /* A kernel ISR above the BASEPRI threshold would preempt the switch
             * critical section and corrupt the ready/wait lists. */
            log_printf(app_log(), LOG_ERROR, "irq",
                "PRIORITY VIOLATION: kernel ISR irq %d prio %u < threshold %u "
                "(would preempt the kernel critical section)",
                (int)e->id, (unsigned)e->prio, (unsigned)zero_latency_threshold);
            viol++;
        } else if (e->cls == IRQ_CLASS_ZERO_LATENCY &&
                   e->prio >= zero_latency_threshold) {
            /* Declared zero-latency but pinned at/below the threshold: it WILL be
             * masked by BASEPRI (defeating the purpose) and is unsafe if it ever
             * calls a kernel API. */
            log_printf(app_log(), LOG_ERROR, "irq",
                "PRIORITY MISCONFIG: zero-latency ISR irq %d prio %u >= threshold %u "
                "(will be masked / unsafe if it calls kernel API)",
                (int)e->id, (unsigned)e->prio, (unsigned)zero_latency_threshold);
            viol++;
        }
    }
    if (viol > 0)
        log_printf(app_log(), LOG_ERROR, "irq",
            "irq_manager_audit_priorities: %d violation(s) found "
            "(enable RTOS_MAX_ZERO_LATENCY_IRQS only after fixing these)", viol);
    return viol;
}

const irq_mgr_entry_t *irq_manager_get(irq_id_t id)
{
    int idx = irq_hal_index(id);
    if (idx < 0 || idx >= (int)IRQ_HAL_TABLE_SIZE)
        return NULL;
    for (int i = 0; i < IRQ_MGR_POOL; i++)
        if (g_mgr[i].registered && g_mgr[i].id == id)
            return &g_mgr[i];     /* first registered on this line */
    return NULL;
}

/* ---- BENCHMARK ISOLATION ----------------------------------------------
 * Mask every enabled handler except those in keep[], so a real-time benchmark
 * sees a clean task-switch path with no external preemption. The masked-line
 * ids are returned in masked_out[] (caller-owned, kept off BSS to protect the
 * RAM-tight build); irq_manager_bench_unquiet() re-enables each via its g_mgr
 * (cb, ctx) so shared lines / ref-counts come back exactly. */
static int bench_keep_contains(irq_id_t *keep, int keep_n, irq_id_t id)
{
    for (int i = 0; i < keep_n; i++)
        if (keep[i] == id)
            return 1;
    return 0;
}

int irq_manager_bench_quiet(irq_id_t *keep, int keep_n,
                            irq_id_t *masked_out, int masked_cap)
{
    int n = 0;
    for (int i = 0; i < IRQ_MGR_POOL; i++) {
        if (!g_mgr[i].registered || !g_mgr[i].enabled)
            continue;               /* only mask live, armed handlers */
        if (bench_keep_contains(keep, keep_n, g_mgr[i].id))
            continue;               /* keep the console + tick alive */
        if (n < masked_cap) {
            masked_out[n] = g_mgr[i].id;
            n++;
        }
        irq_manager_disable(g_mgr[i].id, g_mgr[i].cb, g_mgr[i].ctx);
    }
    return n;                        /* # masked (also the count to pass to unquiet) */
}

void irq_manager_bench_unquiet(irq_id_t *masked, int masked_n)
{
    for (int i = 0; i < masked_n; i++) {
        const irq_mgr_entry_t *e = irq_manager_get(masked[i]);
        if (e)
            irq_manager_enable(masked[i], e->cb, e->ctx);
    }
}

void irq_manager_dump(void)
{
    log_printf(app_log(), LOG_DEBUG, "irq", "--- IRQ manager table ---");
    int n = 0;
    for (int i = 0; i < IRQ_MGR_POOL; i++) {
        if (!g_mgr[i].registered)
            continue;
        n++;
        log_printf(app_log(), LOG_DEBUG, "irq", "  irq %3d : reg=%d en=%d prio=%2u cls=%d cb=%s",
               (int)g_mgr[i].id,
               g_mgr[i].registered,
               g_mgr[i].enabled,
               (unsigned)g_mgr[i].prio,
               (int)g_mgr[i].cls,
               g_mgr[i].cb ? "yes" : "no");
    }
    log_printf(app_log(), LOG_DEBUG, "irq", "  (%d handler(s) attached)", n);
    log_printf(app_log(), LOG_DEBUG, "irq", "--- end IRQ manager table ---");
}
