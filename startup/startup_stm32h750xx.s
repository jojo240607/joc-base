/* ****************************************************************************
  * @file      startup_stm32h750xx.s
  * @author    jOS port (based on MCD Application Team F407 template)
  * @brief     STM32H750xx Devices vector table for GCC based toolchains.
  *            This module performs:
  *                - Set the initial SP
  *                - Set the initial PC == Reset_Handler,
  *                - Set the vector table entries with the exceptions ISR address
  *                - Branches to main in the C library (which eventually
  *                  calls main()).
  *            After Reset the Cortex-M7 processor is in Thread mode,
  *            priority is Privileged, and the Stack is set to Main.
  *
  *            Vector numbering follows the official H7 IRQn enum (0..149);
  *            unimplemented slots (42, 64-67, 123, 126, 143, 147, 148) are 0.
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
    
  .syntax unified
  .cpu cortex-m7
  .fpu fpv5-d16
  .thumb

.global  g_pfnVectors
.global  Default_Handler

/* start address for the initialization values of the .data section. 
defined in linker script */
.word  _sidata
/* start address for the .data section. defined in linker script */  
.word  _sdata
/* end address for the .data section. defined in linker script */
.word  _edata
/* start address for the .bss section. defined in linker script */
.word  _sbss
/* end address for the .bss section. defined in linker script */
.word  _ebss
/* stack used for SystemInit_ExtMemCtl; always internal RAM used */

/**
 * @brief  This is the code that gets called when the processor first
 *          starts execution following a reset event. Only the absolutely
 *          necessary set is performed, after which the application
 *          supplied main() routine is called. 
 * @param  None
 * @retval : None
*/

    .section  .text.Reset_Handler
  .weak  Reset_Handler
  .type  Reset_Handler, %function
Reset_Handler:  
  ldr   sp, =_estack     /* set stack pointer */
  
/* Call the clock system initialization function.*/
  bl  SystemInit  

/* Copy the data segment initializers from flash to SRAM */  
  ldr r0, =_sdata
  ldr r1, =_edata
  ldr r2, =_sidata
  movs r3, #0
  b LoopCopyDataInit

CopyDataInit:
  ldr r4, [r2, r3]
  str r4, [r0, r3]
  adds r3, r3, #4

LoopCopyDataInit:
  adds r4, r0, r3
  cmp r4, r1
  bcc CopyDataInit
  
/* Zero fill the bss segment. */
  ldr r2, =_sbss
  ldr r4, =_ebss
  movs r3, #0
  b LoopFillZerobss

FillZerobss:
  str  r3, [r2]
  adds r2, r2, #4

LoopFillZerobss:
  cmp r2, r4
  bcc FillZerobss

/* Copy the App (.rust_data) LMA to VMA (APP_RAM, same mechanism as .data). */
  ldr r0, =__rust_data_start
  ldr r1, =__rust_data_end
  ldr r2, =__rust_data_lma
  movs r3, #0
  b LoopCopyRustDataInit

CopyRustDataInit:
  ldr r4, [r2, r3]
  str r4, [r0, r3]
  adds r3, r3, #4

LoopCopyRustDataInit:
  adds r4, r0, r3
  cmp r4, r1
  bcc CopyRustDataInit

/* Zero fill the App (.rust_bss) segment (APP_RAM). */
  ldr r2, =__rust_bss_start
  ldr r4, =__rust_bss_end
  movs r3, #0
  b LoopFillRustBss

FillRustBss:
  str  r3, [r2]
  adds r2, r2, #4

LoopFillRustBss:
  cmp r2, r4
  bcc FillRustBss

/* Zero fill the CCM bss segment (RTOS task stacks + TCB pool, DTCM on H7). */
  ldr r2, =__ccm_bss_start
  ldr r4, =__ccm_bss_end
  movs r3, #0
  b LoopFillCCMbss

FillCCMbss:
  str  r3, [r2]
  adds r2, r2, #4

LoopFillCCMbss:
  cmp r2, r4
  bcc FillCCMbss

/* Call static constructors */
    bl __libc_init_array
/* Call the application's entry point.*/
  bl  main
  bx  lr    
.size  Reset_Handler, .-Reset_Handler

/**
 * @brief  This is the code that gets called when the processor receives an 
 *         unexpected interrupt.  This simply enters an infinite loop, preserving
 *         the system state for examination by a debugger.
 * @param  None     
 * @retval None       
*/
    .section  .text.Default_Handler,"ax",%progbits
Default_Handler:
Infinite_Loop:
  b  Infinite_Loop
  .size  Default_Handler, .-Default_Handler
/******************************************************************************
*
* The minimal vector table for a Cortex M7. Note that the proper constructs
* must be placed on this to ensure that it ends up at physical address
* 0x0000.0000.
* 
*******************************************************************************/
   .section  .isr_vector,"a",%progbits
  .type  g_pfnVectors, %object
    
    
g_pfnVectors:
  .word  _estack
  .word  Reset_Handler
  .word  NMI_Handler
  .word  HardFault_Handler
  .word  MemManage_Handler
  .word  BusFault_Handler
  .word  UsageFault_Handler
  .word  0
  .word  0
  .word  0
  .word  0
  .word  SVC_Handler
  .word  DebugMon_Handler
  .word  0
  .word  PendSV_Handler
  .word IRQ_CommonHandler   /* SysTick_Handler -> shared ISR */
  
  /* External Interrupts (H750, official IRQn numbering 0..149) */
  .word     IRQ_CommonHandler                   /*  0: WWDG                         */
  .word     IRQ_CommonHandler                   /*  1: PVD_AVD                      */
  .word     IRQ_CommonHandler                   /*  2: TAMP_STAMP                   */
  .word     IRQ_CommonHandler                   /*  3: RTC_WKUP                     */
  .word     IRQ_CommonHandler                   /*  4: FLASH                        */
  .word     IRQ_CommonHandler                   /*  5: RCC                          */
  .word     IRQ_CommonHandler                   /*  6: EXTI0                        */
  .word     IRQ_CommonHandler                   /*  7: EXTI1                        */
  .word     IRQ_CommonHandler                   /*  8: EXTI2                        */
  .word     IRQ_CommonHandler                   /*  9: EXTI3                        */
  .word     IRQ_CommonHandler                   /* 10: EXTI4                        */
  .word     IRQ_CommonHandler                   /* 11: DMA1_Stream0                 */
  .word     IRQ_CommonHandler                   /* 12: DMA1_Stream1                 */
  .word     IRQ_CommonHandler                   /* 13: DMA1_Stream2                 */
  .word     IRQ_CommonHandler                   /* 14: DMA1_Stream3                 */
  .word     IRQ_CommonHandler                   /* 15: DMA1_Stream4                 */
  .word     IRQ_CommonHandler                   /* 16: DMA1_Stream5                 */
  .word     IRQ_CommonHandler                   /* 17: DMA1_Stream6                 */
  .word     IRQ_CommonHandler                   /* 18: ADC1/2                       */
  .word     IRQ_CommonHandler                   /* 19: FDCAN1_IT0                   */
  .word     IRQ_CommonHandler                   /* 20: FDCAN2_IT0                   */
  .word     IRQ_CommonHandler                   /* 21: FDCAN1_IT1                   */
  .word     IRQ_CommonHandler                   /* 22: FDCAN2_IT1                   */
  .word     IRQ_CommonHandler                   /* 23: EXTI9_5                      */
  .word     IRQ_CommonHandler                   /* 24: TIM1_BRK                     */
  .word     IRQ_CommonHandler                   /* 25: TIM1_UP                      */
  .word     IRQ_CommonHandler                   /* 26: TIM1_TRG_COM                 */
  .word     IRQ_CommonHandler                   /* 27: TIM1_CC                      */
  .word     IRQ_CommonHandler                   /* 28: TIM2                         */
  .word     IRQ_CommonHandler                   /* 29: TIM3                         */
  .word     IRQ_CommonHandler                   /* 30: TIM4                         */
  .word     IRQ_CommonHandler                   /* 31: I2C1_EV                      */
  .word     IRQ_CommonHandler                   /* 32: I2C1_ER                      */
  .word     IRQ_CommonHandler                   /* 33: I2C2_EV                      */
  .word     IRQ_CommonHandler                   /* 34: I2C2_ER                      */
  .word     IRQ_CommonHandler                   /* 35: SPI1                         */
  .word     IRQ_CommonHandler                   /* 36: SPI2                         */
  .word     IRQ_CommonHandler                   /* 37: USART1                       */
  .word     IRQ_CommonHandler                   /* 38: USART2                       */
  .word     IRQ_CommonHandler                   /* 39: USART3                       */
  .word     IRQ_CommonHandler                   /* 40: EXTI15_10                    */
  .word     IRQ_CommonHandler                   /* 41: RTC_Alarm                    */
  .word     0                                  /* 42: reserved                     */
  .word     IRQ_CommonHandler                   /* 43: TIM8_BRK_TIM12               */
  .word     IRQ_CommonHandler                   /* 44: TIM8_UP_TIM13                */
  .word     IRQ_CommonHandler                   /* 45: TIM8_TRG_COM_TIM14           */
  .word     IRQ_CommonHandler                   /* 46: TIM8_CC                      */
  .word     IRQ_CommonHandler                   /* 47: DMA1_Stream7                 */
  .word     IRQ_CommonHandler                   /* 48: FMC                          */
  .word     IRQ_CommonHandler                   /* 49: SDMMC1                       */
  .word     IRQ_CommonHandler                   /* 50: TIM5                         */
  .word     IRQ_CommonHandler                   /* 51: SPI3                         */
  .word     IRQ_CommonHandler                   /* 52: UART4                        */
  .word     IRQ_CommonHandler                   /* 53: UART5                        */
  .word     IRQ_CommonHandler                   /* 54: TIM6_DAC                     */
  .word     IRQ_CommonHandler                   /* 55: TIM7                         */
  .word     IRQ_CommonHandler                   /* 56: DMA2_Stream0                 */
  .word     IRQ_CommonHandler                   /* 57: DMA2_Stream1                 */
  .word     IRQ_CommonHandler                   /* 58: DMA2_Stream2                 */
  .word     IRQ_CommonHandler                   /* 59: DMA2_Stream3                 */
  .word     IRQ_CommonHandler                   /* 60: DMA2_Stream4                 */
  .word     IRQ_CommonHandler                   /* 61: ETH                          */
  .word     IRQ_CommonHandler                   /* 62: ETH_WKUP                     */
  .word     IRQ_CommonHandler                   /* 63: FDCAN_CAL                    */
  .word     0                                  /* 64: reserved                     */
  .word     0                                  /* 65: reserved                     */
  .word     0                                  /* 66: reserved                     */
  .word     0                                  /* 67: reserved                     */
  .word     IRQ_CommonHandler                   /* 68: DMA2_Stream5                 */
  .word     IRQ_CommonHandler                   /* 69: DMA2_Stream6                 */
  .word     IRQ_CommonHandler                   /* 70: DMA2_Stream7                 */
  .word     IRQ_CommonHandler                   /* 71: USART6                       */
  .word     IRQ_CommonHandler                   /* 72: I2C3_EV                      */
  .word     IRQ_CommonHandler                   /* 73: I2C3_ER                      */
  .word     IRQ_CommonHandler                   /* 74: OTG_HS_EP1_OUT               */
  .word     IRQ_CommonHandler                   /* 75: OTG_HS_EP1_IN                */
  .word     IRQ_CommonHandler                   /* 76: OTG_HS_WKUP                  */
  .word     IRQ_CommonHandler                   /* 77: OTG_HS                       */
  .word     IRQ_CommonHandler                   /* 78: DCMI                         */
  .word     IRQ_CommonHandler                   /* 79: CRYP                         */
  .word     IRQ_CommonHandler                   /* 80: HASH_RNG                     */
  .word     IRQ_CommonHandler                   /* 81: FPU                          */
  .word     IRQ_CommonHandler                   /* 82: UART7                        */
  .word     IRQ_CommonHandler                   /* 83: UART8                        */
  .word     IRQ_CommonHandler                   /* 84: SPI4                         */
  .word     IRQ_CommonHandler                   /* 85: SPI5                         */
  .word     IRQ_CommonHandler                   /* 86: SPI6                         */
  .word     IRQ_CommonHandler                   /* 87: SAI1                         */
  .word     IRQ_CommonHandler                   /* 88: LTDC                         */
  .word     IRQ_CommonHandler                   /* 89: LTDC_ER                      */
  .word     IRQ_CommonHandler                   /* 90: DMA2D                        */
  .word     IRQ_CommonHandler                   /* 91: SAI2                         */
  .word     IRQ_CommonHandler                   /* 92: QUADSPI                      */
  .word     IRQ_CommonHandler                   /* 93: LPTIM1                       */
  .word     IRQ_CommonHandler                   /* 94: CEC                          */
  .word     IRQ_CommonHandler                   /* 95: I2C4_EV                      */
  .word     IRQ_CommonHandler                   /* 96: I2C4_ER                      */
  .word     IRQ_CommonHandler                   /* 97: SPDIF_RX                     */
  .word     IRQ_CommonHandler                   /* 98: OTG_FS_EP1_OUT               */
  .word     IRQ_CommonHandler                   /* 99: OTG_FS_EP1_IN                */
  .word     IRQ_CommonHandler                   /* 100: OTG_FS_WKUP                 */
  .word     IRQ_CommonHandler                   /* 101: OTG_FS                      */
  .word     IRQ_CommonHandler                   /* 102: DMAMUX1_OVR                 */
  .word     IRQ_CommonHandler                   /* 103: HRTIM1_MASTER               */
  .word     IRQ_CommonHandler                   /* 104: HRTIM1_TIMA                 */
  .word     IRQ_CommonHandler                   /* 105: HRTIM1_TIMB                 */
  .word     IRQ_CommonHandler                   /* 106: HRTIM1_TIMC                 */
  .word     IRQ_CommonHandler                   /* 107: HRTIM1_TIMD                 */
  .word     IRQ_CommonHandler                   /* 108: HRTIM1_TIME                 */
  .word     IRQ_CommonHandler                   /* 109: HRTIM1_FLT                  */
  .word     IRQ_CommonHandler                   /* 110: DFSDM1_FLT0                 */
  .word     IRQ_CommonHandler                   /* 111: DFSDM1_FLT1                 */
  .word     IRQ_CommonHandler                   /* 112: DFSDM1_FLT2                 */
  .word     IRQ_CommonHandler                   /* 113: DFSDM1_FLT3                 */
  .word     IRQ_CommonHandler                   /* 114: SAI3                        */
  .word     IRQ_CommonHandler                   /* 115: SWPMI1                      */
  .word     IRQ_CommonHandler                   /* 116: TIM15                       */
  .word     IRQ_CommonHandler                   /* 117: TIM16                       */
  .word     IRQ_CommonHandler                   /* 118: TIM17                       */
  .word     IRQ_CommonHandler                   /* 119: MDIOS_WKUP                  */
  .word     IRQ_CommonHandler                   /* 120: MDIOS                       */
  .word     IRQ_CommonHandler                   /* 121: JPEG                        */
  .word     IRQ_CommonHandler                   /* 122: MDMA                        */
  .word     0                                  /* 123: reserved                   */
  .word     IRQ_CommonHandler                   /* 124: SDMMC2                     */
  .word     IRQ_CommonHandler                   /* 125: HSEM1                      */
  .word     0                                  /* 126: reserved                   */
  .word     IRQ_CommonHandler                   /* 127: ADC3                       */
  .word     IRQ_CommonHandler                   /* 128: DMAMUX2_OVR                */
  .word     IRQ_CommonHandler                   /* 129: BDMA_Channel0              */
  .word     IRQ_CommonHandler                   /* 130: BDMA_Channel1              */
  .word     IRQ_CommonHandler                   /* 131: BDMA_Channel2              */
  .word     IRQ_CommonHandler                   /* 132: BDMA_Channel3              */
  .word     IRQ_CommonHandler                   /* 133: BDMA_Channel4              */
  .word     IRQ_CommonHandler                   /* 134: BDMA_Channel5              */
  .word     IRQ_CommonHandler                   /* 135: BDMA_Channel6              */
  .word     IRQ_CommonHandler                   /* 136: BDMA_Channel7              */
  .word     IRQ_CommonHandler                   /* 137: COMP                       */
  .word     IRQ_CommonHandler                   /* 138: LPTIM2                     */
  .word     IRQ_CommonHandler                   /* 139: LPTIM3                     */
  .word     IRQ_CommonHandler                   /* 140: LPTIM4                     */
  .word     IRQ_CommonHandler                   /* 141: LPTIM5                     */
  .word     IRQ_CommonHandler                   /* 142: LPUART1                    */
  .word     0                                  /* 143: reserved                   */
  .word     IRQ_CommonHandler                   /* 144: CRS                        */
  .word     IRQ_CommonHandler                   /* 145: ECC                        */
  .word     IRQ_CommonHandler                   /* 146: SAI4                       */
  .word     0                                  /* 147: reserved                   */
  .word     0                                  /* 148: reserved                   */
  .word     IRQ_CommonHandler                   /* 149: WAKEUP_PIN                 */

  .size  g_pfnVectors, .-g_pfnVectors

/*******************************************************************************
*
* Provide weak aliases for each Exception handler to the Default_Handler. 
* As they are weak aliases, any function with the same name will override 
* this definition.
* 
*******************************************************************************/
   .weak      NMI_Handler
   .thumb_set NMI_Handler,Default_Handler
  
   .weak      HardFault_Handler
   .thumb_set HardFault_Handler,Default_Handler
  
   .weak      MemManage_Handler
   .thumb_set MemManage_Handler,Default_Handler
  
   .weak      BusFault_Handler
   .thumb_set BusFault_Handler,Default_Handler

   .weak      UsageFault_Handler
   .thumb_set UsageFault_Handler,Default_Handler

   .weak      SVC_Handler
   .thumb_set SVC_Handler,Default_Handler

   .weak      DebugMon_Handler
   .thumb_set DebugMon_Handler,Default_Handler

   .weak      PendSV_Handler
   .thumb_set PendSV_Handler,Default_Handler

   .weak      SysTick_Handler
   .thumb_set SysTick_Handler,Default_Handler              
  
   .weak      WWDG_IRQHandler                   
   .thumb_set WWDG_IRQHandler,Default_Handler      
                  
   .weak      PVD_AVD_IRQHandler      
   .thumb_set PVD_AVD_IRQHandler,Default_Handler
               
   .weak      TAMP_STAMP_IRQHandler            
   .thumb_set TAMP_STAMP_IRQHandler,Default_Handler
            
   .weak      RTC_WKUP_IRQHandler                  
   .thumb_set RTC_WKUP_IRQHandler,Default_Handler
            
   .weak      FLASH_IRQHandler         
   .thumb_set FLASH_IRQHandler,Default_Handler
                  
   .weak      RCC_IRQHandler      
   .thumb_set RCC_IRQHandler,Default_Handler
                  
   .weak      EXTI0_IRQHandler         
   .thumb_set EXTI0_IRQHandler,Default_Handler
                  
   .weak      EXTI1_IRQHandler         
   .thumb_set EXTI1_IRQHandler,Default_Handler
                     
   .weak      EXTI2_IRQHandler         
   .thumb_set EXTI2_IRQHandler,Default_Handler 
                 
   .weak      EXTI3_IRQHandler         
   .thumb_set EXTI3_IRQHandler,Default_Handler
                        
   .weak      EXTI4_IRQHandler         
   .thumb_set EXTI4_IRQHandler,Default_Handler
                  
   .weak      DMA1_Stream0_IRQHandler               
   .thumb_set DMA1_Stream0_IRQHandler,Default_Handler
         
   .weak      DMA1_Stream1_IRQHandler               
   .thumb_set DMA1_Stream1_IRQHandler,Default_Handler
                  
   .weak      DMA1_Stream2_IRQHandler               
   .thumb_set DMA1_Stream2_IRQHandler,Default_Handler
                  
   .weak      DMA1_Stream3_IRQHandler               
   .thumb_set DMA1_Stream3_IRQHandler,Default_Handler 
                 
   .weak      DMA1_Stream4_IRQHandler              
   .thumb_set DMA1_Stream4_IRQHandler,Default_Handler
                  
   .weak      DMA1_Stream5_IRQHandler               
   .thumb_set DMA1_Stream5_IRQHandler,Default_Handler
                  
   .weak      DMA1_Stream6_IRQHandler               
   .thumb_set DMA1_Stream6_IRQHandler,Default_Handler
                  
   .weak      ADC_IRQHandler      
   .thumb_set ADC_IRQHandler,Default_Handler
               
   .weak      FDCAN1_IT0_IRQHandler   
   .thumb_set FDCAN1_IT0_IRQHandler,Default_Handler
            
   .weak      FDCAN2_IT0_IRQHandler                  
   .thumb_set FDCAN2_IT0_IRQHandler,Default_Handler
                           
   .weak      FDCAN1_IT1_IRQHandler                  
   .thumb_set FDCAN1_IT1_IRQHandler,Default_Handler
            
   .weak      FDCAN2_IT1_IRQHandler                  
   .thumb_set FDCAN2_IT1_IRQHandler,Default_Handler
            
   .weak      EXTI9_5_IRQHandler   
   .thumb_set EXTI9_5_IRQHandler,Default_Handler
            
   .weak      TIM1_BRK_IRQHandler            
   .thumb_set TIM1_BRK_IRQHandler,Default_Handler
            
   .weak      TIM1_UP_IRQHandler            
   .thumb_set TIM1_UP_IRQHandler,Default_Handler
      
   .weak      TIM1_TRG_COM_IRQHandler      
   .thumb_set TIM1_TRG_COM_IRQHandler,Default_Handler
      
   .weak      TIM1_CC_IRQHandler   
   .thumb_set TIM1_CC_IRQHandler,Default_Handler
                  
   .weak      TIM2_IRQHandler            
   .thumb_set TIM2_IRQHandler,Default_Handler
                  
   .weak      TIM3_IRQHandler            
   .thumb_set TIM3_IRQHandler,Default_Handler
                  
   .weak      TIM4_IRQHandler            
   .thumb_set TIM4_IRQHandler,Default_Handler
                  
   .weak      I2C1_EV_IRQHandler   
   .thumb_set I2C1_EV_IRQHandler,Default_Handler
                     
   .weak      I2C1_ER_IRQHandler   
   .thumb_set I2C1_ER_IRQHandler,Default_Handler
                     
   .weak      I2C2_EV_IRQHandler   
   .thumb_set I2C2_EV_IRQHandler,Default_Handler
                  
   .weak      I2C2_ER_IRQHandler   
   .thumb_set I2C2_ER_IRQHandler,Default_Handler
                           
   .weak      SPI1_IRQHandler            
   .thumb_set SPI1_IRQHandler,Default_Handler
                        
   .weak      SPI2_IRQHandler            
   .thumb_set SPI2_IRQHandler,Default_Handler
                  
   .weak      USART1_IRQHandler      
   .thumb_set USART1_IRQHandler,Default_Handler
                     
   .weak      USART2_IRQHandler      
   .thumb_set USART2_IRQHandler,Default_Handler
                     
   .weak      USART3_IRQHandler      
   .thumb_set USART3_IRQHandler,Default_Handler
                  
   .weak      EXTI15_10_IRQHandler               
   .thumb_set EXTI15_10_IRQHandler,Default_Handler
               
   .weak      RTC_Alarm_IRQHandler               
   .thumb_set RTC_Alarm_IRQHandler,Default_Handler
            
   .weak      TIM8_BRK_TIM12_IRQHandler         
   .thumb_set TIM8_BRK_TIM12_IRQHandler,Default_Handler
         
   .weak      TIM8_UP_TIM13_IRQHandler            
   .thumb_set TIM8_UP_TIM13_IRQHandler,Default_Handler
         
   .weak      TIM8_TRG_COM_TIM14_IRQHandler      
   .thumb_set TIM8_TRG_COM_TIM14_IRQHandler,Default_Handler
      
   .weak      TIM8_CC_IRQHandler   
   .thumb_set TIM8_CC_IRQHandler,Default_Handler
                  
   .weak      DMA1_Stream7_IRQHandler               
   .thumb_set DMA1_Stream7_IRQHandler,Default_Handler
                  
   .weak      FMC_IRQHandler            
   .thumb_set FMC_IRQHandler,Default_Handler
                  
   .weak      SDMMC1_IRQHandler            
   .thumb_set SDMMC1_IRQHandler,Default_Handler
                  
   .weak      TIM5_IRQHandler            
   .thumb_set TIM5_IRQHandler,Default_Handler
                  
   .weak      SPI3_IRQHandler            
   .thumb_set SPI3_IRQHandler,Default_Handler
                  
   .weak      UART4_IRQHandler            
   .thumb_set UART4_IRQHandler,Default_Handler
                  
   .weak      UART5_IRQHandler            
   .thumb_set UART5_IRQHandler,Default_Handler
                  
   .weak      TIM6_DAC_IRQHandler            
   .thumb_set TIM6_DAC_IRQHandler,Default_Handler
                  
   .weak      TIM7_IRQHandler            
   .thumb_set TIM7_IRQHandler,Default_Handler
                  
   .weak      DMA2_Stream0_IRQHandler               
   .thumb_set DMA2_Stream0_IRQHandler,Default_Handler
                  
   .weak      DMA2_Stream1_IRQHandler               
   .thumb_set DMA2_Stream1_IRQHandler,Default_Handler
                  
   .weak      DMA2_Stream2_IRQHandler               
   .thumb_set DMA2_Stream2_IRQHandler,Default_Handler
                  
   .weak      DMA2_Stream3_IRQHandler               
   .thumb_set DMA2_Stream3_IRQHandler,Default_Handler
                  
   .weak      DMA2_Stream4_IRQHandler               
   .thumb_set DMA2_Stream4_IRQHandler,Default_Handler
                  
   .weak      ETH_IRQHandler            
   .thumb_set ETH_IRQHandler,Default_Handler
                  
   .weak      ETH_WKUP_IRQHandler            
   .thumb_set ETH_WKUP_IRQHandler,Default_Handler
                  
   .weak      FDCAN_CAL_IRQHandler            
   .thumb_set FDCAN_CAL_IRQHandler,Default_Handler
                  
   .weak      DMA2_Stream5_IRQHandler               
   .thumb_set DMA2_Stream5_IRQHandler,Default_Handler
                  
   .weak      DMA2_Stream6_IRQHandler               
   .thumb_set DMA2_Stream6_IRQHandler,Default_Handler
                  
   .weak      DMA2_Stream7_IRQHandler               
   .thumb_set DMA2_Stream7_IRQHandler,Default_Handler
                  
   .weak      USART6_IRQHandler            
   .thumb_set USART6_IRQHandler,Default_Handler
                  
   .weak      I2C3_EV_IRQHandler            
   .thumb_set I2C3_EV_IRQHandler,Default_Handler
                  
   .weak      I2C3_ER_IRQHandler            
   .thumb_set I2C3_ER_IRQHandler,Default_Handler
                  
   .weak      OTG_HS_EP1_OUT_IRQHandler            
   .thumb_set OTG_HS_EP1_OUT_IRQHandler,Default_Handler
                  
   .weak      OTG_HS_EP1_IN_IRQHandler            
   .thumb_set OTG_HS_EP1_IN_IRQHandler,Default_Handler
                  
   .weak      OTG_HS_WKUP_IRQHandler            
   .thumb_set OTG_HS_WKUP_IRQHandler,Default_Handler
                  
   .weak      OTG_HS_IRQHandler            
   .thumb_set OTG_HS_IRQHandler,Default_Handler
                  
   .weak      DCMI_IRQHandler            
   .thumb_set DCMI_IRQHandler,Default_Handler
                  
   .weak      CRYP_IRQHandler            
   .thumb_set CRYP_IRQHandler,Default_Handler
                  
   .weak      HASH_RNG_IRQHandler            
   .thumb_set HASH_RNG_IRQHandler,Default_Handler
                  
   .weak      FPU_IRQHandler            
   .thumb_set FPU_IRQHandler,Default_Handler
                  
   .weak      UART7_IRQHandler            
   .thumb_set UART7_IRQHandler,Default_Handler
                  
   .weak      UART8_IRQHandler            
   .thumb_set UART8_IRQHandler,Default_Handler
                  
   .weak      SPI4_IRQHandler            
   .thumb_set SPI4_IRQHandler,Default_Handler
                  
   .weak      SPI5_IRQHandler            
   .thumb_set SPI5_IRQHandler,Default_Handler
                  
   .weak      SPI6_IRQHandler            
   .thumb_set SPI6_IRQHandler,Default_Handler
                  
   .weak      SAI1_IRQHandler            
   .thumb_set SAI1_IRQHandler,Default_Handler
                  
   .weak      LTDC_IRQHandler            
   .thumb_set LTDC_IRQHandler,Default_Handler
                  
   .weak      LTDC_ER_IRQHandler            
   .thumb_set LTDC_ER_IRQHandler,Default_Handler
                  
   .weak      DMA2D_IRQHandler            
   .thumb_set DMA2D_IRQHandler,Default_Handler
                  
   .weak      SAI2_IRQHandler            
   .thumb_set SAI2_IRQHandler,Default_Handler
                  
   .weak      QUADSPI_IRQHandler            
   .thumb_set QUADSPI_IRQHandler,Default_Handler
                  
   .weak      LPTIM1_IRQHandler            
   .thumb_set LPTIM1_IRQHandler,Default_Handler
                  
   .weak      CEC_IRQHandler            
   .thumb_set CEC_IRQHandler,Default_Handler
                  
   .weak      I2C4_EV_IRQHandler            
   .thumb_set I2C4_EV_IRQHandler,Default_Handler
                  
   .weak      I2C4_ER_IRQHandler            
   .thumb_set I2C4_ER_IRQHandler,Default_Handler
                  
   .weak      SPDIF_RX_IRQHandler            
   .thumb_set SPDIF_RX_IRQHandler,Default_Handler
                  
   .weak      OTG_FS_EP1_OUT_IRQHandler            
   .thumb_set OTG_FS_EP1_OUT_IRQHandler,Default_Handler
                  
   .weak      OTG_FS_EP1_IN_IRQHandler            
   .thumb_set OTG_FS_EP1_IN_IRQHandler,Default_Handler
                  
   .weak      OTG_FS_WKUP_IRQHandler            
   .thumb_set OTG_FS_WKUP_IRQHandler,Default_Handler
                  
   .weak      OTG_FS_IRQHandler            
   .thumb_set OTG_FS_IRQHandler,Default_Handler
                  
   .weak      DMAMUX1_OVR_IRQHandler            
   .thumb_set DMAMUX1_OVR_IRQHandler,Default_Handler
                  
   .weak      HRTIM1_MASTER_IRQHandler            
   .thumb_set HRTIM1_MASTER_IRQHandler,Default_Handler
                  
   .weak      HRTIM1_TIMA_IRQHandler            
   .thumb_set HRTIM1_TIMA_IRQHandler,Default_Handler
                  
   .weak      HRTIM1_TIMB_IRQHandler            
   .thumb_set HRTIM1_TIMB_IRQHandler,Default_Handler
                  
   .weak      HRTIM1_TIMC_IRQHandler            
   .thumb_set HRTIM1_TIMC_IRQHandler,Default_Handler
                  
   .weak      HRTIM1_TIMD_IRQHandler            
   .thumb_set HRTIM1_TIMD_IRQHandler,Default_Handler
                  
   .weak      HRTIM1_TIME_IRQHandler            
   .thumb_set HRTIM1_TIME_IRQHandler,Default_Handler
                  
   .weak      HRTIM1_FLT_IRQHandler            
   .thumb_set HRTIM1_FLT_IRQHandler,Default_Handler
                  
   .weak      DFSDM1_FLT0_IRQHandler            
   .thumb_set DFSDM1_FLT0_IRQHandler,Default_Handler
                  
   .weak      DFSDM1_FLT1_IRQHandler            
   .thumb_set DFSDM1_FLT1_IRQHandler,Default_Handler
                  
   .weak      DFSDM1_FLT2_IRQHandler            
   .thumb_set DFSDM1_FLT2_IRQHandler,Default_Handler
                  
   .weak      DFSDM1_FLT3_IRQHandler            
   .thumb_set DFSDM1_FLT3_IRQHandler,Default_Handler
                  
   .weak      SAI3_IRQHandler            
   .thumb_set SAI3_IRQHandler,Default_Handler
                  
   .weak      SWPMI1_IRQHandler            
   .thumb_set SWPMI1_IRQHandler,Default_Handler
                  
   .weak      TIM15_IRQHandler            
   .thumb_set TIM15_IRQHandler,Default_Handler
                  
   .weak      TIM16_IRQHandler            
   .thumb_set TIM16_IRQHandler,Default_Handler
                  
   .weak      TIM17_IRQHandler            
   .thumb_set TIM17_IRQHandler,Default_Handler
                  
   .weak      MDIOS_WKUP_IRQHandler            
   .thumb_set MDIOS_WKUP_IRQHandler,Default_Handler
                  
   .weak      MDIOS_IRQHandler            
   .thumb_set MDIOS_IRQHandler,Default_Handler
                  
   .weak      JPEG_IRQHandler            
   .thumb_set JPEG_IRQHandler,Default_Handler
                  
   .weak      MDMA_IRQHandler            
   .thumb_set MDMA_IRQHandler,Default_Handler
                  
   .weak      SDMMC2_IRQHandler            
   .thumb_set SDMMC2_IRQHandler,Default_Handler
                  
   .weak      HSEM1_IRQHandler            
   .thumb_set HSEM1_IRQHandler,Default_Handler
                  
   .weak      ADC3_IRQHandler            
   .thumb_set ADC3_IRQHandler,Default_Handler
                  
   .weak      DMAMUX2_OVR_IRQHandler            
   .thumb_set DMAMUX2_OVR_IRQHandler,Default_Handler
                  
   .weak      BDMA_Channel0_IRQHandler            
   .thumb_set BDMA_Channel0_IRQHandler,Default_Handler
                  
   .weak      BDMA_Channel1_IRQHandler            
   .thumb_set BDMA_Channel1_IRQHandler,Default_Handler
                  
   .weak      BDMA_Channel2_IRQHandler            
   .thumb_set BDMA_Channel2_IRQHandler,Default_Handler
                  
   .weak      BDMA_Channel3_IRQHandler            
   .thumb_set BDMA_Channel3_IRQHandler,Default_Handler
                  
   .weak      BDMA_Channel4_IRQHandler            
   .thumb_set BDMA_Channel4_IRQHandler,Default_Handler
                  
   .weak      BDMA_Channel5_IRQHandler            
   .thumb_set BDMA_Channel5_IRQHandler,Default_Handler
                  
   .weak      BDMA_Channel6_IRQHandler            
   .thumb_set BDMA_Channel6_IRQHandler,Default_Handler
                  
   .weak      BDMA_Channel7_IRQHandler            
   .thumb_set BDMA_Channel7_IRQHandler,Default_Handler
                  
   .weak      COMP_IRQHandler            
   .thumb_set COMP_IRQHandler,Default_Handler
                  
   .weak      LPTIM2_IRQHandler            
   .thumb_set LPTIM2_IRQHandler,Default_Handler
                  
   .weak      LPTIM3_IRQHandler            
   .thumb_set LPTIM3_IRQHandler,Default_Handler
                  
   .weak      LPTIM4_IRQHandler            
   .thumb_set LPTIM4_IRQHandler,Default_Handler
                  
   .weak      LPTIM5_IRQHandler            
   .thumb_set LPTIM5_IRQHandler,Default_Handler
                  
   .weak      LPUART1_IRQHandler            
   .thumb_set LPUART1_IRQHandler,Default_Handler
                  
   .weak      CRS_IRQHandler            
   .thumb_set CRS_IRQHandler,Default_Handler
                  
   .weak      ECC_IRQHandler            
   .thumb_set ECC_IRQHandler,Default_Handler
                  
   .weak      SAI4_IRQHandler            
   .thumb_set SAI4_IRQHandler,Default_Handler
                  
   .weak      WAKEUP_PIN_IRQHandler            
   .thumb_set WAKEUP_PIN_IRQHandler,Default_Handler

.end
