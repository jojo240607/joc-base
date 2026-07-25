#include "rtos.h"
#include "stm32f4xx.h"   /* CMSIS: SCB, NVIC, FPCCR */
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * arch 层（Cortex-M4）：触发切换、配置 FPU/PendSV 优先级。
 * 实际的寄存器保存/恢复在 rtos_context.S 中完成。
 * ------------------------------------------------------------------------- */

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
