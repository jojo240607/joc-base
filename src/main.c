/**
 * STM32F4 Discovery (STM32F407VGT6) minimal example
 *  - System clock : HSE(8 MHz) -> PLL -> 168 MHz
 *  - Serial       : USART2 (PA2=TX, PA3=RX) @ 115200 8N1
 *                   -> external USB-TTL adapter -> PC COM8
 *  - LED          : green LD4 on PD12 toggles each loop
 *  - Output       : prints a counter over UART (printf -> _write -> USART2)
 */
#include "stm32f4xx.h"
#include <stdio.h>

/* PCLK1 after clock config (APB1 prescaler = 4, HCLK = 168 MHz) */
#define PCLK1_HZ        42000000UL
#define USART2_BAUDRATE 115200UL

/* ---------------------------------------------------------------
 * System clock: HSE -> PLL -> 168 MHz
 * --------------------------------------------------------------- */
void SystemClock_Config(void)
{
    /* Enable HSE (8 MHz on the Discovery board) */
    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0) { }

    /* Flash latency: 5 WS for 168 MHz @ 3.3 V, enable prefetch + I/D caches */
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_LATENCY_5WS;

    /* PLL: M=8 (->1 MHz), N=336 (->336 MHz), P=2 (->168 MHz), Q=7 (->48 MHz), SRC=HSE */
    RCC->PLLCFGR = (8U   << RCC_PLLCFGR_PLLM_Pos)
                 | (336U << RCC_PLLCFGR_PLLN_Pos)
                 | (0U   << RCC_PLLCFGR_PLLP_Pos)
                 | (7U   << RCC_PLLCFGR_PLLQ_Pos)
                 | RCC_PLLCFGR_PLLSRC_HSE;

    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0) { }

    /* Bus prescalers: AHB /1, APB1 /4, APB2 /2 */
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;

    /* Select PLL as system clock */
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }

    SystemCoreClock = 168000000UL;
}

/* ---------------------------------------------------------------
 * UART2 low-level output (used by syscalls.c _write for printf)
 * --------------------------------------------------------------- */
void uart_putchar(char c)
{
    while ((USART2->SR & USART_SR_TXE) == 0) { }
    USART2->DR = (uint8_t)c;
}

static void uart2_init(void)
{
    /* Enable peripheral clocks */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;

    /* PA2(TX) / PA3(RX) -> Alternate Function 7 (USART2) */
    GPIOA->MODER   = (GPIOA->MODER   & ~((3U << (2 * 2)) | (3U << (3 * 2))))
                   |  ((2U << (2 * 2)) | (2U << (3 * 2)));
    GPIOA->OTYPER &= ~((1U << 2) | (1U << 3));                       /* push-pull */
    GPIOA->OSPEEDR |= ((3U << (2 * 2)) | (3U << (3 * 2)));           /* high speed */
    GPIOA->PUPDR  &= ~((3U << (2 * 2)) | (3U << (3 * 2)));           /* no pull */
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~((0xFU << (2 * 4)) | (0xFU << (3 * 4))))
                   |  ((7U << (2 * 4)) | (7U << (3 * 4)));           /* AF7 */

    /* Baud rate: USARTDIV = PCLK1 / baud */
    USART2->BRR = (uint32_t)(PCLK1_HZ / USART2_BAUDRATE);
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

/* ---------------------------------------------------------------
 * Rough busy-loop delay (~ milliseconds, approximate)
 * --------------------------------------------------------------- */
static void delay_ms(uint32_t ms)
{
    uint32_t iter = (SystemCoreClock / 4000U) * ms;
    for (volatile uint32_t i = 0; i < iter; i++) { }
}

/* ---------------------------------------------------------------
 * main
 * --------------------------------------------------------------- */
int main(void)
{
    SystemClock_Config();
    uart2_init();

    /* Green LED (LD4) on PD12 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    GPIOD->MODER |= (1U << (12 * 2));

    printf("Hello from STM32F407 Discovery!\r\n");
    printf("System clock: %lu Hz, USART2 @ %lu 8N1\r\n",
           (unsigned long)SystemCoreClock, (unsigned long)USART2_BAUDRATE);

    uint32_t tick = 0;
    while (1)
    {
        GPIOD->ODR ^= (1U << 12);
        printf("tick %lu\r\n", (unsigned long)tick++);
        delay_ms(500);
    }
}
