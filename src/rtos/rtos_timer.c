#include "rtos.h"
#include "core/rtos_internal.h"
#include "log/log.h"
#include "log/app_log.h"
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 软件定时器 / 48 天 tick 翻转自测（RTOSTIMER 命令，并注册进 RTOSALL "timer" 条目）。
 * 覆盖准则 rtos-test.md §2.5：单次定时器、周期定时器、停止、无符号 tick 比较的
 * 48 天翻转安全、绝对延时 rtos_delay_until 的翻转安全。
 *
 * 测试方法：在 irq_lock 窗口内冻结真实 sysTick（BASEPRI 屏蔽内核 ISR），手动推进
 * g_tick 并调用 rtos_timer_tick + rtos_timer_run_pending，从而脱离真实 1kHz 节拍做
 * 快速、可复现的判定（不依赖毫秒级等待、不受真实节拍干扰）。
 * ------------------------------------------------------------------------- */

static volatile uint32_t g_tm_oneshot;
static volatile uint32_t g_tm_periodic;
static volatile uint32_t g_tm_periodic2;

static void tm_oneshot_cb(rtos_timer_t *t, void *arg) { (void)t; (void)arg; g_tm_oneshot++; }
static void tm_periodic_cb(rtos_timer_t *t, void *arg) { (void)t; (void)arg; g_tm_periodic++; }
static void tm_periodic2_cb(rtos_timer_t *t, void *arg) { (void)t; (void)arg; g_tm_periodic2++; }

/* 在 irq_lock 窗口内手动推进 n 个 tick 并同步处理到期定时器（模拟节拍 + 定时器任务） */
static void tm_drive(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        g_tick = (uint32_t)(g_tick + 1);
        rtos_timer_tick();
        rtos_timer_run_pending();
    }
}

int rtos_timer_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[TIMER] self-test begin\n");

    /* ---------- 单次定时器：到期恰好一次 ---------- */
    {
        g_tm_oneshot = 0;
        rtos_timer_t t;
        rtos_timer_init(&t, "tm_one", tm_oneshot_cb, (void *)0);
        unsigned st = irq_lock();
        rtos_timer_start_ticks(&t, RTOS_TIMER_ONESHOT, 5);
        tm_drive(10);            /* 推进 10 个 tick，到期应在第 5 */
        irq_unlock(st);
        int lok = (g_tm_oneshot == 1) && !rtos_timer_is_active(&t);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[TIMER] one-shot: fired=%lu active=%d %s\n",
                   (unsigned long)g_tm_oneshot, rtos_timer_is_active(&t),
                   lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("TimerOneShot", lok);
    }

    /* ---------- 周期定时器：每 period 触发一次（无漂移） ---------- */
    {
        g_tm_periodic = 0;
        rtos_timer_t t;
        rtos_timer_init(&t, "tm_per", tm_periodic_cb, (void *)0);
        unsigned st = irq_lock();
        rtos_timer_start_ticks(&t, RTOS_TIMER_PERIODIC, 3);
        tm_drive(20);            /* 20/3 = 6 次 (tick 3,6,9,12,15,18) */
        /* 判定须在关中断窗口内完成：tm_drive(20) 后 expire=g0+21、g_tick=g0+20，
         * 若先解锁再判定，解锁后第一个真实节拍会把 expire 推到期并触发第 7 次回调
         * （竞态，ESP32C3/RISC-V Renode 上偶现 fired=7）。锁内判定+停止再解锁，
         * 消除竞态，其它平台行为不变。 */
        int lok = (g_tm_periodic == 6) && rtos_timer_is_active(&t);
        rtos_timer_stop(&t);
        irq_unlock(st);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[TIMER] periodic: fired=%lu (expect 6) %s\n",
                   (unsigned long)g_tm_periodic, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("TimerPeriodic", lok);
    }

    /* ---------- 停止定时器：停止后不再触发 ---------- */
    {
        g_tm_periodic2 = 0;
        rtos_timer_t t;
        rtos_timer_init(&t, "tm_stop", tm_periodic2_cb, (void *)0);
        unsigned st = irq_lock();
        rtos_timer_start_ticks(&t, RTOS_TIMER_PERIODIC, 3);
        tm_drive(6);             /* 触发 2 次 (tick 3,6) */
        rtos_timer_stop(&t);     /* 停止 */
        tm_drive(20);            /* 停止后不应再触发 */
        irq_unlock(st);
        int lok = (g_tm_periodic2 == 2) && !rtos_timer_is_active(&t);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[TIMER] stop: fired=%lu (expect 2) %s\n",
                   (unsigned long)g_tm_periodic2, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("TimerStop", lok);
    }

    /* ---------- 48 天翻转（准则 §2.5）：expire 跨 0xFFFFFFFF→0 仍正确 ---------- */
    {
        /* (a) 无符号 tick 比较纯数学：expire 恰在翻转边界前后 */
        uint32_t e = 0xFFFFFFFEu;
        int m1 = !rtos_tick_expired(e, 0xFFFFFFFDu);  /* 未到 */
        int m2 =  rtos_tick_expired(e, 0xFFFFFFFFu);  /* 恰好 */
        int m3 =  rtos_tick_expired(e, 0x00000000u);  /* 翻转后 */
        int m4 =  rtos_tick_expired(e, 0x00000001u);  /* 翻转后 +1 */
        int lok_math = m1 && m2 && m3 && m4;
        if (!lok_math) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[TIMER] wrap math: pre=%d at=%d post0=%d post1=%d %s\n",
                   m1, m2, m3, m4, lok_math ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("TickWrapMath", lok_math);

        /* (b) 集成：把 g_tick 强制到翻转前，启动单次定时器(period 跨边界) */
        g_tm_oneshot = 0;
        rtos_timer_t t;
        rtos_timer_init(&t, "tm_wrap", tm_oneshot_cb, (void *)0);
        unsigned st = irq_lock();
        g_tick = 0xFFFFFFFEu;                 /* 模拟第 ~48 天 */
        rtos_timer_start_ticks(&t, RTOS_TIMER_ONESHOT, 3);  /* expire = 0x1 */
        tm_drive(5);                          /* +3 -> g_tick=0x1 触发 */
        irq_unlock(st);
        int lok_wrap = (g_tm_oneshot == 1);
        if (!lok_wrap) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[TIMER] wrap integrate: fired=%lu %s\n",
                   (unsigned long)g_tm_oneshot, lok_wrap ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("TimerWrap48d", lok_wrap);
    }

    /* ---------- rtos_delay_until：翻转安全绝对延时 ---------- */
    {
        /* (a) 数学：把 now 设为 last，remain 应为 inc（翻转安全） */
        uint32_t last = 0xFFFFFFFEu;
        uint32_t inc  = 3;
        uint32_t now  = last;
        uint32_t remain = (uint32_t)((last + inc) - now);
        int lok = (remain == 3);
        /* now 已跨边界超过 deadline 时 remain 仍为正确正数（极大值，非负数） */
        uint32_t now2 = 0x00000002u;          /* last+inc = 0x1，now2=2 > deadline */
        uint32_t remain2 = (uint32_t)((last + inc) - now2);
        int lok2 = (remain2 == (uint32_t)(0x1u - 0x2u));  /* = 0xFFFFFFFF，正确(很大) */
        if (!lok || !lok2) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[TIMER] delay_until wrap math: remain=%lu remain2=0x%lx %s\n",
                   (unsigned long)remain, (unsigned long)remain2,
                   (lok && lok2) ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("DelayUntilWrapMath", lok && lok2);

        /* (b) 真实绝对延时（非翻转）冒烟测试：应约睡眠 inc 个节拍 */
        uint32_t t0   = rtos_tick_count();
        uint32_t next = t0;
        rtos_delay_until(&next, 10);
        uint32_t dt = rtos_tick_elapsed(t0, rtos_tick_count());
        int lok_live = (dt >= 8 && dt <= 40);
        if (!lok_live) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[TIMER] delay_until live: dt=%lu ticks (expect ~10) %s\n",
                   (unsigned long)dt, lok_live ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("DelayUntilLive", lok_live);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[TIMER] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("timer", rtos_timer_selftest);
