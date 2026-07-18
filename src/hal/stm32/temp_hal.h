#ifndef TEMP_HAL_H
#define TEMP_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — STM32 factory temperature calibration.
 * The ONLY place that reads the chip-specific system-memory calibration words.
 */

uint16_t temp_hal_ts_cal1(void);   /* raw ADC @ 30 C  (0x1FFF7A2C) */
uint16_t temp_hal_ts_cal2(void);   /* raw ADC @ 110 C (0x1FFF7A2E) */

#endif /* TEMP_HAL_H */
