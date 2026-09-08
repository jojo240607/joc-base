/**
  ******************************************************************************
  * @file    stm32f103xx.h
  * @brief   CMSIS STM32F103xx Device Peripheral Access Layer Header File.
  *
  *          Minimal register definitions for jOS RTOS port. Covers only the
  *          peripherals used by the F1 HAL: RCC, GPIO, USART, AFIO, FLASH,
  *          EXTI, DMA, SPI, I2C, TIM, ADC, IWDG, RTC, PWR, CRC, CAN.
  ******************************************************************************
  */
#ifndef __STM32F103xx_H
#define __STM32F103xx_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** @addtogroup Configuration_section_for_CMSIS
  * @{
  */
#define __CM3_REV                 0x0001U  /* Core revision r0p1 */
#define __MPU_PRESENT             0U       /* No MPU on F103       */
#define __NVIC_PRIO_BITS          4U       /* 4 bits for priority  */
#define __Vendor_SysTickConfig    0U       /* Standard SysTick     */
/** @} */

/** @addtogbrief IRQn_Type
  * @{
  */
typedef enum IRQn {
    /* Cortex-M3 Core Exceptions (negative numbers, from cortex_m.h) */
    NonMaskableInt_IRQn = -14,
    HardFault_IRQn      = -13,
    SVCall_IRQn         = -5,
    PendSV_IRQn         = -2,
    SysTick_IRQn        = -1,
    /* STM32F103 Device-specific Interrupts */
    WWDG_IRQn           = 0,
    PVD_IRQn            = 1,
    TAMPER_IRQn         = 2,
    RTC_IRQn            = 3,
    FLASH_IRQn          = 4,
    RCC_IRQn            = 5,
    EXTI0_IRQn          = 6,
    EXTI1_IRQn          = 7,
    EXTI2_IRQn          = 8,
    EXTI3_IRQn          = 9,
    EXTI4_IRQn          = 10,
    DMA1_Channel1_IRQn  = 11,
    DMA1_Channel2_IRQn  = 12,
    DMA1_Channel3_IRQn  = 13,
    DMA1_Channel4_IRQn  = 14,
    DMA1_Channel5_IRQn  = 15,
    DMA1_Channel6_IRQn  = 16,
    DMA1_Channel7_IRQn  = 17,
    ADC1_2_IRQn         = 18,
    USB_HP_CAN_TX_IRQn  = 19,
    USB_LP_CAN_RX0_IRQn = 20,
    CAN_RX1_IRQn        = 21,
    CAN_SCE_IRQn        = 22,
    EXTI9_5_IRQn        = 23,
    TIM1_BRK_IRQn       = 24,
    TIM1_UP_IRQn        = 25,
    TIM1_TRG_COM_IRQn   = 26,
    TIM1_CC_IRQn        = 27,
    TIM2_IRQn           = 28,
    TIM3_IRQn           = 29,
    TIM4_IRQn           = 30,
    I2C1_EV_IRQn        = 31,
    I2C1_ER_IRQn        = 32,
    I2C2_EV_IRQn        = 33,
    I2C2_ER_IRQn        = 34,
    SPI1_IRQn           = 35,
    SPI2_IRQn           = 36,
    USART1_IRQn         = 37,
    USART2_IRQn         = 38,
    USART3_IRQn         = 39,
    EXTI15_10_IRQn      = 40,
    RTC_Alarm_IRQn      = 41,
    USBWakeUp_IRQn      = 42,
} IRQn_Type;
/** @} */

#include "core_cm3.h"

/* ADC prescaler selectors (RCC_CFGR bit 15:14). */
#define RCC_CFGR_ADCPRE_0  (1U << 14U)   /* PCLK2 / 2 */
#define RCC_CFGR_ADCPRE_1  (1U << 15U)   /* PCLK2 / 4 */
#define RCC_CFGR_ADCPRE    (3U << 14U)   /* mask */
#define RCC_CFGR_ADCPRE_DIV2   (0U << 14U)
#define RCC_CFGR_ADCPRE_DIV4   (1U << 14U)
#define RCC_CFGR_ADCPRE_DIV6   (2U << 14U)
#define RCC_CFGR_ADCPRE_DIV8   (3U << 14U)

/* ======================== Peripheral Base Addresses ======================== */

#define APB1PERIPH_BASE  0x40000000UL
#define APB2PERIPH_BASE  0x40010000UL
#define AHBPERIPH_BASE   0x40018000UL

#define TIM2_BASE        (APB1PERIPH_BASE + 0x0000UL)
#define TIM3_BASE        (APB1PERIPH_BASE + 0x0400UL)
#define TIM4_BASE        (APB1PERIPH_BASE + 0x0800UL)
#define TIM5_BASE        (APB1PERIPH_BASE + 0x0C00UL)
#define TIM6_BASE        (APB1PERIPH_BASE + 0x1000UL)
#define TIM7_BASE        (APB1PERIPH_BASE + 0x1400UL)
#define RTC_BASE         (APB1PERIPH_BASE + 0x2800UL)
#define IWDG_BASE        (APB1PERIPH_BASE + 0x3000UL)
#define SPI2_BASE        (APB1PERIPH_BASE + 0x3800UL)
#define USART2_BASE      (APB1PERIPH_BASE + 0x4400UL)
#define USART3_BASE      (APB1PERIPH_BASE + 0x4800UL)
#define I2C1_BASE        (APB1PERIPH_BASE + 0x5400UL)
#define I2C2_BASE        (APB1PERIPH_BASE + 0x5800UL)
#define CAN1_BASE        (APB1PERIPH_BASE + 0x6400UL)
#define BKP_BASE         (APB1PERIPH_BASE + 0x6C00UL)
#define PWR_BASE         (APB1PERIPH_BASE + 0x7000UL)
#define DAC_BASE         (APB1PERIPH_BASE + 0x7400UL)

#define AFIO_BASE        (APB2PERIPH_BASE + 0x0000UL)
#define EXTI_BASE        (APB2PERIPH_BASE + 0x0400UL)
#define RCC_BASE         0x40021000UL
#define GPIOA_BASE       (APB2PERIPH_BASE + 0x0800UL)
#define GPIOB_BASE       (APB2PERIPH_BASE + 0x0C00UL)
#define GPIOC_BASE       (APB2PERIPH_BASE + 0x1000UL)
#define GPIOD_BASE       (APB2PERIPH_BASE + 0x1400UL)
#define GPIOE_BASE       (APB2PERIPH_BASE + 0x1800UL)
#define GPIOF_BASE       (APB2PERIPH_BASE + 0x1C00UL)
#define GPIOG_BASE       (APB2PERIPH_BASE + 0x2000UL)
#define ADC1_BASE        (APB2PERIPH_BASE + 0x2400UL)
#define ADC2_BASE        (APB2PERIPH_BASE + 0x2800UL)
#define TIM1_BASE        (APB2PERIPH_BASE + 0x2C00UL)
#define SPI1_BASE        (APB2PERIPH_BASE + 0x3000UL)
#define USART1_BASE      (APB2PERIPH_BASE + 0x3800UL)

#define DMA1_BASE        (AHBPERIPH_BASE + 0x8000UL)
#define DMA1_Channel1_BASE (DMA1_BASE + 0x0008UL)
#define DMA1_Channel2_BASE (DMA1_BASE + 0x001CUL)  /* sic — F1 DMA channels
                                                       are 0x14 apart */
#define DMA1_Channel3_BASE (DMA1_BASE + 0x0030UL)
#define DMA1_Channel4_BASE (DMA1_BASE + 0x0044UL)
#define DMA1_Channel5_BASE (DMA1_BASE + 0x0058UL)
#define DMA1_Channel6_BASE (DMA1_BASE + 0x006CUL)
#define DMA1_Channel7_BASE (DMA1_BASE + 0x0080UL)

#define FLASH_R_BASE     0x40022000UL    /* FLASH controller base (APB mapped at 0x40022000) */
#define CRC_BASE         0x40023000UL    /* CRC base (APB mapped at 0x40023000) */

/* ======================== Peripheral Register Structures ======================== */

/* ---- GPIO ---- */
typedef struct {
    volatile uint32_t CRL;    /* 0x00: Port config low  (pins 0-7)  */
    volatile uint32_t CRH;    /* 0x04: Port config high (pins 8-15) */
    volatile uint32_t IDR;    /* 0x08: Input data register          */
    volatile uint32_t ODR;    /* 0x0C: Output data register         */
    volatile uint32_t BSRR;   /* 0x10: Bit set/reset register       */
    volatile uint32_t BRR;    /* 0x14: Bit reset register           */
    volatile uint32_t LCKR;   /* 0x18: Lock register                */
} GPIO_TypeDef;

/* ---- USART ---- */
typedef struct {
    volatile uint32_t SR;     /* 0x00: Status register              */
    volatile uint32_t DR;     /* 0x04: Data register                */
    volatile uint32_t BRR;    /* 0x08: Baud rate register           */
    volatile uint32_t CR1;    /* 0x0C: Control register 1           */
    volatile uint32_t CR2;    /* 0x10: Control register 2           */
    volatile uint32_t CR3;    /* 0x14: Control register 3           */
    volatile uint32_t GTPR;   /* 0x18: Guard time and prescaler    */
} USART_TypeDef;

/* ---- RCC ---- */
typedef struct {
    volatile uint32_t CR;      /* 0x00: Clock control register      */
    volatile uint32_t CFGR;    /* 0x04: Clock configuration         */
    volatile uint32_t CIR;     /* 0x08: Clock interrupt register    */
    volatile uint32_t APB2RSTR;/* 0x0C: APB2 peripheral reset      */
    volatile uint32_t APB1RSTR;/* 0x10: APB1 peripheral reset      */
    volatile uint32_t AHBENR;  /* 0x14: AHB peripheral clock enable */
    volatile uint32_t APB2ENR; /* 0x18: APB2 peripheral clock enable*/
    volatile uint32_t APB1ENR; /* 0x1C: APB1 peripheral clock enable*/
    volatile uint32_t BDCR;    /* 0x20: Backup domain control       */
    volatile uint32_t CSR;     /* 0x24: Control/status register     */
} RCC_TypeDef;

/* ---- FLASH ---- */
typedef struct {
    volatile uint32_t ACR;     /* 0x00: Flash access control        */
    volatile uint32_t KEYR;    /* 0x04: FPEC key register           */
    volatile uint32_t OPTKEYR; /* 0x08: Option byte key register    */
    volatile uint32_t SR;      /* 0x0C: Status register             */
    volatile uint32_t CR;      /* 0x10: Control register            */
    volatile uint32_t AR;      /* 0x14: Address register            */
    volatile uint32_t OBR;     /* 0x18: Option byte register        */
    volatile uint32_t WRPR;    /* 0x1C: Write protection register   */
} FLASH_TypeDef;

/* ---- AFIO ---- */
typedef struct {
    volatile uint32_t EVCR;    /* 0x00: Event control register      */
    volatile uint32_t MAPR;    /* 0x04: AFIO remap register         */
    volatile uint32_t EXTICR1; /* 0x08: External interrupt cfg 1    */
    volatile uint32_t EXTICR2; /* 0x0C: External interrupt cfg 2    */
    volatile uint32_t EXTICR3; /* 0x10: External interrupt cfg 3    */
    volatile uint32_t EXTICR4; /* 0x14: External interrupt cfg 4    */
} AFIO_TypeDef;

/* ---- EXTI ---- */
typedef struct {
    volatile uint32_t IMR;     /* 0x00: Interrupt mask register     */
    volatile uint32_t EMR;     /* 0x04: Event mask register         */
    volatile uint32_t RTSR;    /* 0x08: Rising trigger selection    */
    volatile uint32_t FTSR;    /* 0x0C: Falling trigger selection   */
    volatile uint32_t SWIER;   /* 0x10: Software interrupt event    */
    volatile uint32_t PR;      /* 0x14: Pending register            */
} EXTI_TypeDef;

/* ---- DMA (F1) ---- */
typedef struct {
    volatile uint32_t ISR;     /* 0x00: Interrupt status register   */
    volatile uint32_t IFCR;    /* 0x04: Interrupt flag clear        */
} DMA_TypeDef;

typedef struct {
    volatile uint32_t CCR;     /* 0x00: Channel config register     */
    volatile uint32_t CNDTR;   /* 0x04: Number of data register     */
    volatile uint32_t CPAR;    /* 0x08: Peripheral address register */
    volatile uint32_t CMAR;    /* 0x0C: Memory address register     */
} DMA_Channel_TypeDef;

/* ---- SPI ---- */
typedef struct {
    volatile uint32_t CR1;     /* 0x00: Control register 1          */
    volatile uint32_t CR2;     /* 0x04: Control register 2          */
    volatile uint32_t SR;      /* 0x08: Status register             */
    volatile uint32_t DR;      /* 0x0C: Data register               */
    volatile uint32_t CRCPR;   /* 0x10: CRC polynomial register     */
    volatile uint32_t RXCRCR;  /* 0x14: RX CRC register             */
    volatile uint32_t TXCRCR;  /* 0x18: TX CRC register             */
    volatile uint32_t I2SCFGR; /* 0x1C: I2S config register         */
    volatile uint32_t I2SPR;   /* 0x20: I2S prescaler register      */
} SPI_TypeDef;

/* ---- I2C ---- */
typedef struct {
    volatile uint32_t CR1;     /* 0x00: Control register 1          */
    volatile uint32_t CR2;     /* 0x04: Control register 2          */
    volatile uint32_t OAR1;    /* 0x08: Own address register 1      */
    volatile uint32_t OAR2;    /* 0x0C: Own address register 2      */
    volatile uint32_t DR;      /* 0x10: Data register               */
    volatile uint32_t SR1;     /* 0x14: Status register 1           */
    volatile uint32_t SR2;     /* 0x18: Status register 2           */
    volatile uint32_t CCR;     /* 0x1C: Clock control register      */
    volatile uint32_t TRISE;   /* 0x20: TRISE register              */
} I2C_TypeDef;

/* ---- TIM (general-purpose, with TIM1 advanced fields) ---- */
typedef struct {
    volatile uint32_t CR1;     /* 0x00: Control register 1          */
    volatile uint32_t CR2;     /* 0x04: Control register 2          */
    volatile uint32_t SMCR;    /* 0x08: Slave mode control          */
    volatile uint32_t DIER;    /* 0x0C: DMA/Interrupt enable        */
    volatile uint32_t SR;      /* 0x10: Status register             */
    volatile uint32_t EGR;     /* 0x14: Event generation register   */
    volatile uint32_t CCMR1;   /* 0x18: CC mode register 1          */
    volatile uint32_t CCMR2;   /* 0x1C: CC mode register 2          */
    volatile uint32_t CCER;    /* 0x20: CC enable register          */
    volatile uint32_t CNT;     /* 0x24: Counter                     */
    volatile uint32_t PSC;     /* 0x28: Prescaler                   */
    volatile uint32_t ARR;     /* 0x2C: Auto-reload register        */
    volatile uint32_t RCR;     /* 0x30: Repetition cnt (TIM1 only)  */
    volatile uint32_t CCR1;    /* 0x34: Capture/compare 1           */
    volatile uint32_t CCR2;    /* 0x38: Capture/compare 2           */
    volatile uint32_t CCR3;    /* 0x3C: Capture/compare 3           */
    volatile uint32_t CCR4;    /* 0x40: Capture/compare 4           */
    volatile uint32_t BDTR;    /* 0x44: Break&dead-time (TIM1 only) */
    volatile uint32_t DCR;     /* 0x48: DMA control (TIM1 only)     */
    volatile uint32_t DMAR;    /* 0x4C: DMA addr for burst (TIM1)   */
} TIM_TypeDef;

/* ---- IWDG ---- */
typedef struct {
    volatile uint32_t KR;      /* 0x00: Key register                */
    volatile uint32_t PR;      /* 0x04: Prescaler register          */
    volatile uint32_t RLR;     /* 0x08: Reload register             */
    volatile uint32_t SR;      /* 0x0C: Status register             */
} IWDG_TypeDef;

/* ---- RTC (F1 basic) ---- */
typedef struct {
    volatile uint32_t CRH;     /* 0x00: Config high (interrupts)    */
    volatile uint32_t CRL;     /* 0x04: Config low                  */
    volatile uint32_t PRLH;    /* 0x08: Prescaler load high         */
    volatile uint32_t PRLL;    /* 0x0C: Prescaler load low          */
    volatile uint32_t DIVH;    /* 0x10: Divider high                */
    volatile uint32_t DIVL;    /* 0x14: Divider low                 */
    volatile uint32_t CNTH;    /* 0x18: Counter high                */
    volatile uint32_t CNTL;    /* 0x1C: Counter low                 */
    volatile uint32_t ALRH;    /* 0x20: Alarm high                  */
    volatile uint32_t ALRL;    /* 0x24: Alarm low                   */
} RTC_TypeDef;

/* ---- ADC ---- */
typedef struct {
    volatile uint32_t SR;      /* 0x00: Status register             */
    volatile uint32_t CR1;     /* 0x04: Control register 1          */
    volatile uint32_t CR2;     /* 0x08: Control register 2          */
    volatile uint32_t SMPR1;   /* 0x0C: Sample time register 1      */
    volatile uint32_t SMPR2;   /* 0x10: Sample time register 2      */
    volatile uint32_t JOFR1;   /* 0x14: Injected data offset 1      */
    volatile uint32_t JOFR2;   /* 0x18: Injected data offset 2      */
    volatile uint32_t JOFR3;   /* 0x1C: Injected data offset 3      */
    volatile uint32_t JOFR4;   /* 0x20: Injected data offset 4      */
    volatile uint32_t HTR;     /* 0x24: Watchdog high threshold     */
    volatile uint32_t LTR;     /* 0x28: Watchdog low threshold      */
    volatile uint32_t SQR1;    /* 0x2C: Sequence register 1         */
    volatile uint32_t SQR2;    /* 0x30: Sequence register 2         */
    volatile uint32_t SQR3;    /* 0x34: Sequence register 3         */
    volatile uint32_t JSQR;    /* 0x38: Injected sequence register  */
    volatile uint32_t JDR1;    /* 0x3C: Injected data 1             */
    volatile uint32_t JDR2;    /* 0x40: Injected data 2             */
    volatile uint32_t JDR3;    /* 0x44: Injected data 3             */
    volatile uint32_t JDR4;    /* 0x48: Injected data 4             */
    volatile uint32_t DR;      /* 0x4C: Data register               */
} ADC_TypeDef;

/* ---- CRC ---- */
typedef struct {
    volatile uint32_t DR;      /* 0x00: Data register               */
    volatile uint32_t IDR;     /* 0x04: Independent data register   */
    volatile uint32_t CR;      /* 0x08: Control register            */
} CRC_TypeDef;

/* ---- CAN (basic mailbox subset) ---- */
typedef struct {
    volatile uint32_t MCR;     /* 0x00: Master control              */
    volatile uint32_t MSR;     /* 0x04: Master status               */
    volatile uint32_t TSR;     /* 0x08: TX status                   */
    volatile uint32_t RF0R;    /* 0x0C: RX FIFO 0                  */
    volatile uint32_t RF1R;    /* 0x10: RX FIFO 1                  */
    volatile uint32_t IER;     /* 0x14: Interrupt enable            */
    volatile uint32_t ESR;     /* 0x18: Error status                */
    volatile uint32_t BTR;     /* 0x1C: Bit timing                  */
} CAN_TypeDef;

/* ---- PWR ---- */
typedef struct {
    volatile uint32_t CR;      /* 0x00: Power control register       */
    volatile uint32_t CSR;     /* 0x04: Power control/status         */
} PWR_TypeDef;

/* ---- WWDG ---- */
typedef struct {
    volatile uint32_t CR;      /* 0x00: Control register (counter)   */
    volatile uint32_t CFR;     /* 0x04: Configuration register       */
    volatile uint32_t SR;      /* 0x08: Status register              */
} WWDG_TypeDef;

/* ---- DAC ---- */
typedef struct {
    volatile uint32_t CR;        /* 0x00: Control register           */
    volatile uint32_t SWTRIGR;   /* 0x04: Software trigger           */
    volatile uint32_t DHR12R1;   /* 0x08: Ch1 12-bit right-aligned  */
    volatile uint32_t DHR12L1;   /* 0x0C: Ch1 12-bit left-aligned   */
    volatile uint32_t DHR8R1;    /* 0x10: Ch1 8-bit right-aligned   */
    volatile uint32_t DHR12R2;   /* 0x14: Ch2 12-bit right-aligned  */
    volatile uint32_t DHR12L2;   /* 0x18: Ch2 12-bit left-aligned   */
    volatile uint32_t DHR8R2;    /* 0x1C: Ch2 8-bit right-aligned   */
    volatile uint32_t DHR12RD;   /* 0x20: Dual 12-bit right-aligned */
    volatile uint32_t DHR12LD;   /* 0x24: Dual 12-bit left-aligned  */
    volatile uint32_t DHR8RD;    /* 0x28: Dual 8-bit right-aligned  */
    volatile uint32_t DOR1;      /* 0x2C: Ch1 data output           */
    volatile uint32_t DOR2;      /* 0x30: Ch2 data output           */
    volatile uint32_t SR;        /* 0x34: Status register           */
} DAC_TypeDef;

/* ======================== Peripheral Declarations ======================== */

#define GPIOA               ((GPIO_TypeDef *)GPIOA_BASE)
#define GPIOB               ((GPIO_TypeDef *)GPIOB_BASE)
#define GPIOC               ((GPIO_TypeDef *)GPIOC_BASE)
#define GPIOD               ((GPIO_TypeDef *)GPIOD_BASE)
#define GPIOE               ((GPIO_TypeDef *)GPIOE_BASE)
#define GPIOF               ((GPIO_TypeDef *)GPIOF_BASE)
#define GPIOG               ((GPIO_TypeDef *)GPIOG_BASE)

#define USART1              ((USART_TypeDef *)USART1_BASE)
#define USART2              ((USART_TypeDef *)USART2_BASE)
#define USART3              ((USART_TypeDef *)USART3_BASE)

#define RCC                 ((RCC_TypeDef *)RCC_BASE)
#define FLASH               ((FLASH_TypeDef *)FLASH_R_BASE)
#define AFIO                ((AFIO_TypeDef *)AFIO_BASE)
#define EXTI                ((EXTI_TypeDef *)EXTI_BASE)
#define DMA1                ((DMA_TypeDef *)DMA1_BASE)
#define SPI1                ((SPI_TypeDef *)SPI1_BASE)
#define SPI2                ((SPI_TypeDef *)SPI2_BASE)
#define I2C1                ((I2C_TypeDef *)I2C1_BASE)
#define I2C2                ((I2C_TypeDef *)I2C2_BASE)
#define TIM1                ((TIM_TypeDef *)TIM1_BASE)
#define TIM2                ((TIM_TypeDef *)TIM2_BASE)
#define TIM3                ((TIM_TypeDef *)TIM3_BASE)
#define TIM4                ((TIM_TypeDef *)TIM4_BASE)
#define TIM5                ((TIM_TypeDef *)TIM5_BASE)
#define TIM6                ((TIM_TypeDef *)TIM6_BASE)
#define TIM7                ((TIM_TypeDef *)TIM7_BASE)
#define ADC1                ((ADC_TypeDef *)ADC1_BASE)
#define ADC2                ((ADC_TypeDef *)ADC2_BASE)
#define IWDG                ((IWDG_TypeDef *)IWDG_BASE)
#define RTC                 ((RTC_TypeDef *)RTC_BASE)
#define CRC                 ((CRC_TypeDef *)CRC_BASE)
#define CAN1                ((CAN_TypeDef *)CAN1_BASE)
#define PWR                 ((PWR_TypeDef *)PWR_BASE)
#define WWDG                ((WWDG_TypeDef *)WWDG_BASE)
#define DAC                 ((DAC_TypeDef *)DAC_BASE)

/* ---- FLASH_BASE and other constants ---- */
#ifndef FLASH_BASE
#define FLASH_BASE          0x08000000UL
#endif

/* ---- SystemCoreClock ---- */
extern uint32_t SystemCoreClock;

/* ======================== RCC Bit Definitions ======================== */

/* RCC_CR */
#define RCC_CR_HSION        0x00000001U
#define RCC_CR_HSIRDY       0x00000002U
#define RCC_CR_HSEON        0x00010000U
#define RCC_CR_HSERDY       0x00020000U
#define RCC_CR_PLLON        0x01000000U
#define RCC_CR_PLLRDY       0x02000000U

/* RCC_CFGR bit positions */
#define RCC_CFGR_SW_Pos     0U
#define RCC_CFGR_SW_Msk     (0x3UL << RCC_CFGR_SW_Pos)
#define RCC_CFGR_SW         RCC_CFGR_SW_Msk
#define RCC_CFGR_SW_HSE     (1U << 0U)
#define RCC_CFGR_SW_PLL     (2U << 0U)

#define RCC_CFGR_SWS_Pos    2U
#define RCC_CFGR_SWS_Msk    (0x3UL << RCC_CFGR_SWS_Pos)
#define RCC_CFGR_SWS        RCC_CFGR_SWS_Msk
#define RCC_CFGR_SWS_HSE    (1U << 2U)
#define RCC_CFGR_SWS_PLL    (2U << 2U)

#define RCC_CFGR_HPRE_Pos   4U
#define RCC_CFGR_HPRE_Msk   (0xFUL << RCC_CFGR_HPRE_Pos)
#define RCC_CFGR_HPRE       RCC_CFGR_HPRE_Msk
#define RCC_CFGR_HPRE_DIV1  (0U << 4U)

#define RCC_CFGR_PPRE1_Pos  8U
#define RCC_CFGR_PPRE1_Msk  (0x7UL << RCC_CFGR_PPRE1_Pos)
#define RCC_CFGR_PPRE1      RCC_CFGR_PPRE1_Msk
#define RCC_CFGR_PPRE1_DIV1 (0U << 8U)
#define RCC_CFGR_PPRE1_DIV2 (4U << 8U)

#define RCC_CFGR_PPRE2_Pos  11U
#define RCC_CFGR_PPRE2_Msk  (0x7UL << RCC_CFGR_PPRE2_Pos)
#define RCC_CFGR_PPRE2      RCC_CFGR_PPRE2_Msk
#define RCC_CFGR_PPRE2_DIV1 (0U << 11U)

#define RCC_CFGR_PLLSRC_Pos 16U
#define RCC_CFGR_PLLSRC     (1U << 16U)

/* PLLXTPRE: bit 17, HSE divider before PLL */
#define RCC_CFGR_PLLXTPRE_Pos 17U
#define RCC_CFGR_PLLXTPRE    (1U << 17U)

/* PLLMULL: bits 18-21 */
#define RCC_CFGR_PLLMULL_Pos 18U
#define RCC_CFGR_PLLMULL_Msk (0xFUL << RCC_CFGR_PLLMULL_Pos)
#define RCC_CFGR_PLLMULL     RCC_CFGR_PLLMULL_Msk
#define RCC_CFGR_PLLMULL9    (7U << 18U)   /* PLL x9 */

/* Convenience: HSE as PLL source (PLLSRC=1) */
#define RCC_CFGR_PLLSRC_HSE_PREDIV RCC_CFGR_PLLSRC

/* RCC_CSR */
#define RCC_CSR_LSION       0x00000001U
#define RCC_CSR_LSIRDY      0x00000002U
#define RCC_CSR_RMVF        0x01000000U
#define RCC_CSR_PINRSTF     0x04000000U
#define RCC_CSR_PORRSTF     0x08000000U
#define RCC_CSR_SFTRSTF     0x10000000U
#define RCC_CSR_IWDGRSTF    0x20000000U
#define RCC_CSR_WWDGRSTF    0x40000000U
#define RCC_CSR_LPWRRSTF    0x80000000U

/* RCC_APB2ENR */
#define RCC_APB2ENR_AFIOEN     0x00000001U
#define RCC_APB2ENR_IOPAEN     (1U << 2U)   /* bit 2: GPIOA enable */
#define RCC_APB2ENR_IOPBEN     (1U << 3U)   /* bit 3: GPIOB enable */
#define RCC_APB2ENR_IOPCEN     (1U << 4U)   /* bit 4: GPIOC enable */
#define RCC_APB2ENR_IOPDEN     (1U << 5U)   /* bit 5: GPIOD enable */
#define RCC_APB2ENR_IOPEEN     (1U << 6U)   /* bit 6: GPIOE enable */
#define RCC_APB2ENR_USART1EN   (1U << 14U)
#define RCC_APB2ENR_TIM1EN     (1U << 11U)
#define RCC_APB2ENR_SPI1EN     (1U << 12U)

/* RCC_APB1ENR */
#define RCC_APB1ENR_USART2EN   (1U << 17U)
#define RCC_APB1ENR_USART3EN   (1U << 18U)
#define RCC_APB1ENR_TIM2EN     (1U << 0U)
#define RCC_APB1ENR_TIM3EN     (1U << 1U)
#define RCC_APB1ENR_TIM4EN     (1U << 2U)
#define RCC_APB1ENR_TIM5EN     (1U << 3U)
#define RCC_APB1ENR_TIM6EN     (1U << 4U)
#define RCC_APB1ENR_TIM7EN     (1U << 5U)
#define RCC_APB1ENR_SPI2EN     (1U << 14U)
#define RCC_APB1ENR_I2C1EN     (1U << 21U)
#define RCC_APB1ENR_I2C2EN     (1U << 22U)

/* ======================== I2C Bit Definitions ======================== */

/* I2C_CR1 */
#define I2C_CR1_PE        (1U << 0U)
#define I2C_CR1_SWRST     (1U << 15U)
#define I2C_CR1_START     (1U << 8U)
#define I2C_CR1_STOP      (1U << 9U)
#define I2C_CR1_ACK       (1U << 10U)
#define I2C_CR1_POS       (1U << 11U)

/* I2C_CR2 */
#define I2C_CR2_FREQ      (0x3FU << 0U)
#define I2C_CR2_ITEVTEN   (1U << 9U)
#define I2C_CR2_ITERREN   (1U << 8U)
#define I2C_CR2_DMAEN     (1U << 11U)
#define I2C_CR2_LAST      (1U << 12U)

/* I2C_SR1 */
#define I2C_SR1_SB        (1U << 0U)
#define I2C_SR1_ADDR      (1U << 1U)
#define I2C_SR1_BTF       (1U << 2U)
#define I2C_SR1_TXE       (1U << 7U)
#define I2C_SR1_RXNE      (1U << 6U)
#define I2C_SR1_AF        (1U << 10U)

/* I2C_SR2 */
#define I2C_SR2_BUSY      (1U << 1U)

/* I2C_CCR */
#define I2C_CCR_CCR       (0xFFFU << 0U)

/* I2C_TRISE */
#define I2C_TRISE_TRISE   (0x3FU << 0U)

/* ======================== SPI Bit Definitions ======================== */

/* SPI_CR1 */
#define SPI_CR1_CPHA      (1U << 0U)
#define SPI_CR1_CPOL      (1U << 1U)
#define SPI_CR1_MSTR      (1U << 2U)
#define SPI_CR1_BR        (7U << 3U)
#define SPI_CR1_SPE       (1U << 6U)
#define SPI_CR1_SSM       (1U << 9U)
#define SPI_CR1_SSI       (1U << 8U)

/* SPI_CR2 */
#define SPI_CR2_RXNEIE    (1U << 6U)
#define SPI_CR2_TXDMAEN   (1U << 1U)
#define SPI_CR2_RXDMAEN   (1U << 0U)

/* SPI_SR */
#define SPI_SR_RXNE       (1U << 0U)
#define SPI_SR_TXE        (1U << 1U)
#define SPI_SR_BSY        (1U << 7U)

/* ======================== FLASH Bit Definitions ======================== */

/* FLASH_ACR */
#define FLASH_ACR_LATENCY_0  0x00000000U  /* 0 WS, 0 < HCLK ≤ 24 MHz */
#define FLASH_ACR_LATENCY_1  0x00000001U  /* 1 WS, 24 < HCLK ≤ 48 MHz */
#define FLASH_ACR_LATENCY_2  0x00000002U  /* 2 WS, 48 < HCLK ≤ 72 MHz */

/* ======================== TIM Bit Definitions ======================== */

/* TIM_CR1 */
#define TIM_CR1_CEN            (1U << 0U)
#define TIM_CR1_URS            (1U << 2U)
#define TIM_CR1_OPM            (1U << 3U)
#define TIM_CR1_ARPE           (1U << 7U)

/* TIM_CR2 */
#define TIM_CR2_MMS_Pos        4U
#define TIM_CR2_MMS_Msk        (0x7UL << TIM_CR2_MMS_Pos)
#define TIM_CR2_MMS_UPDATE     (0x2UL << TIM_CR2_MMS_Pos)

/* TIM_DIER */
#define TIM_DIER_UIE           (1U << 0U)
#define TIM_DIER_UDE           (1U << 8U)

/* TIM_SR */
#define TIM_SR_UIF             (1U << 0U)

/* TIM_EGR */
#define TIM_EGR_UG             (1U << 0U)

/* TIM_CCMR1 output-compare mode */
#define TIM_CCMR1_OC1M_Pos     4U
#define TIM_CCMR1_OC1M_Msk     (0x7UL << TIM_CCMR1_OC1M_Pos)
#define TIM_CCMR1_OC1PE        (1U << 3U)
#define TIM_CCMR1_OC2M_Pos     12U
#define TIM_CCMR1_OC2M_Msk     (0x7UL << TIM_CCMR1_OC2M_Pos)
#define TIM_CCMR1_OC2PE        (1U << 11U)

/* TIM_CCMR2 output-compare mode */
#define TIM_CCMR2_OC3M_Pos     4U
#define TIM_CCMR2_OC3M_Msk     (0x7UL << TIM_CCMR2_OC3M_Pos)
#define TIM_CCMR2_OC3PE        (1U << 3U)
#define TIM_CCMR2_OC4M_Pos     12U
#define TIM_CCMR2_OC4M_Msk     (0x7UL << TIM_CCMR2_OC4M_Pos)
#define TIM_CCMR2_OC4PE        (1U << 11U)

/* TIM_CCER */
#define TIM_CCER_CC1E          (1U << 0U)
#define TIM_CCER_CC1P          (1U << 1U)
#define TIM_CCER_CC1NE         (1U << 2U)
#define TIM_CCER_CC1NP         (1U << 3U)
#define TIM_CCER_CC2E          (1U << 4U)
#define TIM_CCER_CC2P          (1U << 5U)
#define TIM_CCER_CC2NE         (1U << 6U)
#define TIM_CCER_CC2NP         (1U << 7U)
#define TIM_CCER_CC3E          (1U << 8U)
#define TIM_CCER_CC3P          (1U << 9U)
#define TIM_CCER_CC3NE         (1U << 10U)
#define TIM_CCER_CC3NP         (1U << 11U)
#define TIM_CCER_CC4E          (1U << 12U)
#define TIM_CCER_CC4P          (1U << 13U)
#define TIM_CCER_CC4NE         (1U << 14U)
#define TIM_CCER_CC4NP         (1U << 15U)

/* TIM_BDTR (TIM1 advanced timer only) */
#define TIM_BDTR_DTG_Msk       (0xFFU << 0U)
#define TIM_BDTR_DTG           (0xFFU << 0U)
#define TIM_BDTR_LOCK_Msk      (0x3U << 8U)
#define TIM_BDTR_OSSI           (1U << 10U)
#define TIM_BDTR_OSSR           (1U << 11U)
#define TIM_BDTR_BKE            (1U << 12U)
#define TIM_BDTR_BKP            (1U << 13U)
#define TIM_BDTR_AOE            (1U << 14U)
#define TIM_BDTR_MOE            (1U << 15U)

/* ======================== USART Bit Definitions ======================== */

/* USART_SR */
#define USART_SR_PE       0x00000001U
#define USART_SR_FE       0x00000002U
#define USART_SR_NE       0x00000004U
#define USART_SR_ORE      0x00000008U
#define USART_SR_IDLE     0x00000010U
#define USART_SR_RXNE     0x00000020U
#define USART_SR_TC       0x00000040U
#define USART_SR_TXE      0x00000080U

/* USART_CR1 */
#define USART_CR1_SBK     0x00000001U
#define USART_CR1_RWU     0x00000002U
#define USART_CR1_RE      0x00000004U
#define USART_CR1_TE      0x00000008U
#define USART_CR1_IDLEIE  0x00000010U
#define USART_CR1_RXNEIE  0x00000020U
#define USART_CR1_TCIE    0x00000040U
#define USART_CR1_TXEIE   0x00000080U
#define USART_CR1_PEIE    0x00000100U
#define USART_CR1_PS      0x00000200U
#define USART_CR1_PCE     0x00000400U
#define USART_CR1_WAKE    0x00000800U
#define USART_CR1_M       0x00001000U
#define USART_CR1_UE      0x00002000U

/* USART_CR2 */
#define USART_CR2_STOP_0  0x00001000U
#define USART_CR2_STOP_1  0x00002000U

/* USART_CR3 */
#define USART_CR3_DMAT    0x00000080U
#define USART_CR3_DMAR    0x00000040U

/* ======================== SCB ICSR (from core_cm3.h, convenience) ======================== */

#ifndef SCB_ICSR_VECTACTIVE_Pos
#define SCB_ICSR_VECTACTIVE_Pos 0U
#endif
#ifndef SCB_ICSR_VECTACTIVE_Msk
#define SCB_ICSR_VECTACTIVE_Msk (0x1FFUL << SCB_ICSR_VECTACTIVE_Pos)
#endif

/* DMA request IDs — not defined as a macro here; dma_hal.h provides
 * the dma_req_id_t enum with DMA_REQ_NONE as an enumerator. */
/* intentionally no #define DMA_REQ_NONE — see dma_hal.h */

/* ======================== RCC Additional Bit Definitions ======================== */

#define RCC_APB2ENR_ADC1EN      (1U << 9U)
#define RCC_APB2ENR_ADC2EN      (1U << 10U)

#define RCC_APB1ENR_WWDGEN      (1U << 11U)
#define RCC_APB1ENR_DACEN       (1U << 29U)
#define RCC_APB1ENR_CAN1EN      (1U << 25U)
#define RCC_APB1ENR_PWREN       (1U << 28U)

#define RCC_AHBENR_CRCEN        (1U << 6U)

/* RCC_BDCR bits (same bit positions as F4) */
#define RCC_BDCR_LSEON          (1U << 0U)
#define RCC_BDCR_LSERDY         (1U << 1U)
#define RCC_BDCR_LSEBYP         (1U << 2U)
#define RCC_BDCR_RTCSEL         (3U << 8U)
#define RCC_BDCR_RTCSEL_LSE     (1U << 8U)
#define RCC_BDCR_RTCSEL_LSI     (2U << 8U)
#define RCC_BDCR_RTCSEL_HSE     (3U << 8U)
#define RCC_BDCR_RTCEN          (1U << 15U)
#define RCC_BDCR_BDRST          (1U << 16U)

/* ======================== ADC Bit Definitions ======================== */

/* ADC_SR */
#define ADC_SR_EOC              (1U << 1U)
#define ADC_SR_STRT             (1U << 4U)

/* ADC_CR1 */
#define ADC_CR1_EOCIE           (1U << 5U)
#define ADC_CR1_AWDIE           (1U << 6U)
#define ADC_CR1_SCAN            (1U << 8U)
#define ADC_CR1_DISCEN          (1U << 11U)

/* ADC_CR2 */
#define ADC_CR2_ADON            (1U << 0U)
#define ADC_CR2_CONT            (1U << 1U)
#define ADC_CR2_CAL             (1U << 2U)
#define ADC_CR2_RSTCAL          (1U << 3U)
#define ADC_CR2_DMA             (1U << 8U)
#define ADC_CR2_ALIGN           (1U << 11U)
#define ADC_CR2_JEXTSEL         (7U << 12U)
#define ADC_CR2_JEXTTRIG        (1U << 15U)
#define ADC_CR2_EXTSEL          (7U << 17U)
#define ADC_CR2_EXTTRIG         (1U << 20U)
#define ADC_CR2_SWSTART         (1U << 22U)
#define ADC_CR2_TSVREFE         (1U << 23U)

/* ======================== FLASH Bit Definitions ======================== */

/* FLASH_SR */
#define FLASH_SR_BSY            (1U << 0U)
#define FLASH_SR_PGERR          (1U << 2U)
#define FLASH_SR_WRPRTERR       (1U << 4U)
#define FLASH_SR_EOP            (1U << 5U)

/* FLASH_CR */
#define FLASH_CR_PG             (1U << 0U)
#define FLASH_CR_PER            (1U << 1U)
#define FLASH_CR_MER            (1U << 2U)
#define FLASH_CR_OPTPG          (1U << 4U)
#define FLASH_CR_OPTER          (1U << 5U)
#define FLASH_CR_STRT           (1U << 6U)
#define FLASH_CR_LOCK           (1U << 7U)

/* ======================== PWR Bit Definitions ======================== */

/* PWR_CR */
#define PWR_CR_DBP              (1U << 8U)

/* ======================== CAN Bit Definitions ======================== */

#define CAN_MCR_INRQ            (1U << 0U)
#define CAN_MCR_SLEEP           (1U << 1U)
#define CAN_MCR_TXFP            (1U << 2U)
#define CAN_MCR_RFLM            (1U << 3U)
#define CAN_MCR_NART            (1U << 4U)
#define CAN_MCR_AWUM            (1U << 5U)
#define CAN_MCR_ABOM            (1U << 6U)
#define CAN_MCR_TTCM            (1U << 7U)
#define CAN_MCR_RESET           0x00008000U

#define CAN_MSR_INAK            (1U << 1U)
#define CAN_MSR_SLAK            (1U << 2U)
#define CAN_MSR_ERRI            (1U << 3U)
#define CAN_MSR_WKUI            (1U << 4U)
#define CAN_MSR_SLAKI           (1U << 5U)
#define CAN_MSR_TXM             (1U << 8U)
#define CAN_MSR_RXM             (1U << 9U)
#define CAN_MSR_SAMP            (1U << 10U)
#define CAN_MSR_RX              (1U << 11U)

#define CAN_BTR_LBKM            (1U << 30U)
#define CAN_BTR_SILM            (1U << 31U)

#define CAN_RF0R_FMP0           (3U << 0U)
#define CAN_RF0R_FULL0          (1U << 3U)
#define CAN_RF0R_FOVR0          (1U << 4U)
#define CAN_RF0R_RFOM0          (1U << 5U)

#define CAN_RF1R_FMP1           (3U << 0U)
#define CAN_RF1R_FULL1          (1U << 3U)
#define CAN_RF1R_FOVR1          (1U << 4U)
#define CAN_RF1R_RFOM1          (1U << 5U)

#define CAN_ESR_EWGF            (1U << 0U)
#define CAN_ESR_EPVF            (1U << 1U)
#define CAN_ESR_BOF             (1U << 2U)
#define CAN_ESR_LEC             (7U << 4U)
#define CAN_ESR_TEC             0x00FF0000U
#define CAN_ESR_REC             0x0000FF00U

/* ======================== RTC Bit Definitions ======================== */

/* RTC_CRH */
#define RTC_CRH_SECIE           (1U << 0U)
#define RTC_CRH_ALRIE           (1U << 1U)
#define RTC_CRH_OWIE            (1U << 2U)

/* RTC_CRL */
#define RTC_CRL_SECF            (1U << 0U)
#define RTC_CRL_ALRF            (1U << 1U)
#define RTC_CRL_OWF             (1U << 2U)
#define RTC_CRL_RSF             (1U << 3U)
#define RTC_CRL_CNF             (1U << 4U)
#define RTC_CRL_RTOFF           (1U << 5U)

/* ======================== DAC Bit Definitions ======================== */

/* DAC_CR */
#define DAC_CR_EN1              (1U << 0U)
#define DAC_CR_BOFF1            (1U << 1U)
#define DAC_CR_TEN1             (1U << 2U)
#define DAC_CR_TSEL1            (7U << 3U)
#define DAC_CR_DMAEN1           (1U << 12U)
#define DAC_CR_EN2              (1U << 16U)
#define DAC_CR_BOFF2            (1U << 17U)
#define DAC_CR_TEN2             (1U << 18U)
#define DAC_CR_TSEL2            (7U << 19U)
#define DAC_CR_DMAEN2           (1U << 28U)

/* ======================== WWDG Bit Definitions ======================== */

/* WWDG_CFR bits */
#define WWDG_CFR_EWI            (1U << 9U)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __STM32F103xx_H */