#ifndef DAC_HAL_H
#define DAC_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — DAC (STM32F4 DAC1, channels 1/2).
 * All register knowledge lives here; the driver only sees the opaque handle.
 */
typedef struct dac_hal_handle dac_hal_handle_t;

dac_hal_handle_t *dac_hal_create(void *peripheral);   /* DAC_TypeDef* (board only) */
void dac_hal_destroy(dac_hal_handle_t *h);

void dac_hal_enable_clock(dac_hal_handle_t *h);
/* Enable one channel (ch=1|2): ENx=1, trigger disabled (TENx=0 -> DHR auto-
 * transfers to DOR), no waveform, output buffer on. */
void dac_hal_enable_channel(dac_hal_handle_t *h, int ch);
/* Write a 12-bit right-aligned value into the channel's DHR12R register. */
void dac_hal_write(dac_hal_handle_t *h, int ch, uint16_t val);
/* Read back the channel's DOR register (the value latched to the output). */
uint16_t dac_hal_get_dor(dac_hal_handle_t *h, int ch);
/* Read back CR (for verification). */
uint32_t dac_hal_get_cr(dac_hal_handle_t *h);

#endif /* DAC_HAL_H */
