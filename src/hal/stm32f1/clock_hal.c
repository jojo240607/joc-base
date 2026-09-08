#include "clock_hal.h"
#include "stm32f1xx.h"

#define CLOCK_HSE_HZ    8000000UL
#define CLOCK_SYSCLK_HZ 72000000UL

void clock_hal_configure(void)
{
    /* Enable HSE (8 MHz on typical F103 boards) */
    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0) { }

    /* Flash latency: 2 WS @ 72 MHz, 3.3 V */
    FLASH->ACR = FLASH_ACR_LATENCY_2;

    /* PLL: HSE /1 (PLLXTPRE=0), MUL=9 -> 72 MHz, SRC=HSE */
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMULL | RCC_CFGR_PLLSRC))
              | RCC_CFGR_PLLSRC_HSE_PREDIV     /* HSE as PLL source */
              | RCC_CFGR_PLLMULL9;              /* 8 MHz * 9 = 72 MHz */

    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0) { }

    /* Bus prescalers: AHB /1, APB1 /2 (36 MHz), APB2 /1 (72 MHz) */
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2))
              | RCC_CFGR_HPRE_DIV1
              | RCC_CFGR_PPRE1_DIV2             /* APB1 max 36 MHz */
              | RCC_CFGR_PPRE2_DIV1;            /* APB2 = 72 MHz */

    /* Select PLL as system clock */
    RCC->CFGR = (RCC->CFGR & (uint32_t)~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }

    SystemCoreClock = CLOCK_SYSCLK_HZ;
}

uint32_t clock_hal_sysclk_hz(void)
{
    return CLOCK_SYSCLK_HZ;
}