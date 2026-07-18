#ifndef CLOCK_HAL_H
#define CLOCK_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — STM32 system clock (HSE -> PLL -> 168 MHz).
 * The ONLY place that touches RCC/FLASH registers for clock setup.
 */

void    clock_hal_configure(void);
uint32_t clock_hal_sysclk_hz(void);

#endif /* CLOCK_HAL_H */
