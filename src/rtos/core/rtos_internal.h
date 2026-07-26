#ifndef JOC_RTOS_INTERNAL_H
#define JOC_RTOS_INTERNAL_H

#include "rtos.h"

/* ---------------------------------------------------------------------------
 * jOS RTOS 内部共享头（core/ 各 .c 文件共用，不暴露给应用层）
 *
 * 公开 API 见 rtos.h；此处只声明拆分到多个 .c 后需要在 core/ 内部跨文件
 * 共享的调度器全局与少量内部辅助函数。其它内部符号（ready_add / rtos_waitq_*
 * / rtos_pend / rtos_post / rtos_set_eff_prio / rtos_kobj_* 等）已在 rtos.h 声明。
 * ------------------------------------------------------------------------- */

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

#endif /* JOC_RTOS_INTERNAL_H */
