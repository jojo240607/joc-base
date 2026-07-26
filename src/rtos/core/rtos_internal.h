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
static inline unsigned rtos_crit_enter(void) {
    sched_lock((uint8_t)RTOS_MAX_ZERO_LATENCY_IRQS);   /* BASEPRI = 阈值 */
    return 0;
}
static inline void rtos_crit_exit(unsigned st) {
    (void)st;
    sched_unlock();
}
#else
static inline unsigned rtos_crit_enter(void) {
    return irq_lock();                                 /* PRIMASK 全局关中断 */
}
static inline void rtos_crit_exit(unsigned st) {
    irq_unlock(st);
}
#endif

/* 就绪队列级数（与 rtos_config.h 的 RTOS_MAX_PRIORITIES 一致） */
#ifndef PRIO_LEVELS
  #define PRIO_LEVELS RTOS_MAX_PRIORITIES
#endif

/* 就绪队列 pick / remove（sched.c 定义；task.c 的 rtos_start 使用） */
task_t *ready_pick(void);
void    ready_remove(task_t *t);

/* IPC 内部：判断“真 ISR（排除 SVC 重入）”（ipc_sem.c 定义；各 ipc_*.c 共用） */
int rtos_ipc_in_isr(void);

/* 调度器全局（sched.c 定义；task.c / syscalls.c 引用） */
extern task_t          *g_running;
extern volatile uint32_t g_tick;
extern int             g_rtos_started;
/* g_in_svc 已在 rtos.h 声明（extern volatile int g_in_svc;） */

/* 任务计数（sched.c 定义并在 rtos_init 中复位；task.c 创建/查询用） */
extern int g_task_count;

/* IPC 误用计数（§4.2）：当阻塞式 IPC 在“不应阻塞”的上下文（ISR / 内核未启动）
 * 被调用而退化为忙等/非阻塞时累加，便于事后定位误用（行为不变）。sched.c 定义。 */
extern volatile uint32_t g_ipc_misuse;

#endif /* JOC_RTOS_INTERNAL_H */
