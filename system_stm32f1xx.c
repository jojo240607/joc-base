/*
 * Minimal system_stm32f1xx.c for STM32F103.
 *
 * Provides SystemInit() which sets VTOR to the flash base.
 * The full clock configuration is done by clock_hal_configure() in board_tick_init().
 */
#include "stm32f1xx.h"

/* SystemCoreClock 由 clock_hal_configure() 最终设为 72000000 (HSE+PLL@72MHz) */
uint32_t SystemCoreClock = 72000000UL;

void SystemInit(void)
{
    /* Set vector table offset to the start of flash (0x08000000) */
    SCB->VTOR = FLASH_BASE | 0x0000U;

    /* Enable HSI (8 MHz internal RC) as fallback; the clock HAL switches
     * to HSE+PLL=72 MHz later. No FPU on M3, so skip FPU config. */
    RCC->CR |= RCC_CR_HSION;
    while ((RCC->CR & RCC_CR_HSIRDY) == 0) { }
}