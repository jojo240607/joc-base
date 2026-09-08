#include "rtos.h"
#include "rtos_mpu.h"       /* rtos_mpu_set_task_stack_region：每任务栈 region(R4) */
#include "irq.h"            /* irq_id_t */
#include <stdint.h>

/* 调度器启动标志（core/sched.c 定义）：rtos_schedule_request 据此判定能否请求
 * PendSV（避免 PSP 未就绪时切换导致 boot HardFault）。arch 层仅读，不写。 */
extern int g_rtos_started;
/* PSP 是否已切到首个任务栈：SVC_Handler 首切路径里置 1。仅当其为真时才能置
 * PENDSVSET（首切前 PSP=0，置位会破坏内存/HardFault）。arch 层仅读，不写。 */
extern volatile int g_rtos_psp_ready;

/* ---------------------------------------------------------------------------
 * arch 层（Cortex-M4）：触发切换、配置 FPU/PendSV 优先级、提供节拍中断 id。
 *
 * 本文件是内核核心与底层硬件之间的隔离层。它只依赖 ARMv7-M 的 ISA 头
 * core_cm4.h（提供 SCB / NVIC / FPU / SysTick_IRQn），绝不包含任何厂商
 * 芯片头（如 stm32f4xx.h）。真正的寄存器保存/恢复在 context.S 中完成。
 *
 * 【移植旋钮】换芯片时若 NVIC 优先级位数不同，改下面的 __NVIC_PRIO_BITS。
 * ------------------------------------------------------------------------- */

/* 移植旋钮 + ISA 级最小 IRQn 定义（见 cortex_m.h 注释；换芯片只改该头） */
#include "cortex_m.h"   /* CMSIS ISA 头：由 cortex_m.h 统一引入 core_cm*.h */

/* 请求一次上下文切换：置 PENDSVSET，PendSV 在所有 ISR 退出后以最低优先级运行。
 *
 * 关键约束（boot HardFault 根因修复）：PendSV 切换依赖【当前线程已切到 PSP】。
 * 在 rtos_arch_start 的 svc 0 完成“首个任务 → PSP”切换之前，PSP 仍是复位默认值
 * 0（main 启动线程用 MSP，CONTROL.SPSEL=0）。若此时任何 IRQ（如已启动的 1 kHz
 * SysTick）在 svc 之前 firing 并请求调度，PendSV 会用 PSP=0 去做上下文保存，把帧
 * 写到地址 0 附近、破坏内存，并因恢复出垃圾返回地址而 HardFault（CFSR=IBUSERR、
 * fault PC=0)。因此这里要求【被中断的上下文已使用 PSP（CONTROL.SPSEL=1）】才允许
 * 置 PENDSVSET；启动线程(main, MSP)被中断时一律退回，等 svc 切到任务(PSP 就绪)后
 * 才真正请求切换。这与 FreeRTOS 的等价契约一致。 */
void rtos_schedule_request(void) {
    if (!g_rtos_started) return;                 /* 调度器未启动：不请求 */
    /* 仅在【PSP 已就绪（首个任务已切到 PSP）】时才允许置 PENDSVSET。
     * 关键：不能改用“CONTROL.SPSEL==0 就退回”来判定——ISR(Hanlder 模式)里
     * SPSEL 恒为 0，会因此误杀所有 ISR 驱动的调度请求（SysTick 唤醒、ISR 内
     * sem_give/BH 触发等），导致内核卡死。首切之前 PSP=0（启动线程用 MSP），
     * 此时 g_rtos_psp_ready 仍为 0，退回；首切之后，无论线程模式(PSP,SPSEL=1)
     * 还是 ISR 模式(MSP,SPSEL=0)，只要 PSP 指向有效任务栈，都应允许请求切换。 */
    if (!g_rtos_psp_ready) return;               /* 首切前：PSP 未就绪，不能置 PendSV */
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    __DSB();
}

/* 启动调度：配置 FPU 上下文保存（如有）、把 PendSV 设为最低优先级，再用 SVC 0 切换到首个任务。 */
void rtos_arch_start(void) {
#if __FPU_PRESENT
    /* FPU 上下文保存配置（Cortex-M4 标准做法，与 FreeRTOS 一致）：
     *   ASPEN=1  开启“自动 FPU 状态保存”——异常进出时硬件保存/恢复 S0-S15+FPSCR；
     *   LSPEN=1  保留“懒栈”——仅当 Handler 里首次用到 FPU 才真正把 S0-S15+FPSCR
     *             落栈（避免每个异常都无谓压 0x48 字节）。
     * 采用懒栈是必须的：context.S 在 PendSV 里手动保存 s16-s31 时，第一条就是 vstmdb，
     * 这条 VFP 指令会触发懒栈把 S0-S15+FPSCR flush 到任务栈帧，使整段 FPU 上下文
     * 完整落在任务栈上；若关闭懒栈(ASPEN 仍开)在某些嵌套/抢占组合下会与手工保存产生
     * 帧布局错位（表现为返回时 EXC_RETURN 错乱、INVSTATE）。S0-S15/FPSCR 由硬件在异常
     * 返回时自动取回，s16-s31 由 context.S 手动保存/恢复（硬件从不保存它们）。 */
    FPU->FPCCR = (FPU->FPCCR & ~(uint32_t)FPU_FPCCR_ASPEN_Msk)
                              |  (uint32_t)FPU_FPCCR_ASPEN_Msk
                              |  (uint32_t)FPU_FPCCR_LSPEN_Msk;
#endif

    /* PendSV / SysTick 置【最低硬件优先级】(4-bit 时为 15)：这是 FreeRTOS 的
     * configKERNEL_INTERRUPT_PRIORITY 约定——内核节拍与切换异常必须可被 BASEPRI
     * 临界区屏蔽（priority 15 >= 阈值）。NVIC_SetPriority 接收【未移位】的优先级
     * 数(0..15)，内部自行左移 (8-__NVIC_PRIO_BITS)；切勿再手动移位（否则二次移位
     * 变成优先级 0，反而永不被 BASEPRI 屏蔽，会与临界区并发改写就绪/等待链表而出错）。 */
    uint32_t lowest = (uint32_t)((1U << __NVIC_PRIO_BITS) - 1U);  /* 4-bit -> 15 */
    NVIC_SetPriority(PendSV_IRQn, lowest);

#if RTOS_MAX_ZERO_LATENCY_IRQS > 0
    /* 零延迟 IRQ（docs/rtos-design.md §4.5）：节拍置于最低优先级，故它落在
     * “可被内核 BASEPRI 屏蔽”的带内（priority 15 >= 阈值 4），而优先级 < 阈值(=4)
     * 的零延迟 ISR 永不被屏蔽。前提：所有调用内核 API 的 ISR 优先级必须 >= 阈值
     *（FreeRTOS 同款契约），rtos_start 的 irq_manager_audit_priorities 会校验。 */
    NVIC_SetPriority(SysTick_IRQn, lowest);
#endif

    /* 触发首次切换（SVC 从线程模式进入 Handler 模式） */
    __asm__ volatile ("svc 0" : : : "memory");
    for (;;) { }   /* 不会返回到原线程 */
}

/* 节拍中断 id：Cortex-M 的系统节拍异常。内核核心据此向 irq 框架注册 rtos_tick_isr，
 * 从而与具体 systick 外设 / 芯片 HAL 解耦。 */
irq_id_t rtos_arch_tick_id(void) {
    return (irq_id_t)SysTick_IRQn;
}

/* 启动节拍时钟源：直接使能 SysTick 硬件，按 RTOS_TICK_HZ 频率产生节拍中断。
 *
 * 这是【修复 PING 无响应】的关键：内核核心 rtos_init 仅向 irq 框架注册了
 * rtos_tick_isr 回调，但从不启动时钟本身；而原先指望板级 systick 节点经
 * SysTick_Config 去使能 SysTick，但该节点从未出现在 g_nodes[]、且其使能路径
 * 依赖 clock_hal_sysclk_hz() 等外部状态，任一环节失效都会让 SysTick 不运行、
 * g_tick 恒为 0、所有 rtos_msleep 永久阻塞（main 任务睡死在 console_run 的
 * rtos_msleep(1)），从而永不读 uart、RXNE 不清除、PING 无响应。故节拍时钟源
 * 的启动必须由 arch 层在此【无条件】显式保证，与“是否注册了 systick 外设节点”
 * 及“clock 全局是否就绪”彻底解耦。
 *
 * 实现直接写 SysTick 寄存器（core_cm4.h 的 SysTick_Type），不依赖 SysTick_Config
 * 内联函数（某些 CMSIS 包含树里它是 static inline，跨 TU 链接会 undefined），也
 * 不依赖 SystemCoreClock 全局——节拍频率取自 RTOS_TICK_HZ，CPU 频率取 arch 已知
 * 的 STM32F4 设计常量（与 clock_hal 配置一致，见 stm32f4_discovery.c）。 */
#include "rtos_config.h"   /* RTOS_TICK_HZ */
void rtos_arch_tick_start(void) {
    /* STM32F407 核心时钟（HCLK）由板级 clock 驱动锁定为 168 MHz；此处用设计常量，
     * 避免依赖运行时全局（SystemCoreClock 的 .data 初值为 16M，需 clock_hal 改写）。 */
    const uint32_t cpu_hz = (uint32_t)RTOS_CPU_HZ;
    const uint32_t ticks   = cpu_hz / (uint32_t)RTOS_TICK_HZ;

    SysTick->LOAD = (ticks & SysTick_LOAD_RELOAD_Msk) - 1UL;  /* 节拍周期 */
    SysTick->VAL  = 0UL;                                       /* 清当前值与标志 */
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |               /* 内核时钟 (HCLK) */
                    SysTick_CTRL_TICKINT_Msk  |               /* 使能节拍中断 */
                    SysTick_CTRL_ENABLE_Msk;                  /* 启动计数器 */
    /* 优先级随后由 rtos_arch_start 在 RTOS_MAX_ZERO_LATENCY_IRQS>0 分支统一设为最低，
     * 确保 SysTick 可被 BASEPRI 临界区屏蔽（与 FreeRTOS configKERNEL_INTERRUPT_PRIORITY 一致）。 */
}

#if __DWT_PRESENT
/* 使能 DWT 周期计数器（供 P4 收尾自测测量调度延迟 / 上半部有界性）。
 * DWT 属 ARMv7-M ISA 特性，仅在 arch 层访问；可移植核心经 rtos_cycle_now() 只读计数。
 * 幂等：已使能则直接返回，可安全地从 rtos_start() 与自测里多次调用。 */
void rtos_cycle_init(void) {
    if (!(DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk)) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  /* 打开调试/跟踪矩阵时钟 */
        DWT->CYCCNT = 0;
        DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;             /* 开始计数 */
    }
}
uint32_t rtos_cycle_now(void) {
    return DWT->CYCCNT;
}
#else
/* DWT 未使能或不可用：返回 0（临界区审计的 held=0 <= 阈值，不影响功能） */
void   rtos_cycle_init(void) {}
uint32_t rtos_cycle_now(void) { return 0; }
#endif

/* 按当前任务(priv 标志)设置 CONTROL.nPRIV。必须在 Handler 模式(PendSV/SVC)里调用，
 * 异常返回到线程模式时即按新 nPRIV 运行：特权任务 nPRIV=0，非特权任务 nPRIV=1。
 * 常态任务 priv=1，故对现行系统零影响；仅非特权任务(rtos_task_create_ex(...,0))会降权。
 * 切换后顺便把每任务栈 region(R4)重编程到新任务栈（见 docs/rtos-design.md §6 R3）。 */
extern task_t *g_running;
void rtos_arch_apply_task_priv(void) {
    uint32_t c = __get_CONTROL();
    if (g_running && !g_running->priv) c |=  (uint32_t)0x1u;   /* 非特权 */
    else                                c &= ~(uint32_t)0x1u;   /* 特权 */
    /* 关键：每次切换都【清零 FPCA(CONTROL[2])】。硬浮点 ABI(-mfloat-abi=hard)下编译器
     * 几乎在每个函数里都发射 VFP 指令，任务频繁置位 FPCA；若切走时不清，FPCA 会泄漏
     * 到下一个任务，使其被异常抢占时硬件据 FPCA=1 压扩展帧，但已保存帧是基本帧，
     * 导致 SAVE/RESTORE 帧布局错位、PSP 偏 0x40、恢复出垃圾 PC -> IBUSERR。
     * 清零后 FPCA 由硬件按新任务实际是否用 FPU 重新派生，帧类型始终与每任务帧一致。 */
    c &= ~(uint32_t)0x4u;   /* FPCA = 0 */
    __set_CONTROL(c);
    __ISB();
#if RTOS_MPU_PER_TASK_STACK && __MPU_PRESENT
    rtos_mpu_set_task_stack_region(g_running);   /* R4 = 新任务栈(unpriv RW + XN) */
#endif
}

/* 当前是否运行在非特权态（仅用于自测断言）。 */
int rtos_arch_in_unpriv(void) {
    return (__get_CONTROL() & 0x1u) ? 1 : 0;
}

/* SVC 系统调用门：非特权任务经此切换到 Handler 模式执行内核 API。
 * 所有 Cortex-M (M3/M4/M7) 均支持 SVC 指令，故本函数无条件编译。
 * 当 RTOS_USE_MPU=0 时所有任务均为特权，rtos_need_svc() 返回 0，
 * 运行时不会走此路径，但链接器仍需符号解析。 */
__attribute__((naked))
uint32_t rtos_syscall(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2) {
    __asm volatile ("svc 0x80\n bx lr" : : : "memory", "r0", "r1", "r2", "r3");
}