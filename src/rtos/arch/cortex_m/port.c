#include "rtos.h"
#include "irq.h"            /* irq_id_t */
#include <stdint.h>

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
#include "cortex_m.h"
#include "core_cm4.h"   /* CMSIS ISA 头：SCB / NVIC / FPU / SysTick_IRQn */

/* 请求一次上下文切换：置 PENDSVSET，PendSV 在所有 ISR 退出后以最低优先级运行 */
void rtos_schedule_request(void) {
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    __DSB();
}

/* 启动调度：配置 FPU 上下文保存、把 PendSV 设为最低优先级，再用 SVC 0 切换到首个任务。 */
void rtos_arch_start(void) {
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

    /* PendSV 设为最低优先级，保证它只在“无更高优先级异常”时运行 */
    NVIC_SetPriority(PendSV_IRQn, 0xFF);

    /* 触发首次切换（SVC 从线程模式进入 Handler 模式） */
    __asm__ volatile ("svc 0" : : : "memory");
    for (;;) { }   /* 不会返回到原线程 */
}

/* 节拍中断 id：Cortex-M 的系统节拍异常。内核核心据此向 irq 框架注册 rtos_tick_isr，
 * 从而与具体 systick 外设 / 芯片 HAL 解耦。 */
irq_id_t rtos_arch_tick_id(void) {
    return (irq_id_t)SysTick_IRQn;
}

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
