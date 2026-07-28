#include "rtos.h"
#include "rtos_mpu.h"      /* g_robust_fault_active / g_robust_fault_cfsr 故障恢复钩子 */
#include "log/log.h"
#include "log/app_log.h"
#include "irq/irq.h"
#include "irq/irq_manager.h"
#include "stm32f4xx.h"      /* TIM2 / RCC / TIM2_IRQn — 中断风暴需真实定时器 ISR */
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS 鲁棒性/异常注入自测（RTOSROBUST 命令，并注册进 RTOSALL "robust" 条目）。
 * 覆盖准则 §3.1 异常注入(除零/UDF/栈溢出检测/非法return) + §3.2 中断风暴 +
 *          §3.3 嵌套锁无死锁 / 阻塞任务释放恢复 / 资源耗尽不崩。
 *
 * 说明：
 *  - 除零/UDF 通过 mpu.c 的 g_robust_fault_active 恢复分支：仅测试期置位，故障钩子
 *    跳过故障指令使任务继续、系统存活；真实故障仍走 WFI 停机（生产零回归）。
 *  - 栈溢出仅测“检测函数”（破坏栈底哨兵→rtos_stack_check_sentinel 报溢出），不做
 *    破坏性真溢出，避免踩坏相邻 CCM。
 *  - §3.3 真死锁超时 / 删阻塞任务 需未来内核 API(rtos_mutex_timedlock / rtos_task_delete)，
 *    本次以“天花板防反转(无死锁) + 阻塞任务释放恢复”正例覆盖（见 docs/rtos-test-plan.md §6）。
 * ------------------------------------------------------------------------- */

/* ===================== §3.1 除零 =====================
 * 故障恢复（mpu.c g_robust_fault_active 分支）把异常返回 PC 设为 LR，
 * 即“跳过故障指令、回到调用点”。若故障指令直接内联在任务体里，LR 是任务的返回
 * 地址(rtos_task_exit)，恢复后会直接跳过 survived/done 赋值，自测永远判 FAIL。
 * 故必须用 naked 辅助函数承载故障指令：naked 无 prologue、不压栈，异常发生时 SP
 * 即调用者(main 任务)的 SP、LR 即回到任务体的下一条指令，恢复后任务体继续往下执行，
 * 与 mpu.c 的 rtos_mpu_do_violation 同构。 */
static volatile int g_rb_div0_survived;
static volatile int g_rb_div0_done;

__attribute__((naked))
static void rb_div0_trigger(void) {
    /* 除数硬编码为 0：sdiv 触发 DIVBYZERO UsageFault。bx lr 在故障恢复(PC=LR)时被跳过。 */
    __asm volatile(
        "movs r0, #0\n"
        "movs r1, #1\n"
        "sdiv r0, r1, r0\n"   /* r0 = 1 / 0 -> DIVBYZERO */
        "bx   lr\n"
        ::: "r0", "r1", "memory"
    );
}
static void rb_div0_task(void *arg) {
    (void)arg;
    g_rb_div0_survived = 0; g_rb_div0_done = 0;
    rb_div0_trigger();        /* 触发 DIVBYZERO；故障钩子跳过 sdiv 后回到此处继续 */
    g_rb_div0_survived = 1;   /* 故障钩子跳过故障指令后，此处继续执行 */
    g_rb_div0_done = 1;
    rtos_msleep(10);
}

/* ===================== §3.1 UDF ===================== */
static volatile int g_rb_udf_survived;
static volatile int g_rb_udf_done;

__attribute__((naked))
static void rb_udf_trigger(void) {
    __asm volatile(
        "udf #0\n"            /* UNDEFINSTR UsageFault */
        "bx  lr\n"           /* 故障恢复(PC=LR)时跳过 */
        ::: "memory"
    );
}
static void rb_udf_task(void *arg) {
    (void)arg;
    g_rb_udf_survived = 0; g_rb_udf_done = 0;
    rb_udf_trigger();         /* 触发 UNDEFINSTR；恢复后回到此处继续 */
    g_rb_udf_survived = 1;
    g_rb_udf_done = 1;
    rtos_msleep(10);
}

/* ===================== §3.1 栈溢出检测（仅测检测函数） ===================== */
static int rb_stack_sentinel_check(void) {
    task_t *me = rtos_running();
    if (!me || !me->stack_base) return 0;
    rtos_stack_fill_sentinel(me);
    uint32_t *sb = (uint32_t *)me->stack_base;
    uint32_t saved = sb[0];
    sb[0] = 0xDEADBEEFu;                       /* 模拟栈底被踩 */
    int det = rtos_stack_check_sentinel(me);
    sb[0] = saved;                             /* 还原，避免误报 */
    return (det == 1);
}

/* ===================== §3.1 非法 return ===================== */
static volatile int g_rb_ret_done;
static void rb_ret_task(void *arg) {
    (void)arg;
    g_rb_ret_done = 1;     /* 函数体直接 return（无 while），应安全变 DEAD */
}

/* ===================== §3.2 中断风暴（TIM2 ~10kHz） ===================== */
#include "stm32f4xx.h"
static volatile uint32_t g_rb_storm_cnt;
static volatile uint32_t g_rb_storm_wake;
static volatile int       g_rb_storm_run;
static rtos_sem_t         g_rb_storm_sem;
/* irq_manager 回调形式：void (*)(void *ctx)，经唯一 IRQ_CommonHandler 分发。
 * 直接定义弱符号 TIM2_IRQHandler + NVIC_EnableIRQ 无效——向量表已指向
 * IRQ_CommonHandler，未注册的 TIM2 源会让分发器命中空回调/故障而冻结板子。 */
static void rb_storm_isr(void *ctx) {
    (void)ctx;
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR &= ~TIM_SR_UIF;          /* 清溢出标志，避免中断重入 */
        g_rb_storm_cnt++;
        rtos_sem_give(&g_rb_storm_sem);   /* ISR 安全：唤醒等待任务 + 请求 PendSV */
    }
}
static void rb_storm_waiter(void *arg) {
    (void)arg;
    while (g_rb_storm_run) {
        rtos_sem_wait(&g_rb_storm_sem);   /* 被 TIM2 ISR 反复唤醒 */
        g_rb_storm_wake++;
    }
}
static void rb_storm_timer_start(uint32_t hz) {
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    TIM2->CR1 = 0;
    TIM2->PSC = 83;
    TIM2->ARR = (84000000u / 84u / hz) - 1u;
    TIM2->DIER |= TIM_DIER_UIE;
    TIM2->CNT = 0; TIM2->SR = 0;
    TIM2->CR1 |= TIM_CR1_CEN;
}
static void rb_storm_timer_stop(void) {
    TIM2->CR1 &= ~TIM_CR1_CEN;
    RCC->APB1ENR &= ~RCC_APB1ENR_TIM2EN;
}

/* ===================== §3.3 嵌套锁无死锁 ===================== */
static rtos_mutex_t g_rb_m1, g_rb_m2;
static rtos_sem_t   g_rb_nest_rel;
static volatile int g_rb_nest_l_holds, g_rb_nest_h_got;
static void rb_nest_L(void *arg) {
    (void)arg;
    rtos_mutex_lock(&g_rb_m1);
    rtos_mutex_lock(&g_rb_m2);          /* 嵌套（不同互斥量，避免递归锁返回 -1） */
    g_rb_nest_l_holds = 1;
    rtos_sem_wait(&g_rb_nest_rel);      /* 阻塞，等 main 释放 */
    rtos_mutex_unlock(&g_rb_m2);
    rtos_mutex_unlock(&g_rb_m1);
    rtos_msleep(10);
}
static void rb_nest_H(void *arg) {
    (void)arg;
    rtos_mutex_lock(&g_rb_m1);          /* 等 L 释放后 handoff 拿到 */
    g_rb_nest_h_got = 1;
    rtos_mutex_unlock(&g_rb_m1);
    rtos_msleep(10);
}

/* ===================== §3.3 阻塞任务释放恢复 ===================== */
static rtos_sem_t g_rb_blk_rel;
static volatile int g_rb_blk_proceeded;
static void rb_blk_task(void *arg) {
    (void)arg;
    rtos_sem_wait(&g_rb_blk_rel);       /* 阻塞在信号量上 */
    g_rb_blk_proceeded = 1;             /* 被唤醒后继续 */
    rtos_msleep(10);
}

/* ===================== §3.3 资源耗尽不崩 ===================== */
#define RB_EX_MAX 32
/* 测试 filler 任务栈：放主 SRAM(.bss)，不占 CCM（无 DMA，纯 CPU）。 */
static uint8_t rb_ex_stack[RB_EX_MAX][256] __attribute__((aligned(256)));
static volatile int g_rb_ex_stop;
static void rb_ex_filler(void *arg) {
    (void)arg;
    while (!g_rb_ex_stop) rtos_msleep(50);
}

int rtos_robust_selftest(void) {
    int ok = 1;
    log_printf(app_log(), LOG_INFO, "rtos", "[ROBUST] self-test begin\n");

    /* ---------- §3.1 除零 ---------- */
    {
        SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;     /* 使能除零 fault */
        g_robust_fault_cfsr = 0;
        g_rb_div0_survived = 0; g_rb_div0_done = 0;
        g_robust_fault_active = 1;             /* 故障钩子进入“恢复”模式 */
        RTOS_TASK_STACK(std, 512);
        rtos_task_create("rbdiv0", rb_div0_task, (void *)0, 14, std, sizeof(std));
        uint32_t to = 0;
        while (!g_rb_div0_done && to < 1000) { rtos_msleep(2); to += 2; }
        /* DIVBYZERO = CFSR bit25 (SCB_CFSR_DIVBYZERO_Msk) */
        int lok = (g_rb_div0_survived == 1) && ((g_robust_fault_cfsr & (1u << 25u)) != 0);
        g_robust_fault_active = 0;
        SCB->CCR &= ~SCB_CCR_DIV_0_TRP_Msk;    /* 还原 */
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] div0 UsageFault recovered: survived=%d cfsr=0x%lx %s\n",
                   (int)g_rb_div0_survived, (unsigned long)g_robust_fault_cfsr,
                   lok ? "PASS" : "FAIL");
    }

    /* ---------- §3.1 UDF ---------- */
    {
        g_robust_fault_cfsr = 0;
        g_rb_udf_survived = 0; g_rb_udf_done = 0;
        g_robust_fault_active = 1;
        RTOS_TASK_STACK(stu, 512);
        rtos_task_create("rbudf", rb_udf_task, (void *)0, 14, stu, sizeof(stu));
        uint32_t to = 0;
        while (!g_rb_udf_done && to < 1000) { rtos_msleep(2); to += 2; }
        /* UNDEFINSTR = CFSR bit16 (SCB_CFSR_UNDEFINSTR_Msk) */
        int lok = (g_rb_udf_survived == 1) && ((g_robust_fault_cfsr & (1u << 16u)) != 0);
        g_robust_fault_active = 0;
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] udf UsageFault recovered: survived=%d cfsr=0x%lx %s\n",
                   (int)g_rb_udf_survived, (unsigned long)g_robust_fault_cfsr,
                   lok ? "PASS" : "FAIL");
    }

    /* ---------- §3.1 栈溢出检测 ---------- */
    {
        int lok = rb_stack_sentinel_check();
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] stack sentinel detect: %s\n", lok ? "PASS" : "FAIL");
    }

    /* ---------- §3.1 非法 return ---------- */
    {
        g_rb_ret_done = 0;
        RTOS_TASK_STACK(str, 512);
        rtos_task_create("rbret", rb_ret_task, (void *)0, 14, str, sizeof(str));
        uint32_t to = 0, tk0 = rtos_tick_count();
        while (!g_rb_ret_done && to < 1000) { rtos_msleep(2); to += 2; }
        int lok = (g_rb_ret_done == 1) && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] illegal return -> DEAD, system alive: %s\n", lok ? "PASS" : "FAIL");
    }

    /* ---------- §3.2 中断风暴 ---------- */
    {
        rtos_sem_init(&g_rb_storm_sem, 0, 1);
        g_rb_storm_cnt = 0; g_rb_storm_wake = 0; g_rb_storm_run = 1;
        RTOS_TASK_STACK(stw, 512);
        rtos_task_create("rbstormw", rb_storm_waiter, (void *)0, 6, stw, sizeof(stw));
        /* 经 irq_manager 注册 TIM2 回调（ISR 调内核 API -> IRQ_PRIO_KERNEL） */
        irq_manager_attach((irq_id_t)TIM2_IRQn, rb_storm_isr, NULL);
        irq_manager_set_priority((irq_id_t)TIM2_IRQn, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        irq_manager_enable((irq_id_t)TIM2_IRQn, rb_storm_isr, NULL);
        uint32_t tk0 = rtos_tick_count();
        rb_storm_timer_start(10000);          /* ~10kHz 溢出中断 */
        rtos_msleep(1000);                    /* 1s 风暴 */
        rb_storm_timer_stop();
        rtos_sem_give(&g_rb_storm_sem);       /* 解阻塞 waiter 使其退出 */
        g_rb_storm_run = 0;
        rtos_msleep(20);
        irq_manager_disable((irq_id_t)TIM2_IRQn, rb_storm_isr, NULL);
        irq_manager_detach((irq_id_t)TIM2_IRQn, rb_storm_isr, NULL);
        int lok = (g_rb_storm_cnt > 1000) && (g_rb_storm_wake > 1000)
                  && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] interrupt storm(10kHz,1s): isr=%lu wake=%lu alive=%d %s\n",
                   (unsigned long)g_rb_storm_cnt, (unsigned long)g_rb_storm_wake,
                   (int)(rtos_tick_count() > tk0), lok ? "PASS" : "FAIL");
    }

    /* ---------- §3.3 嵌套锁无死锁（天花板协议） ---------- */
    {
        rtos_mutex_init(&g_rb_m1, 5);
        rtos_mutex_init(&g_rb_m2, 5);
        rtos_sem_init(&g_rb_nest_rel, 0, 1);
        g_rb_nest_l_holds = 0; g_rb_nest_h_got = 0;
        RTOS_TASK_STACK(stL, 512); RTOS_TASK_STACK(stH, 512);
        rtos_task_create("rbnestL", rb_nest_L, (void *)0, 14, stL, sizeof(stL));
        uint32_t w = 0;
        while (!g_rb_nest_l_holds && w < 1000) { rtos_msleep(1); w++; }
        rtos_task_create("rbnestH", rb_nest_H, (void *)0, 6, stH, sizeof(stH));
        rtos_sem_give(&g_rb_nest_rel);        /* 释放 L -> H 经 handoff 拿锁 */
        w = 0;
        while (!g_rb_nest_h_got && w < 1500) { rtos_msleep(2); w += 2; }
        int lok = (g_rb_nest_h_got == 1);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] nested-lock no-deadlock: H_got_lock=%d %s\n",
                   (int)g_rb_nest_h_got, lok ? "PASS" : "FAIL");
        rtos_msleep(20);
    }

    /* ---------- §3.3 阻塞任务释放恢复 ---------- */
    {
        rtos_sem_init(&g_rb_blk_rel, 0, 1);
        g_rb_blk_proceeded = 0;
        RTOS_TASK_STACK(stb, 512);
        rtos_task_create("rbblk", rb_blk_task, (void *)0, 14, stb, sizeof(stb));
        rtos_msleep(20);                      /* 让任务阻塞在信号量上 */
        rtos_sem_give(&g_rb_blk_rel);         /* 唤醒 */
        uint32_t w = 0;
        while (!g_rb_blk_proceeded && w < 1000) { rtos_msleep(2); w += 2; }
        int lok = (g_rb_blk_proceeded == 1);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] blocked task resume on release: %s\n", lok ? "PASS" : "FAIL");
        rtos_msleep(20);
    }

    /* ---------- §3.3 资源耗尽不崩 ---------- */
    {
        int b = rtos_task_count();
        g_rb_ex_stop = 0;
        int made = 0;
        for (int i = 0; i < RB_EX_MAX; i++) {
            int before = rtos_task_count();
            char nm[8];
            nm[0] = 'x'; nm[1] = '0' + (char)(i / 10); nm[2] = '0' + (char)(i % 10); nm[3] = 0;
            rtos_task_create(nm, rb_ex_filler, (void *)0, 20,
                             rb_ex_stack[i], sizeof(rb_ex_stack[i]));
            int after = rtos_task_count();
            if (after > before) made++; else break;
        }
        int limit = rtos_task_count();
        uint32_t tk0 = rtos_tick_count();
        rtos_msleep(100);                     /* 池满后仍正常运行 */
        int lok = (limit <= RTOS_MAX_TASKS) && (rtos_tick_count() > tk0);
        if (!lok) ok = 0;
        log_printf(app_log(), LOG_INFO, "rtos",
                   "[ROBUST] resource exhaustion(base=%d made=%d limit=%d) system alive: %s\n",
                   b, made, limit, lok ? "PASS" : "FAIL");
        g_rb_ex_stop = 1; rtos_msleep(100);   /* 释放 filler */
    }

    log_printf(app_log(), LOG_INFO, "rtos", "[ROBUST] self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
RTOS_SELFTEST_ADD("robust", rtos_robust_selftest);
