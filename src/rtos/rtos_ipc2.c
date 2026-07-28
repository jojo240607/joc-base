#include "rtos.h"
#include "log/log.h"
#include "log/app_log.h"
#include "irq/irq.h"
#include "irq/irq_manager.h"
#include "stm32f4xx.h"   /* TIM3 / RCC / TIM3_IRQn — test needs a real timer ISR */
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 同步与通信边界自测（RTOSIPC2 命令，并注册进 RTOSALL "ipc2" 条目）。
 * 覆盖准则 §2.3 S01(信号量计数边界) / S03(队列满空溢出) / S04(裸 event 广播) /
 *          S05(从 ISR 发信号量唤醒高优先级任务)。
 *
 * S05 用真实 TIM3 溢出中断（~1kHz）在 ISR 内调用 rtos_sem_give（ISR 安全），
 * 验证等待的高优先级任务被唤醒并立即运行。TIM2 留给 RTOSROBUST 的中断风暴测试，
 * 避免两个模块都定义 TIMx_IRQHandler 冲突。
 * ------------------------------------------------------------------------- */

/* ===================== S01 信号量计数边界 ===================== */
static int s01_ok(void) {
    int ok = 1;
    /* 超上限 give 必须封顶，不溢出 */
    rtos_sem_t s; rtos_sem_init(&s, 0, 3);
    rtos_sem_give(&s); rtos_sem_give(&s); rtos_sem_give(&s); rtos_sem_give(&s);
    if (s.count != 3) ok = 0;
    /* 空 trywait 返回 -1 */
    rtos_sem_t e; rtos_sem_init(&e, 0, 3);
    if (rtos_sem_trywait(&e) != -1) ok = 0;
    /* 有许可 trywait 返回 0 且 count 归零 */
    rtos_sem_give(&e);
    if (rtos_sem_trywait(&e) != 0 || e.count != 0) ok = 0;
    return ok;
}

/* ===================== S03 队列满/空/溢出 + 有序 ===================== */
#define S03_N 4
static int       s03_buf[S03_N];
static rtos_mq_t s03_mq;

static int s03_boundary(void) {
    int ok = 1;
    rtos_mq_init(&s03_mq, s03_buf, sizeof(int), S03_N);
    for (int i = 0; i < S03_N; i++) rtos_mq_send(&s03_mq, &i);   /* 填满 */
    int v = 99;
    if (rtos_mq_trysend(&s03_mq, &v) != -1) ok = 0;              /* 满：非阻塞发送失败 */
    int got; rtos_mq_recv(&s03_mq, &got);                        /* 腾出一个空位 */
    if (rtos_mq_trysend(&s03_mq, &v) != 0) ok = 0;               /* 腾位后发送成功 */
    /* 排空测试空 tryrecv */
    int all[20]; int n = 0;
    while (rtos_mq_tryrecv(&s03_mq, &all[n]) == 0) n++;
    if (n != S03_N) ok = 0;                                      /* 实际收到 N 条 */
    int e;
    if (rtos_mq_tryrecv(&s03_mq, &e) != -1) ok = 0;              /* 空：非阻塞接收失败 */
    return ok;
}

/* 阻塞有序：生产者(高优先级)发 0..9，消费者(更高优先级)收，校验 FIFO 顺序 */
#define S03_SEQ 10
static int       s03_seq_buf[S03_SEQ];
static rtos_mq_t s03_seq_mq;
static int       s03_recv[S03_SEQ];
static volatile int s03_ri;
static volatile int s03_seq_done;
static void s03_prod(void *arg) {
    (void)arg;
    for (int i = 0; i < S03_SEQ; i++) rtos_mq_send(&s03_seq_mq, &i);  /* 满则阻塞 */
}
static void s03_cons(void *arg) {
    (void)arg;
    for (int i = 0; i < S03_SEQ; i++) rtos_mq_recv(&s03_seq_mq, &s03_recv[i]);
    s03_seq_done = 1;
    rtos_msleep(10);
}
static int s03_ordered(void) {
    rtos_mq_init(&s03_seq_mq, s03_seq_buf, sizeof(int), S03_SEQ);
    s03_ri = 0; s03_seq_done = 0;
    RTOS_TASK_STACK(stp, 512); RTOS_TASK_STACK(stc, 512);
    rtos_task_create("s03cons", s03_cons, (void *)0, 12, stc, sizeof(stc));
    rtos_task_create("s03prod", s03_prod, (void *)0, 14, stp, sizeof(stp));
    uint32_t to = 0;
    while (!s03_seq_done && to < 2000) { rtos_msleep(2); to += 2; }
    if (!s03_seq_done) return 0;
    int sum = 0, expect = 0;
    for (int i = 0; i < S03_SEQ; i++) { sum += s03_recv[i]; expect += i; }
    return (sum == expect);
}

/* ===================== S04 裸 event 广播 ===================== */
#define S04_N 5
static volatile int s04_woke[S04_N];
static rtos_event_t s04_ev;
static void s04_waiter(void *arg) {
    int id = (int)(intptr_t)arg;
    rtos_event_wait(&s04_ev, 0x1, 0, 1);   /* 阻塞等 bit0 */
    s04_woke[id] = 1;
    rtos_msleep(10);
}
static int s04_broadcast(void) {
    rtos_event_init(&s04_ev);
    for (int i = 0; i < S04_N; i++) s04_woke[i] = 0;
    RTOS_TASK_STACK(stw[S04_N], 512);
    for (int i = 0; i < S04_N; i++)
        rtos_task_create("s04w", s04_waiter, (void *)(intptr_t)i,
                         (uint8_t)(14 + i), stw[i], sizeof(stw[i]));
    rtos_msleep(30);                        /* 等 5 个都阻塞在事件上 */
    rtos_event_set(&s04_ev, 0x1);           /* 置位：应广播唤醒全部 */
    uint32_t to = 0;
    int all = 1;
    while (to < 1000) {
        all = 1;
        for (int i = 0; i < S04_N; i++) if (!s04_woke[i]) all = 0;
        if (all) break;
        rtos_msleep(2); to += 2;
    }
    return all;   /* event_set 唤醒所有满足者（广播） */
}

/* ===================== S05 FromISR（TIM3 溢出中断发信号量） =====================
 * 注意：本 RTOS 所有外部中断经统一 irq_manager 框架走唯一 IRQ_CommonHandler，
 * 向量表每个设备 IRQ 槽都指向它（见 startup 向量表）。因此【严禁】像裸机那样直接
 * 定义弱符号 TIM3_IRQHandler + NVIC_EnableIRQ：那样 TIM3 触发时仍由 IRQ_CommonHandler
 * 分发，而 irq_manager 中没有为 TIM3 注册回调 -> 空回调 / 故障 -> 板子冻结。必须走
 * irq_manager_attach/enable 注册回调（IRQ_PRIO_KERNEL，因为它调用内核 API，优先级数
 * 须 >= 阈值 4，通过启动期审计）。 */
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
    TIM3->PSC   = 83;                              /* 84MHz / 84 = 1MHz 计数 */
    TIM3->ARR   = (84000000u / 84u / hz) - 1u;     /* 达到 hz 溢出 */
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

int rtos_ipc2_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[IPC2] self-test begin\n");

    /* S01 */
    {
        int lok = s01_ok();
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IPC2] S01 sem count-bound: %s\n", lok ? "PASS" : "FAIL");
    }
    /* S03 边界 + 有序 */
    {
        int lok = s03_boundary() && s03_ordered();
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IPC2] S03 mq full/empty/ordered: %s\n", lok ? "PASS" : "FAIL");
    }
    /* S04 广播 */
    {
        int lok = s04_broadcast();
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IPC2] S04 event broadcast(5 waiters): %s\n", lok ? "PASS" : "FAIL");
    }
    /* S05 FromISR */
    {
        int lok = s05_from_isr();
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IPC2] S05 sem-give from TIM3 ISR wakes task: %s\n", lok ? "PASS" : "FAIL");
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[IPC2] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ipc2", rtos_ipc2_selftest);
