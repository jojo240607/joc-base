/*
 * ipc2_s05.c — RTOSIPC2 的 S05 用例（平台自测，STM32 家族；条目 "ipc2_s05"）。
 *
 * 从真实 TIM3 溢出中断（~1kHz）在 ISR 内调用 rtos_sem_give（ISR 安全），验证
 * 等待的高优先级任务被唤醒并立即运行。依赖芯片定时器，故下沉到 src/hal/stm32/test/
 * （RTOS 核心的 rtos_ipc2.c 只保留平台无关的 S01/S03/S04）。
 *
 * 注意：本 RTOS 所有外部中断经统一 irq_manager 框架走唯一 IRQ_CommonHandler，
 * 向量表每个设备 IRQ 槽都指向它（见 startup 向量表）。因此【严禁】像裸机那样直接
 * 定义弱符号 TIM3_IRQHandler + NVIC_EnableIRQ：那样 TIM3 触发时仍由 IRQ_CommonHandler
 * 分发，而 irq_manager 中没有为 TIM3 注册回调 -> 空回调 / 故障 -> 板子冻结。必须走
 * irq_manager_attach/enable 注册回调（IRQ_PRIO_KERNEL，因为它调用内核 API，优先级数
 * 须 >= 阈值 4，通过启动期审计）。
 */
#include "rtos.h"
#include "log/log.h"
#include "log/app_log.h"
#include "irq/irq.h"
#include "irq/irq_manager.h"
#ifdef STM32F103xx
#include "stm32f103xx.h"      /* TIM3 / RCC / TIM3_IRQn */
#elif defined(STM32F407xx)
#include "stm32f4xx.h"         /* TIM3 / RCC / TIM3_IRQn — test needs a real timer ISR */
#else
#error "ipc2_s05.c: unsupported platform (needs a real TIM3)"
#endif

#include <stdint.h>

static rtos_sem_t g_s05_sem;
static volatile int g_s05_wake;
static volatile uint32_t g_s05_isr_cnt;

/* irq_manager 回调：从 ISR 上下文调用 rtos_sem_give（ISR 安全）唤醒等待任务 */
static void s05_tim3_isr(void *ctx) {
    (void)ctx;
    if (TIM3->SR & TIM_SR_UIF) {
        TIM3->SR &= ~TIM_SR_UIF;          /* 清溢出标志，否则中断重入 */
        g_s05_isr_cnt++;
        rtos_sem_give(&g_s05_sem);        /* ISR 安全：唤醒等待者 + 请求 PendSV */
    }
}
static void s05_timer3_start(uint32_t hz) {
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    TIM3->CR1   = 0;
#ifdef STM32F103xx
    TIM3->PSC   = 71;                              /* 72MHz / 72 = 1MHz 计数 */
    TIM3->ARR   = (72000000u / 72u / hz) - 1u;     /* 达到 hz 溢出 */
#else
    TIM3->PSC   = 83;                              /* 84MHz / 84 = 1MHz 计数 */
    TIM3->ARR   = (84000000u / 84u / hz) - 1u;     /* 达到 hz 溢出 */
#endif
    TIM3->DIER |= TIM_DIER_UIE;
    TIM3->CNT   = 0;
    TIM3->SR    = 0;
    TIM3->CR1  |= TIM_CR1_CEN;
}
static void s05_timer3_stop(void) {
    TIM3->CR1 &= ~TIM_CR1_CEN;
    RCC->APB1ENR &= ~RCC_APB1ENR_TIM3EN;
}
static void s05_waiter(void *arg) {
    (void)arg;
    rtos_sem_wait(&g_s05_sem);    /* 阻塞，直到 TIM3 ISR give */
    g_s05_wake = 1;
    rtos_msleep(10);
}
static int s05_from_isr(void) {
    rtos_sem_init(&g_s05_sem, 0, 1);
    g_s05_wake = 0; g_s05_isr_cnt = 0;
    RTOS_TASK_STACK(st, 512);
    rtos_task_create("s05w", s05_waiter, (void *)0, 6, st, sizeof(st));
    rtos_msleep(20);              /* 让等待者先阻塞在信号量上 */

    /* 经 irq_manager 注册 TIM3 回调（必须在使能定时器前完成，避免空窗触发空回调） */
    irq_manager_attach((irq_id_t)TIM3_IRQn, s05_tim3_isr, NULL);
    irq_manager_set_priority((irq_id_t)TIM3_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
    irq_manager_enable((irq_id_t)TIM3_IRQn, s05_tim3_isr, NULL);

    log_printf(app_log(), LOG_INFO, "rtos", "[IPC2] S05: TIM3 armed, starting\n");
    s05_timer3_start(1000);       /* ~1kHz 溢出中断 */
    uint32_t to = 0;
    while (!g_s05_wake && to < 1000) { rtos_msleep(2); to += 2; }
    int lok = (g_s05_wake == 1) && (g_s05_isr_cnt > 0);
    s05_timer3_stop();
    irq_manager_disable((irq_id_t)TIM3_IRQn, s05_tim3_isr, NULL);
    irq_manager_detach((irq_id_t)TIM3_IRQn, s05_tim3_isr, NULL);
    return lok;
}

int rtos_ipc2_s05_selftest(void) {
    int ok = s05_from_isr();
    log_printf(app_log(), LOG_INFO, "rtos",
               "[IPC2] S05 sem-give from TIM3 ISR wakes task: %s\n", ok ? "PASS" : "FAIL");
    RTOS_TEST_RESULT("S05_SemGiveFromISR", ok);
    return ok;
}
RTOS_SELFTEST_ADD("ipc2_s05", rtos_ipc2_s05_selftest);
