/* ===========================================================================
 * jOS RTOS —— Rhealstone 子集基准 + 实时延迟量化导出（RTOSBENCH 命令）
 * ---------------------------------------------------------------------------
 * 门控 RTOS_SCHED_TRACE（与 sched_trace 同开关、同 build_trace 配置）：
 *   依赖 rtos_cycle_now()（DWT CYCCNT @168MHz，~6ns，不受 BASEPRI 影响）与
 *   uart_hal polling 输出，二者均在该构建下可用。常态构建不编译本文件，
 *   命令 RTOSBENCH 不注册，零开销。
 *
 * 量化目标（回应"RTOS 是否优秀"的硬指标）：
 *   (1) 任务切换 / 信号量混洗时间（Rhealstone 经典项）：
 *       两等高优先级任务经信号量 ping-pong 交接，round-trip/2 即单次
 *       "切换 + give/take" 时延。取 min/avg/max（cycle + µs）。
 *   (2) 中断→任务唤醒端到端延迟（IRQ->wakeup）：
 *       由 sched_trace.c 的 rtos_lat_* 系统级直方图提供——所有唤醒路径
 *       （含真实 ISR 内 rtos_sem_give / rtos_mq_send_fromisr）最终汇入
 *       ready_add() 打 wake_cycle 戳，PendSV 切入时算差值。本命令末尾
 *       rtos_lat_dump() 打印该直方图，覆盖系统里真实中断唤醒（UART/USB/
 *       TIM 心跳等 ISR 唤醒任务）的最坏延迟。
 *
 * 设计约束（沿用 A6 / sched_trace 惯例）：
 *   - 用 DWT CYCCNT 测延迟，168 cycle/µs。
 *   - 基准任务用普通 .bss 栈（无 DMA，CCM 留给常驻 TCB 池）。
 *   - 不与 board 已占用的 TIM（TIM2..TIM14 全部用作 timer0..13）冲突：
 *     不新造 TIM ISR，IRQ 延迟直接复用系统级 rtos_lat 直方图（已含真实 ISR）。
 * ========================================================================= */
#include "rtos.h"
#include "rtos/core/rtos_internal.h"
#include "rtos/core/sched_trace.h"   /* rtos_lat_* / rtos_lat_dump */
#include "log/log.h"
#include "log/app_log.h"
#include <stdint.h>
#include <stddef.h>

/* 整文件门控 RTOS_SCHED_TRACE：常态构建（=0）编译为空，零开销；
 * 此时 rtos.h 不暴露 rtos_bench_run，console.c 不注册 RTOSBENCH 命令。 */
#if RTOS_SCHED_TRACE

#undef RTOS_TASK_STACK
/* bench 任务栈放主 RAM .bss：缩小尺寸以控制 .bss 增量，避免挤占 boot 期
 * heap(sbrk) 导致 _calloc_r 越界 BusFault。任务栈纯 CPU 访问、无 DMA，本可
 * 放 CCM，但 build_trace 的 CCM 已达 ~95% 余量极小，故留主 RAM 并压尺寸
 * （子任务 256B 浅调用足够，auto 任务 512B 容纳 rtos_bench_run 的深层调用）。 */
#define RTOS_TASK_STACK(name, sz) \
    static uint8_t name[sz] __attribute__((aligned(RTOS_STACK_ALIGN_UP(sz))))

/* ---- 基准参数 ---- */
#define BENCH_SWITCH_N    2000u      /* 任务切换采样次数 */
#define BENCH_SWITCH_PRIO 6          /* 等高优先级（BH_MED 带内，无基线/硬实时冲突） */
#define BENCH_STK_SZ      256

/* 基准结果（全局，供 GDB 直接 dump，绕开串口验证环境限制）。 */
typedef struct {
    uint32_t sw_min_cyc, sw_avg_cyc, sw_max_cyc;  /* 任务切换/信号量混洗：单次(rt/2) */
    uint32_t lat_count, lat_min_cyc, lat_avg_cyc, lat_max_cyc;  /* 硬实时唤醒延迟 */
    uint32_t lat_hist[RTOS_LAT_BUCKETS + 1];
    /* 临界区持锁时长直方图（中断延迟长尾定位器）：0..15us 桶 + 溢出桶(>16us) */
    uint32_t crit_total, crit_max_cyc;
    uint32_t crit_hist[RTOS_CRIT_HIST_BUCKETS + 1];
    /* PendSV 切换分段计时（定位 33µs 长尾真实去向）：7 个里程碑相对入口 T0 的偏移(cyc)。
     *   seg[0]=T0(0) seg[1]=s16-s31压栈后(FPU专有,basic=0) seg[2]=SAVE完成
     *   seg[3]=内核段(rtos_pendsv_switch返回) seg[4]=MPU段(apply_task_priv返回)
     *   seg[5]=RESTORE完成 seg[6]=EXIT段(bx前)。差值即各段耗时。 */
    uint32_t pendsv_seg[RTOS_PENDSV_SEG_N];
    uint32_t valid;   /* 非零 = 结果有效（bench 已跑完） */
} bench_result_t;
bench_result_t g_bench_result;

/* 信号量混洗 ping-pong 共享状态 */
static rtos_sem_t  g_bs_sem;        /* 初始 0，充当交接令牌 */
static volatile int g_bs_done;
static volatile uint32_t g_bs_rt_min, g_bs_rt_max;
static volatile uint64_t g_bs_rt_sum;
static volatile uint32_t g_bs_n;

RTOS_TASK_STACK(g_bs_a_stk, BENCH_STK_SZ);
RTOS_TASK_STACK(g_bs_b_stk, BENCH_STK_SZ);

/* 任务 A：拿 sem -> 记录 -> 交还 sem 给 B，循环 BENCH_SWITCH_N/2 次 */
static void bench_switch_a(void *arg) {
    (void)arg;
    volatile uint32_t t0 = 0;
    for (uint32_t i = 0; i < BENCH_SWITCH_N / 2u; i++) {
        rtos_sem_wait(&g_bs_sem);                 /* 等 B 交还令牌 */
        uint32_t t1 = rtos_cycle_now();
        if (i > 0) {                              /* 跳过首轮冷启动 */
            uint32_t rt = (t1 >= t0) ? (t1 - t0) : 0;  /* = 2 次切换+交接 */
            if (g_bs_n == 0 || rt < g_bs_rt_min) g_bs_rt_min = rt;
            if (rt > g_bs_rt_max)                 g_bs_rt_max = rt;
            g_bs_rt_sum += rt;
            g_bs_n++;
        }
        t0 = t1;
        rtos_sem_give(&g_bs_sem);                 /* 交还令牌给 B */
    }
    g_bs_done = 1;
    while (1) rtos_msleep(1000);
}
/* 任务 B：首轮发起令牌，之后与 A 对称交接 */
static void bench_switch_b(void *arg) {
    (void)arg;
    rtos_sem_give(&g_bs_sem);                     /* 启动令牌 */
    for (uint32_t i = 0; i < BENCH_SWITCH_N / 2u; i++) {
        rtos_sem_wait(&g_bs_sem);                 /* 等 A 交还 */
        rtos_sem_give(&g_bs_sem);                 /* 交还 A */
    }
    while (1) rtos_msleep(1000);
}

/* ---- 硬实时唤醒延迟受控基准（prio 3，BH_HIGH 带内）---- */
#define BENCH_RT_N    2000u
#define BENCH_RT_PRIO 3          /* 硬实时带（<= RTOS_PRIO_BH_HIGH=4） */
static rtos_sem_t  g_bh_sem;
static volatile uint32_t g_bh_done;
RTOS_TASK_STACK(g_bh_stk, 512);

/* 硬实时任务：被唤醒 → 立即回报完成计数 → 重新阻塞。延迟由 rtos_lat_sample
 * 在 PendSV 切入本任务时自动采集（prio 3 在硬实时带内，计入直方图）。 */
static void bench_rt_task(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < BENCH_RT_N; i++) {
        rtos_sem_wait(&g_bh_sem);   /* 阻塞，等待主任务 give 唤醒 */
        g_bh_done++;                /* 回报：已被调度运行 */
    }
    while (1) rtos_msleep(1000);
}

/* 运行 Rhealstone 子集基准并打印结果。命令上下文调用（任务态，可睡眠）。 */
void rtos_bench_run(void) {
    rtos_cycle_init();
    log_printf(app_log(), LOG_INFO, "rtos",
               "[BENCH] Rhealstone subset @168MHz (CYCCNT, 168cyc=1us)\r\n");

    /* ---------- (1) 任务切换 / 信号量混洗 ---------- */
    rtos_sem_init(&g_bs_sem, 0, 1);
    g_bs_done = 0; g_bs_n = 0;
    g_bs_rt_min = 0xFFFFFFFFu; g_bs_rt_max = 0; g_bs_rt_sum = 0;

    rtos_task_create("bench_sw_a", bench_switch_a, NULL,
                     BENCH_SWITCH_PRIO, g_bs_a_stk, sizeof(g_bs_a_stk));
    rtos_task_create("bench_sw_b", bench_switch_b, NULL,
                     BENCH_SWITCH_PRIO, g_bs_b_stk, sizeof(g_bs_b_stk));

    /* 等基准完成（两任务各跑 N/2 次交接；超时保护） */
    uint32_t to = rtos_tick_count() + (BENCH_SWITCH_N / 10u) + 50u;
    while (!g_bs_done && rtos_tick_count() < to) rtos_msleep(2);

    uint32_t avg_rt = (g_bs_n > 0) ? (uint32_t)(g_bs_rt_sum / g_bs_n) : 0;
    /* round-trip = 2 x (task-switch + sem give/take)；单次切换时延 = rt/2 */
    uint32_t sw_min = g_bs_rt_min / 2u, sw_avg = avg_rt / 2u, sw_max = g_bs_rt_max / 2u;
    log_printf(app_log(), LOG_INFO, "rtos",
               "[BENCH] TaskSwitch/SemShuffle: n=%lu samples=%lu\r\n",
               (unsigned long)BENCH_SWITCH_N, (unsigned long)g_bs_n);
    log_printf(app_log(), LOG_INFO, "rtos",
               "[BENCH]   per-switch min=%lucyc(%luus) avg=%lucyc(%luus) max=%lucyc(%luus)\r\n",
               (unsigned long)sw_min,  (unsigned long)(sw_min/168u),
               (unsigned long)sw_avg,  (unsigned long)(sw_avg/168u),
               (unsigned long)sw_max,  (unsigned long)(sw_max/168u));

    /* 清理基准任务 */
    task_t *ta = (task_t *)rtos_kobj_lookup("bench_sw_a");
    task_t *tb = (task_t *)rtos_kobj_lookup("bench_sw_b");
    if (ta) rtos_task_delete(ta);
    if (tb) rtos_task_delete(tb);
    rtos_msleep(20);

    /* ---------- (2) 硬实时唤醒延迟（受控基准，prio<=4 才被直方图统计）----------
     * 创建一个 prio=3 的硬实时任务，阻塞于 sem；主任务(prio 6)循环 give 唤醒它，
     * 共 BENCH_RT_N 次。每次唤醒经 ready_add 打 wake_cycle 戳、PendSV 切入时被
     * rtos_lat_sample 采集（prio 3 在硬实时带内）。这给出「唤醒源 → 硬实时任务
     * 真正运行」的端到端延迟分布，是硬实时性的直接证据。
     * 跑前 rtos_lat_reset 清掉系统残留样本，使直方图仅含本次受控基准。 */
    rtos_lat_reset();
    rtos_sem_init(&g_bh_sem, 0, 1);
    g_bh_done = 0;
    rtos_task_create("bench_rt", bench_rt_task, NULL,
                     BENCH_RT_PRIO, g_bh_stk, sizeof(g_bh_stk));
    for (uint32_t i = 0; i < BENCH_RT_N; i++) {
        rtos_sem_give(&g_bh_sem);          /* 唤醒硬实时任务 */
        /* 等它完成本次（用 g_bh_done 计数器轮询，避免主任务睡眠导致样本丢失） */
        uint32_t w = 0;
        while (g_bh_done <= i && w < 100000u) w++;
    }
    /* 等最后几次被调度进来 */
    uint32_t to2 = rtos_tick_count() + 30u;
    while (g_bh_done < BENCH_RT_N && rtos_tick_count() < to2) rtos_msleep(1);

    log_printf(app_log(), LOG_INFO, "rtos",
               "[BENCH] HardRealTime wakeup (prio<=4 tasks, %lu samples):\r\n",
               (unsigned long)BENCH_RT_N);
    rtos_lat_dump();   /* 打印硬实时延迟直方图：min/avg/max + 0..16us 桶分布 */

    /* 填充全局结果（供 GDB dump，绕开串口验证环境限制） */
    g_bench_result.sw_min_cyc = sw_min; g_bench_result.sw_avg_cyc = sw_avg;
    g_bench_result.sw_max_cyc = sw_max;
    g_bench_result.lat_count = g_lat.count;
    g_bench_result.lat_min_cyc  = g_lat.min_cyc;
    g_bench_result.lat_avg_cyc  = (g_lat.count > 0) ? (uint32_t)(g_lat.sum_cyc / g_lat.count) : 0;
    g_bench_result.lat_max_cyc  = g_lat.max_cyc;
    for (uint32_t i = 0; i <= RTOS_LAT_BUCKETS; i++)
        g_bench_result.lat_hist[i] = g_lat.hist[i];

    /* ---------- (3) 临界区持锁时长直方图（中断延迟长尾定位器）----------
     * 直方图自系统启动起累计（含 bench 过程本身的临界区，具代表性）。它直接揭示
     * 「是谁」持锁过久 —— 即 IRQ→任务唤醒延迟最坏 32.8µs 的元凶。理想分布应
     * 集中在 0~1us 桶；若 >16us 溢出桶显著，则需定位调用方拆细临界区。 */
    log_printf(app_log(), LOG_INFO, "rtos",
               "[BENCH] Critical-section hold-time histogram (total=%lu, worst=%lucyc=%luus):\r\n",
               (unsigned long)g_crit_hist_total,
               (unsigned long)g_crit_hist_max, (unsigned long)(g_crit_hist_max/168u));
    for (uint32_t i = 0; i < RTOS_CRIT_HIST_BUCKETS; i++) {
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BENCH]   [%2lu-%2lu us] : %lu\r\n",
                   (unsigned long)(i), (unsigned long)(i+1),
                   (unsigned long)g_crit_hist[i]);
    }
    log_printf(app_log(), LOG_INFO, "rtos",
               "[BENCH]   [ >%2lu us ] : %lu\r\n",
               (unsigned long)RTOS_CRIT_HIST_BUCKETS,
               (unsigned long)g_crit_hist[RTOS_CRIT_HIST_OVER]);

    g_bench_result.crit_total  = g_crit_hist_total;
    g_bench_result.crit_max_cyc = g_crit_hist_max;
    for (uint32_t i = 0; i <= RTOS_CRIT_HIST_BUCKETS; i++)
        g_bench_result.crit_hist[i] = g_crit_hist[i];

    /* ---------- (4) PendSV 切换分段计时（定位 33µs 长尾真实去向）----------
     * 这是「一次上下文切换」的拆解画像（最后一次 PendSV 的 7 个里程碑相对入口 T0 的偏移）。
     * 直接看 33µs 花在哪段：SAVE / 内核(选任务+临界区) / MPU(栈region重配) / RESTORE / EXIT。
     * seg[1]>0 表示本次是 FPU 任务切换（含 s16-s31 额外开销）。 */
    if (g_pendsv_seg_valid) {
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BENCH] PendSV switch segments (cyc, rel T0): "
                   "SAVE=%lu KERNEL=%lu MPU=%lu RESTORE=%lu EXIT=%lu%s\r\n",
                   (unsigned long)g_pendsv_seg[2],
                   (unsigned long)(g_pendsv_seg[3] - g_pendsv_seg[2]),
                   (unsigned long)(g_pendsv_seg[4] - g_pendsv_seg[3]),
                   (unsigned long)(g_pendsv_seg[5] - g_pendsv_seg[4]),
                   (unsigned long)(g_pendsv_seg[6] - g_pendsv_seg[5]),
                   (g_pendsv_seg[1] > 0) ? " [FPU-task]" : " [non-FPU]");
        if (g_pendsv_seg[1] > 0)
            log_printf(app_log(), LOG_INFO, "rtos",
                       "[BENCH]   FPU s16-s31 save overhead ~%lucyc (seg[1] before r4-r11)\r\n",
                       (unsigned long)g_pendsv_seg[1]);
    } else {
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BENCH] PendSV segments: NOT COLLECTED (no switch occurred)\r\n");
    }
    for (uint32_t i = 0; i < RTOS_PENDSV_SEG_N; i++)
        g_bench_result.pendsv_seg[i] = g_pendsv_seg[i];
    g_bench_result.valid = 1;

    task_t *tr = (task_t *)rtos_kobj_lookup("bench_rt");
    if (tr) rtos_task_delete(tr);
}

#endif /* RTOS_SCHED_TRACE */
