#ifndef TEMP_SENSOR_STM32_H
#define TEMP_SENSOR_STM32_H

#include "stm32f4xx.h"
#include "adc_stm32.h"
#include <stdint.h>

typedef struct _temp_sensor_stm32 temp_sensor_stm32;

struct temp_sensor_stm32Fun {
    void (*destroy)(temp_sensor_stm32 *self);
    void (*init)(temp_sensor_stm32 *self);
    void (*deinit)(temp_sensor_stm32 *self);
    float   (*read_celsius)(temp_sensor_stm32 *self);
    int32_t (*read_celsius_x10)(temp_sensor_stm32 *self);  /* 1 decimal, *10 */
};

struct temp_sensor_stm32Vtable {
    float   (*read_celsius)(temp_sensor_stm32 *self);
    int32_t (*read_celsius_x10)(temp_sensor_stm32 *self);
};

struct _temp_sensor_stm32 {
    struct temp_sensor_stm32Vtable *vtable;
    const struct temp_sensor_stm32Fun *fun;
    adc_stm32 *adc;        /* shared ADC (must be ADC1); switched to CH16 on read */
    uint32_t vdda_mv;      /* actual supply in mV (default 3300) */
    uint16_t ts_cal1;      /* factory calib raw @30C  (0x1FFF7A2C) */
    uint16_t ts_cal2;      /* factory calib raw @110C (0x1FFF7A2E) */
};

temp_sensor_stm32 *temp_sensor_stm32_create(adc_stm32 *adc, uint32_t vdda_mv);
void temp_sensor_stm32_destroy(temp_sensor_stm32 *self);
void temp_sensor_stm32_init(temp_sensor_stm32 *self);
void temp_sensor_stm32_deinit(temp_sensor_stm32 *self);
float   temp_sensor_stm32_read_celsius(temp_sensor_stm32 *self);
int32_t temp_sensor_stm32_read_celsius_x10(temp_sensor_stm32 *self);

extern const struct temp_sensor_stm32Fun temp_sensor_stm32_fun;

#endif /* TEMP_SENSOR_STM32_H */
