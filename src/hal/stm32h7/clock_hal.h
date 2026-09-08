#ifndef CLOCK_HAL_H
#define CLOCK_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — STM32H7 system clock (HSE -> PLL1 -> 480 MHz).
 * The ONLY place that touches RCC/FLASH/PWR registers for clock setup.
 *
 * H750B-DK: HSE 25 MHz, VOS0, PLL1 480 MHz SYSCLK, AHB 240 MHz,
 * APB1/APB2/APB3/APB4 120 MHz, Cortex-M7 clock 480 MHz.
 */

void     clock_hal_configure(void);
uint32_t clock_hal_sysclk_hz(void);

/* USART1 kernel clock (APB2) — used for BRR computation. H7: 120 MHz. */
uint32_t clock_hal_usart_hz(void);

#endif /* CLOCK_HAL_H */
