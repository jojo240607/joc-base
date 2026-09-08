#include "rtos.h"
#include "log/log.h"
#include "log/app_log.h"
#include "irq/irq.h"
#include "irq/irq_manager.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 同步与通信边界自测（RTOSIPC2 命令，并注册进 RTOSALL "ipc2" 条目）。
 * 覆盖准则 §2.3 S01(信号量计数边界) / S03(队列满空溢出) / S04(裸 event 广播)。
 *          S05(从 ISR 发信号量唤醒高优先级任务)依赖真实定时器，已下沉到
 *          src/hal/<platform>/test/ 目录（如 STM32 家族见 ipc2_s05.c）。
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

int rtos_ipc2_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[IPC2] self-test begin\n");

    /* S01 */
    {
        int lok = s01_ok();
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IPC2] S01 sem count-bound: %s\n", lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("S01_SemCountBound", lok);
    }
    /* S03 边界 + 有序 */
    {
        int lok = s03_boundary() && s03_ordered();
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IPC2] S03 mq full/empty/ordered: %s\n", lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("S03_MqFullEmptyOrdered", lok);
    }
    /* S04 广播 */
    {
        int lok = s04_broadcast();
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[IPC2] S04 event broadcast(5 waiters): %s\n", lok ? "PASS" : "FAIL");
        RTOS_TEST_RESULT("S04_EventBroadcast", lok);
    }
    log_printf(app_log(), LOG_INFO, "rtos", "[IPC2] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("ipc2", rtos_ipc2_selftest);
