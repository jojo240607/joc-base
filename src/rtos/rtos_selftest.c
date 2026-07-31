#include "rtos.h"
#include "log/log.h"
#include "log/app_log.h"
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS IPC 运行时自测：从控制台 "RTOSIPC" 命令触发。
 * 覆盖信号量语义、互斥量压力（无丢失更新）、消息队列生产/消费、事件标志唤醒。
 * 自测作为 main 任务的一部分运行；它创建的子任务结束后变为 DEAD，其池槽由
 * rtos_task_create 回收复用，故本自测现在可重复运行（可多次从控制台触发）。
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
    RTOS_TASK_STACK(st_a, 1024);
    RTOS_TASK_STACK(st_b, 1024);
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
    RTOS_TASK_STACK(st_p, 1024);
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
    RTOS_TASK_STACK(st_e, 768);
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

    log_printf(app_log(), LOG_INFO, "rtos", "[IPC] misuse-count=%lu (expect 0 in normal use)\n",
               (unsigned long)rtos_ipc_misuse_count());
    log_printf(app_log(), LOG_INFO, "rtos", "[IPC] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* 编译期注册：RTOSALL 会遍历该段依次执行 */
RTOS_SELFTEST_ADD("ipc", rtos_ipc_selftest);

/* ===========================================================================
 * 时间片轮转（Round-Robin）自测（docs/rtos-design.md §1/§3）。
 * 两个同优先级、计算密集型（不阻塞）任务：若无时间片，先运行的会独占 CPU，
 * 另一个永远饿死；开启 RTOS_TIME_SLICE 后两者应交替运行，计数都增长。
 * 从 "RTOSRR" 命令触发 + 注册进 RTOSALL。
 * ========================================================================= */
static volatile uint32_t g_rr_a, g_rr_b;
static volatile int       g_rr_stop;
static void rr_spin(void *arg) {
    volatile uint32_t *c = (volatile uint32_t *)arg;
    uint32_t i = 0;
    while (!g_rr_stop && i < 3000000UL) { (*c)++; i++; }
}
int rtos_rr_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[RR] self-test begin (RTOS_TIME_SLICE=%d)\n",
               (int)RTOS_TIME_SLICE);
#if RTOS_TIME_SLICE
    g_rr_a = g_rr_b = 0; g_rr_stop = 0;
    RTOS_TASK_STACK(sta, 768);
    RTOS_TASK_STACK(stb, 768);
    rtos_task_create("rr_a", rr_spin, (void *)&g_rr_a, 18, sta, sizeof(sta));
    rtos_task_create("rr_b", rr_spin, (void *)&g_rr_b, 18, stb, sizeof(stb));
    rtos_msleep(100);                 /* 让两个同优先级任务靠时间片交替运行 */
    int lok = (g_rr_a > 0 && g_rr_b > 0);
    if (!lok) ok = 0;
    g_rr_stop = 1;                   /* 让自旋任务退出（变为 DEAD，池槽回收复用） */
    log_printf(app_log(), LOG_INFO, "rtos", "[RR] round-robin: a=%lu b=%lu %s\n",
               (unsigned long)g_rr_a, (unsigned long)g_rr_b, lok ? "PASS" : "FAIL");
#else
    log_printf(app_log(), LOG_INFO, "rtos", "[RR] skipped (RTOS_TIME_SLICE=0)\n");
#endif
    return ok;
}
RTOS_SELFTEST_ADD("rr", rtos_rr_selftest);

/* ===========================================================================
 * 事件总线（RTOS 一等 IPC 原语）自测：从控制台 "RTOSBUS" 命令触发 + RTOSALL。
 * 覆盖：(1) 非阻塞等待（无消息 / 有消息）；(2) 广播：一个发布多订阅者皆唤醒并
 *       取到数据；(3) 内核对象注册（KOBJ_BUS 校验指针）。
 * ======================================================================== */
#define BUS_TOPICS 4
#define BUS_ITEM   16
static rtos_bus_t  g_bus_selftest;
static uint8_t     g_bus_buf[RTOS_BUS_BUF_SIZE(BUS_TOPICS, BUS_ITEM)];
static rtos_sem_t  g_bus_done;
static volatile int g_bus_recv_cnt;

static void bus_sub_topic3(void *arg) {
    (void)arg;
    uint8_t buf[BUS_ITEM]; size_t len = 0;
    if (rtos_bus_wait(&g_bus_selftest, 3, buf, &len, 1) == 0
        && len == 9 && memcmp(buf, "hello-bus", 9) == 0)
        g_bus_recv_cnt++;
    rtos_sem_give(&g_bus_done);
}

int rtos_bus_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[BUS] self-test begin\n");
    rtos_bus_init(&g_bus_selftest, BUS_TOPICS, BUS_ITEM, g_bus_buf, sizeof(g_bus_buf));
    if (!rtos_kobj_validate(&g_bus_selftest, KOBJ_BUS)) {
        ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos", "[BUS] kobj register: FAIL\n");
    } else {
        log_printf(app_log(), LOG_INFO, "rtos", "[BUS] kobj register: PASS\n");
    }

    /* (1) 非阻塞等待：无消息应返回 -1 */
    uint8_t b0[BUS_ITEM]; size_t l0 = 0;
    if (rtos_bus_wait(&g_bus_selftest, 1, b0, &l0, 0) != -1) ok = 0;

    /* (1b) 发布后非阻塞等待应取到 */
    rtos_bus_publish(&g_bus_selftest, 1, "ping", 4);
    if (rtos_bus_wait(&g_bus_selftest, 1, b0, &l0, 0) != 0 || l0 != 4 || memcmp(b0, "ping", 4))
        ok = 0;

    /* (2) 广播：两个订阅者阻塞等 topic3，一个发布皆唤醒并取到 */
    g_bus_recv_cnt = 0;
    rtos_sem_init(&g_bus_done, 0, 8);
    RTOS_TASK_STACK(st_s1, 1024);
    RTOS_TASK_STACK(st_s2, 1024);
    rtos_task_create("bus_s1", bus_sub_topic3, (void *)0, 14, st_s1, sizeof(st_s1));
    rtos_task_create("bus_s2", bus_sub_topic3, (void *)0, 15, st_s2, sizeof(st_s2));
    rtos_msleep(20);                 /* 让两个订阅者先阻塞在 topic3 */
    rtos_bus_publish(&g_bus_selftest, 3, "hello-bus", 9);  /* 广播发布 */
    rtos_sem_wait(&g_bus_done);      /* 等第一个订阅者 */
    rtos_sem_wait(&g_bus_done);      /* 等第二个订阅者 */
    {
        int lok = (g_bus_recv_cnt == 2);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos", "[BUS] broadcast: recv=%d (expect 2) %s\n",
                   g_bus_recv_cnt, lok ? "PASS" : "FAIL");
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[BUS] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("bus", rtos_bus_selftest);

/* ===========================================================================
 * FPU 上下文保存自测（从 RTOSFPU 命令调用）。
 * 证明 PendSV 上下文切换会保存/恢复 s16-s31：
 *   - 每个任务把三个浮点累加器钉在 s16/s17/s18（属于 callee-saved 的 s16-s31，
 *     跨上下文共享，必须随任务上下文保存）；
 *   - 任务间用 rtos_msleep(1) 高频互切，互相踩踏对方的 s16-s18；
 *   - 若 PendSV 不保存 s16-s31，被切出任务的累加器会被切进任务改写，最终与
 *     “相同公式独立重算”的期望值偏差巨大；
 *   - 保存正确则逐位相等（浮点运算顺序与切换无关），判定 PASS。
 * 注意：S0-S15/FPSCR 由硬件懒栈自动压取，本自测只针对软件必须手存的 s16-s31。
 * ======================================================================== */
#define FPU_TASKS 3
#define FPU_ITERS 300

static volatile uint32_t g_fpu_done[FPU_TASKS];
static volatile uint32_t g_fpu_ok[FPU_TASKS];

static void fpu_task(void *arg) {
    int id = (int)(intptr_t)arg;
    /* 钉在 s16-s18：强制累加器跨 rtos_msleep()/上下文切换一直驻留在这些寄存器里 */
    register float a asm("s16") = (float)(id + 1) * 1.25f;
    register float b asm("s17") = (float)(id + 1) * 2.50f;
    register float c asm("s18") = (float)(id + 1) * 0.30f;
    for (int i = 0; i < FPU_ITERS; i++) {
        a = a * 1.0001f + (float)i * 0.5f;
        b = b / 1.00005f - c;
        c = c + a * 0.0001f;
        rtos_msleep(1);                 /* 让出 1 个节拍，强制与其它 FPU 任务互切 */
    }
    /* 独立重算期望值（同公式、无切换；结果应与 a/b/c 逐位一致——只要 s16-s18 被保存） */
    float ea = (float)(id + 1) * 1.25f;
    float eb = (float)(id + 1) * 2.50f;
    float ec = (float)(id + 1) * 0.30f;
    for (int i = 0; i < FPU_ITERS; i++) {
        ea = ea * 1.0001f + (float)i * 0.5f;
        eb = eb / 1.00005f - ec;
        ec = ec + ea * 0.0001f;
    }
    float da = (a > ea) ? (a - ea) : (ea - a);
    float db = (b > eb) ? (b - eb) : (eb - b);
    float dc = (c > ec) ? (c - ec) : (ec - c);
    g_fpu_ok[id]   = (da < 1e-3f && db < 1e-3f && dc < 1e-3f) ? 1U : 0U;
    g_fpu_done[id] = 1U;
}

int rtos_fpu_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[FPU] self-test begin\n");
    for (int i = 0; i < FPU_TASKS; i++) { g_fpu_done[i] = 0; g_fpu_ok[i] = 0; }

    static uint8_t st_f[FPU_TASKS][1024] __attribute__((aligned(8)));
    for (int i = 0; i < FPU_TASKS; i++)
        rtos_task_create("fpu", fpu_task, (void *)(intptr_t)i,
                         (uint8_t)(22 + i), st_f[i], sizeof(st_f[i]));

    /* 等 FPU 任务完成（或超时 5s） */
    uint32_t waited = 0;
    while (waited < 5000) {
        int all = 1;
        for (int i = 0; i < FPU_TASKS; i++) if (!g_fpu_done[i]) all = 0;
        if (all) break;
        rtos_msleep(10);
        waited += 10;
    }

    for (int i = 0; i < FPU_TASKS; i++) {
        int lok = g_fpu_done[i] && g_fpu_ok[i];
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[FPU] task%d s16-s18 preserved: %s\n", i, lok ? "PASS" : "FAIL");
    }
    log_printf(app_log(), LOG_INFO, "rtos", "[FPU] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* 编译期注册：RTOSALL 会遍历该段依次执行 */
RTOS_SELFTEST_ADD("fpu", rtos_fpu_selftest);

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
    /* 硬实时违约兜底（阶段1）：若 RTOSALL 运行期间任一硬实时任务突破截止期/WCET，
     * 即便各子模块自身 PASS，整体也必须 FAIL（严格硬实时契约）。当前无硬实时任务，
     * rtos_rt_violation() 恒为 0，零回归。 */
    if (rtos_rt_violation() != 0) {
        ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[SELFTEST] HARD-RT VIOLATION: deadline/wcet missed (violation=%lu)\n",
                   (unsigned long)rtos_rt_violation());
    }
    return ok;
}
