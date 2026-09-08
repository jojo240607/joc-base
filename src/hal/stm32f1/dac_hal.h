#ifndef DAC_HAL_H
#define DAC_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — DAC (STM32F103).
 *
 * F103 has one DAC with channels 1 and 2. Register layout is identical to
 * F4 (DHR12R1/DOR1, DHR12R2/DOR2, CR, SWTRIGR).
 *
 * Trigger + DMA support is provided for API compatibility with the F4 driver,
 * but F103 DAC DMA requests are routed through DMA1 (not DMA2 like F4).
 */
typedef struct dac_hal_handle dac_hal_handle_t;

dac_hal_handle_t *dac_hal_create(void *peripheral);
void dac_hal_destroy(dac_hal_handle_t *h);

void dac_hal_enable_clock(dac_hal_handle_t *h);
void dac_hal_enable_channel(dac_hal_handle_t *h, int ch);
void dac_hal_write(dac_hal_handle_t *h, int ch, uint16_t val);
uint16_t dac_hal_get_dor(dac_hal_handle_t *h, int ch);
uint16_t dac_hal_get_dhr(dac_hal_handle_t *h, int ch);
uint32_t dac_hal_get_cr(dac_hal_handle_t *h);

/* DMA support */
void    *dac_hal_get_dhr_addr(dac_hal_handle_t *h, int ch);
void     dac_hal_enable_dma(dac_hal_handle_t *h, int ch);
void     dac_hal_disable_dma(dac_hal_handle_t *h, int ch);

/* Trigger mode */
void dac_hal_enable_trigger(dac_hal_handle_t *h, int ch, uint32_t tsel);
void dac_hal_disable_trigger(dac_hal_handle_t *h, int ch);

#endif /* DAC_HAL_H */