#include "rtos.h"
#include "rtos_internal.h"
#include "common/lock.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * 事件标志（32 位，ANY/ALL 等待）（core/ipc_event.c）
 * ------------------------------------------------------------------------- */
void rtos_event_init(rtos_event_t *e) {
    if (!e) return;
    e->flags = 0;
    e->waitq = (void *)0;
    rtos_kobj_register((const char *)0, KOBJ_EVENT, e);
}
void rtos_event_clear(rtos_event_t *e, uint32_t bits) {
    if (!e) return;
    unsigned st = rtos_crit_enter();
    e->flags &= ~bits;
    rtos_crit_exit(st);
}
void rtos_event_set(rtos_event_t *e, uint32_t bits) {
    if (!e) return;
    if (rtos_need_svc()) { rtos_syscall(RTOS_SYS_EVENT_SET, (uint32_t)e, bits, 0); return; }
    unsigned st = rtos_crit_enter();
    e->flags |= bits;
    int awoke = 0;
    task_t *t = (task_t *)e->waitq;
    while (t) {
        task_t *nx = t->wait_next;
        int sat = t->wait_mode
                  ? ((e->flags & t->wait_mask) == t->wait_mask)   /* ALL */
                  : ((e->flags & t->wait_mask) != 0);            /* ANY */
        if (sat) {
            rtos_waitq_remove(&e->waitq, t);
            t->wait_obj = (void *)0; t->wait_mask = 0; t->wait_mode = 0;
            t->state = TASK_READY; ready_add(t); awoke = 1;
        }
        t = nx;
    }
    rtos_crit_exit(st);
    if (awoke) rtos_schedule_request();
}

uint32_t rtos_event_wait(rtos_event_t *e, uint32_t mask, int wait_all, int block) {
    if (!e || rtos_ipc_in_isr() || !rtos_is_started()) return e ? e->flags : 0;
    if (rtos_need_svc()) {
        uint32_t flags = ((wait_all ? 1u : 0u) | ((block ? 1u : 0u) << 1));
        return rtos_syscall(RTOS_SYS_EVENT_WAIT, (uint32_t)e, mask, flags);
    }
    unsigned st = rtos_crit_enter();
    int sat = wait_all ? ((e->flags & mask) == mask) : ((e->flags & mask) != 0);
    if (sat) { rtos_crit_exit(st); return e->flags; }
    if (!block) { rtos_crit_exit(st); return (uint32_t)-1; }
    rtos_running()->wait_mask = mask;
    rtos_running()->wait_mode = wait_all ? 1 : 0;
    rtos_pend(&e->waitq);
    rtos_crit_exit(st);
    rtos_running()->wait_mask = 0; rtos_running()->wait_mode = 0;
    return e->flags;   /* 被唤醒即条件满足 */
}
