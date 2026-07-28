#include "rtos.h"
#include "rtos_internal.h"
#include "common/lock.h"
#include "irq.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 调度核心（core/sched.c）：TCB 全局、就绪位图、睡眠链表、等待队列、
 * 调度切换、节拍、pend/post、有效优先级调整。
 *
 * 所有链表修改都假定调用方已处于临界区（rtos_crit_enter 的 PRIMASK 或 BASEPRI
 * 屏蔽），因此本文件内不另加锁；尤其 rtos_pendsv_switch 内部已用 rtos_crit_enter
 * 包住整段切换，故 PendSV 汇编不再需要 cpsid i。对外 API（rtos_yield/rtos_msleep/
 * rtos_pend/post）
 * 在调用修改链表的逻辑前自行 irq_lock。
 *
 * 任务生命周期 / TCB 池 / rtos_start 见 core/task.c；
 * SVC 系统调用分发见 core/syscalls.c。
 * ------------------------------------------------------------------------- */

/* ---- 全局状态 ---- */
task_t *g_running = (task_t *)0;
volatile uint32_t g_tick = 0;
int g_rtos_started = 0;
volatile int g_in_svc = 0;   /* SVC 分发进行中：防止非特权任务路径递归触发 SVC */

/* ---- 就绪队列：每优先级 FIFO + 位图 ---- */
static task_t *g_ready_head[PRIO_LEVELS];
static task_t *g_ready_tail[PRIO_LEVELS];
static uint32_t g_ready_bmp;

/* ---- 睡眠链表（按 tick 递减） ---- */
static task_t *g_sleep_head;

/* ---- 任务计数（TCB 池定义在 core/task.c） ---- */
int g_task_count = 0;

/* ---- IPC 误用计数（见 docs/rtos-design.md §4.2） ---- */
volatile uint32_t g_ipc_misuse = 0;

/* ---- 时间片轮转（Round-Robin，见 §1/§3）：当前运行任务剩余节拍数 ---- */
#if RTOS_TIME_SLICE
static uint32_t g_slice_ticks = RTOS_TIME_SLICE_TICKS;
#endif

/* ---- 就绪链表操作（调用方持锁） ---- */
void ready_add(task_t *t) {
    int p = t->prio;
    t->sched_prev = g_ready_tail[p];
    t->sched_next = (task_t *)0;
    if (g_ready_tail[p]) g_ready_tail[p]->sched_next = t;
    else                 g_ready_head[p] = t;
    g_ready_tail[p] = t;
    g_ready_bmp |= (1u << p);
}
void ready_remove(task_t *t) {
    int p = t->prio;
    if (t->sched_prev) t->sched_prev->sched_next = t->sched_next;
    else               g_ready_head[p] = t->sched_next;
    if (t->sched_next) t->sched_next->sched_prev = t->sched_prev;
    else               g_ready_tail[p] = t->sched_prev;
    if (!g_ready_head[p]) g_ready_bmp &= ~(1u << p);
    t->sched_next = t->sched_prev = (task_t *)0;
}
task_t *ready_pick(void) {
    if (!g_ready_bmp) return (task_t *)0;
    int p = __builtin_ctz(g_ready_bmp);   /* 最低置位 = 最高优先级 */
    return g_ready_head[p];
}

/* ---- 睡眠链表操作（调用方持锁） ---- */
static void sleep_add(task_t *t) {
    t->sched_prev = (task_t *)0;
    t->sched_next = g_sleep_head;
    if (g_sleep_head) g_sleep_head->sched_prev = t;
    g_sleep_head = t;
}
static void sleep_remove(task_t *t) {
    if (t->sched_prev) t->sched_prev->sched_next = t->sched_next;
    else               g_sleep_head = t->sched_next;
    if (t->sched_next) t->sched_next->sched_prev = t->sched_prev;
    t->sched_next = t->sched_prev = (task_t *)0;
}

/* ---- 等待队列操作（调用方持锁） ---- */
void rtos_waitq_add(void **head, task_t *t) {
    task_t *h = (task_t *)(*head);
    t->wait_prev = (task_t *)0;
    if (!h) { *head = (void *)t; t->wait_next = (task_t *)0; return; }
    task_t *p = h;
    while (p->wait_next) p = p->wait_next;
    p->wait_next = t;
    t->wait_prev = p;
    t->wait_next = (task_t *)0;
}
void rtos_waitq_remove(void **head, task_t *t) {
    if (t->wait_prev) t->wait_prev->wait_next = t->wait_next;
    else              *head = (void *)t->wait_next;
    if (t->wait_next) t->wait_next->wait_prev = t->wait_prev;
    t->wait_next = t->wait_prev = (task_t *)0;
}
task_t *rtos_waitq_pop_highest(void **head) {
    task_t *best = (task_t *)0, *bestprev = (task_t *)0;
    task_t *prev = (task_t *)0, *cur = (task_t *)(*head);
    while (cur) {
        if (!best || cur->prio < best->prio) { best = cur; bestprev = prev; }
        prev = cur; cur = cur->wait_next;
    }
    if (!best) return (task_t *)0;
    if (bestprev) bestprev->wait_next = best->wait_next;
    else          *head = (void *)best->wait_next;
    best->wait_next = (task_t *)0;
    return best;
}

/* ---- 生命周期：内核初始化（仅复位调度器表 + 注册节拍） ---- */
void rtos_init(void) {
    g_running = (task_t *)0;
    g_tick = 0;
    g_rtos_started = 0;
    g_ready_bmp = 0;
    for (int i = 0; i < PRIO_LEVELS; i++) {
        g_ready_head[i] = g_ready_tail[i] = (task_t *)0;
    }
    g_sleep_head = (task_t *)0;
    g_task_count = 0;
#if RTOS_TIME_SLICE
    g_slice_ticks = RTOS_TIME_SLICE_TICKS;
#endif
    /* 在 systick 共享线上注册 RTOS 节拍（与 systick 驱动 ISR 并存）。
     * 节拍中断 id 由 arch 层给出，核心不直接依赖任何芯片 HAL。 */
    irq_register(rtos_arch_tick_id(), rtos_tick_isr, (void *)0);
}

void rtos_yield(void) {
    if (!g_rtos_started) return;
    if (rtos_need_svc()) { rtos_syscall(RTOS_SYS_YIELD, 0, 0, 0); return; }
    unsigned st = rtos_crit_enter();
    if (g_running && g_running->state == TASK_RUNNING) {
        g_running->state = TASK_READY;
        ready_add(g_running);
    }
    rtos_crit_exit(st);
    rtos_schedule_request();
}

void rtos_msleep(uint32_t ms) {
    if (!g_rtos_started) { for (volatile uint32_t i = 0; i < ms * 1000UL; i++) { } return; }
    if (rtos_need_svc()) { rtos_syscall(RTOS_SYS_MSLEEP, ms, 0, 0); return; }
    uint32_t ticks = (ms * RTOS_TICK_HZ + 999U) / 1000U;
    if (ticks == 0) ticks = 1;
    unsigned st = rtos_crit_enter();
    /* 防护：若此前在 sched_lock(屏蔽 PendSV) 区间内调用过 rtos_yield，本任务可能已被
     * 加入就绪队列(state==READY)而切换未发生；睡眠前必须先将其从就绪队列摘除，否则会
     * 同时挂在“就绪”与“睡眠”两条链表上，破坏链表（节拍 ISR 遍历时死循环/越界）。
     * 正常运行态下本任务为 RUNNING，不会命中此分支，零回归。 */
    if (g_running->state == TASK_READY) ready_remove(g_running);
    g_running->state = TASK_SLEEPING;
    g_running->delay_ticks = ticks;
    sleep_add(g_running);
    rtos_crit_exit(st);
    rtos_schedule_request();
}

/* 抢占点（docs/rtos-design.md §3）：仅当存在更高（或同优先级 FIFO 中更靠前）
 * 的就绪任务时才让出 CPU；否则继续当前任务。常用于“临界区内插入调度点”。 */
void rtos_schedule(void) {
    if (!g_rtos_started) return;
    if (rtos_need_svc()) { rtos_syscall(RTOS_SYS_YIELD, 0, 0, 0); return; }
    unsigned st = rtos_crit_enter();
    if (g_running && g_running->state == TASK_RUNNING) {
        int p = g_running->prio;
        /* 更高优先级有就绪，或同优先级 FIFO 里还有别的任务在前面 */
        if ((g_ready_bmp & ~((1u << (p + 1)) - 1u)) || (g_ready_head[p] != (task_t *)0)) {
            g_running->state = TASK_READY;
            ready_add(g_running);
        }
    }
    rtos_crit_exit(st);
    rtos_schedule_request();
}

uint32_t rtos_ipc_misuse_count(void) { return g_ipc_misuse; }

/* 由 PendSV 汇编调用（中断已关）：保存 old_sp，挑选下一任务，返回其 sp */
void *rtos_pendsv_switch(void *old_sp) {
    /* 临界区：保护就绪/等待链表不被“调用了内核 API 的更高优先级 ISR”并发改写。
     * 用统一入口 rtos_crit_enter/exit（见 rtos_internal.h）：
     *   RTOS_MAX_ZERO_LATENCY_IRQS>0 时 BASEPRI 仅屏蔽优先级>=阈值(内核)的异常，
     *     零延迟 ISR 在切换窗口内仍可达（FreeRTOS/Zephyr 式选择性屏蔽，降低中断抖动）；
     *   否则退化为 PRIMASK 全局关中断，与原 PendSV 内 cpsid i 行为一致、零回归。
     * 因此 context.S 的 PendSV_Handler 不再需要 cpsid i/cpsie i。 */
    unsigned st = rtos_crit_enter();
    task_t *cur = g_running;
    if (cur) {
        cur->sp = old_sp;
        if (rtos_stack_check_sentinel(cur)) {   /* 栈溢出检测（MPU 辅助） */
            g_stack_overflow = 1;
        }
        if (cur->state == TASK_RUNNING) {   /* 被抢占：回到就绪队列 */
            cur->state = TASK_READY;
            ready_add(cur);
        }
    }
    task_t *nxt = ready_pick();
    if (!nxt) nxt = cur;                    /* 无其它就绪：继续当前（idle 保证不会空） */
    if (nxt) {
        ready_remove(nxt);
        nxt->state = TASK_RUNNING;
        g_running = nxt;
    }
    rtos_crit_exit(st);
    return nxt ? nxt->sp : old_sp;
}

/* 由 systick 共享线调用（可能运行在 ISR 上下文） */
void rtos_tick_isr(void *ctx) {
    (void)ctx;
    if (!g_rtos_started) return;
    unsigned st = rtos_crit_enter();
    g_tick++;
    int awoke = 0;
    task_t *t = g_sleep_head;
    while (t) {
        task_t *nx = t->sched_next;
        if (t->delay_ticks > 0) {
            if (--t->delay_ticks == 0) {
                sleep_remove(t);
                t->state = TASK_READY;
                ready_add(t);
                awoke = 1;
            }
        }
        t = nx;
    }
#if RTOS_TIME_SLICE
    /* 时间片轮转：同优先级有竞争者时倒计时，用尽则让出到 FIFO 尾部 */
    if (g_running && g_running->state == TASK_RUNNING) {
        uint8_t p = g_running->prio;
        if (g_ready_head[p] != (task_t *)0) {
            if (g_slice_ticks == 0) g_slice_ticks = RTOS_TIME_SLICE_TICKS;
            if (--g_slice_ticks == 0) {
                g_running->state = TASK_READY;
                ready_add(g_running);   /* 加到同优先级 FIFO 尾部 */
                awoke = 1;
            }
        } else {
            g_slice_ticks = RTOS_TIME_SLICE_TICKS;   /* 无竞争者：续跑并重置片 */
        }
    }
#endif
    rtos_crit_exit(st);
    if (awoke) rtos_schedule_request();
}

/* 阻塞当前任务 / 唤醒最高等待者（调用方须持调度锁/关中断） */
void rtos_pend(void **q) {
    if (!g_running) return;
    /* 同 rtos_msleep 的防护：sched_lock 区间内 yield 后本任务可能残留于就绪队列，
     * 阻塞前摘除，避免同时挂在“就绪”与“等待”链表。 */
    if (g_running->state == TASK_READY) ready_remove(g_running);
    g_running->state = TASK_BLOCKED;
    g_running->wait_obj = q;
    rtos_waitq_add(q, g_running);
    rtos_schedule_request();
}
void rtos_post(void **q) {
    task_t *t = rtos_waitq_pop_highest(q);
    if (t) {
        t->wait_obj = (void *)0;
        t->state = TASK_READY;
        ready_add(t);
    }
    rtos_schedule_request();
}

/* 修改任务的有效优先级，并在其位于就绪队列时重排（互斥量提升/恢复用）。
 * 调用方须持调度锁。RUNNING/BLOCKED 任务不在就绪队列中，直接改 prio 即可。 */
void rtos_set_eff_prio(task_t *t, uint8_t new_prio) {
    if (!t || t->prio == new_prio) return;
    if (t->state == TASK_READY) {
        ready_remove(t);
        t->prio = new_prio;
        ready_add(t);
    } else {
        t->prio = new_prio;
    }
}
