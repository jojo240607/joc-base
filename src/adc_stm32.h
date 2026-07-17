#ifndef ADC_STM32_H
#define ADC_STM32_H

#include "stm32f4xx.h"
#include <stdint.h>

typedef struct _adc_stm32 adc_stm32;

struct adc_stm32Fun {
    void (*destroy)(adc_stm32 *self);
    void (*init)(adc_stm32 *self);
    void (*deinit)(adc_stm32 *self);
    uint32_t (*read)(adc_stm32 *self);        /* single conversion, raw 12-bit */
    uint32_t (*read_mv)(adc_stm32 *self);     /* converted to millivolts (VDDA) */
    void (*set_channel)(adc_stm32 *self, uint32_t channel);
};

struct adc_stm32Vtable {
    uint32_t (*read)(adc_stm32 *self);
    uint32_t (*read_mv)(adc_stm32 *self);
    void (*set_channel)(adc_stm32 *self, uint32_t channel);
};

struct _adc_stm32 {
    struct adc_stm32Vtable *vtable;
    const struct adc_stm32Fun *fun;
    ADC_TypeDef *instance;     /* ADC1 / ADC2 / ADC3 */
    uint32_t channel;          /* ADC channel 0..15 (gpio) or 16/17/18 (int) */
    uint32_t vdda_mv;          /* supply voltage in mV (default 3300) */
    uint8_t internal;          /* 1 if channel is temp(16)/vref(17)/vbat(18) */
};

adc_stm32 *adc_stm32_create(ADC_TypeDef *adc, uint32_t channel);
void adc_stm32_destroy(adc_stm32 *self);
void adc_stm32_init(adc_stm32 *self);
void adc_stm32_deinit(adc_stm32 *self);
uint32_t adc_stm32_read(adc_stm32 *self);
uint32_t adc_stm32_read_mv(adc_stm32 *self);
void adc_stm32_set_channel(adc_stm32 *self, uint32_t channel);

extern const struct adc_stm32Fun adc_stm32_fun;

#endif /* ADC_STM32_H */
