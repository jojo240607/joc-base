#ifndef JOC_RTOS_ARCH_H
#define JOC_RTOS_ARCH_H

#include <stdint.h>
#include "irq.h"   /* irq_id_t */

/* ===========================================================================
 * jOS RTOS —— 移植接口（porting contract）
 *
 * 这是“可移植内核核心”与“芯片 / ISA 相关实现”之间的【唯一边界】。
 *
 * 内核核心（rtos_core.c / rtos_ipc.c / rtos_selftest.c / rtos_stress.c）
 * 只透过本文件声明的符号与底层交互：
 *   - 绝不 #include 任何芯片/厂商头文件（如 stm32f4xx.h）；
 *   - 绝不直接读写 SCB / MPU / FPU / NVIC 等寄存器；
 *   - 绝不出现 0xE000xxxx 这类 ISA 魔法地址。
 *
 * 移植到新目标 = 只改 arch/ 下的实现，核心代码一行不动：
 *   - 同 ISA 换芯片（STM32F4 -> STM32F7）：复用 arch/cortex_m/，
 *     仅调整该目录里的“移植旋钮”（内存基址、NVIC 优先级位数等）。
 *   - 换 ISA（Cortex-M -> RISC-V）：新建 arch/<isa>/ 子目录实现本契约，
 *     并提供一个覆盖启动向量表弱符号的上下文切换汇编。
 *
 * 实现文件：src/rtos/arch/cortex_m/{context.S, port.c, mpu.c}
 * ========================================================================= */

/* 启动调度：开启 FPU 惰性栈存、把 PendSV 设为最低优先级，再用 SVC 0 切到首个任务。
 * 由 rtos_start() 调用；调用后不再返回原线程。 */
void rtos_arch_start(void);

/* 请求一次上下文切换：置 PENDSVSET（PendSV 在所有 ISR 退出后以最低优先级运行）。 */
void rtos_schedule_request(void);

/* 节拍中断的“中断 id”：由 arch 决定（Cortex-M 为 SysTick_IRQn = -1）。
 * 内核核心用它向 irq 框架注册 rtos_tick_isr，从而与具体的 systick 外设彻底解耦，
 * 不直接依赖任何芯片 HAL（如 irq_hal_systick_id）。 */
irq_id_t rtos_arch_tick_id(void);

#endif /* JOC_RTOS_ARCH_H */
