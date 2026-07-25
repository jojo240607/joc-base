#include "rtos.h"
#include "log/log.h"
#include "log/app_log.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * jOS IPC 运行时自测：从控制台 "RTOSIPC" 命令触发。
 * 覆盖信号量语义、互斥量压力（无丢失更新）、消息队列生产/消费、事件标志唤醒。
 * 自测作为 main 任务的一部分运行；它创建的子任务结束后变为 DEAD（占住池槽，
 * 故本自测设计为单次调用）。
 * ------------------------------------------------------------------------- */

static volatile uint32_t g_ipc_cnt;
static rtos_mutex_t       g_ipc_mtx;
static rtos_sem_t         g_ipc_done;
static volatile int       g_ipc_ev_seen;

/* 互斥量压力：临界区内累加，验证无竞态丢失 */
static void ipc_mutex_worker(void *arg) {
    int n = (int)(intptr_t)arg;
    for (int i = 0; i < n; i++) {
        rtos_mutex_lock(&g_ipc_mtx);
        g_ipc_cnt++;
        rtos_mutex_unlock(&g_ipc_mtx);
    }
    rtos_sem_give(&g_ipc_done);
}

/* 消息队列生产者：发送 0..9 */
static void ipc_mq_prod(void *arg) {
    rtos_mq_t *q = (rtos_mq_t *)arg;
    for (int i = 0; i < 10; i++) {
        int v = i;
        rtos_mq_send(q, &v);
    }
    rtos_sem_give(&g_ipc_done);
}

/* 事件等待者：等待 bit0 */
static void ipc_ev_waiter(void *arg) {
    rtos_event_t *e = (rtos_event_t *)arg;
    uint32_t f = rtos_event_wait(e, 0x1, 0, 1);
    g_ipc_ev_seen = (int)(f & 0x1u);
    rtos_sem_give(&g_ipc_done);
}

int rtos_ipc_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[IPC] self-test begin\n");

    /* 1) 信号量基本语义 */
    rtos_sem_t sem; rtos_sem_init(&sem, 0, 5);
    rtos_sem_give(&sem);                 /* count=1 */
    if (rtos_sem_trywait(&sem) != 0) ok = 0;
    if (rtos_sem_trywait(&sem) == 0) ok = 0;   /* 应为空 */
    rtos_sem_give(&sem); rtos_sem_give(&sem);  /* count=2 */
    if (sem.count != 2) ok = 0;
    rtos_kobj_register("ipc_sem", KOBJ_SEM, &sem);
    log_printf(app_log(), LOG_INFO, "rtos", "[IPC] sem semantics: %s\n", ok ? "PASS" : "FAIL");

    /* 2) 互斥量压力：2 任务各 +500，期望 1000（无丢失更新） */
    rtos_mutex_init(&g_ipc_mtx, RTOS_PRIO_BLINK);  /* 天花板高于所有使用者 */
    g_ipc_cnt = 0;
    rtos_sem_init(&g_ipc_done, 0, 8);
    static uint8_t st_a[1024] __attribute__((aligned(8)));
    static uint8_t st_b[1024] __attribute__((aligned(8)));
    rtos_task_create("ipc_mA", ipc_mutex_worker, (void *)(intptr_t)500, 10, st_a, sizeof(st_a));
    rtos_task_create("ipc_mB", ipc_mutex_worker, (void *)(intptr_t)500, 11, st_b, sizeof(st_b));
    rtos_sem_wait(&g_ipc_done);
    rtos_sem_wait(&g_ipc_done);
    {
        int lok = (g_ipc_cnt == 1000);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos", "[IPC] mutex stress: cnt=%lu (expect 1000) %s\n",
                   (unsigned long)g_ipc_cnt, lok ? "PASS" : "FAIL");
    }

    /* 3) 消息队列：生产者发 10 个数，主任务收 10 个并校验和 */
    static int       mq_buf[10];
    static rtos_mq_t mq; rtos_mq_init(&mq, mq_buf, sizeof(int), 10);
    rtos_sem_init(&g_ipc_done, 0, 8);
    static uint8_t st_p[1024] __attribute__((aligned(8)));
    rtos_task_create("ipc_prod", ipc_mq_prod, &mq, 12, st_p, sizeof(st_p));
    {
        int sum = 0, got = 0, v = 0;
        for (int i = 0; i < 10; i++)
            if (rtos_mq_recv(&mq, &v) == 0) { sum += v; got++; }
        rtos_sem_wait(&g_ipc_done);   /* 等生产者结束 */
        int lok = (got == 10 && sum == 45);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos", "[IPC] mq: got=%d sum=%d (expect 10/45) %s\n",
                   got, sum, lok ? "PASS" : "FAIL");
    }
    rtos_kobj_register("ipc_mq", KOBJ_MQ, &mq);

    /* 4) 事件标志：等待者阻塞，主任务置位后唤醒 */
    static rtos_event_t ev; rtos_event_init(&ev);
    g_ipc_ev_seen = 0;
    rtos_sem_init(&g_ipc_done, 0, 8);
    static uint8_t st_e[768] __attribute__((aligned(8)));
    rtos_task_create("ipc_ew", ipc_ev_waiter, &ev, 13, st_e, sizeof(st_e));
    rtos_msleep(20);                  /* 让等待者先阻塞在事件上 */
    rtos_event_set(&ev, 0x1);         /* 置位唤醒 */
    rtos_sem_wait(&g_ipc_done);       /* 等等待者确认 */
    {
        int lok = (g_ipc_ev_seen == 1);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos", "[IPC] event: seen=%d (expect 1) %s\n",
                   g_ipc_ev_seen, lok ? "PASS" : "FAIL");
    }
    rtos_kobj_register("ipc_ev", KOBJ_EVENT, &ev);

    log_printf(app_log(), LOG_INFO, "rtos", "[IPC] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* 编译期注册：RTOSALL 会遍历该段依次执行 */
RTOS_SELFTEST_ADD("ipc", rtos_ipc_selftest);

/* 遍历链接器收集到的所有自测项（.rtos_selftests.* 段），依次运行 */
int rtos_selftest_run_all(void) {
    int ok = 1;
    const rtos_selftest_entry_t *end = __rtos_selftest_end;
    size_t n = (size_t)(end - __rtos_selftest_start);
    log_printf(app_log(), LOG_INFO, "rtos", "[SELFTEST] run-all begin (%u entries)\n",
               (unsigned)n);
    if (n == 0) {
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[SELFTEST] WARNING: no self-test entries collected (check linker .rtos_selftests)\n");
    }
    for (const rtos_selftest_entry_t *p = __rtos_selftest_start; p < end; p++) {
        const char *nm = p->name ? p->name : "?";
        log_printf(app_log(), LOG_INFO, "rtos", "[SELFTEST] >>> %s\n", nm);
        int r = p->fn();
        if (!r) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos", "[SELFTEST] %s: %s\n", nm, r ? "PASS" : "FAIL");
    }
    log_printf(app_log(), LOG_INFO, "rtos", "[SELFTEST] ALL: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
