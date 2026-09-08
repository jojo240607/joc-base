/*
 * STM32F103xx Minimal Startup file (Cortex-M3)
 *
 * Standard vector table + Reset_Handler that calls SystemInit then _start (CTOR + main).
 * Adapted from STM32CubeF1 startup_stm32f103xb.s pattern.
 *
 * Stack size: 2 KB (matching 48 KB SRAM, generous for minimal RTOS tasks).
 * Heap: not used (RTOS uses its own allocator).
 */
.syntax unified
.cpu cortex-m3
.thumb

/* Global memory values from linker script */
.equ  _stack_top,   0x2000C000   /* ORIGIN(STACK) 0x2000B000 + LENGTH(STACK) 4K */

.section .isr_vector,"a",%progbits
.word _stack_top
.word Reset_Handler
.word NMI_Handler
.word HardFault_Handler
.word MemManage_Handler
.word BusFault_Handler
.word UsageFault_Handler
.word 0
.word 0
.word 0
.word 0
.word SVC_Handler
.word DebugMon_Handler
.word 0
.word PendSV_Handler
.word IRQ_CommonHandler    /* SysTick_Handler -> shared ISR */

/* External interrupts (STM32F103)
 * 43 lines: WWDG..DAC */
.word IRQ_CommonHandler            /*  0: Window Watchdog */
.word IRQ_CommonHandler             /*  1: PVD through EXTI */
.word IRQ_CommonHandler          /*  2: Tamper */
.word IRQ_CommonHandler             /*  3: RTC */
.word IRQ_CommonHandler           /*  4: Flash */
.word IRQ_CommonHandler             /*  5: RCC */
.word IRQ_CommonHandler           /*  6: EXTI Line0 */
.word IRQ_CommonHandler           /*  7: EXTI Line1 */
.word IRQ_CommonHandler           /*  8: EXTI Line2 */
.word IRQ_CommonHandler           /*  9: EXTI Line3 */
.word IRQ_CommonHandler           /* 10: EXTI Line4 */
.word IRQ_CommonHandler   /* 11: DMA1 Channel1 */
.word IRQ_CommonHandler   /* 12: DMA1 Channel2 */
.word IRQ_CommonHandler   /* 13: DMA1 Channel3 */
.word IRQ_CommonHandler   /* 14: DMA1 Channel4 */
.word IRQ_CommonHandler   /* 15: DMA1 Channel5 */
.word IRQ_CommonHandler   /* 16: DMA1 Channel6 */
.word IRQ_CommonHandler   /* 17: DMA1 Channel7 */
.word IRQ_CommonHandler          /* 18: ADC1/2 */
.word IRQ_CommonHandler /* 19: USB High Priority / CAN1 TX */
.word IRQ_CommonHandler/* 20: USB Low Priority / CAN1 RX0 */
.word IRQ_CommonHandler       /* 21: CAN1 RX1 */
.word IRQ_CommonHandler       /* 22: CAN1 SCE */
.word IRQ_CommonHandler        /* 23: EXTI Line5..9 */
.word IRQ_CommonHandler       /* 24: TIM1 Break */
.word IRQ_CommonHandler        /* 25: TIM1 Update */
.word IRQ_CommonHandler   /* 26: TIM1 Trigger/Commutation */
.word IRQ_CommonHandler        /* 27: TIM1 Capture Compare */
.word IRQ_CommonHandler           /* 28: TIM2 */
.word IRQ_CommonHandler           /* 29: TIM3 */
.word IRQ_CommonHandler           /* 30: TIM4 */
.word IRQ_CommonHandler        /* 31: I2C1 Event */
.word IRQ_CommonHandler        /* 32: I2C1 Error */
.word IRQ_CommonHandler        /* 33: I2C2 Event */
.word IRQ_CommonHandler        /* 34: I2C2 Error */
.word IRQ_CommonHandler           /* 35: SPI1 */
.word IRQ_CommonHandler           /* 36: SPI2 */
.word IRQ_CommonHandler         /* 37: USART1 */
.word IRQ_CommonHandler         /* 38: USART2 */
.word IRQ_CommonHandler         /* 39: USART3 */
.word IRQ_CommonHandler      /* 40: EXTI Line10..15 */
.word IRQ_CommonHandler      /* 41: RTC Alarm through EXTI */
.word IRQ_CommonHandler      /* 42: USB Wakeup */

.section  .text.Reset_Handler
.weak     Reset_Handler
.type     Reset_Handler, %function
Reset_Handler:
    /* Set stack pointer */
    ldr   sp, =_stack_top

    /* Call SystemInit (VTOR, early clock) */
    bl    SystemInit

    /* Clear BSS */
    ldr   r1, =__bss_start__
    ldr   r2, =__bss_end__
    movs  r0, 0
    b     .L_bss_check
.L_bss_loop:
    stmia r1!, {r0}
.L_bss_check:
    cmp   r1, r2
    blo   .L_bss_loop

    /* Copy .data from flash to SRAM */
    ldr   r1, =__data_start__
    ldr   r2, =__data_end__
    ldr   r3, =__data_load__
    b     .L_data_check
.L_data_loop:
    ldmia r3!, {r0}
    stmia r1!, {r0}
.L_data_check:
    cmp   r1, r2
    blo   .L_data_loop

    /* Jump directly to main (skip CRT _start which overwrites our SP to 0x00080000).
     * Our Reset_Handler already sets SP, clears BSS, and copies .data, so the CRT's
     * redundant initialization in _mainCRTStartup would only corrupt the stack pointer
     * and cause a crash in __libc_init_array._init when push writes to the flash alias
     * at 0x00080000 (aliased to 0x08080000, past 256KB flash end on F103). */
    bl    main

    /* main() never returns, but WFI if it does */
.L_loop:
    wfi
    b     .L_loop
.size Reset_Handler, . - Reset_Handler

/* ---- Weak default handlers ---- */
.macro  weak_handler  name
.thumb_func
.weak   \name
.type   \name, %function
\name:
    b     .
.size  \name, . - \name
.endm

    weak_handler  NMI_Handler
    weak_handler  HardFault_Handler
    weak_handler  MemManage_Handler
    weak_handler  BusFault_Handler
    weak_handler  UsageFault_Handler
    weak_handler  SVC_Handler
    weak_handler  DebugMon_Handler
    weak_handler  PendSV_Handler
    weak_handler  SysTick_Handler

    weak_handler  WWDG_IRQHandler
    weak_handler  PVD_IRQHandler
    weak_handler  TAMPER_IRQHandler
    weak_handler  RTC_IRQHandler
    weak_handler  FLASH_IRQHandler
    weak_handler  RCC_IRQHandler
    weak_handler  EXTI0_IRQHandler
    weak_handler  EXTI1_IRQHandler
    weak_handler  EXTI2_IRQHandler
    weak_handler  EXTI3_IRQHandler
    weak_handler  EXTI4_IRQHandler
    weak_handler  DMA1_Channel1_IRQHandler
    weak_handler  DMA1_Channel2_IRQHandler
    weak_handler  DMA1_Channel3_IRQHandler
    weak_handler  DMA1_Channel4_IRQHandler
    weak_handler  DMA1_Channel5_IRQHandler
    weak_handler  DMA1_Channel6_IRQHandler
    weak_handler  DMA1_Channel7_IRQHandler
    weak_handler  ADC1_2_IRQHandler
    weak_handler  USB_HP_CAN1_TX_IRQHandler
    weak_handler  USB_LP_CAN1_RX0_IRQHandler
    weak_handler  CAN1_RX1_IRQHandler
    weak_handler  CAN1_SCE_IRQHandler
    weak_handler  EXTI9_5_IRQHandler
    weak_handler  TIM1_BRK_IRQHandler
    weak_handler  TIM1_UP_IRQHandler
    weak_handler  TIM1_TRG_COM_IRQHandler
    weak_handler  TIM1_CC_IRQHandler
    weak_handler  TIM2_IRQHandler
    weak_handler  TIM3_IRQHandler
    weak_handler  TIM4_IRQHandler
    weak_handler  I2C1_EV_IRQHandler
    weak_handler  I2C1_ER_IRQHandler
    weak_handler  I2C2_EV_IRQHandler
    weak_handler  I2C2_ER_IRQHandler
    weak_handler  SPI1_IRQHandler
    weak_handler  SPI2_IRQHandler
    weak_handler  USART1_IRQHandler
    weak_handler  USART2_IRQHandler
    weak_handler  USART3_IRQHandler
    weak_handler  EXTI15_10_IRQHandler
    weak_handler  RTC_Alarm_IRQHandler
    weak_handler  USBWakeUp_IRQHandler

.end