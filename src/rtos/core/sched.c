#include "rtos.h"
#include "rtos_internal.h"
#include "common/lock.h"
#include "common/ccm_bss.h"
#include "irq.h"
#include "sched_trace.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 调度核心（core/sched.c）：TCB 全局、就绪位图、睡眠链表、等待队列、
 * 调度切换、节拍、pend/post、有效优先级调整。
 *
 * 所有链表修改都假定调用方已处于临界区（rtos_crit_enter 的 PRIMASK 或 BASEPRI
 * 屏蔽），因此本文件内不另加锁；尤其 rtos_pendsv_switch 内部已用 rtos_crit_enter
 * 包住整段切换，故 PendSV 汇编不再需要 cpsid i。对外 API（rtos_yield/rtos_msleep/
 * rtos_pend/post）
 * 在调用修改链表的逻辑前自行 irq_lock。
 *
 * 任务生命周期 / TCB 池 / rtos_start 见 core/task.c；
 * SVC 系统调用分发见 core/syscalls.c。
 * ------------------------------------------------------------------------- */

/* ---- 全局状态 ---- */
/* 以下为纯软件、纯 CPU 访问的调度核心状态（无 DMA 目标缓冲），搬入 CCM 以收缩
 * 主 SRAM .bss，让出的空间下推给 APP_RAM。它们经 extern 被 arch/ 汇编与 port.c
 * 引用（符号链接可见），不依赖所在段位置。 */
task_t *RTOS_CCM_BSS g_running = (task_t *)0;
volatile uint32_t RTOS_CCM_BSS g_tick = 0;
int RTOS_CCM_BSS g_rtos_started = 0;
/* PSP 是否已切到首个任务的栈：在 SVC_Handler 首次启动路径里置 1。
 * 仅当其为 1 时，rtos_schedule_request 才允许置 PENDSVSET——否则（首切之前、
 * 启动线程用 MSP、PSP 仍为 0）置位会用 PSP=0 保存帧、破坏内存并 HardFault。
 * 注意：不能用“ISR 中 CONTROL.SPSEL 恒为 0”来区分，否则会误杀所有 ISR 驱动的
 * 重调度（SysTick 唤醒、ISR 内 sem_give 等），导致内核卡死（见 port.c 注释）。 */
volatile int RTOS_CCM_BSS g_rtos_psp_ready = 0;
volatile int RTOS_CCM_BSS g_in_svc = 0;   /* SVC 分发进行中：防止非特权任务路径递归触发 SVC */

/* ---- 就绪队列：每优先级 FIFO + 位图 ---- */
static task_t *RTOS_CCM_BSS g_ready_head[PRIO_LEVELS];
static task_t *RTOS_CCM_BSS g_ready_tail[PRIO_LEVELS];
static uint32_t RTOS_CCM_BSS g_ready_bmp;

/* ---- 睡眠链表（按 tick 递减） ---- */
static task_t *RTOS_CCM_BSS g_sleep_head;

/* ---- 任务计数（TCB 池定义在 core/task.c） ---- */
int RTOS_CCM_BSS g_task_count = 0;

/* ---- 调度器链表完整性断言记录器（见 rtos_internal.h） ---- */
volatile uint32_t RTOS_CCM_BSS g_sched_invariant_fail = 0;
volatile task_t  *RTOS_CCM_BSS g_sched_bad_tcb        = (task_t *)0;
volatile uint32_t RTOS_CCM_BSS g_sched_bad_line       = 0;
void rtos_sched_assert_fail(const char *file, int line) {
    (void)file;
    g_sched_bad_tcb  = g_running;   /* 当前运行任务即最可能双挂的一方 */
    g_sched_bad_line = (uint32_t)line;
    g_sched_invariant_fail++;
#ifndef RTOS_SCHED_ASSERT_OFF
    /* 诊断：直接把断言行号打到串口，便于无 GDB 环境定位断言来源。
     * 用 polling HAL 发送（不依赖 TX 中断，避免临界区内 uart_tx_blocking 死锁）。
     * 这是断言失败（调度器不变量已破坏）时的最后手段日志，核心层仅以 extern
     * 形式引用驱动层 g_debug_uart_hal，不引入头文件依赖。 */
    extern void uart_hal_putc(void *h, char c);
    extern void *g_debug_uart_hal;
    void *hal = g_debug_uart_hal;
    if (!hal) return;
    const char *pfx = "[SCHED_ASSERT] line=";
    while (*pfx) uart_hal_putc(hal, *pfx++);
    uint32_t v = (uint32_t)line;
    char dig[12]; int di = 0;
    if (v == 0) dig[di++] = '0';
    while (v) { dig[di++] = (char)('0' + (v % 10)); v /= 10; }
    while (di > 0) uart_hal_putc(hal, dig[--di]);
    uart_hal_putc(hal, '\r');
    uart_hal_putc(hal, '\n');
#endif
}

/* ---- 硬实时违约标志（见 docs/rtos-hard-realtime-plan.md 阶段1，零挂起风险） ----
 * 任一硬实时任务截止期/wcet 被突破，对应粘性计数递增，并把“曾发生过违约”汇总到
 * g_rtos_deadline_violation / g_rtos_wcet_violation，供诊断/看门狗读取，不触发异常。 */
volatile uint32_t RTOS_CCM_BSS g_rtos_deadline_violation = 0;
volatile uint32_t RTOS_CCM_BSS g_rtos_wcet_violation     = 0;

/* ---- 临界区持锁超长计数（阶段2，零挂起风险） ----
 * 任何内核临界区（rtos_crit_enter/exit、sched_lock 区间）持续超过 RTOS_CRIT_MAX_TICKS
 * 即递增（粘性）。把“低优长临界区阻塞高优”从不可见变为可测量。 */
volatile uint32_t RTOS_CCM_BSS g_rtos_crit_overflow = 0;

/* P0-3 临界区硬上限执行：升级触发计数（粘性，零挂起风险）。 */
volatile uint32_t RTOS_CCM_BSS g_rtos_crit_kill_count = 0;
/* P0-3 安全态 panic 标志（RTOS_CRIT_KILL=PANIC 时置位，供调试器/复位原因读取）。 */
volatile uint32_t RTOS_CCM_BSS g_rtos_crit_kill_panic = 0;

/* 临界区持锁时长直方图（中断延迟优化定位器，见 docs/rtos-hard-realtime-roadmap.md
 * §中断延迟优化）：每次「最外层」临界区（rtos_crit_enter/exit / sched_lock 区间）退出时，
 * 把其持锁 cycle 数落入细桶直方图，用于定位「是谁」制造了长临界区 —— 这正是之前 IRQ→任务
 * 唤醒延迟最坏 32.8µs 的元凶来源。直方图本身零挂起风险：纯计数、无锁（单核、临界区退出时
 * 仍持调度锁，不重入）。门控 RTOS_SCHED_TRACE：常态构建（=0）零开销、零 RAM；开发/诊断
 * 构建与 RTOSBENCH 同源，可直接 dump。
 * 桶宏 RTOS_CRIT_HIST_BUCKETS/STEP/OVER 与 extern 声明见 rtos_internal.h（同门控）。 */
#if RTOS_SCHED_TRACE
volatile uint32_t RTOS_CCM_BSS g_crit_hist[RTOS_CRIT_HIST_BUCKETS + 1u];  /* +1 溢出桶(>16us) */
volatile uint32_t g_crit_hist_total;        /* 采样总数 */
volatile uint32_t g_crit_hist_max;          /* 历史最坏持锁 cycle 数 */
/* PendSV 切换分段计时（见 rtos_internal.h 注释）：仅存最后一次切换的分段画像。 */
volatile uint32_t g_pendsv_seg[RTOS_PENDSV_SEG_N];
volatile uint32_t g_pendsv_seg_valid;
volatile uint32_t g_pendsv_t0;       /* 入口 CYCCNT 基准（存全局，避免 r12 被 C 调用破坏） */
#endif

/* 临界区审计内部状态：嵌套深度 + 最外层进入 cycle。仅由 rtos_crit_enter_mark /
 * rtos_crit_exit_audit（rtos_internal.h 内联调用）访问，都在关中断/调度锁内，单核安全。
 * 用 cycle(DWT CYCCNT) 而非 tick：锁调度(BASEPRI)屏蔽了 SysTick，tick 在持锁期间不前进，
 * 而 CYCCNT 不受 BASEPRI 影响、零延迟 ISR 仍计数，故能真实反映持锁时长。
 * 注意：DWT 是调试部件，非特权态不可读（读之触发 BusFault）。非特权任务的临界区均经
 * SVC 在 Handler 模式（特权）执行，故审计在特权态总可读；但若路径异常在非特权态进入
 * 临界区（不应发生），直接跳过审计，避免 fault。arch_in_priv 见 common/lock.h。 */
static int      g_crit_nest   = 0;
static uint32_t g_crit_enter_cycle = 0;

#if RTOS_CRIT_KILL == RTOS_CRIT_KILL_TASK
static void rtos_crit_exit_audit_kill(task_t *t);
#elif RTOS_CRIT_KILL == RTOS_CRIT_KILL_PANIC
static void rtos_crit_kill_panic_spin(void);
#endif

void rtos_crit_enter_mark(void) {
    if (!arch_in_priv()) return;                  /* 非特权：跳过（规避 DWT fault） */
    if (g_crit_nest == 0) g_crit_enter_cycle = rtos_cycle_now();  /* 仅最外层记起点 */
    g_crit_nest++;
}
void rtos_crit_exit_audit(void) {
    if (!arch_in_priv()) return;                  /* 非特权：跳过（与 mark 对称） */
    if (g_crit_nest == 0) return;                       /* 防御：不匹配调用 */
    g_crit_nest--;
    if (g_crit_nest == 0) {                             /* 回到最外层：审计总持有时长 */
        uint32_t held = (uint32_t)(rtos_cycle_now() - g_crit_enter_cycle);
#if RTOS_SCHED_TRACE
        /* 持锁时长直方图采样：定位「长临界区」真凶（唤醒延迟长尾来源）。
         * 此处仍在临界区（调度锁未放开），单核不重入，纯计数零挂起风险。 */
        do {
            uint32_t b = held / RTOS_CRIT_HIST_STEP;
            if (b >= RTOS_CRIT_HIST_BUCKETS) b = RTOS_CRIT_HIST_OVER;
            g_crit_hist[b]++;
            g_crit_hist_total++;
            if (held > g_crit_hist_max) g_crit_hist_max = held;
        } while (0);
#endif
#if RTOS_CRIT_MAX_CYCLES > 0
        if (held > (uint32_t)RTOS_CRIT_MAX_CYCLES) {
            g_rtos_crit_overflow++;                     /* 粘性计数（零挂起风险） */
#if RTOS_CRIT_KILL != RTOS_CRIT_KILL_REPORT
            /* P0-3 临界区硬上限执行：超长持锁退出最外层时升级为可配置故障处理。
             * 此刻仍在临界区（BASEPRI/PRIMASK 尚未恢复），但 g_running 有效、
             * 调度锁即将放开，故 kill/wdt/panic 均在特权态安全执行。 */
            if (g_running) {
                g_rtos_crit_kill_count++;               /* 升级触发次数（粘性） */
                switch (RTOS_CRIT_KILL) {
                case RTOS_CRIT_KILL_TASK:               /* 杀持锁任务 */
                    rtos_crit_exit_audit_kill(g_running);
                    break;
                case RTOS_CRIT_KILL_WDT:                /* 武装 IWDG，确定性复位 */
                    if (!rtos_watchdog_is_armed())
                        rtos_watchdog_enable(2000);
                    break;
                case RTOS_CRIT_KILL_PANIC:              /* 进入安全态（自旋） */
                    g_rtos_crit_kill_panic = 1;
                    rtos_crit_kill_panic_spin();
                    break;
                default:                                /* 未知值：退化为仅报告 */
                    break;
                }
            }
#endif
        }
#endif
    }
}

#if RTOS_CRIT_KILL == RTOS_CRIT_KILL_TASK
/* 杀持超长临界区的任务：在临界区内部调用，安全删除 RUNNING 任务自身。
 * rtos_task_delete 对 g_running 走自删除路径（state=DEAD + pend 切换），
 * 临界区在返回后立即被 rtos_crit_exit 原样恢复，PendSV 真正切换走死亡任务。 */
static void rtos_crit_exit_audit_kill(task_t *t)
{
    if (t == g_running)
        rtos_task_delete((task_t *)0);   /* 自删除 */
    else
        rtos_task_delete(t);
}
#elif RTOS_CRIT_KILL == RTOS_CRIT_KILL_PANIC
/* 安全态自旋：置标志后关闭中断并自旋，等待看门狗或调试器介入。
 * 不返回——临界区不会被恢复（已永久关中断），故调用方不应在 panic 后继续。 */
static void rtos_crit_kill_panic_spin(void)
{
    for (;;) { __asm__ volatile("cpsid i" ::: "memory"); }  /* 永久关中断自旋 */
}
#endif

/* ---- 非抢占临界区原语（阶段2，见 docs/rtos-hard-realtime-plan.md §3.1） ----
 * rtos_lock_scheduler / rtos_unlock_scheduler：只屏蔽 PendSV（锁调度），不关中断、
 * 不提 BASEPRI，故零延迟 ISR 仍可达；但时间片剥夺被暂停（tick 跳过 npls_hold 任务），
 * 用于“不可被时间片打断的原子外设序列”。与现有 sched_lock(prio) 区别：本 API 以
 * 任务为粒度维护 npls_hold 标记，且锁的是“调度”（任意 BASEPRI 阈值，统一用
 * RTOS_MAX_ZERO_LATENCY_IRQS 阈值，与内核临界区一致），调用方无需关心优先级数。
 * 嵌套安全：引用计数，最外层解锁才真正放开调度。 */
static int g_npls_nest = 0;
void rtos_lock_scheduler(void) {
    unsigned st = rtos_crit_enter();          /* 关调度锁区间 */
    if (g_npls_nest == 0 && g_running)
        g_running->npls_hold = 1;             /* 仅最外层标记当前任务持非抢占锁 */
    g_npls_nest++;
    rtos_crit_exit(st);
}
void rtos_unlock_scheduler(void) {
    unsigned st = rtos_crit_enter();
    if (g_npls_nest == 0) { rtos_crit_exit(st); return; }   /* 防御 */
    g_npls_nest--;
    if (g_npls_nest == 0 && g_running)
        g_running->npls_hold = 0;
    rtos_crit_exit(st);
}

/* 硬实时违约汇总（供 RTOSALL 自检 / 看门狗读取）。 */
uint32_t rtos_rt_violation(void) {
    return g_rtos_deadline_violation | g_rtos_wcet_violation;
}

/* 临界区超长计数查询（看门狗/RTOSALL 聚合用）。 */
uint32_t rtos_rt_crit_overflow(void) {
    return g_rtos_crit_overflow;
}

/* 硬实时看门狗联动（阶段4 §4.3）：若 RTOS_HARD_RT_WDT 开启且任一违约计数非零，
 * 武装独立看门狗使违约升级为确定性复位。零挂起风险：仅在 tick 中检查、不阻塞调度。
 * 返回 1=已触发联动（看门狗 armed），0=无需触发或本宏关闭。 */
uint32_t rtos_hard_rt_wdt_check(void) {
#if RTOS_HARD_RT_WDT
    if (rtos_rt_violation() != 0 || g_rtos_crit_overflow != 0
        || g_rtos_sched_invalid != 0) {
        if (!rtos_watchdog_is_armed()) {
            rtos_watchdog_enable(2000);   /* 2s 超时：违约后若未恢复则复位 */
        }
        return 1;
    }
#endif
    return 0;
}

/* 硬实时辅助：任务被释放/唤醒时记录释放时刻并清零本窗口预算。
 * 调用方持调度锁。deadline/wcet 为 0 的非实时任务不受影响（计数保持 0）。 */
static inline void rtos_rt_on_release(task_t *t) {
    if (t->rt_class != 0) {   /* 1=硬实时 2=软实时 都参与：软实时只统计不致命 */
        t->release_tick = g_tick;
        t->budget_used  = 0;
    }
}

/* ---- IPC 误用计数（见 docs/rtos-design.md §4.2） ---- */
volatile uint32_t g_ipc_misuse = 0;

/* ---- 时间片轮转（Round-Robin，见 §1/§3）：当前运行任务剩余节拍数 ---- */
#if RTOS_TIME_SLICE
static uint32_t g_slice_ticks = RTOS_TIME_SLICE_TICKS;
#endif

/* ---- 就绪链表操作（调用方持锁） ---- */
void ready_add(task_t *t) {
    /* 延迟量化：在「任务被释放/唤醒」这一刻打 CYCCNT 戳。这是所有唤醒路径
     * （sem/mq/event/mutex/rtos_post/睡眠唤醒）的统一汇入点，故在此一处打戳
     * 即可覆盖全部唤醒源，无需改动各 IPC 文件。rtos_cycle_now() 读 DWT CYCCNT，
     * 168MHz、~6ns 精度、不受 BASEPRI 影响，是量化硬实时延迟的正确时钟。
     * 仅在 trace 构建下启用，常态构建零开销（编译器会消除未使用写）。 */
#if RTOS_SCHED_TRACE
    t->wake_cycle = rtos_cycle_now();
#endif
    /* 双挂防御：进入就绪队列前，TCB 的 sched_next/sched_prev 必须是空（不在任一条
     * 链表上）。若非空，说明该 TCB 已挂在就绪/睡眠链表而未摘除，即“双挂”破坏者。 */
    RTOS_SCHED_ASSERT(t->sched_next == (task_t *)0
                      && t->sched_prev == (task_t *)0);
    int p = t->prio;
    t->sched_prev = g_ready_tail[p];
    t->sched_next = (task_t *)0;
    if (g_ready_tail[p]) g_ready_tail[p]->sched_next = t;
    else                 g_ready_head[p] = t;
    g_ready_tail[p] = t;
    g_ready_bmp |= (1u << p);
    /* 硬实时：任务被释放→就绪时刷新释放基准并清零本窗口预算。
     * 调用惯例：所有唤醒路径在调用 ready_add 前已把 state 设为 TASK_READY
     * （见 rtos_msleep / 各 IPC 唤醒），故此处对"非 RUNNING 的 RT 任务"刷新——
     * 覆盖 睡眠/阻塞唤醒 与 新创建 两种进入就绪的场景；RUNNING（不应出现在
     * ready_add 入参）不刷新。
     * 注：yield(RUNNING→READY) 经 ready_add 也会刷新，语义上"重新运行=新预算窗口"，
     * 与 WCET 统计意图一致，无回归。 */
    if (t->rt_class != 0 && t->state != TASK_RUNNING)
        rtos_rt_on_release(t);
}
void ready_remove(task_t *t) {
    int p = t->prio;
    if (t->sched_prev) t->sched_prev->sched_next = t->sched_next;
    else               g_ready_head[p] = t->sched_next;
    if (t->sched_next) t->sched_next->sched_prev = t->sched_prev;
    else               g_ready_tail[p] = t->sched_prev;
    if (!g_ready_head[p]) g_ready_bmp &= ~(1u << p);
    t->sched_next = t->sched_prev = (task_t *)0;
}
task_t *ready_pick(void) {
    if (!g_ready_bmp) return (task_t *)0;
    int p = __builtin_ctz(g_ready_bmp);   /* 最低置位 = 最高优先级 */
    return g_ready_head[p];
}

/* 睡眠链表辅助（定义见下文；rtos_task_unlink / rtos_cancel_timed_wait 需前向引用）。 */
void    sleep_add(task_t *t);
static void sleep_remove(task_t *t);

/* 把任务从它当前所在的队列（就绪/睡眠/等待，含计时阻塞双链）摘除。调用方持锁。
 * 单核下唯一 RUNNING 任务是 g_running；删除/清理路径对 RUNNING 不做队列操作
 * （其 sp 由上下文切换直接管理）。 */
void rtos_task_unlink(task_t *t) {
    if (!t) return;
    if (t->wait_armed) {            /* 计时阻塞：先摘除睡眠链表条目（双链之一） */
        sleep_remove(t);
        t->wait_armed = 0;
        t->timed_out = 0;
    }
    switch (t->state) {
    case TASK_READY:    ready_remove(t); break;
    case TASK_SLEEPING: sleep_remove(t);  break;
    case TASK_BLOCKED:
        if (t->wait_obj) { rtos_waitq_remove(t->wait_obj, t); t->wait_obj = (void *)0; }
        break;
    default: break;   /* RUNNING / DEAD：不在就绪/睡眠/等待链表中 */
    }
}

/* 取消计时阻塞（rtos_mutex_unlock 的 handoff 在超时前唤醒等待者时调用）。
 * 仅当任务确已挂超时睡眠项(wait_armed==1)时才操作睡眠链表，避免误删未计时任务。 */
void rtos_cancel_timed_wait(task_t *t) {
    if (t && t->wait_armed) {
        sleep_remove(t);
        t->wait_armed = 0;
        t->timed_out  = 0;
    }
}

/* ---- 睡眠链表操作（调用方持锁） ---- */
void sleep_add(task_t *t) {
    /* 双挂防御：进入睡眠队列前同样要求 TCB 不在任何链表上（见 ready_add 注释）。 */
    RTOS_SCHED_ASSERT(t->sched_next == (task_t *)0
                      && t->sched_prev == (task_t *)0);
    t->sched_prev = (task_t *)0;
    t->sched_next = g_sleep_head;
    if (g_sleep_head) g_sleep_head->sched_prev = t;
    g_sleep_head = t;
}
static void sleep_remove(task_t *t) {
    if (t->sched_prev) t->sched_prev->sched_next = t->sched_next;
    else               g_sleep_head = t->sched_next;
    if (t->sched_next) t->sched_next->sched_prev = t->sched_prev;
    t->sched_next = t->sched_prev = (task_t *)0;
}

/* ---- 等待队列操作（调用方持锁） ---- */
void rtos_waitq_add(void **head, task_t *t) {
    task_t *h = (task_t *)(*head);
    t->wait_prev = (task_t *)0;
    if (!h) { *head = (void *)t; t->wait_next = (task_t *)0; return; }
    task_t *p = h;
    while (p->wait_next) p = p->wait_next;
    p->wait_next = t;
    t->wait_prev = p;
    t->wait_next = (task_t *)0;
}
void rtos_waitq_remove(void **head, task_t *t) {
    if (t->wait_prev) t->wait_prev->wait_next = t->wait_next;
    else              *head = (void *)t->wait_next;
    if (t->wait_next) t->wait_next->wait_prev = t->wait_prev;
    t->wait_next = t->wait_prev = (task_t *)0;
}
task_t *rtos_waitq_pop_highest(void **head) {
    task_t *best = (task_t *)0, *bestprev = (task_t *)0;
    task_t *prev = (task_t *)0, *cur = (task_t *)(*head);
    while (cur) {
        if (!best || cur->prio < best->prio) { best = cur; bestprev = prev; }
        prev = cur; cur = cur->wait_next;
    }
    if (!best) return (task_t *)0;
    if (bestprev) bestprev->wait_next = best->wait_next;
    else          *head = (void *)best->wait_next;
    best->wait_next = (task_t *)0;
    return best;
}

/* ---- 生命周期：内核初始化（仅复位调度器表 + 注册节拍） ---- */
void rtos_init(void) {
    g_running = (task_t *)0;
    g_tick = 0;
    g_rtos_started = 0;
    g_ready_bmp = 0;
    for (int i = 0; i < PRIO_LEVELS; i++) {
        g_ready_head[i] = g_ready_tail[i] = (task_t *)0;
    }
    g_sleep_head = (task_t *)0;
    g_task_count = 0;
#if RTOS_TIME_SLICE
    g_slice_ticks = RTOS_TIME_SLICE_TICKS;
#endif
    /* 在 systick 共享线上注册 RTOS 节拍（与 systick 驱动 ISR 并存）。
     * 节拍中断 id 由 arch 层给出，核心不直接依赖任何芯片 HAL。 */
    irq_register(rtos_arch_tick_id(), rtos_tick_isr, (void *)0);
    /* 注册回调后【必须启动节拍时钟源】：否则只挂 ISR 而不使能 SysTick，g_tick 永不
     * 递增，所有 rtos_msleep 永久阻塞（main 任务睡死、uart 永不被读取）。节拍源的
     * 使能在 arch 层（rtos_arch_tick_start），与板级是否注册 systick 节点解耦。 */
    rtos_arch_tick_start();
}

void rtos_yield(void) {
    if (!g_rtos_started) return;
    if (rtos_need_svc()) { rtos_syscall(RTOS_SYS_YIELD, 0, 0, 0); return; }
    unsigned st = rtos_crit_enter();
    if (g_running && g_running->state == TASK_RUNNING) {
        g_running->state = TASK_READY;
        ready_add(g_running);
    }
    rtos_crit_exit(st);
    rtos_schedule_request();
}

void rtos_msleep(uint32_t ms) {
    if (!g_rtos_started) { for (volatile uint32_t i = 0; i < ms * 1000UL; i++) { } return; }
    if (rtos_need_svc()) { rtos_syscall(RTOS_SYS_MSLEEP, ms, 0, 0); return; }
    uint32_t ticks = (ms * RTOS_TICK_HZ + 999U) / 1000U;
    if (ticks == 0) ticks = 1;
    unsigned st = rtos_crit_enter();
    /* 防护：若此前在 sched_lock(屏蔽 PendSV) 区间内调用过 rtos_yield，本任务可能已被
     * 加入就绪队列(state==READY)而切换未发生；睡眠前必须先将其从就绪队列摘除，否则会
     * 同时挂在“就绪”与“睡眠”两条链表上，破坏链表（节拍 ISR 遍历时死循环/越界）。
     * 正常运行态下本任务为 RUNNING，不会命中此分支，零回归。 */
    RTOS_SCHED_ASSERT(g_running->state == TASK_RUNNING
                      || g_running->state == TASK_READY);
    if (g_running->state == TASK_READY) ready_remove(g_running);
    g_running->state = TASK_SLEEPING;
    g_running->delay_ticks = ticks;
    sleep_add(g_running);
    rtos_crit_exit(st);
    rtos_schedule_request();
}

/* 绝对延时（docs/rtos-test-plan.md §6.3，准则 §2.5 vTaskDelayUntil）：delay 直到
 * *last + inc_ticks。用无符号 tick 算术计算剩余 tick（翻转安全），再经相对倒计时
 * rtos_msleep 睡眠——相对倒计时本身亦翻转安全。典型用法：循环里把 next 累加 inc_ticks
 * 得稳定节拍，无累积漂移；48 天(0xFFFFFFFF→0)翻转后 remain 计算仍正确。 */
void rtos_delay_until(uint32_t *last, uint32_t inc_ticks) {
    if (!g_rtos_started) return;
    if (!last) return;
    uint32_t now  = g_tick;
    uint32_t next = (uint32_t)(*last + inc_ticks);   /* 翻转安全加法 */
    *last = next;
    uint32_t remain = (uint32_t)(next - now);        /* 翻转安全剩余 tick */
    if (remain == 0) return;
    uint32_t ms = (remain * 1000U + (RTOS_TICK_HZ - 1U)) / RTOS_TICK_HZ;
    if (ms == 0) ms = 1;
    rtos_msleep(ms);
}

/* 抢占点（docs/rtos-design.md §3）：仅当存在更高（或同优先级 FIFO 中更靠前）
 * 的就绪任务时才让出 CPU；否则继续当前任务。常用于“临界区内插入调度点”。 */
void rtos_schedule(void) {
    if (!g_rtos_started) return;
    if (rtos_need_svc()) { rtos_syscall(RTOS_SYS_YIELD, 0, 0, 0); return; }
    unsigned st = rtos_crit_enter();
    if (g_running && g_running->state == TASK_RUNNING) {
        int p = g_running->prio;
        /* 更高优先级有就绪，或同优先级 FIFO 里还有别的任务在前面 */
        if ((g_ready_bmp & ~((1u << (p + 1)) - 1u)) || (g_ready_head[p] != (task_t *)0)) {
            g_running->state = TASK_READY;
            ready_add(g_running);
        }
    }
    rtos_crit_exit(st);
    rtos_schedule_request();
}

uint32_t rtos_ipc_misuse_count(void) { return g_ipc_misuse; }

/* 由 PendSV 汇编调用（中断已关）：保存 old_sp，挑选下一任务，返回其 sp */
void *rtos_pendsv_switch(void *old_sp) {
    /* 临界区：保护就绪/等待链表不被“调用了内核 API 的更高优先级 ISR”并发改写。
     * 用统一入口 rtos_crit_enter/exit（见 rtos_internal.h）：
     *   RTOS_MAX_ZERO_LATENCY_IRQS>0 时 BASEPRI 仅屏蔽优先级>=阈值(内核)的异常，
     *     零延迟 ISR 在切换窗口内仍可达（FreeRTOS/Zephyr 式选择性屏蔽，降低中断抖动）；
     *   否则退化为 PRIMASK 全局关中断，与原 PendSV 内 cpsid i 行为一致、零回归。
     * 因此 context.S 的 PendSV_Handler 不再需要 cpsid i/cpsie i。 */
    unsigned st = rtos_crit_enter();
    task_t *cur = g_running;
    if (cur) {
        cur->sp = old_sp;
        if (rtos_stack_check_sentinel(cur)) {   /* 栈溢出检测（MPU 辅助） */
            g_stack_overflow = 1;
        }
        if (cur->state == TASK_RUNNING) {   /* 被抢占：回到就绪队列 */
            cur->state = TASK_READY;
            ready_add(cur);
        }
    }
    task_t *nxt = ready_pick();
    if (!nxt) nxt = cur;                    /* 无其它就绪：继续当前（idle 保证不会空） */
    if (nxt) {
        ready_remove(nxt);
        nxt->state = TASK_RUNNING;
        g_running = nxt;
        /* 硬实时：新建首次运行的任务 release_tick 仍为 0，以当前 tick 为释放基准，
         * 避免首 tick 用 g_tick-0 误判违约（预算已在唤醒/创建时清零）。 */
        if (nxt->rt_class != 0 && nxt->release_tick == 0)
            nxt->release_tick = g_tick;
    }
    /* P2-1 调度轨迹：记录 (tick, cur, nxt, reason)。仅在 PendSV 临界区内单点写入，
     * 无锁、ISR 安全；关闭 RTOS_SCHED_TRACE 时 rtos_trace_switch 退化为 no-op。 */
    {
        uint8_t reason;
        if (cur == (task_t *)0)            reason = RTOS_TRACE_START;
        else if (nxt == cur)               reason = RTOS_TRACE_TICK;
        else if (cur->state == TASK_RUNNING) reason = RTOS_TRACE_PREEMPT; /* 被抢占回就绪 */
        else                               reason = RTOS_TRACE_BLOCK;  /* 主动睡眠/阻塞让出 */
        rtos_trace_switch(cur, nxt, reason);
    }
    rtos_crit_exit(st);
    return nxt ? nxt->sp : old_sp;
}

/* 由 systick 共享线调用（可能运行在 ISR 上下文） */
void rtos_tick_isr(void *ctx) {
    (void)ctx;
    if (!g_rtos_started) return;
    unsigned st = rtos_crit_enter();
    g_tick++;
    int awoke = 0;
    /* 睡眠链表遍历：加边界计数，防御双挂/野指针导致的越界或死循环（只遍历至多
     * RTOS_MAX_TASKS+1 个节点；超出说明 sched_next 已损坏，记录并跳出而非死机）。 */
    task_t *t = g_sleep_head;
    int iter = 0;
    while (t) {
        if (++iter > RTOS_MAX_TASKS + 1) {   /* 链表长度不可能超过任务池容量 */
            RTOS_SCHED_ASSERT(0);            /* 睡眠链表损坏：越界/成环 */
            break;
        }
        task_t *nx = t->sched_next;
        if (t->delay_ticks > 0) {
            if (--t->delay_ticks == 0) {
                sleep_remove(t);
                if (t->state == TASK_SLEEPING) {
                    t->state = TASK_READY;
                    ready_add(t);
                } else if (t->state == TASK_BLOCKED) {
                    /* 计时阻塞（rtos_mutex_timedlock）到期：从等待队列摘除并标记超时，
                     * 使被唤醒的任务走“超时未拿到锁”分支返回 -1。 */
                    if (t->wait_obj) { rtos_waitq_remove(t->wait_obj, t); t->wait_obj = (void *)0; }
                    t->wait_armed = 0;
                    t->timed_out  = 1;
                    t->state = TASK_READY;
                    ready_add(t);
                }
                awoke = 1;
            }
        }
        t = nx;
    }

    /* ---- 硬实时违约检测（阶段1，零挂起风险） ----
     * 对当前正运行的硬实时任务，本 tick 累加其运行预算；若超 WCET 预算或超截止期，
     * 递增对应粘性计数并汇总到 g_rtos_*_violation（不触发异常、不停机）。
     * 非实时任务(rt_class==0)或 wcet/deadline 为 0 的不参与，零回归。 */
    if (g_running && (g_running->rt_class == 1 || g_running->rt_class == 2)) {
        task_t *rt = g_running;
        rt->budget_used++;
        if (rt->wcet_ticks != 0 && rt->budget_used > rt->wcet_ticks) {
            rt->wcet_miss++;
            g_rtos_wcet_violation++;
        }
        if (rt->deadline_ticks != 0
            && (uint32_t)(g_tick - rt->release_tick) > rt->deadline_ticks) {
            rt->deadline_miss++;
            g_rtos_deadline_violation++;
        }
    }

#if RTOS_TIME_SLICE
    /* 时间片轮转（硬实时关键改进，见 §3 / rtos-design.md）：
     *  - 同优先级有竞争者：按原语义倒计时，用尽则让出到 FIFO 尾部（防同优先级饿死）。
     *  - 【新增】跨优先级抢占：即使没有同优先级竞争者，只要存在【更高优先级】就绪
     *    任务，当前运行任务连续霸占 CPU 超过 RTOS_TIME_SLICE_TICKS 个节拍后也必须
     *    让出。这把内核从“纯协作式”升级为“准硬实时”——一个低优先级、但忘记 yield
     *    的长循环最多只能阻塞高优先级任务 RTOS_TIME_SLICE_TICKS(默认 5ms)，之后被
     *    强制抢占。这是 zephyr/FreeRTOS“可选时间片”的语义，仅修改 tick 判定，零回归。
     *  注：更高优先级任务被唤醒时会立刻经 rtos_schedule_request 抢占（见 ipc_*.c），
     *  此处时间片只兜底“唤醒发生在更早、但运行任务一直没到调度点”的极端情形。 */
    if (g_running && g_running->state == TASK_RUNNING) {
        uint8_t p = g_running->prio;
        int has_peer = (g_ready_head[p] != (task_t *)0);          /* 同优先级竞争者 */
        int has_higher = (g_ready_bmp
                          & ~((1u << (p + 1)) - 1u)) != 0;        /* 更高优先级就绪 */
        /* 阶段2：持有非抢占临界区(npls_hold)的任务，时间片剥夺被暂停——
         * 它正在执行“不可被时间片打断的原子外设序列”，PendSV 即便被请求也不切换，
         * 直到 rtos_unlock_scheduler 清除标记。零延迟 ISR 仍可达（BASEPRI 未提）。 */
        int npls = (g_running->npls_hold != 0);
        if (!npls && (has_peer || has_higher)) {
            if (g_slice_ticks == 0) g_slice_ticks = RTOS_TIME_SLICE_TICKS;
            if (--g_slice_ticks == 0) {
                g_running->state = TASK_READY;
                ready_add(g_running);   /* 加到就绪队列（同优先级 FIFO 尾部 / 更高优先级在 pick 时自然胜出） */
                awoke = 1;
            }
        } else {
            g_slice_ticks = RTOS_TIME_SLICE_TICKS;   /* 无更高/同优先级就绪：续跑并重置片 */
        }
    }
#endif
    /* 软件定时器（docs/rtos-test-plan.md §6.3）：扫描活动定时器，到期者置 pending
     * 并唤醒“定时器任务”；回调在定时器任务上下文执行（任务模式，可耗时）。
     * 过期判定用无符号 tick 比较，48 天(0xFFFFFFFF→0)翻转安全。 */
    rtos_timer_tick();
    /* 阶段4 §4.3：硬实时看门狗联动检查（若开启且存在违约，武装 IWDG）。
     * 放在临界区内、timer tick 之后，确保违约状态读到的瞬间一致性。 */
    rtos_hard_rt_wdt_check();
    rtos_crit_exit(st);
    if (awoke) rtos_schedule_request();
}

/* 阻塞当前任务 / 唤醒最高等待者（调用方须持调度锁/关中断） */
void rtos_pend(void **q) {
    if (!g_running) return;
    /* 同 rtos_msleep 的防护：sched_lock 区间内 yield 后本任务可能残留于就绪队列，
     * 阻塞前摘除，避免同时挂在“就绪”与“等待”链表。 */
    RTOS_SCHED_ASSERT(g_running->state == TASK_RUNNING
                      || g_running->state == TASK_READY);
    if (g_running->state == TASK_READY) ready_remove(g_running);
    g_running->state = TASK_BLOCKED;
    g_running->wait_obj = q;
    rtos_waitq_add(q, g_running);
    rtos_schedule_request();
}
void rtos_post(void **q) {
    task_t *t = rtos_waitq_pop_highest(q);
    if (t) {
        t->wait_obj = (void *)0;
        t->state = TASK_READY;
        ready_add(t);
    }
    rtos_schedule_request();
}

/* 修改任务的有效优先级，并在其位于就绪队列时重排（互斥量提升/恢复用）。
 * 调用方须持调度锁。RUNNING/BLOCKED 任务不在就绪队列中，直接改 prio 即可。 */
void rtos_set_eff_prio(task_t *t, uint8_t new_prio) {
    if (!t || t->prio == new_prio) return;
    if (t->state == TASK_READY) {
        ready_remove(t);
        t->prio = new_prio;
        ready_add(t);
    } else {
        t->prio = new_prio;
    }
}
