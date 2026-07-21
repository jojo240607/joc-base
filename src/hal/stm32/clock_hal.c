#include "clock_hal.h"
#include "stm32f4xx.h"

#define CLOCK_HSE_HZ    8000000UL
#define CLOCK_SYSCLK_HZ 168000000UL

void clock_hal_configure(void)
{
    /* Enable HSE (8 MHz on the Discovery board) */
    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0) { }

    /* Flash latency: 5 WS @ 3.3 V, I/D caches ON, prefetch OFF (test: ART
     * prefetch may cause boot hang on non-128-bit-aligned firmware sizes). */
    FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_LATENCY_5WS;

    /* PLL: M=8 -> 1 MHz, N=336 -> 336 MHz, P=2 -> 168 MHz, Q=7 -> 48 MHz, SRC=HSE */
    RCC->PLLCFGR = (8U   << RCC_PLLCFGR_PLLM_Pos)
                 | (336U << RCC_PLLCFGR_PLLN_Pos)
                 | (0U   << RCC_PLLCFGR_PLLP_Pos)
                 | (7U   << RCC_PLLCFGR_PLLQ_Pos)
                 | RCC_PLLCFGR_PLLSRC_HSE;

    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0) { }

    /* Bus prescalers: AHB /1, APB1 /4 (42 MHz), APB2 /2 (84 MHz) */
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;

    /* Select PLL as system clock */
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }

    SystemCoreClock = CLOCK_SYSCLK_HZ;
}

uint32_t clock_hal_sysclk_hz(void)
{
    return CLOCK_SYSCLK_HZ;
}
