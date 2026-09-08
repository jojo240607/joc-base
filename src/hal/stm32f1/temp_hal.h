#ifndef TEMP_HAL_H
#define TEMP_HAL_H

/* Minimal stub for F103. Internal temperature sensor not used. */

#include <stdint.h>

#define TEMP_HAL_CAL1_ADDR 0x1FFFF7E8U
#define TEMP_HAL_CAL2_ADDR 0x1FFFF7F0U

static inline int16_t temp_hal_read_cal1(void) { return 0; }
static inline int16_t temp_hal_read_cal2(void) { return 0; }

#endif /* TEMP_HAL_H */