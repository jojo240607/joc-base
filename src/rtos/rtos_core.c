#include "rtos.h"
#include "common/lock.h"
#include "irq.h"
#include "bh.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 内核核心：TCB 池、就绪位图、睡眠链表、等待队列、调度切换、节拍。
 *
 * 所有链表修改都假定调用方已关中断（PRIMASK）或处于 PendSV（cpsid i），
 * 因此本文件内不另加锁；对外 API（rtos_yield/rtos_msleep/rtos_pend/post）
 * 在调用修改链表的逻辑前自行 irq_lock。
 * ------------------------------------------------------------------------- */

/* 前向声明：任务入口返回时调用，标记 TASK_DEAD 并请求重新调度 */
static void rtos_task_exit(void);

#define PRIO_LEVELS RTOS_MAX_PRIORITIES

/* ---- 全局状态 ---- */
task_t *g_running = (task_t *)0;
volatile uint32_t g_tick = 0;
int g_rtos_started = 0;
volatile int g_in_svc = 0;   /* SVC 分发进行中：防止非特权任务路径递归触发 SVC */

/* ---- 就绪队列：每优先级 FIFO + 位图 ---- */
static task_t *g_ready_head[PRIO_LEVELS];
static task_t *g_ready_tail[PRIO_LEVELS];
static uint32_t g_ready_bmp;

/* ---- 睡眠链表（按 tick 递减） ---- */
static task_t *g_sleep_head;

/* ---- TCB 静态池（无堆，确定性） ---- */
static task_t g_task_pool[RTOS_MAX_TASKS];
static int g_task_count = 0;

/* ---- 就绪链表操作（调用方持锁） ---- */
void ready_add(task_t *t) {
    int p = t->prio;
    t->sched_prev = g_ready_tail[p];
    t->sched_next = (task_t *)0;
    if (g_ready_tail[p]) g_ready_tail[p]->sched_next = t;
    else                 g_ready_head[p] = t;
    g_ready_tail[p] = t;
    g_ready_bmp |= (1u << p);
}
static void ready_remove(task_t *t) {
    int p = t->prio;
    if (t->sched_prev) t->sched_prev->sched_next = t->sched_next;
    else               g_ready_head[p] = t->sched_next;
    if (t->sched_next) t->sched_next->sched_prev = t->sched_prev;
    else               g_ready_tail[p] = t->sched_prev;
    if (!g_ready_head[p]) g_ready_bmp &= ~(1u << p);
    t->sched_next = t->sched_prev = (task_t *)0;
}
static task_t *ready_pick(void) {
    if (!g_ready_bmp) return (task_t *)0;
    int p = __builtin_ctz(g_ready_bmp);   /* 最低置位 = 最高优先级 */
    return g_ready_head[p];
}

/* ---- 睡眠链表操作（调用方持锁） ---- */
static void sleep_add(task_t *t) {
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

/* ---- 初始栈帧：伪造一次异常入栈，使首次切换的“恢复”路径合法 ---- */
static void task_stack_init(task_t *t) {
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
}

static void rtos_task_exit(void) {
    unsigned st = irq_lock();
    if (g_running) g_running->state = TASK_DEAD;
    irq_unlock(st);
    /* 非特权任务返回时也需请求切换，但 rtos_schedule_request() 直接写 ICSR
     * (仅特权)，会导致 BusFault。改用 rtos_yield()：它在非特权态会经 SVC 门
     * (RTOS_SYS_YIELD) 在特权 Handler 模式里真正请求切换；特权任务则直连，
     * 行为与历史完全一致。任务已是 TASK_DEAD，rtos_yield 不会把它重新入就绪队列。 */
    rtos_yield();
    for (;;) { }
}

/* ---- 生命周期 ---- */
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
    /* 在 systick 共享线上注册 RTOS 节拍（与 systick 驱动 ISR 并存）。
     * 节拍中断 id 由 arch 层给出，核心不直接依赖任何芯片 HAL。 */
    irq_register(rtos_arch_tick_id(), rtos_tick_isr, (void *)0);
}

void rtos_task_create(const char *name, void (*entry)(void *), void *arg,
                      uint8_t prio, void *stack, size_t stack_size) {
    /* 默认创建特权任务（与历史行为一致，对现有 BIST/自测零回归）。 */
    rtos_task_create_ex(name, entry, arg, prio, stack, stack_size, 1);
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
    if (prio >= PRIO_LEVELS) prio = (uint8_t)(PRIO_LEVELS - 1);
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
    task_stack_init(t);
    rtos_stack_fill_sentinel(t);   /* 填栈底魔数，供切换时检测溢出 */
    ready_add(t);
    if (name) rtos_kobj_register(name, KOBJ_TASK, t);   /* 任务注册进内核对象表（按名可取） */
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
        rtos_task_create(p->name, p->entry, p->arg, p->prio, p->stack, p->stack_size);
    for (const rtos_mq_def_t *p = __rtos_ipc_start; p < __rtos_ipc_end; p++)
        rtos_mq_init(p->mq, p->buf, p->item_size, p->cap);
    for (const rtos_bh_def_t *p = __rtos_bh_start; p < __rtos_bh_end; p++)
        rtos_bh_task_create(p->name, p->prio, p->stack, p->stack_size, p->fn, p->ctx);
}

void rtos_start(void) {
    rtos_cycle_init();           /* 使能 DWT 周期计数器（P4 延迟/有界性测量用） */
    rtos_instantiate_sections(); /* 编译期段收集：自动建任务/IPC/BH（P4） */
    task_t *first = ready_pick();
    if (!first) return;
    ready_remove(first);
    first->state = TASK_RUNNING;
    g_running = first;
    g_rtos_started = 1;
#if RTOS_USE_MPU
    rtos_mpu_init();     /* 配置固定区域并使能 MPU + MemManage（对特权任务透明） */
#endif
    rtos_arch_start();   /* 触发 SVC；切换后不再返回原线程 */
}

/* 当前任务是否需走 SVC 门：非特权 + 非 ISR + 非 SVC 重入。
 * 特权任务（含 ISR、内核启动前）一律直连内核，路径与历史完全一致。 */
int rtos_need_svc(void) {
    return !arch_in_isr() && !g_in_svc && g_running && !g_running->priv;
}

void rtos_yield(void) {
    if (!g_rtos_started) return;
    if (rtos_need_svc()) { rtos_syscall(RTOS_SYS_YIELD, 0, 0, 0); return; }
    unsigned st = irq_lock();
    if (g_running && g_running->state == TASK_RUNNING) {
        g_running->state = TASK_READY;
        ready_add(g_running);
    }
    irq_unlock(st);
    rtos_schedule_request();
}

void rtos_msleep(uint32_t ms) {
    if (!g_rtos_started) { for (volatile uint32_t i = 0; i < ms * 1000UL; i++) { } return; }
    if (rtos_need_svc()) { rtos_syscall(RTOS_SYS_MSLEEP, ms, 0, 0); return; }
    uint32_t ticks = (ms * RTOS_TICK_HZ + 999U) / 1000U;
    if (ticks == 0) ticks = 1;
    unsigned st = irq_lock();
    g_running->state = TASK_SLEEPING;
    g_running->delay_ticks = ticks;
    sleep_add(g_running);
    irq_unlock(st);
    rtos_schedule_request();
}

/* 由 PendSV 汇编调用（中断已关）：保存 old_sp，挑选下一任务，返回其 sp */
void *rtos_pendsv_switch(void *old_sp) {
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
    }
    return nxt ? nxt->sp : old_sp;
}

/* 由 systick 共享线调用（可能运行在 ISR 上下文） */
void rtos_tick_isr(void *ctx) {
    (void)ctx;
    if (!g_rtos_started) return;
    unsigned st = irq_lock();
    g_tick++;
    int awoke = 0;
    task_t *t = g_sleep_head;
    while (t) {
        task_t *nx = t->sched_next;
        if (t->delay_ticks > 0) {
            if (--t->delay_ticks == 0) {
                sleep_remove(t);
                t->state = TASK_READY;
                ready_add(t);
                awoke = 1;
            }
        }
        t = nx;
    }
    irq_unlock(st);
    if (awoke) rtos_schedule_request();
}

/* ===========================================================================
 * SVC 系统调用分发（非特权任务访问内核对象的唯一受控入口）
 *
 * context.S 的 SVC_Handler 在“非启动类 SVC”时以 `b rtos_svc_dispatch_entry`
 * 尾调用进入本函数（lr 仍为 EXC_RETURN）。frame 是触发 SVC 的任务栈帧(PSP)，
 * frame[0..3] = r0..r3 = [调用号, 参数0, 参数1, 参数2]。运行在特权 Handler 模式，
 * 可直接操作内核对象；处理完把返回值写回 frame[0]，随 `bx lr` 带回用户态任务。
 * 全程 g_in_svc=1，故内部再调用的 rtos_sem_ 与 rtos_yield 等不会递归触发 SVC。
 * ========================================================================= */
void rtos_svc_dispatch(uint32_t *frame, uint32_t nr) {
    uint32_t u0 = frame[1], u1 = frame[2], u2 = frame[3];   /* 用户参数(调用号在 frame[0]) */
    uint32_t ret = 0;
    switch (nr) {
    case RTOS_SYS_GET_TICK:  ret = rtos_tick_count(); break;
    case RTOS_SYS_YIELD:     rtos_yield(); break;
    case RTOS_SYS_MSLEEP:    rtos_msleep(u0); break;
    case RTOS_SYS_SEM_GIVE:
        if (rtos_kobj_validate((void *)u0, KOBJ_SEM)) rtos_sem_give((rtos_sem_t *)u0);
        else ret = (uint32_t)-1;
        break;
    case RTOS_SYS_SEM_WAIT:
        ret = rtos_kobj_validate((void *)u0, KOBJ_SEM)
              ? (uint32_t)rtos_sem_wait((rtos_sem_t *)u0) : (uint32_t)-1;
        break;
    case RTOS_SYS_MUTEX_LOCK:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MUTEX)
              ? (uint32_t)rtos_mutex_lock((rtos_mutex_t *)u0) : (uint32_t)-1;
        break;
    case RTOS_SYS_MUTEX_UNLOCK:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MUTEX)
              ? (uint32_t)rtos_mutex_unlock((rtos_mutex_t *)u0) : (uint32_t)-1;
        break;
    case RTOS_SYS_MUTEX_TRYLOCK:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MUTEX)
              ? (uint32_t)rtos_mutex_trylock((rtos_mutex_t *)u0) : (uint32_t)-1;
        break;
    case RTOS_SYS_MQ_SEND:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MQ)
              ? (uint32_t)rtos_mq_send((rtos_mq_t *)u0, (const void *)u1) : (uint32_t)-1;
        break;
    case RTOS_SYS_MQ_RECV:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MQ)
              ? (uint32_t)rtos_mq_recv((rtos_mq_t *)u0, (void *)u1) : (uint32_t)-1;
        break;
    case RTOS_SYS_EVENT_SET:
        if (rtos_kobj_validate((void *)u0, KOBJ_EVENT)) rtos_event_set((rtos_event_t *)u0, u1);
        else ret = (uint32_t)-1;
        break;
    case RTOS_SYS_EVENT_WAIT: {
        int wait_all = (int)(u2 & 1u), block = (int)((u2 >> 1) & 1u);
        ret = rtos_kobj_validate((void *)u0, KOBJ_EVENT)
              ? rtos_event_wait((rtos_event_t *)u0, u1, wait_all, block) : (uint32_t)-1;
        break;
    }
    case RTOS_SYS_TASK_CREATE: {
        rtos_task_create_args_t *p = (rtos_task_create_args_t *)u0;
        rtos_task_create_ex(p->name, p->entry, p->arg, p->prio,
                            p->stack, p->stack_size, p->priv);
        break;
    }
    default: ret = (uint32_t)-1; break;
    }
    frame[0] = ret;   /* 返回值经 r0 带回用户态任务 */
}

/* 由汇编尾调用：置重入标志后分发，确保内部调用不递归 SVC。 */
void rtos_svc_dispatch_entry(uint32_t *frame, uint32_t nr) {
    g_in_svc = 1;
    rtos_svc_dispatch(frame, nr);
    g_in_svc = 0;
}

void rtos_pend(void **q) {
    if (!g_running) return;
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
