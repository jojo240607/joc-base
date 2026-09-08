/**
  ******************************************************************************
  * @file    system_stm32h7xx.c
  * @author  jOS port (based on MCD Application Team H7 template)
  * @brief   CMSIS Cortex-M7 Device Peripheral Access Layer System Source File.
  *
  *   This file provides two functions and one global variable to be called from
  *   user application:
  *      - SystemInit(): This function is called at startup just after reset and
  *                      before branch to main program. This call is made inside
  *                      the "startup_stm32h750xx.s" file.
  *
  *      - SystemCoreClock variable: Contains the core clock (HCLK), it can be used
  *                                  by the user application to setup the SysTick
  *                                  timer or configure other parameters.
  *
  *      - SystemCoreClockUpdate(): Updates the variable SystemCoreClock and must
  *                                 be called whenever the core clock is changed
  *                                 during program execution.
  *
  *   jOS 里程碑 1 最小实现：仅 FPU / I-Cache / VTOR / NVIC 分组。
  *   PLL@480MHz 的完整时钟配置由 main() 的 clock_hal_configure() 完成
  *   （SystemInit 在 .data 拷贝之前执行，故不在此写 SystemCoreClock）。
  *   D-Cache 里程碑 2 使能（DMA 一致性通过 MPU SRAM1 非缓存堆保障）。
  *
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2017 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

#include "stm32h7xx.h"

#if !defined  (HSE_VALUE)
  #define HSE_VALUE    ((uint32_t)25000000U)  /*!< H750B-DK 板载 25 MHz 晶振 */
#endif /* HSE_VALUE */

#if !defined  (HSI_VALUE)
  #define HSI_VALUE    ((uint32_t)64000000U)  /*!< H7 内部 HSI 64 MHz */
#endif /* HSI_VALUE */

#if !defined  (CSI_VALUE)
  #define CSI_VALUE    ((uint32_t)4000000U)   /*!< H7 内部 CSI 4 MHz */
#endif /* CSI_VALUE */

/*!< Uncomment the following line if you need to relocate the vector table
     anywhere in Flash or SRAM, else it will be located at the base address
     of the Flash. */
/* #define USER_VECT_TAB_ADDRESS */

#if defined(USER_VECT_TAB_ADDRESS)
/*!< Uncomment the following line if you need to relocate your vector Table
     in SRAM else user remap will be done in Flash. */
/* #define VECT_TAB_SRAM */
#if defined(VECT_TAB_SRAM)
#define VECT_TAB_BASE_ADDRESS   SRAM_BASE       /*!< Vector Table base address field.
                                                     This value must be a multiple of 0x200. */
#else
#define VECT_TAB_BASE_ADDRESS   FLASH_BASE      /*!< Vector Table base address field.
                                                     This value must be a multiple of 0x200. */
#endif /* VECT_TAB_SRAM */
#if !defined(VECT_TAB_OFFSET)
#define VECT_TAB_OFFSET         0x00000000U     /*!< Vector Table offset field.
                                                     This value must be a multiple of 0x200. */
#endif /* VECT_TAB_OFFSET */
#endif /* USER_VECT_TAB_ADDRESS */

/**
  * @brief  System Clock Frequency (Core Clock)
  *         复位值 = HSI 64 MHz；clock_hal_configure() 切到 PLL@480MHz 后由
  *         clock_hal 直接写 RTOS_CPU_HZ。
  */
uint32_t SystemCoreClock = 64000000U;

/* 官方 H7 模板的 D1CPRE/HPRE 预分频映射表（索引 = 寄存器 4bit 值） */
const uint8_t D1CorePrescTable[16] = {0, 0, 0, 0, 1, 2, 3, 4, 1, 2, 3, 4, 6, 7, 8, 9};

/**
  * @brief  Setup the microcontroller system
  *         Initialize the FPU setting and vector table location
  *         configuration.
  * @param  None
  * @retval None
  */
void SystemInit(void)
{
  /* FPU settings ------------------------------------------------------------*/
  #if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
    SCB->CPACR |= ((3UL << (10*2))|(3UL << (11*2)));  /* set CP10 and CP11 Full Access */
    /* 浮点状态自动惰性保存（FPCCR.ASPEN/LSPEN）复位默认即为 1，本 CMSIS 版本
     * SCB_Type 未暴露 FPCCR 成员；此处依赖复位值：仅当任务实际用过 FPU 时
     * 才在异常入口压 s16-s31。 */
  #endif

  /* I/D-Cache：H7 上 I/D-Cache 硬件恒使能，SCB_Enable* 仅执行失效 + 使能。
   * D-Cache 里程碑 2 使能，DMA 一致性通过 MPU 将 SRAM1 堆设为非缓存保障。 */
  SCB_EnableICache();
  SCB_EnableDCache();

  /* Configure the Vector Table location -------------------------------------*/
#if defined(USER_VECT_TAB_ADDRESS)
  SCB->VTOR = VECT_TAB_BASE_ADDRESS | VECT_TAB_OFFSET; /* Vector Table Relocation in Internal Flash or SRAM */
#else
  SCB->VTOR = FLASH_BASE; /* Default Vector Table Location at the base address of Internal Flash */
#endif /* USER_VECT_TAB_ADDRESS */

  /* NVIC 优先级分组：PRIGROUP=3 → 4 位全抢占、0 子优先级（FreeRTOS 式，
   * 对应 CMSIS 的 NVIC_PRIORITYGROUP_4）。在 main() 之前设定，避免启动早期
   * 出现非预期的中断嵌套。 */
  NVIC_SetPriorityGrouping(0x3UL);
}

/**
  * @brief  Update SystemCoreClock variable according to Clock Register Values.
  *         The SystemCoreClock variable contains the core clock (HCLK), it can
  *         be used by the user application to setup the SysTick timer or configure
  *         other parameters.
  *
  * @note   - The system frequency computed by this function is not the real
  *           frequency in the chip. It is calculated based on the predefined
  *           constant and the selected clock source:
  *
  *           - If SYSCLK source is HSI, SystemCoreClock will contain the HSI_VALUE
  *           - If SYSCLK source is HSE, SystemCoreClock will contain the HSE_VALUE
  *           - If SYSCLK source is PLL, SystemCoreClock will contain the HSE_VALUE
  *             or HSI_VALUE multiplied/divided by the PLL factors.
  *
  * @param  None
  * @retval None
  */
void SystemCoreClockUpdate(void)
{
  uint32_t tmp;
  uint32_t pllsource, pllm, plln, pllp;
  uint32_t common_system_clock;

  /* Get SYSCLK source -------------------------------------------------------*/
  tmp = RCC->CFGR & RCC_CFGR_SWS;

  switch (tmp)
  {
    case RCC_CFGR_SWS_CSI:  /* CSI used as system clock source */
      common_system_clock = CSI_VALUE;
      break;
    case RCC_CFGR_SWS_HSE:  /* HSE used as system clock source */
      common_system_clock = HSE_VALUE;
      break;
    case RCC_CFGR_SWS_PLL1: /* PLL1 used as system clock source */

      /* PLL_VCO = (HSE_VALUE or HSI_VALUE / PLL_M) * PLL_N
         SYSCLK = PLL_VCO / PLL_P                                  */
      pllsource = (RCC->PLLCKSELR & RCC_PLLCKSELR_PLLSRC) >> RCC_PLLCKSELR_PLLSRC_Pos;
      pllm = (RCC->PLLCKSELR & RCC_PLLCKSELR_DIVM1) >> RCC_PLLCKSELR_DIVM1_Pos;
      plln = (RCC->PLL1DIVR & RCC_PLL1DIVR_N1) >> RCC_PLL1DIVR_N1_Pos;
      pllp = ((RCC->PLL1DIVR & RCC_PLL1DIVR_P1) >> RCC_PLL1DIVR_P1_Pos) + 1U;

      if (pllsource == (RCC_PLLCKSELR_PLLSRC_HSE >> RCC_PLLCKSELR_PLLSRC_Pos))
      {
        /* HSE used as PLL clock source */
        common_system_clock = ((HSE_VALUE / pllm) * plln) / pllp;
      }
      else if (pllsource == (RCC_PLLCKSELR_PLLSRC_HSI >> RCC_PLLCKSELR_PLLSRC_Pos))
      {
        /* HSI used as PLL clock source */
        common_system_clock = ((HSI_VALUE / pllm) * plln) / pllp;
      }
      else
      {
        /* CSI used as PLL clock source */
        common_system_clock = ((CSI_VALUE / pllm) * plln) / pllp;
      }
      break;
    default:  /* HSI used as system clock source */
      common_system_clock = HSI_VALUE;
      break;
  }

  /* Compute CPU (CM7) frequency: SYSCLK / D1CPRE ----------------------------*/
  tmp = D1CorePrescTable[(RCC->D1CFGR & RCC_D1CFGR_D1CPRE) >> RCC_D1CFGR_D1CPRE_Pos];
  SystemCoreClock = common_system_clock >> tmp;
}
