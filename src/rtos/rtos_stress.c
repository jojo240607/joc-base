#include "rtos.h"
#include "log/log.h"
#include "log/app_log.h"
#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * jOS 多任务并发压力自测：从控制台 "RTOSSTRESS" 命令触发。
 * 同时跑多个任务，争用互斥量 / 消息队列 / 信号量 / msleep，验证调度器在并发
 * 阻塞下：不死锁、互斥无丢失更新、mq 保序无丢项、各任务都有进展、无栈溢出。
 *
 * 自测作为 main 任务的一部分运行；子任务在 stop 后置位后自我了结（占住池槽，
 * 本自测设计为单次调用）。prod/cons 用非阻塞 trysend/tryrecv 让出 CPU，避免
 * 测试结束时阻塞在满/空队列上造成死锁。
 * ------------------------------------------------------------------------- */

#define STRESS_WORKERS 3

static volatile uint32_t g_stress_stop;
static volatile uint32_t g_stress_shared;            /* 互斥保护 */
static volatile uint32_t g_stress_taskcnt[STRESS_WORKERS];
static rtos_mutex_t       g_stress_mtx;
static rtos_sem_t         g_stress_done;
static volatile uint32_t g_stress_mq_sum;            /* mq 消费者累加 */
static volatile uint32_t g_stress_mq_items;

/* 工作线程：临界区内同时累加共享计数与各自计数（验证互斥无丢失更新），周期让出 CPU */
static void stress_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    while (!g_stress_stop) {
        rtos_mutex_lock(&g_stress_mtx);
        g_stress_shared++;
        g_stress_taskcnt[id]++;
        rtos_mutex_unlock(&g_stress_mtx);
        rtos_msleep(2);
    }
    rtos_sem_give(&g_stress_done);
}

/* 消息队列生产者：不停发送自增项（非阻塞，满则让出） */
static void stress_prod(void *arg) {
    rtos_mq_t *q = (rtos_mq_t *)arg;
    uint32_t v = 0;
    while (!g_stress_stop) {
        if (rtos_mq_trysend(q, &v) == 0) v++;
        rtos_msleep(1);
    }
    rtos_sem_give(&g_stress_done);
}

/* 消息队列消费者：接收并累加（非阻塞，空则让出） */
static void stress_cons(void *arg) {
    rtos_mq_t *q = (rtos_mq_t *)arg;
    int v = 0;
    while (!g_stress_stop) {
        if (rtos_mq_tryrecv(q, &v) == 0) {
            g_stress_mq_sum += (uint32_t)v;
            g_stress_mq_items++;
        }
        rtos_msleep(1);
    }
    rtos_sem_give(&g_stress_done);
}

int rtos_stress_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[STRESS] self-test begin\n");

    g_stress_stop    = 0;
    g_stress_shared  = 0;
    g_stress_mq_sum  = 0;
    g_stress_mq_items = 0;
    for (int i = 0; i < STRESS_WORKERS; i++) g_stress_taskcnt[i] = 0;

    rtos_mutex_init(&g_stress_mtx, 14);          /* 天花板高于所有使用者(20..24) */
    rtos_sem_init(&g_stress_done, 0, 8);

    static uint8_t st_w[STRESS_WORKERS][1024] __attribute__((aligned(8)));
    for (int i = 0; i < STRESS_WORKERS; i++)
        rtos_task_create("stress_w", stress_worker, (void *)(intptr_t)i,
                         (uint8_t)(20 + i), st_w[i], sizeof(st_w[i]));

    static int       mq_buf[16];
    static rtos_mq_t mq; rtos_mq_init(&mq, mq_buf, sizeof(int), 16);
    static uint8_t st_p[1024] __attribute__((aligned(8)));
    static uint8_t st_c[1024] __attribute__((aligned(8)));
    rtos_task_create("stress_prod", stress_prod, &mq, 23, st_p, sizeof(st_p));
    rtos_task_create("stress_cons", stress_cons, &mq, 24, st_c, sizeof(st_c));

    uint32_t t0 = rtos_tick_count();
    rtos_msleep(1500);                           /* 压测 1.5s */
    g_stress_stop = 1;

    /* 等所有子任务退出（stop 后自我了结） */
    for (int i = 0; i < STRESS_WORKERS + 2; i++) rtos_sem_wait(&g_stress_done);

    uint32_t dt = rtos_tick_count() - t0;

    /* 1) 互斥无丢失更新：共享计数必须等于各任务计数之和 */
    uint32_t sum_tc = 0;
    for (int i = 0; i < STRESS_WORKERS; i++) sum_tc += g_stress_taskcnt[i];
    {
        int lok = (g_stress_shared == sum_tc) && (sum_tc > 0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[STRESS] mutex no-lost-update: shared=%lu sum(task)=%lu %s\n",
                   (unsigned long)g_stress_shared, (unsigned long)sum_tc,
                   lok ? "PASS" : "FAIL");
    }

    /* 2) 各任务都有进展（无饥饿） */
    {
        int lok = 1;
        for (int i = 0; i < STRESS_WORKERS; i++)
            if (g_stress_taskcnt[i] == 0) lok = 0;
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[STRESS] all workers progressed: w0=%lu w1=%lu w2=%lu %s\n",
                   (unsigned long)g_stress_taskcnt[0],
                   (unsigned long)g_stress_taskcnt[1],
                   (unsigned long)g_stress_taskcnt[2],
                   lok ? "PASS" : "FAIL");
    }

    /* 3) 消息队列保序无丢项：sum(0..N-1) == N*(N-1)/2 */
    {
        uint32_t N = g_stress_mq_items;
        uint32_t expect = (N > 0) ? (N * (N - 1) / 2u) : 0;
        int lok = (N > 0) && (g_stress_mq_sum == expect);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[STRESS] mq ordered no-drop: items=%lu sum=%lu expect=%lu %s\n",
                   (unsigned long)N, (unsigned long)g_stress_mq_sum,
                   (unsigned long)expect, lok ? "PASS" : "FAIL");
    }

    /* 4) 调度器在跑（节拍推进） + 无栈溢出 */
    {
        int lok = (dt >= 1000) && (g_stack_overflow == 0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[STRESS] scheduler live + no overflow: dt=%lums overflow=%d %s\n",
                   (unsigned long)dt, (int)g_stack_overflow, lok ? "PASS" : "FAIL");
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[STRESS] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
