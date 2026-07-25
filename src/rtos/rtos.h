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
 * 所有阻塞原语（osal_sem / rtos_msleep / IPC）都通过 PendSV 让出 CPU，不忙等。
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
    uint8_t        prio;        /* 有效优先级（可能被互斥量提升） */
    uint8_t        base_prio;   /* 创建时的原始优先级（解锁/恢复用） */
    task_state_t   state;
    uint8_t       *stack_base;
    size_t         stack_size;
    void         (*entry)(void *);
    void          *arg;
    /* 就绪/阻塞/睡眠链表（侵入式双向链表，复用 sched_next/prev） */
    task_t        *sched_next, *sched_prev;
    /* 阻塞在某对象上的等待链表（双向，复用 wait_next/prev） */
    task_t        *wait_next, *wait_prev;
    /* 延时剩余 tick（仅 SLEEPING 使用） */
    uint32_t       delay_ticks;
    uint32_t       runtime;     /* 累计运行 tick（统计用） */
    void          *wait_obj;    /* 阻塞在哪个对象上（调试用） */
    /* 事件标志等待条件（仅 event 使用，任务同时只等一个对象） */
    uint32_t       wait_mask;
    uint8_t        wait_mode;   /* 1=ALL(与), 0=ANY(或) */
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

/* ---- 内部：等待队列（供 osal_rtos.c / rtos_ipc.c 复用） ---- */
void  rtos_waitq_add(void **head, task_t *t);
task_t *rtos_waitq_pop_highest(void **head);
void  rtos_waitq_remove(void **head, task_t *t);
void  ready_add(task_t *t);   /* 把任务加入就绪队列（IPC 唤醒路径复用） */
/* 修改任务有效优先级并重排就绪队列（互斥量优先级提升/恢复用） */
void  rtos_set_eff_prio(task_t *t, uint8_t new_prio);

/* ---- 内部：阻塞当前任务 / 唤醒最高等待者（调用方须持调度锁/关中断） ---- */
void  rtos_pend(void **waitq_head);
void  rtos_post(void **waitq_head);

/* ---- 内部：arch 层实现 ---- */
void rtos_arch_start(void);          /* 配置 FPU/PendSV 优先级并触发首次切换 */
void rtos_schedule_request(void);    /* 置 PENDSVSET，请求上下文切换 */
void *rtos_pendsv_switch(void *old_sp); /* 由 PendSV 汇编调用，返回新 sp */
void  rtos_tick_isr(void *ctx);        /* 由 systick 共享线调用 */

/* ===========================================================================
 * IPC 原语
 * 所有阻塞 API 都可从中断安全的上下文（ISR）调用其 *_give/_set（非阻塞）变体；
 * 阻塞变体只能在任务上下文调用，ISR 中调用会退化为忙等（见具体实现）。
 * ========================================================================= */

/* ---- 信号量（计数/二值） ---- */
typedef struct {
    uint32_t count;
    uint32_t limit;
    void    *waitq;
} rtos_sem_t;
void rtos_sem_init(rtos_sem_t *s, uint32_t initial, uint32_t limit);
int  rtos_sem_wait(rtos_sem_t *s);     /* 阻塞直到有许可 */
int  rtos_sem_trywait(rtos_sem_t *s);  /* 非阻塞，无许可返回 -1 */
void rtos_sem_give(rtos_sem_t *s);     /* 释放许可（ISR 安全） */

/* ---- 互斥量（优先级天花板协议，防优先级反转） ---- */
typedef struct {
    task_t  *owner;
    uint8_t  ceil_prio;   /* 天花板优先级：本互斥量会授予的最高优先级(数值最小) */
    void    *waitq;
} rtos_mutex_t;
void rtos_mutex_init(rtos_mutex_t *m, uint8_t ceil_prio);
int  rtos_mutex_lock(rtos_mutex_t *m);    /* 阻塞直到获得；自锁返回 -1 */
int  rtos_mutex_trylock(rtos_mutex_t *m); /* 非阻塞 */
int  rtos_mutex_unlock(rtos_mutex_t *m);  /* 释放；非持有者返回 -1 */

/* ---- 消息队列（定长项、环形缓冲、阻塞收发） ---- */
typedef struct {
    uint8_t *buf;
    size_t   item_size;
    size_t   cap;
    size_t   count;
    size_t   head;        /* 下一个写入位置 */
    void    *recv_waitq;  /* 因空而阻塞的接收者 */
    void    *send_waitq;  /* 因满而阻塞的发送者 */
} rtos_mq_t;
void rtos_mq_init(rtos_mq_t *q, void *buf, size_t item_size, size_t cap);
int  rtos_mq_send(rtos_mq_t *q, const void *item);   /* 满则阻塞 */
int  rtos_mq_trysend(rtos_mq_t *q, const void *item);/* 满返回 -1 */
int  rtos_mq_recv(rtos_mq_t *q, void *item);         /* 空则阻塞 */
int  rtos_mq_tryrecv(rtos_mq_t *q, void *item);      /* 空返回 -1 */

/* ---- 事件标志（32 位，ANY/ALL 等待） ---- */
typedef struct {
    uint32_t flags;
    void    *waitq;
} rtos_event_t;
void rtos_event_init(rtos_event_t *e);
void rtos_event_set(rtos_event_t *e, uint32_t bits); /* 置位并唤醒满足条件的等待者 */
void rtos_event_clear(rtos_event_t *e, uint32_t bits);
/* 阻塞等待：wait_all=1 要求 mask 所有位，否则任意一位；block=0 非阻塞。
 * 成功返回当前 flags；非阻塞且未满足返回 (uint32_t)-1。 */
uint32_t rtos_event_wait(rtos_event_t *e, uint32_t mask, int wait_all, int block);

/* ---- 内核对象注册表（调试/按名查找） ---- */
typedef enum {
    KOBJ_SEM = 0, KOBJ_MUTEX, KOBJ_MQ, KOBJ_EVENT, KOBJ_TASK, KOBJ_OTHER
} rtos_kobj_type_t;
int  rtos_kobj_register(const char *name, rtos_kobj_type_t type, void *ptr);
void *rtos_kobj_lookup(const char *name);
void rtos_kobj_foreach(void (*cb)(const char *name, rtos_kobj_type_t type, void *ptr));

/* ---- IPC 运行时自测（从 RTOSIPC 命令调用） ---- */
int rtos_ipc_selftest(void);

#endif /* JOC_RTOS_H */
