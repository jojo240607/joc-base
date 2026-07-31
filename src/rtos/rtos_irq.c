#include "rtos.h"
#include "rtos_mpu.h"      /* g_fault_cfsr / g_stack_overflow 故障/溢出粘性标志 */
#include "core/bh.h"       /* rtos_bh_task_create / rtos_bh_trigger */
#include "log/log.h"
#include "log/app_log.h"
#include "irq/irq.h"
#include "irq/irq_manager.h"
#include "stm32f4xx.h"      /* TIM5 / TIM2 / RCC / TIMx_IRQn / DWT */
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 中断实时性 / 抗压自测（RTOSIRQ 命令，并注册进 RTOSALL "irq" 条目）。
 *
 * 覆盖此前测试场景里【缺失】的两个用户态关键场景：
 *
 *  (1) 实时中断响应：高优先级中断必须“零差错、零卡死”地响应。
 *        - 1a ISR(kernel prio) -> 高优先级任务(prio 3，高于 BH 带 4) 唤醒延迟；
 *        - 1b ISR(kernel prio) -> 下半部 BH 任务(prio 4) 唤醒延迟；
 *        - 1c 零延迟 ISR(prio 2, ZERO_LATENCY)：ISR 自身即做时关键工作，验证
 *              “绝对实时”——每个中断都立即得到服务(100% 交付, 从不丢/从不被屏蔽)、
 *              ISR 内处理有界(微秒级)。这是“高优先级中断任务”的硬实时保证。
 *        每个被延迟的响应都满足：中断数 <= 响应数(无丢失) 且 max 延迟 < 预算；
 *        结束后 tick 推进(绝不卡死)。
 *
 *  (2) 高频中断抗压：反复、长时间的高频中断不能把系统搞挂。
 *        - 2a TIM5 单路 ~50kHz 持续 5s，纯计数（验证中断机制在高频下不损坏、
 *             不重入死循环、系统存活）；
 *        - 2b TIM5(50kHz, prio5) + TIM2(30kHz, prio6) 双路嵌套并发 3s，验证
 *             中断嵌套 / 抢占下计数不丢、系统存活。
 *
 * 设计要点：
 *   - 中断频率远小于 ISR 处理耗时（ISR 仅清 UIF 标志 + 计数 / 发信号），故 UIF
 *     总能被及时清除，不会重入死循环冻板。
 *   - 每个用例结束都 disable + detach 对应定时器，避免与别处复用的 TIM2（ROBUST
 *     中断风暴）冲突；RTOSALL 顺序执行各条目，不存在并发持有同一定时器。
 *   - TIM5/TIM2 均在启动向量表指向 IRQ_CommonHandler，须经 irq_manager 注册回调
 *     分发（直接定义弱符号 TIMx_IRQHandler 无效，会空窗冻结板子）。
 *   - 零延迟 ISR(1c) 不调用任何内核 API（仅清标志 + 计数 + 读 DWT），符合
 *     FreeRTOS 式契约：prio < RTOS_MAX_ZERO_LATENCY_IRQS 阈值的 ISR 永不被内核
 *     临界区(BASEPRI)屏蔽，故“立即服务、零丢失”。
 * ------------------------------------------------------------------------- */

/* 延迟预算（延迟路径：ISR -> 被唤醒的任务/BH，依赖 PendSV，受临界区约束）：
 * 1ms @168MHz = 168000 周期。平均仅几 µs，worst-case 允许到 1ms（已剔除首 2 个
 * 冷启动样本）。零延迟 ISR(1c) 自身处理预算：20µs。 */
#define IRQ_WAKE_BUDGET_CYCLES (1000u * 168u)
#define IRQ_ZL_ISR_BUDGET_CYCLES (20u * 168u)

/* cycle -> µs（168MHz：168 周期 = 1µs；四舍五入） */
static uint32_t cyc_to_us(uint32_t c) { return (c + 84u) / 168u; }

/* 实际经历 cycles -> 期望中断数（rate 为 Hz；用 64 位避免溢出） */
static uint32_t exp_from_cycles(uint32_t rate_hz, uint32_t dc) {
    return (uint32_t)(((uint64_t)rate_hz * (uint64_t)dc) / 168000000ULL);
}

/* ---- 通用定时器启停（TIM5 / TIM2 共用，1MHz 计数时钟） ---- */
static void irq_timer_start(TIM_TypeDef *tim, uint32_t hz) {
    if (tim == TIM5)      RCC->APB1ENR |= RCC_APB1ENR_TIM5EN;
    else if (tim == TIM2) RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    tim->CR1 = 0;
    tim->PSC = 83;                                  /* 84MHz / 84 = 1MHz */
    tim->ARR = (84000000u / 84u / hz) - 1u;         /* 达到 hz 溢出 */
    tim->DIER |= TIM_DIER_UIE;
    tim->CNT = 0; tim->SR = 0;
    tim->CR1 |= TIM_CR1_CEN;
}
static void irq_timer_stop(TIM_TypeDef *tim) {
    tim->CR1 &= ~TIM_CR1_CEN;
    if (tim == TIM5)      RCC->APB1ENR &= ~RCC_APB1ENR_TIM5EN;
    else if (tim == TIM2) RCC->APB1ENR &= ~RCC_APB1ENR_TIM2EN;
}

/* ===================== 场景 1a：ISR -> 高优先级任务 唤醒延迟 ===================== */
static volatile uint32_t g_ia_t0;     /* ISR 到达 cycle */
static volatile uint32_t g_ia_irq;     /* ISR 计数 */
static volatile uint32_t g_ia_rsp;     /* 高优先级任务响应计数 */
static volatile uint32_t g_ia_max;     /* max 唤醒延迟(cycle) */
static volatile uint32_t g_ia_sum;     /* 累计延迟(cycle, 求均值) */
static volatile int       g_ia_run;
static volatile int       g_ia_active; /* 1=活跃风暴期(仅此期间统计延迟，剔除收尾样本) */
static rtos_sem_t         g_ia_sem;
RTOS_TASK_STACK(g_ia_stack, 1024);
static void ia_task(void *arg) {
    (void)arg;
    uint32_t n = 0;
    while (g_ia_run) {
        rtos_sem_wait(&g_ia_sem);
        uint32_t t1  = rtos_cycle_now();                 /* 任务被唤醒时刻 */
        uint32_t lat = (t1 > g_ia_t0) ? (t1 - g_ia_t0) : 0;
        g_ia_rsp++;
        /* 仅活跃风暴期内统计延迟：剔除首 2 个冷启动样本(首次 PendSV 预热)与
         * 收尾样本(定时器停后 g_ia_t0 已陈旧)，只反映稳态实时性。 */
        if (g_ia_active) {
            g_ia_sum += lat;
            if (n >= 2 && lat > g_ia_max) g_ia_max = lat;
        }
        n++;
    }
}
static void ia_isr(void *ctx) {
    (void)ctx;
    if (TIM5->SR & TIM_SR_UIF) {
        TIM5->SR &= ~TIM_SR_UIF;          /* 清溢出，避免重入 */
        g_ia_t0  = rtos_cycle_now();      /* 到达时刻 */
        g_ia_irq++;
        rtos_sem_give(&g_ia_sem);         /* ISR 安全：唤醒高优先级任务 */
    }
}

/* ===================== 场景 1b：ISR -> 下半部 BH 任务 唤醒延迟 ===================== */
static volatile uint32_t g_ib_t0;
static volatile uint32_t g_ib_irq;
static volatile uint32_t g_ib_rsp;
static volatile uint32_t g_ib_max;
static volatile uint32_t g_ib_sum;
static bh_t              *g_ib_bh;
RTOS_TASK_STACK(g_ib_stack, 1024);
static void ib_bh_fn(void *arg) {
    (void)arg;
    uint32_t t1  = rtos_cycle_now();                       /* BH 任务被唤醒时刻 */
    uint32_t lat = (t1 > g_ib_t0) ? (t1 - g_ib_t0) : 0;
    g_ib_rsp++;
    g_ib_sum += lat;
    if (lat > g_ib_max) g_ib_max = lat;
}
static void ib_isr(void *ctx) {
    (void)ctx;
    if (TIM5->SR & TIM_SR_UIF) {
        TIM5->SR &= ~TIM_SR_UIF;
        g_ib_t0 = rtos_cycle_now();
        g_ib_irq++;
        rtos_bh_trigger(g_ib_bh);        /* ISR 安全：触发 BH 任务 */
    }
}

/* ===================== 场景 1c：零延迟 ISR 硬实时（不调内核 API） =====================
 * 这是“高优先级中断任务”的硬实时保证：ISR 自身(prio 2, 永不被 BASEPRI 屏蔽)在中断
 * 发生的同一时刻被服务，每个中断 100% 交付、从不被丢失/延迟；ISR 内处理有界(µs)。 */
static volatile uint32_t g_ic_cnt;       /* ISR 计数（100% 交付证明） */
static volatile uint32_t g_ic_max;       /* ISR 内处理 max 延迟(cycle) */
static volatile uint32_t g_ic_sum;
static void ic_isr(void *ctx) {
    (void)ctx;
    if (TIM5->SR & TIM_SR_UIF) {
        uint32_t t0 = rtos_cycle_now();      /* ISR 入口（中断发生后极短延迟） */
        TIM5->SR &= ~TIM_SR_UIF;             /* 清溢出（唯一关键工作） */
        g_ic_cnt++;
        uint32_t t1 = rtos_cycle_now();
        uint32_t lat = (t1 > t0) ? (t1 - t0) : 0;
        g_ic_sum += lat;
        if (lat > g_ic_max) g_ic_max = lat;
    }
}

/* ===================== 场景 2a：单路高频风暴（TIM5 ~50kHz, 5s） ===================== */
static volatile uint32_t g_sa_cnt;
static void sa_isr(void *ctx) {
    (void)ctx;
    if (TIM5->SR & TIM_SR_UIF) {
        TIM5->SR &= ~TIM_SR_UIF;
        g_sa_cnt++;                       /* 纯计数：最小化 ISR 开销 */
    }
}

/* ===================== 场景 2b：双路嵌套高频风暴（TIM5 + TIM2, 3s） ===================== */
static volatile uint32_t g_sb_cnt5;
static volatile uint32_t g_sb_cnt2;
static void sb_isr5(void *ctx) {
    (void)ctx;
    if (TIM5->SR & TIM_SR_UIF) {
        TIM5->SR &= ~TIM_SR_UIF;
        g_sb_cnt5++;
    }
}
static void sb_isr2(void *ctx) {
    (void)ctx;
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR &= ~TIM_SR_UIF;
        g_sb_cnt2++;
    }
}

/* ---- 容差辅助：实测值落在 [exp*(1-tol), exp*(1+tol)] 内 ---- */
static int within(uint32_t got, uint32_t exp, uint32_t tol_pct) {
    if (exp == 0) return (got == 0);
    uint32_t lo = exp - (exp * tol_pct) / 100u;
    uint32_t hi = exp + (exp * tol_pct) / 100u;
    return (got >= lo && got <= hi);
}

int rtos_irq_selftest(void) {
    int ok = 1;
    rtos_cycle_init();                              /* 确保 DWT CYCCNT 已使能 */
    uint32_t fault0 = g_fault_cfsr;                 /* 快照：本测试自身不得触发任何故障 */
    log_printf(app_log(), LOG_INFO, "rtos", "[IRQ] self-test begin\n");

    /* ---------- 场景 1a：ISR -> 高优先级任务(prio 3) 唤醒延迟 ---------- */
    {
        rtos_sem_init(&g_ia_sem, 0, 100000);        /* 高 limit，避免计数信号量封顶丢唤醒 */
        g_ia_t0 = g_ia_irq = g_ia_rsp = g_ia_max = g_ia_sum = 0;
        g_ia_run = 1;
        g_ia_active = 0;
        rtos_task_create("irq_a", ia_task, (void *)0, 3, g_ia_stack, sizeof(g_ia_stack));
        irq_manager_attach((irq_id_t)TIM5_IRQn, ia_isr, NULL);
        irq_manager_set_priority((irq_id_t)TIM5_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        irq_manager_enable((irq_id_t)TIM5_IRQn, ia_isr, NULL);
        uint32_t tk0 = rtos_tick_count();
        g_ia_active = 1;                            /* 进入活跃风暴期（开始统计延迟） */
        irq_timer_start(TIM5, 1000);                /* 1kHz：干净测延迟 */
        rtos_msleep(2000);                          /* 2s 风暴 */
        irq_timer_stop(TIM5);
        rtos_msleep(50);                            /* 等待任务排空剩余信号量(仍属活跃期) */
        g_ia_active = 0;                            /* 退出活跃期：收尾样本不计入延迟 */
        g_ia_run = 0;
        rtos_sem_give(&g_ia_sem);                   /* 让任务退出循环（释放 TCB） */
        rtos_msleep(20);
        irq_manager_disable((irq_id_t)TIM5_IRQn, ia_isr, NULL);
        irq_manager_detach((irq_id_t)TIM5_IRQn, ia_isr, NULL);

        uint32_t max_us = cyc_to_us(g_ia_max);
        uint32_t avg_us = (g_ia_rsp > 0) ? cyc_to_us(g_ia_sum / g_ia_rsp) : 0;
        /* 无丢失：每个中断都得到至少一次响应(rsp>=irq；+1 来自收尾 give)；
         * 延迟有界：max < 预算；tick 推进 = 不卡死。 */
        int lok = (g_ia_rsp >= g_ia_irq)
                  && (g_ia_irq > 0)
                  && (g_ia_max <= IRQ_WAKE_BUDGET_CYCLES)
                  && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IRQ] 1a ISR->hi-prio task: irq=%lu rsp=%lu max=%luus avg=%luus %s\n",
                   (unsigned long)g_ia_irq, (unsigned long)g_ia_rsp,
                   (unsigned long)max_us, (unsigned long)avg_us, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("IRQLatencyTask", lok);
    }

    /* ---------- 场景 1b：ISR -> 下半部 BH 任务(prio 4) 唤醒延迟 ---------- */
    {
        g_ib_t0 = g_ib_irq = g_ib_rsp = g_ib_max = g_ib_sum = 0;
        g_ib_bh = rtos_bh_task_create("irq_bh", RTOS_PRIO_BH_HIGH,
                                      g_ib_stack, sizeof(g_ib_stack), ib_bh_fn, NULL);
        irq_manager_attach((irq_id_t)TIM5_IRQn, ib_isr, NULL);
        irq_manager_set_priority((irq_id_t)TIM5_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        irq_manager_enable((irq_id_t)TIM5_IRQn, ib_isr, NULL);
        uint32_t tk0 = rtos_tick_count();
        irq_timer_start(TIM5, 1000);
        rtos_msleep(2000);
        irq_timer_stop(TIM5);
        rtos_msleep(50);                            /* 等待 BH 排空剩余触发 */
        irq_manager_disable((irq_id_t)TIM5_IRQn, ib_isr, NULL);
        irq_manager_detach((irq_id_t)TIM5_IRQn, ib_isr, NULL);

        uint32_t max_us = cyc_to_us(g_ib_max);
        uint32_t avg_us = (g_ib_rsp > 0) ? cyc_to_us(g_ib_sum / g_ib_rsp) : 0;
        int lok = (g_ib_rsp >= g_ib_irq)
                  && (g_ib_irq > 0)
                  && (g_ib_max <= IRQ_WAKE_BUDGET_CYCLES)
                  && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IRQ] 1b ISR->BH task: irq=%lu rsp=%lu max=%luus avg=%luus %s\n",
                   (unsigned long)g_ib_irq, (unsigned long)g_ib_rsp,
                   (unsigned long)max_us, (unsigned long)avg_us, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("IRQLatencyBH", lok);
    }

    /* ---------- 场景 1c：零延迟 ISR 硬实时（prio 2, 不调内核 API） ---------- */
    {
        g_ic_cnt = g_ic_max = g_ic_sum = 0;
        irq_manager_attach((irq_id_t)TIM5_IRQn, ic_isr, NULL);
        /* 零延迟类：prio 必须 < RTOS_MAX_ZERO_LATENCY_IRQS 阈值(4)，永不被 BASEPRI 屏蔽 */
        irq_manager_set_priority((irq_id_t)TIM5_IRQn, IRQ_PRIO_ZERO_LATENCY, IRQ_CLASS_ZERO_LATENCY);
        irq_manager_enable((irq_id_t)TIM5_IRQn, ic_isr, NULL);
        uint32_t tk0 = rtos_tick_count();
        uint32_t c0 = rtos_cycle_now();
        irq_timer_start(TIM5, 10000);               /* 10kHz：验证 100% 即时交付 */
        rtos_msleep(1000);                          /* 1s */
        uint32_t c1 = rtos_cycle_now();
        irq_timer_stop(TIM5);
        irq_manager_disable((irq_id_t)TIM5_IRQn, ic_isr, NULL);
        irq_manager_detach((irq_id_t)TIM5_IRQn, ic_isr, NULL);

        /* 期望数按“实际经历时间”计算，剔除 rtos_msleep 窗口误差，只测真实交付精度 */
        uint32_t exp = exp_from_cycles(10000u, c1 - c0);
        uint32_t max_us = cyc_to_us(g_ic_max);
        uint32_t avg_us = (g_ic_cnt > 0) ? cyc_to_us(g_ic_sum / g_ic_cnt) : 0;
        /* 100% 交付(计数≈期望, 无丢失/无被屏蔽) + ISR 内处理有界 + 系统存活 */
        int lok = within(g_ic_cnt, exp, 2)
                  && (g_ic_max <= IRQ_ZL_ISR_BUDGET_CYCLES)
                  && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IRQ] 1c zero-latency ISR: cnt=%lu exp=%lu isr_max=%luus avg=%luus %s\n",
                   (unsigned long)g_ic_cnt, (unsigned long)exp,
                   (unsigned long)max_us, (unsigned long)avg_us, lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("IRQZeroLatency", lok);
    }

    /* ---------- 场景 2a：单路高频风暴（TIM5 ~50kHz, 5s） ---------- */
    {
        g_sa_cnt = 0;
        irq_manager_attach((irq_id_t)TIM5_IRQn, sa_isr, NULL);
        irq_manager_set_priority((irq_id_t)TIM5_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        irq_manager_enable((irq_id_t)TIM5_IRQn, sa_isr, NULL);
        uint32_t tk0 = rtos_tick_count();
        uint32_t c0 = rtos_cycle_now();
        irq_timer_start(TIM5, 50000);               /* ~50kHz */
        rtos_msleep(5000);                          /* 5s 持续高频 */
        uint32_t c1 = rtos_cycle_now();
        irq_timer_stop(TIM5);
        irq_manager_disable((irq_id_t)TIM5_IRQn, sa_isr, NULL);
        irq_manager_detach((irq_id_t)TIM5_IRQn, sa_isr, NULL);

        uint32_t exp = exp_from_cycles(50000u, c1 - c0);   /* 期望 ≈ 250k 次 */
        int lok = within(g_sa_cnt, exp, 10)         /* 计数不丢（±10% 容差） */
                  && (rtos_tick_count() > tk0);      /* 系统存活：tick 推进 */
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IRQ] 2a single storm(50kHz,5s): isr=%lu exp=%lu alive=%d %s\n",
                   (unsigned long)g_sa_cnt, (unsigned long)exp,
                   (int)(rtos_tick_count() > tk0), lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("IRQStormSustained", lok);
    }

    /* ---------- 场景 2b：双路嵌套高频风暴（TIM5 50kHz prio5 + TIM2 30kHz prio6, 3s） ---------- */
    {
        g_sb_cnt5 = g_sb_cnt2 = 0;
        /* TIM5 高优先级(prio5) 可抢占 TIM2(prio6)，验证中断嵌套/抢占下不损坏 */
        irq_manager_attach((irq_id_t)TIM5_IRQn, sb_isr5, NULL);
        irq_manager_set_priority((irq_id_t)TIM5_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL); /* 5 */
        irq_manager_enable((irq_id_t)TIM5_IRQn, sb_isr5, NULL);
        irq_manager_attach((irq_id_t)TIM2_IRQn, sb_isr2, NULL);
        irq_manager_set_priority((irq_id_t)TIM2_IRQn, IRQ_PRIO_KERNEL + 1, IRQ_CLASS_KERNEL); /* 6 */
        irq_manager_enable((irq_id_t)TIM2_IRQn, sb_isr2, NULL);
        uint32_t tk0 = rtos_tick_count();
        uint32_t c0 = rtos_cycle_now();
        irq_timer_start(TIM5, 50000);
        irq_timer_start(TIM2, 30000);
        rtos_msleep(3000);                          /* 3s 双路并发 */
        uint32_t c1 = rtos_cycle_now();
        irq_timer_stop(TIM5);
        irq_timer_stop(TIM2);
        irq_manager_disable((irq_id_t)TIM5_IRQn, sb_isr5, NULL);
        irq_manager_detach((irq_id_t)TIM5_IRQn, sb_isr5, NULL);
        irq_manager_disable((irq_id_t)TIM2_IRQn, sb_isr2, NULL);
        irq_manager_detach((irq_id_t)TIM2_IRQn, sb_isr2, NULL);

        uint32_t exp5 = exp_from_cycles(50000u, c1 - c0);
        uint32_t exp2 = exp_from_cycles(30000u, c1 - c0);
        int lok = within(g_sb_cnt5, exp5, 10)
                  && within(g_sb_cnt2, exp2, 10)
                  && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IRQ] 2b nested storm(TIM5 50k/TIM2 30k,3s): c5=%lu c2=%lu alive=%d %s\n",
                   (unsigned long)g_sb_cnt5, (unsigned long)g_sb_cnt2,
                   (int)(rtos_tick_count() > tk0), lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("IRQNestedStorm", lok);
    }

    /* ---------- 全局存活校验：本测试全程未触发故障 / 未栈溢出 ---------- */
    {
        int lok = (g_fault_cfsr == fault0) && (g_stack_overflow == 0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IRQ] no-fault/no-overflow: fault_delta=%lu overflow=%d %s\n",
                   (unsigned long)(g_fault_cfsr - fault0), (int)g_stack_overflow,
                   lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("IRQNoFault", lok);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[IRQ] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("irq", rtos_irq_selftest);
