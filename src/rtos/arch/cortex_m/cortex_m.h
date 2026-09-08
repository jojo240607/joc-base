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
  #define __MPU_PRESENT    0U   /* 默认无 MPU；STM32F4 覆盖为 1 */
#endif
#ifndef __FPU_PRESENT
  #define __FPU_PRESENT    0U   /* 默认无 FPU；STM32F4 覆盖为 1 */
#endif
/* DWT 周期计数器：ARMv7-M (M3/M4/M7) 有可选 DWT，ARMv8-M/M0 无。
 * 移植到有 DWT 的芯片（如 STM32F4）时覆盖为 1。 */
#ifndef __DWT_PRESENT
  #define __DWT_PRESENT    0U
#endif

/* ---- CMSIS Core 头文件选择 ---- */
/* port.c 和 mpu.c 原先各自 #include "core_cm4.h"；现在改由本头统一调配，
 * 换 ISA 只需在编译时定义 CORE_CORTEX_Mx（CMakeLists.txt 中的 JOC_TARGET 完成）。
 *
 * 注意：CORE_HEADER 的 #include 放在 IRQn_Type 定义之后 */
#ifndef CORE_HEADER
#if defined(CORE_CORTEX_M3)
  #define CORE_HEADER "core_cm3.h"
#elif defined(CORE_CORTEX_M4)
  #define CORE_HEADER "core_cm4.h"
#elif defined(CORE_CORTEX_M7)
  #define CORE_HEADER "core_cm7.h"
#elif defined(CORE_CORTEX_M0) || defined(CORE_CORTEX_M0PLUS)
  #define CORE_HEADER "core_cm0.h"
#else
  #error "Define CORE_CORTEX_Mx (-DCORE_CORTEX_M3 etc.) in CMakeLists"
#endif
#endif

/* Cortex-M 内核异常号（ISA 级，所有 Cortex-M 通用，与芯片无关）。
 * 厂商设备头通常定义完整的 IRQn_Type（含各外设中断号）；arch 层不再包含厂商头，
 * 这里给出内核真正需要的最小子集（PendSV / SysTick 等核心异常）。
 *
 * 注意：此定义必须位于 #include CORE_HEADER 之前，因为 core_cm3.h/4.h 的
 * 内联 NVIC 函数（__NVIC_EnableIRQ 等）使用了 IRQn_Type 类型。 */
#ifndef IRQn_Type
typedef enum IRQn {
    NonMaskableInt_IRQn = -14,
    HardFault_IRQn      = -13,
    SVCall_IRQn         = -5,
    PendSV_IRQn         = -2,
    SysTick_IRQn        = -1,
} IRQn_Type;
#endif

#include CORE_HEADER

#endif /* JOC_RTOS_ARCH_CORTEX_M_H */
