#ifndef ADC_HAL_H
#define ADC_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — ADC (this file is the STM32 implementation).
 *
 * The driver layer (drv/) includes THIS header but NEVER sees ADC_TypeDef or
 * any other chip register: it only ever handles the OPAQUE `adc_hal_handle_t *`.
 * All register knowledge lives in adc_hal.c. To port to another chip you
 * rewrite this HAL (and its .c) only — the driver source is untouched.
 *
 * The concrete handle struct (holding ADC_TypeDef*, channel, ...) is defined
 * privately in adc_hal.c, so it stays hidden from every caller.
 */
typedef struct adc_hal_handle adc_hal_handle_t;

/* platform-specific construction: the board passes the real peripheral (cast to
 * void*) and the LOGICAL channel; everything else is hidden inside the handle. */
adc_hal_handle_t *adc_hal_create(void *peripheral, uint32_t channel);
void adc_hal_destroy(adc_hal_handle_t *h);

void     adc_hal_enable_clock(adc_hal_handle_t *h);
void     adc_hal_common_config(adc_hal_handle_t *h);   /* prescaler + internal-sensor enable */
void     adc_hal_config_gpio(adc_hal_handle_t *h);     /* analog mode for external ch */
void     adc_hal_config_channel(adc_hal_handle_t *h);
void     adc_hal_set_channel(adc_hal_handle_t *h, uint32_t channel); /* update + reconfigure */
uint32_t adc_hal_single_convert(adc_hal_handle_t *h);  /* SWSTART + EOC + read DR */
uint32_t adc_hal_to_mv(uint32_t raw, uint32_t vdda_mv);

#endif /* ADC_HAL_H */
