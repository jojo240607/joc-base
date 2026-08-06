/* ---------------------------------------------------------------------------
 * P2-1 调度轨迹 trace —— 环形缓冲记录 (tick, from, to, reason)
 * 仅在 rtos_pendsv_switch 临界区内单点写入；ISR 安全、无锁、不阻塞。
 * 详见 sched_trace.h。
 * ------------------------------------------------------------------------- */
#include "sched_trace.h"
#include "rtos_internal.h"

#if RTOS_SCHED_TRACE

/* 调试串口句柄（与 rtos_sched_assert_fail 同款，无 RTOS 依赖、中断安全）。
 * 核心层不 include 驱动头，沿用 extern void* 形式（见 sched.c 断言打印）。
 * 由 board 在 rtos_start 前绑定（见 console.c 的 g_debug_uart_hal）。 */
extern void uart_hal_putc(void *h, char c);
extern void *g_debug_uart_hal;

/* 环形缓冲状态（无锁：单核、仅在 PendSV 内写，不重入） */
static rtos_trace_rec_t g_trace[RTOS_TRACE_BUF_LEN];
static uint32_t g_trace_head  = 0;   /* 下一个写入槽 */
static uint32_t g_trace_count = 0;   /* 已写总数（<BUF_LEN 表示未绕回） */

/* 把 task_t* 换算成池索引（0..RTOS_MAX_TASKS-1）；NULL -> 0xffff（无） */
static uint16_t tr_id(task_t *t)
{
    if (t == (task_t *)0) return 0xffffu;
    int idx = (int)(t - &g_task_pool[0]);
    if (idx < 0 || idx >= RTOS_MAX_TASKS) return 0xfffeu;
    return (uint16_t)idx;
}

void rtos_trace_switch(task_t *from, task_t *to, uint8_t reason)
{
    /* 跳过纯时间片续跑（TICK）：idle 每 1ms 续跑会淹没缓冲，无调度分析价值。
     * 环形缓冲只保留真实上下文切换（START/PREEMPT/BLOCK），使有限容量能覆盖
     * 更长时间窗口内的有效调度事件。 */
    if (reason == RTOS_TRACE_TICK) return;

    rtos_trace_rec_t *r = &g_trace[g_trace_head];
    r->tick   = g_tick;
    r->from_id = tr_id(from);
    r->to_id   = tr_id(to);
    r->reason = reason;
    g_trace_head = (g_trace_head + 1u) % RTOS_TRACE_BUF_LEN;
    if (g_trace_count < RTOS_TRACE_BUF_LEN) g_trace_count++;
}

/* polling 打印辅助（无 RTOS 依赖，命令上下文 / 中断均可调用） */
static void tr_putc(char c) { uart_hal_putc(g_debug_uart_hal, (uint8_t)c); }
static void tr_puts(const char *s) { while (*s) tr_putc(*s++); }
static void tr_putu(uint32_t v)
{
    char b[12]; int i = 0;
    if (v == 0) { tr_putc('0'); return; }
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) tr_putc(b[--i]);
}

void rtos_trace_dump(void)
{
    static const char *const rname[5] = {
        "START", "PREEMPT", "BLOCK", "TICK", "WAKE"
    };
    tr_puts("[TRACE] count="); tr_putu(g_trace_count);
    tr_puts(" buf="); tr_putu(RTOS_TRACE_BUF_LEN);
    tr_puts(" (oldest->newest)\r\n");

    if (g_trace_count == 0) { tr_puts("[TRACE] <empty>\r\n"); return; }

    /* 环形缓冲 oldest = (head - count + BUF_LEN) % BUF_LEN */
    uint32_t oldest = (g_trace_head + RTOS_TRACE_BUF_LEN - g_trace_count)
                      % RTOS_TRACE_BUF_LEN;
    for (uint32_t k = 0; k < g_trace_count; k++) {
        uint32_t idx = (oldest + k) % RTOS_TRACE_BUF_LEN;
        rtos_trace_rec_t *r = &g_trace[idx];
        tr_puts("[TRACE] t="); tr_putu(r->tick);
        tr_puts(" from=");     tr_putu(r->from_id);
        tr_puts(" to=");       tr_putu(r->to_id);
        tr_puts(" ");
        const char *rn = (r->reason < 5) ? rname[r->reason] : "?";
        tr_puts(rn);
        tr_puts("\r\n");
    }
}

#endif /* RTOS_SCHED_TRACE */
