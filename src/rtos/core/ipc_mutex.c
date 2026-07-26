#include "rtos.h"
#include "rtos_internal.h"
#include "common/lock.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * 互斥量（优先级天花板协议，防优先级反转）（core/ipc_mutex.c）
 * ------------------------------------------------------------------------- */
void rtos_mutex_init(rtos_mutex_t *m, uint8_t ceil_prio) {
    if (!m) return;
    m->owner = (task_t *)0;
    m->ceil_prio = ceil_prio;
    m->waitq = (void *)0;
    rtos_kobj_register((const char *)0, KOBJ_MUTEX, m);
}

int rtos_mutex_trylock(rtos_mutex_t *m) {
    if (!m || !rtos_is_started()) return -1;
    if (rtos_ipc_in_isr()) { g_ipc_misuse++; return -1; }   /* ISR 中不可阻塞：误用计数 */
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_MUTEX_TRYLOCK, (uint32_t)m, 0, 0);
    unsigned st = rtos_crit_enter();
    if (m->owner == (task_t *)0) {
        m->owner = rtos_running();
        rtos_set_eff_prio(rtos_running(), (rtos_running()->prio < m->ceil_prio)
                                      ? rtos_running()->prio : m->ceil_prio);
        rtos_crit_exit(st);
        return 0;
    }
    rtos_crit_exit(st);
    return -1;
}

int rtos_mutex_lock(rtos_mutex_t *m) {
    if (!m || !rtos_is_started()) return -1;
    if (rtos_ipc_in_isr()) { g_ipc_misuse++; return -1; }   /* ISR 中不可阻塞：误用计数 */
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_MUTEX_LOCK, (uint32_t)m, 0, 0);
    unsigned st = rtos_crit_enter();
    if (m->owner == rtos_running()) { rtos_crit_exit(st); return -1; }   /* 不支持递归 */
    if (m->owner == (task_t *)0) {
        m->owner = rtos_running();
        rtos_set_eff_prio(rtos_running(), (rtos_running()->prio < m->ceil_prio)
                                      ? rtos_running()->prio : m->ceil_prio);
        rtos_crit_exit(st);
        return 0;
    }
    /* 有竞争：阻塞（天花板协议下持有者已在 ceil_prio 运行，反转被限界） */
    rtos_pend(&m->waitq);
    rtos_crit_exit(st);
    return 0;   /* 被唤醒即成为新 owner（unlock 的 handoff 已置 owner=本任务） */
}

int rtos_mutex_unlock(rtos_mutex_t *m) {
    if (!m || !rtos_is_started()) return -1;
    if (rtos_ipc_in_isr()) { g_ipc_misuse++; return -1; }   /* ISR 中不可解锁：误用计数 */
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_MUTEX_UNLOCK, (uint32_t)m, 0, 0);
    unsigned st = rtos_crit_enter();
    if (m->owner != rtos_running()) { rtos_crit_exit(st); return -1; }
    /* 恢复自身优先级到原始值 */
    rtos_set_eff_prio(rtos_running(), rtos_running()->base_prio);
    /* handoff：唤醒最高优先级等待者并立为 owner（提升到天花板） */
    task_t *t = rtos_waitq_pop_highest(&m->waitq);
    if (t) {
        m->owner = t;
        t->wait_obj = (void *)0;
        t->state = TASK_READY;
        rtos_set_eff_prio(t, (t->prio < m->ceil_prio) ? t->prio : m->ceil_prio);
        ready_add(t);
        rtos_crit_exit(st);
        rtos_schedule_request();
    } else {
        m->owner = (task_t *)0;
        rtos_crit_exit(st);
    }
    return 0;
}
