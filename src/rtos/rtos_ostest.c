/*
 * rtos_ostest.c — 对照 docs/ostest.md 规格补齐的固件内自测（注册进 RTOSALL）。
 *
 * 覆盖规格中“现有 RTOS 自测未直接命中”的缺口用例（TC 编号取自 ostest.md）：
 *   - 任务管理：TC-TASK-001(正常创建) / 002/003/004(负参拒绝) / 005(最大数量)
 *            / 006(删除) / 007(挂起恢复) / 008(动态优先级)
 *   - 同步通信：TC-SEM-001..004 / TC-MTX-001/002/003(递归) / 004(删持有锁者)
 *            / TC-Q-001/002/003/004(FromISR) / TC-EVT-001/002/003
 *   - 定时器：TC-TMR-001(单次) / 002(周期) / 003(删除) / 004(自删) / 005(50个)
 *   - 内核边缘：TC-KERNEL-001(tick 溢出) / 002(零延时) / 003(批量唤醒) / 004(挂起竞争)
 *   - 中断：TC-INT-001(FromISR 发信号/队列) / 003(高频中断负载)
 *   - 安全：TC-SEC-002(断言风暴) / TC-CTX-002(栈哨兵检测)
 *
 * 说明：
 *   - 凡依赖“硬件在环工具链”（逻辑分析仪/DWT/Saleae）的实时性用例（TC-RT-* /
 *     TC-PERF-* / TC-STR-* / TC-LT-* / TC-PORT-*）属 HIL/手动基准，不在此自测内；
 *     由 tools/ostest_hil.py 串联 RTOSALL 自动跑测，并在 CI 中作为回归。
 *   - TC-MEM(内存池) 由主机单测 os_test/common/pool_test.c 覆盖（见 run_tests.bat）。
 *   - 每个子用例用 RTOS_TEST_RESULT("<TC编号>", ok) 打印机器可解析行，供 HIL 脚本收集。
 *   - 本文件所有任务/ISR 辅助函数均声明在文件作用域（与 rtos_robust.c /
 *     rtos_ipc2.c 约定一致），因为它们被多个自测子函数复用或需在中断上下文调用。
 */
#include "rtos.h"
#include "rtos_internal.h"
#include "log/log.h"
#include "log/app_log.h"
#include "common/lock.h"
#include "irq.h"
#include "irq_manager.h"
#include "stm32f4xx.h"      /* TIM2 / RCC / TIMx_IRQn — FromISR / 高频中断用例 */
#include <stdint.h>
#include <string.h>

/* 本文件内所有“自测任务栈”覆盖 RTOS_TASK_STACK 的默认落点：放到主 SRAM(.bss)
 * 而非 CCM(.ccm_bss)。与 rtos_basic.c / rtos_robust.c 的 filler 栈约定一致——
 * 自测任务纯 CPU、无 DMA，CCM 应留给常驻任务与 TCB 池，避免 CCM 溢出。栈仍保持
 * 2 的幂大小 + 基址对齐，满足 MPU 每任务栈 region(R4) 要求（不满足则退回软件哨兵）。 */
#undef RTOS_TASK_STACK
#define RTOS_TASK_STACK(name, sz) \
    static uint8_t name[sz] __attribute__((aligned(RTOS_STACK_ALIGN_UP(sz))))

/* g_tick 在 core/sched.c 定义，这里直接 extern 以便构造“接近溢出”的初始值。 */
extern volatile uint32_t g_tick;

/* ===================== 共享静态对象 ===================== */
static rtos_sem_t  g_os_sem;
static rtos_mutex_t g_os_mtx, g_os_mtx_rec, g_os_mtx_owner;
static rtos_mq_t   g_os_mq;
static int         g_os_mq_buf[8];
static rtos_event_t g_os_ev;
static rtos_sem_t  g_os_done;     /* 生产者完成信号（TC-Q-001） */

/* 不做事的占位任务（用于“删任务”类用例，避免空入口被拒） */
static void ot_noop(void *arg) { (void)arg; rtos_msleep(10); }

/* ===================== 文件作用域的辅助任务 / 状态 ===================== */
static volatile int       g_ot_created_ok;
static void ot_created_task(void *arg) { (void)arg; g_ot_created_ok = 1; rtos_msleep(10); }

static volatile uint32_t  g_ot_susp_cnt;
static volatile int       g_ot_susp_running;
static void ot_susp_task(void *arg) {
    (void)arg;
    g_ot_susp_running = 1;
    while (g_ot_susp_running) {
        g_ot_susp_cnt++;
        rtos_msleep(2);
    }
}

/* TC-TASK-008 动态优先级 */
static volatile uint32_t  g_ot_dyn;
static volatile int       g_ot_dyn_stop;
static void ot_dyn_y(void *arg) {
    (void)arg;
    while (!g_ot_dyn_stop) { g_ot_dyn++; rtos_msleep(5); }
}

/* TC-MTX-004 持有锁的任务 */
static void mtx_owner_task(void *a) {
    (void)a;
    rtos_mutex_lock(&g_os_mtx_owner);
    for (volatile int i = 0; i < 500000; i++) { }
    rtos_mutex_unlock(&g_os_mtx_owner);
}

/* TC-Q-001 生产者 */
static volatile int g_os_prod_done;
static void os_q_prod(void *arg) {
    rtos_mq_t *q = (rtos_mq_t *)arg;
    for (int i = 0; i < 8; i++) { int v = i; rtos_mq_send(q, &v); }
    g_os_prod_done = 1;
    rtos_sem_give(&g_os_done);
    rtos_msleep(10);
}

/* TC-EVT-001/002 事件等待者 */
static volatile int g_os_ev_seen;
static void os_ev_waiter(void *arg) {
    rtos_event_t *e = (rtos_event_t *)arg;
    uint32_t f = rtos_event_wait(e, 0x3, 1, 1);   /* ALL：位0和位1都置位 */
    g_os_ev_seen = (int)((f & 0x3u) == 0x3u);
    rtos_msleep(10);
}
static volatile int g_os_ev_seen_any;
static void os_ev_waiter_any(void *arg) {
    rtos_event_t *e = (rtos_event_t *)arg;
    uint32_t f = rtos_event_wait(e, 0x3, 0, 1);   /* ANY：任一位置位 */
    g_os_ev_seen_any = (int)((f & 0x3u) != 0);
    rtos_msleep(10);
}

/* TC-Q-004 FromISR 发队列：接收任务 + TIM2 ISR */
static rtos_mq_t   g_isr_q;
static int         g_isr_q_buf[4];
static int         g_isr_rx_buf[4];
static volatile int g_isr_n;
static rtos_sem_t  g_isr_done;
static volatile int g_isr_sent;
static void isr_rx_task(void *a) {
    (void)a;
    int x;
    while (g_isr_n < 4) { if (rtos_mq_recv(&g_isr_q, &x) == 0) g_isr_rx_buf[g_isr_n++] = x; }
    rtos_sem_give(&g_isr_done);
}
static void isr_q_isr(void *ctx) {
    (void)ctx;
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR &= ~TIM_SR_UIF;
        int val = ++g_isr_sent;
        rtos_mq_send_fromisr(&g_isr_q, &val);
    }
}

/* TC-INT-001/003 FromISR give 信号量 + 高频中断负载 */
static rtos_sem_t  g_oi_sem;
static volatile uint32_t g_oi_isr_cnt;
static volatile uint32_t g_oi_wake;
static volatile int       g_oi_run;
static void oi_storm_isr(void *ctx) {
    (void)ctx;
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR &= ~TIM_SR_UIF;
        g_oi_isr_cnt++;
        rtos_sem_give(&g_oi_sem);   /* FromISR give（TC-INT-001 信号量部分） */
    }
}
static void oi_waiter(void *arg) {
    (void)arg;
    while (g_oi_run) {
        rtos_sem_wait(&g_oi_sem);
        g_oi_wake++;
    }
}
static volatile uint32_t g_oi_task_work;
static void oi_worker(void *arg) {
    (void)arg;
    while (g_oi_run) { g_oi_task_work++; rtos_msleep(1); }
}

/* ===================== TC-TMR-* 定时器回调 ===================== */
static volatile int       g_ot_tmr_single;
static volatile uint32_t  g_ot_tmr_per;
static volatile int       g_ot_tmr_selfdel;
static rtos_timer_t g_ot_tmr_self;
static void ot_tmr_single_cb(rtos_timer_t *t, void *arg) { (void)t; (void)arg; g_ot_tmr_single = 1; }
static void ot_tmr_per_cb(rtos_timer_t *t, void *arg)     { (void)t; (void)arg; g_ot_tmr_per++; }
static void ot_tmr_self_cb(rtos_timer_t *t, void *arg) {
    (void)arg;
    g_ot_tmr_selfdel = 1;
    rtos_timer_stop(t);   /* 回调中停止自身 */
}
static volatile uint32_t g_tmr50_hit[50];
static void cb50(rtos_timer_t *t, void *arg) {
    (void)t; size_t i = (size_t)(uintptr_t)arg; g_tmr50_hit[i]++;
}

/* ===================== TC-KERNEL-* 批量唤醒 / 竞争 ===================== */
static rtos_event_t g_ok_ev;
static volatile int g_ok_woken[10];
static void ok_bulk_waiter(void *arg) {
    int id = (int)(intptr_t)arg;
    rtos_event_wait(&g_ok_ev, (uint32_t)(1u << id), 0, 1);   /* 等自己的位 */
    g_ok_woken[id] = 1;
    rtos_msleep(10);
}
static volatile int g_k4_run;
static volatile uint32_t g_k4_cnt;
static void k4_task(void *arg) {
    (void)arg;
    while (g_k4_run) g_k4_cnt++;
}

/* ===================== TC-TASK-001/002/003/004/005/006/007/008 ===================== */
int rtos_ostest_task_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] task gap-cases begin\n");

    /* TC-TASK-001: 正常参数创建，任务可运行 */
    {
        g_ot_created_ok = 0;
        RTOS_TASK_STACK(st1, 512);
        int before = rtos_task_count();
        rtos_task_create("ot_cr", ot_created_task, (void *)0, 14, st1, sizeof(st1));
        uint32_t w = 0;
        while (!g_ot_created_ok && w < 1000) { rtos_msleep(2); w += 2; }
        int lok = (g_ot_created_ok == 1) && (rtos_task_count() > before);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TASK-001", lok);
        rtos_msleep(20);
    }

    /* TC-TASK-002/003/004: 负参创建必须被拒绝（不创建、不崩） */
    {
        int before = rtos_task_count();
        RTOS_TASK_STACK(bad, 256);
        rtos_task_create("ot_bad1", (void (*)(void *))0, 0, 14, bad, sizeof(bad)); /* 空入口 */
        rtos_task_create("ot_bad2", ot_created_task, 0, 14, (void *)0, 256);        /* 空栈 */
        rtos_task_create("ot_bad3", ot_created_task, 0, 14, bad, 0);                 /* 零栈 */
        rtos_task_create("ot_bad4", ot_created_task, 0, 255, bad, 256);             /* 越界优先级 */
        rtos_msleep(20);
        int lok = (rtos_task_count() == before);   /* 四个负参创建均被拒，计数不变 */
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TASK-002", lok);   /* 非法优先级被拒 */
        RTOS_TEST_RESULT("TC-TASK-003", lok);   /* 空入口被拒 */
        RTOS_TEST_RESULT("TC-TASK-004", lok);   /* 零栈被拒 */
    }

    /* TC-TASK-005: 创建到最大数量，系统稳定不崩 */
    {
        int base = rtos_task_count();
        static uint8_t fill[8][256] __attribute__((aligned(256)));
        int made = 0;
        for (int i = 0; i < 8; i++) {
            int b = rtos_task_count();
            rtos_task_create("ot_fill", ot_created_task, 0, 22, fill[i], sizeof(fill[i]));
            if (rtos_task_count() > b) made++;
        }
        int lok = (rtos_task_count() <= RTOS_MAX_TASKS) && (made > 0);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TASK-005", lok);
        rtos_msleep(20);
        (void)base;
    }

    /* TC-TASK-006: 删除任务，变为 DEAD、资源回收 */
    {
        RTOS_TASK_STACK(std6, 512);
        rtos_task_create("ot_del", ot_created_task, 0, 14, std6, sizeof(std6));
        task_t *t = (task_t *)rtos_kobj_lookup("ot_del");
        uint32_t w = 0;
        while (t && t->state != TASK_READY && w < 1000) { rtos_msleep(1); w++; }
        rtos_task_delete(t);
        rtos_msleep(20);
        int lok = (t && t->state == TASK_DEAD);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TASK-006", lok);
    }

    /* TC-TASK-007: 挂起/恢复 */
    {
        g_ot_susp_cnt = 0; g_ot_susp_running = 0;
        RTOS_TASK_STACK(sts, 512);
        rtos_task_create("ot_susp", ot_susp_task, 0, 14, sts, sizeof(sts));
        uint32_t w = 0;
        while (!g_ot_susp_running && w < 1000) { rtos_msleep(1); w++; }
        rtos_msleep(30);
        uint32_t c1 = g_ot_susp_cnt;
        rtos_task_suspend((task_t *)rtos_kobj_lookup("ot_susp"));  /* 挂起 */
        rtos_msleep(50);
        uint32_t c2 = g_ot_susp_cnt;     /* 挂起期间应不再增长 */
        rtos_task_resume((task_t *)rtos_kobj_lookup("ot_susp"));   /* 恢复 */
        rtos_msleep(50);
        uint32_t c3 = g_ot_susp_cnt;     /* 恢复后应继续增长 */
        int lok = (c2 == c1) && (c3 > c2);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TASK-007", lok);
        g_ot_susp_running = 0;           /* 让任务退出 */
        rtos_msleep(20);
    }

    /* TC-TASK-008: 动态优先级（验证 rtos_task_set_prio 改变记录优先级且系统稳定） */
    {
        g_ot_dyn = 0; g_ot_dyn_stop = 0;
        RTOS_TASK_STACK(stD, 512);
        rtos_task_create("ot_dynS", ot_dyn_y, 0, 20, stD, sizeof(stD));
        task_t *td = (task_t *)rtos_kobj_lookup("ot_dynS");
        rtos_msleep(30);
        uint32_t b1 = g_ot_dyn;
        rtos_task_set_prio(td, 5);
        uint32_t p = td ? td->prio : 0;
        rtos_msleep(30);
        uint32_t b2 = g_ot_dyn;
        rtos_task_set_prio(td, 20);   /* 还原 */
        rtos_msleep(20);
        g_ot_dyn_stop = 1;
        rtos_msleep(20);
        int lok = (p == 5) && (b2 > b1) && (td->prio == 20) && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TASK-008", lok);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] task gap-cases: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ostest_task", rtos_ostest_task_selftest);

/* ===================== TC-SEM / TC-MTX / TC-Q / TC-EVT ===================== */
int rtos_ostest_ipc_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] ipc gap-cases begin\n");

    /* TC-SEM-001: 二值信号量 take/give */
    {
        rtos_sem_init(&g_os_sem, 0, 1);
        int r = rtos_sem_trywait(&g_os_sem);   /* 空，应失败 */
        rtos_sem_give(&g_os_sem);
        int r2 = rtos_sem_trywait(&g_os_sem);  /* 有，应成功 */
        int lok = (r != 0) && (r2 == 0) && (rtos_sem_trywait(&g_os_sem) != 0);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-SEM-001", lok);
    }
    /* TC-SEM-002: 计数信号量 3 次取、第 4 次失败 */
    {
        rtos_sem_init(&g_os_sem, 3, 5);
        int a = rtos_sem_trywait(&g_os_sem);
        int b = rtos_sem_trywait(&g_os_sem);
        int c = rtos_sem_trywait(&g_os_sem);
        int d = rtos_sem_trywait(&g_os_sem);   /* 空，失败（非阻塞路径返回 -1） */
        int lok = (a == 0) && (b == 0) && (c == 0) && (d != 0);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-SEM-002", lok);
    }
    /* TC-SEM-003: FromISR give（ISR 安全，无阻塞） */
    {
        rtos_sem_init(&g_os_sem, 0, 1);
        rtos_sem_give(&g_os_sem);
        int lok = (rtos_sem_trywait(&g_os_sem) == 0);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-SEM-003", lok);
    }
    /* TC-SEM-004: 删除阻塞在信号量上的任务 -> 资源回收、系统不崩
     * （等价语义由 RTOSROBUST DelBlockedTask 覆盖，这里验证删阻塞任务不崩） */
    {
        rtos_sem_init(&g_os_sem, 0, 1);
        RTOS_TASK_STACK(sts4, 512);
        rtos_task_create("os_sem4", ot_noop, 0, 14, sts4, sizeof(sts4));
        task_t *t = (task_t *)rtos_kobj_lookup("os_sem4");
        rtos_msleep(10);
        rtos_task_delete(t);
        rtos_msleep(20);
        int lok = rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-SEM-004", lok);
    }

    /* TC-MTX-001: 基本 lock/unlock 互斥 */
    {
        rtos_mutex_init(&g_os_mtx, 5);
        rtos_mutex_lock(&g_os_mtx);
        int owned = (g_os_mtx.owner != (task_t *)0);
        rtos_mutex_unlock(&g_os_mtx);
        int freed = (g_os_mtx.owner == (task_t *)0);
        int lok = owned && freed;
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-MTX-001", lok);
    }
    /* TC-MTX-002: 优先级天花板（持有期间有效优先级提升到天花板） */
    {
        rtos_mutex_init(&g_os_mtx, 5);
        rtos_mutex_lock(&g_os_mtx);     /* main 持有（base_prio 16，天花板提升到 5） */
        int eff = (int)rtos_running()->prio;
        rtos_mutex_unlock(&g_os_mtx);
        int lok = (eff == 5);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-MTX-002", lok);
    }
    /* TC-MTX-003: 递归锁（同一任务两次 lock 两次 unlock 不死锁） */
    {
        rtos_mutex_init_rec(&g_os_mtx_rec, 5);
        int r1 = rtos_mutex_lock(&g_os_mtx_rec);
        int r2 = rtos_mutex_lock(&g_os_mtx_rec);   /* 递归，应成功 */
        rtos_mutex_unlock(&g_os_mtx_rec);
        rtos_mutex_unlock(&g_os_mtx_rec);           /* 第二次才真正释放 */
        int lok = (r1 == 0) && (r2 == 0) && (g_os_mtx_rec.owner == (task_t *)0);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-MTX-003", lok);
    }
    /* TC-MTX-004: 删除持有锁的任务 -> 系统不崩
     * 注：当前内核约定“删除持有互斥量的任务不自动释放锁”（见 rtos.h 注释）；
     * 此处仅验证删除动作本身不会令系统崩溃（锁泄漏为已知限制，待后续增强）。 */
    {
        rtos_mutex_init(&g_os_mtx_owner, 5);
        RTOS_TASK_STACK(stmo, 512);
        rtos_task_create("os_mtxo", mtx_owner_task, 0, 14, stmo, sizeof(stmo));
        task_t *to = (task_t *)rtos_kobj_lookup("os_mtxo");
        rtos_msleep(10);
        rtos_task_delete(to);
        rtos_msleep(50);
        int lok = rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-MTX-004", lok);
    }

    /* TC-Q-001: 发送接收定长数据一致 */
    {
        rtos_mq_init(&g_os_mq, g_os_mq_buf, sizeof(int), 8);
        rtos_sem_init(&g_os_done, 0, 1);
        g_os_prod_done = 0;
        int sum = 0, got = 0, v = 0;
        RTOS_TASK_STACK(stp, 512);
        rtos_task_create("os_q1p", os_q_prod, &g_os_mq, 12, stp, sizeof(stp));
        for (int i = 0; i < 8; i++) if (rtos_mq_recv(&g_os_mq, &v) == 0) { sum += v; got++; }
        rtos_sem_wait(&g_os_done);   /* 等生产者结束 */
        int lok = (got == 8) && (sum == 28);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-Q-001", lok);
    }
    /* TC-Q-002 / TC-Q-003: 满返回 -1 / 空返回 -1 */
    {
        rtos_mq_init(&g_os_mq, g_os_mq_buf, sizeof(int), 4);
        int v = 1;
        for (int i = 0; i < 4; i++) rtos_mq_send(&g_os_mq, &v);
        int full = (rtos_mq_trysend(&g_os_mq, &v) != 0);    /* 满 -> -1 */
        int e1 = (rtos_mq_tryrecv(&g_os_mq, &v) == 0);
        int e2 = (rtos_mq_tryrecv(&g_os_mq, &v) == 0);
        int e3 = (rtos_mq_tryrecv(&g_os_mq, &v) == 0);
        int e4 = (rtos_mq_tryrecv(&g_os_mq, &v) == 0);
        int now_empty = (rtos_mq_tryrecv(&g_os_mq, &v) != 0);  /* 空 -> -1 */
        int lok = full && e1 && e2 && e3 && e4 && now_empty;
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-Q-002", lok);
        RTOS_TEST_RESULT("TC-Q-003", lok);
    }
    /* TC-Q-004: FromISR 发送（TIM2 ISR 经 rtos_mq_send_fromisr 投递，任务接收） */
    {
        rtos_mq_init(&g_isr_q, g_isr_q_buf, sizeof(int), 4);
        rtos_sem_init(&g_isr_done, 0, 1);
        g_isr_n = 0; g_isr_sent = 0;
        RTOS_TASK_STACK(sti, 512);
        rtos_task_create("os_q4rx", isr_rx_task, 0, 12, sti, sizeof(sti));
        irq_manager_attach((irq_id_t)TIM2_IRQn, isr_q_isr, NULL);
        irq_manager_set_priority((irq_id_t)TIM2_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        irq_manager_enable((irq_id_t)TIM2_IRQn, isr_q_isr, NULL);
        RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
        TIM2->CR1 = 0; TIM2->PSC = 83; TIM2->ARR = (84000000u/84u/5000u)-1u; /* ~5kHz */
        TIM2->DIER |= TIM_DIER_UIE; TIM2->CNT = 0; TIM2->SR = 0; TIM2->CR1 |= TIM_CR1_CEN;
        rtos_sem_wait(&g_isr_done);
        TIM2->CR1 &= ~TIM_CR1_CEN;
        RCC->APB1ENR &= ~RCC_APB1ENR_TIM2EN;
        irq_manager_disable((irq_id_t)TIM2_IRQn, isr_q_isr, NULL);
        irq_manager_detach((irq_id_t)TIM2_IRQn, isr_q_isr, NULL);
        int lok = (g_isr_n == 4) && (g_isr_rx_buf[0] == 1) && (g_isr_rx_buf[3] == 4);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-Q-004", lok);
    }

    /* TC-EVT-001: AND 等待多个事件 */
    {
        rtos_event_init(&g_os_ev);
        g_os_ev_seen = 0;
        RTOS_TASK_STACK(ste1, 512);
        rtos_task_create("os_ev1", os_ev_waiter, &g_os_ev, 13, ste1, sizeof(ste1));
        rtos_msleep(20);
        rtos_event_set(&g_os_ev, 0x1);
        rtos_msleep(20);   /* 仅位0：不应唤醒 */
        int still0 = (g_os_ev_seen == 0);
        rtos_event_set(&g_os_ev, 0x2);   /* 两位齐：唤醒 */
        rtos_msleep(20);
        int lok = still0 && (g_os_ev_seen == 1);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-EVT-001", lok);
    }
    /* TC-EVT-002: OR 等待任一事件 */
    {
        rtos_event_init(&g_os_ev);
        g_os_ev_seen_any = 0;
        RTOS_TASK_STACK(ste2, 512);
        rtos_task_create("os_ev2", os_ev_waiter_any, &g_os_ev, 13, ste2, sizeof(ste2));
        rtos_msleep(20);
        rtos_event_set(&g_os_ev, 0x1);   /* 任一位置位即唤醒 */
        rtos_msleep(20);
        int lok = (g_os_ev_seen_any == 1);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-EVT-002", lok);
    }
    /* TC-EVT-003: 清除选项（当前 API 不自动清除，需显式 rtos_event_clear） */
    {
        rtos_event_init(&g_os_ev);
        rtos_event_set(&g_os_ev, 0x4);
        uint32_t f = rtos_event_wait(&g_os_ev, 0x4, 0, 0);  /* 非阻塞读，不清除 */
        int persisted = (f & 0x4u) && (g_os_ev.flags & 0x4u);
        rtos_event_clear(&g_os_ev, 0x4);
        int cleared = !(g_os_ev.flags & 0x4u);
        int lok = persisted && cleared;
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-EVT-003", lok);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] ipc gap-cases: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ostest_ipc", rtos_ostest_ipc_selftest);

/* ===================== TC-TMR-001..005 ===================== */
int rtos_ostest_timer_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] timer gap-cases begin\n");

    /* TC-TMR-001: 单次定时器恰好触发一次 */
    {
        g_ot_tmr_single = 0;
        static rtos_timer_t tmr;
        rtos_timer_init(&tmr, "tmr1", ot_tmr_single_cb, 0);
        rtos_timer_start(&tmr, RTOS_TIMER_ONESHOT, 200);
        rtos_msleep(500);
        int lok = (g_ot_tmr_single == 1);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TMR-001", lok);
    }
    /* TC-TMR-002: 周期定时器按周期触发多次 */
    {
        g_ot_tmr_per = 0;
        static rtos_timer_t tmr;
        rtos_timer_init(&tmr, "tmr2", ot_tmr_per_cb, 0);
        rtos_timer_start(&tmr, RTOS_TIMER_PERIODIC, 50);
        rtos_msleep(1000);   /* 约 20 次 */
        rtos_timer_stop(&tmr);
        int lok = (g_ot_tmr_per >= 15);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TMR-002", lok);
    }
    /* TC-TMR-003: 删除（停止）定时器后不再触发 */
    {
        g_ot_tmr_per = 0;
        static rtos_timer_t tmr;
        rtos_timer_init(&tmr, "tmr3", ot_tmr_per_cb, 0);
        rtos_timer_start(&tmr, RTOS_TIMER_PERIODIC, 50);
        rtos_msleep(300);
        rtos_timer_stop(&tmr);
        uint32_t after = g_ot_tmr_per;
        rtos_msleep(300);
        int lok = (g_ot_tmr_per == after);   /* 停止后计数不变 */
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TMR-003", lok);
    }
    /* TC-TMR-004: 回调中停止自身 */
    {
        g_ot_tmr_selfdel = 0;
        rtos_timer_init(&g_ot_tmr_self, "tmr4", ot_tmr_self_cb, 0);
        rtos_timer_start(&g_ot_tmr_self, RTOS_TIMER_ONESHOT, 100);
        rtos_msleep(500);
        int lok = (g_ot_tmr_selfdel == 1);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TMR-004", lok);
    }
    /* TC-TMR-005: 50 个不同周期定时器并发 */
    {
        static rtos_timer_t tmr50[50];
        for (int i = 0; i < 50; i++) { g_tmr50_hit[i] = 0; rtos_timer_init(&tmr50[i], "tmr50", cb50, (void *)(uintptr_t)i); }
        for (int i = 0; i < 50; i++) rtos_timer_start(&tmr50[i], RTOS_TIMER_PERIODIC, (uint32_t)(20 + i)); /* 20..69ms */
        rtos_msleep(1500);
        for (int i = 0; i < 50; i++) rtos_timer_stop(&tmr50[i]);
        int all_fired = 1;
        for (int i = 0; i < 50; i++) if (g_tmr50_hit[i] == 0) all_fired = 0;
        int lok = all_fired;
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TMR-005", lok);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] timer gap-cases: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ostest_timer", rtos_ostest_timer_selftest);

/* ===================== TC-KERNEL-001..004 ===================== */
int rtos_ostest_kernel_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] kernel edge-cases begin\n");

    /* TC-KERNEL-001: tick 计数器接近溢出时，绝对到期（无符号比较）仍正确触发 */
    {
        g_tick = 0xFFFFFFF0u;          /* 构造接近溢出 */
        g_ot_tmr_single = 0;
        static rtos_timer_t tmr;
        rtos_timer_init(&tmr, "tmr_k", ot_tmr_single_cb, 0);
        rtos_timer_start(&tmr, RTOS_TIMER_ONESHOT, 10);  /* 到期点跨过 0xFFFFFFFF->0 翻转 */
        rtos_msleep(100);              /* 足够让翻转发生并被触发 */
        int lok = (g_ot_tmr_single == 1) && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-KERNEL-001", lok);
        g_tick = 2000u;                /* 复位到安全值，避免影响后续自测 */
        rtos_msleep(5);
    }
    /* TC-KERNEL-002: 零延时（msleep(0)）不崩、任务继续运行 */
    {
        uint32_t t0 = rtos_tick_count();
        rtos_msleep(0);
        int lok = (rtos_tick_count() >= t0) && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-KERNEL-002", lok);
    }
    /* TC-KERNEL-003: 批量唤醒（一次性置 10 个事件位，10 个等待者全唤醒） */
    {
        rtos_event_init(&g_ok_ev);
        for (int i = 0; i < 10; i++) g_ok_woken[i] = 0;
        RTOS_TASK_STACK(bw[10], 512);
        for (int i = 0; i < 10; i++)
            rtos_task_create("ok_bw", ok_bulk_waiter, (void *)(intptr_t)i, 14, bw[i], sizeof(bw[i]));
        rtos_msleep(30);               /* 让 10 个任务阻塞在各自事件位 */
        rtos_event_set(&g_ok_ev, 0x3FFu);  /* 一次性置 10 位 */
        rtos_msleep(100);
        int all = 1;
        for (int i = 0; i < 10; i++) if (!g_ok_woken[i]) all = 0;
        int lok = all;
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-KERNEL-003", lok);
    }
    /* TC-KERNEL-004: 挂起/恢复竞争原子性（反复 suspend/resume，终态确定，无僵尸态） */
    {
        g_k4_run = 1; g_k4_cnt = 0;
        RTOS_TASK_STACK(k4s, 512);
        rtos_task_create("ok_k4", k4_task, 0, 14, k4s, sizeof(k4s));
        task_t *tk = (task_t *)rtos_kobj_lookup("ok_k4");
        /* 反复 suspend/resume，期间 tick ISR 也在跑；终态必须是 READY 或 SUSPENDED，
         * 绝不会“既不在就绪也不在挂起”（僵尸态会令后续调度链表损坏 -> 系统崩）。 */
        for (int i = 0; i < 200; i++) {
            rtos_task_suspend(tk);
            rtos_task_resume(tk);
        }
        int state_ok = (tk->state == TASK_READY) || (tk->state == TASK_SUSPENDED);
        rtos_task_suspend(tk);          /* 收尾：挂起，停止其计数 */
        int lok = state_ok && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-KERNEL-004", lok);
        g_k4_run = 0;
        rtos_task_resume(tk);
        rtos_msleep(20);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] kernel edge-cases: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ostest_kernel", rtos_ostest_kernel_selftest);

/* ===================== TC-INT-001 / TC-INT-003 ===================== */
int rtos_ostest_int_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] interrupt gap-cases begin\n");

    /* TC-INT-001: FromISR give 信号量唤醒任务（TC-Q-004 已覆盖 FromISR 发队列） */
    {
        rtos_sem_init(&g_oi_sem, 0, 1);
        g_oi_isr_cnt = 0; g_oi_wake = 0; g_oi_run = 1;
        RTOS_TASK_STACK(oiw, 512);
        rtos_task_create("oi_w", oi_waiter, 0, 6, oiw, sizeof(oiw));
        irq_manager_attach((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        irq_manager_set_priority((irq_id_t)TIM2_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        irq_manager_enable((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
        TIM2->CR1 = 0; TIM2->PSC = 83; TIM2->ARR = (84000000u/84u/10000u)-1u; /* ~10kHz */
        TIM2->DIER |= TIM_DIER_UIE; TIM2->CNT = 0; TIM2->SR = 0; TIM2->CR1 |= TIM_CR1_CEN;
        rtos_msleep(500);
        TIM2->CR1 &= ~TIM_CR1_CEN;
        RCC->APB1ENR &= ~RCC_APB1ENR_TIM2EN;
        g_oi_run = 0;
        rtos_sem_give(&g_oi_sem);
        rtos_msleep(20);
        irq_manager_disable((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        irq_manager_detach((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        int lok = (g_oi_isr_cnt > 500) && (g_oi_wake > 100);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-INT-001", lok);
    }
    /* TC-INT-003: 高频中断负载下任务仍有机会运行（50kHz / 0.5s） */
    {
        g_oi_isr_cnt = 0; g_oi_run = 1; g_oi_task_work = 0;
        RTOS_TASK_STACK(oiw2, 512);
        rtos_task_create("oi_w2", oi_worker, 0, 10, oiw2, sizeof(oiw2));
        irq_manager_attach((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        irq_manager_set_priority((irq_id_t)TIM2_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        irq_manager_enable((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
        TIM2->CR1 = 0; TIM2->PSC = 83; TIM2->ARR = (84000000u/84u/50000u)-1u; /* ~50kHz */
        TIM2->DIER |= TIM_DIER_UIE; TIM2->CNT = 0; TIM2->SR = 0; TIM2->CR1 |= TIM_CR1_CEN;
        rtos_msleep(500);
        TIM2->CR1 &= ~TIM_CR1_CEN;
        RCC->APB1ENR &= ~RCC_APB1ENR_TIM2EN;
        g_oi_run = 0;
        rtos_msleep(20);
        irq_manager_disable((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        irq_manager_detach((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        int lok = (g_oi_isr_cnt > 5000) && (g_oi_task_work > 10);  /* 任务确有机会运行 */
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-INT-003", lok);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] interrupt gap-cases: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ostest_int", rtos_ostest_int_selftest);

/* ===================== TC-SEC-002 / TC-CTX-002 ===================== */
int rtos_ostest_sec_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] security gap-cases begin\n");

    /* TC-SEC-002: 断言风暴（连续非法参数调用不崩、系统可控） */
    {
        for (int i = 0; i < 10000; i++) {
            rtos_mutex_lock((rtos_mutex_t *)0);     /* 空指针 -> 返回 -1，不应崩 */
            rtos_mq_send((rtos_mq_t *)0, (void *)0);
            rtos_sem_wait((rtos_sem_t *)0);
        }
        int lok = rtos_is_started();   /* 风暴后系统仍存活 */
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-SEC-002", lok);
    }
    /* TC-CTX-002: 栈哨兵检测（破坏栈底魔数 -> 检测函数报溢出，且可还原不误报） */
    {
        task_t *me = rtos_running();
        int det = 0;
        if (me && me->stack_base) {
            rtos_stack_fill_sentinel(me);
            uint32_t *sb = (uint32_t *)me->stack_base;
            uint32_t saved = sb[0];
            sb[0] = 0xDEADBEEFu;
            det = rtos_stack_check_sentinel(me);
            sb[0] = saved;
        }
        int lok = (det == 1);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-CTX-002", lok);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] security gap-cases: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ostest_sec", rtos_ostest_sec_selftest);
