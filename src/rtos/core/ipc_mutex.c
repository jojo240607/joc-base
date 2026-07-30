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
    m->recursive = 0;
    m->rec_count = 0;
    m->waitq = (void *)0;
    rtos_kobj_register((const char *)0, KOBJ_MUTEX, m);
}

void rtos_mutex_init_rec(rtos_mutex_t *m, uint8_t ceil_prio) {
    if (!m) return;
    m->owner = (task_t *)0;
    m->ceil_prio = ceil_prio;
    m->recursive = 1;
    m->rec_count = 0;
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

int rtos_mutex_timedlock(rtos_mutex_t *m, uint32_t timeout_ms) {
    if (!m || !rtos_is_started()) return -1;
    if (rtos_ipc_in_isr()) { g_ipc_misuse++; return -1; }   /* ISR 中不可阻塞：误用计数 */
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_MUTEX_TIMEDLOCK, (uint32_t)m, timeout_ms, 0);
    if (timeout_ms == 0) return rtos_mutex_trylock(m);      /* 等价非阻塞 */
    unsigned st = rtos_crit_enter();
    if (m->owner == rtos_running()) {                       /* 递归锁：自锁计数 +1 */
        if (m->recursive) { m->rec_count++; rtos_crit_exit(st); return 0; }
        rtos_crit_exit(st); return -1;                      /* 非递归不支持自锁 */
    }
    if (m->owner == (task_t *)0) {
        m->owner = rtos_running();
        rtos_set_eff_prio(rtos_running(), (rtos_running()->prio < m->ceil_prio)
                                      ? rtos_running()->prio : m->ceil_prio);
        rtos_crit_exit(st);
        return 0;
    }
    /* 有竞争：阻塞并挂超时（双链——互斥量 waitq + 睡眠链表计时）。
     * 防护：若此前在 sched_lock 区间内 yield 过，本任务可能残留于就绪队列，
     * 阻塞前摘除，避免同时挂在“就绪”与“等待”两条链表上破坏结构。 */
    uint32_t ticks = (timeout_ms * RTOS_TICK_HZ + 999U) / 1000U;
    if (ticks == 0) ticks = 1;
    if (g_running->state == TASK_READY) ready_remove(g_running);
    g_running->state     = TASK_BLOCKED;
    g_running->wait_obj  = &m->waitq;
    g_running->wait_armed = 1;
    g_running->timed_out  = 0;
    g_running->delay_ticks = ticks;
    rtos_waitq_add(&m->waitq, g_running);
    sleep_add(g_running);            /* 计时：随节拍递减 delay_ticks，到期置 timed_out */
    rtos_crit_exit(st);
    rtos_schedule_request();
    /* 被唤醒（handoff 拿到锁）或超时（timed_out=1）后在此继续 */
    st = rtos_crit_enter();
    int got = (m->owner == g_running);
    int to  = g_running->timed_out;
    g_running->wait_armed = 0;
    g_running->timed_out  = 0;
    rtos_crit_exit(st);
    if (got) return 0;
    if (to)  return -1;
    return -1;   /* 兜底（理论上不会到达） */
}

int rtos_mutex_lock(rtos_mutex_t *m) {
    if (!m || !rtos_is_started()) return -1;
    if (rtos_ipc_in_isr()) { g_ipc_misuse++; return -1; }   /* ISR 中不可阻塞：误用计数 */
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_MUTEX_LOCK, (uint32_t)m, 0, 0);
    unsigned st = rtos_crit_enter();
    if (m->owner == rtos_running()) {                       /* 递归锁：自锁计数 +1 */
        if (m->recursive) { m->rec_count++; rtos_crit_exit(st); return 0; }
        rtos_crit_exit(st); return -1;                      /* 非递归不支持自锁 */
    }
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
    /* 递归锁：仅当递归计数减到 0 才真正释放（TC-MTX-003） */
    if (m->recursive && m->rec_count > 0) {
        m->rec_count--;
        if (m->rec_count > 0) { rtos_crit_exit(st); return 0; }
    }
    /* 恢复自身优先级到原始值 */
    rtos_set_eff_prio(rtos_running(), rtos_running()->base_prio);
    /* handoff：唤醒最高优先级等待者并立为 owner（提升到天花板） */
    task_t *t = rtos_waitq_pop_highest(&m->waitq);
    if (t) {
        rtos_cancel_timed_wait(t);   /* 若它挂了超时睡眠项，摘除并清标记（已脱离 waitq） */
        m->owner = t;
        t->wait_obj = (void *)0;
        t->state = TASK_READY;
        /* 先按天花板设定有效优先级，再加入就绪队列：t 此刻刚脱离 waitq、尚不在
         * 就绪队列，若先调 rtos_set_eff_prio 会误判 state==READY 而执行 ready_remove，
         * 把 g_ready_head[prio]/位图清零、破坏就绪队列，导致被唤醒者不入队、系统死锁。
         * 直接改 eff 字段再 ready_add 可避免该双重操作。 */
        t->prio  = (t->prio < m->ceil_prio) ? t->prio : m->ceil_prio;
        ready_add(t);
        rtos_crit_exit(st);
        rtos_schedule_request();
    } else {
        m->owner = (task_t *)0;
        rtos_crit_exit(st);
    }
    return 0;
}
