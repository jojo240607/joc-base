#include "rtos.h"
#include "rtos_internal.h"
#include "common/lock.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * 信号量（core/ipc_sem.c）
 *
 * 阻塞 API 仅可在任务上下文调用；ISR 或内核未启动时的“阻塞”调用退化为
 * 非阻塞/忙等（见各函数头部的 fallback），以保证启动期与中断安全。
 * ------------------------------------------------------------------------- */

/* 是否在中断上下文：透过 common/lock.h 的 arch_in_isr() 探测，
 * 不直接读写任何 ISA 魔法地址（0xE000ED04），保持核心可移植。
 * 注意：SVC 分发（g_in_svc==1）代表特权 Handler 模式“代替某任务”执行内核
 * 调用，逻辑上仍属任务上下文——此刻若按 ISR 处理，阻塞式 API 会误走忙等/
 * 直接返回分支，使非特权任务经 SVC 门时死循环（rtos_sem_wait）或静默失败
 * （rtos_mutex_* 直接 -1）。故排除该态，仅“真 ISR + 非 SVC”才算 ISR。
 * 本函数供所有 ipc_*.c 共用（声明见 core/rtos_internal.h）。 */
int rtos_ipc_in_isr(void) {
    return arch_in_isr() && !g_in_svc;
}

void rtos_sem_init(rtos_sem_t *s, uint32_t initial, uint32_t limit) {
    if (!s) return;
    if (limit == 0) limit = 1;
    if (initial > limit) initial = limit;
    s->count = initial;
    s->limit = limit;
    s->waitq = (void *)0;
    rtos_kobj_register((const char *)0, KOBJ_SEM, s);
}

int rtos_sem_trywait(rtos_sem_t *s) {
    if (!s) return -1;
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_SEM_TRYWAIT, (uint32_t)s, 0, 0);
    unsigned st = irq_lock();
    if (s->count > 0) { s->count--; irq_unlock(st); return 0; }
    irq_unlock(st);
    return -1;
}

int rtos_sem_wait(rtos_sem_t *s) {
    if (!s) return -1;
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_SEM_WAIT, (uint32_t)s, 0, 0);
    /* ISR / 未启动：退化为忙等 */
    if (rtos_ipc_in_isr() || !rtos_is_started()) {
        for (;;) {
            unsigned st = irq_lock();
            if (s->count > 0) { s->count--; irq_unlock(st); return 0; }
            irq_unlock(st);
        }
    }
    unsigned st = irq_lock();
    if (s->count > 0) { s->count--; irq_unlock(st); return 0; }
    rtos_pend(&s->waitq);     /* 无许可：阻塞；被唤醒时许可已被本任务“占有” */
    irq_unlock(st);
    return 0;
}

void rtos_sem_give(rtos_sem_t *s) {
    if (!s) return;
    if (rtos_need_svc()) { rtos_syscall(RTOS_SYS_SEM_GIVE, (uint32_t)s, 0, 0); return; }
    unsigned st = irq_lock();
    task_t *t = (task_t *)s->waitq;
    if (t) {
        /* 有等待者：直接唤醒最高优先级者（不增 count，唤醒即“消费”） */
        t = rtos_waitq_pop_highest(&s->waitq);
        t->wait_obj = (void *)0;
        t->state = TASK_READY;
        ready_add(t);
    } else if (s->count < s->limit) {
        s->count++;
    }
    irq_unlock(st);
    if (t) rtos_schedule_request();
}
