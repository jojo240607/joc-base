/* ---------------------------------------------------------------------------
 * jOS 可调度性静态自检（core/rtos_sched_analysis.c）：阶段3（见
 * docs/rtos-hard-realtime-plan.md §4）。
 *
 * 提供固定优先级「响应时间分析（RTA / Worst-Case Response Time）」：
 *   WCRT_i = C_i + Σ_{j : p_j < p_i} ⌈ WCRT_i / T_j ⌉ · C_j
 * 其中 C_j = wcet_ticks[j]（最坏单次执行），T_j = deadline_ticks[j]（最坏到达
 * 间隔，保守取等于截止期）。对每个硬实时任务迭代到不动点；若 WCRT_i > T_i
 * （= 截止期）则判定「不可调度」，列出违约任务。
 *
 * 设计要点：
 *  - 纯函数 rtos_wcrt_compute 对传入的 (C,T,P) 数组做 RTA，不依赖内核状态，
 *    便于自测构造已知正/反例。
 *  - rtos_sched_validate 扫描当前任务池里的硬实时任务（rt_class==1 且
 *    deadline/wcet 均非 0），跑 RTA 并把「不可调度任务数」写进
 *    g_rtos_sched_invalid（粘性、零挂起风险），不阻塞启动，但 RTOSALL 会 FAIL。
 *  - 额外给出 Liu & Layland 利用率必要判据 U = Σ C_i/T_i 的上界 1（单处理器）
 *    作为快速必要（非充分）校验，并在日志里打印，便于早期定位过载。
 *
 * 保守性说明：RTA 只考虑 rt_class==1 任务之间的相互抢占干扰。若高优先级带里
 * 混有「无 wcet 上限」的非实时任务，其最坏执行无法被本分析上界，故硬实时任务
 * 应占据系统最高优先级（阶段4 §4.2 也强制 prio<=RTOS_PRIO_BH_HIGH）。这是与
 * 计划书一致的、文档化的保守假设。
 * ------------------------------------------------------------------------- */

#include "rtos.h"
#include "rtos/core/rtos_internal.h"
#include "log/log.h"
#include "log/app_log.h"
#include <string.h>

/* 诊断：用固定的全局哨兵变量（主 SRAM .bss）写阶段魔术字，供 OpenOCD 读取定位
 * HardFault 位置（不依赖 UART，内存写不会 fault；用全局变量而非固定地址避免与
 * 主栈/CCM 栈重叠导致读数被覆盖）。 */
volatile uint32_t g_dbg_sentinel = 0;
static void dbg_mark(uint32_t v) { g_dbg_sentinel = v; }

/* 不可调度粘性标志：>0 表示存在截止期内不可调度的硬实时任务（存违约任务数）。
 * RTOSALL / 看门狗读取，零挂起风险：不触发异常、不停机。 */
volatile uint32_t g_rtos_sched_invalid = 0;

/* 单次固定优先级 RTA（Joseph & Pandya 响应时间分析）。
 * 入参：C[i]=最坏执行, T[i]=周期/截止期(必须 >0), P[i]=优先级(0=最高), n=任务数。
 * 出参：wcrt_out[i] = 该任务最坏响应时间（仅在返回可行时有效）。
 * 返回：不可调度（wcrt>T）的任务个数；0 = 全可调度；负数 = 参数非法。
 * 算法：按优先级从最高(p 最小)到最低依次求不动点；任务 i 的干扰来自所有
 * p_j < p_i 的任务 j。迭代上界 n*(max_T) 防止死循环（理论上 ≤ n 轮即收敛）。 */
int rtos_wcrt_compute(const uint32_t *C, const uint32_t *T, const uint8_t *P,
                      int n, uint32_t *wcrt_out) {
    if (!C || !T || !P || n <= 0 || !wcrt_out) return -1;
    int infeasible = 0;
    for (int i = 0; i < n; i++) {
        if (T[i] == 0 || C[i] == 0) {          /* 无截止期/无预算：不参与、不判 */
            wcrt_out[i] = 0;
            continue;
        }
        /* 不动点迭代：W 从自身 C_i 起，累加高优任务干扰，直到稳定。 */
        uint32_t W = C[i];
        int stabilized = 0;
        /* 最多 n 轮（RTA 收敛上界为任务数）；再加大上限防极端数值。 */
        for (int iter = 0; iter < n + 1; iter++) {
            uint32_t newW = C[i];
            for (int j = 0; j < n; j++) {
                if (j == i) continue;
                if (P[j] < P[i] && T[j] > 0 && C[j] > 0) {   /* j 优先级更高 */
                    /* ⌈W / T_j⌉ · C_j：高优任务在本窗口内的最坏到达次数 × 其 WCET */
                    uint32_t occ = (W + T[j] - 1U) / T[j];
                    newW += occ * C[j];
                }
            }
            if (newW == W) { stabilized = 1; W = newW; break; }
            W = newW;
            if (W > T[i]) break;   /* 提前剪枝：已超截止期 */
        }
        wcrt_out[i] = W;
        if (!stabilized || W > T[i]) infeasible++;
    }
    return infeasible;
}

/* 计算硬实时任务集的总利用率 U = Σ C_i/T_i（Liu & Layland 必要判据）。
 * 返回放大 1000 倍的整数百分比（避免浮点）。仅统计 rt_class==1 且 C,T 均非 0。 */
static uint32_t rtos_sched_utilization_x1000(void) {
    dbg_mark(0x1001u);
    uint32_t num = 0;   /* 分子 Σ C_i*1000/T_i 的整数累加 */
    int n = rtos_task_count();
    for (int i = 0; i < n; i++) {
        uint32_t c = rtos_task_wcet(i);
        uint32_t t = rtos_task_deadline(i);
        if (rtos_task_rt_class(i) == 1 && c != 0 && t != 0) {
            num += (c * 1000U) / t;
        }
    }
    return num;   /* 实际 U = num/1000 */
}

/* 启动/测试期可调度性校验：扫描任务池硬实时任务，跑 RTA，置位粘性标志。
 * 不阻塞启动（即便不可调度也继续 boot），但 RTOSALL 会据此 FAIL。
 * 返回不可调度任务数。 */
/* 调试实验：把分析数组从栈搬到 static，规避 CCM 主栈膨胀；并把日志改为一次性
 * 计算 + 裸 UART 输出（避免 newlib vfprintf 深栈嵌套破坏异常帧）。 */
static uint32_t g_dbg_infeasible = 0;
int rtos_sched_validate(void) {
    dbg_mark(0x2002u);
    int n = rtos_task_count();
    /* 最坏情况：全部任务都是硬实时（RTOS_MAX_TASKS）。用 static 定长数组，不占栈。 */
    static uint32_t C[RTOS_MAX_TASKS];
    static uint32_t T[RTOS_MAX_TASKS];
    static uint8_t  P[RTOS_MAX_TASKS];
    int m = 0;   /* 实际参与分析的硬实时任务数 */
    for (int i = 0; i < n && m < RTOS_MAX_TASKS; i++) {
        uint32_t c = rtos_task_wcet(i);
        uint32_t t = rtos_task_deadline(i);
        if (rtos_task_rt_class(i) == 1 && c != 0 && t != 0) {
            C[m] = c; T[m] = t; P[m] = rtos_task_prio(i);
            m++;
        }
    }
    static uint32_t wcrt[RTOS_MAX_TASKS];
    int infeasible;
    if (m == 0) {
        infeasible = 0;   /* 无硬实时任务：平凡可调度，零回归 */
    } else {
        dbg_mark(0x3001u);
        infeasible = rtos_wcrt_compute(C, T, P, m, wcrt);
        dbg_mark(0x4001u);
    }
    g_rtos_sched_invalid = (infeasible > 0) ? (uint32_t)infeasible : 0;
    g_dbg_infeasible = (uint32_t)infeasible;

    dbg_mark(0x5001u);
    uint32_t util_x1000 = rtos_sched_utilization_x1000();
    dbg_mark(0x5002u);
    /* 注意：本函数在 rtos_start() 内、首任务切换前执行，此时 uart 设备尚未被
     * device_manager_get 触发 create（lazy 模式），USART1 时钟/引脚未就绪。
     * 故此处【绝不】调用 uart_console_putc——否则会卡死在 while(!TXE) 导致 boot 挂起。
     * 校验结果已写入 g_rtos_sched_invalid / g_dbg_infeasible，由 RTOSALL/看门狗读取。 */
    (void)util_x1000;
    dbg_mark(0x5003u);
    return infeasible;
}

/* 控制台 RTOSSCHED 用的详细打印：列出每个硬实时任务的 C/T/P 与 WCRT。 */
void rtos_sched_analysis_print(void) {
    int n = rtos_task_count();
    uint32_t C[RTOS_MAX_TASKS];
    uint32_t T[RTOS_MAX_TASKS];
    uint8_t  P[RTOS_MAX_TASKS];
    int m = 0;
    for (int i = 0; i < n && m < RTOS_MAX_TASKS; i++) {
        uint32_t c = rtos_task_wcet(i);
        uint32_t t = rtos_task_deadline(i);
        if (rtos_task_rt_class(i) == 1 && c != 0 && t != 0) {
            C[m] = c; T[m] = t; P[m] = rtos_task_prio(i);
            m++;
        }
    }
    log_printf(app_log(), LOG_INFO, "rtos",
               "[SCHED] hard-rt tasks=%d  util=%lu.%03lu%%  invalid=%lu\n",
               m,
               (unsigned long)(rtos_sched_utilization_x1000() / 1000U),
               (unsigned long)(rtos_sched_utilization_x1000() % 1000U),
               (unsigned long)g_rtos_sched_invalid);
    uint32_t wcrt[RTOS_MAX_TASKS];
    if (m > 0) {
        rtos_wcrt_compute(C, T, P, m, wcrt);
        for (int k = 0; k < m; k++) {
            /* 反查任务名（与 rtos_sched_validate 同款顺序定位）。 */
            const char *nm = (const char *)0;
            int jj = 0, kk = 0;
            for (; jj < n && kk < RTOS_MAX_TASKS; jj++) {
                uint32_t cc = rtos_task_wcet(jj);
                uint32_t tt = rtos_task_deadline(jj);
                if (rtos_task_rt_class(jj) == 1 && cc != 0 && tt != 0) {
                    if (kk == k) { nm = rtos_task_name(jj); break; }
                    kk++;
                }
            }
            log_printf(app_log(), LOG_INFO, "rtos",
                       "[SCHED]   '%s' prio=%u C=%lu T=%lu WCRT=%lu %s\n",
                       nm ? nm : "?", (unsigned)P[k],
                       (unsigned long)C[k], (unsigned long)T[k],
                       (unsigned long)wcrt[k],
                       (wcrt[k] <= T[k]) ? "OK" : "INFEASIBLE");
        }
    } else {
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[SCHED] (no hard-real-time tasks declared)\n");
    }
}

uint32_t rtos_rt_sched_invalid(void) { return g_rtos_sched_invalid; }
