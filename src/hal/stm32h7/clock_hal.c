#include "clock_hal.h"
#include "stm32h7xx.h"

#define CLOCK_HSE_HZ     25000000UL
#define CLOCK_SYSCLK_HZ  480000000UL
#define CLOCK_PCLK2_HZ   120000000UL   /* APB2: USART1 kernel clock */

/* Bounded wait budget: every PLL/oscillator poll times out instead of hanging.
 * (F103 lesson: a stuck SWS poll wedges the scheduler; with a timeout the port
 * still boots on HSI and prints diagnostics — robust in Renode AND on silicon.) */
#define CLOCK_WAIT_BUDGET 1000000u

static void clock_wait_ready(volatile uint32_t *reg, uint32_t mask,
                             uint32_t expected)
{
    uint32_t budget = CLOCK_WAIT_BUDGET;
    while ((*reg & mask) != expected) {
        if (--budget == 0U) break;
    }
}

void clock_hal_configure(void)
{
    /* 1. HSE 25 MHz (H750B-DK board crystal) */
    RCC->CR |= RCC_CR_HSEON;
    clock_wait_ready(&RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY);

    /* 2. PWR VOS0 — required for 480 MHz (D3CR.VOS = 0b11, wait VOSRDY) */
    PWR->D3CR = (PWR->D3CR & ~PWR_D3CR_VOS) | PWR_D3CR_VOS_SCALE0;
    clock_wait_ready(&PWR->D3CR, PWR_D3CR_VOSRDY, PWR_D3CR_VOSRDY);

    /* 3. FLASH ACR: 2 wait states + WRHIGHFREQ for 480 MHz @ VOS0 */
    FLASH->ACR = FLASH_ACR_LATENCY_2WS | FLASH_ACR_WRHIGHFREQ_1;

    /* 4. PLL1: HSE 25 MHz / DIVM1=5 -> 5 MHz ref; N1=192 -> VCO 960 MHz;
     *    P1=1 (div /2) -> SYSCLK 480 MHz; Q1=3 (div /4) -> 240 MHz kernel
     *    (QSPI); R1=1 (div /2) -> 240 MHz. */
    RCC->PLLCKSELR = RCC_PLLCKSELR_PLLSRC_HSE
                   | (5U << RCC_PLLCKSELR_DIVM1_Pos);
    RCC->PLL1DIVR  = (192U << RCC_PLL1DIVR_N1_Pos)
                   | (1U  << RCC_PLL1DIVR_P1_Pos)
                   | (3U  << RCC_PLL1DIVR_Q1_Pos)
                   | (1U  << RCC_PLL1DIVR_R1_Pos);
    RCC->CR |= RCC_CR_PLL1ON;
    clock_wait_ready(&RCC->CR, RCC_CR_PLL1RDY, RCC_CR_PLL1RDY);

    /* 5. Bus prescalers (must be set BEFORE switching SYSCLK to PLL1):
     *    D1: HPRE /2 -> AHB 240 MHz, D1PPRE /2 -> APB3 120 MHz,
     *        D1CPRE /1 -> Cortex-M7 clock 480 MHz;
     *    D2: PPRE1 /2 -> APB1 120 MHz, PPRE2 /2 -> APB2 120 MHz;
     *    D3: D3PPRE /2 -> APB4 120 MHz. */
    RCC->D1CFGR = RCC_D1CFGR_HPRE_DIV2 | RCC_D1CFGR_D1PPRE_DIV2
                | RCC_D1CFGR_D1CPRE_DIV1;
    RCC->D2CFGR = RCC_D2CFGR_D2PPRE1_DIV2 | RCC_D2CFGR_D2PPRE2_DIV2;
    RCC->D3CFGR = RCC_D3CFGR_D3PPRE_DIV2;

    /* 6. Switch SYSCLK to PLL1 (clear SW first — never |= into a stale source) */
    RCC->CFGR = (RCC->CFGR & (uint32_t)~RCC_CFGR_SW) | RCC_CFGR_SW_PLL1;
    clock_wait_ready(&RCC->CFGR, RCC_CFGR_SWS_Msk, RCC_CFGR_SWS_PLL1);

    SystemCoreClock = CLOCK_SYSCLK_HZ;
}

uint32_t clock_hal_sysclk_hz(void)
{
    return CLOCK_SYSCLK_HZ;
}

uint32_t clock_hal_usart_hz(void)
{
    return CLOCK_PCLK2_HZ;
}
