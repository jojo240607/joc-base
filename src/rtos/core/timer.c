#include "rtos.h"
#include "core/rtos_internal.h"
#include "log/log.h"
#include "log/app_log.h"
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 软件定时器（core/timer.c，docs/rtos-test-plan.md §6.3，准则 rtos-test.md §2.5）
 *
 * 设计：
 *  - 用户分配 rtos_timer_t（静态/栈），rtos_timer_init 清零后由 start/stop 挂入
 *    内核“活动定时器链表”。链表操作在任务/ISR 上下文均经 rtos_crit_enter 保护
 *    （与 sysTick ISR 的 BASEPRI 临界区互斥，故不会并发改写链表）。
 *  - 过期判定用【无符号 tick 比较】rtos_tick_expired(expire, now)
 *    = (int32_t)(now - expire) >= 0，因此 32 位 g_tick 在 0xFFFFFFFF→0 翻转后
 *    仍可正确判定唤醒点（48 天翻转安全）。
 *  - 节拍 ISR（rtos_tick_isr -> rtos_timer_tick）扫描活动定时器：到期者置
 *    pending 并唤醒“定时器任务”；定时器任务（专用任务上下文，非 ISR）执行回调，
 *    周期定时器在回调前把 expire += period（锚定原始相位，无累积漂移）。
 *  - 定时器任务在 rtos_start 里一次性创建（rtos_timer_init_daemon），绝不从
 *    ISR 懒建任务——与 workqueue 同款约束（见 docs/rtos-design.md §4.6 / 记忆 77847432）。
 * ------------------------------------------------------------------------- */

/* 活动定时器单链表（head 在本文件内部） */
static rtos_timer_t *g_timer_head;
static rtos_sem_t    g_timer_sem;          /* 定时器任务等待的信号量 */
RTOS_TASK_STACK(g_timer_stack, 1024);      /* 定时器任务栈（回调运行于此） */

/* 把 t 从活动链表摘除（调用方持锁） */
static void timer_unlink(rtos_timer_t *t) {
    rtos_timer_t **pp = &g_timer_head;
    while (*pp) {
        if (*pp == t) { *pp = t->next; break; }
        pp = &(*pp)->next;
    }
}

/* 由 rtos_tick_isr 调用（ISR 上下文，已处于临界区）：扫描活动定时器，
 * 到期者置 pending 并唤醒定时器任务。 */
void rtos_timer_tick(void) {
    unsigned st = rtos_crit_enter();
    int wake = 0;
    for (rtos_timer_t *t = g_timer_head; t; t = t->next) {
        if (t->active && !t->pending && rtos_tick_expired(t->expire, g_tick)) {
            t->pending = 1;
            wake = 1;
        }
    }
    rtos_crit_exit(st);
    if (wake) rtos_sem_give(&g_timer_sem);   /* ISR 安全：唤醒定时器任务 */
}

/* 处理所有 pending 定时器（执行回调于任务上下文）。定时器任务体与此函数共用，
 * 便于确定性自测在 irq_lock 窗口内直接驱动（见 rtos_timer_selftest）。 */
void rtos_timer_run_pending(void) {
    for (;;) {
        rtos_timer_t *t = (rtos_timer_t *)0;
        unsigned st = rtos_crit_enter();
        for (rtos_timer_t *p = g_timer_head; p; p = p->next) {
            if (p->pending) { t = p; break; }
        }
        if (t) {
            t->pending = 0;
            if (t->mode == RTOS_TIMER_ONESHOT) {
                t->active = 0;
                timer_unlink(t);             /* 单次：到期后移出活动链表 */
            } else {
                /* 锚定原始相位 + period（翻转安全加法），无累积漂移 */
                t->expire = (uint32_t)(t->expire + t->period_ticks);
            }
        }
        rtos_crit_exit(st);
        if (!t) break;
        if (t->cb) t->cb(t, t->arg);         /* 回调在任务上下文执行（持有栈） */
    }
}

/* 专用定时器任务：被唤醒后处理所有 pending 定时器。 */
static void rtos_timer_daemon(void *arg) {
    (void)arg;
    for (;;) {
        rtos_sem_wait(&g_timer_sem);
        rtos_timer_run_pending();
    }
}

void rtos_timer_init_daemon(void) {
    rtos_sem_init(&g_timer_sem, 0, 1);
    rtos_task_create("rtos_timer", rtos_timer_daemon, (void *)0,
                     RTOS_PRIO_TIMER, g_timer_stack, sizeof(g_timer_stack));
}

void rtos_timer_init(rtos_timer_t *t, const char *name,
                     rtos_timer_cb_t cb, void *arg) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->name = name; t->cb = cb; t->arg = arg;
}

void rtos_timer_start_ticks(rtos_timer_t *t, rtos_timer_mode_t mode,
                            uint32_t period_ticks) {
    if (!t || period_ticks == 0) return;
    unsigned st = rtos_crit_enter();
    t->mode = mode;
    t->period_ticks = period_ticks;
    t->expire = (uint32_t)(g_tick + period_ticks);  /* 翻转安全加法 */
    t->pending = 0;
    if (!t->active) { t->active = 1; t->next = g_timer_head; g_timer_head = t; }
    rtos_crit_exit(st);
}

void rtos_timer_start(rtos_timer_t *t, rtos_timer_mode_t mode, uint32_t period_ms) {
    uint32_t ticks = (period_ms * RTOS_TICK_HZ + 999U) / 1000U;
    if (ticks == 0) ticks = 1;
    rtos_timer_start_ticks(t, mode, ticks);
}

void rtos_timer_stop(rtos_timer_t *t) {
    if (!t) return;
    unsigned st = rtos_crit_enter();
    if (t->active) { t->active = 0; t->pending = 0; timer_unlink(t); }
    rtos_crit_exit(st);
}

int rtos_timer_is_active(rtos_timer_t *t) {
    return (t && t->active) ? 1 : 0;
}
