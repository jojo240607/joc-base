#include "rtos.h"
#include "common/ccm_bss.h"
#include "rtos_internal.h"
#include "common/lock.h"
#include "irq.h"
#include "irq_manager.h"   /* irq_manager_audit_priorities：启动期中断优先级契约校验 */
#include "bh.h"
#include <string.h>

#if defined(__riscv)
#include "riscv.h"   /* RISCV_FRAME_* 初始帧常量（task_stack_init） */
/* __global_pointer$ 来自链接脚本（.sdata + 0x800），task 恢复时必须保持有效 */
extern uint32_t __global_pointer$;
#endif

/* ---------------------------------------------------------------------------
 * jOS 任务管理（core/task.c）：TCB 静态池、初始栈帧、任务创建/退出、
 * 编译期段收集实例化（rtos_instantiate_sections）、rtos_start、查询 API。
 *
 * 就绪队列/睡眠/切换见 core/sched.c；SVC 分发见 core/syscalls.c。
 * ------------------------------------------------------------------------- */

/* ---- TCB 静态池（无堆，确定性）：放 CCM，纯 CPU 访问、不占主 SRAM ---- */
/* 非 static：rtos_internal.h 已 extern 声明，供 sched_trace.c 在切换点把 task_t*
 * 换算成稳定池索引（诊断/导出用，不参与调度逻辑）。其余访问仍走 rtos_task_* API。 */
task_t g_task_pool[RTOS_MAX_TASKS] RTOS_CCM_BSS;

/* 前向声明：任务入口返回时调用，标记 TASK_DEAD 并请求重新调度 */
static void rtos_task_exit(void);

/* ---- 初始栈帧：伪造一次异常入栈，使首次切换的“恢复”路径合法 ---- */
static void task_stack_init(task_t *t) {
#if defined(__riscv)
    /* RISC-V：无硬件自动压栈，初始帧即 _trap_handler 保存的 32 字帧
     * （与 riscv.h 的 RISCV_FRAME_* 严格一致）。t->sp 指向帧基址，恢复时
     * sp += 0x80 后 mret。寄存器语义：
     *   mstatus = MPIE|MPP_M —— 首次 mret 后 MIE=1（开中断）、M 模式；
     *   mepc    = 任务入口；
     *   a0      = 任务参数（arg）；
     *   ra      = rtos_task_exit（任务返回时的退出路径，与 Cortex-M LR 同义）。
     * 其余寄存器清零（gp/tp 由运行时设置或不用）。 */
    uint32_t *sp = (uint32_t *)((uint8_t *)t->stack_base + t->stack_size);
    sp -= RISCV_FRAME_SIZE / 4u;                 /* 回退 32 字 = 0x80 B */
    memset(sp, 0, RISCV_FRAME_SIZE);
    sp[RISCV_FRAME_MSTATUS / 4u] = RISCV_FRAME_MSTATUS_INIT;
    sp[RISCV_FRAME_MEPC    / 4u] = (uint32_t)(uintptr_t)t->entry;
    sp[RISCV_FRAME_A0      / 4u] = (uint32_t)(uintptr_t)t->arg;
    sp[RISCV_FRAME_RA      / 4u] = (uint32_t)(uintptr_t)rtos_task_exit;
    /* gp：初始帧全部清零后 gp=x3=0，但 RISC-V 的 gp-relative 小数据寻址依赖
     * 全局指针。若不清零则首次 mret 后 gp=0，所有 gp-relative 访问乱了。
     * 这里把链接脚本算出的 __global_pointer$ 值写入帧内 gp 槽，
     * 使首任务（及所有用此函数创建的任务）一启动即持有正确的 gp。 */
    sp[RISCV_FRAME_GP      / 4u] = (uint32_t)(uintptr_t)&__global_pointer$;
    t->sp = (void *)sp;
#else
    uint32_t *sp = (uint32_t *)((uint8_t *)t->stack_base + t->stack_size);
    /* 硬件自动弹出的 8 字异常帧：xPSR, PC, LR, R12, R3, R2, R1, R0 */
    *--sp = 0x01000000u;               /* xPSR（T 位必须置 1） */
    *--sp = (uint32_t)t->entry;        /* PC = 任务入口 */
    *--sp = (uint32_t)rtos_task_exit;  /* LR = 任务返回地址 */
    *--sp = 0;                         /* R12 */
    *--sp = 0;                         /* R3 */
    *--sp = 0;                         /* R2 */
    *--sp = 0;                         /* R1 */
    *--sp = (uint32_t)t->arg;          /* R0 = 参数 */
    /* 软件保存的 r4..r11 */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* R11 R10 R9 R8 */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* R7  R6  R5 R4 */
    /* EXC_RETURN 槽：使“首次 SVC 启动的任务”在第一次被 PendSV 抢占/恢复时，
     * 其栈帧布局与“被抢占的任务”完全一致（栈顶都是 EXC_RETURN），否则 PendSV
     * 恢复路径会误把 R4 当 EXC_RETURN 导致 INVSTATE HardFault。 */
    *--sp = 0xFFFFFFFDu;                          /* 返回线程模式 / PSP / 无 FPU 帧 */
    t->sp = (void *)sp;
#endif
}

/* 任务真正退出的内核路径（特权上下文执行）：标记 TASK_DEAD 并请求切换。
 * 不能在 U 模式直接执行——rtos_crit_enter 的 irq_lock 读 mstatus（M 模式 CSR），
 * U 模式访问会触发 Illegal instruction（mcause=2）。非特权任务经
 * RTOS_SYS_TASK_EXIT SVC 门由 syscalls.c 在 M 模式分发调用本函数。 */
void rtos_task_exit_priv(void) {
    unsigned st = rtos_crit_enter();
    if (g_running) {
        rtos_kobj_deregister(KOBJ_TASK, g_running);   /* 回收内核对象表条目，避免 RTOSALL 串联时注册表溢出 */
        g_running->state = TASK_DEAD;
    }
    rtos_crit_exit(st);
    /* 请求切换：任务已是 TASK_DEAD，rtos_yield 不会把它重新入就绪队列，仅置位
     * 调度请求（RISC-V 上写 msip），下一个 READY 任务在 mret 后接管本 CPU。 */
    rtos_yield();
}

static void rtos_task_exit(void) {
    if (rtos_need_svc()) {
        /* 非特权任务：直接执行上面的特权路径会读 mstatus 触发 Illegal instruction，
         * 故经 SVC 门在 M 模式执行（syscalls.c 的 RTOS_SYS_TASK_EXIT）。mret 后本
         * 任务停在 for(;;) 挂死，随后挂起的软件中断把已 DEAD 的它切出、永不再调度。 */
        rtos_syscall(RTOS_SYS_TASK_EXIT, 0, 0, 0);
        for (;;) { }
    }
    rtos_task_exit_priv();
    for (;;) { }
}

/* ---- 生命周期 ---- */
void rtos_task_create(const char *name, void (*entry)(void *), void *arg,
                      uint8_t prio, void *stack, size_t stack_size) {
    /* 默认创建特权任务（与历史行为一致，对现有 BIST/自测零回归）。 */
    rtos_task_create_ex(name, entry, arg, prio, stack, stack_size, 1);
}

/* 真正创建（内部）：完成参数校验、槽位回收、TCB 初始化与就绪入队。
 * attr 可空（NULL = 非实时任务，阶段1 新字段全 0，零回归）。 */
static void rtos_task_create_full(const char *name, void (*entry)(void *), void *arg,
                                  uint8_t prio, void *stack, size_t stack_size,
                                  uint8_t priv, const rtos_task_attr_t *attr) {
    /* 参数校验（TC-TASK-002/003/004）：非法优先级 / 空入口 / 空栈 / 零栈大小
     * 一律拒绝创建，系统不崩溃。 */
    if (!entry || !stack || stack_size == 0) return;
    if (prio >= PRIO_LEVELS) return;
    task_t *t = (task_t *)0;
    /* 回收已终止(TASK_DEAD)的槽位，使 RTOS* 自测可重复运行：
     * 任务正常返回(entry 落到 rtos_task_exit 置 DEAD)后，其池槽可被新任务复用，
     * 否则 g_task_count 单调增长、几次后创建即被拒（原设计只能单次调用）。 */
    for (int i = 0; i < g_task_count; i++) {
        if (g_task_pool[i].state == TASK_DEAD) { t = &g_task_pool[i]; break; }
    }
    if (!t) {
        if (g_task_count >= RTOS_MAX_TASKS) return;   /* 池真正耗尽 */
        t = &g_task_pool[g_task_count++];
    }
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->prio = prio;
    t->base_prio = prio;
    t->priv = priv ? 1u : 0u;
    t->entry = entry;
    t->arg = arg;
    t->stack_base = (uint8_t *)stack;
    t->stack_size = stack_size;
    t->state = TASK_READY;
    /* 硬实时属性（阶段1）：attr 非空才写，否则全 0 = 非实时。 */
    if (attr) {
        t->rt_class      = attr->rt_class;
        t->deadline_ticks = attr->deadline_ticks;
        t->wcet_ticks    = attr->wcet_ticks;
        /* release_tick / budget_used 在首次被释放/唤醒时由调度器写入（见 sched.c）。
         * 这里给个初始 release = 0，避免未运行前误报违约。 */
        t->release_tick  = 0;
        t->budget_used   = 0;
    }
    task_stack_init(t);
    rtos_stack_fill_watermark(t);  /* 未使用区填 0xEE（栈水位高水位测量） */
    rtos_stack_fill_sentinel(t);   /* 栈底魔数覆盖 0xEE，供切换时检测溢出 */
    ready_add(t);
    if (name) rtos_kobj_register(name, KOBJ_TASK, t);   /* 任务注册进内核对象表（按名可取） */
}

void rtos_task_create_ex(const char *name, void (*entry)(void *), void *arg,
                         uint8_t prio, void *stack, size_t stack_size, uint8_t priv) {
    /* 非特权任务调用本接口时，必须经由 SVC 门（在特权 Handler 模式里真正建任务）。
     * 用 g_in_svc 标记避免：SVC 处理内部再次调用本函数时又触发 SVC 而死循环。 */
    if (rtos_need_svc()) {
        rtos_task_create_args_t a;
        a.name = name; a.entry = entry; a.arg = arg;
        a.prio = prio; a.stack = stack; a.stack_size = stack_size; a.priv = priv;
        rtos_syscall(RTOS_SYS_TASK_CREATE, (uint32_t)&a, 0, 0);
        return;
    }
    rtos_task_create_full(name, entry, arg, prio, stack, stack_size, priv, (const rtos_task_attr_t *)0);
}

void rtos_task_create_rt(const char *name, void (*entry)(void *), void *arg,
                         uint8_t prio, void *stack, size_t stack_size,
                         uint8_t priv, const rtos_task_attr_t *attr) {
    /* 硬实时任务创建：仅特权上下文创建（非特权任务不应自己声明硬实时属性，
     * 须经 SVC 由特权创建者指定）。此处直接走 full，attr 落地。 */
    if (rtos_need_svc()) {
        /* 非特权路径：硬实时任务应由特权代码创建，这里退回普通 ex 语义（attr 忽略），
         * 保持不崩溃；真正的硬实时创建应在特权上下文调用本函数。 */
        rtos_task_create_ex(name, entry, arg, prio, stack, stack_size, priv);
        return;
    }
    /* 阶段4 §4.2：硬实时任务必须占据系统最高优先级带（prio <= RTOS_PRIO_BH_HIGH），
     * 防止被放到会被时间片/低优先任务耽误的低优先级，保证其截止期有抢占保障。
     * 零回归：仅对 rt_class!=0 的任务施加；非实时 attr=NULL 的任务不调用本接口。 */
    if (attr && attr->rt_class != 0 && prio > RTOS_PRIO_BH_HIGH) {
        RTOS_SCHED_ASSERT(0);   /* 硬实时任务优先级越界：必须置于 BH_HIGH 及以上 */
        /* 不返回错误码（创建接口无失败返回），退回裁剪到上限优先级，保证仍可运行。 */
        prio = RTOS_PRIO_BH_HIGH;
    }
    rtos_task_create_full(name, entry, arg, prio, stack, stack_size, priv, attr);
}

/* 删除任务（docs/rtos-test-plan.md §6.1）：从当前所在队列摘除并置 TASK_DEAD。
 * 删除自身时标记 DEAD 后让出 CPU 且不再返回（栈不再使用），由下一个任务接管。 */
void rtos_task_delete(task_t *t) {
    if (!g_rtos_started) return;
    if (rtos_need_svc()) {
        rtos_syscall(RTOS_SYS_TASK_DELETE, (uint32_t)t, 0, 0);
        return;
    }
    unsigned st = rtos_crit_enter();
    if (!t) t = g_running;
    /* 单核下唯一 RUNNING 任务是 g_running；若 t 既非自身也非 RUNNING，则它必处于
     * 就绪/睡眠/阻塞中的某一队列，可直接摘除。删除自身走下方 self 分支。 */
    int self = (t == g_running);
    rtos_task_unlink(t);                 /* 从就绪/睡眠/等待队列摘除 */
    rtos_kobj_deregister(KOBJ_TASK, t);  /* 回收内核对象表条目 */
    t->state = TASK_DEAD;                /* TCB 槽可被后续 rtos_task_create 复用 */
    rtos_crit_exit(st);
    if (self) {
        /* 当前任务进入 DEAD：g_running 仍指向本任务，但 pend 路径不会再把 DEAD
         * 任务入就绪队列；请求切换后下一个 READY 任务接管，本任务栈不再被使用。 */
        rtos_yield();
        for (;;) { }
    }
    rtos_schedule_request();
}

/* ---- 动态优先级（TC-TASK-008） ---- */
void rtos_task_set_prio(task_t *t, uint8_t prio) {
    if (!g_rtos_started || !t) return;
    if (rtos_need_svc()) {
        rtos_syscall(RTOS_SYS_TASK_SET_PRIO, (uint32_t)t, prio, 0);
        return;
    }
    if (prio >= PRIO_LEVELS) prio = (uint8_t)(PRIO_LEVELS - 1);
    unsigned st = rtos_crit_enter();
    t->base_prio = prio;
    rtos_set_eff_prio(t, prio);   /* 若 t 在就绪队列则原子重排，否则直接改 prio */
    rtos_crit_exit(st);
}

/* ---- 挂起 / 恢复（TC-TASK-007 / TC-KERNEL-004） ---- */
void rtos_task_suspend(task_t *t) {
    if (!g_rtos_started || !t) return;
    if (rtos_need_svc()) {
        rtos_syscall(RTOS_SYS_TASK_SUSPEND, (uint32_t)t, 0, 0);
        return;
    }
    unsigned st = rtos_crit_enter();
    /* 从当前所在队列（就绪/睡眠/等待，含计时阻塞双链）摘除，再置挂起态。
     * RUNNING（g_running）任务不在任何链表，unlink 走 default 分支无操作。 */
    rtos_task_unlink(t);
    t->state = TASK_SUSPENDED;
    rtos_crit_exit(st);
    if (t == g_running) {
        /* 挂起自身：请求切换，下一个 PendSV 选其它 READY 任务接管；本任务
         * 因不在任何队列中，永不被再次调度，直到 rtos_task_resume。 */
        rtos_schedule_request();
    }
}

void rtos_task_resume(task_t *t) {
    if (!g_rtos_started || !t) return;
    if (rtos_need_svc()) {
        rtos_syscall(RTOS_SYS_TASK_RESUME, (uint32_t)t, 0, 0);
        return;
    }
    unsigned st = rtos_crit_enter();
    if (t->state == TASK_SUSPENDED) {
        t->state = TASK_READY;
        ready_add(t);
    }
    rtos_crit_exit(st);
    rtos_schedule_request();
}

/* 编译期段收集（P4，见 docs/rtos-design.md 第 5 章）：遍历 ._rtos_tasks /
 * ._rtos_ipc / ._rtos_bh 段，自动建任务 / 消息队列 / 下半部任务。
 * 一次性执行（加功能只需在源文件里放一个 RTOS_TASK/MSGQ/BH 宏，无需改此处）。
 * 段起止符号由链接脚本 PROVIDE；空段（无宏）时范围相等、循环不执行。 */
void rtos_instantiate_sections(void) {
    static int done;
    if (done) return;
    done = 1;
    for (const rtos_task_def_t *p = __rtos_tasks_start; p < __rtos_tasks_end; p++)
        rtos_task_create_ex(p->name, p->entry, p->arg, p->prio,
                            p->stack, p->stack_size, p->priv);
    for (const rtos_mq_def_t *p = __rtos_ipc_start; p < __rtos_ipc_end; p++)
        rtos_mq_init(p->mq, p->buf, p->item_size, p->cap);
    for (const rtos_bh_def_t *p = __rtos_bh_start; p < __rtos_bh_end; p++)
        rtos_bh_task_create(p->name, p->prio, p->stack, p->stack_size, p->fn, p->ctx);
}

void rtos_start(void) {
    rtos_cycle_init();           /* 使能 DWT 周期计数器（P4 延迟/有界性测量用） */
    rtos_instantiate_sections(); /* 编译期段收集：自动建任务/IPC/BH（P4） */
    rtos_workq_init();            /* 提前建好共享工作队列 worker（任务上下文，避免 ISR 内建任务） */
    rtos_timer_init_daemon();      /* 提前建好专用定时器任务（任务上下文，避免 ISR 内建任务） */

    /* 启动期中断优先级契约审计：启用零延迟 IRQ（RTOS_MAX_ZERO_LATENCY_IRQS>0）时，
     * 任何调用内核 API 的 ISR 必须落在可被 BASEPRI 屏蔽的优先级带（prio >= 阈值），
     * 否则会在切换临界区里抢占就绪表造成重入。违例会被审计函数逐个打印；默认阈值=0
     * 不触发任何违例，行为与此前完全一致、零回归。 */
    irq_manager_audit_priorities((uint8_t)RTOS_MAX_ZERO_LATENCY_IRQS);
    task_t *first = ready_pick();
    if (!first) return;
    ready_remove(first);
    first->state = TASK_RUNNING;
    g_running = first;
    g_rtos_started = 1;
#if RTOS_USE_MPU
    rtos_mpu_init();     /* 配置固定区域并使能 MPU + MemManage（对特权任务透明） */
#endif
    /* 阶段3：启动期可调度性静态自检（固定优先级 RTA）。即便发现不可调度任务也不
     * 阻塞 boot（只置位粘性标志 g_rtos_sched_invalid 供 RTOSALL/看门狗读取）；
     * 当前系统无硬实时任务，平凡通过、零回归。 */
    rtos_sched_validate();
    rtos_arch_start();   /* 触发 SVC；切换后不再返回原线程 */
}

/* ---- 查询 API ---- */
task_t *rtos_running(void)       { return g_running; }
uint32_t rtos_tick_count(void)    { return g_tick; }
int rtos_is_started(void)         { return g_rtos_started; }
int rtos_task_count(void)         { return g_task_count; }
const char *rtos_task_name(int i) { return (i >= 0 && i < g_task_count) ? g_task_pool[i].name : (const char *)0; }
uint8_t rtos_task_prio(int i)     { return (i >= 0 && i < g_task_count) ? g_task_pool[i].prio : 0; }
task_state_t rtos_task_state(int i) {
    return (i >= 0 && i < g_task_count) ? g_task_pool[i].state : TASK_DEAD;
}
task_t *rtos_task_ptr(int i) {
    return (i >= 0 && i < g_task_count) ? &g_task_pool[i] : (task_t *)0;
}

/* ---- 硬实时属性访问器（阶段1；见 docs/rtos-hard-realtime-plan.md） ----
 * 供控制台 RTOSDEADLINE 命令与自测读取 TCB 的硬实时字段（私有结构不外泄）。 */
uint8_t  rtos_task_rt_class(int i)    { return (i>=0 && i<g_task_count) ? g_task_pool[i].rt_class : 0; }
uint32_t rtos_task_deadline(int i)    { return (i>=0 && i<g_task_count) ? g_task_pool[i].deadline_ticks : 0; }
uint32_t rtos_task_wcet(int i)        { return (i>=0 && i<g_task_count) ? g_task_pool[i].wcet_ticks : 0; }
uint32_t rtos_task_budget(int i)      { return (i>=0 && i<g_task_count) ? g_task_pool[i].budget_used : 0; }
uint32_t rtos_task_deadline_miss(int i){return (i>=0 && i<g_task_count) ? g_task_pool[i].deadline_miss : 0; }
uint32_t rtos_task_wcet_miss(int i)   { return (i>=0 && i<g_task_count) ? g_task_pool[i].wcet_miss : 0; }

/* ---- 优先级天花板辅助（阶段2，RTOS_LOCK_CEILING 宏用，见 rtos.h） ----
 * rtos_task_raise_prio：把当前运行任务有效优先级顶到 ceil_prio（仅当更高时），
 * 返回原优先级供恢复。rtos_task_restore_prio：若当前优先级与原优先级不同，
 * 恢复回原优先级。两者都仅在任务上下文使用（内部走 rtos_set_eff_prio，会重排
 * 就绪队列——ISR 内禁用）。用于“非 mutex 共享资源”的优先级反转防护。 */
uint8_t rtos_task_raise_prio(uint8_t ceil_prio) {
    uint8_t save = 0;
    if (!g_running) return 0;
    unsigned st = rtos_crit_enter();
    save = g_running->prio;          /* 保存“有效优先级”，使天花板可嵌套还原 */
    if (ceil_prio < g_running->prio)
        rtos_set_eff_prio(g_running, ceil_prio);
    rtos_crit_exit(st);
    return save;
}
void rtos_task_restore_prio(uint8_t save_prio) {
    if (!g_running) return;
    unsigned st = rtos_crit_enter();
    if (g_running->prio != save_prio)
        rtos_set_eff_prio(g_running, save_prio);
    rtos_crit_exit(st);
}
