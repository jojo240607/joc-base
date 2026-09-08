/*
 * ostest_isr.c — 对照 docs/ostest.md 补齐的 ISR 缺口用例（平台自测，STM32 家族）。
 *
 * 依赖芯片定时器（TIM2/TIM4 溢出中断经统一 irq_manager 框架回调），故从 RTOS 核心的
 * rtos_ostest.c 下沉到 src/hal/stm32/test/，只保留在 STM32 平台编译；条目 "ostest_isr"
 * 通过 RTOS_SELFTEST_ADD 段收集进 RTOSALL。
 *
 * 覆盖：
 *   - TC-Q-004    FromISR 发队列（TIM2 ISR 经 rtos_mq_send_fromisr 投递，任务接收）
 *   - TC-KERNEL-004 挂起/恢复原子性（TIM4 ISR 并发 resume vs 任务 self-suspend）
 *   - TC-INT-001  FromISR give 信号量唤醒任务（TIM2 ISR）
 *   - TC-INT-003  高频中断负载下任务仍有机会运行（TIM2 ISR 风暴）
 *
 * 与 ipc2_s05.c 约定一致：外部中断必须走 irq_manager_attach/enable 注册回调
 * （IRQ_PRIO_KERNEL，因回调调用内核 API，优先级数须 >= 阈值 4）。
 */
#include "rtos.h"
#include "rtos/core/rtos_internal.h"   /* task_t 状态常量 / rtos_running */
#include "log/log.h"
#include "log/app_log.h"
#include "irq/irq.h"
#include "irq/irq_manager.h"
#ifdef STM32F103xx
#include "stm32f103xx.h"      /* TIM2/TIM4 / RCC / TIMx_IRQn */
#elif defined(STM32F407xx)
#include "stm32f4xx.h"         /* TIM2 / TIM4 / RCC / TIMx_IRQn */
#else
#error "ostest_isr.c: unsupported platform (needs TIM2/TIM4)"
#endif

#include <stdint.h>

/* 自测任务栈放主 SRAM(.bss) 而非 CCM(.ccm_bss)——与 rtos_ostest.c 约定一致：
 * 自测任务纯 CPU、无 DMA，CCM 应留给常驻任务与 TCB 池。 */
#undef RTOS_TASK_STACK
#define RTOS_TASK_STACK(name, sz) \
    static uint8_t name[sz] __attribute__((aligned(RTOS_STACK_ALIGN_UP(sz))))

/* ===================== TIM 启动/停止 辅助（F1=72MHz / F4=84MHz APB1 定时器时钟） ===== */
static uint32_t ostim_psc(void) {
#ifdef STM32F103xx
    return 71u;                              /* 72MHz / 72 = 1MHz 计数 */
#else
    return 83u;                              /* 84MHz / 84 = 1MHz 计数 */
#endif
}
static void ostim_start(TIM_TypeDef *tim, uint32_t rcc_bit, uint32_t hz) {
    RCC->APB1ENR |= rcc_bit;
    tim->CR1   = 0;
    tim->PSC   = ostim_psc();
    tim->ARR   = (1000000u / hz) - 1u;       /* 1MHz 计数，达到 hz 溢出 */
    tim->DIER |= TIM_DIER_UIE;
    tim->CNT   = 0;
    tim->SR    = 0;
    tim->CR1  |= TIM_CR1_CEN;
}
static void ostim_stop(TIM_TypeDef *tim, uint32_t rcc_bit) {
    tim->CR1 &= ~TIM_CR1_CEN;
    RCC->APB1ENR &= ~rcc_bit;
}

/* ===================== TC-Q-004 FromISR 发队列：接收任务 + TIM2 ISR ============= */
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

/* ===================== TC-INT-001/003 FromISR give 信号量 + 高频中断负载 ========= */
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

/* ===================== TC-KERNEL-004 挂起/恢复并发：TIM4 ISR ==================== */
static volatile int       g_k4_run;
static volatile uint32_t  g_k4_cnt;
static volatile uint32_t  g_k4_isr_cnt;
static task_t            *g_k4_tk;
static void k4_task(void *arg) {
    (void)arg;
    /* 自挂起循环：每轮先 suspend 自身 -> 等 ISR 在中断上下文 resume -> 恢复后计数。
     * 真实 TIM4 溢出 ISR（~1kHz）调 rtos_task_resume，与这里的 self-suspend 形成【真
     * 并发】竞争（不是顺序调用）。任务优先级 22 低于 main(16)，不饿死自测任务。
     * 关键点：ISR 可在“刚 unlink、尚未完成 PendSV 切换”的窗口抢入 resume，正好压到
     * suspend/resume 的原子性边界——若内核在该窗口有竞态（g_running 仍留就绪表 /
     * 状态机不一致），本任务会丢失（僵尸态，永不再被调度）或系统崩，从而被测出。 */
    while (g_k4_run) {
        rtos_task_suspend(rtos_running());
        g_k4_cnt++;
    }
}
static void k4_tim4_isr(void *ctx) {
    (void)ctx;
    if (TIM4->SR & TIM_SR_UIF) {
        TIM4->SR &= ~TIM_SR_UIF;          /* 清溢出标志，否则中断重入 */
        g_k4_isr_cnt++;
        if (g_k4_tk) rtos_task_resume(g_k4_tk);   /* ISR 安全：唤醒被挂起的 k4 */
    }
}

/* ===================== 自测入口：TC-Q-004 / TC-KERNEL-004 / TC-INT-001 / 003 ===== */
int rtos_ostest_isr_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] isr gap-cases begin\n");

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
        ostim_start(TIM2, RCC_APB1ENR_TIM2EN, 5000);   /* ~5kHz */
        rtos_sem_wait(&g_isr_done);
        ostim_stop(TIM2, RCC_APB1ENR_TIM2EN);
        irq_manager_disable((irq_id_t)TIM2_IRQn, isr_q_isr, NULL);
        irq_manager_detach((irq_id_t)TIM2_IRQn, isr_q_isr, NULL);
        int lok = (g_isr_n == 4) && (g_isr_rx_buf[0] == 1) && (g_isr_rx_buf[3] == 4);
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-Q-004", lok);
    }

    /* TC-KERNEL-004: 挂起/恢复原子性（真实 TIM4 ISR 并发 resume vs 任务 self-suspend） */
    {
        g_k4_run = 1; g_k4_cnt = 0; g_k4_isr_cnt = 0; g_k4_tk = (task_t *)0;
        RTOS_TASK_STACK(k4s, 512);
        rtos_task_create("ok_k4", k4_task, 0, 22, k4s, sizeof(k4s));  /* 低于 main(16) */
        g_k4_tk = (task_t *)rtos_kobj_lookup("ok_k4");
        /* 真实 TIM4 溢出 ISR（~1kHz）在中断上下文对 ok_k4 调 resume，与任务自身的
         * self-suspend 真并发。验证：任务持续被唤醒（cnt 增长）、终态合法、无僵尸态、
         * 系统不崩——若 suspend/resume 有原子性竞态（g_running 留就绪表 / 状态机错乱），
         * 任务会丢失调度或系统崩，从而被测出。 */
        if (g_k4_tk) {
            irq_manager_attach((irq_id_t)TIM4_IRQn, k4_tim4_isr, NULL);
            irq_manager_set_priority((irq_id_t)TIM4_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
            irq_manager_enable((irq_id_t)TIM4_IRQn, k4_tim4_isr, NULL);
            ostim_start(TIM4, RCC_APB1ENR_TIM4EN, 1000);
            rtos_msleep(400);                 /* 让 ISR 与 self-suspend 高频交错 */
            ostim_stop(TIM4, RCC_APB1ENR_TIM4EN);
            irq_manager_disable((irq_id_t)TIM4_IRQn, k4_tim4_isr, NULL);
            irq_manager_detach((irq_id_t)TIM4_IRQn, k4_tim4_isr, NULL);
        }
        int state_ok = (g_k4_tk == 0) ||
                       (g_k4_tk->state == TASK_READY) ||
                       (g_k4_tk->state == TASK_SUSPENDED);
        int lok = (g_k4_tk != 0) && (g_k4_isr_cnt > 100) && (g_k4_cnt > 100) &&
                  state_ok && rtos_is_started();
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-KERNEL-004", lok);
        /* 收尾：停止竞争后让 k4 自行退出（置 run=0 并恢复，使其退出循环 ->
         * rtos_task_exit 置 DEAD，槽位释放） */
        g_k4_run = 0;
        if (g_k4_tk) rtos_task_resume(g_k4_tk);
        rtos_msleep(30);
    }

    /* TC-INT-001: FromISR give 信号量唤醒任务（TC-Q-004 已覆盖 FromISR 发队列） */
    {
        rtos_sem_init(&g_oi_sem, 0, 1);
        g_oi_isr_cnt = 0; g_oi_wake = 0; g_oi_run = 1;
        RTOS_TASK_STACK(oiw, 512);
        rtos_task_create("oi_w", oi_waiter, 0, 6, oiw, sizeof(oiw));
        irq_manager_attach((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        irq_manager_set_priority((irq_id_t)TIM2_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        irq_manager_enable((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        ostim_start(TIM2, RCC_APB1ENR_TIM2EN, 10000);   /* ~10kHz */
        rtos_msleep(500);
        ostim_stop(TIM2, RCC_APB1ENR_TIM2EN);
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
        ostim_start(TIM2, RCC_APB1ENR_TIM2EN, 50000);   /* ~50kHz */
        rtos_msleep(500);
        ostim_stop(TIM2, RCC_APB1ENR_TIM2EN);
        g_oi_run = 0;
        rtos_msleep(20);
        irq_manager_disable((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        irq_manager_detach((irq_id_t)TIM2_IRQn, oi_storm_isr, NULL);
        int lok = (g_oi_isr_cnt > 5000) && (g_oi_task_work > 10);  /* 任务确有机会运行 */
        if (!lok) ok = 0;
        RTOS_TEST_RESULT("TC-INT-003", lok);
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[OSTEST] isr gap-cases: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ostest_isr", rtos_ostest_isr_selftest);
