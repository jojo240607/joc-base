#include "rtos.h"
#include "rtos_mpu.h"      /* g_robust_fault_active / g_robust_fault_cfsr 故障恢复钩子 */
#include "log/log.h"
#include "log/app_log.h"
#include "irq/irq.h"
#include "irq/irq_manager.h"
#include "stm32f4xx.h"      /* TIM2 / RCC / TIM2_IRQn — 中断风暴需真实定时器 ISR */
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 鲁棒性/异常注入自测（RTOSROBUST 命令，并注册进 RTOSALL "robust" 条目）。
 * 覆盖准则 §3.1 异常注入(除零/UDF/栈溢出检测/非法return) + §3.2 中断风暴 +
 *          §3.3 嵌套锁无死锁 / 阻塞任务释放恢复 / 资源耗尽不崩。
 *
 * 说明：
 *  - 除零/UDF 通过 mpu.c 的 g_robust_fault_active 恢复分支：仅测试期置位，故障钩子
 *    跳过故障指令使任务继续、系统存活；真实故障仍走 WFI 停机（生产零回归）。
 *  - 栈溢出仅测“检测函数”（破坏栈底哨兵→rtos_stack_check_sentinel 报溢出），不做
 *    破坏性真溢出，避免踩坏相邻 CCM。
 *  - §3.3 真死锁超时 / 删阻塞任务 已由内核新 API(rtos_mutex_timedlock / rtos_task_delete)
 *    补齐，本文件新增 MutexTimedLock / DelBlockedTask 两个用例覆盖（见 docs/rtos-test-plan.md §6）。
 * ------------------------------------------------------------------------- */

/* ===================== §3.1 除零 =====================
 * 故障恢复（mpu.c g_robust_fault_active 分支）把异常返回 PC 设为 LR，
 * 即“跳过故障指令、回到调用点”。若故障指令直接内联在任务体里，LR 是任务的返回
 * 地址(rtos_task_exit)，恢复后会直接跳过 survived/done 赋值，自测永远判 FAIL。
 * 故必须用 naked 辅助函数承载故障指令：naked 无 prologue、不压栈，异常发生时 SP
 * 即调用者(main 任务)的 SP、LR 即回到任务体的下一条指令，恢复后任务体继续往下执行，
 * 与 mpu.c 的 rtos_mpu_do_violation 同构。 */
static volatile int g_rb_div0_survived;
static volatile int g_rb_div0_done;

__attribute__((naked))
static void rb_div0_trigger(void) {
    /* 除数硬编码为 0：sdiv 触发 DIVBYZERO UsageFault。bx lr 在故障恢复(PC=LR)时被跳过。 */
    __asm volatile(
        "movs r0, #0\n"
        "movs r1, #1\n"
        "sdiv r0, r1, r0\n"   /* r0 = 1 / 0 -> DIVBYZERO */
        "bx   lr\n"
        ::: "r0", "r1", "memory"
    );
}
static void rb_div0_task(void *arg) {
    (void)arg;
    g_rb_div0_survived = 0; g_rb_div0_done = 0;
    rb_div0_trigger();        /* 触发 DIVBYZERO；故障钩子跳过 sdiv 后回到此处继续 */
    g_rb_div0_survived = 1;   /* 故障钩子跳过故障指令后，此处继续执行 */
    g_rb_div0_done = 1;
    rtos_msleep(10);
}

/* ===================== §3.1 UDF ===================== */
static volatile int g_rb_udf_survived;
static volatile int g_rb_udf_done;

__attribute__((naked))
static void rb_udf_trigger(void) {
    __asm volatile(
        "udf #0\n"            /* UNDEFINSTR UsageFault */
        "bx  lr\n"           /* 故障恢复(PC=LR)时跳过 */
        ::: "memory"
    );
}
static void rb_udf_task(void *arg) {
    (void)arg;
    g_rb_udf_survived = 0; g_rb_udf_done = 0;
    rb_udf_trigger();         /* 触发 UNDEFINSTR；恢复后回到此处继续 */
    g_rb_udf_survived = 1;
    g_rb_udf_done = 1;
    rtos_msleep(10);
}

/* ===================== §3.1 栈溢出检测（仅测检测函数） ===================== */
static int rb_stack_sentinel_check(void) {
    task_t *me = rtos_running();
    if (!me || !me->stack_base) return 0;
    rtos_stack_fill_sentinel(me);
    uint32_t *sb = (uint32_t *)me->stack_base;
    uint32_t saved = sb[0];
    sb[0] = 0xDEADBEEFu;                       /* 模拟栈底被踩 */
    int det = rtos_stack_check_sentinel(me);
    sb[0] = saved;                             /* 还原，避免误报 */
    return (det == 1);
}

/* ===================== §3.1 非法 return ===================== */
static volatile int g_rb_ret_done;
static void rb_ret_task(void *arg) {
    (void)arg;
    g_rb_ret_done = 1;     /* 函数体直接 return（无 while），应安全变 DEAD */
}

/* ===================== §3.2 中断风暴（TIM2 ~10kHz） ===================== */
#include "stm32f4xx.h"
static volatile uint32_t g_rb_storm_cnt;
static volatile uint32_t g_rb_storm_wake;
static volatile int       g_rb_storm_run;
static rtos_sem_t         g_rb_storm_sem;
/* irq_manager 回调形式：void (*)(void *ctx)，经唯一 IRQ_CommonHandler 分发。
 * 直接定义弱符号 TIM2_IRQHandler + NVIC_EnableIRQ 无效——向量表已指向
 * IRQ_CommonHandler，未注册的 TIM2 源会让分发器命中空回调/故障而冻结板子。 */
static void rb_storm_isr(void *ctx) {
    (void)ctx;
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR &= ~TIM_SR_UIF;          /* 清溢出标志，避免中断重入 */
        g_rb_storm_cnt++;
        rtos_sem_give(&g_rb_storm_sem);   /* ISR 安全：唤醒等待任务 + 请求 PendSV */
    }
}
static void rb_storm_waiter(void *arg) {
    (void)arg;
    while (g_rb_storm_run) {
        rtos_sem_wait(&g_rb_storm_sem);   /* 被 TIM2 ISR 反复唤醒 */
        g_rb_storm_wake++;
    }
}
static void rb_storm_timer_start(uint32_t hz) {
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    TIM2->CR1 = 0;
    TIM2->PSC = 83;
    TIM2->ARR = (84000000u / 84u / hz) - 1u;
    TIM2->DIER |= TIM_DIER_UIE;
    TIM2->CNT = 0; TIM2->SR = 0;
    TIM2->CR1 |= TIM_CR1_CEN;
}
static void rb_storm_timer_stop(void) {
    TIM2->CR1 &= ~TIM_CR1_CEN;
    RCC->APB1ENR &= ~RCC_APB1ENR_TIM2EN;
}

/* ===================== §3.3 嵌套锁无死锁 ===================== */
static rtos_mutex_t g_rb_m1, g_rb_m2;
static rtos_sem_t   g_rb_nest_rel;
static volatile int g_rb_nest_l_holds, g_rb_nest_h_got;
static void rb_nest_L(void *arg) {
    (void)arg;
    rtos_mutex_lock(&g_rb_m1);
    rtos_mutex_lock(&g_rb_m2);          /* 嵌套（不同互斥量，避免递归锁返回 -1） */
    g_rb_nest_l_holds = 1;
    rtos_sem_wait(&g_rb_nest_rel);      /* 阻塞，等 main 释放 */
    rtos_mutex_unlock(&g_rb_m2);
    rtos_mutex_unlock(&g_rb_m1);
    rtos_msleep(10);
}
static void rb_nest_H(void *arg) {
    (void)arg;
    rtos_mutex_lock(&g_rb_m1);          /* 等 L 释放后 handoff 拿到 */
    g_rb_nest_h_got = 1;
    rtos_mutex_unlock(&g_rb_m1);
    rtos_msleep(10);
}

/* ===================== §3.3 阻塞任务释放恢复 ===================== */
static rtos_sem_t g_rb_blk_rel;
static volatile int g_rb_blk_proceeded;
static void rb_blk_task(void *arg) {
    (void)arg;
    rtos_sem_wait(&g_rb_blk_rel);       /* 阻塞在信号量上 */
    g_rb_blk_proceeded = 1;             /* 被唤醒后继续 */
    rtos_msleep(10);
}

/* ===================== §3.3 资源耗尽不崩 ===================== */
#define RB_EX_MAX 32
/* 测试 filler 任务栈：放主 SRAM(.bss)，不占 CCM（无 DMA，纯 CPU）。 */
static uint8_t rb_ex_stack[RB_EX_MAX][256] __attribute__((aligned(256)));
static volatile int g_rb_ex_stop;
static void rb_ex_filler(void *arg) {
    (void)arg;
    while (!g_rb_ex_stop) rtos_msleep(50);
}

/* ===================== §3.3 死锁超时（rtos_mutex_timedlock） ===================== */
static rtos_mutex_t g_rb_dl_m;
static volatile int g_rb_dl_holding;
static void rb_dl_holder(void *arg) {
    (void)arg;
    rtos_mutex_lock(&g_rb_dl_m);     /* 长期持锁，远超测试超时 */
    g_rb_dl_holding = 1;
    rtos_msleep(500);                /* 持锁 500ms */
    rtos_mutex_unlock(&g_rb_dl_m);
    rtos_msleep(10);
}

/* ===================== §3.3 删除阻塞任务（队列满时发送者被删，rtos_task_delete） ===================== */
static uint8_t      g_rb_db_buf[1 * 4];   /* cap=1 的 MQ 缓冲 */
static rtos_mq_t    g_rb_db_q;
static void rb_db_sender(void *arg) {
    (void)arg;
    int v = 0x12345678;
    for (;;) rtos_mq_send(&g_rb_db_q, &v);   /* 队列满 -> 阻塞，等被删除 */
}

/* ===================== §6.5 栈水位（0xEE 填充高水位） ===================== */
static volatile int       g_rb_wm_done;
static volatile size_t    g_rb_wm_used;
static volatile size_t    g_rb_wm_base;
static volatile uint32_t  g_rb_wm_sink;
/* 一次性在栈上分配确定大小的缓冲(1400B)并真实写入，迫使栈向下增长；用“基线 vs 占用后”
 * 的差值证明水位机制在跟踪真实栈使用。特意不用递归（会被 -O2 尾调用优化成单帧循环）。 */
static void rb_wm_task(void *arg) {
    (void)arg;
    g_rb_wm_done = 0; g_rb_wm_used = 0; g_rb_wm_base = 0; g_rb_wm_sink = 0;
    g_rb_wm_base = rtos_stack_used(rtos_running());    /* 占用前基线 */
    volatile uint8_t big[1400];
    for (int i = 0; i < 1400; i++) { big[i] = (uint8_t)i; g_rb_wm_sink += big[i]; }
    g_rb_wm_used = rtos_stack_used(rtos_running());    /* 大缓冲占用后峰值 */
    g_rb_wm_done = 1;
    rtos_msleep(10);
}

/* ===================== §6.5 优先级边界（1-tick 抢占 / 最小睡眠边界） ===================== */
static volatile int       g_rb_pb1_done;
static volatile uint32_t  g_rb_pb1_dt;
static void rb_pb1_task(void *arg) {
    (void)arg;
    uint32_t t0 = rtos_tick_count();
    rtos_msleep(1);                          /* 恰好睡眠 1 个节拍 */
    g_rb_pb1_dt  = rtos_tick_count() - t0;   /* 应 ≈1（量化到 [1,2]） */
    g_rb_pb1_done = 1;
    rtos_msleep(10);
}

int rtos_robust_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[ROBUST] self-test begin\n");

    /* ---------- §3.1 除零 ---------- */
    {
        SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;     /* 使能除零 fault */
        g_robust_fault_cfsr = 0;
        g_rb_div0_survived = 0; g_rb_div0_done = 0;
        g_robust_fault_active = 1;             /* 故障钩子进入“恢复”模式 */
        RTOS_TASK_STACK(std, 512);
        rtos_task_create("rbdiv0", rb_div0_task, (void *)0, 14, std, sizeof(std));
        uint32_t to = 0;
        while (!g_rb_div0_done && to < 1000) { rtos_msleep(2); to += 2; }
        /* DIVBYZERO = CFSR bit25 (SCB_CFSR_DIVBYZERO_Msk) */
        int lok = (g_rb_div0_survived == 1) && ((g_robust_fault_cfsr & (1u << 25u)) != 0);
        g_robust_fault_active = 0;
        SCB->CCR &= ~SCB_CCR_DIV_0_TRP_Msk;    /* 还原 */
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] div0 UsageFault recovered: survived=%d cfsr=0x%lx %s\n",
                   (int)g_rb_div0_survived, (unsigned long)g_robust_fault_cfsr,
                   lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("DIV0_Recover", lok);
    }

    /* ---------- §3.1 UDF ---------- */
    {
        g_robust_fault_cfsr = 0;
        g_rb_udf_survived = 0; g_rb_udf_done = 0;
        g_robust_fault_active = 1;
        RTOS_TASK_STACK(stu, 512);
        rtos_task_create("rbudf", rb_udf_task, (void *)0, 14, stu, sizeof(stu));
        uint32_t to = 0;
        while (!g_rb_udf_done && to < 1000) { rtos_msleep(2); to += 2; }
        /* UNDEFINSTR = CFSR bit16 (SCB_CFSR_UNDEFINSTR_Msk) */
        int lok = (g_rb_udf_survived == 1) && ((g_robust_fault_cfsr & (1u << 16u)) != 0);
        g_robust_fault_active = 0;
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] udf UsageFault recovered: survived=%d cfsr=0x%lx %s\n",
                   (int)g_rb_udf_survived, (unsigned long)g_robust_fault_cfsr,
                   lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("UDF_Recover", lok);
    }

    /* ---------- §3.1 栈溢出检测 ---------- */
    {
        int lok = rb_stack_sentinel_check();
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] stack sentinel detect: %s\n", lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("StackSentinelDetect", lok);
    }

    /* ---------- §3.1 非法 return ---------- */
    {
        g_rb_ret_done = 0;
        RTOS_TASK_STACK(str, 512);
        rtos_task_create("rbret", rb_ret_task, (void *)0, 14, str, sizeof(str));
        uint32_t to = 0, tk0 = rtos_tick_count();
        while (!g_rb_ret_done && to < 1000) { rtos_msleep(2); to += 2; }
        int lok = (g_rb_ret_done == 1) && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] illegal return -> DEAD, system alive: %s\n", lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("IllegalReturn", lok);
    }

    /* ---------- §3.2 中断风暴 ---------- */
    {
        rtos_sem_init(&g_rb_storm_sem, 0, 1);
        g_rb_storm_cnt = 0; g_rb_storm_wake = 0; g_rb_storm_run = 1;
        RTOS_TASK_STACK(stw, 512);
        rtos_task_create("rbstormw", rb_storm_waiter, (void *)0, 6, stw, sizeof(stw));
        /* 经 irq_manager 注册 TIM2 回调（ISR 调内核 API -> IRQ_PRIO_KERNEL） */
        irq_manager_attach((irq_id_t)TIM2_IRQn, rb_storm_isr, NULL);
        irq_manager_set_priority((irq_id_t)TIM2_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        irq_manager_enable((irq_id_t)TIM2_IRQn, rb_storm_isr, NULL);
        uint32_t tk0 = rtos_tick_count();
        rb_storm_timer_start(10000);          /* ~10kHz 溢出中断 */
        rtos_msleep(1000);                    /* 1s 风暴 */
        rb_storm_timer_stop();
        rtos_sem_give(&g_rb_storm_sem);       /* 解阻塞 waiter 使其退出 */
        g_rb_storm_run = 0;
        rtos_msleep(20);
        irq_manager_disable((irq_id_t)TIM2_IRQn, rb_storm_isr, NULL);
        irq_manager_detach((irq_id_t)TIM2_IRQn, rb_storm_isr, NULL);
        int lok = (g_rb_storm_cnt > 1000) && (g_rb_storm_wake > 1000)
                  && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] interrupt storm(10kHz,1s): isr=%lu wake=%lu alive=%d %s\n",
                   (unsigned long)g_rb_storm_cnt, (unsigned long)g_rb_storm_wake,
                   (int)(rtos_tick_count() > tk0), lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("IRQStorm", lok);
    }

    /* ---------- §3.3 嵌套锁无死锁（天花板协议） ---------- */
    {
        rtos_mutex_init(&g_rb_m1, 5);
        rtos_mutex_init(&g_rb_m2, 5);
        rtos_sem_init(&g_rb_nest_rel, 0, 1);
        g_rb_nest_l_holds = 0; g_rb_nest_h_got = 0;
        RTOS_TASK_STACK(stL, 512); RTOS_TASK_STACK(stH, 512);
        rtos_task_create("rbnestL", rb_nest_L, (void *)0, 14, stL, sizeof(stL));
        uint32_t w = 0;
        while (!g_rb_nest_l_holds && w < 1000) { rtos_msleep(1); w++; }
        rtos_task_create("rbnestH", rb_nest_H, (void *)0, 6, stH, sizeof(stH));
        rtos_sem_give(&g_rb_nest_rel);        /* 释放 L -> H 经 handoff 拿锁 */
        w = 0;
        while (!g_rb_nest_h_got && w < 1500) { rtos_msleep(2); w += 2; }
        int lok = (g_rb_nest_h_got == 1);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] nested-lock no-deadlock: H_got_lock=%d %s\n",
                   (int)g_rb_nest_h_got, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("NestedLockNoDeadlock", lok);
        rtos_msleep(20);
    }

    /* ---------- §3.3 阻塞任务释放恢复 ---------- */
    {
        rtos_sem_init(&g_rb_blk_rel, 0, 1);
        g_rb_blk_proceeded = 0;
        RTOS_TASK_STACK(stb, 512);
        rtos_task_create("rbblk", rb_blk_task, (void *)0, 14, stb, sizeof(stb));
        rtos_msleep(20);                      /* 让任务阻塞在信号量上 */
        rtos_sem_give(&g_rb_blk_rel);         /* 唤醒 */
        uint32_t w = 0;
        while (!g_rb_blk_proceeded && w < 1000) { rtos_msleep(2); w += 2; }
        int lok = (g_rb_blk_proceeded == 1);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] blocked task resume on release: %s\n", lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("BlockedResume", lok);
        rtos_msleep(20);
    }

    /* ---------- §3.3 资源耗尽不崩 ---------- */
    {
        int b = rtos_task_count();
        g_rb_ex_stop = 0;
        int made = 0;
        for (int i = 0; i < RB_EX_MAX; i++) {
            int before = rtos_task_count();
            char nm[8];
            nm[0] = 'x'; nm[1] = '0' + (char)(i / 10); nm[2] = '0' + (char)(i % 10); nm[3] = 0;
            rtos_task_create(nm, rb_ex_filler, (void *)0, 20,
                             rb_ex_stack[i], sizeof(rb_ex_stack[i]));
            int after = rtos_task_count();
            if (after > before) made++; else break;
        }
        int limit = rtos_task_count();
        uint32_t tk0 = rtos_tick_count();
        rtos_msleep(100);                     /* 池满后仍正常运行 */
        int lok = (limit <= RTOS_MAX_TASKS) && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] resource exhaustion(base=%d made=%d limit=%d) system alive: %s\n",
                   b, made, limit, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("ResExhaustAlive", lok);
        g_rb_ex_stop = 1; rtos_msleep(100);   /* 释放 filler */
    }

    /* ---------- §3.3 死锁超时（rtos_mutex_timedlock） ---------- */
    {
        rtos_mutex_init(&g_rb_dl_m, 5);
        /* 1) 空闲锁应立即拿到（0），随后释放 */
        int fast = rtos_mutex_timedlock(&g_rb_dl_m, 100);
        rtos_mutex_unlock(&g_rb_dl_m);
        /* 2) 持锁者长期不释放 -> 超时返回 -1（约 timeout_ms） */
        g_rb_dl_holding = 0;
        RTOS_TASK_STACK(stdl, 512);
        rtos_task_create("rbdlh", rb_dl_holder, (void *)0, 14, stdl, sizeof(stdl));
        uint32_t w = 0;
        while (!g_rb_dl_holding && w < 1000) { rtos_msleep(1); w++; }
        uint32_t tk0 = rtos_tick_count();
        int r = rtos_mutex_timedlock(&g_rb_dl_m, 60);   /* 应超时 -1，约 60ms */
        uint32_t dt = rtos_tick_count() - tk0;
        int lok = (fast == 0) && (r == -1) && (dt >= 40 && dt <= 200);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] mutex timedlock: fast=%d timeout_ret=%d dt=%lu %s\n",
                   (int)fast, (int)r, (unsigned long)dt, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("MutexTimedLock", lok);
        rtos_msleep(550);   /* 等 holder 释放并退出，避免影响后续用例 */
    }

    /* ---------- §3.3 删除阻塞任务（队列满时发送者被删，rtos_task_delete） ---------- */
    {
        rtos_mq_init(&g_rb_db_q, g_rb_db_buf, 4, 1);   /* cap=1 */
        int fill = 0xA5A5A5A5;
        rtos_mq_send(&g_rb_db_q, &fill);               /* 填满 */
        RTOS_TASK_STACK(stdb, 512);
        rtos_task_create("rbdbs", rb_db_sender, (void *)0, 14, stdb, sizeof(stdb));
        task_t *s = (task_t *)rtos_kobj_lookup("rbdbs");
        uint32_t w = 0;
        while (s && s->state != TASK_BLOCKED && w < 1000) { rtos_msleep(1); w++; }
        int was_blocked = (s && s->state == TASK_BLOCKED);
        uint32_t tk0 = rtos_tick_count();
        rtos_task_delete(s);                           /* 删除阻塞的发送者 */
        rtos_msleep(100);                              /* 系统应继续运行 */
        int v = 0;
        rtos_mq_tryrecv(&g_rb_db_q, &v);               /* 队列仍含原 1 项（未被破坏） */
        int lok = was_blocked && (s && s->state == TASK_DEAD)
                  && (rtos_tick_count() > tk0) && (v == fill);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] delete blocked task: was_blocked=%d dead=%d qval=0x%lx %s\n",
                   (int)was_blocked, (s ? (int)(s->state == TASK_DEAD) : 0),
                   (unsigned long)v, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("DelBlockedTask", lok);
    }

    /* ---------- §6.5 栈水位（0xEE 高水位） ---------- */
    {
        RTOS_TASK_STACK(rbwm, 2048);
        g_rb_wm_done = 0; g_rb_wm_used = 0; g_rb_wm_sink = 0;
        rtos_task_create("rbwm", rb_wm_task, (void *)0, 14, rbwm, sizeof(rbwm));
        uint32_t w = 0;
        while (!g_rb_wm_done && w < 1000) { rtos_msleep(2); w += 2; }
        /* 峰值在递归最深处已记入 g_rb_wm_used（任务返回前）；任务退出后其名字会从
         * 内核对象表注销，故用“记录峰值”作主判据，若任务仍存活则用实时 API 复核。 */
        task_t *t = (task_t *)rtos_kobj_lookup("rbwm");
        size_t used = g_rb_wm_used;        /* 峰值（大缓冲占用后） */
        size_t base = g_rb_wm_base;        /* 基线（占用前） */
        size_t free = sizeof(rbwm) - used;
        if (t) { used = rtos_stack_used(t); base = 0; free = rtos_stack_free(t); }
        size_t grew = (used > base) ? (used - base) : 0;
        /* 水位机制确在跟踪真实栈增长（grew>200），且占用后仍有余量未触底溢出。 */
        int grew_ok = (grew > 200);
        int fok     = (free > 64);
        int ook     = (g_stack_overflow == 0);
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] stack watermark: base=%lu peak=%lu grew=%lu free=%lu (stack=%zu)\n",
                   (unsigned long)base, (unsigned long)used, (unsigned long)grew,
                   (unsigned long)free, sizeof(rbwm));
        /* 全任务水位体检：逐个报告 used/free；若某任务哨兵被踩(真实溢出) 以其名发 FAIL
         * 行定位（g_stack_overflow 为运行时粘性标志，任何任务曾触底即置位）。 */
        int n = rtos_task_count(), full = 0, corrupt = 0;
        for (int i = 0; i < n; i++) {
            task_t *tt = rtos_task_ptr(i);
            if (!tt || !tt->stack_base) continue;
            size_t f = rtos_stack_free(tt);
            if (f == 0) full++;
            if (rtos_stack_check_sentinel(tt)) {
                corrupt++;
                RTOS_TEST_RESULT(tt->name ? tt->name : "?", 0);   /* 真实溢出：暴露任务名 */
            }
            log_printf(app_log(), LOG_INFO, "rtos",
                       "[ROBUST]   task '%s' used=%lu free=%lu prio=%u state=%d\n",
                       tt->name ? tt->name : "?", (unsigned long)rtos_stack_used(tt),
                       (unsigned long)f, (unsigned)tt->prio, (int)tt->state);
        }
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] stack watermark sweep: tasks=%d full=%d corrupt=%d\n",
                   n, full, corrupt);
        int lok2 = grew_ok && fok && ook && (corrupt == 0);
        if (!lok2) ok = 0;
        RTOS_TEST_RESULT("StackWatermark", lok2);
        rtos_msleep(20);
    }

    /* ---------- §6.5 优先级边界（1-tick 最小睡眠不塌缩/不溢出） ---------- */
    {
        RTOS_TASK_STACK(rbpb, 512);
        g_rb_pb1_done = 0; g_rb_pb1_dt = 0;
        uint32_t tk0 = rtos_tick_count();
        /* 高优先级(6)：其 msleep(1) 应在下一节拍被唤醒并抢占低优先级工作。 */
        rtos_task_create("rbpb1", rb_pb1_task, (void *)0, 6, rbpb, sizeof(rbpb));
        uint32_t w = 0;
        while (!g_rb_pb1_done && w < 1000) { rtos_msleep(2); w += 2; }
        /* 1-tick 睡眠：量化到 [1,2] 个节拍——既不会塌缩成 0（立即返回），
         * 也不会溢出到 3+（严重超睡）。即“1 tick 抢占”边界。 */
        int lok = (g_rb_pb1_dt >= 1 && g_rb_pb1_dt <= 2) && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] 1-tick preemption boundary: dt=%lu %s\n",
                   (unsigned long)g_rb_pb1_dt, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("PrioBoundary1Tick", lok);
        rtos_msleep(20);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[ROBUST] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("robust", rtos_robust_selftest);
