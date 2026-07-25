#ifndef JOC_RTOS_ARCH_CORTEX_M_H
#define JOC_RTOS_ARCH_CORTEX_M_H

#include <stdint.h>

/* ===========================================================================
 * arch/cortex_m 移植旋钮（芯片相关，换芯片时只改这一处头文件）
 * ======================================================================== */
#ifndef __NVIC_PRIO_BITS
  #define __NVIC_PRIO_BITS 4U   /* STM32F4：4 位中断优先级 */
#endif
#ifndef __MPU_PRESENT
  #define __MPU_PRESENT    1U   /* STM32F4：含 ARMv7-M MPU */
#endif
#ifndef __FPU_PRESENT
  #define __FPU_PRESENT    1U   /* STM32F4：带单精度 FPU（编译开了 -mfpu=fpv4-sp-d16） */
#endif

/* Cortex-M 内核异常号（ISA 级，所有 Cortex-M 通用，与芯片无关）。
 * 厂商设备头通常定义完整的 IRQn_Type（含各外设中断号）；arch 层不再包含厂商头，
 * 这里给出内核真正需要的最小子集（PendSV / SysTick 等核心异常）。 */
#ifndef IRQn_Type
typedef enum IRQn {
    NonMaskableInt_IRQn = -14,
    HardFault_IRQn      = -13,
    SVCall_IRQn         = -5,
    PendSV_IRQn         = -2,
    SysTick_IRQn        = -1,
} IRQn_Type;
#endif

#endif /* JOC_RTOS_ARCH_CORTEX_M_H */
