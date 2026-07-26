#include "rtos.h"
#include "rtos_internal.h"
#include "common/lock.h"
#include "irq.h"
#include "bh.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 任务管理（core/task.c）：TCB 静态池、初始栈帧、任务创建/退出、
 * 编译期段收集实例化（rtos_instantiate_sections）、rtos_start、查询 API。
 *
 * 就绪队列/睡眠/切换见 core/sched.c；SVC 分发见 core/syscalls.c。
 * ------------------------------------------------------------------------- */

/* ---- TCB 静态池（无堆，确定性） ---- */
static task_t g_task_pool[RTOS_MAX_TASKS];

/* 前向声明：任务入口返回时调用，标记 TASK_DEAD 并请求重新调度 */
static void rtos_task_exit(void);

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
    unsigned st = rtos_crit_enter();
    if (g_running) g_running->state = TASK_DEAD;
    rtos_crit_exit(st);
    /* 非特权任务返回时也需请求切换，但 rtos_schedule_request() 直接写 ICSR
     * (仅特权)，会导致 BusFault。改用 rtos_yield()：它在非特权态会经 SVC 门
     * (RTOS_SYS_YIELD) 在特权 Handler 模式里真正请求切换；特权任务则直连，
     * 行为与历史完全一致。任务已是 TASK_DEAD，rtos_yield 不会把它重新入就绪队列。 */
    rtos_yield();
    for (;;) { }
}

/* ---- 生命周期 ---- */
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
