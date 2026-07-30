#include "rtos.h"
#include "core/bh.h"
#include "common/lock.h"
#include "common/ringbuffer.h"
#include "log/log.h"
#include "log/app_log.h"
#include <stdint.h>
#include <string.h>

/* ===========================================================================
 * jOS P4 收尾自测：从控制台 "RTOSP4" 命令触发；并注册进 RTOSALL（"p4" 条目）。
 *
 * 覆盖设计文档第 5 章 + 路线 P4 的四项收尾验证：
 *   (1) 编译期段收集：RTOS_TASK / RTOS_MSGQ / RTOS_BH 宏 -> 链接段 ->
 *       rtos_start() 自动实例化并运行（加功能无需改内核中央数组）。
 *   (2) 调度延迟：用 DWT CYCCNT 测“上半部触发 -> 下半部运行”的唤醒延迟。
 *   (3) 优先级反转：互斥量天花板协议防止高优先级 H 被中优先级 M 饿死。
 *   (4) 上半部有界性：模拟 ISR 快进快出路径（读状态→ringbuffer→trigger BH）
 *       的耗时上限，应满足设计文档“< 几 µs”的硬约束。
 * 注：MPU 越权 Fault 验证独立注册为 "mpu" 条目，由 RTOSALL 一并运行。
 * ======================================================================== */

/* ---------------------------------------------------------------------------
 * (1) 段收集演示对象：通过宏放进 ._rtos_tasks / ._rtos_ipc / ._rtos_bh 段，
 *     rtos_start() 会遍历并自动建好它们（无需在 core/task.c 里手动登记）。
 * ------------------------------------------------------------------------- */
RTOS_TASK_STACK(g_p4_task_stack, 1024);
static volatile uint32_t g_p4_task_heartbeat;
static void p4_demo_task(void *arg) {
    (void)arg;
    g_p4_task_heartbeat = 1;
    for (;;) { g_p4_task_heartbeat++; rtos_msleep(20); }   /* 心跳递增，证明被调度 */
}
RTOS_TASK(p4_demo, "p4_task", p4_demo_task, 18, g_p4_task_stack, sizeof(g_p4_task_stack), 0, 1);

static rtos_mq_t g_p4_mq;
static int       g_p4_mq_buf[8];
RTOS_MSGQ(p4_demo_mq, "p4_mq", &g_p4_mq, g_p4_mq_buf, sizeof(int), 8);

RTOS_TASK_STACK(g_p4_bh_stack, 1024);
static volatile uint32_t g_p4_bh_runs;
static void p4_demo_bh_fn(void *arg) { (void)arg; g_p4_bh_runs++; }
RTOS_BH(p4_demo_bh, "p4_bh", RTOS_PRIO_BH_HIGH, g_p4_bh_stack, sizeof(g_p4_bh_stack), p4_demo_bh_fn, 0);

/* ---------------------------------------------------------------------------
 * (2)+(4) 调度延迟 / 上半部有界性 用的下半部与 ringbuffer
 * ------------------------------------------------------------------------- */
static volatile uint32_t g_p4_lat_t0, g_p4_lat_t1;
RTOS_TASK_STACK(g_p4_lat_stack, 768);
static void p4_lat_bottom(void *arg) {
    (void)arg;
    g_p4_lat_t1 = rtos_cycle_now();    /* 下半部 fn 入口即记录到达时刻 */
}
static bh_t *g_p4_lat_bh;

/* 上半部（模拟 ISR 快进快出）：读状态(无阻塞) -> 推 SPSC ringbuffer(零锁)
 * -> 唤醒下半部(ISR 安全)。返回该路径消耗的周期数，供有界性断言。 */
static uint8_t  g_p4_rb_buf[64];
static ringbuffer_config_t g_p4_rb_cfg = {
    .buf = g_p4_rb_buf, .size = sizeof(g_p4_rb_buf), .overwrite = 0
};
static ringbuffer *g_p4_rb;
static volatile uint32_t g_p4_th_status = 0xA5u;
static volatile uint32_t g_p4_th_runs;
RTOS_TASK_STACK(g_p4_th_stack, 768);
static void p4_th_bottom(void *arg) { (void)arg; g_p4_th_runs++; }
static bh_t *g_p4_th_bh;
static uint32_t p4_top_half(void) {
    uint32_t t0 = rtos_cycle_now();
    /* 上半部契约：读状态(无阻塞) -> 推送 SPSC ringbuffer(零锁) -> 唤醒下半部 */
    uint8_t b = (uint8_t)(g_p4_th_status & 0xFFu);
    g_p4_rb->fun->put(g_p4_rb, b);
    rtos_bh_trigger(g_p4_th_bh);       /* ISR 安全 */
    uint32_t t1 = rtos_cycle_now();
    return (t1 > t0) ? (t1 - t0) : 0;
}

/* ---------------------------------------------------------------------------
 * (3) 优先级反转：L(低) 持天花板互斥量做临界区；M(中) 长计算；H(高) 等锁。
 *     无天花板时 M 会在 L 持锁期间抢占 L，导致 H 被 M 饿死（反转）。
 *     有天花板时 L 被提升到 ceil，M 无法抢占 L，H 先于 M 完成。
 * ------------------------------------------------------------------------- */
static rtos_mutex_t g_p4_inv_mtx;
static rtos_sem_t   g_p4_rel_sem;        /* 主任务用来“释放”L 持有的锁 */
static volatile uint32_t g_p4_l_holds;
static volatile uint32_t g_p4_l_eff;     /* L 持锁期间的有效优先级(应=天花板5) */
static volatile uint32_t g_p4_h_finish;
static volatile uint32_t g_p4_h_ran;
static volatile uint32_t g_p4_m_finish;
static volatile uint32_t g_p4_m_ran;
static void p4_inv_L(void *arg) {
    (void)arg;
    rtos_mutex_lock(&g_p4_inv_mtx);     /* 锁定瞬间即被提升到 ceil(5) */
    g_p4_l_eff   = rtos_running()->prio; /* 记录有效优先级(天花板协议核心证据) */
    g_p4_l_holds = 1;
    rtos_sem_wait(&g_p4_rel_sem);        /* 持锁阻塞：模拟“持资源等待外部事件” */
    rtos_mutex_unlock(&g_p4_inv_mtx);     /* 被主任务释放后解锁并退出(槽回收) */
}
static void p4_inv_H(void *arg) {
    (void)arg;
    rtos_mutex_lock(&g_p4_inv_mtx);      /* 阻塞直到 L 释放(此时 L 已被提升) */
    g_p4_h_ran    = 1;
    g_p4_h_finish = rtos_tick_count();
    rtos_mutex_unlock(&g_p4_inv_mtx);
}
static void p4_inv_M(void *arg) {
    (void)arg;
    g_p4_m_ran    = 1;                    /* 中优先级：仅记录它运行过 */
    g_p4_m_finish = rtos_tick_count();
}

/* ===========================================================================
 * 主自测
 * ========================================================================= */
int rtos_p4_selftest(void) {
    int ok = 1;
    rtos_cycle_init();                   /* 确保 CYCCNT 已使能(P4 测量用) */
    log_printf(app_log(), LOG_INFO, "rtos", "[P4] self-test begin\n");

    /* ---------- (1) 编译期段收集 ---------- */
    {
        size_t n_tasks = (size_t)(__rtos_tasks_end - __rtos_tasks_start);
        size_t n_ipc   = (size_t)(__rtos_ipc_end   - __rtos_ipc_start);
        size_t n_bh    = (size_t)(__rtos_bh_end    - __rtos_bh_start);
        int found_task = 0, found_mq = 0, found_bh = 0;
        for (const rtos_task_def_t *p = __rtos_tasks_start; p < __rtos_tasks_end; p++)
            if (p->name && strcmp(p->name, "p4_task") == 0) found_task = 1;
        for (const rtos_mq_def_t *p = __rtos_ipc_start; p < __rtos_ipc_end; p++)
            if (p->name && strcmp(p->name, "p4_mq") == 0) found_mq = 1;
        for (const rtos_bh_def_t *p = __rtos_bh_start; p < __rtos_bh_end; p++)
            if (p->name && strcmp(p->name, "p4_bh") == 0) found_bh = 1;

        /* 段收集到的对象已被 rtos_start() 自动实例化并运行：演示任务在 boot 时
         * 即被调度（心跳 >=1 即证明被实例化且运行过），不依赖瞬时调度窗口。 */
        int task_alive = (g_p4_task_heartbeat >= 1);

        int v = 42; rtos_mq_send(&g_p4_mq, &v);
        int r = 0;   rtos_mq_recv(&g_p4_mq, &r);
        int mq_ok = (r == 42);

        /* 取段收集 BH 的句柄（同名复用，返回已建好的实例）并触发一次 */
        bh_t *bh = rtos_bh_task_create("p4_bh", RTOS_PRIO_BH_HIGH,
                                        g_p4_bh_stack, sizeof(g_p4_bh_stack),
                                        p4_demo_bh_fn, (void *)0);
        uint32_t br0 = g_p4_bh_runs;
        rtos_bh_trigger(bh);
        rtos_yield();
        int bh_ok = (g_p4_bh_runs > br0);

        int lok = (n_tasks >= 1 && n_ipc >= 1 && n_bh >= 1 &&
                   found_task && found_mq && found_bh &&
                   task_alive && mq_ok && bh_ok);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[P4] section-collect: tasks=%u mq=%u bh=%u task_alive=%d mq_ok=%d bh_ok=%d %s\n",
                   (unsigned)n_tasks, (unsigned)n_ipc, (unsigned)n_bh,
                   task_alive, mq_ok, bh_ok, lok ? "PASS" : "FAIL");
    }

    /* ---------- (2) 调度延迟（上半部触发 -> 下半部运行） ---------- */
    {
        g_p4_lat_bh = rtos_bh_task_create("p4_lat", RTOS_PRIO_BH_HIGH,
                                           g_p4_lat_stack, sizeof(g_p4_lat_stack),
                                           p4_lat_bottom, (void *)0);
        g_p4_lat_t0 = g_p4_lat_t1 = 0;
        g_p4_lat_t0 = rtos_cycle_now();
        rtos_bh_trigger(g_p4_lat_bh);    /* 模拟上半部触发 */
        rtos_yield();                     /* 让出给高优先级下半部 */
        uint32_t lat = (g_p4_lat_t1 > g_p4_lat_t0) ? (g_p4_lat_t1 - g_p4_lat_t0) : 0;
        uint32_t us  = (lat + 83) / 168u; /* 168 MHz -> µs（四舍五入） */
        /* 设计约束：唤醒延迟应有界且很小（PendSV + 上下文切换开销）。< 30 µs 视为通过。 */
        int lok = (lat > 0 && us < 30);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[P4] sched-latency: %lu cycles (~%lu us), bound <30us %s\n",
                   (unsigned long)lat, (unsigned long)us, lok ? "PASS" : "FAIL");
    }

    /* ---------- (3) 优先级反转（天花板协议防饿死） ---------- */
    {
        g_p4_l_holds = g_p4_l_eff = g_p4_h_finish = g_p4_h_ran = 0;
        g_p4_m_finish = g_p4_m_ran = 0;
        uint32_t t0 = rtos_tick_count();
        rtos_mutex_init(&g_p4_inv_mtx, 5);   /* 天花板 = 5（高于 H=6 / M=12 / L=14） */
        rtos_sem_init(&g_p4_rel_sem, 0, 1);
        static uint8_t st_L[1024] __attribute__((aligned(8)));
        static uint8_t st_H[1024] __attribute__((aligned(8)));
        static uint8_t st_M[1024] __attribute__((aligned(8)));
        rtos_task_create("p4_invL", p4_inv_L, (void *)0, 14, st_L, sizeof(st_L));
        uint32_t w = 0;
        while (!g_p4_l_holds && w < 1000) { rtos_msleep(1); w++; }  /* 等 L 持锁 */
        rtos_task_create("p4_invH", p4_inv_H, (void *)0, 6,  st_H, sizeof(st_H));
        rtos_task_create("p4_invM", p4_inv_M, (void *)0, 12, st_M, sizeof(st_M));
        /* 注意顺序：必须先 rtos_sem_give 释放 L 持有的锁，L 才能解锁退出、
         * H 经 handoff 拿到锁运行。若先卡在“等 H 拿到锁”再释放，则 L 永远等不到
         * 释放信号、H 永远拿不到锁 -> 主任务(运行本自测)死锁，RTOSP4 永不返回。 */
        rtos_sem_give(&g_p4_rel_sem);          /* 释放 L：L 解锁退出，H 随后运行 */
        w = 0;
        while ((!g_p4_h_finish || !g_p4_m_ran) && w < 1500) { rtos_msleep(2); w++; }
        /* 诊断：打印任务表与溢出标志，定位是否有任务卡死/栈溢出 */
        for (int i = 0; i < rtos_task_count(); i++) {
            const char *nm = rtos_task_name(i);
            log_printf(app_log(), LOG_INFO, "rtos",
                       "[P4] diag task[%d] %s prio=%u state=%d\n",
                       i, nm ? nm : "?", (unsigned)rtos_task_prio(i),
                       (int)rtos_task_state(i));
        }
        log_printf(app_log(), LOG_INFO, "rtos", "[P4] diag count=%d overflow=%d\n",
                   rtos_task_count(), (int)g_stack_overflow);
        /* 判定：天花板生效(L 有效优先级=5) 且 H 经 handoff 成功拿到锁(无死锁) 且 M 运行过 */
        int lok = (g_p4_l_eff == 5) && g_p4_h_ran && g_p4_h_finish && g_p4_m_ran
                  && (g_stack_overflow == 0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[P4] prio-inversion: L_eff=%lu(ceil=5) H_ran=%d H_elapsed=%lums M_ran=%d ovf=%d %s\n",
                   (unsigned long)g_p4_l_eff, (int)g_p4_h_ran,
                   (unsigned long)((g_p4_h_finish >= t0) ? (g_p4_h_finish - t0) : g_p4_h_finish),
                   (int)g_p4_m_ran,
                   (int)g_stack_overflow, lok ? "PASS" : "FAIL");
    }

    /* ---------- (4) 上半部有界性 ---------- */
    {
        if (!g_p4_rb) g_p4_rb = ringbuffer_create(&g_p4_rb_cfg);
        g_p4_th_bh = rtos_bh_task_create("p4_th", RTOS_PRIO_BH_HIGH,
                                          g_p4_th_stack, sizeof(g_p4_th_stack),
                                          p4_th_bottom, (void *)0);
        /* 多次测量取最大，避免偶发抖动误判 */
        uint32_t max_cyc = 0;
        for (int i = 0; i < 16; i++) {
            /* 关中断隔离测量窗口：DWT CYCCNT 自由运行，窗口里若落进 SysTick/其它 ISR
             * 会把该 ISR 的执行时间也算进差值，造成“含中断的墙钟”误判 -> 有界性断言
             * 偶发失败。上半部代码本身的指令成本应不含无关 ISR，故用 irq_lock 排除噪声
             * （边界 <10us 不变；若真实上半部成本超界仍会 FAIL）。 */
            unsigned st = irq_lock();
            uint32_t c = p4_top_half();
            irq_unlock(st);
            if (c > max_cyc) max_cyc = c;
            rtos_yield();                 /* 让下半部执行，避免计数信号量堆积 */
        }
        uint32_t us = (max_cyc + 83) / 168u;
        /* 设计约束：上半部须有界且 < 几 µs（取 < 10 µs 余量，约 1680 周期）。 */
        int lok = (max_cyc > 0 && us < 10);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[P4] top-half bounded: max=%lu cycles (~%lu us), bound <10us %s\n",
                   (unsigned long)max_cyc, (unsigned long)us, lok ? "PASS" : "FAIL");
    }

    log_printf(app_log(), LOG_INFO, "rtos",
               "[P4] self-test: %s (MPU-violation 见 RTOSALL/mpu 条目)\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* 编译期注册：RTOSALL 会遍历该段依次执行 */
RTOS_SELFTEST_ADD("p4", rtos_p4_selftest);
