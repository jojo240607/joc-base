#ifndef JOC_RTOS_INTERNAL_H
#define JOC_RTOS_INTERNAL_H

#include "rtos.h"
#include "common/lock.h"

/* ---------------------------------------------------------------------------
 * jOS RTOS 内部共享头（core/ 各 .c 文件共用，不暴露给应用层）
 *
 * 公开 API 见 rtos.h；此处只声明拆分到多个 .c 后需要在 core/ 内部跨文件
 * 共享的调度器全局与少量内部辅助函数。其它内部符号（ready_add / rtos_waitq_*
 * / rtos_pend / rtos_post / rtos_set_eff_prio / rtos_kobj_* 等）已在 rtos.h 声明。
 * ------------------------------------------------------------------------- */

/* ---- 内核临界区（统一入口，见 docs/rtos-design.md §4.5） ----
 * 把“关中断”收敛到一个入口：RTOS_MAX_ZERO_LATENCY_IRQS>0 时用 BASEPRI 仅屏蔽
 * 优先级 >= 阈值的中断（更高优先级的零延迟 ISR 仍可达），否则退化为 PRIMASK
 * 全局关中断（与此前行为完全一致）。两个分支返回类型相同（unsigned），调用方
 * 用 `unsigned st = rtos_crit_enter(); ... rtos_crit_exit(st);` 即可，无需感知分支。 */

/* 临界区审计辅助（sched.c 定义）：mark 记录最外层进入 cycle（嵌套计数保护）；
 * audit 在退出最外层临界区时比较当前 cycle，超 RTOS_CRIT_MAX_CYCLES 则递增
 * g_rtos_crit_overflow（粘性，零挂起风险）。声明前置，供下方 inline 调用。 */
void rtos_crit_enter_mark(void);
void rtos_crit_exit_audit(void);

#if RTOS_MAX_ZERO_LATENCY_IRQS > 0
/* BASEPRI 临界区原语——设计对标 FreeRTOS 的 portSET/CLEAR_INTERRUPT_MASK_FROM_ISR：
 *   enter：先 `mrs` 读出【当前】BASEPRI 存进局部变量 saved（保存掩码），
 *          再把 BASEPRI 提升到阈值（屏蔽优先级 >= 阈值的异常）；
 *   exit ：直接 `msr BASEPRI, saved` 把【当初保存的那个值】原样恢复，绝不无条件清零。
 * 关键：嵌套临界区（如 SVC/优先级更高异常夹在中间）里，内层 exit 只恢复到外层的值，
 * 不会破坏外层屏蔽状态；否则临界区会在嵌套时失效，就绪/等待链表被并发改写、调度器损坏。
 *
 * BASEPRI 写入的是“已左移 (8-__NVIC_PRIO_BITS) 位的硬件优先级值”；NVIC_SetPriority
 * 内部会再移位一次，故这里不走 NVIC_SetPriority，直接算好移位值写入。
 *
 * 阶段2 临界区审计：enter 记录进入 cycle（静态局部，嵌套时不覆盖外层起点）；exit 时
 * 若历经 cycle 数超过 RTOS_CRIT_MAX_CYCLES，调 rtos_crit_audit() 递增粘性计数
 * g_rtos_crit_overflow（不触发异常、不停机）。审计只在“最外层”临界区测总持有时长，
 * 内层嵌套共用外层起点，符合“总阻塞窗口”语义。用 cycle 而非 tick：锁调度用 BASEPRI
 * 屏蔽了 SysTick，tick 在持锁期间不前进，cycle(DWT CYCCNT) 不受 BASEPRI 影响仍计数。 */
static inline unsigned rtos_crit_enter(void) {
    uint32_t saved;
    __asm__ volatile("mrs %0, BASEPRI" : "=r"(saved));
    __asm__ volatile("msr BASEPRI, %0" :
                     : "r"((uint32_t)(RTOS_MAX_ZERO_LATENCY_IRQS
                                      << (8U - __NVIC_PRIO_BITS)))
                     : "memory");
    rtos_crit_enter_mark();                  /* 记录最外层进入 cycle（嵌套安全） */
    return (unsigned)saved;            /* 保存原掩码，exit 时原样恢复 */
}
static inline void rtos_crit_exit(unsigned st) {
    rtos_crit_exit_audit();                  /* 审计最外层临界区持锁时长 */
    __asm__ volatile("msr BASEPRI, %0" : : "r"((uint32_t)st) : "memory");
}
#else
static inline unsigned rtos_crit_enter(void) {
    unsigned s = (unsigned)irq_lock();       /* PRIMASK 全局关中断（保存 PRIMASK） */
    rtos_crit_enter_mark();
    return s;
}
static inline void rtos_crit_exit(unsigned st) {
    rtos_crit_exit_audit();
    irq_unlock((irq_state_t)st);             /* 从保存的 PRIMASK 值恢复，不直接开全局中断 */
}
#endif

/* 就绪队列级数（与 rtos_config.h 的 RTOS_MAX_PRIORITIES 一致） */
#ifndef PRIO_LEVELS
  #define PRIO_LEVELS RTOS_MAX_PRIORITIES
#endif

/* 就绪队列 pick / remove（sched.c 定义；task.c 的 rtos_start 使用） */
task_t *ready_pick(void);
void    ready_remove(task_t *t);
/* 睡眠链表加入（sched.c 定义；ipc_mutex.c 的 rtos_mutex_timedlock 用于挂计时项） */
void    sleep_add(task_t *t);

/* 把任务从它当前所在的队列（就绪/睡眠/等待，含计时阻塞双链）摘除（sched.c）。 */
void rtos_task_unlink(task_t *t);
/* 取消任务的计时阻塞（mutex handoff 在超时前拿到锁时调用）：仅当 wait_armed==1
 * 时摘除其睡眠链表条目并清标记，避免误删未计时的任务。 */
void rtos_cancel_timed_wait(task_t *t);

/* IPC 内部：判断“真 ISR（排除 SVC 重入）”（ipc_sem.c 定义；各 ipc_*.c 共用） */
int rtos_ipc_in_isr(void);

/* ---------------------------------------------------------------------------
 * 调度器链表完整性断言（硬实时内核调试辅助，零挂起风险）
 *
 * 协作式调度 + sched_lock/yield 交互历史上导致 RUNNING 任务的 TCB 同时挂在
 * “就绪”与“睡眠/等待”两条链表上（双挂），节拍 ISR 遍历损坏的 sched_next
 * 时越界/死循环 → 随机 HardFault。为定位第一个破坏者，在关键摘除点加断言，
 * 命中时【不】触发异常（避免在临界区内重入 fault handler），而是置一个粘性
 * 标志并把“被双挂的任务”指针记录下来，供调试器 / RTOSALL 自检读取。
 *
 * 设计取舍：
 *   - 不停机、不关中断，绝不引入新的挂起路径（硬实时内核红线）。
 *   - RTOS_SCHED_ASSERT 默认开启；发布构建可 #define RTOS_SCHED_ASSERT_OFF 关闭
 *     （仅在 rtos_internal.h 顶部，对 production 镜像零开销）。
 *   - 断言失败计数器 g_sched_invariant_fail 单调递增，故即便破坏者已被后续
 *     调度覆盖，也保留“曾发生过”的证据；g_sched_bad_tcb 指向最近一次命中时的 TCB。
 * ------------------------------------------------------------------------- */
#ifndef RTOS_SCHED_ASSERT_OFF
  #define RTOS_SCHED_ASSERT(cond)                                          \
      do {                                                                 \
          if (!(cond)) { rtos_sched_assert_fail((const char *)__FILE__,    \
                                                (int)__LINE__); }          \
      } while (0)
#else
  #define RTOS_SCHED_ASSERT(cond) do { (void)0; } while (0)
#endif

/* 断言失败记录器（sched.c 定义；rtos_internal.h 声明以便跨 core 文件调用） */
void rtos_sched_assert_fail(const char *file, int line);

/* 粘性标志：链表不变量被破坏的次数（永不清零，单调递增） */
extern volatile uint32_t g_sched_invariant_fail;
/* 最近一次破坏发生时涉及的 TCB（指向其 prio/state/链表指针，供调试器解析） */
extern volatile task_t  *g_sched_bad_tcb;
/* 最近一次断言失败的源文件行号（便于无符号也能定位） */
extern volatile uint32_t g_sched_bad_line;

/* 调度器全局（sched.c 定义；task.c / syscalls.c 引用） */
extern task_t          *g_running;
extern volatile uint32_t g_tick;
extern int             g_rtos_started;
/* g_in_svc 已在 rtos.h 声明（extern volatile int g_in_svc;） */

/* 临界区持锁超长计数（阶段2，sched.c 定义）：任何内核临界区超过 RTOS_CRIT_MAX_TICKS
 * 即递增（粘性、零挂起风险）。供 RTOSALL/RTOSCRIT 自检读取。 */
extern volatile uint32_t g_rtos_crit_overflow;

/* 临界区持锁时长直方图（中断延迟优化定位器，sched.c 定义，门控 RTOS_SCHED_TRACE）：
 * 每次「最外层」临界区退出时把持锁 cycle 数落入细桶，定位「长临界区」真凶 —— 即 IRQ→任务
 * 唤醒延迟最坏 32.8µs 的元凶来源。桶定义与 rtos_latency_t 一致，便于同表对照。
 *   桶 b (0..BUCKETS-1) 覆盖 [b*STEP, (b+1)*STEP) cyc；溢出桶 = BUCKETS (>BUCKETS*STEP cyc)。
 * 常态构建（RTOS_SCHED_TRACE=0）不定义这些符号（零开销、零 RAM）。 */
#if RTOS_SCHED_TRACE
  #ifndef RTOS_CRIT_HIST_BUCKETS
    #define RTOS_CRIT_HIST_BUCKETS 16u        /* 0..15us 共 16 桶 */
  #endif
  #define RTOS_CRIT_HIST_STEP 168u            /* 1us @168MHz */
  #define RTOS_CRIT_HIST_OVER (RTOS_CRIT_HIST_BUCKETS)
extern volatile uint32_t g_crit_hist[RTOS_CRIT_HIST_BUCKETS + 1u];
extern volatile uint32_t g_crit_hist_total;
extern volatile uint32_t g_crit_hist_max;
#endif

/* PendSV 切换分段计时（定位 33µs 唤醒延迟长尾的真实去向，门控 RTOS_SCHED_TRACE）：
 * 在 context.S 的 PendSV_Handler 内、7 个里程碑处各打一次 DWT CYCCNT 戳（相对入口 T0 的
 * 偏移，cycle 数），用于把「一次上下文切换」拆成 SAVE / KERNEL(选任务+临界区) / MPU(栈 region
 * 重配) / RESTORE / EXIT 各段，精确看 33µs 花在哪。
 *   索引：0=T0(入口,恒0) 1=s16-s31 压栈后(FPU任务专有,basic帧不更新)
 *         2=SAVE 完成(r4-r11 ± s16-s31) 3=rtos_pendsv_switch 返回(内核段)
 *         4=apply_task_priv 返回(MPU段) 5=RESTORE 完成 6=bx 前(EXIT段)
 * 仅存「最后一次」切换的分段（典型切换画像）；g_pendsv_seg_valid 非零表示已采集。
 * 常态构建（=0）不定义，零开销、零 RAM。 */
#if RTOS_SCHED_TRACE
  #define RTOS_PENDSV_SEG_N 7u
extern volatile uint32_t g_pendsv_seg[RTOS_PENDSV_SEG_N];
extern volatile uint32_t g_pendsv_seg_valid;
extern volatile uint32_t g_pendsv_t0;   /* PendSV 入口 CYCCNT 基准（存全局，避免被 C 调用破坏 r12） */
#endif

/* 软件定时器（core/timer.c，docs/rtos-test-plan.md §6.3）：
 *  - rtos_timer_tick 由 rtos_tick_isr 调用（ISR 上下文，已处于临界区），
 *    扫描活动定时器、到期者置 pending 并唤醒定时器任务；
 *  - rtos_timer_init_daemon 由 rtos_start 调用，创建专用定时器任务（任务上下文，
 *    切到首个任务之前），绝不在 ISR 里懒建任务。 */
void rtos_timer_tick(void);
void rtos_timer_init_daemon(void);
/* 处理所有 pending 定时器（执行回调于任务上下文）。定时器任务体与确定性自测共用：
 * 自测在 irq_lock 窗口内冻结真实 sysTick 后直接调用，脱离 1kHz 节拍做快速断言。 */
void rtos_timer_run_pending(void);

/* 任务计数（sched.c 定义并在 rtos_init 中复位；task.c 创建/查询用） */
extern int g_task_count;

/* 任务对象池（task.c 定义，CCM/.ccm_bss；静态分配、无堆）。
 * 导出供 sched_trace.c 在切换点把 task_t* 换算成稳定池索引（诊断/导出用，
 * 不参与调度逻辑，零回归）。 */
extern task_t g_task_pool[RTOS_MAX_TASKS];

/* IPC 误用计数（§4.2）：当阻塞式 IPC 在“不应阻塞”的上下文（ISR / 内核未启动）
 * 被调用而退化为忙等/非阻塞时累加，便于事后定位误用（行为不变）。sched.c 定义。 */
extern volatile uint32_t g_ipc_misuse;

#endif /* JOC_RTOS_INTERNAL_H */
