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

/* 启动调度：开启 FPU 惰性栈存（切换不必手动保存 s16-s31），
 * 把 PendSV 设为最低优先级，再用 SVC 0 切换到首个任务。 */
void rtos_arch_start(void) {
    /* 开启 FPU 惰性栈存，避免切换路径手动处理 s16-s31 */
    FPU->FPCCR |= FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk;

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
