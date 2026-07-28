#ifndef JOC_RTOS_INTERNAL_H
#define JOC_RTOS_INTERNAL_H

#include "rtos.h"
#include "common/lock.h"

/* ---------------------------------------------------------------------------
 * jOS RTOS 内部共享头（core/ 各 .c 文件共用，不暴露给应用层）
 *
 * 公开 API 见 rtos.h；此处只声明拆分到多个 .c 后需要在 core/ 内部跨文件
 * 共享的调度器全局与少量内部辅助函数。其它内部符号（ready_add / rtos_waitq_*
 * / rtos_pend / rtos_post / rtos_set_eff_prio / rtos_kobj_* 等）已在 rtos.h 声明。
 * ------------------------------------------------------------------------- */

/* ---- 内核临界区（统一入口，见 docs/rtos-design.md §4.5） ----
 * 把“关中断”收敛到一个入口：RTOS_MAX_ZERO_LATENCY_IRQS>0 时用 BASEPRI 仅屏蔽
 * 优先级 >= 阈值的中断（更高优先级的零延迟 ISR 仍可达），否则退化为 PRIMASK
 * 全局关中断（与此前行为完全一致）。两个分支返回类型相同（unsigned），调用方
 * 用 `unsigned st = rtos_crit_enter(); ... rtos_crit_exit(st);` 即可，无需感知分支。 */
#if RTOS_MAX_ZERO_LATENCY_IRQS > 0
/* BASEPRI 临界区原语——设计对标 FreeRTOS 的 portSET/CLEAR_INTERRUPT_MASK_FROM_ISR：
 *   enter：先 `mrs` 读出【当前】BASEPRI 存进局部变量 saved（保存掩码），
 *          再把 BASEPRI 提升到阈值（屏蔽优先级 >= 阈值的异常）；
 *   exit ：直接 `msr BASEPRI, saved` 把【当初保存的那个值】原样恢复，绝不无条件清零。
 * 关键：嵌套临界区（如 SVC/优先级更高异常夹在中间）里，内层 exit 只恢复到外层的值，
 * 不会破坏外层屏蔽状态；否则临界区会在嵌套时失效，就绪/等待链表被并发改写、调度器损坏。
 *
 * BASEPRI 写入的是“已左移 (8-__NVIC_PRIO_BITS) 位的硬件优先级值”；NVIC_SetPriority
 * 内部会再移位一次，故这里不走 NVIC_SetPriority，直接算好移位值写入。 */
static inline unsigned rtos_crit_enter(void) {
    uint32_t saved;
    __asm__ volatile("mrs %0, BASEPRI" : "=r"(saved));
    __asm__ volatile("msr BASEPRI, %0" :
                     : "r"((uint32_t)(RTOS_MAX_ZERO_LATENCY_IRQS
                                      << (8U - __NVIC_PRIO_BITS)))
                     : "memory");
    return (unsigned)saved;            /* 保存原掩码，exit 时原样恢复 */
}
static inline void rtos_crit_exit(unsigned st) {
    __asm__ volatile("msr BASEPRI, %0" : : "r"((uint32_t)st) : "memory");
}
#else
static inline unsigned rtos_crit_enter(void) {
    return irq_lock();                                 /* PRIMASK 全局关中断（保存 PRIMASK） */
}
static inline void rtos_crit_exit(unsigned st) {
    irq_unlock(st);                                    /* 从保存的 PRIMASK 值恢复，不直接开全局中断 */
}
#endif

/* 就绪队列级数（与 rtos_config.h 的 RTOS_MAX_PRIORITIES 一致） */
#ifndef PRIO_LEVELS
  #define PRIO_LEVELS RTOS_MAX_PRIORITIES
#endif

/* 就绪队列 pick / remove（sched.c 定义；task.c 的 rtos_start 使用） */
task_t *ready_pick(void);
void    ready_remove(task_t *t);
/* 睡眠链表加入（sched.c 定义；ipc_mutex.c 的 rtos_mutex_timedlock 用于挂计时项） */
void    sleep_add(task_t *t);

/* 把任务从它当前所在的队列（就绪/睡眠/等待，含计时阻塞双链）摘除（sched.c）。 */
void rtos_task_unlink(task_t *t);
/* 取消任务的计时阻塞（mutex handoff 在超时前拿到锁时调用）：仅当 wait_armed==1
 * 时摘除其睡眠链表条目并清标记，避免误删未计时的任务。 */
void rtos_cancel_timed_wait(task_t *t);

/* IPC 内部：判断“真 ISR（排除 SVC 重入）”（ipc_sem.c 定义；各 ipc_*.c 共用） */
int rtos_ipc_in_isr(void);

/* 调度器全局（sched.c 定义；task.c / syscalls.c 引用） */
extern task_t          *g_running;
extern volatile uint32_t g_tick;
extern int             g_rtos_started;
/* g_in_svc 已在 rtos.h 声明（extern volatile int g_in_svc;） */

/* 软件定时器（core/timer.c，docs/rtos-test-plan.md §6.3）：
 *  - rtos_timer_tick 由 rtos_tick_isr 调用（ISR 上下文，已处于临界区），
 *    扫描活动定时器、到期者置 pending 并唤醒定时器任务；
 *  - rtos_timer_init_daemon 由 rtos_start 调用，创建专用定时器任务（任务上下文，
 *    切到首个任务之前），绝不在 ISR 里懒建任务。 */
void rtos_timer_tick(void);
void rtos_timer_init_daemon(void);
/* 处理所有 pending 定时器（执行回调于任务上下文）。定时器任务体与确定性自测共用：
 * 自测在 irq_lock 窗口内冻结真实 sysTick 后直接调用，脱离 1kHz 节拍做快速断言。 */
void rtos_timer_run_pending(void);

/* 任务计数（sched.c 定义并在 rtos_init 中复位；task.c 创建/查询用） */
extern int g_task_count;

/* IPC 误用计数（§4.2）：当阻塞式 IPC 在“不应阻塞”的上下文（ISR / 内核未启动）
 * 被调用而退化为忙等/非阻塞时累加，便于事后定位误用（行为不变）。sched.c 定义。 */
extern volatile uint32_t g_ipc_misuse;

#endif /* JOC_RTOS_INTERNAL_H */
