#ifndef JOC_RTOS_SCHED_TRACE_H
#define JOC_RTOS_SCHED_TRACE_H
/* ---------------------------------------------------------------------------
 * P2-1 调度轨迹 trace（开发/诊断用，发布构建零开销）
 *
 * 目的：在唯一发生任务上下文切换的点（rtos_pendsv_switch）记录
 *   (tick, task_from, task_to, reason)
 * 到固定大小环形缓冲，供控制台命令 RTOSTRACE 实时/事后导出，用于
 * 分析调度时序（抢占/睡眠/唤醒/时间片轮转）、定位优先级反转/饿死。
 *
 * 设计约束（零挂起风险，遵循内核现有惯例）：
 *   - 仅在 rtos_pendsv_switch 的临界区内、单点写入；单核不重入，无需锁。
 *   - 写入纯环形（head 自增取模），绕回覆盖最旧记录，永不阻塞/分配。
 *   - 导出 rtos_trace_dump 用 uart_hal_putc 直接 polling 打印
 *     （与 rtos_sched_assert_fail 同款，无 RTOS 依赖、中断安全）。
 *   - 编译期门控 RTOS_SCHED_TRACE（默认 0）：关闭时 sched_trace.c 不编译、
 *     rtos_pendsv_switch 内 #if 包裹、rtos.h 不暴露 API、RTOSTRACE 命令不注册。
 * ------------------------------------------------------------------------- */

#include "rtos.h"   /* task_t / g_tick 等核心类型 */

/* 切换原因（也用作导出时的短标签）。
 * 注意：这些常量【始终】可见（在 #if 外），使调用点 rtos_pendsv_switch 在
 * RTOS_SCHED_TRACE=0 时仍能编译（rtos_trace_switch 退化为 no-op，但参数须合法）。 */
#define RTOS_TRACE_START   0   /* 首次启动（cur==NULL） */
#define RTOS_TRACE_PREEMPT 1   /* cur 被更高优先级抢占回就绪 */
#define RTOS_TRACE_BLOCK   2   /* cur 主动睡眠/阻塞让出 CPU */
#define RTOS_TRACE_TICK    3   /* 时间片轮转续跑同一任务 */
#define RTOS_TRACE_WAKE    4   /* 唤醒路径切换（cur 阻塞、nxt 是被唤醒者） */

#if RTOS_SCHED_TRACE

/* 环形缓冲容量（编译期；占 RAM = N * 12B）。256 = ~3KB，开发构建可接受。 */
#ifndef RTOS_TRACE_BUF_LEN
  #define RTOS_TRACE_BUF_LEN 256u
#endif

/* 单条轨迹记录（12 字节，避免 padding） */
typedef struct {
    uint32_t tick;     /* 切换发生时的 g_tick */
    uint16_t from_id;  /* 切出任务的 TCB 池索引（< RTOS_MAX_TASKS，0=无） */
    uint16_t to_id;    /* 切入任务的 TCB 池索引 */
    uint8_t  reason;   /* RTOS_TRACE_* */
    uint8_t  _pad[3];
} rtos_trace_rec_t;

/* 在 rtos_pendsv_switch 内、临界区内调用（ISR 安全、单点、无锁）。 */
void rtos_trace_switch(task_t *from, task_t *to, uint8_t reason);

/* 经调试 UART 导出整段环形缓冲（polling 打印，命令上下文调用）。 */
void rtos_trace_dump(void);

#else /* !RTOS_SCHED_TRACE */

/* 关闭时退化为 no-op，调用点无需 #if 守卫（保持 sched.c 干净）。 */
#define rtos_trace_switch(from, to, reason)  ((void)0)
#define rtos_trace_dump()                    ((void)0)

#endif /* RTOS_SCHED_TRACE */

#endif /* JOC_RTOS_SCHED_TRACE_H */
