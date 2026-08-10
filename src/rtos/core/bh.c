#include "rtos.h"
#include "bh.h"
#include "common/lock.h"
#include "common/ccm_bss.h"
#include "log/log.h"
#include "log/app_log.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 中断上下半部实现（P3 阶段）（core/bh.c）
 *
 * (A) BH 任务：每个驱动拥有专属的高优先级下半部任务。上半部 ISR 调
 *     rtos_bh_trigger()（= ISR 安全的 rtos_sem_give + 请求 PendSV 调度），
 *     任务体的 trampoline 在计数信号量上阻塞，被唤醒即执行用户 fn。
 * (B) 工作队列：一组共享 worker 任务（RTOS_PRIO_BH_MED）从链表出队执行
 *     rtos_work_submit() 挂入的工作，省去每驱动一个独立栈的开销。
 *
 * 所有“触发/提交”路径均 ISR 安全：信号量用 irq_lock 保护，唤醒后请求调度；
 * 下半部/worker 跑在任务模式，可被任意更高优先级 IRQ/任务抢占。
 * ------------------------------------------------------------------------- */

/* ===========================================================================
 * (A) BH 任务
 * ========================================================================= */
#define RTOS_BH_MAX        8
#define RTOS_BH_PEND_LIMIT 64   /* 每个 BH 的待处理触发上限（计数信号量上限） */

struct bh {
    const char   *name;
    rtos_sem_t    sem;          /* 计数信号量：每次 trigger 对应一次唤醒 */
    void (*fn)(void *);
    void         *ctx;
    uint8_t       prio;
    uint8_t      *stack;
    size_t        stack_size;
    int           used;
};

/* 纯软件 BH 任务表，不含 DMA 目标缓冲，搬入 CCM(发布版)收缩主 SRAM .bss。 */
static bh_t RTOS_CCM_BSS g_bh[RTOS_BH_MAX];

/* 下半部任务体：被触发即调用用户 fn；fn 返回后继续等下一次触发。 */
static void rtos_bh_trampoline(void *p) {
    bh_t *bh = (bh_t *)p;
    for (;;) {
        rtos_sem_wait(&bh->sem);     /* 阻塞在计数信号量上（下半部在此让出 CPU） */
        if (bh->fn) bh->fn(bh->ctx);
    }
}

bh_t *rtos_bh_task_create(const char *name, uint8_t prio,
                          void *stack, size_t stack_size,
                          void (*fn)(void *), void *ctx) {
    if (!name || !stack || !fn) return (bh_t *)0;
    /* 同名复用：驱动 init 或重复自测只建一次，避免重复任务/栈浪费 */
    for (int i = 0; i < RTOS_BH_MAX; i++) {
        if (g_bh[i].used && g_bh[i].name && strcmp(g_bh[i].name, name) == 0) {
            g_bh[i].fn  = fn;       /* 刷新回调/上下文；任务继续复用 */
            g_bh[i].ctx = ctx;
            return &g_bh[i];
        }
    }
    for (int i = 0; i < RTOS_BH_MAX; i++) {
        if (!g_bh[i].used) {
            bh_t *bh = &g_bh[i];
            bh->name       = name;
            bh->prio       = prio;
            bh->stack      = (uint8_t *)stack;
            bh->stack_size = stack_size;
            bh->fn         = fn;
            bh->ctx        = ctx;
            bh->used       = 1;
            rtos_sem_init(&bh->sem, 0, RTOS_BH_PEND_LIMIT);
            rtos_task_create(name, rtos_bh_trampoline, bh, prio, stack, stack_size);
            return bh;
        }
    }
    return (bh_t *)0;
}

void rtos_bh_trigger(bh_t *bh) {
    if (!bh) return;
    rtos_sem_give(&bh->sem);          /* ISR 安全 + 唤醒后请求 PendSV 调度 */
}

void rtos_bh_wait(bh_t *bh) {
    if (!bh) return;
    rtos_sem_wait(&bh->sem);
}

/* ===========================================================================
 * (B) 工作队列（共享 worker 任务）
 * ========================================================================= */
#define RTOS_WORKQ_STACK_WORDS 256    /* 256 字 = 1 KB 栈（worker 仅出队执行 fn） */
RTOS_TASK_STACK(g_wq_stack, RTOS_WORKQ_STACK_WORDS * 4);
/* 纯软件，不含 DMA 目标缓冲，搬入 CCM(发布版)收缩主 SRAM .bss。 */
static rtos_work_t *RTOS_CCM_BSS g_wq_head;
static rtos_work_t *RTOS_CCM_BSS g_wq_tail;
static rtos_sem_t   RTOS_CCM_BSS g_wq_sem;
static int          RTOS_CCM_BSS g_wq_inited;

/* 共享 worker：被唤醒后【排空】整条队列（一次唤醒处理所有已提交工作，
 * 避免“二进制信号量把多次 submit 折叠成一次唤醒、剩余工作饿死”的缺陷）。 */
static void rtos_workq_worker(void *arg) {
    (void)arg;
    for (;;) {
        rtos_sem_wait(&g_wq_sem);
        for (;;) {
            rtos_work_t *w;
            unsigned st = irq_lock();
            w = g_wq_head;
            if (w) {
                g_wq_head = w->next;
                if (!g_wq_head) g_wq_tail = (rtos_work_t *)0;
            }
            irq_unlock(st);
            if (!w) break;              /* 队列空，回到等待 */
            if (w->fn) w->fn(w->arg);
        }
    }
}

/* 共享 worker 初始化：必须在 rtos_start()（任务上下文，切到首个任务之前）调用一次。
 * 关键约束：绝不能在 ISR 里懒创建——rtos_work_submit 可由上半部(ISR)调用，若在此
 * 首次提交才建任务，rtos_task_create 会在中断上下文执行，破坏调度器（实测会卡死）。
 * 故 wq 在 rtos_start 里提前建好，rtos_work_submit 只做 ISR 安全的入队 + 唤醒。 */
void rtos_workq_init(void) {
    if (g_wq_inited) return;
    rtos_sem_init(&g_wq_sem, 0, 1);
    rtos_task_create("wq", rtos_workq_worker, (void *)0, RTOS_PRIO_BH_MED,
                     g_wq_stack, sizeof(g_wq_stack));
    g_wq_inited = 1;
}

void rtos_work_submit(rtos_work_t *w) {
    if (!w) return;
    if (!g_wq_inited) return;          /* 防护：wq 未初始化（正常 rtos_start 已建好） */
    unsigned st = irq_lock();          /* ISR 安全：保护链表头/尾 */
    w->next = (rtos_work_t *)0;
    if (g_wq_tail) g_wq_tail->next = w;
    else g_wq_head = w;
    g_wq_tail = w;
    irq_unlock(st);
    rtos_sem_give(&g_wq_sem);          /* ISR 安全：唤醒 worker */
}

#if RTOS_SELFTEST
/* ===========================================================================
 * BH / 工作队列 运行时自测（RTOSBH 命令 + 注册进 RTOSALL）
 * 验证：上半部（模拟 ISR）trigger -> 下半部高优先级任务被唤醒并执行；
 *       工作队列提交 -> worker 执行。
 * ========================================================================= */
#define BH_SELFTEST_TRIG 12

static bh_t            *g_bh_selftest_h;
static volatile uint32_t g_bh_runs;
static volatile uint32_t g_wq_runs[4];
static rtos_work_t       g_wq_works[4];

static void bh_selftest_bottom(void *ctx) {
    (void)ctx;
    g_bh_runs++;
}
static void bh_selftest_work(void *arg) {
    int id = (int)(intptr_t)arg;
    if (id >= 0 && id < 4) g_wq_runs[id]++;
}

int rtos_bh_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[BH] self-test begin\n");

    /* (A) BH 任务：模拟 ISR 连续 trigger，下半部应被唤醒并执行对应次数 */
    g_bh_runs = 0;
    RTOS_TASK_STACK(bh_stack, 1024);
    g_bh_selftest_h = rtos_bh_task_create("bh_test", RTOS_PRIO_BH_HIGH,
                                          bh_stack, sizeof(bh_stack),
                                          bh_selftest_bottom, (void *)0);
    if (!g_bh_selftest_h) {
        ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos", "[BH] task create: FAIL (NULL)\n");
    } else {
        for (int i = 0; i < BH_SELFTEST_TRIG; i++)
            rtos_bh_trigger(g_bh_selftest_h);   /* 模拟 ISR 多次触发 */
        uint32_t waited = 0;
        while (g_bh_runs < BH_SELFTEST_TRIG && waited < 1000) {
            rtos_msleep(1); waited++;          /* 让出，给下半部执行时间 */
        }
        int lok = (g_bh_runs == BH_SELFTEST_TRIG);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[BH] task: bottom runs=%lu (expect %d) %s\n",
                   (unsigned long)g_bh_runs, BH_SELFTEST_TRIG, lok ? "PASS" : "FAIL");
    }

    /* (B) 工作队列：提交 4 个 work，worker 应各执行一次 */
    for (int i = 0; i < 4; i++) {
        g_wq_runs[i] = 0;
        g_wq_works[i].fn  = bh_selftest_work;
        g_wq_works[i].arg = (void *)(intptr_t)i;
    }
    for (int i = 0; i < 4; i++) rtos_work_submit(&g_wq_works[i]);
    uint32_t waited = 0;
    int all = 0;
    while (waited < 1000) {
        all = 1;
        for (int i = 0; i < 4; i++) if (!g_wq_runs[i]) { all = 0; break; }
        if (all) break;
        rtos_msleep(1); waited++;
    }
    if (!all) ok = 0;
    log_printf(app_log(), LOG_INFO, "rtos", "[BH] workqueue: %s\n", all ? "PASS" : "FAIL");

    log_printf(app_log(), LOG_INFO, "rtos", "[BH] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* 编译期注册：RTOSALL 会遍历该段依次执行 */
RTOS_SELFTEST_ADD("bh", rtos_bh_selftest);
#endif /* RTOS_SELFTEST */
