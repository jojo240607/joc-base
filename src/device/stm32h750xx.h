/**
  ******************************************************************************
  * @file    stm32h750xx.h
  * @brief   CMSIS STM32H750xx Device Peripheral Access Layer Header File.
  *
  *          Minimal register definitions for jOS RTOS port. Covers only the
  *          peripherals used by the H7 HAL: RCC, GPIO, SYSCFG, FLASH, PWR,
  *          USART, EXTI, IWDG, CRC, RTC, QUADSPI, DMA, SPI, I2C, TIM.
  *
  *          Register layouts verified against ST official stm32h750xx.h
  *          (RM0433): H7 has D1/D2/D3 clock domains (RCC->D1CFGR/D2CFGR/
  *          D3CFGR), GPIO clocks live in RCC->AHB4ENR, EXTI has no clock
  *          gate, USART uses ISR/ICR/RDR/TDR (F7-style, no SR), reset flags
  *          live in RCC->RSR (not CSR), FLASH_ACR has LATENCY only (M7
  *          I/D-caches are always enabled).
  ******************************************************************************
  */
#ifndef __STM32H750xx_H
#define __STM32H750xx_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** @addtogroup Configuration_section_for_CMSIS
  * @{
  */
#define __CM7_REV                 0x0101U  /* Core revision r1p1       */
#define __MPU_PRESENT             1U       /* MPU present              */
#define __NVIC_PRIO_BITS          4U       /* 4 bits for priority      */
#define __Vendor_SysTickConfig    0U       /* Standard SysTick         */
#define __FPU_PRESENT             1U       /* FPU present (fpv5-d16)   */
#define __ICACHE_PRESENT          1U       /* I-Cache present          */
#define __DCACHE_PRESENT          1U       /* D-Cache present          */
/** @} */

/** @addtogroup IRQn_Type
  * @{
  */
typedef enum IRQn {
    /* Cortex-M7 Core Exceptions (negative numbers, from cortex_m.h) */
    NonMaskableInt_IRQn      = -14,
    HardFault_IRQn           = -13,
    MemoryManagement_IRQn    = -12,
    BusFault_IRQn            = -11,
    UsageFault_IRQn          = -10,
    SVCall_IRQn              = -5,
    DebugMonitor_IRQn        = -4,
    PendSV_IRQn              = -2,
    SysTick_IRQn             = -1,
    /* STM32H750 Device-specific Interrupts (0..149, official numbering) */
    WWDG_IRQn                = 0,
    PVD_AVD_IRQn             = 1,
    TAMP_STAMP_IRQn          = 2,
    RTC_WKUP_IRQn            = 3,
    FLASH_IRQn               = 4,
    RCC_IRQn                 = 5,
    EXTI0_IRQn               = 6,
    EXTI1_IRQn               = 7,
    EXTI2_IRQn               = 8,
    EXTI3_IRQn               = 9,
    EXTI4_IRQn               = 10,
    DMA1_Stream0_IRQn        = 11,
    DMA1_Stream1_IRQn        = 12,
    DMA1_Stream2_IRQn        = 13,
    DMA1_Stream3_IRQn        = 14,
    DMA1_Stream4_IRQn        = 15,
    DMA1_Stream5_IRQn        = 16,
    DMA1_Stream6_IRQn        = 17,
    ADC_IRQn                 = 18,
    FDCAN1_IT0_IRQn          = 19,
    FDCAN2_IT0_IRQn          = 20,
    FDCAN1_IT1_IRQn          = 21,
    FDCAN2_IT1_IRQn          = 22,
    EXTI9_5_IRQn             = 23,
    TIM1_BRK_IRQn            = 24,
    TIM1_UP_IRQn             = 25,
    TIM1_TRG_COM_IRQn        = 26,
    TIM1_CC_IRQn             = 27,
    TIM2_IRQn                = 28,
    TIM3_IRQn                = 29,
    TIM4_IRQn                = 30,
    I2C1_EV_IRQn             = 31,
    I2C1_ER_IRQn             = 32,
    I2C2_EV_IRQn             = 33,
    I2C2_ER_IRQn             = 34,
    SPI1_IRQn                = 35,
    SPI2_IRQn                = 36,
    USART1_IRQn              = 37,
    USART2_IRQn              = 38,
    USART3_IRQn              = 39,
    EXTI15_10_IRQn           = 40,
    RTC_Alarm_IRQn           = 41,
    /* IRQ 42 not implemented on H750 */
    TIM8_BRK_TIM12_IRQn      = 43,
    TIM8_UP_TIM13_IRQn       = 44,
    TIM8_TRG_COM_TIM14_IRQn  = 45,
    TIM8_CC_IRQn             = 46,
    DMA1_Stream7_IRQn        = 47,
    FMC_IRQn                 = 48,
    SDMMC1_IRQn              = 49,
    TIM5_IRQn                = 50,
    SPI3_IRQn                = 51,
    UART4_IRQn               = 52,
    UART5_IRQn               = 53,
    TIM6_DAC_IRQn            = 54,
    TIM7_IRQn                = 55,
    DMA2_Stream0_IRQn        = 56,
    DMA2_Stream1_IRQn        = 57,
    DMA2_Stream2_IRQn        = 58,
    DMA2_Stream3_IRQn        = 59,
    DMA2_Stream4_IRQn        = 60,
    ETH_IRQn                 = 61,
    ETH_WKUP_IRQn            = 62,
    FDCAN_CAL_IRQn           = 63,
    /* IRQs 64..67 not implemented on H750 */
    DMA2_Stream5_IRQn        = 68,
    DMA2_Stream6_IRQn        = 69,
    DMA2_Stream7_IRQn        = 70,
    USART6_IRQn              = 71,
    I2C3_EV_IRQn             = 72,
    I2C3_ER_IRQn             = 73,
    OTG_HS_EP1_OUT_IRQn      = 74,
    OTG_HS_EP1_IN_IRQn       = 75,
    OTG_HS_WKUP_IRQn         = 76,
    OTG_HS_IRQn              = 77,
    DCMI_IRQn                = 78,
    CRYP_IRQn                = 79,
    HASH_RNG_IRQn            = 80,
    FPU_IRQn                 = 81,
    UART7_IRQn               = 82,
    UART8_IRQn               = 83,
    SPI4_IRQn                = 84,
    SPI5_IRQn                = 85,
    SPI6_IRQn                = 86,
    SAI1_IRQn                = 87,
    LTDC_IRQn                = 88,
    LTDC_ER_IRQn             = 89,
    DMA2D_IRQn               = 90,
    SAI2_IRQn                = 91,
    QUADSPI_IRQn             = 92,
    LPTIM1_IRQn              = 93,
    CEC_IRQn                 = 94,
    I2C4_EV_IRQn             = 95,
    I2C4_ER_IRQn             = 96,
    SPDIF_RX_IRQn            = 97,
    OTG_FS_EP1_OUT_IRQn      = 98,
    OTG_FS_EP1_IN_IRQn       = 99,
    OTG_FS_WKUP_IRQn         = 100,
    OTG_FS_IRQn              = 101,
    DMAMUX1_OVR_IRQn         = 102,
    HRTIM1_MASTER_IRQn       = 103,
    HRTIM1_TIMA_IRQn         = 104,
    HRTIM1_TIMB_IRQn         = 105,
    HRTIM1_TIMC_IRQn         = 106,
    HRTIM1_TIMD_IRQn         = 107,
    HRTIM1_TIME_IRQn         = 108,
    HRTIM1_FLT_IRQn          = 109,
    DFSDM1_FLT0_IRQn         = 110,
    DFSDM1_FLT1_IRQn         = 111,
    DFSDM1_FLT2_IRQn         = 112,
    DFSDM1_FLT3_IRQn         = 113,
    SAI3_IRQn                = 114,
    SWPMI1_IRQn              = 115,
    TIM15_IRQn               = 116,
    TIM16_IRQn               = 117,
    TIM17_IRQn               = 118,
    MDIOS_WKUP_IRQn          = 119,
    MDIOS_IRQn               = 120,
    JPEG_IRQn                = 121,
    MDMA_IRQn                = 122,
    /* IRQ 123 not implemented on H750 */
    SDMMC2_IRQn              = 124,
    HSEM1_IRQn               = 125,
    /* IRQ 126 not implemented on H750 */
    ADC3_IRQn                = 127,
    DMAMUX2_OVR_IRQn         = 128,
    BDMA_Channel0_IRQn       = 129,
    BDMA_Channel1_IRQn       = 130,
    BDMA_Channel2_IRQn       = 131,
    BDMA_Channel3_IRQn       = 132,
    BDMA_Channel4_IRQn       = 133,
    BDMA_Channel5_IRQn       = 134,
    BDMA_Channel6_IRQn       = 135,
    BDMA_Channel7_IRQn       = 136,
    COMP_IRQn                = 137,
    LPTIM2_IRQn              = 138,
    LPTIM3_IRQn              = 139,
    LPTIM4_IRQn              = 140,
    LPTIM5_IRQn              = 141,
    LPUART1_IRQn             = 142,
    /* IRQ 143 not implemented on H750 */
    CRS_IRQn                 = 144,
    ECC_IRQn                 = 145,
    SAI4_IRQn                = 146,
    /* IRQs 147,148 not implemented on H750 */
    WAKEUP_PIN_IRQn          = 149,
} IRQn_Type;
/** @} */

#include "core_cm7.h"

/* ======================== Peripheral Base Addresses ======================== */

#define PERIPH_BASE          0x40000000UL
#define APB1PERIPH_BASE      PERIPH_BASE
#define APB2PERIPH_BASE      (PERIPH_BASE + 0x00010000UL)
#define AHB1PERIPH_BASE      (PERIPH_BASE + 0x00020000UL)
#define AHB2PERIPH_BASE      (PERIPH_BASE + 0x10000000UL)
#define APB3PERIPH_BASE      (PERIPH_BASE + 0x11000000UL)
#define AHB3PERIPH_BASE      (PERIPH_BASE + 0x12000000UL)
#define APB4PERIPH_BASE      (PERIPH_BASE + 0x18000000UL)
#define AHB4PERIPH_BASE      (PERIPH_BASE + 0x18020000UL)

/* D2 domain (APB1 low word) */
#define TIM2_BASE           (APB1PERIPH_BASE + 0x0000UL)
#define TIM3_BASE           (APB1PERIPH_BASE + 0x0400UL)
#define TIM4_BASE           (APB1PERIPH_BASE + 0x0800UL)
#define TIM5_BASE           (APB1PERIPH_BASE + 0x0C00UL)
#define TIM6_BASE           (APB1PERIPH_BASE + 0x1000UL)
#define TIM7_BASE           (APB1PERIPH_BASE + 0x1400UL)
#define TIM12_BASE          (APB1PERIPH_BASE + 0x1800UL)
#define TIM13_BASE          (APB1PERIPH_BASE + 0x1C00UL)
#define TIM14_BASE          (APB1PERIPH_BASE + 0x2000UL)
#define LPTIM1_BASE         (APB1PERIPH_BASE + 0x2400UL)
#define SPI2_BASE           (APB1PERIPH_BASE + 0x3800UL)
#define SPI3_BASE           (APB1PERIPH_BASE + 0x3C00UL)
#define USART2_BASE         (APB1PERIPH_BASE + 0x4400UL)
#define USART3_BASE         (APB1PERIPH_BASE + 0x4800UL)
#define UART4_BASE          (APB1PERIPH_BASE + 0x4C00UL)
#define UART5_BASE          (APB1PERIPH_BASE + 0x5000UL)
#define I2C1_BASE           (APB1PERIPH_BASE + 0x5400UL)
#define I2C2_BASE           (APB1PERIPH_BASE + 0x5800UL)
#define I2C3_BASE           (APB1PERIPH_BASE + 0x5C00UL)
#define DAC1_BASE           (APB1PERIPH_BASE + 0x7400UL)
#define UART7_BASE          (APB1PERIPH_BASE + 0x7800UL)
#define UART8_BASE          (APB1PERIPH_BASE + 0x7C00UL)

/* D2 domain (APB2) */
#define TIM1_BASE           (APB2PERIPH_BASE + 0x0000UL)
#define TIM8_BASE           (APB2PERIPH_BASE + 0x0400UL)
#define USART1_BASE         (APB2PERIPH_BASE + 0x1000UL)
#define USART6_BASE         (APB2PERIPH_BASE + 0x1400UL)
#define SPI1_BASE           (APB2PERIPH_BASE + 0x3000UL)
#define SPI4_BASE           (APB2PERIPH_BASE + 0x3400UL)
#define TIM15_BASE          (APB2PERIPH_BASE + 0x4000UL)
#define TIM16_BASE          (APB2PERIPH_BASE + 0x4400UL)
#define TIM17_BASE          (APB2PERIPH_BASE + 0x4800UL)
#define SPI5_BASE           (APB2PERIPH_BASE + 0x5000UL)

/* D2 domain (AHB1) */
#define DMA1_BASE           (AHB1PERIPH_BASE + 0x0000UL)
#define DMA2_BASE           (AHB1PERIPH_BASE + 0x0400UL)
#define DMAMUX1_BASE        (AHB1PERIPH_BASE + 0x0800UL)
#define ADC1_BASE           (AHB1PERIPH_BASE + 0x2000UL)
#define ADC2_BASE           (AHB1PERIPH_BASE + 0x2100UL)
#define ADC12_COMMON_BASE   (AHB1PERIPH_BASE + 0x2300UL)

/* D3 domain (AHB1 @ 0x58020000): ADC3 + its common block */
#define ADC3_BASE           0x58026000UL
#define ADC3_COMMON_BASE    0x58026300UL

/* D1 domain (AHB3) */
#define FLASH_R_BASE        (AHB3PERIPH_BASE + 0x2000UL)  /* FLASH controller */
#define QSPI_R_BASE         (AHB3PERIPH_BASE + 0x5000UL)  /* QUADSPI         */
#define SDMMC1_BASE         (AHB3PERIPH_BASE + 0x7000UL)
/* SDMMC2: D3 AHB4 @ 0x48022400 — not contiguous with SDMMC1 */
#define SDMMC2_BASE         0x48022400UL

/* USB OTG */
#define USB_OTG_FS_PERIPH_BASE  0x50000000UL
#define USB_OTG_HS_PERIPH_BASE  0x40040000UL

#define USB_OTG_GLOBAL_BASE     0x000UL
#define USB_OTG_DEVICE_BASE     0x800UL
#define USB_OTG_IN_ENDPOINT_BASE    0x900UL
#define USB_OTG_OUT_ENDPOINT_BASE   0xB00UL
#define USB_OTG_EP_REG_SIZE     0x20UL
#define USB_OTG_HOST_BASE       0x400UL
#define USB_OTG_HOST_PORT_BASE  0x440UL
#define USB_OTG_HOST_CHANNEL_BASE    0x500UL
#define USB_OTG_HOST_CHANNEL_SIZE    0x20UL
#define USB_OTG_PCGCCTL_BASE    0xE00UL
#define USB_OTG_FIFO_BASE       0x1000UL
#define USB_OTG_FIFO_SIZE       0x1000UL

/* D3 domain (APB4) */
#define EXTI_BASE           (APB4PERIPH_BASE + 0x0000UL)
#define SYSCFG_BASE         (APB4PERIPH_BASE + 0x0400UL)
#define LPUART1_BASE        (APB4PERIPH_BASE + 0x0C00UL)
#define SPI6_BASE           (APB4PERIPH_BASE + 0x1400UL)
#define I2C4_BASE           (APB4PERIPH_BASE + 0x1C00UL)
#define RTC_BASE            (APB4PERIPH_BASE + 0x4000UL)
#define IWDG1_BASE          (APB4PERIPH_BASE + 0x4800UL)
#define IWDG2_BASE          (APB4PERIPH_BASE + 0x4C00UL)
#define WWDG1_BASE          (APB4PERIPH_BASE + 0x2C00UL)

/* D3 domain (AHB4) */
#define GPIOA_BASE          (AHB4PERIPH_BASE + 0x0000UL)
#define GPIOB_BASE          (AHB4PERIPH_BASE + 0x0400UL)
#define GPIOC_BASE          (AHB4PERIPH_BASE + 0x0800UL)
#define GPIOD_BASE          (AHB4PERIPH_BASE + 0x0C00UL)
#define GPIOE_BASE          (AHB4PERIPH_BASE + 0x1000UL)
#define GPIOF_BASE          (AHB4PERIPH_BASE + 0x1400UL)
#define GPIOG_BASE          (AHB4PERIPH_BASE + 0x1800UL)
#define GPIOH_BASE          (AHB4PERIPH_BASE + 0x1C00UL)
#define GPIOI_BASE          (AHB4PERIPH_BASE + 0x2000UL)
#define GPIOJ_BASE          (AHB4PERIPH_BASE + 0x2400UL)
#define GPIOK_BASE          (AHB4PERIPH_BASE + 0x2800UL)
#define RCC_BASE            (AHB4PERIPH_BASE + 0x4400UL)
#define PWR_BASE            (AHB4PERIPH_BASE + 0x4800UL)
#define CRC_BASE            (AHB4PERIPH_BASE + 0x4C00UL)
/* RNG: real H750 D3 AHB2 base (RM0433). The minimal-header AHB2PERIPH_BASE
 * above is a placeholder — nothing else maps through it — so RNG pins its own
 * silicon address. */
#define RNG_BASE            0x48021800UL
#define BDMA_BASE           (AHB4PERIPH_BASE + 0x5400UL)

/* DMA channel bases: 8 channels per controller, 0x20 apart, channel 0 at +0x00.
 * (H7 DMA1/2 are CHANNEL-based — unlike the F4 stream layout — and each channel
 *  selects its peripheral request via the DMAMUX1 request-ID register.) */
#define DMA1_Channel0_BASE  (DMA1_BASE + 0x020UL)
#define DMA1_Channel1_BASE  (DMA1_BASE + 0x040UL)
#define DMA1_Channel2_BASE  (DMA1_BASE + 0x060UL)
#define DMA1_Channel3_BASE  (DMA1_BASE + 0x080UL)
#define DMA1_Channel4_BASE  (DMA1_BASE + 0x0A0UL)
#define DMA1_Channel5_BASE  (DMA1_BASE + 0x0C0UL)
#define DMA1_Channel6_BASE  (DMA1_BASE + 0x0E0UL)
#define DMA1_Channel7_BASE  (DMA1_BASE + 0x100UL)
#define DMA2_Channel0_BASE  (DMA2_BASE + 0x020UL)
#define DMA2_Channel1_BASE  (DMA2_BASE + 0x040UL)
#define DMA2_Channel2_BASE  (DMA2_BASE + 0x060UL)
#define DMA2_Channel3_BASE  (DMA2_BASE + 0x080UL)
#define DMA2_Channel4_BASE  (DMA2_BASE + 0x0A0UL)
#define DMA2_Channel5_BASE  (DMA2_BASE + 0x0C0UL)
#define DMA2_Channel6_BASE  (DMA2_BASE + 0x0E0UL)
#define DMA2_Channel7_BASE  (DMA2_BASE + 0x100UL)

/* ======================== Peripheral Register Structures ======================== */

/* ---- GPIO (F4-identical layout) ---- */
typedef struct {
    volatile uint32_t MODER;   /* 0x00: Mode register                */
    volatile uint32_t OTYPER;  /* 0x04: Output type register         */
    volatile uint32_t OSPEEDR; /* 0x08: Output speed register        */
    volatile uint32_t PUPDR;   /* 0x0C: Pull-up/pull-down register   */
    volatile uint32_t IDR;     /* 0x10: Input data register          */
    volatile uint32_t ODR;     /* 0x14: Output data register         */
    volatile uint32_t BSRR;    /* 0x18: Bit set/reset register       */
    volatile uint32_t LCKR;    /* 0x1C: Lock register                */
    volatile uint32_t AFR[2];  /* 0x20: Alternate function low/high  */
} GPIO_TypeDef;

/* ---- USART (H7/F7 style: ISR/ICR/RDR/TDR, no SR) ---- */
typedef struct {
    volatile uint32_t CR1;     /* 0x00: Control register 1           */
    volatile uint32_t CR2;     /* 0x04: Control register 2           */
    volatile uint32_t CR3;     /* 0x08: Control register 3           */
    volatile uint32_t BRR;     /* 0x0C: Baud rate register           */
    volatile uint32_t GTPR;    /* 0x10: Guard time and prescaler     */
    volatile uint32_t RTOR;    /* 0x14: Receiver timeout register    */
    volatile uint32_t RQR;     /* 0x18: Request register             */
    volatile uint32_t ISR;     /* 0x1C: Interrupt/status register    */
    volatile uint32_t ICR;     /* 0x20: Interrupt flag clear         */
    volatile uint32_t RDR;     /* 0x24: Receive data register        */
    volatile uint32_t TDR;     /* 0x28: Transmit data register       */
    volatile uint32_t PRESC;   /* 0x2C: Clock prescaler              */
} USART_TypeDef;

/* ---- RCC (H7 D1/D2/D3 domain layout) ---- */
typedef struct {
    volatile uint32_t CR;      /* 0x00: Clock control register       */
    volatile uint32_t HSICFGR; /* 0x04: HSI config                   */
    volatile uint32_t CRRCR;   /* 0x08: Core reset clock control     */
    volatile uint32_t CSICFGR; /* 0x0C: CSI config                   */
    volatile uint32_t CFGR;    /* 0x10: Clock configuration          */
    volatile uint32_t RES1;    /* 0x14: Reserved                     */
    volatile uint32_t D1CFGR;  /* 0x18: D1 domain config             */
    volatile uint32_t D2CFGR;  /* 0x1C: D2 domain config             */
    volatile uint32_t D3CFGR;  /* 0x20: D3 domain config             */
    volatile uint32_t RES2;    /* 0x24: Reserved                     */
    volatile uint32_t PLLCKSELR; /* 0x28: PLL clock source selection */
    volatile uint32_t PLLCFGR; /* 0x2C: PLL config                   */
    volatile uint32_t PLL1DIVR; /* 0x30: PLL1 dividers               */
    volatile uint32_t PLL1FRACR; /* 0x34: PLL1 fractional            */
    volatile uint32_t PLL2DIVR; /* 0x38: PLL2 dividers               */
    volatile uint32_t PLL2FRACR; /* 0x3C: PLL2 fractional            */
    volatile uint32_t PLL3DIVR; /* 0x40: PLL3 dividers               */
    volatile uint32_t PLL3FRACR; /* 0x44: PLL3 fractional            */
    volatile uint32_t RES3;    /* 0x48: Reserved                     */
    volatile uint32_t D1CCIPR; /* 0x4C: D1 kernel clock config       */
    volatile uint32_t D2CCIP1R; /* 0x50: D2 kernel clock config 1    */
    volatile uint32_t D2CCIP2R; /* 0x54: D2 kernel clock config 2    */
    volatile uint32_t D3CCIPR; /* 0x58: D3 kernel clock config       */
    volatile uint32_t RES4;    /* 0x5C: Reserved                     */
    volatile uint32_t CIER;    /* 0x60: Clock interrupt enable       */
    volatile uint32_t CIFR;    /* 0x64: Clock interrupt flag         */
    volatile uint32_t CICR;    /* 0x68: Clock interrupt clear        */
    volatile uint32_t RES5;    /* 0x6C: Reserved                     */
    volatile uint32_t BDCR;    /* 0x70: Backup domain control        */
    volatile uint32_t CSR;     /* 0x74: Control/status register      */
    volatile uint32_t RES6;    /* 0x78: Reserved                     */
    volatile uint32_t AHB3RSTR; /* 0x7C: AHB3 reset                  */
    volatile uint32_t AHB1RSTR; /* 0x80: AHB1 reset                  */
    volatile uint32_t AHB2RSTR; /* 0x84: AHB2 reset                  */
    volatile uint32_t AHB4RSTR; /* 0x88: AHB4 reset                  */
    volatile uint32_t APB3RSTR; /* 0x8C: APB3 reset                  */
    volatile uint32_t APB1LRSTR; /* 0x90: APB1 low reset              */
    volatile uint32_t APB1HRSTR; /* 0x94: APB1 high reset             */
    volatile uint32_t APB2RSTR; /* 0x98: APB2 reset                  */
    volatile uint32_t APB4RSTR; /* 0x9C: APB4 reset                  */
    volatile uint32_t GCR;     /* 0xA0: Global config                */
    volatile uint32_t RES7;    /* 0xA4: Reserved                     */
    volatile uint32_t D3AMR;   /* 0xA8: D3 autonomous mode           */
    volatile uint32_t RES8[9]; /* 0xAC-0xCC: Reserved                */
    volatile uint32_t RSR;     /* 0xD0: Reset status register        */
    volatile uint32_t AHB3ENR; /* 0xD4: AHB3 clock enable            */
    volatile uint32_t AHB1ENR; /* 0xD8: AHB1 clock enable            */
    volatile uint32_t AHB2ENR; /* 0xDC: AHB2 clock enable            */
    volatile uint32_t AHB4ENR; /* 0xE0: AHB4 clock enable            */
    volatile uint32_t APB3ENR; /* 0xE4: APB3 clock enable            */
    volatile uint32_t APB1LENR; /* 0xE8: APB1 low clock enable        */
    volatile uint32_t APB1HENR; /* 0xEC: APB1 high clock enable       */
    volatile uint32_t APB2ENR; /* 0xF0: APB2 clock enable            */
    volatile uint32_t APB4ENR; /* 0xF4: APB4 clock enable            */
} RCC_TypeDef;

/* ---- FLASH (H7 single-bank controller) ---- */
typedef struct {
    volatile uint32_t ACR;      /* 0x00: Access control              */
    volatile uint32_t KEYR1;    /* 0x04: Key register 1              */
    volatile uint32_t OPTKEYR;  /* 0x08: Option key register         */
    volatile uint32_t CR1;      /* 0x0C: Control register 1          */
    volatile uint32_t SR1;      /* 0x10: Status register 1           */
    volatile uint32_t CCR1;     /* 0x14: Clear control register 1    */
    volatile uint32_t OPTCR;    /* 0x18: Option control register     */
    volatile uint32_t OPTSR_CUR;/* 0x1C: Option status current       */
    volatile uint32_t OPTSR_PRG;/* 0x20: Option status program       */
} FLASH_TypeDef;

/* ---- PWR (H7 D3-domain layout) ---- */
typedef struct {
    volatile uint32_t CR1;      /* 0x00: Power control 1             */
    volatile uint32_t CSR1;     /* 0x04: Power control/status 1      */
    volatile uint32_t CR2;      /* 0x08: Power control 2             */
    volatile uint32_t CR3;      /* 0x0C: Power control 3             */
    volatile uint32_t CPUCR;    /* 0x10: CPU control                 */
    volatile uint32_t RES1;     /* 0x14: Reserved                    */
    volatile uint32_t D3CR;     /* 0x18: D3 domain control           */
    volatile uint32_t WKUPCR;   /* 0x1C: Wakeup clear                */
    volatile uint32_t WKUPFR;   /* 0x20: Wakeup flag                 */
    volatile uint32_t WKUPEPR;  /* 0x24: Wakeup enable/polarity      */
} PWR_TypeDef;

/* ---- EXTI (H7 dual-block; block1 covers lines 0..31) ---- */
typedef struct {
    volatile uint32_t RTSR1;    /* 0x00: Rising trigger select 1     */
    volatile uint32_t FTSR1;    /* 0x04: Falling trigger select 1    */
    volatile uint32_t SWIER1;   /* 0x08: Software interrupt event 1  */
    volatile uint32_t D3PMR1;   /* 0x0C: D3 pending mask 1           */
    volatile uint32_t D3PCR1L;  /* 0x10: D3 pending clear low 1      */
    volatile uint32_t D3PCR1H;  /* 0x14: D3 pending clear high 1     */
    volatile uint32_t RES1[2];  /* 0x18-0x1C: Reserved               */
    volatile uint32_t RTSR2;    /* 0x20: Rising trigger select 2     */
    volatile uint32_t FTSR2;    /* 0x24: Falling trigger select 2    */
    volatile uint32_t SWIER2;   /* 0x28: Software interrupt event 2  */
    volatile uint32_t D3PMR2;   /* 0x2C: D3 pending mask 2           */
    volatile uint32_t D3PCR2L;  /* 0x30: D3 pending clear low 2      */
    volatile uint32_t D3PCR2H;  /* 0x34: D3 pending clear high 2     */
    volatile uint32_t RES2[2];  /* 0x38-0x3C: Reserved               */
    volatile uint32_t RTSR3;    /* 0x40: Rising trigger select 3     */
    volatile uint32_t FTSR3;    /* 0x44: Falling trigger select 3    */
    volatile uint32_t SWIER3;   /* 0x48: Software interrupt event 3  */
    volatile uint32_t D3PMR3;   /* 0x4C: D3 pending mask 3           */
    volatile uint32_t D3PCR3L;  /* 0x50: D3 pending clear low 3      */
    volatile uint32_t D3PCR3H;  /* 0x54: D3 pending clear high 3     */
    volatile uint32_t RES3[10]; /* 0x58-0x7C: Reserved               */
    volatile uint32_t IMR1;     /* 0x80: Interrupt mask 1            */
    volatile uint32_t EMR1;     /* 0x84: Event mask 1                */
    volatile uint32_t PR1;      /* 0x88: Pending register 1 (W1C)    */
    volatile uint32_t RES4;     /* 0x8C: Reserved                    */
    volatile uint32_t IMR2;     /* 0x90: Interrupt mask 2            */
    volatile uint32_t EMR2;     /* 0x94: Event mask 2                */
    volatile uint32_t PR2;      /* 0x98: Pending register 2 (W1C)    */
    volatile uint32_t RES5;     /* 0x9C: Reserved                    */
    volatile uint32_t IMR3;     /* 0xA0: Interrupt mask 3            */
    volatile uint32_t EMR3;     /* 0xA4: Event mask 3                */
    volatile uint32_t PR3;      /* 0xA8: Pending register 3 (W1C)    */
} EXTI_TypeDef;

/* ---- SYSCFG ---- */
typedef struct {
    volatile uint32_t MEMRMP;   /* 0x00: Memory remap                */
    volatile uint32_t CFGR1;    /* 0x04: Config 1                    */
    volatile uint32_t CFGR2;    /* 0x08: Config 2                    */
    volatile uint32_t CFGR3;    /* 0x0C: Config 3                    */
    volatile uint32_t PMCR;     /* 0x10: Ports mode config           */
    volatile uint32_t RES1;     /* 0x14: Reserved                    */
    volatile uint32_t UR;       /* 0x18: User register               */
} SYSCFG_TypeDef;

/* ---- IWDG ---- */
typedef struct {
    volatile uint32_t KR;       /* 0x00: Key register                */
    volatile uint32_t PR;       /* 0x04: Prescaler register          */
    volatile uint32_t RLR;      /* 0x08: Reload register             */
    volatile uint32_t SR;       /* 0x0C: Status register             */
    volatile uint32_t WINR;     /* 0x10: Window register             */
} IWDG_TypeDef;

/* ---- WWDG (F4-identical layout; H7: WWDG1 in D3 APB4 domain) ---- */
typedef struct {
    volatile uint32_t CR;       /* 0x00: Control register            */
    volatile uint32_t CFR;      /* 0x04: Configuration register      */
    volatile uint32_t SR;       /* 0x08: Status register             */
} WWDG_TypeDef;

/* ---- CRC ---- */
typedef struct {
    volatile uint32_t DR;       /* 0x00: Data register               */
    volatile uint32_t IDR;      /* 0x04: Independent data register   */
    volatile uint32_t CR;       /* 0x08: Control register            */
    volatile uint32_t RES1;     /* 0x0C: Reserved                    */
    volatile uint32_t INIT;     /* 0x10: Initial CRC value           */
    volatile uint32_t POL;      /* 0x14: Polynomial                  */
} CRC_TypeDef;

/* ---- RNG (CR/SR/DR F4-identical; H7 adds HTCFG/HTCR tail) ---- */
typedef struct {
    volatile uint32_t CR;       /* 0x00: Control register            */
    volatile uint32_t SR;       /* 0x04: Status register             */
    volatile uint32_t DR;       /* 0x08: Data register               */
    volatile uint32_t HTCFG;    /* 0x0C: Health-test config          */
    volatile uint32_t HTCR;     /* 0x10: Health-test control         */
} RNG_TypeDef;

/* ---- DAC (F4-identical layout; H7 DAC1 in D2 APB1 @ 0x40007400) ---- */
typedef struct {
    volatile uint32_t CR;       /* 0x00: Control register            */
    volatile uint32_t SWTRIGR;  /* 0x04: Software trigger register   */
    volatile uint32_t DHR12R1;  /* 0x08: DHR 12-bit right-aligned 1 */
    volatile uint32_t DHR12L1;  /* 0x0C: DHR 12-bit left-aligned 1  */
    volatile uint32_t DHR8R1;   /* 0x10: DHR 8-bit right-aligned 1  */
    volatile uint32_t DHR12R2;  /* 0x14: DHR 12-bit right-aligned 2 */
    volatile uint32_t DHR12L2;  /* 0x18: DHR 12-bit left-aligned 2  */
    volatile uint32_t DHR8R2;   /* 0x1C: DHR 8-bit right-aligned 2  */
    volatile uint32_t DHR12RD;  /* 0x20: DHR 12-bit dual            */
    volatile uint32_t DHR12LD;  /* 0x24: DHR 12-bit dual left       */
    volatile uint32_t DHR8RD;   /* 0x28: DHR 8-bit dual             */
    volatile uint32_t DOR1;     /* 0x2C: Data output 1              */
    volatile uint32_t DOR2;     /* 0x30: Data output 2              */
    volatile uint32_t SR;       /* 0x34: Status register             */
} DAC_TypeDef;

/* ---- RTC (F4-style) ---- */
typedef struct {
    volatile uint32_t TR;       /* 0x00: Time register               */
    volatile uint32_t DR;       /* 0x04: Date register               */
    volatile uint32_t SSR;      /* 0x08: Sub-second register         */
    volatile uint32_t ICSR;     /* 0x0C: Init/status register        */
    volatile uint32_t PRER;     /* 0x10: Prescaler register          */
    volatile uint32_t WUTR;     /* 0x14: Wakeup timer register       */
    volatile uint32_t CALIBR;   /* 0x18: Calibration register        */
    volatile uint32_t ALRMAR;   /* 0x1C: Alarm A register            */
    volatile uint32_t ALRMBR;   /* 0x20: Alarm B register            */
    volatile uint32_t WPR;      /* 0x24: Write protection register   */
    volatile uint32_t CALR;     /* 0x28: Calibration register        */
    volatile uint32_t SHIFTR;   /* 0x2C: Shift control register      */
    volatile uint32_t TSTR;     /* 0x30: Timestamp time register     */
    volatile uint32_t TSDR;     /* 0x34: Timestamp date register     */
    volatile uint32_t TSSSR;    /* 0x38: Timestamp sub-second        */
    volatile uint32_t RES1;     /* 0x3C: Reserved                    */
    volatile uint32_t ALRMASSR; /* 0x40: Alarm A sub-second          */
    volatile uint32_t ALRMBSSR; /* 0x44: Alarm B sub-second          */
} RTC_TypeDef;

/* ---- QUADSPI ---- */
typedef struct {
    volatile uint32_t CR;       /* 0x00: Control register            */
    volatile uint32_t DCR;      /* 0x04: Device config register      */
    volatile uint32_t SR;       /* 0x08: Status register             */
    volatile uint32_t FCR;      /* 0x0C: Flag clear register         */
    volatile uint32_t DLR;      /* 0x10: Data length register        */
    volatile uint32_t CCR;      /* 0x14: Communication config reg    */
    volatile uint32_t AR;       /* 0x18: Address register            */
    volatile uint32_t ABR;      /* 0x1C: Alternate bytes register    */
    volatile uint32_t DR;       /* 0x20: Data register               */
    volatile uint32_t PSMKR;    /* 0x24: Polling status mask reg     */
    volatile uint32_t PSMAR;    /* 0x28: Polling status match reg    */
    volatile uint32_t PIR;      /* 0x2C: Polling interval register   */
    volatile uint32_t LPTR;     /* 0x30: Low-power timeout register  */
} QUADSPI_TypeDef;

/* ---- DMA (channel-based, H7 layout) ---- */
typedef struct {
    volatile uint32_t CCR;      /* 0x00: Channel config              */
    volatile uint32_t CNDTR;    /* 0x04: Number of data              */
    volatile uint32_t CPAR;     /* 0x08: Peripheral address          */
    volatile uint32_t CM0AR;    /* 0x0C: Memory 0 address            */
    volatile uint32_t CM1AR;    /* 0x10: Memory 1 address            */
    volatile uint32_t RESERVED0[3]; /* 0x14-0x1C: pad to 0x20 stride */
} DMA_Channel_TypeDef;

typedef struct {
    volatile uint32_t LISR;     /* 0x00: Low interrupt status        */
    volatile uint32_t HISR;     /* 0x04: High interrupt status       */
    volatile uint32_t LIFCR;    /* 0x08: Low interrupt flag clear    */
    volatile uint32_t HIFCR;    /* 0x0C: High interrupt flag clear   */
    DMA_Channel_TypeDef S[8];   /* 0x20: S[0]@0x20, S[1]@0x40, ...  */
} DMA_TypeDef;

/* ---- DMAMUX1 ---- */
typedef struct {
    volatile uint32_t CCR;      /* 0x00: Channel config (DMAREQ_ID[6:0], SOIE=16, EGE=17) */
} DMAMUX_Channel_TypeDef;

typedef struct {
    DMAMUX_Channel_TypeDef C[16]; /* 0x000..0x03C */
    volatile uint32_t RGSR;     /* 0x080 */
    volatile uint32_t RGCFR;    /* 0x084 */
} DMAMUX_TypeDef;

/* ---- SDMMC (H7, compatible with F4 SDIO register layout) ---- */
typedef struct {
    volatile uint32_t POWER;    /* 0x00: Power control              */
    volatile uint32_t CLKCR;    /* 0x04: Clock control              */
    volatile uint32_t ARG;      /* 0x08: Argument register          */
    volatile uint32_t CMD;      /* 0x0C: Command register           */
    volatile const uint32_t RESPCMD; /* 0x10: Command response       */
    volatile const uint32_t RESP1;   /* 0x14: Response 1            */
    volatile const uint32_t RESP2;   /* 0x18: Response 2            */
    volatile const uint32_t RESP3;   /* 0x1C: Response 3            */
    volatile const uint32_t RESP4;   /* 0x20: Response 4            */
    volatile uint32_t DTIMER;  /* 0x24: Data timer                  */
    volatile uint32_t DLEN;    /* 0x28: Data length                 */
    volatile uint32_t DCTRL;   /* 0x2C: Data control                */
    volatile const uint32_t DCOUNT;  /* 0x30: Data counter           */
    volatile const uint32_t STA;     /* 0x34: Status register        */
    volatile uint32_t ICR;     /* 0x38: Interrupt clear             */
    volatile uint32_t MASK;    /* 0x3C: Mask register               */
    uint32_t RESERVED0[2];     /* 0x40-0x44                        */
    volatile const uint32_t FIFOCNT; /* 0x48: FIFO counter          */
    uint32_t RESERVED1[13];    /* 0x4C-0x7C                        */
    volatile uint32_t FIFO;    /* 0x80: FIFO data                  */
} SDMMC_TypeDef;

/* ---- ADC (H7 V5 layout: 16-bit, DMNGT data management) ----
 * NOTE: this is the NEW silicon layout (PCSEL/LTR1/HTR1, JSQR@0x4C,
 * OFR@0x60, JDR@0x80) used by current STM32H750 parts and the official
 * CMSIS headers since CubeH7 v1.11. The legacy RM0433 Rev7 layout
 * (TR1-3, JSQR@0x80) applies to old Y-revision silicon only. */
typedef struct {
    volatile uint32_t ISR;      /* 0x00: Interrupt and status        */
    volatile uint32_t IER;      /* 0x04: Interrupt enable            */
    volatile uint32_t CR;       /* 0x08: Control                     */
    volatile uint32_t CFGR;     /* 0x0C: Configuration               */
    volatile uint32_t CFGR2;    /* 0x10: Configuration 2             */
    volatile uint32_t SMPR1;    /* 0x14: Sample time 1 (ch 0-9)      */
    volatile uint32_t SMPR2;    /* 0x18: Sample time 2 (ch 10-18)    */
    volatile uint32_t PCSEL;    /* 0x1C: Pre-channel selection       */
    volatile uint32_t LTR1;     /* 0x20: Watchdog 1 lower threshold  */
    volatile uint32_t HTR1;     /* 0x24: Watchdog 1 higher threshold */
    uint32_t RESERVED28;        /* 0x28                             */
    uint32_t RESERVED2C;        /* 0x2C                             */
    volatile uint32_t SQR1;     /* 0x30: Regular sequence 1          */
    volatile uint32_t SQR2;     /* 0x34: Regular sequence 2          */
    volatile uint32_t SQR3;     /* 0x38: Regular sequence 3          */
    volatile uint32_t SQR4;     /* 0x3C: Regular sequence 4          */
    volatile uint32_t DR;       /* 0x40: Regular data                */
    uint32_t RESERVED44[2];     /* 0x44-0x48                        */
    volatile uint32_t JSQR;     /* 0x4C: Injected sequence           */
    uint32_t RESERVED50[4];     /* 0x50-0x5C                        */
    volatile uint32_t OFR1;     /* 0x60: Offset 1                    */
    volatile uint32_t OFR2;     /* 0x64: Offset 2                    */
    volatile uint32_t OFR3;     /* 0x68: Offset 3                    */
    volatile uint32_t OFR4;     /* 0x6C: Offset 4                    */
    uint32_t RESERVED70[4];     /* 0x70-0x7C                        */
    volatile uint32_t JDR1;     /* 0x80: Injected data 1             */
    volatile uint32_t JDR2;     /* 0x84: Injected data 2             */
    volatile uint32_t JDR3;     /* 0x88: Injected data 3             */
    volatile uint32_t JDR4;     /* 0x8C: Injected data 4             */
    uint32_t RESERVED90[4];     /* 0x90-0x9C                        */
    volatile uint32_t AWD2CR;   /* 0xA0: Watchdog 2 config           */
    volatile uint32_t AWD3CR;   /* 0xA4: Watchdog 3 config           */
    uint32_t RESERVEDA8[2];     /* 0xA8-0xAC                        */
    volatile uint32_t LTR2;     /* 0xB0: Watchdog 2 lower threshold  */
    volatile uint32_t HTR2;     /* 0xB4: Watchdog 2 higher threshold */
    volatile uint32_t LTR3;     /* 0xB8: Watchdog 3 lower threshold  */
    volatile uint32_t HTR3;     /* 0xBC: Watchdog 3 higher threshold */
    volatile uint32_t DIFSEL;   /* 0xC0: Differential mode selection */
    volatile uint32_t CALFACT;  /* 0xC4: Calibration factors         */
    volatile uint32_t CALFACT2; /* 0xC8: Linearity calibration       */
} ADC_TypeDef;

typedef struct {
    volatile uint32_t CSR;      /* 0x300: Common status              */
    uint32_t RESERVED304;       /* 0x304                            */
    volatile uint32_t CCR;      /* 0x308: Common control             */
    volatile uint32_t CDR;      /* 0x30C: Common data (dual mode)    */
    volatile uint32_t CDR2;     /* 0x310: Common data (32-bit dual)  */
} ADC_Common_TypeDef;

/* ---- USB OTG (FS/HS) ---- */
typedef struct {
    volatile uint32_t GOTGCTL;          /* 0x00: Control and Status            */
    volatile uint32_t GOTGINT;          /* 0x04: Interrupt                     */
    volatile uint32_t GAHBCFG;          /* 0x08: AHB Configuration             */
    volatile uint32_t GUSBCFG;          /* 0x0C: USB Configuration             */
    volatile uint32_t GRSTCTL;          /* 0x10: Reset                         */
    volatile uint32_t GINTSTS;          /* 0x14: Interrupt status              */
    volatile uint32_t GINTMSK;          /* 0x18: Interrupt mask                */
    volatile uint32_t GRXSTSR;          /* 0x1C: Receive status read           */
    volatile uint32_t GRXSTSP;          /* 0x20: Receive status pop            */
    volatile uint32_t GRXFSIZ;          /* 0x24: Receive FIFO size             */
    volatile uint32_t DIEPTXF0_HNPTXFSIZ; /* 0x28: EP0 / Non-periodic TX FIFO  */
    volatile uint32_t HNPTXSTS;         /* 0x2C: Non-periodic TX FIFO status   */
    uint32_t Reserved30[2];             /* 0x30-0x34                          */
    volatile uint32_t GCCFG;            /* 0x38: General purpose config        */
    volatile uint32_t CID;              /* 0x3C: User ID                       */
    uint32_t  Reserved40[48];           /* 0x40-0xFF                          */
    volatile uint32_t HPTXFSIZ;         /* 0x100: Host periodic TX FIFO size   */
    volatile uint32_t DIEPTXF[15];      /* 0x104-0x144: Dev periodic TX FIFO   */
} USB_OTG_GlobalTypeDef;

typedef struct {
    volatile uint32_t DCFG;             /* 0x800: dev Configuration            */
    volatile uint32_t DCTL;             /* 0x804: dev Control                  */
    volatile uint32_t DSTS;             /* 0x808: dev Status (RO)              */
    uint32_t Reserved0C;
    volatile uint32_t DIEPMSK;          /* 0x810: IN EP mask                   */
    volatile uint32_t DOEPMSK;          /* 0x814: OUT EP mask                  */
    volatile uint32_t DAINT;            /* 0x818: All EP interrupt             */
    volatile uint32_t DAINTMSK;         /* 0x81C: All EP interrupt mask        */
    uint32_t Reserved20;
    uint32_t Reserved9;
    volatile uint32_t DVBUSDIS;         /* 0x828: VBUS discharge               */
    volatile uint32_t DVBUSPULSE;       /* 0x82C: VBUS pulse                   */
    volatile uint32_t DTHRCTL;          /* 0x830: threshold                    */
    volatile uint32_t DIEPEMPMSK;       /* 0x834: IN EP empty mask             */
    volatile uint32_t DEACHINT;         /* 0x838: dedicated EP interrupt       */
    volatile uint32_t DEACHMSK;         /* 0x83C: dedicated EP mask            */
    uint32_t Reserved40;
    volatile uint32_t DINEP1MSK;        /* 0x844: dedicated EP1 IN mask        */
    uint32_t  Reserved44[15];           /* 0x848-0x87C                        */
    volatile uint32_t DOUTEP1MSK;       /* 0x884: dedicated EP1 OUT mask       */
} USB_OTG_DeviceTypeDef;

typedef struct {
    volatile uint32_t DIEPCTL;          /* 0x00: IN EP control                 */
    uint32_t Reserved04;
    volatile uint32_t DIEPINT;          /* 0x08: IN EP interrupt               */
    uint32_t Reserved0C;
    volatile uint32_t DIEPTSIZ;         /* 0x10: IN EP transfer size           */
    volatile uint32_t DIEPDMA;          /* 0x14: IN EP DMA address             */
    volatile uint32_t DTXFSTS;          /* 0x18: IN EP TX FIFO status          */
    uint32_t Reserved18;
} USB_OTG_INEndpointTypeDef;

typedef struct {
    volatile uint32_t DOEPCTL;          /* 0x00: OUT EP control                */
    uint32_t Reserved04;
    volatile uint32_t DOEPINT;          /* 0x08: OUT EP interrupt              */
    uint32_t Reserved0C;
    volatile uint32_t DOEPTSIZ;         /* 0x10: OUT EP transfer size          */
    volatile uint32_t DOEPDMA;          /* 0x14: OUT EP DMA address            */
    uint32_t Reserved18[2];
} USB_OTG_OUTEndpointTypeDef;

/* ---- SPI (H7 v3 layout) ---- */
typedef struct {
    volatile uint32_t CR1;      /* 0x00: Control register 1          */
    volatile uint32_t CR2;      /* 0x04: Control register 2          */
    volatile uint32_t CFG1;     /* 0x08: Config register 1           */
    volatile uint32_t CFG2;     /* 0x0C: Config register 2           */
    volatile uint32_t IER;      /* 0x10: Interrupt enable            */
    volatile uint32_t SR;       /* 0x14: Status register             */
    volatile uint32_t IFCR;     /* 0x18: Interrupt flag clear        */
    volatile uint32_t RES1;     /* 0x1C: Reserved                    */
    volatile uint32_t TXDR;     /* 0x20: Transmit data               */
    volatile uint32_t RES2[3];  /* 0x24-0x2C: Reserved               */
    volatile uint32_t RXDR;     /* 0x30: Receive data                */
    volatile uint32_t RES3[3];  /* 0x34-0x3C: Reserved               */
    volatile uint32_t CRCPOLY;  /* 0x40: CRC polynomial              */
    volatile uint32_t TXCRC;    /* 0x44: TX CRC                      */
    volatile uint32_t RXCRC;    /* 0x48: RX CRC                      */
    volatile uint32_t UDRDR;    /* 0x4C: Underrun data               */
    volatile uint32_t I2SCFGR;  /* 0x50: I2S config                  */
} SPI_TypeDef;

/* ---- I2C (H7 F7-style) ---- */
typedef struct {
    volatile uint32_t CR1;      /* 0x00: Control register 1          */
    volatile uint32_t CR2;      /* 0x04: Control register 2          */
    volatile uint32_t OAR1;     /* 0x08: Own address 1               */
    volatile uint32_t OAR2;     /* 0x0C: Own address 2               */
    volatile uint32_t TIMINGR;  /* 0x10: Timing register             */
    volatile uint32_t TIMEOUTR; /* 0x14: Timeout register            */
    volatile uint32_t ISR;      /* 0x18: Interrupt/status register   */
    volatile uint32_t ICR;      /* 0x1C: Interrupt clear register    */
    volatile uint32_t PECR;     /* 0x20: Packet error check          */
    volatile uint32_t RXDR;     /* 0x24: Receive data                */
    volatile uint32_t TXDR;     /* 0x28: Transmit data               */
} I2C_TypeDef;

/* ---- TIM (with H7 AF1/AF2/TISEL tail) ---- */
typedef struct {
    volatile uint32_t CR1;      /* 0x00: Control register 1          */
    volatile uint32_t CR2;      /* 0x04: Control register 2          */
    volatile uint32_t SMCR;     /* 0x08: Slave mode control          */
    volatile uint32_t DIER;     /* 0x0C: DMA/Interrupt enable        */
    volatile uint32_t SR;       /* 0x10: Status register             */
    volatile uint32_t EGR;      /* 0x14: Event generation register   */
    volatile uint32_t CCMR1;    /* 0x18: CC mode register 1          */
    volatile uint32_t CCMR2;    /* 0x1C: CC mode register 2          */
    volatile uint32_t CCER;     /* 0x20: CC enable register          */
    volatile uint32_t CNT;      /* 0x24: Counter                     */
    volatile uint32_t PSC;      /* 0x28: Prescaler                   */
    volatile uint32_t ARR;      /* 0x2C: Auto-reload register        */
    volatile uint32_t RCR;      /* 0x30: Repetition counter          */
    volatile uint32_t CCR1;     /* 0x34: Capture/compare 1           */
    volatile uint32_t CCR2;     /* 0x38: Capture/compare 2           */
    volatile uint32_t CCR3;     /* 0x3C: Capture/compare 3           */
    volatile uint32_t CCR4;     /* 0x40: Capture/compare 4           */
    volatile uint32_t BDTR;     /* 0x44: Break & dead-time           */
    volatile uint32_t DCR;      /* 0x48: DMA control                 */
    volatile uint32_t DMAR;     /* 0x4C: DMA address for burst       */
    volatile uint32_t RES1;     /* 0x50: Reserved                    */
    volatile uint32_t CCMR3;    /* 0x54: CC mode register 3          */
    volatile uint32_t CCR5;     /* 0x58: Capture/compare 5           */
    volatile uint32_t CCR6;     /* 0x5C: Capture/compare 6           */
    volatile uint32_t AF1;      /* 0x60: Alternate function 1        */
    volatile uint32_t AF2;      /* 0x64: Alternate function 2        */
    volatile uint32_t TISEL;    /* 0x68: Trigger input selection     */
} TIM_TypeDef;

/* ======================== Peripheral Declarations ======================== */

#define GPIOA               ((GPIO_TypeDef *)GPIOA_BASE)
#define GPIOB               ((GPIO_TypeDef *)GPIOB_BASE)
#define GPIOC               ((GPIO_TypeDef *)GPIOC_BASE)
#define GPIOD               ((GPIO_TypeDef *)GPIOD_BASE)
#define GPIOE               ((GPIO_TypeDef *)GPIOE_BASE)
#define GPIOF               ((GPIO_TypeDef *)GPIOF_BASE)
#define GPIOG               ((GPIO_TypeDef *)GPIOG_BASE)
#define GPIOH               ((GPIO_TypeDef *)GPIOH_BASE)
#define GPIOI               ((GPIO_TypeDef *)GPIOI_BASE)
#define GPIOJ               ((GPIO_TypeDef *)GPIOJ_BASE)
#define GPIOK               ((GPIO_TypeDef *)GPIOK_BASE)

#define USART1              ((USART_TypeDef *)USART1_BASE)
#define USART2              ((USART_TypeDef *)USART2_BASE)
#define USART3              ((USART_TypeDef *)USART3_BASE)
#define UART4               ((USART_TypeDef *)UART4_BASE)
#define UART5               ((USART_TypeDef *)UART5_BASE)
#define USART6              ((USART_TypeDef *)USART6_BASE)
#define UART7               ((USART_TypeDef *)UART7_BASE)
#define UART8               ((USART_TypeDef *)UART8_BASE)
#define LPUART1             ((USART_TypeDef *)LPUART1_BASE)

#define RCC                 ((RCC_TypeDef *)RCC_BASE)
#define FLASH               ((FLASH_TypeDef *)FLASH_R_BASE)
#define PWR                 ((PWR_TypeDef *)PWR_BASE)
#define EXTI                ((EXTI_TypeDef *)EXTI_BASE)
#define SYSCFG              ((SYSCFG_TypeDef *)SYSCFG_BASE)
#define IWDG1               ((IWDG_TypeDef *)IWDG1_BASE)
#define IWDG2               ((IWDG_TypeDef *)IWDG2_BASE)
#define WWDG1               ((WWDG_TypeDef *)WWDG1_BASE)
#define CRC                 ((CRC_TypeDef *)CRC_BASE)
#define RTC                 ((RTC_TypeDef *)RTC_BASE)
#define RNG                 ((RNG_TypeDef *)RNG_BASE)
#define QUADSPI             ((QUADSPI_TypeDef *)QSPI_R_BASE)

#define DMA1                ((DMA_TypeDef *)DMA1_BASE)
#define DMA2                ((DMA_TypeDef *)DMA2_BASE)
#define SDMMC1              ((SDMMC_TypeDef *)SDMMC1_BASE)
#define SDMMC2              ((SDMMC_TypeDef *)SDMMC2_BASE)

/* ADC */
#define ADC1                ((ADC_TypeDef *)ADC1_BASE)
#define ADC2                ((ADC_TypeDef *)ADC2_BASE)
#define ADC3                ((ADC_TypeDef *)ADC3_BASE)
#define ADC12_COMMON        ((ADC_Common_TypeDef *)ADC12_COMMON_BASE)
#define ADC3_COMMON         ((ADC_Common_TypeDef *)ADC3_COMMON_BASE)

/* USB OTG */
#define USB_OTG_FS          ((USB_OTG_GlobalTypeDef *)USB_OTG_FS_PERIPH_BASE)
#define USB_OTG_HS          ((USB_OTG_GlobalTypeDef *)USB_OTG_HS_PERIPH_BASE)
#define DMAMUX1             ((DMAMUX_TypeDef *)DMAMUX1_BASE)
#define DMA1_Channel0       ((DMA_Channel_TypeDef *)DMA1_Channel0_BASE)
#define DMA1_Channel1       ((DMA_Channel_TypeDef *)DMA1_Channel1_BASE)
#define DMA1_Channel2       ((DMA_Channel_TypeDef *)DMA1_Channel2_BASE)
#define DMA1_Channel3       ((DMA_Channel_TypeDef *)DMA1_Channel3_BASE)
#define DMA1_Channel4       ((DMA_Channel_TypeDef *)DMA1_Channel4_BASE)
#define DMA1_Channel5       ((DMA_Channel_TypeDef *)DMA1_Channel5_BASE)
#define DMA1_Channel6       ((DMA_Channel_TypeDef *)DMA1_Channel6_BASE)
#define DMA1_Channel7       ((DMA_Channel_TypeDef *)DMA1_Channel7_BASE)
#define DMA2_Channel0       ((DMA_Channel_TypeDef *)DMA2_Channel0_BASE)
#define DMA2_Channel1       ((DMA_Channel_TypeDef *)DMA2_Channel1_BASE)
#define DMA2_Channel2       ((DMA_Channel_TypeDef *)DMA2_Channel2_BASE)
#define DMA2_Channel3       ((DMA_Channel_TypeDef *)DMA2_Channel3_BASE)
#define DMA2_Channel4       ((DMA_Channel_TypeDef *)DMA2_Channel4_BASE)
#define DMA2_Channel5       ((DMA_Channel_TypeDef *)DMA2_Channel5_BASE)
#define DMA2_Channel6       ((DMA_Channel_TypeDef *)DMA2_Channel6_BASE)
#define DMA2_Channel7       ((DMA_Channel_TypeDef *)DMA2_Channel7_BASE)

#define SPI1                ((SPI_TypeDef *)SPI1_BASE)
#define SPI2                ((SPI_TypeDef *)SPI2_BASE)
#define SPI3                ((SPI_TypeDef *)SPI3_BASE)
#define SPI4                ((SPI_TypeDef *)SPI4_BASE)
#define SPI5                ((SPI_TypeDef *)SPI5_BASE)
#define SPI6                ((SPI_TypeDef *)SPI6_BASE)

#define I2C1                ((I2C_TypeDef *)I2C1_BASE)
#define I2C2                ((I2C_TypeDef *)I2C2_BASE)
#define I2C3                ((I2C_TypeDef *)I2C3_BASE)
#define I2C4                ((I2C_TypeDef *)I2C4_BASE)
#define DAC1                ((DAC_TypeDef *)DAC1_BASE)

#define TIM1                ((TIM_TypeDef *)TIM1_BASE)
#define TIM2                ((TIM_TypeDef *)TIM2_BASE)
#define TIM3                ((TIM_TypeDef *)TIM3_BASE)
#define TIM4                ((TIM_TypeDef *)TIM4_BASE)
#define TIM5                ((TIM_TypeDef *)TIM5_BASE)
#define TIM6                ((TIM_TypeDef *)TIM6_BASE)
#define TIM7                ((TIM_TypeDef *)TIM7_BASE)
#define TIM8                ((TIM_TypeDef *)TIM8_BASE)
#define TIM12               ((TIM_TypeDef *)TIM12_BASE)
#define TIM13               ((TIM_TypeDef *)TIM13_BASE)
#define TIM14               ((TIM_TypeDef *)TIM14_BASE)
#define TIM15               ((TIM_TypeDef *)TIM15_BASE)
#define TIM16               ((TIM_TypeDef *)TIM16_BASE)
#define TIM17               ((TIM_TypeDef *)TIM17_BASE)

/* ---- FLASH_BASE and other memory-region constants ---- */
#ifndef FLASH_BASE
#define FLASH_BASE          0x08000000UL
#endif
#define FLASH_BANK1_BASE    FLASH_BASE
#define FLASH_END           0x0801FFFFUL    /* H750: single 128KB bank */
#define FLASH_SECTOR_SIZE   0x00020000UL    /* 128KB (bank = sector)   */
#define DTCM_BASE           0x20000000UL    /* DTCM 128KB              */
#define AXI_SRAM_BASE       0x24000000UL    /* AXI SRAM 512KB          */
#define SRAM1_BASE          0x30000000UL    /* SRAM1 128KB (D2, non-cacheable via MPU) */
#define QSPI_BASE           0x90000000UL    /* QSPI memory-mapped      */

/* ---- SystemCoreClock ---- */
extern uint32_t SystemCoreClock;

/* ======================== RCC Bit Definitions ======================== */

/* RCC_CR */
#define RCC_CR_HSION        0x00000001U
#define RCC_CR_HSIRDY       0x00000004U
#define RCC_CR_CSION        0x00000080U
#define RCC_CR_CSIRDY       0x00000100U
#define RCC_CR_HSEON        0x00010000U
#define RCC_CR_HSERDY       0x00020000U
#define RCC_CR_PLL1ON       0x01000000U
#define RCC_CR_PLL1RDY      0x02000000U

/* RCC_CFGR */
#define RCC_CFGR_SW_Pos     0U
#define RCC_CFGR_SW_Msk     (0x7UL << RCC_CFGR_SW_Pos)
#define RCC_CFGR_SW         RCC_CFGR_SW_Msk
#define RCC_CFGR_SW_HSI     (0U << 0U)
#define RCC_CFGR_SW_CSI     (1U << 0U)
#define RCC_CFGR_SW_HSE     (2U << 0U)
#define RCC_CFGR_SW_PLL1    (3U << 0U)
#define RCC_CFGR_SWS_Pos    3U
#define RCC_CFGR_SWS_Msk    (0x7UL << RCC_CFGR_SWS_Pos)
#define RCC_CFGR_SWS        RCC_CFGR_SWS_Msk
#define RCC_CFGR_SWS_HSI    (0U << 3U)
#define RCC_CFGR_SWS_CSI    (1U << 3U)
#define RCC_CFGR_SWS_HSE    (2U << 3U)
#define RCC_CFGR_SWS_PLL1   (3U << 3U)

/* RCC_D1CFGR */
#define RCC_D1CFGR_HPRE_Pos     0U
#define RCC_D1CFGR_HPRE_Msk     (0xFUL << RCC_D1CFGR_HPRE_Pos)
#define RCC_D1CFGR_HPRE         RCC_D1CFGR_HPRE_Msk
#define RCC_D1CFGR_HPRE_DIV1    (0U << 0U)
#define RCC_D1CFGR_HPRE_DIV2    (8U << 0U)
#define RCC_D1CFGR_D1PPRE_Pos   4U
#define RCC_D1CFGR_D1PPRE_Msk   (0x7UL << RCC_D1CFGR_D1PPRE_Pos)
#define RCC_D1CFGR_D1PPRE       RCC_D1CFGR_D1PPRE_Msk
#define RCC_D1CFGR_D1PPRE_DIV1  (0U << 4U)
#define RCC_D1CFGR_D1PPRE_DIV2  (4U << 4U)
#define RCC_D1CFGR_D1CPRE_Pos   8U
#define RCC_D1CFGR_D1CPRE_Msk   (0xFUL << RCC_D1CFGR_D1CPRE_Pos)
#define RCC_D1CFGR_D1CPRE       RCC_D1CFGR_D1CPRE_Msk
#define RCC_D1CFGR_D1CPRE_DIV1  (0U << 8U)

/* RCC_D2CFGR */
#define RCC_D2CFGR_D2PPRE1_Pos  4U
#define RCC_D2CFGR_D2PPRE1_Msk  (0x7UL << RCC_D2CFGR_D2PPRE1_Pos)
#define RCC_D2CFGR_D2PPRE1      RCC_D2CFGR_D2PPRE1_Msk
#define RCC_D2CFGR_D2PPRE1_DIV1 (0U << 4U)
#define RCC_D2CFGR_D2PPRE1_DIV2 (4U << 4U)
#define RCC_D2CFGR_D2PPRE2_Pos  8U
#define RCC_D2CFGR_D2PPRE2_Msk  (0x7UL << RCC_D2CFGR_D2PPRE2_Pos)
#define RCC_D2CFGR_D2PPRE2      RCC_D2CFGR_D2PPRE2_Msk
#define RCC_D2CFGR_D2PPRE2_DIV1 (0U << 8U)
#define RCC_D2CFGR_D2PPRE2_DIV2 (4U << 8U)

/* RCC_D3CFGR */
#define RCC_D3CFGR_D3PPRE_Pos   4U
#define RCC_D3CFGR_D3PPRE_Msk   (0x7UL << RCC_D3CFGR_D3PPRE_Pos)
#define RCC_D3CFGR_D3PPRE       RCC_D3CFGR_D3PPRE_Msk
#define RCC_D3CFGR_D3PPRE_DIV1  (0U << 4U)
#define RCC_D3CFGR_D3PPRE_DIV2  (4U << 4U)

/* RCC_PLLCKSELR */
#define RCC_PLLCKSELR_PLLSRC_Pos    0U
#define RCC_PLLCKSELR_PLLSRC_Msk    (0x3UL << RCC_PLLCKSELR_PLLSRC_Pos)
#define RCC_PLLCKSELR_PLLSRC        RCC_PLLCKSELR_PLLSRC_Msk
#define RCC_PLLCKSELR_PLLSRC_HSI    (0U << 0U)
#define RCC_PLLCKSELR_PLLSRC_CSI    (1U << 0U)
#define RCC_PLLCKSELR_PLLSRC_HSE    (2U << 0U)
#define RCC_PLLCKSELR_DIVM1_Pos     4U
#define RCC_PLLCKSELR_DIVM1_Msk     (0x3FUL << RCC_PLLCKSELR_DIVM1_Pos)
#define RCC_PLLCKSELR_DIVM1         RCC_PLLCKSELR_DIVM1_Msk

/* RCC_PLLCFGR */
#define RCC_PLLCFGR_PLL1EN      0x00000001U
#define RCC_PLLCFGR_PLL2EN      0x00000002U
#define RCC_PLLCFGR_PLL3EN      0x00000004U

/* RCC_PLL1DIVR */
#define RCC_PLL1DIVR_N1_Pos     0U
#define RCC_PLL1DIVR_N1_Msk     (0x1FFUL << RCC_PLL1DIVR_N1_Pos)
#define RCC_PLL1DIVR_N1         RCC_PLL1DIVR_N1_Msk
#define RCC_PLL1DIVR_P1_Pos     9U
#define RCC_PLL1DIVR_P1_Msk     (0x7FUL << RCC_PLL1DIVR_P1_Pos)
#define RCC_PLL1DIVR_P1         RCC_PLL1DIVR_P1_Msk
#define RCC_PLL1DIVR_Q1_Pos     16U
#define RCC_PLL1DIVR_Q1_Msk     (0x7FUL << RCC_PLL1DIVR_Q1_Pos)
#define RCC_PLL1DIVR_Q1         RCC_PLL1DIVR_Q1_Msk
#define RCC_PLL1DIVR_R1_Pos     24U
#define RCC_PLL1DIVR_R1_Msk     (0x7FUL << RCC_PLL1DIVR_R1_Pos)
#define RCC_PLL1DIVR_R1         RCC_PLL1DIVR_R1_Msk

/* RCC_D1CCIPR — kernel clock selection */
#define RCC_D1CCIPR_QSPISEL_Pos 4U
#define RCC_D1CCIPR_QSPISEL_Msk (0x3UL << RCC_D1CCIPR_QSPISEL_Pos)
#define RCC_D1CCIPR_QSPISEL     RCC_D1CCIPR_QSPISEL_Msk

/* RCC_RSR — reset status register (H7: flags live here, not in CSR) */
#define RCC_RSR_RMVF        0x00010000U
#define RCC_RSR_BORRSTF     0x00200000U
#define RCC_RSR_PINRSTF     0x00400000U
#define RCC_RSR_PORRSTF     0x00800000U
#define RCC_RSR_SFTRSTF     0x01000000U
#define RCC_RSR_IWDG1RSTF   0x04000000U
#define RCC_RSR_WWDG1RSTF   0x10000000U
#define RCC_RSR_LPWRRSTF    0x40000000U

/* RCC_RSR bit-name aliases (jOS system_init.c reset-reason decode) */
#define RCC_CSR_RMVF        RCC_RSR_RMVF
#define RCC_CSR_BORRSTF     RCC_RSR_BORRSTF
#define RCC_CSR_PINRSTF     RCC_RSR_PINRSTF
#define RCC_CSR_PORRSTF     RCC_RSR_PORRSTF
#define RCC_CSR_SFTRSTF     RCC_RSR_SFTRSTF
#define RCC_CSR_IWDGRSTF    RCC_RSR_IWDG1RSTF
#define RCC_CSR_WWDGRSTF    RCC_RSR_WWDG1RSTF
#define RCC_CSR_LPWRRSTF    RCC_RSR_LPWRRSTF

/* RCC_CSR — LSI control (bit positions match F4; used by IWDG/RTC HALs) */
#define RCC_CSR_LSION           0x00000001U
#define RCC_CSR_LSIRDY          0x00000002U

/* RCC_BDCR — backup domain (RTC source select + enable) */
#define RCC_BDCR_RTCSEL         0x00000300U
#define RCC_BDCR_RTCSEL_0       0x00000100U   /* LSE */
#define RCC_BDCR_RTCSEL_1       0x00000200U   /* LSI */
#define RCC_BDCR_RTCEN          0x00008000U

/* RCC_AHB3ENR */
#define RCC_AHB3ENR_MDMAEN      0x00000001U
#define RCC_AHB3ENR_DMA2DEN     0x00000010U
#define RCC_AHB3ENR_FMCEN       0x00001000U
#define RCC_AHB3ENR_QSPIEN      0x00004000U
#define RCC_AHB3ENR_SDMMC1EN    0x00010000U

/* RCC_AHB1ENR (D2 domain) — ADC12 clock gate moved here on new
 * silicon (old Y-revision parts gate ADC on AHB2ENR instead) */
#define RCC_AHB1ENR_DMA1EN      0x00000001U
#define RCC_AHB1ENR_DMA2EN      0x00000002U
#define RCC_AHB1ENR_DMAMUX1EN   0x00000004U
#define RCC_AHB1ENR_ADC12EN     0x00000020U
#define RCC_AHB1ENR_CRCEN       0x00001000U

/* RCC_AHB2ENR (D3 domain) */
#define RCC_AHB2ENR_RNGEN       0x00000040U
#define RCC_AHB2ENR_OTGFSEN     0x00000002U

/* RCC_AHB4ENR — GPIO clocks (H7: GPIO is on AHB4, NOT AHB1) */
#define RCC_AHB4ENR_GPIOAEN     0x00000001U
#define RCC_AHB4ENR_GPIOBEN     0x00000002U
#define RCC_AHB4ENR_GPIOCEN     0x00000004U
#define RCC_AHB4ENR_GPIODEN     0x00000008U
#define RCC_AHB4ENR_ADC3EN      0x01000000U  /* ADC3 clock gate (D3 domain) */
#define RCC_AHB4ENR_GPIOEEN     0x00000010U
#define RCC_AHB4ENR_GPIOFEN     0x00000020U
#define RCC_AHB4ENR_GPIOGEN     0x00000040U
#define RCC_AHB4ENR_GPIOHEN     0x00000080U
#define RCC_AHB4ENR_GPIOIEN     0x00000100U
#define RCC_AHB4ENR_GPIOJEN     0x00000200U
#define RCC_AHB4ENR_GPIOKEN     0x00000400U

/* RCC_APB2ENR */
#define RCC_APB2ENR_TIM1EN      0x00000001U
#define RCC_APB2ENR_TIM8EN      0x00000002U
#define RCC_APB2ENR_USART1EN    0x00000010U
#define RCC_APB2ENR_USART6EN    0x00000020U
#define RCC_APB2ENR_TIM15EN     0x00010000U
#define RCC_APB2ENR_TIM16EN     0x00020000U
#define RCC_APB2ENR_TIM17EN     0x00040000U
#define RCC_APB2ENR_SPI1EN      0x00001000U
#define RCC_APB2ENR_SPI4EN      0x00002000U
#define RCC_APB2ENR_SPI5EN      0x00100000U

/* RCC_APB1LENR */
#define RCC_APB1LENR_TIM2EN     0x00000001U
#define RCC_APB1LENR_TIM3EN     0x00000002U
#define RCC_APB1LENR_TIM4EN     0x00000004U
#define RCC_APB1LENR_TIM5EN     0x00000008U
#define RCC_APB1LENR_TIM6EN     0x00000010U
#define RCC_APB1LENR_TIM7EN     0x00000020U
#define RCC_APB1LENR_TIM12EN    0x00000040U
#define RCC_APB1LENR_TIM13EN    0x00000080U
#define RCC_APB1LENR_TIM14EN    0x00000100U
#define RCC_APB1LENR_LPTIM1EN   0x00000200U
#define RCC_APB1LENR_SPI2EN     0x00004000U
#define RCC_APB1LENR_SPI3EN     0x00008000U
#define RCC_APB1LENR_USART2EN   0x00020000U
#define RCC_APB1LENR_USART3EN   0x00040000U
#define RCC_APB1LENR_UART4EN    0x00080000U
#define RCC_APB1LENR_UART5EN    0x00100000U
#define RCC_APB1LENR_I2C1EN     0x00200000U
#define RCC_APB1LENR_I2C2EN     0x00400000U
#define RCC_APB1LENR_I2C3EN     0x00800000U
#define RCC_APB1LENR_DAC12EN    0x20000000U

/* RCC_APB4ENR — note: H7 EXTI has NO clock gate (always clocked). Bit values
 * follow RM0433 (PWREN=0, SYSCFGEN=1, LPUART1EN=2, SPI6EN=3, I2C4EN=4,
 * LPTIM2-5EN=5..8, COMP1/2EN=9/10, WWDG1EN=11, VREFEN=12, RTCAPBEN=13). */
#define RCC_APB4ENR_PWREN       0x00000001U
#define RCC_APB4ENR_SYSCFGEN    0x00000002U
#define RCC_APB4ENR_LPUART1EN   0x00000004U
#define RCC_APB4ENR_SPI6EN      0x00000008U
#define RCC_APB4ENR_I2C4EN      0x00000010U
#define RCC_APB4ENR_LPTIM2EN    0x00000020U
#define RCC_APB4ENR_LPTIM3EN    0x00000040U
#define RCC_APB4ENR_LPTIM4EN    0x00000080U
#define RCC_APB4ENR_LPTIM5EN    0x00000100U
#define RCC_APB4ENR_COMP1EN     0x00000200U
#define RCC_APB4ENR_COMP2EN     0x00000400U
#define RCC_APB4ENR_WWDG1EN     0x00000800U
#define RCC_APB4ENR_VREFEN      0x00001000U
#define RCC_APB4ENR_RTCAPBEN    0x00002000U

/* ======================== GPIO Bit Definitions ======================== */

#define GPIO_MODER_MODE0_Pos    0U
#define GPIO_MODER_MODE0_Msk    (0x3UL << GPIO_MODER_MODE0_Pos)
#define GPIO_OTYPER_OT0         0x00000001U
#define GPIO_PUPDR_PUPD0_Pos    0U
#define GPIO_PUPDR_PUPD0_Msk    (0x3UL << GPIO_PUPDR_PUPD0_Pos)
#define GPIO_BSRR_BS0           0x00000001U
#define GPIO_BSRR_BR0           0x00010000U

/* ======================== USART Bit Definitions ======================== */

/* USART_ISR */
#define USART_ISR_PE         0x00000001U
#define USART_ISR_FE         0x00000002U
#define USART_ISR_NE         0x00000004U
#define USART_ISR_ORE        0x00000008U
#define USART_ISR_IDLE       0x00000010U
#define USART_ISR_RXNE_RXFNE 0x00000020U
#define USART_ISR_TC         0x00000040U
#define USART_ISR_TXE_TXFNF  0x00000080U
#define USART_ISR_BUSY       0x00010000U

/* USART_ICR (write 1 to clear) */
#define USART_ICR_PECF       0x00000001U
#define USART_ICR_FECF       0x00000002U
#define USART_ICR_NECF       0x00000004U
#define USART_ICR_ORECF      0x00000008U
#define USART_ICR_IDLECF     0x00000010U
#define USART_ICR_RXNE_RXFECF 0x00000020U
#define USART_ICR_TCCF       0x00000040U
#define USART_ICR_TXE_TXFECF 0x00000080U

/* USART_CR1 */
#define USART_CR1_UE         0x00000001U
#define USART_CR1_IDLEIE     0x00000010U
#define USART_CR1_RE         0x00000004U
#define USART_CR1_TE         0x00000008U
#define USART_CR1_RXNEIE_RXFNEIE 0x00000020U
#define USART_CR1_TCIE       0x00000040U
#define USART_CR1_TXEIE_TXFNFIE  0x00000080U
#define USART_CR1_PEIE       0x00000100U
#define USART_CR1_PS         0x00000200U
#define USART_CR1_PCE        0x00000400U
#define USART_CR1_WAKE       0x00000800U
#define USART_CR1_M0         0x00001000U
#define USART_CR1_M1         0x10000000U
#define USART_CR1_FIFOEN     0x20000000U
#define USART_CR1_TXFEIE     0x40000000U
#define USART_CR1_RXFFIE     0x80000000U

/* USART_CR2 */
#define USART_CR2_STOP_0     0x00001000U
#define USART_CR2_STOP_1     0x00002000U

/* USART_CR3 */
#define USART_CR3_DMAT       0x00000080U
#define USART_CR3_DMAR       0x00000040U

/* ======================== DMA Bit Definitions ======================== */

/* DMA_CCR */
#define DMA_CCR_EN           0x00000001U
#define DMA_CCR_DMEIE        0x00000002U
#define DMA_CCR_TEIE         0x00000004U
#define DMA_CCR_HTIE         0x00000008U
#define DMA_CCR_TCIE         0x00000010U
#define DMA_CCR_PFCTRL       0x00000020U
#define DMA_CCR_DIR          0x00000040U   /* 1-bit: 0=P2M, 1=M2P */
#define DMA_CCR_CIRC         0x00000080U
#define DMA_CCR_PINC         0x00000100U
#define DMA_CCR_MINC         0x00000200U
#define DMA_CCR_PINCOS       0x00000400U
#define DMA_CCR_PSIZE_Pos    11U
#define DMA_CCR_PSIZE_Msk    (0x3UL << DMA_CCR_PSIZE_Pos)
#define DMA_CCR_MSIZE_Pos    13U
#define DMA_CCR_MSIZE_Msk    (0x3UL << DMA_CCR_MSIZE_Pos)
#define DMA_CCR_PL_Pos       16U
#define DMA_CCR_PL_Msk       (0x3UL << DMA_CCR_PL_Pos)
#define DMA_CCR_MBURST_Pos   18U
#define DMA_CCR_MBURST_Msk   (0x3UL << DMA_CCR_MBURST_Pos)
#define DMA_CCR_PBURST_Pos   20U
#define DMA_CCR_PBURST_Msk   (0x3UL << DMA_CCR_PBURST_Pos)
#define DMA_CCR_TRBUFF_Pos   22U
#define DMA_CCR_TRBUFF_Msk   (0x3UL << DMA_CCR_TRBUFF_Pos)
#define DMA_CCR_DBM          0x10000000U
#define DMA_CCR_CT           0x20000000U

/* DMAMUX_CxCR */
#define DMAMUX_CxCR_DMAREQ_ID_Pos  0U
#define DMAMUX_CxCR_DMAREQ_ID_Msk  (0x7FUL << DMAMUX_CxCR_DMAREQ_ID_Pos)
#define DMAMUX_CxCR_SOIE           0x00010000U
#define DMAMUX_CxCR_EGE            0x00020000U

/* DMA flag positions (per channel, shifted by group offset) */
#define DMA_FLAG_FE         0U
#define DMA_FLAG_DME        2U
#define DMA_FLAG_TE         3U
#define DMA_FLAG_HT         4U
#define DMA_FLAG_TC         5U

/* ======================== FLASH Bit Definitions ======================== */

/* FLASH_ACR — H7 has LATENCY only (M7 caches are always enabled) */
#define FLASH_ACR_LATENCY_Pos      0U
#define FLASH_ACR_LATENCY_Msk      (0xFUL << FLASH_ACR_LATENCY_Pos)
#define FLASH_ACR_LATENCY          FLASH_ACR_LATENCY_Msk
#define FLASH_ACR_LATENCY_0WS      0x00000000U
#define FLASH_ACR_LATENCY_1WS      0x00000001U
#define FLASH_ACR_LATENCY_2WS      0x00000002U
#define FLASH_ACR_LATENCY_3WS      0x00000003U
#define FLASH_ACR_LATENCY_4WS      0x00000004U
#define FLASH_ACR_LATENCY_5WS      0x00000005U
#define FLASH_ACR_LATENCY_6WS      0x00000006U
#define FLASH_ACR_LATENCY_7WS      0x00000007U
#define FLASH_ACR_WRHIGHFREQ_Pos   4U
#define FLASH_ACR_WRHIGHFREQ_Msk   (0x3UL << FLASH_ACR_WRHIGHFREQ_Pos)
#define FLASH_ACR_WRHIGHFREQ       FLASH_ACR_WRHIGHFREQ_Msk
#define FLASH_ACR_WRHIGHFREQ_0     0x00000010U
#define FLASH_ACR_WRHIGHFREQ_1     0x00000020U

/* FLASH_CR1 (erase/program left for milestone 2) */
#define FLASH_CR1_LOCK        0x00000001U
#define FLASH_CR1_PG          0x00000002U
#define FLASH_CR1_SER         0x00000004U
#define FLASH_CR1_BER         0x00000008U
#define FLASH_CR1_PSIZE       0x00000030U
#define FLASH_CR1_FW          0x00000040U
#define FLASH_CR1_START       0x00000080U
#define FLASH_CR1_SNB_Pos     8U
#define FLASH_CR1_SNB_Msk     (0x7UL << FLASH_CR1_SNB_Pos)

/* FLASH_SR1 */
#define FLASH_SR1_BSY         0x00000001U
#define FLASH_SR1_EOP         0x00000002U
#define FLASH_SR1_WRPERR      0x00000010U

/* ======================== PWR Bit Definitions ======================== */

/* PWR_D3CR */
#define PWR_D3CR_VOSRDY_Pos    13U
#define PWR_D3CR_VOSRDY        (1UL << PWR_D3CR_VOSRDY_Pos)
#define PWR_D3CR_VOS_Pos       14U
#define PWR_D3CR_VOS_Msk       (0x3UL << PWR_D3CR_VOS_Pos)
#define PWR_D3CR_VOS           PWR_D3CR_VOS_Msk
#define PWR_D3CR_VOS_0         (0x1UL << PWR_D3CR_VOS_Pos)  /* 0x4000 */
#define PWR_D3CR_VOS_1         (0x2UL << PWR_D3CR_VOS_Pos)  /* 0x8000 */
#define PWR_D3CR_VOS_SCALE3    (0x0UL << PWR_D3CR_VOS_Pos)
#define PWR_D3CR_VOS_SCALE2    (0x1UL << PWR_D3CR_VOS_Pos)
#define PWR_D3CR_VOS_SCALE1    (0x2UL << PWR_D3CR_VOS_Pos)
#define PWR_D3CR_VOS_SCALE0    (0x3UL << PWR_D3CR_VOS_Pos)  /* 0xC000, 480MHz */

/* PWR_CR1 — backup-domain access unlock (RTC/IWDG write protection) */
#define PWR_CR1_DBP            0x00000100U

/* ======================== EXTI Bit Definitions ======================== */

/* EXTI block 1 (lines 0..31) — direct GPIO→EXTI routing, no EXTICR on H7 */
#define EXTI_IMR1_IM0         0x00000001U
#define EXTI_IMR1_IM1         0x00000002U
#define EXTI_IMR1_IM2         0x00000004U
#define EXTI_IMR1_IM3         0x00000008U
#define EXTI_IMR1_IM4         0x00000010U
#define EXTI_IMR1_IM5         0x00000020U
#define EXTI_IMR1_IM6         0x00000040U
#define EXTI_IMR1_IM7         0x00000080U
#define EXTI_IMR1_IM8         0x00000100U
#define EXTI_IMR1_IM9         0x00000200U
#define EXTI_IMR1_IM10        0x00000400U
#define EXTI_IMR1_IM11        0x00000800U
#define EXTI_IMR1_IM12        0x00001000U
#define EXTI_IMR1_IM13        0x00002000U
#define EXTI_IMR1_IM14        0x00004000U
#define EXTI_IMR1_IM15        0x00008000U

#define EXTI_RTSR1_RT0        0x00000001U
#define EXTI_FTSR1_FT0        0x00000001U
#define EXTI_PR1_PR0          0x00000001U
#define EXTI_SWIER1_SWI0      0x00000001U

/* ======================== QUADSPI Bit Definitions ======================== */

/* QUADSPI_CR */
#define QUADSPI_CR_EN            0x00000001U
#define QUADSPI_CR_ABORT         0x00000002U
#define QUADSPI_CR_DMAEN         0x00000004U
#define QUADSPI_CR_TCEN          0x00000008U
#define QUADSPI_CR_SSHIFT        0x00000010U
#define QUADSPI_CR_DFM           0x00000040U
#define QUADSPI_CR_FSEL          0x00000080U
#define QUADSPI_CR_FTHRES_Pos    8U
#define QUADSPI_CR_FTHRES_Msk    (0x7UL << QUADSPI_CR_FTHRES_Pos)
#define QUADSPI_CR_TEIE          0x00010000U
#define QUADSPI_CR_TCIE          0x00020000U
#define QUADSPI_CR_FTIE          0x00040000U
#define QUADSPI_CR_SMIE          0x00080000U
#define QUADSPI_CR_TOIE          0x00100000U
#define QUADSPI_CR_APMS          0x00400000U
#define QUADSPI_CR_PRESCALER_Pos 24U
#define QUADSPI_CR_PRESCALER_Msk (0xFFUL << QUADSPI_CR_PRESCALER_Pos)

/* QUADSPI_CCR — H7 layout has NO INSTMODE bit (IMODE/ADMODE/ABMODE/DMODE/FMODE) */
#define QUADSPI_CCR_INSTRUCTION_Pos  0U
#define QUADSPI_CCR_INSTRUCTION_Msk  (0xFFUL << QUADSPI_CCR_INSTRUCTION_Pos)
#define QUADSPI_CCR_INSTRUCTION      QUADSPI_CCR_INSTRUCTION_Msk
#define QUADSPI_CCR_IMODE_Pos        8U
#define QUADSPI_CCR_IMODE_Msk        (0x3UL << QUADSPI_CCR_IMODE_Pos)
#define QUADSPI_CCR_IMODE_0          (0x1UL << QUADSPI_CCR_IMODE_Pos)  /* 1-line */
#define QUADSPI_CCR_IMODE_1          (0x2UL << QUADSPI_CCR_IMODE_Pos)  /* 2-line */
#define QUADSPI_CCR_ADMODE_Pos       10U
#define QUADSPI_CCR_ADMODE_Msk       (0x3UL << QUADSPI_CCR_ADMODE_Pos)
#define QUADSPI_CCR_ADSIZE_Pos       12U
#define QUADSPI_CCR_ADSIZE_Msk       (0x3UL << QUADSPI_CCR_ADSIZE_Pos)
#define QUADSPI_CCR_ABMODE_Pos       14U
#define QUADSPI_CCR_ABMODE_Msk       (0x3UL << QUADSPI_CCR_ABMODE_Pos)
#define QUADSPI_CCR_ABSIZE_Pos       16U
#define QUADSPI_CCR_ABSIZE_Msk       (0x3UL << QUADSPI_CCR_ABSIZE_Pos)
#define QUADSPI_CCR_DCYC_Pos         18U
#define QUADSPI_CCR_DCYC_Msk         (0x1FUL << QUADSPI_CCR_DCYC_Pos)
#define QUADSPI_CCR_DMODE_Pos        24U
#define QUADSPI_CCR_DMODE_Msk        (0x3UL << QUADSPI_CCR_DMODE_Pos)
#define QUADSPI_CCR_DMODE_0          (0x1UL << QUADSPI_CCR_DMODE_Pos)
#define QUADSPI_CCR_DMODE_1          (0x2UL << QUADSPI_CCR_DMODE_Pos)
#define QUADSPI_CCR_FMODE_Pos        26U
#define QUADSPI_CCR_FMODE_Msk        (0x3UL << QUADSPI_CCR_FMODE_Pos)
#define QUADSPI_CCR_FMODE_0          (0x1UL << QUADSPI_CCR_FMODE_Pos)  /* indirect read */
#define QUADSPI_CCR_FMODE_1          (0x2UL << QUADSPI_CCR_FMODE_Pos)  /* auto-polling */
#define QUADSPI_CCR_FMODE_MM         (0x3UL << QUADSPI_CCR_FMODE_Pos)  /* memory-mapped */
#define QUADSPI_CCR_SIOO_Pos         28U
#define QUADSPI_CCR_SIOO             (1UL << QUADSPI_CCR_SIOO_Pos)
#define QUADSPI_CCR_DHHC_Pos         30U
#define QUADSPI_CCR_DHHC             (1UL << QUADSPI_CCR_DHHC_Pos)
#define QUADSPI_CCR_DDRM_Pos         31U
#define QUADSPI_CCR_DDRM             (1UL << QUADSPI_CCR_DDRM_Pos)

/* QUADSPI_DCR */
#define QUADSPI_DCR_CKMODE       0x00000001U
#define QUADSPI_DCR_CSHT_Pos     8U
#define QUADSPI_DCR_CSHT_Msk     (0x7UL << QUADSPI_DCR_CSHT_Pos)
#define QUADSPI_DCR_FSIZE_Pos    16U
#define QUADSPI_DCR_FSIZE_Msk    (0x1FUL << QUADSPI_DCR_FSIZE_Pos)

/* QUADSPI_SR */
#define QUADSPI_SR_TEF           0x00000001U
#define QUADSPI_SR_TCF           0x00000002U
#define QUADSPI_SR_FTF           0x00000004U
#define QUADSPI_SR_SMF           0x00000008U
#define QUADSPI_SR_TOF           0x00000010U
#define QUADSPI_SR_BUSY          0x00000020U
#define QUADSPI_SR_FLEVEL_Pos    8U
#define QUADSPI_SR_FLEVEL_Msk    (0x20UL << QUADSPI_SR_FLEVEL_Pos)

/* QUADSPI_FCR (write 1 to clear) */
#define QUADSPI_FCR_CTEF         0x00000001U
#define QUADSPI_FCR_CTCF         0x00000002U
#define QUADSPI_FCR_CSMF         0x00000008U
#define QUADSPI_FCR_CTOF         0x00000010U

/* QUADSPI_DLR */
#define QUADSPI_DLR_DL_Pos       0U
#define QUADSPI_DLR_DL_Msk       (0xFFFFFFFFUL << QUADSPI_DLR_DL_Pos)

/* ======================== IWDG Bit Definitions ======================== */

#define IWDG_KR_KEY             0x00005555U
#define IWDG_KR_RELOAD          0x0000AAAAU
#define IWDG_KR_START           0x0000CCCCU
#define IWDG_PR_PR_Pos          0U
#define IWDG_PR_PR_Msk          (0x7UL << IWDG_PR_PR_Pos)
#define IWDG_RLR_RL_Pos         0U
#define IWDG_RLR_RL_Msk         (0xFFFUL << IWDG_RLR_RL_Pos)

/* ======================== CRC Bit Definitions ======================== */

#define CRC_CR_RESET            0x00000001U
#define CRC_CR_POLYSIZE_Pos     3U
#define CRC_CR_POLYSIZE_Msk     (0x3UL << CRC_CR_POLYSIZE_Pos)

/* ======================== TIM Bit Definitions ======================== */

/* TIM_CR1 */
#define TIM_CR1_CEN             (1U << 0U)
#define TIM_CR1_URS             (1U << 2U)
#define TIM_CR1_OPM             (1U << 3U)
#define TIM_CR1_ARPE            (1U << 7U)

/* TIM_CR2 */
#define TIM_CR2_MMS_Pos         4U
#define TIM_CR2_MMS_Msk         (0x7UL << TIM_CR2_MMS_Pos)
#define TIM_CR2_MMS_UPDATE      (0x2UL << TIM_CR2_MMS_Pos)

/* TIM_DIER */
#define TIM_DIER_UIE            (1U << 0U)
#define TIM_DIER_UDE            (1U << 8U)

/* TIM_SR */
#define TIM_SR_UIF              (1U << 0U)

/* TIM_EGR */
#define TIM_EGR_UG              (1U << 0U)

/* TIM_BDTR */
#define TIM_BDTR_DTG_Pos        0U
#define TIM_BDTR_DTG_Msk        (0xFFUL << TIM_BDTR_DTG_Pos)
#define TIM_BDTR_BKE            (1U << 12U)
#define TIM_BDTR_BKP            (1U << 13U)
#define TIM_BDTR_MOE            (1U << 15U)

/* ======================== RNG Bit Definitions ======================== */

/* RNG_CR */
#define RNG_CR_RNGEN            0x00000004U

/* RNG_SR */
#define RNG_SR_DRDY             0x00000001U
#define RNG_SR_CEIS             0x00000020U
#define RNG_SR_SEIS             0x00000040U

/* ======================== RTC Bit Definitions ======================== */

/* RTC_ICSR (init/status; H7 field name for the F4 ISR) */
#define RTC_ICSR_RSF            0x00000020U
#define RTC_ICSR_INITF          0x00000040U
#define RTC_ICSR_INIT           0x00000080U

/* RTC_PRER */
#define RTC_PRER_PREDIV_A_Pos   16U

/* ======================== WWDG Bit Definitions ======================== */

/* WWDG_CR */
#define WWDG_CR_WDGA            0x00000080U

/* WWDG_CFR */
#define WWDG_CFR_WDGTB_Pos      7U
#define WWDG_CFR_WDGTB_Msk      (0x3UL << WWDG_CFR_WDGTB_Pos)
#define WWDG_CFR_W_Pos          0U
#define WWDG_CFR_W_Msk          (0x7FUL << WWDG_CFR_W_Pos)

/* ======================== SPI Bit Definitions ======================== */

/* SPI_CR1 */
#define SPI_CR1_SPE             (1U << 0U)
#define SPI_CR1_SSI             (1U << 12U)
#define SPI_CR1_MASRX           (1U << 15U)

/* SPI_CR2 */
#define SPI_CR2_TSIZE_Pos       0U
#define SPI_CR2_TSIZE_Msk       (0xFFFFUL << SPI_CR2_TSIZE_Pos)

/* SPI_CFG1 */
#define SPI_CFG1_DSIZE_Pos      0U
#define SPI_CFG1_DSIZE_Msk      (0x1FUL << SPI_CFG1_DSIZE_Pos)
#define SPI_CFG1_MBR_Pos        28U
#define SPI_CFG1_MBR_Msk        (0x7UL << SPI_CFG1_MBR_Pos)
#define SPI_CFG1_TXDMAEN        (1U << 15U)
#define SPI_CFG1_RXDMAEN        (1U << 14U)

/* SPI_CFG2 */
#define SPI_CFG2_MASTER         (1U << 22U)
#define SPI_CFG2_SSM            (1U << 26U)
#define SPI_CFG2_CPOL           (1U << 25U)
#define SPI_CFG2_CPHA           (1U << 24U)

/* SPI_IER */
#define SPI_IER_RXPIE           (1U << 0U)
#define SPI_IER_TXPIE           (1U << 1U)
#define SPI_IER_EOTIE           (1U << 3U)

/* SPI_SR */
#define SPI_SR_RXP              (1U << 0U)
#define SPI_SR_TXP              (1U << 1U)
#define SPI_SR_EOT              (1U << 3U)
#define SPI_SR_TXTF             (1U << 4U)
#define SPI_SR_OVR              (1U << 6U)
#define SPI_SR_TXC              (1U << 12U)

/* SPI_IFCR */
#define SPI_IFCR_EOTC           (1U << 3U)
#define SPI_IFCR_TXTFC          (1U << 4U)

/* ======================== I2C Bit Definitions ======================== */

/* I2C_CR1 */
#define I2C_CR1_PE              (1U << 0U)
#define I2C_CR1_TXIE            (1U << 1U)
#define I2C_CR1_RXIE            (1U << 2U)
#define I2C_CR1_ADDRIE          (1U << 3U)
#define I2C_CR1_NACKIE          (1U << 4U)
#define I2C_CR1_STOPIE          (1U << 5U)
#define I2C_CR1_TCIE            (1U << 6U)
#define I2C_CR1_TXDMAEN         (1U << 14U)
#define I2C_CR1_RXDMAEN         (1U << 15U)

/* I2C_CR2 */
#define I2C_CR2_SADD_Pos        0U
#define I2C_CR2_SADD_Msk        (0x3FFUL << I2C_CR2_SADD_Pos)
#define I2C_CR2_RD_WRN          (1U << 10U)
#define I2C_CR2_ADD10           (1U << 11U)
#define I2C_CR2_START           (1U << 13U)
#define I2C_CR2_STOP            (1U << 14U)
#define I2C_CR2_NBYTES_Pos     16U
#define I2C_CR2_NBYTES_Msk     (0xFFUL << I2C_CR2_NBYTES_Pos)
#define I2C_CR2_RELOAD          (1U << 24U)
#define I2C_CR2_AUTOEND         (1U << 25U)

/* I2C_ISR */
#define I2C_ISR_TXE             (1U << 0U)
#define I2C_ISR_TXIS            (1U << 1U)
#define I2C_ISR_RXNE            (1U << 2U)
#define I2C_ISR_ADDR            (1U << 3U)
#define I2C_ISR_NACKF           (1U << 4U)
#define I2C_ISR_STOPF           (1U << 5U)
#define I2C_ISR_TC              (1U << 6U)
#define I2C_ISR_TCR             (1U << 7U)
#define I2C_ISR_BUSY            (1U << 15U)

/* I2C_ICR */
#define I2C_ICR_ADDRCF          (1U << 3U)
#define I2C_ICR_NACKCF          (1U << 4U)
#define I2C_ICR_STOPCF          (1U << 5U)

/* ======================== DAC Bit Definitions ======================== */

/* DAC_CR */
#define DAC_CR_EN1              (1U << 0U)
#define DAC_CR_EN2              (1U << 16U)
#define DAC_CR_DMAEN1           (1U << 12U)
#define DAC_CR_DMAEN2           (1U << 28U)
#define DAC_CR_TEN1             (1U << 2U)
#define DAC_CR_TEN2             (1U << 18U)

/* ======================== SDMMC Bit Definitions ======================== */

/* SDMMC_POWER */
#define SDMMC_POWER_PWRCTRL_Pos     0U
#define SDMMC_POWER_PWRCTRL_Msk     (0x3UL << SDMMC_POWER_PWRCTRL_Pos)
#define SDMMC_POWER_PWRCTRL_0       (1U << 0U)
#define SDMMC_POWER_PWRCTRL_1       (1U << 1U)

/* SDMMC_CLKCR */
#define SDMMC_CLKCR_CLKDIV_Pos      0U
#define SDMMC_CLKCR_CLKDIV_Msk      (0xFFUL << SDMMC_CLKCR_CLKDIV_Pos)
#define SDMMC_CLKCR_CLKEN           (1U << 8U)
#define SDMMC_CLKCR_PWRSAV          (1U << 9U)
#define SDMMC_CLKCR_BYPASS          (1U << 10U)
#define SDMMC_CLKCR_WIDBUS_Pos      11U
#define SDMMC_CLKCR_WIDBUS_Msk      (0x3UL << SDMMC_CLKCR_WIDBUS_Pos)
#define SDMMC_CLKCR_WIDBUS_0        (1U << 11U)
#define SDMMC_CLKCR_WIDBUS_1        (1U << 12U)
#define SDMMC_CLKCR_NEGEDGE         (1U << 13U)
#define SDMMC_CLKCR_HWFC_EN         (1U << 14U)

/* SDMMC_CMD */
#define SDMMC_CMD_CMDINDEX_Pos      0U
#define SDMMC_CMD_CMDINDEX_Msk      (0x3FUL << SDMMC_CMD_CMDINDEX_Pos)
#define SDMMC_CMD_WAITRESP_Pos      6U
#define SDMMC_CMD_WAITRESP_Msk      (0x3UL << SDMMC_CMD_WAITRESP_Pos)
#define SDMMC_CMD_WAITRESP_0        (1U << 6U)
#define SDMMC_CMD_WAITRESP_1        (1U << 7U)
#define SDMMC_CMD_CPSMEN            (1U << 10U)

/* SDMMC_DCTRL */
#define SDMMC_DCTRL_DTEN            (1U << 0U)
#define SDMMC_DCTRL_DTDIR           (1U << 1U)
#define SDMMC_DCTRL_DMAEN           (1U << 3U)
#define SDMMC_DCTRL_DBLOCKSIZE_Pos  4U
#define SDMMC_DCTRL_DBLOCKSIZE_Msk  (0xFUL << SDMMC_DCTRL_DBLOCKSIZE_Pos)

/* SDMMC_STA */
#define SDMMC_STA_CMDREND           (1U << 6U)
#define SDMMC_STA_CMDSENT           (1U << 7U)
#define SDMMC_STA_DATAEND           (1U << 10U)
#define SDMMC_STA_RXDAVL            (1U << 17U)
#define SDMMC_STA_TXFIFOE           (1U << 18U)
#define SDMMC_STA_CCRCFAIL          (1U << 0U)
#define SDMMC_STA_DCRCFAIL          (1U << 1U)
#define SDMMC_STA_CTIMEOUT          (1U << 2U)
#define SDMMC_STA_DTIMEOUT          (1U << 3U)

/* SDMMC_ICR */
#define SDMMC_ICR_CCRCFAILC         (1U << 0U)
#define SDMMC_ICR_DCRCFAILC         (1U << 1U)
#define SDMMC_ICR_CTIMEOUTC         (1U << 2U)
#define SDMMC_ICR_DTIMEOUTC         (1U << 3U)
#define SDMMC_ICR_CMDRENDC          (1U << 6U)
#define SDMMC_ICR_CMDSENTC          (1U << 7U)
#define SDMMC_ICR_DATAENDC          (1U << 10U)

/* ======================== ADC Bit Definitions ========================
 * H7 V5 silicon layout (16-bit ADC). */

/* ADC_ISR */
#define ADC_ISR_ADRDY               (1U << 0U)
#define ADC_ISR_EOSMP               (1U << 1U)
#define ADC_ISR_EOC                 (1U << 2U)
#define ADC_ISR_EOS                 (1U << 3U)
#define ADC_ISR_OVR                 (1U << 4U)
#define ADC_ISR_LDORDY              (1U << 12U)

/* ADC_IER */
#define ADC_IER_EOCIE               (1U << 2U)

/* ADC_CR */
#define ADC_CR_ADEN                 (1U << 0U)
#define ADC_CR_ADDIS                (1U << 1U)
#define ADC_CR_ADSTART              (1U << 2U)
#define ADC_CR_JADSTART             (1U << 3U)
#define ADC_CR_ADSTP                (1U << 4U)
#define ADC_CR_JADSTP               (1U << 5U)
#define ADC_CR_BOOST_Pos            8U
#define ADC_CR_BOOST_Msk            (0x3UL << ADC_CR_BOOST_Pos)
#define ADC_CR_ADVREGEN             (1U << 28U)
#define ADC_CR_DEEPPWD              (1U << 29U)
#define ADC_CR_ADCALDIF             (1U << 30U)
#define ADC_CR_ADCAL                (1U << 31U)

/* ADC_CFGR */
#define ADC_CFGR_DMNGT_Pos          0U
#define ADC_CFGR_DMNGT_Msk          (0x3UL << ADC_CFGR_DMNGT_Pos)
#define ADC_CFGR_DMNGT_0            (1U << 0U)   /* DR + DMA1 request */
#define ADC_CFGR_RES_Pos            2U
#define ADC_CFGR_RES_Msk            (0x7UL << ADC_CFGR_RES_Pos)  /* 000=16bit */
#define ADC_CFGR_EXTSEL_Pos         5U
#define ADC_CFGR_EXTSEL_Msk         (0x1FUL << ADC_CFGR_EXTSEL_Pos)
#define ADC_CFGR_EXTEN_Pos          10U
#define ADC_CFGR_EXTEN_Msk          (0x3UL << ADC_CFGR_EXTEN_Pos)
#define ADC_CFGR_OVRMOD             (1U << 12U)
#define ADC_CFGR_CONT               (1U << 13U)
#define ADC_CFGR_AUTDLY             (1U << 14U)
#define ADC_CFGR_DISCEN             (1U << 16U)
#define ADC_CFGR_DISCNUM_Pos        17U
#define ADC_CFGR_DISCNUM_Msk        (0x7UL << ADC_CFGR_DISCNUM_Pos)
#define ADC_CFGR_JDISCEN            (1U << 20U)
#define ADC_CFGR_JQM                (1U << 21U)
#define ADC_CFGR_AWD1SGL            (1U << 22U)
#define ADC_CFGR_AWD1EN             (1U << 23U)
#define ADC_CFGR_JAWD1EN            (1U << 24U)
#define ADC_CFGR_JAUTO              (1U << 25U)
#define ADC_CFGR_AWD1CH_Pos         26U
#define ADC_CFGR_AWD1CH_Msk         (0x1FUL << ADC_CFGR_AWD1CH_Pos)
#define ADC_CFGR_JQDIS              (1U << 31U)

/* ADC_SMPR1: channels 0-9, 3 bits each */
#define ADC_SMPR1_SMP0_Pos          0U
#define ADC_SMPR1_SMP1_Pos          3U
#define ADC_SMPR1_SMP2_Pos          6U
#define ADC_SMPR1_SMP3_Pos          9U
#define ADC_SMPR1_SMP4_Pos          12U
#define ADC_SMPR1_SMP5_Pos          15U
#define ADC_SMPR1_SMP6_Pos          18U
#define ADC_SMPR1_SMP7_Pos          21U
#define ADC_SMPR1_SMP8_Pos          24U
#define ADC_SMPR1_SMP9_Pos          27U

/* ADC_SMPR2: channels 10-18, 3 bits each */
#define ADC_SMPR2_SMP10_Pos         0U
#define ADC_SMPR2_SMP11_Pos         3U
#define ADC_SMPR2_SMP12_Pos         6U
#define ADC_SMPR2_SMP13_Pos         9U
#define ADC_SMPR2_SMP14_Pos         12U
#define ADC_SMPR2_SMP15_Pos         15U
#define ADC_SMPR2_SMP16_Pos         18U
#define ADC_SMPR2_SMP17_Pos         21U
#define ADC_SMPR2_SMP18_Pos         24U

/* ADC_SQR1 */
#define ADC_SQR1_L_Pos              0U
#define ADC_SQR1_L_Msk              (0xFUL << ADC_SQR1_L_Pos)
#define ADC_SQR1_SQ1_Pos            6U
#define ADC_SQR1_SQ1_Msk            (0x1FUL << ADC_SQR1_SQ1_Pos)
#define ADC_SQR1_SQ2_Pos            12U
#define ADC_SQR1_SQ2_Msk            (0x1FUL << ADC_SQR1_SQ2_Pos)
#define ADC_SQR1_SQ3_Pos            18U
#define ADC_SQR1_SQ3_Msk            (0x1FUL << ADC_SQR1_SQ3_Pos)
#define ADC_SQR1_SQ4_Pos            24U
#define ADC_SQR1_SQ4_Msk            (0x1FUL << ADC_SQR1_SQ4_Pos)

/* ADC_CCR (common control, ADC12_COMMON/ADC3_COMMON + 0x08) */
#define ADC_CCR_CKMODE_Pos          16U
#define ADC_CCR_CKMODE_Msk          (0x3UL << ADC_CCR_CKMODE_Pos)
#define ADC_CCR_PRESC_Pos           18U
#define ADC_CCR_PRESC_Msk           (0xFUL << ADC_CCR_PRESC_Pos)
#define ADC_CCR_VREFEN              (1U << 22U)
#define ADC_CCR_TSEN                (1U << 23U)
#define ADC_CCR_VBATEN              (1U << 24U)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __STM32H750xx_H */
