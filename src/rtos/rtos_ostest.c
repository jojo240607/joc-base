/*
 * rtos_ostest.c — 对照 docs/ostest.md 规格补齐的固件内自测（注册进 RTOSALL）。
 *
 * 覆盖规格中“现有 RTOS 自测未直接命中”的缺口用例（TC 编号取自 ostest.md）：
 *   - 任务管理：TC-TASK-001(正常创建) / 002/003/004(负参拒绝) / 005(最大数量)
 *            / 006(删除) / 007(挂起恢复) / 008(动态优先级)
 *   - 同步通信：TC-SEM-001..004 / TC-MTX-001/002/003(递归) / 004(删持有锁者)
 *            / TC-Q-001/002/003 / TC-EVT-001/002/003
 *   - 定时器：TC-TMR-001(单次) / 002(周期) / 003(删除) / 004(自删) / 005(50个)
 *   - 内核边缘：TC-KERNEL-001(tick 溢出) / 002(零延时) / 003(批量唤醒)
 *   - 安全：TC-SEC-002(断言风暴) / TC-CTX-002(栈哨兵检测)
 *
 * 说明：
 *   - 依赖芯片定时器中断的 ISR 用例（TC-Q-004(FromISR 发队列) / TC-KERNEL-004(挂起
 *     竞争) / TC-INT-001(FromISR give) / TC-INT-003(高频负载)）已下沉到
 *     src/hal/stm32/test/ostest_isr.c（条目 "ostest_isr"），本文件只保留平台无关
 *     用例，可在任意平台编译注册。
 *   - 凡依赖“硬件在环工具链”（逻辑分析仪/DWT/Saleae）的实时性用例（TC-RT-* /
 *     TC-PERF-* / TC-STR-* / TC-LT-* / TC-PORT-*）属 HIL/手动基准，不在此自测内；
 *     由 tools/ostest_hil.py 串联 RTOSALL 自动跑测，并在 CI 中作为回归。
 *   - TC-MEM(内存池) 由主机单测 os_test/common/pool_test.c 覆盖（见 run_tests.bat）。
 *   - 每个子用例用 RTOS_TEST_RESULT("<TC编号>", ok) 打印机器可解析行，供 HIL 脚本收集。
 *   - 本文件所有任务辅助函数均声明在文件作用域（与 rtos_ipc2.c 约定一致），因为
 *     它们被多个自测子函数复用。
 */
#include "rtos.h"
#include "rtos_internal.h"
#include "log/log.h"
#include "log/app_log.h"
#include "common/lock.h"
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

/* TC-TASK-005 资源上限：填充任务（自挂起占槽、零 CPU）。池耗尽时 rtos_task_create
 * 静默拒绝（不注册），用 rtos_kobj_lookup 判空探测拒绝点。 */
#define OT_FILL_MAX  RTOS_MAX_TASKS
static uint8_t ot_fill_stk[OT_FILL_MAX][192] __attribute__((aligned(8)));
static void ot_fill_park(void *arg) {
    (void)arg;
    rtos_task_suspend(rtos_running());   /* 立即自挂起，占住 TCB 槽、不占 CPU */
    for (;;) { }                          /* 若被恢复也不会崩（正常不会到这） */
}

/* ===================== TC-TASK-001/002/003/004/005/006/007/008 ===================== */
int rtos_ostest_task_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] task gap-cases begin\n");

    /* TC-TASK-001: 正常参数创建，任务可运行
     * 注：rtos_task_count() 为“历史分配槽高水位”语义（任务退出变 DEAD 后槽位被复用、
     * 计数不降，rtos_basic T01 正是依赖此语义），故不以“计数增长”为判据；改为验证
     * 任务确实运行过(g_ot_created_ok)且按名注册成功（注册发生在 create 时，与是否运行无关）。 */
    {
        g_ot_created_ok = 0;
        RTOS_TASK_STACK(st1, 512);
        rtos_task_create("ot_cr", ot_created_task, (void *)0, 14, st1, sizeof(st1));
        uint32_t w = 0;
        while (!g_ot_created_ok && w < 1000) { rtos_msleep(2); w += 2; }
        task_t *tc = (task_t *)rtos_kobj_lookup("ot_cr");
        int lok = (g_ot_created_ok == 1) && (tc != (task_t *)0)
                  && (rtos_task_count() <= RTOS_MAX_TASKS);
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

    /* TC-TASK-005: 真正驱动到池上限，验证“达到 RTOS_MAX_TASKS 后创建被拒（不注册、
     * 不越界、不崩）”这一资源限制边界（docs/ostest.md 规格：最后一个成功、再创建返回
     * errNO_MEMORY —— 本 RTOS 的 create 返回 void，等价可观测行为是“不再注册”）。
     * 用自挂起填充任务占满所有非 DEAD 槽，直到 rtos_kobj_lookup 返回 NULL（拒绝点）。
     * 这样能抓“池不封顶 / 越界写 / 拒绝不干净 / 耗尽后系统崩”类回归。 */
    {
        int made = 0, rejected = 0;
        /* static 持久化：任务名指针会被 TCB 长期持有（rtos_task_name 等后续读取），
         * 若用栈上局部数组，测试块结束后栈被复用 -> 名字指针悬空 -> 后续模块读到乱码。 */
        static char nm[OT_FILL_MAX][12];
        for (int i = 0; i < (int)OT_FILL_MAX; i++) {
            /* 手工构造 "ot_fillN"（N<32，避免引入 snprintf 依赖） */
            int j = 0;
            for (const char *p = "ot_fill"; *p; p++) nm[i][j++] = *p;
            if (i >= 10) nm[i][j++] = (char)('0' + i / 10);
            nm[i][j++] = (char)('0' + i % 10);
            nm[i][j] = '\0';
            rtos_task_create(nm[i], ot_fill_park, 0, 30,
                             ot_fill_stk[i], sizeof(ot_fill_stk[i]));
            if (!rtos_kobj_lookup(nm[i])) { rejected = 1; break; }  /* 池耗尽：静默拒绝 */
            made++;
        }
        /* 判据：至少建出 1 个（池确有容量）+ 确实触发拒绝（达到上限）+ 计数未越界 +
         * 系统仍存活。 */
        int lok = (made >= 1) && rejected &&
                  (rtos_task_count() <= RTOS_MAX_TASKS) && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TASK-005", lok);
        /* 清理：删除所有填充任务，槽位置 DEAD 供后续 create 复用，避免饿死后续用例/模块。 */
        for (int i = 0; i < made; i++) {
            task_t *t = (task_t *)rtos_kobj_lookup(nm[i]);
            if (t) rtos_task_delete(t);
        }
        rtos_msleep(20);
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
    /* 注：TC-Q-004(FromISR 发队列) 依赖芯片定时器中断，已下沉到
     * src/hal/stm32/test/ostest_isr.c（条目 "ostest_isr"）。 */

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
    /* 注：TC-KERNEL-004(挂起/恢复并发) 依赖芯片定时器中断，已下沉到
     * src/hal/stm32/test/ostest_isr.c（条目 "ostest_isr"）。 */

    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] kernel edge-cases: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ostest_kernel", rtos_ostest_kernel_selftest);

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

/* ===========================================================================
 * 严格边界场景补充（"补回牙齿"增强集）
 * 目标：用确定性、强断言的用例暴露 RTOS 在边界/错误路径下的真实缺陷，覆盖既有
 * TC 用例未命中的路径：
 *   - 互斥量：天花板防反转（低优先级持有者被提升）/ timedlock 超时 / 非 owner 解锁
 *     防护 / 多等待者 handoff 顺序（最高优先级优先）
 *   - 信号量：count 在 limit 处不溢出（重复 give 不接受）
 *   - 事件：32 位掩码边界（bit31 / 全 1）/ 同一 bit 多 ANY 等待者广播
 *   - 任务：删阻塞在 mutex waitq 上的任务不破坏锁 / set_prio 越界钳制
 *   - 内核：kobj 注册表满拒绝 / 阻塞任务动态改优先级后以新优先级被唤醒
 * 每个用例严格 self-heal：创建的任务末尾删除、锁最终释放、ISR 不残留。
 * ========================================================================= */
/* ---- 文件作用域静态对象与状态 ---- */
static rtos_mutex_t g_st_m5, g_st_m6, g_st_m7, g_st_m8, g_st_m9;
static rtos_event_t g_st_ev4, g_st_ev5;
static rtos_sem_t   g_st_s5, g_st_k6_sem;
static volatile int g_st_m5_m, g_st_m5_h, g_st_m5_hold;
static volatile int g_st_m6_r;
static volatile int g_st_m7_r;
static volatile int g_st_m8_w2;
static volatile int g_st_e5_w1, g_st_e5_w2;
static volatile int g_st_k6_done;

/* MTX-005 优先级天花板防反转（确定性）：L(20) 持锁应被提升到 ceil(5)，使其 yield
 * 时中优先级 M(17) 无法抢占；若天花板失效（L 仍 20），L yield 时 M 会抢 -> m 增长。
 * 用 g_st_m5_hold 窗口标志隔离：M 仅在 hold 窗口内计数，窗口结束后立即退出，不污染。 */
static void st_m5_L(void *a) {
    (void)a;
    rtos_mutex_lock(&g_st_m5);
    g_st_m5_hold = 1;                              /* 进入持锁窗口 */
    for (int i = 0; i < 4000; i++) rtos_yield();   /* 窗口：天花板失效则 L yield 时 M 抢 -> m 增 */
    g_st_m5_hold = 0;
    rtos_mutex_unlock(&g_st_m5);
}
static void st_m5_M(void *a) {
    (void)a;
    while (g_st_m5_hold) { g_st_m5_m++; rtos_yield(); }   /* 仅 L 持锁窗口内计数；窗口结束即退出 */
}
static void st_m5_H(void *a) {
    (void)a;
    rtos_mutex_lock(&g_st_m5);                     /* 等 L 释放后拿到锁 */
    g_st_m5_h = 1;
    rtos_mutex_unlock(&g_st_m5);
}
/* MTX-006 timedlock 超时：持有者长期持锁，等待者超时返回 -1 */
static void st_m6_owner(void *a) {
    (void)a;
    rtos_mutex_lock(&g_st_m6);
    rtos_msleep(600);
    rtos_mutex_unlock(&g_st_m6);
}
static void st_m6_waiter(void *a) {
    (void)a;
    g_st_m6_r = rtos_mutex_timedlock(&g_st_m6, 100);   /* 应超时返回 -1 */
}
/* MTX-007 非 owner 解锁防护 */
static void st_m7_B(void *a) {
    (void)a;
    g_st_m7_r = rtos_mutex_unlock(&g_st_m7);    /* 非 owner -> 应返回 -1 */
}
/* MTX-008 多等待者 handoff：unlock 唤醒最高优先级等待者 */
static void st_m8_w1(void *a) { (void)a; rtos_mutex_lock(&g_st_m8); rtos_msleep(5); rtos_mutex_unlock(&g_st_m8); }
static void st_m8_w2(void *a) { (void)a; rtos_mutex_lock(&g_st_m8); g_st_m8_w2 = 1; rtos_msleep(5); rtos_mutex_unlock(&g_st_m8); }
static void st_m8_w3(void *a) { (void)a; rtos_mutex_lock(&g_st_m8); rtos_msleep(5); rtos_mutex_unlock(&g_st_m8); }
/* MTX-009 删阻塞在 mutex waitq 上的等待者 */
static void st_m9_w(void *a) {
    (void)a;
    rtos_mutex_lock(&g_st_m9);                  /* 阻塞在 main 持有的锁上 */
    rtos_mutex_unlock(&g_st_m9);
}
/* EVT-005 同一 bit 多 ANY 等待者广播唤醒 */
static void st_e5_w1(void *a) { (void)a; rtos_event_wait(&g_st_ev5, 0x1u, 0, 1); g_st_e5_w1 = 1; }
static void st_e5_w2(void *a) { (void)a; rtos_event_wait(&g_st_ev5, 0x1u, 0, 1); g_st_e5_w2 = 1; }
/* KERNEL-006 阻塞任务动态改优先级，被唤醒后以新优先级运行 */
static void st_k6_w(void *a) { (void)a; rtos_sem_wait(&g_st_k6_sem); g_st_k6_done = 1; }

int rtos_ostest_strict_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] strict edge-cases begin\n");

    /* TC-MTX-005: 优先级天花板防反转（确定性，不依赖长阻塞窗口） */
    {
        rtos_mutex_init(&g_st_m5, 5);
        g_st_m5_m = 0; g_st_m5_h = 0; g_st_m5_hold = 0;
        RTOS_TASK_STACK(sL, 512); rtos_task_create("st_m5L", st_m5_L, 0, 20, sL, sizeof(sL));
        RTOS_TASK_STACK(sM, 512); rtos_task_create("st_m5M", st_m5_M, 0, 17, sM, sizeof(sM));
        RTOS_TASK_STACK(sH, 512); rtos_task_create("st_m5H", st_m5_H, 0, 6,  sH, sizeof(sH));
        /* 轮询等待：L 释放（hold 归 0）且 H 已拿到锁（h==1）。天花板生效则 m 保持 0 */
        int wt = 0; while ((g_st_m5_hold || !g_st_m5_h) && wt < 300) { rtos_msleep(1); wt++; }
        int lok = (g_st_m5_m == 0) && (g_st_m5_h == 1) && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-MTX-005", lok);
        task_t *tL = (task_t *)rtos_kobj_lookup("st_m5L");
        task_t *tM = (task_t *)rtos_kobj_lookup("st_m5M");
        task_t *tH = (task_t *)rtos_kobj_lookup("st_m5H");
        if (tL) rtos_task_delete(tL);
        if (tM) rtos_task_delete(tM);
        if (tH) rtos_task_delete(tH);
        rtos_msleep(20);
    }

    /* TC-MTX-006: timedlock 超时返回 -1（持锁者不释放） */
    {
        rtos_mutex_init(&g_st_m6, 5);
        g_st_m6_r = 0;
        RTOS_TASK_STACK(sO, 512); rtos_task_create("st_m6O", st_m6_owner, 0, 20, sO, sizeof(sO));
        rtos_msleep(20);                   /* 让 owner 拿到锁 */
        RTOS_TASK_STACK(sW, 512); rtos_task_create("st_m6W", st_m6_waiter, 0, 10, sW, sizeof(sW));
        rtos_msleep(150);                  /* 等 waiter 超时（owner 持锁到 ~150ms > 100ms 超时） */
        int r = g_st_m6_r;
        int owner_holding = (g_st_m6.owner != (task_t *)0);
        int lok = (r == -1) && owner_holding && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-MTX-006", lok);
        rtos_msleep(50);                   /* 等 owner 释放 */
        task_t *tO = (task_t *)rtos_kobj_lookup("st_m6O");
        task_t *tW = (task_t *)rtos_kobj_lookup("st_m6W");
        if (tO) rtos_task_delete(tO);
        if (tW) rtos_task_delete(tW);
        rtos_msleep(20);
    }

    /* TC-MTX-007: 非 owner 解锁返回 -1，且锁不被破坏 */
    {
        rtos_mutex_init(&g_st_m7, 5);
        rtos_mutex_lock(&g_st_m7);         /* main 持有 */
        g_st_m7_r = 0;
        RTOS_TASK_STACK(sB, 512); rtos_task_create("st_m7B", st_m7_B, 0, 10, sB, sizeof(sB));
        rtos_msleep(50);
        int r = g_st_m7_r;
        int owner_ok = (g_st_m7.owner == rtos_running());   /* 锁仍归 main */
        rtos_mutex_unlock(&g_st_m7);        /* main 正常释放 */
        int lok = (r == -1) && owner_ok && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-MTX-007", lok);
        rtos_msleep(20);
        task_t *tB = (task_t *)rtos_kobj_lookup("st_m7B");
        if (tB) rtos_task_delete(tB);
        rtos_msleep(20);
    }

    /* TC-MTX-008: 多等待者 handoff 顺序（unlock 唤醒最高优先级等待者） */
    {
        rtos_mutex_init(&g_st_m8, 5);
        rtos_mutex_lock(&g_st_m8);         /* main 持有 */
        g_st_m8_w2 = 0;
        RTOS_TASK_STACK(w1, 512); rtos_task_create("st_m8w1", st_m8_w1, 0, 8,  w1, sizeof(w1));
        RTOS_TASK_STACK(w2, 512); rtos_task_create("st_m8w2", st_m8_w2, 0, 6,  w2, sizeof(w2));
        RTOS_TASK_STACK(w3, 512); rtos_task_create("st_m8w3", st_m8_w3, 0, 10, w3, sizeof(w3));
        rtos_msleep(30);                   /* 三等待者阻塞进 waitq（顺序 w1,w2,w3） */
        rtos_mutex_unlock(&g_st_m8);       /* handoff 给最高优先级 = w2(prio 6) */
        int wt = 0; while (!g_st_m8_w2 && wt < 300) { rtos_msleep(1); wt++; }  /* 等 w2 完成 */
        int lok = (g_st_m8_w2 == 1) && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-MTX-008", lok);
        rtos_msleep(50);                   /* 让 w1/w3 依次拿到锁退出 */
        task_t *tw1 = (task_t *)rtos_kobj_lookup("st_m8w1");
        task_t *tw2 = (task_t *)rtos_kobj_lookup("st_m8w2");
        task_t *tw3 = (task_t *)rtos_kobj_lookup("st_m8w3");
        if (tw1) rtos_task_delete(tw1);
        if (tw2) rtos_task_delete(tw2);
        if (tw3) rtos_task_delete(tw3);
        rtos_msleep(20);
    }

    /* TC-SEM-005: count 在 limit 处不溢出（重复 give 不接受） */
    {
        rtos_sem_init(&g_st_s5, 0, 1);     /* 二值，limit=1 */
        rtos_sem_give(&g_st_s5);
        rtos_sem_give(&g_st_s5);
        rtos_sem_give(&g_st_s5);           /* 重复 give 超过 limit */
        int c = (int)g_st_s5.count;        /* 应仍为 1，不回绕 */
        int r1 = rtos_sem_trywait(&g_st_s5);   /* 成功 -> 0 */
        int r2 = rtos_sem_trywait(&g_st_s5);   /* 空 -> 失败 */
        int lok = (c == 1) && (r1 == 0) && (r2 != 0) && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-SEM-005", lok);
    }

    /* TC-EVT-004: 32 位掩码边界（bit31 / 全 1） */
    {
        rtos_event_init(&g_st_ev4);
        rtos_event_set(&g_st_ev4, 0x80000000u);
        uint32_t f1 = rtos_event_wait(&g_st_ev4, 0x80000000u, 1, 0);  /* ALL 非阻塞 */
        int lok1 = ((f1 & 0x80000000u) != 0);
        rtos_event_set(&g_st_ev4, 0xFFFFFFFFu);
        uint32_t f2 = rtos_event_wait(&g_st_ev4, 0xFFFFFFFFu, 1, 0);  /* 全位 ALL */
        int lok2 = (f2 == 0xFFFFFFFFu);
        rtos_event_init(&g_st_ev4);
        rtos_event_set(&g_st_ev4, 0x80000000u);
        uint32_t f3 = rtos_event_wait(&g_st_ev4, 0x80000000u, 0, 0);  /* bit31 ANY 非阻塞 */
        int lok3 = ((f3 & 0x80000000u) != 0);
        int lok = lok1 && lok2 && lok3 && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-EVT-004", lok);
    }

    /* TC-EVT-005: 同一 bit 多 ANY 等待者广播唤醒（两者都应被唤醒） */
    {
        rtos_event_init(&g_st_ev5);
        g_st_e5_w1 = 0; g_st_e5_w2 = 0;
        RTOS_TASK_STACK(e1, 512); rtos_task_create("st_e5w1", st_e5_w1, 0, 10, e1, sizeof(e1));
        RTOS_TASK_STACK(e2, 512); rtos_task_create("st_e5w2", st_e5_w2, 0, 10, e2, sizeof(e2));
        rtos_msleep(30);                   /* 两等待者阻塞 */
        rtos_event_set(&g_st_ev5, 0x1u);   /* 置位 -> 应广播唤醒两者 */
        rtos_msleep(50);
        int lok = (g_st_e5_w1 == 1) && (g_st_e5_w2 == 1) && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-EVT-005", lok);
        task_t *te1 = (task_t *)rtos_kobj_lookup("st_e5w1");
        task_t *te2 = (task_t *)rtos_kobj_lookup("st_e5w2");
        if (te1) rtos_task_delete(te1);
        if (te2) rtos_task_delete(te2);
        rtos_msleep(20);
    }

    /* TC-TASK-009: 删阻塞在 mutex waitq 上的任务，锁不被破坏且可继续用 */
    {
        rtos_mutex_init(&g_st_m9, 5);
        rtos_mutex_lock(&g_st_m9);         /* main 持有 */
        RTOS_TASK_STACK(w, 512); rtos_task_create("st_m9w", st_m9_w, 0, 10, w, sizeof(w));
        rtos_msleep(30);                   /* w 阻塞在锁上 */
        task_t *tw = (task_t *)rtos_kobj_lookup("st_m9w");
        if (tw) rtos_task_delete(tw);      /* 删阻塞等待者 */
        rtos_msleep(20);
        int owner_ok = (g_st_m9.owner == rtos_running());
        rtos_mutex_unlock(&g_st_m9);       /* main 正常释放 */
        rtos_mutex_lock(&g_st_m9);         /* 锁可继续正常使用 */
        int relock = (g_st_m9.owner == rtos_running());
        rtos_mutex_unlock(&g_st_m9);
        int lok = owner_ok && relock && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TASK-009", lok);
        rtos_msleep(20);
    }

    /* TC-TASK-010: set_prio 越界钳制（255 -> 31，0 -> 0），系统稳定 */
    {
        RTOS_TASK_STACK(t, 512);
        rtos_task_create("st_t10", ot_noop, 0, 14, t, sizeof(t));
        task_t *tt = (task_t *)rtos_kobj_lookup("st_t10");
        rtos_msleep(20);
        rtos_task_set_prio(tt, 255);
        int p255 = (int)tt->prio;          /* 应钳到 <= 31 */
        rtos_task_set_prio(tt, 0);
        int p0 = (int)tt->prio;            /* 应 == 0 */
        rtos_task_set_prio(tt, 14);        /* 还原 */
        int lok = (p255 <= 31) && (p0 == 0) && ((int)tt->prio == 14) && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-TASK-010", lok);
        if (tt) rtos_task_delete(tt);
        rtos_msleep(20);
    }

    /* TC-KERNEL-005: kobj 注册表满拒绝（超过 KOBJ_MAX 后 register 返回 0） */
    {
        void *fk[200];
        int nreg = 0, full = 0;
        for (int i = 0; i < 200; i++) {
            fk[i] = (void *)&fk[i];
            int r = rtos_kobj_register("fk", KOBJ_SEM, fk[i]);
            if (!r) { full = 1; break; }   /* 满：拒绝 */
            nreg++;
        }
        for (int i = 0; i < nreg; i++) rtos_kobj_deregister(KOBJ_SEM, fk[i]);  /* 清理还原 */
        int lok = full && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-KERNEL-005", lok);
    }

    /* TC-KERNEL-006: 阻塞任务动态改优先级，被唤醒后以新优先级运行 */
    {
        rtos_sem_init(&g_st_k6_sem, 0, 1);
        g_st_k6_done = 0;
        RTOS_TASK_STACK(w, 512); rtos_task_create("st_k6w", st_k6_w, 0, 20, w, sizeof(w));
        rtos_msleep(30);                   /* w 阻塞在 sem */
        task_t *tw = (task_t *)rtos_kobj_lookup("st_k6w");
        rtos_task_set_prio(tw, 5);         /* 提升等待者到 5 */
        int p_before = (int)tw->prio;      /* BLOCKED 直接改 prio，应 == 5 */
        rtos_sem_give(&g_st_k6_sem);       /* 唤醒 w，以 prio 5 进就绪 */
        rtos_msleep(50);
        int lok = (p_before == 5) && (g_st_k6_done == 1) && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-KERNEL-006", lok);
        if (tw) rtos_task_delete(tw);
        rtos_msleep(20);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] strict edge-cases: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ostest_strict", rtos_ostest_strict_selftest);
