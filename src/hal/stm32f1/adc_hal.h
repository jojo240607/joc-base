#ifndef ADC_HAL_H
#define ADC_HAL_H

#include <stdint.h>
#include "irq.h"

/* Minimal stub for F103. ADC not used on this port. */
typedef struct adc_hal_handle adc_hal_handle_t;

adc_hal_handle_t *adc_hal_create(void *peripheral, uint32_t channel);
void adc_hal_destroy(adc_hal_handle_t *h);
void adc_hal_enable_clock(adc_hal_handle_t *h);
void adc_hal_common_config(adc_hal_handle_t *h);
void adc_hal_config_gpio(adc_hal_handle_t *h);
void adc_hal_config_channel(adc_hal_handle_t *h);
void adc_hal_set_channel(adc_hal_handle_t *h, uint32_t channel);
uint32_t adc_hal_single_convert(adc_hal_handle_t *h);
uint32_t adc_hal_to_mv(uint32_t raw, uint32_t vdda_mv);
irq_id_t adc_hal_irq_id(adc_hal_handle_t *h);
void adc_hal_enable_eoc_irq(adc_hal_handle_t *h);
void adc_hal_disable_eoc_irq(adc_hal_handle_t *h);
void adc_hal_start_convert(adc_hal_handle_t *h);
uint32_t adc_hal_read_dr(adc_hal_handle_t *h);
void *adc_hal_get_dr_addr(adc_hal_handle_t *h);
void adc_hal_enable_dma(adc_hal_handle_t *h);
void adc_hal_disable_dma(adc_hal_handle_t *h);

#endif /* ADC_HAL_H */