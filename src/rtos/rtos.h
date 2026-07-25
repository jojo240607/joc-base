#ifndef JOC_RTOS_H
#define JOC_RTOS_H

#include <stddef.h>
#include <stdint.h>
#include "rtos_config.h"

/* ---------------------------------------------------------------------------
 * jOS RTOS 公开 API
 *
 * 调度模型：抢占式固定优先级（0=最高），就绪队列位图 O(1) 选最高就绪任务；
 * 上下文切换走 PendSV（最低优先级异常）；时基来自 systick（1 kHz）。
 *
 * 任务使用 PSP（进程栈），内核/ISR 使用 MSP，天然隔离。
 * 所有阻塞原语（osal_sem / rtos_msleep）都通过 PendSV 让出 CPU，不忙等。
 * ------------------------------------------------------------------------- */

typedef enum {
    TASK_READY    = 0,
    TASK_RUNNING  = 1,
    TASK_BLOCKED  = 2,   /* 阻塞在某个 IPC 对象上 */
    TASK_SLEEPING = 3,   /* 阻塞在延时上 */
    TASK_DEAD     = 4
} task_state_t;

typedef struct task task_t;
struct task {
    /* sp 必须放在 offset 0：SVC_Handler 汇编里 ldr r1,[r0] 直接取 sp，
     * 不能依赖字段名推导偏移，故放在最前。 */
    void          *sp;          /* 当前栈指针（指向已保存的 r4 上下文） */
    const char    *name;
    uint8_t        prio;
    task_state_t   state;
    uint8_t       *stack_base;
    size_t         stack_size;
    void         (*entry)(void *);
    void          *arg;
    /* 就绪/阻塞/睡眠链表（侵入式双向链表，复用 sched_next/prev） */
    task_t        *sched_next, *sched_prev;
    /* 阻塞在某对象上的等待链表（单向） */
    task_t        *wait_next, *wait_prev;
    /* 延时剩余 tick（仅 SLEEPING 使用） */
    uint32_t       delay_ticks;
    uint32_t       runtime;     /* 累计运行 tick（统计用） */
    void          *wait_obj;    /* 阻塞在哪个对象上（调试用） */
};

/* ---- 内核生命周期 ---- */
void rtos_init(void);   /* 初始化内部表 + 在 systick 线上注册 RTOS 节拍 */
void rtos_task_create(const char *name, void (*entry)(void *), void *arg,
                      uint8_t prio, void *stack, size_t stack_size);
void rtos_start(void);  /* 选取首个任务并切换到任务模式（不再返回到原线程） */

/* ---- 任务主动让出 / 延时 ---- */
void rtos_yield(void);
void rtos_msleep(uint32_t ms);

/* ---- 查询 ---- */
task_t     *rtos_running(void);
uint32_t    rtos_tick_count(void);
int         rtos_is_started(void);
int         rtos_task_count(void);
const char *rtos_task_name(int i);
uint8_t     rtos_task_prio(int i);
task_state_t rtos_task_state(int i);

/* ---- 内部：等待队列（供 osal_rtos.c 复用） ---- */
void  rtos_waitq_add(void **head, task_t *t);
task_t *rtos_waitq_pop_highest(void **head);
/* 阻塞/唤醒当前任务（调用方须持调度锁/关中断） */
void  rtos_pend(void **waitq_head);
void  rtos_post(void **waitq_head);

/* ---- 内部：arch 层实现 ---- */
void rtos_arch_start(void);          /* 配置 FPU/PendSV 优先级并触发首次切换 */
void rtos_schedule_request(void);    /* 置 PENDSVSET，请求上下文切换 */
void *rtos_pendsv_switch(void *old_sp); /* 由 PendSV 汇编调用，返回新 sp */
void  rtos_tick_isr(void *ctx);        /* 由 systick 共享线调用 */

#endif /* JOC_RTOS_H */
