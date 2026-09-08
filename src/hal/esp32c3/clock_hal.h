#ifndef CLOCK_HAL_H
#define CLOCK_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — ESP32-C3 system clock.
 * Phase-1 (Renode): the RISC-V core runs at a fixed 40 MHz and there is no
 * clock-tree programming (Renode tags absorb writes), so configure() is a
 * no-op and sysclk_hz() returns the fixed platform constant.
 */

void    clock_hal_configure(void);
uint32_t clock_hal_sysclk_hz(void);

#endif /* CLOCK_HAL_H */
