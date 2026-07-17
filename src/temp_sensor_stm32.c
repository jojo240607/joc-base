#include "temp_sensor_stm32.h"
#include <stdlib.h>
#include <string.h>

/* Factory calibration values (12-bit ADC raw) measured at VDDA = 3.3 V.
   Stored in the system memory area (see RM0090 "Temperature sensor"). */
#define TS_CAL1_ADDR   ((const volatile uint16_t *)0x1FFF7A2CUL)
#define TS_CAL2_ADDR   ((const volatile uint16_t *)0x1FFF7A2EUL)
#define TS_CAL_VDDA    3.3f   /* calibration was done at VDDA = 3.3 V */

/* Temperature sensor has a NEGATIVE coefficient: VSENSE falls ~2.5 mV/°C,
   so the raw ADC reading at 110 °C (TS_CAL2) is LOWER than at 30 °C (TS_CAL1).
   Linear interpolation between the two factory points:
       T = 30 + 80 * (TS_CAL1 - TS_DATA) / (TS_CAL1 - TS_CAL2)
   TS_DATA is first scaled to the calibration VDDA (3.3 V) so the formula
   stays valid when the actual VDDA differs. */
static float temp_sensor_vread_celsius(temp_sensor_stm32 *self);
static int32_t temp_sensor_vread_celsius_x10(temp_sensor_stm32 *self);

const struct temp_sensor_stm32Fun temp_sensor_stm32_fun = {
    .destroy          = temp_sensor_stm32_destroy,
    .init             = temp_sensor_stm32_init,
    .deinit           = temp_sensor_stm32_deinit,
    .read_celsius     = temp_sensor_vread_celsius,
    .read_celsius_x10 = temp_sensor_vread_celsius_x10,
};

temp_sensor_stm32 *temp_sensor_stm32_create(adc_stm32 *adc, uint32_t vdda_mv)
{
    temp_sensor_stm32 *self = (temp_sensor_stm32 *)malloc(sizeof(temp_sensor_stm32));
    if (!self) return NULL;
    memset(self, 0, sizeof(temp_sensor_stm32));
    self->adc     = adc;
    self->vdda_mv = vdda_mv;
    self->ts_cal1 = *TS_CAL1_ADDR;
    self->ts_cal2 = *TS_CAL2_ADDR;
    temp_sensor_stm32_init(self);
    return self;
}

void temp_sensor_stm32_destroy(temp_sensor_stm32 *self)
{
    if (!self) return;
    temp_sensor_stm32_deinit(self);
    free(self);
    /* self->adc is owned by the caller, so we do NOT destroy it here */
}

void temp_sensor_stm32_init(temp_sensor_stm32 *self)
{
    if (!self) return;
    if (!self->vtable) {
        self->vtable = (struct temp_sensor_stm32Vtable *)malloc(sizeof(struct temp_sensor_stm32Vtable));
        if (self->vtable) memset(self->vtable, 0, sizeof(struct temp_sensor_stm32Vtable));
    }
    self->fun = &temp_sensor_stm32_fun;
    self->vtable->read_celsius     = temp_sensor_vread_celsius;
    self->vtable->read_celsius_x10 = temp_sensor_vread_celsius_x10;
}

void temp_sensor_stm32_deinit(temp_sensor_stm32 *self)
{
    if (!self) return;
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}

float temp_sensor_stm32_read_celsius(temp_sensor_stm32 *self)
{
    if (!self || !self->vtable) return -273.0f;
    return self->vtable->read_celsius(self);
}

int32_t temp_sensor_stm32_read_celsius_x10(temp_sensor_stm32 *self)
{
    if (!self || !self->vtable) return -2730;
    return self->vtable->read_celsius_x10(self);
}

static float temp_sensor_vread_celsius(temp_sensor_stm32 *self)
{
    adc_stm32 *adc = self->adc;
    uint32_t saved = adc->channel;

    adc_stm32_set_channel(adc, 16U);          /* temperature sensor (ADC1_IN16) */
    uint32_t raw = adc_stm32_read(adc);
    adc_stm32_set_channel(adc, saved);        /* restore previous channel */

    /* scale the reading to the calibration VDDA (3.3 V) */
    float vdda = (float)self->vdda_mv / 1000.0f;
    float ts = (float)raw * TS_CAL_VDDA / vdda;

    float cal1 = (float)self->ts_cal1;
    float cal2 = (float)self->ts_cal2;
    float temp = 30.0f + 80.0f * (cal1 - ts) / (cal1 - cal2);
    return temp;
}

static int32_t temp_sensor_vread_celsius_x10(temp_sensor_stm32 *self)
{
    return (int32_t)(temp_sensor_vread_celsius(self) * 10.0f);
}
