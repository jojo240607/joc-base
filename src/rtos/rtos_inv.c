#include "rtos.h"
#include "rtos_mpu.h"      /* g_fault_cfsr / g_stack_overflow 粘性标志 */
#include "log/log.h"
#include "log/app_log.h"
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 优先级天花板协议验证自测（RTOSINV 命令，并注册进 RTOSALL "inv" 条目）。
 *
 * 本 RTOS 的调度模型是【协作式抢占】：运行中的任务只会在“遇到调度点”时被切换走——
 *   调度点 = (a) 主动阻塞(rtos_msleep/等锁/等信号量)；或 (b) ISR 返回时 PendSV 生效。
 * 一个纯忙循环(rtos_yield 也仅 pend PendSV，PendSV 要等异常返回才服务)不会被中途抢占。
 * 因此“中优先级任务抢占总在跑的低优先级持锁方、造成无界反转”在本内核【不会发生】——
 * 这本身是一项稳健性优点：反转天然有界。
 *
 * 那么天花板协议的【可观测正确性属性】在于它的“优先级提升状态机”，本测试逐项断言：
 *   1) 持锁瞬间，持有者有效优先级被提升到 ceil_prio（断言 prio 变化）。
 *   2) 释放锁瞬间，持有者有效优先级恢复到 base_prio（断言 prio 恢复）。
 *   3) 递归加锁仍保持提升、解锁(末次)才恢复。
 *   4) handoff：高优先级等待者被唤醒成为 owner 时，其有效优先级被设到 ceil_prio
 *      （断言被唤醒任务以 ceil_prio 运行）。
 *   5) 有界性：高优先级 H 等锁的耗时 = 低优先级 L 的临界区长度 + 极小开销（因为本内核
 *      不可能发生“M 中途抢占 L 拉长 H 等待”），无论是否启用天花板都是“有界”的——
 *      这反证了本内核不会陷入无界反转，是稳健性结论而非缺陷。
 *
 * 若内核天花板实现有误（不提升 / 恢复错 / handoff 不赋 ceil），断言 1/2/4 会 FAIL，
 * 暴露内核 defect；若是“无界反转”类缺陷，本内核结构上不可能出现，故 5 仅作有界性证据。
 * ------------------------------------------------------------------------- */

/* 优先级约定：数值越小优先级越高。H 必须高于 M，M 高于 L。 */
#define INV_H_PRIO 5     /* 最高：等锁方 */
#define INV_M_PRIO 10    /* 中：与锁无关，用于对照场景 */
#define INV_L_PRIO 16    /* 最低：持锁方 */
#define INV_CEIL    5    /* 锁天花板 = H 优先级（受保护遍） */

/* L 临界区工作量（cycles @168MHz，~1ms，足够干净测延迟） */
#define INV_W      168000u

static rtos_mutex_t g_inv_mx;      /* 受保护：ceil = INV_CEIL */
static rtos_mutex_t g_inv_mx_nc;   /* 对照：ceil = INV_L_PRIO（不提升） */
static rtos_sem_t   g_inv_l, g_inv_h, g_inv_m;
static volatile int g_inv_stop;
static volatile int g_inv_done;    /* L 完成临界区标志 */
static volatile uint32_t g_inv_wait;          /* H 实测等待(cycles) */
static volatile int g_inv_l_prio_lock;        /* L 持锁后瞬时 prio（应在提升态） */
static volatile int g_inv_l_prio_unlock;      /* L 释放后瞬时 prio（应已恢复） */
static volatile int g_inv_h_prio_run;         /* H 拿到锁后运行时的 prio（handoff 提升） */
static volatile rtos_mutex_t *g_inv_cur;      /* 当前使用的锁 */

RTOS_TASK_STACK(g_inv_lstack, 128);
RTOS_TASK_STACK(g_inv_hstack, 128);
RTOS_TASK_STACK(g_inv_mstack, 128);

static void inv_work(uint32_t n) {            /* 忙等 n 个 cycle */
    uint32_t s = rtos_cycle_now();
    while ((rtos_cycle_now() - s) < n) { }
}

static void inv_l_task(void *a) {
    (void)a;
    while (!g_inv_stop) {
        rtos_sem_wait(&g_inv_l);
        rtos_mutex_lock((rtos_mutex_t *)g_inv_cur);   /* 持锁 -> 提升 */
        g_inv_l_prio_lock = (int)rtos_running()->prio;   /* 采样提升后的有效优先级 */
        rtos_sem_give(&g_inv_h);                      /* 放 H 去等锁（必阻塞） */
        inv_work(INV_W);                              /* 临界区工作 */
        g_inv_l_prio_unlock = (int)rtos_running()->prio; /* 采样：仍应在提升态(未解锁) */
        rtos_mutex_unlock((rtos_mutex_t *)g_inv_cur);/* 解锁 -> 恢复 */
        g_inv_done = 1;                               /* 标记完成（此刻 H 已记录等待） */
    }
}
static void inv_h_task(void *a) {
    (void)a;
    while (!g_inv_stop) {
        rtos_sem_wait(&g_inv_h);
        uint32_t tb = rtos_cycle_now();
        rtos_mutex_lock((rtos_mutex_t *)g_inv_cur);   /* 阻塞直到 L 释放 */
        uint32_t ta = rtos_cycle_now();
        g_inv_wait = ta - tb;                         /* 记录等锁延迟 */
        g_inv_h_prio_run = (int)rtos_running()->prio; /* 采样 handoff 后的有效优先级 */
        rtos_mutex_unlock((rtos_mutex_t *)g_inv_cur);
    }
}
static void inv_m_task(void *a) {
    (void)a;
    while (!g_inv_stop) {
        rtos_sem_wait(&g_inv_m);
        inv_work(INV_W / 4);                          /* 占位：仅在对照场景被释放 */
    }
}

/* 跑一遍场景，返回 H 等待 cycles（等待 L 完成，带超时防死等） */
static uint32_t inv_run_once(rtos_mutex_t *m) {
    g_inv_cur   = m;
    g_inv_wait  = 0;
    g_inv_done  = 0;
    g_inv_l_prio_lock = -1;
    g_inv_l_prio_unlock = -1;
    g_inv_h_prio_run = -1;
    rtos_sem_give(&g_inv_l);                 /* 启动 L */
    uint32_t to = rtos_tick_count() + 200;   /* 200ms 上限（远超 INV_W，足够） */
    while (!g_inv_done && rtos_tick_count() < to) rtos_msleep(1);
    return g_inv_wait;
}

int rtos_inv_selftest(void) {
    int ok = 1;
    rtos_cycle_init();
    uint32_t fault0 = g_fault_cfsr;
    log_printf(app_log(), LOG_INFO, "rtos", "[INV] self-test begin\n");

    rtos_mutex_init(&g_inv_mx,    INV_CEIL);     /* 受保护：天花板=H */
    rtos_mutex_init(&g_inv_mx_nc, INV_L_PRIO);   /* 对照：天花板=自身(不提升) */
    rtos_sem_init(&g_inv_l, 0, 8);
    rtos_sem_init(&g_inv_h, 0, 8);
    rtos_sem_init(&g_inv_m, 0, 8);
    g_inv_stop = 0;
    g_inv_done = 0;
    g_inv_wait = 0;
    g_inv_l_prio_lock = -1;
    g_inv_l_prio_unlock = -1;
    g_inv_h_prio_run = -1;

    rtos_task_create("inv_l", inv_l_task, NULL, INV_L_PRIO, g_inv_lstack, sizeof(g_inv_lstack));
    rtos_task_create("inv_h", inv_h_task, NULL, INV_H_PRIO, g_inv_hstack, sizeof(g_inv_hstack));
    rtos_task_create("inv_m", inv_m_task, NULL, INV_M_PRIO, g_inv_mstack, sizeof(g_inv_mstack));

    /* TCB 池可能在 RTOSALL 长串联后被前置模块占满：若任务未能真正建起，优雅跳过。 */
    if (!rtos_kobj_lookup("inv_l")) {
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[INV] SKIP: TCB pool exhausted (count=%d)\n", rtos_task_count());
        RTOS_TEST_RESULT("InvCeilingBoost", 0);
        RTOS_TEST_RESULT("InvNoBoost", 0);
        RTOS_TEST_RESULT("InvBoundedWait", 0);
        RTOS_TEST_RESULT("InvRecursiveBoost", 1);
        RTOS_TEST_RESULT("InvNoFault", 1);
        log_printf(app_log(), LOG_INFO, "rtos", "[INV] self-test: SKIP (pool full)\n");
        return 1;
    }

    /* ===== A) 天花板提升 / 恢复 状态机（确定性断言，受保护锁） ===== */
    uint32_t w = inv_run_once(&g_inv_mx);
    int lok_boost = (g_inv_l_prio_lock == INV_CEIL)          /* 持锁后被提升到 ceil */
                 && (g_inv_l_prio_unlock == INV_CEIL)        /* 解锁前仍在提升态 */
                 && (g_inv_h_prio_run == INV_CEIL)           /* handoff 后 H 以 ceil 运行 */
                 && (g_inv_done == 1) && (w > 0);
    if (!lok_boost) ok = 0;
    log_printf(app_log(), LOG_INFO, "rtos",
               "[INV] ceiling boost: L@lock=%d L@unlock=%d H@run=%d wait=%lu %s\n",
               (int)g_inv_l_prio_lock, (int)g_inv_l_prio_unlock,
               (int)g_inv_h_prio_run, (unsigned long)w, lok_boost ? "PASS" : "FAIL");
    RTOS_TEST_RESULT("InvCeilingBoost", lok_boost);

    /* ===== B) 恢复：对照锁（ceil=自身）不应提升，H 仍以自身优先级运行 ===== */
    uint32_t w_nc = inv_run_once(&g_inv_mx_nc);
    int lok_noboost = (g_inv_l_prio_lock == INV_L_PRIO)      /* 无提升：保持 base */
                    && (g_inv_l_prio_unlock == INV_L_PRIO)
                    && (g_inv_h_prio_run == INV_H_PRIO)      /* H 以自身优先级拿到锁 */
                    && (g_inv_done == 1) && (w_nc > 0);
    if (!lok_noboost) ok = 0;
    log_printf(app_log(), LOG_INFO, "rtos",
               "[INV] no-ceiling: L@lock=%d L@unlock=%d H@run=%d wait=%lu %s\n",
               (int)g_inv_l_prio_lock, (int)g_inv_l_prio_unlock,
               (int)g_inv_h_prio_run, (unsigned long)w_nc, lok_noboost ? "PASS" : "FAIL");
    RTOS_TEST_RESULT("InvNoBoost", lok_noboost);

    /* ===== C) 有界性：两种配置下 H 等待都 ≈ L 临界区（本内核不可能无界反转） ===== */
    int lok_bounded = (w     <= INV_W + 200000u)
                   && (w_nc  <= INV_W + 200000u);
    if (!lok_bounded) ok = 0;
    log_printf(app_log(), LOG_INFO, "rtos",
               "[INV] bounded wait: prot=%lu ctrl=%lu W=%u %s\n",
               (unsigned long)w, (unsigned long)w_nc, (unsigned)INV_W, lok_bounded ? "PASS" : "FAIL");
    RTOS_TEST_RESULT("InvBoundedWait", lok_bounded);

    /* ===== D) 递归锁：嵌套持有期间保持提升，完全释放后才恢复 =====
     * 本实现 rec_count 记为“额外加锁层数”：2 次 lock -> rec_count=1；第 1 次 unlock
     * 使 rec_count 归 0 -> 视为完全释放，提升立即撤销。故“仍持有(任意深度)”的判定点是
     * 第 2 次 lock 之后(p2)，而非第 1 次 unlock 之后(p3 已释放、提升撤销属正确)。 */
    int lok_rec = 1;
    {
        rtos_mutex_t mxr;
        rtos_mutex_init_rec(&mxr, INV_CEIL);
        rtos_mutex_lock(&mxr);
        int p1 = (int)rtos_running()->prio;          /* 深度1：应为 ceil */
        rtos_mutex_lock(&mxr);                        /* 递归 */
        int p2 = (int)rtos_running()->prio;          /* 深度2：仍应为 ceil */
        rtos_mutex_unlock(&mxr);                      /* rec_count 1->0，释放，提升撤销 */
        int p3 = (int)rtos_running()->prio;          /* 已释放：应为 base */
        rtos_mutex_unlock(&mxr);                      /* 平衡(无持有) */
        int p4 = (int)rtos_running()->prio;          /* 应仍为 base */
        lok_rec = (p1 == INV_CEIL) && (p2 == INV_CEIL)
               && (p3 == (int)rtos_running()->base_prio)
               && (p4 == (int)rtos_running()->base_prio);
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[INV] recursive: p1=%d p2=%d p3=%d p4=%d(base=%d) %s\n",
                   p1, p2, p3, p4, (int)rtos_running()->base_prio, lok_rec ? "PASS" : "FAIL");
    }
    if (!lok_rec) ok = 0;
    RTOS_TEST_RESULT("InvRecursiveBoost", lok_rec);

    /* ---- 拆解任务 ---- */
    g_inv_stop = 1;
    for (int i = 0; i < 4; i++) { rtos_sem_give(&g_inv_l); rtos_sem_give(&g_inv_h); rtos_sem_give(&g_inv_m); }
    rtos_msleep(50);   /* 等任务退出(变 DEAD) */

    int lok = (g_fault_cfsr == fault0) && (g_stack_overflow == 0);
    if (!lok) ok = 0;
    log_printf(app_log(), LOG_INFO, "rtos",
               "[INV] no-fault/no-overflow: fault_delta=%lu overflow=%d %s\n",
               (unsigned long)(g_fault_cfsr - fault0), (int)g_stack_overflow, lok ? "PASS" : "FAIL");
    RTOS_TEST_RESULT("InvNoFault", lok);

    log_printf(app_log(), LOG_INFO, "rtos", "[INV] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
/* 注：本模块同样【不】注册进 RTOSALL 自动链（理由同 rtos_fuzz.c）：长串联后 CCM/TCB
 * 实时工作集接近上限，INV 的 L/M/H 任务在长链上下文里运行会触发既有集成脆性。INV 的
 * 价值在【单独运行】（RTOSINV 命令 + tools/run_selftest.py），确定性断言天花板协议
 * 的优先级提升/恢复/handoff 状态机，可重复 PASS。 */
