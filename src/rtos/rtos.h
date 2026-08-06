#ifndef JOC_RTOS_H
#define JOC_RTOS_H

#include <stddef.h>
#include <stdint.h>
#include "rtos_config.h"
#include "arch/rtos_arch.h"   /* 移植契约：arch 层必须提供的入口（rtos_arch_start / rtos_schedule_request / rtos_arch_tick_id） */

/* ---------------------------------------------------------------------------
 * jOS RTOS 公开 API
 *
 * 调度模型：抢占式固定优先级（0=最高），就绪队列位图 O(1) 选最高就绪任务；
 * 上下文切换走 PendSV（最低优先级异常）；时基来自 systick（1 kHz）。
 *
 * 任务使用 PSP（进程栈），内核/ISR 使用 MSP，天然隔离。
 * 所有阻塞原语（osal_sem / rtos_msleep / IPC）都通过 PendSV 让出 CPU，不忙等。
 * ------------------------------------------------------------------------- */

typedef enum {
    TASK_READY    = 0,
    TASK_RUNNING  = 1,
    TASK_BLOCKED  = 2,   /* 阻塞在某个 IPC 对象上 */
    TASK_SLEEPING = 3,   /* 阻塞在延时上 */
    TASK_DEAD     = 4,
    TASK_SUSPENDED = 5   /* 被 rtos_task_suspend 挂起，不在任何就绪/等待队列中 */
} task_state_t;

typedef struct task task_t;
struct task {
    /* sp 必须放在 offset 0：SVC_Handler 汇编里 ldr r1,[r0] 直接取 sp，
     * 不能依赖字段名推导偏移，故放在最前。 */
    void          *sp;          /* 当前栈指针（指向已保存的 r4 上下文） */
    const char    *name;
    uint8_t        prio;        /* 有效优先级（可能被互斥量提升） */
    uint8_t        base_prio;   /* 创建时的原始优先级（解锁/恢复用） */
    uint8_t        priv;        /* 1=特权(默认), 0=非特权；非特权任务须经 SVC 门访问内核对象 */
    task_state_t   state;
    uint8_t       *stack_base;
    size_t         stack_size;
    void         (*entry)(void *);
    void          *arg;
    /* 就绪/阻塞/睡眠链表（侵入式双向链表，复用 sched_next/prev） */
    task_t        *sched_next, *sched_prev;
    /* 阻塞在某对象上的等待链表（双向，复用 wait_next/prev） */
    task_t        *wait_next, *wait_prev;
    /* 延时剩余 tick（仅 SLEEPING 使用） */
    uint32_t       delay_ticks;
    uint32_t       runtime;     /* 累计运行 tick（统计用） */
    void          *wait_obj;    /* 阻塞在哪个对象上（调试用） */
    /* 事件标志等待条件（仅 event 使用，任务同时只等一个对象） */
    uint32_t       wait_mask;
    uint8_t        wait_mode;   /* 1=ALL(与), 0=ANY(或) */
    /* 计时阻塞（rtos_mutex_timedlock）辅助：wait_armed=1 表示任务当前同时挂在
     * “睡眠链表(计时)”与“某互斥量等待队列”上（双链）；timed_out=1 表示
     * 节拍超时已触发、任务已从等待队列摘除并置 READY。 */
    uint8_t        wait_armed;
    uint8_t        timed_out;
    /* ---- 硬实时扩展（见 docs/rtos-hard-realtime-plan.md 阶段1）----
     * 全部默认 0 = 非实时任务，语义不变、零回归。仅 rt_class!=0 的任务参与违约检测。 */
    uint8_t        rt_class;     /* 0=非实时(默认) 1=硬实时 2=软实时 */
    uint8_t        npls_hold;    /* 持有非抢占临界区(锁调度)标记（阶段2用） */
    uint32_t       deadline_ticks; /* 相对释放时间的截止期(0=无截止期) */
    uint32_t       release_tick;   /* 最近一次被释放/唤醒的 tick（WCRT/违约计算基准） */
    uint32_t       wcet_ticks;     /* 单次运行最坏预算(0=不限)，超出即 WCET 违约 */
    uint32_t       budget_used;    /* 当前运行窗口已用 tick（tick 累加，释放清零） */
    volatile uint32_t deadline_miss; /* 截止期违约计数（粘性，永不清零） */
    volatile uint32_t wcet_miss;      /* WCET 预算超出计数（粘性） */
    /* ---- 实时延迟量化（RTOS_SCHED_TRACE 构建用）----
     * wake_cycle：任务被释放/唤醒（ready_add 汇入点）那一刻的 CYCCNT 戳；
     * 在 PendSV 实际切入该任务时算 latency = switch_cycle - wake_cycle，
     * 即「ISR/唤醒源 → 任务真正运行」的端到端延迟（含 BASEPRI 屏蔽窗 + PendSV 排队）。
     * 常态构建（RTOS_SCHED_TRACE=0）不使用，零开销。 */
    uint32_t       wake_cycle;
};

/* ---- 内核生命周期 ---- */
void rtos_init(void);   /* 初始化内部表 + 在 systick 线上注册 RTOS 节拍 */
void rtos_task_create(const char *name, void (*entry)(void *), void *arg,
                      uint8_t prio, void *stack, size_t stack_size);
void rtos_start(void);  /* 选取首个任务并切换到任务模式（不再返回到原线程） */

/* 删除任务（补齐 docs/rtos-test-plan.md §6 缺口）：t==NULL 或 t==当前运行任务
 * 时删除自身；否则删除指定任务。实现从“就绪/睡眠/等待”队列摘除并置 TASK_DEAD，
 * 其 TCB 槽可被后续 rtos_task_create 复用（与任务正常返回等价，无泄漏）。
 * 约定：不要删除仍持有互斥量的任务——其 owner 指针会悬挂，等待者将永久阻塞。
 * 可安全删除的对象：就绪/睡眠中、或阻塞在某 IPC 等待队列上的任务（含“队列满时
 * 被阻塞的发送者”这类 §3.3 场景）。 */
void rtos_task_delete(task_t *t);

/* 动态修改任务优先级（TC-TASK-008）：同时更新原始优先级(base_prio) 与有效优先级。
 * 任务在就绪队列中则原子重排；被阻塞/运行时直接改 prio 并在下次调度生效。
 * 提升优先级应触发抢占（调用方随后可 rtos_schedule_request / 由内核抢占）。
 * unpriv 任务经 SVC 门执行。 */
void rtos_task_set_prio(task_t *t, uint8_t prio);

/* 挂起/恢复任务（TC-TASK-007 / TC-KERNEL-004）：
 *  - suspend：把任务从当前所在队列摘除并置 TASK_SUSPENDED（不在任何就绪/等待链表，
 *    故永不被调度；挂起自身则请求切换，由 PendSV 选其它就绪任务接管）。
 *  - resume：仅当处于 SUSPENDED 时恢复为 READY 并入就绪队列、请求调度。
 * 挂起/恢复是原子的（关中断），故高频中断里反复 suspend/resume 同一任务不会出现
 * “既不在就绪也不在挂起”的僵尸态（TC-KERNEL-004）。unpriv 任务经 SVC 门执行。 */
void rtos_task_suspend(task_t *t);
void rtos_task_resume(task_t *t);

/* 声明一块“2 的幂大小 + 基址对齐到该大小”的任务栈，供 MPU 每任务栈 region(R4)
 * 作为【栈底 subregion 溢出哨兵】使用（见 docs/rtos-design.md §6 R3）。
 * 用法：  RTOS_TASK_STACK(my_stack, 768);
 *         rtos_task_create(..., my_stack, sizeof(my_stack));  // sizeof = 对齐后的 2 的幂
 * 不满足该对齐的任务栈会自动退回软件哨兵（见 RTOS_MPU_PER_TASK_STACK）。 */
#define RTOS_STACK_ALIGN_UP(sz) \
    (((sz) <= 256)  ? 256u  : (sz) <= 512  ? 512u  : (sz) <= 1024 ? 1024u : \
     (sz) <= 2048   ? 2048u : (sz) <= 4096 ? 4096u : (sz) <= 8192 ? 8192u : 16384u)
#define RTOS_TASK_STACK(name, sz) \
    static uint8_t name[RTOS_STACK_ALIGN_UP(sz)] \
        __attribute__((aligned(RTOS_STACK_ALIGN_UP(sz)), section(".ccm_bss")))

/* ---- 任务主动让出 / 延时 / 抢占点 ---- */
void rtos_yield(void);
void rtos_msleep(uint32_t ms);
/* 抢占点（见 docs/rtos-design.md §3）：仅当存在更高(或同优先级 FIFO 中更靠前)
 * 的就绪任务时才让出 CPU；否则继续当前任务。区别于 rtos_yield（无条件让出）。 */
void rtos_schedule(void);

/* ---- 软件定时器（docs/rtos-test-plan.md §6.3，准则 rtos-test.md §2.5）----
 * 用户分配 rtos_timer_t（静态/栈）并 rtos_timer_init，再 start/stop。回调在
 * 专用“定时器任务”上下文执行（非 ISR），可做任意耗时操作（栈见定时器任务）。
 * 过期判定用【无符号 tick 比较】，故 32 位 g_tick 在 0xFFFFFFFF→0 翻转后仍能
 * 正确判定唤醒点（48 天翻转安全）。周期定时器锚定原始相位 + period，无累积漂移。 */
typedef enum {
    RTOS_TIMER_ONESHOT = 0,
    RTOS_TIMER_PERIODIC
} rtos_timer_mode_t;
typedef struct rtos_timer rtos_timer_t;
typedef void (*rtos_timer_cb_t)(rtos_timer_t *t, void *arg);

struct rtos_timer {
    const char        *name;        /* 调试名（可选） */
    rtos_timer_cb_t    cb;          /* 到期回调（任务上下文） */
    void              *arg;         /* 回调参数 */
    rtos_timer_mode_t  mode;        /* 单次 / 周期 */
    uint32_t           period_ticks;/* 周期（tick）；one-shot 为首次延时 */
    uint32_t           expire;      /* 绝对过期 tick（无符号比较，翻转安全） */
    uint8_t            active;      /* 内部：1=在活动链表中 */
    uint8_t            pending;     /* 内部：1=已到期待回调 */
    rtos_timer_t      *next;        /* 内部：活动链表 */
};

void rtos_timer_init(rtos_timer_t *t, const char *name, rtos_timer_cb_t cb, void *arg);
void rtos_timer_start(rtos_timer_t *t, rtos_timer_mode_t mode, uint32_t period_ms);
void rtos_timer_start_ticks(rtos_timer_t *t, rtos_timer_mode_t mode, uint32_t period_ticks);
void rtos_timer_stop(rtos_timer_t *t);
int  rtos_timer_is_active(rtos_timer_t *t);

/* 绝对延时（准则 §2.5 vTaskDelayUntil）：delay 直到 *last + inc_ticks，翻转安全。
 * 典型用法：uint32_t next = rtos_tick_count();
 *           for (;;) { work(); rtos_delay_until(&next, 10); }  // 每 10 tick 一次，无漂移 */
void rtos_delay_until(uint32_t *last, uint32_t inc_ticks);

/* 无符号 tick 比较辅助（翻转安全，见准则 §2.5）：
 *   rtos_tick_expired(d, now)  : now 已到/过截止点 d（含 d 本身）
 *   rtos_tick_elapsed(s, now)  : 从 s 到 now 流逝的 tick 数（翻转安全） */
static inline int      rtos_tick_expired(uint32_t deadline, uint32_t now) {
    return (int32_t)(now - deadline) >= 0;
}
static inline uint32_t rtos_tick_elapsed(uint32_t start, uint32_t now) {
    return (uint32_t)(now - start);
}

/* ---- 看门狗喂狗 + 马拉松长跑（docs/rtos-test-plan.md §6.4，准则 §4）----
 * 用 §6.3 的软件定时器原语驱动周期喂狗：回调在定时器任务(特权态)上下文刷新 IWDG。
 * 系统在某高优先级任务死锁时定时器任务抢不到 CPU -> IWDG 超时复位（看门狗意义）。
 * 注意：rtos_watchdog_enable 会 START(KR=0xCCCC) IWDG，该状态存活至下次复位，
 * 故仅马拉松模式显式调用；自测绝不 arming（仅验证喂狗/定时器路径）。 */
void rtos_watchdog_compute(uint32_t timeout_ms, uint32_t *pr, uint32_t *rlr);
void rtos_watchdog_feed(void);                       /* 手动喂狗（安全，不 arming） */
int  rtos_watchdog_enable(uint32_t timeout_ms);      /* 配置+ARM（致命，马拉松模式） */
int  rtos_watchdog_is_armed(void);
uint32_t rtos_watchdog_feeds(void);

int  rtos_marathon_start(uint8_t arm_wdt);           /* 派生长跑心跳任务组(常驻) */
void rtos_marathon_stop(void);
int  rtos_marathon_is_running(void);

/* ---- 查询 ---- */
task_t     *rtos_running(void);
uint32_t    rtos_tick_count(void);
int         rtos_is_started(void);
int         rtos_task_count(void);
const char *rtos_task_name(int i);
uint8_t     rtos_task_prio(int i);
task_state_t rtos_task_state(int i);
task_t     *rtos_task_ptr(int i);   /* 按索引取 TCB（栈水位/诊断用） */

/* ---- 硬实时属性访问器（阶段1；见 docs/rtos-hard-realtime-plan.md） ---- */
uint8_t  rtos_task_rt_class(int i);
uint32_t rtos_task_deadline(int i);
uint32_t rtos_task_wcet(int i);
uint32_t rtos_task_budget(int i);
uint32_t rtos_task_deadline_miss(int i);
uint32_t rtos_task_wcet_miss(int i);
/* 硬实时违约汇总查询（供 RTOSALL 自检与看门狗读取） */
uint32_t rtos_rt_violation(void);   /* 返回 g_rtos_deadline_violation | wcet */

/* ---- 内部：等待队列（供 osal_rtos.c / core/ipc_*.c 复用） ---- */
void  rtos_waitq_add(void **head, task_t *t);
task_t *rtos_waitq_pop_highest(void **head);
void  rtos_waitq_remove(void **head, task_t *t);
void  ready_add(task_t *t);   /* 把任务加入就绪队列（IPC 唤醒路径复用） */
/* 修改任务有效优先级并重排就绪队列（互斥量优先级提升/恢复用） */
void  rtos_set_eff_prio(task_t *t, uint8_t new_prio);

/* ---- 栈哨兵（MPU 模块实现，调度器在创建/切换任务时调用） ---- */
void rtos_stack_fill_sentinel(task_t *t);
int  rtos_stack_check_sentinel(task_t *t);
extern volatile int g_stack_overflow;

/* ---- 调度器链表完整性断言标志（sched.c 定义；见 core/rtos_internal.h） ----
 * 协作式调度 + sched_lock/yield 交互若导致 TCB 双挂（同时挂在两条链表），
 * RTOS_SCHED_ASSERT 命中时置位并递增计数（不触发异常、不停机，零挂起风险）。
 * 自检（RTOSALL/stress/p4/robust）应校验其保持为 0，否则说明链表被破坏。 */
extern volatile uint32_t g_sched_invariant_fail;
extern volatile task_t  *g_sched_bad_tcb;
extern volatile uint32_t g_sched_bad_line;

/* ---- 硬实时违约标志（sched.c 定义；见 docs/rtos-hard-realtime-plan.md 阶段1） ----
 * 任一硬实时任务的截止期/wcet 被突破，对应粘性计数递增，并把“曾发生过违约”汇总到
 * g_rtos_deadline_violation。零挂起风险：不触发异常、不停机，仅置位供诊断/看门狗读取。
 * 自检（RTOSALL）应校验其保持为 0；控制台 RTOSDEADLINE 命令打印分解计数。 */
extern volatile uint32_t g_rtos_deadline_violation;  /* OR 所有硬实时任务违约 */
extern volatile uint32_t g_rtos_wcet_violation;      /* 单独的 WCET 违约 OR */

/* ---- 非抢占临界区原语（阶段2，见 docs/rtos-hard-realtime-plan.md §3.1） ----
 * 只屏蔽 PendSV（锁调度），不关中断、不提 BASEPRI，故零延迟 ISR 仍可达；
 * 但 tick 里的时间片剥夺被暂停（持有任务 npls_hold 标记），用于“不可被时间片
 * 打断的原子外设序列”。嵌套安全（引用计数）；与内核临界区 rtos_crit_enter/exit
 * 正交：本原语不禁止更高优先级任务被唤醒抢占，只禁止时间片轮转剥夺当前任务。 */
void rtos_lock_scheduler(void);
void rtos_unlock_scheduler(void);

/* ---- 临界区持锁超长计数（阶段2，sched.c 定义） ----
 * 任何内核临界区超过 RTOS_CRIT_MAX_TICKS 即递增（粘性、零挂起风险）。
 * 供 RTOSALL / RTOSCRIT 自检读取。 */
extern volatile uint32_t g_rtos_crit_overflow;
uint32_t rtos_rt_crit_overflow(void);   /* 返回 g_rtos_crit_overflow（看门狗聚合用） */

/* ---- 临界区硬上限执行计数（P0-3，sched.c 定义） ----
 * RTOS_CRIT_KILL != REPORT 时，每次超长临界区退出触发升级处理（杀任务/武装 WDT/
 * 安全态 panic）即递增（粘性、零挂起风险）。供 RTOSALL / RTOSCRIT 自检读取。 */
extern volatile uint32_t g_rtos_crit_kill_count;
/* P0-3 安全态 panic 标志（RTOS_CRIT_KILL=PANIC 时置位）。 */
extern volatile uint32_t g_rtos_crit_kill_panic;

/* ---- 可调度性静态自检（阶段3，core/rtos_sched_analysis.c） ----
 * 固定优先级「响应时间分析（RTA/WCRT）」在启动/测试期证明硬实时任务集在截止期内
 * 可调度的。若 WCRT > deadline 即判定不可调度，粘性计数 g_rtos_sched_invalid 递增
 * （存违约任务数）。零挂起风险：不触发异常、不停机，仅置位供诊断/看门狗/RTOSALL 读取。 */
extern volatile uint32_t g_rtos_sched_invalid;   /* >0 = 存在截止期内不可调度的硬实时任务 */
uint32_t rtos_rt_sched_invalid(void);   /* 返回 g_rtos_sched_invalid（看门狗聚合用） */
/* 纯函数 RTA：对传入 (C=WCET, T=截止期/周期, P=优先级) 数组算 WCRT；返回不可调度任务数
 *（0=全可调度，负数=参数非法）；wcrt_out 输出每个任务的最坏响应时间。 */
int  rtos_wcrt_compute(const uint32_t *C, const uint32_t *T, const uint8_t *P,
                       int n, uint32_t *wcrt_out);
/* 扫描当前任务池硬实时任务跑 RTA，置位 g_rtos_sched_invalid，打印明细；返回违约任务数。 */
int  rtos_sched_validate(void);
/* 控制台 RTOSSCHED 用的详细打印（每任务 C/T/P/WCRT）。 */
void rtos_sched_analysis_print(void);

/* ---- 硬实时看门狗联动（阶段4 §4.3，sched.c 定义） ----
 * 若 RTOS_HARD_RT_WDT 开启且任一硬性实时违约计数（deadline/wcet/crit/sched）非零，
 * 武装独立看门狗（IWDG）使违约升级为确定性复位。零挂起风险：仅在 tick 中检查，
 * 不阻塞调度。返回 1=已触发看门狗联动，0=无需触发（或本宏关闭）。 */
uint32_t rtos_hard_rt_wdt_check(void);

/* 优先级天花板辅助（RTOS_LOCK_CEILING 宏用，仅任务上下文）：把当前任务有效优先级
 * 顶到 ceil_prio 并返回原优先级；restore 恢复原优先级。 */
uint8_t rtos_task_raise_prio(uint8_t ceil_prio);
void    rtos_task_restore_prio(uint8_t save_prio);

/* ---- 优先级天花板宏（阶段2 opt-in，见 §3.3） ----
 * 持锁期间把当前任务有效优先级顶到 ceil_prio，释放恢复。覆盖“非 mutex 共享资源”
 * 的优先级反转（替代裸自旋锁的优先级反转问题）。默认不强制，仅提供给硬实时任务
 * 在访问非内核对象临界资源时显式使用。需配合 rtos_set_eff_prio 行为，故仅在任务
 * 上下文使用（不可在 ISR 内调用 rtos_set_eff_prio，因其会改就绪队列）。
 * 用法：RTOS_LOCK_CEILING(PRIO) { ...临界区... }（离开作用域自动恢复原优先级）。 */
#define RTOS_LOCK_CEILING(ceil_prio)                                          \
    for (uint8_t _lc_save = rtos_task_raise_prio(ceil_prio),                  \
                  _lc_active = 1; _lc_active;                                 \
         rtos_task_restore_prio(_lc_save), _lc_active = 0)

/* ---- 硬实时任务属性与创建（阶段1） ----
 * rt_class: 0=非实时(默认) 1=硬实时 2=软实时。
 * deadline_ticks: 相对释放时间的最坏完成期限(0=无截止期)。
 * wcet_ticks: 单次运行最坏预算(0=不限)，超出即 WCET 违约。
 * 非实时任务用 rtos_task_create / rtos_task_create_ex（attr=NULL），新字段全 0，零回归。 */
typedef struct {
    uint8_t  rt_class;
    uint32_t deadline_ticks;
    uint32_t wcet_ticks;
} rtos_task_attr_t;
void rtos_task_create_rt(const char *name, void (*entry)(void *), void *arg,
                         uint8_t prio, void *stack, size_t stack_size,
                         uint8_t priv, const rtos_task_attr_t *attr);

/* ---- 栈水位（docs/rtos-test-plan.md §6.5）：创建任务时把“未使用区域”填 0xEE，
 * 运行时任务压栈从高地址向低地址覆盖真实数据；rtos_stack_used/free 从栈顶向下
 * 数连续 0xEE 计算已用/空闲字节（高水位，含初始异常帧）。仅在创建/测试路径调用，
 * 运行时零开销（不扫栈）。配合 RTOS_TASK_STACK 可给出推荐栈大小。 ---- */
void   rtos_stack_fill_watermark(task_t *t);
size_t rtos_stack_used(task_t *t);   /* 历史峰值已用字节（高水位） */
size_t rtos_stack_free(task_t *t);   /* 当前仍空闲字节 = stack_size - used */

/* ---- 内部：阻塞当前任务 / 唤醒最高等待者（调用方须持调度锁/关中断） ---- */
void  rtos_pend(void **waitq_head);
void  rtos_post(void **waitq_head);

/* ---- 内部：arch 层接口（契约见 src/rtos/arch/rtos_arch.h） ----
 * rtos_arch_start / rtos_schedule_request / rtos_arch_tick_id 由 arch 层实现；
 * 此处仅保留“可移植核心”自身实现、但供汇编/外部调用的符号。 */
void *rtos_pendsv_switch(void *old_sp); /* 由 PendSV 汇编调用，返回新 sp */
void  rtos_tick_isr(void *ctx);        /* 由 systick 共享线调用 */
void rtos_mpu_init(void);             /* 配置固定 MPU 区域并使能（对特权任务透明） */

/* ---- P2-1 调度轨迹 trace（编译期门控 RTOS_SCHED_TRACE；关闭时此 API 不暴露） ----
 * 经调试 UART 导出环形缓冲内的 (tick, from, to, reason) 切换轨迹，用于分析
 * 抢占/睡眠/时间片轮转时序。详见 core/sched_trace.h。 */
#if RTOS_SCHED_TRACE
void rtos_trace_dump(void);
/* Rhealstone 子集基准 + 实时延迟量化导出（RTOSBENCH 命令）：
 * 测任务切换/信号量混洗时间，并打印系统级 IRQ→任务唤醒延迟直方图。
 * 详见 core/rtos_bench.c。 */
void rtos_bench_run(void);
#endif

/* ===========================================================================
 * IPC 原语
 * 所有阻塞 API 都可从中断安全的上下文（ISR）调用其 *_give/_set（非阻塞）变体；
 * 阻塞变体只能在任务上下文调用，ISR 中调用会退化为忙等（见具体实现）。
 * ========================================================================= */

/* ---- 信号量（计数/二值） ---- */
typedef struct {
    uint32_t count;
    uint32_t limit;
    void    *waitq;
} rtos_sem_t;
void rtos_sem_init(rtos_sem_t *s, uint32_t initial, uint32_t limit);
int  rtos_sem_wait(rtos_sem_t *s);     /* 阻塞直到有许可 */
int  rtos_sem_trywait(rtos_sem_t *s);  /* 非阻塞，无许可返回 -1 */
void rtos_sem_give(rtos_sem_t *s);     /* 释放许可（ISR 安全） */

/* ---- 互斥量（优先级天花板协议，防优先级反转） ---- */
typedef struct {
    task_t  *owner;
    uint8_t  ceil_prio;   /* 天花板优先级：本互斥量会授予的最高优先级(数值最小) */
    uint8_t  recursive;   /* 1=递归锁（同一任务可多次 lock，TC-MTX-003） */
    uint8_t  rec_count;   /* 递归加锁计数（unlock 减到 0 才真正释放） */
    void    *waitq;
} rtos_mutex_t;
void rtos_mutex_init(rtos_mutex_t *m, uint8_t ceil_prio);
/* 初始化为递归互斥量（同一任务可重复 lock 而不死锁，TC-MTX-003） */
void rtos_mutex_init_rec(rtos_mutex_t *m, uint8_t ceil_prio);
int  rtos_mutex_lock(rtos_mutex_t *m);    /* 阻塞直到获得；自锁返回 -1 */
int  rtos_mutex_trylock(rtos_mutex_t *m); /* 非阻塞 */
int  rtos_mutex_unlock(rtos_mutex_t *m);  /* 释放；非持有者返回 -1 */
/* 带超时互斥锁：timeout_ms 内拿到锁返回 0；超时（未拿到）返回 -1。
 * 其余语义同 rtos_mutex_lock（天花板协议、不支持递归）。超时基于系统节拍，
 * 粒度 = 1/RTOS_TICK_HZ；timeout_ms==0 退化为 rtos_mutex_trylock。 */
int  rtos_mutex_timedlock(rtos_mutex_t *m, uint32_t timeout_ms);

/* ---- 消息队列（定长项、环形缓冲、阻塞收发） ---- */
typedef struct {
    uint8_t *buf;
    size_t   item_size;
    size_t   cap;
    size_t   count;
    size_t   head;        /* 下一个写入位置 */
    void    *recv_waitq;  /* 因空而阻塞的接收者 */
    void    *send_waitq;  /* 因满而阻塞的发送者 */
} rtos_mq_t;
void rtos_mq_init(rtos_mq_t *q, void *buf, size_t item_size, size_t cap);
int  rtos_mq_send(rtos_mq_t *q, const void *item);   /* 满则阻塞 */
int  rtos_mq_trysend(rtos_mq_t *q, const void *item);/* 满返回 -1 */
int  rtos_mq_recv(rtos_mq_t *q, void *item);         /* 空则阻塞 */
int  rtos_mq_tryrecv(rtos_mq_t *q, void *item);      /* 空返回 -1 */
/* 中断上下文安全发送（TC-Q-004 / TC-INT-001）：ISR 内调用，不阻塞；若队列满返回 -1，
 * 若有接收者阻塞则唤醒并请求 PendSV 延迟切换。等价于 FreeRTOS 的 xQueueSendFromISR。 */
int  rtos_mq_send_fromisr(rtos_mq_t *q, const void *item);

/* ---- 事件标志（32 位，ANY/ALL 等待） ---- */
typedef struct {
    uint32_t flags;
    void    *waitq;
} rtos_event_t;
void rtos_event_init(rtos_event_t *e);
void rtos_event_set(rtos_event_t *e, uint32_t bits); /* 置位并唤醒满足条件的等待者 */
void rtos_event_clear(rtos_event_t *e, uint32_t bits);
/* 阻塞等待：wait_all=1 要求 mask 所有位，否则任意一位；block=0 非阻塞。
 * 成功返回当前 flags；非阻塞且未满足返回 (uint32_t)-1。 */
uint32_t rtos_event_wait(rtos_event_t *e, uint32_t mask, int wait_all, int block);

/* ---- 事件总线（RTOS 一等 IPC 原语：阻塞式 topic 邮箱 + 广播唤醒）----
 * 与 src/bus/bus.c 的“同步回调式 pub/sub”（logging 后端）互补：本原语面向
 * 任务间异步解耦，订阅者以“阻塞等待某 topic”方式接收，发布时唤醒该 topic
 * 上所有等待任务（广播，pub/sub 语义）。每 topic 一个固定大小邮箱，最新一条
 * 覆盖旧条。经 SVC 门校验指针（KOBJ_BUS），非特权任务可安全使用。
 * 注意：本原语不实现超时（与 sem/mutex/mq/event 一致，阻塞即无限等待），
 * timeout_ms 仅用于区分“非阻塞(timeout_ms==0)”与“阻塞(非 0)”。 */
typedef struct {
    uint16_t  max_topics;     /* topic 数（0..max_topics-1） */
    uint16_t  item_size;      /* 每 topic 邮箱载荷最大字节数 */
    uint8_t  *mbuf;           /* max_topics*item_size：每 topic 一个最新邮箱 */
    uint16_t *mlen;           /* max_topics：每 topic 邮箱有效长度 */
    uint16_t *mpend;          /* max_topics：每 topic 是否有待取消息(0/1) */
    void    **waitq;          /* max_topics：每 topic 阻塞等待队列(任务链表头) */
} rtos_bus_t;
/* 计算 rtos_bus_init 所需后备缓冲字节数（静态分配用） */
#define RTOS_BUS_BUF_SIZE(MAX_TOPICS, ITEM_SIZE)                              \
    ((size_t)(MAX_TOPICS) * (size_t)(ITEM_SIZE)                              \
     + (size_t)(MAX_TOPICS) * sizeof(uint16_t)                              \
     + (size_t)(MAX_TOPICS) * sizeof(uint16_t)                              \
     + (size_t)(MAX_TOPICS) * sizeof(void *))
void rtos_bus_init(rtos_bus_t *b, uint16_t max_topics, size_t item_size,
                   void *buf, size_t buf_size);
/* 阻塞等待 topic：有消息则立即拷贝返回 0；无消息且 timeout_ms==0 返回 -1（非阻塞）；
 * 否则阻塞直到该 topic 被发布，唤醒后拷贝返回 0。buf/len 为输出（载荷及其长度）。*/
int  rtos_bus_wait(rtos_bus_t *b, uint16_t topic, void *buf, size_t *len, uint32_t timeout_ms);
/* 发布 topic：拷贝 data(len) 进该 topic 邮箱并广播唤醒所有等待者；返回 0。
 * 截断到 item_size。ISR 安全（仅拷贝+唤醒，不阻塞）。*/
int  rtos_bus_publish(rtos_bus_t *b, uint16_t topic, const void *data, size_t len);

/* SVC 门参数块（非特权任务经 rtos_syscall 传入，避免越过 3 个参数寄存器限制） */
typedef struct {
    rtos_bus_t *bus;
    uint16_t    topic;
    void       *buf;       /* out: 载荷 */
    size_t     *len;       /* out: 载荷长度 */
    uint32_t    timeout_ms;
} rtos_bus_wait_args_t;
typedef struct {
    rtos_bus_t       *bus;
    uint16_t          topic;
    const void       *data;
    size_t            len;
} rtos_bus_publish_args_t;

/* ---- 内核对象注册表（调试/按名查找 + SVC 门指针校验） ---- */
typedef enum {
    KOBJ_SEM = 0, KOBJ_MUTEX, KOBJ_MQ, KOBJ_EVENT, KOBJ_BUS, KOBJ_TASK, KOBJ_OTHER
} rtos_kobj_type_t;
int  rtos_kobj_register(const char *name, rtos_kobj_type_t type, void *ptr);
void rtos_kobj_deregister(rtos_kobj_type_t type, void *ptr);   /* 任务退出时回收槽位 */
void *rtos_kobj_lookup(const char *name);
void rtos_kobj_foreach(void (*cb)(const char *name, rtos_kobj_type_t type, void *ptr));
/* SVC 门用它校验“用户态传入的内核对象指针”是否真实登记过，防伪造指针越权访问。 */
int  rtos_kobj_validate(void *ptr, rtos_kobj_type_t type);
/* 控制台诊断：把当前注册表 dump 出来（RTOSKOBJ 命令） */
void rtos_kobj_dump(void);

/* IPC 误用计数（见 docs/rtos-design.md §4.2）：阻塞式 IPC 在 ISR / 内核未启动
 * 上下文被调用而退化为忙等/非阻塞的次数，供事后定位误用（行为不变）。 */
uint32_t rtos_ipc_misuse_count(void);

/* ---- 任务特权模式 + SVC 系统调用门（见 docs/rtos-design.md 第 6/8 章）----
 * 默认任务运行在特权态（驱动可直接访外设，零改造）。把 priv=0 的任务经
 * rtos_task_create_ex 创建为非特权：它不能直接碰外设（MPU 外设区仅特权），
 * 一切内核对象操作（IPC/延时/创建任务）必须走 SVC 门，由特权 Handler 模式
 * 代为执行并先用 rtos_kobj_validate 校验指针。常态任务保持特权，故对现有
 * BIST/自测零回归；非特权能力是“按需 opt-in”并通过 RTOSUSR 自测验证。 */
typedef enum {
    RTOS_SYS_GET_TICK = 1,   /* a0: -        -> 返回 g_tick */
    RTOS_SYS_YIELD,          /* 让出 CPU */
    RTOS_SYS_MSLEEP,         /* a0: ms */
    RTOS_SYS_SEM_GIVE,       /* a0: rtos_sem_t* */
    RTOS_SYS_SEM_WAIT,       /* a0: rtos_sem_t* -> 0/-1 */
    RTOS_SYS_MUTEX_LOCK,     /* a0: rtos_mutex_t* -> 0/-1 */
    RTOS_SYS_MUTEX_UNLOCK,   /* a0: rtos_mutex_t* -> 0/-1 */
    RTOS_SYS_MUTEX_TRYLOCK,  /* a0: rtos_mutex_t* -> 0/-1 */
    RTOS_SYS_MQ_SEND,        /* a0: rtos_mq_t*, a1: item* -> 0/-1 */
    RTOS_SYS_MQ_RECV,        /* a0: rtos_mq_t*, a1: item* -> 0/-1 */
    RTOS_SYS_SEM_TRYWAIT,    /* a0: rtos_sem_t* -> 0/-1 */
    RTOS_SYS_MQ_TRYSEND,     /* a0: rtos_mq_t*, a1: item* -> 0/-1 */
    RTOS_SYS_MQ_TRYRECV,     /* a0: rtos_mq_t*, a1: item* -> 0/-1 */
    RTOS_SYS_EVENT_SET,      /* a0: rtos_event_t*, a1: bits */
    RTOS_SYS_EVENT_WAIT,     /* a0: rtos_event_t*, a1: mask, a2: wait_all, a3: block -> flags/-1 */
    RTOS_SYS_BUS_WAIT,       /* a0: rtos_bus_wait_args_t* */
    RTOS_SYS_BUS_PUBLISH,    /* a0: rtos_bus_publish_args_t* */
    RTOS_SYS_TASK_CREATE,    /* a0: rtos_task_create_args_t* */
    RTOS_SYS_MUTEX_TIMEDLOCK,/* a0: rtos_mutex_t*, a1: timeout_ms -> 0/-1 */
    RTOS_SYS_TASK_DELETE,    /* a0: task_t* (NULL=删除自身) */
    RTOS_SYS_TASK_SET_PRIO,  /* a0: task_t*, a1: prio */
    RTOS_SYS_TASK_SUSPEND,   /* a0: task_t* */
    RTOS_SYS_TASK_RESUME     /* a0: task_t* */
} rtos_syscall_nr_t;

/* 非特权任务调用：从用户态触发 SVC，回到特权 Handler 模式执行系统调用。
 * 约定：r0=调用号, r1..r3=参数；返回值经 r0 带回。仅非特权路径使用。 */
uint32_t rtos_syscall(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2);

/* 当前运行任务是否“需要走 SVC 门”：非特权 + 非 ISR + 非 SVC 重入。 */
int  rtos_need_svc(void);
/* SVC 分发进行中（特权 Handler 模式代表某任务执行内核调用，非真正 ISR）。
 * IPC 原语的“是否在中断上下文”判定须排除该态，否则阻塞式 API 会误走
 * 忙等/直接返回分支，导致非特权任务经 SVC 门时死循环或静默失败。 */
extern volatile int g_in_svc;
/* 当前是否运行在非特权态（arch 探针，仅用于自测断言）。 */
int  rtos_arch_in_unpriv(void);

/* 创建任务并指定特权模式；rtos_task_create() 是其“特权”封装。 */
typedef struct {
    const char    *name;
    void (*entry)(void *);
    void          *arg;
    uint8_t        prio;
    void          *stack;
    size_t         stack_size;
    uint8_t        priv;     /* 1=特权, 0=非特权 */
} rtos_task_create_args_t;
void rtos_task_create_ex(const char *name, void (*entry)(void *), void *arg,
                         uint8_t prio, void *stack, size_t stack_size, uint8_t priv);

#if RTOS_SELFTEST
/* 非特权任务经 SVC 门使用内核对象的端到端自测（RTOSUSR 命令 + RTOSALL） */
int rtos_usr_selftest(void);

/* ---- IPC 运行时自测（从 RTOSIPC 命令调用） ---- */
int rtos_ipc_selftest(void);

/* ---- 任务/调度基础自测（从 RTOSBASIC 命令调用，并注册进 RTOSALL） ---- */
int rtos_basic_selftest(void);

/* ---- 同步与通信边界自测（从 RTOSIPC2 命令调用，并注册进 RTOSALL） ---- */
int rtos_ipc2_selftest(void);

/* ---- 鲁棒性/异常注入自测（从 RTOSROBUST 命令调用，并注册进 RTOSALL） ---- */
int rtos_robust_selftest(void);

/* ---- 时间片轮转（Round-Robin）自测（从 RTOSRR 命令调用，并注册进 RTOSALL） ---- */
int rtos_rr_selftest(void);

/* ---- 事件总线（RTOS 一等 IPC 原语）自测（从 RTOSBUS 命令调用，并注册进 RTOSALL） ---- */
int rtos_bus_selftest(void);

/* ---- 多任务并发压力自测（从 RTOSSTRESS 命令调用） ---- */
int rtos_stress_selftest(void);

/* ---- FPU 上下文保存自测（从 RTOSFPU 命令调用） ----
 * 验证 PendSV 上下文切换正确保存/恢复 s16-s31：多个任务把浮点累加器钉在 s16-s18，
 * 高频切换互相踩踏，若 s16-s31 未保存，累加器会被其它任务破坏，与期望值不符。 */
int rtos_fpu_selftest(void);

/* ---- 中断上下半部自测（从 RTOSBH 命令调用，并注册进 RTOSALL） ----
 * 验证上半部(模拟 ISR) trigger -> 下半部高优先级任务被唤醒并执行；
 * 以及工作队列提交 -> 共享 worker 执行。见 docs/rtos-design.md 第 4 章(P3)。 */
int rtos_bh_selftest(void);

/* ---- 软件定时器 / 48 天 tick 翻转自测（从 RTOSTIMER 命令调用，并注册进 RTOSALL） ----
 * 覆盖准则 §2.5：单次/周期定时器、停止、无符号 tick 比较的 48 天翻转安全、
 * 绝对延时 rtos_delay_until 的翻转安全。 */
int rtos_timer_selftest(void);

/* ---- 中断实时性 / 抗压自测（从 RTOSIRQ 命令调用，并注册进 RTOSALL "irq" 条目） ----
 * 覆盖两类关键用户态场景：(1) 实时中断响应——ISR->高优先级任务/BH 唤醒延迟有界、
 * 每个中断恰一次响应(无丢失/无重复)、绝不卡死；(2) 高频中断抗压——单路 50kHz 持续
 * 5s 与双路(TIM5/TIM2)嵌套并发 3s 下系统不崩溃、计数不丢。 */
int rtos_irq_selftest(void);

/* ---- 优先级反转验证（从 RTOSINV 命令调用，并注册进 RTOSALL "inv" 条目） ----
 * 用 L/M/H 三任务 + 优先级天花板协议，对比“受保护(天花板生效)”与“对照(无天花板)”
 * 两遍：断言天花板把高优任务等锁延迟压到“有界”(≈低优持锁方临界区)，而对照下
 * 中优先级任务抢占持锁方、使等待成倍变长（反转确实发生），从而反衬协议有效。 */
int rtos_inv_selftest(void);

/* ---- 混沌压力自测（从 RTOSFUZZ 命令调用，并注册进 RTOSALL "fuzz" 条目） ----
 * 多优先级任务对共享 IPC 池(信号量/互斥/消息队列/事件)做随机非阻塞操作持续数秒，
 * 验证不崩溃、不死锁(高优心跳持续递增)、并发数据结构在抢占下不损坏。 */
int rtos_fuzz_selftest(void);

/* ---- 看门狗喂狗 + 马拉松长跑自测（从 RTOSMARATHON 命令触发，并注册进 RTOSALL
 *      "watchdog" 条目）：仅验证安全、确定性部分（复位原因解码 / 喂狗路径 /
 *      周期喂狗定时器集成）；不 arming IWDG，避免把板子锁进复位循环。 */
int rtos_watchdog_selftest(void);

/* ---- 硬实时验收与稳定性测试套件（从 RTOSACCEPT 命令调用，注册进 RTOSALL
 *      "accept" 条目但刻意不进 RTOSALL 长链）：A 实时性量化(周期任务集成验收 /
 *      中断唤醒延迟 WCET 数据库 / 调度抖动 / RTA 正确性) + B 稳定性(长时 soak /
 *      资源耗尽 graceful 降级) + C 健壮性深度注入(真实栈溢出 / 并发故障 /
 *      长临界区突破硬实时)。见 docs/rtos-acceptance-test-plan.md。 */
int rtos_accept_selftest(void);
/* P1-4：RTOSACCEPT long —— 1 小时长时 soak（无 TCB 泄漏 / 无计数漂移验证）。 */
int acc_b1_soak_long(void);
#endif /* RTOS_SELFTEST */

/* ===========================================================================
 * 编译期段收集（见 docs/rtos-design.md 第 5 章 / P4）
 *
 * 用 RTOS_TASK / RTOS_MSGQ / RTOS_BH 宏把对象“定义”放进专用链接段
 * (._rtos_tasks / ._rtos_ipc / ._rtos_bh)；rtos_instantiate_sections() 在
 * rtos_start() 里遍历这些段，自动建任务 / IPC / 下半部任务。加功能只需在源
 * 文件里放一个宏，无需编辑内核中央数组（对标 Zephyr 段收集 / init_array）。
 *
 * 栈缓冲不能放进 const 段，故由调用方另行声明（宏只收集“定义指针”）。
 * ========================================================================= */
typedef struct {
    const char    *name;
    void (*entry)(void *);
    uint8_t        prio;
    uint8_t       *stack;
    size_t         stack_size;
    void          *arg;
    uint8_t        priv;   /* 1=特权(默认), 0=非特权(用户态任务，须走 SVC 门) */
} rtos_task_def_t;

typedef struct {
    const char *name;
    rtos_mq_t  *mq;          /* 用户提供的 rtos_mq_t 对象（RAM） */
    void       *buf;         /* 用户提供的环形缓冲（RAM） */
    size_t      item_size;
    size_t      cap;
} rtos_mq_def_t;

typedef struct {
    const char    *name;
    uint8_t        prio;
    uint8_t       *stack;
    size_t         stack_size;
    void (*fn)(void *);
    void          *ctx;
} rtos_bh_def_t;

extern const rtos_task_def_t __rtos_tasks_start[];
extern const rtos_task_def_t __rtos_tasks_end[];
extern const rtos_mq_def_t   __rtos_ipc_start[];
extern const rtos_mq_def_t   __rtos_ipc_end[];
extern const rtos_bh_def_t   __rtos_bh_start[];
extern const rtos_bh_def_t   __rtos_bh_end[];

#define RTOS_TASK(_sym, _name, _entry, _prio, _stack, _ssz, _arg, _priv)    \
    static const rtos_task_def_t __attribute__((used,                         \
        section("._rtos_tasks." #_sym))) _rtos_task_##_sym = {               \
        .name = _name, .entry = _entry, .prio = _prio,                       \
        .stack = (uint8_t *)(_stack), .stack_size = _ssz,                    \
        .arg = (void *)(_arg), .priv = (uint8_t)(_priv) }

#define RTOS_MSGQ(_sym, _name, _mq, _buf, _isz, _cap)                        \
    static const rtos_mq_def_t __attribute__((used,                          \
        section("._rtos_ipc." #_sym))) _rtos_mq_##_sym = {                   \
        .name = _name, .mq = _mq, .buf = _buf, .item_size = _isz, .cap = _cap }

#define RTOS_BH(_sym, _name, _prio, _stack, _ssz, _fn, _ctx)                 \
    static const rtos_bh_def_t __attribute__((used,                          \
        section("._rtos_bh." #_sym))) _rtos_bh_##_sym = {                    \
        .name = _name, .prio = _prio, .stack = (uint8_t *)(_stack),          \
        .stack_size = _ssz, .fn = _fn, .ctx = (void *)(_ctx) }

/* 遍历上述三个链接段，自动实例化所有段收集到的对象（rtos_start() 调用一次）。 */
void rtos_instantiate_sections(void);

#if RTOS_SELFTEST
/* ---- 编译期段收集 + 收尾自测（从 RTOSP4 命令调用，并注册进 RTOSALL） ----
 * 覆盖：(1) 段收集机制验证（宏→链接段→自动建对象且运行）；
 *       (2) 调度延迟（DWT CYCCNT 测上半部触发→下半部运行的 wake latency）；
 *       (3) 优先级反转（互斥量天花板协议防止 H 被 M 饿死）；
 *       (4) 上半部有界性（模拟 ISR 快进快出路径的耗时上限）。
 * 另：MPU 越权 Fault 自测独立注册为 "mpu" 条目，由 RTOSALL 一并运行。 */
int rtos_p4_selftest(void);

/* ---- 编译期自测注册表（链接器段收集，见 linker .rtos_selftests） ----
 * 各模块用 RTOS_SELFTEST_ADD("name", fn) 把自测注册进 .rtos_selftests.<name>
 * 段；rtos_selftest_run_all() 在运行时遍历该段依次执行，无需手动逐个调用。
 * 段起止符号由链接脚本 PROVIDE（__rtos_selftest_start / _end）。 */
typedef int (*rtos_selftest_fn_t)(void);
typedef struct {
    const char         *name;
    rtos_selftest_fn_t  fn;
} rtos_selftest_entry_t;

extern const rtos_selftest_entry_t __rtos_selftest_start[];
extern const rtos_selftest_entry_t __rtos_selftest_end[];

#define RTOS_SELFTEST_ADD(_name, _fn)                                          \
    static const rtos_selftest_entry_t __attribute__((used,                    \
        section(".rtos_selftests." _name))) _rtos_selftest_##_fn = {          \
        .name = _name, .fn = _fn                                               \
    }

/* 统一测试报告宏（见 docs/rtos-test-plan.md §5）：打印机器可解析的
 *   [RESULT] <CaseName>: PASS|FAIL
 * 行，供上位机 tools/rtos_test_all.py 收集汇成报告。需在使用处已包含
 * "log/log.h" 与 "log/app_log.h"（各 *_selftest 模块均包含）。 */
#define RTOS_TEST_RESULT(_name, _ok)                                          \
    log_printf(app_log(), LOG_INFO, "rtos", "[RESULT] %s: %s\n",              \
               (_name), (_ok) ? "PASS" : "FAIL")

/* 遍历编译期收集的所有自测并依次运行，返回整体是否全部 PASS */
int rtos_selftest_run_all(void);
#else
  /* 发布构建：自测注册退化为 no-op，链接段 .rtos_selftests 自然为空；
   * rtos_selftest_run_all 不被编译（其调用方 RTOSALL 命令同样被门控）。 */
  #define RTOS_SELFTEST_ADD(_name, _fn)  /* no-op（发布构建剔除自测） */
#endif /* RTOS_SELFTEST */

#endif /* JOC_RTOS_H */
