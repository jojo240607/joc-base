#include "temp_hal.h"
#include "stm32h7xx.h"

/* H7 factory temperature calibration words (RM0433, device memory map):
 *   TS_CAL1 = raw ADC @ 30 C  -> 0x1FF1E820
 *   TS_CAL2 = raw ADC @ 130 C -> 0x1FF1E840   (H7 upper point is 130 C,
 *   NOT the F4's 110 C — the driver formula must use 130 C for H7.) */

#define TS_CAL1_ADDR   ((const volatile uint16_t *)0x1FF1E820UL)
#define TS_CAL2_ADDR   ((const volatile uint16_t *)0x1FF1E840UL)

uint16_t temp_hal_ts_cal1(void)
{
    return *TS_CAL1_ADDR;
}

uint16_t temp_hal_ts_cal2(void)
{
    return *TS_CAL2_ADDR;
}
