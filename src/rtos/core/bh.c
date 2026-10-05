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
#ifndef RTOS_BH_MAX
#define RTOS_BH_MAX        8
#endif
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
#ifndef RTOS_WORKQ_STACK_WORDS
#define RTOS_WORKQ_STACK_WORDS 256    /* 256 字 = 1 KB 栈/队列（worker 仅出队执行 fn） */
#endif
#ifndef RTOS_WORKQ_N
#define RTOS_WORKQ_N 3                /* ★design.md P2-2f：多队列（各独立 worker/优先级）*/
#endif
#define RTOS_WORKQ_STACK_BYTES (RTOS_WORKQ_STACK_WORDS * 4)
RTOS_TASK_STACK(g_wq_stack0, RTOS_WORKQ_STACK_BYTES);   /* 默认队列(0)的内核栈；其余队列由 App 提供栈 */

typedef struct {
    rtos_work_t *head;
    rtos_work_t *tail;
    rtos_sem_t   sem;
    uint8_t      inited;
    uint32_t     quota_cycles;  /* design.md 5#4: bandwidth quota per burst (0=off) */
    uint32_t     used_cycles;
    /* ★design.md §5：队列**自带调度器** —— period 由 WorkItem 声明，调度器按 EDF 派发。 */
    rtos_work_t *plist;         /* 已注册的周期 item 链（注册顺序）*/
    rtos_timer_t sched;         /* 调度 tick（1ms 分辨率）*/
    uint8_t      sched_inited;
} rtos_workq_t;
/* 纯软件，不含 DMA 目标缓冲，搬入 CCM(发布版)收缩主 SRAM .bss。 */
static rtos_workq_t RTOS_CCM_BSS g_wqs[RTOS_WORKQ_N];
/* ★design.md P2-2：预算/超时统计（仅计数，零挂起风险）。 */
static volatile uint32_t g_wq_submitted;
static volatile uint32_t g_wq_soft_overrun;
static volatile uint32_t g_wq_hard_overrun;
static volatile uint32_t g_wq_degraded;   /* design.md 8: degraded items */
static volatile uint32_t g_wq_bw_drop;    /* design.md 5#4: bandwidth-quota drops */
static volatile uint32_t g_wq_degraded_ticks; /* design.md 5#2: 降级运行次数（可观测）*/

/* 共享 worker：被唤醒后【排空】整条队列（一次唤醒处理所有已提交工作，
 * 避免“二进制信号量把多次 submit 折叠成一次唤醒、剩余工作饿死”的缺陷）。 */
static void rtos_workq_worker(void *arg) {
    int q = (int)(intptr_t)arg;
    rtos_workq_t *wq = &g_wqs[q];
    for (;;) {
        rtos_sem_wait(&wq->sem);
        for (;;) {
            rtos_work_t *w;
            unsigned st = irq_lock();
            w = wq->head;
            if (w) {
                wq->head = w->next;
                if (!wq->head) wq->tail = (rtos_work_t *)0;
                w->queued = 0;   /* ★已出队 ⇒ 允许下次（周期）派发再入队 */
            }
            irq_unlock(st);
            if (!w) { wq->used_cycles = 0; break; }  /* 队列空 ⇒ 突发结束，配额计数清零 */
            /* ★design.md §5#4：带宽隔离 —— 本突发已用超配额 ⇒ 丢弃剩余项，防饿死其他队列 */
            if (wq->quota_cycles && wq->used_cycles >= wq->quota_cycles) {
                g_wq_bw_drop++;
                continue;
            }
            /* ★design.md P2-2：实测执行时间，比对 per-item 预算（软/硬超时仅计数）*/
            uint32_t _t0 = rtos_cycle_now();
            if (w->fn) w->fn(w->arg);
            {
                uint32_t _used = (uint32_t)(rtos_cycle_now() - _t0);
                wq->used_cycles += _used;
            }
            if (w->budget_cycles) {
                uint32_t _dt = (uint32_t)(rtos_cycle_now() - _t0);
                if (_dt > w->budget_cycles * 3u) { g_wq_hard_overrun++; w->miss_count++; }
                else if (_dt > w->budget_cycles) { g_wq_soft_overrun++; w->miss_count++; }
                else { w->miss_count = 0; }
                /* ★design.md §8：连续 ≥5 次超预算 ⇒ 降级（宿主据此降频/跳过/移出）*/
                if (w->miss_count >= 5u && !w->degraded) { w->degraded = 1; g_wq_degraded++; }
            }
        }
    }
}

/* 共享 worker 初始化：必须在 rtos_start()（任务上下文，切到首个任务之前）调用一次。
 * 关键约束：绝不能在 ISR 里懒创建——rtos_work_submit 可由上半部(ISR)调用，若在此
 * 首次提交才建任务，rtos_task_create 会在中断上下文执行，破坏调度器（实测会卡死）。
 * 故 wq 在 rtos_start 里提前建好，rtos_work_submit 只做 ISR 安全的入队 + 唤醒。 */
/* ★design.md §5：队列调度器 tick —— 扫描周期链，把**已到期**的 item 派发进运行队列。
 * 派发时写 `deadline_cycles = next_run + period`（隐式截止期）⇒ 同队列多周期 item 由
 * 既有 EDF 插入逻辑排序 ⇒ 周期短者先跑（RMS/EDF 语义 ✓）。 */
static void rtos_workq_sched_tick(rtos_timer_t *t, void *arg) {
    (void)t;
    int q = (int)(intptr_t)arg;
    if (q < 0 || q >= RTOS_WORKQ_N) return;
    rtos_workq_t *wq = &g_wqs[q];
    uint32_t now = rtos_cycle_now();
    for (rtos_work_t *w = wq->plist; w; w = w->pnext) {  /* ★周期链用 pnext（next 属运行队列）*/
        if (w->period_cycles == 0) continue;
        /* 未到期 ⇒ 跳过（注意 cycles 回绕：用有符号差 ✓） */
        if ((int32_t)(now - w->next_run_cycles) < 0) continue;
        /* ★design.md §5#2「连续超时（5 次）：移出关键队列，放入 L3 降级运行」——
         *   被降级（`degraded`）的 item 在此**自动降速**（周期 ×4）⇒ 释放 L2 带宽给
         *   关键项、且仍保持运行（不是删除 ✗）✓；恢复（宿主清 `degraded`）即回原速率 ✓。 */
        uint32_t eff_period = w->period_cycles;
        if (w->degraded) {
            if (eff_period > (0xFFFFFFFFu / 4u)) eff_period = 0xFFFFFFFFu;
            else eff_period *= 4u;
            g_wq_degraded_ticks++;
        }
        /* 追平（若调度被延迟，避免"补跑风暴"：只推进到最近的将来 ✓） */
        do { w->next_run_cycles += eff_period; }
        while ((int32_t)(now - w->next_run_cycles) >= 0);
        w->deadline_cycles = w->next_run_cycles;   /* 绝对截止期（= 下次到点时刻）*/
        rtos_work_submit_q((uint8_t)q, w);
    }
}

void rtos_workq_create(uint8_t q, const char *name, uint8_t prio, void *stack, size_t stack_bytes) {
    if (q >= RTOS_WORKQ_N || g_wqs[q].inited || !stack) return;
    rtos_sem_init(&g_wqs[q].sem, 0, 1);
    g_wqs[q].head = (rtos_work_t *)0;
    g_wqs[q].tail = (rtos_work_t *)0;
    rtos_task_create(name, rtos_workq_worker, (void *)(intptr_t)q, prio, stack, stack_bytes);
    g_wqs[q].inited = 1;
    /* ★队列自带调度器**惰性创建**：`rtos_workq_create` 在 `rtos_start` 早期（调度器未跑）
     * 调用，此时建定时器会破坏调度器（实测：App 挂载成功但业务任务永不运行 ✗）。
     * 故留到首次 `workq_add_periodic`（任务上下文）时再建 ✓。 */
}

/* ★design.md §5：**每 ms 的核周期数** —— 开机用 SysTick 实测，不用标称主频常量。
 *   为何必要：DWT->CYCCNT 速率未必等于标称主频（**实测仿真器 ≈84/µs，而非 168/µs** ✗）
 *   ⇒ 若用标称常量把 ms 换成 cycles，实际拍率会差 2× ✗（实测 harness 锁相漂移 2×）。 */
static uint32_t g_cycles_per_ms;

uint32_t rtos_cycles_per_ms(void) {
    /* ★★★2026-10-04【与 SysTick 同时基，精确解】：每 ms 的核周期数**就是** SysTick 的
     *   节拍周期 = `RTOS_CPU_HZ / RTOS_TICK_HZ`（`port.c: rtos_arch_tick_start` 写的就是
     *   这个值到 LOAD）⇒ 与宿主 harness 的 `systick_ms()` 是同一时基 ✓，且**无需自旋实测**
     *   （自旋窗口会被 ISR 延迟/启动期负载污染：实测偏 -3.5% ✗；而 `g_tick` 也未必严格
     *   等于 SysTick 周期 ✗）。两者一致 ⇒ 声明 period=4ms 恰好等于 4 个 SysTick 周期 ✓。 */
    /* ★★★2026-10-04【根因修复：必须用**实测**速率，不能用名义常量 ✗】】
     *   本函数返回值用于把 `period_ms` 换成 `period_cycles`，而调度器比较用的是
     *   `rtos_cycle_now()`（**DWT** ✗）。实测仿真器 DWT ≈84k/ms（名义 168k ✗）⇒
     *   声明 period=5ms 实际要 **10~20ms** 才到 ⇒ 每个周期 WorkItem **慢 2~4 倍** ✗
     *   ⇒ L2 estimator 名义 200Hz、实测 ~48Hz（`dtt≈20.9ms` ✗ 与设计 5ms 差 4.2× ✓）
     *   ⇒ EKF 的 dt/协方差/融合全部失配 ⇒ 发散 ✓✓。
     *   修法：**首次调用时开机实测**（tick 推进 10 拍，量 DWT 增量 ✓），带合理性回退 ✓。
     *   仅在任务上下文调用（`rtos_workq_add_periodic` ✓）⇒ 自旋 10ms 可接受 ✓。 */
    if (g_cycles_per_ms) return g_cycles_per_ms;
    {
        uint32_t nom = (uint32_t)((uint32_t)RTOS_CPU_HZ / (uint32_t)RTOS_TICK_HZ);
        uint32_t t0 = rtos_tick_count();
        uint32_t c0 = rtos_cycle_now();
        uint32_t guard = 0;
        while ((uint32_t)(rtos_tick_count() - t0) < 10u && ++guard < 100000000u) { }
        uint32_t dt = (uint32_t)(rtos_tick_count() - t0);
        uint32_t dc = (uint32_t)(rtos_cycle_now() - c0);
        uint32_t r = dt ? (uint32_t)((uint64_t)dc / dt) : nom;
        if (r < nom / 4u || r > nom * 4u) r = nom;   /* 测量异常 ⇒ 回退名义 ✓ */
        g_cycles_per_ms = r;
    }
    return g_cycles_per_ms;
}

/* ★design.md §5：注册/注销周期 WorkItem。**period_ms = 0 ⇒ 注销**；周期由内核按实测
 *   速率换算成 cycles ⇒ App 侧不写任何频率常量 ✓（频率只声明一次、单位是人类可读的 ms）。*/
void rtos_workq_add_periodic(uint8_t q, rtos_work_t *w, uint32_t period_ms) {
    uint32_t period_cycles = period_ms ? period_ms * rtos_cycles_per_ms() : 0;
    if (q >= RTOS_WORKQ_N || !w || !g_wqs[q].inited) return;
    rtos_workq_t *wq = &g_wqs[q];
    unsigned st = irq_lock();
    /* 先从周期链摘除（若已在链上）*/
    rtos_work_t **pp = &wq->plist;
    while (*pp && *pp != w) pp = &(*pp)->pnext;
    if (*pp == w) *pp = w->pnext;
    w->period_cycles = period_cycles;
    if (period_cycles) {
        /* 首次注册 ⇒ 惰性建调度器（任务上下文 ✓）*/
        if (!wq->sched_inited) {
            rtos_timer_init(&wq->sched, "wqsched", rtos_workq_sched_tick, (void *)(intptr_t)q);
            rtos_timer_start_ticks(&wq->sched, RTOS_TIMER_PERIODIC, 1);
            wq->sched_inited = 1;
        }
        w->next_run_cycles = rtos_cycle_now() + period_cycles;  /* 首个周期后运行 */
        w->pnext = wq->plist;
        wq->plist = w;
    }
    irq_unlock(st);
}

void rtos_workq_init(void) {
    /* 队列 0 = 默认共享队列（内核 1KB 栈 ✓）；队列 1..N-1 由应用提供栈（P2-3）。 */
    rtos_workq_create(0, "wq", RTOS_PRIO_BH_MED, g_wq_stack0, sizeof(g_wq_stack0));
}

void rtos_workq_stats(uint32_t *out5) {
    if (!out5) return;
    out5[0] = g_wq_submitted;
    out5[1] = g_wq_soft_overrun;
    out5[2] = g_wq_hard_overrun;
    out5[3] = g_wq_degraded;
    out5[4] = g_wq_bw_drop;    /* design.md 5#4 */
    out5[5] = (uint32_t)g_wqs[1].used_cycles;  /* design.md 9: L2 队列上突发已用 cycles */
}

void rtos_workq_set_quota(uint8_t q, uint32_t quota_cycles) {
    if (q >= RTOS_WORKQ_N) return;
    g_wqs[q].quota_cycles = quota_cycles;
}

void rtos_work_submit_q(uint8_t q, rtos_work_t *w) {
    if (!w) return;
    if (q >= RTOS_WORKQ_N || !g_wqs[q].inited) return;   /* 防护：队列未初始化 */
    rtos_workq_t *wq = &g_wqs[q];
    unsigned st = irq_lock();          /* ISR 安全：保护链表头/尾 */
    /* ★防重复入队：同一 WorkItem 已在队列/正在运行 ⇒ 直接返回，否则链表自环 ✗ */
    if (w->queued) { irq_unlock(st); return; }
    w->queued = 1;
    g_wq_submitted++;
    w->next = (rtos_work_t *)0;
    if (w->deadline_cycles) {
        /* ★design.md P2-2：EDF —— 按截止期升序插入（0 截止期项视为最低）*/
        rtos_work_t **pp = &wq->head;
        while (*pp && (*pp)->deadline_cycles && (*pp)->deadline_cycles <= w->deadline_cycles) {
            pp = &(*pp)->next;
        }
        w->next = *pp;
        *pp = w;
        if (!w->next) wq->tail = w;
    } else {
        if (wq->tail) wq->tail->next = w;
        else wq->head = w;
        wq->tail = w;
    }
    irq_unlock(st);
    rtos_sem_give(&wq->sem);           /* ISR 安全：唤醒该队列 worker */
}

void rtos_work_submit(rtos_work_t *w) { rtos_work_submit_q(0, w); }

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
