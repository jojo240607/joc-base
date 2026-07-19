#ifndef ADC_HAL_H
#define ADC_HAL_H

#include <stdint.h>
#include "irq.h"              /* irq_id_t — the HAL returns the platform irq id */

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

/* --- interrupt support (used by the driver via the platform-independent
 *     irq framework: it registers adc_hal_irq_id() with irq_register) --- */
irq_id_t adc_hal_irq_id(adc_hal_handle_t *h);          /* chip IRQn for this ADC */
void     adc_hal_enable_eoc_irq(adc_hal_handle_t *h);  /* set ADC_CR1_EOCIE */
void     adc_hal_disable_eoc_irq(adc_hal_handle_t *h); /* clear ADC_CR1_EOCIE */
void     adc_hal_start_convert(adc_hal_handle_t *h);   /* SWSTART only (no wait) */
uint32_t adc_hal_read_dr(adc_hal_handle_t *h);         /* read DR (clears EOC) */

#endif /* ADC_HAL_H */
