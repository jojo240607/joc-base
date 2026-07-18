#include "temp_hal.h"
#include "stm32f4xx.h"

#define TS_CAL1_ADDR   ((const volatile uint16_t *)0x1FFF7A2CUL)
#define TS_CAL2_ADDR   ((const volatile uint16_t *)0x1FFF7A2EUL)

uint16_t temp_hal_ts_cal1(void)
{
    return *TS_CAL1_ADDR;
}

uint16_t temp_hal_ts_cal2(void)
{
    return *TS_CAL2_ADDR;
}
