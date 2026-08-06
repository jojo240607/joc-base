/* ---------------------------------------------------------------------------
 * P2-1 调度轨迹 trace —— 环形缓冲记录 (tick, from, to, reason)
 * 仅在 rtos_pendsv_switch 临界区内单点写入；ISR 安全、无锁、不阻塞。
 * 详见 sched_trace.h。
 * ------------------------------------------------------------------------- */
#include "sched_trace.h"
#include "rtos_internal.h"
#include "rtos_config.h"   /* RTOS_PRIO_BH_HIGH：硬实时优先级带上限 */

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

/* 实时延迟直方图（IRQ→任务唤醒端到端延迟）。
 * 仅在 rtos_pendsv_switch 切入点 to 时 rtos_lat_sample 写入（单核、PendSV 内、不重入）。
 * 非 static：供 rtos_bench.c 在 GDB 可 dump 的结果中引用（同构建门控）。 */
rtos_latency_t g_lat;

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

    /* 延迟量化：用刚切入的 to 任务的 wake_cycle 戳算端到端延迟。 */
    rtos_lat_sample(to);
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

/* ---------------------------------------------------------------------------
 * 实时延迟量化：IRQ/唤醒源 → 任务真正运行的端到端延迟
 * ------------------------------------------------------------------------- */
void rtos_lat_sample(task_t *to)
{
    if (to == (task_t *)0) return;
    /* 仅统计硬实时带（prio <= RTOS_PRIO_BH_HIGH=4）任务的唤醒延迟。
     * 非实时/低优先级任务的唤醒排队延迟（可达成百 ms）会污染直方图 max，
     * 不能用来证明「硬实时延迟有界」。分层后直方图 max 即硬实时最坏延迟，
     * 才是证明 RTOS 实时性优秀的真实指标。 */
    if (to->prio > RTOS_PRIO_BH_HIGH) { to->wake_cycle = 0; return; }
    uint32_t wc = to->wake_cycle;
    if (wc == 0) return;                 /* 该任务本次并非经唤醒路径切入 */
    uint32_t now = rtos_cycle_now();
    /* 处理 CYCCNT 32位回绕（~25.6s @168MHz，单次延迟远小于此，取差即可） */
    uint32_t lat = (now >= wc) ? (now - wc) : (0xFFFFFFFFu - wc + now + 1u);
    if (lat == 0) return;

    rtos_latency_t *L = &g_lat;
    if (L->count == 0 || lat < L->min_cyc) L->min_cyc = lat;
    if (lat > L->max_cyc)                  L->max_cyc = lat;
    L->sum_cyc += lat;
    L->count++;
    uint32_t b = lat / RTOS_LAT_STEP_CYCLE;   /* 桶索引 = 延迟(us) */
    if (b > RTOS_LAT_OVER_BUCKET) b = RTOS_LAT_OVER_BUCKET;
    L->hist[b]++;

    /* 消费即清零：每个「唤醒事件」只采样一次。否则被抢占/时间片重入的任务
     * 会沿用上一次 BLOCK→ready_add 设的陈旧 wc，跨多次切换算出天文数字
     * （CYCCNT 回绕 + 残留值叠加）。清零后：仅「刚经 ready_add 唤醒后首次
     * 切入」会被采样，非 READY→READY 的重入切换自然被 wc==0 守卫跳过。 */
    to->wake_cycle = 0;
}

void rtos_lat_reset(void)
{
    rtos_latency_t *L = &g_lat;
    L->count = 0; L->min_cyc = 0; L->max_cyc = 0; L->sum_cyc = 0;
    for (uint32_t i = 0; i <= RTOS_LAT_BUCKETS; i++) L->hist[i] = 0;
}

void rtos_lat_dump(void)
{
    rtos_latency_t *L = &g_lat;
    tr_puts("[LAT] IRQ->task wakeup end-to-end (CYCCNT@168MHz, 1us=168cyc)\r\n");
    tr_puts("[LAT] samples="); tr_putu(L->count);
    if (L->count == 0) { tr_puts(" <none>\r\n"); return; }
    tr_puts(" min=");  tr_putu(L->min_cyc);  tr_puts("cyc(");
    tr_putu(L->min_cyc / 168u); tr_puts("us)");
    tr_puts(" max=");  tr_putu(L->max_cyc);  tr_puts("cyc(");
    tr_putu(L->max_cyc / 168u); tr_puts("us)");
    tr_puts(" avg=");  tr_putu((uint32_t)(L->sum_cyc / L->count));
    tr_puts("cyc(");   tr_putu((uint32_t)(L->sum_cyc / L->count / 168u));
    tr_puts("us)\r\n");
    /* 直方图：0..15us 各桶 + 溢出桶 */
    for (uint32_t i = 0; i <= RTOS_LAT_BUCKETS; i++) {
        if (L->hist[i] == 0 && i != RTOS_LAT_OVER_BUCKET) continue;
        tr_puts("[LAT]   ");
        if (i == RTOS_LAT_OVER_BUCKET) tr_puts(" >16us:");
        else { tr_putu(i); tr_puts("us:"); }
        tr_putu(L->hist[i]); tr_puts("\r\n");
    }
}

#endif /* RTOS_SCHED_TRACE */
