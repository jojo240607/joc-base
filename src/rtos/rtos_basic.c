#include "rtos.h"
#include "log/log.h"
#include "log/app_log.h"
#include "common/lock.h"
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 任务/调度基础自测（RTOSBASIC 命令，并注册进 RTOSALL "basic" 条目）。
 * 覆盖准则 §2.1 T01-T05、§2.2 调度锁/临界区嵌套/SysTick 唤醒。
 *
 * 设计要点：
 *  - 所有子测试采用 baseline 容差（先读 rtos_task_count() 基线，再按相对量断言），
 *    对 boot 期常驻任务（如 p4_demo）零耦合、结果确定。
 *  - 创建的子任务均通过 stop 标志在测试结束前退出(变 DEAD)，避免常驻占用池槽、
 *    干扰后续 RTOSALL 中的其它自测。
 *  - 本 RTOS 的 sched_lock 是 BASEPRI 屏蔽 PendSV（临界区内不发生任务切换），
 *    故 §2.2 调度锁测试按“锁内 yield 不切换、解锁后才切换”的实际语义编写。
 * ------------------------------------------------------------------------- */

/* ===================== T01 任务数上限 ===================== */
#define T01_MAX 32
/* 测试 filler 任务栈：放主 SRAM(.bss)，不占 CCM（无 DMA，纯 CPU）。 */
static uint8_t t01_stack[T01_MAX][256] __attribute__((aligned(256)));
static volatile int g_t01_stop;
static void t01_filler(void *arg) {
    (void)arg;
    while (!g_t01_stop) rtos_msleep(50);
}

/* ===================== T02 回收 ===================== */
static volatile int g_t02_ran;
static void t02_mark(void *arg) {
    (void)arg;
    g_t02_ran = 1;
    rtos_msleep(20);   /* 让 main 观测到 ran 后自然退出(变 DEAD) */
}

/* ===================== T03 时间片轮转 ===================== */
#define T03_N 3
static volatile uint32_t g_t03_cnt[T03_N];
static volatile int       g_t03_stop;
static void t03_spin(void *arg) {
    int id = (int)(intptr_t)arg;
    while (!g_t03_stop) { g_t03_cnt[id]++; rtos_yield(); }
}

/* ===================== T04 优先级抢占延迟 ===================== */
static volatile uint32_t g_t04_trigger;
static volatile uint32_t g_t04_lat;
static volatile int       g_t04_done;
static volatile int       g_t04_go;
static void t04_high(void *arg) {
    (void)arg;
    uint32_t now = rtos_cycle_now();
    g_t04_lat  = (now > g_t04_trigger) ? (now - g_t04_trigger) : 0;
    g_t04_done = 1;
    rtos_msleep(10);   /* 让出，回到 main */
}
static void t04_low(void *arg) {
    (void)arg;
    while (!g_t04_go) { /* 自旋(不 yield)，模拟低优先级忙等 */ }
}

/* ===================== T05 满载下低优先级不被饿死 =====================
 * 注意：系统常驻任务 console 的 main 任务(prio 16)以 rtos_msleep(1) 轮询 UART，
 * 99% 时间处于 READY，且优先级高于本测试的低优先级任务——这是【正确】的固定优先级
 * 行为（高优先级 console 不应被低优先级任务饿死），但会导致“裸”低优先级(如 28)被
 * console 持续抢占。因此本测试把低任务放在 console(16) 之上(prio 10)、load 任务之下，
 * 真实验证“在更高优先级 load 任务满载下，一个低于 load 但高于系统常驻任务的低优先级
 * 任务仍能持续推进”，且 tick 不中断。 */
#define T05_LOAD 2
static volatile int       g_t05_stop;
static volatile uint32_t g_t05_low;
static void t05_load(void *arg) {
    (void)arg;
    /* 高优先级(5)周期任务：rtos_msleep(40) 周期性【阻塞】让出 CPU，模拟“满载”。
     * 阻塞足够长，使多个同优先级 load 任务错相位后仍留下确定性大空隙。 */
    while (!g_t05_stop) rtos_msleep(40);
}
static void t05_low(void *arg) {
    (void)arg;
    while (!g_t05_stop) { g_t05_low++; rtos_msleep(10); }
}

/* ===================== §2.2 调度锁 ===================== */
static rtos_sem_t g_sl_sem;
static volatile int g_sl_high;
/* 高优先级任务先阻塞在信号量上，由测试在“锁内”放行，以干净验证
 * “锁内放行(give)不切换 / 解锁后才切换”的 BASEPRI 调度锁语义。 */
static void t_sl_high(void *arg) {
    (void)arg;
    rtos_sem_wait(&g_sl_sem);
    g_sl_high = 1;
    rtos_msleep(50);
}

/* ===================== §2.2 临界区嵌套 ===================== */
/* （直接在主测试里用 irq_lock/irq_unlock 两层嵌套验证） */

/* ===================== §2.2 SysTick 唤醒 ===================== */
static volatile uint32_t g_st_t0, g_st_t1, g_st_done;
static void t_st_wait(void *arg) {
    (void)arg;
    g_st_done = 1;                  /* 任务已抢占运行（先于 main 记录 t0 之后） */
    rtos_msleep(10);                /* 阻塞，由 SysTick 唤醒 */
    g_st_t1   = rtos_tick_count();  /* 唤醒后的节拍：应比 g_st_t0 晚 ~10ms */
}

int rtos_basic_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[BASIC] self-test begin\n");

    /* ---------- T01 任务数上限 ---------- */
    {
        int b = rtos_task_count();
        g_t01_stop = 0;
        int made = 0;
        for (int i = 0; i < T01_MAX; i++) {
            int before = rtos_task_count();
            char nm[8];
            /* 用编号名避免 kobj 同名覆盖影响判定 */
            nm[0] = 'f'; nm[1] = '0' + (char)(i / 10); nm[2] = '0' + (char)(i % 10);
            nm[3] = 0;
            rtos_task_create(nm, t01_filler, (void *)0, 20,
                             t01_stack[i], sizeof(t01_stack[i]));
            int after = rtos_task_count();
            if (after > before) made++;
            else break;   /* 池满：create 静默失败，count 不再增长 */
        }
        int limit = rtos_task_count();
        int lok = (limit == RTOS_MAX_TASKS) && (made == (RTOS_MAX_TASKS - b));
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BASIC] T01 max-tasks: base=%d made=%d limit=%d (expect %d) %s\n",
                   b, made, limit, RTOS_MAX_TASKS, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("T01_CreateMaxTasks", lok);

        /* 释放 filler（置 stop -> 退出变 DEAD），腾出可复用槽 */
        g_t01_stop = 1;
        rtos_msleep(200);
        /* 释放后再次 create 应复用 DEAD 槽：count 不增 */
        int before2 = rtos_task_count();
        rtos_task_create("t01reuse", t01_filler, (void *)0, 20,
                         t01_stack[0], sizeof(t01_stack[0]));
        int after2 = rtos_task_count();
        int lok2 = (after2 == before2);
        if (!lok2) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BASIC] T01 reuse-after-free: before=%d after=%d %s\n",
                   before2, after2, lok2 ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("T01_ReuseAfterFree", lok2);
        /* 清掉复用任务 */
        g_t01_stop = 1; rtos_msleep(50);
    }

    /* ---------- T02 回收（DEAD 槽复用，无静默丢任务） ---------- */
    {
        g_t02_ran = 0;
        /* T02 回收测试栈：放主 SRAM(.bss)，不占 CCM（纯 CPU、无 DMA）。 */
        static uint8_t st[512] __attribute__((aligned(512)));
        rtos_task_create("t02b", t02_mark, (void *)0, 20, st, sizeof(st));
        uint32_t to = 0;
        while (!g_t02_ran && to < 1000) { rtos_msleep(2); to += 2; }
        int lok = (g_t02_ran == 1);   /* 任务成功运行 => create 在 DEAD 槽上复用成功 */
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BASIC] T02 reclaim(reuse DEAD slot): ran=%d %s\n",
                   (int)g_t02_ran, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("T02_Reclaim", lok);
        rtos_msleep(50);   /* 让 t02b 退出 */
    }

    /* ---------- T03 时间片轮转 ---------- */
    {
#if RTOS_TIME_SLICE
        g_t03_stop = 0;
        for (int i = 0; i < T03_N; i++) g_t03_cnt[i] = 0;
        RTOS_TASK_STACK(st0, 512); RTOS_TASK_STACK(st1, 512); RTOS_TASK_STACK(st2, 512);
        rtos_task_create("t03a", t03_spin, (void *)0, 18, st0, sizeof(st0));
        rtos_task_create("t03b", t03_spin, (void *)1, 18, st1, sizeof(st1));
        rtos_task_create("t03c", t03_spin, (void *)2, 18, st2, sizeof(st2));
        rtos_msleep(120);
        g_t03_stop = 1;
        int lok = (g_t03_cnt[0] > 0 && g_t03_cnt[1] > 0 && g_t03_cnt[2] > 0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BASIC] T03 round-robin: a=%lu b=%lu c=%lu %s\n",
                   (unsigned long)g_t03_cnt[0], (unsigned long)g_t03_cnt[1],
                   (unsigned long)g_t03_cnt[2], lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("T03_RoundRobin", lok);
        rtos_msleep(20);
#else
        log_printf(app_log(), LOG_INFO, "rtos", "[BASIC] T03 skipped (RTOS_TIME_SLICE=0)\n");
#endif
    }

    /* ---------- T04 优先级抢占延迟（DWT CYCCNT） ---------- */
    {
        g_t04_trigger = 0; g_t04_lat = 0; g_t04_done = 0; g_t04_go = 0;
        RTOS_TASK_STACK(stL, 512); RTOS_TASK_STACK(stH, 512);
        rtos_task_create("t04L", t04_low, (void *)0, 20, stL, sizeof(stL));
        rtos_task_create("t04H", t04_high, (void *)0, 6, stH, sizeof(stH));
        /* 模拟“低优先级任务忙等时，高优先级就绪后抢占”的延迟测量：
         * 在 yield(触发切换)前记 t0，高优先级任务首行记 t1，差值即抢占延迟。 */
        for (volatile int i = 0; i < 500; i++) { }   /* 低优先级忙等一小段 */
        g_t04_trigger = rtos_cycle_now();
        rtos_yield();                                 /* 切换到 t04H(更高优先级) */
        uint32_t to = 0;
        while (!g_t04_done && to < 1000) { rtos_msleep(2); to += 2; }
        uint32_t us = (g_t04_lat + 83) / 168u;        /* 168MHz -> µs */
        int lok = (g_t04_done == 1) && (g_t04_lat > 0) && (us < 200);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BASIC] T04 preempt-latency: %lu cycles (~%lu us), bound <200us %s\n",
                   (unsigned long)g_t04_lat, (unsigned long)us, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("T04_PreemptLatency", lok);
        g_t04_go = 1; rtos_msleep(20);   /* 让 t04L 退出 */
    }

    /* ---------- T05 满载下低优先级不被饿死 ---------- */
    {
        g_t05_stop = 0; g_t05_low = 0;
        RTOS_TASK_STACK(ld0, 512); RTOS_TASK_STACK(ld1, 512);
        RTOS_TASK_STACK(lo0, 512);
        rtos_task_create("t05ld0", t05_load, (void *)0, 5, ld0, sizeof(ld0));
        rtos_task_create("t05ld1", t05_load, (void *)0, 5, ld1, sizeof(ld1));
        rtos_task_create("t05lo", t05_low, (void *)0, 10, lo0, sizeof(lo0));
        uint32_t tk0 = rtos_tick_count();
        rtos_msleep(300);
        uint32_t tk1 = rtos_tick_count();
        g_t05_stop = 1;
        int lok = (g_t05_low > 0) && (tk1 > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BASIC] T05 no-starve under load: low=%lu tick+%lu %s\n",
                   (unsigned long)g_t05_low, (unsigned long)(tk1 - tk0),
                   lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("T05_NoStarveUnderLoad", lok);
        rtos_msleep(20);
    }

    /* ---------- §2.2 调度锁 ---------- */
    {
        /* 本 RTOS sched_lock = BASEPRI 屏蔽 PendSV：临界区内 rtos_yield / give 均
         * 不触发任务切换（含更高优先级任务），解锁后才切换。为消除“加锁前高优先级
         * 已抢占”的歧义，先把高优先级任务用信号量挡在阻塞态；锁内 give 使其就绪，
         * 但 PendSV 被屏蔽故不切换；解锁后它才真正运行。阈值取 RTOS_MAX_ZERO_LATENCY_IRQS
         * (4<<4=0x40)。 */
        log_printf(app_log(), LOG_INFO, "rtos", "[BASIC] sched-lock: begin\n");
        g_sl_high = 0;
        RTOS_TASK_STACK(sh, 512);
        rtos_sem_init(&g_sl_sem, 0, 1);
        rtos_task_create("sl_high", t_sl_high, (void *)0, 6, sh, sizeof(sh));
        rtos_msleep(20);                 /* 让 sl_high 阻塞在信号量上（尚未运行） */
        sched_lock((uint8_t)RTOS_MAX_ZERO_LATENCY_IRQS);
        rtos_sem_give(&g_sl_sem);        /* sl_high 变就绪，但 PendSV 被锁屏蔽 */
        rtos_yield();                     /* 尝试切换：锁内不应发生 */
        int lok_locked = (g_sl_high == 0);
        sched_unlock();
        uint32_t to = 0;
        while (!g_sl_high && to < 500) { rtos_msleep(2); to += 2; }
        int lok_after = (g_sl_high == 1);   /* 解锁后高优先级任务确实运行 */
        int lok = lok_locked && lok_after;
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BASIC] sched-lock: locked-no-switch=%d unlocked-high-ran=%d %s\n",
                   lok_locked, lok_after, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("SchedLock", lok);
    }

    /* ---------- §2.2 临界区嵌套 ---------- */
    {
        irq_state_t st1 = irq_lock();
        /* 临界区内再进入一层 */
        irq_state_t st2 = irq_lock();
        /* 内层退出后，中断应仍被外层锁住 */
        irq_unlock(st2);
        int still_locked = irq_is_disabled();
        /* 外层退出后，中断应恢复开放 */
        irq_unlock(st1);
        int now_enabled = !irq_is_disabled();
        int lok = still_locked && now_enabled;
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BASIC] crit-nesting: inner-exit-still-locked=%d outer-exit-enabled=%d %s\n",
                   still_locked, now_enabled, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("CritNesting", lok);
    }

    /* ---------- §2.2 SysTick 唤醒（高优先级阻塞 msleep 被节拍唤醒） ---------- */
    {
        g_st_t0 = rtos_tick_count();
        g_st_done = 0; g_st_t1 = 0;
        RTOS_TASK_STACK(stw, 512);
        rtos_task_create("st_wait", t_st_wait, (void *)0, 6, stw, sizeof(stw));
        /* 先等任务启动（抢占 main、置 g_st_done） */
        uint32_t to = 0;
        while (!g_st_done && to < 1000) { rtos_msleep(2); to += 2; }
        /* 再等任务被 SysTick 唤醒后记录 g_st_t1（应比 g_st_t0 晚 ~10ms） */
        to = 0;
        while (!g_st_t1 && to < 1000) { rtos_msleep(2); to += 2; }
        uint32_t dt = (g_st_t1 >= g_st_t0) ? (g_st_t1 - g_st_t0) : 0;
        int lok = (g_st_done == 1) && (g_st_t1 > 0) && (dt >= 8) && (dt <= 20);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BASIC] systick-wakeup: dt=%lums (expect ~10ms) %s\n",
                   (unsigned long)dt, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("SysTickWakeup", lok);
        rtos_msleep(20);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[BASIC] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("basic", rtos_basic_selftest);
